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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,438 of 1,650 single-document results against simdjson On Demand, tied 61 and lost 151. On reverse-order reads it won 371 of 400. On Windows/MSVC it won 311 of 330 single-document results, tied 13 and lost 6.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 138 of 150 results, tied 9 and lost 3, and it won 117 of the 120 corpus record-stream results, tied 3 and lost none.

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
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 41 of the 1,650 single-document results and 25 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 158 / 170 | 151 / 160 | 309 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 166 / 170 | 154 / 160 | 320 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 162 / 170 | 158 / 160 | 320 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `79cbad3`, Jsonifier `2773d05` and simdjson `171066d58`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on November 3, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-2 | 18-1-1 | 20-0-0 | 20-0-0 | 17-2-1 | 18-2-0 | 20-0-0 | 20-0-0 | 154-7-4 |
| Linux / Clang | 4-0-1 | 17-2-1 | 19-0-1 | 20-0-0 | 16-0-4 | 12-1-7 | 19-0-1 | 13-4-3 | 14-2-4 | 134-9-22 |
| Linux / GCC | 4-0-1 | 17-0-3 | 18-0-2 | 17-1-2 | 20-0-0 | 13-0-7 | 17-0-3 | 19-0-1 | 19-1-0 | 144-2-19 |
| macOS / GCC | 5-0-0 | 16-3-1 | 20-0-0 | 19-0-1 | 20-0-0 | 17-0-3 | 20-0-0 | 17-3-0 | 18-0-2 | 152-6-7 |
| macOS / Clang | 3-0-2 | 15-2-3 | 18-0-2 | 14-3-3 | 19-0-1 | 16-0-4 | 18-0-2 | 15-1-4 | 13-1-6 | 131-7-27 |
| **All** | **21-0-4** | **81-9-10** | **93-1-6** | **90-4-6** | **95-0-5** | **75-3-22** | **92-2-6** | **84-8-8** | **84-4-12** | **715-31-79** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 18-2-0 | 20-0-0 | 20-0-0 | 18-2-0 | 20-0-0 | 19-0-1 | 20-0-0 | 157-6-2 |
| Linux / Clang | 4-0-1 | 19-0-1 | 19-0-1 | 20-0-0 | 16-1-3 | 13-2-5 | 19-0-1 | 13-2-5 | 16-1-3 | 139-6-20 |
| Linux / GCC | 3-0-2 | 17-0-3 | 18-0-2 | 18-0-2 | 19-1-0 | 15-0-5 | 17-0-3 | 17-1-2 | 19-0-1 | 143-2-20 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 19-0-1 | 19-1-0 | 20-0-0 | 20-0-0 | 18-2-0 | 19-1-0 | 158-5-2 |
| macOS / Clang | 3-1-1 | 15-2-3 | 17-1-2 | 14-1-5 | 16-1-3 | 18-0-2 | 18-1-1 | 12-1-7 | 13-3-4 | 126-11-28 |
| **All** | **20-1-4** | **86-5-9** | **92-3-5** | **91-1-8** | **90-4-6** | **84-4-12** | **94-1-5** | **79-6-15** | **87-5-8** | **723-30-72** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (41 of them did not converge; see §9), Jsonifier won 1,438, tied 61 and lost 151: 715-31-79 with fresh output objects and 723-30-72 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 185 of 200 full-size reverse results (4 ties, 11 losses) and 186 of 200 small reverse results (3 ties, 11 losses). macOS/GCC lost none of its 80 and Windows/MSVC lost one. Every loss is on Canada or Google Maps. The 11 full-size losses are Canada on Linux/GCC (minified −3.3% and −3.7%, prettified fresh −0.5%), prettified Google Maps on Linux/GCC (reused, −0.9%) and Linux/Clang (−0.2% and −0.6%), minified Canada on MSVC (fresh, −4.6%) and Google Maps on macOS/Clang in both forms and both runs (−5.6% to −6.6%). The 11 small losses are Canada Small on Linux/GCC (4), prettified Google Maps Small on Linux/GCC (2) and Linux/Clang (2), and Google Maps Small on macOS/Clang (3, −10.3% to −15.8%).
- **Windows/MSVC lost six of its 330 single-document results.** Three are full-size in-order reads: minified Canada (fresh, −2.0%) and minified Marine IK (−7.1% fresh, −10.7% reused). The other three are minified Canada Reverse (fresh, −4.6%), minified Marine IK Small in order (fresh, −5.8%) and prettified Google Maps Small sparse (reused, −24.2%). It won all 10 POD results and every full-size sparse and sparse-reverse result.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.3–14.5% for Jsonifier and 6.5–9.4% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.4 to +3.7 percentage points on the non-MSVC builds and by +6.7 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +292.3% to +319.6%, and Apache Builds (pretty) from +20.6% to +31.3%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +169.9% / +170.8% | +96.3% / +98.8% | +124.1% / +119.0% | +55.4% / +48.1% | −10.7% / tie |
| Double | +185.1% / +219.8% | +82.9% / +86.5% | +95.9% / +98.9% | +125.9% / +134.9% | +51.8% / +94.3% |
| Int64 | +178.9% / +188.4% | +90.9% / +96.6% | +67.2% / +70.3% | +72.5% / +103.1% | +38.3% / +34.4% |
| String | +36.8% / +71.7% | −6.9% / −9.6% | +0.5% / −3.6% | +27.6% / +35.0% | −10.3% / −21.5% |
| Uint64 | +181.0% / +186.7% | +89.5% / +97.9% | −2.4% / −2.0% | +94.7% / +58.4% | +32.9% / +41.1% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +11.5% (n/c) / +28.0% | +15.1% / +24.7% | +22.2% / +33.4% | +10.1% / +29.6% | +13.8% / +25.6% |
| Apache Builds (pretty) | +20.6% / +31.3% | +12.4% / +18.7% | +24.9% / +33.7% | +23.0% / +36.3% | +24.6% / +16.1% |
| Canada (min) | −2.0% / tie | +5.0% / +6.1% | −8.3% / −9.5% | +1.9% / +2.5% | tie / +7.5% |
| Canada (pretty) | tie / +3.6% | +5.6% / +6.5% | −5.0% / −5.5% | tie / +1.8% | +11.1% (n/c) / −6.5% (n/c) |
| CitmCatalog (min) | +24.2% / +24.5% | +5.4% / +6.1% | +13.2% / +17.7% | +14.3% / +29.6% | +17.2% / +5.6% |
| CitmCatalog (pretty) | +23.8% / +26.4% | +2.6% / +3.3% | +11.1% / +10.3% | +21.7% / +16.4% | −1.1% / −32.0% |
| Discord (min) | +38.0% / +31.4% | +10.4% / +17.3% | +20.8% / +25.4% | +18.0% / +22.6% | +14.9% / +16.9% |
| Discord (pretty) | +20.6% / +38.4% | +12.3% / +14.4% | +17.9% / +25.8% | +22.1% / +25.9% | +16.6% / +11.3% |
| Github Events (min) | +42.2% / +61.2% | +25.0% / +32.6% | +16.5% / +21.2% | +38.2% / +32.0% | +35.3% / +31.8% |
| Github Events (pretty) | +41.9% / +69.0% | +23.5% / +26.7% | +11.3% / +13.8% | +29.3% / +40.3% | +40.9% / +37.0% |
| Google Maps (min) | +26.1% / +32.6% | +17.0% / +17.8% | +27.5% / +30.8% | +22.3% / +29.0% | −1.3% / tie |
| Google Maps (pretty) | +32.2% / +37.8% | +9.1% / +10.5% | +20.6% / +23.4% | +21.0% / +36.5% | tie / +20.0% |
| Instruments (min) | +45.8% / +52.5% | tie / +0.5% | +10.3% / +8.7% | −2.2% / +12.6% | +16.7% / −8.4% |
| Instruments (pretty) | +50.7% / +47.5% | −3.7% / −1.5% | +9.7% / +9.6% | +11.0% / +45.8% | −11.6% / tie |
| Marine IK (min) | −7.1% / −10.7% | +3.8% / +6.1% | +2.9% / +1.7% | tie / tie | +4.1% / +7.2% (n/c) |
| Marine IK (pretty) | tie / tie | tie / +3.3% | −2.5% / −3.1% | tie / −12.9% | +6.8% / +6.8% |
| Random (min) | +34.3% / +44.3% | +22.7% / +28.3% | +37.3% / +47.0% | +42.3% / +38.9% | +27.0% / +27.9% |
| Random (pretty) | +32.4% / +33.8% | +18.0% / +22.1% | +34.8% / +46.3% | +43.2% / +58.6% | +31.1% / +32.9% |
| Twitter (min) | +37.7% / +48.0% | +13.5% / +9.0% | +10.0% / +13.8% | +14.5% / +14.1% | +7.3% / +2.7% |
| Twitter (pretty) | +30.6% / +42.1% | +3.9% / +6.2% | +2.8% / +6.0% | +18.9% / +36.9% | +8.0% / +3.9% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +45.8% / +60.7% | +59.5% / +79.3% | +63.7% / +79.5% | +64.1% / +82.3% | +80.0% / +98.6% |
| Apache Builds (pretty) | +49.3% (n/c) / +86.0% | +52.6% / +65.0% | +59.4% / +75.2% | +46.0% / +56.1% | +52.4% / +70.7% |
| Canada (min) | −4.6% / tie | +8.0% / +9.2% | −3.3% / −3.7% | +10.4% / +6.7% | +13.0% / +12.9% (n/c) |
| Canada (pretty) | tie (n/c) / tie | +7.4% / +8.9% | −0.5% / +0.6% | +4.8% / +3.2% | +12.7% / tie |
| CitmCatalog (min) | +82.7% / +81.4% | +62.1% / +65.5% | +72.0% / +78.7% | +37.8% / +60.7% | +83.0% (n/c) / +47.5% (n/c) |
| CitmCatalog (pretty) | +76.0% / +88.8% (n/c) | +56.7% / +58.9% | +67.3% / +70.7% | +44.5% / +61.4% | +43.5% / +63.7% |
| Discord (min) | +292.3% / +319.6% | +243.9% / +271.7% | +222.1% / +244.9% | +236.0% / +260.5% | +286.8% / +306.4% |
| Discord (pretty) | +264.6% / +301.3% | +213.6% / +231.6% | +198.6% / +217.9% | +265.0% / +291.8% | +246.4% / +260.7% |
| Github Events (min) | +210.4% / +262.8% | +126.0% / +141.6% | +115.5% / +133.2% | +92.6% / +128.7% | +125.9% / +121.1% |
| Github Events (pretty) | +176.0% / +221.3% | +105.3% / +118.2% | +92.5% / +105.4% | +73.8% / +111.7% | +94.2% / +96.8% |
| Google Maps (min) | +33.0% / +37.0% | +2.2% / +3.3% | +5.0% / +6.4% | +3.6% / +6.4% | −5.6% / −6.5% |
| Google Maps (pretty) | +37.0% / +45.2% (n/c) | −0.2% / −0.6% | +1.3% / −0.9% | +16.7% / +27.0% | −6.6% / −6.2% |
| Instruments (min) | +316.9% / +332.7% | +159.4% / +166.0% | +126.6% / +129.4% | +146.6% / +139.9% | +156.6% / +161.7% |
| Instruments (pretty) | +259.7% / +270.4% | +127.3% / +132.4% | +104.3% / +107.2% | +89.2% / +112.2% | +158.7% / +163.1% |
| Marine IK (min) | +35.9% / +38.5% | +55.7% / +59.3% | +44.7% / +47.2% | +48.9% / +58.8% | +65.1% (n/c) / +59.2% |
| Marine IK (pretty) | +37.6% (n/c) / +41.6% | +53.9% / +58.4% | +55.9% / +58.9% | +59.3% (n/c) / +52.5% (n/c) | +43.2% / +44.9% (n/c) |
| Random (min) | +116.4% / +137.0% | +74.4% / +87.5% | +66.8% / +75.2% | +85.6% / +131.0% | +86.7% / +103.8% |
| Random (pretty) | +104.5% / +121.5% | +63.2% / +74.0% | +57.5% / +63.7% | +65.4% / +95.5% | +75.5% / +76.4% |
| Twitter (min) | +301.9% / +359.7% | +145.4% / +156.3% | +122.5% / +126.2% | +147.5% / +136.9% | +108.0% / +128.0% |
| Twitter (pretty) | +341.9% (n/c) / +358.9% (n/c) | +126.7% / +138.9% | +96.9% / +101.5% | +79.4% / +82.5% | +154.8% / +121.5% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +16.6% / +42.1% | +30.5% / +40.4% | +47.3% / +57.1% | +30.8% / +43.9% | +34.8% / +51.1% |
| Apache Builds (pretty) | +21.5% / +46.7% | +19.7% / +28.4% | +33.1% / +44.1% | +29.1% / +41.6% | +8.6% / +14.7% |
| Canada (min) | +64.3% / +62.6% | +7.0% / +5.7% | tie / +0.2% | +17.5% / +17.4% | +41.3% / +40.4% |
| Canada (pretty) | +83.5% / +86.8% | +6.5% / +6.4% | +15.0% / +15.1% | +12.4% / +7.9% | +31.9% / +36.5% |
| CitmCatalog (min) | +48.1% / +43.4% | +6.8% / +6.0% | +10.4% / +10.2% | +39.3% / +28.0% | tie / −1.5% |
| CitmCatalog (pretty) | +48.9% / +45.0% | +4.3% / +5.8% | +9.9% / +9.2% | +42.5% / +9.6% | +2.8% / +8.0% |
| Discord (min) | +24.4% / +27.4% | +12.4% / +14.7% | +10.9% / +13.6% | +15.3% / +1.8% | +45.3% / +3.1% |
| Discord (pretty) | +16.2% / +13.2% | +6.2% / +5.2% | +11.1% / +10.5% | +32.2% / +34.8% | tie / −4.7% |
| Github Events (min) | +59.8% / +77.3% | +8.2% / +10.3% | +19.7% / +22.4% | +30.2% / +38.6% | −8.9% / +17.5% |
| Github Events (pretty) | +56.2% / +67.4% | +7.0% / +7.6% | +13.1% / +15.7% | +24.9% / +21.0% | −9.3% / −6.9% |
| Google Maps (min) | +40.0% (n/c) / +41.2% | +7.3% / +8.3% | +15.6% / +17.2% | +20.1% / +24.3% | +8.0% / tie |
| Google Maps (pretty) | +46.7% / +56.9% (n/c) | +4.1% / +3.5% | +9.6% / +7.0% | +24.7% / +24.0% | +4.9% / −1.1% |
| Instruments (min) | +55.1% / +61.2% (n/c) | +6.9% / +7.5% | +9.3% / +10.6% | +12.1% / +12.9% | +21.3% / +9.9% |
| Instruments (pretty) | +50.2% / +68.3% | +3.5% / +4.6% | +8.5% / +8.3% | +16.5% / +11.1% | −10.4% / −4.5% |
| Marine IK (min) | +36.6% / +33.8% | +7.6% / +7.4% | −4.2% / −4.0% | −20.6% (n/c) / −4.3% | +23.7% / +30.3% |
| Marine IK (pretty) | +75.8% / +63.2% | +5.2% / +3.4% | −7.6% / −7.6% | +24.6% / +11.9% | +27.3% / +16.0% |
| Random (min) | +49.7% / +55.9% | +12.9% / +14.7% | +19.0% / +21.6% | +19.9% / +31.9% | +27.2% / +30.0% |
| Random (pretty) | +43.7% / +49.1% | +7.9% / +9.2% | +9.4% / +14.0% | +32.5% / +42.6% | +23.1% / +15.5% |
| Twitter (min) | +89.7% / +94.7% | +6.9% / +7.5% | +14.1% / +15.6% | +24.6% / +43.2% | +9.2% / +4.7% |
| Twitter (pretty) | +75.8% / +72.5% | +4.8% / +4.5% | +9.4% / +10.1% | +6.3% / +9.1% | tie / +9.0% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.8% / +26.7% | +11.2% / +14.7% | +20.9% / +26.3% | +33.4% / +45.1% | +16.9% / +21.0% |
| Apache Builds (pretty) | +25.3% / +44.2% | +7.9% / +10.3% | +13.6% / +16.5% | +20.3% / +34.9% | +13.7% / +7.6% |
| Canada (min) | +38.1% / +37.3% | +2.4% / +2.3% | +3.0% / +2.9% | +15.4% / +14.1% | +12.9% / +22.4% |
| Canada (pretty) | +58.4% / +68.0% | +4.0% / +3.6% | +8.9% / +12.6% | +8.2% / +9.3% | +11.1% / +25.5% |
| CitmCatalog (min) | +20.7% / +27.5% | −4.0% / −5.8% | +7.1% / +7.0% | +1.8% / +2.2% | +3.6% / +3.3% |
| CitmCatalog (pretty) | +40.4% / +49.5% | −3.2% / −3.9% | +4.8% / +3.6% | +10.6% / +4.4% | +12.0% / tie |
| Discord (min) | +64.5% / +76.0% | +27.2% / +30.5% | +31.3% / +37.5% | +32.7% / +42.0% | +38.2% / +50.7% |
| Discord (pretty) | +58.3% / +71.7% | +24.7% / +28.7% | +27.1% / +32.6% | +43.9% / +53.4% | +7.8% / +19.1% |
| Github Events (min) | +92.9% / +95.8% | +23.6% / +24.1% | +34.2% / +37.4% | +39.1% / +51.6% | +54.5% / +74.7% |
| Github Events (pretty) | +72.1% / +90.3% | +24.2% / +24.8% | +26.7% / +28.4% | +30.8% / +36.1% | +29.8% / +18.2% |
| Google Maps (min) | +42.2% / +46.8% | −1.4% / tie | +4.1% / +4.3% | +6.6% / tie | +15.2% / −1.8% |
| Google Maps (pretty) | +49.6% / +52.0% | −5.5% / −4.9% | +0.3% / tie | +7.8% / +27.0% | −1.9% / +2.3% |
| Instruments (min) | +70.2% / +87.8% | +23.1% / +22.7% | +22.2% / +23.5% | +31.8% / +37.3% | +22.9% / +23.5% |
| Instruments (pretty) | +71.7% / +82.9% | +11.7% / +6.5% | +15.4% / +16.5% | +28.5% / +26.8% | +15.3% / −7.2% |
| Marine IK (min) | +67.9% (n/c) / +51.7% | +41.0% / +40.4% | +23.9% / +24.1% | +9.9% / +13.3% | +42.1% / +41.3% |
| Marine IK (pretty) | +77.8% / +70.0% | +27.9% / +28.8% | +16.2% / +16.7% | +22.7% / +26.2% | +29.7% / +32.0% |
| Random (min) | +59.2% / +76.6% | +16.0% / +18.2% | +24.4% / +27.0% | +31.7% / +35.4% | +24.8% / +37.7% |
| Random (pretty) | +45.8% / +58.2% | +9.9% / +11.8% | +13.6% / +17.6% | +33.9% / +35.6% | +25.7% / +30.2% |
| Twitter (min) | +87.1% / +84.7% | +20.4% / +19.5% | +16.0% / +15.2% | +27.9% (n/c) / +33.5% | +14.3% / +17.3% |
| Twitter (pretty) | +76.4% / +72.9% | +12.8% / +14.6% | +13.3% / +14.3% | +30.6% / +16.3% | +20.8% / −8.0% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.6% / +17.4% | +11.9% / +14.1% | +21.3% / +29.8% | +33.0% / +60.2% (n/c) | +13.8% / +6.4% |
| Apache Builds (pretty) | +14.4% / +20.0% | +11.0% / +13.7% | +19.2% / +30.1% | +25.5% / +36.0% (n/c) | +28.1% / +30.0% |
| Canada (min) | tie / tie (n/c) | +5.9% / +3.7% | −6.8% / −10.8% | +3.1% / +2.4% | +2.2% / +9.5% |
| Canada (pretty) | tie / +3.6% | +5.8% / +2.6% | −5.5% / −6.6% | +2.2% / +1.4% | +2.1% / +25.4% |
| CitmCatalog (min) | +13.2% / +15.4% | −5.0% / −4.4% | −1.3% / +0.6% | +48.0% / +15.4% | +5.1% / +7.6% |
| CitmCatalog (pretty) | +22.4% / +15.9% | −5.2% / −4.0% | −0.3% / +1.2% | −8.3% / +26.7% | −26.8% / −26.7% |
| Discord (min) | +9.9% / +24.3% | +3.2% / +4.8% | +6.1% / +15.8% | −5.5% / +37.7% | +16.3% / +23.1% |
| Discord (pretty) | +12.4% / +25.8% (n/c) | −1.3% / +1.6% | +7.3% / +15.4% | +7.9% / +19.3% | +4.6% / +11.7% |
| Github Events (min) | +48.1% (n/c) / +61.6% | +9.4% / +9.2% | +1.6% / +3.9% | −2.2% / +18.4% | +11.9% / +13.5% |
| Github Events (pretty) | +47.6% (n/c) / +53.4% | +3.2% / +1.6% | −1.9% / −2.5% | +25.4% (n/c) / +11.2% | +5.6% / +5.6% |
| Google Maps (min) | +31.3% / +32.7% | +16.0% / +16.4% | +24.1% / +30.2% | +44.5% / +48.3% | +9.3% / +4.3% |
| Google Maps (pretty) | +34.8% (n/c) / +42.8% (n/c) | +9.8% / +9.7% | +20.8% / +21.6% | +36.4% / +33.7% | −2.3% / +5.0% |
| Instruments (min) | +37.2% / +45.1% | −4.3% / −6.0% | +5.4% / +6.4% | +36.8% / +56.5% | +3.9% / +4.8% |
| Instruments (pretty) | +47.2% / +64.5% | −9.2% / −8.9% | +1.7% / +4.4% | +29.2% / +7.9% | −1.4% / −0.2% |
| Marine IK (min) | −5.8% / tie | tie / tie | −1.2% / −1.1% | +19.0% / +17.7% | +2.3% / +20.8% |
| Marine IK (pretty) | +9.4% (n/c) / +4.9% | +0.7% / tie | −2.5% / −2.9% | +8.0% / +17.6% | −3.8% / +14.5% |
| Random (min) | +24.7% / +31.2% (n/c) | +12.0% / +17.4% | +25.1% / +33.0% | +29.7% / +55.3% | +22.9% / +21.8% |
| Random (pretty) | +26.3% / +33.6% | +10.9% / +13.2% | +24.3% / +31.0% | +40.4% / +24.8% | +11.3% / +40.0% |
| Twitter (min) | +22.0% / +35.9% | −1.8% / +1.6% | +9.4% / +13.1% | +14.1% / +36.0% | +5.1% / +11.2% |
| Twitter (pretty) | +30.5% / +32.2% | −3.4% / −0.7% | +5.8% / +11.1% | +9.9% / +34.3% | +3.0% / +4.7% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +75.9% / +106.0% | +73.0% / +85.5% | +71.0% / +81.5% | +58.7% / +86.8% | +66.2% / +112.4% |
| Apache Builds (pretty) | +68.5% / +99.6% | +57.4% / +68.6% | +56.6% / +67.3% | +62.9% / +92.7% | +70.1% (n/c) / +77.0% |
| Canada (min) | tie / +9.5% | +7.1% / +8.2% | −7.5% / −5.8% | +4.5% / +6.9% | +6.0% / tie |
| Canada (pretty) | tie / +4.5% | +8.0% / +8.2% | −4.3% / −5.3% | +4.0% / +5.2% | +20.3% / +14.1% |
| CitmCatalog (min) | +84.4% / +81.9% | +56.9% / +61.9% | +42.0% / +50.0% | +68.5% / +54.2% | +82.7% / +109.0% |
| CitmCatalog (pretty) | +76.4% / +95.5% | +43.9% / +48.0% | +36.5% / +38.9% | +62.9% / +44.7% | +39.6% / +55.3% |
| Discord (min) | +309.6% / +385.3% | +218.3% / +250.5% | +171.8% / +195.3% | +233.2% / +220.5% (n/c) | +304.5% / +380.0% |
| Discord (pretty) | +281.0% / +337.7% | +199.5% / +232.0% | +144.7% / +171.8% | +124.0% / +258.4% | +207.6% / +207.3% |
| Github Events (min) | +85.7% / +101.4% | +31.5% / +35.0% | +22.6% / +22.3% | +33.5% / +48.9% | +39.1% / +45.4% |
| Github Events (pretty) | +62.1% / +86.0% | +21.1% / +27.0% | +14.4% / +13.0% | +41.6% / +28.8% | +24.7% / +45.7% |
| Google Maps (min) | +32.3% / +38.9% | +2.0% / +0.4% | +3.0% / +4.5% | +24.8% / +22.1% | −15.8% / +7.7% |
| Google Maps (pretty) | +34.7% / +32.1% | −1.8% / −3.6% | −2.2% / −3.9% | +29.0% / +38.3% | −11.4% / −10.3% |
| Instruments (min) | +306.0% / +303.4% | +139.4% / +148.1% | +88.7% / +92.4% | +90.3% / +85.5% | +152.0% / +162.1% |
| Instruments (pretty) | +299.2% / +319.4% | +115.3% / +119.2% | +77.9% / +80.5% | +81.4% / +109.4% | +127.1% / +135.1% |
| Marine IK (min) | +50.7% / +53.7% | +44.5% / +47.9% | +31.0% / +34.0% | +42.4% / +45.4% | +45.2% / +56.0% |
| Marine IK (pretty) | +49.8% / +53.0% | +35.6% / +37.9% | +30.7% / +33.5% | +37.2% / +42.3% | +27.8% / +21.4% |
| Random (min) | +102.4% / +131.5% | +65.7% / +75.0% | +54.9% / +60.8% | +80.8% / +104.3% | +117.6% / +92.8% |
| Random (pretty) | +98.4% / +120.8% | +59.7% / +67.7% | +43.2% / +50.4% | +87.2% / +63.8% | +79.7% / +85.5% |
| Twitter (min) | +307.4% / +337.9% | +146.4% / +153.9% | +95.0% / +93.6% | +144.4% / +105.2% | +153.3% / +161.6% |
| Twitter (pretty) | +300.8% / +345.8% | +129.9% / +137.4% | +79.9% / +83.5% | +104.3% / +122.0% | +135.5% / +144.8% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +28.0% / +53.9% | +24.9% / +23.7% | +35.3% / +41.1% | +35.7% / +57.2% | +27.5% / +16.7% |
| Apache Builds (pretty) | +26.6% / +58.5% | +17.3% / +16.9% | +28.3% / +30.6% | +36.1% / +49.0% | −5.0% / +8.7% |
| Canada (min) | +72.0% / +72.2% | +5.0% / +6.5% | +8.1% / +7.2% | +19.5% / +19.3% | +4.1% / +9.7% |
| Canada (pretty) | +99.0% / +99.3% | +6.1% / −1.4% | +18.1% / +16.6% | +13.7% / +15.0% | −21.5% / +30.9% |
| CitmCatalog (min) | +47.8% / +56.2% | +6.3% / +3.6% | +11.4% / +12.7% | +7.6% / +20.0% | +7.9% / −4.1% |
| CitmCatalog (pretty) | +47.5% / +52.6% | +2.3% / +3.9% | +12.2% / +12.8% | +48.4% / +6.5% | +4.7% / +2.6% |
| Discord (min) | +27.4% / +43.9% | +7.5% / +9.0% | +4.4% / +7.8% | +17.6% / +22.9% | +4.5% / +1.8% |
| Discord (pretty) | +24.7% / +37.3% | tie / +3.2% | +3.1% / +5.3% | +6.7% / +10.4% | +4.6% / −4.1% |
| Github Events (min) | +51.4% / +56.1% | +1.1% / tie | +11.4% / +11.1% | +22.5% / +17.5% (n/c) | +11.3% / +15.9% |
| Github Events (pretty) | +42.5% / +55.7% | tie / +1.1% | +6.6% / +6.5% | +23.6% / +15.4% | −3.3% / −13.2% |
| Google Maps (min) | +25.7% / +41.1% | +6.9% / +6.0% | +12.8% / +12.2% | +18.9% / +38.0% | +5.2% / −3.2% |
| Google Maps (pretty) | +43.5% / −24.2% | tie / −0.7% | +8.1% / +6.7% | +21.4% / +29.2% | +3.6% / −15.6% |
| Instruments (min) | +38.7% / +40.7% | +2.0% / +4.1% | +2.3% / +1.2% | +8.5% / +10.7% | +32.1% / +37.3% |
| Instruments (pretty) | +36.9% / +46.8% | −0.7% / −0.6% | −0.7% / −1.6% | +5.6% / +5.0% | tie / −5.5% |
| Marine IK (min) | +30.9% / +30.0% | tie / tie | +5.2% / +6.3% | +32.8% / +8.8% | +7.7% / +8.0% |
| Marine IK (pretty) | +50.4% / +52.6% | +3.0% / +2.2% | +12.1% / +8.2% | tie / +11.0% | +1.0% / +1.3% |
| Random (min) | +34.0% (n/c) / +47.0% | +11.9% / +11.8% | +15.9% / +16.6% | tie / +14.8% | +15.8% / +22.8% |
| Random (pretty) | +46.0% / +33.0% | +6.2% / +5.6% | +12.3% / +11.8% | +20.0% / +10.7% | +12.6% / +17.8% |
| Twitter (min) | +24.9% / +30.0% | −3.9% / −2.1% | +1.0% / −1.0% | tie (n/c) / tie | +1.3% / tie |
| Twitter (pretty) | +25.9% / +27.5% | −3.1% / −3.0% | +1.0% / tie | +18.4% / tie | −4.8% / −9.5% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +36.7% / +46.4% | +10.7% / +12.2% | +18.4% / +20.9% | +33.2% / +49.2% | −4.3% / +23.5% (n/c) |
| Apache Builds (pretty) | +35.7% / +39.4% | +5.1% / +4.2% | +7.1% / +8.0% | +23.4% / +28.3% | −2.9% / tie |
| Canada (min) | +46.4% / +45.2% | tie / +0.7% | +5.7% / +4.4% | +15.3% / +14.1% | −18.6% / −15.2% |
| Canada (pretty) | +79.0% / +77.8% | −6.9% / −3.5% | +10.9% / +10.9% | −18.0% / +11.0% | +21.7% / tie |
| CitmCatalog (min) | +26.5% / +31.1% | −2.6% / +1.5% | +3.0% / +2.4% | +16.7% / +7.4% | +6.9% / +8.4% |
| CitmCatalog (pretty) | +37.3% / +34.4% | −4.5% / −8.8% | +3.6% / +2.8% | −4.4% / +25.9% | −1.2% / −5.7% |
| Discord (min) | +79.3% / +101.4% | +27.1% / +30.7% | +26.1% / +29.9% | +37.7% / +84.3% | +48.2% / +28.7% |
| Discord (pretty) | +61.8% / +80.9% | +18.8% / +21.9% | +20.5% / +23.2% | +38.2% / +33.2% | +24.2% / +32.0% |
| Github Events (min) | +71.1% / +87.2% | +7.6% / +8.2% | +19.6% / +21.4% | +21.6% / +13.0% | +9.8% / +32.8% |
| Github Events (pretty) | +64.4% / +73.9% | +8.8% / +11.3% | +12.7% / +15.0% | +17.2% / +32.4% | +8.2% / +10.3% |
| Google Maps (min) | +42.2% / +26.0% | tie / tie | +3.9% / +2.4% | +10.9% / tie | −4.7% / −3.3% |
| Google Maps (pretty) | +45.8% / +49.0% | −5.4% / −4.8% | tie / −2.1% | +14.9% / +13.0% | −2.6% / −3.2% |
| Instruments (min) | +58.9% / +67.6% | +16.9% / +16.1% | +14.1% / +15.8% | +31.5% / +26.7% | +22.8% / +23.7% |
| Instruments (pretty) | +62.9% / +74.1% | +4.9% / +6.3% | +9.5% / +9.0% | +24.2% / +13.3% | +10.9% / tie |
| Marine IK (min) | +70.2% / +58.0% | +36.7% / +34.1% | +29.6% / +29.4% | +17.3% / +33.6% | +32.3% / +32.0% |
| Marine IK (pretty) | +83.2% / +72.4% | +44.5% / +46.1% | +28.9% / +28.0% | +30.3% / +31.8% | +14.8% / +14.5% |
| Random (min) | +44.9% / +53.4% | +18.9% / +18.3% | +14.7% / +15.5% | +6.1% / +26.5% | +23.8% / +27.4% |
| Random (pretty) | +50.3% / +52.9% | +12.6% / +10.0% | +13.2% / +10.9% | +22.6% / +27.4% | tie / +11.3% |
| Twitter (min) | +42.0% / +47.2% | +7.7% / +6.3% | +13.0% / +10.9% | +34.8% / +29.0% | +9.7% / +10.3% |
| Twitter (pretty) | +42.3% / +47.2% | +1.2% / +0.8% | +6.4% / +5.6% | +21.1% / +14.9% | +3.3% / +3.5% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.8x to 4.6x faster at full size: +79.4% (Twitter pretty, macOS/GCC, fresh) up to +359.7% (Twitter min, MSVC, reused), and up to +306.4% outside MSVC (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +77.9% to +385.3%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −6.6% (prettified Google Maps on macOS/Clang, fresh) to +27.0%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events rarely loses.** It accounts for 8 of the 151 single-document losses: 5 sparse reads on macOS/Clang (−3.3% to −13.2%), prettified Github Events Small in order on Linux/GCC (−1.9% and −2.5%) and minified Github Events Small in order on macOS/GCC (fresh, −2.2%). Read in reverse at full size it wins on every build (+73.8% to +262.8%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 185 of 200 full-size sparse-reverse results across both allocation modes (4 ties, 11 losses), and 171 of 200 small ones (9 ties, 20 losses). MSVC won all 80. The full-size losses are 7 on Linux/Clang, all CitmCatalog or Google Maps (−1.4% to −5.8%), and 4 on macOS/Clang (−1.8% to −8.0%): Google Maps twice, prettified Instruments and prettified Twitter. 18 of the 20 small losses are on Canada, CitmCatalog or Google Maps; the other two are Apache Builds Small on macOS/Clang (fresh, minified −4.3% and prettified −2.9%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 180.8 MB/s on MSVC against 289.1 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 659.2 and 906.9 MB/s. Every MSVC full-size sparse result is a win, by +13.2% to +94.7%, and every full-size sparse-reverse result by +20.7% to +95.8%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 181 of 200 full-size sparse results (5 ties, 14 losses) and 163 of 200 small ones (14 ties, 23 losses). MSVC and Linux/Clang won all 40 of their full-size results, and MSVC won 39 of 40 small. The full-size losses are on macOS/Clang (8), Linux/GCC (4, all Marine IK, −4.0% to −7.6%) and macOS/GCC (2, both minified Marine IK). macOS/Clang holds 11 of the 23 small losses, Linux/Clang 8, Linux/GCC 3 and MSVC 1.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 167, tied 14 and lost 19. Linux/Clang went 36-2-2, Linux/GCC 34-0-6, macOS/GCC 34-4-2, MSVC 33-4-3 and macOS/Clang 30-4-6 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 159, tied 7 and lost 34. macOS/GCC went 37-0-3, MSVC 35-4-1, macOS/Clang 34-0-6, Linux/GCC 28-0-12 and Linux/Clang 25-3-12. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −0.7% to −9.2%, led by CitmCatalog and Instruments, and Linux/GCC's from −0.3% to −10.8%, led by Canada and Marine IK. The largest is prettified CitmCatalog Small on macOS/Clang (−26.8% fresh, −26.7% reused).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−5.0% to −9.5%) and prettified Marine IK in both runs (−2.5% and −3.1%). MSVC lost minified Canada (fresh, −2.0%) and minified Marine IK in both runs (−7.1% and −10.7%), and macOS/GCC lost minified Instruments (fresh, −2.2%) and prettified Marine IK (reused, −12.9%). Linux/Clang lost only prettified Instruments (−3.7% and −1.5%). macOS/Clang's six losses are prettified CitmCatalog (−1.1% fresh, −32.0% reused), Instruments twice (minified reused −8.4%, prettified fresh −11.6%), minified Google Maps (fresh, −1.3%) and prettified Canada (reused, −6.5%, n/c). Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 41-1-8 on them. Double and Int64 won all 10 results each (+51.8% to +219.8% and +34.4% to +188.4%). Bool won 8 (+48.1% to +170.8%), losing on macOS/Clang fresh (−10.7%) and tying there reused, and Uint64 won 8 (+32.9% to +186.7%, with both losses on Linux/GCC, −2.4% and −2.0%). String is the exception: 5 wins and 5 losses, on Linux/Clang in both runs (−6.9% and −9.6%), macOS/Clang in both runs (−10.3% and −21.5%) and Linux/GCC reused (−3.6%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 5.9% to 12.0% below its own NDJSON throughput on the same records, in 17 of the 18 build and stream pairs. The exception is CitmCatalog on MSVC, where comma mode is 1.7% faster.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs from the 26 streaming tests that existed at the time are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`, before the Apache Builds streams were added.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 22 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 23 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 20 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 3-2-1 | 12-0-0 | 12-0-0 | 27-2-1 |
| Linux / GCC | 4-1-1 | 12-0-0 | 12-0-0 | 28-1-1 |
| macOS / GCC | 5-1-0 | 11-1-0 | 12-0-0 | 28-2-0 |
| macOS / Clang | 3-2-1 | 11-1-0 | 11-1-0 | 25-4-1 |
| **All** | **21-6-3** | **58-2-0** | **59-1-0** | **138-9-3** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +25.8% | +24.4% | +20.6% | +13.7% | tie |
| Large Amazon Cellphones (NDJSON) | +22.8% | +11.1% | tie | +16.5% | +27.7% |
| Stream Formats Small (NDJSON) | +64.1% (n/c) | tie | −0.7% | +20.7% | tie (n/c) |
| Stream Formats Small (comma) | +116.8% | +71.4% | +102.3% | +93.3% | +36.9% (n/c) |
| Stream Formats Large (NDJSON) | +97.0% (n/c) | −3.7% | +20.4% | tie (n/c) | −12.0% |
| Stream Formats Large (comma) | +112.7% | tie | +30.5% | +17.9% | +2.3% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +40.1% | +19.4% | +15.9% | +28.9% | +30.4% (n/c) |
| Apache Builds (comma) | +43.2% | +27.1% | +23.6% | +36.9% | +19.6% (n/c) |
| CitmCatalog (NDJSON) | +27.2% | +5.5% | +14.6% | tie | +12.1% |
| CitmCatalog (comma) | +35.6% (n/c) | +23.3% | +36.7% | +16.8% (n/c) | +27.7% |
| Google Maps (NDJSON) | +36.3% | +28.3% | +36.6% | +19.9% | +15.9% |
| Google Maps (comma) | +42.7% | +38.4% | +52.2% | +24.6% | +21.1% |
| Instruments (NDJSON) | +72.6% | +15.8% | +21.1% | +15.8% | tie |
| Instruments (comma) | +78.9% | +34.6% | +63.8% | +50.9% | +24.4% |
| Random (NDJSON) | +38.8% (n/c) | +21.4% | +35.2% | +49.0% | +32.9% |
| Random (comma) | +46.6% | +33.8% | +55.2% | +80.8% (n/c) | +56.0% |
| Twitter (NDJSON) | +32.0% | +7.0% | +2.6% | +26.0% | +13.7% |
| Twitter (comma) | +36.3% | +19.0% | +20.8% | +13.3% | +15.5% (n/c) |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +40.9% | +18.0% | +2.8% | +12.6% | +21.9% (n/c) |
| Apache Builds (comma) | +51.8% | +30.7% | +20.8% | +34.2% | +40.8% |
| CitmCatalog (NDJSON) | +47.6% (n/c) | +26.3% | +38.5% | +22.0% | +44.3% |
| CitmCatalog (comma) | +61.6% | +42.3% | +64.7% | +60.7% | +59.9% |
| Google Maps (NDJSON) | +35.4% (n/c) | +7.8% | +15.0% | +12.8% (n/c) | tie (n/c) |
| Google Maps (comma) | +37.7% | +13.5% | +34.3% | +28.0% | +16.4% |
| Instruments (NDJSON) | +127.7% | +36.2% | +27.8% | +22.5% | +26.2% |
| Instruments (comma) | +145.0% | +58.6% | +55.9% | +43.4% (n/c) | +54.3% (n/c) |
| Random (NDJSON) | +112.4% | +71.2% | +59.2% | +88.7% (n/c) | +103.7% (n/c) |
| Random (comma) | +118.5% | +86.1% | +79.4% | +89.0% | +89.8% |
| Twitter (NDJSON) | +354.0% (n/c) | +164.9% | +148.2% | +139.4% | +175.6% |
| Twitter (comma) | +386.3% (n/c) | +177.7% | +162.8% | +86.8% (n/c) | +189.0% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 21 of the 30 results in that group, tied 6 and lost 3. It won Stream Formats Small (comma) on all five builds (+36.9% to +116.8%), and Amazon Cellphones (+13.7% to +25.8%) and Large Amazon Cellphones (+11.1% to +27.7%) on four, tying on macOS/Clang and Linux/GCC respectively. The three losses are all Stream Formats read as NDJSON: Small on Linux/GCC (−0.7%), and Large on Linux/Clang (−3.7%) and macOS/Clang (−12.0%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 30-0-0. macOS/GCC lost none (28-2-0), Linux/GCC and Linux/Clang lost one each (28-1-1 and 27-2-1), and macOS/Clang went 25-4-1. Every streaming loss is a Stream Formats read as NDJSON.
- **In-order corpus streams never lose.** Of the 60 in-order corpus results, Jsonifier won 58 and tied 2: CitmCatalog (NDJSON) on macOS/GCC and Instruments (NDJSON) on macOS/Clang.
- **Reverse order carries over from single documents to streams.** Jsonifier won 59 of the 60 reverse stream results and tied the other, Google Maps (NDJSON) on macOS/Clang, which did not converge. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +86.8% to +386.3%, while Google Maps rows, which have few, give a tie to +37.7%. Instruments, whose reverse streams have the most room to lose, wins on every build (+22.5% to +145.0%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 257.4 MB/s under MSVC and 471.7 MB/s under Linux/Clang, while Jsonifier reads it at 562.4 and 877.9 MB/s.

## 9. Limitations

- **The Clang builds hold the most single-document losses:** 55 of the 151 on macOS/Clang and 42 on Linux/Clang, against 39 on Linux/GCC, 9 on macOS/GCC and 6 on MSVC. 29 of Linux/Clang's are small documents, led by 12 small in-order, and 30 of macOS/Clang's, led by 11 small sparse. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's prettified CitmCatalog read in order (reused, −32.0%) and its prettified CitmCatalog Small read in order (−26.8% fresh, −26.7% reused); the largest x86 loss is MSVC's prettified Google Maps Small sparse read (reused, −24.2%).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 81 of the 151: Google Maps 33 (18 on macOS/Clang, 10 on Linux/Clang), Canada 25 (15 on Linux/GCC) and CitmCatalog 23 (11 on Linux/Clang, 8 on macOS/Clang). Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 15 of its 39 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 88 of the 151 losses are small documents (30 on macOS/Clang, 29 on Linux/Clang, 22 on Linux/GCC, 5 on macOS/GCC, 2 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats read as NDJSON** are the only streaming losses: Small on Linux/GCC (−0.7%), and Large on Linux/Clang (−3.7%) and macOS/Clang (−12.0%). No corpus record stream loses on any build.
- **Some results did not converge.** 41 of the 1,650 single-document results did not converge: 21 on Windows/MSVC, 10 on macOS/GCC and 10 on macOS/Clang. 25 of the 150 streaming results did not converge: 10 on macOS/Clang, 8 on Windows/MSVC and 7 on macOS/GCC. Both Linux builds converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,438, tied 61 and lost 151. On reverse reads it won 371 of 400 at both sizes, and on Windows/MSVC it lost 6 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (715-31-79 fresh, 723-30-72 reused). The main remaining targets are the small-object documents (Canada, Google Maps and CitmCatalog, with 81 of the 151 losses), small documents in general, and the Clang builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 138, tied 9 and lost 3, and on the 120 corpus record-stream results it went 117-3-0. Stream Formats read as NDJSON, which hold all three losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.