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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,600 of 1,650 single-document results against simdjson On Demand, tied 12 and lost 38. On reverse-order reads it won 395 of 400. On Windows/MSVC it won 325 of 330 single-document results, tied 4 and lost 1.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 148 of 150 results and lost 2, and it won all 120 corpus record-stream results.

The whole suite was also run on three AVX-512 builds on an AMD EPYC 9V45, against simdjson's own AVX-512 kernel (§9). There Jsonifier won 927 of 990 single-document results, tied 17 and lost 46, won 238 of 240 reverse-order results and lost none, and won 76 of 90 streaming results.

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
- **Correctness:** on the measured commits, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, read directly from upstream simdjson's internal stage-1 parser after `stage1()`, with no fork or patch. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `a1328b5` and simdjson `4ba1cb3`, the benchmarked commits, on November 27, 2026 (the two macOS benchmark builds ran `fececf7`, which differs from `a1328b5` only by a trailing newline in one header). The tape checks ran on cloud-hosted CI runners, not on the benchmark hosts: their Linux builds report Azure kernels (`7.0.0-1020-azure`), the harness does not record their CPU model, and of their compilers only Linux/GCC differs from the benchmark builds' (GCC 16.0.1, against 16.2.1 below); MSVC 19.52, Clang 24.0 and the macOS GCC 16.2.0 and Clang 23.1.6 are the same. The AVX-512 builds could not run on the Core i9-14900KF, which has no AVX-512; their benchmarks ran on separate cloud hosts, described in §9. The benchmark runs of §7 and §8 did not use those runners: their Linux builds ran on the 14900KF under `7.0.0-43-generic`, the "Linux 7.0.0" of the build table below.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 12 of the 1,650 single-document results and 16 of the 150 streaming results did not converge (see §10). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead. On the same CPU, Jsonifier reads Int64 (fresh) at 570.0 MB/s under MSVC against 721.7 MB/s under Linux/Clang, and Uint64 at 534.1 against 709.3 MB/s; simdjson reads Int64 at 321.9 against 375.6 MB/s and Uint64 at 301.3 against 366.2 MB/s.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 167 / 170 | 155 / 160 | 322 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 167 / 170 | 160 / 160 | 327 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.6 | NEON | 169 / 170 | 160 / 160 | 329 / 330 |

Every build ran BenchmarkSuite `ff0f871` and simdjson `4ba1cb3`; the three x86 builds ran Jsonifier `a1328b5` and the two macOS builds `fececf7`, which differs from it only by a trailing newline in one header. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The five builds ran on November 27 and 28, 2026, and the results were published to `generic-parsing-main`.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 162-2-1 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 17-1-2 | 19-0-1 | 18-0-2 | 20-0-0 | 20-0-0 | 19-0-1 | 157-1-7 |
| Linux / GCC | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 18-0-2 | 20-0-0 | 20-0-0 | 159-0-6 |
| macOS / GCC | 5-0-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 18-2-0 | 20-0-0 | 18-1-1 | 19-1-0 | 158-4-3 |
| macOS / Clang | 4-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 163-1-1 |
| **All** | **22-0-3** | **94-3-3** | **100-0-0** | **96-1-3** | **99-0-1** | **94-2-4** | **98-0-2** | **98-1-1** | **98-1-1** | **799-8-18** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 163-2-0 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 18-0-2 | 19-0-1 | 17-0-3 | 19-0-1 | 20-0-0 | 20-0-0 | 157-0-8 |
| Linux / GCC | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 18-0-2 | 20-0-0 | 20-0-0 | 159-0-6 |
| macOS / GCC | 5-0-0 | 17-1-2 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 19-1-0 | 19-0-1 | 159-2-4 |
| macOS / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 163-0-2 |
| **All** | **22-0-3** | **94-3-3** | **100-0-0** | **97-0-3** | **99-0-1** | **95-0-5** | **97-0-3** | **99-1-0** | **98-0-2** | **801-4-20** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (12 of them did not converge; see §10), Jsonifier won 1,600, tied 12 and lost 38: 799-8-18 with fresh output objects and 801-4-20 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won all 200 full-size reverse results and 195 of 200 small reverse results (no ties, 5 losses). MSVC, macOS/GCC and macOS/Clang went 80-0-0, Linux/Clang 79-0-1 and Linux/GCC 76-0-4. Every loss is Canada Small: Linux/GCC in both forms and both runs (−3.0% to −5.2%) and prettified and reused on Linux/Clang (−1.9%).
- **Windows/MSVC lost 1 of its 330 single-document results, minified Marine IK read in order** (fresh, −3.3%). Its 4 ties are minified Canada read in order (fresh), minified Marine IK read in order (reused) and prettified Marine IK read in order in both runs. It won all 10 POD results, all 80 reverse results, all 160 sparse and sparse-reverse results and all 40 small in-order results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.7–15.1% for Jsonifier and 5.6–11.2% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.6 percentage points on macOS/Clang, +1.8 on Linux/GCC, +2.2 on Linux/Clang and +3.9 on macOS/GCC and MSVC. On MSVC, Discord Reverse (min) goes from +193.9% to +230.9%, and Apache Builds (min) from +9.3% to +16.1%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +102.6% / +101.6% | +32.8% / +30.5% | +68.1% / +80.4% | +115.2% / +124.5% | +42.1% / +34.9% |
| Double | +83.4% / +91.5% | +64.1% / +67.5% | +69.8% / +84.8% | +88.5% / +111.3% | +60.7% / +89.1% |
| Int64 | +77.1% / +95.8% | +92.1% / +99.0% | +124.2% / +140.7% | +115.8% / +139.0% | +71.1% / +95.9% |
| String | +1.4% / +9.7% | −26.4% / −30.4% | −7.4% / −4.9% | +24.2% / +26.7% | −16.1% / −13.0% |
| Uint64 | +77.3% / +92.7% | +93.7% / +107.9% | +135.2% / +155.7% | +134.9% / +147.3% | +140.6% / +116.0% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +9.3% / +16.1% | +14.0% / +20.9% | +23.3% / +35.9% | +38.6% / +64.8% | +24.6% / +23.6% (n/c) |
| Apache Builds (pretty) | +8.0% / +15.8% | +15.1% / +20.7% | +24.1% / +34.0% | +22.2% / +59.0% | tie / +19.3% |
| Canada (min) | tie / +1.5% | +2.8% / +2.5% | −3.2% / −3.7% | +1.1% / −2.6% | +7.0% / +8.6% |
| Canada (pretty) | +3.1% / +6.1% | +2.0% / +2.0% | +2.6% / +1.7% | +7.1% / +5.7% | +4.8% / +5.0% |
| CitmCatalog (min) | +17.9% / +15.5% | +11.1% / +14.7% | +23.4% / +25.5% | +9.5% / +11.7% | +13.4% / +14.0% |
| CitmCatalog (pretty) | +18.6% / +16.9% | +6.6% / +7.8% | +19.9% / +24.6% | +21.9% / +17.2% | +8.6% / +9.3% |
| Discord (min) | +14.2% / +20.8% | +13.3% / +25.6% | +27.2% / +36.5% | +63.1% / +38.0% | +25.3% / +27.2% |
| Discord (pretty) | +14.3% / +18.9% | +14.7% / +22.4% | +17.8% / +25.9% | +33.6% / +44.6% | +18.7% / +22.3% |
| Github Events (min) | +18.7% / +33.6% | +28.7% / +33.0% | +29.9% / +34.6% | +52.9% / +55.0% | +69.1% / +58.6% |
| Github Events (pretty) | +16.8% / +23.7% | +21.7% / +27.0% | +22.5% / +28.7% | +35.9% / +35.3% | +40.8% / +49.8% |
| Google Maps (min) | +23.0% / +22.4% | +22.8% / +26.8% | +34.8% / +33.9% | +31.3% / +26.4% | +11.3% / +11.8% |
| Google Maps (pretty) | +15.6% / +20.1% | +17.6% / +16.9% | +25.4% / +26.2% | +36.2% / +43.2% | +4.2% / +4.3% |
| Instruments (min) | +21.3% / +27.8% | +6.9% / +7.5% | +11.0% / +10.8% | +7.7% / +11.7% | +14.7% / +15.9% |
| Instruments (pretty) | +22.4% / +25.8% | +1.1% / +1.4% | +4.6% / +4.3% | +13.2% / +16.6% | +5.2% / +5.0% |
| Marine IK (min) | −3.3% / tie | +0.6% / +1.2% | +2.9% / +3.4% | −7.2% / −4.2% | +4.3% / +5.5% |
| Marine IK (pretty) | tie / tie (n/c) | +2.2% / +0.6% | +1.5% / +0.8% | +3.3% / tie | +3.5% / +4.9% |
| Random (min) | +18.5% / +23.2% | +22.1% / +24.9% | +36.3% / +42.5% | +56.5% / +78.1% | +35.9% / +41.4% |
| Random (pretty) | +18.5% / +23.2% | +20.7% / +24.4% | +31.4% / +40.1% | +45.3% / +62.3% | +30.1% / +32.9% |
| Twitter (min) | +22.5% / +26.9% | +2.1% / +15.4% | +7.1% / +12.9% | +33.6% / +40.2% | +14.2% / +17.3% |
| Twitter (pretty) | +18.7% / +23.0% | +3.8% / +13.8% | +1.8% / +2.5% | +20.1% / +23.8% | +9.2% / +12.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +34.2% / +53.8% | +62.5% / +82.5% | +64.0% / +88.1% | +69.9% / +98.2% | +77.3% / +102.3% |
| Apache Builds (pretty) | +31.6% / +44.7% | +57.7% / +75.3% | +64.8% / +84.8% | +71.8% / +105.6% | +72.2% / +58.4% |
| Canada (min) | +2.8% / +3.0% (n/c) | +4.0% / +4.3% | +0.4% / +2.1% | +4.0% / +2.5% | +9.9% / +10.9% |
| Canada (pretty) | +5.0% / +8.4% | +4.6% / +4.2% | +3.1% / +3.8% | +3.4% (n/c) / +7.7% (n/c) | +7.3% / +6.7% |
| CitmCatalog (min) | +48.4% / +57.4% | +68.8% / +74.0% | +73.7% / +78.7% | +89.1% / +88.9% | +79.3% / +83.2% |
| CitmCatalog (pretty) | +42.2% / +47.1% | +59.6% / +61.9% | +64.6% / +67.5% | +86.2% / +68.9% | +69.3% / +71.9% |
| Discord (min) | +193.9% / +230.9% | +256.3% / +281.7% | +260.7% / +289.7% | +286.4% / +313.4% | +345.9% / +370.1% |
| Discord (pretty) | +174.0% / +206.3% | +213.1% / +232.0% | +225.5% / +244.7% | +241.9% / +242.4% | +300.9% / +313.6% |
| Github Events (min) | +82.9% / +112.4% | +120.2% / +141.1% | +103.5% / +119.8% | +95.3% / +136.0% | +170.4% / +195.6% |
| Github Events (pretty) | +77.0% / +98.2% | +95.5% / +109.9% | +84.9% / +101.0% | +89.5% / +101.9% | +149.4% / +166.4% |
| Google Maps (min) | +10.5% / +12.2% | +14.5% / +12.6% | +15.9% / +16.4% | +36.3% / +33.9% | +21.7% / +21.6% |
| Google Maps (pretty) | +12.5% / +11.8% | +8.7% / +10.6% | +10.5% / +11.1% | +28.5% / +37.8% | +12.5% / +14.7% |
| Instruments (min) | +179.0% / +190.0% | +166.5% / +171.9% | +164.5% / +171.8% | +144.8% / +156.2% | +240.9% / +251.1% |
| Instruments (pretty) | +164.1% / +173.6% | +134.4% / +139.7% | +135.2% / +142.6% | +126.4% / +121.0% | +206.8% / +213.5% |
| Marine IK (min) | +28.3% (n/c) / +32.2% | +43.8% / +44.0% | +46.6% / +48.0% | +57.6% / +44.0% | +51.7% / +54.9% |
| Marine IK (pretty) | +38.0% / +40.1% | +47.3% / +48.9% | +48.0% / +51.8% | +47.9% (n/c) / +46.8% | +51.1% / +51.9% |
| Random (min) | +56.7% / +68.5% | +85.3% / +96.8% | +86.0% / +98.5% | +84.9% / +121.2% | +115.5% / +124.9% |
| Random (pretty) | +47.8% / +56.7% | +72.7% / +79.9% | +78.5% / +88.4% | +86.4% / +82.3% | +98.9% / +106.3% |
| Twitter (min) | +162.3% / +188.8% | +150.2% / +172.0% | +133.9% / +144.4% | +125.5% / +146.8% | +236.3% / +253.6% |
| Twitter (pretty) | +139.0% / +156.5% | +123.4% / +131.9% | +106.0% / +114.7% | +102.2% / +113.5% | +215.3% / +229.8% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.0% / +28.7% | +32.1% / +39.2% | +41.7% / +65.7% | +46.5% / +87.9% | +30.0% / +32.1% |
| Apache Builds (pretty) | +13.2% / +23.2% | +17.4% / +29.0% | +39.7% / +48.2% | +50.3% / +52.6% | +22.3% / +18.6% |
| Canada (min) | +20.6% / +18.7% | −1.1% / −2.3% | +5.1% / +4.7% | +31.5% / +26.2% | +37.2% / +33.8% |
| Canada (pretty) | +32.2% / +32.9% | +6.7% / +5.2% | +17.3% / +17.9% | +26.4% / +26.3% | +17.9% / +6.2% |
| CitmCatalog (min) | +14.2% / +13.9% | +10.2% / +9.9% | +11.7% / +11.6% | +9.4% / +5.5% | +5.9% / +11.4% |
| CitmCatalog (pretty) | +20.8% / +21.5% | +4.5% / +3.4% | +14.7% / +12.9% | +4.3% / +16.0% | +2.6% / +1.7% |
| Discord (min) | +6.4% / +9.4% | +7.1% / +8.6% | +16.3% / +21.9% | +17.8% / +29.9% | +24.8% / +28.0% |
| Discord (pretty) | +4.5% / +4.7% | +5.7% / +5.3% | +18.0% / +21.4% | +29.0% / +35.3% | +14.9% / +16.0% |
| Github Events (min) | +21.9% / +27.3% | +22.2% / +22.5% | +31.4% / +32.5% | +38.6% / +37.5% | +30.3% / +30.4% |
| Github Events (pretty) | +16.5% / +19.7% | +12.0% / +12.3% | +20.0% / +19.5% | +23.5% / +10.5% | +15.6% / +19.2% |
| Google Maps (min) | +20.7% / +21.4% | +16.1% / +16.2% | +34.6% / +31.6% | +18.5% / +20.5% | +8.9% / +6.9% |
| Google Maps (pretty) | +17.7% / +18.8% | +6.3% / +7.0% | +20.1% / +22.3% | +22.8% / +20.2% | +0.6% / +0.4% |
| Instruments (min) | +21.5% / +25.1% | +18.2% / +17.2% | +20.2% / +19.5% | +17.1% / +6.9% | +23.5% / +23.4% |
| Instruments (pretty) | +21.5% / +23.9% | +10.0% / +8.7% | +17.9% / +17.4% | +13.0% / +3.4% | +8.8% / +10.5% |
| Marine IK (min) | +8.0% / +7.4% | tie / +0.7% | +2.7% / +2.9% | −11.2% / −10.4% | +15.6% / +16.2% |
| Marine IK (pretty) | +19.6% / +20.1% | −3.8% / −3.4% | +6.3% / +12.5% | +18.3% / +9.8% | +8.6% / +9.0% |
| Random (min) | +23.4% / +28.0% | +21.4% / +26.3% | +33.2% / +39.9% | +49.4% / +59.8% | +40.2% / +43.4% |
| Random (pretty) | +27.3% / +30.3% | +13.4% / +13.7% | +22.4% / +27.3% | +35.5% / +45.0% | +28.2% / +31.1% |
| Twitter (min) | +59.0% / +67.5% | +15.1% / +15.2% | +21.3% / +21.6% | +17.2% / +16.6% | +26.9% / +29.1% |
| Twitter (pretty) | +53.7% / +60.5% | +7.8% / +8.3% | +15.4% / +15.9% | +15.6% / +16.5% | +16.4% / +17.4% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +15.0% / +21.0% | +17.6% / +20.7% | +23.8% / +29.0% | +42.4% / +62.3% | +23.2% / +29.6% |
| Apache Builds (pretty) | +8.1% / +13.6% | +10.4% / +14.4% | +17.5% / +23.4% | +34.9% / +44.6% | +19.5% / +22.9% |
| Canada (min) | +15.7% / +14.8% | −1.4% / −2.9% | +6.4% / +5.8% | +25.8% / +25.0% | +26.4% / +25.9% |
| Canada (pretty) | +25.0% / +24.1% | +4.2% / +6.2% | +13.3% / +15.2% | +13.8% / +11.3% | +14.7% / +15.9% |
| CitmCatalog (min) | +9.1% / +11.5% | +6.3% / +6.3% | +6.5% / +9.2% | +15.7% / +5.6% | +10.5% / +11.7% |
| CitmCatalog (pretty) | +17.3% / +17.0% | +1.3% / +1.1% | +7.8% / +6.2% | +16.0% / +9.6% | +4.4% / +4.5% |
| Discord (min) | +33.5% / +43.8% | +34.1% / +42.2% | +38.4% / +46.6% | +44.4% / +61.0% | +77.5% / +81.9% |
| Discord (pretty) | +32.2% / +40.7% | +23.0% / +28.6% | +39.0% / +47.2% | +43.5% / +44.1% | +61.1% / +68.1% |
| Github Events (min) | +36.9% / +45.2% | +39.7% / +42.7% | +48.7% / +50.5% | +54.0% / +50.9% | +60.0% / +61.6% |
| Github Events (pretty) | +33.7% / +40.4% | +32.8% / +34.7% | +40.4% / +36.6% | +47.8% / +43.9% | +45.0% / +43.2% |
| Google Maps (min) | +15.3% / +17.8% | +9.4% / +9.7% | +16.0% / +17.4% | +3.7% / +8.5% | +11.1% / +12.0% |
| Google Maps (pretty) | +22.6% / +23.4% | +1.5% / +4.3% | +7.7% / +8.6% | +7.5% / +11.4% | +4.2% / +5.8% |
| Instruments (min) | +28.7% / +34.0% | +35.2% / +37.9% | +26.6% / +27.3% | +24.2% / +34.4% | +44.8% / +48.0% |
| Instruments (pretty) | +31.6% / +35.3% | +25.1% / +25.2% | +31.5% / +31.2% | +26.3% / +14.7% | +28.8% / +30.7% |
| Marine IK (min) | +33.8% / +34.7% | +25.3% / +25.0% | +27.6% / +27.7% | +17.6% / +14.2% | +56.4% / +59.2% |
| Marine IK (pretty) | +37.3% / +36.9% | +17.1% / +16.2% | +28.0% / +25.9% | +14.9% / +17.9% | +32.4% / +32.3% |
| Random (min) | +18.8% / +25.5% | +25.6% / +30.2% | +37.5% / +43.3% | +54.9% / +51.5% | +54.7% / +56.5% |
| Random (pretty) | +22.6% / +30.2% | +20.8% / +23.0% | +34.0% / +34.8% | +38.1% / +40.2% | +42.3% / +43.5% |
| Twitter (min) | +50.3% / +54.3% | +30.5% / +32.2% | +28.1% / +29.0% | +31.5% / +24.6% | +38.6% / +40.8% |
| Twitter (pretty) | +48.1% / +52.0% | +20.5% / +20.0% | +24.8% / +23.7% | +6.8% / +20.6% | +27.8% / +28.1% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +13.1% / +25.4% | +16.8% / +26.9% | +29.8% / +39.5% | +46.6% / +51.4% | +23.4% / +25.1% |
| Apache Builds (pretty) | +10.5% / +18.2% | +17.7% / +25.2% | +21.0% / +32.2% | +31.7% / +46.7% | +15.7% / +20.1% |
| Canada (min) | +1.0% / +3.5% | +0.6% / −1.0% | −7.1% / −7.8% | +1.9% / +9.5% | +6.2% / +7.1% |
| Canada (pretty) | +0.8% / +2.5% | −3.0% / −4.6% | −6.4% / −6.7% | tie / +2.0% | +3.2% / +3.7% |
| CitmCatalog (min) | +9.2% / +8.7% | +3.2% / +7.0% | +11.7% / +14.4% | +15.5% / +15.4% | +10.7% / +13.8% |
| CitmCatalog (pretty) | +10.5% / +10.1% | +0.2% / +1.3% | +8.6% / +8.8% | +12.3% / +13.3% | +5.1% / +9.2% |
| Discord (min) | +10.0% / +24.3% | +6.1% / +14.9% | +17.3% / +34.0% | +81.9% / +61.8% | +22.6% / +32.3% |
| Discord (pretty) | +7.9% / +21.0% | +6.4% / +12.8% | +8.1% / +17.6% | +39.3% / +49.3% | +20.2% / +26.5% |
| Github Events (min) | +14.6% / +17.1% | +4.7% / +8.3% | +12.2% / +12.2% | +20.7% / +24.9% | +27.2% / +17.2% |
| Github Events (pretty) | +6.1% / +11.5% | +1.4% / +2.1% | +2.4% / +1.6% | +3.6% / +15.5% | +15.7% / +20.1% |
| Google Maps (min) | +17.9% / +19.4% | +22.0% / +23.7% | +30.2% / +31.3% | +31.6% / +14.8% | +15.9% / +16.8% |
| Google Maps (pretty) | +17.1% / +18.9% | +15.0% / +13.3% | +21.5% / +23.1% | +34.2% / +37.5% | +9.3% / +9.2% |
| Instruments (min) | +19.0% / +26.0% | +2.3% / +3.6% | +8.8% / +6.6% | +10.0% / +13.4% | +14.7% / +16.3% |
| Instruments (pretty) | +22.4% / +22.8% | −3.2% / −3.3% | +0.7% / +0.6% | +12.6% / +10.5% | +6.6% / +7.2% |
| Marine IK (min) | +5.3% / +2.5% | +4.8% / +8.7% | +5.5% / +7.0% | tie / +12.9% | +3.2% / +13.0% |
| Marine IK (pretty) | +3.5% / +6.9% | +5.1% / +9.5% | +1.5% / +1.1% | +7.3% / +7.5% | +7.6% / +10.4% |
| Random (min) | +15.8% / +20.7% | +16.7% / +21.3% | +33.3% / +39.7% | +64.2% / +59.0% | +50.1% / +34.3% |
| Random (pretty) | +12.8% / +18.0% | +18.0% / +20.9% | +30.1% / +35.4% | +48.8% / +57.5% | +29.8% / +31.5% |
| Twitter (min) | +24.8% / +26.7% | +7.6% / +14.2% | +15.0% / +22.3% | +35.9% / +40.9% | +19.6% / +22.5% |
| Twitter (pretty) | +21.0% / +24.8% | +9.3% / +13.0% | +8.8% / +13.1% | +17.7% / +26.8% | +12.3% / +17.9% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +49.5% / +82.3% | +81.8% / +97.4% | +90.0% / +106.8% | +94.3% / +124.9% | +89.0% / +118.0% |
| Apache Builds (pretty) | +46.1% / +72.5% | +73.1% / +81.7% | +85.5% / +100.2% | +74.5% / +103.6% | +77.0% / +101.1% |
| Canada (min) | +1.3% / +1.2% | +4.0% / +3.7% | −5.2% / −3.0% | +3.7% / +5.2% | +8.5% / +9.8% |
| Canada (pretty) | +0.3% / +1.5% | +0.3% / −1.9% | −3.3% / −3.8% | +7.4% / +5.1% | +5.7% / +5.5% |
| CitmCatalog (min) | +35.4% / +38.3% | +60.7% / +66.8% | +64.4% / +68.7% | +66.8% / +70.8% | +82.7% / +96.3% |
| CitmCatalog (pretty) | +33.4% / +41.4% | +48.8% / +53.1% | +54.0% / +57.0% | +51.9% / +67.1% | +70.8% / +78.6% |
| Discord (min) | +178.4% / +237.3% | +244.8% / +276.4% | +222.5% / +256.1% | +242.0% / +307.2% | +353.9% / +411.5% |
| Discord (pretty) | +165.1% / +223.1% | +204.5% / +236.9% | +208.5% / +239.1% | +214.7% / +243.7% | +320.7% / +365.4% |
| Github Events (min) | +25.5% / +31.4% | +37.6% / +44.1% | +31.9% / +36.4% | +41.7% / +63.5% | +71.2% / +80.6% |
| Github Events (pretty) | +10.1% / +17.8% | +23.0% / +26.5% | +21.8% / +24.0% | +34.0% / +36.1% | +53.1% / +38.6% |
| Google Maps (min) | +12.0% / +13.4% | +14.6% / +15.7% | +15.5% / +16.7% | +25.4% / +37.1% | +19.1% / +20.3% |
| Google Maps (pretty) | +12.5% (n/c) / +10.0% | +6.3% / +9.6% | +8.0% / +6.3% | +28.0% / +27.9% | +13.7% / +14.0% |
| Instruments (min) | +155.2% / +151.0% | +159.0% / +165.8% | +160.7% / +167.7% | +141.5% / +157.2% | +240.6% / +257.2% |
| Instruments (pretty) | +138.1% / +174.3% | +127.1% / +131.5% | +133.9% / +137.9% | +111.8% / +126.3% | +207.7% / +217.3% |
| Marine IK (min) | +25.4% / +32.5% | +39.5% / +41.0% | +43.5% / +43.6% | +43.8% / +49.9% | +53.8% / +60.7% |
| Marine IK (pretty) | +25.4% / +26.3% | +34.6% / +37.1% | +33.9% / +35.8% | +35.0% / +46.5% | +48.0% / +52.0% |
| Random (min) | +53.2% / +62.5% | +84.4% / +96.9% | +86.0% / +96.1% | +87.9% / +119.7% | +119.5% / +133.7% |
| Random (pretty) | +41.7% / +49.9% | +70.6% / +78.1% | +68.8% / +78.0% | +69.0% / +88.2% | +102.1% / +109.9% |
| Twitter (min) | +146.7% / +168.1% | +149.8% / +158.0% | +120.1% / +127.1% | +132.4% / +144.7% | +237.5% / +232.8% |
| Twitter (pretty) | +131.7% / +141.8% | +133.8% / +143.4% | +103.2% / +110.6% | +108.7% / +109.5% | +188.8% / +239.3% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +20.7% / +37.2% | +31.1% / +36.1% | +45.1% / +56.5% | +59.6% / +73.4% | +36.0% / +40.4% |
| Apache Builds (pretty) | +18.2% / +29.8% | +25.2% / +30.0% | +31.2% / +40.9% | +37.4% / +53.8% | +25.3% / +26.5% |
| Canada (min) | +39.2% / +42.0% | +5.7% / +0.9% | +21.3% / +20.3% | +18.7% / +20.7% | +11.4% / +11.1% |
| Canada (pretty) | +38.2% / +41.6% | +6.5% / +4.9% | +17.4% / +18.7% | +18.0% / +23.0% | +2.3% / +1.9% |
| CitmCatalog (min) | +33.5% / +37.3% | +15.3% / +21.8% | +22.2% / +21.5% | +14.0% / +6.7% | +18.5% / +19.7% |
| CitmCatalog (pretty) | +23.7% / +25.7% | +7.6% / +7.5% | +13.9% / +13.6% | tie / +18.3% | +11.0% / +11.7% |
| Discord (min) | +9.1% / +12.8% | +12.1% / +14.7% | +17.8% / +26.3% | +19.7% / +25.3% | +24.8% / +27.9% |
| Discord (pretty) | +10.7% / +14.7% | +8.6% / +7.9% | +15.3% / +13.5% | +24.4% / +18.6% | +16.4% / +17.7% |
| Github Events (min) | +19.6% / +31.5% | +23.7% / +20.5% | +27.4% / +28.3% | +28.1% / +36.9% | +19.7% / +18.7% |
| Github Events (pretty) | +8.6% / +17.5% | +11.0% / +12.5% | +15.6% / +16.2% | +17.8% / tie | +13.9% / +11.6% |
| Google Maps (min) | +24.3% / +15.9% | +11.3% / +14.0% | +32.8% / +29.3% | +24.2% / +21.1% | +11.4% / +10.8% |
| Google Maps (pretty) | +18.4% (n/c) / +21.1% | +4.4% / +5.4% | +17.4% / +13.0% | +22.1% / +19.1% | +6.3% / +5.7% |
| Instruments (min) | +13.5% / +22.9% | +13.0% / +14.7% | +12.7% / +13.5% | +7.8% / +8.1% | +23.3% / +21.5% |
| Instruments (pretty) | +13.3% / +20.3% | +6.4% / +8.0% | +6.7% / +7.0% | +12.3% / +8.2% | +10.9% / +10.1% |
| Marine IK (min) | +12.5% / +16.0% | +0.8% / +2.2% | +12.5% / +12.8% | −2.2% / +6.1% | +9.2% / +14.8% |
| Marine IK (pretty) | +15.3% / +16.7% | +22.7% / +23.2% | +12.8% / +12.3% | +14.7% / +19.3% | +8.4% / +8.5% |
| Random (min) | +13.5% / +16.6% | +22.5% / +24.5% | +30.3% / +36.6% | +39.2% / +43.6% | +26.2% / +33.0% |
| Random (pretty) | +15.5% / +15.1% | +13.8% / +15.4% | +19.8% / +23.4% | +34.5% / +42.7% | +17.4% / +25.7% |
| Twitter (min) | +33.2% / +36.1% | +13.5% / +8.1% | +20.2% / +16.9% | +9.5% / +13.2% | +19.5% / +19.4% |
| Twitter (pretty) | +32.3% / +34.9% | +4.9% / +6.4% | +10.7% / +10.4% | +6.1% / +11.7% | +9.6% / +8.9% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.6% / +24.6% | +16.1% / +16.9% | +23.8% / +25.5% | +44.8% / +58.4% | +20.2% / +19.7% |
| Apache Builds (pretty) | +10.5% / +17.5% | +12.0% / +12.9% | +17.3% / +19.2% | +31.0% / +39.9% | +26.8% / +12.0% |
| Canada (min) | +15.9% / +16.0% | +8.4% / +9.1% | +11.2% / +11.5% | +14.7% / +12.7% | +7.9% / +7.5% |
| Canada (pretty) | +28.1% / +25.3% (n/c) | +19.7% / +18.9% | +13.0% / +17.1% | +11.9% / +23.9% | +2.1% / +1.5% |
| CitmCatalog (min) | +17.7% / +19.8% | +8.6% / +8.8% | +9.6% / +10.1% | +7.7% / −10.4% | +19.0% / +17.3% |
| CitmCatalog (pretty) | +17.7% / +20.6% | +3.8% / +3.5% | +8.8% / +7.8% | +9.0% / +15.6% | +12.3% / −3.0% |
| Discord (min) | +37.2% / +42.2% | +44.0% / +50.2% | +41.8% / +49.8% | +37.2% / +52.1% | +73.5% / +81.6% |
| Discord (pretty) | +29.6% (n/c) / +41.2% | +33.2% / +39.4% | +34.3% / +40.4% | +39.2% / +48.5% | +63.1% / +66.5% |
| Github Events (min) | +27.3% / +29.9% | +31.1% / +33.9% | +36.3% / +37.5% | +38.4% / +38.0% | +41.5% / +43.4% |
| Github Events (pretty) | +17.1% / +24.0% | +22.9% / +27.8% | +23.2% / +20.9% | +27.5% / +16.4% | +53.6% / +33.3% |
| Google Maps (min) | +13.9% / +29.3% | +5.2% / +7.7% | +15.4% / +14.8% | +8.9% / +8.5% | +13.0% / +14.3% |
| Google Maps (pretty) | +11.0% (n/c) / +18.2% | −3.8% / +1.7% | +5.7% / +5.3% | tie / +12.8% | +8.9% / +8.8% |
| Instruments (min) | +21.4% / +24.6% | +30.5% / +33.5% | +28.1% / +27.4% | +28.8% / +27.8% | +45.6% / +48.1% |
| Instruments (pretty) | +26.7% / +43.7% | +21.5% / +21.4% | +22.1% / +17.8% | +12.1% / +21.8% | +32.9% / +18.3% |
| Marine IK (min) | +18.2% / +18.8% | +14.8% / +15.3% | +8.3% / +11.2% | +28.5% / +23.1% | +56.5% / +57.8% |
| Marine IK (pretty) | +24.2% / +22.9% | +18.2% / +23.3% | +15.4% / +17.7% | +23.4% / +29.6% | +37.4% / +38.3% |
| Random (min) | +19.8% / +18.0% | +28.4% / +30.3% | +42.2% / +43.2% | +38.6% / +50.1% | +43.6% / +58.2% |
| Random (pretty) | +15.9% / +16.3% | +21.9% / +22.1% | +31.0% / +28.5% | +27.8% / +23.6% | +37.5% / +40.2% |
| Twitter (min) | +27.5% / +31.3% | +15.2% / +19.1% | +24.0% / +23.5% | +19.2% / +49.3% | +28.1% / +28.0% |
| Twitter (pretty) | +24.1% / +26.0% | +16.2% / +17.3% | +14.5% / +16.6% | +10.5% / +12.1% | +19.5% / +20.7% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.0x to 4.7x faster at full size: +102.2% (Twitter pretty, macOS/GCC, fresh) up to +370.1% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +103.2% to +411.5%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size result runs from +0.4% (minified Canada on Linux/GCC, fresh) to +37.8% (prettified Google Maps on macOS/GCC, reused). That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** It won 159 of its 160 results and tied the other (prettified Github Events Small read sparse on macOS/GCC, reused). Read in reverse at full size it wins on every build (+77.0% to +195.6%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 198 of 200 full-size sparse-reverse results across both allocation modes (no ties, 2 losses), and 196 of 200 small ones (1 tie, 3 losses). MSVC and Linux/GCC won every result at both sizes. The full-size losses are minified Canada on Linux/Clang (−1.4% fresh, −2.9% reused). The small losses are prettified Google Maps Small on Linux/Clang (fresh, −3.8%), prettified CitmCatalog Small on macOS/Clang (reused, −3.0%) and minified CitmCatalog Small on macOS/GCC (reused, −10.4%); the tie is prettified Google Maps Small on macOS/GCC (fresh).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 249.4 MB/s on MSVC against 293.3 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 683.3 and 918.3 MB/s. Every MSVC full-size sparse result is a win, by +4.5% to +67.5%, and every full-size sparse-reverse result by +8.1% to +54.3%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 193 of 200 full-size sparse results (1 tie, 6 losses) and 197 of 200 small ones (2 ties, 1 loss). MSVC, Linux/GCC and macOS/Clang won all 40 of their full-size results. The full-size losses are minified Canada (−1.1% fresh, −2.3% reused) and prettified Marine IK (−3.8% fresh, −3.4% reused) on Linux/Clang and minified Marine IK on macOS/GCC (−11.2% fresh, −10.4% reused); the tie is minified Marine IK on Linux/Clang (fresh). The small loss is minified Marine IK Small on macOS/GCC (fresh, −2.2%), and the ties are prettified CitmCatalog Small (fresh) and Github Events Small (reused) on macOS/GCC. The largest loss is minified Marine IK on macOS/GCC (fresh, −11.2%).
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 188, tied 6 and lost 6. Linux/Clang went 40-0-0, macOS/Clang 39-1-0, Linux/GCC 38-0-2, macOS/GCC 36-1-3 and MSVC 35-4-1 across the two modes.
- **Small documents read in order hold the most losses of any category.** Across 200 results, Jsonifier won 189, tied 2 and lost 9. MSVC and macOS/Clang went 40-0-0, macOS/GCC 38-2-0, Linux/GCC 36-0-4 and Linux/Clang 35-0-5. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Canada Small holds 7 of the 9 losses: Linux/GCC in both forms and both runs (−6.4% to −7.8%) and Linux/Clang minified (reused, −1.0%) and prettified (−3.0% fresh, −4.6% reused). The other 2 are prettified Instruments Small on Linux/Clang (−3.2% fresh, −3.3% reused). The largest is minified Canada Small on Linux/GCC (reused, −7.8%). The ties are prettified Canada Small and minified Marine IK Small on macOS/GCC, both fresh.
- **In-order losses at full size fall on two documents.** Linux/GCC lost minified Canada in both runs (−3.2% and −3.7%), MSVC minified Marine IK (fresh, −3.3%), and macOS/GCC minified Canada (reused, −2.6%) and minified Marine IK in both runs (−7.2% and −4.2%). Linux/Clang and macOS/Clang lost none. Canada and Marine IK hold all 6; both are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 44-0-6 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+30.5% to +124.5%, +60.7% to +111.3%, +71.1% to +140.7% and +77.3% to +155.7%). String is the weakest: 4 wins (+1.4% to +26.7%) and 6 losses, on Linux/Clang (−26.4% fresh, −30.4% reused), Linux/GCC (−7.4% fresh, −4.9% reused) and macOS/Clang (−16.1% fresh, −13.0% reused).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is below its own NDJSON throughput on the same records in all 18 build and stream pairs, by 2.2% to 18.4% (4.6% to 18.4% on the two Linux builds).

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.52 | AVX2 | 18 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 27 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.6 | NEON | 29 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| **All** | **28-0-2** | **60-0-0** | **60-0-0** | **148-0-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | −4.1% | +18.1% | +26.6% | +23.0% | +21.1% |
| Large Amazon Cellphones (NDJSON) | −5.0% | +8.2% | +15.9% | +18.0% | +16.1% |
| Stream Formats Small (NDJSON) | +14.7% | +17.3% | +26.2% | +4.7% | +15.8% |
| Stream Formats Small (comma) | +60.3% | +77.1% | +115.8% | +81.8% | +76.1% |
| Stream Formats Large (NDJSON) | +22.5% | +18.0% | +15.7% | +16.8% | +11.9% |
| Stream Formats Large (comma) | +19.4% | +20.7% | +38.5% | +22.9% | +15.2% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +13.8% | +17.2% | +28.1% | +47.7% | +20.3% |
| Apache Builds (comma) | +21.5% | +26.6% | +47.8% | +60.5% | +34.2% |
| CitmCatalog (NDJSON) | +12.1% (n/c) | +10.2% | +25.6% | +14.1% | +14.1% |
| CitmCatalog (comma) | +24.6% | +38.1% | +54.1% | +43.9% (n/c) | +26.5% |
| Google Maps (NDJSON) | +30.2% (n/c) | +28.6% | +32.1% | +38.4% | +8.1% |
| Google Maps (comma) | +25.6% (n/c) | +35.1% | +52.4% | +55.1% | +14.8% |
| Instruments (NDJSON) | +35.1% | +26.6% | +16.6% | +8.8% | +21.9% |
| Instruments (comma) | +60.7% (n/c) | +35.9% | +63.2% | +48.4% | +38.9% |
| Random (NDJSON) | +19.8% | +24.7% | +29.6% | +63.6% | +49.8% |
| Random (comma) | +30.8% (n/c) | +29.5% | +53.3% | +64.6% (n/c) | +59.5% |
| Twitter (NDJSON) | +21.3% | +6.7% | +8.5% | +21.0% | +17.1% |
| Twitter (comma) | +30.1% | +18.6% | +30.2% | +19.2% | +23.3% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +10.0% (n/c) | +12.9% | +19.6% | +39.3% | +25.3% |
| Apache Builds (comma) | +21.1% | +24.6% | +41.3% | +57.6% | +37.9% |
| CitmCatalog (NDJSON) | +40.1% | +33.8% | +43.8% | +39.8% | +46.6% |
| CitmCatalog (comma) | +22.0% (n/c) | +50.7% | +63.3% | +64.1% | +64.1% |
| Google Maps (NDJSON) | +13.4% (n/c) | +7.7% | +11.6% | +24.8% | +11.8% |
| Google Maps (comma) | +11.9% (n/c) | +17.3% | +25.6% | +27.5% | +18.1% |
| Instruments (NDJSON) | +33.5% (n/c) | +22.9% | +23.6% | +45.5% | +55.2% |
| Instruments (comma) | +72.4% | +48.1% | +54.0% | +52.2% | +74.4% |
| Random (NDJSON) | +40.6% | +72.0% | +77.7% | +81.6% | +105.7% |
| Random (comma) | +47.4% (n/c) | +87.3% | +98.8% | +108.1% (n/c) | +119.4% |
| Twitter (NDJSON) | +189.0% | +159.5% | +143.8% | +119.7% | +250.6% (n/c) |
| Twitter (comma) | +194.2% (n/c) | +166.9% | +154.4% | +143.8% | +259.4% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier won 28 of the 30 results in that group and lost 2. It won Stream Formats Small comma-separated on all five builds (+60.3% to +115.8%) and as NDJSON on all five (+4.7% to +26.2%), and Stream Formats Large in both forms on all five (NDJSON +11.9% to +22.5%, comma +15.2% to +38.5%). Amazon Cellphones won on every build but MSVC (+18.1% to +26.6%), which lost it (−4.1%); Large Amazon Cellphones won on the same four (+8.2% to +18.0%) and lost on MSVC (−5.0%).
- **Four builds won every streaming test,** 30-0-0: Linux/Clang, Linux/GCC, macOS/GCC and macOS/Clang. MSVC went 28-0-2.
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +119.7% to +259.4%, while Google Maps rows, which have few, give +7.7% to +27.5%, and Instruments sits between them (+22.9% to +74.4%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 380.1 MB/s under MSVC and 475.0 MB/s under Linux/Clang, while Jsonifier reads it at 560.4 and 889.6 MB/s.

## 9. AVX-512 builds

Every result in §7 and §8 comes from AVX2 and NEON builds. This section repeats the whole suite, single documents and streams, on three builds that run Jsonifier's AVX-512 stage-1 kernel against simdjson's own AVX-512 kernel (`icelake`).

### 9.1 Method

- **Harness:** Json-Performance, branch `generic-parsing-avx512-main`, with the same tests, readers, allocation modes, batch size and adaptive-sampling stage as §6 and §8.3, and the same convergence limits as the x86 builds there (RSE < 5% and shift < 2.5%).
- **Hosts:** the builds ran on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, under Linux `7.0.0-1020-azure` and Windows 10.0.26100. These are shared virtual machines, not the dedicated Core i9-14900KF of §6, which has no AVX-512.
- **Compilers:** Clang 24.0 and MSVC 19.52 are the same as in §6. GCC 16.0.1 is the runner image's compiler and is older than §6's GCC 16.2.1.
- **Commits:** BenchmarkSuite `ff0f871`, Jsonifier `a1328b5` and simdjson `4ba1cb3`, the same as §6's x86 builds. All three builds ran on November 27, 2026. The stage-1 tapes of these builds match simdjson's position for position (§6, *Correctness*).

| Build | Stage-1 kernel | Converged, single documents | Converged, streams |
|---|---|---|---|
| Windows 10.0.26100 / MSVC 19.52 | AVX-512 | 321 / 330 | 15 / 30 |
| Linux 7.0.0 (Azure) / Clang 24.0 | AVX-512 | 330 / 330 | 28 / 30 |
| Linux 7.0.0 (Azure) / GCC 16.0.1 | AVX-512 | 330 / 330 | 29 / 30 |

The CPU, the operating-system images and the GCC version differ from §6, so a difference between an AVX-512 result here and the matching AVX2 result in §7 cannot be credited to the instruction set alone. Each comparison below is between the two libraries within one AVX-512 build.

### 9.2 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 15-3-2 | 19-1-0 | 19-0-1 | 18-0-2 | 17-1-2 | 20-0-0 | 20-0-0 | 20-0-0 | 153-5-7 |
| Linux / Clang | 4-0-1 | 17-1-2 | 20-0-0 | 20-0-0 | 19-1-0 | 16-0-4 | 20-0-0 | 20-0-0 | 20-0-0 | 156-2-7 |
| Linux / GCC | 5-0-0 | 17-0-3 | 20-0-0 | 19-1-0 | 17-1-2 | 14-2-4 | 20-0-0 | 19-0-1 | 19-0-1 | 150-4-11 |
| **All** | **14-0-1** | **49-4-7** | **59-1-0** | **58-1-1** | **54-2-4** | **47-3-10** | **60-0-0** | **59-0-1** | **59-0-1** | **459-11-25** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 20-0-0 | 18-1-1 | 18-0-2 | 19-0-1 | 19-1-0 | 20-0-0 | 19-0-1 | 155-3-7 |
| Linux / Clang | 4-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 19-1-0 | 160-2-3 |
| Linux / GCC | 5-0-0 | 17-1-2 | 20-0-0 | 19-0-1 | 18-0-2 | 16-0-4 | 20-0-0 | 19-0-1 | 19-0-1 | 153-1-11 |
| **All** | **14-0-1** | **53-3-4** | **60-0-0** | **57-1-2** | **56-0-4** | **53-0-7** | **59-1-0** | **59-0-1** | **57-1-2** | **468-6-21** |

**Streams:**

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 10-0-2 | 9-2-1 | 23-2-5 |
| Linux / Clang | 4-0-2 | 10-0-2 | 12-0-0 | 26-0-4 |
| Linux / GCC | 6-0-0 | 11-0-1 | 10-0-2 | 27-0-3 |
| **All** | **14-0-4** | **31-0-5** | **31-2-3** | **76-2-12** |

Each cell is wins-ties-losses.

Across the 990 single-document results (9 of them did not converge), Jsonifier won 927, tied 17 and lost 46: 459-11-25 with fresh output objects and 468-6-21 with reused ones. Across the 90 streaming results (18 of them did not converge) it won 76, tied 2 and lost 12.

### 9.3 POD tests

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Bool | +23.9% (n/c) / +102.1% | +39.3% / +40.9% | +70.9% / +65.9% |
| Double | +63.9% / +92.5% | +71.4% / +74.5% | +146.7% / +157.4% |
| Int64 | +77.4% / +106.2% | +103.8% / +126.6% | +167.0% / +184.7% |
| String | +11.2% / +19.5% | −15.1% / −16.2% | +6.2% / +1.2% |
| Uint64 | +63.0% / +79.1% | +107.1% / +128.1% | +168.0% / +177.5% |

### 9.4 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −2.3% / −6.9% | +4.8% / +8.3% | +5.8% / +8.0% |
| Apache Builds (pretty) | −1.8% / −5.6% | +6.0% / +9.1% | +5.7% / +6.8% |
| Canada (min) | tie (n/c) / +2.2% | +9.1% / +11.0% | +8.8% / +8.4% |
| Canada (pretty) | +3.8% / +2.7% | +7.2% / +7.6% | +7.1% / +9.9% |
| CitmCatalog (min) | +2.1% / +2.8% | tie / +0.8% | +8.5% / +6.2% |
| CitmCatalog (pretty) | +2.3% / +1.7% | +0.4% / tie | +2.3% / +0.5% |
| Discord (min) | +15.6% / +25.1% | +13.3% / +17.5% | +5.1% / +9.8% |
| Discord (pretty) | +14.2% / +22.9% | +11.3% / +16.3% | +3.1% / +7.2% |
| Github Events (min) | +24.9% / +32.8% | +30.8% / +45.6% | +20.7% / +24.6% |
| Github Events (pretty) | +19.2% / +23.5% | +28.2% / +34.4% | +12.5% / +16.4% |
| Google Maps (min) | +21.9% / +23.6% | +9.1% / +14.6% | −1.1% / −0.9% |
| Google Maps (pretty) | +13.8% / +12.3% | +8.3% / +8.1% | +1.2% / +3.2% |
| Instruments (min) | +17.4% / +19.7% | +10.1% / +12.2% | +13.3% / +14.7% |
| Instruments (pretty) | +22.7% / +19.1% | +7.9% / +6.8% | +8.1% / +6.9% |
| Marine IK (min) | tie / +4.1% (n/c) | +4.7% / +4.5% | +18.0% / +18.5% |
| Marine IK (pretty) | tie / tie (n/c) | +2.3% / +2.0% | +10.7% / +10.0% |
| Random (min) | +14.2% / +19.3% | +11.0% / +12.9% | +9.4% / +11.4% |
| Random (pretty) | +20.2% / +23.2% | +10.6% / +12.7% | +7.7% / +8.3% |
| Twitter (min) | +12.9% / +20.1% | −3.8% / +2.5% | −4.4% / tie |
| Twitter (pretty) | +17.4% / +20.1% | −4.6% / +8.8% | −4.6% / −5.1% |

### 9.5 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +29.7% / +50.6% | +77.3% / +97.4% | +66.0% / +80.4% |
| Apache Builds (pretty) | +36.4% / +51.8% | +76.1% / +94.3% | +60.3% / +71.3% |
| Canada (min) | +5.6% / +8.7% | +16.9% / +19.3% | +12.9% / +14.1% |
| Canada (pretty) | tie / +8.3% (n/c) | +13.8% / +14.4% | +11.5% / +13.7% |
| CitmCatalog (min) | +57.9% / +58.1% | +105.6% / +120.5% | +95.4% / +101.3% |
| CitmCatalog (pretty) | +61.9% / +65.5% | +98.8% / +109.9% | +85.9% / +91.1% |
| Discord (min) | +290.1% / +355.8% | +338.2% / +362.8% | +315.7% / +338.7% |
| Discord (pretty) | +288.2% / +338.7% | +320.5% / +345.3% | +280.3% / +298.6% |
| Github Events (min) | +91.4% / +116.6% | +107.9% / +126.9% | +76.1% / +86.5% |
| Github Events (pretty) | +77.2% / +96.6% | +96.4% / +116.3% | +64.3% / +69.8% |
| Google Maps (min) | +14.2% / +14.7% | +20.2% / +23.2% | +7.4% / +6.7% |
| Google Maps (pretty) | +17.2% / +15.5% | +20.4% / +22.2% | +3.2% / +3.6% |
| Instruments (min) | +208.0% / +219.9% | +203.2% / +209.4% | +160.7% / +157.6% |
| Instruments (pretty) | +192.0% / +204.2% | +171.8% / +178.4% | +130.1% / +132.7% |
| Marine IK (min) | +28.5% / +28.0% (n/c) | +87.3% / +93.3% | +89.2% / +91.4% |
| Marine IK (pretty) | +32.9% / +34.3% | +79.0% / +82.0% | +72.3% / +76.0% |
| Random (min) | +104.2% / +119.9% | +107.3% / +114.6% | +67.1% / +77.6% |
| Random (pretty) | +89.8% / +100.1% (n/c) | +97.7% / +108.6% | +59.5% / +66.6% |
| Twitter (min) | +182.5% / +207.8% | +177.7% / +185.1% | +116.5% / +119.2% |
| Twitter (pretty) | +180.8% / +193.1% | +165.9% / +158.9% | +106.3% / +111.8% |

### 9.6 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +2.0% / +3.1% | +20.7% / +26.1% | +18.4% / +22.5% |
| Apache Builds (pretty) | −4.6% / −4.9% | +21.2% / +21.6% | +14.3% / +15.2% |
| Canada (min) | +34.2% / +39.2% | +17.6% / +18.7% | +25.6% / +25.5% |
| Canada (pretty) | +34.3% / +43.8% | +17.6% / +18.6% | +13.8% / +13.5% |
| CitmCatalog (min) | +22.0% / +22.0% | +52.9% / +45.9% | +24.8% / +24.1% |
| CitmCatalog (pretty) | +22.3% / +17.0% | +36.5% / +37.9% | +19.5% / +23.4% |
| Discord (min) | +4.7% / +6.2% | +5.6% / +8.5% | +7.2% / +8.5% |
| Discord (pretty) | +5.6% / tie | +6.2% / +6.3% | +6.2% / +5.9% |
| Github Events (min) | +28.4% / +42.2% | +18.9% / +22.2% | +26.1% / +26.2% |
| Github Events (pretty) | +32.6% / +30.6% | +21.8% / +23.4% | +14.4% / +15.1% |
| Google Maps (min) | +36.3% / +38.5% | +8.0% / +5.9% | +15.8% / +16.2% |
| Google Maps (pretty) | +35.3% / +33.9% | +4.3% / +3.3% | tie / −1.1% |
| Instruments (min) | +35.4% / +42.2% | +22.3% / +25.3% | +15.8% / +21.0% |
| Instruments (pretty) | +29.6% / +37.1% | +12.7% / +6.8% | +12.3% / +9.3% |
| Marine IK (min) | +17.4% / +17.5% | +28.8% / +26.2% | +43.1% / +41.8% |
| Marine IK (pretty) | +27.9% (n/c) / +26.7% | +22.8% / +22.3% | +31.3% / +30.8% |
| Random (min) | +32.9% / +42.6% | +31.0% / +28.1% | +16.2% / +18.2% |
| Random (pretty) | +24.7% / +38.1% | +19.6% / +20.5% | +11.6% / +11.2% |
| Twitter (min) | +104.8% / +142.2% | +17.7% / +19.3% | +10.5% / +7.3% |
| Twitter (pretty) | +94.5% / +115.9% | +18.3% / +18.3% | +5.7% / +6.5% |

### 9.7 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | −4.8% / −3.0% | +18.1% / +22.3% | +13.5% / +14.3% |
| Apache Builds (pretty) | −2.7% / −1.3% | +13.7% / +17.3% | tie / +7.4% |
| Canada (min) | +16.4% / +17.6% | tie / +7.2% | +16.7% / +17.8% |
| Canada (pretty) | +34.7% / +36.1% | +6.3% / +5.7% | +24.2% / +19.6% |
| CitmCatalog (min) | +28.8% / +23.4% | +10.4% / +19.9% | +12.6% / +13.1% |
| CitmCatalog (pretty) | +30.9% / +22.6% | +13.7% / +15.9% | +5.1% / +3.2% |
| Discord (min) | +28.7% / +33.0% | +50.3% / +62.0% | +36.2% / +43.4% |
| Discord (pretty) | +29.4% / +42.6% | +46.0% / +54.1% | +29.6% / +38.1% |
| Github Events (min) | +59.2% / +95.3% | +46.7% / +50.6% | +54.2% / +56.5% |
| Github Events (pretty) | +60.5% / +78.0% | +56.8% / +61.6% | +49.3% / +49.7% |
| Google Maps (min) | +21.5% / +20.4% | +13.0% / +12.7% | −1.0% / −3.0% |
| Google Maps (pretty) | +22.2% / +20.4% | +9.2% / +8.6% | −2.7% / −4.0% |
| Instruments (min) | +56.7% / +48.8% | +62.4% / +66.9% | +61.5% / +63.0% |
| Instruments (pretty) | +51.9% / +57.2% | +48.6% / +51.7% | +51.2% / +48.4% |
| Marine IK (min) | +52.8% / +52.2% | +107.6% / +106.3% | +123.4% / +94.4% |
| Marine IK (pretty) | +46.0% / +49.8% | +70.6% / +70.7% | +65.4% / +72.5% |
| Random (min) | +50.5% / +68.7% | +45.6% / +36.4% | +23.2% / +30.2% |
| Random (pretty) | +58.8% / +68.1% | +35.8% / +45.1% | +21.6% / +23.3% |
| Twitter (min) | +118.3% / +118.1% | +42.9% / +49.9% | +17.9% / +19.1% |
| Twitter (pretty) | +98.3% / +109.1% | +41.3% / +39.4% | +15.3% / +6.4% |

### 9.8 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +5.5% / +6.5% | +19.6% / +23.7% | tie / +0.5% |
| Apache Builds (pretty) | +8.0% / +7.7% | +11.4% / +12.5% | +5.5% / +6.2% |
| Canada (min) | −2.8% / −0.9% | +9.4% / +10.9% | +7.2% / +8.9% |
| Canada (pretty) | +5.7% / +2.2% | +7.0% / +8.3% | +5.6% / +1.6% |
| CitmCatalog (min) | +4.4% / +3.7% | −6.4% / −5.5% | +4.7% / +7.6% |
| CitmCatalog (pretty) | −2.8% / +9.5% | −7.3% / −4.6% | −1.8% / −3.0% |
| Discord (min) | +9.8% / +32.7% | +7.6% / +16.1% | +0.3% / +11.5% |
| Discord (pretty) | +13.4% / +33.0% | +6.4% / +10.4% | +2.5% / +6.7% |
| Github Events (min) | +8.8% / +9.4% | +4.8% / +1.6% | −5.7% / −2.4% |
| Github Events (pretty) | +5.8% / +10.5% | −9.5% / +1.5% | −20.5% / −21.9% |
| Google Maps (min) | +14.2% / +17.2% | +13.0% / +14.8% | +3.4% / +3.1% |
| Google Maps (pretty) | +10.8% / +21.7% | +6.1% / +8.8% | +0.9% / +0.5% |
| Instruments (min) | +18.8% / +19.7% | +8.4% / +10.0% | +15.0% / +15.6% |
| Instruments (pretty) | +15.0% / +20.0% | +6.1% / +6.6% | +4.7% / +5.7% |
| Marine IK (min) | tie / +7.9% | +7.5% / +16.0% | +22.1% / +12.7% |
| Marine IK (pretty) | +4.0% / +3.6% | +9.1% / +9.0% | +12.4% / +7.3% |
| Random (min) | +17.7% / +18.3% | +15.9% / +16.3% | +10.9% / +11.1% |
| Random (pretty) | +17.7% / +20.8% | +9.8% / +11.8% | +4.1% / +12.0% |
| Twitter (min) | +14.3% / +28.4% | +1.1% / +4.9% | tie / +3.7% |
| Twitter (pretty) | +14.2% / +25.7% | −2.9% / +1.4% | −10.9% / −8.4% |

### 9.9 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +71.0% / +97.6% | +116.7% / +136.0% | +84.7% / +94.6% |
| Apache Builds (pretty) | +65.8% / +86.1% | +101.8% / +113.9% | +68.9% / +80.2% |
| Canada (min) | +3.4% / +3.9% | +13.2% / +16.3% | +11.8% / +13.9% |
| Canada (pretty) | +3.6% / tie | +7.6% / +7.4% | +9.1% / +7.5% |
| CitmCatalog (min) | +49.3% / +56.4% | +95.0% / +118.1% | +81.9% / +83.4% |
| CitmCatalog (pretty) | +51.4% / +56.7% | +87.1% / +101.3% | +66.5% / +73.2% |
| Discord (min) | +241.0% / +364.2% | +321.7% / +362.5% | +272.9% / +301.1% |
| Discord (pretty) | +248.8% / +336.5% | +294.6% / +332.3% | +238.3% / +261.0% |
| Github Events (min) | +38.7% / +44.7% | +47.9% / +56.8% | +18.3% / +22.0% |
| Github Events (pretty) | +29.4% / +30.8% | +27.4% / +30.1% | +4.5% / +4.5% |
| Google Maps (min) | +7.0% / +15.1% | +22.5% / +23.8% | +10.7% / +10.5% |
| Google Maps (pretty) | +14.9% / +19.8% | +21.9% / +22.9% | +4.5% / +5.6% |
| Instruments (min) | +204.1% / +215.8% | +194.4% / +204.2% | +152.4% / +161.9% |
| Instruments (pretty) | +193.9% / +202.6% | +168.3% / +161.9% | +125.5% / +132.7% |
| Marine IK (min) | +31.7% / +27.9% | +72.4% / +93.8% | +69.1% / +66.2% |
| Marine IK (pretty) | +30.1% / +33.8% | +69.9% / +75.3% | +55.5% / +57.9% |
| Random (min) | +93.5% / +110.7% | +106.6% / +115.8% | +70.0% / +70.6% |
| Random (pretty) | +78.1% / +99.6% | +97.5% / +115.5% | +57.4% / +57.7% |
| Twitter (min) | +200.6% / +219.9% | +163.1% / +175.5% | +112.8% / +114.3% |
| Twitter (pretty) | +198.9% / +196.2% | +158.8% / +161.3% | +109.0% / +96.7% |

### 9.10 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +33.1% / +35.0% | +35.1% / +33.8% | +23.9% / +23.1% |
| Apache Builds (pretty) | +21.4% / +29.2% | +26.0% / +26.1% | +11.8% / +12.2% |
| Canada (min) | +24.1% / +27.5% | +15.0% / +14.8% | +26.4% / +31.1% |
| Canada (pretty) | +36.3% / +36.6% | +17.6% / +16.5% | +6.7% / +8.6% |
| CitmCatalog (min) | +68.3% / +66.0% | +30.8% / +39.5% | +17.9% / +16.4% |
| CitmCatalog (pretty) | +48.8% / +58.1% | +25.1% / +29.3% | +11.6% / +16.8% |
| Discord (min) | +26.8% / +26.6% | +19.2% / +18.6% | +12.4% / +15.4% |
| Discord (pretty) | +28.2% / +25.2% | +17.5% / +15.7% | +7.4% / +11.6% |
| Github Events (min) | +30.9% / +43.0% | +25.0% / +28.7% | +15.7% / +18.7% |
| Github Events (pretty) | +37.7% / +33.7% | +22.4% / +20.8% | +4.9% / +6.7% |
| Google Maps (min) | +24.7% / +40.6% | +11.4% / +17.2% | +16.0% / +14.4% |
| Google Maps (pretty) | +30.9% / +36.9% | +9.3% / +9.1% | +1.2% / +0.7% |
| Instruments (min) | +47.2% / +54.3% | +25.2% / +27.6% | +12.9% / +14.9% |
| Instruments (pretty) | +41.5% / +46.5% | +16.5% / +15.8% | +11.1% / +15.0% |
| Marine IK (min) | +22.2% / +22.9% (n/c) | +18.5% / +21.8% | +23.4% / +26.8% |
| Marine IK (pretty) | +31.4% / +33.5% | +20.9% / +8.2% | +24.8% / +19.9% |
| Random (min) | +33.5% / +38.0% | +25.9% / +25.8% | +14.7% / +20.9% |
| Random (pretty) | +31.1% / +35.0% | +23.3% / +25.7% | +3.8% / +11.2% |
| Twitter (min) | +84.8% / +96.2% | +11.9% / +13.7% | +4.3% / +4.3% |
| Twitter (pretty) | +78.6% / +86.5% | +6.3% / +11.5% | −2.7% / −1.6% |

### 9.11 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (min) | +24.5% / +17.5% | +27.2% / +36.7% | +25.6% / +21.9% |
| Apache Builds (pretty) | +17.5% / +17.5% | +27.7% / +28.0% | +17.0% / +8.7% |
| Canada (min) | +20.4% / +17.7% | +34.9% / +4.3% | +7.8% / +23.9% |
| Canada (pretty) | +35.3% / +38.9% | +8.1% / tie | +11.5% / +12.9% |
| CitmCatalog (min) | +60.1% / +66.0% | +18.9% / +15.5% | +11.0% / +2.6% |
| CitmCatalog (pretty) | +50.9% / +56.7% | +18.0% / +19.7% | +12.7% / +2.4% |
| Discord (min) | +74.8% / +92.8% | +73.1% / +76.6% | +64.0% / +57.0% |
| Discord (pretty) | +69.1% / +85.7% | +68.3% / +71.7% | +45.9% / +43.3% |
| Github Events (min) | +56.1% / +78.8% | +46.8% / +52.1% | +31.9% / +36.9% |
| Github Events (pretty) | +54.4% / −12.1% | +51.4% / +48.1% | +34.0% / +33.7% |
| Google Maps (min) | +26.5% / +27.5% | +10.1% / +11.6% | +2.5% / +1.4% |
| Google Maps (pretty) | +23.7% / +20.8% | +6.9% / +8.2% | −0.8% / −2.0% |
| Instruments (min) | +64.0% / +68.7% | +69.7% / +77.5% | +53.2% / +56.4% |
| Instruments (pretty) | +62.4% / +71.6% | +49.4% / +53.9% | +44.0% / +49.8% |
| Marine IK (min) | +55.4% / +62.7% | +84.9% / +82.8% | +73.2% / +78.0% |
| Marine IK (pretty) | +53.2% / +49.6% | +60.4% / +63.4% | +54.6% / +61.6% |
| Random (min) | +64.5% / +70.9% | +40.1% / +41.5% | +29.6% / +31.4% |
| Random (pretty) | +61.8% / +63.0% | +41.5% / +39.0% | +28.1% / +25.6% |
| Twitter (min) | +83.2% / +95.4% | +35.3% / +41.0% | +13.4% / +11.3% |
| Twitter (pretty) | +73.4% / +78.1% | +22.3% / +27.9% | +5.0% / +5.6% |

### 9.12 Streams

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 4-0-2 | 10-0-2 | 9-2-1 | 23-2-5 |
| Linux / Clang | 4-0-2 | 10-0-2 | 12-0-0 | 26-0-4 |
| Linux / GCC | 6-0-0 | 11-0-1 | 10-0-2 | 27-0-3 |
| **All** | **14-0-4** | **31-0-5** | **31-2-3** | **76-2-12** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Amazon Cellphones (NDJSON) | −15.2% | +9.0% | +13.7% |
| Large Amazon Cellphones (NDJSON) | −14.9% | +10.5% | +0.6% |
| Stream Formats Small (NDJSON) | +24.0% | +9.9% | +7.6% |
| Stream Formats Small (comma) | +59.0% | +58.2% (n/c) | +84.6% |
| Stream Formats Large (NDJSON) | +21.4% | −2.1% | +12.0% |
| Stream Formats Large (comma) | +19.3% | −4.2% | +20.6% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | −7.0% (n/c) | +14.3% | +5.0% |
| Apache Builds (comma) | −4.8% | +14.4% | +16.4% |
| CitmCatalog (NDJSON) | +4.1% (n/c) | −1.3% | +6.6% |
| CitmCatalog (comma) | +8.2% | +13.2% | +22.9% |
| Google Maps (NDJSON) | +9.8% | +17.1% | +2.0% |
| Google Maps (comma) | +50.4% (n/c) | +8.4% | +16.3% |
| Instruments (NDJSON) | +52.7% | +21.4% | +9.5% |
| Instruments (comma) | +51.7% (n/c) | +28.5% | +36.6% |
| Random (NDJSON) | +14.9% (n/c) | +14.7% (n/c) | +7.1% |
| Random (comma) | +15.0% | +17.8% | +20.1% (n/c) |
| Twitter (NDJSON) | +18.1% | −6.0% | −12.5% |
| Twitter (comma) | +19.9% (n/c) | +1.8% | +2.1% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC |
|---|---|---|---|
| Apache Builds (NDJSON) | −4.9% (n/c) | +18.0% | +2.5% |
| Apache Builds (comma) | tie (n/c) | +24.9% | +13.1% |
| CitmCatalog (NDJSON) | +29.9% (n/c) | +65.1% | +36.9% |
| CitmCatalog (comma) | +52.3% (n/c) | +83.7% | +62.8% |
| Google Maps (NDJSON) | tie (n/c) | +16.5% | −5.4% |
| Google Maps (comma) | +12.8% (n/c) | +17.1% | −5.0% |
| Instruments (NDJSON) | +91.6% | +67.0% | +10.9% |
| Instruments (comma) | +93.7% | +74.9% | +33.8% |
| Random (NDJSON) | +91.7% (n/c) | +86.7% | +39.6% |
| Random (comma) | +87.9% | +107.8% | +75.3% |
| Twitter (NDJSON) | +184.3% (n/c) | +176.4% | +114.8% |
| Twitter (comma) | +190.7% (n/c) | +179.5% | +123.3% |

### 9.13 What the results show

- **Reverse order wins on every AVX-512 build.** Jsonifier won 238 of the 240 reverse results and lost none: Linux/Clang and Linux/GCC went 80-0-0 and MSVC 78-2-0. The ties are prettified Canada (fresh) and prettified Canada Small (reused), both on MSVC. On the documents whose objects have many fields (Discord, Twitter, Instruments) the full-size margin runs from +106.3% (Twitter pretty, Linux/GCC, fresh) to +362.8% (Discord min, Linux/Clang, reused), and the small-document margin from +96.7% to +364.2%.
- **simdjson's AVX-512 kernel does not close the reverse-order gap.** On MSVC, simdjson reads Discord Reverse (pretty, fresh) at 164.4 MB/s and Jsonifier at 638.3 MB/s; on Linux/Clang the figures are 236.0 and 992.3 MB/s. simdjson's rescans are bound by how many keys it walks, not by how fast stage 1 runs, so a wider stage-1 kernel cannot shorten them.
- **Sparse reverse reads stay a strength.** Jsonifier won 110 of the 120 full-size sparse-reverse results (2 ties, 8 losses) and 116 of the 120 small ones (1 tie, 3 losses). Linux/Clang lost none of its 80 sparse-reverse results. The full-size losses are Apache Builds on MSVC in both forms and both runs (−1.3% to −4.8%) and Google Maps on Linux/GCC in both forms and both runs (−1.0% to −4.0%); the ties are minified Canada on Linux/Clang and prettified Apache Builds on Linux/GCC, both fresh. The small losses are prettified Google Maps Small on Linux/GCC (−0.8% fresh, −2.0% reused) and prettified Github Events Small on MSVC (reused, −12.1%).
- **Linux/Clang loses mostly on in-order reads.** It went 316-4-10 and won all 80 of its reverse results. 8 of its 10 losses are in-order reads: 2 at full size (Twitter in both forms, fresh, −3.8% and −4.6%) and 6 small (CitmCatalog Small in both forms and both runs, −4.6% to −7.3%, prettified Github Events Small and prettified Twitter Small, both fresh, −9.5% and −2.9%). The other 2 are String POD.
- **Linux/GCC holds most of the AVX-512 losses:** 22 of the 46. 13 of them are in-order reads (5 at full size, 8 small), against 6 in-order losses for the GCC 16.2.1 build of §7 on the 14900KF. Google Maps accounts for 9 of the 22 and Twitter for 7, then Github Events Small with 4 and CitmCatalog Small with 2. Read in order at full size, Linux/GCC lost Twitter three times (−4.4% to −5.1%; minified reused is a tie) and minified Google Maps in both runs (−1.1% and −0.9%). The largest AVX-512 losses are its prettified Github Events Small read in order (−20.5% fresh, −21.9% reused). Twitter wins by wide margins when read in reverse on this build (+106.3% and more), so the gap is in reading in order, not in field lookup. Because this build differs from §7's in compiler version as well as CPU, these results alone do not say which of the two is responsible.
- **String POD loses on Linux/Clang in both runs** (−15.1% fresh, −16.2% reused) and wins on MSVC and Linux/GCC in both runs (+1.2% to +19.5%). The other four POD tests won on all three builds (+23.9% to +184.7%).
- **Most MSVC losses are Apache Builds.** MSVC lost 14 of its 330 single-document results. 10 are Apache Builds at full size: read in order (4, −1.8% to −6.9%), prettified and read sparse (2, −4.6% and −4.9%) and read sparse in reverse (4, −1.3% to −4.8%). The other 4 are small: minified Canada Small read in order (−2.8% fresh, −0.9% reused), prettified CitmCatalog Small read in order (fresh, −2.8%) and prettified Github Events Small read sparse in reverse (reused, −12.1%). Its 8 ties are Marine IK read in order (minified fresh, prettified in both runs), minified Canada read in order (fresh, n/c), minified Marine IK Small read in order (fresh), prettified Canada and Canada Small read in reverse, and prettified Discord read sparse (reused).
- **Streams:** Jsonifier won 14 of the 18 Amazon Cellphones and Stream Formats results, including Stream Formats Large on MSVC by +21.4% (NDJSON) and +19.3% (comma-separated); MSVC lost Amazon Cellphones (−15.2%) and Large Amazon Cellphones (−14.9%), and Linux/Clang lost Stream Formats Large in both forms (−2.1% NDJSON, −4.2% comma). MSVC went 23-2-5, also losing Apache Builds read in order (NDJSON −7.0%, comma −4.8%) and read in reverse as NDJSON (−4.9%), and tying Apache Builds read in reverse comma-separated and Google Maps read in reverse as NDJSON; Linux/Clang went 26-0-4, also losing CitmCatalog (−1.3%) and Twitter (−6.0%) read in order as NDJSON; Linux/GCC went 27-0-3, losing Twitter read in order as NDJSON (−12.5%) and Google Maps read in reverse in both forms (−5.4% NDJSON, −5.0% comma). Twitter read in reverse wins on all three builds in both forms (+114.8% to +190.7%).
- **Convergence on the cloud runners was close to §6's.** 9 of MSVC's 330 single-document results and 15 of its 30 streaming results did not converge, against 8 and 12 for the MSVC build of §6; Linux/Clang left 0 and 2 unconverged, and Linux/GCC 0 and 1.

## 10. Limitations

- **Linux/Clang holds the most single-document losses:** 15 of the 38, against 12 on Linux/GCC, 7 on macOS/GCC, 3 on macOS/Clang and 1 on MSVC. The largest single losses are Linux/Clang's String POD test (−26.4% fresh, −30.4% reused), the largest x86 losses, then macOS/Clang's String POD test (fresh, −16.1%) and macOS/GCC's minified Marine IK read sparse (fresh, −11.2%).
- **Float-heavy documents hold most of the losses.** Canada and Marine IK, the two documents dominated by float arrays, account for 27 of the 38: Canada 19 (10 on Linux/GCC, 8 on Linux/Clang, 1 on macOS/GCC) and Marine IK 8 (5 on macOS/GCC, 2 on Linux/Clang, 1 on MSVC). There much of the cost is float parsing, not field lookup. The other 11 are String POD 6, Instruments 2 (Linux/Clang), CitmCatalog 2 (macOS/GCC and macOS/Clang) and Google Maps 1 (Linux/Clang).
- **Linux/GCC loses mostly on Canada.** 10 of its 12 losses are Canada, read in order at both sizes and in reverse at small size, and the other 2 are String POD; its sparse and sparse-reverse reads all win.
- **Small documents carry many of the losses.** 18 of the 38 losses are small documents (8 on Linux/GCC, 7 on Linux/Clang, 2 on macOS/GCC, 1 on macOS/Clang, none on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost weighs more (§7.11), and it is the next target.
- **Two streaming losses, both on MSVC:** Amazon Cellphones (−4.1%) and Large Amazon Cellphones (−5.0%).
- **Some results did not converge.** 12 of the 1,650 single-document results did not converge: 8 on Windows/MSVC, 3 on macOS/GCC and 1 on macOS/Clang. 16 of the 150 streaming results did not converge: 12 on Windows/MSVC, 3 on macOS/GCC and 1 on macOS/Clang. Linux/Clang and Linux/GCC converged on every single-document and streaming result.
- **The AVX-512 builds ran on shared cloud runners.** §9's three builds used an AMD EPYC 9V45 on shared virtual machines, an older GCC version than §6 and different operating-system images, so their results cannot be set directly against §7's to isolate the effect of AVX-512. 9 of their 990 single-document and 18 of their 90 streaming results did not converge. Linux/GCC lost 22 of its 330 AVX-512 single-document results, most of them on Google Maps and Twitter, against 12 for the Linux/GCC build of §7.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,600, tied 12 and lost 38. On reverse reads it won 395 of 400 at both sizes, and on Windows/MSVC it lost 1 of its 330 single-document results, minified Marine IK read in order. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (799-8-18 fresh, 801-4-20 reused). The main remaining targets are the float-heavy documents (Canada and Marine IK, with 27 of the 38 losses), small documents in general, and the String POD test on Linux/Clang.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 148 and lost 2, and it won all 120 corpus record-stream results. Both losses are the Amazon Cellphones streams under MSVC, the main remaining streaming target.

The lead holds against simdjson's own AVX-512 kernel. On three AVX-512 builds, Jsonifier won 927 of 990 single-document results, tied 17 and lost 46, won 238 of 240 reverse results and lost none, and won 76 of 90 streaming results; a wider stage 1 does not shorten simdjson's rescans. On those builds the targets are in-order reads under GCC, where Google Maps, Twitter and Github Events hold 20 of its 22 losses, and Apache Builds under MSVC, which holds 10 of that build's 14.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.1–2.4x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 12. Dedication

This paper is dedicated to the author's Little Buddy.

## References

1. simdjson. https://github.com/simdjson/simdjson
2. Nihilai Collective Corp. "Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier." Nihilai Collective Corp, 2026.
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
