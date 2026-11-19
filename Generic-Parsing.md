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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,554 of 1,650 single-document results against simdjson On Demand, tied 30 and lost 66. On reverse-order reads it won 387 of 400. On Windows/MSVC it won 318 of 330 single-document results, tied 5 and lost 7.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 145 of 150 results and lost 5, and it won 119 of the 120 corpus record-stream results.

The whole suite was also run on three AVX-512 builds on an AMD EPYC 9V45, against simdjson's own AVX-512 kernel (§9). There Jsonifier won 911 of 990 single-document results, tied 14 and lost 65, won 236 of 240 reverse-order results, and won 82 of 90 streaming results.

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

Accessors return `[[nodiscard]] parse_statuses` and write their result to an out-parameter; they don't wrap results in a `result<T>`. A structural error is stored in the handle and passed along to every handle derived from it. `document::atEnd()` checks that the whole document was consumed. Errors therefore cost one well-predicted branch per access.

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
- **Correctness:** on the measured commits, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, read directly from upstream simdjson's internal stage-1 parser after `stage1()`, with no fork or patch. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `79b3e67` and simdjson `36c5b3c`, the benchmarked commits, on November 17, 2026. The tape checks ran on cloud-hosted CI runners, not on the benchmark hosts: their Linux builds report Azure kernels (`7.0.0-1019-azure`), the harness does not record their CPU model, and their x86 compilers differ from the benchmark builds' (MSVC 19.51 and GCC 16.0.1, against 19.52 and 16.2.1 below; Clang 24.0 is the same), while the macOS tape builds used the same GCC 16.2.0 and Clang 23.1.5, against 23.1.4 below. The AVX-512 builds could not run on the Core i9-14900KF, which has no AVX-512; their benchmarks ran on separate cloud hosts, described in §9. The benchmark runs of §7 and §8 did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-43-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 22 of the 1,650 single-document results and 10 of the 150 streaming results did not converge (see §10). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. Both libraries read integers more slowly under MSVC than under Linux/Clang on the same CPU: simdjson reads Int64 (fresh) at 574.7 MB/s under MSVC against 794.3 MB/s under Linux/Clang, while Jsonifier reads it at 820.0 and 1,546.0 MB/s; for Uint64 (fresh) the figures are 564.0 against 732.6 MB/s for simdjson and 853.7 against 1,542.8 MB/s for Jsonifier.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 169 / 170 | 159 / 160 | 328 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 158 / 170 | 157 / 160 | 315 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 165 / 170 | 160 / 160 | 325 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `71afc62`, Jsonifier `79b3e67` and simdjson `36c5b3c`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. All five builds ran on November 17, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 17-0-3 | 19-0-1 | 19-0-1 | 20-0-0 | 158-2-5 |
| Linux / Clang | 5-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 19-0-1 | 17-0-3 | 20-0-0 | 20-0-0 | 20-0-0 | 159-0-6 |
| Linux / GCC | 5-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 16-0-4 | 18-0-2 | 19-0-1 | 20-0-0 | 156-0-9 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 19-0-1 | 17-1-2 | 18-1-1 | 19-1-0 | 19-0-1 | 18-1-1 | 153-5-7 |
| macOS / Clang | 4-0-1 | 18-2-0 | 20-0-0 | 19-0-1 | 19-1-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 158-3-4 |
| **All** | **24-0-1** | **90-5-5** | **100-0-0** | **98-0-2** | **95-2-3** | **87-1-12** | **96-1-3** | **96-0-4** | **98-1-1** | **784-10-31** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 18-2-0 | 19-0-1 | 19-0-1 | 20-0-0 | 160-3-2 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 18-0-2 | 19-0-1 | 20-0-0 | 20-0-0 | 159-0-6 |
| Linux / GCC | 5-0-0 | 18-0-2 | 19-1-0 | 20-0-0 | 20-0-0 | 17-0-3 | 18-0-2 | 19-0-1 | 20-0-0 | 156-1-8 |
| macOS / GCC | 5-0-0 | 17-2-1 | 18-2-0 | 18-0-2 | 16-2-2 | 18-1-1 | 19-0-1 | 17-3-0 | 15-3-2 | 143-13-9 |
| macOS / Clang | 4-0-1 | 18-0-2 | 20-0-0 | 18-0-2 | 20-0-0 | 16-2-2 | 19-0-1 | 17-1-2 | 20-0-0 | 152-3-10 |
| **All** | **23-0-2** | **91-3-6** | **97-3-0** | **96-0-4** | **95-2-3** | **87-5-8** | **94-0-6** | **92-4-4** | **95-3-2** | **770-20-35** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (22 of them did not converge; see §10), Jsonifier won 1,554, tied 30 and lost 66: 784-10-31 with fresh output objects and 770-20-35 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 197 of 200 full-size reverse results (3 ties, no losses) and 190 of 200 small reverse results (1 tie, 9 losses). Linux/Clang and macOS/Clang went 79-0-1, MSVC 78-0-2, macOS/GCC 76-3-1 and Linux/GCC 75-1-4. Every full-size tie is Canada, all reused: prettified on Linux/GCC and both forms on macOS/GCC (n/c). At small size, Canada Small holds 7 of the 9 losses: Linux/GCC in both forms and both runs (−7.4% to −9.3%), and prettified and reused on Linux/Clang (−0.3%), macOS/GCC (−8.2%) and macOS/Clang (−14.4%). The other 2 are minified Google Maps Small on MSVC (−2.3% fresh, −4.0% reused).
- **Windows/MSVC lost 7 of its 330 single-document results, all on small documents.** Read in order, it lost Canada Small twice (fresh, −0.4% minified and −1.1% prettified) and minified Marine IK Small once (fresh, −1.2%); the other 4 are minified Google Maps Small read in reverse (−2.3% fresh, −4.0% reused) and minified Marine IK Small read sparse (−6.7% fresh, −6.8% reused). Its 5 ties are Marine IK read in order (minified in both runs, prettified fresh), minified Marine IK Small read in order (reused) and prettified CitmCatalog Small read in order (reused). It won all 10 POD results, 78 of its 80 reverse results and 158 of its 160 sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.9–13.4% for Jsonifier and 6.1–10.6% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.5 percentage points on Linux/Clang and Linux/GCC, +2.1 on macOS/GCC, +3.2 on MSVC and +3.7 on macOS/Clang. On MSVC, Discord Reverse (min) goes from +188.4% to +229.6%, and Apache Builds (min) from +11.9% to +18.5%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +94.7% / +102.8% | +94.1% / +93.3% | +129.7% / +129.2% | +49.5% / +67.0% | +9.1% / +28.8% |
| Double | +92.8% / +92.9% | +88.8% / +92.7% | +79.6% / +81.3% | +97.9% / +108.2% | +62.2% / +75.5% |
| Int64 | +42.7% / +47.2% | +94.6% / +100.8% | +81.5% / +82.7% | +58.5% / +63.4% | +40.7% / +49.3% |
| String | +4.1% / +4.4% | +4.3% / −10.0% | +2.5% / +5.8% | +2.3% / +20.5% | −19.0% / −27.6% |
| Uint64 | +51.4% / +43.9% | +110.6% / +119.0% | +79.6% / +83.0% | +59.4% / +55.2% | +49.6% / +39.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +11.9% / +18.5% | +22.6% / +33.1% | +26.2% / +40.4% | +40.9% / +106.8% | +39.4% / +29.2% |
| Apache Builds (pretty) | +8.6% / +14.1% | +16.0% / +21.9% | +22.6% / +34.7% | +39.8% / +81.3% | +19.6% / +30.8% |
| Canada (min) | +4.6% / +9.1% | +4.0% / +4.0% | −4.9% / −4.4% | +4.1% (n/c) / tie | +2.7% / +17.7% (n/c) |
| Canada (pretty) | +8.2% / +5.7% | −2.2% / −1.4% | −3.8% / −6.2% | −4.1% / tie | tie / +3.9% |
| CitmCatalog (min) | +7.4% / +7.5% | +11.7% / +12.7% | +20.4% / +19.4% | +10.3% / +10.4% | +17.9% / +26.0% |
| CitmCatalog (pretty) | +11.3% / +5.2% | +4.9% / +5.7% | +11.2% / +12.7% | +12.0% / +21.7% | +28.7% / −14.6% |
| Discord (min) | +9.6% / +14.9% | +17.3% / +25.7% | +13.5% / +21.3% | +34.1% / +172.4% | +23.4% / +30.3% |
| Discord (pretty) | +6.7% / +13.6% | +11.5% / +18.9% | +14.6% / +11.2% | +27.8% / +142.0% | +18.4% / +20.3% |
| Github Events (min) | +12.0% / +25.4% | +30.2% / +36.7% | +23.9% / +28.5% | +39.2% / +62.5% | +38.6% / +43.1% |
| Github Events (pretty) | +8.6% / +24.3% | +22.3% / +26.2% | +19.7% / +22.6% | +26.9% / +39.0% | +44.5% / +65.3% |
| Google Maps (min) | +12.2% / +14.3% | +24.4% / +28.0% | +28.5% / +30.9% | +30.3% / +33.9% | +18.8% / +22.1% |
| Google Maps (pretty) | +9.5% / +11.2% | +12.3% / +13.9% | +20.4% / +21.4% | +35.2% / +35.0% | +12.3% / +30.2% |
| Instruments (min) | +13.7% / +15.2% | +7.4% / +8.6% | +9.9% / +9.2% | +12.8% / +2.3% | +4.8% / +7.6% |
| Instruments (pretty) | +8.6% / +14.1% | −1.4% / +0.3% | +4.1% / +3.4% | +20.9% / +22.6% | +1.1% / +6.6% |
| Marine IK (min) | tie / tie | +7.1% / +7.1% | +5.2% / +7.0% | tie / +3.7% | tie / −11.6% |
| Marine IK (pretty) | tie / +1.7% | +5.3% / +5.1% | +4.1% / +3.1% | +4.4% / −5.1% | +26.2% (n/c) / +40.6% |
| Random (min) | +18.0% / +23.4% | +34.7% / +45.4% | +37.8% / +46.5% | +42.3% / +58.9% | +31.5% / +23.7% |
| Random (pretty) | +16.8% / +22.4% | +25.0% / +30.7% | +29.1% / +36.0% | +54.6% / +87.0% | +34.0% / +39.6% |
| Twitter (min) | +15.9% / +18.6% | +13.1% / +21.4% | +14.9% / +17.3% | +5.3% / +34.9% | +6.3% / +6.7% |
| Twitter (pretty) | +14.4% / +16.3% | +13.9% / +14.4% | +13.9% / +19.2% | +36.5% / +37.1% | +9.6% / +10.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +48.6% / +68.5% | +60.8% / +80.9% | +59.0% / +77.2% | +118.0% / +136.2% | +88.8% / +91.9% |
| Apache Builds (pretty) | +29.6% / +51.0% | +55.6% / +70.7% | +57.5% / +69.9% | +52.4% / +97.7% | +59.6% / +107.1% |
| Canada (min) | +9.2% / +10.6% | +5.0% / +6.2% | +0.6% / +1.1% | +7.9% / tie (n/c) | +15.2% / +3.8% |
| Canada (pretty) | +12.0% / +8.6% | +4.2% / +2.6% | +2.4% / tie | +2.9% / tie (n/c) | +8.0% / +23.9% |
| CitmCatalog (min) | +37.4% / +48.2% | +68.1% / +72.6% | +63.1% / +66.6% | +93.7% / +65.2% (n/c) | +88.4% / +85.2% |
| CitmCatalog (pretty) | +32.7% / +45.4% | +57.9% / +62.7% | +56.2% / +61.8% | +68.5% / +79.3% | +43.4% / +54.2% |
| Discord (min) | +188.4% / +229.6% | +241.4% / +271.5% | +209.1% / +229.7% | +294.7% / +294.4% | +318.1% / +411.1% |
| Discord (pretty) | +167.1% / +189.1% | +203.4% / +224.0% | +189.9% / +204.8% | +215.3% (n/c) / +238.8% | +289.4% / +313.9% |
| Github Events (min) | +93.9% / +109.8% | +120.8% / +136.0% | +93.6% / +106.8% | +142.1% / +103.9% | +158.3% / +198.5% |
| Github Events (pretty) | +75.1% / +95.1% | +90.1% / +102.3% | +79.5% / +90.4% | +124.5% / +75.2% | +145.0% / +133.8% |
| Google Maps (min) | +8.4% / +8.7% | +14.0% / +14.0% | +7.8% / +11.6% | +64.4% / +32.5% | +14.5% / +18.8% |
| Google Maps (pretty) | +0.6% / +1.8% | +10.0% / +10.1% | +9.4% / +10.3% | +16.0% / +5.7% | +5.7% / +15.7% |
| Instruments (min) | +169.3% / +182.0% | +160.9% / +166.0% | +134.3% / +137.1% | +145.1% / +155.6% | +188.2% / +218.1% |
| Instruments (pretty) | +132.5% / +150.8% | +128.4% / +135.0% | +106.3% / +105.5% | +94.7% / +97.5% | +194.7% / +199.9% |
| Marine IK (min) | +32.9% / +32.0% | +48.3% / +50.1% | +50.1% / +50.5% | +46.4% / +50.7% (n/c) | +52.2% (n/c) / +36.9% |
| Marine IK (pretty) | +34.2% / +34.7% | +44.6% / +49.1% | +50.0% / +51.3% | +47.1% / +31.6% | +55.6% / +76.2% (n/c) |
| Random (min) | +61.0% / +73.2% | +83.5% / +96.8% | +67.3% / +75.1% | +72.0% / +72.6% | +93.5% / +134.1% |
| Random (pretty) | +51.4% / +48.2% | +66.8% / +76.9% | +56.9% / +62.3% | +58.6% / +85.9% | +111.7% / +99.6% |
| Twitter (min) | +153.0% / +174.0% | +145.8% / +161.4% | +112.3% / +120.3% | +163.9% / +146.7% (n/c) | +202.7% / +201.6% |
| Twitter (pretty) | +128.5% / +150.4% | +124.7% / +139.5% | +95.5% / +98.6% | +97.5% / +98.7% | +209.1% / +239.2% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.7% / +27.0% (n/c) | +32.9% / +43.9% | +39.6% / +57.0% | +66.6% / +66.3% | +41.3% / +64.2% |
| Apache Builds (pretty) | +12.7% / +16.1% | +23.6% / +24.9% | +25.2% / +39.4% | +40.5% / +62.4% | +7.2% / +20.3% |
| Canada (min) | +30.7% / +30.0% | +12.2% / +14.1% | +19.3% / +17.5% | +10.5% / +13.6% | +36.5% / +42.3% (n/c) |
| Canada (pretty) | +30.4% / +29.9% | +9.4% / +8.7% | +16.8% / +17.8% | +13.6% / +26.1% | +24.4% / +29.4% |
| CitmCatalog (min) | +15.8% / +23.5% | +7.5% / +8.8% | +9.2% / +7.9% | +17.3% / +6.3% (n/c) | +7.0% / +39.0% |
| CitmCatalog (pretty) | +15.0% / +18.1% | +3.4% / +0.6% | +8.8% / +8.2% | +25.9% / −4.4% | +15.0% / −5.7% |
| Discord (min) | +5.4% / +10.4% | +15.4% / +19.8% | +15.3% / +17.4% | +1.8% / +28.2% | +21.5% / +24.7% |
| Discord (pretty) | +3.1% / +6.0% | +20.0% / +18.3% | +11.0% / +10.9% | +30.5% / +16.6% | +8.9% / +8.4% |
| Github Events (min) | +17.7% / +24.9% | +15.4% / +18.1% | +19.0% / +22.8% | +37.4% / +29.9% | +20.9% / +21.1% |
| Github Events (pretty) | +10.0% / +12.0% | +6.0% / +7.0% | +10.4% / +12.0% | +16.5% / +13.7% | −4.3% / +4.1% |
| Google Maps (min) | +6.7% / +7.0% | +14.0% / +14.7% | +16.4% / +19.0% | +19.4% / +17.6% | +16.3% / +16.3% |
| Google Maps (pretty) | +7.9% / +8.4% | +3.0% / +3.7% | +7.4% / +9.6% | +21.7% / +21.2% | +13.0% / +9.7% |
| Instruments (min) | +14.0% / +18.0% | +17.5% / +19.4% | +10.2% / +11.5% | +12.4% / +5.1% | +38.8% / +18.0% |
| Instruments (pretty) | +11.4% / +12.3% | +7.2% / +7.1% | +2.0% / +3.9% | +14.1% / +11.6% | +3.2% / −3.3% |
| Marine IK (min) | +16.4% / +15.5% | +8.3% / +6.3% | +18.1% / +18.6% | −12.2% / −9.7% | +17.9% / +36.1% |
| Marine IK (pretty) | +13.0% / +14.5% | +4.6% / +2.3% | +7.9% / +7.8% | +4.3% / +8.2% | +31.2% / +42.3% |
| Random (min) | +23.2% / +24.6% | +25.8% / +28.8% | +27.4% / +31.0% | +45.8% / +24.1% | +44.7% / +49.6% |
| Random (pretty) | +25.6% / +21.7% | +15.8% / +16.2% | +13.7% / +15.5% | +33.7% / +52.7% | +33.3% / +31.4% |
| Twitter (min) | +46.2% / +51.6% | +16.7% / +16.1% | +12.0% / +10.6% | +33.8% / +25.6% | +26.7% / +29.5% |
| Twitter (pretty) | +36.8% / +40.7% | +8.3% / +9.9% | +5.3% / +5.7% | +4.5% / +23.4% | +5.0% / +14.0% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.9% / +22.9% | +19.5% / +21.6% | +18.3% / +22.9% | +31.5% / +49.9% | +28.5% / +19.8% |
| Apache Builds (pretty) | +9.3% / +8.9% | +11.4% / +12.5% | +12.8% / +14.0% | +18.1% / +22.9% | +19.6% / +35.5% |
| Canada (min) | +20.5% / +27.0% | +20.5% / +22.0% | +12.9% / +16.9% | +10.3% / +15.2% | +33.8% / +42.7% |
| Canada (pretty) | +23.0% / +19.4% | +7.3% / +7.2% | +14.2% / +15.3% | tie (n/c) / tie | +31.0% / +36.4% |
| CitmCatalog (min) | +6.2% / +8.4% | +2.9% / +2.6% | +3.1% / +1.9% | +4.0% (n/c) / tie | tie / +6.9% |
| CitmCatalog (pretty) | +8.7% / +11.3% | −3.0% / −1.1% | +1.4% / +0.8% | −6.2% / −2.0% | +11.6% / +26.3% |
| Discord (min) | +35.8% / +45.2% | +33.5% / +42.1% | +40.0% / +48.8% | +60.3% / +79.5% | +78.5% / +70.1% |
| Discord (pretty) | +20.2% / +23.9% | +28.4% / +32.0% | +32.8% / +37.5% | +5.5% / +46.3% | +41.9% / +67.4% |
| Github Events (min) | +36.2% / +43.9% | +34.6% / +37.4% | +33.5% / +38.0% | +54.5% / +63.1% | +61.6% / +42.9% |
| Github Events (pretty) | +29.0% / +33.7% | +25.2% / +25.4% | +28.3% / +31.1% | +8.9% / +59.2% | +25.0% / +47.5% |
| Google Maps (min) | +17.1% / +21.0% | +11.0% / +11.4% | +8.0% / +8.4% | −11.8% / −9.1% | +18.0% / +16.8% |
| Google Maps (pretty) | +14.5% / +15.3% | +3.8% / +4.6% | +4.6% / +4.3% | +19.5% / +7.1% | +12.9% / +12.1% |
| Instruments (min) | +33.0% / +42.1% | +27.2% / +31.0% | +30.5% / +32.2% | +6.1% / +34.3% | +38.3% / +40.4% |
| Instruments (pretty) | +25.5% / +26.6% | +18.1% / +19.4% | +20.6% / +21.9% | +13.7% / +13.7% | +24.1% / +25.8% |
| Marine IK (min) | +36.8% / +39.1% | +39.1% / +33.8% | +37.2% / +42.5% | +26.6% / +17.7% | +56.9% / +39.1% |
| Marine IK (pretty) | +36.6% / +33.1% | +23.3% / +22.9% | +30.9% / +31.4% | +27.3% / +19.5% | +34.7% / +36.6% |
| Random (min) | +27.1% / +31.2% | +28.2% / +28.8% | +34.9% / +35.0% | +67.9% / +63.6% | +55.7% / +51.6% |
| Random (pretty) | +20.4% / +24.6% | +17.1% / +19.3% | +18.6% / +22.0% | +21.3% (n/c) / +29.9% (n/c) | +42.9% / +45.4% |
| Twitter (min) | +53.8% / +57.4% | +27.1% / +29.4% | +19.3% / +20.8% | +27.8% / +22.7% | +19.9% / +28.5% |
| Twitter (pretty) | +48.1% / +46.0% | +19.3% / +19.2% | +15.0% / +14.9% | +20.9% / +20.7% | +21.3% / +22.5% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.2% / +24.6% | +17.2% / +26.6% | +25.8% / +32.1% | +26.5% / +60.9% | +4.9% / +20.6% |
| Apache Builds (pretty) | +5.6% / +18.0% | +16.2% / +20.4% | +21.4% / +28.4% | +40.3% / +37.0% | +13.7% / +23.6% |
| Canada (min) | −0.4% / +0.6% | +2.3% / +0.7% | −10.2% / −12.7% | +2.4% / +5.6% | +6.1% / tie |
| Canada (pretty) | −1.1% / +2.8% | −4.2% / −4.6% | −11.4% / −14.4% | +3.8% / −2.2% | −8.3% / −9.8% |
| CitmCatalog (min) | +3.6% / +0.7% | +1.8% / +1.4% | +0.6% / +2.6% | +16.1% / tie | +2.4% / +7.5% |
| CitmCatalog (pretty) | +0.8% / tie | −1.3% / +0.6% | −0.5% / +2.9% | +13.5% / +12.5% | +8.1% / +10.1% |
| Discord (min) | +2.0% / +16.6% | +6.6% / +16.7% | +6.4% / +12.0% | +22.9% / +110.4% | +12.8% / +35.2% |
| Discord (pretty) | +1.6% / +7.1% | +2.6% / +9.1% | +3.0% / +8.4% | +20.1% / +100.4% | +7.5% / +25.8% |
| Github Events (min) | +8.8% / +11.9% | +7.5% / +7.9% | +1.6% / +0.9% | +8.0% / +14.2% | +4.2% / +15.3% |
| Github Events (pretty) | +4.5% / +3.9% | +2.3% / +3.8% | +1.4% / +0.6% | +31.2% / +13.4% | +16.9% / +21.7% |
| Google Maps (min) | +18.8% / +24.8% | +21.9% / +22.1% | +26.9% / +28.4% | +32.5% / +52.8% | +13.3% / +11.0% |
| Google Maps (pretty) | +11.2% / +18.5% | +11.7% / +13.6% | +19.9% / +18.9% | +13.6% / +25.6% (n/c) | +11.3% / +16.9% |
| Instruments (min) | +12.8% / +13.6% | +2.5% / +3.4% | +1.9% / +3.0% | tie / +23.6% | +8.7% / +14.2% |
| Instruments (pretty) | +9.2% / +7.8% | −6.0% / −5.6% | −1.6% / −2.4% | −6.0% / +43.4% | +16.3% / −8.3% |
| Marine IK (min) | −1.2% / tie | +3.4% / +4.2% | +2.3% / +0.5% | +10.0% / +7.7% | +10.9% / +5.8% |
| Marine IK (pretty) | +0.4% / +1.1% | +4.7% / +6.0% | +2.6% / +1.0% | +2.8% / +22.6% | +22.4% / +10.2% |
| Random (min) | +11.9% / +15.2% | +17.8% / +21.6% | +23.4% / +31.5% | +57.7% / +31.8% | +39.1% / +38.4% |
| Random (pretty) | +10.9% / +12.9% | +9.2% / +15.7% | +19.7% / +26.3% | +53.8% / +52.0% | +26.1% / +28.3% |
| Twitter (min) | +9.1% / +18.5% | +7.8% / +10.9% | +6.1% / +9.3% | +33.0% / +43.2% | +12.8% / +21.6% |
| Twitter (pretty) | +6.9% / +14.5% | +5.1% / +7.4% | +5.7% / +8.8% | +24.1% / +42.4% | +10.0% / tie |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +47.5% / +88.1% | +75.4% / +90.1% | +76.2% / +81.2% | +81.8% / +124.2% | +94.4% / +164.2% |
| Apache Builds (pretty) | +42.8% / +75.0% (n/c) | +61.6% / +71.2% | +67.0% / +72.9% | +63.5% / +77.4% | +99.6% / +99.2% |
| Canada (min) | +1.8% / +0.7% | +5.1% / +4.3% | −7.4% / −8.3% | +15.0% / +17.4% | +3.2% / +13.2% |
| Canada (pretty) | +3.1% / +1.0% | +0.1% / −0.3% | −8.1% / −9.3% | tie / −8.2% | +26.3% / −14.4% |
| CitmCatalog (min) | +57.8% / +61.7% | +59.3% / +66.1% | +52.1% / +57.0% | +68.1% / +66.8% | +69.8% / +92.1% |
| CitmCatalog (pretty) | +46.2% / +53.7% | +44.5% / +50.8% | +43.9% / +48.6% | +50.1% / +41.0% | +58.2% / +70.6% |
| Discord (min) | +164.4% / +223.5% | +225.0% / +264.6% | +185.2% / +212.0% | +273.5% / +274.0% | +326.0% / +382.0% |
| Discord (pretty) | +149.0% / +197.0% | +196.4% / +220.2% | +167.2% / +194.1% | +185.0% / +275.1% | +280.1% / +344.2% |
| Github Events (min) | +22.4% / +29.1% | +37.5% / +42.9% | +23.2% / +26.5% | +40.0% / +39.6% | +60.9% / +74.3% |
| Github Events (pretty) | +19.0% / +22.2% | +21.9% / +23.8% | +12.5% / +16.3% | +25.1% / +31.4% | +48.8% / +51.1% |
| Google Maps (min) | −2.3% / −4.0% | +13.4% / +13.3% | +9.6% / +9.4% | +53.1% / +27.5% | +20.8% / +21.1% |
| Google Maps (pretty) | +6.6% / +5.5% | +9.7% / +6.8% | +7.6% / +7.2% | +19.9% / +12.7% | +11.8% / +21.1% |
| Instruments (min) | +145.4% / +154.6% | +142.8% / +151.6% | +130.9% / +132.1% | +154.1% / +154.1% | +206.3% / +246.7% |
| Instruments (pretty) | +127.8% / +135.1% | +117.4% / +124.8% | +105.5% / +106.8% | +100.5% / +129.0% | +199.9% / +222.2% |
| Marine IK (min) | +27.2% / +26.3% | +35.4% / +36.4% | +33.7% / +35.0% | +44.7% / +69.8% | +54.6% / +68.2% |
| Marine IK (pretty) | +24.9% / +27.2% | +32.2% / +32.7% | +30.5% / +30.3% | +16.9% / +33.2% | +47.3% / +46.4% |
| Random (min) | +55.9% / +64.8% | +76.5% / +88.3% | +60.1% / +70.1% | +98.9% / +118.5% | +125.5% / +133.3% |
| Random (pretty) | +41.3% / +49.8% | +59.5% / +68.1% | +52.6% / +59.4% | +80.4% / +76.7% | +92.6% / +119.3% |
| Twitter (min) | +128.3% / +143.4% | +137.9% / +137.6% | +102.4% / +109.5% | +140.7% / +134.2% | +231.6% / +232.3% |
| Twitter (pretty) | +113.9% / +133.1% | +121.8% / +127.3% | +87.6% / +91.4% | +121.8% / +90.4% | +216.1% / +222.1% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +18.6% / +30.4% | +30.4% / +31.6% | +38.5% / +45.6% | +62.5% / +78.1% | +28.5% / +35.6% |
| Apache Builds (pretty) | +9.4% / +15.0% | +20.5% / +21.6% | +25.2% / +30.4% | +54.7% / +59.6% | +9.2% / +21.5% |
| Canada (min) | +33.6% / +36.7% | +13.1% / +13.7% | +11.5% / +9.6% | +24.4% / +26.1% | +28.0% / +8.6% |
| Canada (pretty) | +29.5% / +28.2% | +14.5% / +14.8% | +11.9% / +13.2% | +13.0% / +25.7% | +2.8% / +2.7% |
| CitmCatalog (min) | +18.4% / +24.0% | +2.0% / +3.5% | +6.3% / +8.2% | +4.3% / +5.1% | +7.7% / +9.7% |
| CitmCatalog (pretty) | +10.6% / +14.9% | +0.5% / +3.1% | +6.8% / +6.5% | +17.0% / +28.6% | +9.2% / +9.5% |
| Discord (min) | +5.9% / +7.4% | +11.9% / +18.8% | +9.1% / +10.4% | +32.3% / +26.5% | +17.9% / +23.0% |
| Discord (pretty) | +1.6% / +2.8% | +7.2% / +7.3% | +4.8% / +6.3% | +14.4% / +16.2% | +7.6% / +9.0% |
| Github Events (min) | +16.6% / +19.1% | +8.6% / +12.3% | +15.6% / +14.3% | +34.9% / +14.9% | +8.5% / +14.8% |
| Github Events (pretty) | +6.5% / +5.1% | +2.4% / +4.7% | +10.8% / +5.9% | +4.9% / tie (n/c) | +10.3% / +8.8% |
| Google Maps (min) | +2.1% / +7.7% | +6.0% / +11.5% | +13.7% / +13.5% | +30.4% / tie | +18.6% / +14.0% |
| Google Maps (pretty) | +4.6% / +5.2% | +0.2% / +0.8% | +6.5% / +6.2% | +10.4% / +21.2% | +17.5% / +8.0% |
| Instruments (min) | +1.5% / +4.8% | +7.6% / +7.7% | +2.7% / +3.9% | −6.0% / tie | −1.5% / +11.9% |
| Instruments (pretty) | +3.5% / +5.7% | +3.0% / +4.1% | −2.0% / −0.6% | +5.4% / +23.3% | +3.2% / −0.8% |
| Marine IK (min) | −6.7% / −6.8% | +4.4% / +5.4% | +6.1% / +6.4% | +9.6% / +10.3% | +13.6% / +4.2% |
| Marine IK (pretty) | +9.9% / +9.4% | +2.6% / +3.2% | +4.1% / +5.0% | +10.4% / +12.5% | +2.8% / −4.5% |
| Random (min) | +3.9% / +7.1% | +15.3% / +16.8% | +18.6% / +21.9% | +8.8% / +39.9% | +23.6% / +28.6% |
| Random (pretty) | +2.2% / +5.4% | +6.4% / +7.8% | +7.6% / +8.2% | +24.8% / +38.4% | +9.5% / +15.2% |
| Twitter (min) | +18.4% / +21.8% | +9.6% / +4.2% | +1.4% / +4.6% | +25.5% / +19.5% | +6.5% / +10.3% |
| Twitter (pretty) | +20.4% / +22.5% | +6.3% / +4.9% | +1.9% / +1.0% | +7.5% / +17.1% | +7.6% / tie |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +8.2% / +13.8% | +17.9% / +18.2% | +12.6% / +15.7% | +39.1% / +85.5% | +21.2% / +24.9% |
| Apache Builds (pretty) | +4.1% / +4.2% | +9.1% / +6.7% | +7.5% / +10.0% | +35.1% / −6.5% | +11.4% / +14.8% |
| Canada (min) | +26.8% / +25.5% | +12.0% / +20.5% | +27.3% / +25.7% | +3.3% / +15.3% | +4.2% / +5.7% |
| Canada (pretty) | +24.6% / +21.5% | +20.6% / +22.4% | +25.8% / +26.2% | +13.2% / tie | +10.2% / +2.6% |
| CitmCatalog (min) | +12.5% / +15.8% | +4.9% / +5.3% | +5.6% / +4.5% | +21.1% / tie | +11.1% / +4.2% |
| CitmCatalog (pretty) | +15.9% / +16.1% | +0.7% / +1.0% | +0.4% / +1.8% | −6.2% / +4.7% | +7.4% / +10.0% |
| Discord (min) | +25.3% / +32.4% | +39.2% / +39.8% | +30.1% / +35.7% | +53.8% / +49.1% | +74.1% / +76.8% |
| Discord (pretty) | +13.8% / +17.4% | +27.8% / +34.5% | +24.9% / +28.1% | tie (n/c) / +11.4% | +48.3% / +51.2% |
| Github Events (min) | +28.0% / +28.9% | +28.2% / +27.4% | +18.4% / +18.7% | +16.5% / +36.0% | +61.7% / +40.7% |
| Github Events (pretty) | +18.8% / +19.7% | +14.3% / +19.6% | +15.3% / +12.0% | +13.5% / +19.7% | +25.7% / +54.7% |
| Google Maps (min) | +13.1% / +14.4% | +7.8% / +8.4% | +4.7% / +4.2% | +7.1% / −12.6% | +8.0% / +15.2% |
| Google Maps (pretty) | +7.1% / +6.9% | +0.7% / +1.4% | +1.0% / +2.0% | +9.2% / +31.2% | +14.0% / +16.2% |
| Instruments (min) | +24.1% / +28.2% | +26.5% / +29.7% | +23.1% / +26.3% | +26.9% / +24.2% | +32.3% / +41.8% |
| Instruments (pretty) | +19.6% / +23.7% | +17.2% / +13.9% | +20.5% / +19.0% | +43.7% / +11.5% | +23.0% / +47.4% |
| Marine IK (min) | +16.3% / +17.0% | +27.6% / +28.2% | +37.2% / +35.2% | +50.2% / +69.0% | +30.7% / +36.1% |
| Marine IK (pretty) | +29.3% / +24.6% | +29.4% / +30.1% | +34.7% / +37.3% | +38.7% / +28.6% | +18.1% / +18.4% |
| Random (min) | +16.1% / +19.0% | +22.1% / +22.2% | +29.0% / +28.4% | +24.7% / +35.1% | +25.3% / +54.2% |
| Random (pretty) | +8.6% / +11.6% | +11.7% / +13.3% | +19.1% / +19.0% | +27.2% / +14.1% | +38.0% / +32.1% |
| Twitter (min) | +29.7% / +31.8% | +16.8% / +14.2% | +14.5% / +13.9% | +12.0% / +14.1% | +24.4% / +22.6% |
| Twitter (pretty) | +26.5% / +30.0% | +10.2% / +10.3% | +11.8% / +12.5% | +7.2% / tie | +11.9% / +12.5% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 5.1x faster at full size: +94.7% (Instruments pretty, macOS/GCC, fresh) up to +411.1% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +87.6% to +382.0%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from a tie (minified Canada on macOS/GCC, reused) to +64.4%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events almost never loses.** It won 158 of its 160 results, tied 1 (prettified Github Events Small read sparse on macOS/GCC, reused, n/c) and lost 1 (prettified, read sparse on macOS/Clang, fresh, −4.3%). Read in reverse at full size it wins on every build (+75.1% to +198.5%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 190 of 200 full-size sparse-reverse results across both allocation modes (4 ties, 6 losses), and 193 of 200 small ones (4 ties, 3 losses). MSVC, Linux/GCC and the small-document reads on Linux/Clang and macOS/Clang won every result. The full-size losses are prettified CitmCatalog on Linux/Clang (−3.0% fresh, −1.1% reused) and macOS/GCC (−6.2% fresh, −2.0% reused) and minified Google Maps on macOS/GCC (−11.8% fresh, −9.1% reused). The small losses are all on macOS/GCC: prettified Apache Builds Small (reused, −6.5%), prettified CitmCatalog Small (fresh, −6.2%) and minified Google Maps Small (reused, −12.6%). 7 of the 8 ties are on macOS/GCC; the other is minified CitmCatalog on macOS/Clang (fresh).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 240.0 MB/s on MSVC against 297.3 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 641.2 and 901.9 MB/s. Every MSVC full-size sparse result is a win, by +3.1% to +51.6%, and every full-size sparse-reverse result by +6.2% to +57.4%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 194 of 200 full-size sparse results (no ties, 6 losses) and 188 of 200 small ones (4 ties, 8 losses). MSVC, Linux/Clang and Linux/GCC won all 40 of their full-size results. The full-size losses are all on macOS: minified Marine IK on macOS/GCC (−12.2% fresh, −9.7% reused), prettified CitmCatalog on macOS/GCC and macOS/Clang (reused, −4.4% and −5.7%), and prettified Github Events (fresh, −4.3%) and Instruments (reused, −3.3%) on macOS/Clang. Instruments Small holds 5 of the 8 small losses: prettified on Linux/GCC in both runs (−2.0% and −0.6%) and on macOS/Clang (reused, −0.8%), and minified on macOS/Clang and macOS/GCC (fresh, −1.5% and −6.0%). The other 3 are minified Marine IK Small on MSVC (−6.7% fresh, −6.8% reused) and prettified Marine IK Small on macOS/Clang (reused, −4.5%). 3 of the 4 ties are on macOS/GCC. The largest loss is minified Marine IK on macOS/GCC (fresh, −12.2%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 181, tied 8 and lost 11. MSVC went 37-3-0, Linux/Clang 37-0-3, Linux/GCC 36-0-4, macOS/Clang 36-2-2 and macOS/GCC 35-3-2 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 174, tied 6 and lost 20. macOS/GCC went 36-2-2, MSVC and macOS/Clang 35-2-3, Linux/Clang 35-0-5 and Linux/GCC 33-0-7. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's are prettified Canada Small (−4.2% and −4.6%), prettified Instruments Small (−6.0% and −5.6%) and prettified CitmCatalog Small (fresh, −1.3%), MSVC's (Canada Small and Marine IK Small) run from −0.4% to −1.2%, macOS/GCC's are prettified Instruments Small (fresh, −6.0%) and prettified Canada Small (reused, −2.2%), macOS/Clang's are prettified Canada Small (−8.3% fresh, −9.8% reused) and prettified Instruments Small (reused, −8.3%), and Linux/GCC's run from −0.5% to −14.4%, led by Canada. The largest is prettified Canada Small on Linux/GCC (reused, −14.4%).
- **In-order losses at full size fall mostly on two documents.** Linux/GCC lost Canada in both forms and both runs (−3.8% to −6.2%), and Linux/Clang lost prettified Canada in both runs (−2.2% and −1.4%) and prettified Instruments (fresh, −1.4%). macOS/GCC lost prettified Canada (fresh, −4.1%) and prettified Marine IK (reused, −5.1%), and macOS/Clang lost prettified CitmCatalog (reused, −14.6%) and minified Marine IK (reused, −11.6%). MSVC lost none. Canada and Marine IK hold 9 of the 11; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 47-0-3 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+9.1% to +129.7%, +62.2% to +108.2%, +40.7% to +100.8% and +39.2% to +119.0%). String is the weakest: 7 wins (+2.3% to +20.5%) and 3 losses, on Linux/Clang (reused, −10.0%) and macOS/Clang (−19.0% fresh, −27.6% reused).

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
- **Documents cut by the window edge.** No document is ever parsed from two windows. The next window starts at the first byte of the first document that the current window could not complete, and a document larger than a whole window returns `parse_statuses::capacity`, as in simdjson.
- **Comma-separated streams.** In comma mode, a comma between top-level documents is accepted as a separator. A leading or doubled comma is a `tape_error`, and a trailing comma is `trailing_content`. A comma that falls at the start of a new window is still consumed as a separator.
- **Reporting.** The stream exposes `truncatedBytes()` for an unfinished final document, and each iterator exposes `currentIndex()` and `source()` for the current document's offset and raw text. After an error, the stream yields that error once and then ends.
- **Lifetime.** As with simdjson, advancing the stream invalidates the previous document; the only thing that survives is a zero-copy view of an unescaped string, which points into the caller's buffer.

### 8.2 Finding where documents end

The simplest way to find each document's end is to walk its structurals and count depth before handing the document out. That bounds every later tape scan to the document, but a reader that consumes the document in order walks the same structurals again, so every in-order document pays for two passes over its part of the tape.

The whitespace-delimited mode finds boundaries without that pass:

- **A local rule marks document starts.** Inside any container, every value is preceded on the tape by `[`, `{`, `,` or `:`. So in a whitespace-delimited stream, a structural that can start a value and directly follows a closer or a scalar begins a new top-level document. No depth needs to be tracked to recognize it.
- **One short backward scan per window.** Scanning backward from the end of the window's tape, the first position that satisfies the rule is the start of the window's last document. Everything before it is complete. That last document is deferred to the next window unless this is the final window; only when a window holds a single document, or for the final document of the final window, is a depth walk used to decide completeness. The scan usually stops within a few structurals of the window's end.
- **The reader tells the stream where the document ended.** When the caller advances, the parser's state already records how far the reads went. If the cursor is still inside the document, the stream finishes the open containers from the cursor. For an in-order reader, whose cursor rests just before the closing brace, that is about one step. If the cursor is past a closed root value, it is the end. Only a document the caller never touched is skipped from its start.

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is below its own NDJSON throughput on the same records in 17 of the 18 build and stream pairs, by 3.1% to 12.6% outside MSVC; the exception is Apache Builds on MSVC, where comma mode runs 1.7% faster.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 29 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 27 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 24 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| Linux / Clang | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| Linux / GCC | 6-0-0 | 12-0-0 | 11-0-1 | 29-0-1 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| **All** | **26-0-4** | **60-0-0** | **59-0-1** | **145-0-5** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +1.3% | +25.7% | +29.2% | +23.6% | +13.9% |
| Large Amazon Cellphones (NDJSON) | −2.3% | +18.4% | +16.4% | +13.1% | +10.4% |
| Stream Formats Small (NDJSON) | +9.8% | +23.3% | +23.6% | +8.7% | +11.5% |
| Stream Formats Small (comma) | +57.5% | +113.6% | +85.4% | +87.0% (n/c) | +42.3% (n/c) |
| Stream Formats Large (NDJSON) | +9.2% | −2.0% | +6.8% | +11.6% | −5.7% |
| Stream Formats Large (comma) | +14.7% | +7.6% | +11.7% | +16.4% | −5.2% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +10.5% | +25.5% | +35.0% | +60.0% | +47.4% |
| Apache Builds (comma) | +22.6% | +41.7% | +52.3% | +50.9% | +42.3% |
| CitmCatalog (NDJSON) | +7.2% | +12.0% | +18.2% | +7.0% | +9.5% |
| CitmCatalog (comma) | +19.9% | +36.4% | +42.1% | +35.2% | +24.1% |
| Google Maps (NDJSON) | +19.1% | +26.1% | +30.5% | +30.6% | +24.7% |
| Google Maps (comma) | +24.7% | +42.0% | +47.5% | +33.4% | +25.0% |
| Instruments (NDJSON) | +17.7% | +16.5% | +18.6% | +36.1% | +10.3% |
| Instruments (comma) | +34.0% | +41.6% | +53.7% | +63.7% | +30.9% |
| Random (NDJSON) | +23.2% | +30.7% | +34.5% | +60.8% (n/c) | +66.4% |
| Random (comma) | +27.0% | +44.2% | +51.0% | +74.3% | +66.7% (n/c) |
| Twitter (NDJSON) | +15.7% | +16.5% | +11.1% | +18.2% | +23.8% |
| Twitter (comma) | +24.2% | +32.6% | +28.3% | +29.1% | +29.5% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +14.6% | +25.2% | +15.3% | +33.7% | +15.7% |
| Apache Builds (comma) | +21.4% | +65.9% | +34.7% | +51.2% | +34.2% |
| CitmCatalog (NDJSON) | +23.7% | +29.5% | +31.8% | +27.4% | +45.7% |
| CitmCatalog (comma) | +37.2% | +56.7% | +51.9% | +51.8% | +52.3% (n/c) |
| Google Maps (NDJSON) | +5.2% | +12.8% | +9.1% | +29.5% (n/c) | +24.2% |
| Google Maps (comma) | +14.8% | +27.2% | +20.3% | +24.8% | +32.7% (n/c) |
| Instruments (NDJSON) | +34.6% | +35.0% | −6.0% | +28.7% | +42.5% |
| Instruments (comma) | +47.8% | +58.0% | +19.3% | +61.4% | +64.2% (n/c) |
| Random (NDJSON) | +51.0% (n/c) | +70.7% | +56.0% | +73.9% | +111.1% |
| Random (comma) | +62.2% | +85.4% | +75.0% | +77.3% | +122.8% |
| Twitter (NDJSON) | +169.2% | +159.1% | +122.4% | +113.0% | +237.5% |
| Twitter (comma) | +175.2% | +172.7% | +130.3% | +128.1% | +271.9% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 26 of the 30 results in that group and lost 4. It won Stream Formats Small on all five builds, as NDJSON (+8.7% to +23.6%) and comma-separated (+42.3% to +113.6%), and Amazon Cellphones on all five (+1.3% to +29.2%). Large Amazon Cellphones won on every build but MSVC (+10.4% to +18.4%), which lost it (−2.3%). Stream Formats Large won in both forms on MSVC, Linux/GCC and macOS/GCC; Linux/Clang won it comma-separated and lost it as NDJSON (−2.0%), and macOS/Clang lost both (NDJSON −5.7%, comma −5.2%).
- **One build won every streaming test,** 30-0-0: macOS/GCC. MSVC, Linux/Clang and Linux/GCC each went 29-0-1, and macOS/Clang 28-0-2.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won 59 of the 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +113.0% to +271.9%, while Google Maps rows, which have few, give +5.2% to +32.7%. Instruments wins on four builds (up to +64.2%) and loses as NDJSON on Linux/GCC (−6.0%), the only reverse loss.
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 361.8 MB/s under MSVC and 456.8 MB/s under Linux/Clang, while Jsonifier reads it at 586.7 and 846.7 MB/s.

## 9. AVX-512 builds

Every result in §7 and §8 comes from AVX2 and NEON builds. This section repeats the whole suite, single documents and streams, on three builds that run Jsonifier's AVX-512 stage-1 kernel against simdjson's own AVX-512 kernel (`icelake`).

### 9.1 Method

- **Harness:** Json-Performance, branch `generic-parsing-avx512-main`, with the same tests, readers, allocation modes, batch size and adaptive-sampling stage as §6 and §8.3, and the same convergence limits as the x86 builds there (RSE < 5% and shift < 2.5%).
- **Hosts:** the builds ran on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, under Linux `7.0.0-1019-azure` and Windows 10.0.26100. These are shared virtual machines, not the dedicated Core i9-14900KF of §6, which has no AVX-512.
- **Compilers:** Clang 24.0 is the same as in §6. GCC 16.0.1 and MSVC 19.51 are the runner images' compilers and are older than §6's GCC 16.2.1 and MSVC 19.52.
- **Commits:** BenchmarkSuite `71afc62`, Jsonifier `79b3e67` and simdjson `36c5b3c`, the same as §6. All three builds ran on November 17, 2026. The stage-1 tapes of these builds match simdjson's position for position (§6, *Correctness*).

| Build | Stage-1 kernel | Converged, single documents | Converged, streams |
|---|---|---|---|
| Windows 10.0.26100 / MSVC 19.51 | AVX-512 | 323 / 330 | 23 / 30 |
| Linux 7.0.0 (Azure) / Clang 24.0 | AVX-512 | 328 / 330 | 27 / 30 |
| Linux 7.0.0 (Azure) / GCC 16.0.1 | AVX-512 | 327 / 330 | 25 / 30 |

The CPU, the operating-system images and two of the three compilers differ from §6, so a difference between an AVX-512 result here and the matching AVX2 result in §7 cannot be credited to the instruction set alone. Each comparison below is between the two libraries within one AVX-512 build.

### 9.2 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 4-1-0 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 17-1-2 | 20-0-0 | 20-0-0 | 19-0-1 | 159-3-3 |
| Linux / Clang | 4-0-1 | 18-0-2 | 20-0-0 | 17-2-1 | 20-0-0 | 16-0-4 | 20-0-0 | 20-0-0 | 20-0-0 | 155-2-8 |
| Linux / GCC | 4-0-1 | 16-0-4 | 20-0-0 | 18-0-2 | 18-0-2 | 9-1-10 | 19-0-1 | 17-1-2 | 19-0-1 | 140-2-23 |
| **All** | **12-1-2** | **53-1-6** | **60-0-0** | **55-2-3** | **58-0-2** | **42-2-16** | **59-0-1** | **57-1-2** | **58-0-2** | **454-7-34** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 18-0-2 | 20-0-0 | 158-3-4 |
| Linux / Clang | 4-0-1 | 18-0-2 | 20-0-0 | 17-2-1 | 20-0-0 | 17-0-3 | 20-0-0 | 19-0-1 | 20-0-0 | 155-2-8 |
| Linux / GCC | 4-0-1 | 17-0-3 | 20-0-0 | 19-0-1 | 19-0-1 | 10-0-10 | 19-0-1 | 17-2-1 | 19-0-1 | 144-2-19 |
| **All** | **13-0-2** | **52-2-6** | **59-1-0** | **56-2-2** | **59-0-1** | **47-0-13** | **58-0-2** | **54-2-4** | **59-0-1** | **457-7-31** |

**Streams:**

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 11-1-0 | 12-0-0 | 29-1-0 |
| Linux / Clang | 5-0-1 | 11-0-1 | 12-0-0 | 28-0-2 |
| Linux / GCC | 5-1-0 | 9-2-1 | 11-0-1 | 25-3-2 |
| **All** | **16-1-1** | **31-3-2** | **35-0-1** | **82-4-4** |

Each cell is wins-ties-losses.

Across the 990 single-document results (12 of them did not converge), Jsonifier won 911, tied 14 and lost 65: 454-7-34 with fresh output objects and 457-7-31 with reused ones. Across the 90 streaming results (15 of them did not converge) it won 82, tied 4 and lost 4.

### 9.3 POD tests

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Bool | +211.0% / +214.1% | +55.1% / +53.7% | +107.0% / +89.8% |
| Double | +172.7% / +160.3% | +58.6% / +50.0% | +66.7% / +68.2% |
| Int64 | +88.7% / +96.0% | +37.4% / +37.1% | +44.8% / +46.1% |
| String | tie / +0.9% | −37.7% / −45.9% | −31.5% / −36.7% |
| Uint64 | +83.5% / +93.5% | +38.6% / +33.1% | +43.8% / +52.6% |

### 9.4 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | tie / −2.5% | +13.2% / +14.9% | +1.1% / +1.9% |
| Apache Builds (pretty) | +2.6% / tie | +10.9% / +17.8% | +2.0% / +3.6% |
| Canada (min) | +4.1% / tie | +6.1% / +10.1% | +6.3% / +8.7% |
| Canada (pretty) | +6.0% (n/c) / +6.2% | +2.9% / +8.7% | +4.6% / +9.3% |
| CitmCatalog (min) | +9.8% / +6.8% | +7.4% / +7.8% | +8.4% / +10.4% |
| CitmCatalog (pretty) | +14.9% / +9.9% | +4.6% / +2.7% | +2.1% / +1.2% |
| Discord (min) | +12.8% / +18.7% | +13.4% / +15.6% | +8.0% / +4.8% |
| Discord (pretty) | +15.0% / +21.0% | +7.2% / +16.6% | +3.0% / +6.1% |
| Github Events (min) | +27.7% / +32.3% | +23.4% / +44.7% | +10.3% / +23.2% |
| Github Events (pretty) | +30.6% / +31.5% | +21.2% / +33.2% | +14.6% / +23.8% |
| Google Maps (min) | +19.7% / +17.9% | +22.1% / +17.6% | +8.7% / +6.1% |
| Google Maps (pretty) | +22.4% / +22.7% | +14.8% / +17.6% | −4.9% / +5.7% |
| Instruments (min) | +15.7% / +17.3% | +9.2% / +11.8% | +1.1% / +3.0% |
| Instruments (pretty) | +21.3% / +20.3% | +7.9% / +11.2% | −4.4% / −7.4% |
| Marine IK (min) | +4.0% (n/c) / +11.8% (n/c) | +5.4% / +1.5% | +7.6% / +14.1% |
| Marine IK (pretty) | +17.9% / +10.4% | +8.5% / +7.0% | +1.7% / +7.9% |
| Random (min) | +14.8% / +20.4% | +17.6% / +23.8% | +1.5% / +2.8% |
| Random (pretty) | +16.9% / +23.6% | +15.5% / +17.7% | +1.9% / +1.7% |
| Twitter (min) | +17.0% / +14.0% | −3.2% / −5.8% | −9.6% / −6.9% |
| Twitter (pretty) | +16.9% / +19.4% | −2.5% / −1.2% | −11.6% / −12.6% |

### 9.5 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +51.4% / +57.1% | +77.7% / +106.6% | +65.2% / +89.2% |
| Apache Builds (pretty) | +34.4% / +46.7% | +82.9% / +100.0% | +59.2% / +70.4% |
| Canada (min) | +5.7% / +6.1% (n/c) | +9.7% / +13.4% | +6.3% / +4.6% |
| Canada (pretty) | +4.7% / tie (n/c) | +11.6% (n/c) / +14.1% | +10.1% (n/c) / +7.9% |
| CitmCatalog (min) | +59.4% / +64.5% (n/c) | +106.2% / +109.4% | +76.1% / +88.6% |
| CitmCatalog (pretty) | +51.6% / +60.6% | +85.8% / +98.4% | +77.4% / +81.3% |
| Discord (min) | +270.8% / +335.7% | +315.2% / +332.8% | +305.5% / +299.9% |
| Discord (pretty) | +269.1% / +292.7% | +339.6% / +370.2% | +234.0% / +285.5% |
| Github Events (min) | +92.0% / +109.5% | +95.9% / +114.8% | +67.1% / +72.2% |
| Github Events (pretty) | +86.6% / +104.2% | +90.4% / +99.3% | +50.2% / +61.7% |
| Google Maps (min) | +23.7% / +30.4% | +30.9% / +28.0% | +15.1% / +14.2% |
| Google Maps (pretty) | +26.8% / +30.2% | +27.1% / +24.3% | +15.3% / +9.2% |
| Instruments (min) | +209.9% / +212.2% | +189.7% / +198.6% | +158.7% / +149.2% |
| Instruments (pretty) | +191.1% / +193.8% | +171.3% / +166.7% | +123.3% / +122.7% |
| Marine IK (min) | +35.8% (n/c) / +43.1% | +77.9% / +76.0% | +81.8% (n/c) / +85.5% |
| Marine IK (pretty) | +39.9% / +37.5% | +66.9% / +75.1% | +64.3% / +74.3% (n/c) |
| Random (min) | +104.7% / +115.1% | +114.6% / +133.0% | +60.6% / +67.8% |
| Random (pretty) | +93.9% / +108.2% | +95.9% / +116.8% | +59.2% / +63.4% |
| Twitter (min) | +191.3% / +201.0% | +158.8% / +168.9% | +101.9% / +107.4% |
| Twitter (pretty) | +184.2% / +188.0% | +148.5% / +152.0% | +90.5% / +92.3% |

### 9.6 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +6.4% / +11.2% | +26.0% / +33.9% | +11.6% / +16.7% |
| Apache Builds (pretty) | +8.4% / +10.1% | +14.6% / +12.6% | +7.4% / +9.5% |
| Canada (min) | +94.9% / +92.6% | +29.7% / +19.5% | +26.6% / +34.0% |
| Canada (pretty) | +140.2% / +127.7% | +16.2% / +21.0% | +23.1% / +27.6% |
| CitmCatalog (min) | +47.9% / +50.4% | +15.8% / +20.3% | +25.9% / +24.5% |
| CitmCatalog (pretty) | +72.0% / +74.0% | +15.8% / +10.5% | +20.1% / +20.0% |
| Discord (min) | +15.3% / +21.8% | tie / tie | +1.6% / +2.9% |
| Discord (pretty) | +18.3% / +27.9% | tie / tie | −3.9% / +4.3% |
| Github Events (min) | +63.2% / +75.8% | +23.9% / +24.0% | +15.6% / +15.3% |
| Github Events (pretty) | +50.7% / +57.3% | +21.3% / +25.4% | +11.1% / +13.8% |
| Google Maps (min) | +39.6% / +39.1% | +23.2% / +22.1% | +0.2% / −2.5% |
| Google Maps (pretty) | +42.2% / +44.7% | +20.2% / +21.3% | −1.3% / +1.9% |
| Instruments (min) | +53.6% / +66.0% | +26.1% / +27.2% | +19.1% / +11.9% |
| Instruments (pretty) | +63.9% / +74.4% | +15.4% / +23.5% | +3.4% / +17.7% |
| Marine IK (min) | +47.9% / +51.8% | −3.1% / −7.5% | +50.8% / +43.8% |
| Marine IK (pretty) | +96.0% / +108.0% | +3.6% / +4.2% | +37.9% / +30.0% |
| Random (min) | +43.8% / +44.1% | +31.5% / +26.5% | +2.9% / +6.6% |
| Random (pretty) | +46.4% / +46.5% | +22.5% / +28.5% | +3.6% / +8.7% |
| Twitter (min) | +126.2% / +139.3% | +11.3% / +10.8% | +14.3% / +12.5% |
| Twitter (pretty) | +124.4% / +135.6% | +12.7% / +15.9% | +7.8% / +5.0% |

### 9.7 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +9.3% / +8.9% | +7.3% / +14.9% | +3.3% / +4.2% |
| Apache Builds (pretty) | +9.3% / +13.1% | +5.4% / +9.1% | −2.6% / +3.8% |
| Canada (min) | +53.4% / +45.6% | +28.9% / +36.3% | +32.7% / +36.5% |
| Canada (pretty) | +83.4% / +81.7% | +9.8% / +4.0% | +31.4% / +30.3% |
| CitmCatalog (min) | +37.6% / +33.5% | +10.2% / +6.9% | +19.1% / +22.3% |
| CitmCatalog (pretty) | +51.8% / +51.1% | +9.0% / +13.0% | +17.2% / +22.8% |
| Discord (min) | +56.8% / +73.2% | +30.9% / +33.7% | +33.4% / +34.6% |
| Discord (pretty) | +52.8% / +71.3% | +32.1% / +42.1% | +18.6% / +31.5% |
| Github Events (min) | +86.6% / +98.1% | +59.0% / +65.1% | +36.8% / +46.4% |
| Github Events (pretty) | +85.8% / +99.0% | +40.6% / +48.3% | +33.0% / +41.4% |
| Google Maps (min) | +30.3% / +34.5% | +18.1% / +12.5% | +1.5% / +1.6% |
| Google Maps (pretty) | +27.1% / +39.5% | +15.8% / +18.3% | −4.8% / −4.1% |
| Instruments (min) | +91.6% / +97.3% | +43.6% / +46.8% | +53.0% / +50.6% |
| Instruments (pretty) | +96.8% / +100.0% | +29.7% / +34.1% | +43.5% / +43.8% |
| Marine IK (min) | +72.0% / +74.1% | +52.7% / +52.1% | +78.5% / +86.0% |
| Marine IK (pretty) | +102.3% / +96.0% | +55.1% / +58.5% | +53.5% / +52.0% |
| Random (min) | +64.4% / +74.0% | +41.6% / +45.0% | +9.8% / +20.2% |
| Random (pretty) | +62.6% / +73.8% | +33.2% / +39.0% | +18.0% / +16.7% |
| Twitter (min) | +111.9% / +112.6% | +38.9% / +39.9% | +20.0% / +14.3% |
| Twitter (pretty) | +119.2% / +123.8% | +24.6% / +19.6% | +21.4% / +13.2% |

### 9.8 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +3.3% / +9.7% | +12.4% / +17.3% | +6.4% / +12.1% |
| Apache Builds (pretty) | +3.1% / +2.5% | +10.9% / +129.8% (n/c) | −2.5% / +6.1% |
| Canada (min) | +10.0% / +4.0% | +11.1% / +15.0% | +10.6% / +9.2% |
| Canada (pretty) | tie / +5.2% | +7.0% / +7.4% | +6.5% / +7.0% |
| CitmCatalog (min) | +6.1% / +5.1% | −2.4% / +1.6% | −7.1% / −5.6% |
| CitmCatalog (pretty) | +7.1% / +7.0% | −2.9% / −0.8% | −8.5% / −2.6% |
| Discord (min) | −1.1% / +22.4% | +1.5% / +10.4% | −5.2% / −2.9% |
| Discord (pretty) | +7.5% / +20.7% | +2.0% / +9.3% | −5.9% / −1.9% |
| Github Events (min) | +12.6% / +16.0% | +5.5% / +4.5% | −16.7% / −11.7% |
| Github Events (pretty) | +12.3% / +22.9% | +1.0% / −1.3% | −21.2% / −20.2% |
| Google Maps (min) | +13.6% / +22.4% | +24.9% / +23.9% | +2.6% / +6.4% |
| Google Maps (pretty) | −29.4% / +23.0% | +19.3% / +20.8% | +2.3% / +2.1% |
| Instruments (min) | +15.6% / +12.2% | +4.9% / +5.2% | tie / −0.8% |
| Instruments (pretty) | +17.1% / +18.4% | +8.5% / +7.1% | −8.5% / −7.3% |
| Marine IK (min) | +3.5% / +3.4% | +8.5% / +5.4% | +9.8% / +1.8% |
| Marine IK (pretty) | +6.0% / +8.9% | +10.9% / +9.1% | +2.5% / +2.5% |
| Random (min) | +17.6% / +19.4% | +11.9% / +19.5% | +11.5% / +5.7% |
| Random (pretty) | +20.4% / +21.4% | +14.9% / +15.2% | +9.1% / +9.4% |
| Twitter (min) | +81.2% / +22.1% | −8.8% / +1.0% | −11.1% / −9.2% |
| Twitter (pretty) | +7.8% / +16.4% | −6.4% / −3.4% | −14.8% / −7.6% |

### 9.9 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +74.1% / +106.8% | +116.0% / +148.1% | +69.6% / +88.1% |
| Apache Builds (pretty) | +63.7% / +89.9% | +104.1% / +127.3% | +59.9% / +79.0% |
| Canada (min) | +5.5% / −2.5% | +6.8% / +13.3% | +14.3% / +15.0% |
| Canada (pretty) | +6.0% / +4.7% | +8.1% / +12.9% | +9.2% / +9.8% |
| CitmCatalog (min) | +62.1% / +58.5% | +108.9% / +95.9% | +68.8% / +65.3% |
| CitmCatalog (pretty) | +52.6% / +56.7% | +85.6% / +93.0% | +55.6% / +57.9% |
| Discord (min) | +257.2% / +318.9% | +308.3% / +361.9% | +239.6% / +283.8% |
| Discord (pretty) | +269.1% / +305.0% | +298.6% / +376.1% | +210.9% / +238.9% |
| Github Events (min) | +30.4% / +43.5% | +29.6% / +40.8% | +6.4% / +9.0% |
| Github Events (pretty) | +34.4% / +38.4% | +18.4% / +26.1% | −5.4% / −1.2% |
| Google Maps (min) | +21.1% / +17.2% | +33.2% / +31.8% | +9.7% / +11.0% |
| Google Maps (pretty) | +25.7% / +36.3% | +26.4% / +27.4% | +6.1% / +8.8% |
| Instruments (min) | +205.0% / +216.7% | +192.2% / +199.0% | +133.6% / +140.2% |
| Instruments (pretty) | +188.3% / +209.4% | +151.8% / +158.9% | +103.0% / +123.2% |
| Marine IK (min) | +31.1% / +33.6% | +65.3% / +68.4% | +58.6% / +61.2% |
| Marine IK (pretty) | +32.2% / +36.0% | +66.2% / +68.1% | +51.1% / +55.9% |
| Random (min) | +88.9% / +98.2% | +128.7% / +134.3% | +58.8% / +71.6% |
| Random (pretty) | +85.6% / +101.8% | +96.4% / +108.0% | +52.4% / +57.6% |
| Twitter (min) | +192.4% / +206.2% | +159.4% / +165.3% | +98.9% / +104.8% |
| Twitter (pretty) | +323.5% / +95.7% | +144.3% / +151.9% | +90.8% / +96.3% |

### 9.10 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +37.0% / +38.6% | +36.2% / +28.8% | +18.3% / +31.9% |
| Apache Builds (pretty) | +39.7% / +41.3% | +18.7% / +11.7% | +16.9% / +25.2% |
| Canada (min) | +90.8% / +96.6% | +26.0% / +16.9% | +18.5% / +20.2% |
| Canada (pretty) | +111.9% / +113.7% | +16.7% / +51.3% | +24.7% / +20.7% |
| CitmCatalog (min) | +67.6% / +80.0% | +7.3% / +20.2% | +23.0% / +22.9% |
| CitmCatalog (pretty) | +88.2% / +94.6% | +12.0% / +11.3% | +18.2% / +19.6% |
| Discord (min) | +37.6% / +43.8% | +19.1% / +23.4% | +8.9% / +10.2% |
| Discord (pretty) | +42.9% / +45.1% | +10.3% / +12.3% | +8.5% / +6.4% |
| Github Events (min) | +52.8% / +63.8% | +30.0% / +33.0% | +5.7% / +6.6% |
| Github Events (pretty) | +42.5% / +50.0% | +11.8% / +16.4% | +6.0% / +7.3% |
| Google Maps (min) | +36.7% / +38.6% | +17.5% / +23.3% | tie / tie |
| Google Maps (pretty) | +41.8% / −20.6% | +23.3% / +24.7% | −1.3% / +1.6% |
| Instruments (min) | +65.0% / +78.3% | +27.1% / +33.2% | +22.3% / +22.4% |
| Instruments (pretty) | +65.7% / +77.7% | +17.2% / +25.1% | +10.1% / +12.1% |
| Marine IK (min) | +46.3% / +44.3% | +6.2% / +8.6% | +25.5% / +36.8% |
| Marine IK (pretty) | +81.1% / +88.6% | +19.7% / +12.3% | +22.6% / +30.3% |
| Random (min) | +39.0% / +48.1% | +34.6% / +40.6% | +6.4% / +6.7% |
| Random (pretty) | +33.2% / −16.8% | +32.0% / +35.7% | +2.6% / +7.2% |
| Twitter (min) | +75.7% / +76.5% | +1.0% / +4.9% | +0.9% / tie |
| Twitter (pretty) | +68.2% / +73.0% | +3.4% / −3.2% | −1.4% / −6.4% |

### 9.11 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +27.5% / +22.3% | +10.8% / +17.9% | +15.1% / +7.0% |
| Apache Builds (pretty) | +22.7% / +28.9% | +8.4% / +20.5% | +6.4% / +8.0% |
| Canada (min) | +50.5% / +51.9% | +16.2% / +15.2% | +32.7% / +30.6% |
| Canada (pretty) | +64.4% / +69.3% | +16.7% / +23.9% | +26.8% / +31.0% |
| CitmCatalog (min) | +58.7% / +64.0% | +20.3% / +16.9% | +10.9% / +17.2% |
| CitmCatalog (pretty) | +72.0% / +77.7% | +16.3% / +20.7% | +14.8% / +13.1% |
| Discord (min) | +77.6% / +90.4% | +62.7% / +72.5% | +57.2% / +58.2% |
| Discord (pretty) | +74.9% / +98.5% | +58.3% / +66.4% | +30.3% / +33.1% |
| Github Events (min) | +71.8% / +66.9% | +52.3% / +59.9% | +21.0% / +24.1% |
| Github Events (pretty) | +58.7% / +74.8% | +45.6% / +51.9% | +13.8% / +12.7% |
| Google Maps (min) | +30.8% / +35.3% | +18.8% / +21.0% | +4.1% / +7.5% |
| Google Maps (pretty) | +35.0% / +37.3% | +18.8% / +20.1% | −14.0% / −3.6% |
| Instruments (min) | +93.8% / +105.3% | +51.8% / +50.2% | +57.1% / +54.1% |
| Instruments (pretty) | +82.2% / +93.2% | +35.0% / +38.7% | +28.3% / +37.7% |
| Marine IK (min) | +66.2% / +78.0% | +49.8% / +53.9% | +59.2% / +61.7% |
| Marine IK (pretty) | +80.7% / +86.8% | +54.7% / +50.7% | +46.0% / +51.2% |
| Random (min) | +143.1% / +73.6% | +61.2% / +65.8% | +22.1% / +22.1% |
| Random (pretty) | −18.3% / +67.6% | +42.1% / +50.7% | +16.0% / +12.2% |
| Twitter (min) | +73.3% / +82.0% | +10.6% / +15.4% | +9.8% / +7.0% |
| Twitter (pretty) | +67.6% / +68.0% | +9.3% / +10.7% | +1.2% / +5.0% |

### 9.12 Streams

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 11-1-0 | 12-0-0 | 29-1-0 |
| Linux / Clang | 5-0-1 | 11-0-1 | 12-0-0 | 28-0-2 |
| Linux / GCC | 5-1-0 | 9-2-1 | 11-0-1 | 25-3-2 |
| **All** | **16-1-1** | **31-3-2** | **35-0-1** | **82-4-4** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Amazon Cellphones (NDJSON) | +26.6% | +16.2% | +10.2% |
| Large Amazon Cellphones (NDJSON) | +6.5% | +7.8% | +3.1% |
| Stream Formats Small (NDJSON) | +34.6% | −2.0% | tie (n/c) |
| Stream Formats Small (comma) | +66.2% | +64.1% (n/c) | +84.6% |
| Stream Formats Large (NDJSON) | +188.2% | +19.8% | +39.2% |
| Stream Formats Large (comma) | +182.4% | +27.7% | +45.1% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | +2.5% | +18.9% | +1.1% |
| Apache Builds (comma) | tie (n/c) | +30.0% | +15.0% |
| CitmCatalog (NDJSON) | +6.2% | +3.3% | tie |
| CitmCatalog (comma) | +11.5% | +22.0% | +19.7% (n/c) |
| Google Maps (NDJSON) | +23.6% (n/c) | +11.9% | tie |
| Google Maps (comma) | +26.8% (n/c) | +23.2% | +14.6% |
| Instruments (NDJSON) | +32.4% | +25.1% | +1.0% |
| Instruments (comma) | +47.0% | +42.0% | +22.7% |
| Random (NDJSON) | +22.5% | +13.4% | +3.3% |
| Random (comma) | +25.1% | +17.2% | +21.2% (n/c) |
| Twitter (NDJSON) | +9.7% | −3.5% | −8.8% |
| Twitter (comma) | +15.2% | +6.4% | +2.4% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | +8.5% | +24.6% | +1.9% |
| Apache Builds (comma) | +13.4% | +37.2% | +13.5% |
| CitmCatalog (NDJSON) | +37.6% (n/c) | +43.6% | +37.1% |
| CitmCatalog (comma) | +41.7% | +65.9% (n/c) | +60.4% (n/c) |
| Google Maps (NDJSON) | +22.6% (n/c) | +15.8% | −5.0% (n/c) |
| Google Maps (comma) | +32.1% | +19.0% | +10.0% |
| Instruments (NDJSON) | +90.0% | +67.6% | +8.8% |
| Instruments (comma) | +114.3% (n/c) | +81.1% | +31.6% |
| Random (NDJSON) | +97.1% | +79.8% | +54.4% |
| Random (comma) | +107.3% (n/c) | +121.9% (n/c) | +66.6% |
| Twitter (NDJSON) | +199.4% | +171.7% | +97.1% |
| Twitter (comma) | +203.8% | +172.4% | +113.3% |

### 9.13 What the results show

- **Reverse order wins on every AVX-512 build.** Jsonifier won 236 of the 240 reverse results (1 tie, 3 losses): Linux/Clang went 80-0-0, MSVC 78-1-1 and Linux/GCC 78-0-2. The losses are prettified Github Events Small on Linux/GCC (−5.4% fresh, −1.2% reused) and minified Canada Small on MSVC (reused, −2.5%), and the tie is prettified Canada on MSVC (reused, n/c). On the documents whose objects have many fields (Discord, Twitter, Instruments) the full-size margin runs from +90.5% (Twitter pretty, Linux/GCC, fresh) to +370.2% (Discord pretty, Linux/Clang, reused), and the small-document margin from +90.8% to +376.1%.
- **simdjson's AVX-512 kernel does not close the reverse-order gap.** On MSVC, simdjson reads Discord Reverse (pretty, fresh) at 175.5 MB/s and Jsonifier at 647.7 MB/s; on Linux/Clang the figures are 215.1 and 945.5 MB/s. simdjson's rescans are bound by how many keys it walks, not by how fast stage 1 runs, so a wider stage-1 kernel cannot shorten them.
- **Sparse reverse reads stay a strength.** Jsonifier won 117 of the 120 full-size sparse-reverse results and 117 of the 120 small ones, with no ties. MSVC and Linux/Clang won all 40 of their full-size results, and Linux/Clang all 40 small ones; every full-size loss is prettified on Linux/GCC: Apache Builds (fresh, −2.6%) and Google Maps (−4.8% fresh, −4.1% reused). The small losses are prettified Google Maps Small on Linux/GCC (−14.0% fresh, −3.6% reused) and prettified Random Small on MSVC (fresh, −18.3%).
- **Linux/Clang loses mostly on Twitter.** It went 310-4-16 and won all 160 of its reverse and sparse-reverse results. 8 of its 16 losses are Twitter: read in order at full size in both forms and both runs (−1.2% to −5.8%), and 4 small results (−3.2% to −8.8%). The rest are String POD (2), minified Marine IK read sparse (−3.1% fresh, −7.5% reused) and 4 small in-order results on CitmCatalog Small and Github Events Small (−0.8% to −2.9%). Its 4 ties are Discord read sparse.
- **Linux/GCC holds most of the AVX-512 losses:** 42 of the 65. 27 of them are in-order reads (7 at full size, 20 small), against 11 in-order losses for the GCC 16.2.1 build of §7 on the 14900KF. Twitter accounts for 10 of the 42, then Google Maps with 8, Github Events with 6, and Discord and Instruments with 5 each. Read in order at full size, Linux/GCC lost Twitter in both forms and both runs (−6.9% to −12.6%), prettified Instruments in both runs (−4.4% and −7.4%) and prettified Google Maps (fresh, −4.9%), and it lost 20 of its 40 small in-order results. Twitter wins by wide margins when read in reverse on this build (+90.5% and more), so the gap is in reading in order, not in field lookup. Because this build differs from §7's in compiler version as well as CPU, these results alone do not say which of the two is responsible.
- **String POD loses on both Linux AVX-512 builds:** −37.7% fresh and −45.9% reused on Linux/Clang, −31.5% and −36.7% on Linux/GCC. On Linux/Clang simdjson reads it at 1,221.7 MB/s and Jsonifier at 761.0 MB/s (fresh). MSVC tied String fresh and won it reused (+0.9%), and the other four POD tests won on all three builds (+33.1% to +214.1%).
- **MSVC lost 7 of its 330 single-document results, and every one of them ties or wins in the other allocation mode.** Four are large and on prettified small documents: Google Maps Small read in order (fresh, −29.4%; +23.0% reused), Random Small read sparse in reverse (fresh, −18.3%; +67.6% reused), Google Maps Small read sparse (reused, −20.6%; +41.8% fresh) and Random Small read sparse (reused, −16.8%; +33.2% fresh). The other 3 are minified Apache Builds read in order (reused, −2.5%; a tie fresh), minified Discord Small read in order (fresh, −1.1%) and minified Canada Small read in reverse (reused, −2.5%). The MSVC runner also left the most unconverged results, so these are best read with the shared-runner caveat of §9.1; they stay in the tallies. It won all 80 full-size sparse and sparse-reverse results.
- **Streams:** Jsonifier won 16 of the 18 Amazon Cellphones and Stream Formats results, including Stream Formats Large on MSVC by +188.2% (NDJSON) and +182.4% (comma-separated), and Amazon Cellphones on MSVC by +26.6%. Stream Formats Small as NDJSON is the exception: Linux/Clang lost it (−2.0%) and Linux/GCC tied it. MSVC went 29-1-0, tying Apache Builds read in order comma-separated (n/c); Linux/Clang went 28-0-2, also losing Twitter read in order as NDJSON (−3.5%); Linux/GCC went 25-3-2, losing Twitter read in order as NDJSON (−8.8%) and Google Maps read in reverse as NDJSON (−5.0%, n/c). Twitter read in reverse wins on all three builds in both forms (+97.1% to +203.8%).
- **Convergence was weaker on the cloud runners.** 7 of MSVC's 330 single-document results and 7 of its 30 streaming results did not converge, against 2 and 1 for the MSVC build of §6; Linux/GCC left 3 single-document and 5 streaming results unconverged, and Linux/Clang 2 and 3.

## 10. Limitations

- **Linux/GCC holds the most single-document losses:** 17 of the 66, against 16 on macOS/GCC, 14 on macOS/Clang, 12 on Linux/Clang and 7 on MSVC. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (−19.0% fresh, −27.6% reused), then macOS/Clang's prettified CitmCatalog read in order (reused, −14.6%) and Linux/GCC's prettified Canada Small read in order (reused, −14.4%), the largest x86 loss.
- **Float-heavy documents hold half of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 33 of the 66: Canada 25 (12 on Linux/GCC, 5 on Linux/Clang, 3 each on macOS/GCC and macOS/Clang, 2 on MSVC) and Marine IK 8 (3 each on MSVC and macOS/GCC, 2 on macOS/Clang). There much of the cost is float parsing, not field lookup. The other 33 are Instruments 13 (4 each on Linux/GCC and macOS/Clang, 3 on Linux/Clang, 2 on macOS/GCC), CitmCatalog 10 (4 on macOS/GCC, 3 on Linux/Clang, 2 on macOS/Clang, 1 on Linux/GCC), Google Maps 5 (3 on macOS/GCC, 2 on MSVC), the 3 String POD losses, Apache Builds 1 (macOS/GCC) and Github Events 1 (macOS/Clang).
- **Linux/GCC loses mostly on Canada.** 12 of its 17 losses are Canada, read in order at both sizes and in reverse at small size; its sparse and sparse-reverse Canada reads all win, since there much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 40 of the 66 losses are small documents (13 on Linux/GCC, 7 each on MSVC, macOS/GCC and macOS/Clang, 6 on Linux/Clang). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Five streaming losses, on four builds:** MSVC's Large Amazon Cellphones (−2.3%), Linux/Clang's Stream Formats Large as NDJSON (−2.0%), Linux/GCC's Instruments record stream read in reverse as NDJSON (−6.0%), and macOS/Clang's Stream Formats Large as NDJSON (−5.7%) and comma-separated (−5.2%).
- **Some results did not converge.** 22 of the 1,650 single-document results did not converge: 15 on macOS/GCC, 5 on macOS/Clang and 2 on Windows/MSVC. 10 of the 150 streaming results did not converge: 6 on macOS/Clang, 3 on macOS/GCC and 1 on Windows/MSVC. Linux/Clang and Linux/GCC converged on every single-document and streaming result.
- **The AVX-512 builds ran on shared cloud runners.** §9's three builds used an AMD EPYC 9V45 on shared virtual machines, older GCC and MSVC versions than §6 and different operating-system images, so their results cannot be set directly against §7's to isolate the effect of AVX-512. They converged less often (12 of 990 single-document and 15 of 90 streaming results did not converge, 7 of each on MSVC), and every MSVC loss flips to a tie or win in the other allocation mode (§9.13). Linux/GCC lost 42 of its 330 AVX-512 single-document results, most of them in-order reads, against 17 for the Linux/GCC build of §7.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,554, tied 30 and lost 66. On reverse reads it won 387 of 400 at both sizes, and on Windows/MSVC it lost 7 of its 330 single-document results, all on small documents. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (784-10-31 fresh, 770-20-35 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 33 of the 66 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 145 and lost 5, and it won 119 of the 120 corpus record-stream results. Stream Formats Large holds three of the five losses and is the main remaining streaming target.

The lead holds against simdjson's own AVX-512 kernel. On three AVX-512 builds, Jsonifier won 911 of 990 single-document results, tied 14 and lost 65, won 236 of 240 reverse results and won 82 of 90 streaming results; a wider stage 1 does not shorten simdjson's rescans. On those builds the target is in-order reading under GCC, which holds 27 of the 65 losses.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 12. Dedication

This paper is dedicated to the author's Little Buddy.

## References

1. simdjson. https://github.com/simdjson/simdjson
2. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
