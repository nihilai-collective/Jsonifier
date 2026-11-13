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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,552 of 1,650 single-document results against simdjson On Demand, tied 18 and lost 80. On reverse-order reads it won 384 of 400. On Windows/MSVC it won 312 of 330 single-document results, tied 7 and lost 11.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 145 of 150 results and lost 5, and it won all 120 corpus record-stream results.

---

## 1. Background: why On Demand is hard to beat

simdjson's On Demand API does not build a DOM. Stage 1 produces a tape of structural positions, and stage 2 is a cursor over that tape driven by the caller. A value that is never requested is skipped over by structural depth, without being parsed. Reading in document order costs roughly one pass over the tape.

The price is a strictly forward-only API. Each value can be consumed once, and reading an object's fields out of order means rescanning the object for every key.

## 2. Design goals

1. **Keep On Demand's shape.** Existing On Demand code should port to `jsonifier::generic` mostly mechanically.
2. **Allow values to be read again.** A value handle remembers where it sits on the tape and can re-seek to it, instead of being invalidated.
3. **Make in-order reads free.** The common case must cost no more than a cursor increment and a key compare.
4. **Keep out-of-order reads close to in-order speed.** Their cost should stay close to linear in the size of the object, no matter what order the caller reads keys in.

## 3. Architecture

### 3.1 The tape

Stage 1 is Jsonifier's existing structural indexer (see *Two Stages, On Demand*, §4). It writes one `uint32_t` offset per structural character, and appends a sentinel entry `tape[count] = length`. Because of the sentinel, stage 2 never has to bounds-check the end of the tape on its hot paths.

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

- **Harness:** Json-Performance, branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 10 documents (Apache Builds, Canada, CitmCatalog, Discord, Github Events, Google Maps, Instruments, Marine IK, Random and Twitter) each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 170 single-document results per build at full size and another 160 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal. The in-order and sparse readers use each library's ordered lookup (`findField`, `find_field`), and the reverse and sparse-reverse readers use its unordered lookup (`findFieldUnordered`, `find_field_unordered`).
- **Correctness:** on the measured commits, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, read directly from upstream simdjson's internal stage-1 parser after `stage1()`, with no fork or patch. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `4919934` and simdjson `449f38e`, the benchmarked commits, on November 13, 2026. The tape checks ran on cloud-hosted CI runners, not on the benchmark hosts: their Linux builds report an Azure kernel (`7.0.0-1019-azure`), the harness does not record their CPU model, and their x86 compilers differ from the benchmark builds' (MSVC 19.51 and GCC 16.0.1, against 19.52 and 16.2.1 below; Clang 24.0 is the same), while the macOS tape builds used the same GCC 16.2.0 and Clang 23.1.4. The AVX-512 builds could not run on the Core i9-14900KF, which has no AVX-512. The benchmark runs did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-43-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 8 of the 1,650 single-document results and 7 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. Both libraries read integers more slowly under MSVC than under Linux/Clang on the same CPU: simdjson reads Int64 (fresh) at 571.4 MB/s under MSVC against 784.0 MB/s under Linux/Clang, while Jsonifier reads it at 830.6 and 1,549.2 MB/s; for Uint64 (fresh) the figures are 555.9 against 715.7 MB/s for simdjson and 836.8 against 1,514.7 MB/s for Jsonifier.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 167 / 170 | 157 / 160 | 324 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 170 / 170 | 160 / 160 | 330 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `27950a9`, Jsonifier `4919934` and simdjson `449f38e`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. All five builds ran on November 13, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 15-3-2 | 18-1-1 | 19-0-1 | 19-0-1 | 154-6-5 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 16-0-4 | 19-0-1 | 20-0-0 | 18-0-2 | 156-0-9 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 13-0-7 | 18-0-2 | 19-0-1 | 20-0-0 | 145-0-20 |
| macOS / GCC | 5-0-0 | 19-1-0 | 20-0-0 | 18-0-2 | 20-0-0 | 19-0-1 | 20-0-0 | 19-1-0 | 20-0-0 | 160-2-3 |
| macOS / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 162-0-3 |
| **All** | **23-0-2** | **93-3-4** | **98-0-2** | **94-0-6** | **98-0-2** | **83-3-14** | **95-1-4** | **97-1-2** | **96-0-4** | **777-8-40** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 14-1-5 | 20-0-0 | 20-0-0 | 20-0-0 | 158-1-6 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 17-0-3 | 18-1-1 | 20-0-0 | 18-0-2 | 155-1-9 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 14-0-6 | 18-0-2 | 19-0-1 | 20-0-0 | 146-0-19 |
| macOS / GCC | 4-1-0 | 17-3-0 | 18-2-0 | 18-0-2 | 19-0-1 | 20-0-0 | 19-1-0 | 19-1-0 | 20-0-0 | 154-8-3 |
| macOS / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 162-0-3 |
| **All** | **22-1-2** | **91-3-6** | **96-2-2** | **94-0-6** | **97-0-3** | **84-1-15** | **95-2-3** | **98-1-1** | **98-0-2** | **775-10-40** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (8 of them did not converge; see §9), Jsonifier won 1,552, tied 18 and lost 80: 777-8-40 with fresh output objects and 775-10-40 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 194 of 200 full-size reverse results (2 ties, 4 losses) and 190 of 200 small reverse results (3 ties, 7 losses). macOS/Clang won all 80; MSVC went 78-1-1, macOS/GCC 77-3-0, Linux/Clang 77-1-2 and Linux/GCC 72-0-8. Every reverse loss and tie is Canada: at full size, Linux/GCC lost both forms in both runs (−2.9% to −6.9%), with ties on macOS/GCC (both forms, reused); at small size, Linux/GCC lost Canada Small in both forms and both runs (−7.0% to −11.8%), Linux/Clang lost prettified Canada Small in both runs (−3.2% fresh, −1.0% reused) and MSVC lost it once (prettified, fresh, −9.3%), with ties on MSVC (minified, fresh), Linux/Clang (minified, reused) and macOS/GCC (prettified, reused).
- **Windows/MSVC lost 11 of its 330 single-document results,** 10 of them on small documents. Its one full-size loss is minified Marine IK read in order (reused, −8.5%, n/c). Read in order, it lost Canada Small 3 times (−1.0% to −3.8%), Marine IK Small 3 times (−3.0% to −3.1%) and prettified CitmCatalog Small once (reused, −7.1%); the other 3 are prettified Canada Small read in reverse (fresh, −9.3%), prettified Instruments Small read sparse (fresh, −4.2%) and prettified Apache Builds Small read sparse in reverse (fresh, −3.2%). Its 7 ties are in-order or reverse reads of Canada, Marine IK, CitmCatalog and Discord. It won all 10 POD results, 78 of its 80 reverse results and 158 of its 160 sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.9–14.2% for Jsonifier and 5.3–10.6% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.5 percentage points on Linux/GCC, +1.8 on Linux/Clang, +2.4 on macOS/GCC and macOS/Clang, and +3.6 on MSVC. On MSVC, Discord Reverse (min) goes from +192.9% to +226.8%, and Apache Builds (min) from +11.5% to +17.8%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +93.6% / +98.3% | +93.5% / +92.0% | +145.0% / +144.8% | +47.6% / +50.0% | +17.1% / +19.8% |
| Double | +65.1% / +101.6% | +87.5% / +92.8% | +92.8% / +96.4% | +103.7% / +123.2% | +55.1% / +72.5% |
| Int64 | +45.4% / +51.5% | +97.6% / +100.0% | +92.7% / +92.9% | +72.8% / +53.7% | +44.8% / +50.1% |
| String | +3.1% / +2.6% | −5.8% / −13.6% | +2.8% / +5.9% | +12.6% / tie | −33.2% / −23.6% |
| Uint64 | +50.5% / +46.6% | +111.6% / +123.1% | +91.2% / +95.7% | +57.9% / +56.0% | +21.6% / +78.7% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +11.5% / +17.8% | +19.9% / +27.8% | +25.6% / +39.1% | +52.1% / +50.0% | +26.6% / +27.1% |
| Apache Builds (pretty) | +6.5% / +11.3% | +13.6% / +22.1% | +24.8% / +34.5% | +30.2% / +78.9% | +7.2% / +33.0% |
| Canada (min) | +1.8% / +1.4% | +0.6% / +1.6% | −9.9% / −11.0% | tie / tie | +6.8% / +8.4% |
| Canada (pretty) | tie / +5.5% | +0.2% / −0.4% | −7.5% / −8.0% | +7.7% / +8.8% (n/c) | +3.6% / +5.4% |
| CitmCatalog (min) | +3.1% / +1.9% | +10.6% / +10.1% | +17.2% / +19.7% | +14.4% / +30.1% | +12.8% / +12.6% |
| CitmCatalog (pretty) | +2.2% / +1.1% | +7.8% / +8.1% | +10.6% / +11.3% | +36.6% / +36.7% | +11.9% / +12.5% |
| Discord (min) | +10.1% / +13.8% | +15.8% / +25.1% | +12.2% / +18.2% | +25.4% / +148.0% | +24.2% / +28.0% |
| Discord (pretty) | +5.9% / +9.2% | +11.9% / +19.3% | +14.1% / +12.9% | +27.6% / +153.1% | +17.5% / +20.4% |
| Github Events (min) | +20.8% / +25.8% | +27.8% / +34.3% | +21.4% / +26.8% | +45.8% / +55.0% | +49.2% / +59.1% |
| Github Events (pretty) | +16.8% / +21.2% | +20.7% / +27.4% | +18.6% / +21.9% | +32.3% / +38.4% | +39.0% / +46.8% |
| Google Maps (min) | +13.6% / +14.3% | +25.5% / +27.9% | +28.3% / +29.5% | +37.8% / +40.9% | +17.6% / +18.1% |
| Google Maps (pretty) | +10.1% / +10.2% | +15.9% / +16.6% | +21.8% / +21.8% | +30.4% / +27.3% | +15.1% / +14.4% |
| Instruments (min) | +14.2% / +18.0% | +5.1% / +6.3% | +7.6% / +9.4% | +17.5% / +17.2% | +7.3% / +8.6% |
| Instruments (pretty) | +10.6% / +11.7% | +2.0% / +2.5% | +4.0% / +4.2% | +10.3% / +12.4% | +3.8% / +5.1% |
| Marine IK (min) | tie (n/c) / −8.5% (n/c) | +3.4% / +4.1% | −2.4% / −2.6% | +1.8% / tie | +8.8% / +9.3% |
| Marine IK (pretty) | +9.6% / +5.7% | +4.3% / +4.5% | −4.4% / −4.0% | +3.4% / tie | +8.1% / +9.0% |
| Random (min) | +18.2% / +22.4% | +28.1% / +35.0% | +31.3% / +38.1% | +64.2% / +83.9% | +39.7% / +44.7% |
| Random (pretty) | +16.2% / +19.9% | +24.8% / +30.8% | +26.3% / +35.4% | +57.3% / +68.1% | +35.8% / +39.6% |
| Twitter (min) | +13.4% / +16.6% | +10.4% / +26.7% | +12.0% / +22.2% | +30.2% / +35.5% | +11.9% / +14.7% |
| Twitter (pretty) | +8.8% / +12.7% | +9.9% / +12.7% | +1.3% / +10.1% | +24.6% / +17.7% | +9.2% / +11.5% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +39.2% / +55.2% | +56.5% / +72.5% | +60.2% / +77.8% | +81.9% / +81.9% | +70.3% / +88.3% |
| Apache Builds (pretty) | +29.2% / +42.5% | +52.1% / +69.6% | +56.5% / +72.5% | +66.4% / +120.3% | +63.4% / +80.1% |
| Canada (min) | +2.1% / +2.4% | +3.7% / +5.1% | −5.9% / −6.9% | +17.8% / tie | +9.9% / +10.7% |
| Canada (pretty) | +3.7% / +5.1% | +4.2% / +3.8% | −4.0% / −2.9% | +6.4% / tie | +7.6% / +8.5% |
| CitmCatalog (min) | +44.2% / +46.7% | +64.9% / +68.3% | +59.9% / +63.8% | +59.2% / +64.0% | +74.2% / +77.9% |
| CitmCatalog (pretty) | +39.7% / +40.8% | +54.3% / +56.1% | +52.0% / +57.2% | +60.4% / +62.1% | +70.1% / +72.6% |
| Discord (min) | +192.9% / +226.8% | +238.7% / +267.8% | +200.2% / +218.7% | +327.7% / +320.7% | +330.3% / +347.5% |
| Discord (pretty) | +156.7% / +181.4% | +197.0% / +214.8% | +182.0% / +198.8% | +206.4% / +245.4% | +279.0% / +294.3% |
| Github Events (min) | +94.4% / +116.8% | +115.1% / +132.5% | +91.5% / +104.3% | +103.7% / +122.4% | +158.5% / +187.4% |
| Github Events (pretty) | +76.7% / +94.0% | +90.0% / +104.6% | +76.5% / +89.9% | +84.4% / +100.7% | +139.8% / +163.9% |
| Google Maps (min) | +13.8% / +13.9% | +13.6% / +12.8% | +10.5% / +9.6% | +30.9% / +30.4% | +21.1% / +24.6% |
| Google Maps (pretty) | +6.4% / +3.4% | +8.0% / +9.4% | +8.5% / +8.3% | +19.0% / +23.9% | +16.7% / +18.3% |
| Instruments (min) | +158.7% / +171.8% | +151.5% / +155.7% | +137.4% / +142.5% | +147.5% / +134.8% | +226.2% / +233.8% |
| Instruments (pretty) | +132.1% / +142.5% | +128.4% / +130.9% | +109.2% / +115.1% | +109.8% / +98.2% | +198.0% / +202.5% |
| Marine IK (min) | +30.3% / +27.5% | +48.3% / +50.1% | +39.1% / +40.7% | +49.2% / +48.2% | +54.4% / +56.9% |
| Marine IK (pretty) | +35.1% / +38.8% (n/c) | +38.0% / +47.4% | +43.2% / +59.1% (n/c) | +54.4% / +51.2% | +103.4% / +107.9% |
| Random (min) | +61.7% / +73.8% | +82.1% / +95.6% | +67.2% / +76.3% | +84.2% / +99.7% | +115.3% / +126.4% |
| Random (pretty) | +47.8% / +57.8% | +64.0% / +73.1% | +54.4% / +58.2% | +63.5% / +79.6% | +98.2% / +106.0% |
| Twitter (min) | +157.7% / +182.6% | +144.0% / +155.8% | +105.1% / +113.4% | +122.9% / +124.8% | +225.9% / +240.6% |
| Twitter (pretty) | +129.3% / +149.5% | +122.9% / +128.7% | +82.4% / +88.1% | +101.2% / +114.2% | +208.1% / +224.1% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.0% / +25.8% | +33.5% / +44.2% | +40.1% / +56.5% | +19.2% / +53.7% | +33.1% / +39.4% |
| Apache Builds (pretty) | +12.0% / +13.3% | +22.6% / +24.9% | +28.7% / +40.7% | +38.7% / +56.7% | +25.7% / +29.3% |
| Canada (min) | +29.8% / +29.8% | +10.8% / +11.1% | −5.9% / −7.3% | +15.7% / +20.9% | +47.7% / +47.5% |
| Canada (pretty) | +31.8% / +31.9% | +9.4% / +10.3% | +14.8% / +14.9% | +13.0% / +8.5% | +29.8% / +29.9% |
| CitmCatalog (min) | +12.7% / +12.2% | +7.0% / +7.0% | +9.4% / +10.4% | +24.8% / +16.1% | +6.4% / +5.8% |
| CitmCatalog (pretty) | +12.0% / +11.9% | +4.8% / +3.6% | +9.9% / +11.7% | −1.4% / −4.8% | +9.9% / +8.8% |
| Discord (min) | +11.1% / +18.8% | +21.3% / +22.2% | +14.7% / +16.5% | +11.6% / +38.3% | +20.7% / +24.2% |
| Discord (pretty) | +9.1% / +14.4% | +16.7% / +19.2% | +12.9% / +14.1% | +18.3% / +4.6% | +13.0% / +14.8% |
| Github Events (min) | +16.5% / +20.1% | +18.7% / +18.6% | +21.1% / +26.5% | +37.9% / +39.1% | +20.6% / +19.6% |
| Github Events (pretty) | +10.7% / +11.4% | +8.7% / +10.2% | +13.3% / +15.9% | +17.4% / +23.2% | +8.9% / +10.2% |
| Google Maps (min) | +8.4% / +9.9% | +19.1% / +19.6% | +16.0% / +17.4% | +26.5% / +21.8% | +14.2% / +14.5% |
| Google Maps (pretty) | +10.5% / +12.8% | +8.0% / +10.3% | +11.2% / +12.7% | +20.3% / +23.0% | +12.9% / +12.2% |
| Instruments (min) | +15.5% / +21.5% | +12.1% / +13.2% | +11.0% / +11.9% | +20.4% / +10.1% | +14.9% / +14.2% |
| Instruments (pretty) | +7.4% / +9.5% | +4.0% / +4.3% | +7.9% / +5.8% | +27.6% / +11.8% | +3.6% / +3.7% |
| Marine IK (min) | +17.1% / +16.3% | +9.6% / +9.0% | −7.1% / −7.4% | −2.9% / −9.3% | −13.6% / −9.6% |
| Marine IK (pretty) | +16.8% / +15.2% | +3.9% / +5.8% | −9.8% / −9.5% | +8.3% / +11.5% | +17.7% / +16.3% |
| Random (min) | +20.2% / +23.8% | +28.6% / +31.9% | +24.2% / +28.6% | +50.0% / +54.2% | +40.8% / +45.0% |
| Random (pretty) | +16.5% / +16.7% | +17.0% / +19.1% | +12.3% / +15.4% | +40.0% / +37.1% | +30.5% / +33.4% |
| Twitter (min) | +48.3% / +55.0% | +19.6% / +19.9% | +15.6% / +14.5% | +26.7% / +22.7% | +21.0% / +22.7% |
| Twitter (pretty) | +36.4% / +38.4% | +11.2% / +11.2% | +8.7% / +9.6% | +13.4% / +14.4% | +11.4% / +11.5% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.6% / +19.2% | +18.7% / +21.4% | +18.3% / +21.0% | +50.8% / +55.6% | +24.0% / +29.7% |
| Apache Builds (pretty) | +7.8% / +9.3% | +11.0% / +12.9% | +12.2% / +14.2% | +68.6% / +58.4% | +19.6% / +23.8% |
| Canada (min) | +22.9% / +23.1% | +9.0% / +8.9% | −4.7% / −3.7% | +6.9% / +10.1% | +37.5% / +39.2% |
| Canada (pretty) | +24.3% / +23.8% | +7.9% / +9.6% | +12.0% / +12.3% | +7.1% / +7.3% | +25.2% / +24.9% |
| CitmCatalog (min) | +7.4% / +9.3% | +0.9% / +1.4% | +1.5% / +1.7% | +40.8% / +31.6% | +5.5% / +4.6% |
| CitmCatalog (pretty) | +12.9% / +12.6% | −4.0% / −4.4% | +1.4% / +2.3% | +21.4% / −2.1% | +9.3% / +8.6% |
| Discord (min) | +33.5% / +44.2% | +36.3% / +43.3% | +38.2% / +48.2% | +37.8% / +56.0% | +74.2% / +79.3% |
| Discord (pretty) | +29.7% / +37.3% | +29.7% / +35.5% | +31.9% / +38.6% | +46.4% / +44.7% | +57.8% / +62.9% |
| Github Events (min) | +36.8% / +42.8% | +39.1% / +41.5% | +36.7% / +39.4% | +48.9% / +51.9% | +48.8% / +50.7% |
| Github Events (pretty) | +26.1% / +31.1% | +26.4% / +28.9% | +32.0% / +33.5% | +35.7% / +36.6% | +36.5% / +36.5% |
| Google Maps (min) | +13.0% / +13.9% | +14.2% / +13.8% | +6.3% / +4.8% | +5.3% / +5.6% | +15.7% / +17.5% |
| Google Maps (pretty) | +6.2% / +6.6% | +0.8% / +3.2% | +5.7% / +6.7% | +9.5% / +9.8% | +13.8% / +13.8% |
| Instruments (min) | +34.0% / +39.7% | +26.5% / +28.3% | +30.3% / +31.9% | +25.2% / +23.8% | +37.0% / +38.0% |
| Instruments (pretty) | +26.1% / +28.9% | +23.6% / +24.4% | +24.2% / +25.1% | +11.6% / +22.2% | +24.3% / +25.1% |
| Marine IK (min) | +37.6% / +41.0% | +36.3% / +36.8% | +25.5% / +30.8% | +20.8% / +22.0% | +39.2% / +39.1% |
| Marine IK (pretty) | +35.6% / +34.7% | +25.5% / +24.7% | +10.6% / +12.4% | +24.7% / +23.6% | +31.8% / +32.2% |
| Random (min) | +21.5% / +23.1% | +29.0% / +29.6% | +26.2% / +30.0% | +49.2% / +51.0% | +52.8% / +56.8% |
| Random (pretty) | +19.6% / +22.5% | +15.9% / +17.4% | +16.4% / +21.8% | +31.1% / +35.2% | +42.1% / +45.0% |
| Twitter (min) | +47.9% / +53.3% | +22.8% / +23.9% | +20.9% / +21.8% | +24.4% / +27.8% | +33.3% / +32.6% |
| Twitter (pretty) | +36.5% / +38.1% | +18.1% / +18.3% | +17.2% / +16.6% | +21.5% / +16.3% | +22.5% / +22.4% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.4% / +21.4% | +19.4% / +22.5% | +29.6% / +38.3% | +35.7% / +46.7% | +14.3% / +25.2% |
| Apache Builds (pretty) | +2.9% / +13.3% | +17.3% / +19.0% | +21.8% / +32.8% | +29.1% / +46.2% | +13.3% / +19.5% |
| Canada (min) | +8.8% / −3.1% | −4.9% / −5.9% | −14.5% / −19.4% | +7.0% / +10.7% | +5.3% / +6.3% |
| Canada (pretty) | −1.0% / −3.8% | −6.2% / −5.2% | −12.5% / −15.6% | −3.3% / +3.5% | +2.6% / +3.4% |
| CitmCatalog (min) | +3.5% / tie (n/c) | +0.9% / +4.5% | −2.3% / +4.8% | +8.8% / +14.0% | +7.7% / −10.1% |
| CitmCatalog (pretty) | tie / −7.1% | −1.0% / +1.2% | +2.8% / +4.0% | +9.9% / +15.1% | +6.2% / +8.6% |
| Discord (min) | +2.7% / +11.4% | +6.5% / +11.5% | +5.5% / +11.8% | +21.5% / +100.8% | +17.0% / +25.0% |
| Discord (pretty) | tie / +5.4% | +4.1% / +8.0% | +5.1% / +10.6% | +25.6% / +97.0% | +13.5% / +19.7% |
| Github Events (min) | +7.1% / +9.1% | +6.1% / +10.4% | −2.5% / −2.7% | +16.8% / +44.2% | +19.4% / +31.3% |
| Github Events (pretty) | +3.5% / +5.8% | +1.6% / +1.7% | +1.7% / +0.3% | +10.0% / +20.8% | +9.8% / +17.4% |
| Google Maps (min) | +15.9% / +20.1% | +23.2% / +26.0% | +29.1% / +28.3% | +32.1% / +36.2% | +16.9% / +19.7% |
| Google Maps (pretty) | +12.1% / +14.9% | +13.3% / +15.9% | +20.3% / +19.5% | +29.0% / +31.1% | +15.9% / +16.6% |
| Instruments (min) | +11.8% / +15.8% | +1.0% / +0.7% | +1.0% / +0.3% | +12.7% / +12.5% | +9.4% / +8.5% |
| Instruments (pretty) | +8.5% / +11.2% | −3.4% / −1.8% | −2.5% / −2.3% | +9.2% / +1.5% | +6.8% / +4.6% |
| Marine IK (min) | −3.0% / −3.0% | +3.1% / +4.1% | −4.5% / −3.1% | +7.2% / +19.5% | +9.0% / +10.3% |
| Marine IK (pretty) | tie / −3.1% | +2.6% / +3.0% | −5.0% / −3.4% | +7.9% / +10.2% | +7.2% / +8.8% |
| Random (min) | +12.2% / +19.3% | +18.4% / +27.0% | +24.1% / +33.7% | +48.6% / +67.9% | +33.2% / +36.4% |
| Random (pretty) | +8.1% / +13.6% | +14.8% / +19.1% | +19.5% / +26.2% | +50.3% / +68.1% | +29.1% / +33.4% |
| Twitter (min) | +9.0% / +16.1% | +8.1% / +11.7% | +9.7% / +11.2% | +23.7% / +43.4% | +9.6% / +13.7% |
| Twitter (pretty) | +7.9% / +12.4% | +4.5% / +8.6% | +11.5% / +11.5% | +21.1% / +28.8% | +8.0% / +11.7% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +55.6% / +78.2% | +76.5% / +91.1% | +74.1% / +85.6% | +85.9% / +115.5% | +84.5% / +115.5% |
| Apache Builds (pretty) | +42.3% / +68.1% | +59.8% / +72.8% | +65.4% / +75.0% | +63.2% / +81.7% | +74.7% / +97.2% |
| Canada (min) | tie / +0.6% (n/c) | +1.2% / tie | −9.4% / −11.8% | +2.9% / +19.0% | +8.4% / +9.9% |
| Canada (pretty) | −9.3% / +0.4% | −3.2% / −1.0% | −7.0% / −11.2% | +8.8% / tie | +5.8% / +5.1% |
| CitmCatalog (min) | +47.7% / +50.0% | +54.6% / +59.7% | +47.4% / +53.0% | +61.0% / +69.9% | +76.7% / +88.0% |
| CitmCatalog (pretty) | +36.5% / +45.1% | +38.8% / +43.2% | +44.5% / +50.2% | +47.8% / +49.8% | +67.4% / +76.4% |
| Discord (min) | +173.6% / +220.6% | +219.7% / +253.1% | +186.6% / +213.0% | +232.1% / +298.8% | +329.6% / +374.5% |
| Discord (pretty) | +146.2% / +193.5% | +188.9% / +209.9% | +164.8% / +185.8% | +186.6% / +232.6% | +288.8% / +326.3% |
| Github Events (min) | +29.3% / +35.0% | +37.2% / +40.8% | +21.2% / +21.6% | +39.3% / +53.6% | +59.0% / +74.2% |
| Github Events (pretty) | +15.8% / +21.0% | +24.2% / +26.4% | +15.3% / +16.4% | +33.2% / +42.9% | +47.1% / +57.0% |
| Google Maps (min) | +4.6% / +8.3% | +10.6% / +11.2% | +5.7% / +9.7% | +29.8% / +27.6% | +20.6% / +20.6% |
| Google Maps (pretty) | +1.7% / +2.3% | +3.9% / +3.6% | +5.7% / +5.5% | +15.7% / +19.3% | +13.4% / +14.6% |
| Instruments (min) | +134.6% / +135.6% | +139.7% / +145.8% | +121.9% / +125.6% | +133.3% / +151.7% | +225.3% / +239.7% |
| Instruments (pretty) | +111.3% / +119.6% | +120.8% / +128.5% | +103.6% / +103.4% | +95.6% / +112.0% | +196.7% / +211.1% |
| Marine IK (min) | +24.7% / +25.1% | +45.2% / +50.2% | +29.2% / +30.5% | +48.3% / +48.9% | +51.2% / +58.9% |
| Marine IK (pretty) | +22.1% / +24.7% | +44.7% / +45.2% | +25.6% / +31.0% | +40.5% / +44.7% | +45.6% / +49.2% |
| Random (min) | +53.7% / +68.9% | +80.0% / +91.7% | +59.4% / +66.4% | +88.0% / +104.2% | +115.5% / +133.4% |
| Random (pretty) | +43.1% / +48.5% | +60.9% / +67.2% | +49.0% / +55.5% | +60.5% / +82.7% | +99.6% / +108.2% |
| Twitter (min) | +122.1% / +152.2% | +139.9% / +146.4% | +95.9% / +100.5% | +125.4% / +133.8% | +226.3% / +239.7% |
| Twitter (pretty) | +122.7% / +127.7% | +117.3% / +124.3% | +75.6% / +80.7% | +104.2% / +109.9% | +218.0% / +229.0% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +13.3% / +25.2% | +29.5% / +34.7% | +33.4% / +41.6% | +27.8% / +67.2% | +27.2% / +50.6% |
| Apache Builds (pretty) | +8.0% / +12.1% | +19.7% / +19.8% | +22.2% / +30.9% | +36.4% / +38.2% | +19.0% / +19.5% |
| Canada (min) | +28.3% / +37.7% | +12.9% / +15.0% | +7.8% / +10.5% | +10.9% / +13.2% | +6.4% / +5.0% |
| Canada (pretty) | +30.8% / +31.4% | +13.9% / +13.0% | +14.1% / +13.7% | +17.1% / +14.4% | +2.3% / +1.7% |
| CitmCatalog (min) | +13.9% / +17.2% | +4.3% / +9.6% | +11.2% / +10.0% | +12.1% / +12.2% | +11.1% / +11.3% |
| CitmCatalog (pretty) | +10.7% / +15.6% | +11.7% / +9.6% | +12.4% / +11.5% | +14.0% / +11.8% | +9.0% / +9.5% |
| Discord (min) | +6.8% / +11.2% | +10.7% / +18.8% | +7.4% / +8.9% | +16.0% / +25.5% | +15.9% / +20.0% |
| Discord (pretty) | +2.1% / +7.6% | +8.4% / +10.8% | +4.1% / +6.3% | +9.4% / +23.4% | +6.4% / +7.8% |
| Github Events (min) | +10.9% / +12.5% | +12.6% / +14.3% | +17.1% / +18.4% | +28.7% / +22.0% | +10.7% / +11.7% |
| Github Events (pretty) | +6.2% / +8.4% | +8.1% / +6.3% | +9.9% / +8.9% | +20.3% / +12.3% | +7.3% / +2.1% |
| Google Maps (min) | +3.8% / +8.0% | +13.9% / +15.5% | +12.7% / +13.6% | +21.8% / +19.4% | +14.2% / +12.6% |
| Google Maps (pretty) | +7.7% / +9.2% | +7.1% / +5.2% | +8.7% / +6.6% | +16.3% / +20.6% | +12.3% / +13.5% |
| Instruments (min) | +6.3% / +10.7% | +7.6% / +8.6% | +1.1% / +2.0% | +14.1% / +8.0% | +13.3% / +14.2% |
| Instruments (pretty) | −4.2% / +2.4% | +1.0% / +1.2% | −3.1% / −2.4% | tie / +6.4% | +2.5% / +2.7% |
| Marine IK (min) | +11.0% / +15.7% | +7.2% / +6.5% | +1.1% / +1.5% | +8.9% / +6.2% | +7.3% / +7.1% |
| Marine IK (pretty) | +20.6% / +23.2% | +1.3% / +2.7% | +8.4% / +8.2% | +13.1% / +15.4% | +3.2% / +3.1% |
| Random (min) | +8.4% / +9.8% | +16.3% / +19.8% | +15.4% / +19.1% | +28.4% / +38.6% | +25.4% / +29.1% |
| Random (pretty) | +2.2% / +4.4% | +9.2% / +11.1% | +7.4% / +10.0% | +19.8% / +23.8% | +16.9% / +7.6% |
| Twitter (min) | +25.5% / +38.7% | +4.6% / +1.8% | +2.6% / +13.3% | +9.9% / +12.3% | +11.3% / +8.9% |
| Twitter (pretty) | +22.7% / +22.8% | +0.7% / +0.8% | +3.3% / +3.1% | +5.4% / tie | +2.9% / +0.9% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +10.4% / +19.4% | +16.1% / +14.8% | +14.2% / +15.9% | +35.9% / +46.4% | +12.7% / +17.9% |
| Apache Builds (pretty) | −3.2% / +2.9% | +8.0% / +9.7% | +7.5% / +11.1% | +25.7% / +27.6% | +7.6% / +7.2% |
| Canada (min) | +22.2% / +25.8% | +2.6% / +5.1% | +28.5% / +26.0% | +8.1% / +9.4% | +4.2% / +4.0% |
| Canada (pretty) | +20.5% / +21.1% | +13.1% / +12.2% | +24.5% / +24.3% | +16.3% / +15.8% | +1.7% / +2.2% |
| CitmCatalog (min) | +8.7% / +15.9% | +4.6% / +2.8% | +4.3% / +3.1% | +7.0% / +7.9% | −2.2% / +9.6% |
| CitmCatalog (pretty) | +8.6% / +16.4% | −5.4% / −4.4% | +3.6% / +3.0% | +9.7% / +6.0% | +9.5% / +7.6% |
| Discord (min) | +25.1% / +33.1% | +40.4% / +43.8% | +32.3% / +32.3% | +43.8% / +52.0% | +67.1% / +76.8% |
| Discord (pretty) | +23.6% / +25.5% | +31.0% / +38.3% | +25.1% / +29.1% | +28.2% / +34.8% | +48.6% / +54.1% |
| Github Events (min) | +26.2% / +34.7% | +29.1% / +31.9% | +20.9% / +19.3% | +22.2% / +31.5% | +34.6% / +35.4% |
| Github Events (pretty) | +14.7% / +18.8% | +19.8% / +20.0% | +18.6% / +19.1% | +8.7% / +15.9% | +24.7% / +25.2% |
| Google Maps (min) | +11.6% / +11.1% | +10.7% / +11.2% | +6.7% / +4.6% | +7.1% / +7.1% | +14.1% / +18.3% |
| Google Maps (pretty) | +2.6% / +3.5% | −3.3% / −3.2% | +1.1% / +1.7% | +7.8% / +7.9% | +12.0% / +13.9% |
| Instruments (min) | +24.7% / +27.5% | +27.2% / +31.3% | +23.8% / +25.5% | +25.1% / +18.2% | +36.3% / +39.6% |
| Instruments (pretty) | +24.5% / +25.9% | +18.4% / +18.5% | +21.9% / +21.6% | +14.1% / +19.7% | +23.5% / +24.5% |
| Marine IK (min) | +31.7% / +34.5% | +46.2% / +42.6% | +31.5% / +31.7% | +37.2% / +34.9% | +33.5% / +33.9% |
| Marine IK (pretty) | +27.9% / +28.8% | +19.9% / +20.6% | +40.6% / +41.0% | +29.8% / +28.4% | +11.7% / +21.7% |
| Random (min) | +12.0% / +16.3% | +18.8% / +21.2% | +27.5% / +30.1% | +35.6% / +38.1% | +42.6% / +46.5% |
| Random (pretty) | +8.8% / +10.4% | +13.4% / +14.5% | +20.6% / +17.4% | +28.9% / +20.3% | +34.0% / +37.6% |
| Twitter (min) | +22.9% (n/c) / +27.8% | +9.8% / +9.4% | +15.7% / +15.3% | +13.1% / +12.3% | +7.3% / +15.0% |
| Twitter (pretty) | +13.5% / +16.8% | +10.0% / +8.5% | +15.9% / +15.9% | +14.3% / +11.0% | +12.6% / +13.2% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.8x to 4.5x faster at full size: +82.4% (Twitter pretty, Linux/GCC, fresh) up to +347.5% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +75.6% to +374.5%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −6.9% (minified Canada on Linux/GCC, reused) to +30.9%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events almost never loses.** It won 158 of its 160 results; the 2 losses are minified Github Events Small read in order on Linux/GCC (−2.5% fresh, −2.7% reused). Read in reverse at full size it wins on every build (+76.5% to +187.4%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 195 of 200 full-size sparse-reverse results across both allocation modes (no ties, 5 losses), and 194 of 200 small ones (no ties, 6 losses). MSVC won all 40 at full size and 39 of 40 small; no build won all 80. The full-size losses are prettified CitmCatalog on Linux/Clang (−4.0% fresh, −4.4% reused) and macOS/GCC (reused, −2.1%), and minified Canada on Linux/GCC (−4.7% fresh, −3.7% reused). The small losses are prettified CitmCatalog Small (−5.4% fresh, −4.4% reused) and prettified Google Maps Small (−3.3% fresh, −3.2% reused) on Linux/Clang, prettified Apache Builds Small on MSVC (fresh, −3.2%) and minified CitmCatalog Small on macOS/Clang (fresh, −2.2%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 251.9 MB/s on MSVC against 303.0 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 646.7 and 899.8 MB/s. Every MSVC full-size sparse result is a win, by +7.4% to +55.0%, and every full-size sparse-reverse result by +6.2% to +53.3%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 188 of 200 full-size sparse results (no ties, 12 losses) and 195 of 200 small ones (2 ties, 3 losses). MSVC won all 40 of its full-size results, and Linux/Clang won all 80. The full-size losses are on Linux/GCC (6: minified Canada, −5.9% and −7.3%, and Marine IK in both forms, −7.1% to −9.8%), macOS/GCC (4: prettified CitmCatalog, −1.4% fresh and −4.8% reused, and minified Marine IK, −2.9% fresh and −9.3% reused) and macOS/Clang (2: minified Marine IK, −13.6% fresh and −9.6% reused). The 3 small losses are all prettified Instruments Small: on Linux/GCC in both runs (−3.1% and −2.4%) and on MSVC (fresh, −4.2%). The largest is minified Marine IK on macOS/Clang (fresh, −13.6%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 184, tied 6 and lost 10. macOS/Clang went 40-0-0, Linux/Clang 39-0-1, MSVC 37-2-1, macOS/GCC 36-4-0 and Linux/GCC 32-0-8 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 167, tied 4 and lost 29. macOS/GCC and macOS/Clang went 39-0-1, Linux/Clang 33-0-7, MSVC 29-4-7 and Linux/GCC 27-0-13. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's are Canada Small (−4.9% to −6.2%), prettified Instruments Small (−3.4% and −1.8%) and prettified CitmCatalog Small (fresh, −1.0%), MSVC's (Canada Small, Marine IK Small and CitmCatalog Small) run from −1.0% to −7.1%, macOS/GCC's one is prettified Canada Small (fresh, −3.3%), macOS/Clang's one is minified CitmCatalog Small (reused, −10.1%), and Linux/GCC's run from −2.3% to −19.4%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −19.4%).
- **In-order losses at full size fall on two documents.** Linux/GCC lost Canada in both forms and both runs (−7.5% to −11.0%) and Marine IK in both forms and both runs (−2.4% to −4.4%). MSVC lost minified Marine IK (reused, −8.5%, n/c), and Linux/Clang lost prettified Canada (reused, −0.4%). macOS/GCC and macOS/Clang lost none. Canada and Marine IK hold all 10; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 45-1-4 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+17.1% to +145.0%, +55.1% to +123.2%, +44.8% to +100.0% and +21.6% to +123.1%). String is the weakest: 5 wins (+2.6% to +12.6%), 1 tie (macOS/GCC, reused) and 4 losses, on Linux/Clang (−5.8% and −13.6%) and macOS/Clang (−33.2% and −23.6%) in both runs.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 4.4% to 15.7% below its own NDJSON throughput on the same records in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 29 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 29 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 29 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 28 / 30 |
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
| macOS / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| **All** | **25-0-5** | **60-0-0** | **60-0-0** | **145-0-5** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | −7.9% | +15.8% | +32.8% | +27.5% | +18.9% |
| Large Amazon Cellphones (NDJSON) | −10.7% (n/c) | +10.4% | +9.5% | +19.8% | +13.2% |
| Stream Formats Small (NDJSON) | +13.8% | +11.5% | +14.3% | −2.7% | +12.2% (n/c) |
| Stream Formats Small (comma) | +57.1% | +111.2% | +75.0% | +87.5% | +63.7% |
| Stream Formats Large (NDJSON) | +7.5% | +0.8% | +22.8% | +11.4% | −7.2% |
| Stream Formats Large (comma) | +13.0% | +9.0% | +35.3% (n/c) | +15.4% | −4.9% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +11.6% | +25.4% | +26.2% | +48.5% | +31.9% |
| Apache Builds (comma) | +18.3% | +39.1% | +42.6% | +65.7% | +41.8% |
| CitmCatalog (NDJSON) | +3.5% | +11.2% | +14.8% | +5.4% | +14.1% |
| CitmCatalog (comma) | +13.3% | +36.4% | +38.5% | +24.2% | +27.3% |
| Google Maps (NDJSON) | +18.7% | +29.0% | +31.0% | +20.7% | +16.2% |
| Google Maps (comma) | +22.9% | +46.0% | +50.2% | +43.5% | +24.2% |
| Instruments (NDJSON) | +16.5% | +15.7% | +13.0% | +34.7% | +13.8% |
| Instruments (comma) | +32.5% | +46.0% | +47.2% | +68.1% | +32.5% |
| Random (NDJSON) | +19.1% | +29.7% | +27.0% | +53.6% | +57.6% |
| Random (comma) | +24.4% | +41.9% | +34.5% | +74.3% | +67.6% |
| Twitter (NDJSON) | +11.7% | +14.6% | +1.3% | +9.6% | +19.2% |
| Twitter (comma) | +19.6% | +26.5% | +19.1% | +29.2% | +27.6% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +14.2% | +20.1% | +10.1% | +33.4% | +28.3% |
| Apache Builds (comma) | +21.8% | +39.6% | +28.0% | +49.7% | +40.8% |
| CitmCatalog (NDJSON) | +22.1% | +28.1% | +27.9% | +27.8% | +44.0% |
| CitmCatalog (comma) | +35.4% | +50.1% | +47.5% | +58.5% | +61.4% |
| Google Maps (NDJSON) | +5.9% | +10.7% | +5.6% | +21.6% | +14.8% |
| Google Maps (comma) | +11.7% | +22.2% | +17.7% | +26.6% | +21.1% |
| Instruments (NDJSON) | +34.9% | +28.9% | +5.0% | +30.0% (n/c) | +46.6% |
| Instruments (comma) | +42.4% | +53.6% | +31.0% | +53.3% | +65.8% (n/c) |
| Random (NDJSON) | +50.5% | +70.1% | +52.6% | +70.0% | +100.4% |
| Random (comma) | +58.4% | +87.5% | +72.6% | +88.9% | +122.5% |
| Twitter (NDJSON) | +165.3% | +162.5% | +112.6% | +107.0% (n/c) | +244.9% |
| Twitter (comma) | +174.1% | +166.2% (n/c) | +123.5% | +130.4% | +254.7% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 25 of the 30 results in that group and lost 5. It won Stream Formats Small (comma) on all five builds (+57.1% to +111.2%). Amazon Cellphones (+15.8% to +32.8%) and Large Amazon Cellphones (+9.5% to +19.8%) won on every build but MSVC, which lost both (−7.9%, and −10.7% n/c). Stream Formats Small (NDJSON) won on four builds (+11.5% to +14.3%) and lost on macOS/GCC (−2.7%). Stream Formats Large won in both forms on MSVC, Linux/Clang, Linux/GCC and macOS/GCC, and lost both on macOS/Clang (NDJSON −7.2%, comma −4.9%).
- **Two builds won every streaming test,** 30-0-0 each: Linux/Clang and Linux/GCC. macOS/GCC went 29-0-1, and Windows/MSVC and macOS/Clang 28-0-2.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +107.0% to +254.7%, while Google Maps rows, which have few, give +5.6% to +26.6%. Instruments wins on every build (+5.0% to +65.8%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 365.0 MB/s under MSVC and 455.7 MB/s under Linux/Clang, while Jsonifier reads it at 578.4 and 854.6 MB/s.

## 9. Limitations

- **Linux/GCC holds the most single-document losses:** 39 of the 80, against 18 on Linux/Clang, 11 on MSVC and 6 each on macOS/GCC and macOS/Clang. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (−33.2% fresh, −23.6% reused), then Linux/GCC's minified Canada Small read in order (reused, −19.4%), the largest x86 loss.
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 52 of the 80: Canada 32 (20 on Linux/GCC, 7 on Linux/Clang, 4 on MSVC, 1 on macOS/GCC) and Marine IK 20 (12 on Linux/GCC, 4 on MSVC, 2 each on macOS/GCC and macOS/Clang). There much of the cost is float parsing, not field lookup. The other 28 are CitmCatalog 12 (5 on Linux/Clang, 3 on macOS/GCC, 2 on macOS/Clang, 1 each on MSVC and Linux/GCC), Instruments 7 (4 on Linux/GCC, 2 on Linux/Clang, 1 on MSVC), the 4 String POD losses, Google Maps 2 (both Linux/Clang), Github Events 2 (both Linux/GCC) and Apache Builds 1 (MSVC).
- **Linux/GCC loses mostly on Canada.** 20 of its 39 losses are Canada, at both sizes and under every access pattern except the small sparse and sparse-reverse reads, where much of the cost is number parsing rather than field lookup.
- **Small documents carry more than half the losses.** 45 of the 80 losses are small documents (19 on Linux/GCC, 13 on Linux/Clang, 10 on MSVC, 2 on macOS/Clang, 1 on macOS/GCC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Five streaming losses, on three builds:** MSVC's Amazon Cellphones (−7.9%) and Large Amazon Cellphones (−10.7%, n/c), macOS/Clang's Stream Formats Large as NDJSON (−7.2%) and comma-separated (−4.9%), and macOS/GCC's Stream Formats Small as NDJSON (−2.7%).
- **Some results did not converge.** 8 of the 1,650 single-document results did not converge: 6 on Windows/MSVC and 1 each on Linux/GCC and macOS/GCC. 7 of the 150 streaming results did not converge: 2 each on macOS/GCC and macOS/Clang and 1 each on Windows/MSVC, Linux/Clang and Linux/GCC. Only Linux/Clang and macOS/Clang converged on every single-document result.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate, the key trail and the small documents were added, and MSVC was not swept. The wrap count of 2, the trail capacity of 16 and the direction score's limits (engage at 4, saturate at 8) were not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces make up the design:

- a stage-1 reader with a dedicated path for inputs shorter than one SIMD step;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,552, tied 18 and lost 80. On reverse reads it won 384 of 400 at both sizes, and on Windows/MSVC it lost 11 of its 330 single-document results, 10 of them on small documents. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (777-8-40 fresh, 775-10-40 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 52 of the 80 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 145 and lost 5, and it won all 120 corpus record-stream results. The Amazon Cellphones streams on MSVC and the Stream Formats reads on macOS, which hold all five losses, are the main remaining streaming targets.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.