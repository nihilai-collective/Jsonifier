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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,503 of 1,650 single-document results against simdjson On Demand, tied 29 and lost 118. On reverse-order reads it won 380 of 400. On Windows/MSVC it won 322 of 330 single-document results, tied 4 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 136 of 150 results, tied 6 and lost 8, and it won 112 of the 120 corpus record-stream results, tied 4 and lost 4.

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
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 5 of the 1,650 single-document results and 22 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 169 / 170 | 160 / 160 | 329 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 167 / 170 | 159 / 160 | 326 / 330 |

All builds ran BenchmarkSuite `79cbad3`, Jsonifier `ed75df2` and simdjson `65dd859bb`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on November 2, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 161-2-2 |
| Linux / Clang | 4-0-1 | 20-0-0 | 19-0-1 | 17-1-2 | 16-0-4 | 14-0-6 | 19-1-0 | 15-2-3 | 16-0-4 | 140-4-21 |
| Linux / GCC | 4-0-1 | 17-1-2 | 19-0-1 | 17-1-2 | 18-1-1 | 17-0-3 | 18-0-2 | 18-1-1 | 19-0-1 | 147-4-14 |
| macOS / GCC | 5-0-0 | 17-2-1 | 20-0-0 | 19-0-1 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 160-2-3 |
| macOS / Clang | 4-0-1 | 19-1-0 | 18-0-2 | 19-0-1 | 19-0-1 | 16-1-3 | 18-0-2 | 15-0-5 | 17-1-2 | 145-3-17 |
| **All** | **22-0-3** | **90-5-5** | **95-1-4** | **92-2-6** | **93-1-6** | **86-1-13** | **95-1-4** | **88-3-9** | **92-1-7** | **753-15-57** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-0-2 | 19-1-0 | 20-0-0 | 20-0-0 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 161-2-2 |
| Linux / Clang | 4-0-1 | 19-1-0 | 19-0-1 | 20-0-0 | 16-0-4 | 15-0-5 | 19-1-0 | 15-1-4 | 16-0-4 | 143-3-19 |
| Linux / GCC | 3-0-2 | 18-0-2 | 18-1-1 | 18-0-2 | 19-1-0 | 16-1-3 | 18-0-2 | 18-0-2 | 19-0-1 | 147-3-15 |
| macOS / GCC | 5-0-0 | 18-1-1 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 19-1-0 | 20-0-0 | 161-2-2 |
| macOS / Clang | 4-0-1 | 15-1-4 | 18-0-2 | 16-2-2 | 17-1-2 | 18-0-2 | 19-0-1 | 16-0-4 | 15-0-5 | 138-4-23 |
| **All** | **21-0-4** | **88-3-9** | **94-2-4** | **93-2-5** | **92-2-6** | **88-2-10** | **96-1-3** | **88-2-10** | **90-0-10** | **750-14-61** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (5 of them did not converge; see §9), Jsonifier won 1,503, tied 29 and lost 118: 753-15-57 with fresh output objects and 750-14-61 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 189 of 200 full-size reverse results (3 ties, 8 losses) and 191 of 200 small reverse results (2 ties, 7 losses). Windows/MSVC and macOS/GCC lost none of their 80. The 8 full-size losses are minified Canada on Linux/GCC (−2.4% and −3.6%), prettified Google Maps on Linux/Clang (−1.2% and −0.9%) and Google Maps on macOS/Clang in both forms and both runs (−5.7% to −17.6%). The 7 small losses are Canada Small on Linux/GCC (4) and Google Maps Small on macOS/Clang (3).
- **Windows/MSVC lost four of its 330 single-document results.** All four are full-size in-order reads in both runs: minified Canada (−1.3% fresh, −2.6% reused) and minified Marine IK (−1.3% fresh, −1.7% reused). Its 4 ties are all Canada. It won all 10 POD results and every other test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.9–14.1% for Jsonifier and 6.8–7.9% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +0.5 to +3.0 percentage points on the non-MSVC builds and by +8.9 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +293.1% to +341.4%, and Apache Builds (pretty) from +40.5% to +65.1%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +168.9% / +170.2% | +98.8% / +99.6% | +131.4% / +122.7% | +54.5% / +51.8% | +1.4% / +8.1% |
| Double | +180.5% / +214.4% | +80.5% / +83.9% | +98.8% / +101.7% | +130.1% / +107.9% | +51.7% / +106.5% |
| Int64 | +172.6% / +185.1% | +94.0% / +98.9% | +68.1% / +70.4% | +87.4% / +89.5% | +31.8% / +55.9% |
| String | +42.0% / +54.9% | −6.9% / −7.2% | +2.7% / −2.0% | +15.7% / +23.1% | −31.6% / −23.6% |
| Uint64 | +175.6% / +174.7% | +92.8% / +100.8% | −2.2% / −1.4% | +79.5% / +84.9% | +35.4% / +36.9% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +29.3% / +65.8% | +15.5% / +24.3% | +21.2% / +32.7% | +33.5% / +47.6% | +21.7% / +44.1% |
| Apache Builds (pretty) | +40.5% / +65.1% | +16.3% / +22.7% | +24.1% / +33.7% | +26.9% / +40.5% | +38.2% / +50.4% |
| Canada (min) | −1.3% / −2.6% | +5.4% / +5.9% | −5.8% / −6.8% | +1.4% / +1.8% | +4.0% / +37.5% |
| Canada (pretty) | tie / +2.0% | +5.0% / +6.4% | −5.4% / −5.8% | tie / +1.2% | +13.7% / +6.9% |
| CitmCatalog (min) | +47.7% / +49.3% | +9.2% / +8.1% | +14.3% / +18.0% | +9.7% / +10.3% | +10.6% / −2.8% |
| CitmCatalog (pretty) | +48.7% / +52.4% | +5.2% / +6.9% | +8.4% / +11.4% | +10.9% / +11.6% | +8.2% / −30.5% |
| Discord (min) | +55.0% / +75.6% | +10.1% / +14.6% | +18.8% / +26.9% | +23.1% / +28.8% | tie / +19.1% |
| Discord (pretty) | +58.0% / +78.0% | +10.6% / +16.8% | +14.4% / +24.4% | +21.3% / +25.7% | +10.3% / +19.5% |
| Github Events (min) | +62.0% / +87.2% | +30.5% / +34.6% | +19.1% / +23.0% | +36.3% / +45.9% | +50.3% / +37.4% |
| Github Events (pretty) | +64.8% / +83.1% | +26.4% / +27.8% | +12.4% / +16.9% | +29.6% / +35.6% | +5.6% / +29.7% |
| Google Maps (min) | +58.4% / +65.9% | +19.9% / +23.1% | +31.4% / +32.3% | +26.9% / +30.0% | +28.9% / +26.1% |
| Google Maps (pretty) | +58.9% / +66.1% | +19.0% / +17.4% | +27.2% / +29.9% | +27.1% / +30.1% | +15.7% / +7.3% |
| Instruments (min) | +78.3% / +93.0% | +1.6% / +1.8% | +11.6% / +11.6% | +14.2% / +16.4% | +4.3% / tie |
| Instruments (pretty) | +76.6% / +88.4% | +2.1% / +1.6% | +11.1% / +10.3% | +12.2% / +12.9% | +4.7% / −7.3% |
| Marine IK (min) | −1.3% / −1.7% | +3.0% / +3.9% | +3.2% / +2.9% | −1.5% / −1.6% | +10.6% / +5.2% |
| Marine IK (pretty) | +7.0% / +4.1% | +0.8% / tie | +3.3% / +2.7% | tie / tie | +18.6% / +6.7% |
| Random (min) | +57.9% / +75.3% | +22.6% / +29.2% | +67.8% / +82.7% | +46.5% / +57.7% | +36.3% / +29.9% |
| Random (pretty) | +59.3% / +77.1% | +24.4% / +30.5% | +60.3% / +78.4% | +42.1% / +52.5% | +43.4% / +32.1% |
| Twitter (min) | +53.0% / +69.5% | +8.3% / +12.3% | +7.8% / +13.2% | +20.8% / +25.4% | +27.7% / +29.5% |
| Twitter (pretty) | +51.3% / +66.6% | +6.0% / +11.3% | tie / +11.1% | +17.5% / +19.5% | +5.5% / −4.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +62.1% / +100.1% | +57.4% / +75.5% | +60.4% / +80.7% | +60.9% / +79.7% | +56.4% / +88.4% |
| Apache Builds (pretty) | +61.9% / +94.3% | +54.2% / +71.3% | +61.0% / +78.9% | +52.5% / +66.4% | +49.6% / +64.2% |
| Canada (min) | tie / tie | +9.0% / +9.4% | −2.4% / −3.6% | +4.9% / +5.7% | +10.6% / +12.4% |
| Canada (pretty) | +4.4% / +5.1% | +8.7% / +9.9% | +0.6% / tie | +4.2% / +4.8% | +14.3% / +14.6% |
| CitmCatalog (min) | +90.4% / +93.8% | +61.6% / +64.6% | +68.6% / +74.6% | +61.9% / +66.7% | +80.9% / +67.5% |
| CitmCatalog (pretty) | +88.5% / +92.7% | +56.3% / +59.8% | +63.2% / +66.6% | +56.5% / +61.6% | +49.0% / +85.3% |
| Discord (min) | +293.1% / +341.4% | +232.7% / +259.2% | +222.5% / +244.2% | +243.6% / +270.2% | +350.9% / +336.2% |
| Discord (pretty) | +257.8% / +298.6% | +184.5% / +199.1% | +201.3% / +220.3% | +207.2% / +226.6% | +186.3% / +247.8% |
| Github Events (min) | +206.4% / +258.8% | +131.3% / +152.6% | +118.6% / +133.9% | +90.9% / +116.4% | +117.8% / +173.4% (n/c) |
| Github Events (pretty) | +184.4% / +234.2% | +108.6% / +127.0% | +89.0% / +100.9% | +83.1% / +89.7% | +91.3% / +85.7% |
| Google Maps (min) | +54.4% / +60.0% | +3.8% / +3.4% | +8.3% / +12.6% | +24.4% / +24.4% | −8.5% / −14.5% |
| Google Maps (pretty) | +43.9% / +47.4% | −1.2% / −0.9% | +2.4% / +4.0% | +21.4% / +21.4% | −5.7% / −17.6% |
| Instruments (min) | +309.4% / +330.9% | +154.5% / +158.1% | +121.4% / +123.1% | +134.0% / +135.9% | +158.3% / +151.6% |
| Instruments (pretty) | +273.5% / +293.0% | +125.6% / +132.3% | +102.6% / +102.3% | +105.2% / +109.1% | +111.6% / +118.1% |
| Marine IK (min) | +34.8% / +35.6% | +50.6% / +54.7% | +47.4% / +49.6% | +47.2% (n/c) / +49.9% | +45.0% / +70.3% (n/c) |
| Marine IK (pretty) | +47.0% / +42.3% | +52.5% / +50.8% | +50.8% / +54.2% | +49.5% / +50.6% | +47.9% / +44.5% |
| Random (min) | +141.0% / +168.4% | +82.8% / +93.3% | +69.2% / +76.9% | +85.1% / +101.3% | +81.0% / +75.9% |
| Random (pretty) | +119.7% / +138.9% | +65.3% / +74.1% | +55.3% / +64.8% | +73.4% / +81.4% | +89.4% / +117.4% (n/c) |
| Twitter (min) | +331.0% / +381.9% | +136.3% / +148.3% | +122.0% / +128.1% | +112.1% / +123.2% | +145.1% / +155.6% |
| Twitter (pretty) | +306.4% / +351.6% | +123.4% / +134.1% | +98.4% / +105.7% | +92.7% / +101.4% | +141.9% / +139.2% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +49.9% / +73.1% | +27.2% / +34.3% | +39.6% / +52.3% | +45.2% / +63.3% | +37.6% / +33.0% |
| Apache Builds (pretty) | +50.4% / +69.6% | +24.7% / +30.3% | +36.9% / +42.7% | +32.7% / +44.3% | +6.4% / +32.9% |
| Canada (min) | +58.9% / +58.4% | +9.0% / +8.2% | tie / +0.9% | +18.8% / +17.8% | +53.2% / +48.4% |
| Canada (pretty) | +84.2% / +85.6% | +5.8% / +5.6% | +15.1% / +14.6% | +10.9% / +11.8% | +29.5% / +30.1% |
| CitmCatalog (min) | +42.7% / +45.6% | tie / +0.2% | +10.7% / +9.2% | +15.3% / +14.4% | −8.6% / +2.9% |
| CitmCatalog (pretty) | +55.1% / +59.3% | +0.8% / +0.3% | +8.6% / +8.0% | +16.8% / +17.5% | +3.4% / +8.0% |
| Discord (min) | +37.9% / +47.7% | +12.3% / +11.5% | +9.6% / +13.6% | +26.0% / +32.3% | +10.0% / +8.5% |
| Discord (pretty) | +35.4% / +45.8% | +11.8% / +11.6% | +9.0% / +9.2% | +21.1% / +26.8% | +4.6% / +4.9% |
| Github Events (min) | +65.8% / +84.6% | +13.0% / +12.4% | +17.2% / +22.7% | +38.7% / +40.9% | +7.4% / +8.6% |
| Github Events (pretty) | +66.8% / +79.1% | +9.1% / +8.6% | +14.8% / +15.0% | +21.0% / +22.4% | +1.3% / +11.6% |
| Google Maps (min) | +53.2% / +58.8% | −0.4% / +3.3% | +17.5% / +16.2% | +25.5% / +27.6% | +7.3% / tie |
| Google Maps (pretty) | +62.0% / +59.2% | +3.9% / +5.1% | +7.8% / +9.1% | +25.6% / +25.3% | +4.0% / −7.8% |
| Instruments (min) | +57.0% / +65.9% | +8.8% / +10.4% | +12.7% / +14.0% | +11.8% / +12.0% | +8.8% / tie |
| Instruments (pretty) | +59.7% / +67.8% | −1.0% / +0.8% | +8.9% / +10.1% | +11.3% / +12.0% | +1.6% / −2.1% |
| Marine IK (min) | +34.1% / +34.6% | +3.4% / +3.6% | −4.5% / −5.8% | −7.7% / −4.6% | +25.1% / +7.2% |
| Marine IK (pretty) | +64.5% / +64.7% | +3.9% / +4.5% | −7.4% / −8.2% | +6.8% / +7.2% | +12.8% / +9.3% |
| Random (min) | +57.7% / +65.1% | +11.7% / +12.8% | +19.3% / +21.9% | +40.4% / +46.1% | +29.4% / +23.7% |
| Random (pretty) | +59.6% / +67.3% | +7.6% / +9.8% | +9.4% / +13.8% | +31.2% / +35.1% | +7.1% / +34.4% |
| Twitter (min) | +67.3% / +72.5% | +9.0% / +8.5% | +13.1% / +13.1% | +26.5% / +27.2% | +12.3% / +12.3% |
| Twitter (pretty) | +68.8% / +76.3% | +7.3% / +7.3% | +7.6% / +7.2% | +14.7% / +14.1% | +3.5% / +3.5% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +46.6% / +58.5% | +15.8% / +19.9% | +23.8% / +26.9% | +34.1% / +44.0% | +29.2% / +31.1% |
| Apache Builds (pretty) | +44.2% / +57.4% | +6.7% / +6.9% | +15.2% / +17.6% | +29.3% / +36.6% | +24.7% / +13.5% |
| Canada (min) | +41.0% / +42.6% | +5.1% / +4.6% | tie / tie | +13.9% / +13.7% | +44.2% / +36.9% |
| Canada (pretty) | +65.3% / +67.8% | +3.6% / +4.1% | +11.6% / +11.4% | +9.1% / +9.1% | +14.9% / +28.4% |
| CitmCatalog (min) | +38.3% / +40.0% | −2.0% / −1.3% | +6.2% / +6.3% | +8.7% / +9.3% | +15.8% / +13.4% |
| CitmCatalog (pretty) | +44.2% / +43.5% | −1.1% / −1.7% | +1.0% / +2.8% | +10.3% / +10.4% | +14.5% / +13.3% |
| Discord (min) | +65.8% / +82.6% | +26.3% / +32.0% | +32.7% / +37.4% | +41.5% / +48.7% | +40.1% / +34.0% |
| Discord (pretty) | +63.0% / +74.5% | +23.1% / +27.0% | +24.7% / +34.4% | +31.7% / +39.4% | +44.4% / +41.8% |
| Github Events (min) | +90.6% / +108.5% | +25.2% / +26.5% | +33.7% / +36.2% | +46.6% / +50.8% | +40.3% / +45.0% |
| Github Events (pretty) | +79.3% / +89.7% | +20.4% / +20.1% | +26.0% / +28.5% | +28.6% / +31.3% | +24.2% / +32.3% |
| Google Maps (min) | +55.0% / +59.4% | −7.3% / −7.0% | +7.8% / +7.6% | +12.0% / +12.7% | +1.5% / −16.1% |
| Google Maps (pretty) | +53.5% / +56.2% | −6.0% / −5.9% | −0.4% / +0.8% | +12.7% / +14.2% | −13.7% / −20.3% |
| Instruments (min) | +80.3% / +93.3% | +22.7% / +23.9% | +22.2% / +22.6% | +25.3% / +26.4% | +11.2% / +30.8% |
| Instruments (pretty) | +78.3% / +82.3% | +14.0% / +15.9% | +15.9% / +14.4% | +20.0% / +20.5% | +20.4% / tie |
| Marine IK (min) | +54.7% / +55.7% | +46.0% / +38.3% | +22.7% / +23.6% | +24.3% / +18.3% | +41.9% / +56.3% |
| Marine IK (pretty) | +72.3% / +72.9% | +28.0% / +25.2% | +16.2% / +16.5% | +24.8% / +25.7% | +36.3% / +34.6% |
| Random (min) | +68.1% / +78.6% | +16.6% / +18.7% | +24.7% / +26.4% | +41.1% / +46.3% | +51.4% / +25.3% |
| Random (pretty) | +66.9% / +75.7% | +12.0% / +14.3% | +15.2% / +18.0% | +32.6% / +36.3% | +22.2% / +24.3% |
| Twitter (min) | +84.2% / +90.0% | +11.9% / +11.9% | +13.7% / +14.2% | +30.4% / +31.8% | +21.3% / +20.1% |
| Twitter (pretty) | +76.8% / +82.9% | +9.4% / +9.4% | +11.5% / +13.1% | +18.5% / +19.5% | +12.5% / +4.7% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +28.3% / +47.6% | +17.7% / +15.0% | +23.6% / +31.3% | +31.8% / +50.0% | +18.7% / +21.5% |
| Apache Builds (pretty) | +30.0% / +50.8% | +10.9% / +14.9% | +20.5% / +31.6% | +32.8% / +44.0% | +11.5% / +15.8% |
| Canada (min) | +0.7% / tie | +7.9% / +2.2% | −3.9% / −6.8% | −8.6% / +3.2% | +4.9% / +11.0% |
| Canada (pretty) | +4.3% / +3.3% | +6.3% / +4.4% | −6.1% / −9.1% | +2.4% / +2.6% | +2.2% / +1.5% |
| CitmCatalog (min) | +26.6% / +35.3% | +1.9% / +0.8% | +2.4% / +4.0% | +10.7% / +13.3% | +2.5% / +21.2% |
| CitmCatalog (pretty) | +28.2% / +36.1% | −4.8% / −3.1% | +3.2% / +4.6% | +10.5% / +11.1% | −4.0% / −17.9% |
| Discord (min) | +42.1% / +63.7% | −1.2% / +5.4% | +7.0% / +19.1% | +23.0% / +40.5% | +11.1% / +17.7% |
| Discord (pretty) | +43.6% / +65.0% | +0.6% / +6.9% | +8.4% / +17.5% | +23.6% / +36.8% | +5.4% / +6.3% |
| Github Events (min) | +58.0% / +71.2% | +9.3% / +8.0% | +3.3% / +3.6% | +24.2% / +29.0% | +13.1% / +16.7% |
| Github Events (pretty) | +54.6% / +70.4% | −0.4% / +2.8% | +3.1% / tie | +15.7% / +15.9% | −2.5% / +11.5% |
| Google Maps (min) | +43.4% / +49.5% | +17.2% / +17.2% | +28.8% / +31.2% | +30.8% / +36.5% | +11.9% / +13.3% |
| Google Maps (pretty) | +46.0% / +49.9% | +15.8% / +15.0% | +21.5% / +24.6% | +28.4% / +34.5% | +7.7% / +8.0% |
| Instruments (min) | +72.6% / +78.8% | −5.7% / −5.9% | +4.4% / +6.3% | +17.9% / +16.4% | +4.4% / +3.5% |
| Instruments (pretty) | +68.6% / +76.4% | −6.2% / −4.8% | +3.4% / +3.5% | +12.2% / +12.4% | tie / −1.5% |
| Marine IK (min) | +9.4% / +10.6% | −1.9% / −0.5% | +0.8% / +1.0% | +7.8% / +7.9% | +5.5% / +18.9% |
| Marine IK (pretty) | +16.1% / +18.3% | +0.6% / −0.5% | −2.2% / −2.6% | +5.0% / +6.5% | −6.0% / +4.9% |
| Random (min) | +52.7% / +66.4% | +17.2% / +22.4% | +32.7% / +38.0% | +41.7% / +55.6% | +24.0% / +28.7% |
| Random (pretty) | +54.4% / +69.4% | +12.4% / +18.4% | +28.8% / +34.6% | +42.9% / +53.0% | +20.5% / +23.2% |
| Twitter (min) | +48.0% / +60.2% | +3.3% / +4.6% | +9.2% / +15.3% | +28.8% / +34.4% | +5.3% / +9.5% |
| Twitter (pretty) | +46.6% / +61.0% | +1.2% / +2.2% | +8.8% / +12.8% | +19.4% / +38.7% | +16.0% / +5.1% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +92.8% / +127.0% | +78.9% / +86.8% | +68.6% / +82.2% | +55.2% / +97.9% | +73.6% / +96.8% |
| Apache Builds (pretty) | +88.7% / +120.4% | +69.4% / +75.6% | +53.1% / +68.1% | +57.1% / +77.3% | +62.6% / +83.8% |
| Canada (min) | +2.0% / +1.8% | +8.1% / +8.7% | −3.9% / −6.6% | +6.4% / +5.2% | +7.4% / +8.2% |
| Canada (pretty) | +4.1% / +3.1% | +4.6% / +2.9% | −4.8% / −6.1% | +4.8% / +3.8% | +4.7% / +5.2% |
| CitmCatalog (min) | +84.7% / +86.8% | +54.6% / +62.7% | +44.7% / +48.4% | +33.9% / +63.0% | +64.5% / +74.3% |
| CitmCatalog (pretty) | +77.5% / +80.8% | +45.5% / +50.2% | +39.1% / +42.9% | +47.5% / +54.6% | +48.0% / +60.0% |
| Discord (min) | +320.1% / +397.2% | +216.0% / +248.4% | +169.2% / +195.5% | +202.9% / +243.5% | +282.8% / +285.4% |
| Discord (pretty) | +296.6% / +366.7% | +183.8% / +218.1% | +155.9% / +184.4% | +174.0% / +212.7% | +230.3% / +298.3% |
| Github Events (min) | +87.2% / +102.7% | +24.1% / +29.3% | +21.5% / +23.1% | +40.8% / +45.5% | +58.5% / +67.3% |
| Github Events (pretty) | +76.2% / +89.2% | +17.7% / +26.0% | +12.3% / +16.0% | +26.9% / +30.0% | +24.3% / +38.5% |
| Google Maps (min) | +44.6% / +53.8% | +4.2% / +6.5% | +5.7% / +8.5% | +22.9% / +9.8% | −11.2% / +10.8% |
| Google Maps (pretty) | +31.6% / +38.3% | tie / tie | +0.8% / +1.5% | +20.7% / +21.9% | −9.1% / −14.6% |
| Instruments (min) | +307.2% / +317.5% | +141.1% / +145.6% | +88.0% / +89.3% | +124.5% / +134.9% | +150.5% / +160.8% |
| Instruments (pretty) | +270.3% / +283.7% | +121.3% / +122.9% | +80.1% / +81.3% | +102.2% / +81.3% | +124.6% / +132.0% |
| Marine IK (min) | +51.6% / +56.0% | +42.1% / +46.2% | +35.2% / +35.5% | +41.1% / +47.0% | +45.6% / +49.5% |
| Marine IK (pretty) | +50.3% / +54.1% | +35.2% / +38.2% | +32.2% / +33.3% | +36.6% / +39.8% | +39.5% / +45.0% |
| Random (min) | +131.8% / +159.5% | +75.9% / +86.7% | +60.0% / +65.7% | +84.1% / +99.9% | +92.3% / +101.7% |
| Random (pretty) | +113.1% / +135.2% | +56.5% / +65.5% | +46.5% / +53.8% | +69.1% / +78.6% | +75.7% / +82.7% |
| Twitter (min) | +311.1% / +336.9% | +130.6% / +138.3% | +97.7% / +101.4% | +118.7% / +130.9% | +146.6% / +151.3% |
| Twitter (pretty) | +285.5% / +312.6% | +129.4% / +139.4% | +83.1% / +83.0% | +101.0% / +109.6% | +132.3% / +140.2% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +46.2% / +87.0% | +23.8% / +26.3% | +38.2% / +46.1% | +45.8% / +68.8% | +23.0% / +27.2% |
| Apache Builds (pretty) | +46.2% / +75.1% | +17.3% / +21.3% | +27.9% / +33.6% | +32.2% / +43.3% | +12.9% / +12.3% |
| Canada (min) | +57.5% / +63.3% | +17.1% / +7.5% | +9.8% / +10.3% | +17.0% / +19.7% | +1.3% / +1.8% |
| Canada (pretty) | +89.7% / +97.1% | +18.1% / +14.2% | +14.7% / +15.3% | +25.7% / +12.3% | −0.7% / +15.5% |
| CitmCatalog (min) | +46.7% / +52.4% | +7.1% / +4.7% | +12.3% / +11.6% | +4.4% / +18.5% | +8.9% / +8.4% |
| CitmCatalog (pretty) | +52.6% / +59.1% | +2.6% / +1.1% | +8.6% / +10.2% | +14.3% / +15.7% | +3.4% / +3.2% |
| Discord (min) | +43.6% / +57.1% | +4.3% / +9.6% | +6.4% / +9.3% | +20.7% / +27.1% | +9.5% / +19.3% |
| Discord (pretty) | +43.6% / +52.5% | +1.5% / +2.9% | +6.5% / +6.5% | +12.6% / +16.7% | +1.5% / −15.5% |
| Github Events (min) | +62.6% / +75.9% | +4.2% / +4.6% | +11.4% / +12.4% | +26.7% / +32.3% | +5.1% / +4.8% |
| Github Events (pretty) | +48.6% / +58.6% | tie / −3.1% | +6.0% / +8.6% | +20.4% / +35.9% | −1.3% / −3.3% |
| Google Maps (min) | +46.5% / +55.2% | tie / tie | +11.7% / +11.7% | +29.0% / +28.4% | +1.4% / +1.9% |
| Google Maps (pretty) | +53.6% / +57.3% | +1.5% / +2.2% | +3.1% / +4.6% | +26.3% / +30.8% | −0.8% / +0.6% |
| Instruments (min) | +42.4% / +54.4% | +1.9% / +2.6% | +3.3% / +3.2% | +8.1% / +9.4% | +2.4% / +4.2% |
| Instruments (pretty) | +45.0% / +53.4% | −8.4% / −5.7% | −1.2% / −0.9% | +5.4% / +4.6% | −3.0% / −4.7% |
| Marine IK (min) | +30.4% / +34.4% | +2.6% / +4.1% | +8.5% / +6.4% | +9.2% / +8.1% | +8.4% / +8.1% |
| Marine IK (pretty) | +61.6% / +65.7% | +1.7% / +4.5% | +9.7% / +11.4% | +7.1% / tie | +1.6% / +0.8% |
| Random (min) | +47.5% / +55.5% | +16.1% / +13.5% | +19.5% / +18.0% | +28.8% / +37.3% | +12.2% / +13.9% |
| Random (pretty) | +43.1% / +54.7% | +7.5% / +8.9% | +11.4% / +12.3% | +22.7% / +39.2% | +10.2% / +7.5% |
| Twitter (min) | +22.6% / +25.4% | −2.6% / −2.3% | tie / −0.8% | +17.4% / +13.7% | −6.6% / +22.2% |
| Twitter (pretty) | +25.7% / +27.0% | −3.5% / −4.5% | +0.6% / +1.5% | +6.6% / +4.2% | +4.8% / −15.1% (n/c) |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.5% / +63.5% | +10.2% / +14.6% | +18.5% / +25.4% | +29.1% / +42.0% | +8.1% / +7.5% |
| Apache Builds (pretty) | +38.6% / +57.8% | +1.3% / +4.7% | +8.9% / +12.7% | +21.4% / +25.1% | +0.6% / +1.8% |
| Canada (min) | +43.0% / +45.2% | +24.3% / +22.9% | +4.2% / +4.2% | +13.2% / +13.4% | −21.7% / −2.6% |
| Canada (pretty) | +69.6% / +74.9% | +11.0% / +14.4% | +9.3% / +11.5% | +9.8% / +9.5% | −0.9% / −1.9% |
| CitmCatalog (min) | +43.3% / +43.8% | −4.2% / −2.6% | +2.3% / +1.8% | +11.2% / +10.5% | +15.5% / +5.1% |
| CitmCatalog (pretty) | +45.0% / +47.1% | −3.7% / −5.6% | +3.3% / +2.0% | +8.9% / +8.4% | +2.7% / −14.5% |
| Discord (min) | +81.7% / +102.2% | +31.6% / +39.7% | +26.9% / +31.5% | +48.7% / +58.2% | +5.8% / +47.0% |
| Discord (pretty) | +69.0% / +90.7% | +19.5% / +22.5% | +20.5% / +24.1% | +17.9% / +38.4% | +22.2% / +29.8% |
| Github Events (min) | +84.7% / +96.7% | +16.4% / +11.8% | +19.9% / +21.4% | +31.1% / +33.7% | +14.1% / +9.3% |
| Github Events (pretty) | +63.8% / +74.1% | +2.3% / +4.9% | +17.6% / +14.8% | +24.5% / +33.5% | +11.1% / +9.1% |
| Google Maps (min) | +55.0% / +62.2% | −8.7% / −8.3% | +1.5% / +2.0% | +15.7% / +15.1% | +1.2% / +0.7% |
| Google Maps (pretty) | +49.3% / +53.7% | −6.1% / −5.8% | −2.7% / −3.3% | +15.4% / +15.5% | +9.9% / −2.6% |
| Instruments (min) | +69.3% / +87.2% | +17.3% / +19.1% | +14.4% / +17.0% | +20.9% / +22.2% | +3.8% / +22.3% |
| Instruments (pretty) | +71.1% / +83.1% | +5.2% / +6.5% | +7.6% / +8.8% | +14.6% / +14.8% | +12.2% / +10.3% |
| Marine IK (min) | +37.8% / +44.2% | +39.1% / +41.8% | +32.0% / +31.0% | +32.7% / +34.1% | +32.0% / +32.3% |
| Marine IK (pretty) | +60.8% / +64.6% | +23.9% / +19.7% | +29.0% / +28.3% | +26.7% / +25.4% | +14.7% / +14.7% |
| Random (min) | +61.2% / +70.9% | +24.3% / +23.9% | +16.1% / +16.2% | +34.1% / +38.8% | +23.0% / +23.9% |
| Random (pretty) | +53.5% / +63.1% | +13.9% / +13.2% | +14.4% / +12.5% | +26.0% / +28.5% | +15.7% / +16.2% |
| Twitter (min) | +52.9% / +54.4% | +1.4% / +3.2% | +11.4% / +11.9% | +19.3% / +19.1% | +9.8% / +8.8% |
| Twitter (pretty) | +42.6% / +43.6% | +3.2% / +2.7% | +7.3% / +8.8% | +8.4% / +8.2% | tie / −11.1% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 4.8x faster at full size: +92.7% (Twitter pretty, macOS/GCC, fresh) up to +381.9% (Twitter min, MSVC, reused), and up to +350.9% outside MSVC (Discord min, macOS/Clang, fresh). The small documents show the same pattern, from +80.1% to +397.2%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −17.6% (prettified Google Maps on macOS/Clang, reused) to +24.4%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events almost never loses.** It accounts for 5 of the 118 single-document losses, all of them prettified Github Events Small on a Clang build, from −0.4% to −3.3%. Read in reverse at full size it wins on every build (+83.1% to +258.8%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 185 of 200 full-size sparse-reverse results across both allocation modes (3 ties, 12 losses), and 182 of 200 small ones (1 tie, 17 losses). MSVC and macOS/GCC won all 80. All 12 full-size losses are on CitmCatalog or Google Maps: 8 on Linux/Clang (−1.1% to −7.3%), 1 on Linux/GCC (−0.4%) and 3 on macOS/Clang (−13.7% to −20.3%). 16 of the 17 small losses are on Canada, CitmCatalog or Google Maps; the other is prettified Twitter Small on macOS/Clang (reused, −11.1%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 186.5 MB/s on MSVC against 307.0 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 667.1 and 873.4 MB/s. Every MSVC full-size sparse result is a win, by +34.1% to +85.6%, and every full-size sparse-reverse result by +38.3% to +108.5%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 185 of 200 full-size sparse results (4 ties, 11 losses) and 176 of 200 small ones (5 ties, 19 losses). MSVC won all 40 of its full-size results and all 40 small. The full-size losses are on Linux/GCC (all Marine IK, −4.5% to −8.2%), macOS/Clang (3), Linux/Clang (2) and macOS/GCC (2, both Marine IK). macOS/Clang holds 9 of the 19 small losses, Linux/Clang 7 and Linux/GCC 3.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 178, tied 8 and lost 14. Linux/Clang went 39-1-0, MSVC 35-1-4, Linux/GCC 35-1-4, macOS/GCC 35-3-2 and macOS/Clang 34-2-4 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 174, tied 3 and lost 23. MSVC went 39-1-0, macOS/GCC 39-0-1, macOS/Clang 34-1-5, Linux/GCC 33-1-6 and Linux/Clang 29-0-11. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The losses are mostly modest and concentrate on Marine IK, Canada and Instruments: Linux/Clang's run from −0.4% to −6.2%, and the largest is prettified CitmCatalog on macOS/Clang (reused, −17.9%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−5.4% to −6.8%). MSVC lost minified Canada and minified Marine IK in both runs (−1.3% to −2.6%), and macOS/GCC lost minified Marine IK in both runs (−1.5% and −1.6%). macOS/Clang's four losses are all reused runs: CitmCatalog in both forms (minified −2.8%, prettified −30.5%), prettified Instruments (−7.3%) and prettified Twitter (−4.0%). Linux/Clang lost none. Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 43-0-7 on them. Bool, Double and Int64 won all 10 results each (+1.4% to +170.2%, +51.7% to +214.4% and +31.8% to +185.1%), and Uint64 won 8 (+35.4% to +175.6%, with both losses on Linux/GCC, −2.2% and −1.4%). String is the exception: 5 wins and 5 losses, on Linux/Clang in both runs (−6.9% and −7.2%), macOS/Clang in both runs (−31.6% and −23.6%) and Linux/GCC reused (−2.0%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on MSVC and Linux/GCC, Jsonifier's comma-separated throughput is 3.4% to 11.5% below its own NDJSON throughput on the same records. Linux/Clang agrees on three of the six streams (5.5% to 7.5%) but not on the rest: Apache Builds is 45.6% below, Google Maps 27.9% below (its NDJSON result did not converge), and Instruments is 51.6% above, because that build's Instruments (NDJSON) result is unusually low (§8.5).

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs from the 26 streaming tests that existed at the time are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`, before the Apache Builds streams were added.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 30 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 22 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 27 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 19 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 4-0-2 | 10-1-1 | 8-2-2 | 22-3-5 |
| Linux / GCC | 5-0-1 | 12-0-0 | 12-0-0 | 29-0-1 |
| macOS / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / Clang | 3-2-1 | 10-1-1 | 12-0-0 | 25-3-2 |
| **All** | **24-2-4** | **56-2-2** | **56-2-2** | **136-6-8** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +38.9% | +18.6% | +15.5% | +25.1% | +18.5% |
| Large Amazon Cellphones (NDJSON) | +20.3% | +9.9% | +2.6% | +10.7% | +29.8% |
| Stream Formats Small (NDJSON) | +87.5% | −0.9% | −0.9% | +8.7% | tie |
| Stream Formats Small (comma) | +149.9% | +73.5% | +101.0% | +90.4% | +60.5% |
| Stream Formats Large (NDJSON) | +101.1% | −4.7% | +21.7% | +10.7% | −7.4% |
| Stream Formats Large (comma) | +105.9% | +3.8% | +30.1% | +13.7% | tie |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +63.1% | +89.6% | +17.0% | +33.4% | +26.5% |
| Apache Builds (comma) | +69.7% | +19.0% (n/c) | +25.0% | +45.3% | +36.8% |
| CitmCatalog (NDJSON) | +53.5% | tie | +14.9% | +6.2% | +11.9% (n/c) |
| CitmCatalog (comma) | +62.1% | +17.3% (n/c) | +38.5% | +27.4% | +22.5% (n/c) |
| Google Maps (NDJSON) | +65.3% | +47.3% (n/c) | +27.4% | +25.6% | tie (n/c) |
| Google Maps (comma) | +67.3% | +8.0% (n/c) | +44.5% | +39.4% | +18.7% |
| Instruments (NDJSON) | +125.1% | −33.1% | +20.7% | +16.3% | −4.4% |
| Instruments (comma) | +135.7% | +33.2% | +63.4% | +44.1% | +3.3% |
| Random (NDJSON) | +73.2% | +25.5% | +67.5% | +45.2% | +77.8% (n/c) |
| Random (comma) | +77.5% | +39.1% | +88.9% | +61.6% | +88.1% (n/c) |
| Twitter (NDJSON) | +60.1% | +9.6% | +3.1% | +11.3% | +28.1% |
| Twitter (comma) | +63.5% | +21.1% | +20.9% | +30.2% | +21.6% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +57.0% | −6.8% (n/c) | +6.0% | +20.9% | +20.8% |
| Apache Builds (comma) | +64.8% | +15.6% | +23.7% | +46.6% (n/c) | +37.7% |
| CitmCatalog (NDJSON) | +65.7% | tie (n/c) | +34.0% | +26.6% | +32.5% (n/c) |
| CitmCatalog (comma) | +76.4% | +23.5% (n/c) | +59.9% | +49.4% | +45.6% |
| Google Maps (NDJSON) | +42.9% | −13.0% (n/c) | +5.3% | +13.1% | +9.6% |
| Google Maps (comma) | +46.3% | tie | +25.9% | +29.1% | +5.5% (n/c) |
| Instruments (NDJSON) | +125.8% | +32.3% | +26.5% | +21.5% (n/c) | +6.8% |
| Instruments (comma) | +139.1% | +44.0% | +55.9% | +48.0% | +78.5% (n/c) |
| Random (NDJSON) | +136.8% | +66.7% | +55.7% | +67.6% | +80.8% (n/c) |
| Random (comma) | +138.5% | +81.0% | +75.8% | +93.5% | +98.2% (n/c) |
| Twitter (NDJSON) | +383.9% | +161.2% | +148.4% | +139.4% (n/c) | +149.2% (n/c) |
| Twitter (comma) | +372.0% | +170.3% | +166.7% | +131.6% | +187.5% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 24 of the 30 results in that group, tied 2 and lost 4. It won Amazon Cellphones (+15.5% to +38.9%), Large Amazon Cellphones (+2.6% to +29.8%) and Stream Formats Small (comma) (+60.5% to +149.9%) on all five builds. The four losses are all Stream Formats read as NDJSON: Small on Linux/Clang (−0.9%) and Linux/GCC (−0.9%), and Large on Linux/Clang (−4.7%) and macOS/Clang (−7.4%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC and macOS/GCC won every streaming test,** 30-0-0 each; Linux/GCC lost one of 30, macOS/Clang went 25-3-2 and Linux/Clang 22-3-5. All five Linux/Clang losses are NDJSON streams, and 8 of its 30 results did not converge.
- **In-order corpus streams lose only Instruments (NDJSON) on the Clang builds.** Of the 60 in-order corpus results, Jsonifier won 56, tied 2 and lost 2. The losses are −4.4% on macOS/Clang and −33.1% on Linux/Clang; both converged. The Linux/Clang figure is an outlier: Jsonifier reads the same records at 1,020.5 MB/s as NDJSON and 1,547.5 MB/s as comma-separated on that build, while its NDJSON read is the faster of the two on the other four builds.
- **Reverse order carries over from single documents to streams.** Jsonifier won 56 of the 60 reverse stream results, tied 2 and lost 2; all four non-wins are on Linux/Clang, and both losses (Apache Builds and Google Maps, NDJSON, −6.8% and −13.0%) did not converge. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +131.6% to +383.9%, while Google Maps rows, which have few, give −13.0% to +46.3%. Instruments, whose reverse streams have the most room to lose, wins on every build (+6.8% to +139.1%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 246.9 MB/s under MSVC and 474.6 MB/s under Linux/Clang, while Jsonifier reads it at 589.0 and 858.8 MB/s.

## 9. Limitations

- **The Clang builds hold the most single-document losses:** 40 of the 118 each on Linux/Clang and macOS/Clang, against 29 on Linux/GCC, 5 on macOS/GCC and 4 on MSVC. 26 of Linux/Clang's are small documents, led by 11 small in-order, and 24 of macOS/Clang's, led by 9 small sparse. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's String POD (−31.6% fresh, −23.6% reused) and its prettified CitmCatalog read in order (reused, −30.5%); the largest x86 loss is Linux/GCC's prettified Canada Small read in order (reused, −9.1%).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 65 of the 118: Google Maps 27 (13 on macOS/Clang, 11 on Linux/Clang), Canada 22 (14 on Linux/GCC) and CitmCatalog 16 (10 on Linux/Clang). Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 14 of its 29 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 66 of the 118 losses are small documents (26 on Linux/Clang, 24 on macOS/Clang, 15 on Linux/GCC, 1 on macOS/GCC, none on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats read as NDJSON** lose on three builds: Small on Linux/Clang and Linux/GCC (both −0.9%), and Large on Linux/Clang (−4.7%) and macOS/Clang (−7.4%). Four corpus record-stream results also lose, all NDJSON on Clang builds: Instruments in order on Linux/Clang (−33.1%) and macOS/Clang (−4.4%), and Apache Builds and Google Maps in reverse on Linux/Clang (−6.8% and −13.0%, n/c). The Linux/Clang Instruments result has not been explained.
- **Some results did not converge.** 5 of the 1,650 single-document results did not converge: 4 on macOS/Clang and 1 on macOS/GCC. 22 of the 150 streaming results did not converge: 11 on macOS/Clang, 8 on Linux/Clang and 3 on macOS/GCC. Windows/MSVC and Linux/GCC converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,503, tied 29 and lost 118. On reverse reads it won 380 of 400 at both sizes, and on Windows/MSVC it lost 4 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (753-15-57 fresh, 750-14-61 reused). The main remaining targets are the small-object documents (Canada, Google Maps and CitmCatalog, with 65 of the 118 losses), small documents in general, and the Clang builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 136, tied 6 and lost 8, and on the 120 corpus record-stream results it went 112-4-4. NDJSON streams on the Clang builds are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.