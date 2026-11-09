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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,565 of 1,650 single-document results against simdjson On Demand, tied 13 and lost 72. On reverse-order reads it won 390 of 400. On Windows/MSVC it won 325 of 330 single-document results, tied 1 and lost 4.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`), and on the 3 AVX-512 builds as well (`correctness-validation-avx512-main`), all at Jsonifier `c80a6d5` and simdjson `7002cd3`, November 8, 2026.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 9 of the 1,650 single-document results and 6 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 167 / 170 | 160 / 160 | 327 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 164 / 170 | 160 / 160 | 324 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 170 / 170 | 160 / 160 | 330 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `ed43ce8`, Jsonifier `c80a6d5` and simdjson `f0c2d2ac2`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. Every build ran on November 9, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 162-0-3 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 17-0-3 | 20-0-0 | 158-0-7 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 20-0-0 | 15-0-5 | 18-0-2 | 20-0-0 | 20-0-0 | 149-0-16 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 18-0-2 | 20-0-0 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 160-2-3 |
| macOS / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 17-2-1 | 20-0-0 | 18-1-1 | 19-1-0 | 156-4-5 |
| **All** | **23-0-2** | **90-1-9** | **98-0-2** | **94-0-6** | **100-0-0** | **88-3-9** | **98-0-2** | **95-1-4** | **99-1-0** | **785-6-34** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 163-1-1 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 16-2-2 | 18-0-2 | 155-2-8 |
| Linux / GCC | 5-0-0 | 16-0-4 | 18-0-2 | 17-0-3 | 19-1-0 | 16-0-4 | 18-0-2 | 20-0-0 | 20-0-0 | 149-1-15 |
| macOS / GCC | 5-0-0 | 17-1-2 | 20-0-0 | 19-0-1 | 20-0-0 | 18-0-2 | 20-0-0 | 19-1-0 | 19-0-1 | 157-2-6 |
| macOS / Clang | 4-0-1 | 19-0-1 | 19-0-1 | 19-0-1 | 20-0-0 | 18-1-1 | 19-0-1 | 18-0-2 | 20-0-0 | 156-1-8 |
| **All** | **23-0-2** | **89-2-9** | **97-0-3** | **95-0-5** | **99-1-0** | **90-1-9** | **97-0-3** | **93-3-4** | **97-0-3** | **780-7-38** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (9 of them did not converge; see §9), Jsonifier won 1,565, tied 13 and lost 72: 785-6-34 with fresh output objects and 780-7-38 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 195 of 200 full-size reverse results and 195 of 200 small reverse results, with no ties and 5 losses at each size. Windows/MSVC, Linux/Clang and macOS/GCC won all 80 each. Nine of the ten losses are Canada: at full size, minified Canada on Linux/GCC (−7.3% fresh, −8.0% reused) and on macOS/Clang (reused, −8.8%) and prettified Canada on Linux/GCC (−4.4% and −5.6%); at small size, Canada Small on Linux/GCC in both forms and both runs (−9.6% to −12.7%). The tenth is prettified Google Maps Small on macOS/Clang (reused, −2.7%).
- **Windows/MSVC lost four of its 330 single-document results,** all minified in-order reads of Canada or Marine IK: Canada (fresh, −4.2%, n/c), Marine IK (−7.8% fresh, −9.9% reused, n/c) and Marine IK Small (fresh, −1.3%). Its only tie is minified Canada (reused). It won all 10 POD results, all 80 of its reverse results and all 160 of its sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.5–13.7% for Jsonifier and 5.4–9.8% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.7 to +1.9 percentage points on the Linux builds and macOS/Clang, and by +4.8 points on MSVC and +5.7 on macOS/GCC. On MSVC, Discord Reverse (min) goes from +194.0% to +224.1%, and Apache Builds (pretty) from +17.1% to +25.2%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +246.6% / +245.8% | +90.2% / +93.5% | +140.9% / +144.8% | +54.3% / +23.8% (n/c) | +1.9% / +6.4% |
| Double | +198.8% / +216.3% | +85.9% / +93.6% | +95.6% / +98.5% | +68.0% / +138.5% | +70.5% / +68.8% |
| Int64 | +166.7% / +179.5% | +94.4% / +101.3% | +93.9% / +96.5% | +73.9% (n/c) / +40.7% | +45.3% / +52.7% |
| String | +39.8% / +63.3% | −5.5% / −10.4% | +5.2% / +5.0% | +12.8% / +15.5% | −8.6% / −18.4% |
| Uint64 | +167.4% / +166.1% | +109.7% / +117.8% | +88.3% / +91.1% | +118.3% / +50.3% | +50.1% / +42.5% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.0% / +24.3% | +18.6% / +30.4% | +22.7% / +31.9% | +79.3% / +87.9% | +15.1% / +26.5% |
| Apache Builds (pretty) | +17.1% / +25.2% | +18.9% / +27.6% | +23.3% / +31.6% | +21.3% / +30.7% | +22.3% / +28.0% |
| Canada (min) | −4.2% (n/c) / tie | +4.9% / +4.5% | −10.7% / −13.0% | tie / +32.6% (n/c) | +5.7% / +6.6% |
| Canada (pretty) | +4.1% / +4.4% | +4.3% / +3.2% | −8.8% / −10.4% | −10.1% / −9.2% | +1.4% / +2.7% |
| CitmCatalog (min) | +15.3% / +14.5% | +7.8% / +9.1% | +9.6% / +10.0% | +11.4% / +25.4% | +9.0% / +10.3% |
| CitmCatalog (pretty) | +16.8% / +17.8% | +5.1% / +4.5% | +3.5% / +4.4% | +12.7% / +10.1% | +8.9% / +9.4% |
| Discord (min) | +18.9% / +24.5% | +13.3% / +20.5% | +12.3% / +18.3% | +18.0% / +21.1% | +18.9% / +22.0% |
| Discord (pretty) | +19.8% / +21.6% | +12.4% / +17.5% | +13.8% / +19.5% | +39.5% / +49.5% | +13.4% / +15.1% |
| Github Events (min) | +31.3% / +41.4% | +31.8% / +38.7% | +29.4% / +35.1% | +62.4% / +50.6% | +45.4% / +55.1% |
| Github Events (pretty) | +28.5% / +37.0% | +24.7% / +30.2% | +25.1% / +30.6% | +54.0% / +78.9% | +34.1% / +40.8% |
| Google Maps (min) | +20.3% / +22.8% | +18.8% / +19.8% | +26.5% / +27.9% | +11.4% / +37.4% | +13.8% / +13.5% |
| Google Maps (pretty) | +21.6% / +23.0% | +11.4% / +10.6% | +24.8% / +25.0% | +41.1% / +47.4% | +9.9% / +10.1% |
| Instruments (min) | +31.4% / +37.6% | +3.4% / +4.3% | +7.0% / +7.7% | +15.2% / +14.6% | +8.0% / +7.1% |
| Instruments (pretty) | +31.5% / +36.6% | −1.3% / −0.8% | +2.4% / +2.3% | +10.4% / +10.0% | −1.2% / −1.4% |
| Marine IK (min) | −7.8% / −9.9% (n/c) | +2.8% / +6.3% | −5.6% / −6.1% | +4.5% / −5.7% | +5.3% / +39.3% |
| Marine IK (pretty) | +2.4% / +1.6% | +0.3% / +2.6% | −4.8% / −6.0% | +10.1% / tie | +5.6% / +20.4% |
| Random (min) | +26.3% / +34.5% | +24.9% / +31.3% | +30.6% / +37.9% | +58.0% / +82.8% | +32.0% / +35.9% |
| Random (pretty) | +25.1% / +31.2% | +22.5% / +27.5% | +25.4% / +34.1% | +69.9% / +75.0% | +28.3% / +30.8% |
| Twitter (min) | +24.1% / +31.7% | +21.8% / +16.5% | +10.4% / +23.7% | +22.3% / +27.5% | +12.6% / +12.6% |
| Twitter (pretty) | +22.4% / +30.8% | +16.7% / +11.1% | +4.5% / +7.6% | +16.7% / +21.9% | +1.6% / +6.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +41.8% / +60.9% | +62.2% / +80.0% | +58.1% / +75.0% | +79.1% / +103.0% | +68.0% / +86.5% |
| Apache Builds (pretty) | +37.1% / +54.9% | +58.9% / +73.9% | +58.3% / +73.7% | +70.8% / +90.6% | +58.3% / +73.8% |
| Canada (min) | +1.9% / +1.4% | +8.3% / +8.6% | −7.3% / −8.0% | +5.1% / +7.5% | +7.1% / −8.8% |
| Canada (pretty) | +5.6% / +7.5% | +6.0% / +8.4% | −4.4% / −5.6% | +13.2% / +5.5% | +5.5% / +6.2% |
| CitmCatalog (min) | +58.7% / +61.5% | +62.8% / +68.2% | +58.2% / +62.6% | +95.3% / +124.7% | +71.8% / +76.5% |
| CitmCatalog (pretty) | +51.9% / +54.0% | +57.7% / +59.0% | +52.4% / +54.5% | +56.4% / +87.7% | +67.6% / +70.5% |
| Discord (min) | +194.0% / +224.1% | +260.2% / +279.7% | +228.1% / +250.8% | +301.2% / +326.2% | +323.9% / +345.9% |
| Discord (pretty) | +174.8% / +197.8% | +222.8% / +240.6% | +203.8% / +223.3% | +239.0% / +266.7% | +278.4% / +292.6% |
| Github Events (min) | +144.4% / +179.7% | +139.9% / +158.9% | +117.2% / +133.3% | +139.2% / +114.4% | +168.2% / +194.7% |
| Github Events (pretty) | +130.3% / +161.8% | +120.7% / +137.0% | +94.8% / +109.4% | +65.9% / +104.0% | +144.0% / +163.0% |
| Google Maps (min) | +17.5% / +19.6% | +9.5% / +8.2% | +16.9% / +16.5% | +40.4% / +23.3% | +14.4% / +17.6% |
| Google Maps (pretty) | +16.1% / +16.4% | +8.2% / +10.1% | +13.4% / +14.6% | +23.8% / +36.5% | +10.5% / +11.8% |
| Instruments (min) | +189.6% / +201.6% | +174.2% / +181.4% | +161.8% / +168.6% | +137.2% / +160.2% | +215.9% / +222.7% |
| Instruments (pretty) | +162.7% / +170.6% | +136.0% / +139.9% | +137.5% / +142.2% | +113.1% / +126.7% | +187.7% / +191.8% |
| Marine IK (min) | +25.1% / +25.4% | +48.1% / +51.3% | +36.8% / +37.4% | +51.1% / +54.5% | +52.7% / +65.8% |
| Marine IK (pretty) | +38.5% / +29.1% (n/c) | +49.8% / +48.7% | +39.1% / +41.4% | +51.0% / +49.9% (n/c) | +52.8% / +67.8% |
| Random (min) | +65.6% / +76.1% | +87.1% / +99.0% | +86.8% / +99.8% | +97.5% / +112.4% | +115.3% / +123.7% |
| Random (pretty) | +59.4% / +70.1% | +80.0% / +89.7% | +80.2% / +89.8% | +80.3% / +82.8% | +97.0% / +104.7% |
| Twitter (min) | +223.0% / +259.3% | +160.9% / +172.4% | +129.9% / +141.4% | +177.5% / +123.0% (n/c) | +224.7% / +241.3% |
| Twitter (pretty) | +193.5% / +221.1% | +141.2% / +149.2% | +106.1% / +112.2% | +173.1% / +126.4% | +205.9% / +218.2% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +23.8% / +34.7% | +34.7% / +44.1% | +42.5% / +52.3% | +58.8% / +108.8% | +35.0% / +40.1% |
| Apache Builds (pretty) | +23.6% / +31.3% | +26.6% / +35.2% | +32.1% / +39.9% | +29.1% / +152.9% | +22.9% / +25.6% |
| Canada (min) | +57.9% / +57.8% | +11.9% / +12.2% | −4.6% / −4.6% | +12.3% / +12.6% | +48.2% / +42.9% |
| Canada (pretty) | +84.6% / +84.9% | +7.3% / +7.5% | +15.9% / +16.4% | +6.2% / +6.2% | +29.2% / +29.8% |
| CitmCatalog (min) | +30.2% / +30.8% | +5.3% / +4.4% | +5.6% / +5.3% | +7.0% / +4.5% | +5.9% / +6.2% |
| CitmCatalog (pretty) | +47.4% / +47.3% | +1.1% / +1.5% | +4.5% / +5.2% | +13.7% / +28.4% (n/c) | +8.0% / +7.8% |
| Discord (min) | +10.3% / +14.4% | +14.0% / +13.0% | +23.2% / +26.7% | +22.1% / +34.0% | +19.4% / +21.0% |
| Discord (pretty) | +10.2% / +15.8% | +12.6% / +15.1% | +20.3% / +23.7% | +8.8% / +19.6% | +9.3% / +9.5% |
| Github Events (min) | +46.4% / +55.3% | +18.2% / +20.9% | +23.6% / +24.0% | +28.8% / +75.4% | +15.8% / +17.9% |
| Github Events (pretty) | +47.0% / +55.5% | +12.1% / +13.6% | +14.4% / +15.4% | +9.3% / +27.9% | +6.2% / +6.4% |
| Google Maps (min) | +25.1% / +28.9% | +15.5% / +15.7% | +19.2% / +20.5% | +10.1% / +19.2% | +11.7% / +13.9% |
| Google Maps (pretty) | +25.0% / +27.8% | +1.6% / +1.1% | +10.6% / +12.8% | +44.5% / +53.7% | +7.4% / +9.3% |
| Instruments (min) | +29.1% / +37.1% | +16.2% / +18.4% | +8.9% / +9.9% | +10.0% / +9.5% | +10.4% / +10.8% |
| Instruments (pretty) | +33.3% / +35.6% | +8.5% / +8.0% | +5.4% / +5.4% | +14.4% / +35.8% | −0.7% / −0.2% |
| Marine IK (min) | +29.3% / +29.6% | +7.8% / +7.6% | −7.3% / −8.5% | −9.7% / −6.1% | +17.8% / +17.5% |
| Marine IK (pretty) | +61.8% / +62.7% | +6.2% / +8.6% | −7.1% / −6.7% | +14.7% / +4.4% | +17.1% / +17.5% |
| Random (min) | +32.5% / +37.5% | +25.1% / +27.3% | +23.0% / +25.4% | +49.4% / +55.8% | +35.9% / +37.2% |
| Random (pretty) | +30.3% / +35.6% | +17.7% / +20.7% | +11.6% / +15.3% | +38.3% / +42.9% | +24.1% / +26.0% |
| Twitter (min) | +59.6% / +65.0% | +16.5% / +16.9% | +17.7% / +19.3% | −5.8% / +5.6% | +15.2% / +18.5% |
| Twitter (pretty) | +50.5% / +55.3% | +8.4% / +8.4% | +14.0% / +13.9% | +36.7% / +30.0% | +7.2% / +7.6% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +17.0% / +24.0% | +18.3% / +22.4% | +16.3% / +17.9% | +13.9% / +80.9% | +25.1% / +28.7% |
| Apache Builds (pretty) | +15.8% / +19.0% | +13.1% / +14.9% | +14.5% / +17.3% | +43.4% / +37.2% | +18.2% / +19.7% |
| Canada (min) | +40.6% / +40.1% | +9.3% / +9.4% | +1.9% / tie | +9.3% / +10.0% | +36.2% / +37.3% |
| Canada (pretty) | +65.5% / +67.0% | +8.0% / +6.4% | +12.1% / +12.5% | +7.1% / +7.8% | +25.3% / +25.2% |
| CitmCatalog (min) | +18.4% / +16.0% | +2.5% / +2.5% | +4.3% / +4.6% | +15.6% / +4.7% | +5.9% / +5.9% |
| CitmCatalog (pretty) | +31.1% / +31.4% | +2.5% / +0.8% | +4.1% / +3.0% | +26.1% / +10.1% | +6.6% / +6.0% |
| Discord (min) | +40.3% / +50.5% | +35.1% / +41.3% | +44.2% / +51.7% | +45.8% / +55.3% | +70.3% / +75.6% |
| Discord (pretty) | +37.4% / +43.7% | +38.6% / +42.1% | +37.7% / +42.5% | +53.3% / +61.1% | +55.7% / +57.1% |
| Github Events (min) | +61.0% / +73.7% | +34.9% / +39.5% | +40.3% / +42.0% | +40.6% / +31.0% | +45.9% / +49.0% |
| Github Events (pretty) | +58.8% / +67.6% | +30.9% / +33.4% | +37.0% / +39.6% | +11.6% / +54.3% | +34.6% / +34.3% |
| Google Maps (min) | +18.9% / +22.5% | +2.3% / +2.5% | +7.3% / +8.2% | +33.0% / +63.0% | +7.8% / +8.4% |
| Google Maps (pretty) | +23.7% / +27.9% | +1.3% / +2.5% | +6.9% / +7.7% | +34.6% / +52.0% | +5.4% / +6.2% |
| Instruments (min) | +44.8% / +50.6% | +22.6% / +26.4% | +19.3% / +20.2% | +26.7% / +19.2% | +35.1% / +38.3% |
| Instruments (pretty) | +45.2% / +50.5% | +20.4% / +22.2% | +25.4% / +25.8% | +25.9% / +60.6% | +22.7% / +23.6% |
| Marine IK (min) | +49.3% / +48.0% | +35.7% / +35.7% | +19.9% / +20.3% | +31.0% / +29.0% | +42.7% / +40.7% |
| Marine IK (pretty) | +70.5% / +66.7% | +26.0% / +27.6% | +11.0% / +12.7% | +28.3% / +33.9% | +32.4% / +32.7% |
| Random (min) | +39.6% / +44.0% | +26.5% / +29.1% | +24.2% / +26.0% | +55.9% / +63.7% | +47.8% / +51.9% |
| Random (pretty) | +36.5% / +40.2% | +18.9% / +22.3% | +16.5% / +20.4% | +39.7% / +53.0% | +36.8% / +45.7% |
| Twitter (min) | +57.8% / +61.2% | +24.3% / +25.3% | +25.8% / +26.3% | +13.8% / +19.3% | +31.6% / +32.2% |
| Twitter (pretty) | +54.9% / +58.5% | +20.9% / +21.3% | +22.5% / +22.8% | +16.9% / +18.6% | +20.4% / +20.2% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.3% / +15.3% | +15.6% / +20.7% | +26.3% / +30.7% | +32.3% / +48.5% | +11.7% / +46.2% |
| Apache Builds (pretty) | +10.0% / +14.2% | +16.1% / +22.2% | +21.9% / +31.4% | +28.7% / +39.9% | +9.0% / +4.1% |
| Canada (min) | +0.8% / +2.2% | +5.4% / +0.5% | −14.4% / −18.2% | +1.6% / +2.1% | +4.1% / +3.5% |
| Canada (pretty) | +3.1% / +5.4% | +1.1% / +1.5% | −14.4% / −16.6% | tie / +1.5% | −0.5% / +0.7% |
| CitmCatalog (min) | +6.8% / +9.2% | +1.3% / +3.7% | +1.2% / +2.5% | +11.9% / −4.4% | +5.8% / +8.7% |
| CitmCatalog (pretty) | +9.4% / +12.3% | −1.9% / −1.4% | +0.5% / +3.1% | +21.5% / +14.2% | +3.7% / −5.6% |
| Discord (min) | +6.8% / +17.0% | +9.1% / +13.5% | +3.1% / +11.4% | +21.7% / +38.9% | +13.2% / +19.6% |
| Discord (pretty) | +5.2% / +18.9% | +2.4% / +10.0% | +4.6% / +13.7% | +19.5% / +34.7% | +7.3% / +12.8% |
| Github Events (min) | +26.5% / +30.1% | +13.1% / +14.6% | +8.7% / +8.3% | +36.1% / +47.5% | +15.9% / +35.2% |
| Github Events (pretty) | +18.3% / +24.0% | +6.0% / +7.8% | +2.5% / +1.0% | +5.2% / +17.7% | +3.6% / +12.4% |
| Google Maps (min) | +25.8% / +30.0% | +17.4% / +17.9% | +20.6% / +23.8% | +33.6% / +37.8% | tie / +16.1% |
| Google Maps (pretty) | +25.7% / +32.6% | +9.8% / +9.0% | +21.7% / +22.6% | +32.1% / +39.8% | +10.9% / +9.7% |
| Instruments (min) | +27.7% / +30.6% | +0.9% / +1.0% | +1.2% / +0.9% | +13.0% / +13.7% | +7.8% / +9.2% |
| Instruments (pretty) | +29.6% / +29.5% | −5.2% / −3.3% | −1.3% / −2.5% | +8.8% / −2.7% | tie / tie |
| Marine IK (min) | −1.3% / +5.6% | +2.7% / +3.6% | −2.6% / +1.7% | +13.9% / +9.0% | +4.7% / +23.1% |
| Marine IK (pretty) | +3.4% / +4.4% | +0.8% / +3.1% | −6.1% / −3.6% | +11.8% / +7.8% | +3.0% / +17.4% |
| Random (min) | +24.8% / +27.6% | +15.9% / +18.3% | +21.4% / +28.6% | +47.7% / +60.7% | +80.5% / +30.0% |
| Random (pretty) | +22.2% / +25.5% | +16.5% / +14.3% | +21.4% / +26.4% | +60.0% / +57.7% | +23.4% / +25.7% |
| Twitter (min) | +18.8% / +27.6% | +4.3% / +7.6% | +8.0% / +13.6% | +23.5% / +30.5% | +9.7% / +12.4% |
| Twitter (pretty) | +16.3% / +22.4% | +2.7% / +4.9% | +6.5% / +9.9% | +20.7% / +28.4% | +5.1% / +7.6% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +50.3% / +72.3% | +77.3% / +85.4% | +79.7% / +95.0% | +87.6% / +117.0% | +85.9% / +112.9% |
| Apache Builds (pretty) | +44.8% / +67.2% | +65.6% / +77.2% | +73.6% / +83.2% | +73.1% / +98.7% | +72.2% / +93.5% |
| Canada (min) | +2.3% / +2.0% | +8.8% / +10.1% | −9.9% / −12.7% | +4.1% / +3.8% | +6.7% / +7.2% |
| Canada (pretty) | +3.8% / +4.7% | +3.0% / +4.4% | −9.6% / −9.8% | +3.5% / +3.6% | +3.1% / +3.9% |
| CitmCatalog (min) | +63.8% / +71.8% | +52.8% / +59.4% | +59.6% / +66.8% | +65.1% / +73.1% | +72.9% / +83.0% |
| CitmCatalog (pretty) | +60.0% / +67.5% | +46.1% / +50.6% | +52.6% / +60.3% | +54.9% / +61.9% | +62.3% / +73.4% |
| Discord (min) | +196.8% / +261.1% | +235.7% / +285.2% | +213.9% / +233.0% | +246.0% / +302.0% | +317.8% / +373.1% |
| Discord (pretty) | +182.7% / +234.2% | +215.5% / +249.9% | +201.8% / +224.9% | +211.9% / +257.7% | +291.2% / +324.2% |
| Github Events (min) | +42.1% / +50.3% | +41.0% / +46.7% | +28.5% / +34.0% | +44.8% / +54.8% | +58.1% / +73.1% |
| Github Events (pretty) | +32.6% / +40.2% | +30.4% / +34.2% | +22.3% / +25.5% | +32.0% / +35.2% | +43.4% / +53.7% |
| Google Maps (min) | +14.2% / +18.1% | +6.2% / +5.4% | +11.4% / +11.3% | +34.6% / +9.5% | +13.9% / +17.1% |
| Google Maps (pretty) | +16.8% / +19.9% | +7.1% / +7.5% | +9.1% / +10.8% | +19.5% / +21.3% | +8.2% / −2.7% |
| Instruments (min) | +159.6% / +175.7% | +156.9% / +165.9% | +152.1% / +158.3% | +148.0% / +154.9% | +215.7% / +231.1% |
| Instruments (pretty) | +147.3% / +151.1% | +133.5% / +132.0% | +134.0% / +140.7% | +96.9% / +127.3% | +187.0% / +198.1% |
| Marine IK (min) | +29.5% / +30.4% | +40.2% / +44.1% | +37.5% / +40.1% | +51.7% / +52.5% | +50.4% / +63.2% |
| Marine IK (pretty) | +30.4% / +33.6% | +35.7% / +38.8% | +34.7% / +37.0% | +38.4% / +31.2% | +42.2% / +54.2% |
| Random (min) | +64.1% / +76.6% | +77.4% / +92.0% | +83.7% / +93.6% | +91.7% / +107.5% | +117.4% / +129.0% |
| Random (pretty) | +51.8% / +60.3% | +72.3% / +82.7% | +74.0% / +82.7% | +70.2% / +83.0% | +95.7% / +70.9% |
| Twitter (min) | +167.7% / +174.9% | +170.0% / +174.9% | +134.2% / +138.1% | +140.0% / +146.3% | +218.8% / +237.0% |
| Twitter (pretty) | +147.7% / +155.4% | +145.2% / +156.0% | +117.2% / +119.5% | +117.4% / +128.2% | +158.1% / +223.9% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +25.7% / +50.4% | +32.9% / +31.3% | +36.7% / +44.4% | +30.9% / +60.1% | +27.4% / +30.9% |
| Apache Builds (pretty) | +19.0% / +37.3% | +22.4% / +20.1% | +27.5% / +36.0% | +32.6% / +127.6% | +17.2% / +17.8% |
| Canada (min) | +56.6% / +60.4% | +10.1% / +9.7% | +12.0% / +10.3% | +9.2% / +12.3% | +5.7% / +5.1% |
| Canada (pretty) | +78.6% / +89.2% | +9.3% / +10.9% | +16.4% / +15.9% | +11.5% / +11.4% | +1.8% / +5.5% |
| CitmCatalog (min) | +30.0% / +35.0% | +2.0% / −1.3% | +9.6% / +7.5% | +17.2% / +16.2% | +38.4% / +10.8% |
| CitmCatalog (pretty) | +39.6% / +46.9% | −2.9% / tie | +9.2% / +9.3% | +14.7% / +13.2% | +6.8% / +6.5% |
| Discord (min) | +14.1% / +23.2% | +11.3% / +16.4% | +11.0% / +14.2% | +22.6% / +28.3% | +11.0% / +15.3% |
| Discord (pretty) | +17.3% / +22.6% | +6.0% / +6.8% | +8.6% / +16.8% | +15.6% / +20.0% | +1.4% / +2.7% |
| Github Events (min) | +37.2% / +42.8% | +9.2% / +8.9% | +13.7% / +16.7% | +28.5% / +26.9% | +9.0% / +6.1% |
| Github Events (pretty) | +36.5% / +35.2% | +5.7% / +6.9% | +12.9% / +13.8% | +19.9% / +22.1% | +3.9% / +2.3% |
| Google Maps (min) | +26.2% / +29.1% | +13.1% / +12.7% | +11.6% / +12.7% | +52.4% / +55.0% | +15.4% / +22.1% |
| Google Maps (pretty) | +23.7% / +28.6% | −0.4% / tie | +7.7% / +9.1% | +45.5% / +48.5% | +10.2% / +11.3% |
| Instruments (min) | +21.0% / +26.1% | +8.1% / +12.5% | +3.7% / +5.2% | +7.8% / +8.7% | +10.9% / +10.9% |
| Instruments (pretty) | +26.9% / +33.1% | +1.1% / +2.6% | +1.6% / +1.0% | +4.2% / +14.5% | −0.9% / −2.3% |
| Marine IK (min) | +35.3% / +20.2% | −12.5% / −10.1% | +4.7% / +4.3% | +9.9% / +10.0% | +8.9% / +8.3% |
| Marine IK (pretty) | +46.0% / +51.8% | +6.5% / +4.8% | +9.6% / +9.7% | +9.7% / +10.7% | +2.6% / +2.6% |
| Random (min) | +14.2% / +21.2% | +17.6% / +21.4% | +19.5% / +16.9% | +37.9% / +75.8% | +25.3% / +30.1% |
| Random (pretty) | +18.7% / +19.7% | +9.2% / +12.4% | +12.3% / +13.8% | +17.3% / +29.1% | +16.3% / +18.3% |
| Twitter (min) | +22.5% / +21.9% | +9.4% / +7.7% | +9.1% / +8.4% | +14.2% / +14.3% | +8.3% / +6.7% |
| Twitter (pretty) | +12.6% / +17.2% | +6.0% / +5.9% | +9.8% / +9.3% | +24.3% / tie | tie / −1.6% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +21.3% / +32.6% | +15.9% / +17.3% | +23.8% / +21.7% | +36.8% / +45.6% | +21.2% / +25.6% |
| Apache Builds (pretty) | +17.9% / +21.3% | +11.3% / +11.9% | +13.8% / +19.1% | +22.2% / +29.8% | tie / +7.8% |
| Canada (min) | +39.3% / +46.3% | +2.8% / +7.8% | +35.1% / +33.0% | +9.8% / +9.6% | +4.8% / +3.7% |
| Canada (pretty) | +71.1% / +73.5% | +6.4% / +7.7% | +33.7% / +31.9% | +8.2% / +8.9% | +1.7% / +1.1% |
| CitmCatalog (min) | +22.5% / +23.8% | +0.3% / −2.1% | +2.4% / +3.4% | +13.2% / −4.6% | +10.5% / +9.7% |
| CitmCatalog (pretty) | +27.4% / +33.2% | +1.8% / −1.0% | +1.9% / +6.5% | +6.5% / +10.6% | +6.9% / +19.9% |
| Discord (min) | +38.7% / +46.4% | +41.1% / +53.8% | +46.8% / +49.5% | +48.1% / +59.2% | +67.9% / +71.2% |
| Discord (pretty) | +37.7% / +44.3% | +42.1% / +43.7% | +38.2% / +40.1% | +32.9% / +38.7% | +48.1% / +50.6% |
| Github Events (min) | +44.3% / +45.7% | +20.9% / +26.3% | +29.2% / +30.6% | +25.4% / +57.0% | +30.6% / +31.1% |
| Github Events (pretty) | +42.7% / +47.2% | +17.3% / +17.1% | +28.1% / +30.1% | +16.7% / +29.7% | +21.1% / +24.0% |
| Google Maps (min) | +15.2% / +15.9% | +2.0% / +5.4% | +10.5% / +9.1% | +41.6% / +44.5% | +7.3% / +8.1% |
| Google Maps (pretty) | +16.8% / +21.3% | +1.4% / +1.6% | +4.7% / +3.9% | +59.7% / +54.0% | +8.7% / +5.3% |
| Instruments (min) | +36.3% / +40.5% | +25.8% / +27.2% | +23.4% / +23.7% | +26.2% / +41.1% | +37.8% / +39.8% |
| Instruments (pretty) | +33.5% / +41.9% | +16.5% / +18.1% | +23.1% / +26.7% | +16.8% / +44.1% | +23.0% / +23.1% |
| Marine IK (min) | +47.6% / +51.2% | +51.1% / +49.1% | +42.8% / +44.5% | +41.0% / +30.4% | +34.5% / +34.7% |
| Marine IK (pretty) | +52.2% / +54.1% | +40.8% / +40.6% | +37.6% / +38.8% | +5.5% / +16.4% | +20.7% / +20.8% |
| Random (min) | +28.6% / +34.2% | +29.4% / +28.2% | +27.9% / +25.0% | +45.3% / +52.6% | +41.4% / +76.3% |
| Random (pretty) | +33.0% / +28.7% | +22.9% / +21.0% | +19.7% / +18.8% | +48.8% / +56.7% | +30.1% / +26.9% |
| Twitter (min) | +30.3% / +30.2% | +5.7% / +12.3% | +16.7% / +16.1% | +15.3% / +18.4% | +22.0% / +21.7% |
| Twitter (pretty) | +24.9% / +26.4% | +13.5% / +14.4% | +15.8% / +14.6% | +36.7% / +13.7% | +12.6% / +11.2% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.1x to 4.5x faster at full size: +106.1% (Twitter pretty, Linux/GCC, fresh) up to +345.9% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +96.9% to +373.1%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −8.8% (minified Canada on macOS/Clang, reused) to +40.4%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events never loses.** None of the 72 single-document losses or 13 ties is a Github Events read: it wins all 100 of its results. Read in reverse at full size it wins on every build (+65.9% to +194.7%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 196 of 200 full-size sparse-reverse results across both allocation modes (no ties, 4 losses), and 192 of 200 small ones (1 tie, 7 losses). MSVC, Linux/GCC and macOS/GCC won all 80 each. The full-size losses are prettified CitmCatalog on Linux/Clang (−3.8% fresh, −2.9% reused) and minified Canada on Linux/GCC (−3.7% and −3.2%). Of the 7 small losses, 5 are on macOS/Clang (−1.9% to −4.9%, on Canada, CitmCatalog, Google Maps and Twitter) and 2 on Linux/Clang (prettified CitmCatalog Small, −1.9% and −1.4%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 233.4 MB/s on MSVC against 281.8 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 641.4 and 909.7 MB/s. Every MSVC full-size sparse result is a win, by +10.2% to +84.9%, and every full-size sparse-reverse result by +15.8% to +73.7%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 190 of 200 full-size sparse results (1 tie, 9 losses) and 188 of 200 small ones (3 ties, 9 losses). MSVC won all 40 of its full-size results and all 40 small. The full-size losses are on Linux/GCC (6: minified Canada, −1.5% and −3.3%, and Marine IK, −6.0% to −9.1%), macOS/GCC (2: minified Marine IK, −3.1% and −3.2%) and macOS/Clang (1: prettified Instruments, fresh, −0.7%). macOS/Clang holds 6 of the 9 small losses, including the largest (prettified Instruments Small, reused, −22.3%), Linux/GCC 2 and Linux/Clang 1.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 179, tied 3 and lost 18. Linux/Clang and macOS/Clang went 38-0-2, MSVC 36-1-3, macOS/GCC 35-2-3 and Linux/GCC 32-0-8 across the two modes.
- **Small documents read in order tie full-size in-order reads for the most losses of any category.** Across 200 results, Jsonifier won 178, tied 4 and lost 18. MSVC went 39-0-1, macOS/GCC 37-1-2, Linux/Clang 36-0-4, macOS/Clang 35-3-2 and Linux/GCC 31-0-9. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −1.4% to −5.2%, on CitmCatalog and Instruments, and Linux/GCC's from −1.3% to −18.2%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −18.2%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−6.1% to −11.7%) and Marine IK in both forms and both runs (−1.3% to −5.6%). MSVC lost minified Marine IK in both runs (−7.2% and −6.9%) and minified Canada (reused, −2.2%). macOS/Clang lost prettified Instruments in both runs (−1.7% and −1.8%) and prettified Twitter (reused, −2.1%). macOS/GCC lost prettified Canada (reused, −7.7%). Linux/Clang lost none. Canada and Marine IK, which hold 12 of the 15, are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 46-0-4 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+1.9% to +246.6%, +68.0% to +216.3%, +40.7% to +179.5% and +42.5% to +167.4%). String is the exception: 6 wins (+5.0% to +63.3%) and 4 losses, on Linux/Clang (−5.5% and −10.4%) and macOS/Clang (−8.6% and −18.4%) in both runs.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 3.9% to 12.5% below its own NDJSON throughput on the same records in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 30 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 26 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 28 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 4-0-2 | 11-0-1 | 12-0-0 | 27-0-3 |
| **All** | **28-0-2** | **59-0-1** | **60-0-0** | **147-0-3** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +32.5% | +18.9% | +23.3% | +29.9% | +17.9% |
| Large Amazon Cellphones (NDJSON) | +16.1% | +10.0% | +3.3% | +12.9% | +14.6% |
| Stream Formats Small (NDJSON) | +42.5% | +1.6% | +23.4% | +4.2% | +9.0% |
| Stream Formats Small (comma) | +78.9% | +89.5% | +110.0% | +82.2% | +59.1% |
| Stream Formats Large (NDJSON) | +91.5% | +1.4% | +26.1% | +11.0% | −7.9% |
| Stream Formats Large (comma) | +94.5% | +19.9% | +35.1% | +15.5% | −4.0% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +22.9% | +31.3% | +26.8% | +54.7% | +30.7% |
| Apache Builds (comma) | +28.6% | +45.0% | +40.3% | +64.2% (n/c) | +39.1% |
| CitmCatalog (NDJSON) | +12.4% | +8.3% | +5.5% | +13.2% | +10.8% |
| CitmCatalog (comma) | +23.5% | +28.0% | +30.3% | +32.7% | +32.1% |
| Google Maps (NDJSON) | +26.6% | +28.5% | +26.5% | +25.3% | +9.1% |
| Google Maps (comma) | +35.2% | +42.2% | +43.1% | +40.6% | +37.5% |
| Instruments (NDJSON) | +47.9% | +8.6% | +12.4% | +16.1% | −11.1% |
| Instruments (comma) | +59.3% | +35.1% | +43.0% | +45.9% | +23.5% |
| Random (NDJSON) | +28.3% | +27.0% | +24.2% | +58.3% | +47.8% |
| Random (comma) | +34.5% | +36.2% | +39.5% | +72.2% (n/c) | +69.9% |
| Twitter (NDJSON) | +26.2% | +13.2% | +2.5% | +17.6% | +15.4% |
| Twitter (comma) | +31.3% | +24.3% | +20.3% | +29.4% | +19.8% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +18.8% | +24.5% | +12.7% | +37.8% | +25.2% |
| Apache Builds (comma) | +30.8% | +41.5% | +30.0% | +56.6% | +37.4% |
| CitmCatalog (NDJSON) | +31.8% | +33.5% | +26.4% | +34.3% | +50.4% |
| CitmCatalog (comma) | +43.2% | +53.6% | +46.9% | +62.2% | +53.2% |
| Google Maps (NDJSON) | +18.4% | +12.9% | +7.6% | +17.2% | +33.2% |
| Google Maps (comma) | +22.8% | +22.6% | +20.3% | +23.2% | +21.6% (n/c) |
| Instruments (NDJSON) | +51.0% | +32.9% | +20.8% | +28.5% | +40.3% |
| Instruments (comma) | +65.5% | +58.2% | +44.1% | +51.4% (n/c) | +81.6% |
| Random (NDJSON) | +60.4% | +83.7% | +70.9% | +78.3% | +105.8% |
| Random (comma) | +68.6% | +98.5% | +97.2% | +94.5% | +115.4% |
| Twitter (NDJSON) | +229.1% | +185.2% | +144.0% | +134.1% (n/c) | +250.6% |
| Twitter (comma) | +247.8% | +192.8% | +154.3% | +147.9% | +276.6% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 28 of the 30 results in that group and lost 2. It won Stream Formats Small (comma) (+59.1% to +110.0%), Amazon Cellphones (+17.9% to +32.5%), Large Amazon Cellphones (+3.3% to +16.1%) and Stream Formats Small (NDJSON) (+1.6% to +42.5%) on all five builds. Stream Formats Large won on four builds in both forms and lost on macOS/Clang (comma −4.0%, NDJSON −7.9%).
- **Four builds won every streaming test,** 30-0-0 each: Windows/MSVC, Linux/Clang, Linux/GCC and macOS/GCC. macOS/Clang went 27-0-3: its two Stream Formats Large reads and Instruments (NDJSON) read in order (−11.1%).
- **In-order corpus streams never lose.** Jsonifier won all 60 in-order corpus results.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +134.1% to +276.6%, while Google Maps rows, which have few, give +7.6% to +33.2%. Instruments, whose reverse streams have the most room to lose, wins on every build (+20.8% to +81.6%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 291.7 MB/s under MSVC and 451.2 MB/s under Linux/Clang, while Jsonifier reads it at 574.9 and 812.6 MB/s.

## 9. Limitations

- **Linux/GCC holds the most single-document losses:** 31 of the 72, against 15 on Linux/Clang, 13 on macOS/Clang, 9 on macOS/GCC and 4 on MSVC. 34 of the 72 are small documents, led by 13 on Linux/GCC and 11 on Linux/Clang. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (reused, −18.4%) and Linux/GCC's minified Canada Small read in order (reused, −18.2%), which is also the largest x86 loss.
- **Canada, Google Maps and CitmCatalog hold many of the losses.** Together they account for 34 of the 72: Canada 23 (18 on Linux/GCC, 2 on macOS/Clang, 2 on macOS/GCC, 1 on MSVC), CitmCatalog 9 (6 on Linux/Clang, 2 on macOS/GCC, 1 on macOS/Clang) and Google Maps 2 (1 on Linux/Clang, 1 on macOS/Clang). Marine IK, the other float-dense document, adds 19. Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 18 of its 31 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry almost half the losses.** 34 of the 72 losses are small documents (13 on Linux/GCC, 11 on Linux/Clang, 6 on macOS/Clang, 3 on macOS/GCC, 1 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **macOS/Clang holds all three streaming losses:** Stream Formats Large as NDJSON (−7.9%) and comma-separated (−4.0%), and Instruments (NDJSON) read in order (−11.1%).
- **Some results did not converge.** 9 of the 1,650 single-document results did not converge: 6 on macOS/GCC and 3 on Windows/MSVC. 6 of the 150 streaming results did not converge: 4 on macOS/GCC and 2 on macOS/Clang. Linux/Clang and Linux/GCC converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,565, tied 13 and lost 72. On reverse reads it won 390 of 400 at both sizes, and on Windows/MSVC it lost 4 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (785-6-34 fresh, 780-7-38 reused). The main remaining targets are the small-object and float-heavy documents (Canada, Google Maps, CitmCatalog and Marine IK, with 53 of the 72 losses), small documents in general, and the Linux/GCC build.

The design also carries over to streams. Because each streamed document is an ordinary `generic::generic_document<false>` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 148 and lost 2, and it won all 120 corpus record-stream results. The Stream Formats Large reads on macOS/Clang, which hold both losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.