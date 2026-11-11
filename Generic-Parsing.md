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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,565 of 1,650 single-document results against simdjson On Demand, tied 11 and lost 74. On reverse-order reads it won 389 of 400. On Windows/MSVC it won 320 of 330 single-document results, tied 5 and lost 5.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 148 of 150 results and lost 2, and it won all 120 corpus record-stream results.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `b2f5e1d` and simdjson `7002cd3`, November 11, 2026. The tape check cannot use the benchmarked simdjson commit (`f0c2d2a`), because upstream simdjson does not expose its tape: `7002cd3` is upstream `9b33047` (June 2026) plus one commit that adds the tape export, so it predates the stage-1 changes upstream made between `9b33047` and `f0c2d2a`. The AVX-512 builds did not run on the Core i9-14900KF, which has no AVX-512; they ran on cloud-hosted CI runners (the Linux builds report an Azure kernel, `7.0.0-1019-azure`), whose CPU model the harness does not record. The benchmark runs did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-42-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 11 of the 1,650 single-document results and 9 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. MSVC still posts the largest Int64 and Uint64 margins in §7.2 because simdjson's MSVC build is slow on them, not because Jsonifier's is fast: on the same CPU, simdjson reads Int64 (fresh) at 321.7 MB/s under MSVC against 792.4 MB/s under Linux/Clang, while Jsonifier reads it at 855.6 and 1,551.1 MB/s; for Uint64 (fresh) the figures are 319.2 against 734.3 MB/s for simdjson and 838.8 against 1,557.6 MB/s for Jsonifier.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 167 / 170 | 160 / 160 | 327 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 165 / 170 | 159 / 160 | 324 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 168 / 170 | 160 / 160 | 328 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `cb0a5d9`, Jsonifier `b2f5e1d` and simdjson `f0c2d2a`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. Every build ran on November 11, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 161-2-2 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 162-0-3 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 13-1-6 | 18-0-2 | 20-0-0 | 20-0-0 | 146-1-18 |
| macOS / GCC | 5-0-0 | 15-3-2 | 20-0-0 | 19-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 156-3-6 |
| macOS / Clang | 3-0-2 | 19-0-1 | 19-0-1 | 19-0-1 | 20-0-0 | 18-0-2 | 20-0-0 | 18-0-2 | 19-0-1 | 155-0-10 |
| **All** | **22-0-3** | **87-5-8** | **97-0-3** | **95-0-5** | **98-0-2** | **89-1-10** | **98-0-2** | **97-0-3** | **97-0-3** | **780-6-39** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 19-1-0 | 20-0-0 | 20-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 159-3-3 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 18-0-2 | 19-0-1 | 160-0-5 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 15-1-4 | 18-0-2 | 20-0-0 | 20-0-0 | 148-1-16 |
| macOS / GCC | 5-0-0 | 18-0-2 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 161-0-4 |
| macOS / Clang | 3-0-2 | 19-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 18-0-2 | 20-0-0 | 157-1-7 |
| **All** | **22-0-3** | **90-1-9** | **96-2-2** | **96-0-4** | **99-0-1** | **90-2-8** | **98-0-2** | **96-0-4** | **98-0-2** | **785-5-35** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (11 of them did not converge; see §9), Jsonifier won 1,565, tied 11 and lost 74: 780-6-39 with fresh output objects and 785-5-35 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 193 of 200 full-size reverse results (2 ties, 5 losses) and 196 of 200 small reverse results (no ties, 4 losses). Linux/Clang and macOS/GCC won all 80 each. Every reverse loss and tie is Canada: at full size, Linux/GCC lost both forms in both runs (−2.2% to −6.0%) and macOS/Clang lost prettified Canada (fresh, −11.3%), with ties on MSVC (minified, reused) and macOS/Clang (prettified, reused); at small size, Linux/GCC lost Canada Small in both forms and both runs (−4.4% to −8.2%).
- **Windows/MSVC lost five of its 330 single-document results,** all minified in-order reads of Canada or Marine IK: Canada (reused, −3.5%), Marine IK (−3.8% fresh, −7.0% reused) and Marine IK Small (−1.0% fresh, −2.5% reused). Its five ties are minified Canada read in order (fresh) and in reverse (reused), prettified Marine IK read in order (both runs) and minified Canada Small read in order (reused). It won all 10 POD results, 79 of its 80 reverse results and all 160 of its sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.8–12.9% for Jsonifier and 5.1–9.4% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.8 percentage points on both Linux builds, +2.3 on macOS/Clang, +2.6 on macOS/GCC and +5.3 on MSVC. On MSVC, Discord Reverse (min) goes from +209.0% to +227.6%, and Apache Builds (min) from +14.0% to +25.8%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +273.8% / +285.0% | +85.7% / +90.1% | +139.1% / +141.9% | +45.1% / +45.0% | −3.6% / −0.5% |
| Double | +190.5% / +212.8% | +89.2% / +93.1% | +94.6% / +96.1% | +92.9% / +93.5% | +66.3% / +80.3% |
| Int64 | +166.0% / +174.3% | +95.8% / +99.9% | +90.1% / +93.8% | +62.1% / +48.8% | +45.0% / +89.0% |
| String | +36.8% / +54.3% | −6.8% / −12.5% | +4.8% / +5.3% | +6.3% / +6.9% | −15.9% / −19.5% |
| Uint64 | +162.7% / +164.0% | +112.1% / +116.5% | +92.2% / +98.7% | +56.4% / +54.4% | +30.7% / +68.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +14.0% / +25.8% | +18.1% / +23.7% | +21.0% / +34.6% | +40.0% / +64.3% | +26.0% / +33.5% |
| Apache Builds (pretty) | +14.4% / +12.2% | +11.8% / +17.6% | +22.8% / +32.9% | +38.9% / +58.2% | +24.1% / +29.0% |
| Canada (min) | tie / −3.5% | +3.7% / +4.2% | −9.3% / −10.4% | tie / +4.0% | +13.7% (n/c) / +2.0% |
| Canada (pretty) | +4.6% / +5.0% (n/c) | +4.6% / +4.6% | −7.0% / −8.3% | tie / +3.8% | +4.0% / +4.0% |
| CitmCatalog (min) | +13.2% / +10.1% | +7.8% / +6.5% | +10.4% / +11.2% | +19.4% / +5.3% | +9.1% / +9.7% |
| CitmCatalog (pretty) | +15.9% / +18.8% | +4.8% / +3.3% | +3.1% / +4.7% | +12.3% / +13.8% | +8.4% / +9.3% |
| Discord (min) | +18.0% / +24.7% | +14.8% / +21.1% | +12.3% / +20.9% | +26.5% / +35.4% | +20.3% / +21.7% |
| Discord (pretty) | +18.3% / +25.8% | +9.7% / +18.4% | +9.2% / +18.6% | +27.5% / +34.8% | +13.1% / +14.9% |
| Github Events (min) | +34.2% / +46.3% | +31.0% / +37.4% | +29.6% / +34.9% | +43.1% / +54.8% | +45.0% / +55.1% |
| Github Events (pretty) | +30.1% / +42.9% | +29.3% / +33.8% | +37.3% / +31.2% | +74.6% / +50.9% | +37.2% / +44.3% |
| Google Maps (min) | +17.8% / +20.2% | +20.8% / +24.0% | +28.8% / +32.3% | +30.1% / +35.5% | +13.6% / +12.8% |
| Google Maps (pretty) | +21.9% / +32.2% | +13.3% / +14.3% | +22.9% / +24.6% | +28.7% / +30.4% | +8.3% / +11.3% |
| Instruments (min) | +31.0% / +32.0% | +10.5% / +11.6% | +6.1% / +6.4% | tie / +14.8% | +7.4% / +8.6% |
| Instruments (pretty) | +28.9% / +36.3% | +3.8% / +5.7% | +0.9% / +1.6% | +23.4% / +23.0% | −1.2% / −1.4% |
| Marine IK (min) | −3.8% / −7.0% | +6.5% / +7.0% | −2.8% / −3.6% | −6.8% / −6.1% | +2.0% / +16.5% |
| Marine IK (pretty) | tie / tie | +3.1% / +3.5% | −2.9% / −4.2% | −7.0% / −3.4% | +6.1% / +20.9% |
| Random (min) | +26.4% / +32.5% | +20.7% / +25.1% | +30.0% / +36.1% | +96.5% / +61.3% (n/c) | +33.7% / +40.3% |
| Random (pretty) | +25.9% / +33.4% | +17.7% / +21.7% | +27.2% / +35.7% | +51.5% / +62.3% | +29.8% / +33.0% |
| Twitter (min) | +22.7% / +38.1% | +12.3% / +15.8% | +11.7% / +11.1% | +14.8% / +12.6% | +7.8% / +10.2% |
| Twitter (pretty) | +25.5% / +33.9% | +15.3% / +19.9% | +5.9% / +8.2% | +18.8% / +21.8% | +4.0% / +5.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +44.8% / +63.7% | +61.6% / +82.5% | +67.0% / +87.3% | +77.6% / +105.2% | +68.1% / +86.9% |
| Apache Builds (pretty) | +45.3% / +56.7% | +55.2% / +70.5% | +57.9% / +74.9% | +68.6% / +93.0% | +62.2% / +65.5% |
| Canada (min) | +9.3% / tie | +7.8% / +8.3% | −6.0% / −5.1% | +5.7% / +8.6% | +28.3% (n/c) / +10.4% |
| Canada (pretty) | +5.6% (n/c) / +2.9% | +8.8% / +9.4% | −2.8% / −2.2% | +8.0% (n/c) / +1.7% | −11.3% / tie |
| CitmCatalog (min) | +56.3% / +51.1% | +62.5% / +67.3% | +63.1% / +68.5% | +70.8% / +63.4% | +72.9% / +76.2% |
| CitmCatalog (pretty) | +57.2% / +56.4% | +55.1% / +58.4% | +55.1% / +60.5% | +64.8% / +69.5% | +63.4% / +65.4% |
| Discord (min) | +209.0% / +227.6% | +252.4% / +280.4% | +233.3% / +253.1% | +235.0% / +251.8% | +326.6% / +341.6% |
| Discord (pretty) | +185.9% / +212.6% | +201.8% / +218.3% | +211.4% / +223.5% | +206.9% / +250.1% | +276.8% / +288.3% |
| Github Events (min) | +163.2% / +188.2% | +148.2% / +172.1% | +116.1% / +134.6% | +73.7% / +128.9% | +168.3% / +192.9% |
| Github Events (pretty) | +146.6% / +171.6% | +120.4% / +135.7% | +97.4% / +110.4% | +92.0% / +108.4% | +148.1% / +166.1% |
| Google Maps (min) | +21.7% / +23.9% | +9.4% / +10.4% | +12.8% / +14.4% | +25.3% / +26.5% | +14.2% / +17.8% |
| Google Maps (pretty) | +22.5% / +21.9% | +7.5% / +7.7% | +8.9% / +12.3% | +19.2% / +21.6% | +9.1% / +12.1% |
| Instruments (min) | +186.1% / +195.4% | +166.4% / +176.6% | +164.4% / +172.8% | +131.3% / +113.7% | +218.5% / +226.0% |
| Instruments (pretty) | +170.2% / +181.6% | +140.3% / +142.0% | +138.2% / +137.8% | +167.7% / +139.5% | +187.7% / +199.7% |
| Marine IK (min) | +27.4% / +29.2% | +48.7% / +49.4% | +40.8% / +44.6% | +55.3% (n/c) / +66.8% | +49.5% / +60.6% |
| Marine IK (pretty) | +35.9% / +27.6% (n/c) | +51.5% / +53.4% | +39.8% / +43.3% | +40.8% / +62.9% (n/c) | +52.6% / +63.5% |
| Random (min) | +78.4% / +93.9% | +78.8% / +88.3% | +89.6% / +101.2% | +102.9% / +112.1% | +115.7% / +127.1% |
| Random (pretty) | +72.9% / +86.3% | +70.7% / +79.7% | +72.9% / +81.3% | +76.6% / +89.3% | +99.0% / +106.3% |
| Twitter (min) | +226.1% / +238.2% | +167.4% / +179.8% | +132.2% / +148.9% | +138.3% / +146.8% | +215.4% / +237.1% |
| Twitter (pretty) | +202.2% / +213.2% | +140.3% / +150.3% | +104.4% / +110.3% | +111.1% / +121.8% | +200.4% / +212.9% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +20.8% / +37.4% | +31.9% / +45.5% | +40.1% / +55.0% | +51.8% / +111.1% | +23.4% / +26.9% |
| Apache Builds (pretty) | +23.4% / +38.0% | +22.5% / +31.1% | +32.3% / +43.3% | +30.5% / +30.0% | +25.3% / +29.4% |
| Canada (min) | +50.7% / +56.5% | +11.5% / +9.6% | −2.2% / −2.8% | +11.7% / +10.7% | +54.5% / +60.3% |
| Canada (pretty) | +93.4% / +83.2% | +10.4% / +9.4% | +15.4% / +15.9% | +27.5% / +17.7% | +7.6% / +4.4% |
| CitmCatalog (min) | +30.7% / +32.5% | +5.9% / +6.8% | +6.3% / +5.7% | +13.4% / +14.3% | +5.9% / +6.4% |
| CitmCatalog (pretty) | +55.6% / +46.7% | +3.1% / +2.9% | +7.3% / +8.4% | +15.8% / +16.4% | +7.9% / +8.3% |
| Discord (min) | +7.1% / +12.3% | +7.5% / +10.4% | +24.2% / +26.5% | +21.1% / +24.5% | +18.9% / +21.0% |
| Discord (pretty) | +10.8% / +15.7% | +6.5% / +5.7% | +19.7% / +22.9% | +17.9% / +24.3% | +8.0% / +10.0% |
| Github Events (min) | +44.5% / +64.1% | +15.4% / +17.5% | +23.0% / +25.0% | +26.8% / +31.5% | +17.9% / +19.6% |
| Github Events (pretty) | +49.9% / +56.5% | +9.1% / +8.6% | +18.7% / +16.5% | +21.0% / +22.3% | +5.4% / +6.5% |
| Google Maps (min) | +26.0% / +29.5% | +18.0% / +17.6% | +20.7% / +23.1% | +25.1% / +14.5% | +13.4% / +14.2% |
| Google Maps (pretty) | +27.1% / +32.1% | +10.1% / +11.0% | +15.3% / +14.3% | +18.7% / +7.8% | +10.2% / +10.7% |
| Instruments (min) | +33.8% / +34.9% | +8.7% / +10.7% | +10.8% / +9.7% | +15.2% / +26.7% | +11.8% / +12.4% |
| Instruments (pretty) | +37.5% / +40.5% | +8.3% / +7.0% | +6.6% / +6.5% | +11.2% / +22.7% | −0.9% / +1.3% |
| Marine IK (min) | +27.7% / +26.0% | +10.3% / +9.7% | −6.1% / −6.8% | −7.5% / +15.1% (n/c) | +17.7% / +17.4% |
| Marine IK (pretty) | +65.2% / +60.9% | +9.6% / +9.9% | −9.3% / −9.1% | +8.2% / −3.4% | +17.2% / +17.9% |
| Random (min) | +30.6% / +41.1% | +22.9% / +25.4% | +23.7% / +27.1% | +47.1% / +54.4% | +35.6% / +38.2% |
| Random (pretty) | +28.1% / +26.9% | +18.2% / +20.2% | +12.9% / +19.4% | +35.3% / +43.5% | +25.0% / +26.2% |
| Twitter (min) | +59.7% / +63.1% | +17.7% / +18.4% | +14.1% / +14.7% | +5.3% / +28.0% | +17.6% / +17.0% |
| Twitter (pretty) | +50.9% / +55.1% | +12.0% / +12.3% | +10.7% / +11.8% | +12.5% / +12.4% | +7.5% / +8.3% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.3% / +23.8% | +18.5% / +23.8% | +20.6% / +25.8% | +40.6% / +78.7% | +27.2% / +29.8% |
| Apache Builds (pretty) | +19.1% / +25.8% | +10.4% / +12.6% | +13.3% / +18.2% | +26.8% / +37.9% | +19.0% / +35.8% |
| Canada (min) | +38.0% / +36.3% | +7.0% / +7.4% | −2.6% / −2.3% | −5.3% / +4.2% | +35.2% / +36.1% |
| Canada (pretty) | +69.2% / +68.2% | +8.5% / +8.8% | +10.9% / +12.2% | +5.6% / +31.3% | +24.9% / +25.0% |
| CitmCatalog (min) | +20.6% / +19.1% | +1.5% / +2.8% | +5.7% / +5.8% | +6.0% / +5.0% | +5.9% / +6.2% |
| CitmCatalog (pretty) | +31.2% / +29.0% | +1.9% / +1.7% | +5.0% / +5.5% | +8.9% / +26.0% | +6.9% / +7.0% |
| Discord (min) | +42.4% / +53.4% | +33.8% / +37.2% | +49.7% / +53.9% | +42.2% / +75.3% | +72.4% / +76.6% |
| Discord (pretty) | +46.4% / +47.8% | +29.1% / +31.1% | +38.5% / +40.3% | +55.1% / +12.7% | +56.1% / +57.7% |
| Github Events (min) | +66.7% / +77.4% | +35.6% / +36.7% | +41.6% / +42.7% | +47.6% / +46.2% | +48.7% / +50.6% |
| Github Events (pretty) | +59.7% / +66.6% | +30.8% / +31.9% | +30.4% / +33.6% | +34.7% / +35.9% | +32.9% / +35.0% |
| Google Maps (min) | +19.0% / +23.5% | +3.3% / +2.7% | +7.1% / +7.5% | +27.2% / +16.8% | +7.9% / +19.1% |
| Google Maps (pretty) | +24.1% / +25.8% | +0.8% / +2.1% | +5.4% / +5.3% | +9.6% / +8.1% | +5.2% / +5.4% |
| Instruments (min) | +52.3% / +60.8% | +25.7% / +26.5% | +29.9% / +32.1% | +25.6% / +41.3% | +36.6% / +37.6% |
| Instruments (pretty) | +50.2% / +59.9% | +14.2% / +13.2% | +19.7% / +19.7% | +13.5% / +35.2% | +22.5% / +22.9% |
| Marine IK (min) | +51.5% / +51.1% | +39.0% / +40.0% | +22.9% / +22.0% | +18.4% / +16.6% | +40.1% / +43.0% |
| Marine IK (pretty) | +72.0% / +72.5% | +27.0% / +27.1% | +11.4% / +11.0% | +27.7% / +30.8% | +32.7% / +32.8% |
| Random (min) | +37.4% / +40.7% | +28.1% / +31.1% | +21.8% / +25.7% | +24.7% / +48.8% | +47.3% / +50.5% |
| Random (pretty) | +45.5% / +43.2% | +17.8% / +21.4% | +19.5% / +21.5% | +38.1% / +31.3% | +37.4% / +38.6% |
| Twitter (min) | +62.1% / +70.0% | +28.4% / +29.1% | +28.5% / +29.2% | +27.2% / +27.0% | +30.8% / +31.7% |
| Twitter (pretty) | +63.3% / +69.0% | +24.7% / +24.2% | +23.5% / +24.2% | +16.9% / +16.8% | +20.4% / +20.9% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +6.8% / +18.5% | +12.3% / +16.1% | +22.8% / +34.4% | +34.5% / +65.0% | +14.4% / +22.6% |
| Apache Builds (pretty) | +5.3% / +10.1% | +13.5% / +17.7% | +23.4% / +30.7% | +30.1% / +39.5% | +12.6% / +16.6% |
| Canada (min) | +6.6% / tie | +6.5% / +3.7% | −12.6% / −14.1% | +1.5% / +1.4% | −1.2% / −1.8% |
| Canada (pretty) | +1.3% / +1.5% | +1.4% / +0.7% | −11.1% / −12.7% | +1.1% / +1.3% | +1.1% / +1.1% |
| CitmCatalog (min) | +8.9% / +8.6% | +2.0% / +3.5% | tie / +3.4% | +9.9% / +11.4% | +4.8% / +8.1% |
| CitmCatalog (pretty) | +11.8% / +13.8% | −3.0% / −2.2% | −1.6% / +1.5% | +9.0% / +10.8% | +3.0% / +5.2% |
| Discord (min) | +7.0% / +17.1% | +7.2% / +12.9% | +5.9% / +13.1% | +20.1% / +8.5% | +11.1% / +19.6% |
| Discord (pretty) | +7.0% / +16.7% | +5.7% / +10.8% | +6.2% / +11.3% | +17.7% / +32.8% | +7.4% / +12.1% |
| Github Events (min) | +24.5% / +28.8% | +12.1% / +14.2% | +8.2% / +8.2% | +17.3% / +25.1% | +20.2% / +25.5% |
| Github Events (pretty) | +22.2% / +27.1% | +7.2% / +8.4% | +8.4% / +7.0% | +11.9% / +2.7% | +9.8% / +14.9% |
| Google Maps (min) | +24.2% / +28.1% | +18.2% / +21.2% | +26.7% / +29.7% | +33.8% / +41.3% | +13.2% / +14.5% |
| Google Maps (pretty) | +21.9% / +28.9% | +12.1% / +12.8% | +22.0% / +24.1% | +27.2% / +32.6% | +10.3% / +10.3% |
| Instruments (min) | +31.8% / +33.1% | +4.0% / +5.5% | +0.6% / +1.7% | +12.3% / +13.9% | +8.4% / +9.1% |
| Instruments (pretty) | +29.6% / +30.2% | +2.0% / +1.6% | −3.2% / −5.3% | +8.5% / +11.2% | −0.5% / −0.7% |
| Marine IK (min) | −1.0% / −2.5% | +5.9% / +6.4% | −1.4% / tie | +7.9% / +9.2% | +1.6% / +17.0% |
| Marine IK (pretty) | +3.1% / +5.0% | +4.5% / +4.6% | −3.0% / −2.9% | +6.5% / +7.3% | +3.6% / +20.0% |
| Random (min) | +22.8% / +29.4% | +15.5% / +20.1% | +24.4% / +32.8% | +47.0% / +61.3% | +22.8% / +32.8% |
| Random (pretty) | +20.1% / +28.5% | +13.4% / +17.2% | +19.5% / +26.2% | +41.4% / +52.2% | +24.5% / +26.1% |
| Twitter (min) | +16.9% / +19.8% | +8.2% / +8.2% | +5.9% / +9.6% | +23.0% / +28.0% | +8.4% / +12.7% |
| Twitter (pretty) | +13.1% / +23.6% | +3.7% / +4.4% | +7.1% / +9.5% | +19.8% / +24.1% | +3.8% / +6.3% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +57.3% / +85.5% | +86.1% / +101.6% | +86.7% / +99.8% | +85.5% / +117.8% | +83.8% / +110.8% |
| Apache Builds (pretty) | +52.5% / +74.6% | +72.5% / +83.6% | +75.1% / +85.0% | +71.1% / +91.1% | +81.1% / +106.7% |
| Canada (min) | +0.9% / +3.7% | +6.7% / +6.2% | −4.4% / −5.8% | +3.1% / +3.3% | +2.5% / +3.1% |
| Canada (pretty) | +3.3% / +4.8% | +4.1% / +3.1% | −7.9% / −8.2% | +3.2% / +3.9% | +4.6% / +4.0% |
| CitmCatalog (min) | +63.1% / +67.2% | +59.4% / +68.5% | +59.4% / +64.8% | +65.9% / +75.5% | +73.1% / +86.4% |
| CitmCatalog (pretty) | +60.4% / +67.0% | +47.0% / +52.6% | +55.3% / +58.3% | +57.1% / +64.2% | +64.8% / +67.6% |
| Discord (min) | +200.5% / +267.6% | +240.7% / +267.8% | +215.9% / +256.6% | +246.6% / +302.8% | +322.3% / +377.8% |
| Discord (pretty) | +183.6% / +240.7% | +196.5% / +226.4% | +199.7% / +225.8% | +213.1% / +256.4% | +282.3% / +328.5% |
| Github Events (min) | +53.2% / +61.1% | +39.1% / +45.1% | +33.4% / +34.1% | +41.2% / +53.3% | +62.9% / +83.1% |
| Github Events (pretty) | +41.6% / +45.4% | +31.4% / +34.9% | +24.6% / +29.3% | +9.2% (n/c) / +33.0% | +46.5% / +59.1% |
| Google Maps (min) | +19.4% / +28.0% | +4.5% / +5.3% | +10.3% / +10.3% | +22.0% / +23.6% | +14.2% / +17.4% |
| Google Maps (pretty) | +20.7% / +23.6% | +2.5% / +3.3% | +5.0% / +7.6% | +19.7% / +20.6% | +10.8% / +10.6% |
| Instruments (min) | +165.5% / +173.2% | +163.0% / +166.9% | +160.3% / +165.9% | +147.3% / +155.7% | +217.5% / +233.0% |
| Instruments (pretty) | +156.5% / +163.8% | +135.0% / +134.0% | +130.2% / +137.2% | +124.1% / +125.9% | +193.1% / +198.3% |
| Marine IK (min) | +26.9% / +30.0% | +38.5% / +46.9% | +37.2% / +40.7% | +48.9% / +53.4% | +44.2% / +57.2% |
| Marine IK (pretty) | +28.6% / +33.2% | +35.9% / +39.4% | +34.4% / +36.9% | +39.2% / +42.2% | +50.8% / +56.3% |
| Random (min) | +75.0% / +91.2% | +77.0% / +87.8% | +81.8% / +97.6% | +89.9% / +109.0% | +116.6% / +131.1% |
| Random (pretty) | +65.1% / +79.5% | +64.9% / +70.9% | +71.1% / +73.6% | +71.3% / +72.3% | +95.6% / +108.1% |
| Twitter (min) | +175.6% / +188.6% | +165.3% / +176.1% | +142.6% / +149.2% | +139.5% / +152.3% | +225.3% / +239.8% |
| Twitter (pretty) | +149.8% / +164.8% | +142.7% / +152.3% | +117.3% / +121.6% | +126.0% / +161.5% | +207.5% / +221.5% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +24.6% / +46.5% | +29.3% / +31.4% | +35.4% / +45.8% | +47.8% / +65.2% | +28.0% / +37.3% |
| Apache Builds (pretty) | +23.6% / +39.5% | +20.3% / +22.3% | +27.0% / +31.2% | +20.1% / +45.7% | +18.1% / +18.6% |
| Canada (min) | +45.7% / +51.6% | +8.9% / +10.2% | +25.9% / +24.1% | +9.3% / +11.6% | +5.4% / +4.5% |
| Canada (pretty) | +78.8% / +81.0% | +3.0% / +3.4% | +25.3% / +23.6% | +9.1% / +10.8% | +2.0% / +1.0% |
| CitmCatalog (min) | +31.6% / +35.5% | +3.2% / +2.1% | +6.2% / +4.6% | +17.8% / +15.3% | +11.1% / +11.7% |
| CitmCatalog (pretty) | +39.5% / +45.7% | +3.2% / +2.1% | +8.3% / +6.2% | +14.5% / +9.8% | +6.6% / +7.1% |
| Discord (min) | +13.5% / +21.1% | +8.8% / +12.8% | +10.4% / +13.9% | +19.4% / +28.4% | +13.9% / +16.8% |
| Discord (pretty) | +14.9% / +22.0% | +4.3% / +8.0% | +8.8% / +12.4% | +13.4% / +17.4% | +1.9% / +0.8% |
| Github Events (min) | +40.1% / +44.9% | +6.8% / +9.5% | +13.6% / +15.9% | +26.4% / +26.5% | +9.7% / +7.1% |
| Github Events (pretty) | +30.1% / +36.7% | +1.4% / +4.0% | +14.1% / +14.6% | +27.6% / +16.0% | +4.9% / +2.2% |
| Google Maps (min) | +25.1% / +27.4% | +11.0% / +13.0% | +17.8% / +14.7% | +21.6% / +22.8% | +14.7% / +17.0% |
| Google Maps (pretty) | +24.5% / +27.2% | +6.3% / +9.9% | +13.5% / +7.8% | +19.5% / +19.5% | +10.1% / +10.5% |
| Instruments (min) | +25.4% / +35.4% | +4.0% / +4.1% | +3.3% / +2.1% | +8.2% / +9.4% | +10.6% / +11.2% |
| Instruments (pretty) | +28.8% / +34.4% | +2.2% / +2.6% | +0.9% / +2.1% | +4.6% / +4.1% | −2.0% / −2.1% |
| Marine IK (min) | +23.1% / +38.3% | −11.5% / −9.4% | +2.1% / +3.0% | +12.9% / +11.5% | +8.9% / +9.3% |
| Marine IK (pretty) | +50.6% / +53.3% | +4.6% / −4.9% | +10.9% / +10.9% | +17.1% / +13.7% | +2.8% / +2.7% |
| Random (min) | +16.1% / +19.1% | +19.5% / +19.6% | +16.9% / +20.4% | +31.9% / +40.0% | +25.0% / +30.5% |
| Random (pretty) | +16.6% / +21.7% | +12.6% / +14.2% | +10.6% / +13.4% | +24.0% / +28.7% | +15.0% / +19.0% |
| Twitter (min) | +20.4% / +22.7% | +14.1% / +13.2% | +9.8% / +6.3% | +13.0% / +14.8% | +7.1% / +6.7% |
| Twitter (pretty) | +19.0% / +19.3% | +6.1% / +5.9% | +11.5% / +8.3% | +3.9% / +6.5% | −0.9% / −2.1% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.8% / +34.8% | +12.3% / +15.5% | +20.7% / +27.1% | +34.3% / +30.1% | +21.5% / +24.7% |
| Apache Builds (pretty) | +17.9% / +24.5% | +5.6% / +8.6% | +15.1% / +20.1% | +22.0% / +28.2% | +12.5% / +13.1% |
| Canada (min) | +31.0% / +31.3% | +2.9% / +5.1% | +19.3% / +9.9% | +9.0% / +10.4% | −7.2% / +4.5% |
| Canada (pretty) | +57.1% / +61.8% | +8.4% / +9.0% | +29.1% / +27.6% | +10.0% / +8.5% | +1.5% / +1.2% |
| CitmCatalog (min) | +26.7% / +28.8% | +3.4% / +1.8% | +2.9% / +1.3% | −4.9% / −14.3% | +8.8% / +10.3% |
| CitmCatalog (pretty) | +27.8% / +33.3% | +0.5% / −0.6% | +5.0% / +2.2% | +16.0% / +19.4% | +8.0% / +6.2% |
| Discord (min) | +46.5% / +57.5% | +41.6% / +47.7% | +44.0% / +49.4% | +47.1% / +55.4% | +66.2% / +72.2% |
| Discord (pretty) | +37.9% / +48.8% | +34.0% / +36.6% | +36.5% / +36.9% | +33.7% / +58.5% | +48.2% / +52.5% |
| Github Events (min) | +45.2% / +55.1% | +19.7% / +16.1% | +24.8% / +23.0% | +26.8% / +21.3% | +31.5% / +31.9% |
| Github Events (pretty) | +41.6% / +48.8% | +18.1% / +22.5% | +22.1% / +23.7% | +10.5% / +13.2% | +23.7% / +24.4% |
| Google Maps (min) | +16.3% / +17.7% | +1.0% / +1.6% | +4.3% / +2.3% | +9.3% / +9.3% | +6.8% / +8.3% |
| Google Maps (pretty) | +24.1% / +20.6% | +0.6% / +2.8% | +2.5% / +3.0% | −1.7% / +10.9% | +4.4% / +16.6% |
| Instruments (min) | +44.3% / +52.8% | +34.8% / +30.1% | +29.6% / +28.5% | +27.0% / +27.9% | +37.2% / +39.3% |
| Instruments (pretty) | +47.5% / +54.8% | +15.6% / +17.4% | +23.3% / +23.1% | +18.9% / +17.9% | +22.5% / +23.9% |
| Marine IK (min) | +37.4% / +42.5% | +37.5% / +36.5% | +39.3% / +39.9% | +40.3% / +42.9% | +27.0% / +27.7% |
| Marine IK (pretty) | +61.1% / +63.9% | +37.4% / +38.4% | +39.6% / +43.6% | +32.0% / +31.0% | +20.7% / +21.0% |
| Random (min) | +29.2% / +31.8% | +30.2% / +29.2% | +19.9% / +20.9% | +36.8% / +41.2% | +41.8% / +46.9% |
| Random (pretty) | +25.4% / +29.8% | +18.7% / +17.8% | +18.0% / +17.1% | +44.3% / +31.8% | +29.8% / +34.1% |
| Twitter (min) | +31.9% / +36.8% | +17.0% / +15.0% | +20.6% / +19.9% | +18.4% / +17.8% | +21.7% / +22.8% |
| Twitter (pretty) | +22.8% / +31.2% | +14.4% / +12.9% | +13.5% / +13.2% | +10.1% / +10.3% | +11.5% / +12.9% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.0x to 4.4x faster at full size: +104.4% (Twitter pretty, Linux/GCC, fresh) up to +341.6% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +117.3% to +377.8%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −11.3% (prettified Canada on macOS/Clang, fresh) to +28.3%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** None of the 74 single-document losses or 11 ties is a Github Events read: it wins all 160 of its results. Read in reverse at full size it wins on every build (+73.7% to +192.9%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 197 of 200 full-size sparse-reverse results across both allocation modes (no ties, 3 losses), and 195 of 200 small ones (no ties, 5 losses). MSVC won all 80. The full-size losses are minified Canada on Linux/GCC (−2.6% fresh, −2.3% reused) and on macOS/GCC (fresh, −5.3%). Of the 5 small losses, 3 are on macOS/GCC (minified CitmCatalog Small, −4.9% fresh and −14.3% reused, and prettified Google Maps Small, fresh, −1.7%), 1 on macOS/Clang (minified Canada Small, fresh, −7.2%) and 1 on Linux/Clang (prettified CitmCatalog Small, reused, −0.6%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 228.8 MB/s on MSVC against 288.9 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 654.2 and 871.9 MB/s. Every MSVC full-size sparse result is a win, by +7.1% to +93.4%, and every full-size sparse-reverse result by +15.3% to +77.4%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 191 of 200 full-size sparse results (no ties, 9 losses) and 193 of 200 small ones (no ties, 7 losses). MSVC won all 40 of its full-size results and all 40 small. The full-size losses are on Linux/GCC (6: minified Canada, −2.2% and −2.8%, and Marine IK, −6.1% to −9.3%), macOS/GCC (2: Marine IK, minified fresh −7.5% and prettified reused −3.4%) and macOS/Clang (1: prettified Instruments, fresh, −0.9%). macOS/Clang holds 4 of the 7 small losses (prettified Instruments Small and Twitter Small, −0.9% to −2.1%), and Linux/Clang the other 3, all Marine IK Small, including the largest (minified, fresh, −11.5%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 177, tied 6 and lost 17. Linux/Clang went 40-0-0, macOS/Clang 38-0-2, MSVC 34-3-3, macOS/GCC 33-3-4 and Linux/GCC 32-0-8 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 179, tied 3 and lost 18. macOS/GCC went 40-0-0, Linux/Clang 38-0-2, MSVC 37-1-2, macOS/Clang 36-0-4 and Linux/GCC 28-2-10. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's are prettified CitmCatalog Small (−3.0% and −2.2%), macOS/Clang's run from −0.5% to −1.8%, and Linux/GCC's from −1.4% to −14.1%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −14.1%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−7.0% to −10.4%) and Marine IK in both forms and both runs (−2.8% to −4.2%). macOS/GCC lost Marine IK in both forms and both runs (−3.4% to −7.0%). MSVC lost minified Marine IK in both runs (−3.8% and −7.0%) and minified Canada (reused, −3.5%). macOS/Clang lost prettified Instruments in both runs (−1.2% and −1.4%). Linux/Clang lost none. Canada and Marine IK, which hold 15 of the 17, are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 44-0-6 on them. Double, Int64 and Uint64 won all 10 results each (+66.3% to +212.8%, +45.0% to +174.3% and +30.7% to +164.0%), and Bool won 8 (+45.0% to +285.0%), losing on macOS/Clang in both runs (−3.6% and −0.5%). String is the weakest: 6 wins (+4.8% to +54.3%) and 4 losses, on Linux/Clang (−6.8% and −12.5%) and macOS/Clang (−15.9% and −19.5%) in both runs.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 2.9% to 14.0% below its own NDJSON throughput on the same records in 17 of the 18 build and stream pairs; the exception is Apache Builds on MSVC, where comma runs 0.7% above.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 26 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 25 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 30 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| **All** | **28-0-2** | **60-0-0** | **60-0-0** | **148-0-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +35.2% | +13.8% | +20.1% | +29.3% | +21.3% |
| Large Amazon Cellphones (NDJSON) | +16.2% | +10.6% | +5.9% | +12.5% | +15.3% |
| Stream Formats Small (NDJSON) | +43.6% | +2.8% | +25.5% | +25.3% | +8.8% |
| Stream Formats Small (comma) | +92.4% | +91.1% | +111.7% | +103.7% | +59.5% |
| Stream Formats Large (NDJSON) | +96.3% | +8.7% | +24.2% | +16.4% | −8.1% |
| Stream Formats Large (comma) | +96.2% | +11.5% | +50.4% | +12.5% | −4.7% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +17.4% | +19.5% | +24.3% | +78.6% | +31.9% |
| Apache Builds (comma) | +25.2% | +35.7% | +41.4% | +65.2% | +42.4% |
| CitmCatalog (NDJSON) | +18.4% | +6.6% | +5.7% | +7.9% | +10.8% |
| CitmCatalog (comma) | +32.0% | +28.6% | +35.8% | +37.1% | +24.2% |
| Google Maps (NDJSON) | +23.2% | +31.9% | +31.0% | +28.2% | +12.3% |
| Google Maps (comma) | +28.7% | +47.0% | +48.1% | +38.8% | +20.5% |
| Instruments (NDJSON) | +43.9% | +12.8% | +14.6% | +42.0% | +5.3% |
| Instruments (comma) | +64.8% | +35.8% | +46.9% | +70.0% | +17.2% |
| Random (NDJSON) | +26.2% | +22.0% | +27.6% | +67.4% | +50.9% |
| Random (comma) | +34.7% (n/c) | +34.6% | +44.3% | +78.8% (n/c) | +57.6% |
| Twitter (NDJSON) | +20.2% | +12.6% | +2.3% | +22.5% | +13.1% |
| Twitter (comma) | +31.4% | +27.5% | +21.6% | +34.0% | +22.2% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +31.2% | +16.0% | +13.3% | +56.9% | +25.5% |
| Apache Builds (comma) | +33.7% | +35.1% | +34.7% | +57.2% | +36.4% |
| CitmCatalog (NDJSON) | +41.0% | +26.4% | +29.7% | +79.1% (n/c) | +40.5% |
| CitmCatalog (comma) | +47.5% | +51.9% | +48.5% | +33.1% (n/c) | +59.9% |
| Google Maps (NDJSON) | +17.5% | +14.5% | +8.3% | +16.4% | +10.8% |
| Google Maps (comma) | +27.1% | +27.7% | +20.0% | +24.8% | +15.9% |
| Instruments (NDJSON) | +68.2% | +36.6% | +29.1% | +29.1% | +39.6% |
| Instruments (comma) | +88.8% | +61.6% | +57.2% | +54.1% | +58.6% |
| Random (NDJSON) | +87.7% (n/c) | +73.9% | +74.6% | +92.7% (n/c) | +106.9% |
| Random (comma) | +84.7% | +91.8% | +89.2% | +100.5% | +118.2% |
| Twitter (NDJSON) | +224.7% (n/c) | +181.9% | +143.4% | +127.5% (n/c) | +242.4% |
| Twitter (comma) | +264.5% (n/c) | +189.7% | +154.1% | +146.3% | +248.5% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 28 of the 30 results in that group and lost 2. It won Stream Formats Small (comma) (+59.5% to +111.7%), Amazon Cellphones (+13.8% to +35.2%), Large Amazon Cellphones (+5.9% to +16.2%) and Stream Formats Small (NDJSON) (+2.8% to +43.6%) on all five builds. Stream Formats Large won on four builds in both forms and lost on macOS/Clang (comma −4.7%, NDJSON −8.1%).
- **Four builds won every streaming test,** 30-0-0 each: Windows/MSVC, Linux/Clang, Linux/GCC and macOS/GCC. macOS/Clang went 28-0-2, losing only its two Stream Formats Large reads.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +127.5% to +264.5%, while Google Maps rows, which have few, give +8.3% to +27.7%. Instruments wins on every build (+29.1% to +88.8%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 321.5 MB/s under MSVC and 459.7 MB/s under Linux/Clang, while Jsonifier reads it at 593.8 and 881.9 MB/s.

## 9. Limitations

- **Linux/GCC holds the most single-document losses:** 34 of the 74, against 17 on macOS/Clang, 10 on macOS/GCC, 8 on Linux/Clang and 5 on MSVC. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (reused, −19.5%) and macOS/GCC's minified CitmCatalog Small read sparse in reverse (reused, −14.3%); the largest x86 loss is Linux/GCC's minified Canada Small read in order (reused, −14.1%).
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 50 of the 74: Canada 26 (20 on Linux/GCC, 4 on macOS/Clang, 1 on MSVC, 1 on macOS/GCC) and Marine IK 24 (11 on Linux/GCC, 6 on macOS/GCC, 4 on MSVC, 3 on Linux/Clang). There much of the cost is float parsing, not field lookup. CitmCatalog and Google Maps are not float-heavy; they are made of small objects, where the field index and the key trail have little to save. CitmCatalog adds 6 losses (3 on Linux/Clang, 2 on macOS/GCC, 1 on Linux/GCC) and Google Maps adds 1 (on macOS/GCC).
- **Linux/GCC loses mostly on Canada.** 20 of its 34 losses are Canada, at both sizes and under every access pattern, where much of the cost is number parsing rather than field lookup.
- **Small documents carry almost half the losses.** 34 of the 74 losses are small documents (14 on Linux/GCC, 9 on macOS/Clang, 6 on Linux/Clang, 3 on macOS/GCC, 2 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **macOS/Clang holds both streaming losses:** Stream Formats Large as NDJSON (−8.1%) and comma-separated (−4.7%).
- **Some results did not converge.** 11 of the 1,650 single-document results did not converge: 6 on macOS/GCC, 3 on Windows/MSVC and 2 on macOS/Clang. 9 of the 150 streaming results did not converge: 5 on macOS/GCC and 4 on Windows/MSVC. Linux/Clang and Linux/GCC converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,565, tied 11 and lost 74. On reverse reads it won 389 of 400 at both sizes, and on Windows/MSVC it lost 5 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (780-6-39 fresh, 785-5-35 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 50 of the 74 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 148 and lost 2, and it won all 120 corpus record-stream results. The Stream Formats Large reads on macOS/Clang, which hold both losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.