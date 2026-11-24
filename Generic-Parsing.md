# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader that writes a lone scalar or string straight to the tape and classifies a short final remainder with narrower registers;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,576 of 1,650 single-document results against simdjson On Demand, tied 18 and lost 56. On reverse-order reads it won 389 of 400. On Windows/MSVC it won 324 of 330 single-document results, tied 4 and lost 2.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 146 of 150 results, tied 2 and lost 2, and it won all 120 corpus record-stream results.

The whole suite was also run on three AVX-512 builds on an AMD EPYC 9V45, against simdjson's own AVX-512 kernel (§9). There Jsonifier won 918 of 990 single-document results, tied 10 and lost 62, won 237 of 240 reverse-order results, and won 79 of 90 streaming results.

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

Two short paths keep small inputs from paying for the full-width machinery. A document that is a single scalar or string with only whitespace around it is recognized by a byte-class scan (`tryResetLoneValue`) and gets a one-entry tape with no vector work. Every other input runs the full reader, which scans in batches of up to `simdBytesPerStep` bytes, halves the batch as the input runs out, and classifies a final remainder of at most 32 bytes with 16- or 32-byte registers instead of a padded 64-byte block (*Batched Drain, Fused Scan* [3], §5 and §8.1). The 5 KiB *Small* documents of §6 therefore run the same reader as the full-size ones, with only their last partial block narrowed.

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
- **Correctness:** on the measured commits, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, read directly from upstream simdjson's internal stage-1 parser after `stage1()`, with no fork or patch. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `4f52e70` and simdjson `3448984`, the benchmarked commits, on November 23, 2026. The tape checks ran on cloud-hosted CI runners, not on the benchmark hosts: their Linux builds report Azure kernels (`7.0.0-1020-azure`), the harness does not record their CPU model, and of their compilers only Linux/GCC differs from the benchmark builds' (GCC 16.0.1, against 16.2.1 below); MSVC 19.52, Clang 24.0 and the macOS GCC 16.2.0 and Clang 23.1.5 are the same. The AVX-512 builds could not run on the Core i9-14900KF, which has no AVX-512; their benchmarks ran on separate cloud hosts, described in §9. The benchmark runs of §7 and §8 did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-43-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 25 of the 1,650 single-document results and 20 of the 150 streaming results did not converge (see §10). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. On the same CPU, Jsonifier reads Int64 (fresh) at 577.4 MB/s under MSVC against 640.1 MB/s under Linux/Clang, and Uint64 at 522.9 against 646.9 MB/s; simdjson reads Int64 at 321.9 against 373.6 MB/s and Uint64 at 290.9 against 205.8 MB/s.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 164 / 170 | 158 / 160 | 322 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 160 / 170 | 158 / 160 | 318 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.5 | NEON | 166 / 170 | 159 / 160 | 325 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `71afc62`, Jsonifier `4f52e70` and simdjson `3448984`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The five builds ran on November 23 and 24, 2026, and the results were published to `generic-parsing-main`.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 163-1-1 |
| Linux / Clang | 4-0-1 | 19-1-0 | 20-0-0 | 19-0-1 | 20-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 160-1-4 |
| Linux / GCC | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 18-0-2 | 20-0-0 | 20-0-0 | 159-0-6 |
| macOS / GCC | 5-0-0 | 16-2-2 | 18-0-2 | 18-0-2 | 20-0-0 | 19-0-1 | 19-0-1 | 18-1-1 | 20-0-0 | 153-3-9 |
| macOS / Clang | 4-0-1 | 16-1-3 | 20-0-0 | 18-0-2 | 18-1-1 | 19-0-1 | 20-0-0 | 19-0-1 | 18-1-1 | 152-3-10 |
| **All** | **22-0-3** | **88-5-7** | **98-0-2** | **95-0-5** | **98-1-1** | **94-0-6** | **97-0-3** | **97-1-2** | **98-1-1** | **787-8-30** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 161-3-1 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 18-1-1 | 19-1-0 | 17-0-3 | 20-0-0 | 20-0-0 | 20-0-0 | 158-2-5 |
| Linux / GCC | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 18-0-2 | 20-0-0 | 20-0-0 | 159-0-6 |
| macOS / GCC | 5-0-0 | 18-0-2 | 20-0-0 | 18-1-1 | 20-0-0 | 18-0-2 | 19-0-1 | 20-0-0 | 20-0-0 | 158-1-6 |
| macOS / Clang | 3-0-2 | 18-1-1 | 18-2-0 | 18-0-2 | 18-1-1 | 19-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 153-4-8 |
| **All** | **21-0-4** | **93-2-5** | **98-2-0** | **94-2-4** | **97-2-1** | **90-2-8** | **96-0-4** | **100-0-0** | **100-0-0** | **789-10-26** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (25 of them did not converge; see §10), Jsonifier won 1,576, tied 18 and lost 56: 787-8-30 with fresh output objects and 789-10-26 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 196 of 200 full-size reverse results (2 ties, 2 losses) and 193 of 200 small reverse results (no ties, 7 losses). MSVC and Linux/Clang went 80-0-0, macOS/Clang 77-2-1, and Linux/GCC and macOS/GCC 76-0-4. The full-size ties are Google Maps on macOS/Clang in both forms, reused, and the full-size losses are Canada on macOS/GCC, fresh (−7.4% minified, −11.7% prettified, n/c). Every small loss is Canada Small: Linux/GCC in both forms and both runs (−3.6% to −5.2%), minified on macOS/GCC (−1.8% fresh, −5.9% reused) and prettified and reused on macOS/Clang (−3.7%).
- **Windows/MSVC lost 2 of its 330 single-document results, both minified Marine IK read in order** (−8.2% fresh, −5.2% reused). Its 4 ties are minified Canada read in order (reused), prettified Marine IK read in order (fresh) and Canada Small read in order in both forms (reused). It won all 10 POD results, all 80 reverse results and all 160 sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.7–14.4% for Jsonifier and 5.9–11.1% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.6 percentage points on macOS/Clang, +1.9 on Linux/Clang, +2.4 on Linux/GCC, +2.7 on macOS/GCC and +3.4 on MSVC. On MSVC, Discord Reverse (min) goes from +216.2% to +254.5%, and Apache Builds (min) from +7.7% to +15.9%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +106.7% / +107.1% | +18.1% / +18.9% | +68.8% / +87.4% | +109.6% / +88.0% | +48.9% (n/c) / +23.2% |
| Double | +85.2% / +106.6% | +54.1% / +52.4% | +85.8% / +94.8% | +118.3% / +131.4% | +105.6% / −51.8% |
| Int64 | +79.4% / +100.7% | +71.3% / +81.4% | +121.6% / +148.2% | +140.2% / +168.1% | +74.1% / +94.6% |
| String | +3.7% / +7.2% | −30.8% / −34.8% | −6.8% / −5.5% | +5.9% / +36.1% | −3.5% / −4.5% |
| Uint64 | +79.8% / +94.1% | +214.4% / +88.7% | +138.8% / +155.3% | +126.9% / +153.8% | +124.8% / +107.4% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.7% / +15.9% | +16.2% / +23.6% | +28.0% / +41.4% | +37.8% / +44.9% | +47.6% / +27.2% |
| Apache Builds (pretty) | +6.7% / +12.4% | +14.6% / +18.1% | +26.0% / +37.4% | +52.5% / +58.5% | −1.1% / +9.2% |
| Canada (min) | +2.3% / tie | tie / +0.5% | −3.0% / −5.0% | −3.5% / +3.5% | +8.1% / +5.4% |
| Canada (pretty) | +3.8% / +2.0% (n/c) | +2.8% / +3.1% | +0.5% / +1.0% | +26.5% / +26.3% (n/c) | +3.7% / +2.6% |
| CitmCatalog (min) | +18.6% / +9.9% | +12.5% / +13.6% | +20.0% / +21.2% | +12.9% / +16.8% | +9.1% / +12.7% |
| CitmCatalog (pretty) | +13.2% / +13.2% | +8.6% / +9.6% | +15.4% / +17.8% | +15.4% / +39.4% | +10.6% / +2.5% |
| Discord (min) | +19.3% / +31.9% | +11.9% / +25.1% | +28.9% / +40.9% | +73.6% / +54.3% | +33.4% / +17.8% |
| Discord (pretty) | +12.4% / +21.4% | +16.6% / +22.6% | +14.6% / +25.4% | +21.3% / +27.1% | +17.3% / +19.3% |
| Github Events (min) | +23.1% / +25.2% | +32.7% / +39.1% | +28.8% / +36.1% | +56.0% / +55.4% | +47.9% / +45.0% |
| Github Events (pretty) | +15.7% / +22.9% | +25.3% / +30.6% | +23.9% / +28.6% | +48.1% / +50.3% | +20.5% / +50.9% |
| Google Maps (min) | +35.0% / +39.9% | +17.7% / +20.9% | +28.9% / +32.3% | +32.8% / +33.4% | +15.1% / +42.2% |
| Google Maps (pretty) | +28.7% / +44.5% | +10.9% / +11.3% | +22.8% / +25.1% | +30.6% / +33.9% | tie / +13.5% |
| Instruments (min) | +20.8% / +28.6% | +6.9% / +7.7% | +11.3% / +10.2% | tie / +9.2% | +16.1% / +9.7% |
| Instruments (pretty) | +16.1% / +15.8% | +1.5% / +2.2% | +4.7% / +4.0% | +7.6% / +11.4% | −8.6% / tie |
| Marine IK (min) | −8.2% / −5.2% | +4.3% / +4.7% | +3.4% / +5.0% | −6.3% (n/c) / −3.6% (n/c) | +3.5% / +18.3% |
| Marine IK (pretty) | tie / +5.9% | +2.5% / +0.6% | +1.5% / +1.8% | tie / −11.4% (n/c) | +23.8% / +7.0% |
| Random (min) | +15.9% (n/c) / +22.3% | +24.7% / +30.4% | +35.9% / +43.4% | +41.4% / +55.2% | +21.6% / +29.3% |
| Random (pretty) | +23.9% / +22.6% | +23.4% / +30.8% | +33.4% / +40.8% | +53.1% / +47.3% | +33.0% / +49.5% |
| Twitter (min) | +20.4% / +27.2% | +10.9% / +15.7% | +9.0% / +27.2% | +31.2% / +43.0% | −1.1% / −1.1% |
| Twitter (pretty) | +17.8% / +25.9% | +8.2% / +11.4% | +3.8% / +15.6% | +5.3% / +41.0% | +1.2% / +9.3% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +38.1% / +76.0% | +58.1% / +76.6% | +69.6% / +88.0% | +69.5% / +152.3% | +71.5% / +116.5% |
| Apache Builds (pretty) | +31.8% / +49.5% | +50.3% / +65.2% | +66.9% / +82.2% | +77.7% / +98.2% | +56.9% / +70.3% |
| Canada (min) | +4.8% (n/c) / +3.2% | +2.2% / +3.6% | +0.5% / +0.5% | −7.4% / +4.5% (n/c) | +9.4% / +11.1% |
| Canada (pretty) | +4.3% (n/c) / +4.3% | +6.1% / +6.0% | +3.8% / +4.3% | −11.7% (n/c) / +8.6% | +9.8% / +7.2% |
| CitmCatalog (min) | +46.8% / +49.8% | +68.3% / +72.9% | +73.2% / +78.5% | +77.1% / +81.8% | +81.5% / +103.9% |
| CitmCatalog (pretty) | +45.3% / +50.0% | +57.4% / +60.9% | +61.5% / +65.0% | +91.2% / +60.1% | +104.2% / +67.2% |
| Discord (min) | +216.2% / +254.5% | +257.1% / +279.9% | +258.7% / +284.0% | +363.3% / +291.5% | +443.1% / +373.3% |
| Discord (pretty) | +190.8% / +211.5% | +201.9% / +220.8% | +222.0% / +242.3% | +187.0% / +251.4% | +272.1% / +316.4% |
| Github Events (min) | +84.4% / +111.0% | +132.9% / +155.5% | +100.9% / +116.5% | +83.2% / +115.5% | +169.1% / +195.5% |
| Github Events (pretty) | +88.7% / +110.1% | +103.5% / +121.2% | +84.5% / +97.2% | +76.6% / +80.7% | +156.1% / +200.1% |
| Google Maps (min) | +6.9% / +6.8% | +9.5% / +10.1% | +16.3% / +16.2% | +31.1% / +32.9% | +23.3% / tie |
| Google Maps (pretty) | +10.3% / +8.7% | +3.6% / +4.5% | +9.0% / +9.1% | +30.8% / +31.8% | +9.6% / tie |
| Instruments (min) | +175.3% / +198.9% | +175.0% / +181.2% | +169.1% / +173.8% | +147.5% / +138.9% | +271.2% / +243.2% |
| Instruments (pretty) | +169.1% / +190.2% | +144.7% / +148.4% | +142.2% / +145.7% | +127.1% / +106.9% | +197.3% / +183.3% |
| Marine IK (min) | +35.3% / +30.7% (n/c) | +49.2% / +49.2% | +49.9% / +52.4% | +61.2% (n/c) / +55.3% | +32.6% / +34.1% |
| Marine IK (pretty) | +24.6% / +31.5% | +44.8% / +54.5% | +49.2% / +55.2% | +44.0% / +42.9% (n/c) | +44.2% (n/c) / +41.1% (n/c) |
| Random (min) | +72.3% / +77.5% | +90.8% / +103.5% | +91.4% / +106.0% | +89.1% / +101.7% | +138.7% / +122.8% |
| Random (pretty) | +55.4% / +71.8% | +76.7% / +87.1% | +77.2% / +89.2% | +92.9% / +98.6% | +110.0% / +139.2% |
| Twitter (min) | +162.2% / +195.0% | +155.6% / +167.4% | +126.8% / +143.2% | +123.8% / +143.8% | +228.3% / +247.5% |
| Twitter (pretty) | +144.8% / +154.5% | +124.7% / +134.6% | +101.6% / +110.2% | +61.3% / +61.8% | +245.1% / +238.7% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.7% / +16.7% | +26.4% / +37.6% | +55.8% / +75.9% | +54.5% / +56.7% | +21.2% / +38.5% |
| Apache Builds (pretty) | +17.8% / +21.3% | +19.5% / +29.3% | +39.2% / +53.6% | +44.9% / +43.0% | +18.9% / +18.1% |
| Canada (min) | +14.4% / +16.0% | −2.4% / −2.3% | +8.9% / +8.3% | +26.9% / +13.5% | +28.3% / +41.5% |
| Canada (pretty) | +33.1% / +32.5% | +6.0% / +5.8% | +17.4% / +17.6% | +29.7% / +30.0% | +19.8% / +19.3% |
| CitmCatalog (min) | +16.5% / +13.1% | +9.7% / +10.1% | +8.4% / +8.1% | −17.5% / −16.5% | +13.4% / +4.7% |
| CitmCatalog (pretty) | +21.4% / +27.7% | +7.1% / +6.6% | +11.4% / +12.4% | +26.2% / +20.9% | −9.6% / −10.8% |
| Discord (min) | +15.2% / +18.1% | +6.6% / +9.4% | +16.3% / +21.4% | +14.6% / +61.0% | +30.3% / +31.2% |
| Discord (pretty) | +15.0% / +24.3% | +7.3% / +8.4% | +19.2% / +21.2% | +27.5% / +36.0% | +3.8% / +13.4% |
| Github Events (min) | +18.4% / +29.3% | +21.7% / +22.8% | +30.2% / +32.1% | +44.1% / +43.3% | +31.9% / +38.1% |
| Github Events (pretty) | +20.9% (n/c) / +18.2% | +11.9% / +13.1% | +17.2% / +18.3% | +32.1% / +34.7% | +15.5% / +18.4% |
| Google Maps (min) | +23.0% / +22.5% | +16.4% / +15.3% | +25.5% / +26.3% | +28.2% / +19.1% (n/c) | +6.5% / +16.5% |
| Google Maps (pretty) | +18.0% / +19.6% | +7.8% / +8.4% | +13.0% / +13.3% | +26.4% / +18.0% | +14.7% / +14.2% |
| Instruments (min) | +19.4% / +30.6% | +22.0% / +24.2% | +18.9% / +19.9% | +5.9% / +17.0% | +9.0% / +19.0% |
| Instruments (pretty) | +20.3% / +15.4% | +11.0% / +11.0% | +15.6% / +16.5% | +25.7% / +7.6% | −7.2% / −5.6% |
| Marine IK (min) | +6.1% / +7.1% | +0.5% / tie | +4.0% / +5.3% | −10.1% / tie | +16.4% / +15.2% |
| Marine IK (pretty) | +16.3% / +17.7% | +1.0% / +0.4% | +4.9% / +9.1% | +12.1% / +6.6% | +10.6% / +8.0% |
| Random (min) | +28.1% / +29.9% | +26.8% / +31.9% | +33.0% / +39.3% | +35.3% / +65.2% | +43.4% / +37.5% |
| Random (pretty) | +26.3% / +38.2% | +18.3% / +21.7% | +19.7% / +24.9% | +42.6% / +40.4% | +18.2% / +37.1% |
| Twitter (min) | +66.2% / +68.2% | +15.8% / +17.2% | +20.6% / +22.2% | +41.3% / +19.3% | +43.9% / +44.0% |
| Twitter (pretty) | +51.1% / +59.6% | +10.8% / +10.6% | +14.1% / +13.6% | +28.2% / +42.9% | +32.6% / +29.6% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +13.1% / +28.7% | +20.4% / +26.6% | +27.2% / +33.5% | +52.3% / +45.2% | +15.8% / +25.2% |
| Apache Builds (pretty) | +14.8% / +12.8% | +7.4% / +11.0% | +22.1% / +25.2% | +19.5% / +37.3% | +29.8% / +18.4% |
| Canada (min) | +15.9% / +11.1% | +2.3% / +1.6% | +10.2% / +8.6% | +24.5% / +21.1% | +28.8% / +27.6% |
| Canada (pretty) | +27.2% / +23.8% | +4.6% / +5.0% | +13.8% / +13.0% | +26.4% / +37.4% | +14.7% / +17.9% |
| CitmCatalog (min) | +12.7% / +18.0% | +2.2% / +1.5% | +8.2% / +8.4% | +6.3% / +12.3% | +6.9% / +9.5% |
| CitmCatalog (pretty) | +22.9% / +23.8% | +1.5% / tie | +8.2% / +8.3% | +23.5% / +0.9% | −8.2% / tie |
| Discord (min) | +34.7% / +46.0% | +41.2% / +51.4% | +41.1% / +49.6% | +27.5% / +38.9% | +132.7% / +82.2% |
| Discord (pretty) | +30.0% / +34.3% | +39.0% / +46.9% | +40.1% / +49.4% | +39.4% / +44.9% | +111.8% / +41.6% |
| Github Events (min) | +42.8% / +61.2% | +37.1% / +41.4% | +49.6% / +50.1% | +52.0% / +73.9% | +55.3% / +61.5% |
| Github Events (pretty) | +43.9% / +43.4% | +31.3% / +33.3% | +36.4% / +39.9% | +52.6% / +44.6% | +62.8% / +44.2% (n/c) |
| Google Maps (min) | +18.4% / +19.8% | +8.5% / +7.1% | +12.5% / +12.5% | +10.3% / +5.8% | +31.6% / +24.7% |
| Google Maps (pretty) | +19.8% / +17.2% | +6.0% / +5.4% | +9.3% / +8.6% | +17.6% / +13.3% | +18.5% / −13.8% |
| Instruments (min) | +37.4% / +39.5% | +38.2% / +39.3% | +27.4% / +29.3% | +26.9% / +23.9% | +35.6% / +40.3% |
| Instruments (pretty) | +43.0% / +37.1% | +25.0% / +25.6% | +29.7% / +30.7% | +30.3% / +11.0% | +20.0% / +21.4% |
| Marine IK (min) | +39.9% / +39.0% | +25.0% / +25.9% | +31.6% / +31.2% | +13.0% / +39.6% | +40.2% / +50.5% |
| Marine IK (pretty) | +27.2% / +24.9% | +16.9% / +18.4% | +27.5% / +25.9% | +17.3% / +16.9% (n/c) | +28.4% / +17.7% |
| Random (min) | +39.6% / +35.9% | +34.0% / +38.6% | +39.8% / +43.9% | +60.7% / +66.0% | +49.6% / +38.3% |
| Random (pretty) | +12.4% / +23.5% | +28.2% / +31.2% | +35.0% / +37.6% | +31.8% / +47.3% | +38.5% / +39.8% |
| Twitter (min) | +54.4% / +56.3% | +32.9% / +32.9% | +31.6% / +32.2% | +28.0% / +3.0% | +38.9% / +34.6% |
| Twitter (pretty) | +42.3% / +62.7% | +21.5% / +21.9% | +25.4% / +26.1% | +52.7% / +41.7% | tie / +25.5% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +13.8% / +29.5% | +19.0% / +22.4% | +23.5% / +36.1% | +63.1% / +62.9% | +16.3% / +21.7% |
| Apache Builds (pretty) | +20.0% / +17.5% | +15.4% / +20.5% | +21.5% / +29.8% | +51.8% / +51.3% | −4.2% / +17.0% |
| Canada (min) | +7.8% / tie | −0.4% / −1.8% | −7.0% / −9.7% | +12.2% / −3.5% | +12.7% / +28.3% |
| Canada (pretty) | +0.6% / tie | +0.3% / −2.8% | −5.4% / −6.5% | −1.4% / −3.6% | +5.3% / −2.1% |
| CitmCatalog (min) | +8.1% / +5.9% | +0.9% / +3.5% | +7.7% / +12.0% | +15.3% / +18.7% | +8.1% / +14.7% |
| CitmCatalog (pretty) | +9.7% / +10.3% | +1.1% / +2.2% | +3.6% / +7.5% | +12.6% / +15.0% | +7.6% / +9.6% |
| Discord (min) | +9.5% / +27.7% | +7.5% / +13.8% | +16.1% / +28.9% | +36.1% / +60.8% | +23.3% / +31.8% |
| Discord (pretty) | +13.5% / +23.5% | +7.4% / +12.5% | +5.3% / +13.7% | +25.0% / +43.7% | +20.4% / +56.1% |
| Github Events (min) | +9.9% / +15.6% | +7.0% / +10.1% | +7.1% / +7.8% | +20.8% / +29.8% | +23.0% / +30.2% |
| Github Events (pretty) | +5.9% / +7.0% | +3.1% / +5.8% | +1.9% / +4.1% | +12.8% / +18.1% | +11.7% / +11.8% |
| Google Maps (min) | +5.5% / +9.8% | +12.5% / +16.0% | +25.7% / +29.7% | +34.0% / +38.9% | +23.5% / +17.2% |
| Google Maps (pretty) | +7.7% / +17.0% | +9.9% / +12.0% | +22.4% / +23.3% | +31.5% / +35.1% | +20.7% / +13.7% |
| Instruments (min) | +22.6% / +21.8% | +1.0% / +2.8% | +7.8% / +9.0% | +12.4% / +8.4% | +14.4% / +15.8% |
| Instruments (pretty) | +20.1% / +17.6% | −0.9% / −1.5% | +1.3% / +1.2% | +11.5% / +8.1% | +7.5% / +6.6% |
| Marine IK (min) | +0.5% / +1.4% | +5.4% / +6.5% | +5.9% / +6.3% | +9.6% / +10.8% | +10.2% / +11.0% |
| Marine IK (pretty) | +0.3% / +1.5% | +3.8% / +2.8% | +1.5% / +1.2% | +7.7% / +26.0% | +13.8% / +23.4% |
| Random (min) | +15.6% / +19.0% | +13.8% / +18.7% | +27.8% / +33.9% | +49.2% / +62.4% | +33.0% / +32.8% |
| Random (pretty) | +9.8% (n/c) / +25.4% | +14.0% / +15.5% | +23.3% / +31.0% | +44.2% / +59.0% | +32.8% / +31.4% |
| Twitter (min) | +25.7% / +34.8% | +8.2% / +10.1% | +16.7% / +19.7% | +32.4% / +39.9% | +17.1% / +21.8% |
| Twitter (pretty) | +19.3% / +22.6% | +7.2% / +9.8% | +11.3% / +13.9% | +24.1% / +36.4% | +12.7% / +16.3% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +73.7% / +97.8% | +78.7% / +94.0% | +93.1% / +110.3% | +97.6% / +151.4% | +125.3% / +123.7% |
| Apache Builds (pretty) | +59.0% / +71.1% | +68.5% / +78.8% | +87.9% / +101.0% | +82.6% / +100.3% | +82.8% / +97.2% |
| Canada (min) | +1.2% / +3.2% | +0.4% / +0.8% | −4.3% / −5.2% | −1.8% / −5.9% | +23.2% / +13.7% |
| Canada (pretty) | +1.5% / +3.9% | +1.0% / +1.0% | −3.6% / −4.6% | +3.5% / +3.9% | +2.6% / −3.7% |
| CitmCatalog (min) | +49.1% / +51.9% | +61.0% / +72.0% | +63.3% / +69.6% | +68.9% / +69.9% | +83.0% / +94.4% |
| CitmCatalog (pretty) | +35.5% / +36.9% | +54.2% / +57.1% | +51.1% / +56.1% | +56.9% / +49.4% | +68.6% / +77.2% |
| Discord (min) | +171.4% / +242.1% | +239.0% / +279.4% | +227.5% / +264.7% | +248.6% / +306.5% | +351.0% / +405.4% |
| Discord (pretty) | +185.7% / +214.5% | +204.4% / +235.5% | +205.8% / +229.1% | +216.7% / +261.8% | +364.1% / +321.9% (n/c) |
| Github Events (min) | +31.7% / +34.2% | +42.7% / +47.9% | +32.2% / +34.9% | +52.6% / +55.7% | +68.1% / +83.1% |
| Github Events (pretty) | +16.0% / +23.8% | +25.5% / +30.1% | +22.5% / +25.0% | +30.0% / +35.3% | +51.4% / +52.8% |
| Google Maps (min) | +2.3% / +3.0% | +6.2% / +8.6% | +16.7% / +16.7% | +30.2% / +33.2% | +13.0% / +20.2% |
| Google Maps (pretty) | +3.1% / +1.3% | +4.0% / +4.4% | +10.8% / +12.0% | +28.0% / +30.5% | +11.0% / +11.3% |
| Instruments (min) | +167.0% / +174.7% | +162.4% / +165.7% | +163.0% / +168.0% | +132.0% / +153.0% | +225.7% / +266.6% |
| Instruments (pretty) | +143.3% / +154.2% | +138.2% / +139.8% | +140.9% / +144.3% | +109.0% / +123.8% | +206.5% / +217.0% |
| Marine IK (min) | +30.4% / +33.2% | +45.2% / +49.4% | +41.5% / +43.4% | +56.0% / +50.2% | +71.4% / +67.8% |
| Marine IK (pretty) | +30.9% / +25.0% | +35.6% / +35.4% | +37.6% / +38.9% | +48.9% / +44.8% | +52.3% / +46.2% |
| Random (min) | +55.5% / +80.6% | +84.7% / +95.7% | +80.2% / +97.7% | +85.7% / +148.1% | +119.1% / +176.8% |
| Random (pretty) | +39.0% / +53.5% | +65.2% / +74.1% | +67.7% / +77.9% | +95.0% / +80.1% | +97.9% / +110.5% |
| Twitter (min) | +161.5% / +157.2% | +153.3% / +160.1% | +125.8% / +129.6% | +136.7% / +147.5% | +238.0% / +251.4% |
| Twitter (pretty) | +145.3% / +143.8% | +132.2% / +140.9% | +110.1% / +111.2% | +89.3% / +169.9% | +245.4% / +260.6% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +24.4% / +35.7% | +31.4% / +29.4% | +44.6% / +53.1% | +58.2% / +90.4% (n/c) | +33.7% / +40.0% |
| Apache Builds (pretty) | +13.8% / +21.6% | +25.7% / +29.5% | +30.4% / +36.7% | +37.5% / +53.4% | +25.5% / +27.1% |
| Canada (min) | +45.2% / +38.1% | +15.4% / +15.7% | +21.9% / +23.0% | +24.3% / +24.9% | +1.5% / +16.4% |
| Canada (pretty) | +36.5% / +42.0% | +7.4% / +7.9% | +21.0% / +22.7% | +18.0% / +18.7% | −2.4% / +1.3% |
| CitmCatalog (min) | +33.1% / +36.6% | +13.8% / +16.3% | +16.1% / +16.2% | +24.8% / +14.4% | +42.9% / +18.5% |
| CitmCatalog (pretty) | +24.1% / +25.9% | +5.7% / +11.7% | +10.7% / +10.5% | tie / +16.2% | +6.8% / +6.7% |
| Discord (min) | +12.2% / +22.4% | +12.0% / +12.6% | +18.1% / +20.1% | +16.5% / +23.0% | +61.0% / +63.3% |
| Discord (pretty) | +17.6% / +19.3% | +9.2% / +11.2% | +14.6% / +19.1% | +19.6% / +23.8% | +19.9% / +23.6% |
| Github Events (min) | +18.1% / +21.7% | +19.8% / +17.3% | +26.6% / +28.6% | +32.7% / +33.9% | +18.2% / +17.5% |
| Github Events (pretty) | +12.4% / +14.7% | +12.5% / +11.7% | +13.3% / +14.4% | +17.7% / +9.6% | +8.1% / +3.2% |
| Google Maps (min) | +20.6% / +26.6% | +9.7% / +13.8% | +27.1% / +26.7% | +20.1% / +22.1% | +13.6% / +13.6% |
| Google Maps (pretty) | +17.1% / +23.0% | +6.5% / +5.4% | +10.0% / +11.9% | +17.3% / +24.1% | +1.8% / +3.1% |
| Instruments (min) | +17.9% / +25.0% | +16.4% / +17.1% | +13.9% / +12.6% | +11.0% / +11.6% | +20.6% / +21.8% |
| Instruments (pretty) | +17.5% / +18.4% | +12.8% / +11.1% | +8.7% / +10.4% | +9.7% / +10.5% | +9.7% / +9.6% |
| Marine IK (min) | +9.8% / +12.4% | +9.5% / +27.0% | +9.9% / +12.3% | −8.7% / +5.6% | +14.1% / +21.4% |
| Marine IK (pretty) | +14.3% / +16.3% | +18.2% / +18.7% | +14.4% / +13.6% | +8.2% / +15.7% | +9.6% / +16.2% |
| Random (min) | +14.8% / +20.8% | +21.6% / +23.7% | +31.8% / +35.5% | +50.5% / +54.4% | +16.7% / +38.3% |
| Random (pretty) | +14.7% (n/c) / +20.2% | +18.5% / +18.9% | +23.1% / +21.5% | +6.7% / +50.3% | +10.3% / +33.1% |
| Twitter (min) | +41.0% / +37.3% | +7.5% / +8.7% | +19.1% / +17.0% | +21.0% / +4.8% | +15.8% / +17.5% |
| Twitter (pretty) | +29.6% / +24.0% | +2.4% / +4.9% | +8.4% / +11.1% | +9.6% / +5.5% | +11.2% / +6.7% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +27.6% / +26.0% | +22.9% / +20.1% | +23.3% / +26.9% | +55.5% / +57.6% | +15.5% / +20.9% |
| Apache Builds (pretty) | +17.6% / +16.3% | +11.5% / +12.8% | +14.7% / +20.4% | +27.2% / +33.3% | +9.6% / +10.7% |
| Canada (min) | +25.6% / +28.9% | +14.3% / +16.1% | +12.2% / +12.0% | +18.6% / +35.4% | tie / +13.2% |
| Canada (pretty) | +30.2% / +35.0% | +19.1% / +20.4% | +16.4% / +14.3% | +15.1% / +15.8% | −14.6% / +1.1% |
| CitmCatalog (min) | +27.4% / +28.1% | +3.7% / +2.4% | +12.9% / +13.2% | +10.5% / +10.0% | +32.5% / +55.2% |
| CitmCatalog (pretty) | +26.2% / +23.2% | +2.0% / +0.3% | +11.0% / +10.0% | +9.4% / +9.4% | +6.8% / +7.1% |
| Discord (min) | +26.3% / +41.3% | +46.6% / +50.9% | +42.0% / +49.5% | +46.3% / +75.0% | +94.5% / +98.1% |
| Discord (pretty) | +27.5% / +35.0% | +32.4% / +40.0% | +38.9% / +45.9% | +39.9% / +45.2% | +85.9% / +98.5% |
| Github Events (min) | +33.9% / +40.5% | +31.5% / +30.5% | +34.2% / +42.5% | +61.0% / +18.7% (n/c) | +45.3% / +29.7% |
| Github Events (pretty) | +31.9% / +28.3% | +26.0% / +27.2% | +22.5% / +23.8% | +27.0% / +29.0% | +35.0% / +38.2% |
| Google Maps (min) | +17.8% / +19.5% | +5.4% / +5.5% | +11.7% / +8.3% | +7.9% / +9.2% | +13.2% / +13.3% |
| Google Maps (pretty) | +17.9% / +17.4% | +4.0% / +5.0% | +9.0% / +12.1% | +9.6% / +9.9% | +6.0% / +16.6% |
| Instruments (min) | +32.6% / +40.1% | +32.4% / +35.6% | +19.3% / +20.0% | +33.6% / +27.6% | +46.0% / +48.8% |
| Instruments (pretty) | +28.7% / +35.1% | +22.4% / +21.3% | +24.7% / +26.8% | +16.8% / +18.8% | +32.5% / +32.3% |
| Marine IK (min) | +7.6% / +6.5% | +27.7% / +28.9% | +12.0% / +15.7% | +31.9% / +31.6% | +36.0% / +41.7% |
| Marine IK (pretty) | +35.0% / +45.8% | +16.0% / +17.8% | +17.9% / +17.7% | +29.8% / +29.7% | +19.6% / +21.5% |
| Random (min) | +18.6% / +26.7% | +26.5% / +30.6% | +37.5% / +39.1% | +58.0% / +29.8% | +48.1% / +52.1% |
| Random (pretty) | +13.6% / +17.4% | +25.5% / +25.9% | +31.3% / +30.7% | +7.9% / +32.9% | +36.6% / +26.4% |
| Twitter (min) | +41.7% / +40.0% | +20.3% / +22.6% | +22.4% / +25.0% | +5.6% / +27.4% | +27.3% / +28.3% |
| Twitter (pretty) | +24.1% / +27.6% | +14.2% / +16.2% | +14.7% / +11.7% | +23.2% / +8.2% | +18.3% / +18.1% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.6x to 5.4x faster at full size: +61.3% (Twitter pretty, macOS/GCC, fresh) up to +443.1% (Discord min, macOS/Clang, fresh). The small documents show the same pattern, from +89.3% to +405.4%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size result runs from a loss (prettified Canada on macOS/GCC, fresh, −11.7%, n/c) to +32.9%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** It won all 160 of its results. Read in reverse at full size it wins on every build (+76.6% to +200.1%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 195 of 200 full-size sparse-reverse results across both allocation modes (3 ties, 2 losses), and 198 of 200 small ones (1 tie, 1 loss). MSVC, Linux/GCC and macOS/GCC won every result at both sizes. Every loss and tie but one is on macOS/Clang: the full-size losses are prettified CitmCatalog (fresh, −8.2%) and prettified Google Maps (reused, −13.8%), and the small loss is prettified Canada Small (fresh, −14.6%). The other tie is prettified CitmCatalog on Linux/Clang (reused).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 237.6 MB/s on MSVC against 299.6 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 690.9 and 904.5 MB/s. Every MSVC full-size sparse result is a win, by +6.1% to +68.2%, and every full-size sparse-reverse result by +11.1% to +62.7%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 189 of 200 full-size sparse results (2 ties, 9 losses) and 197 of 200 small ones (1 tie, 2 losses). MSVC and Linux/GCC won all 40 of their full-size results. The full-size losses are minified Canada on Linux/Clang (−2.4% fresh, −2.3% reused), minified CitmCatalog on macOS/GCC (−17.5% fresh, −16.5% reused) and minified Marine IK on macOS/GCC (fresh, −10.1%), and prettified CitmCatalog (−9.6% fresh, −10.8% reused) and Instruments (−7.2% fresh, −5.6% reused) on macOS/Clang. The small losses are prettified Canada Small on macOS/Clang (fresh, −2.4%) and minified Marine IK Small on macOS/GCC (fresh, −8.7%). The largest loss is minified CitmCatalog on macOS/GCC (fresh, −17.5%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 181, tied 7 and lost 12. Linux/Clang went 39-1-0, Linux/GCC 38-0-2, MSVC 36-2-2, and macOS/GCC and macOS/Clang 34-2-4 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 184, tied 2 and lost 14. MSVC went 38-2-0, macOS/Clang 38-0-2, macOS/GCC 37-0-3, Linux/GCC 36-0-4 and Linux/Clang 35-0-5. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Canada Small holds 11 of the 14 losses: Linux/GCC in both forms and both runs (−5.4% to −9.7%), Linux/Clang minified (−0.4% fresh, −1.8% reused) and prettified (reused, −2.8%), macOS/GCC minified (reused, −3.5%) and prettified (−1.4% fresh, −3.6% reused), and macOS/Clang prettified (reused, −2.1%). The other 3 are prettified Instruments Small on Linux/Clang (−0.9% fresh, −1.5% reused) and prettified Apache Builds Small on macOS/Clang (fresh, −4.2%). The largest is minified Canada Small on Linux/GCC (reused, −9.7%).
- **In-order losses at full size fall mostly on two documents.** Linux/GCC lost minified Canada in both runs (−3.0% and −5.0%) and MSVC minified Marine IK in both runs (−8.2% and −5.2%). macOS/GCC lost minified Canada (fresh, −3.5%), minified Marine IK in both runs (−6.3% and −3.6%, n/c) and prettified Marine IK (reused, −11.4%, n/c), and macOS/Clang lost prettified Instruments (fresh, −8.6%), prettified Apache Builds (fresh, −1.1%) and minified Twitter in both runs (−1.1%). Linux/Clang lost none. Canada and Marine IK hold 8 of the 12; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 43-0-7 on them. Bool, Int64 and Uint64 won all 10 results each (+18.1% to +109.6%, +71.3% to +168.1% and +79.8% to +214.4%), and Double won 9 (+52.4% to +131.4%) and lost reused on macOS/Clang (−51.8%). String is the weakest: 4 wins (+3.7% to +36.1%) and 6 losses, on Linux/Clang (−30.8% fresh, −34.8% reused), Linux/GCC (−6.8% fresh, −5.5% reused) and macOS/Clang (−3.5% fresh, −4.5% reused).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is below its own NDJSON throughput on the same records in all 18 build and stream pairs, by 4.7% to 19.2% (7.1% to 14.8% on the two Linux builds).

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 22 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 25 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.5 | NEON | 23 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 4-2-0 | 12-0-0 | 12-0-0 | 28-2-0 |
| macOS / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| **All** | **26-2-2** | **60-0-0** | **60-0-0** | **146-2-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | −5.0% | +21.4% | +36.4% | +20.8% | +25.0% |
| Large Amazon Cellphones (NDJSON) | −9.3% | +11.5% | +18.7% | tie | +5.5% |
| Stream Formats Small (NDJSON) | +27.0% | +11.4% | +27.4% | tie (n/c) | +6.7% |
| Stream Formats Small (comma) | +83.8% | +79.5% | +119.7% | +93.3% | +73.1% |
| Stream Formats Large (NDJSON) | +26.0% | +13.3% | +15.4% | +8.1% | +3.2% (n/c) |
| Stream Formats Large (comma) | +29.5% | +19.6% | +23.7% | +23.1% | +11.1% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +15.0% | +18.6% | +29.9% | +62.6% | +21.0% |
| Apache Builds (comma) | +22.0% | +25.5% | +48.8% | +67.7% | +18.2% |
| CitmCatalog (NDJSON) | +15.6% (n/c) | +12.5% | +19.9% | +9.9% | +11.9% |
| CitmCatalog (comma) | +24.6% (n/c) | +31.6% | +48.9% | +52.7% | +25.6% |
| Google Maps (NDJSON) | +37.7% (n/c) | +28.5% | +26.3% | +30.0% | +7.3% |
| Google Maps (comma) | +42.4% (n/c) | +38.8% | +48.0% | +52.5% (n/c) | +9.7% |
| Instruments (NDJSON) | +43.9% | +19.6% | +16.7% | +4.7% (n/c) | +20.5% |
| Instruments (comma) | +40.2% | +35.0% | +58.0% | +23.7% | +35.8% |
| Random (NDJSON) | +20.5% | +23.6% | +34.2% | +49.6% (n/c) | +44.5% |
| Random (comma) | +22.9% | +35.0% | +51.4% | +74.2% | +47.2% |
| Twitter (NDJSON) | +30.0% | +9.5% | +5.2% | +15.0% | +9.3% |
| Twitter (comma) | +33.5% | +19.0% | +26.1% | +30.5% | +11.7% (n/c) |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +9.7% | +15.0% | +21.6% | +26.8% | +25.6% |
| Apache Builds (comma) | +24.5% | +26.6% | +45.7% | +57.1% | +35.8% |
| CitmCatalog (NDJSON) | +25.5% | +33.3% | +38.6% | +31.8% | +46.2% (n/c) |
| CitmCatalog (comma) | +41.9% (n/c) | +51.0% | +58.1% | +52.3% | +69.9% (n/c) |
| Google Maps (NDJSON) | +17.0% | +13.4% | +12.3% | +17.0% | +11.7% |
| Google Maps (comma) | +22.3% | +20.7% | +26.9% | +34.3% | +17.3% |
| Instruments (NDJSON) | +36.9% | +34.1% | +26.3% | +29.3% | +43.0% (n/c) |
| Instruments (comma) | +51.7% (n/c) | +54.4% | +56.8% | +56.6% (n/c) | +76.2% |
| Random (NDJSON) | +57.1% | +78.8% | +77.4% | +82.2% | +104.8% (n/c) |
| Random (comma) | +65.1% | +92.2% | +97.1% | +92.8% | +128.2% |
| Twitter (NDJSON) | +170.1% (n/c) | +165.0% | +140.9% | +124.5% | +233.1% |
| Twitter (comma) | +202.5% (n/c) | +172.8% | +150.5% | +149.3% | +268.0% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 26 of the 30 results in that group, tied 2 and lost 2. It won Stream Formats Small comma-separated on all five builds (+73.1% to +119.7%) and as NDJSON on four (+6.7% to +27.4%), tying it on macOS/GCC, and Stream Formats Large in both forms on all five (NDJSON +3.2% to +26.0%, comma +11.1% to +29.5%). Amazon Cellphones won on every build but MSVC (+20.8% to +36.4%), which lost it (−5.0%); Large Amazon Cellphones won on three builds (+5.5% to +18.7%), tied on macOS/GCC and lost on MSVC (−9.3%).
- **Three builds won every streaming test,** 30-0-0: Linux/Clang, Linux/GCC and macOS/Clang. macOS/GCC went 28-2-0 and MSVC 28-0-2.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +124.5% to +268.0%, while Google Maps rows, which have few, give +11.7% to +34.3%, and Instruments sits between them (+26.3% to +76.2%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 351.3 MB/s under MSVC and 451.4 MB/s under Linux/Clang, while Jsonifier reads it at 580.1 and 867.7 MB/s.

## 9. AVX-512 builds

Every result in §7 and §8 comes from AVX2 and NEON builds. This section repeats the whole suite, single documents and streams, on three builds that run Jsonifier's AVX-512 stage-1 kernel against simdjson's own AVX-512 kernel (`icelake`).

### 9.1 Method

- **Harness:** Json-Performance, branch `generic-parsing-avx512-main`, with the same tests, readers, allocation modes, batch size and adaptive-sampling stage as §6 and §8.3, and the same convergence limits as the x86 builds there (RSE < 5% and shift < 2.5%).
- **Hosts:** the builds ran on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, under Linux `7.0.0-1020-azure` and Windows 10.0.26100. These are shared virtual machines, not the dedicated Core i9-14900KF of §6, which has no AVX-512.
- **Compilers:** Clang 24.0 and MSVC 19.52 are the same as in §6. GCC 16.0.1 is the runner image's compiler and is older than §6's GCC 16.2.1.
- **Commits:** BenchmarkSuite `71afc62`, Jsonifier `4f52e70` and simdjson `3448984`, the same as §6. All three builds ran on November 23, 2026. The stage-1 tapes of these builds match simdjson's position for position (§6, *Correctness*).

| Build | Stage-1 kernel | Converged, single documents | Converged, streams |
|---|---|---|---|
| Windows 10.0.26100 / MSVC 19.52 | AVX-512 | 325 / 330 | 27 / 30 |
| Linux 7.0.0 (Azure) / Clang 24.0 | AVX-512 | 326 / 330 | 26 / 30 |
| Linux 7.0.0 (Azure) / GCC 16.0.1 | AVX-512 | 328 / 330 | 29 / 30 |

The CPU, the operating-system images and the GCC version differ from §6, so a difference between an AVX-512 result here and the matching AVX2 result in §7 cannot be credited to the instruction set alone. Each comparison below is between the two libraries within one AVX-512 build.

### 9.2 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-0-2 | 20-0-0 | 17-2-1 | 18-0-2 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 157-2-6 |
| Linux / Clang | 4-0-1 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 15-1-4 | 20-0-0 | 20-0-0 | 19-0-1 | 156-2-7 |
| Linux / GCC | 4-0-1 | 16-1-3 | 20-0-0 | 19-0-1 | 18-1-1 | 14-0-6 | 20-0-0 | 17-0-3 | 19-0-1 | 147-2-16 |
| **All** | **13-0-2** | **52-2-6** | **60-0-0** | **56-2-2** | **56-1-3** | **49-1-10** | **60-0-0** | **56-0-4** | **58-0-2** | **460-6-29** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-0-2 | 19-1-0 | 17-1-2 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 157-2-6 |
| Linux / Clang | 4-0-1 | 15-0-5 | 20-0-0 | 20-0-0 | 20-0-0 | 17-0-3 | 20-0-0 | 20-0-0 | 19-1-0 | 155-1-9 |
| Linux / GCC | 5-0-0 | 15-1-4 | 20-0-0 | 18-0-2 | 18-0-2 | 15-0-5 | 18-0-2 | 18-0-2 | 19-0-1 | 146-1-18 |
| **All** | **14-0-1** | **48-1-11** | **59-1-0** | **55-1-4** | **56-0-4** | **52-0-8** | **58-0-2** | **58-0-2** | **58-1-1** | **458-4-33** |

**Streams:**

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 10-0-2 | 11-0-1 | 25-0-5 |
| Linux / Clang | 6-0-0 | 10-0-2 | 11-1-0 | 27-1-2 |
| Linux / GCC | 6-0-0 | 10-1-1 | 11-0-1 | 27-1-2 |
| **All** | **16-0-2** | **30-1-5** | **33-1-2** | **79-2-9** |

Each cell is wins-ties-losses.

Across the 990 single-document results (11 of them did not converge), Jsonifier won 918, tied 10 and lost 62: 460-6-29 with fresh output objects and 458-4-33 with reused ones. Across the 90 streaming results (8 of them did not converge) it won 79, tied 2 and lost 9.

### 9.3 POD tests

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Bool | +98.4% / +105.6% | +34.0% / +33.9% | +65.7% / +59.3% |
| Double | +54.2% / +82.6% | +74.9% / +76.4% | +119.9% / +140.8% |
| Int64 | +76.8% / +99.8% | +103.3% / +115.6% | +161.4% / +177.4% |
| String | +13.7% / +19.4% | −16.9% / −18.5% | −4.2% / +7.0% |
| Uint64 | +61.3% / +79.3% | +110.3% / +112.9% | +167.5% / +183.0% |

### 9.4 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −2.3% / −8.7% | +6.1% / +3.6% | +3.7% / +2.9% |
| Apache Builds (pretty) | −2.4% / −9.2% | +5.4% / −2.4% | +2.9% / +1.9% |
| Canada (min) | +3.0% / +4.8% | +8.1% / +6.4% | +9.2% / +9.4% |
| Canada (pretty) | +5.5% / +2.0% (n/c) | tie / −1.2% | +5.4% / +5.9% |
| CitmCatalog (min) | +9.0% / +2.5% | +1.2% / −4.1% | +5.0% / +13.7% |
| CitmCatalog (pretty) | +9.2% / +4.1% | +0.7% / −1.4% | +3.5% / tie |
| Discord (min) | +15.5% / +22.9% | +7.4% / +16.3% | tie / +10.5% |
| Discord (pretty) | +14.5% / +21.7% | +5.3% / +12.5% | +2.7% / +6.4% |
| Github Events (min) | +24.2% / +30.0% | +37.6% / +38.4% | +15.4% / +24.7% |
| Github Events (pretty) | +18.9% / +24.5% | +32.1% / +33.0% | +9.4% / +11.9% |
| Google Maps (min) | +15.7% / +15.5% | +7.9% / +7.4% | +3.2% / −0.7% |
| Google Maps (pretty) | +13.1% / +15.4% | +2.7% / +6.3% | −1.7% / −1.0% |
| Instruments (min) | +10.5% / +14.0% | +10.1% / +9.0% | +10.4% / +4.4% |
| Instruments (pretty) | +21.7% / +24.7% | +8.8% / +7.6% | +2.1% / +1.4% |
| Marine IK (min) | +2.1% / +5.1% | +3.7% / +4.3% | +7.9% / +8.8% |
| Marine IK (pretty) | +6.8% / +4.7% (n/c) | +4.0% / +7.2% | +7.1% / +8.0% |
| Random (min) | +10.9% / +16.6% | +10.8% / +8.6% | +7.1% / +10.1% |
| Random (pretty) | +14.1% / +17.7% | +7.3% / +14.1% | +5.8% / +9.9% |
| Twitter (min) | +20.4% / +23.5% | +2.8% / −1.5% | −15.6% / −10.5% |
| Twitter (pretty) | +19.8% / +22.0% | −6.4% / +2.2% | −12.5% / −7.8% |

### 9.5 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +34.8% / +45.2% | +79.4% / +95.0% | +67.6% / +75.2% |
| Apache Builds (pretty) | +36.3% / +53.4% | +75.5% / +91.5% | +60.0% / +71.9% |
| Canada (min) | +5.5% / tie | +10.2% (n/c) / +17.4% | +17.8% / +20.8% |
| Canada (pretty) | +7.4% / +4.6% | +16.1% / +13.8% | +10.1% / +14.6% |
| CitmCatalog (min) | +59.9% / +61.4% | +103.2% / +122.9% | +105.8% / +107.7% |
| CitmCatalog (pretty) | +58.5% / +63.0% | +74.5% / +100.6% | +97.4% / +96.3% |
| Discord (min) | +279.4% / +323.5% | +339.6% / +375.0% | +340.6% / +387.3% |
| Discord (pretty) | +319.1% / +363.1% | +333.4% / +367.2% | +256.9% / +283.2% |
| Github Events (min) | +90.6% / +112.3% | +109.0% / +128.5% | +64.1% / +79.2% |
| Github Events (pretty) | +84.5% / +103.2% | +90.6% / +119.4% | +58.9% / +59.4% |
| Google Maps (min) | +17.4% / +21.5% | +30.0% / +27.9% | +8.3% / +10.4% |
| Google Maps (pretty) | +19.6% / +18.9% | +18.5% / +26.9% | +3.5% / +2.6% |
| Instruments (min) | +214.5% / +228.4% | +195.7% / +223.0% | +150.9% / +162.8% |
| Instruments (pretty) | +199.9% / +211.2% | +183.0% / +188.5% | +128.4% / +141.9% |
| Marine IK (min) | +43.2% (n/c) / +40.3% (n/c) | +87.7% / +94.9% (n/c) | +98.4% (n/c) / +78.5% |
| Marine IK (pretty) | +37.2% / +41.6% | +69.4% / +65.9% | +69.4% (n/c) / +78.5% |
| Random (min) | +100.8% / +121.8% | +98.0% / +117.1% | +65.1% / +79.0% |
| Random (pretty) | +106.2% / +127.8% | +92.9% / +107.9% | +45.4% / +56.4% |
| Twitter (min) | +189.4% / +220.6% (n/c) | +162.9% / +179.7% | +110.3% / +110.4% |
| Twitter (pretty) | +177.2% / +194.3% | +147.3% / +152.8% | +79.0% / +92.2% |

### 9.6 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −1.0% / −0.6% | +19.8% / +23.6% | +15.5% / +12.4% |
| Apache Builds (pretty) | tie / −3.9% | +16.1% / +20.3% | +12.1% / +17.1% |
| Canada (min) | +28.9% / +29.8% | +3.2% (n/c) / +15.5% | +23.0% / +24.5% |
| Canada (pretty) | +31.0% / +32.2% | +15.5% (n/c) / +16.9% | +12.3% / +17.6% |
| CitmCatalog (min) | +15.9% / +18.1% | +21.8% / +28.0% | +21.7% / +21.7% |
| CitmCatalog (pretty) | +22.9% / +23.9% | +27.1% / +26.1% | +20.5% / +22.1% |
| Discord (min) | tie / +0.6% | +4.6% / +3.1% | +5.1% / +5.6% |
| Discord (pretty) | +1.4% / tie | +4.9% / +2.8% | +2.0% / +5.6% |
| Github Events (min) | +31.1% / +39.9% | +36.5% / +28.0% | +26.1% / +24.1% |
| Github Events (pretty) | +24.7% / +33.3% | +25.1% / +20.3% | +13.8% / +10.4% |
| Google Maps (min) | +32.8% / +34.7% | +5.2% / +6.5% | +19.1% / +15.5% |
| Google Maps (pretty) | +31.1% / +31.7% | +4.5% / +5.9% | −2.3% / −2.3% |
| Instruments (min) | +30.0% / +35.5% | +21.4% / +19.5% | +28.5% / +25.2% |
| Instruments (pretty) | +29.2% / +30.5% | +15.3% / +20.7% | +15.2% / +12.5% |
| Marine IK (min) | +19.1% / +18.9% | +26.8% / +25.5% | +35.0% / +39.3% |
| Marine IK (pretty) | +23.2% / +22.2% | +17.2% / +20.2% | +33.8% / +32.3% |
| Random (min) | +27.5% / +35.0% | +24.7% / +28.7% | +13.7% / +13.8% |
| Random (pretty) | +28.5% / +33.5% | +14.1% / +26.2% | +11.0% / +10.0% |
| Twitter (min) | +118.6% / +134.1% | +21.7% / +20.8% | +5.9% / +9.9% |
| Twitter (pretty) | +99.7% / +113.0% | +21.8% / +21.5% | +7.4% / −0.4% |

### 9.7 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −1.3% / −2.8% | +14.7% / +20.7% | +4.3% / +12.7% |
| Apache Builds (pretty) | −3.2% / −4.6% | +13.4% / +17.5% | tie / +6.8% |
| Canada (min) | +26.2% / +26.7% | +2.5% / +19.8% | +6.9% / +13.6% |
| Canada (pretty) | +27.1% / +27.9% | +27.5% / +21.0% | +22.9% / +21.2% |
| CitmCatalog (min) | +29.2% / +31.0% | +24.5% / +22.6% | +23.8% / +19.9% |
| CitmCatalog (pretty) | +29.8% / +30.9% | +15.7% / +17.2% | +10.5% / +11.9% |
| Discord (min) | +44.3% / +57.1% | +49.6% / +57.8% | +35.6% / +42.9% |
| Discord (pretty) | +42.9% / +53.9% | +38.6% / +48.5% | +27.7% / +33.4% |
| Github Events (min) | +81.9% / +96.0% | +43.5% / +51.3% | +42.0% / +44.9% |
| Github Events (pretty) | +73.5% / +84.5% | +56.0% / +64.2% | +43.9% / +30.6% |
| Google Maps (min) | +21.4% / +21.7% | +1.5% / +16.1% | +0.3% / −3.8% |
| Google Maps (pretty) | +19.3% / +22.4% | +9.0% / +15.7% | −3.9% / −7.4% |
| Instruments (min) | +65.0% / +70.1% | +53.0% / +47.8% | +55.1% / +54.8% |
| Instruments (pretty) | +52.1% / +63.3% | +42.7% / +45.4% | +40.7% / +48.2% |
| Marine IK (min) | +53.8% / +53.8% | +121.1% / +128.8% | +92.6% / +102.2% |
| Marine IK (pretty) | +44.6% / +43.8% | +80.5% / +76.5% | +48.5% / +62.7% |
| Random (min) | +53.6% / +60.2% | +37.2% / +37.5% | +20.7% / +17.3% |
| Random (pretty) | +55.4% / +60.8% | +22.4% / +27.9% | +11.9% / +13.9% |
| Twitter (min) | +106.9% / +116.7% | +43.7% / +41.1% | +20.9% / +13.9% |
| Twitter (pretty) | +97.2% / +103.3% | +46.2% / +47.9% | +13.8% / +13.2% |

### 9.8 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +4.3% / +7.6% | +7.1% / +12.6% | +24.2% / +5.0% |
| Apache Builds (pretty) | +8.4% / +8.6% | +8.0% / +10.7% | +4.1% / +6.2% |
| Canada (min) | +3.2% / +3.1% | +9.9% / +7.4% | +9.6% / +6.4% |
| Canada (pretty) | +2.8% / +2.9% | +12.3% / +5.5% | +8.4% / +6.5% |
| CitmCatalog (min) | +1.2% / +3.3% | −7.8% / −9.6% | +4.6% / +4.1% |
| CitmCatalog (pretty) | +3.1% / +5.7% | −7.5% / −7.3% | −0.4% / −1.1% |
| Discord (min) | +14.5% / +33.4% | +3.1% / +14.1% | +4.9% / +20.5% |
| Discord (pretty) | +13.8% / +30.9% | +4.6% / +10.4% | +5.1% / +10.4% |
| Github Events (min) | +13.2% / +13.3% | +5.9% / +6.1% | −3.3% / −4.1% |
| Github Events (pretty) | +3.8% / +5.5% | −6.5% / −0.3% | −16.0% / −18.3% |
| Google Maps (min) | +12.9% / +15.5% | +10.8% / +19.7% | +0.3% / +0.8% |
| Google Maps (pretty) | +14.0% / +17.7% | +10.1% / +10.6% | −2.9% / −3.0% |
| Instruments (min) | +19.7% / +4.6% | +6.5% / +12.9% | +4.7% / +10.2% |
| Instruments (pretty) | +17.2% / +22.2% | +0.3% / +3.5% | +4.8% / +2.9% |
| Marine IK (min) | +4.9% / +5.2% | +10.9% / +15.1% | +8.3% / +4.2% |
| Marine IK (pretty) | +5.8% / +4.4% | +12.7% / +12.8% | +5.6% / +5.1% |
| Random (min) | +11.0% / +16.4% | +6.1% / +17.4% | +7.0% / +3.3% |
| Random (pretty) | +11.3% / +16.2% | +3.6% / +8.1% | +6.4% / +9.8% |
| Twitter (min) | +20.7% / +29.9% | −1.4% / +9.9% | −1.7% / +0.7% |
| Twitter (pretty) | +19.7% / +27.8% | tie / +3.4% | −5.8% / −1.5% |

### 9.9 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +73.0% / +23.0% | +106.7% / +117.0% | +83.7% / +88.8% |
| Apache Builds (pretty) | +66.4% / +93.4% | +95.4% / +120.3% | +65.6% / +76.7% |
| Canada (min) | +4.7% / +4.7% | +8.7% / +9.1% | +8.5% / +11.9% |
| Canada (pretty) | +4.3% / +4.3% | +9.9% / +14.1% | +13.6% / +9.2% |
| CitmCatalog (min) | +56.9% / +65.0% | +88.3% / +97.5% | +90.8% / +102.6% |
| CitmCatalog (pretty) | +52.8% / +60.3% | +74.1% / +76.0% | +62.2% / +72.7% |
| Discord (min) | +277.8% / +361.4% | +312.7% / +380.0% | +267.4% / +303.4% |
| Discord (pretty) | +272.1% / +362.4% | +304.8% / +342.9% | +217.0% / +258.7% |
| Github Events (min) | +36.5% / +41.5% | +44.5% / +55.2% | +16.2% / +15.4% |
| Github Events (pretty) | +29.4% / +35.8% | +30.9% / +26.9% | +0.6% / −3.1% |
| Google Maps (min) | +17.7% / +23.3% | +16.6% / +23.1% | +9.7% / +11.7% |
| Google Maps (pretty) | +17.0% / +20.4% | +25.5% / +24.7% | +5.6% / −3.5% |
| Instruments (min) | +204.9% / +222.0% | +196.1% / +205.3% | +147.3% / +165.5% |
| Instruments (pretty) | +198.2% / +210.6% | +167.9% / +164.5% | +121.2% / +130.4% |
| Marine IK (min) | +32.9% / +35.9% | +85.1% / +84.6% | +58.4% / +62.4% |
| Marine IK (pretty) | +29.9% / +33.2% | +74.5% / +75.3% | +42.8% / +51.7% |
| Random (min) | +96.8% / +119.4% | +124.0% / +115.4% | +67.5% / +79.2% |
| Random (pretty) | +98.4% / +123.5% | +98.2% / +107.9% | +47.3% / +53.3% |
| Twitter (min) | +192.0% / +209.5% | +166.4% / +172.4% | +111.5% / +107.9% |
| Twitter (pretty) | +187.8% / +196.9% | +151.2% / +151.2% | +92.8% / +88.6% |

### 9.10 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +32.2% / +38.1% | +25.7% / +29.7% | +23.6% / +22.1% |
| Apache Builds (pretty) | −30.1% / +25.7% | +22.9% / +22.8% | +11.4% / +12.8% |
| Canada (min) | +25.7% / +29.4% | +11.4% / +13.0% | +21.3% / +20.8% |
| Canada (pretty) | +34.2% / +37.3% | +18.8% / +32.2% | +8.2% / +6.6% |
| CitmCatalog (min) | +54.2% / +66.3% | +19.4% / +25.2% | +22.1% / +10.7% |
| CitmCatalog (pretty) | +48.2% / +51.8% | +22.6% / +22.9% | +15.7% / +15.0% |
| Discord (min) | +19.6% / +24.4% | +18.6% / +23.5% | +13.5% / +19.2% |
| Discord (pretty) | +21.8% / +20.6% | +12.3% / +13.2% | +11.9% / +12.6% |
| Github Events (min) | +38.4% / +45.8% | +28.6% / +38.6% | +10.8% / +10.0% |
| Github Events (pretty) | +23.7% / +34.3% | +16.9% / +25.0% | +3.5% / +4.4% |
| Google Maps (min) | +30.0% / +30.7% | +11.4% / +9.9% | +16.2% / +10.8% |
| Google Maps (pretty) | +29.1% / +30.8% | +4.3% / +11.9% | −2.0% / −2.6% |
| Instruments (min) | +53.5% / +56.5% | +25.6% / +28.1% | +12.4% / +20.0% |
| Instruments (pretty) | +44.2% / +41.2% | +21.3% / +21.0% | +7.4% / +16.1% |
| Marine IK (min) | +26.2% / +26.0% | +17.9% / +16.7% | +29.0% / +26.2% |
| Marine IK (pretty) | +34.9% / +35.3% | +15.8% / +23.2% | +27.2% / +27.8% |
| Random (min) | +33.6% / +36.8% | +30.7% / +36.0% | +13.7% / +13.2% |
| Random (pretty) | +28.6% / +34.3% | +15.4% / +13.5% | +12.7% / +8.1% |
| Twitter (min) | +82.3% / +90.3% | +22.5% / +24.7% | −2.3% / −0.6% |
| Twitter (pretty) | +77.5% / +90.7% | +19.1% / +27.0% | −3.3% / +4.6% |

### 9.11 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +20.1% / +18.2% | +28.6% / +30.9% | +20.4% / +21.0% |
| Apache Builds (pretty) | +15.2% / +13.3% | +17.2% / +27.3% | +17.2% / +10.6% |
| Canada (min) | +22.4% / +23.8% | −15.8% / tie | +14.0% / +8.9% |
| Canada (pretty) | +31.7% / +31.3% | +15.1% / +12.0% | +14.2% / +13.6% |
| CitmCatalog (min) | +57.6% / +59.2% | +14.4% / +28.2% | +18.1% / +28.2% |
| CitmCatalog (pretty) | +52.4% / +52.9% | +12.2% / +22.3% | +10.7% / +11.5% |
| Discord (min) | +71.2% / +89.4% | +78.5% / +88.6% | +66.2% / +55.8% |
| Discord (pretty) | +64.5% / +88.9% | +67.6% / +68.8% | +44.1% / +45.3% |
| Github Events (min) | +67.9% / +84.8% | +53.9% / +47.4% | +28.2% / +29.0% |
| Github Events (pretty) | +56.5% / +56.2% | +51.8% / +53.9% | +15.1% / +23.6% |
| Google Maps (min) | +23.1% / +23.2% | +9.9% / +19.1% | +1.6% / +2.3% |
| Google Maps (pretty) | +20.1% / +21.1% | +4.7% / +6.9% | −3.0% / −3.7% |
| Instruments (min) | +80.5% / +94.2% | +47.3% / +57.9% | +47.5% / +59.8% |
| Instruments (pretty) | +63.4% / +68.0% | +50.1% / +57.9% | +52.2% / +45.6% |
| Marine IK (min) | +59.6% / +59.5% | +86.4% / +95.5% | +72.2% / +80.4% |
| Marine IK (pretty) | +53.9% / +46.2% | +73.3% / +73.1% | +44.2% / +49.3% |
| Random (min) | +56.9% / +66.5% | +42.6% / +38.2% | +18.3% / +21.9% |
| Random (pretty) | +52.7% / +63.2% | +36.4% / +47.6% | +17.3% / +16.7% |
| Twitter (min) | +72.7% / +83.0% | +36.1% / +25.5% | +12.3% / +8.7% |
| Twitter (pretty) | +71.6% / +78.1% | +25.0% / +24.5% | +11.9% / +13.2% |

### 9.12 Streams

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 10-0-2 | 11-0-1 | 25-0-5 |
| Linux / Clang | 6-0-0 | 10-0-2 | 11-1-0 | 27-1-2 |
| Linux / GCC | 6-0-0 | 10-1-1 | 11-0-1 | 27-1-2 |
| **All** | **16-0-2** | **30-1-5** | **33-1-2** | **79-2-9** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Amazon Cellphones (NDJSON) | −17.9% | +22.5% | +11.9% |
| Large Amazon Cellphones (NDJSON) | −19.0% | +6.8% | +2.5% |
| Stream Formats Small (NDJSON) | +22.6% | +7.9% (n/c) | +11.9% (n/c) |
| Stream Formats Small (comma) | +61.6% | +50.2% | +91.4% |
| Stream Formats Large (NDJSON) | +44.0% | +20.7% | +30.9% |
| Stream Formats Large (comma) | +50.0% | +28.7% | +36.0% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | −15.0% | +7.5% | +3.8% |
| Apache Builds (comma) | −6.8% | +15.5% | +14.7% |
| CitmCatalog (NDJSON) | +2.7% | −3.1% | +4.5% |
| CitmCatalog (comma) | +5.7% | +12.0% | +24.5% |
| Google Maps (NDJSON) | +15.7% | +10.5% | +2.8% |
| Google Maps (comma) | +21.0% | +15.0% | +14.9% |
| Instruments (NDJSON) | +48.3% | +22.4% | +14.1% |
| Instruments (comma) | +57.1% | +28.6% | +39.6% |
| Random (NDJSON) | +17.7% | +8.7% | +12.1% |
| Random (comma) | +22.6% | +20.3% | +26.9% |
| Twitter (NDJSON) | +20.0% | −9.5% | −19.0% |
| Twitter (comma) | +23.6% | +1.0% | tie |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | −4.2% | +21.8% | +2.6% |
| Apache Builds (comma) | +3.6% | +30.7% | +16.1% |
| CitmCatalog (NDJSON) | +35.7% | +49.4% | +46.5% |
| CitmCatalog (comma) | +48.0% | +71.2% (n/c) | +69.1% |
| Google Maps (NDJSON) | +13.7% (n/c) | tie (n/c) | −10.5% |
| Google Maps (comma) | +15.2% | +14.0% | +2.0% |
| Instruments (NDJSON) | +88.3% | +66.2% | +9.1% |
| Instruments (comma) | +102.6% (n/c) | +79.1% | +33.9% |
| Random (NDJSON) | +114.5% (n/c) | +96.4% | +43.4% |
| Random (comma) | +123.8% | +105.6% | +55.7% |
| Twitter (NDJSON) | +212.2% | +165.0% | +99.1% |
| Twitter (comma) | +213.8% | +178.6% (n/c) | +111.6% |

### 9.13 What the results show

- **Reverse order wins on every AVX-512 build.** Jsonifier won 237 of the 240 reverse results (1 tie, 2 losses): Linux/Clang went 80-0-0, MSVC 79-1-0 and Linux/GCC 78-0-2. The losses are prettified Github Events Small (−3.1%) and prettified Google Maps Small (−3.5%) on Linux/GCC, both reused, and the tie is minified Canada on MSVC (reused). On the documents whose objects have many fields (Discord, Twitter, Instruments) the full-size margin runs from +79.0% (Twitter pretty, Linux/GCC, fresh) to +387.3% (Discord min, Linux/GCC, reused), and the small-document margin from +88.6% to +380.0%.
- **simdjson's AVX-512 kernel does not close the reverse-order gap.** On MSVC, simdjson reads Discord Reverse (pretty, fresh) at 174.7 MB/s and Jsonifier at 732.0 MB/s; on Linux/Clang the figures are 227.3 and 985.2 MB/s. simdjson's rescans are bound by how many keys it walks, not by how fast stage 1 runs, so a wider stage-1 kernel cannot shorten them.
- **Sparse reverse reads stay a strength.** Jsonifier won 112 of the 120 full-size sparse-reverse results (1 tie, 7 losses) and 116 of the 120 small ones (1 tie, 3 losses). Linux/Clang won all 40 of its full-size results and MSVC all 40 of its small ones. The full-size losses are Apache Builds on MSVC in both forms and both runs (−1.3% to −4.6%) and Google Maps on Linux/GCC (minified reused, −3.8%; prettified −3.9% fresh and −7.4% reused). The small losses are minified Canada Small on Linux/Clang (fresh, −15.8%; a tie reused) and prettified Google Maps Small on Linux/GCC (−3.0% fresh, −3.7% reused).
- **Linux/Clang loses mostly on in-order reads.** It went 311-3-16 and won all 80 of its reverse results. 13 of its 16 losses are in-order reads: 6 at full size (Apache Builds, Canada, CitmCatalog in both forms and Twitter in both forms, −1.2% to −6.4%) and 7 small (CitmCatalog Small in both forms and both runs, −7.3% to −9.6%, prettified Github Events Small in both runs and minified Twitter Small, fresh). The rest are String POD (2) and minified Canada Small read sparse in reverse (fresh, −15.8%).
- **Linux/GCC holds most of the AVX-512 losses:** 34 of the 62. 18 of them are in-order reads (7 at full size, 11 small), against 6 in-order losses for the GCC 16.2.1 build of §7 on the 14900KF. Google Maps accounts for 15 of the 34 and Twitter for 11, then Github Events with 5, CitmCatalog Small with 2 and String POD with 1. Read in order at full size, Linux/GCC lost Twitter in both forms and both runs (−7.8% to −15.6%) and Google Maps three times (−0.7% to −1.7%). Twitter wins by wide margins when read in reverse on this build (+79.0% and more), so the gap is in reading in order, not in field lookup. Because this build differs from §7's in compiler version as well as CPU, these results alone do not say which of the two is responsible.
- **String POD loses on Linux/Clang in both runs** (−16.9% fresh, −18.5% reused) and on Linux/GCC fresh (−4.2%), and wins on MSVC in both runs and on Linux/GCC reused (+7.0% to +19.4%). The other four POD tests won on all three builds (+33.9% to +183.0%).
- **Every MSVC loss is Apache Builds.** MSVC lost 12 of its 330 single-document results: Apache Builds read in order, sparse and sparse in reverse at full size (11 results, −0.6% to −9.2%), and prettified Apache Builds Small read sparse (fresh, −30.1%; a win reused). Its 4 ties are prettified Apache Builds read sparse (fresh), Discord read sparse (minified fresh, prettified reused) and minified Canada read in reverse (reused). It won all 80 small sparse and sparse-reverse results except that one Apache Builds Small result, and all 80 reverse results but the one tie.
- **Streams:** Jsonifier won 16 of the 18 Amazon Cellphones and Stream Formats results, including Stream Formats Large on MSVC by +44.0% (NDJSON) and +50.0% (comma-separated); MSVC lost Amazon Cellphones (−17.9%) and Large Amazon Cellphones (−19.0%). MSVC went 25-0-5, also losing Apache Builds read in order (NDJSON −15.0%, comma −6.8%) and read in reverse as NDJSON (−4.2%); Linux/Clang went 27-1-2, losing CitmCatalog (−3.1%) and Twitter (−9.5%) read in order as NDJSON and tying Google Maps read in reverse as NDJSON; Linux/GCC went 27-1-2, losing Twitter read in order as NDJSON (−19.0%) and Google Maps read in reverse as NDJSON (−10.5%) and tying Twitter read in order comma-separated. Twitter read in reverse wins on all three builds in both forms (+99.1% to +213.8%).
- **Convergence on the cloud runners was close to §6's.** 5 of MSVC's 330 single-document results and 3 of its 30 streaming results did not converge, against 8 and 8 for the MSVC build of §6; Linux/Clang left 4 and 4 unconverged, and Linux/GCC 2 and 1.

## 10. Limitations

- **macOS/Clang holds the most single-document losses:** 18 of the 56, against 15 on macOS/GCC, 12 on Linux/GCC, 9 on Linux/Clang and 2 on MSVC. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's Double POD test (reused, −51.8%) and Linux/Clang's String POD test (−30.8% fresh, −34.8% reused), the largest x86 losses, then macOS/GCC's minified CitmCatalog read sparse (fresh, −17.5%).
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 34 of the 56: Canada 27 (10 on Linux/GCC, 8 on macOS/GCC, 5 on Linux/Clang, 4 on macOS/Clang) and Marine IK 7 (5 on macOS/GCC, 2 on MSVC). There much of the cost is float parsing, not field lookup. The other 22 are the POD tests (6 String, 1 Double), Instruments 5 (3 on macOS/Clang, 2 on Linux/Clang), CitmCatalog 5 (3 on macOS/Clang, 2 on macOS/GCC), and Apache Builds 2, Twitter 2 and Google Maps 1, all on macOS/Clang.
- **Linux/GCC loses mostly on Canada.** 10 of its 12 losses are Canada, read in order at both sizes and in reverse at small size, and the other 2 are String POD; its sparse and sparse-reverse reads all win.
- **Small documents carry many of the losses.** 24 of the 56 losses are small documents (8 on Linux/GCC, 6 on macOS/GCC, 5 each on Linux/Clang and macOS/Clang, none on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost weighs more (§7.11), and it is the next target.
- **Two streaming losses, both on MSVC:** Amazon Cellphones (−5.0%) and Large Amazon Cellphones (−9.3%).
- **Some results did not converge.** 25 of the 1,650 single-document results did not converge: 12 on macOS/GCC, 8 on Windows/MSVC and 5 on macOS/Clang. 20 of the 150 streaming results did not converge: 8 on Windows/MSVC, 7 on macOS/Clang and 5 on macOS/GCC. Linux/Clang and Linux/GCC converged on every single-document and streaming result.
- **The AVX-512 builds ran on shared cloud runners.** §9's three builds used an AMD EPYC 9V45 on shared virtual machines, an older GCC version than §6 and different operating-system images, so their results cannot be set directly against §7's to isolate the effect of AVX-512. 11 of their 990 single-document and 8 of their 90 streaming results did not converge. Linux/GCC lost 34 of its 330 AVX-512 single-document results, most of them on Google Maps and Twitter, against 12 for the Linux/GCC build of §7.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate, the key trail and the small documents were added, and MSVC was not swept. The wrap count of 2, the trail capacity of 16 and the direction score's limits (engage at 4, saturate at 8) were not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.

## 11. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces make up the design:

- a stage-1 reader that writes a lone scalar or string straight to the tape and classifies a short final remainder with narrower registers;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,576, tied 18 and lost 56. On reverse reads it won 389 of 400 at both sizes, and on Windows/MSVC it lost 2 of its 330 single-document results, both minified Marine IK read in order. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (787-8-30 fresh, 789-10-26 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 34 of the 56 losses), small documents in general, and the String POD test on Linux/Clang.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 146, tied 2 and lost 2, and it won all 120 corpus record-stream results. Both losses are the Amazon Cellphones streams under MSVC, the main remaining streaming target.

The lead holds against simdjson's own AVX-512 kernel. On three AVX-512 builds, Jsonifier won 918 of 990 single-document results, tied 10 and lost 62, won 237 of 240 reverse results and won 79 of 90 streaming results; a wider stage 1 does not shorten simdjson's rescans. On those builds the targets are Google Maps and Twitter under GCC, which hold 26 of the 62 losses, and Apache Builds under MSVC, which holds all 12 of that build's.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 12. Dedication

This paper is dedicated to the author's Little Buddy.

## References

1. simdjson. https://github.com/simdjson/simdjson
2. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
