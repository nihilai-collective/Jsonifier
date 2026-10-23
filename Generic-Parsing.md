# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader tuned for small documents;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,489 of 1,650 single-document results against simdjson On Demand, tied 43 and lost 118. On reverse-order reads it won 375 of 400. On Windows/MSVC it won 318 of 330 single-document results, tied 8 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 144 of 150 results, tied 2 and lost 4, and lost none of the 120 corpus record-stream results.

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
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document, reused, Apache Builds and Github Events tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 21 of the 1,650 single-document results and 15 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 170 / 170 | 159 / 160 | 329 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 157 / 170 | 159 / 160 | 316 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 165 / 170 | 160 / 160 | 325 / 330 |

All builds ran BenchmarkSuite `35ec835` and Jsonifier `b6f72f8`, with simdjson `6d17180cd` on Linux and macOS and `443c02514` on Windows/MSVC. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on October 30, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 19-0-1 | 20-0-0 | 20-0-0 | 18-1-1 | 19-1-0 | 20-0-0 | 20-0-0 | 158-4-3 |
| Linux / Clang | 4-0-1 | 18-0-2 | 19-1-0 | 20-0-0 | 17-0-3 | 14-0-6 | 19-0-1 | 16-0-4 | 17-1-2 | 144-2-19 |
| Linux / GCC | 4-0-1 | 16-2-2 | 18-1-1 | 17-1-2 | 19-0-1 | 17-1-2 | 17-0-3 | 19-1-0 | 18-0-2 | 145-6-14 |
| macOS / GCC | 5-0-0 | 16-2-2 | 18-2-0 | 18-0-2 | 18-0-2 | 18-0-2 | 19-1-0 | 19-0-1 | 18-0-2 | 149-5-11 |
| macOS / Clang | 4-0-1 | 15-3-2 | 19-1-0 | 15-2-3 | 16-0-4 | 18-0-2 | 18-0-2 | 17-0-3 | 17-1-2 | 139-7-19 |
| **All** | **22-0-3** | **82-9-9** | **93-5-2** | **90-3-7** | **90-0-10** | **85-2-13** | **92-2-6** | **91-1-8** | **90-2-8** | **735-24-66** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 19-0-1 | 20-0-0 | 20-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 160-4-1 |
| Linux / Clang | 4-0-1 | 18-1-1 | 20-0-0 | 20-0-0 | 17-1-2 | 16-0-4 | 19-0-1 | 18-0-2 | 18-0-2 | 150-2-13 |
| Linux / GCC | 5-0-0 | 17-1-2 | 19-0-1 | 17-0-3 | 18-1-1 | 17-0-3 | 17-0-3 | 20-0-0 | 17-0-3 | 147-2-16 |
| macOS / GCC | 5-0-0 | 17-2-1 | 20-0-0 | 18-1-1 | 19-0-1 | 18-0-2 | 20-0-0 | 19-1-0 | 19-1-0 | 155-5-5 |
| macOS / Clang | 3-0-2 | 16-2-2 | 18-0-2 | 18-2-0 | 19-0-1 | 19-0-1 | 18-0-2 | 14-2-4 | 17-0-3 | 142-6-17 |
| **All** | **22-0-3** | **86-8-6** | **96-0-4** | **93-3-4** | **93-2-5** | **88-2-10** | **94-0-6** | **91-3-6** | **91-1-8** | **754-19-52** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (21 of them did not converge; see §9), Jsonifier won 1,489, tied 43 and lost 118: 735-24-66 with fresh output objects and 754-19-52 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 189 of 200 full-size reverse results (5 ties, 6 losses) and 186 of 200 small reverse results (2 ties, 12 losses). macOS/GCC lost none of its 80. Of the 6 full-size losses, 4 are minified Canada (Linux/GCC in both runs, −3.0% and −2.2%; MSVC in both runs, −3.9% and −3.3%) and 2 are reused Google Maps on macOS/Clang (minified −3.8%, prettified −7.2%). All 12 small losses are Canada Small or Google Maps Small, on Linux/GCC (6), macOS/Clang (4) and Linux/Clang (2).
- **Windows/MSVC lost four of its 330 single-document results.** All four are minified Canada: read in order at full size (fresh, −3.2%), read in reverse at full size (−3.9% fresh, −3.3% reused) and Canada Small read in order (fresh, −1.4%). Its 8 ties are all Canada and Marine IK. It won all 10 POD results and every other test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.2–13.8% for Jsonifier and 6.8–8.3% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.4 to +3.9 percentage points on the non-MSVC builds and by +9.4 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +288.4% to +327.5%, and Apache Builds (pretty) from +38.8% to +62.8%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +155.4% / +171.5% | +96.0% / +103.2% | +115.8% / +98.5% | +46.9% / +42.8% | +3.4% / −3.4% |
| Double | +182.8% / +211.3% | +84.8% / +85.2% | +95.7% / +99.0% | +117.6% / +120.0% | +61.1% / +67.1% |
| Int64 | +170.3% / +178.7% | +92.3% / +97.6% | +59.7% / +72.0% | +96.1% / +87.4% | +44.0% / +52.0% |
| String | +36.7% / +56.4% | −3.4% / −7.1% | −0.4% / +0.4% | +19.0% / +16.4% | −22.7% / −26.6% |
| Uint64 | +167.2% / +177.3% | +77.2% / +79.3% | +94.9% / +103.0% | +79.5% / +104.1% | +44.6% / +45.5% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +23.9% / +64.8% | +14.9% / +21.7% | +20.0% / +33.2% | +30.5% / +36.8% | +20.5% / +27.6% |
| Apache Builds (pretty) | +38.8% / +62.8% | +13.0% / +19.5% | +18.3% / +24.4% | +22.9% / +37.3% | +18.1% / +23.8% |
| Canada (min) | −3.2% / tie | +5.8% / +6.6% | −6.3% / −6.5% | tie / tie (n/c) | +8.6% / +15.7% |
| Canada (pretty) | tie / +2.8% | +5.2% / +6.2% | −4.0% / −4.6% | +9.6% (n/c) / +11.0% (n/c) | tie / tie |
| CitmCatalog (min) | +42.6% / +49.8% | +10.4% / +9.2% | +12.4% / +9.7% | −5.4% (n/c) / +1.4% | tie / +8.7% |
| CitmCatalog (pretty) | +46.1% / +55.7% | +5.8% / +7.7% | +5.6% / +6.8% | +9.8% / tie | +7.0% / +8.9% |
| Discord (min) | +54.8% / +66.8% | +8.8% / +13.4% | +16.8% / +24.2% | +3.4% / +29.9% | +12.3% / +20.2% |
| Discord (pretty) | +62.0% / +86.3% | +9.3% / +10.2% | +16.0% / +23.5% | +20.1% / +53.7% (n/c) | +1.2% / +13.5% |
| Github Events (min) | +61.1% / +94.9% | +30.3% / +36.4% | +17.7% / +20.4% | +46.1% / +46.4% | +26.5% / +34.5% |
| Github Events (pretty) | +69.0% / +89.3% | +25.3% / +29.7% | +13.6% / +16.8% | +36.2% / +40.8% | +28.0% / +32.6% |
| Google Maps (min) | +55.3% / +64.3% | +21.1% / +21.3% | +28.8% / +32.8% | +37.7% / +20.9% | +11.9% / +11.7% |
| Google Maps (pretty) | +52.9% / +62.2% | +11.2% / +10.8% | +23.0% / +27.2% | +32.3% / +23.4% | +8.1% / +7.2% |
| Instruments (min) | +71.9% / +85.0% | +3.2% / +4.0% | +13.9% / +14.3% | +13.2% / +52.9% | +5.6% / +5.7% |
| Instruments (pretty) | +78.0% / +91.0% | −5.1% / −5.5% | +9.3% / +10.0% | tie / +10.3% | −3.4% / −4.0% |
| Marine IK (min) | tie / tie | +6.9% / +7.5% | tie / +0.6% | +5.3% / +9.7% (n/c) | +3.3% / +6.9% |
| Marine IK (pretty) | +5.3% / +8.0% | +3.5% / tie | tie / tie | −9.1% / −9.9% | +6.5% / tie |
| Random (min) | +62.1% / +76.5% | +28.6% / +34.1% | +38.5% / +47.3% | +33.9% / +71.5% | +27.2% / +39.9% |
| Random (pretty) | +62.8% / +84.5% | +25.0% / +30.3% | +27.9% / +35.4% | +33.1% / +44.3% | +23.9% / +13.1% |
| Twitter (min) | +52.6% / +68.4% | +5.9% / +17.7% | +4.5% / +8.9% | +24.0% / +40.3% | tie / +3.2% |
| Twitter (pretty) | +50.5% / +65.0% | −1.0% / +3.3% | +5.5% / +7.5% | +12.7% / +12.1% | −10.8% / −9.9% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +65.1% / +101.6% | +60.7% / +79.8% | +59.1% / +77.9% | +55.4% / +80.6% | +73.6% / +92.9% |
| Apache Builds (pretty) | +69.1% / +101.2% | +52.6% / +70.8% | +60.0% / +73.5% | +48.9% / +77.0% | +57.8% / +59.5% |
| Canada (min) | −3.9% / −3.3% | +6.7% / +6.9% | −3.0% / −2.2% | tie / +19.6% | +7.6% / +9.2% |
| Canada (pretty) | +2.7% / +2.9% | +8.0% / +10.1% | tie / +0.4% | tie (n/c) / +15.2% | tie / +6.8% |
| CitmCatalog (min) | +88.7% / +90.9% | +61.9% / +65.5% | +71.1% / +74.0% | +58.7% (n/c) / +60.1% | +66.9% / +79.7% |
| CitmCatalog (pretty) | +92.5% / +89.4% | +59.1% / +63.0% | +76.3% / +80.4% (n/c) | +40.1% / +56.0% | +36.2% / +96.3% |
| Discord (min) | +288.4% / +327.5% | +238.3% / +261.9% | +219.7% / +241.2% | +260.8% / +249.6% | +284.7% / +263.6% |
| Discord (pretty) | +255.7% / +302.7% | +217.5% / +237.1% | +202.0% / +220.6% | +188.2% / +232.0% | +299.9% / +321.8% |
| Github Events (min) | +212.7% / +258.7% | +134.6% / +154.2% | +113.5% / +130.1% | +101.5% / +137.6% | +117.7% / +112.3% |
| Github Events (pretty) | +183.9% / +227.2% | +108.8% / +124.6% | +90.1% / +101.5% | +78.4% / +128.2% | +96.5% / +102.8% |
| Google Maps (min) | +40.3% / +42.6% | +4.4% / +5.3% | +6.5% / +7.7% | +22.1% / +35.0% | +13.6% / −3.8% |
| Google Maps (pretty) | +59.2% / +62.5% | tie / +2.1% | +2.3% / +2.3% | +20.2% / +10.5% | +9.2% / −7.2% |
| Instruments (min) | +305.7% / +328.2% | +154.1% / +158.5% | +119.7% / +124.9% | +127.8% / +125.1% | +160.6% / +164.4% |
| Instruments (pretty) | +273.5% / +297.6% | +126.1% / +128.9% | +101.2% / +102.4% | +90.2% / +94.3% | +136.4% / +134.2% |
| Marine IK (min) | +32.5% / +36.7% | +47.0% / +56.0% | +47.1% / +50.5% | +46.6% (n/c) / +65.9% (n/c) | +44.5% (n/c) / +46.5% (n/c) |
| Marine IK (pretty) | +38.3% / +40.4% | +49.6% / +47.0% | +52.3% / +54.9% | +64.0% / +44.5% | +50.5% / +51.9% |
| Random (min) | +135.5% / +156.9% | +84.7% / +98.3% | +65.5% / +74.2% | +84.8% / +92.1% | +98.7% / +84.1% |
| Random (pretty) | +132.4% / +145.2% | +69.8% / +81.7% | +57.6% / +65.7% | +61.7% / +94.6% | +53.8% / +103.7% (n/c) |
| Twitter (min) | +342.0% / +378.0% | +147.6% / +156.1% | +122.6% / +131.5% | +122.1% / +116.9% | +207.3% / +157.3% |
| Twitter (pretty) | +309.4% / +357.8% | +124.0% / +135.1% | +90.7% / +97.2% | +89.6% / +96.6% | +139.3% / +130.5% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +48.3% / +82.3% | +26.8% / +38.1% | +37.4% / +48.7% | +49.8% / +56.0% | +28.2% / +35.7% |
| Apache Builds (pretty) | +53.6% / +79.2% | +18.4% / +24.5% | +35.1% / +40.3% | +30.5% / +50.4% | +17.9% / +22.4% |
| Canada (min) | +61.8% / +60.0% | +7.9% / +8.3% | tie / −1.5% | +6.1% / +30.7% (n/c) | +48.7% / +44.4% |
| Canada (pretty) | +84.7% / +81.0% | +5.2% / +6.8% | +16.2% / +15.2% | +2.7% / tie | +17.9% / +31.5% |
| CitmCatalog (min) | +47.0% / +46.7% | +9.8% / +10.1% | +8.2% / +8.0% | +25.2% / +30.1% | −5.0% / +3.6% |
| CitmCatalog (pretty) | +56.9% / +59.9% | +1.0% / +3.9% | +8.8% / +9.8% | −4.6% / +22.3% | +7.2% / +8.1% |
| Discord (min) | +28.8% / +43.6% | +7.0% / +6.4% | +9.9% / +13.4% | +30.4% / +31.2% | +5.3% / +34.4% |
| Discord (pretty) | +26.7% / +34.5% | +3.9% / +2.5% | +5.6% / +9.7% | +16.6% / +34.6% | −2.8% / tie |
| Github Events (min) | +71.1% / +96.1% | +14.9% / +15.4% | +18.6% / +18.5% | +55.5% / +44.5% | +14.2% / +14.2% |
| Github Events (pretty) | +68.6% / +81.8% | +8.0% / +8.2% | +11.7% / +12.2% | +25.2% / +21.5% | +2.9% / +3.6% |
| Google Maps (min) | +50.4% / +66.7% | +11.8% / +12.8% | +18.1% / +18.2% | +11.1% (n/c) / +17.4% | +1.2% / +1.8% |
| Google Maps (pretty) | +59.8% / +64.2% | +5.3% / +5.1% | +8.1% / +8.6% | +21.9% / +24.0% | −1.0% / +4.0% |
| Instruments (min) | +59.4% / +76.0% | +10.3% / +11.8% | +8.9% / +9.4% | +28.6% / +4.1% | +24.9% / tie |
| Instruments (pretty) | +57.5% / +66.0% | +2.9% / +3.1% | +7.8% / +6.9% | +36.3% / +17.9% | tie / +7.6% |
| Marine IK (min) | +44.9% / +45.4% | +8.8% / +3.5% | −5.5% / −4.5% | −16.1% / −12.0% | +34.3% / +23.0% |
| Marine IK (pretty) | +61.4% / +66.4% | +4.4% / +3.0% | −7.7% / −7.8% | +7.8% / +25.6% | +13.9% / +5.2% |
| Random (min) | +57.9% / +67.6% | +16.0% / +18.8% | +17.6% / +20.6% | +43.9% / +32.5% | +20.1% / +31.8% |
| Random (pretty) | +57.0% / +64.0% | +10.1% / +11.6% | +8.4% / +10.8% | +29.7% / +35.0% | +13.1% / +17.8% |
| Twitter (min) | +74.8% / +75.6% | +11.2% / +12.2% | +13.9% / +14.7% | +36.8% / +23.3% | tie / +7.2% |
| Twitter (pretty) | +68.9% / +74.1% | +4.7% / +5.3% | +11.5% / +11.3% | +7.1% / +14.2% | +7.7% / +5.7% (n/c) |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +34.4% / +53.3% | +13.1% / +16.3% | +21.2% / +23.5% | +35.6% / +64.6% | +19.7% / +22.4% |
| Apache Builds (pretty) | +42.1% / +56.7% | +6.0% / +7.5% | +12.4% / +13.8% | +34.2% / +29.0% | +11.2% / +13.4% |
| Canada (min) | +44.8% / +44.3% | +3.7% / +5.2% | +0.7% / tie | +23.1% / −8.1% | +37.8% / +51.0% |
| Canada (pretty) | +70.4% / +66.6% | +5.6% / +5.3% | +14.3% / +12.8% | +10.7% / +23.1% | +10.3% / +18.0% |
| CitmCatalog (min) | +39.5% / +40.9% | +1.6% / +2.4% | +5.7% / +5.6% | −2.6% / +40.3% | +3.6% / +12.2% |
| CitmCatalog (pretty) | +48.4% / +52.8% | −4.3% / −3.9% | +4.9% / +5.3% | −6.0% / +16.4% (n/c) | −2.7% / +11.3% |
| Discord (min) | +62.5% / +85.3% | +32.0% / +40.4% | +30.0% / +36.3% | +41.9% / +59.6% | +43.6% / +56.2% |
| Discord (pretty) | +66.4% / +83.1% | +19.4% / +23.6% | +23.9% / +28.6% | +56.9% / +40.7% | +14.4% / +22.1% |
| Github Events (min) | +86.8% / +109.1% | +32.1% / +33.7% | +33.4% / +35.4% | +42.3% / +83.9% | +35.9% / +11.7% |
| Github Events (pretty) | +90.4% / +106.3% | +21.7% / +23.8% | +26.2% / +26.5% | +35.6% / +39.2% | +23.8% / +23.5% |
| Google Maps (min) | +54.9% / +60.9% | −0.9% / tie | +3.2% / +3.2% | +7.4% / +22.0% | −7.5% / −14.7% |
| Google Maps (pretty) | +52.6% / +63.7% | −9.1% / −8.8% | −0.7% / −1.3% | +15.7% / +14.7% | −1.1% / +0.4% |
| Instruments (min) | +79.7% / +87.9% | +19.5% / +19.8% | +18.3% / +19.0% | +16.0% / +14.8% | +29.8% / +16.3% |
| Instruments (pretty) | +75.5% / +80.3% | +20.7% / +21.8% | +14.7% / +15.9% | +23.3% / +15.1% | +17.8% / +17.9% |
| Marine IK (min) | +55.8% / +59.6% | +46.7% / +43.6% | +22.2% / +25.1% | +32.1% / +12.0% | +43.1% / +41.2% |
| Marine IK (pretty) | +66.0% / +75.1% | +23.8% / +26.4% | +17.5% / +18.2% | +26.1% / +48.4% | +34.4% (n/c) / +21.7% |
| Random (min) | +71.1% / +81.1% | +24.1% / +27.1% | +21.4% / +24.1% | +72.8% / +38.8% | +24.9% / +25.5% |
| Random (pretty) | +61.4% / +73.8% | +10.4% / +11.6% | +7.1% / +9.6% | +18.1% / +28.0% | +21.5% / +22.5% |
| Twitter (min) | +80.0% / +85.4% | +15.1% / +15.2% | +14.6% / +15.3% | +12.0% / +28.0% | +8.1% / +10.6% |
| Twitter (pretty) | +72.1% / +76.4% | +13.3% / +13.8% | +10.0% / +9.8% | +17.9% / +18.0% | −7.9% / +12.1% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +31.6% / +52.3% | +11.7% / +17.8% | +23.0% / +34.3% | +59.8% / +55.4% | +17.6% / +22.4% |
| Apache Builds (pretty) | +32.4% / +53.8% | +8.7% / +13.3% | +20.2% / +31.6% | +44.3% / +34.0% | +12.1% / +17.7% |
| Canada (min) | −1.4% / tie | +7.0% / +7.1% | −5.0% / −6.7% | −14.1% / −5.5% | +5.2% / +6.1% |
| Canada (pretty) | tie / tie | +3.4% / +2.8% | −7.2% / −8.5% | −3.5% / −7.1% | +16.8% / +4.8% |
| CitmCatalog (min) | +32.3% / +33.0% | −1.1% / −2.1% | tie / +4.6% | +11.1% / +15.0% | −3.2% / +11.1% |
| CitmCatalog (pretty) | +31.2% / +32.1% | −5.2% / −2.7% | +1.5% / +2.2% | +9.0% / +12.5% | +2.6% / +1.5% |
| Discord (min) | +32.9% / +58.7% | +5.6% / +6.9% | +8.6% / +16.6% | +28.5% / +35.4% | +11.0% / +39.4% |
| Discord (pretty) | +25.2% / +62.5% | +1.7% / +7.6% | +7.2% / +12.9% | +16.0% / +31.5% | +5.3% / +12.6% |
| Github Events (min) | +62.2% / +77.5% | +10.4% / +12.4% | +2.6% / +3.6% | +21.6% / +25.5% | +13.2% / +14.6% |
| Github Events (pretty) | +56.6% / +65.2% | +3.1% / +4.4% | +0.7% / −1.5% | +26.5% / +15.9% | +8.8% / +7.3% |
| Google Maps (min) | +40.0% (n/c) / +54.4% | +18.3% / +20.4% | +25.9% / +27.1% | +34.0% / +38.4% | +12.3% / +13.2% |
| Google Maps (pretty) | +54.8% / +61.4% | +8.0% / +8.4% | +20.8% / +22.3% | +25.8% / +29.5% | +8.8% / +10.0% |
| Instruments (min) | +66.0% / +73.2% | +1.2% / +1.9% | +6.6% / +7.5% | +16.6% / +19.5% | +6.4% / +7.0% |
| Instruments (pretty) | +78.9% / +72.1% | −9.9% / −10.4% | +2.0% / +3.5% | +20.8% / +14.7% | −3.1% / −3.4% |
| Marine IK (min) | +11.8% / +9.0% | +1.8% / +0.5% | +1.9% / +2.0% | +7.0% / +8.5% | +6.6% / +6.1% |
| Marine IK (pretty) | +14.3% / +18.1% | −1.3% / −2.2% | +1.7% / +2.4% | +3.9% / +6.8% | +3.5% / +5.0% |
| Random (min) | +55.8% / +63.9% | +17.5% / +22.1% | +28.9% / +36.5% | +42.7% / +54.0% | +26.9% / +15.7% |
| Random (pretty) | +58.6% / +76.1% | +14.5% / +18.1% | +23.8% / +28.8% | +52.1% / +39.8% | +20.3% / +22.5% |
| Twitter (min) | +48.7% / +50.5% | −0.8% / +5.1% | +8.5% / +11.0% | +26.1% / +68.3% | +8.3% / +6.5% |
| Twitter (pretty) | +46.9% / +56.1% | −3.1% / +2.3% | +8.9% / +10.0% | +20.5% / +29.6% | +4.7% / +4.4% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +91.0% / +140.5% | +74.4% / +92.2% | +66.1% / +77.7% | +78.3% / +102.2% | +74.5% / +97.2% |
| Apache Builds (pretty) | +87.5% / +124.5% | +62.7% / +74.5% | +54.8% / +64.8% | +46.0% / +69.3% | +65.1% / +84.1% |
| Canada (min) | tie / +3.4% | +5.6% / +6.6% | −4.0% / −4.3% | tie / +3.9% | +24.0% / +14.7% |
| Canada (pretty) | +7.2% / +5.0% | +4.1% / +5.6% | −5.0% / −5.0% | +17.3% / +3.1% | −5.2% / +11.3% |
| CitmCatalog (min) | +88.4% / +99.4% | +59.7% / +64.1% | +46.0% / +49.6% | +61.4% / +60.9% | +46.2% / +80.2% |
| CitmCatalog (pretty) | +81.7% / +84.3% | +52.9% / +59.0% | +37.0% / +40.3% | +46.0% / +49.4% | +45.0% / +79.9% |
| Discord (min) | +314.7% / +408.2% | +218.8% / +256.6% | +166.1% / +194.2% | +165.6% / +247.1% | +262.6% / +274.5% |
| Discord (pretty) | +288.0% / +373.9% | +198.8% / +229.8% | +148.3% / +174.0% | +176.3% / +207.0% | +237.2% / +277.1% |
| Github Events (min) | +86.7% / +100.8% | +35.2% / +37.5% | +20.5% / +21.9% | +42.4% / +44.7% | +36.0% / +45.0% |
| Github Events (pretty) | +81.0% / +85.2% | +22.2% / +26.9% | +14.8% / +15.1% | +22.5% / +29.8% | +28.2% / +31.0% |
| Google Maps (min) | +43.7% / +50.5% | +1.2% / +0.4% | +6.1% / +5.1% | +23.2% / +25.4% | −4.4% / −6.0% |
| Google Maps (pretty) | +45.4% / +52.3% | −0.7% / −2.4% | −0.2% / −0.6% | +19.3% / +21.9% | +7.6% / −2.7% |
| Instruments (min) | +312.9% / +325.3% | +141.6% / +148.5% | +87.2% / +94.3% | +120.0% / +127.9% | +153.8% / +163.1% |
| Instruments (pretty) | +278.2% / +290.2% | +121.2% / +126.0% | +79.9% / +80.7% | +96.8% / +104.9% | +125.7% / +134.0% |
| Marine IK (min) | +46.2% / +55.1% | +39.4% / +42.1% | +36.0% / +36.8% | +44.1% / +48.6% | +45.6% / +47.5% |
| Marine IK (pretty) | +51.3% / +54.6% | +34.5% / +38.3% | +29.7% / +32.3% | +37.4% / +40.4% | +38.3% / +41.8% |
| Random (min) | +132.3% / +165.6% | +80.0% / +89.3% | +59.7% / +66.8% | +84.1% / +99.4% | +89.1% / +118.3% |
| Random (pretty) | +108.5% / +150.1% | +66.5% / +75.7% | +50.4% / +57.5% | +67.3% / +79.7% | +35.2% / +56.3% |
| Twitter (min) | +305.3% / +358.4% | +146.7% / +153.3% | +94.0% / +99.7% | +117.0% / +101.5% | +154.4% / +159.6% |
| Twitter (pretty) | +312.8% / +320.7% | +131.6% / +134.6% | +73.8% / +78.3% | +99.8% / +105.6% | +142.4% / +140.8% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +51.7% / +83.1% | +30.3% / +29.1% | +35.3% / +45.2% | +25.1% / +45.4% | +23.6% / +29.9% |
| Apache Builds (pretty) | +51.6% / +87.8% | +20.0% / +19.2% | +27.7% / +32.5% | +43.1% / +22.8% | +15.8% / +14.8% |
| Canada (min) | +56.4% / +60.2% | +8.6% / +8.4% | +7.2% / +9.0% | +18.2% / +17.2% | +5.1% / +15.6% |
| Canada (pretty) | +86.9% / +96.9% | +8.5% / +10.3% | +13.3% / +13.8% | +15.0% / +12.4% | +2.2% / tie |
| CitmCatalog (min) | +43.8% / +52.3% | +5.7% / +7.3% | +9.6% / +13.7% | +18.6% / +19.6% | +8.3% / +6.5% |
| CitmCatalog (pretty) | +54.0% / +63.7% | −0.5% / +2.5% | +8.2% / +9.2% | +14.3% / +15.0% | +4.7% / +4.3% |
| Discord (min) | +40.6% / +47.7% | +6.2% / +4.1% | +3.9% / +7.6% | +21.1% / +24.6% | +3.9% / +18.8% |
| Discord (pretty) | +39.0% / +52.6% | −1.4% / −1.8% | +3.9% / +7.7% | +15.7% / +18.0% | −15.4% / −7.2% |
| Github Events (min) | +63.7% / +72.5% | +3.5% / +3.0% | +8.3% / +9.9% | +27.6% / +32.5% | +37.1% / +4.0% |
| Github Events (pretty) | +44.2% / +62.9% | +1.5% / +1.6% | +4.5% / +6.9% | +18.4% / +20.6% | +2.3% / tie |
| Google Maps (min) | +51.7% / +53.5% | +11.2% / +10.2% | +12.4% / +13.8% | +21.2% / +19.5% | +1.5% / +2.6% |
| Google Maps (pretty) | +38.7% / +48.6% | +2.7% / +1.2% | +5.5% / +5.3% | +37.5% / +25.4% | −15.4% / +0.9% |
| Instruments (min) | +43.4% / +53.4% | +3.6% / +5.2% | +2.9% / +1.8% | −2.7% / +9.5% | +7.3% / +7.0% |
| Instruments (pretty) | +43.5% / +50.2% | −0.8% / +1.5% | tie / +0.9% | +4.7% / +5.2% | −2.5% / −3.7% |
| Marine IK (min) | +43.3% / +46.9% | +1.3% / +1.9% | +8.9% / +6.6% | +8.6% / +9.9% | +7.8% / +7.6% |
| Marine IK (pretty) | +56.4% / +72.3% | +0.6% / +1.5% | +10.2% / +11.8% | +12.1% / +13.4% | +2.2% / +1.9% |
| Random (min) | +60.5% / +69.5% | +16.9% / +16.6% | +15.7% / +17.4% | +30.1% / +35.7% | +26.7% / +8.2% |
| Random (pretty) | +44.3% / +43.3% | +9.7% / +8.1% | +11.0% / +10.5% | +20.0% / +40.8% | +6.0% / −14.4% |
| Twitter (min) | +26.2% / +31.4% | +0.6% / +1.0% | +2.6% / +2.7% | +31.3% / +35.8% | +5.3% / +4.4% |
| Twitter (pretty) | +24.4% / +33.0% | −6.6% / −2.2% | +5.7% / +5.4% | +27.9% / tie | +3.4% / −4.2% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +42.2% / +51.7% | +11.1% / +15.8% | +19.3% / +22.4% | −2.5% / +28.9% | tie / −5.2% |
| Apache Builds (pretty) | +41.0% / +57.9% | +5.5% / +6.1% | +9.9% / +11.3% | +23.3% / +43.0% | +1.8% / +3.2% |
| Canada (min) | +49.4% / +54.3% | +4.5% / +12.0% | +0.7% / +1.7% | +13.1% / +13.3% | −13.7% / +13.2% |
| Canada (pretty) | +80.0% / +73.7% | +4.3% / +5.0% | +9.1% / +8.9% | +8.7% / +6.1% | +17.1% / +23.1% |
| CitmCatalog (min) | +50.5% / +56.2% | tie / +1.9% | −0.2% / −1.9% | +10.4% / +23.4% | +1.2% / +4.7% |
| CitmCatalog (pretty) | +58.6% / +62.2% | −1.8% / −2.9% | +0.5% / −0.2% | −10.9% / tie | +2.7% / +2.0% |
| Discord (min) | +79.4% / +97.9% | +33.6% / +39.1% | +24.9% / +26.9% | +47.3% / +59.6% | +33.2% / +42.2% |
| Discord (pretty) | +72.6% / +92.2% | +21.7% / +22.0% | +19.3% / +22.9% | +32.0% / +37.3% | +23.7% / +25.7% |
| Github Events (min) | +63.0% / +89.7% | +18.5% / +19.7% | +17.6% / +19.1% | +31.3% / +32.0% | +19.8% / +20.0% |
| Github Events (pretty) | +58.4% / +91.0% | +4.8% / +4.6% | +12.7% / +14.2% | +24.0% / +27.1% | +12.5% / +12.0% |
| Google Maps (min) | +53.8% / +53.7% | +1.1% / +1.3% | +2.2% / +1.6% | +16.5% / +16.5% | +1.4% / +1.5% |
| Google Maps (pretty) | +66.6% / +64.0% | −11.1% / −9.4% | −1.6% / −2.4% | +15.6% / +14.3% | −2.2% / −2.8% |
| Instruments (min) | +77.3% / +87.1% | +11.5% / +21.3% | +14.0% / +14.5% | +24.5% / +23.6% | +23.3% / +23.3% |
| Instruments (pretty) | +64.5% / +72.5% | +14.2% / +16.7% | +7.9% / +9.2% | +14.0% / +15.1% | +11.2% / −2.2% |
| Marine IK (min) | +52.6% / +52.9% | +11.9% / +12.1% | +29.7% / +32.7% | +34.5% / +41.4% | +28.0% / +31.4% |
| Marine IK (pretty) | +69.0% / +64.9% | +12.5% / +14.3% | +27.5% / +28.9% | +27.9% / +27.1% | +28.6% / +15.9% |
| Random (min) | +51.3% / +69.4% | +26.4% / +27.2% | +14.4% / +12.3% | +30.5% / +37.4% | +22.9% / +15.4% |
| Random (pretty) | +58.3% / +63.6% | +13.1% / +10.6% | +10.5% / +9.8% | +24.4% / +40.8% | +15.1% / +22.7% |
| Twitter (min) | +37.4% / +57.9% | +8.0% / +8.2% | +12.7% / +11.8% | +20.2% (n/c) / +19.0% | +8.3% / +15.2% |
| Twitter (pretty) | +37.3% / +46.8% | +7.9% / +7.9% | +12.8% / +11.4% | +19.9% / +16.1% | +8.1% / +4.1% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 4.8x faster at full size: +89.6% (Twitter pretty, macOS/GCC, fresh) up to +378.0% (Twitter min, MSVC, reused), and up to +321.8% outside MSVC (Discord pretty, macOS/Clang, reused). The small documents show the same pattern, from +73.8% to +408.2%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −7.2% (prettified Google Maps on macOS/Clang, reused) to +35.0%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events almost never loses.** It accounts for 1 of the 118 single-document losses: prettified Github Events Small read in order on Linux/GCC (reused, −1.5%). Read in reverse at full size it wins on every build (+78.4% to +258.7%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 183 of 200 full-size sparse-reverse results across both allocation modes (2 ties, 15 losses), and 181 of 200 small ones (3 ties, 16 losses). MSVC won all 80. All but one of the full-size losses are on Canada, CitmCatalog or Google Maps: 5 on Linux/Clang (−0.9% to −9.1%), 2 on Linux/GCC (−0.7% and −1.3%), 3 on macOS/GCC and 5 on macOS/Clang (−1.1% to −14.7%, including prettified Twitter at −7.9%). 13 of the 16 small losses are on those three documents as well.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 182.2 MB/s on MSVC against 290.2 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 648.1 and 921.3 MB/s. Every MSVC full-size sparse result is a win, by +26.7% to +96.1%, and every full-size sparse-reverse result by +34.4% to +109.1%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 183 of 200 full-size sparse results (6 ties, 11 losses) and 182 of 200 small ones (4 ties, 14 losses). MSVC and Linux/Clang won all 40 of their full-size results, and MSVC all 40 small. The full-size losses are on Linux/GCC (Canada and Marine IK, −1.5% to −7.8%), macOS/GCC (3) and macOS/Clang (3). macOS/Clang holds 7 of the 14 small losses and Linux/Clang 6.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 168, tied 17 and lost 15. Linux/Clang went 36-1-3, MSVC 35-4-1, Linux/GCC 33-3-4, macOS/GCC 33-4-3 and macOS/Clang 31-5-4 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 173, tied 4 and lost 23. macOS/Clang went 37-0-3, MSVC 36-3-1, macOS/GCC 36-0-4, Linux/GCC 34-1-5 and Linux/Clang 30-0-10. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The losses are mostly modest and concentrate on CitmCatalog, Canada and Instruments: Linux/Clang's run from −0.8% to −10.4%, and the largest is Canada on macOS/GCC (minified, fresh, −14.1%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−4.0% to −6.5%), and MSVC lost minified Canada once (fresh, −3.2%). macOS/GCC lost prettified Marine IK twice (−9.1% and −9.9%) and minified CitmCatalog once (fresh, −5.4%, n/c). Linux/Clang lost prettified Instruments twice (−5.1% and −5.5%) and prettified Twitter once (fresh, −1.0%), and macOS/Clang lost those same two prettified documents in both runs (Instruments −3.4% and −4.0%, Twitter −10.8% and −9.9%). Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 44-0-6 on them. Double, Int64 and Uint64 won all 10 results each (+61.1% to +211.3%, +44.0% to +178.7% and +44.6% to +177.3%) and Bool 9 (up to +171.5%, with one loss on macOS/Clang reused, −3.4%). String is the exception: 5 wins and 5 losses, on Linux/Clang in both runs (−3.4% and −7.1%), macOS/Clang in both runs (−22.7% and −26.6%) and Linux/GCC fresh (−0.4%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 2.2% to 15.5% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs from the 26 streaming tests that existed at the time are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`, before the Apache Builds streams were added.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 29 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 24 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 22 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / GCC | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 5-0-1 | 10-2-0 | 12-0-0 | 27-2-1 |
| **All** | **26-0-4** | **58-2-0** | **60-0-0** | **144-2-4** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +33.2% | +21.7% | +15.9% | +14.8% | +15.7% |
| Large Amazon Cellphones (NDJSON) | +15.7% | +12.5% | +1.3% | +11.0% | +14.3% (n/c) |
| Stream Formats Small (NDJSON) | +88.0% | −1.5% | −1.4% | +22.3% | +1.4% |
| Stream Formats Small (comma) | +148.8% | +73.3% | +103.7% | +107.9% | +64.1% |
| Stream Formats Large (NDJSON) | +100.9% | −2.2% | +21.6% | +10.6% | +6.2% |
| Stream Formats Large (comma) | +106.7% | +3.9% | +28.8% | +15.9% | −0.5% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +65.9% | +17.2% | +10.5% | +31.7% | +27.6% |
| Apache Builds (comma) | +76.3% | +29.1% | +20.6% | +42.3% | +33.2% |
| CitmCatalog (NDJSON) | +53.4% | +8.1% | +10.0% | +4.9% | +38.5% (n/c) |
| CitmCatalog (comma) | +65.8% | +24.9% | +34.5% | +26.4% | +26.9% (n/c) |
| Google Maps (NDJSON) | +61.4% | +28.0% | +27.5% | +22.0% | +10.8% |
| Google Maps (comma) | +67.2% | +37.1% | +45.7% | +37.0% | +17.7% (n/c) |
| Instruments (NDJSON) | +130.1% | +13.6% | +18.3% | +19.8% | tie |
| Instruments (comma) | +120.1% | +27.1% | +64.5% | +47.6% | +19.8% |
| Random (NDJSON) | +82.7% | +28.9% | +25.8% | +45.0% | +53.8% |
| Random (comma) | +85.5% | +38.4% | +48.2% | +83.5% (n/c) | +58.9% (n/c) |
| Twitter (NDJSON) | +57.3% (n/c) | +7.5% | +3.3% | +11.0% | tie |
| Twitter (comma) | +64.9% | +18.9% | +21.2% | +25.4% | +16.1% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +63.1% | +16.7% | +3.7% | +21.5% | +21.0% |
| Apache Builds (comma) | +69.7% | +32.1% | +17.1% | +38.6% | +36.0% (n/c) |
| CitmCatalog (NDJSON) | +67.1% | +29.6% | +34.3% | +26.2% | +28.3% |
| CitmCatalog (comma) | +79.9% | +46.7% | +57.4% | +70.5% (n/c) | +54.6% |
| Google Maps (NDJSON) | +54.5% | +11.3% | +7.4% | +13.8% | +33.1% (n/c) |
| Google Maps (comma) | +59.9% | +19.2% | +27.6% | +29.3% | +15.8% (n/c) |
| Instruments (NDJSON) | +130.9% | +30.9% | +25.6% | +20.5% | +29.8% |
| Instruments (comma) | +144.3% | +35.9% | +57.3% | +47.3% (n/c) | +62.2% |
| Random (NDJSON) | +138.8% | +72.3% | +53.3% | +66.5% | +81.0% |
| Random (comma) | +150.8% | +85.3% | +74.9% | +55.7% (n/c) | +100.7% |
| Twitter (NDJSON) | +385.0% | +160.6% | +137.5% | +111.5% (n/c) | +157.4% |
| Twitter (comma) | +369.3% | +167.2% | +152.8% | +146.6% (n/c) | +171.7% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 26 of the 30 results in that group and lost 4. It won Amazon Cellphones (+14.8% to +33.2%), Large Amazon Cellphones (+1.3% to +15.7%) and Stream Formats Small (comma) (+64.1% to +148.8%) on all five builds. The four losses are all Stream Formats and all within 2.2%: Small (NDJSON) on Linux/Clang (−1.5%) and Linux/GCC (−1.4%), Large (NDJSON) on Linux/Clang (−2.2%), and Large (comma) on macOS/Clang (−0.5%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC and macOS/GCC won every streaming test,** 30-0-0 each; Linux/GCC lost one of 30, macOS/Clang one (27-2-1) and Linux/Clang two.
- **In-order corpus streams never lose.** Of the 60 in-order corpus results, Jsonifier won 58 and tied 2, both on macOS/Clang: Instruments (NDJSON) and Twitter (NDJSON).
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +111.5% to +385.0%, while Google Maps rows, which have few, give +7.4% to +59.9%. Instruments, whose reverse streams have the most room to lose, wins on every build (+20.5% to +144.3%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 231.2 MB/s under MSVC and 477.3 MB/s under Linux/Clang, while Jsonifier reads it at 579.8 and 884.4 MB/s.

## 9. Limitations

- **macOS/Clang holds the most single-document losses:** 36 of the 118, against 32 on Linux/Clang, 30 on Linux/GCC, 16 on macOS/GCC and 4 on MSVC. 19 of macOS/Clang's are small documents, led by 7 small sparse. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD (−26.6% reused, −22.7% fresh), followed by macOS/GCC's minified Marine IK Sparse (fresh, −16.1%); the largest x86 loss is Linux/Clang's prettified Google Maps Small Sparse Reverse (fresh, −11.1%).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 71 of the 118: Canada 26 (15 on Linux/GCC), Google Maps 25 (12 on macOS/Clang) and CitmCatalog 20 (9 on Linux/Clang). Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 15 of its 30 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 65 of the 118 losses are small documents (22 on Linux/Clang, 19 on macOS/Clang, 16 on Linux/GCC, 7 on macOS/GCC, 1 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats read as NDJSON** lose narrowly on two builds: Small on Linux/Clang (−1.5%) and Linux/GCC (−1.4%), and Large on Linux/Clang (−2.2%). macOS/Clang also loses Large (comma) (−0.5%). Every corpus record stream wins or ties.
- **Some results did not converge.** 21 of the 1,650 single-document results did not converge: 14 on macOS/GCC, 5 on macOS/Clang, 1 on Linux/GCC and 1 on Windows/MSVC. 15 of the 150 streaming results did not converge: 8 on macOS/Clang, 6 on macOS/GCC and 1 on Windows/MSVC. Linux/Clang converged on every result.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate, the key trail and the small documents were added, and MSVC was not swept. The wrap count of 2, the trail capacity of 16 and the direction score's limits (engage at 4, saturate at 8) were not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commit** (`3e7bf6d`; see §6), and does not cover the Apache Builds and Github Events tests.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,489, tied 43 and lost 118. On reverse reads it won 375 of 400 at both sizes, and on Windows/MSVC it lost 4 of its 330 single-document results. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (735-24-66 fresh, 754-19-52 reused). The main remaining targets are the small-object documents (Canada, Google Maps and CitmCatalog, with 71 of the 118 losses), small documents in general, and the Clang builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 144, tied 2 and lost 4, and it won or tied all 120 corpus record-stream results. The Stream Formats tests are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.