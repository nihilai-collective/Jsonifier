# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader tuned for small documents;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,512 of 1,650 single-document results against simdjson On Demand, tied 34 and lost 104. On reverse-order reads it won 375 of 400. On Windows/MSVC it won 318 of 330 single-document results, tied 8 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 141 of 150 results, tied 2 and lost 7, and it won 116 of the 120 corpus record-stream results, tied 2 and lost 2.

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

Small documents are routed to a dedicated POD reader. When the input is shorter than one SIMD step, the fixed cost of setting up the full-width reader outweighs the actual work, and that reader's step sizes are chosen to fit the input.

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

Non-ASCII bytes go through UTF-8 validation on the view path. The `std::string` getter is split differently: pure-ASCII strings take the fast path, and everything else goes through the one-pass unescape-and-copy scanner. This keeps validation work off the path that already has to copy the bytes.

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

The first version recorded every key the scanner skipped past, in every object. On Marine IK Reverse that made the parser **2.3x slower** than having no index at all, because Marine IK consists mostly of small objects with 3–6 fields each. For objects that small, re-scanning a few keys is cheaper than hashing, inserting and probing them.

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

*T* = 8 is the lowest value with no reverse-order losses on any build. It also matched or beat *T* = 16 on most reverse tests: on Linux/Clang it lifted CitmCatalog Reverse from +85% to +106% and Discord Reverse from +307% to +365%, and in the sweep runs it turned Linux/Clang's in-order record from 21-0-2 into 22-1-0. (The sweep runs are separate from the runs reported in §7.)

The threshold is therefore chosen per OS/compiler, the same way Jsonifier chooses its stage-1 step geometry:

| Build | *T* | Basis |
|---|---|---|
| Linux / GCC | 8 | swept |
| Linux / Clang | 8 | swept |
| macOS / GCC | 8 | swept |
| macOS / Clang | 16 | swept; 8 and 16 had no reverse losses and could not be separated on this host |
| Windows / MSVC | 8 | not swept; uses the value that won on the other builds |

On macOS/Clang, 16 kept larger margins on the documents whose objects have many fields (Twitter, Discord), while 8 won on Random, Google Maps and Instruments. Neither produced a reverse loss.

### 5.5 Requiring evidence of out-of-order access

A skipped-key count cannot tell a large object read out of order from a large object read sparsely in order. A sparse reader that asks for three fields of a 30-field object skips most of its keys on the way, so the count crosses *T* even though every lookup is found ahead of the cursor. Recording keys during those skips costs a hash and an insert per key for an index that no lookup needs.

The trigger in the measured code (§6) therefore asks for two kinds of evidence, both kept per object handle:

- **Size:** the skipped-key count has reached *T*, chosen per build as in §5.4. This is the gate that keeps small objects such as Marine IK's off the map.
- **Out-of-order access:** at least two lookups on this object have missed the expected field, scanned to the end of the object, and wrapped around to its start. A wrap is the event that only out-of-order reads produce. One wrap is not enough: a sparse reverse read of three fields often wraps once on its last lookup, whose key is first in the object, and an index built then would serve no further lookup.

Until both hold, lookups use scan-and-wrap and record nothing, so skips never pay for hashing. Once both hold, the next lookup that misses the expected field builds the object's index in one dedicated pass over the whole object, recording every key, and then probes it. Every later out-of-order lookup on that object is a single probe; a key the map does not hold falls through to the escape-aware retry of §3.4.

### 5.6 Small objects: a key trail and a direction score

The field index only pays for objects with at least *T* skipped keys, so objects below that size are still rescanned on every out-of-order lookup. Two cheaper mechanisms cover them:

- **A key trail per object.** The first lookup on an object scans forward from the cursor as in §3.4. When that scan misses, the object starts a trail instead of wrapping around: it walks its keys from the start, records each key's tape offset in a 16-entry buffer held in the handle, and compares as it goes. Later lookups compare against the recorded offsets first, without skipping any values, and extend the trail only when the key has not been seen yet. An object with more than 16 keys overflows the trail and falls back to scan-and-wrap and the field index above; the trail also handles escaped keys by decoding only the recorded keys that contain an escape.
- **A direction score per document.** The document keeps one saturating counter from 0 to 8. A lookup that lands before the point where it resumed adds one, and one that lands ahead of it subtracts one. At 4 or more, a resumed lookup searches the part of the object before the cursor first and only then scans forward, so a reader that consistently asks for keys in reverse no longer pays a failed forward scan per lookup. Reads in document order keep the score at 0 and never take this path.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`, November 5, 2026).
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 9 of the 1,650 single-document results and 24 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 167 / 170 | 158 / 160 | 325 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 170 / 170 | 157 / 160 | 327 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `ed43ce8`, Jsonifier `86c028c` and simdjson `37a1d727e`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The Linux/Clang build ran on November 4, 2026, the others on November 5, and the results were published to `generic-parsing-main` on November 5, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-3-1 | 19-1-0 | 20-0-0 | 20-0-0 | 19-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 158-5-2 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 17-0-3 | 20-0-0 | 19-0-1 | 17-1-2 | 155-1-9 |
| Linux / GCC | 4-0-1 | 16-2-2 | 17-1-2 | 17-1-2 | 18-1-1 | 13-0-7 | 17-0-3 | 20-0-0 | 19-0-1 | 141-5-19 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 18-1-1 | 20-0-0 | 18-1-1 | 18-0-2 | 18-1-1 | 18-2-0 | 153-6-6 |
| macOS / Clang | 3-0-2 | 16-1-3 | 18-1-1 | 18-0-2 | 20-0-0 | 17-0-3 | 18-0-2 | 17-0-3 | 18-0-2 | 145-2-18 |
| **All** | **21-0-4** | **85-7-8** | **94-3-3** | **93-2-5** | **97-1-2** | **84-1-15** | **92-1-7** | **94-1-5** | **92-3-5** | **752-19-54** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 19-1-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 160-3-2 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 16-0-4 | 20-0-0 | 19-1-0 | 16-0-4 | 153-1-11 |
| Linux / GCC | 4-0-1 | 17-1-2 | 18-0-2 | 17-0-3 | 19-0-1 | 13-0-7 | 17-0-3 | 20-0-0 | 19-0-1 | 144-1-20 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 19-1-0 | 18-2-0 | 18-2-0 | 158-6-1 |
| macOS / Clang | 4-0-1 | 18-1-1 | 17-1-2 | 18-1-1 | 20-0-0 | 17-1-2 | 19-0-1 | 14-0-6 | 18-0-2 | 145-4-16 |
| **All** | **22-0-3** | **89-5-6** | **94-2-4** | **95-1-4** | **98-0-2** | **85-1-14** | **95-1-4** | **91-3-6** | **91-2-7** | **760-15-50** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (9 of them did not converge; see §9), Jsonifier won 1,512, tied 34 and lost 104: 752-19-54 with fresh output objects and 760-15-50 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 188 of 200 full-size reverse results (5 ties, 7 losses) and 187 of 200 small reverse results (2 ties, 11 losses). Windows/MSVC and Linux/Clang lost none of their 80, and macOS/GCC lost two. Every loss is on Canada or Google Maps. The 7 full-size losses are minified Canada on Linux/GCC (−3.3% fresh, −2.3% reused), prettified Google Maps on Linux/GCC (−2.4% and −2.0%) and Google Maps on macOS/Clang (minified reused, −8.4%, and prettified in both runs, −11.8% and −18.0%). The 11 small losses are Canada Small on Linux/GCC (4, −5.7% to −7.4%), prettified Google Maps Small on Linux/GCC (2), Canada Small on macOS/GCC (fresh, both forms), and on macOS/Clang Canada Small and Google Maps Small (3, −10.3% to −24.1%).
- **Windows/MSVC lost four of its 330 single-document results,** all in-order reads of minified Canada Small or minified Marine IK: Canada Small (−1.0% in both runs) and Marine IK (fresh, −4.1%; reused, −3.2%). It won all 10 POD results and all 160 of its sparse and sparse-reverse results, and lost no reverse read.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.9–13.4% for Jsonifier and 6.5–9.3% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +0.9 to +2.5 percentage points on the non-MSVC builds and by +7.0 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +268.6% to +308.7%, and Apache Builds (pretty) from +19.7% to +33.8%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +163.9% / +157.2% | +96.9% / +99.3% | +125.8% / +120.1% | +77.3% / +52.9% | −3.7% / +28.6% |
| Double | +163.5% / +220.0% | +80.8% / +79.2% | +96.5% / +100.1% | +123.4% / +133.9% | +73.2% / +67.5% |
| Int64 | +170.8% / +181.8% | +95.0% / +96.8% | +63.7% / +66.4% | +90.0% / +90.6% | +32.9% / +44.5% |
| String | +41.2% / +50.6% | −4.0% / −8.7% | −0.5% / −1.3% | +16.1% / +35.7% | −27.2% / −36.8% |
| Uint64 | +165.6% / +180.2% | +95.2% / +99.0% | +97.5% / +99.9% | +82.8% / +81.6% | +50.6% / +53.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +18.9% / +33.1% | +18.9% / +36.6% | +23.0% / +33.5% | +37.2% / +52.7% | +27.4% / +27.2% |
| Apache Builds (pretty) | +19.7% / +33.8% | +19.1% / +24.4% | +24.1% / +35.3% | +30.5% / +45.3% | +17.2% / +34.0% |
| Canada (min) | tie / tie | +7.1% / +7.6% | −5.2% / −6.6% | +14.4% (n/c) / +1.9% | +6.0% / +4.3% |
| Canada (pretty) | tie / +2.1% | +5.5% / +6.5% | −4.5% / −4.9% | +17.2% / +7.4% | +7.4% / +19.9% |
| CitmCatalog (min) | +27.2% / +29.4% | +8.9% / +8.5% | +10.8% / +7.0% | +9.6% / +11.1% | +11.4% / +13.0% |
| CitmCatalog (pretty) | +30.7% / +33.9% | +4.7% / +8.4% | +5.1% / +2.0% | +9.9% / +11.7% | +3.1% / −40.8% |
| Discord (min) | +27.7% / +39.1% | +17.2% / +20.6% | +18.1% / +20.9% | +24.4% / +28.7% | +18.0% / +13.2% |
| Discord (pretty) | +34.4% / +42.5% | +12.4% / +16.6% | +14.7% / +16.4% | +22.6% / +26.6% | +15.3% / +16.2% |
| Github Events (min) | +54.6% / +76.9% | +30.8% / +37.5% | +18.8% / +22.6% | +39.3% / +52.4% | +38.9% / +43.2% |
| Github Events (pretty) | +47.4% / +72.8% | +25.7% / +29.8% | +13.0% / +17.8% | +30.3% / +36.1% | +36.9% / +34.8% |
| Google Maps (min) | +34.7% / +33.8% | +27.9% / +31.2% | +21.7% / +27.6% | +7.6% / +8.2% | +13.5% / +27.9% |
| Google Maps (pretty) | +36.4% / +38.2% | +13.9% / +13.9% | +19.5% / +19.8% | +32.0% / +24.0% | −6.3% / +17.1% |
| Instruments (min) | +44.5% / +51.6% | +5.7% / +4.8% | +5.4% / +5.2% | +20.6% / +18.4% | +16.6% / +26.0% |
| Instruments (pretty) | +51.5% / +59.8% | −1.5% / −1.7% | +3.8% / +4.1% | +8.5% / +10.5% | −3.9% / +5.5% |
| Marine IK (min) | −4.1% / −3.2% | +4.2% / +5.2% | tie / +1.3% | −18.4% / −2.8% | +7.3% / +8.0% |
| Marine IK (pretty) | tie / tie | +1.8% / +1.5% | tie / tie | tie / tie | tie / tie |
| Random (min) | +33.5% / +45.4% | +26.1% / +32.9% | +34.8% / +42.7% | +39.6% / +31.6% | +38.9% / +33.2% |
| Random (pretty) | +37.0% / +44.9% | +22.1% / +27.1% | +27.5% / +37.8% | +44.4% / +62.8% | +34.2% / +15.2% |
| Twitter (min) | +36.4% / +50.4% | +11.9% / +21.9% | +15.2% / +15.5% | +27.2% / +25.7% | −7.7% / +9.9% |
| Twitter (pretty) | +36.7% / +48.5% | +7.7% / +10.9% | +1.8% / +3.9% | +20.9% / +33.3% | +4.5% / +5.7% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +55.6% / +78.6% | +60.8% / +79.8% | +61.5% / +82.2% | +61.5% / +80.3% | +51.1% / +85.4% |
| Apache Builds (pretty) | +52.6% / +73.5% | +53.4% / +69.3% | +56.1% / +74.5% | +51.7% / +87.9% | +95.3% / +70.7% |
| Canada (min) | tie / tie | +8.6% / +9.2% | −3.3% / −2.3% | +6.1% / +4.5% | +12.7% / tie |
| Canada (pretty) | +5.3% / +5.3% | +9.3% / +10.1% | tie / +1.0% | +5.5% / +4.5% | +4.2% / +9.1% |
| CitmCatalog (min) | +85.8% / +87.8% | +62.6% / +67.5% | +67.6% / +72.0% | +43.7% / +68.8% | +89.1% / +76.0% |
| CitmCatalog (pretty) | +85.8% / +82.3% | +59.3% / +61.9% | +62.0% / +65.5% | +58.4% / +60.9% | +54.5% / +74.6% |
| Discord (min) | +268.6% / +308.7% | +230.8% / +256.1% | +217.8% / +240.8% | +249.6% / +273.5% | +297.0% / +311.3% |
| Discord (pretty) | +251.5% / +300.7% | +211.4% / +232.8% | +200.3% / +219.3% | +210.0% / +207.6% | +242.4% / +258.2% |
| Github Events (min) | +206.2% / +254.7% | +135.9% / +153.1% | +124.6% / +144.7% | +121.7% / +129.5% | +107.4% / +170.9% |
| Github Events (pretty) | +184.3% / +233.0% | +104.8% / +116.3% | +98.9% / +116.4% | +31.7% / +94.8% | +107.8% / +120.8% |
| Google Maps (min) | +38.6% / +42.5% | +5.8% / +6.7% | +3.4% / +3.9% | +32.9% / +25.0% | tie / −8.4% |
| Google Maps (pretty) | +44.0% / +50.4% | +2.0% / +1.9% | −2.4% / −2.0% | +25.5% / +23.3% | −11.8% / −18.0% |
| Instruments (min) | +303.9% / +328.7% | +158.6% / +165.2% | +138.2% / +142.2% | +161.7% / +143.1% | +202.0% / +145.6% |
| Instruments (pretty) | +261.9% / +283.8% | +133.8% / +136.2% | +111.3% / +112.2% | +91.0% / +101.1% | +108.7% / +100.8% |
| Marine IK (min) | +34.4% / +32.9% | +50.9% / +53.4% | +43.4% / +49.6% | +59.6% / +69.9% | +44.7% / +97.6% |
| Marine IK (pretty) | +42.4% / +35.5% | +45.9% / +52.0% | +55.1% (n/c) / +53.6% | +47.9% / +69.6% (n/c) | +65.1% / +36.5% |
| Random (min) | +121.1% / +148.4% | +82.8% / +95.3% | +60.0% / +69.9% | +48.5% / +64.8% | +117.8% / +137.7% |
| Random (pretty) | +109.6% / +135.5% | +65.5% / +73.8% | +52.9% / +60.8% | +90.1% / +101.3% | +75.9% / +65.9% |
| Twitter (min) | +328.0% / +381.8% | +142.6% / +156.9% | +131.0% / +140.9% | +116.1% / +128.0% | +161.9% / +172.7% |
| Twitter (pretty) | +305.2% / +347.9% | +121.8% / +130.0% | +101.7% / +113.3% | +85.1% / +120.2% | +147.0% / +156.4% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +36.2% / +54.1% | +36.7% / +51.3% | +46.2% / +54.6% | +51.4% / +69.8% | +31.8% / +34.3% |
| Apache Builds (pretty) | +32.6% / +48.3% | +25.6% / +37.1% | +35.5% / +43.7% | +37.6% / +50.3% | +41.1% / +15.7% |
| Canada (min) | +50.1% / +53.6% | +9.2% / +8.9% | tie / −0.3% | +18.5% / +18.0% | +41.6% / +47.4% |
| Canada (pretty) | +84.7% / +88.9% | +9.3% / +7.3% | +17.0% / +15.7% | +10.6% / +10.4% | +19.2% / +16.4% |
| CitmCatalog (min) | +38.3% / +43.8% | +13.5% / +12.4% | +10.1% / +10.0% | +15.5% / +15.3% | +1.8% / +6.5% |
| CitmCatalog (pretty) | +53.1% / +56.4% | +6.2% / +5.9% | +9.3% / +9.9% | +16.9% / +17.8% | +9.4% / +7.3% |
| Discord (min) | +16.5% / +22.3% | +4.6% / +7.9% | +11.5% / +14.5% | +27.6% / +34.4% | +16.2% / +19.1% |
| Discord (pretty) | +15.4% / +24.3% | +6.0% / +7.2% | +13.6% / +9.7% | +23.3% / +29.2% | +8.3% / +8.5% |
| Github Events (min) | +60.4% / +74.1% | +15.1% / +16.2% | +21.0% / +23.0% | +17.9% / +42.2% | +14.9% / +28.2% |
| Github Events (pretty) | +61.7% / +69.4% | +11.3% / +10.4% | +13.7% / +17.0% | +20.4% / +22.7% | +29.3% / +45.8% |
| Google Maps (min) | +43.1% / +42.9% | +15.6% / +18.5% | +19.9% / +18.9% | +22.1% / +27.0% | +6.3% / +1.5% |
| Google Maps (pretty) | +53.1% / +48.3% | +9.7% / +9.5% | +8.2% / +7.2% | +17.0% / +23.0% | +10.9% / +7.2% |
| Instruments (min) | +42.3% / +54.1% | +10.6% / +11.1% | +10.1% / +10.9% | tie / +22.7% | +22.9% / +17.6% |
| Instruments (pretty) | +51.2% / +58.7% | +6.2% / +5.9% | +8.1% / +8.1% | +8.8% / +6.3% | −25.6% / tie |
| Marine IK (min) | +35.9% / +32.6% | +6.8% / +6.2% | −3.6% / −3.2% | −6.1% / +10.3% | −5.7% / −6.9% |
| Marine IK (pretty) | +64.1% / +58.0% | +4.5% / +3.2% | −8.3% / −8.8% | +14.3% / +3.6% | +49.7% / +23.8% |
| Random (min) | +40.3% / +47.2% | +18.0% / +20.4% | +19.8% / +22.0% | +33.1% / +48.6% | +26.8% / +29.6% |
| Random (pretty) | +39.7% / +46.2% | +12.0% / +15.3% | +11.3% / +14.4% | +30.6% / +35.0% | +16.5% / +26.6% |
| Twitter (min) | +69.0% / +75.4% | +12.1% / +12.6% | +14.3% / +15.6% | +27.1% / +23.8% | +12.4% / +7.2% |
| Twitter (pretty) | +68.5% / +75.7% | +8.9% / +9.6% | +11.4% / +12.7% | +15.3% / +9.9% | +13.7% / +4.7% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +31.5% / +43.2% | +14.7% / +17.2% | +22.5% / +28.0% | +40.2% / +71.5% | +15.9% / +23.6% |
| Apache Builds (pretty) | +27.4% / +34.3% | +8.5% / +8.9% | +11.4% / +18.5% | +13.3% / +38.9% | +2.1% / +18.9% |
| Canada (min) | +39.7% / +38.6% | +6.4% / +6.6% | tie / −0.7% | +14.5% / +15.4% | +34.9% / +66.3% |
| Canada (pretty) | +69.3% / +70.9% | +5.4% / +4.4% | +12.8% / +12.4% | +18.5% / +8.5% | +22.6% / +11.7% |
| CitmCatalog (min) | +28.7% / +35.1% | +5.3% / +5.3% | +6.5% / +5.7% | +8.3% / +7.7% | +3.5% / +4.3% |
| CitmCatalog (pretty) | +42.5% / +42.2% | +2.0% / +1.3% | +5.6% / +5.4% | +10.9% / +10.1% | +5.5% / +6.4% |
| Discord (min) | +56.7% / +73.1% | +27.2% / +33.7% | +36.1% / +42.5% | +43.7% / +50.6% | +44.6% / +47.9% |
| Discord (pretty) | +61.1% / +78.2% | +18.5% / +23.6% | +31.3% / +37.1% | +34.9% / +23.9% | +30.8% / +32.9% |
| Github Events (min) | +88.9% / +108.3% | +33.4% / +34.7% | +31.8% / +36.5% | +47.4% / +50.7% | +27.1% / +54.8% |
| Github Events (pretty) | +82.5% / +93.4% | +26.6% / +23.0% | +25.7% / +29.3% | +68.6% / +45.3% | +30.5% / +36.1% |
| Google Maps (min) | +43.8% / +48.0% | +4.0% / +4.0% | +4.4% / +3.7% | +18.8% / +14.5% | +3.4% / +4.2% |
| Google Maps (pretty) | +43.3% / +46.3% | −0.4% / −1.5% | −1.6% / +0.3% | +15.5% / +13.8% | +10.3% / +16.0% |
| Instruments (min) | +75.0% / +87.1% | +22.5% / +23.9% | +19.4% / +19.8% | +26.2% / +41.4% | +13.4% / +10.8% |
| Instruments (pretty) | +63.1% / +70.5% | +16.9% / +14.4% | +13.3% / +13.1% | +37.5% / +15.8% | +16.9% / +14.0% |
| Marine IK (min) | +57.3% / +52.6% | +29.8% / +29.6% | +25.5% / +26.1% | +17.2% (n/c) / +11.7% | +50.7% / +36.1% |
| Marine IK (pretty) | +74.1% / +71.1% | +24.0% / +24.1% | +14.8% / +16.3% | +26.2% / +41.3% | +40.9% / +33.4% |
| Random (min) | +61.9% / +68.8% | +30.5% / +34.6% | +18.2% / +19.9% | +71.3% / +65.6% | +31.5% / +33.2% |
| Random (pretty) | +64.2% / +71.5% | +21.2% / +26.0% | +15.6% / +20.2% | +39.6% / +40.3% | +17.9% / +35.5% |
| Twitter (min) | +83.4% / +91.7% | +18.3% / +19.3% | +15.9% / +14.0% | +35.1% / +32.7% | +21.6% / +31.1% |
| Twitter (pretty) | +79.9% / +86.5% | +10.6% / +10.8% | +14.5% / +14.8% | +13.0% / +35.0% | +22.9% / +12.2% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +18.6% / +25.9% | +19.0% / +21.3% | +21.4% / +31.2% | +35.6% / +51.6% | +19.5% / +24.7% |
| Apache Builds (pretty) | +16.5% / +28.6% | +16.1% / +22.4% | +18.9% / +28.8% | +22.8% / +35.2% | +14.3% / +19.8% |
| Canada (min) | −1.0% / −1.0% | +5.1% / +4.5% | −5.2% / −9.5% | −5.6% / +2.8% | +7.3% / +6.9% |
| Canada (pretty) | +3.0% / +2.8% | +4.9% / +3.3% | −8.5% / −10.5% | +4.7% / +2.1% | +3.3% / +3.2% |
| CitmCatalog (min) | +12.8% / +19.0% | −3.8% / −1.5% | −2.8% / −1.5% | tie / +25.9% | +32.4% / +18.1% |
| CitmCatalog (pretty) | +17.9% / +22.7% | −5.8% / −4.0% | −0.6% / −2.4% | +12.9% / +9.5% | +13.1% / −11.6% |
| Discord (min) | +9.6% / +26.4% | +5.2% / +10.5% | +5.1% / +12.0% | +39.3% / +63.0% | +14.3% / +31.6% |
| Discord (pretty) | +11.2% / +28.9% | +4.6% / +6.6% | +3.9% / +11.2% | +3.4% / +30.2% | −1.6% / +6.0% (n/c) |
| Github Events (min) | +56.4% / +66.7% | +12.3% / +9.0% | +5.5% / +3.4% | +28.7% / +24.0% | +26.1% / +31.8% |
| Github Events (pretty) | +47.5% / +57.3% | +6.5% / +7.4% | −2.2% / −1.1% | +6.9% / +10.6% | +13.8% / tie |
| Google Maps (min) | +31.0% / +33.8% | +26.3% / +27.3% | +22.4% / +23.0% | +30.2% / +25.3% | +12.3% / +14.1% |
| Google Maps (pretty) | +38.9% / +38.0% | +14.5% / +12.3% | +18.9% / +19.9% | +17.5% / +52.6% | +12.9% / +11.8% |
| Instruments (min) | +42.9% / +46.6% | +0.9% / −1.8% | +1.5% / +2.4% | +9.7% / +14.1% | −8.6% / +14.6% |
| Instruments (pretty) | +48.2% / +51.7% | −5.6% / −6.1% | +1.1% / +1.5% | +21.0% / +13.3% | −2.6% / −12.5% |
| Marine IK (min) | +3.4% / +3.7% | +5.5% / +6.0% | −1.7% / −2.0% | +6.9% / +6.4% | +6.9% / +8.8% |
| Marine IK (pretty) | +10.7% / +9.4% | +1.2% / +1.4% | −2.8% / −2.8% | +23.6% / +11.7% | +4.3% / +4.6% |
| Random (min) | +26.7% / +40.2% | +20.3% / +26.0% | +23.9% / +32.1% | +45.1% / +57.1% | +46.2% / +19.5% |
| Random (pretty) | +30.9% / +43.0% | +16.0% / +18.8% | +24.1% / +30.2% | +42.0% / +52.2% | +31.7% / +25.5% (n/c) |
| Twitter (min) | +28.5% / +37.9% | +2.7% / +6.0% | +9.1% / +10.9% | +61.3% / +8.1% | +15.8% / +15.9% |
| Twitter (pretty) | +32.7% / +39.4% | +0.7% / +3.3% | +7.2% / +11.9% | +25.1% / +37.7% | +7.6% / +5.3% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +81.1% / +114.7% | +72.1% / +83.8% | +68.0% / +82.7% | +66.6% / +90.2% | +67.2% / +135.3% |
| Apache Builds (pretty) | +71.9% / +105.8% | +60.1% / +69.7% | +57.2% / +66.7% | +55.5% / +75.3% | +64.8% / +86.7% |
| Canada (min) | tie / +3.5% | +9.0% / +9.5% | −6.8% / −5.7% | −9.2% / tie | +7.9% / +9.0% |
| Canada (pretty) | +3.8% / +4.9% | +3.9% / +4.3% | −6.1% / −7.4% | −10.2% / +4.0% | −24.1% / +7.2% |
| CitmCatalog (min) | +79.0% / +85.2% | +54.5% / +60.8% | +45.6% / +49.7% | +69.7% / +52.1% | +48.1% / +95.9% |
| CitmCatalog (pretty) | +74.9% / +78.3% | +46.3% / +50.1% | +39.1% / +41.4% | +55.2% / +53.1% | +56.7% / +66.8% |
| Discord (min) | +289.8% / +377.3% | +209.8% / +244.0% | +172.1% / +198.7% | +207.0% / +239.4% | +218.2% / +253.9% |
| Discord (pretty) | +266.7% / +351.9% | +194.5% / +223.0% | +158.4% / +177.8% | +184.2% / +167.3% | +266.1% / +317.4% |
| Github Events (min) | +91.6% / +106.7% | +34.1% / +36.7% | +24.7% / +26.1% | +56.7% / +38.3% | +34.4% / +56.2% |
| Github Events (pretty) | +74.7% / +92.3% | +17.9% / +21.8% | +17.9% / +18.0% | +34.0% / +34.5% | +34.5% / +36.9% |
| Google Maps (min) | +35.7% / +37.4% | +6.4% / +7.4% | +3.1% / +3.2% | +34.8% / +37.0% | −10.3% / +9.5% |
| Google Maps (pretty) | +25.5% / +29.5% | +1.9% / +1.3% | −2.0% / −2.3% | +24.6% / +17.2% | +2.6% / −14.5% |
| Instruments (min) | +302.0% / +306.0% | +145.7% / +156.3% | +93.0% / +99.0% | +91.0% / +135.5% | +148.2% / +175.4% |
| Instruments (pretty) | +272.5% / +288.1% | +130.5% / +132.8% | +83.1% / +86.1% | +132.3% / +115.6% | +126.5% / +123.2% |
| Marine IK (min) | +47.4% / +49.5% | +47.8% / +52.7% | +33.6% / +36.5% | +35.9% / +37.3% | +47.9% / +51.7% |
| Marine IK (pretty) | +43.6% / +45.9% | +33.4% / +36.4% | +30.7% / +32.3% | +17.6% / +51.7% | +41.3% / +52.8% |
| Random (min) | +113.3% / +138.3% | +77.3% / +85.2% | +56.3% / +64.3% | +83.3% / +102.1% | +77.5% / +88.2% |
| Random (pretty) | +105.3% / +125.2% | +59.7% / +67.0% | +48.2% / +53.6% | +67.8% / +81.3% | +83.2% / +102.5% |
| Twitter (min) | +307.5% / +331.3% | +130.9% / +142.8% | +102.4% / +109.1% | +116.2% / +144.9% | +161.1% / +162.7% |
| Twitter (pretty) | +302.2% / +319.6% | +126.3% / +129.7% | +93.6% / +100.5% | +121.7% / +119.1% | +149.2% / +159.3% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +38.1% / +65.8% | +31.6% / +32.3% | +33.0% / +41.2% | +47.0% / +73.0% | +27.3% / +32.4% |
| Apache Builds (pretty) | +38.2% / +56.7% | +20.6% / +23.0% | +22.8% / +27.3% | +24.4% / +56.7% | +18.3% / +20.0% |
| Canada (min) | +54.2% / +58.3% | +17.8% / +7.3% | +10.5% / +11.0% | +31.6% / +10.5% | +6.1% / +6.0% |
| Canada (pretty) | +83.3% / +86.2% | +16.3% / +15.5% | +15.7% / +13.5% | +25.8% / tie | +4.4% / −4.1% |
| CitmCatalog (min) | +42.3% / +53.1% | +7.2% / +8.9% | +12.2% / +11.3% | +20.3% / +17.5% | +8.6% / +7.2% |
| CitmCatalog (pretty) | +51.3% / +56.8% | +5.3% / +3.5% | +11.3% / +9.9% | +6.1% / tie | +1.4% / +9.4% |
| Discord (min) | +29.9% / +41.0% | +7.7% / +7.9% | +10.0% / +10.4% | +20.4% / +52.3% | +43.1% / +10.2% |
| Discord (pretty) | +30.4% / +38.0% | +3.2% / +2.4% | +5.4% / +6.8% | +14.1% (n/c) / +12.7% | +2.0% / −1.8% |
| Github Events (min) | +52.2% / +61.0% | +4.8% / +4.8% | +8.2% / +14.6% | +37.2% / +23.0% | +31.3% / +28.5% |
| Github Events (pretty) | +48.4% / +63.0% | +4.1% / +2.9% | +6.6% / +6.3% | +17.7% / +5.6% (n/c) | +3.4% / +7.0% |
| Google Maps (min) | +39.7% / +41.5% | +13.8% / +10.9% | +12.3% / +13.0% | +21.4% / +18.9% | +7.7% / +5.5% |
| Google Maps (pretty) | +47.8% / +51.9% | +6.2% / +4.6% | +5.6% / +6.5% | +28.0% / +43.9% | +5.5% / +5.3% |
| Instruments (min) | +41.6% / +49.9% | +4.5% / +4.9% | +4.5% / +5.6% | +24.7% / +12.4% | +6.7% / +20.2% |
| Instruments (pretty) | +43.6% / +52.2% | −0.2% / +0.8% | +1.7% / +2.2% | −15.2% / +8.4% | −10.6% / −20.1% |
| Marine IK (min) | +30.2% / +32.5% | +0.7% / +0.5% | +8.9% / +8.9% | +7.7% / +8.0% | −8.8% / −12.7% |
| Marine IK (pretty) | +62.4% / +65.6% | +2.6% / +4.4% | +11.0% / +10.5% | +7.1% / +7.6% | +3.5% / +1.6% |
| Random (min) | +31.5% / +37.7% | +14.4% / +14.3% | +15.0% / +14.0% | +29.1% / +41.0% | +5.8% / +51.1% (n/c) |
| Random (pretty) | +30.5% / +34.3% | +9.1% / +9.0% | +13.5% / +13.4% | +23.2% / +25.2% | +7.1% / −2.0% |
| Twitter (min) | +23.3% / +25.5% | +5.2% / +2.0% | +4.2% / +2.9% | tie / +7.3% | +6.4% / +3.8% |
| Twitter (pretty) | +22.5% / +25.2% | +1.1% / tie | +6.9% / +1.3% | +27.5% / +6.0% | −1.6% / −3.8% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +34.7% / +53.2% | +9.2% / +11.4% | +15.8% / +17.0% | +34.1% / +45.1% | +15.5% / +22.1% |
| Apache Builds (pretty) | +31.0% / +42.0% | +1.8% / −0.4% | +7.2% / +7.7% | +30.8% / +49.1% | +6.2% / +6.2% |
| Canada (min) | +40.9% / +41.3% | +14.3% / +11.4% | +4.3% / +4.8% | +13.1% / +19.1% | −15.3% / +11.7% |
| Canada (pretty) | +63.4% / +68.7% | +3.6% / +2.4% | +13.9% / +10.7% | +14.3% / +25.0% | −10.3% / −11.0% |
| CitmCatalog (min) | +50.7% / +45.5% | tie / −1.2% | +1.7% / +2.7% | +27.1% / +16.9% | +10.4% / +3.7% |
| CitmCatalog (pretty) | +48.1% / +48.1% | −4.8% / −5.2% | +4.2% / +2.1% | +27.4% / tie | +33.3% / +7.8% |
| Discord (min) | +77.1% / +100.8% | +27.4% / +33.3% | +27.5% / +33.1% | +36.7% / +34.6% | +78.5% / +60.3% |
| Discord (pretty) | +70.5% / +87.5% | +19.1% / +21.5% | +21.4% / +25.9% | +35.3% / +48.0% | +25.0% / +17.8% |
| Github Events (min) | +72.4% / +87.9% | +12.8% / +17.8% | +17.8% / +20.5% | +21.6% / +49.1% | +18.1% / +15.2% |
| Github Events (pretty) | +62.8% / +83.5% | +12.6% / +14.7% | +13.4% / +15.3% | +38.4% / tie | +10.3% / +7.8% |
| Google Maps (min) | +42.6% / +47.6% | +2.5% / +2.2% | +2.9% / +2.9% | tie / +19.7% | +4.0% / +6.5% |
| Google Maps (pretty) | +42.1% / +47.0% | −3.3% / −1.2% | −2.6% / −2.7% | +31.4% / +15.0% | +12.2% / −2.0% |
| Instruments (min) | +71.0% / +80.1% | +20.0% / +22.5% | +19.5% / +21.5% | +20.7% / +55.8% | +26.2% / +27.0% |
| Instruments (pretty) | +67.4% / +73.9% | +10.8% / +12.8% | +12.7% / +13.5% | tie / +24.2% | +12.1% / +7.3% |
| Marine IK (min) | +56.5% / +59.4% | +3.2% / +2.2% | +36.2% / +31.8% | +34.0% / +35.3% | +19.7% / +32.7% |
| Marine IK (pretty) | +62.1% / +65.7% | +22.1% / +21.9% | +28.4% / +29.0% | +10.2% / +27.9% | +18.3% / +15.0% |
| Random (min) | +56.9% / +63.9% | +27.3% / +24.7% | +12.1% / +12.6% | +14.0% / +41.1% | +48.0% / +32.1% |
| Random (pretty) | +53.7% / +66.1% | +19.6% / +18.5% | +10.5% / +12.3% | +26.0% / +28.1% | +9.3% / +7.2% |
| Twitter (min) | +45.0% / +49.3% | +10.1% / +11.9% | +14.7% / +14.0% | +15.8% / +29.6% | +13.0% / +13.1% |
| Twitter (pretty) | +43.2% / +44.5% | +9.5% / +7.3% | +10.9% / +11.7% | +22.9% / +8.0% | +5.9% / +3.3% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 4.8x faster at full size: +85.1% (Twitter pretty, macOS/GCC, fresh) up to +381.8% (Twitter min, MSVC, reused), and up to +311.3% outside MSVC (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +83.1% to +377.3%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −18.0% (prettified Google Maps on macOS/Clang, reused) to +32.9%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events rarely loses.** It accounts for 2 of the 104 single-document losses, both prettified Github Events Small read in order on Linux/GCC (−2.2% and −1.1%). Read in reverse at full size it wins on every build (+31.7% to +254.7%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 195 of 200 full-size sparse-reverse results across both allocation modes (1 tie, 4 losses), and 183 of 200 small ones (5 ties, 12 losses). MSVC won all 80. The full-size losses are prettified Google Maps on Linux/Clang (−0.4% and −1.5%) and Linux/GCC (fresh, −1.6%), and minified Canada on Linux/GCC (reused, −0.7%). 11 of the 12 small losses are on Canada, CitmCatalog or Google Maps; the other is prettified Apache Builds Small on Linux/Clang (reused, −0.4%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 189.0 MB/s on MSVC against 293.5 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 664.4 and 913.9 MB/s. Every MSVC full-size sparse result is a win, by +15.4% to +88.9%, and every full-size sparse-reverse result by +27.4% to +108.3%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 188 of 200 full-size sparse results (3 ties, 9 losses) and 185 of 200 small ones (4 ties, 11 losses). MSVC and Linux/Clang won all 40 of their full-size results, and MSVC and Linux/GCC won all 40 small. The full-size losses are on Linux/GCC (5: minified Canada, reused, −0.3%, and four Marine IK, −3.2% to −8.8%), macOS/Clang (3: prettified Instruments, fresh, −25.6%, and minified Marine IK in both runs) and macOS/GCC (1: minified Marine IK, fresh, −6.1%). macOS/Clang holds 9 of the 11 small losses, Linux/Clang 1 and macOS/GCC 1.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 174, tied 12 and lost 14. Linux/Clang went 38-0-2, macOS/GCC 36-2-2, macOS/Clang 34-2-4, MSVC 33-5-2 and Linux/GCC 33-3-4 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 169, tied 2 and lost 29. MSVC went 38-0-2, macOS/GCC 38-1-1, macOS/Clang 34-1-5, Linux/Clang 33-0-7 and Linux/GCC 26-0-14. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −1.5% to −6.1%, led by CitmCatalog and Instruments, and Linux/GCC's from −0.6% to −10.5%, led by Canada, CitmCatalog and Marine IK. The largest is prettified Instruments Small on macOS/Clang (reused, −12.5%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−4.5% to −6.6%). MSVC lost minified Marine IK in both runs (−4.1% and −3.2%), and macOS/GCC lost the same test (fresh, −18.4%; reused, −2.8%). Linux/Clang lost only prettified Instruments (−1.5% and −1.7%). macOS/Clang's four losses are prettified CitmCatalog (reused, −40.8%), prettified Google Maps (fresh, −6.3%), prettified Instruments (fresh, −3.9%) and minified Twitter (fresh, −7.7%). Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 43-0-7 on them. Double, Int64 and Uint64 won all 10 results each (+67.5% to +220.0%, +32.9% to +181.8% and +50.6% to +180.2%), and Bool won 9 (+28.6% to +163.9%), losing only on macOS/Clang (fresh, −3.7%). String is the exception: 4 wins and 6 losses, on Linux/Clang (−4.0% and −8.7%), Linux/GCC (−0.5% and −1.3%) and macOS/Clang (−27.2% and −36.8%), each in both runs.

## 8. Streams: many documents in one buffer

Many real inputs are not one document but a sequence of them: NDJSON logs, JSON Lines exports, or documents joined by commas. simdjson handles these with `iterate_many`, which runs stage 1 over a window of the input and hands out one On Demand document at a time. `jsonifier::generic` does the same through `parser::iterateMany`, and every document it hands out is an ordinary `generic::document`, with re-readable values and the per-object field index from §5.

### 8.1 Design

```cpp
for (jsonifier::generic::document doc : parser.iterateMany(buffer)) { ... }
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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 2.8% to 13.1% below its own NDJSON throughput on the same records, in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 29 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 16 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 21 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / GCC | 5-0-1 | 11-1-0 | 12-0-0 | 28-1-1 |
| macOS / GCC | 6-0-0 | 11-0-1 | 12-0-0 | 29-0-1 |
| macOS / Clang | 4-0-2 | 11-0-1 | 11-1-0 | 26-1-3 |
| **All** | **25-0-5** | **57-1-2** | **59-1-0** | **141-2-7** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +31.5% | +20.4% | +15.4% | +13.1% | +16.3% |
| Large Amazon Cellphones (NDJSON) | +16.0% | +9.1% | +3.0% | +11.7% | +27.7% |
| Stream Formats Small (NDJSON) | +62.6% | −5.5% | −1.7% | +15.1% | +12.9% (n/c) |
| Stream Formats Small (comma) | +112.9% | +80.9% | +101.8% | +94.3% (n/c) | +52.8% (n/c) |
| Stream Formats Large (NDJSON) | +99.8% | −13.7% | +19.0% | +23.8% | −15.0% |
| Stream Formats Large (comma) | +104.0% | +2.2% | +28.7% | +14.7% | −6.4% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +34.1% | +25.2% | +15.9% | +37.7% | +18.9% |
| Apache Builds (comma) | +43.4% | +36.8% | +23.0% | +50.0% | +50.0% |
| CitmCatalog (NDJSON) | +41.5% | +7.5% | +8.8% | +6.4% | +7.2% |
| CitmCatalog (comma) | +53.0% | +28.0% | +32.1% | +27.7% | +34.5% (n/c) |
| Google Maps (NDJSON) | +43.6% | +30.0% | +25.9% | +40.1% | +7.6% |
| Google Maps (comma) | +49.8% | +45.6% | +44.4% | +32.4% (n/c) | +18.0% (n/c) |
| Instruments (NDJSON) | +77.3% | +6.1% | +15.6% | +11.8% | −8.4% |
| Instruments (comma) | +83.7% | +26.2% | +54.5% | +70.6% (n/c) | +26.9% |
| Random (NDJSON) | +44.7% | +22.7% | +29.3% | +33.7% (n/c) | +54.4% |
| Random (comma) | +46.5% | +33.5% | +50.9% | +53.5% | +58.4% |
| Twitter (NDJSON) | +33.3% | +10.4% | tie | −6.6% (n/c) | +7.2% |
| Twitter (comma) | +47.1% | +23.1% | +18.8% | +8.2% | +18.1% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +44.3% | +17.6% | +1.9% | +19.8% | +23.1% |
| Apache Builds (comma) | +51.1% | +33.6% | +17.7% | +39.3% | +36.6% |
| CitmCatalog (NDJSON) | +60.0% | +28.3% | +32.2% | +43.3% (n/c) | +42.2% |
| CitmCatalog (comma) | +72.5% | +50.0% | +55.5% | +77.2% (n/c) | +67.5% (n/c) |
| Google Maps (NDJSON) | +44.2% | +11.4% | +4.0% | +8.1% (n/c) | +11.8% |
| Google Maps (comma) | +45.7% | +22.2% | +22.6% | +22.6% (n/c) | tie (n/c) |
| Instruments (NDJSON) | +117.7% | +23.2% | +21.8% | +21.8% | +37.9% |
| Instruments (comma) | +130.9% | +41.1% | +53.6% | +62.0% (n/c) | +51.8% |
| Random (NDJSON) | +126.6% | +70.4% | +47.9% | +53.7% (n/c) | +62.6% (n/c) |
| Random (comma) | +139.0% (n/c) | +82.9% | +70.8% | +93.5% (n/c) | +109.1% (n/c) |
| Twitter (NDJSON) | +374.8% | +158.3% | +157.6% | +104.6% (n/c) | +163.7% (n/c) |
| Twitter (comma) | +379.7% | +170.2% | +180.0% | +111.6% (n/c) | +157.1% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 25 of the 30 results in that group and lost 5. It won Stream Formats Small (comma) (+52.8% to +112.9%), Amazon Cellphones (+13.1% to +31.5%) and Large Amazon Cellphones (+3.0% to +27.7%) on all five builds. All five losses are Stream Formats reads: Small as NDJSON on Linux/Clang (−5.5%) and Linux/GCC (−1.7%), Large as NDJSON on Linux/Clang (−13.7%) and macOS/Clang (−15.0%), and Large comma-separated on macOS/Clang (−6.4%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 30-0-0. macOS/GCC lost one (29-0-1), Linux/GCC one (28-1-1), Linux/Clang two (28-0-2), and macOS/Clang went 26-1-3. Every streaming loss but two is a Stream Formats read; the exceptions are Twitter (NDJSON) on macOS/GCC and Instruments (NDJSON) on macOS/Clang.
- **In-order corpus streams lose twice.** Of the 60 in-order corpus results, Jsonifier won 57, tied 1 and lost 2. The tie is Twitter (NDJSON) on Linux/GCC, and the losses are Twitter (NDJSON) on macOS/GCC (−6.6%, n/c) and Instruments (NDJSON) on macOS/Clang (−8.4%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 59 of the 60 reverse stream results and tied the other, Google Maps (comma) on macOS/Clang (n/c). The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +104.6% to +379.7%, while Google Maps rows, which have few, give +4.0% to +45.7% where they win. Instruments, whose reverse streams have the most room to lose, wins on every build (+21.8% to +130.9%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 246.0 MB/s under MSVC and 483.8 MB/s under Linux/Clang, while Jsonifier reads it at 588.0 and 884.7 MB/s.

## 9. Limitations

- **Linux/GCC and macOS/Clang hold the most single-document losses:** 39 and 34 of the 104, against 20 on Linux/Clang, 7 on macOS/GCC and 4 on MSVC. 22 of Linux/GCC's are small documents, led by 14 small in-order, and 21 of macOS/Clang's, led by 9 small sparse. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's prettified CitmCatalog read in order (reused, −40.8%) and its String POD test (reused, −36.8%; fresh, −27.2%); the largest x86 loss is Linux/GCC's prettified Canada Small read in order (reused, −10.5%).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 57 of the 104: Canada 26 (16 on Linux/GCC, 5 on macOS/Clang), Google Maps 18 (7 each on Linux/GCC and macOS/Clang) and CitmCatalog 13 (7 on Linux/Clang, 4 on Linux/GCC). Marine IK, the other float-dense document, adds 17. Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 16 of its 39 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 63 of the 104 losses are small documents (22 on Linux/GCC, 21 on macOS/Clang, 14 on Linux/Clang, 4 on macOS/GCC, 2 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats reads hold five of the seven streaming losses:** Small as NDJSON on Linux/Clang (−5.5%) and Linux/GCC (−1.7%), and Large on Linux/Clang (NDJSON, −13.7%) and macOS/Clang (NDJSON, −15.0%, and comma-separated, −6.4%). The only corpus record streams to lose are Twitter (NDJSON) on macOS/GCC (−6.6%, n/c) and Instruments (NDJSON) on macOS/Clang (−8.4%).
- **Some results did not converge.** 9 of the 1,650 single-document results did not converge: 5 on macOS/GCC, 3 on macOS/Clang and 1 on Linux/GCC. 24 of the 150 streaming results did not converge: 14 on macOS/GCC, 9 on macOS/Clang and 1 on Windows/MSVC. Linux/Clang converged on every result.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate, the key trail and the small documents were added, and MSVC was not swept. The wrap count of 2, the trail capacity of 16 and the direction score's limits (engage at 4, saturate at 8) were not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,512, tied 34 and lost 104. On reverse reads it won 375 of 400 at both sizes, and on Windows/MSVC it lost 4 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (752-19-54 fresh, 760-15-50 reused). The main remaining targets are the small-object documents (Canada, Google Maps and CitmCatalog, with 57 of the 104 losses), small documents in general, and the Linux/GCC and macOS/Clang builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 141, tied 2 and lost 7, and on the 120 corpus record-stream results it went 116-2-2. Stream Formats reads, which hold five of the seven losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.