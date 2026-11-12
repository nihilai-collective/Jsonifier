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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,542 of 1,650 single-document results against simdjson On Demand, tied 14 and lost 94. On reverse-order reads it won 381 of 400. On Windows/MSVC it won 308 of 330 single-document results, tied 1 and lost 21.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 146 of 150 results, tied 1 and lost 3, and it won all 120 corpus record-stream results.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `55d73f0` and simdjson `7002cd3`, November 12, 2026. The tape check cannot use the benchmarked simdjson commit (`05da81d`), because upstream simdjson does not expose its tape: `7002cd3` is upstream `9b33047` (June 2026) plus one commit that adds the tape export, so it predates the stage-1 changes upstream made between `9b33047` and `05da81d`. The AVX-512 builds did not run on the Core i9-14900KF, which has no AVX-512; they ran on cloud-hosted CI runners (the Linux builds report an Azure kernel, `7.0.0-1019-azure`), whose CPU model the harness does not record. The benchmark runs did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-42-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 14 of the 1,650 single-document results and 18 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. MSVC still posts the largest Int64 margins in §7.2, and Uint64 margins close to the largest, because simdjson's MSVC build is slow on them, not because Jsonifier's is fast: on the same CPU, simdjson reads Int64 (fresh) at 420.6 MB/s under MSVC against 796.1 MB/s under Linux/Clang, while Jsonifier reads it at 881.5 and 1,540.8 MB/s; for Uint64 (fresh) the figures are 418.5 against 728.8 MB/s for simdjson and 875.6 against 1,506.4 MB/s for Jsonifier.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 167 / 170 | 159 / 160 | 326 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 166 / 170 | 157 / 160 | 323 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 167 / 170 | 160 / 160 | 327 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `0aaa9ac`, Jsonifier `55d73f0` and simdjson `05da81d`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The Linux builds ran on November 11, 2026, the Windows and macOS builds on November 12, and the results were published to `generic-parsing-main` on November 12.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-0-4 | 18-1-1 | 20-0-0 | 20-0-0 | 16-0-4 | 18-0-2 | 20-0-0 | 20-0-0 | 153-1-11 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 17-0-3 | 20-0-0 | 20-0-0 | 20-0-0 | 161-0-4 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 14-0-6 | 18-0-2 | 19-0-1 | 19-0-1 | 145-0-20 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 19-0-1 | 19-1-0 | 18-1-1 | 18-0-2 | 17-1-2 | 18-1-1 | 152-5-8 |
| macOS / Clang | 4-0-1 | 19-1-0 | 19-1-0 | 20-0-0 | 19-1-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 159-3-3 |
| **All** | **23-0-2** | **89-2-9** | **95-2-3** | **96-0-4** | **97-2-1** | **84-1-15** | **94-0-6** | **95-1-4** | **97-1-2** | **770-9-46** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-0-4 | 19-0-1 | 20-0-0 | 20-0-0 | 16-0-4 | 19-0-1 | 20-0-0 | 20-0-0 | 155-0-10 |
| Linux / Clang | 4-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 17-0-3 | 19-0-1 | 20-0-0 | 20-0-0 | 159-1-5 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-0-1 | 15-0-5 | 18-0-2 | 20-0-0 | 20-0-0 | 148-0-17 |
| macOS / GCC | 5-0-0 | 19-0-1 | 19-0-1 | 17-0-3 | 17-1-2 | 20-0-0 | 20-0-0 | 18-2-0 | 18-1-1 | 153-4-8 |
| macOS / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 17-0-3 | 19-0-1 | 157-0-8 |
| **All** | **23-0-2** | **89-1-10** | **96-0-4** | **94-0-6** | **96-1-3** | **86-0-14** | **96-0-4** | **95-2-3** | **97-1-2** | **772-5-48** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (14 of them did not converge; see §9), Jsonifier won 1,542, tied 14 and lost 94: 770-9-46 with fresh output objects and 772-5-48 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 191 of 200 full-size reverse results (2 ties, 7 losses) and 190 of 200 small reverse results (no ties, 10 losses). No build won all 80: Linux/Clang went 79-0-1 and macOS/Clang 79-1-0. Every reverse loss and tie is Canada: at full size, Linux/GCC lost both forms in both runs (−5.5% to −10.9%), MSVC lost minified Canada in both runs (−5.0% fresh, −2.4% reused) and macOS/GCC lost it reused (−5.7%), with ties on MSVC and macOS/Clang (prettified, fresh); at small size, Linux/GCC lost Canada Small in both forms and both runs (−10.4% to −11.6%), MSVC lost it 3 times (−3.0% to −4.6%), macOS/GCC twice (fresh, −1.8% and −4.3%) and Linux/Clang once (minified, reused, −1.5%).
- **Windows/MSVC lost 21 of its 330 single-document results,** all Canada or Marine IK. 16 are in-order reads, in both forms and both runs at both sizes: Canada (−2.5% to −7.3%), Marine IK (−10.2% to −14.7%), Canada Small (−4.5% to −4.8%) and Marine IK Small (−4.1% to −6.7%). The other 5 are Canada read in reverse: minified Canada in both runs (−5.0% and −2.4%) and Canada Small (−3.0% to −4.6%). Its one tie is prettified Canada read in reverse (fresh). It won all 10 POD results, 74 of its 80 reverse results and all 160 of its sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.2–13.5% for Jsonifier and 6.4–10.4% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.4 percentage points on Linux/Clang, +1.6 on Linux/GCC, +3.2 on MSVC, +3.5 on macOS/Clang and +5.1 on macOS/GCC. On MSVC, Discord Reverse (min) goes from +192.8% to +226.5%, and Apache Builds (min) from +8.3% to +15.7%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +171.5% / +171.3% | +102.7% / +110.6% | +133.1% / +135.2% | +44.0% / +58.8% | +6.4% / +1.2% |
| Double | +97.1% / +116.3% | +89.0% / +90.8% | +100.2% / +111.6% | +94.9% / +104.6% | +64.5% / +75.2% |
| Int64 | +109.6% (n/c) / +120.1% | +93.6% / +97.0% | +88.4% / +89.5% | +60.1% / +60.6% | +39.9% / +41.4% |
| String | +2.8% / +6.9% | −5.6% / −9.7% | +3.9% / +3.9% | +5.6% / +6.1% | −19.6% / −33.6% |
| Uint64 | +109.2% / +109.9% | +106.7% / +117.9% | +88.7% / +93.2% | +58.5% / +51.0% | +23.9% / +40.5% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +8.3% / +15.7% | +20.8% / +31.0% | +22.4% / +29.2% | +41.2% / +44.2% | +37.8% / +46.0% |
| Apache Builds (pretty) | +9.5% / +15.7% | +21.9% / +29.8% | +18.5% / +29.9% | +37.0% / +56.5% | +47.6% / +52.8% |
| Canada (min) | −3.3% / −7.3% | +1.1% / +0.8% | −14.0% / −15.3% | +3.4% / +4.3% | +12.9% / +16.6% |
| Canada (pretty) | −3.8% / −2.5% | +0.7% / tie | −10.1% / −10.5% | +2.8% / +3.1% | tie (n/c) / −3.8% |
| CitmCatalog (min) | +13.9% / +14.4% | +12.4% / +14.7% | +11.6% / +12.0% | +12.3% / +12.5% | +11.3% / +12.1% |
| CitmCatalog (pretty) | +11.0% / +9.4% | +10.8% / +9.6% | +5.7% / +7.7% | +2.2% / +16.9% | +9.3% / +16.9% |
| Discord (min) | +14.1% / +19.9% | +12.6% / +23.2% | +13.3% / +13.5% | +29.4% / +170.6% | +21.0% / +32.2% |
| Discord (pretty) | +11.2% / +16.3% | +11.7% / +21.7% | +14.8% / +12.4% | +68.7% / +124.8% | +17.6% / +22.2% |
| Github Events (min) | +21.6% / +31.1% | +30.9% / +38.0% | +23.1% / +28.2% | +50.0% / +59.5% | +43.4% / +55.6% |
| Github Events (pretty) | +18.1% / +24.8% | +25.3% / +30.1% | +20.0% / +23.1% | +34.7% / +23.3% | +31.7% / +48.5% |
| Google Maps (min) | +17.5% / +21.2% | +24.6% / +26.6% | +22.5% / +24.2% | +18.2% / +30.6% | +16.8% / +17.0% |
| Google Maps (pretty) | +15.9% / +18.8% | +19.7% / +21.6% | +17.3% / +16.8% | +38.3% / +27.1% | +12.6% / +12.1% |
| Instruments (min) | +16.5% / +20.4% | +6.4% / +7.4% | +8.7% / +8.4% | +15.5% / +48.6% | +10.0% / +7.5% |
| Instruments (pretty) | +17.4% / +17.1% | +3.7% / +4.8% | +5.2% / +3.4% | +19.8% / +11.5% | +7.2% / +12.6% |
| Marine IK (min) | −14.7% / −14.1% | +3.9% / +4.8% | −7.0% / −7.2% | tie / +11.5% | +14.8% / +16.6% |
| Marine IK (pretty) | −10.2% / −13.3% | +2.0% / +1.5% | −6.7% / −7.5% | −7.9% / −16.1% | +4.8% (n/c) / +14.4% |
| Random (min) | +21.3% / +24.5% | +30.3% / +36.6% | +27.7% / +32.6% | +72.8% / +112.6% | +39.6% / +45.7% |
| Random (pretty) | +21.2% / +24.4% | +27.5% / +33.8% | +26.6% / +35.0% | +44.7% / +38.0% | +35.5% / +39.3% |
| Twitter (min) | +17.5% / +21.6% | +14.6% / +25.6% | +9.2% / +13.9% | +18.4% / +23.9% | +7.5% / +10.8% |
| Twitter (pretty) | +14.7% / +16.6% | +6.2% / +15.7% | +4.1% / +5.7% | +27.4% / +9.9% | +10.3% / +2.5% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +33.8% / +53.0% | +65.1% / +81.2% | +62.7% / +78.9% | +77.8% / +105.1% | +69.3% / +92.7% |
| Apache Builds (pretty) | +38.3% / +52.7% | +53.6% / +64.9% | +55.3% / +71.3% | +68.4% / +92.8% | +61.5% / +71.7% |
| Canada (min) | −5.0% / −2.4% | +3.9% / +5.0% | −10.5% / −10.9% | +5.8% / −5.7% | +13.0% / +12.6% |
| Canada (pretty) | tie / +8.9% | +4.9% / +4.2% | −5.5% / −5.7% | +4.8% / +22.3% | tie / +14.2% |
| CitmCatalog (min) | +54.0% / +57.8% | +69.0% / +74.4% | +61.0% / +66.1% | +67.7% / +39.1% | +82.6% / +81.3% |
| CitmCatalog (pretty) | +48.7% (n/c) / +52.3% | +61.2% / +60.4% | +49.0% / +52.9% | +53.8% / +54.5% | +70.2% / +79.2% |
| Discord (min) | +192.8% / +226.5% | +248.1% / +272.9% | +214.4% / +231.9% | +223.1% / +290.2% | +325.1% / +353.7% |
| Discord (pretty) | +168.6% / +194.8% | +217.2% / +245.2% | +188.2% / +203.5% | +218.2% / +271.1% | +295.7% / +294.7% |
| Github Events (min) | +106.8% / +126.3% | +125.0% / +142.8% | +93.7% / +106.2% | +96.1% / +117.1% | +162.7% / +193.0% |
| Github Events (pretty) | +86.4% / +106.4% | +99.9% / +117.7% | +76.9% / +90.6% | +62.9% / +85.8% | +134.2% / +137.7% |
| Google Maps (min) | +14.7% / +17.5% | +10.9% / +10.3% | +6.3% / +6.1% | +10.2% / +35.9% | +20.5% / +22.6% |
| Google Maps (pretty) | +16.4% / +17.5% | +6.7% / +6.5% | +5.6% / +8.6% | +23.8% / +41.6% | +15.7% / +14.8% |
| Instruments (min) | +175.4% / +188.2% | +178.7% / +177.8% | +141.9% / +148.7% | +164.2% / +116.8% | +241.7% / +227.9% |
| Instruments (pretty) | +164.5% / +175.1% | +143.9% / +142.0% | +116.9% / +117.6% | +100.9% / +111.9% | +228.7% / +232.1% |
| Marine IK (min) | +17.3% (n/c) / +20.6% | +51.0% / +54.0% | +39.1% / +40.9% | +68.7% / +65.7% | +58.7% / +59.5% |
| Marine IK (pretty) | +24.8% / +25.4% | +51.5% / +54.9% | +41.0% / +43.1% | +27.6% (n/c) / +29.8% (n/c) | +55.7% / +61.7% |
| Random (min) | +58.8% / +71.7% | +92.6% / +110.0% | +67.6% / +77.7% | +116.6% / +128.5% | +115.3% / +162.9% (n/c) |
| Random (pretty) | +59.9% / +68.8% | +74.6% / +86.2% | +52.9% / +60.5% | +38.1% / +60.8% | +95.9% / +104.0% |
| Twitter (min) | +174.5% / +205.0% | +144.8% / +159.2% | +108.6% / +117.2% | +113.6% / +99.5% | +225.6% / +250.8% |
| Twitter (pretty) | +152.6% / +173.7% | +127.5% / +136.5% | +81.1% / +86.2% | +100.2% / +137.5% | +195.9% / +217.2% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.4% / +28.8% | +28.2% / +36.9% | +44.4% / +50.9% | +53.1% / +73.6% | +44.0% / +38.5% |
| Apache Builds (pretty) | +13.3% / +18.5% | +25.8% / +31.4% | +35.3% / +36.7% | +40.7% / +54.9% | +48.6% / +54.0% |
| Canada (min) | +39.5% / +39.1% | +12.3% / +11.3% | −4.3% / −4.8% | +2.4% / +16.3% | +48.6% / +47.4% |
| Canada (pretty) | +37.1% / +38.4% | +11.8% / +10.8% | +15.9% / +16.4% | +11.3% / +10.2% | +41.5% / +33.2% |
| CitmCatalog (min) | +8.9% / +10.5% | +4.5% / +5.4% | +8.7% / +8.0% | +4.8% / +7.0% | +4.6% / +9.6% |
| CitmCatalog (pretty) | +18.9% / +20.0% | +2.9% / +2.2% | +11.0% / +10.2% | +18.8% / +16.9% | +7.0% / +7.9% |
| Discord (min) | +13.6% / +20.2% | +13.9% / +14.1% | +10.8% / +14.2% | +25.0% / +30.1% | +24.1% / +22.7% |
| Discord (pretty) | +6.7% / +8.2% | +8.1% / +7.1% | +9.4% / +11.9% | +22.6% / +25.5% | +18.7% / +13.3% |
| Github Events (min) | +18.5% / +21.3% | +20.4% / +22.6% | +17.8% / +21.2% | +31.6% / +40.2% | +7.9% / +16.3% |
| Github Events (pretty) | +15.4% / +17.2% | +8.6% / +9.9% | +13.5% / +14.6% | +11.9% / +30.5% | +21.5% / +4.2% |
| Google Maps (min) | +19.9% / +22.1% | +18.0% / +16.7% | +13.0% / +14.1% | +13.9% (n/c) / +2.5% | +17.9% / +7.9% |
| Google Maps (pretty) | +21.3% / +21.5% | +8.7% / +8.7% | +6.5% / +8.4% | +34.6% / +34.8% | +7.1% / +14.3% |
| Instruments (min) | +18.2% / +20.4% | +18.3% / +17.4% | +8.3% / +10.8% | +34.1% / +5.9% | +20.6% / +16.2% |
| Instruments (pretty) | +24.9% / +26.1% | +8.7% / +10.8% | +4.1% / +4.8% | +21.0% / −0.9% | +3.3% / +3.5% |
| Marine IK (min) | +17.2% / +17.4% | +8.6% / +7.2% | −7.0% / −7.8% | −2.7% / −6.8% | +20.6% / +17.8% |
| Marine IK (pretty) | +25.4% / +24.6% | +7.3% / +5.1% | −7.9% / −8.2% | +7.5% / +7.8% | +5.4% / +4.7% |
| Random (min) | +28.6% / +31.5% | +22.9% / +24.1% | +20.7% / +27.0% | +45.7% / +50.8% | +54.3% / +78.3% |
| Random (pretty) | +24.7% / +27.7% | +16.2% / +17.9% | +12.0% / +17.9% | +36.3% / +40.5% | +34.1% / +25.7% |
| Twitter (min) | +20.8% / +23.1% | +17.3% / +17.7% | +12.4% / +12.9% | +12.1% / −3.7% | +26.9% / +25.0% |
| Twitter (pretty) | +19.8% / +22.2% | +10.4% / +11.2% | +9.4% / +10.3% | +18.9% / +28.1% | +12.7% / +13.7% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +10.8% / +16.3% | +15.7% / +21.8% | +20.2% / +25.2% | +43.2% / +55.1% | tie / +31.7% |
| Apache Builds (pretty) | +10.9% / +14.8% | +10.9% / +11.9% | +12.8% / +13.6% | +35.1% / +43.9% | +18.2% / +13.7% |
| Canada (min) | +26.7% / +24.7% | +17.4% / +20.4% | −2.0% / −2.6% | +26.4% / +31.9% | +48.0% / +45.1% |
| Canada (pretty) | +29.9% / +31.9% | +8.8% / +9.1% | +12.1% / +13.4% | tie / −5.5% | +28.6% / +21.6% |
| CitmCatalog (min) | +9.5% / +11.3% | +3.7% / +4.9% | +1.5% / +2.2% | +5.4% / +16.4% | +24.3% / +10.6% |
| CitmCatalog (pretty) | +16.0% / +15.8% | +1.5% / +1.8% | +2.0% / +0.8% | +9.9% / tie | +7.9% / +9.2% |
| Discord (min) | +40.5% / +51.7% | +37.9% / +43.1% | +41.2% / +49.2% | +43.7% / +48.4% | +70.2% / +66.8% |
| Discord (pretty) | +39.9% / +51.8% | +25.5% / +31.0% | +31.7% / +40.5% | +22.3% / +26.2% | +53.2% / +79.0% |
| Github Events (min) | +38.1% / +45.8% | +35.2% / +38.8% | +30.8% / +33.7% | +42.0% / +32.5% | +35.6% / +46.4% |
| Github Events (pretty) | +33.2% / +38.2% | +28.9% / +33.6% | +25.8% / +27.4% | +20.6% / +44.9% | +35.1% / +36.2% |
| Google Maps (min) | +18.0% / +22.9% | +5.1% / +5.3% | +1.0% / +1.8% | +7.6% / +20.4% | +22.0% / +12.4% |
| Google Maps (pretty) | +15.8% / +21.6% | +3.1% / +3.9% | +1.5% / +0.9% | +4.9% / −10.5% | +12.2% / +11.0% |
| Instruments (min) | +28.8% / +32.8% | +33.3% / +34.7% | +33.6% / +33.5% | +22.1% / +22.2% | +39.3% / +70.5% |
| Instruments (pretty) | +40.1% / +44.1% | +25.0% / +26.0% | +25.1% / +25.2% | +35.9% / +34.6% | +18.4% / +17.0% |
| Marine IK (min) | +35.5% / +33.5% | +46.0% / +45.1% | +32.0% / +33.5% | +22.0% / +19.6% | +30.2% / +54.2% |
| Marine IK (pretty) | +43.2% / +43.3% | +23.7% / +23.8% | +10.5% / +13.0% | +23.2% / +23.2% | +32.3% / +32.2% |
| Random (min) | +28.7% / +32.4% | +31.7% / +35.2% | +25.2% / +30.7% | +49.9% (n/c) / +64.2% | +53.2% / +53.6% |
| Random (pretty) | +28.2% / +29.7% | +24.2% / +23.6% | +17.3% / +20.1% | +36.8% / +33.7% | +43.6% / +42.1% |
| Twitter (min) | +36.1% / +37.2% | +25.1% / +24.9% | +19.8% / +19.8% | +28.0% / +29.2% | +39.2% / +31.1% |
| Twitter (pretty) | +21.5% / +24.6% | +14.5% / +16.0% | +16.4% / +16.2% | +13.2% / +23.5% | +26.2% / +22.8% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +9.5% / +21.0% | +18.2% / +25.1% | +25.2% / +30.5% | +32.1% / +68.6% | +20.7% / +26.0% |
| Apache Builds (pretty) | +9.5% / +19.4% | +15.9% / +21.3% | +19.0% / +25.8% | +21.2% / +26.4% | +12.9% / +20.9% |
| Canada (min) | −4.7% / −4.8% | −4.1% / −4.2% | −16.1% / −17.5% | +8.9% / +16.6% | +4.4% / −0.6% |
| Canada (pretty) | −4.5% / −4.5% | −2.6% / −3.9% | −12.7% / −15.1% | −4.6% (n/c) / +10.5% | −1.1% / +7.2% |
| CitmCatalog (min) | +8.5% / +8.1% | +0.6% / +6.6% | −1.8% / +1.0% | +19.6% / +11.7% | +5.1% / +3.1% |
| CitmCatalog (pretty) | +7.0% / +4.0% | +3.0% / +1.9% | +0.6% / +1.3% | +10.5% / +13.0% | +1.8% / +7.9% |
| Discord (min) | +4.6% / +23.1% | +5.9% / +12.9% | +6.8% / +12.8% | +26.3% / +105.3% | +13.9% / +21.1% |
| Discord (pretty) | +3.4% / +19.6% | +5.4% / +12.2% | +2.1% / +11.7% | +11.5% / +87.5% | +4.8% / +17.5% |
| Github Events (min) | +13.3% / +11.7% | +8.5% / +9.6% | +3.8% / +0.8% | +17.6% / +27.3% | +28.3% / +26.2% |
| Github Events (pretty) | +7.1% / +8.9% | +4.0% / +4.5% | +0.8% / +1.3% | +9.9% / +16.9% | +6.7% / +15.7% |
| Google Maps (min) | +14.6% / +16.9% | +23.0% / +24.8% | +18.6% / +17.6% | +45.6% / +14.8% | +14.8% / +23.2% |
| Google Maps (pretty) | +16.1% / +15.9% | +15.0% / +15.9% | +13.0% / +14.4% | +31.7% / +22.3% | +14.0% / +17.9% |
| Instruments (min) | +20.5% / +20.9% | +0.2% / +1.9% | +0.8% / +2.6% | +16.5% / +9.6% | +8.2% / +14.6% |
| Instruments (pretty) | +7.5% / +12.8% | −0.7% / −1.1% | −2.1% / −3.6% | +17.9% / +23.4% | +2.3% / −4.7% |
| Marine IK (min) | −6.6% / −6.7% | +1.2% / +3.3% | −4.3% / −4.4% | +4.0% (n/c) / +14.0% | +16.9% / +4.7% |
| Marine IK (pretty) | −4.1% / −4.9% | +3.0% / +0.4% | −2.7% / −2.1% | tie / +13.1% | +5.9% / +11.9% |
| Random (min) | +13.0% / +18.5% | +20.8% / +26.8% | +21.9% / +30.5% | +60.9% / +56.9% | +31.7% / +37.3% |
| Random (pretty) | +12.4% / +16.2% | +18.9% / +22.7% | +20.6% / +27.3% | +31.9% / +48.3% | +31.3% / +28.6% |
| Twitter (min) | +10.9% / +16.4% | +8.2% / +12.6% | +4.2% / +7.1% | +24.3% / +34.4% | +17.1% / +27.9% |
| Twitter (pretty) | +10.4% / +19.2% | +4.8% / +9.2% | +5.8% / +7.3% | +20.3% / +28.3% | +3.9% / +8.8% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +52.1% / +82.8% | +76.5% / +87.6% | +75.6% / +86.6% | +95.3% / +108.4% | +84.7% / +110.6% |
| Apache Builds (pretty) | +51.8% / +80.4% | +60.6% / +72.2% | +63.3% / +72.4% | +70.4% / +87.2% | +67.0% / +95.2% |
| Canada (min) | −4.6% / +5.5% | +0.1% / −1.5% | −10.4% / −11.2% | −4.3% / +1.5% | +4.1% / +9.2% |
| Canada (pretty) | −3.0% / −3.6% | +1.5% / +1.3% | −11.6% / −11.5% | −1.8% / +2.4% | +1.0% / +8.0% |
| CitmCatalog (min) | +53.0% / +56.6% | +67.1% / +72.7% | +49.8% / +57.7% | +60.8% / +60.1% | +76.7% / +88.8% |
| CitmCatalog (pretty) | +45.9% / +55.7% | +51.4% / +56.8% | +44.6% / +48.3% | +53.0% / +50.5% | +85.4% / +83.0% |
| Discord (min) | +179.0% / +235.4% | +236.3% / +271.0% | +189.3% / +215.2% | +222.2% / +280.0% | +330.2% / +384.7% |
| Discord (pretty) | +161.5% / +223.6% | +208.3% / +241.7% | +171.7% / +191.7% | +190.7% / +229.3% | +288.3% / +350.0% |
| Github Events (min) | +33.8% / +35.0% | +35.8% / +44.7% | +20.8% / +23.6% | +33.7% / +62.1% | +62.0% / +79.6% |
| Github Events (pretty) | +27.4% / +30.3% | +26.0% / +27.9% | +13.0% / +19.0% | +23.2% / +33.3% | +44.4% / +54.3% |
| Google Maps (min) | +14.3% / +17.7% | +9.1% / +9.5% | +3.9% / +3.5% | +38.7% / +40.1% | +19.1% / +22.6% |
| Google Maps (pretty) | +13.6% / +14.3% | +1.0% / +2.7% | +1.9% / +3.2% | +22.4% / +15.5% | +14.7% / +10.4% |
| Instruments (min) | +138.5% / +155.7% | +169.2% / +168.2% | +126.8% / +132.0% | +130.3% / +148.7% | +228.0% / +238.9% |
| Instruments (pretty) | +144.3% / +166.4% | +132.7% / +133.5% | +106.0% / +110.1% | +123.1% / +85.7% | +200.6% / +208.0% |
| Marine IK (min) | +21.2% / +24.3% | +36.0% / +36.9% | +31.3% / +35.7% | +48.0% / +40.3% | +54.0% / +60.0% |
| Marine IK (pretty) | +17.7% / +20.9% | +33.2% / +37.5% | +31.5% / +35.5% | +41.0% / +51.5% | +45.4% / +51.2% |
| Random (min) | +49.9% / +64.3% | +87.3% / +97.3% | +60.6% / +67.6% | +85.9% / +142.2% | +124.3% / +134.7% |
| Random (pretty) | +49.7% / +59.2% | +70.3% / +80.0% | +49.7% / +53.9% | +92.3% / +84.0% | +93.9% / +112.2% |
| Twitter (min) | +171.9% / +176.4% | +134.8% / +139.4% | +101.1% / +104.7% | +122.7% / +131.3% | +219.1% / +225.5% |
| Twitter (pretty) | +134.4% / +155.0% | +125.2% / +131.3% | +82.3% / +88.1% | +168.1% / +81.4% | +133.6% / +172.6% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +18.6% / +22.8% | +30.6% / +37.2% | +36.2% / +42.3% | +45.0% / +68.7% | +25.7% / +31.1% |
| Apache Builds (pretty) | +12.1% / +19.2% | +21.0% / +22.2% | +25.5% / +31.0% | +28.9% / +37.4% | +14.8% / +20.9% |
| Canada (min) | +17.7% / +16.7% | +14.7% / +14.8% | +10.3% / +10.7% | +27.9% / +21.4% | +8.9% / +5.3% |
| Canada (pretty) | +26.7% / +26.5% | +5.8% / +5.8% | +13.4% / +15.2% | +5.9% / +9.1% | +2.2% / −9.4% |
| CitmCatalog (min) | +5.3% / +16.3% | +6.4% / +5.8% | +7.3% / +8.3% | +8.3% / +34.1% | +13.2% / +19.1% |
| CitmCatalog (pretty) | +6.9% / +9.8% | +7.3% / +4.9% | +7.4% / +8.4% | +12.2% / +15.6% | +12.0% / +8.8% |
| Discord (min) | +14.2% / +15.5% | +11.5% / +16.4% | +4.6% / +12.6% | +23.4% / +42.7% (n/c) | +21.1% / +25.5% |
| Discord (pretty) | +4.5% / +4.5% | +6.1% / +7.3% | +9.2% / +9.6% | +23.1% / +12.8% | +10.4% / +9.2% |
| Github Events (min) | +13.2% / +16.4% | +17.4% / +16.6% | +11.2% / +13.7% | +30.0% / +16.8% | +13.2% / +11.8% |
| Github Events (pretty) | +10.8% / +14.5% | +6.4% / +7.4% | +10.1% / +12.5% | +11.7% / +35.9% | +3.1% / +6.7% |
| Google Maps (min) | +19.2% / +21.6% | +14.5% / +17.7% | +15.2% / +12.2% | +17.2% / +11.0% | +27.8% / +15.4% |
| Google Maps (pretty) | +14.9% / +21.6% | +5.6% / +3.9% | +3.1% / +4.7% | +17.1% / +22.0% | +7.7% / +11.7% |
| Instruments (min) | +7.7% / +10.1% | +12.4% / +13.1% | +2.2% / +3.7% | −10.7% / tie | +14.7% / +15.0% |
| Instruments (pretty) | +10.7% / +11.9% | +2.3% / +3.5% | −3.8% / +0.2% | −2.7% / tie | +13.5% / −15.0% |
| Marine IK (min) | +15.4% / +22.5% | +6.3% / +6.6% | +3.1% / +3.7% | tie / +2.6% | +8.8% / +12.4% |
| Marine IK (pretty) | +26.1% / +24.6% | +4.1% / +4.5% | +10.0% / +10.9% | +17.5% / +6.3% | −8.4% / −8.6% |
| Random (min) | +9.4% / +19.9% | +18.0% / +21.2% | +20.4% / +19.7% | +40.7% / +47.9% | +26.6% / +25.4% |
| Random (pretty) | +9.3% / +10.0% | +8.5% / +9.4% | +6.7% / +9.9% | +19.7% / +28.0% | +15.2% / +20.0% |
| Twitter (min) | +2.9% / +2.8% | +5.9% / +8.5% | +1.0% / +1.7% | +10.7% / +10.4% | +11.1% / +8.9% |
| Twitter (pretty) | +4.2% / +8.2% | +0.8% / +1.2% | +5.6% / +3.7% | +3.6% / +7.1% | +1.1% / +5.9% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.7% / +16.3% | +11.6% / +17.6% | +14.9% / +19.4% | +43.3% / +39.5% | +21.3% / +24.2% |
| Apache Builds (pretty) | +14.9% / +10.2% | +8.1% / +7.7% | +10.3% / +11.2% | +16.1% / +43.4% | +3.5% / +4.1% |
| Canada (min) | +19.2% / +20.4% | +19.7% / +21.4% | +25.5% / +24.0% | +18.9% / +14.0% | +7.2% / +3.8% |
| Canada (pretty) | +17.1% / +18.7% | +20.7% / +22.3% | +28.0% / +28.1% | +8.5% / +11.5% | +3.8% / −1.7% |
| CitmCatalog (min) | +7.4% / +5.4% | +2.1% / +0.9% | +1.6% / +0.5% | +14.7% / +9.2% | +11.9% / +14.3% |
| CitmCatalog (pretty) | +1.7% / +6.6% | +3.9% / +0.8% | −5.1% / +0.4% | +7.1% / +3.1% | +20.7% / +8.9% |
| Discord (min) | +35.3% / +43.9% | +38.4% / +44.0% | +28.5% / +35.7% | +26.0% / +52.9% | +68.2% / +82.1% |
| Discord (pretty) | +28.4% / +39.1% | +24.1% / +31.1% | +24.3% / +29.0% | +30.4% / +37.6% | +34.9% / +53.9% |
| Github Events (min) | +30.8% (n/c) / +29.9% | +21.6% / +21.8% | +17.9% / +17.5% | +11.3% / +31.6% | +34.0% / +42.0% |
| Github Events (pretty) | +20.4% / +27.8% | +15.2% / +21.3% | +16.0% / +16.8% | +13.6% / +13.2% | +30.1% / +20.6% |
| Google Maps (min) | +13.4% / +15.1% | +2.6% / +1.6% | +1.0% / +0.3% | −7.5% / tie | +15.0% / +19.4% |
| Google Maps (pretty) | +15.4% / +21.7% | +2.4% / +0.3% | +0.3% / +0.3% | +21.3% / −1.7% | +12.2% / +13.6% |
| Instruments (min) | +16.9% / +29.1% | +33.0% / +36.4% | +23.7% / +25.7% | tie / +30.5% | +42.5% / +42.7% |
| Instruments (pretty) | +24.9% / +33.1% | +23.4% / +25.5% | +21.5% / +23.1% | +18.0% / +22.0% | +24.3% / +24.1% |
| Marine IK (min) | +17.3% / +17.4% | +36.3% / +35.7% | +33.6% / +32.8% | +31.3% / +33.8% | +37.7% / +35.9% |
| Marine IK (pretty) | +22.4% / +24.2% | +30.0% / +28.1% | +41.7% / +42.0% | +72.8% / +26.6% | +22.1% / +23.2% |
| Random (min) | +18.8% / +22.1% | +26.3% / +27.5% | +25.9% / +23.3% | +14.9% / +35.9% | +46.5% / +51.6% |
| Random (pretty) | +15.9% / +16.8% | +16.2% / +16.1% | +15.5% / +16.9% | +29.1% / +23.9% | +34.2% / +37.5% |
| Twitter (min) | +23.7% / +20.0% | +12.4% / +10.4% | +9.9% / +10.6% | +17.6% / +14.1% | +20.8% / +20.6% |
| Twitter (pretty) | +11.1% / +11.2% | +6.4% / +3.9% | +9.0% / +6.8% | +9.5% / +9.4% | +11.2% / +11.8% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.8x to 4.5x faster at full size: +81.1% (Twitter pretty, Linux/GCC, fresh) up to +353.7% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +81.4% to +384.7%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −10.9% (minified Canada on Linux/GCC, reused) to +41.6%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** None of the 94 single-document losses or 14 ties is a Github Events read: it wins all 160 of its results. Read in reverse at full size it wins on every build (+62.9% to +193.0%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 193 of 200 full-size sparse-reverse results across both allocation modes (3 ties, 4 losses), and 194 of 200 small ones (2 ties, 4 losses). MSVC and Linux/Clang won all 80. The full-size losses are minified Canada on Linux/GCC (−2.0% fresh, −2.6% reused) and two prettified reads on macOS/GCC (reused): Canada (−5.5%) and Google Maps (−10.5%). The small losses are minified Google Maps Small on macOS/GCC (fresh, −7.5%), prettified Google Maps Small on macOS/GCC (reused, −1.7%), prettified CitmCatalog Small on Linux/GCC (fresh, −5.1%) and prettified Canada Small on macOS/Clang (reused, −1.7%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 243.9 MB/s on MSVC against 291.9 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 655.1 and 926.0 MB/s. Every MSVC full-size sparse result is a win, by +6.7% to +39.5%, and every full-size sparse-reverse result by +9.5% to +51.8%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 190 of 200 full-size sparse results (no ties, 10 losses) and 190 of 200 small ones (3 ties, 7 losses). MSVC won all 40 of its full-size results and all 40 small, and so did Linux/Clang. The full-size losses are on Linux/GCC (6: minified Canada, −4.3% and −4.8%, and Marine IK, −7.0% to −8.2%) and macOS/GCC (4: minified Marine IK, −2.7% fresh and −6.8% reused, minified Twitter, reused, −3.7%, and prettified Instruments, reused, −0.9%). macOS/Clang holds 4 of the 7 small losses, all prettified (Canada Small, Instruments Small and Marine IK Small, −8.4% to −15.0%), macOS/GCC 2 (Instruments Small, fresh, −10.7% minified and −2.7% prettified) and Linux/GCC 1 (prettified Instruments Small, fresh, −3.8%). The largest is prettified Instruments Small on macOS/Clang (reused, −15.0%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 178, tied 3 and lost 19. Linux/Clang went 39-1-0, macOS/Clang 38-1-1, macOS/GCC 37-1-2, MSVC 32-0-8 and Linux/GCC 32-0-8 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 170, tied 1 and lost 29. macOS/GCC went 38-1-1, macOS/Clang 37-0-3, Linux/Clang 34-0-6, MSVC 32-0-8 and Linux/GCC 29-0-11. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's are Canada Small (−2.6% to −4.2%) and prettified Instruments Small (−0.7% and −1.1%), macOS/Clang's run from −0.6% to −4.7%, MSVC's (Canada Small and Marine IK Small) from −4.1% to −6.7%, and Linux/GCC's from −1.8% to −17.5%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −17.5%).
- **In-order losses at full size fall on two documents.** Linux/GCC lost Canada in both forms and both runs (−10.1% to −15.3%) and Marine IK in both forms and both runs (−6.7% to −7.5%). MSVC lost Canada in both forms and both runs (−2.5% to −7.3%) and Marine IK in both forms and both runs (−10.2% to −14.7%). macOS/GCC lost prettified Marine IK in both runs (−7.9% and −16.1%), and macOS/Clang prettified Canada (reused, −3.8%). Linux/Clang lost none. Canada and Marine IK hold all 19; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 46-0-4 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+1.2% to +171.5%, +64.5% to +116.3%, +39.9% to +120.1% and +23.9% to +117.9%). String is the weakest: 6 wins (+2.8% to +6.9%) and 4 losses, on Linux/Clang (−5.6% and −9.7%) and macOS/Clang (−19.6% and −33.6%) in both runs.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 4.4% to 12.4% below its own NDJSON throughput on the same records in 17 of the 18 build and stream pairs; the exception is Apache Builds on MSVC, where comma runs 0.6% above.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 22 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 23 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 27 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 5-1-0 | 12-0-0 | 12-0-0 | 29-1-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| macOS / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| **All** | **26-1-3** | **60-0-0** | **60-0-0** | **146-1-3** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +13.5% | +18.0% | +28.1% | +71.8% | +21.1% |
| Large Amazon Cellphones (NDJSON) | +4.4% | +14.0% | +9.6% | +4.1% (n/c) | +34.2% (n/c) |
| Stream Formats Small (NDJSON) | +38.6% | +15.8% | +15.6% | −8.1% (n/c) | +7.4% (n/c) |
| Stream Formats Small (comma) | +78.4% (n/c) | +83.5% | +76.8% | +69.0% | +60.3% |
| Stream Formats Large (NDJSON) | +13.3% | tie | +22.3% | +10.4% | −7.6% |
| Stream Formats Large (comma) | +20.5% | +6.9% | +33.9% | +15.5% | −6.3% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +14.3% | +34.3% | +23.5% | +48.1% | +27.6% |
| Apache Builds (comma) | +28.7% | +42.6% | +41.6% | +65.3% | +44.6% |
| CitmCatalog (NDJSON) | +11.4% (n/c) | +14.7% | +11.3% | +9.2% | +12.8% |
| CitmCatalog (comma) | +30.5% | +38.6% | +34.2% | +29.0% | +25.9% |
| Google Maps (NDJSON) | +20.8% (n/c) | +31.3% | +22.0% | +42.7% | +15.3% |
| Google Maps (comma) | +24.4% | +46.9% | +42.3% | +25.9% (n/c) | +26.2% |
| Instruments (NDJSON) | +33.8% | +20.4% | +17.1% | +32.7% | +14.3% |
| Instruments (comma) | +38.7% | +42.4% | +55.3% | +55.4% | +29.8% |
| Random (NDJSON) | +22.4% | +36.3% | +22.0% | +45.6% | +57.7% |
| Random (comma) | +31.1% | +43.3% | +38.3% | +91.7% (n/c) | +63.1% |
| Twitter (NDJSON) | +19.6% (n/c) | +16.2% | +2.0% | +5.0% | +19.3% |
| Twitter (comma) | +17.3% | +25.5% | +22.1% | +28.0% | +29.5% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +27.6% | +22.7% | +13.6% | +36.6% | +26.7% |
| Apache Builds (comma) | +34.7% | +34.8% | +32.9% | +55.4% (n/c) | +38.8% |
| CitmCatalog (NDJSON) | +34.4% (n/c) | +36.8% | +24.6% | +35.2% | +42.6% |
| CitmCatalog (comma) | +45.7% | +57.8% | +44.7% | +67.2% (n/c) | +59.3% |
| Google Maps (NDJSON) | +26.0% (n/c) | +11.8% | +5.3% | +13.4% | +8.9% |
| Google Maps (comma) | +20.1% | +20.5% | +16.9% | +27.1% (n/c) | +16.6% |
| Instruments (NDJSON) | +55.3% | +32.3% | +18.5% | +29.0% | +43.8% |
| Instruments (comma) | +72.3% | +53.5% | +47.5% | +49.8% | +59.1% |
| Random (NDJSON) | +62.0% (n/c) | +75.2% | +50.1% | +93.5% | +104.1% |
| Random (comma) | +69.0% | +86.1% | +67.8% | +110.8% | +128.9% |
| Twitter (NDJSON) | +202.5% | +163.9% | +112.9% | +90.5% | +250.3% |
| Twitter (comma) | +189.7% (n/c) | +174.1% | +123.3% | +164.6% | +229.3% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 26 of the 30 results in that group, tied 1 and lost 3. It won Stream Formats Small (comma) (+60.3% to +83.5%), Amazon Cellphones (+13.5% to +71.8%) and Large Amazon Cellphones (+4.1% to +34.2%) on all five builds. Stream Formats Small (NDJSON) won on four builds (+7.4% to +38.6%) and lost on macOS/GCC (−8.1%, n/c). Stream Formats Large won in both forms on MSVC, Linux/GCC and macOS/GCC, won comma and tied NDJSON on Linux/Clang, and lost both on macOS/Clang (NDJSON −7.6%, comma −6.3%).
- **Two builds won every streaming test,** 30-0-0 each: Windows/MSVC and Linux/GCC. Linux/Clang went 29-1-0 (its tie is Stream Formats Large as NDJSON), macOS/GCC 29-0-1 and macOS/Clang 28-0-2.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +90.5% to +250.3%, while Google Maps rows, which have few, give +5.3% to +27.1%. Instruments wins on every build (+18.5% to +72.3%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 340.2 MB/s under MSVC and 461.1 MB/s under Linux/Clang, while Jsonifier reads it at 574.9 and 858.0 MB/s.

## 9. Limitations

- **Linux/GCC holds the most single-document losses:** 37 of the 94, against 21 on MSVC, 16 on macOS/GCC, 11 on macOS/Clang and 9 on Linux/Clang. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (−33.6% reused, −19.6% fresh), then Linux/GCC's minified Canada Small read in order (reused, −17.5%), the largest x86 loss.
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 74 of the 94: Canada 48 (20 on Linux/GCC, 13 on MSVC, 5 each on Linux/Clang, macOS/GCC and macOS/Clang) and Marine IK 26 (12 on Linux/GCC, 8 on MSVC, 4 on macOS/GCC, 2 on macOS/Clang). There much of the cost is float parsing, not field lookup. The other 20 are Instruments 10 (3 each on Linux/GCC and macOS/GCC, 2 each on Linux/Clang and macOS/Clang), the 4 String POD losses, Google Maps 3 (all macOS/GCC), CitmCatalog 2 (both Linux/GCC) and Twitter 1 (macOS/GCC).
- **Linux/GCC loses mostly on Canada.** 20 of its 37 losses are Canada, at both sizes and under every access pattern, where much of the cost is number parsing rather than field lookup.
- **Small documents carry more than half the losses.** 50 of the 94 losses are small documents (17 on Linux/GCC, 11 on MSVC, 8 on macOS/Clang, 7 each on Linux/Clang and macOS/GCC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **macOS holds all three streaming losses:** macOS/Clang's Stream Formats Large as NDJSON (−7.6%) and comma-separated (−6.3%), and macOS/GCC's Stream Formats Small as NDJSON (−8.1%, n/c).
- **Some results did not converge.** 14 of the 1,650 single-document results did not converge: 7 on macOS/GCC, 4 on Windows/MSVC and 3 on macOS/Clang. 18 of the 150 streaming results did not converge: 8 on Windows/MSVC, 7 on macOS/GCC and 3 on macOS/Clang. Linux/Clang and Linux/GCC converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,542, tied 14 and lost 94. On reverse reads it won 381 of 400 at both sizes, and on Windows/MSVC it lost 21 of its 330 single-document results, all Canada or Marine IK. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (770-9-46 fresh, 772-5-48 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 74 of the 94 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 146, tied 1 and lost 3, and it won all 120 corpus record-stream results. The Stream Formats reads on macOS, which hold all three losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.