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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,517 of 1,650 single-document results against simdjson On Demand, tied 44 and lost 89. On reverse-order reads it won 378 of 400. On Windows/MSVC it won 317 of 330 single-document results, tied 6 and lost 7.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`, Jsonifier `39b6202`, November 6, 2026).
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 26 of the 1,650 single-document results and 21 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 160 / 170 | 158 / 160 | 318 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 164 / 170 | 159 / 160 | 323 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 164 / 170 | 160 / 160 | 324 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `ed43ce8`, Jsonifier `39b6202` and simdjson `dfe02cfe3`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. Every build ran on November 6, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 19-1-0 | 20-0-0 | 20-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 159-3-3 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-1-0 | 20-0-0 | 17-0-3 | 20-0-0 | 18-0-2 | 19-1-0 | 157-2-6 |
| Linux / GCC | 5-0-0 | 17-0-3 | 18-1-1 | 17-1-2 | 19-1-0 | 15-0-5 | 18-0-2 | 19-0-1 | 20-0-0 | 148-3-14 |
| macOS / GCC | 5-0-0 | 17-1-2 | 19-0-1 | 20-0-0 | 20-0-0 | 17-2-1 | 18-2-0 | 18-1-1 | 19-1-0 | 153-7-5 |
| macOS / Clang | 3-1-1 | 17-2-1 | 18-1-1 | 18-2-0 | 18-1-1 | 16-1-3 | 20-0-0 | 16-1-3 | 18-1-1 | 144-10-11 |
| **All** | **22-1-2** | **88-4-8** | **94-3-3** | **94-4-2** | **97-2-1** | **83-4-13** | **96-2-2** | **91-2-7** | **96-3-1** | **761-25-39** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 18-2-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 20-0-0 | 20-0-0 | 158-3-4 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 18-0-2 | 20-0-0 | 17-1-2 | 20-0-0 | 158-1-6 |
| Linux / GCC | 5-0-0 | 16-1-3 | 18-1-1 | 17-1-2 | 19-0-1 | 15-1-4 | 18-0-2 | 20-0-0 | 20-0-0 | 148-4-13 |
| macOS / GCC | 5-0-0 | 18-1-1 | 18-0-2 | 18-1-1 | 19-0-1 | 18-2-0 | 18-0-2 | 18-1-1 | 20-0-0 | 152-5-8 |
| macOS / Clang | 4-0-1 | 16-3-1 | 20-0-0 | 17-1-2 | 18-0-2 | 15-2-3 | 18-0-2 | 15-0-5 | 17-0-3 | 140-6-19 |
| **All** | **23-0-2** | **87-6-7** | **94-3-3** | **91-3-6** | **96-0-4** | **84-5-11** | **94-0-6** | **90-2-8** | **97-0-3** | **756-19-50** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (26 of them did not converge; see §9), Jsonifier won 1,517, tied 44 and lost 89: 761-25-39 with fresh output objects and 756-19-50 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 188 of 200 full-size reverse results (6 ties, 6 losses) and 190 of 200 small reverse results (2 ties, 8 losses). Windows/MSVC and Linux/Clang lost none of their 80. Every loss is on Canada or Google Maps. The 6 full-size losses are minified Canada on Linux/GCC (−5.3% fresh, −7.6% reused), macOS/GCC (−6.7% and −4.8%) and macOS/Clang (fresh, −1.5%), and prettified Google Maps on macOS/GCC (reused, −2.4%). All 8 small losses are Canada Small: on Linux/GCC (4, −4.6% to −11.0%), macOS/GCC (reused, both forms, −7.0% and −9.4%) and macOS/Clang (reused, both forms, −1.1% and −3.9%).
- **Windows/MSVC lost seven of its 330 single-document results,** all in-order reads of Canada or Marine IK: minified Canada (−5.2% fresh, −4.1% reused), Marine IK (minified reused, −9.0%; prettified fresh, −1.9%), minified Canada Small (reused, −1.0%) and minified Marine IK Small (−1.2% and −1.1%). It won all 10 POD results and all 160 of its sparse and sparse-reverse results, and lost no reverse read.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.3–13.3% for Jsonifier and 5.8–10.0% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +0.9 to +3.3 percentage points on the non-MSVC builds and by +5.2 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +195.7% to +210.9%, and Apache Builds (pretty) from +5.9% to +25.7%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +259.3% / +252.2% | +90.3% / +93.3% | +134.1% / +112.5% | +31.8% / +45.2% | tie / +6.6% |
| Double | +177.6% / +229.8% | +83.2% / +87.7% | +104.9% / +111.0% | +86.0% / +82.4% | +60.3% / +71.3% |
| Int64 | +167.8% / +178.3% | +86.4% / +95.0% | +103.0% / +111.2% | +94.4% / +65.4% | +61.0% / +48.4% |
| String | +46.7% / +58.1% | −5.8% / −9.0% | +2.2% / +1.5% | +1.9% / +35.0% | −15.2% / −20.2% |
| Uint64 | +178.9% / +185.6% | +106.0% / +114.2% | +105.9% / +107.3% | +93.3% / +133.6% | +29.2% / +44.0% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.7% / +26.5% | +18.3% / +26.8% | +21.3% / +32.4% | +63.9% / +52.2% | +25.1% / +37.9% |
| Apache Builds (pretty) | +5.9% / +25.7% | +18.6% / +29.5% | +22.3% / +33.3% | +63.3% / +54.8% | +23.5% / +45.2% |
| Canada (min) | −5.2% / −4.1% | +5.2% / +6.8% | −11.1% / −11.0% | −6.4% / −7.9% | +6.1% / +6.9% |
| Canada (pretty) | +6.5% / +9.3% | +3.2% / +4.1% | −5.7% / −4.4% | +15.7% / +8.6% | +9.3% / −6.2% |
| CitmCatalog (min) | +8.7% / +15.9% | +10.5% / +12.7% | +12.4% / +15.3% | +5.4% / +9.0% | tie / +18.6% |
| CitmCatalog (pretty) | +13.0% / +16.1% | +5.8% / +6.9% | +9.4% / +10.1% | +15.4% / +14.0% | +4.9% / +1.9% (n/c) |
| Discord (min) | +16.4% / +21.4% | +17.2% / +26.0% | +15.7% / +18.4% | +24.0% / +15.9% | +11.9% / tie |
| Discord (pretty) | +12.9% / +16.8% | +16.0% / +20.3% | +8.2% / +10.8% | +22.2% / +22.6% | +26.6% / +22.9% |
| Github Events (min) | +31.9% / +43.9% | +34.9% / +43.2% | +29.8% / +33.4% | +46.1% / +55.0% | +49.7% / +56.4% |
| Github Events (pretty) | +20.5% / +37.9% | +28.0% / +34.1% | +25.4% / +30.1% | +32.6% / +43.3% | +39.6% / +44.4% |
| Google Maps (min) | +29.0% / +25.6% | +23.1% / +25.4% | +27.0% / +32.3% | +3.8% / +26.6% | +18.1% / +15.9% |
| Google Maps (pretty) | +21.1% / +33.3% | +11.1% / +14.0% | +21.9% / +24.7% | +28.5% / +34.7% | +17.0% / +15.8% |
| Instruments (min) | +30.3% / +24.2% | +10.7% / +7.3% | +8.6% / +8.0% | +2.3% / +13.7% | +3.8% / tie |
| Instruments (pretty) | +28.7% (n/c) / +34.2% | +1.9% / +3.5% | +2.5% / +2.3% | tie / +20.2% | tie / +10.6% |
| Marine IK (min) | tie / −9.0% | +7.9% / +8.8% | −5.0% / −4.7% | −5.8% (n/c) / +2.9% (n/c) | −6.0% / tie (n/c) |
| Marine IK (pretty) | −1.9% / tie | +7.7% / +8.2% | +1.1% / tie | +6.1% / tie | +8.6% / +5.3% |
| Random (min) | +19.0% / +34.7% | +25.2% / +28.5% | +34.6% / +39.0% | +48.7% / +62.3% | +34.7% / +40.3% |
| Random (pretty) | +17.5% / +29.9% | +24.4% / +28.3% | +31.4% / +38.7% | +50.4% / +56.4% | +29.8% / +34.2% |
| Twitter (min) | +28.0% / +31.4% | +14.9% / +26.2% | +11.5% / +14.2% | +25.0% / +7.5% | +8.0% / +4.4% |
| Twitter (pretty) | +25.8% / +33.0% | +9.5% / +16.6% | +3.2% / +7.4% | +18.0% / +6.2% (n/c) | +6.4% / +10.9% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +44.8% / +68.2% | +60.1% / +82.4% | +57.4% / +76.2% | +39.2% / +87.0% | +26.9% / +81.8% |
| Apache Builds (pretty) | +47.2% / +70.2% | +50.1% / +65.9% | +57.0% / +70.6% | +52.0% / +66.3% | +11.5% / +85.5% |
| Canada (min) | tie (n/c) / tie (n/c) | +9.1% / +8.7% | −5.3% / −7.6% | −6.7% / −4.8% | −1.5% / +14.2% |
| Canada (pretty) | +5.4% (n/c) / tie (n/c) | +9.5% / +11.9% | tie / tie | +26.5% / +28.0% | tie (n/c) / +19.8% |
| CitmCatalog (min) | +57.4% / +62.0% | +66.7% / +70.2% | +58.9% / +64.2% | +65.9% / +88.1% | +71.5% / +69.0% |
| CitmCatalog (pretty) | +59.0% / +60.9% | +57.8% / +61.6% | +54.0% / +58.4% | +46.7% / +38.8% | +90.6% (n/c) / +60.2% |
| Discord (min) | +195.7% / +210.9% | +276.8% / +301.7% | +232.1% / +256.3% | +236.8% / +256.5% | +310.9% / +319.3% |
| Discord (pretty) | +164.4% / +187.9% | +234.9% / +253.4% | +200.6% / +218.6% | +221.8% / +249.0% | +251.6% / +300.3% |
| Github Events (min) | +144.9% / +183.9% | +145.2% / +170.6% | +121.6% / +142.1% | +125.3% / +113.5% | +151.6% / +199.6% |
| Github Events (pretty) | +130.0% / +159.4% | +123.3% / +138.7% | +97.6% / +111.8% | +93.7% / +110.0% | +156.8% / +178.3% |
| Google Maps (min) | +20.6% / +22.1% | +11.2% / +11.7% | +15.1% / +19.3% | +51.0% / +36.4% | +20.4% / +21.6% |
| Google Maps (pretty) | +24.5% / +24.0% | +8.0% / +9.4% | +12.0% / +12.1% | +13.0% / −2.4% | +11.0% / +12.1% |
| Instruments (min) | +184.6% / +198.0% | +189.9% / +198.2% | +156.6% / +162.2% | +153.7% / +150.7% | +171.9% / +212.2% |
| Instruments (pretty) | +170.6% (n/c) / +179.2% | +164.5% / +170.9% | +139.2% / +143.7% | +120.9% / +81.5% | +148.2% / +191.1% |
| Marine IK (min) | +24.7% / +27.7% | +49.8% / +54.5% | +40.5% / +40.9% | +46.9% (n/c) / +43.3% | +53.3% / +52.3% (n/c) |
| Marine IK (pretty) | +24.2% / +22.8% | +54.1% / +55.0% | +44.7% / +46.6% | +65.3% / +36.0% | +53.7% / +53.4% |
| Random (min) | +57.9% (n/c) / +72.3% | +94.7% / +110.6% | +88.6% / +102.3% | +72.4% / +111.7% | +100.0% / +137.5% |
| Random (pretty) | +58.0% / +70.1% | +87.4% / +95.7% | +73.1% / +85.3% | +50.9% / +102.8% | +106.1% / +105.9% |
| Twitter (min) | +227.9% / +277.4% | +172.4% / +179.5% | +130.5% / +142.7% | +100.6% / +123.2% | +222.1% / +240.5% |
| Twitter (pretty) | +178.6% / +222.6% (n/c) | +142.3% / +153.6% | +105.2% / +112.7% | +101.0% / +109.9% | +190.5% / +220.8% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +29.5% / +38.6% | +36.3% / +47.4% | +37.8% / +49.6% | +48.9% / +48.3% | +38.2% / +53.0% |
| Apache Builds (pretty) | +24.4% / +34.7% | +27.0% / +36.7% | +30.8% / +41.1% | +53.3% / +36.6% | +25.7% / +26.6% |
| Canada (min) | +64.4% / +54.4% | +10.4% / +12.9% | tie / tie | +37.3% / +6.8% | +71.1% / +34.0% |
| Canada (pretty) | +81.1% / +85.8% | +10.6% / +7.3% | +13.0% / +13.8% | +15.8% / +8.9% | +26.7% (n/c) / +12.5% |
| CitmCatalog (min) | +28.1% / +29.7% | +5.9% / +6.6% | +7.2% / +6.8% | +15.3% / +34.0% | +17.4% / −3.2% |
| CitmCatalog (pretty) | +54.7% / +59.7% | +2.1% / −0.5% | +8.1% / +9.9% | +41.2% / +21.8% | +17.8% / +11.7% |
| Discord (min) | +18.5% / +23.9% | +4.7% / +6.7% | +20.9% / +26.7% | +21.4% / +32.8% | +11.0% / +43.3% |
| Discord (pretty) | +25.5% / +30.3% | +4.5% / +4.5% | +19.4% / +23.4% | +21.7% / +30.5% | tie / +19.8% |
| Github Events (min) | +51.2% / +63.6% | +15.4% / +17.8% | +21.0% / +22.5% | +32.8% / +39.0% | +8.2% / +12.5% |
| Github Events (pretty) | +49.0% / +59.0% | +9.5% / +14.5% | +14.1% / +19.4% | +20.1% / +22.4% | +29.6% / +16.6% |
| Google Maps (min) | +25.3% / +25.9% | +15.1% / +16.7% | +19.4% / +20.4% | +3.2% / +24.9% | +7.1% / +18.4% |
| Google Maps (pretty) | +25.6% / +27.7% | +9.5% / +10.7% | +13.8% / +15.2% | +51.7% / +54.0% | +38.0% / +47.2% |
| Instruments (min) | +29.3% / +41.8% | +12.3% / +10.1% | +10.5% / +10.8% | +13.5% / +11.9% | +33.0% / +7.6% |
| Instruments (pretty) | +30.6% / +36.3% | +8.0% / +8.3% | +5.5% / +6.2% | +16.8% / +1.6% | tie / −4.8% |
| Marine IK (min) | +28.1% / +33.0% | +11.3% / +9.7% | −6.7% / −9.6% | +11.3% / −3.4% | +32.9% / +7.4% |
| Marine IK (pretty) | +65.5% / +50.5% | tie / +4.8% | −9.4% / −8.0% (n/c) | +7.4% / tie | +17.8% / +30.7% |
| Random (min) | +27.8% / +32.3% | +28.0% / +30.8% | +22.8% / +27.9% | +41.0% / +45.8% | +21.1% / +47.5% |
| Random (pretty) | +27.9% / +31.0% | +17.9% / +23.3% | +11.5% / +15.8% | +19.6% / +19.7% | +33.4% / +28.8% |
| Twitter (min) | +52.0% / +59.2% | +18.6% / +19.8% | +15.4% / +14.6% | +42.9% / +24.5% | +16.9% / +17.5% |
| Twitter (pretty) | +53.9% / +61.4% | +5.6% / +5.5% | +13.0% / +13.1% | +26.4% / +25.7% | +12.8% / tie |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +19.6% / +26.4% | +15.2% / +17.2% | +17.3% / +18.2% | +39.7% / +45.9% | +24.4% / +32.6% |
| Apache Builds (pretty) | +19.9% / +31.0% | +7.0% / +9.6% | +14.7% / +16.1% | +40.2% / +39.8% | +22.1% / +18.2% |
| Canada (min) | +38.0% / +32.5% | +7.9% / +7.9% | tie / −2.3% | +1.7% / +47.1% (n/c) | +52.8% / +51.7% |
| Canada (pretty) | +65.8% / +64.0% | +11.8% / +10.0% | +11.8% / +9.8% | +6.7% (n/c) / +26.9% | +27.9% / +30.4% |
| CitmCatalog (min) | +19.8% / +11.5% | +7.8% / +8.4% | +4.5% / +5.5% | +12.4% / −4.2% | tie / −11.7% |
| CitmCatalog (pretty) | +22.5% / +33.2% | +6.1% / +6.1% | +4.8% / +5.3% | +28.4% / +27.2% | +29.8% / +29.6% |
| Discord (min) | +38.4% / +49.5% | +43.8% / +52.9% | +43.5% / +52.1% | +46.6% / +24.5% | +71.7% / +74.1% |
| Discord (pretty) | +39.3% (n/c) / +53.4% | +36.3% / +43.9% | +37.1% / +42.1% | +47.6% / +43.2% | +50.7% / +43.8% |
| Github Events (min) | +67.8% / +75.3% | +42.2% / +46.7% | +43.5% / +46.5% | +43.4% / +20.0% | +43.9% / +46.7% |
| Github Events (pretty) | +61.2% / +68.4% | +35.2% / +38.0% | +37.8% / +39.5% | +72.1% / +18.5% | +43.4% / +37.5% |
| Google Maps (min) | +23.1% / +33.2% | +7.2% / +7.2% | +7.2% / +8.4% | +23.5% / +30.5% | −5.2% / −4.4% |
| Google Maps (pretty) | +26.7% / +32.4% | +3.4% / +2.7% | +8.0% / +9.6% | +7.8% / +4.2% | +4.3% / +17.1% |
| Instruments (min) | +39.6% / +46.2% | +26.9% / +28.2% | +23.3% / +23.6% | +9.6% / +8.3% | +45.1% / +36.1% |
| Instruments (pretty) | +43.5% / +47.8% | +13.6% / +15.1% | +19.3% / +22.0% | +11.7% / +4.6% | +30.7% / +10.9% |
| Marine IK (min) | +48.4% / +47.2% | +40.5% / +41.7% | +24.1% / +23.5% | +27.0% / +24.6% | +26.3% / +29.5% |
| Marine IK (pretty) | +70.9% / +75.9% | +26.4% / +27.0% | +17.7% / +10.4% | +23.3% / +32.0% | +30.6% / +31.5% |
| Random (min) | +33.2% / +36.5% | +30.1% / +34.3% | +21.9% / +25.1% | +43.2% / +81.6% | +45.6% / +62.7% |
| Random (pretty) | +33.7% / +35.6% | +18.8% / +18.5% | +18.4% / +22.5% | +50.4% / +40.5% | +29.3% / +36.4% |
| Twitter (min) | +64.6% (n/c) / +63.8% | +29.5% / +30.5% | +25.7% / +25.7% | +20.8% / +41.1% | +40.7% / +39.2% |
| Twitter (pretty) | +52.9% / +56.0% | +20.7% / +21.1% | +20.4% / +21.7% | +27.3% / +24.3% | +17.7% / +20.4% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +9.7% / +18.7% | +12.3% / +19.1% | +26.2% / +35.8% | +19.8% / +41.4% | +13.8% / +23.1% |
| Apache Builds (pretty) | +8.7% / +21.2% | +14.3% / +18.4% | +21.2% / +32.8% | +54.0% / +28.7% | +11.5% / +16.9% |
| Canada (min) | tie / −1.0% | +2.0% / +3.1% | −11.4% / −14.7% | −4.6% / +6.6% | +7.1% / −2.1% |
| Canada (pretty) | +1.5% / +2.4% | +1.3% / +1.5% | −8.6% / −8.7% | tie / tie | +10.4% / +2.9% |
| CitmCatalog (min) | +8.0% / +9.9% | −4.4% / +1.1% | +1.0% / +2.6% | +9.7% / +21.1% | −6.3% / +17.7% |
| CitmCatalog (pretty) | +9.8% / +15.0% | −4.0% / −2.5% | +1.2% / +2.0% | +3.4% / +17.3% | +13.3% / +6.3% |
| Discord (min) | +5.9% / +14.5% | +7.9% / +14.4% | +5.1% / +10.3% | +26.3% / +41.8% | +8.3% / +18.4% |
| Discord (pretty) | +6.7% / +16.2% | +4.8% / +11.2% | −2.1% / +4.6% | +26.4% / +37.8% | +10.3% / +9.0% |
| Github Events (min) | +19.7% / +29.0% | +16.7% / +18.9% | +8.3% / +11.5% | +18.1% / +22.8% | +22.3% / +35.7% |
| Github Events (pretty) | +20.6% / +23.3% | +10.5% / +11.6% | +5.1% / +4.1% | +17.0% / +18.2% | +14.5% / tie |
| Google Maps (min) | +28.4% / +30.3% | +19.5% / +21.1% | +25.6% / +27.2% | +33.6% / +30.8% | +14.1% / +15.0% |
| Google Maps (pretty) | +29.6% / +35.0% | +11.9% / +13.6% | +19.4% / +21.1% | +29.1% / +36.1% | +20.5% / +2.7% |
| Instruments (min) | +28.9% / +31.4% | +1.4% / +2.9% | +1.9% / +2.6% | +13.5% / +16.8% | −2.1% / +12.2% |
| Instruments (pretty) | +28.0% / +31.5% | −1.9% / −2.9% | −2.2% / −0.3% | +10.4% / +10.4% | −9.5% / −2.4% |
| Marine IK (min) | −1.2% / −1.1% | +0.9% / +4.1% | −2.5% / −1.6% | tie / +1.4% | tie / tie |
| Marine IK (pretty) | +4.9% / +4.8% | +2.2% / +2.9% | +0.3% / tie | +1.4% / +5.6% | +13.9% / −7.1% |
| Random (min) | +23.0% / +28.5% | +17.6% / +21.2% | +20.5% / +24.5% | +46.3% / +56.4% | +29.3% / +40.4% |
| Random (pretty) | +19.3% / +25.8% | +15.4% / +18.4% | +20.1% / +24.3% | +60.4% / +51.5% | +33.5% / +28.1% |
| Twitter (min) | +15.5% / +28.2% (n/c) | +5.0% / +6.0% | +6.4% / +10.5% | +14.6% / +30.0% | +12.9% / +7.0% |
| Twitter (pretty) | +18.6% / +22.0% | +3.9% / +8.6% | +7.4% / +10.7% | +46.1% / tie | +1.4% / +7.1% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +54.2% / +87.0% | +78.6% / +89.9% | +81.5% / +94.9% | +45.2% / +167.9% | +85.6% / +113.5% |
| Apache Builds (pretty) | +44.7% / +74.2% | +61.2% / +70.7% | +72.5% / +83.9% | +17.6% / +75.7% | +37.9% / +111.2% |
| Canada (min) | +1.0% / +0.7% | +5.2% / +4.5% | −11.0% / −9.5% | tie / −7.0% | +5.6% / −1.1% |
| Canada (pretty) | +3.9% / +4.5% | +4.0% / +3.3% | −4.6% / −6.0% | tie / −9.4% | +17.3% / −3.9% |
| CitmCatalog (min) | +56.8% / +58.5% | +60.0% / +69.4% | +58.1% / +63.8% | +44.4% / +66.1% | +52.3% / +79.4% |
| CitmCatalog (pretty) | +49.2% / +49.9% | +48.8% / +55.7% | +52.2% / +58.8% | +40.1% / +45.2% | +77.3% / +55.8% |
| Discord (min) | +196.7% / +249.9% | +246.4% / +287.7% | +216.8% / +246.2% | +202.3% / +255.0% | +326.5% / +400.7% |
| Discord (pretty) | +177.8% / +217.6% | +222.3% / +253.0% | +194.6% / +225.1% | +179.1% / +191.5% | +315.1% / +280.9% |
| Github Events (min) | +45.2% / +52.3% | +47.4% / +53.4% | +29.9% / +36.4% | +43.8% / +39.7% | +86.4% / +82.5% |
| Github Events (pretty) | +35.7% / +39.8% | +31.2% / +33.0% | +23.1% / +24.6% | +22.8% / +12.0% | +70.2% / +70.0% |
| Google Maps (min) | +27.9% / +26.4% | +8.7% / +14.1% | +13.7% / +14.4% | +28.7% / +26.0% | +31.8% / +25.1% |
| Google Maps (pretty) | +31.8% / +29.0% | +6.1% / +7.7% | +11.8% / +10.9% | +19.5% / +22.7% | +9.4% / +7.6% |
| Instruments (min) | +156.1% / +167.5% | +186.7% / +193.1% | +150.8% / +154.2% | +115.4% / +117.4% | +177.6% / +222.6% |
| Instruments (pretty) | +146.1% / +151.8% | +163.2% / +162.6% | +135.9% / +141.8% | +124.9% / +144.6% | +175.9% / +182.6% |
| Marine IK (min) | +26.6% / +28.4% | +44.1% / +45.4% | +39.2% / +40.5% | +35.2% / +46.7% | +55.0% / +50.1% |
| Marine IK (pretty) | +27.8% / +36.8% | +36.4% / +40.9% | +36.5% / +38.6% | +22.1% / +42.8% | +36.9% / +44.7% |
| Random (min) | +63.1% / +74.4% | +87.9% / +98.2% | +82.8% / +91.3% | +89.8% / +109.9% | +115.1% / +132.5% |
| Random (pretty) | +54.7% / +64.5% | +76.1% / +85.4% | +69.5% / +77.1% | +64.6% / +66.3% | +91.0% / +83.0% |
| Twitter (min) | +169.8% / +168.1% | +166.8% / +177.1% | +133.1% / +137.8% | +124.2% / +113.4% | +222.4% / +237.6% |
| Twitter (pretty) | +140.3% / +157.1% | +144.7% / +154.0% | +122.2% / +123.0% | +118.4% / +128.6% | +224.4% / +192.0% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.4% / +49.0% | +30.1% / +33.7% | +38.5% / +47.0% | +48.1% / +121.7% | +28.5% / +33.1% |
| Apache Builds (pretty) | +26.5% / +40.5% | +24.5% / +25.4% | +27.2% / +33.8% | +60.4% / +30.1% | +24.4% / +17.0% |
| Canada (min) | +57.7% / +68.2% | +11.5% / +10.9% | +23.6% / +24.7% | +23.3% / +19.5% | +5.5% / +4.5% |
| Canada (pretty) | +83.0% / +93.3% | +13.9% / +12.3% | +24.7% / +24.1% | +14.9% / +9.6% | −6.3% / +1.2% |
| CitmCatalog (min) | +24.6% / +36.4% | +8.8% / +7.8% | +7.4% / +7.3% | +20.9% / +36.1% | +21.2% / +13.8% |
| CitmCatalog (pretty) | +37.5% / +46.7% | −0.8% / −0.4% | +8.8% / +8.0% | +14.3% / +17.4% | +25.3% / +26.8% |
| Discord (min) | +22.9% / +32.0% | +13.2% / +16.4% | +11.7% / +15.8% | +23.8% / +29.2% | −4.4% / +19.2% |
| Discord (pretty) | +18.4% / +26.9% | +5.2% / +7.0% | +10.3% / +15.1% | +20.4% / +28.2% | +15.5% / −14.6% |
| Github Events (min) | +39.7% / +49.3% | +13.2% / +12.5% | +8.6% / +12.5% | +26.8% / +37.2% | +57.8% / −2.4% |
| Github Events (pretty) | +46.2% / +39.1% | +12.9% / +10.7% | +11.5% / +14.3% | +18.2% / +21.6% | −3.4% / +10.3% |
| Google Maps (min) | +23.7% / +30.1% | +12.1% / +12.8% | +14.9% / +14.7% | +20.6% / +22.4% | +12.0% / +32.9% |
| Google Maps (pretty) | +21.9% / +26.7% | +7.8% / +7.8% | +11.9% / +10.7% | +19.9% / +16.2% | +2.2% / +12.8% |
| Instruments (min) | +25.8% / +30.5% | +5.3% / +5.9% | +2.3% / +3.7% | +2.9% / +7.6% | +20.8% / +26.0% |
| Instruments (pretty) | +28.3% / +25.5% | +2.8% / +1.8% | −1.4% / +1.7% | +5.3% / +9.3% | +4.8% / −10.6% |
| Marine IK (min) | +14.4% / +22.0% | −9.7% / −9.6% | +0.9% / +2.1% | +3.9% / tie | +6.3% / +2.7% |
| Marine IK (pretty) | +62.6% / +68.0% | +2.6% / +2.7% | +8.1% / +8.3% | tie / +13.0% | +2.5% / −3.0% |
| Random (min) | +19.8% / +26.5% | +22.3% / +25.2% | +19.1% / +19.3% | +30.2% / +33.3% | +31.8% / +32.3% |
| Random (pretty) | +18.3% / +20.6% | +16.9% / +14.8% | +10.3% / +12.5% | +8.3% / +17.8% | +16.5% / +27.0% |
| Twitter (min) | +14.8% / +19.4% | +14.5% / +9.4% | +4.1% / +6.6% | +25.6% / +7.0% | +7.9% / +7.6% |
| Twitter (pretty) | +13.5% / +17.3% | +0.8% / tie | +9.0% / +8.0% | −7.6% / −3.5% | tie / −0.8% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +23.3% (n/c) / +35.8% | +11.7% / +9.3% | +21.2% / +25.3% | tie / +92.5% | +16.6% / +26.2% |
| Apache Builds (pretty) | +18.9% / +29.9% | +5.0% / +2.0% | +14.7% / +17.4% | +21.8% / +39.1% | +9.0% / +12.6% |
| Canada (min) | +48.2% / +45.1% | +6.3% / +5.2% | +27.8% / +29.1% | +34.9% / +15.0% | −11.3% / +34.7% |
| Canada (pretty) | +72.4% / +73.5% | +6.6% / +6.7% | +28.4% / +28.4% | +16.1% / +24.2% | tie / −3.1% |
| CitmCatalog (min) | +24.0% / +22.3% | +10.2% / +10.8% | +4.0% / +7.8% | +9.7% / +11.8% | +14.2% / +15.0% |
| CitmCatalog (pretty) | +30.0% / +33.0% | +7.4% / +3.3% | +2.9% / +1.1% | +4.7% / +2.1% | +1.5% / −5.4% |
| Discord (min) | +38.6% / +54.4% | +49.4% / +52.2% | +43.3% / +49.1% | +47.4% / +54.8% | +66.2% / +87.0% |
| Discord (pretty) | +39.8% / +49.9% | +36.0% / +39.9% | +36.4% / +40.4% | +25.7% / +38.3% | +49.1% / +64.3% |
| Github Events (min) | +49.6% / +46.8% | +30.4% / +31.4% | +29.0% / +30.0% | +31.7% / +35.7% | +29.2% / +39.0% |
| Github Events (pretty) | +40.2% / +44.0% | +25.3% / +26.3% | +22.3% / +28.9% | +29.6% / +17.5% | +23.7% / +11.4% |
| Google Maps (min) | +20.8% / +22.7% | +6.6% / +6.2% | +7.3% / +7.4% | +13.8% / +18.7% | +17.3% / −9.0% |
| Google Maps (pretty) | +21.1% / +23.6% | tie / +0.5% | +2.7% / +6.1% | +31.6% / +15.7% | +4.1% / +9.4% |
| Instruments (min) | +35.4% / +40.2% | +28.8% / +35.2% | +24.3% / +25.2% | +29.2% / +34.5% | +63.8% / +21.6% |
| Instruments (pretty) | +38.1% / +43.4% | +18.7% / +21.4% | +18.6% / +19.4% | +21.9% / +18.4% | +23.6% / +19.6% |
| Marine IK (min) | +38.0% / +41.9% | +28.8% / +31.4% | +43.4% / +44.1% | +41.4% / +46.8% | +34.5% / +35.0% |
| Marine IK (pretty) | +45.3% / +47.4% | +19.3% / +19.9% | +38.9% / +39.7% | +32.5% / +41.6% | +24.6% / +21.2% |
| Random (min) | +31.7% / +34.0% | +34.1% / +35.1% | +27.7% / +23.8% | +18.3% / +34.1% | +42.0% / +46.6% |
| Random (pretty) | +28.2% / +31.9% | +21.2% / +19.3% | +19.6% / +20.6% | +29.6% / +12.9% | +37.7% / +34.5% |
| Twitter (min) | +28.6% / +31.0% | +21.0% / +18.3% | +15.9% / +15.3% | +19.0% (n/c) / +27.7% | +20.7% / +25.9% |
| Twitter (pretty) | +22.9% / +19.6% | +12.6% / +10.8% | +15.6% / +14.9% | +6.4% / +16.7% | +26.0% / +10.1% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.8x to 4.2x faster at full size: +81.5% (Instruments pretty, macOS/GCC, reused) up to +319.3% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +113.4% to +400.7%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −7.6% (minified Canada on Linux/GCC, reused) to +51.0%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events rarely loses.** It accounts for 2 of the 89 single-document losses, both sparse reads of Github Events Small on macOS/Clang (minified reused, −2.4%; prettified fresh, −3.4%). Read in reverse at full size it wins on every build (+93.7% to +199.6%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 193 of 200 full-size sparse-reverse results across both allocation modes (2 ties, 5 losses), and 193 of 200 small ones (3 ties, 4 losses). MSVC won all 80. Every loss is on Canada, CitmCatalog or Google Maps. The full-size losses are minified Canada on Linux/GCC (reused, −2.3%), minified CitmCatalog on macOS/GCC (reused, −4.2%) and on macOS/Clang (reused, −11.7%), and minified Google Maps on macOS/Clang (−5.2% fresh, −4.4% reused). All 4 small losses are on macOS/Clang (−3.1% to −11.3%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 235.3 MB/s on MSVC against 268.7 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 622.1 and 900.0 MB/s. Every MSVC full-size sparse result is a win, by +18.5% to +85.8%, and every full-size sparse-reverse result by +11.5% to +75.9%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 185 of 200 full-size sparse results (7 ties, 8 losses) and 181 of 200 small ones (4 ties, 15 losses). MSVC won all 40 of its full-size results and all 40 small. The full-size losses are on Linux/GCC (4: Marine IK, −6.7% to −9.6%), macOS/Clang (2: minified CitmCatalog, reused, −3.2%, and prettified Instruments, reused, −4.8%), Linux/Clang (1: prettified CitmCatalog, reused, −0.5%) and macOS/GCC (1: minified Marine IK, reused, −3.4%). macOS/Clang holds 8 of the 15 small losses, Linux/Clang 4, macOS/GCC 2 and Linux/GCC 1.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 175, tied 10 and lost 15. Linux/Clang went 40-0-0, macOS/GCC 35-2-3, MSVC 34-2-4, macOS/Clang 33-5-2 and Linux/GCC 33-1-6 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 167, tied 9 and lost 24. MSVC went 36-1-3, macOS/GCC 35-4-1, Linux/Clang 35-0-5, macOS/Clang 31-3-6 and Linux/GCC 30-1-9. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −1.9% to −4.4%, led by CitmCatalog and Instruments, and Linux/GCC's from −0.3% to −14.7%, led by Canada. The largest is minified Canada Small on Linux/GCC (reused, −14.7%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−4.4% to −11.1%) and minified Marine IK in both runs (−5.0% and −4.7%). MSVC lost minified Canada in both runs (−5.2% and −4.1%), minified Marine IK (reused, −9.0%) and prettified Marine IK (fresh, −1.9%). macOS/GCC lost minified Canada in both runs (−6.4% and −7.9%) and minified Marine IK (fresh, −5.8%, n/c). macOS/Clang lost prettified Canada (reused, −6.2%) and minified Marine IK (fresh, −6.0%). Linux/Clang lost none. Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 45-1-4 on them. Double, Int64 and Uint64 won all 10 results each (+60.3% to +229.8%, +48.4% to +178.3% and +29.2% to +185.6%), and Bool won 9 (+6.6% to +259.3%), tying only on macOS/Clang (fresh, −1.3%). String is the exception: 6 wins and 4 losses, on Linux/Clang (−5.8% and −9.0%) and macOS/Clang (−15.2% and −20.2%), each in both runs.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 5.9% to 12.7% below its own NDJSON throughput on the same records, in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 23 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 22 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 24 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 5-1-0 | 12-0-0 | 12-0-0 | 29-1-0 |
| Linux / GCC | 6-0-0 | 11-1-0 | 12-0-0 | 29-1-0 |
| macOS / GCC | 5-1-0 | 12-0-0 | 12-0-0 | 29-1-0 |
| macOS / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| **All** | **26-2-2** | **59-1-0** | **60-0-0** | **145-3-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +26.2% | +19.8% | +21.4% | +37.0% | +19.4% |
| Large Amazon Cellphones (NDJSON) | +16.9% | +10.9% | +5.9% | tie (n/c) | +14.6% |
| Stream Formats Small (NDJSON) | +41.0% | +13.8% | +30.8% | +13.5% (n/c) | +8.8% |
| Stream Formats Small (comma) | +68.0% | +113.0% | +98.6% | +123.5% | +58.5% |
| Stream Formats Large (NDJSON) | +85.4% | tie | +34.9% | +25.2% (n/c) | −8.0% |
| Stream Formats Large (comma) | +96.3% (n/c) | +16.7% | +34.7% | +11.0% | −3.7% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +25.0% | +28.8% | +27.9% | +39.7% | +34.9% |
| Apache Builds (comma) | +30.5% | +47.6% | +41.4% | +70.9% | +38.9% |
| CitmCatalog (NDJSON) | +11.8% | +7.2% | +13.5% | +17.2% | +9.6% |
| CitmCatalog (comma) | +20.4% | +32.9% | +36.2% | +25.1% | +26.0% |
| Google Maps (NDJSON) | +29.0% | +28.6% | +24.1% | +27.3% | +14.1% |
| Google Maps (comma) | +39.8% (n/c) | +48.3% | +41.5% | +33.3% | +20.1% |
| Instruments (NDJSON) | +42.4% | +14.5% | +15.1% | +8.8% | +6.2% |
| Instruments (comma) | +55.3% | +48.6% | +54.7% | +42.9% | +27.6% |
| Random (NDJSON) | +23.1% (n/c) | +33.8% | +28.9% | +61.9% | +65.9% |
| Random (comma) | +33.1% | +46.7% | +44.9% | +55.1% | +78.1% (n/c) |
| Twitter (NDJSON) | +21.9% (n/c) | +15.5% | tie | +3.7% | +10.5% |
| Twitter (comma) | +24.4% | +28.5% | +23.0% | +26.1% (n/c) | +16.4% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +20.9% (n/c) | +17.7% | +12.3% | +10.1% | +25.6% |
| Apache Builds (comma) | +34.8% | +35.4% | +28.6% | +20.0% | +35.7% |
| CitmCatalog (NDJSON) | +39.0% (n/c) | +34.8% | +29.0% | +22.4% | +40.3% |
| CitmCatalog (comma) | +53.7% | +56.6% | +48.2% | +62.5% | +61.4% (n/c) |
| Google Maps (NDJSON) | +18.1% | +12.9% | +7.6% | +16.5% | +10.4% |
| Google Maps (comma) | +26.8% | +27.6% | +18.2% | +28.2% | +18.3% |
| Instruments (NDJSON) | +48.9% | +51.6% | +19.3% | +28.4% (n/c) | +52.7% (n/c) |
| Instruments (comma) | +70.3% (n/c) | +79.8% | +41.2% | +60.4% (n/c) | +79.1% |
| Random (NDJSON) | +58.0% | +92.1% | +77.7% | +86.5% (n/c) | +111.9% |
| Random (comma) | +66.8% | +110.2% | +89.7% | +92.0% | +155.4% (n/c) |
| Twitter (NDJSON) | +249.4% | +178.3% | +139.0% | +131.4% | +291.3% (n/c) |
| Twitter (comma) | +240.0% | +187.6% | +154.4% | +149.7% (n/c) | +233.3% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 26 of the 30 results in that group, tied 2 and lost 2. It won Stream Formats Small (comma) (+58.5% to +123.5%), Stream Formats Small (NDJSON) (+8.8% to +41.0%) and Amazon Cellphones (+19.4% to +37.0%) on all five builds, and Large Amazon Cellphones on four (+5.9% to +16.9%), tying on macOS/GCC (−2.1%). Both losses are Stream Formats Large on macOS/Clang (NDJSON, −8.0%; comma-separated, −3.7%), and the other tie is Stream Formats Large (NDJSON) on Linux/Clang (+0.2%).
- **Windows/MSVC won every streaming test,** 30-0-0. Linux/Clang, Linux/GCC and macOS/GCC each went 29-1-0, and macOS/Clang 28-0-2. Both streaming losses are Stream Formats Large reads on macOS/Clang.
- **In-order corpus streams never lose.** Of the 60 in-order corpus results, Jsonifier won 59 and tied 1, Twitter (NDJSON) on Linux/GCC (+1.2%).
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +131.4% to +291.3%, while Google Maps rows, which have few, give +7.6% to +28.2%. Instruments, whose reverse streams have the most room to lose, wins on every build (+19.3% to +79.8%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 246.0 MB/s under MSVC and 483.8 MB/s under Linux/Clang, while Jsonifier reads it at 588.0 and 884.7 MB/s.

## 9. Limitations

- **macOS/Clang and Linux/GCC hold the most single-document losses:** 30 and 27 of the 89, against 13 on macOS/GCC, 12 on Linux/Clang and 7 on MSVC. 20 of macOS/Clang's are small documents, led by 8 small sparse, and 14 of Linux/GCC's, led by 9 small in-order. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD test (reused, −20.2%; fresh, −15.2%) and Linux/GCC's minified Canada Small read in order (reused, −14.7%), which is also the largest x86 loss.
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 48 of the 89: Canada 33 (15 on Linux/GCC, 8 on macOS/Clang, 7 on macOS/GCC), CitmCatalog 11 (6 on Linux/Clang, 4 on macOS/Clang) and Google Maps 4 (3 on macOS/Clang). Marine IK, the other float-dense document, adds 19. Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 15 of its 27 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 51 of the 89 losses are small documents (20 on macOS/Clang, 14 on Linux/GCC, 9 on Linux/Clang, 5 on macOS/GCC, 3 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats reads hold both streaming losses:** Stream Formats Large on macOS/Clang (NDJSON, −8.0%, and comma-separated, −3.7%). No corpus record stream loses.
- **Some results did not converge.** 26 of the 1,650 single-document results did not converge: 12 on Windows/MSVC, 7 on macOS/GCC, 6 on macOS/Clang and 1 on Linux/GCC. 21 of the 150 streaming results did not converge: 8 on macOS/GCC, 7 on Windows/MSVC and 6 on macOS/Clang. Linux/Clang converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,517, tied 44 and lost 89. On reverse reads it won 378 of 400 at both sizes, and on Windows/MSVC it lost 7 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (761-25-39 fresh, 756-19-50 reused). The main remaining targets are the small-object and float-heavy documents (Canada, Google Maps, CitmCatalog and Marine IK, with 67 of the 89 losses), small documents in general, and the macOS/Clang and Linux/GCC builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 145, tied 3 and lost 2, and on the 120 corpus record-stream results it went 119-1-0. Stream Formats Large on macOS/Clang, which holds both losses, is the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.