# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader with a dedicated path for inputs shorter than one SIMD step;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,548 of 1,650 single-document results against simdjson On Demand, tied 17 and lost 85. On reverse-order reads it won 384 of 400. On Windows/MSVC it won 313 of 330 single-document results, tied 4 and lost 13.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 143 of 150 results, tied 1 and lost 6, and it won 118 of the 120 corpus record-stream results.

The whole suite was also run on three AVX-512 builds on an AMD EPYC 9V45, against simdjson's own AVX-512 kernel (§9). There Jsonifier won 905 of 990 single-document results, tied 17 and lost 68, won 237 of 240 reverse-order results, and won 87 of 90 streaming results.

---

## 1. Background: why On Demand is hard to beat

simdjson's On Demand API [1] does not build a DOM. Stage 1 produces a tape of structural positions, and stage 2 is a cursor over that tape driven by the caller. A value that is never requested is skipped over by structural depth, without being parsed. Reading in document order costs roughly one pass over the tape.

The price is a strictly forward-only API. Each value can be consumed once, and reading an object's fields out of order means rescanning the object for every key.

## 2. Design goals

1. **Keep On Demand's shape.** Existing On Demand code should port to `jsonifier::generic` [4] mostly mechanically.
2. **Allow values to be read again.** A value handle remembers where it sits on the tape and can re-seek to it, instead of being invalidated.
3. **Make in-order reads free.** The common case must cost no more than a cursor increment and a key compare.
4. **Keep out-of-order reads close to in-order speed.** Their cost should stay close to linear in the size of the object, no matter what order the caller reads keys in.

## 3. Architecture

### 3.1 The tape

Stage 1 is Jsonifier's existing structural indexer (see *Two Stages, On Demand* [2], §4, and *Batched Drain, Fused Scan* [3]). It writes one `uint32_t` offset per structural character, and appends a sentinel entry `tape[count] = length`. Because of the sentinel, stage 2 never has to bounds-check the end of the tape on its hot paths.

Inputs shorter than one SIMD step (`simdBytesPerStep`, a few hundred bytes at most) are routed to a dedicated short-input reader (`pod_block_reader` in the code; it is unrelated to the POD tests of §7.2). For inputs that short, the fixed cost of setting up the full-width reader outweighs the actual work, and the short-input reader's step sizes are chosen to fit the input. This cutoff is far below the 5 KiB *Small* tests of §6, so most Small documents still take the full-width reader.

### 3.2 A shared forward cursor

`document_state` holds one cursor into the tape and one depth counter. Every object, array and value handle refers to that shared state; a handle does not own its own iterator.

When a handle is used, it *settles* first:

- If the shared state is deeper than the handle, the state finishes the open containers up to the handle's depth.
- If the depth still doesn't match, the handle rewinds the cursor to its own opening position.

In-order reads never trigger either branch. Out-of-order reads pay only for the distance they actually move. On the hot paths, the cursor and depth are copied into locals, advanced there, and written back to the shared state once, so the compiler can keep them in registers.

### 3.3 Lazy errors

Accessors return `[[nodiscard]] error_code` and write their result to an out-parameter; they don't wrap results in a `result<T>`. A structural error is stored in the handle and passed along to every handle derived from it. `document::atEnd()` checks that the whole document was consumed. Errors therefore cost one well-predicted branch per access.

### 3.4 Finding fields

`findFieldUnordered(key)` runs in three tiers:

1. **The expected field.** In-order reads almost always find the requested key right at the cursor, possibly after skipping one comma. This tier needs one tape read and one key compare.
2. **Scan and wrap.** If the key isn't at the cursor, scan forward to the end of the object, then wrap around to the start of the object and scan up to where the cursor began.
3. **Escape-aware retry.** Only if both scans miss, rescan the object comparing keys with escape sequences decoded. Escaped keys are rare, so the common path never pays for decoding.

Objects with at most 16 keys replace tier 2 with a recorded key trail, and larger objects replace it with a probe of the field index once they have shown that they are being read out of order (§5). `findField(key)` is the ordered counterpart, matching simdjson's `find_field`: it scans forward only, never wraps and records nothing, and it falls back only to the escape-aware retry.

### 3.5 Build-time CPU selection

Jsonifier compiles exactly one instruction-set tier per build, chosen when CMake configures the project, either by running a detector on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS`, and capped with `JSONIFIER_SIMD_TIER`. Every call is then a direct, inlinable call into code built for that ISA, with no CPUID read and no per-call check. The reason is inlining: per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, so GCC and Clang reject multi-tier builds of that code. The cost is that a binary for mixed hardware is configured for the lowest tier it must support. simdjson instead dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time.

`jsonifier::generic` is compiled for the same tier as the rest of the library, so its stage 1 is the build's AVX-512, AVX2, AVX, NEON or SVE2 kernel, with no change to the calling code.

## 4. Zero-copy strings

`getString(std::string_view&)` returns a view straight into the input buffer whenever the string contains no escapes. It finds where the string ends with SIMD stop masks (a mask marking where a quote, backslash or control character appears):

- The first register is handled outside the loop ("peeled"), so short strings never enter the loop at all.
- The leftover bytes at the end are handled by a lookup table instead of a byte-by-byte loop.

Non-ASCII bytes go through UTF-8 validation on the view path. The `std::string` getter is split differently: pure-ASCII strings take the fast path, and everything else goes through the one-pass unescape-and-copy scanner, which validates UTF-8 with a SIMD validator in that same pass. Non-ASCII strings are therefore validated on both getters; the copying getter just does it while it copies instead of in a separate scan.

Strings that do contain escapes are decoded into an arena. Each string's tape offset maps to a fixed slot in the arena, so decoding the same value twice yields the same view. Earlier designs let the arena wrap around, which could overwrite views the caller still held; the fixed slots remove that failure.

**A GCC pitfall.** With `-funroll-loops`, GCC unrolls an un-peeled SIMD scan loop into a long prologue that runs even for short strings, which are most of the strings in the suite. The fix is to peel the first iteration by hand and use the table-driven tail described above.

## 5. The field index: making out-of-order reads cheap

Reading in reverse order is the worst case for tier 2 in §3.4. Each lookup scans past every key that comes after the target, so reading all *n* keys of an object in reverse costs O(n²).

### 5.1 The map

`field_index_map` is an insert-only, SwissTable-style hash map built specifically for this job:

- **Control bytes** come in 16-byte groups. An empty slot holds `-128`. An occupied slot holds `H2 = hash & 0x7F`, so every occupied control byte is non-negative.
- **Each entry** stores `{hash, object tape index, key tape index}`. It doesn't copy the key text; the key is compared against the tape directly.
- **The hash** mixes the key bytes with the object's tape index. The same key name in two different objects therefore hashes to different entries, and one map can serve the whole document. Short keys go through a single `mix64` mixing step; keys longer than 16 bytes are hashed a block at a time, murmur-style.
- **Probing** compares a whole control group against `H2` in one step, through the library's own SIMD wrappers (`gatherValuesU`, `opCmpEqRaw`, `opBitMaskRaw`), which lower to:
  - x86: `_mm_cmpeq_epi8` + `_mm_movemask_epi8`, one bit per slot;
  - NEON: `vceqq_u8` + a `vshrn`-narrowed nibble mask, four bits per slot, since NEON has no movemask instruction;
  - SVE2 (128-bit vectors): `svcmpeq_u8`, packed into the same four-bits-per-slot nibble mask;
  - any other target: the library's scalar SIMD emulation.
- **Reset** between documents clears the control bytes with `memset`, and only if the map was actually used for the previous document.

### 5.2 The lesson: whole-document indexing lost

The first version recorded every key the scanner skipped past, in every object. On Marine IK Reverse that made the parser **2.1–2.4x slower** than having no index at all, because Marine IK consists mostly of small objects with 3–6 fields each. For objects that small, re-scanning a few keys is cheaper than hashing, inserting and probing them.

### 5.3 Waiting until an object earns an index

The first fix was a counter on each object handle:

- The handle counts how many keys it has scanned past.
- Until that count reaches a threshold *T*, lookups use the plain scan-and-wrap path.
- After that, later scans record every key they pass into the map, and later lookups probe the map first.

Small objects never touch the map; large objects get O(1) lookups after one pass over their keys. The first A/B of that counter, run with *T* = 16:

| Test | Compiler | No index | Whole-doc index | Index after 16 skipped keys | vs simdjson |
|---|---|---|---|---|---|
| Marine IK Reverse (min) | GCC 16 | 7.49 ms | 17.1 ms | 6.24 ms | +79% |
| Marine IK Reverse (pretty) | GCC 16 | 8.77 ms | 18.0 ms | 7.42 ms | +61% |
| Marine IK Reverse (min) | Clang 24 | 8.03 ms | 19.2 ms | 6.48 ms | +42% |
| Marine IK Reverse (pretty) | Clang 24 | 8.86 ms | 20.0 ms | 7.25 ms | +40% |

*The "no index" and "index after 16 skipped keys" columns come from one A/B run and the "whole-doc index" column from another; each value is the median of 3 rounds, built with the CI flags. The "vs simdjson" column compares the "index after 16 skipped keys" build against simdjson in that same run.*

### 5.4 Choosing the threshold: a parameter sweep

*T* trades two costs. Set too high, objects with many fields rescan for longer before the map activates. Set too low, medium-sized objects pay for recording scans (hashing and inserting every key they pass) that a short rescan would have beaten. We swept *T* ∈ {2, 4, 8, 16} on four builds, running the full suite at each value, with the counter-only trigger above. The table shows Jsonifier's reverse-order margin over simdjson on the two tests that are sensitive to the threshold:

| Build | Test | *T* = 16 | *T* = 8 | *T* = 4 | *T* = 2 |
|---|---|---|---|---|---|
| Linux / GCC | Marine IK Reverse (min) | +74% | +82% | +11% | +39% |
| Linux / GCC | Random Reverse (min) | +29% | +37% | +42% | **−26%** |
| Linux / Clang | Marine IK Reverse (min) | +39% | +51% | **−14%** | +13% |
| Linux / Clang | Random Reverse (min) | +34% | +39% | +55% | **−25%** |
| macOS / Clang | Marine IK Reverse (min) | n/c | +55% | **−6%** | n/c |
| macOS / Clang | Random Reverse (min) | tie | n/c | +43% | **−27%** |
| macOS / GCC | Random Reverse (min) | +32% | +35% | n/c | **−38%** |

*"n/c" means the result did not converge in that run.*

Two failure modes repeat across builds:

- **At *T* = 2, Random Reverse loses on all four builds** where it converged. Random's objects are mid-sized, so almost every one crosses the threshold and pays for recording scans it didn't need.
- **At *T* = 4, Marine IK Reverse loses on two of the three builds** where it converged, for the same reason. On the third, Linux/GCC, it still wins, but its margin falls from +82% at *T* = 8 to +11%.

*T* = 8 is the lowest value with no reverse-order loss among the converged results in this table. Two gaps limit that claim: macOS/Clang's Random Reverse did not converge at *T* = 8, and the table has no macOS/GCC Marine IK row, so neither is shown loss-free there. It also matched or beat *T* = 16 on most reverse tests: on Linux/Clang it lifted CitmCatalog Reverse from +85% to +106% and Discord Reverse from +307% to +365%, and in the sweep runs it turned Linux/Clang's in-order record from 21-0-2 into 22-1-0. (The sweep runs are separate from the runs reported in §7.)

The threshold is therefore chosen per OS/compiler, the same way Jsonifier chooses its stage-1 step geometry:

| Build | *T* | Basis |
|---|---|---|
| Linux / GCC | 8 | swept |
| Linux / Clang | 8 | swept |
| macOS / GCC | 8 | swept |
| macOS / Clang | 16 | swept; neither 8 nor 16 lost a converged reverse result, but each left a sensitive test unconverged and 16 tied Random, so the sweep could not separate them |
| Windows / MSVC | 8 | not swept; uses the value that won on the other builds |

On macOS/Clang, 16 kept larger margins on the documents whose objects have many fields (Twitter, Discord), while 8 won on Random, Google Maps and Instruments. Neither produced a converged reverse loss, but at *T* = 16 Marine IK did not converge and Random tied, and at *T* = 8 Random did not converge, so the choice rests on the many-field documents rather than on a clean sweep.

### 5.5 Requiring evidence of out-of-order access

A skipped-key count cannot tell a large object read out of order from a large object read sparsely in order. A sparse reader that asks for three fields of a 30-field object skips most of its keys on the way, so the count crosses *T* even though every lookup is found ahead of the cursor. Recording keys during those skips costs a hash and an insert per key for an index that no lookup needs.

The trigger in the measured code (§6) therefore asks for two kinds of evidence, both kept per object handle:

- **Size:** the skipped-key count has reached *T*, chosen per build as in §5.4. When the threshold was introduced this was the gate that kept small objects such as Marine IK's off the map; in the measured code the key trail of §5.6 does that first, and *T* only applies to objects that have overflowed the trail.
- **Out-of-order access:** at least two lookups on this object have missed the expected field, scanned to the end of the object, and wrapped around to its start. A wrap is the event that only out-of-order reads produce. One wrap is not enough: a sparse reverse read of three fields often wraps once on its last lookup, whose key is first in the object, and an index built then would serve no further lookup.

Until both hold, lookups use scan-and-wrap and record nothing, so skips never pay for hashing. Once both hold, the next lookup that misses the expected field builds the object's index in one dedicated pass over the whole object, recording every key, and then probes it. Every later out-of-order lookup on that object is a single probe; a key the map does not hold falls through to the escape-aware retry of §3.4.

### 5.6 Small objects: a key trail and a direction score

The field index only pays for large objects, so without further help smaller objects would still be rescanned on every out-of-order lookup. Two cheaper mechanisms cover them:

- **A key trail per object.** The first lookup on an object scans forward from the cursor as in §3.4. When that scan misses, the object starts a trail instead of wrapping around: it walks its keys from the start, records each key's tape offset in a 16-entry buffer held in the handle, and compares as it goes. Later lookups compare against the recorded offsets first, without skipping any values, and extend the trail only when the key has not been seen yet. An object with more than 16 keys overflows the trail and falls back to scan-and-wrap and the field index above; the trail also handles escaped keys by decoding only the recorded keys that contain an escape.
- **A direction score per document.** The document keeps one saturating counter from 0 to 8. A lookup that lands before the point where it resumed adds one, and one that lands ahead of it subtracts one. At 4 or more, a resumed lookup searches the part of the object before the cursor first and only then scans forward, so a reader that consistently asks for keys in reverse no longer pays a failed forward scan per lookup. Reads in document order keep the score at 0 and never take this path.

The trail and the field index therefore split objects by size rather than overlap. An object with at most 16 keys stays on its trail: it never wraps, so it never counts toward the two-wrap gate of §5.5 and never reaches the index, whatever *T* is. Only an object that overflows the trail falls back to scan-and-wrap, and only then do its wraps and skipped keys count toward the gate. In the measured code the index thus serves only objects with more than 16 keys, and *T* acts as a floor within that group. The §5.4 sweep ran before the trail existed, which is when *T* = 8 could still index objects of 9 to 16 keys.

## 6. Methodology

- **Harness:** Json-Performance [5], branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 10 documents (Apache Builds, Canada, CitmCatalog, Discord, Github Events, Google Maps, Instruments, Marine IK, Random and Twitter) each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 170 single-document results per build at full size and another 160 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal. The in-order and sparse readers use each library's ordered lookup (`findField`, `find_field`), and the reverse and sparse-reverse readers use its unordered lookup (`findFieldUnordered`, `find_field_unordered`).
- **Correctness:** on the measured commits, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, read directly from upstream simdjson's internal stage-1 parser after `stage1()`, with no fork or patch. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `d4727ec` and simdjson `449f38e`, the benchmarked commits, on November 14, 2026. The tape checks ran on cloud-hosted CI runners, not on the benchmark hosts: their Linux builds report Azure kernels (`7.0.0-1019-azure`) except the AVX-512 GCC build, which reports an AWS kernel (`7.0.0-1016-aws`), the harness does not record their CPU model, and their x86 compilers differ from the benchmark builds' (MSVC 19.51 and GCC 16.0.1, against 19.52 and 16.2.1 below; Clang 24.0 is the same), while the macOS tape builds used the same GCC 16.2.0 and Clang 23.1.4. The AVX-512 builds could not run on the Core i9-14900KF, which has no AVX-512; their benchmarks ran on separate cloud hosts, described in §9. The benchmark runs of §7 and §8 did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-43-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 22 of the 1,650 single-document results and 10 of the 150 streaming results did not converge (see §10). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. Both libraries read integers more slowly under MSVC than under Linux/Clang on the same CPU: simdjson reads Int64 (fresh) at 579.2 MB/s under MSVC against 795.5 MB/s under Linux/Clang, while Jsonifier reads it at 825.9 and 1,557.8 MB/s; for Uint64 (fresh) the figures are 566.3 against 729.1 MB/s for simdjson and 834.4 against 1,542.9 MB/s for Jsonifier.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 162 / 170 | 155 / 160 | 317 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 164 / 170 | 158 / 160 | 322 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `27950a9`, Jsonifier `d4727ec` and simdjson `449f38e`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. All five builds ran on November 14, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 20-0-0 | 20-0-0 | 20-0-0 | 16-0-4 | 20-0-0 | 19-0-1 | 20-0-0 | 157-2-6 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 16-0-4 | 18-0-2 | 18-0-2 | 18-0-2 | 152-0-13 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 20-0-0 | 20-0-0 | 15-0-5 | 18-0-2 | 19-0-1 | 19-0-1 | 150-0-15 |
| macOS / GCC | 5-0-0 | 18-1-1 | 19-0-1 | 18-1-1 | 20-0-0 | 19-1-0 | 20-0-0 | 16-3-1 | 19-1-0 | 154-7-4 |
| macOS / Clang | 4-0-1 | 19-1-0 | 19-0-1 | 20-0-0 | 20-0-0 | 17-2-1 | 19-0-1 | 20-0-0 | 18-1-1 | 156-4-5 |
| **All** | **23-0-2** | **89-4-7** | **96-0-4** | **98-1-1** | **99-0-1** | **83-3-14** | **95-0-5** | **92-3-5** | **94-2-4** | **769-13-43** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 20-0-0 | 20-0-0 | 20-0-0 | 15-0-5 | 19-0-1 | 20-0-0 | 20-0-0 | 156-2-7 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 17-0-3 | 19-0-1 | 18-0-2 | 18-0-2 | 154-0-11 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 20-0-0 | 20-0-0 | 15-0-5 | 18-0-2 | 20-0-0 | 20-0-0 | 152-0-13 |
| macOS / GCC | 5-0-0 | 20-0-0 | 19-1-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 19-1-0 | 19-0-1 | 160-2-3 |
| macOS / Clang | 4-0-1 | 17-0-3 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 18-0-2 | 20-0-0 | 157-0-8 |
| **All** | **23-0-2** | **89-2-9** | **97-1-2** | **99-0-1** | **99-0-1** | **84-0-16** | **96-0-4** | **95-1-4** | **97-0-3** | **779-4-42** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (22 of them did not converge; see §10), Jsonifier won 1,548, tied 17 and lost 85: 769-13-43 with fresh output objects and 779-4-42 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 193 of 200 full-size reverse results (1 tie, 6 losses) and 191 of 200 small reverse results (no ties, 9 losses). MSVC went 79-0-1, macOS/Clang 78-0-2, macOS/GCC 78-1-1, Linux/Clang 77-0-3 and Linux/GCC 72-0-8. Every full-size reverse loss and tie is Canada: Linux/GCC lost both forms in both runs (−3.4% to −5.3%), macOS/Clang lost minified Canada (fresh, −5.6%) and macOS/GCC prettified Canada (fresh, −4.8%), with a tie on macOS/GCC (minified, reused). At small size, Canada Small holds 7 of the 9 losses: Linux/GCC in both forms and both runs (−9.9% to −11.7%) and Linux/Clang three times (−0.8% to −4.7%). The other 2 are prettified Google Maps Small, on MSVC (reused, −1.1%) and macOS/Clang (fresh, −9.9%).
- **Windows/MSVC lost 13 of its 330 single-document results,** 11 of them on small documents. Its two full-size losses are prettified Marine IK read in order (−4.7% fresh, −5.0% reused). Read in order, it lost Marine IK Small 3 times (−1.9% to −2.4%), Canada Small 3 times (−0.9% to −2.4%), CitmCatalog Small twice (reused, −8.5% minified and −1.6% prettified) and prettified Discord Small once (fresh, −1.5%); the other 2 are prettified Instruments Small read sparse (fresh, −1.0%) and prettified Google Maps Small read in reverse (reused, −1.1%). Its 4 ties are minified Canada and minified Marine IK read in order, in both runs. It won all 10 POD results, 79 of its 80 reverse results and 159 of its 160 sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.1–13.0% for Jsonifier and 6.4–10.2% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.7 percentage points on Linux/Clang, +1.8 on macOS/Clang, +2.0 on Linux/GCC, +2.9 on MSVC and +6.7 on macOS/GCC. On MSVC, Discord Reverse (min) goes from +194.7% to +230.7%, and Apache Builds (min) from +15.3% to +16.6%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +100.0% / +93.2% | +93.6% / +95.2% | +129.4% / +131.1% | +44.4% / +49.3% | +6.4% / +19.4% |
| Double | +77.0% / +111.3% | +86.9% / +92.8% | +85.5% / +90.7% | +97.9% (n/c) / +99.9% | +93.5% / +72.9% |
| Int64 | +42.6% / +52.5% | +95.8% / +103.0% | +79.9% / +83.0% | +73.9% / +82.9% | +73.8% / +47.8% |
| String | +3.2% / +5.2% | −7.7% / −15.1% | +2.3% / +5.3% | +17.6% / +11.2% | −34.1% / −32.3% (n/c) |
| Uint64 | +47.3% / +43.4% | +111.6% / +117.4% | +79.6% / +84.5% | +52.1% / +50.6% | +66.8% / +42.8% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.3% / +16.6% | +18.1% / +30.7% | +26.7% / +40.5% | +42.5% / +79.4% | +35.5% / +32.6% |
| Apache Builds (pretty) | +5.7% / +10.5% | +16.5% / +23.1% | +25.9% / +36.6% | +37.0% / +55.8% | +26.3% / +24.0% |
| Canada (min) | tie / tie | +1.9% / +1.5% | −8.6% / −9.5% | +4.3% / +28.5% (n/c) | +5.6% / −6.7% (n/c) |
| Canada (pretty) | +2.5% / +7.7% | −0.6% / −0.9% | −7.1% / −8.4% | +13.7% (n/c) / +4.2% (n/c) | +13.4% (n/c) / +11.6% |
| CitmCatalog (min) | +7.4% / +6.2% | +10.4% / +11.3% | +15.7% / +17.7% | +2.8% (n/c) / +16.3% | +10.1% / +11.3% |
| CitmCatalog (pretty) | +5.6% / +7.0% | +7.4% / +6.2% | +10.2% / +12.0% | tie / +7.8% | +10.5% / +11.2% |
| Discord (min) | +10.6% / +14.8% | +15.9% / +25.5% | +13.4% / +22.3% | +28.9% / +169.0% | +24.1% / +26.3% |
| Discord (pretty) | +5.5% / +9.1% | +12.8% / +16.9% | +10.1% / +16.0% | +28.0% / +168.0% | +19.0% / +20.1% |
| Github Events (min) | +18.8% / +25.2% | +27.4% / +34.3% | +22.1% / +27.3% | +51.0% / +53.2% | +48.2% / +89.4% |
| Github Events (pretty) | +14.6% / +21.0% | +20.5% / +25.5% | +18.9% / +22.3% | +33.9% / +41.2% | +70.6% / +47.6% |
| Google Maps (min) | +13.5% / +15.5% | +27.5% / +28.5% | +28.6% / +31.2% | +30.0% / +33.7% | +16.7% / +31.6% |
| Google Maps (pretty) | +10.3% / +10.4% | +14.2% / +16.3% | +21.2% / +20.8% | +29.6% / +31.1% | +13.7% / +8.1% |
| Instruments (min) | +14.5% / +18.8% | +5.6% / +5.8% | +11.5% / +11.6% | +4.3% / +14.7% | +30.3% / +7.0% |
| Instruments (pretty) | +9.7% / +12.3% | +3.2% / +4.5% | +3.9% / +3.9% | +13.4% / +12.2% | +18.0% / −13.6% |
| Marine IK (min) | tie / tie | +2.8% / +3.9% | −1.7% / −1.0% | −22.8% / +3.4% | tie / −11.7% (n/c) |
| Marine IK (pretty) | −4.7% / −5.0% | +6.1% / +4.5% | −1.5% / −1.9% | +21.6% / +9.0% | +7.0% / +3.8% |
| Random (min) | +19.7% / +23.6% | +27.4% / +34.3% | +37.7% / +48.8% | +31.7% / +70.8% | +34.5% / +40.7% |
| Random (pretty) | +15.7% / +21.0% | +24.0% / +28.7% | +30.9% / +36.3% | +59.6% / +56.0% | +28.0% / +36.9% |
| Twitter (min) | +12.8% / +19.6% | +15.0% / +23.0% | +11.6% / +18.9% | +25.2% / +30.2% | +10.1% / +17.0% |
| Twitter (pretty) | +12.8% / +13.3% | +11.0% / +18.8% | +14.8% / +12.3% | +1.3% / +24.1% | +20.6% / +7.1% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +36.7% / +53.6% | +57.4% / +71.5% | +59.4% / +73.9% | +77.9% / +163.8% | +107.0% / +73.1% |
| Apache Builds (pretty) | +30.2% / +43.2% | +52.3% / +67.9% | +55.8% / +70.8% | +87.2% / +88.4% | +87.9% / +69.8% |
| Canada (min) | +3.5% / +4.1% | +4.7% / +5.2% | −5.1% / −5.3% | +9.4% / tie | −5.6% / +3.1% |
| Canada (pretty) | +4.4% / +4.2% | +2.8% / +3.1% | −3.6% / −3.4% | −4.8% / +2.6% (n/c) | +28.9% (n/c) / +33.2% |
| CitmCatalog (min) | +43.0% / +43.2% | +62.5% / +67.7% | +60.5% / +65.3% | +42.2% / +74.9% | +73.9% / +78.1% |
| CitmCatalog (pretty) | +42.3% / +44.8% | +53.9% / +56.2% | +55.2% / +57.8% | +40.4% / +95.5% | +80.1% / +90.4% |
| Discord (min) | +194.7% / +230.7% | +237.4% / +265.7% | +207.8% / +226.4% | +326.2% / +345.9% | +375.9% / +426.5% |
| Discord (pretty) | +155.1% / +177.2% | +201.6% / +218.5% | +183.9% / +197.5% | +161.2% / +229.1% | +226.9% / +277.9% |
| Github Events (min) | +89.9% / +115.7% | +120.7% / +139.3% | +91.3% / +105.4% | +105.3% / +123.9% | +138.7% / +235.4% |
| Github Events (pretty) | +81.0% / +96.1% | +91.7% / +102.8% | +75.8% / +86.7% | +82.4% / +96.2% | +143.2% / +203.1% |
| Google Maps (min) | +11.5% / +13.6% | +13.0% / +13.6% | +10.7% / +10.1% | +29.4% / +32.3% | +19.3% / +33.0% |
| Google Maps (pretty) | +4.5% / +4.9% | +6.6% / +4.9% | +6.3% / +8.7% | +22.7% / +24.7% | +22.7% / +12.9% |
| Instruments (min) | +156.9% / +166.4% | +150.5% / +156.8% | +137.5% / +140.7% | +141.0% / +149.6% | +227.0% / +313.7% |
| Instruments (pretty) | +135.4% / +143.6% | +130.5% / +131.8% | +110.9% / +114.6% | +110.2% / +116.3% | +169.8% / +142.1% |
| Marine IK (min) | +30.7% / +31.9% | +44.2% / +47.0% | +43.0% / +44.7% | +20.6% / +51.8% | +53.5% / +84.5% |
| Marine IK (pretty) | +32.5% / +35.8% (n/c) | +53.3% / +53.7% | +45.8% / +44.7% | +62.8% / +46.9% (n/c) | +75.9% (n/c) / +35.4% |
| Random (min) | +61.9% / +72.2% | +81.3% / +94.5% | +71.0% / +80.6% | +51.4% / +89.4% | +113.8% / +125.8% |
| Random (pretty) | +49.3% / +55.6% | +63.1% / +70.1% | +56.6% / +65.6% | +66.0% / +83.5% | +77.0% / +70.6% |
| Twitter (min) | +160.5% / +177.4% | +147.6% / +159.3% | +111.7% / +120.7% | +122.7% / +132.9% | +191.7% / +223.5% |
| Twitter (pretty) | +130.3% / +152.3% | +123.0% / +132.6% | +91.5% / +98.1% | +99.6% / +110.6% | +201.6% / +200.1% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +11.4% / +25.7% | +31.5% / +43.3% | +38.5% / +51.2% | +34.6% / +67.3% | +80.6% / +43.2% |
| Apache Builds (pretty) | +5.3% / +13.9% | +20.1% / +25.7% | +26.2% / +38.5% | +30.8% / +47.7% | +22.3% / +28.9% |
| Canada (min) | +29.3% / +29.9% | +8.8% / +9.6% | +15.4% / +15.4% | +31.0% / +8.9% (n/c) | +52.0% / +34.4% |
| Canada (pretty) | +31.7% / +29.9% | +9.7% / +9.3% | +17.5% / +18.0% | +2.1% / +10.3% | +34.7% / +17.0% |
| CitmCatalog (min) | +12.3% / +13.9% | +6.2% / +7.8% | +12.4% / +12.6% | tie / +5.9% | +6.0% / +7.8% |
| CitmCatalog (pretty) | +11.2% / +11.2% | +3.4% / +4.8% | +8.9% / +10.0% | +15.2% / +25.2% | +9.3% / +9.7% |
| Discord (min) | +11.5% / +17.5% | +25.3% / +23.2% | +14.3% / +16.3% | +53.8% / +46.3% | +36.3% / +43.4% |
| Discord (pretty) | +9.7% / +13.8% | +16.2% / +18.0% | +9.4% / +11.2% | +31.4% / +38.1% | +12.8% / +35.8% |
| Github Events (min) | +16.9% / +23.1% | +16.7% / +20.9% | +18.6% / +21.2% | +35.0% / +39.8% | +14.5% / +29.1% |
| Github Events (pretty) | +9.5% / +9.8% | +7.9% / +10.6% | +9.4% / +10.4% | +20.2% / +22.6% | +14.6% / +13.3% |
| Google Maps (min) | +7.0% / +8.8% | +18.1% / +18.9% | +20.5% / +21.8% | +19.4% / +20.9% | +21.1% / +8.8% |
| Google Maps (pretty) | +9.7% / +12.1% | +8.8% / +8.7% | +10.6% / +14.2% | +38.9% / +14.2% | +12.6% / +17.7% |
| Instruments (min) | +15.3% / +19.9% | +11.9% / +12.3% | +11.5% / +12.9% | +20.0% / +11.4% | +12.9% / +15.0% |
| Instruments (pretty) | +7.7% / +9.4% | +3.7% / +3.8% | +3.5% / +4.8% | +18.8% / +11.8% | +3.0% / +12.8% |
| Marine IK (min) | +14.7% / +16.4% | +8.0% / +9.9% | +16.6% / +17.0% | −14.4% / −6.0% | +19.8% / +16.9% |
| Marine IK (pretty) | +15.1% / +15.0% | +8.9% / +6.6% | +4.7% / +6.0% | +12.6% / +18.7% | +18.9% / +17.6% |
| Random (min) | +19.9% / +23.3% | +29.0% / +32.2% | +25.4% / +30.4% | +34.3% / +42.3% | +54.9% / +52.0% |
| Random (pretty) | +14.7% / +17.5% | +16.1% / +16.8% | +17.0% / +19.5% | +32.1% / +35.0% | +23.8% / +33.6% |
| Twitter (min) | +49.3% / +53.0% | +18.4% / +18.4% | +12.9% / +12.9% | +29.1% / +42.1% | +28.0% / +23.3% |
| Twitter (pretty) | +36.1% / +38.3% | +9.4% / +9.3% | +7.2% / +6.5% | +36.4% / +11.6% | +16.5% / +22.4% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.7% / +15.2% | +17.9% / +20.3% | +17.1% / +18.0% | +44.3% / +75.2% | +36.8% / +47.7% |
| Apache Builds (pretty) | +4.1% / +9.9% | +11.9% / +15.1% | +11.4% / +13.5% | +33.6% / +57.0% | +19.6% / +23.1% |
| Canada (min) | +21.9% / +22.4% | +8.2% / +8.1% | +13.1% / +13.3% | +36.9% / +18.3% | +35.6% / +38.0% |
| Canada (pretty) | +25.1% / +24.0% | +9.3% / +7.9% | +14.0% / +15.6% | +10.2% / +18.2% | +25.1% / +25.5% |
| CitmCatalog (min) | +7.1% / +8.8% | +1.9% / +1.4% | +3.4% / +3.2% | +5.5% / +5.8% | +5.5% / +5.4% |
| CitmCatalog (pretty) | +12.7% / +13.2% | −2.7% / −1.8% | +0.6% / +0.8% | +9.1% / +8.7% | +8.0% / +8.0% |
| Discord (min) | +36.8% / +47.8% | +37.0% / +40.8% | +39.7% / +46.1% | +37.0% / +49.9% | +111.7% / +105.1% |
| Discord (pretty) | +29.4% / +36.1% | +31.2% / +35.5% | +30.1% / +35.4% | +31.6% / +36.6% | +56.9% / +34.8% |
| Github Events (min) | +35.9% / +41.9% | +39.6% / +44.6% | +33.9% / +38.0% | +48.8% / +60.5% | +48.5% / +55.2% |
| Github Events (pretty) | +25.0% / +31.1% | +26.0% / +28.1% | +26.1% / +28.1% | +29.7% / +37.3% | +34.4% / +25.4% |
| Google Maps (min) | +11.6% / +13.1% | +12.9% / +14.0% | +11.4% / +10.0% | +8.0% / +8.1% | +11.9% / +11.6% |
| Google Maps (pretty) | +6.6% / +7.4% | +0.6% / +1.3% | +4.9% / +5.6% | +9.5% / +14.0% | +16.9% / +9.0% |
| Instruments (min) | +30.0% / +34.3% | +25.8% / +25.3% | +28.3% / +28.9% | +24.5% / +25.7% | +61.9% / +84.5% |
| Instruments (pretty) | +24.3% / +24.7% | +23.9% / +25.0% | +19.0% / +18.7% | +19.9% / +21.5% | +21.0% / +24.1% |
| Marine IK (min) | +35.9% / +35.2% | +35.8% / +41.7% | +41.7% / +40.0% | +14.0% / +6.5% | +38.5% / +39.6% |
| Marine IK (pretty) | +33.5% / +33.8% | +29.5% / +27.9% | +29.2% / +29.5% | +16.1% / +24.5% | +33.2% / +30.8% |
| Random (min) | +21.0% / +23.8% | +24.8% / +28.0% | +32.3% / +35.1% | +33.2% / +40.2% | +68.3% / +61.5% |
| Random (pretty) | +18.3% / +20.5% | +13.7% / +17.2% | +20.8% / +22.7% | +32.5% / +36.3% | +36.7% / +57.5% |
| Twitter (min) | +49.2% / +51.5% | +22.6% / +24.1% | +16.8% / +19.3% | +14.8% / +17.9% | +19.1% / +23.3% |
| Twitter (pretty) | +37.2% / +37.3% | +17.1% / +16.8% | +13.5% / +15.0% | +28.8% / +13.4% | +22.5% / +35.5% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.8% / +23.9% | +18.2% / +23.6% | +27.0% / +36.9% | +21.3% / +42.1% | +14.9% / +23.0% |
| Apache Builds (pretty) | +2.8% / +8.8% | +15.7% / +20.8% | +20.8% / +31.1% | +28.8% (n/c) / +42.6% | +15.2% / +19.8% |
| Canada (min) | +4.9% / −0.9% | −5.1% / −4.7% | −14.9% / −18.6% | +3.6% / +3.8% | +2.5% / +5.9% |
| Canada (pretty) | −2.4% / −2.3% | −2.1% / −7.1% | −12.8% / −17.7% | +2.9% / +3.3% | tie / +1.3% |
| CitmCatalog (min) | +3.4% / −8.5% | +2.1% / +1.8% | +1.5% / +5.6% | +10.4% / +12.8% | +9.9% / +10.6% |
| CitmCatalog (pretty) | +1.9% / −1.6% | −1.1% / +0.5% | +1.3% / +3.1% | +9.1% / +13.3% | +4.3% / +8.4% |
| Discord (min) | +1.1% / +10.0% | +5.9% / +10.6% | +5.6% / +12.2% | +19.8% / +110.2% | +8.7% / +25.1% |
| Discord (pretty) | −1.5% / +6.0% | +3.4% / +9.2% | +1.5% / +7.4% | +20.8% / +106.8% | +12.9% / +18.5% |
| Github Events (min) | +10.0% / +8.6% | +9.8% / +9.9% | +5.3% / +4.1% | +10.4% / +15.3% | +36.8% / +14.9% |
| Github Events (pretty) | +4.6% / +5.4% | +3.1% / +2.0% | +1.0% / +2.3% | +5.0% / +20.9% | +11.7% / +16.3% |
| Google Maps (min) | +14.0% / +19.4% | +24.7% / +25.9% | +29.1% / +28.9% | tie / +25.8% | tie / +31.8% |
| Google Maps (pretty) | +9.3% / +13.3% | +13.5% / +15.1% | +21.5% / +19.7% | +30.5% / +45.7% | −2.5% / −2.9% |
| Instruments (min) | +11.6% / +12.6% | +0.4% / +1.0% | +3.6% / +3.5% | +22.3% / +14.0% | +7.8% / +20.9% |
| Instruments (pretty) | +9.0% / +8.3% | −2.6% / −1.2% | −2.5% / −3.2% | +14.3% / +18.9% | +5.0% / +3.8% |
| Marine IK (min) | −1.9% / −2.2% | +0.9% / +2.9% | −2.5% / −2.7% | +16.8% / −7.5% | +35.8% / +7.3% |
| Marine IK (pretty) | −2.4% / +3.4% | +5.2% / +4.3% | −6.0% / −2.8% | +17.4% / +10.0% | +16.0% / −8.6% |
| Random (min) | +11.2% / +11.1% | +19.2% / +24.4% | +26.1% / +33.1% | +21.9% / +61.1% | +32.0% / +52.6% |
| Random (pretty) | +8.6% / +10.7% | +17.2% / +21.5% | +19.5% / +24.5% | +46.6% / +56.5% | +27.8% / +31.7% |
| Twitter (min) | +13.7% / +17.4% | +8.0% / +10.3% | +6.1% / +9.6% | +28.2% / +32.3% | +8.8% / +48.0% (n/c) |
| Twitter (pretty) | +6.4% / +12.0% | +4.9% / +7.2% | +3.7% / +6.6% | +17.9% / +23.6% | +3.9% / +8.3% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +47.5% / +80.7% | +77.1% / +90.1% | +74.1% / +82.6% | +64.9% / +154.4% | +90.3% / +135.4% |
| Apache Builds (pretty) | +39.9% / +64.0% | +60.2% / +74.2% | +61.6% / +69.6% | +45.6% / +91.5% | +56.6% / +96.0% |
| Canada (min) | +0.5% / +1.3% | −4.7% / +1.0% | −11.2% / −10.9% | +4.6% / +4.9% | +9.6% / +4.4% |
| Canada (pretty) | +2.3% / +1.1% | −0.8% / −2.0% | −9.9% / −11.7% | +4.7% / +4.8% | +3.9% / +2.0% |
| CitmCatalog (min) | +31.9% / +43.8% | +53.5% / +58.3% | +47.8% / +54.0% | +65.6% / +83.8% | +77.6% / +89.7% |
| CitmCatalog (pretty) | +33.4% / +36.2% | +40.6% / +43.0% | +43.7% / +48.1% | +53.5% / +54.0% | +66.7% / +76.1% |
| Discord (min) | +164.0% / +230.1% | +215.2% / +252.7% | +187.0% / +214.1% | +274.3% / +289.5% | +353.0% / +397.6% |
| Discord (pretty) | +139.2% / +191.1% | +187.1% / +214.4% | +167.9% / +192.8% | +163.6% / +243.2% | +300.2% / +319.9% |
| Github Events (min) | +28.5% / +34.9% | +38.6% / +44.9% | +22.3% / +24.3% | +15.3% / +44.3% | +60.2% / +77.9% |
| Github Events (pretty) | +16.7% / +23.6% | +23.4% / +25.4% | +16.4% / +17.3% | +23.7% / +50.0% | +52.3% / +59.3% |
| Google Maps (min) | +3.1% / +5.0% | +10.9% / +13.3% | +5.5% / +6.9% | +13.1% / +25.9% | +20.1% / +24.0% |
| Google Maps (pretty) | +1.7% / −1.1% | +4.6% / +5.8% | +5.1% / +5.3% | +39.7% / +24.2% | −9.9% / +31.5% |
| Instruments (min) | +123.8% / +136.3% | +140.5% / +144.0% | +130.9% / +130.8% | +183.9% / +164.0% | +223.0% / +238.9% |
| Instruments (pretty) | +104.7% / +112.8% | +113.5% / +122.3% | +99.3% / +103.3% | +110.0% / +90.1% | +198.1% / +208.4% |
| Marine IK (min) | +26.9% / +26.4% | +45.8% / +48.7% | +31.3% / +32.9% | +41.4% / +65.5% | +58.5% / +40.6% |
| Marine IK (pretty) | +22.7% / +26.2% | +42.0% / +44.4% | +28.0% / +28.8% | +43.6% / +21.3% | +35.6% / +38.0% |
| Random (min) | +56.0% / +68.2% | +78.9% / +90.2% | +62.5% / +69.9% | +72.8% / +84.9% | +116.2% / +128.9% |
| Random (pretty) | +40.8% / +48.7% | +59.1% / +64.0% | +50.9% / +56.0% | +50.1% / +64.2% | +97.8% / +104.3% |
| Twitter (min) | +134.8% / +151.5% | +140.2% / +146.6% | +101.5% / +105.4% | +129.0% / +120.9% | +222.2% / +305.1% |
| Twitter (pretty) | +114.3% / +135.5% | +118.9% / +124.2% | +85.1% / +87.6% | +148.5% / +146.7% | +218.3% / +231.9% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.4% / +22.1% | +32.8% / +33.2% | +35.9% / +42.8% | +53.5% / +63.5% | +7.0% / +25.6% |
| Apache Builds (pretty) | +9.3% / +12.2% | +20.3% / +23.4% | +21.1% / +24.1% | +31.2% / +62.6% | +20.3% / +16.1% |
| Canada (min) | +33.0% / +37.8% | +13.8% / +13.5% | +10.4% / +9.8% | +17.3% / +16.6% | +6.0% / +4.0% |
| Canada (pretty) | +23.9% / +29.3% | +13.8% / +14.6% | +10.9% / +10.1% | +13.6% / +28.2% | +3.1% / −10.3% |
| CitmCatalog (min) | +15.0% / +21.2% | +7.7% / +8.2% | +13.6% / +13.8% | +15.5% / +16.0% | +10.4% / +14.4% |
| CitmCatalog (pretty) | +11.7% / +13.7% | +10.8% / +10.0% | +10.6% / +9.3% | +33.9% / +14.2% | +9.0% / +10.0% |
| Discord (min) | +7.5% / +11.4% | +11.0% / +15.6% | +9.8% / +13.8% | +11.7% / +26.5% | +20.3% / +16.0% |
| Discord (pretty) | +2.7% / +5.6% | +7.3% / +7.5% | +3.6% / +7.1% | +13.3% / +20.5% | +6.3% / +7.4% |
| Github Events (min) | +14.3% / +15.3% | +9.1% / +10.9% | +12.8% / +13.7% | tie / tie | +10.7% / +10.7% |
| Github Events (pretty) | +3.7% / +6.6% | +6.4% / +4.4% | +5.5% / +6.6% | tie / +23.1% | +7.5% / +10.6% |
| Google Maps (min) | +7.6% / +6.8% | +11.8% / +15.2% | +17.1% / +18.1% | +34.9% / +55.7% | +13.3% / +23.6% (n/c) |
| Google Maps (pretty) | +5.4% / +8.6% | +6.6% / +7.7% | +10.3% / +12.5% | +9.8% (n/c) / +29.0% | +16.2% / +7.0% |
| Instruments (min) | +6.9% / +13.2% | +4.3% / +5.9% | +2.4% / +6.0% | −12.1% / +10.3% | +30.6% / −5.6% |
| Instruments (pretty) | −1.0% / +2.6% | −0.7% / −1.2% | −1.9% / +0.8% | tie / +23.1% | +37.1% / +1.8% |
| Marine IK (min) | +11.4% / +14.8% | +5.7% / +5.5% | +6.2% / +6.8% | +21.3% / +9.3% | +7.5% / +7.2% |
| Marine IK (pretty) | +18.9% / +21.7% | −2.4% / −2.7% | +0.5% / +4.2% | +16.6% / +40.6% | +3.3% / +3.5% |
| Random (min) | +5.1% / +9.9% | +17.3% / +18.2% | +20.9% / +22.1% | +28.8% / +44.2% | +24.1% / +31.2% |
| Random (pretty) | +6.9% / +4.6% | +8.7% / +9.9% | +11.2% / +11.9% | +22.8% / +20.1% | +16.0% / +21.8% |
| Twitter (min) | +28.0% / +33.4% | +4.9% / +0.5% | +1.0% / +12.5% | +35.1% (n/c) / +11.0% | +10.2% / +10.0% |
| Twitter (pretty) | +22.5% / +23.1% | +3.5% / +2.9% | +0.5% / +2.5% | +8.3% / +10.2% | +14.5% / +1.1% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.8% / +19.6% | +17.5% / +16.6% | +11.8% / +16.7% | +22.5% / +45.6% | +8.9% / +21.9% |
| Apache Builds (pretty) | +5.8% / +5.3% | +8.9% / +7.6% | +5.8% / +7.9% | +20.7% / +28.3% | +7.0% / +7.7% |
| Canada (min) | +22.9% / +21.3% | +5.2% / +6.7% | +28.5% / +27.0% | +18.2% / +14.5% | +4.4% / +3.6% |
| Canada (pretty) | +20.4% / +24.7% | +12.7% / +13.1% | +26.4% / +25.2% | +11.2% / +10.8% | +1.6% / +1.2% |
| CitmCatalog (min) | +9.2% / +13.9% | +2.8% / +0.2% | +1.0% / +0.8% | +9.6% / +19.6% | +10.5% / +9.5% |
| CitmCatalog (pretty) | +10.3% / +16.3% | −5.0% / −3.4% | −2.3% / +1.8% | +25.4% / +6.8% | +8.9% / +7.7% |
| Discord (min) | +25.0% / +35.1% | +41.5% / +46.8% | +30.3% / +35.4% | +46.1% / +52.6% | +82.9% / +58.3% |
| Discord (pretty) | +22.1% / +26.6% | +33.1% / +37.7% | +23.2% / +28.8% | +24.0% / +31.9% | +58.7% / +80.8% |
| Github Events (min) | +27.7% / +31.1% | +23.7% / +30.6% | +20.2% / +18.4% | +38.3% / +35.0% | +29.0% / +36.0% |
| Github Events (pretty) | +18.7% / +18.5% | +14.6% / +19.2% | +16.7% / +14.9% | +30.2% (n/c) / +6.2% | +32.3% / +21.0% |
| Google Maps (min) | +9.9% / +9.9% | +10.2% / +9.6% | +8.7% / +6.6% | tie / +4.1% | tie / +3.6% |
| Google Maps (pretty) | +5.6% / +5.0% | −2.9% / −3.7% | +4.6% / +5.4% | +9.3% / +7.7% | +7.0% / +25.4% |
| Instruments (min) | +25.4% / +26.4% | +21.7% / +25.1% | +22.0% / +28.6% | +23.6% / +33.5% | +36.3% / +39.1% |
| Instruments (pretty) | +23.3% / +25.5% | +18.3% / +21.8% | +21.8% / +24.1% | +15.4% / +13.2% | −6.4% / +1.2% |
| Marine IK (min) | +32.2% / +30.9% | +41.4% / +43.5% | +36.9% / +37.7% | +40.7% / +25.0% | +33.7% / +35.2% |
| Marine IK (pretty) | +24.3% / +26.6% | +18.8% / +18.8% | +37.7% / +37.3% | +25.7% / +32.4% | +21.5% / +22.5% |
| Random (min) | +12.0% / +15.2% | +20.7% / +21.5% | +29.7% / +27.6% | +18.3% / +71.1% | +41.3% / +47.3% |
| Random (pretty) | +8.0% / +8.8% | +10.1% / +11.3% | +22.2% / +21.1% | +23.6% / +47.2% | +22.4% / +19.0% |
| Twitter (min) | +25.5% / +27.4% | +7.4% / +6.2% | +12.8% / +10.8% | +3.3% / −2.6% (n/c) | +21.5% / +21.7% |
| Twitter (pretty) | +12.9% / +14.5% | +9.1% / +9.8% | +12.7% / +12.2% | +11.4% / +9.4% | +12.5% / +12.1% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 5.3x faster at full size: +91.5% (Twitter pretty, Linux/GCC, fresh) up to +426.5% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +85.1% to +397.6%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −5.6% (minified Canada on macOS/Clang, fresh) to +33.2%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** It won 157 of its 160 results and tied 3, all on macOS/GCC's small sparse reads. Read in reverse at full size it wins on every build (+75.8% to +235.4%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 198 of 200 full-size sparse-reverse results across both allocation modes (no ties, 2 losses), and 191 of 200 small ones (2 ties, 7 losses). MSVC won all 80. The full-size losses are both prettified CitmCatalog on Linux/Clang (−2.7% fresh, −1.8% reused). The small losses are prettified CitmCatalog Small on Linux/Clang (−5.0% fresh, −3.4% reused) and Linux/GCC (fresh, −2.3%), prettified Google Maps Small on Linux/Clang (−2.9% fresh, −3.7% reused), prettified Instruments Small on macOS/Clang (fresh, −6.4%) and minified Twitter Small on macOS/GCC (reused, −2.6%, n/c); the 2 ties are minified Google Maps Small on both macOS builds (fresh).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 252.2 MB/s on MSVC against 306.1 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 643.2 and 923.3 MB/s. Every MSVC full-size sparse result is a win, by +5.3% to +53.0%, and every full-size sparse-reverse result by +4.1% to +51.5%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 197 of 200 full-size sparse results (1 tie, 2 losses) and 187 of 200 small ones (4 ties, 9 losses). MSVC, Linux/Clang, Linux/GCC and macOS/Clang won all 40 of their full-size results. The full-size losses are minified Marine IK on macOS/GCC (−14.4% fresh, −6.0% reused). Instruments Small holds 6 of the 9 small losses: prettified on Linux/Clang in both runs (−0.7% and −1.2%), on Linux/GCC and MSVC (fresh, −1.9% and −1.0%), and minified on macOS/GCC (fresh, −12.1%) and macOS/Clang (reused, −5.6%). The other 3 are prettified Marine IK Small on Linux/Clang (−2.4% fresh, −2.7% reused) and prettified Canada Small on macOS/Clang (reused, −10.3%). All 5 ties are on macOS/GCC. The largest loss is minified Marine IK on macOS/GCC (fresh, −14.4%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 178, tied 6 and lost 16. Linux/Clang went 38-0-2, macOS/GCC 38-1-1, macOS/Clang 36-1-3, MSVC 34-4-2 and Linux/GCC 32-0-8 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 167, tied 3 and lost 30. macOS/GCC went 38-1-1, macOS/Clang 35-2-3, Linux/Clang 33-0-7, MSVC 31-0-9 and Linux/GCC 30-0-10. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's are Canada Small (−2.1% to −7.1%), prettified Instruments Small (−2.6% and −1.2%) and prettified CitmCatalog Small (fresh, −1.1%), MSVC's (Canada Small, CitmCatalog Small, Marine IK Small and Discord Small) run from −0.9% to −8.5%, macOS/GCC's one is minified Marine IK Small (reused, −7.5%), macOS/Clang's are prettified Google Maps Small (−2.5% fresh, −2.9% reused) and prettified Marine IK Small (reused, −8.6%), and Linux/GCC's run from −2.5% to −18.6%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −18.6%).
- **In-order losses at full size fall mostly on two documents.** Linux/GCC lost Canada in both forms and both runs (−7.1% to −9.5%) and Marine IK in both forms and both runs (−1.0% to −1.9%). MSVC lost prettified Marine IK in both runs (−4.7% and −5.0%), and Linux/Clang lost prettified Canada in both runs (−0.6% and −0.9%). macOS/GCC lost minified Marine IK (fresh, −22.8%), and macOS/Clang lost minified Canada (reused, −6.7%, n/c), minified Marine IK (reused, −11.7%, n/c) and prettified Instruments (reused, −13.6%). Canada and Marine IK hold 15 of the 16; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 46-0-4 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+6.4% to +131.1%, +72.9% to +111.3%, +42.6% to +103.0% and +42.8% to +117.4%). String is the weakest: 6 wins (+2.3% to +17.6%) and 4 losses, on Linux/Clang (−7.7% and −15.1%) and macOS/Clang (−34.1% and −32.3%, n/c) in both runs.

## 8. Streams: many documents in one buffer

Many real inputs are not one document but a sequence of them: NDJSON logs, JSON Lines exports, or documents joined by commas. simdjson handles these with `iterate_many`, which runs stage 1 over a window of the input and hands out one On Demand document at a time. `jsonifier::generic` does the same through `parser::iterateMany`, and every document it hands out is an ordinary `generic::generic_document<false>`, with re-readable values and the per-object field index from §5.

### 8.1 Design

```cpp
for (jsonifier::generic::generic_document<false> doc : parser.iterateMany(buffer)) { ... }
auto stream = parser.iterateMany<parse_options{ .newLineDelimited = false }>(buffer, batchSize);
```

- **The separator is part of the type.** `parse_options::newLineDelimited` selects it: `true` (NDJSON and other whitespace-separated streams, the `iterateMany` default) or `false` for comma-delimited streams. The two modes find document boundaries in different ways (§8.2), and the choice is made at compile time.
- **Windowed stage 1.** Stage 1 runs over one window of `batchSize` bytes (1 MiB by default), using the same indexer, and the same small-input routing, as `iterate()`. Tape offsets are relative to the window, so no single window can exceed 4 GiB, but the stream as a whole can.
- **One document per tape slice.** Each document is bound to the parser's shared cursor (§3.2), starting at its first structural. The accessors, the lazy errors and the field index work unchanged; the field index is reset per document.
- **Documents cut by the window edge.** No document is ever parsed from two windows. The next window starts at the first byte of the first document that the current window could not complete, and a document larger than a whole window returns `error_code::capacity`, as in simdjson.
- **Comma-separated streams.** In comma mode, a comma between top-level documents is accepted as a separator. A leading or doubled comma is a `tape_error`, and a trailing comma is `trailing_content`. A comma that falls at the start of a new window is still consumed as a separator.
- **Reporting.** The stream exposes `truncatedBytes()` for an unfinished final document, and each iterator exposes `currentIndex()` and `source()` for the current document's offset and raw text. After an error, the stream yields that error once and then ends.
- **Lifetime.** As with simdjson, advancing the stream invalidates the previous document; the only thing that survives is a zero-copy view of an unescaped string, which points into the caller's buffer.

### 8.2 Finding where documents end

The simplest way to find each document's end is to walk its structurals and count depth before handing the document out. That bounds every later tape scan to the document, but a reader that consumes the document in order walks the same structurals again, so every in-order document pays for two passes over its part of the tape.

The whitespace-delimited mode finds boundaries without that pass:

- **A local rule marks document starts.** Inside any container, every value is preceded on the tape by `[`, `{`, `,` or `:`. So in a whitespace-delimited stream, a structural that can start a value and directly follows a closer or a scalar begins a new top-level document. No depth needs to be tracked to recognize it.
- **One short backward scan per window.** Scanning backward from the end of the window's tape, the first position that satisfies the rule is the start of the window's last document. Everything before it is complete. That last document is deferred to the next window unless this is the final window; only when a window holds a single document, or for the final document of the final window, is a depth walk used to decide completeness. The scan usually stops within a few structurals of the window's end.
- **The reader tells the stream where the document ended.** When the caller advances, the parser's state already records how far the reads went. If the cursor is still inside the document, the stream finishes the open containers from the cursor. For an in-order reader, whose cursor rests just before the closing brace, that is about one step. If the cursor is past a closed root value, it is the end. Only a document the caller never touched is skipped from its start.

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 0.6% to 11.5% below its own NDJSON throughput on the same records in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 30 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 22 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 28 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| macOS / Clang | 4-0-2 | 10-1-1 | 12-0-0 | 26-1-3 |
| **All** | **25-0-5** | **58-1-1** | **60-0-0** | **143-1-6** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | −1.9% | +26.5% | +29.6% | +23.5% | +18.5% |
| Large Amazon Cellphones (NDJSON) | −3.9% | +11.9% | +18.9% | +12.0% (n/c) | +13.0% |
| Stream Formats Small (NDJSON) | +13.6% | +12.3% | +18.3% | −10.9% (n/c) | +11.3% |
| Stream Formats Small (comma) | +58.9% | +109.9% | +72.2% | +81.4% | +62.9% |
| Stream Formats Large (NDJSON) | +9.9% | +5.4% | +16.7% | +16.4% | −7.0% |
| Stream Formats Large (comma) | +15.1% | +13.9% | +22.2% | +11.9% | −15.7% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +8.9% | +23.4% | +36.3% | +52.1% | +31.5% |
| Apache Builds (comma) | +20.8% | +37.9% | +54.3% | +65.9% | +24.7% |
| CitmCatalog (NDJSON) | +3.9% | +11.8% | +15.4% | +35.1% | −7.3% |
| CitmCatalog (comma) | +13.5% | +35.0% | +39.5% | +12.9% | +25.6% |
| Google Maps (NDJSON) | +16.8% | +29.7% | +42.7% | +28.2% | +21.4% |
| Google Maps (comma) | +26.2% | +47.7% | +59.5% | +54.6% (n/c) | +27.7% |
| Instruments (NDJSON) | +17.5% | +16.9% | +10.0% | +35.5% (n/c) | tie |
| Instruments (comma) | +33.4% | +45.8% | +49.4% | +73.3% | +57.5% |
| Random (NDJSON) | +18.1% | +28.6% | +40.0% | +59.8% | +44.8% |
| Random (comma) | +27.2% | +41.1% | +54.0% | +78.2% | +67.2% |
| Twitter (NDJSON) | +14.2% | +14.5% | +9.1% | +32.7% (n/c) | +26.4% |
| Twitter (comma) | +20.7% | +28.9% | +27.3% | +28.9% (n/c) | +23.9% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +10.3% | +19.2% | +15.2% | +39.2% | +54.2% |
| Apache Builds (comma) | +20.3% | +38.2% | +33.1% | +42.5% | +48.8% |
| CitmCatalog (NDJSON) | +21.4% | +29.3% | +31.0% | +29.1% | +54.4% |
| CitmCatalog (comma) | +34.8% | +49.2% | +50.9% | +66.0% | +65.8% |
| Google Maps (NDJSON) | +5.3% | +10.1% | +11.9% | +15.7% | +15.9% |
| Google Maps (comma) | +13.4% | +24.3% | +24.9% | +17.0% | +19.2% |
| Instruments (NDJSON) | +32.3% | +30.5% | +3.8% | +27.8% | +46.3% (n/c) |
| Instruments (comma) | +41.5% | +52.9% | +28.1% | +61.3% | +59.1% |
| Random (NDJSON) | +48.8% | +67.2% | +62.4% | +76.2% | +124.7% |
| Random (comma) | +58.8% | +85.9% | +82.0% | +117.3% (n/c) | +130.9% (n/c) |
| Twitter (NDJSON) | +168.5% | +157.8% | +118.8% | +166.5% (n/c) | +237.2% |
| Twitter (comma) | +177.2% | +167.8% | +130.5% | +107.2% | +228.5% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 25 of the 30 results in that group and lost 5. It won Stream Formats Small (comma) on all five builds (+58.9% to +109.9%). Amazon Cellphones (+18.5% to +29.6%) and Large Amazon Cellphones (+11.9% to +18.9%) won on every build but MSVC, which lost both (−1.9% and −3.9%). Stream Formats Small (NDJSON) won on four builds (+11.3% to +18.3%) and lost on macOS/GCC (−10.9%, n/c). Stream Formats Large won in both forms on MSVC, Linux/Clang, Linux/GCC and macOS/GCC, and lost both on macOS/Clang (NDJSON −7.0%, comma −15.7%).
- **Two builds won every streaming test,** 30-0-0 each: Linux/Clang and Linux/GCC. macOS/GCC went 29-0-1, Windows/MSVC 28-0-2 and macOS/Clang 26-1-3.
- **In-order corpus streams almost never lose.** Jsonifier won 58 of the 60 in-order corpus results; macOS/Clang lost CitmCatalog as NDJSON (−7.3%) and tied Instruments as NDJSON.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +107.2% to +237.2%, while Google Maps rows, which have few, give +5.3% to +24.9%. Instruments wins on every build (+3.8% to +61.3%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 366.4 MB/s under MSVC and 461.3 MB/s under Linux/Clang, while Jsonifier reads it at 581.9 and 857.6 MB/s.

## 9. AVX-512 builds

Every result in §7 and §8 comes from AVX2 and NEON builds. This section repeats the whole suite, single documents and streams, on three builds that run Jsonifier's AVX-512 stage-1 kernel against simdjson's own AVX-512 kernel (`icelake`).

### 9.1 Method

- **Harness:** Json-Performance, branch `generic-parsing-avx512-main`, with the same tests, readers, allocation modes, batch size and adaptive-sampling stage as §6 and §8.3, and the same convergence limits as the x86 builds there (RSE < 5% and shift < 2.5%).
- **Hosts:** the builds ran on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, under Linux `7.0.0-1019-azure` and Windows 10.0.26100. These are shared virtual machines, not the dedicated Core i9-14900KF of §6, which has no AVX-512.
- **Compilers:** Clang 24.0 is the same as in §6. GCC 16.0.1 and MSVC 19.51 are the runner images' compilers and are older than §6's GCC 16.2.1 and MSVC 19.52.
- **Commits:** BenchmarkSuite `27950a9`, Jsonifier `d4727ec` and simdjson `449f38e`, the same as §6. All three builds ran on November 14, 2026. The stage-1 tapes of these builds match simdjson's position for position (§6, *Correctness*).

| Build | Stage-1 kernel | Converged, single documents | Converged, streams |
|---|---|---|---|
| Windows 10.0.26100 / MSVC 19.51 | AVX-512 | 321 / 330 | 20 / 30 |
| Linux 7.0.0 (Azure) / Clang 24.0 | AVX-512 | 330 / 330 | 28 / 30 |
| Linux 7.0.0 (Azure) / GCC 16.0.1 | AVX-512 | 327 / 330 | 28 / 30 |

The CPU, the operating-system images and two of the three compilers differ from §6, so a difference between an AVX-512 result here and the matching AVX2 result in §7 cannot be credited to the instruction set alone. Each comparison below is between the two libraries within one AVX-512 build.

### 9.2 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 20-0-0 | 19-1-0 | 20-0-0 | 18-1-1 | 20-0-0 | 19-0-1 | 19-0-1 | 157-4-4 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 15-2-3 | 20-0-0 | 18-0-2 | 18-0-2 | 154-2-9 |
| Linux / GCC | 4-0-1 | 16-0-4 | 20-0-0 | 18-0-2 | 19-0-1 | 10-1-9 | 20-0-0 | 18-0-2 | 19-0-1 | 144-1-20 |
| **All** | **13-0-2** | **53-2-5** | **60-0-0** | **56-1-3** | **59-0-1** | **43-4-13** | **60-0-0** | **55-0-5** | **56-0-4** | **455-7-33** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 19-1-0 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 159-3-3 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 16-1-3 | 20-0-0 | 17-0-3 | 19-0-1 | 155-1-9 |
| Linux / GCC | 4-0-1 | 14-1-5 | 20-0-0 | 18-1-1 | 18-1-1 | 7-2-11 | 18-1-1 | 18-0-2 | 19-0-1 | 136-6-23 |
| **All** | **13-0-2** | **52-3-5** | **59-1-0** | **55-1-4** | **58-1-1** | **43-3-14** | **58-1-1** | **54-0-6** | **58-0-2** | **450-10-35** |

**Streams:**

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 11-0-1 | 10-1-1 | 27-1-2 |
| **All** | **18-0-0** | **35-0-1** | **34-1-1** | **87-1-2** |

Each cell is wins-ties-losses.

Across the 990 single-document results (12 of them did not converge), Jsonifier won 905, tied 17 and lost 68: 455-7-33 with fresh output objects and 450-10-35 with reused ones. Across the 90 streaming results (14 of them did not converge) it won 87, tied 1 and lost 2.

### 9.3 POD tests

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Bool | +175.5% / +194.3% | +18.4% / +20.6% | +96.7% / +101.1% |
| Double | +147.9% / +75.6% | +19.9% / +26.8% | +60.9% / +65.9% |
| Int64 | +78.5% / +102.5% | +14.1% / +12.2% | +73.1% / +45.5% |
| String | +13.7% / +8.2% | −39.8% / −48.9% | −30.5% / −35.0% |
| Uint64 | +86.1% / +78.2% | +20.0% / +20.1% | +69.0% / +42.1% |

### 9.4 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −35.9% / tie | +12.7% / +19.3% | +3.1% / −1.9% |
| Apache Builds (pretty) | tie / +1.7% | +14.3% / +10.7% | +6.7% / +9.3% |
| Canada (min) | +9.7% (n/c) / +6.2% | +10.1% / +10.0% | +3.5% / +2.6% |
| Canada (pretty) | +7.2% / +8.0% | +3.4% / +5.7% | +5.1% / +4.8% |
| CitmCatalog (min) | +10.5% / +1.0% | +6.1% / +6.5% | +4.6% / +4.9% |
| CitmCatalog (pretty) | +15.0% / +10.6% | +3.2% / +5.2% | +2.7% / +3.6% |
| Discord (min) | +9.7% / +27.7% | +11.4% / +23.4% | +2.4% / tie |
| Discord (pretty) | +15.4% / +28.1% | +16.7% / +20.2% | +3.3% / +11.3% |
| Github Events (min) | +17.8% / +27.1% | +21.9% / +27.3% | +18.3% / +19.5% |
| Github Events (pretty) | +23.7% / +31.5% | +21.2% / +30.9% | +11.3% / +12.8% |
| Google Maps (min) | +16.7% / +22.3% | +26.0% / +28.2% | +5.9% / +0.9% |
| Google Maps (pretty) | +19.1% / +23.4% | +20.9% / +23.1% | +3.4% / +6.7% |
| Instruments (min) | +12.6% / +16.4% | +4.0% / +12.7% | −3.4% / −5.4% |
| Instruments (pretty) | +23.3% / +22.4% | +7.7% / +6.4% | −9.0% / −7.3% |
| Marine IK (min) | +2.7% / tie | +0.8% / +0.5% | +3.5% / +5.0% |
| Marine IK (pretty) | tie / +9.3% (n/c) | +3.3% / +1.8% | +2.5% / +2.7% |
| Random (min) | +11.7% / +24.1% | +13.3% / +24.3% | +3.6% / +1.3% |
| Random (pretty) | +11.8% / +16.2% | +15.1% / +21.7% | +1.5% / +12.4% |
| Twitter (min) | +15.2% / +20.8% | +10.1% / +8.0% | −3.9% / −1.3% |
| Twitter (pretty) | +20.7% / +24.5% | +1.4% / +0.6% | −9.8% / −12.5% |

### 9.5 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +34.8% / +55.9% | +77.1% / +101.3% | +69.0% / +75.1% |
| Apache Builds (pretty) | +30.7% / +49.0% | +82.7% / +100.9% | +51.0% / +67.5% |
| Canada (min) | +3.1% / tie (n/c) | +9.7% / +9.3% | +4.1% / +8.3% |
| Canada (pretty) | +8.8% (n/c) / +6.2% | +8.7% / +11.7% | +5.6% / +6.4% |
| CitmCatalog (min) | +60.0% / +61.0% (n/c) | +98.7% / +99.3% | +90.9% / +98.1% |
| CitmCatalog (pretty) | +64.8% / +61.1% | +88.8% / +88.2% | +85.1% / +78.3% |
| Discord (min) | +287.7% / +315.2% | +347.5% / +390.1% | +275.2% / +304.0% |
| Discord (pretty) | +283.9% / +324.8% | +318.8% / +312.3% | +271.6% / +305.6% |
| Github Events (min) | +93.7% / +112.0% | +105.1% / +116.6% | +64.4% / +65.8% |
| Github Events (pretty) | +88.7% / +103.0% | +89.2% / +104.8% | +56.0% / +62.1% |
| Google Maps (min) | +17.5% / +25.9% | +26.8% / +25.7% | +8.7% / +11.4% |
| Google Maps (pretty) | +27.0% / +24.8% | +13.3% / +16.5% | +5.5% / +9.9% |
| Instruments (min) | +208.0% / +211.7% | +211.2% / +210.7% | +143.2% / +154.0% |
| Instruments (pretty) | +192.5% / +200.4% | +174.5% / +181.9% | +124.4% / +127.8% |
| Marine IK (min) | +42.5% / +35.9% | +81.3% / +76.0% | +74.1% / +80.4% (n/c) |
| Marine IK (pretty) | +37.7% / +41.2% (n/c) | +60.9% / +73.1% | +62.3% (n/c) / +61.0% (n/c) |
| Random (min) | +115.0% / +120.9% (n/c) | +106.7% / +124.6% | +60.0% / +61.1% |
| Random (pretty) | +109.0% / +127.4% (n/c) | +99.8% / +112.0% | +54.3% / +57.2% |
| Twitter (min) | +179.3% / +186.6% (n/c) | +155.4% / +162.0% | +102.2% / +110.9% |
| Twitter (pretty) | +166.8% / +197.6% | +142.2% / +166.3% | +87.2% / +94.5% |

### 9.6 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | tie / −29.4% | +23.0% / +30.8% | +10.8% / +15.3% |
| Apache Builds (pretty) | +11.1% / +12.7% | +17.1% / +24.6% | +3.2% / +10.0% |
| Canada (min) | +101.2% / +104.0% | +21.7% / +22.8% | +21.9% / +25.7% |
| Canada (pretty) | +122.9% / +137.8% | +19.2% / +24.1% | +21.9% / +28.8% |
| CitmCatalog (min) | +46.0% / +51.1% | +17.4% / +17.0% | +42.5% / +38.0% |
| CitmCatalog (pretty) | +77.6% / +75.0% | +11.6% / +9.5% | +19.5% / +25.2% |
| Discord (min) | +11.0% / +15.9% | +4.7% / +1.8% | +2.5% / +6.2% |
| Discord (pretty) | +17.1% / +23.4% | +5.1% / +9.2% | +6.4% / +5.5% |
| Github Events (min) | +55.9% / +69.6% | +31.7% / +31.8% | +20.0% / +18.1% |
| Github Events (pretty) | +58.6% / +71.3% | +23.9% / +30.5% | +14.7% / +16.9% |
| Google Maps (min) | +37.1% / +38.5% | +19.8% / +20.4% | −3.6% / −2.8% |
| Google Maps (pretty) | +42.6% / −21.7% | +19.0% / +19.0% | +2.6% / +1.2% |
| Instruments (min) | +59.7% / +68.2% | +25.3% / +29.1% | +14.7% / +13.5% |
| Instruments (pretty) | +62.7% / +70.5% | +17.1% / +15.5% | +8.4% / +9.4% |
| Marine IK (min) | +53.5% / +46.4% | −6.5% / −5.0% | +45.1% / +53.9% |
| Marine IK (pretty) | +91.7% / +98.0% | +10.4% / +2.9% | +36.2% / +32.1% |
| Random (min) | +46.4% / +46.2% | +29.5% / +35.4% | +4.5% / tie |
| Random (pretty) | +47.2% / +49.3% | +34.4% / +37.6% | −1.4% / +1.4% |
| Twitter (min) | +130.2% / +149.1% | +8.2% / +7.2% | +9.3% / +7.8% |
| Twitter (pretty) | +114.4% / +125.9% | +6.3% / +13.0% | +5.7% / +5.2% |

### 9.7 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +13.9% / +18.3% | +8.3% / +8.2% | +6.8% / +12.1% |
| Apache Builds (pretty) | +1.7% / +11.3% | +1.6% / +14.2% | +1.2% / +0.7% |
| Canada (min) | +50.9% / +46.2% | +0.9% / +0.7% | +28.7% / +28.6% |
| Canada (pretty) | +81.3% / +87.8% | +17.7% / +17.1% | +29.0% / +22.9% |
| CitmCatalog (min) | +29.2% / +36.9% | +11.9% / +8.9% | +20.1% / +19.6% |
| CitmCatalog (pretty) | +49.8% / +55.3% | +13.1% / +12.2% | +22.1% / +15.8% |
| Discord (min) | +52.7% / +79.6% | +42.5% / +49.6% | +29.8% / +36.7% |
| Discord (pretty) | +51.4% / +68.9% | +38.6% / +44.1% | +28.7% / +35.8% |
| Github Events (min) | +75.8% / +95.8% | +51.5% / +56.7% | +47.3% / +47.4% |
| Github Events (pretty) | +86.6% / +103.3% | +50.2% / +53.4% | +39.7% / +41.0% |
| Google Maps (min) | +31.8% / +31.9% | +17.4% / +18.1% | +4.9% / tie |
| Google Maps (pretty) | +37.8% / +39.9% | +11.4% / +19.3% | −2.8% / −2.0% |
| Instruments (min) | +72.3% / +75.8% | +38.6% / +34.3% | +56.9% / +59.0% |
| Instruments (pretty) | +80.7% / +86.5% | +42.9% / +48.1% | +33.3% / +35.5% |
| Marine IK (min) | +65.0% / +66.6% | +58.1% / +64.2% | +76.0% / +73.8% |
| Marine IK (pretty) | +95.1% / +103.5% | +68.0% / +66.4% | +58.2% / +55.8% |
| Random (min) | +63.5% / +71.2% | +35.0% / +32.6% | +14.7% / +15.8% |
| Random (pretty) | +64.1% / +78.4% | +40.9% / +36.3% | +12.0% / +12.9% |
| Twitter (min) | +127.9% / +121.1% | +38.4% / +42.2% | +15.6% / +15.7% |
| Twitter (pretty) | +108.7% / +120.0% | +28.4% / +30.0% | +12.7% / +18.0% |

### 9.8 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +3.7% / +3.1% | +9.8% / +9.1% | +2.0% / +3.8% |
| Apache Builds (pretty) | +7.2% / +7.4% | +9.6% / +8.5% | +1.6% / tie |
| Canada (min) | +4.5% / +4.2% | +8.4% / +5.7% | +2.4% / tie |
| Canada (pretty) | +6.0% / +4.5% | +7.7% / +8.8% | +3.9% / −3.2% |
| CitmCatalog (min) | +4.0% / +3.0% | +2.1% / tie | −4.3% / −2.7% |
| CitmCatalog (pretty) | +7.1% / +9.6% | −3.9% / −3.8% | −9.0% / −3.3% |
| Discord (min) | +2.7% / +25.4% | tie / +20.2% | −5.1% / −0.4% |
| Discord (pretty) | +8.0% / +19.9% | +5.0% / +20.0% | −7.2% / +0.7% |
| Github Events (min) | +11.6% / +13.6% | +7.3% / +9.5% | −12.9% / −6.0% |
| Github Events (pretty) | +9.7% / +10.7% | tie / −1.2% | −11.9% / −12.3% |
| Google Maps (min) | +13.1% / +20.5% | +28.9% / +31.6% | +2.0% / +1.8% |
| Google Maps (pretty) | +16.3% / +21.6% | +27.8% / +24.0% | +2.5% / +0.9% |
| Instruments (min) | +17.5% / +19.5% | +5.5% / +3.4% | +2.3% / +3.2% |
| Instruments (pretty) | +21.2% / +23.0% | +5.9% / +7.6% | −9.6% / −4.4% |
| Marine IK (min) | tie / +2.2% | +12.1% / +5.1% | +0.6% / −1.5% |
| Marine IK (pretty) | +5.4% / +5.9% | +8.0% / +6.8% | tie / −6.5% |
| Random (min) | +16.9% / +20.3% | +15.8% / +25.1% | +5.4% / +3.1% |
| Random (pretty) | +17.2% / +25.4% | +26.8% / +28.8% | +4.8% / +8.2% |
| Twitter (min) | −31.5% / +96.5% | −1.8% / +4.4% | −6.0% / −7.9% |
| Twitter (pretty) | +16.3% / +23.9% | −5.9% / −0.8% | −15.0% / −13.4% |

### 9.9 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +68.1% / +99.5% | +114.1% / +131.7% | +69.6% / +80.9% |
| Apache Builds (pretty) | +65.1% / +90.7% | +98.3% / +116.4% | +52.6% / +65.2% |
| Canada (min) | +3.9% / +5.7% | +6.9% / +8.7% | +2.2% / tie |
| Canada (pretty) | +4.6% / +5.0% | +7.7% / +23.4% | +3.6% / +3.6% |
| CitmCatalog (min) | +53.9% / +59.5% | +83.1% / +90.4% | +72.0% / +81.3% |
| CitmCatalog (pretty) | +67.0% / +56.9% | +80.7% / +83.3% | +56.4% / +71.0% |
| Discord (min) | +265.5% / +330.4% | +319.3% / +386.1% | +233.4% / +277.5% |
| Discord (pretty) | +245.3% / +326.4% | +283.0% / +326.3% | +213.6% / +226.3% |
| Github Events (min) | +37.7% / +40.1% | +46.9% / +42.7% | +10.1% / +7.5% |
| Github Events (pretty) | +32.2% / +39.5% | +21.6% / +37.2% | +1.3% / −1.7% |
| Google Maps (min) | +24.4% / +28.8% | +15.6% / +25.2% | +8.7% / +9.0% |
| Google Maps (pretty) | +30.6% / +27.4% | +21.1% / +23.8% | +2.6% / +6.1% |
| Instruments (min) | +202.1% / +210.4% | +190.6% / +180.0% | +141.0% / +149.5% |
| Instruments (pretty) | +194.1% / +204.3% | +166.8% / +163.7% | +122.1% / +125.2% |
| Marine IK (min) | +33.1% / +34.1% | +60.1% / +73.2% | +53.5% / +52.8% |
| Marine IK (pretty) | +32.8% / +36.6% | +63.1% / +66.3% | +45.9% / +50.6% |
| Random (min) | +101.4% / +125.9% | +101.0% / +145.5% | +66.1% / +77.4% |
| Random (pretty) | +99.1% / +130.9% | +93.8% / +116.7% | +55.5% / +61.8% |
| Twitter (min) | +90.4% / +208.2% | +158.9% / +148.5% | +99.6% / +95.8% |
| Twitter (pretty) | +183.0% / +199.1% | +147.0% / +139.0% | +89.1% / +89.0% |

### 9.10 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +33.4% / +38.9% | +32.1% / +41.3% | +26.1% / +30.3% |
| Apache Builds (pretty) | +37.1% / +42.2% | +34.6% / +28.9% | +13.7% / +20.4% |
| Canada (min) | +65.8% / +70.3% | +24.4% / +17.9% | +21.0% / +25.5% |
| Canada (pretty) | +110.7% / +111.2% | +14.8% / +10.0% | +24.0% / +24.1% |
| CitmCatalog (min) | +65.9% / +81.2% | +17.7% / +18.1% | +18.7% / +24.0% |
| CitmCatalog (pretty) | +94.4% / +103.3% | +10.0% / +19.5% | +17.7% / +20.3% |
| Discord (min) | +37.3% / +41.1% | +17.3% / +26.0% | +5.7% / +8.7% |
| Discord (pretty) | +31.9% / +42.2% | +16.0% / +18.2% | +4.4% / +2.6% |
| Github Events (min) | +50.5% / +61.2% | +21.9% / +23.6% | +5.2% / +5.9% |
| Github Events (pretty) | +60.1% / +49.8% | +11.9% / +13.0% | +2.2% / +7.4% |
| Google Maps (min) | +31.4% / +39.1% | +25.9% / +22.1% | +1.7% / +1.3% |
| Google Maps (pretty) | +38.9% / +46.1% | +18.2% / +20.8% | +4.2% / +5.4% |
| Instruments (min) | +53.5% / −2.6% | +28.8% / +25.6% | +19.1% / +24.0% |
| Instruments (pretty) | +68.9% / +79.4% | +13.0% / +6.4% | +12.5% / +15.8% |
| Marine IK (min) | +36.2% / +43.7% | +5.6% / −4.4% | +25.1% / +22.4% |
| Marine IK (pretty) | −2.6% / +89.2% | +8.0% / +3.6% | +24.5% / +25.9% |
| Random (min) | +41.9% / +46.5% | +38.6% / +36.9% | +4.9% / +6.1% |
| Random (pretty) | +50.2% / +57.1% | +33.6% / +35.0% | +1.1% / +2.1% |
| Twitter (min) | +76.3% / +79.9% | −5.6% / −4.9% | −2.4% / −2.7% |
| Twitter (pretty) | +81.6% / +93.3% | −5.1% / −1.0% | −2.8% / −3.3% |

### 9.11 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +5.5% / +29.4% | +2.3% / +14.2% | +13.2% / +7.1% |
| Apache Builds (pretty) | +20.2% / +26.2% | −0.9% / +7.8% | +5.3% / +5.8% |
| Canada (min) | +38.8% / +38.0% | −1.1% / −3.9% | +26.1% / +32.5% |
| Canada (pretty) | +75.6% / +79.4% | +18.1% / +17.9% | +24.8% / +26.8% |
| CitmCatalog (min) | +61.6% / +55.0% | +11.7% / +20.5% | +17.9% / +17.3% |
| CitmCatalog (pretty) | +73.2% / +68.4% | +13.8% / +15.1% | +13.2% / +12.0% |
| Discord (min) | +73.8% / +90.5% | +74.5% / +77.6% | +60.2% / +59.3% |
| Discord (pretty) | +71.1% / +83.8% | +66.7% / +73.2% | +39.9% / +35.9% |
| Github Events (min) | +64.7% / +77.5% | +29.8% / +40.0% | +24.2% / +18.4% |
| Github Events (pretty) | −15.3% / +75.1% | +29.2% / +30.1% | +11.6% / +20.0% |
| Google Maps (min) | +32.3% / +43.4% | +14.8% / +19.5% | +0.3% / +1.8% |
| Google Maps (pretty) | +37.6% / +39.3% | +15.5% / +16.9% | −5.1% / −4.7% |
| Instruments (min) | +81.3% / +82.8% | +51.6% / +45.8% | +48.0% / +55.2% |
| Instruments (pretty) | +79.1% / +96.4% | +52.7% / +50.9% | +33.4% / +39.8% |
| Marine IK (min) | +73.5% / +69.2% | +55.3% / +61.0% | +63.0% / +63.7% |
| Marine IK (pretty) | +95.6% / +97.8% | +67.0% / +65.5% | +43.6% / +40.3% |
| Random (min) | +59.9% / +69.2% | +44.6% / +50.7% | +19.9% / +19.7% |
| Random (pretty) | +54.3% / +69.9% | +43.7% / +43.8% | +15.8% / +13.6% |
| Twitter (min) | +71.7% / +75.3% | +16.0% / +19.1% | +1.5% / +2.8% |
| Twitter (pretty) | +69.8% / +74.3% | +3.7% / +7.4% | +2.2% / +4.4% |

### 9.12 Streams

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 11-0-1 | 10-1-1 | 27-1-2 |
| **All** | **18-0-0** | **35-0-1** | **34-1-1** | **87-1-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Amazon Cellphones (NDJSON) | +38.6% | +21.0% | +11.1% |
| Large Amazon Cellphones (NDJSON) | +14.9% | +8.9% (n/c) | +0.4% |
| Stream Formats Small (NDJSON) | +36.5% | +17.7% | +12.9% |
| Stream Formats Small (comma) | +157.0% (n/c) | +58.3% | +87.1% |
| Stream Formats Large (NDJSON) | +184.5% | +21.7% | +47.2% |
| Stream Formats Large (comma) | +190.3% | +28.2% | +47.4% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | +4.5% (n/c) | +21.5% | +3.6% |
| Apache Builds (comma) | +11.5% | +25.3% | +11.8% |
| CitmCatalog (NDJSON) | +10.4% | +5.0% | +4.3% |
| CitmCatalog (comma) | +14.4% (n/c) | +15.3% | +21.2% |
| Google Maps (NDJSON) | +22.1% (n/c) | +16.9% | +1.6% |
| Google Maps (comma) | +30.8% | +24.0% | +12.2% |
| Instruments (NDJSON) | +35.0% (n/c) | +27.7% | +0.6% |
| Instruments (comma) | +57.9% (n/c) | +36.1% | +26.7% |
| Random (NDJSON) | +20.7% | +23.9% | +9.1% |
| Random (comma) | +28.9% | +22.5% | +18.2% |
| Twitter (NDJSON) | +21.1% | +1.2% | −13.3% |
| Twitter (comma) | +25.0% | +3.1% | +0.8% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | +8.7% | +22.4% (n/c) | tie (n/c) |
| Apache Builds (comma) | +18.6% | +33.5% | +6.7% |
| CitmCatalog (NDJSON) | +34.5% | +55.8% | +45.0% |
| CitmCatalog (comma) | +45.3% | +70.8% | +63.9% |
| Google Maps (NDJSON) | +23.8% | +14.5% | −4.6% |
| Google Maps (comma) | +23.4% (n/c) | +15.6% | +5.1% (n/c) |
| Instruments (NDJSON) | +81.7% (n/c) | +71.0% | +10.5% |
| Instruments (comma) | +111.2% | +85.7% | +29.1% |
| Random (NDJSON) | +101.6% (n/c) | +111.6% | +48.9% |
| Random (comma) | +120.0% | +101.0% | +61.9% |
| Twitter (NDJSON) | +189.3% | +163.6% | +104.2% |
| Twitter (comma) | +193.2% (n/c) | +162.6% | +114.2% |

### 9.13 What the results show

- **Reverse order wins on every AVX-512 build.** Jsonifier won 237 of the 240 reverse results (2 ties, 1 loss): Linux/Clang went 80-0-0, MSVC 79-1-0 and Linux/GCC 78-1-1. The one loss is prettified Github Events Small on Linux/GCC (reused, −1.7%), and the ties are minified Canada on MSVC (reused, n/c) and minified Canada Small on Linux/GCC (reused). On the documents whose objects have many fields (Discord, Twitter, Instruments) the full-size margin runs from +87.2% (Twitter pretty, Linux/GCC, fresh) to +390.1% (Discord min, Linux/Clang, reused), and the small-document margin from +89.0% to +386.1%.
- **simdjson's AVX-512 kernel does not close the reverse-order gap.** On MSVC, simdjson reads Discord Reverse (pretty, fresh) at 170.6 MB/s and Jsonifier at 654.7 MB/s (+283.9%); on Linux/Clang the figures are 225.3 and 943.7 MB/s. simdjson's rescans are bound by how many keys it walks, not by how fast stage 1 runs, so a wider stage-1 kernel cannot shorten them.
- **Sparse reverse reads stay a strength.** Jsonifier won 117 of the 120 full-size sparse-reverse results (1 tie, 2 losses) and 114 of the 120 small ones (6 losses). MSVC and Linux/Clang won all 40 of their full-size results; every full-size non-win is prettified or minified Google Maps on Linux/GCC (−2.8% fresh and −2.0% reused, prettified; a tie, minified, reused).
- **Linux/Clang is the strongest AVX-512 build.** It went 309-3-18, won all 40 of its full-size in-order results and all 80 of its reverse results, and lost only 2 full-size results outside the POD tests: minified Marine IK read sparse (−6.5% fresh, −5.0% reused). Its other 16 losses are String POD (2) and 14 small-document results (−0.8% to −5.9%).
- **Linux/GCC holds most of the AVX-512 losses:** 43 of the 68. 29 of them are in-order reads (9 at full size, 20 small), against 18 in-order losses for the GCC 16.2.1 build of §7 on the 14900KF. Twitter accounts for 12 of the 43 (−1.3% to −15.0%), then Instruments and Google Maps with 6 each and Github Events with 5. Read in order, Linux/GCC lost Twitter and Instruments at full size in both forms and both runs (−1.3% to −12.5%), and it lost 20 of its 40 small in-order results. Twitter and Instruments win by wide margins when read in reverse on this build (+87.2% and more), so the gap is in reading in order, not in field lookup. Because this build differs from §7's in compiler version as well as CPU, these results alone do not say which of the two is responsible.
- **String POD loses on both Linux AVX-512 builds:** −39.8% fresh and −48.9% reused on Linux/Clang, −30.5% and −35.0% on Linux/GCC. On Linux/Clang simdjson reads it at 1,135.0 MB/s and Jsonifier at 683.8 MB/s (fresh). MSVC won String (+8.2% and +13.7%), and the other four POD tests won on all three builds (+12.2% to +194.3%).
- **MSVC lost 7 of its 330 single-document results, 5 of them by wide margins in one allocation mode only.** Minified Apache Builds read in order lost by −35.9% fresh and tied reused; minified Twitter Small read in order lost by −31.5% fresh and won by +96.5% reused; minified Apache Builds read sparse tied fresh and lost by −29.4% reused; prettified Google Maps read sparse won by +42.6% fresh and lost by −21.7% reused; prettified Github Events Small read sparse in reverse lost by −15.3% fresh and won by +75.1% reused. In each of the five, the same test in the other allocation mode ties or wins. The MSVC runner also produced the six largest fresh-versus-reused differences in Jsonifier's margin among this section's results, and the most unconverged results, so these five are best read with the shared-runner caveat of §9.1; they stay in the tallies. MSVC's other 2 losses are Instruments Small read sparse (minified, reused, −2.6%) and Marine IK Small read sparse (prettified, fresh, −2.6%). It won all 10 POD results and all 40 full-size sparse-reverse results.
- **Streams:** Jsonifier won all 18 Amazon Cellphones and Stream Formats results, including Stream Formats Large on MSVC by +184.5% (NDJSON) and +190.3% (comma-separated), and Amazon Cellphones on MSVC by +38.6%, a test that MSVC loses in §8.4. MSVC and Linux/Clang won all 30 of their streaming tests; Linux/GCC went 27-1-2, losing Twitter read in order as NDJSON (−13.3%) and Google Maps read in reverse as NDJSON (−4.6%), and tying Apache Builds read in reverse as NDJSON. Twitter read in reverse wins on all three builds in both forms (+104.2% to +193.2%).
- **Convergence was weaker on the cloud runners.** 9 of MSVC's 330 single-document results and 10 of its 30 streaming results did not converge, against 1 and 0 for the MSVC build of §6; Linux/GCC left 3 single-document and 2 streaming results unconverged, and Linux/Clang 2 streaming results.

## 10. Limitations

- **Linux/GCC holds the most single-document losses:** 28 of the 85, against 24 on Linux/Clang, 13 each on MSVC and macOS/Clang, and 7 on macOS/GCC. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (−34.1% fresh, −32.3% reused, n/c), then macOS/GCC's minified Marine IK read in order (fresh, −22.8%) and Linux/GCC's minified Canada Small read in order (reused, −18.6%), the largest x86 loss.
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 53 of the 85: Canada 32 (16 on Linux/GCC, 9 on Linux/Clang, 3 each on MSVC and macOS/Clang, 1 on macOS/GCC) and Marine IK 21 (8 on Linux/GCC, 5 on MSVC, 4 on macOS/GCC, 2 each on Linux/Clang and macOS/Clang). There much of the cost is float parsing, not field lookup. The other 32 are Instruments 12 (4 on Linux/Clang, 3 each on Linux/GCC and macOS/Clang, 1 each on MSVC and macOS/GCC), CitmCatalog 8 (5 on Linux/Clang, 2 on MSVC, 1 on Linux/GCC), Google Maps 6 (3 on macOS/Clang, 2 on Linux/Clang, 1 on MSVC), the 4 String POD losses, Discord 1 (MSVC) and Twitter 1 (macOS/GCC).
- **Linux/GCC loses mostly on Canada.** 16 of its 28 losses are Canada, read in order and in reverse at both sizes; its sparse and sparse-reverse Canada reads all win, since there much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 55 of the 85 losses are small documents (18 on Linux/Clang, 16 on Linux/GCC, 11 on MSVC, 7 on macOS/Clang, 3 on macOS/GCC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Six streaming losses, on three builds:** MSVC's Amazon Cellphones (−1.9%) and Large Amazon Cellphones (−3.9%), macOS/Clang's Stream Formats Large as NDJSON (−7.0%) and comma-separated (−15.7%) and its CitmCatalog record stream as NDJSON (−7.3%), and macOS/GCC's Stream Formats Small as NDJSON (−10.9%, n/c).
- **Some results did not converge.** 22 of the 1,650 single-document results did not converge: 13 on macOS/GCC, 8 on macOS/Clang and 1 on Windows/MSVC. 10 of the 150 streaming results did not converge: 8 on macOS/GCC and 2 on macOS/Clang. Linux/Clang and Linux/GCC converged on every single-document and streaming result.
- **The AVX-512 builds ran on shared cloud runners.** §9's three builds used an AMD EPYC 9V45 on shared virtual machines, older GCC and MSVC versions than §6 and different operating-system images, so their results cannot be set directly against §7's to isolate the effect of AVX-512. They converged less often (12 of 990 single-document and 14 of 90 streaming results did not converge, 10 of those streams on MSVC), and the MSVC build shows five losses that flip to ties or wins in the other allocation mode (§9.13). Linux/GCC lost 43 of its 330 AVX-512 single-document results, most of them in-order reads, against 28 for the Linux/GCC build of §7.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate, the key trail and the small documents were added, and MSVC was not swept. The wrap count of 2, the trail capacity of 16 and the direction score's limits (engage at 4, saturate at 8) were not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.

## 11. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces make up the design:

- a stage-1 reader with a dedicated path for inputs shorter than one SIMD step;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,548, tied 17 and lost 85. On reverse reads it won 384 of 400 at both sizes, and on Windows/MSVC it lost 13 of its 330 single-document results, 11 of them on small documents. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (769-13-43 fresh, 779-4-42 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 53 of the 85 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 143, tied 1 and lost 6, and it won 118 of the 120 corpus record-stream results. The Amazon Cellphones streams on MSVC and the Stream Formats reads on macOS hold five of the six losses and are the main remaining streaming targets.

The lead holds against simdjson's own AVX-512 kernel. On three AVX-512 builds, Jsonifier won 905 of 990 single-document results, tied 17 and lost 68, won 237 of 240 reverse results and won 87 of 90 streaming results; a wider stage 1 does not shorten simdjson's rescans. On those builds the target is in-order reading under GCC, which holds 29 of the 68 losses.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 12. Dedication

This paper is dedicated to the author's Little Buddy.

## References

1. simdjson. https://github.com/simdjson/simdjson
2. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
