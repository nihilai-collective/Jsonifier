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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,490 of 1,650 single-document results against simdjson On Demand, tied 44 and lost 116. On reverse-order reads it won 374 of 400. On Windows/MSVC it won 315 of 330 single-document results, tied 11 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 139 of 150 results, tied 4 and lost 7, and it won 116 of the 120 corpus record-stream results, tied 3 and lost 1.

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
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 51 of the 1,650 single-document results and 28 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 146 / 170 | 144 / 160 | 290 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 165 / 170 | 160 / 160 | 325 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 165 / 170 | 159 / 160 | 324 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `ed43ce8`, Jsonifier `28b18c4` and simdjson `ebd28cbd4`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The Linux builds ran on November 3, 2026, the others on November 4, and the results were published to `generic-parsing-main` on November 4, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-2 | 18-2-0 | 20-0-0 | 20-0-0 | 17-2-1 | 18-2-0 | 20-0-0 | 20-0-0 | 154-8-3 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 18-1-1 | 17-0-3 | 20-0-0 | 17-0-3 | 17-1-2 | 152-2-11 |
| Linux / GCC | 5-0-0 | 16-1-3 | 17-1-2 | 17-0-3 | 19-0-1 | 13-0-7 | 17-0-3 | 19-0-1 | 19-0-1 | 142-2-21 |
| macOS / GCC | 5-0-0 | 19-0-1 | 20-0-0 | 17-2-1 | 20-0-0 | 19-1-0 | 19-0-1 | 19-0-1 | 20-0-0 | 158-3-4 |
| macOS / Clang | 4-0-1 | 20-0-0 | 17-0-3 | 16-2-2 | 20-0-0 | 12-2-6 | 18-0-2 | 13-1-6 | 16-3-1 | 136-8-21 |
| **All** | **23-0-2** | **90-3-7** | **92-3-5** | **90-4-6** | **97-1-2** | **78-5-17** | **92-2-6** | **88-1-11** | **92-4-4** | **742-23-60** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-2-1 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 161-3-1 |
| Linux / Clang | 4-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 19-0-1 | 18-0-2 | 20-0-0 | 17-0-3 | 17-0-3 | 154-0-11 |
| Linux / GCC | 4-0-1 | 16-1-3 | 18-1-1 | 17-1-2 | 18-1-1 | 14-0-6 | 17-0-3 | 20-0-0 | 19-0-1 | 143-4-18 |
| macOS / GCC | 5-0-0 | 19-1-0 | 20-0-0 | 16-1-3 | 20-0-0 | 17-1-2 | 19-0-1 | 16-2-2 | 18-0-2 | 150-5-10 |
| macOS / Clang | 4-0-1 | 18-0-2 | 18-0-2 | 17-0-3 | 18-1-1 | 17-1-2 | 19-0-1 | 16-2-2 | 13-5-2 | 140-9-16 |
| **All** | **22-0-3** | **89-4-7** | **95-2-3** | **90-2-8** | **95-2-3** | **86-2-12** | **95-0-5** | **89-4-7** | **87-5-8** | **748-21-56** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (51 of them did not converge; see §9), Jsonifier won 1,490, tied 44 and lost 116: 742-23-60 with fresh output objects and 748-21-56 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 187 of 200 full-size reverse results (5 ties, 8 losses) and 187 of 200 small reverse results (2 ties, 11 losses). Windows/MSVC and Linux/Clang lost none of their 80, and macOS/GCC lost two. Every loss is on Canada or Google Maps. The 8 full-size losses are minified Canada on Linux/GCC (−5.4% fresh, −3.6% reused), prettified Google Maps on Linux/GCC (fresh, −3.0%), prettified Canada on macOS/Clang (fresh, −4.9%) and Google Maps on macOS/Clang in both forms and both runs (−5.7% to −17.7%). The 11 small losses are Canada Small on Linux/GCC (4), prettified Google Maps Small on Linux/GCC (2), prettified Canada Small on macOS/GCC (2), and on macOS/Clang minified Canada Small and Google Maps Small (3, −8.3% to −16.6%).
- **Windows/MSVC lost four of its 330 single-document results,** all in-order reads of Canada or Marine IK: prettified Canada (fresh, −5.3%), minified Marine IK (fresh, −15.5%, n/c), prettified Marine IK (reused, −7.2%) and prettified Canada Small (fresh, −8.3%). It won all 10 POD results and all 160 of its sparse and sparse-reverse results, and lost no reverse read.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 8.1–14.2% for Jsonifier and 6.6–9.6% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.2 to +3.1 percentage points on the non-MSVC builds and by +8.8 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +281.0% to +314.6%, and Apache Builds (pretty) from +5.0% to +33.2%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +113.8% / +115.1% | +108.2% / +107.3% | +132.6% / +118.9% | +35.6% / +73.6% | +6.4% / +21.8% |
| Double | +118.8% / +79.6% | +94.9% / +96.2% | +101.2% / +104.2% | +122.0% / +143.9% | +60.9% / +77.7% |
| Int64 | +171.0% / +191.6% | +138.3% / +145.5% | +70.0% / +74.3% | +136.5% / +102.1% | +66.6% / +53.4% |
| String | +119.5% / +56.6% | −3.7% / −5.7% | +2.2% / −1.7% | +3.4% (n/c) / +16.2% | −14.7% / −21.4% |
| Uint64 | +98.8% / +159.1% | +143.0% / +143.8% | +96.7% / +102.6% | +87.2% / +81.6% | +44.1% / +40.4% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +7.0% / +33.0% | +21.8% / +34.1% | +22.6% / +34.5% | +24.4% / +57.6% | +29.6% / +29.9% |
| Apache Builds (pretty) | +5.0% / +33.2% | +17.8% / +26.9% | +22.1% / +34.2% | +38.0% / +39.3% | +33.1% / +15.0% |
| Canada (min) | tie / tie | +8.2% / +5.1% | −7.3% / −8.7% | +3.5% / +4.2% | +27.2% / +18.1% |
| Canada (pretty) | −5.3% / +7.5% | +3.7% / +4.8% | −4.3% / −6.8% | +3.9% / +3.7% | +5.4% / −4.3% (n/c) |
| CitmCatalog (min) | +17.3% / +20.6% | +9.1% / +8.3% | +11.5% / +11.9% | +19.8% / +12.3% | +9.9% / +11.0% |
| CitmCatalog (pretty) | +23.9% / +26.8% | +6.0% / +7.0% | +4.4% / +5.5% | +9.0% / +10.4% | +9.7% / −29.2% |
| Discord (min) | +34.4% / +44.6% | +18.5% / +24.1% | +17.3% / +21.3% | +24.3% / +28.8% | +12.1% / +15.8% |
| Discord (pretty) | +29.7% / +36.5% | +12.7% / +19.7% | +16.2% / +21.6% | +23.7% / +26.5% | +24.4% / +15.9% |
| Github Events (min) | +52.7% / +71.4% | +30.7% / +35.7% | +17.5% / +20.9% | +37.1% / +53.4% | +47.1% / +43.7% |
| Github Events (pretty) | +36.4% (n/c) / +75.4% | +25.4% / +29.5% | +13.8% / +16.7% | +29.5% / +30.2% | +24.8% / +15.7% |
| Google Maps (min) | +22.7% / +42.6% | +26.5% / +29.7% | +22.1% / +26.0% | +31.0% / +30.9% | +15.4% / +13.9% |
| Google Maps (pretty) | +34.5% / +32.6% (n/c) | +16.0% / +17.4% | +18.4% / +19.8% | +18.1% / +25.5% | +11.5% / +9.2% |
| Instruments (min) | +25.5% (n/c) / +68.2% (n/c) | +7.1% / +7.9% | +6.8% / +6.6% | +2.4% / +13.6% | +10.9% / +1.4% |
| Instruments (pretty) | +45.1% (n/c) / +82.4% | −2.5% / −1.9% | +5.1% / +6.3% | +6.3% / +10.6% | +1.1% / +6.9% |
| Marine IK (min) | −15.5% (n/c) / tie (n/c) | +7.0% / +6.9% | +1.7% / tie | −5.3% / +4.6% | +14.3% / +9.4% (n/c) |
| Marine IK (pretty) | tie / −7.2% | +2.0% / +2.9% | −2.5% / −3.1% | +1.6% / tie | +23.3% / +7.8% |
| Random (min) | +32.9% / +47.0% (n/c) | +26.7% / +32.4% | +35.2% / +39.7% | +52.4% / +49.9% | +41.2% / +38.4% |
| Random (pretty) | +30.8% / +33.2% (n/c) | +23.1% / +26.6% | +29.9% / +37.5% | +42.7% / +54.6% | +27.0% / +34.3% |
| Twitter (min) | +40.0% / +49.7% | +5.2% / +15.9% | +17.0% / +20.7% | +20.6% / +24.3% | +7.9% / +9.7% |
| Twitter (pretty) | +32.0% / +48.1% | +3.3% / +10.4% | tie / +11.0% | +16.7% / +20.5% | +6.7% / +6.2% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +48.4% / +80.5% | +65.8% / +83.1% | +62.9% / +82.7% | +34.8% / +61.2% | +44.8% / +107.8% |
| Apache Builds (pretty) | +42.8% / +75.8% | +55.7% / +73.1% | +61.1% / +74.7% | +51.2% / +68.4% | +59.8% / +69.0% |
| Canada (min) | tie (n/c) / tie | +9.8% / +9.4% | −5.4% / −3.6% | +7.1% (n/c) / +8.3% | +17.4% (n/c) / +11.6% |
| Canada (pretty) | tie / +7.4% (n/c) | +9.6% / +9.4% | tie / tie | +16.1% / +9.3% | −4.9% / +5.7% |
| CitmCatalog (min) | +83.6% (n/c) / +77.1% | +68.6% / +71.0% | +67.4% / +74.4% | +56.9% / +67.9% | +64.5% / +70.3% (n/c) |
| CitmCatalog (pretty) | +83.8% / +90.8% | +62.0% / +66.3% | +60.8% / +67.4% | +59.0% / +60.1% | +51.9% / +75.8% |
| Discord (min) | +281.0% / +314.6% | +245.0% / +272.1% | +221.9% / +244.8% | +238.5% / +278.5% | +312.7% / +349.8% |
| Discord (pretty) | +255.0% / +295.7% | +225.6% / +248.2% | +201.7% / +221.0% | +221.8% / +230.4% | +261.2% / +257.4% |
| Github Events (min) | +210.7% / +278.3% | +137.1% / +155.9% | +123.3% / +143.1% | +99.8% / +126.3% | +125.3% / +178.7% |
| Github Events (pretty) | +179.9% / +239.9% | +109.5% / +126.2% | +99.8% / +111.6% | +81.1% / +97.3% | +91.2% / +118.5% |
| Google Maps (min) | +22.0% (n/c) / +18.3% (n/c) | +12.4% / +13.3% | +6.7% / +10.0% | +25.2% / +19.4% | −5.7% / −10.1% |
| Google Maps (pretty) | +30.1% / +23.6% | +4.7% / +5.4% | −3.0% / +0.4% | +20.7% / +25.5% | −5.8% / −17.7% |
| Instruments (min) | +297.0% / +336.3% | +167.6% / +174.3% | +138.4% / +141.3% | +136.1% / +139.2% | +169.2% / +190.3% |
| Instruments (pretty) | +248.6% / +306.1% | +150.5% / +155.4% | +115.0% / +118.8% | +116.2% / +128.0% | +150.0% / +126.0% |
| Marine IK (min) | +35.1% / +28.4% (n/c) | +54.8% / +57.5% | +43.6% / +46.7% | +65.0% (n/c) / +60.2% | +41.5% / +56.5% |
| Marine IK (pretty) | +40.2% / +50.4% (n/c) | +51.4% / +55.6% | +52.3% / +52.8% | +30.8% / +21.3% (n/c) | +57.5% / +61.6% |
| Random (min) | +140.1% / +143.8% | +83.8% / +98.3% | +64.2% / +74.9% | +83.8% / +96.8% | +99.3% / +114.5% |
| Random (pretty) | +113.9% / +151.2% (n/c) | +74.1% / +85.8% | +51.6% / +59.3% | +72.0% / +90.4% | +79.1% / +92.8% |
| Twitter (min) | +338.1% / +379.6% | +143.5% / +155.0% | +128.4% / +135.5% | +129.4% / +192.8% | +158.0% / +168.2% |
| Twitter (pretty) | +312.3% / +360.1% | +120.4% / +127.9% | +103.2% / +112.5% | +57.9% / +83.7% | +147.3% / +208.3% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +27.7% / +50.2% | +32.8% / +44.5% | +38.5% / +56.1% | +40.3% / +80.3% | +20.8% / +27.9% |
| Apache Builds (pretty) | +36.4% / +38.2% | +27.1% / +30.5% | +38.0% / +47.5% | +33.1% / +32.2% | +21.2% / +24.1% |
| Canada (min) | +58.1% / +58.4% | +6.9% / +6.4% | −0.3% / tie | +17.1% / +10.6% | +33.8% / +35.7% |
| Canada (pretty) | +90.5% / +91.3% | +6.6% / +7.0% | +14.3% / +17.5% | tie / tie | +29.7% / +20.3% |
| CitmCatalog (min) | +44.8% / +42.8% | +12.9% / +11.8% | +8.8% / +9.6% | +15.5% / +15.8% | −13.8% / −2.6% |
| CitmCatalog (pretty) | +52.5% / +57.1% | +5.8% / +5.7% | +7.9% / +9.3% | +16.8% / +13.8% | +22.3% / +10.1% (n/c) |
| Discord (min) | +18.3% / +17.4% | +12.3% / +12.2% | +20.6% / +15.8% | +24.3% / +35.5% | +11.0% / +12.9% |
| Discord (pretty) | +19.0% / +31.2% | +5.3% / +2.8% | +17.9% / +10.2% | +23.1% / +27.4% | +7.5% / +15.1% |
| Github Events (min) | +45.4% / +72.0% | +12.6% / +14.4% | +18.5% / +20.1% | +41.6% / +41.7% | +26.3% / +6.5% |
| Github Events (pretty) | +52.7% / +73.0% | +8.7% / +8.3% | +14.0% / +13.5% | +17.5% / +22.5% | tie / +1.9% |
| Google Maps (min) | +44.2% / +47.2% | +13.5% / +14.2% | +18.4% / +16.1% | +22.9% / +24.0% | +6.6% / +5.7% |
| Google Maps (pretty) | +52.5% (n/c) / +74.6% | +6.2% / +5.5% | +4.6% / +4.8% | +18.3% / +23.6% | tie / +5.9% |
| Instruments (min) | +62.4% / +74.0% (n/c) | +16.4% / +17.5% | +8.1% / +9.7% | tie / +20.7% | +8.0% / +9.3% |
| Instruments (pretty) | +69.0% / +64.4% | +7.7% / +7.6% | +9.2% / +8.7% | +19.6% / −9.5% | +4.6% / +7.0% |
| Marine IK (min) | +35.5% / +40.4% | +6.9% / +7.3% | −3.4% / −3.0% | −15.3% / −15.3% | −2.0% / −3.7% |
| Marine IK (pretty) | +49.1% (n/c) / +66.2% (n/c) | +4.8% / +4.0% | −9.8% / −9.1% | +31.3% (n/c) / −11.6% | +8.2% / +26.0% |
| Random (min) | +36.8% / +45.8% | +18.2% / +20.5% | +21.5% / +24.0% | +41.4% / +48.0% | +27.6% / +29.4% |
| Random (pretty) | +42.4% / +45.1% | +12.6% / +15.1% | +12.0% / +14.7% | +31.5% / +35.7% | +3.7% / +12.1% |
| Twitter (min) | +93.3% / +78.5% (n/c) | +13.1% / +13.3% | +16.2% / +16.5% | +20.9% / +20.5% | +26.0% / +12.3% |
| Twitter (pretty) | +71.6% / +69.0% | +5.8% / +6.2% | +12.9% / +11.1% | +15.0% / +24.2% | +31.1% / −2.2% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +29.9% / +37.1% | +20.2% / +23.9% | +26.5% / +25.6% | +30.4% / +48.2% | +15.1% / +12.5% |
| Apache Builds (pretty) | +19.2% / +43.5% | +10.2% / +13.5% | +17.3% / +17.2% | +29.5% / +36.8% | +13.9% / +15.8% |
| Canada (min) | +40.1% / +39.8% | +6.2% / +5.4% | +1.1% / tie | +11.5% / +11.5% | +36.1% / +30.3% |
| Canada (pretty) | +67.5% / +69.5% | +5.2% / +5.1% | +9.2% / +11.9% | +20.6% / +10.0% | +25.2% / +25.1% |
| CitmCatalog (min) | +29.1% / +33.4% | +2.8% / +3.0% | +7.7% / +7.1% | +7.4% / +10.8% | +3.9% / −14.4% |
| CitmCatalog (pretty) | +33.4% / +32.9% | tie / +0.3% | +3.8% / +4.0% | +10.2% / +7.3% | +22.6% / tie |
| Discord (min) | +73.8% / +69.7% | +32.4% / +38.6% | +34.3% / +42.3% | +43.1% / +45.9% | +12.9% / +52.8% |
| Discord (pretty) | +52.1% / +81.8% | +30.5% / +34.7% | +30.9% / +37.4% | +32.7% / +40.5% | +39.9% / +34.8% |
| Github Events (min) | +86.8% / +107.5% | +38.5% / +40.5% | +32.5% / +31.6% | +44.7% / +57.0% | +32.8% / +21.3% |
| Github Events (pretty) | +88.0% (n/c) / +114.1% | +27.5% / +30.4% | +24.5% / +23.9% | +27.0% / +32.4% | +20.5% / +16.1% |
| Google Maps (min) | +45.5% (n/c) / +70.9% | +0.8% / +0.8% | +2.5% / +2.6% | +11.3% / +15.3% | +4.1% / +1.4% |
| Google Maps (pretty) | +48.9% / +44.0% | −0.4% / −0.6% | −1.2% / −0.9% | +29.6% / +10.1% | +9.6% / +2.8% |
| Instruments (min) | +69.9% / +76.9% | +32.1% / +33.3% | +22.3% / +21.5% | +17.9% / +17.1% | +30.4% / +30.2% |
| Instruments (pretty) | +65.8% / +79.2% | +20.1% / +21.2% | +15.7% / +15.8% | +24.8% / +21.3% | +24.3% / +40.4% |
| Marine IK (min) | +64.3% / +59.6% | +29.3% / +24.7% | +27.6% / +25.8% | +10.8% / +23.6% | +35.9% / +29.2% |
| Marine IK (pretty) | +75.2% / +71.0% | +25.4% / +25.6% | +14.9% / +14.0% | +21.2% / +32.9% | +41.9% / +32.5% |
| Random (min) | +66.0% / +76.3% | +27.7% / +29.9% | +22.5% / +24.6% | +67.3% / +49.1% | +30.5% / +33.0% |
| Random (pretty) | +71.2% / +82.4% | +17.2% / +19.8% | +11.4% / +15.8% | +24.7% / +30.1% | +3.6% / +17.5% |
| Twitter (min) | +82.8% / +101.1% | +20.5% / +21.1% | +17.0% / +16.3% | +33.4% / +13.0% | +27.6% / +24.4% |
| Twitter (pretty) | +78.7% / +83.5% | +12.9% / +13.4% | +15.2% / +15.6% | +22.1% / +19.7% | +14.6% / +9.7% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.1% / +21.0% | +22.8% / +32.0% | +21.9% / +37.8% | +35.8% / +50.0% | +14.6% / +24.3% |
| Apache Builds (pretty) | +14.6% / +22.9% | +19.1% / +25.7% | +18.1% / +30.4% | +31.2% / +43.0% | +26.4% / +5.1% |
| Canada (min) | +7.6% / +1.5% | +3.8% / +5.4% | −8.0% / −10.0% | +1.5% / +2.3% | −6.6% / +11.4% |
| Canada (pretty) | −8.3% / +7.7% | +3.5% / +3.2% | −3.9% / −11.2% | tie / −2.4% | tie / −5.1% |
| CitmCatalog (min) | +8.7% / +21.6% (n/c) | −1.0% / +1.9% | −0.9% / +0.7% | +10.1% / tie | −2.7% / +7.2% |
| CitmCatalog (pretty) | +26.9% / +15.4% | −3.3% / −1.1% | −1.4% / −0.4% | +9.0% / +11.5% | −2.3% / −18.1% |
| Discord (min) | tie / +31.4% | +4.4% / +11.6% | +5.6% / +14.1% | +22.1% / +40.3% | +13.7% / +22.8% |
| Discord (pretty) | +14.0% / +33.5% | +6.6% / +9.7% | +5.8% / +14.5% | +18.9% / +36.5% | +5.2% / +12.3% |
| Github Events (min) | +47.4% / +63.4% | +13.6% / +14.7% | +6.0% / +2.8% | +21.4% / +29.2% | +16.1% / +21.6% |
| Github Events (pretty) | +52.3% / +56.5% | +7.3% / +9.4% | −3.0% / −0.6% | +16.3% / +19.7% | tie / +13.1% |
| Google Maps (min) | +38.4% / +37.4% | +30.2% / +30.1% | +24.3% / +25.3% | +35.0% / +36.5% | +28.2% / +13.7% |
| Google Maps (pretty) | +50.8% (n/c) / +37.9% | +18.5% / +18.9% | +18.5% / +17.6% | +28.0% / +26.4% | +35.4% / +12.0% |
| Instruments (min) | +37.8% / +46.7% | +3.9% / +1.1% | +1.8% / +2.2% | +12.8% / +14.0% | −9.9% / +18.2% |
| Instruments (pretty) | +49.5% / +55.6% | −5.4% / −5.7% | +0.3% / +0.7% | +11.9% / −1.1% | +4.0% / tie |
| Marine IK (min) | tie / +2.6% | +6.4% / +7.5% | −1.4% / −1.7% | +9.0% / +8.8% | +2.6% / +14.8% |
| Marine IK (pretty) | +8.0% / +8.3% | +3.8% / +1.8% | −3.0% / −2.5% | +4.9% / +6.3% | −5.5% / +2.7% |
| Random (min) | +30.7% / +30.0% | +25.5% / +31.5% | +23.9% / +29.0% | +45.3% / +78.0% | +24.9% / +49.0% |
| Random (pretty) | +39.8% (n/c) / +48.9% | +17.3% / +21.6% | +19.3% / +25.7% | +42.1% / +52.0% | +14.9% / +46.3% |
| Twitter (min) | +31.5% / +39.8% | +7.8% / +5.6% | +8.4% / +7.4% | +24.3% / +14.1% | −1.9% / +4.9% |
| Twitter (pretty) | +33.9% / +39.4% | +4.5% / +3.6% | +6.2% / +6.9% | +17.2% / +24.2% | +4.6% / +12.2% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +88.9% / +123.8% | +77.7% / +93.4% | +68.8% / +82.8% | +62.5% / +99.4% | +77.2% / +72.0% |
| Apache Builds (pretty) | +80.0% / +99.1% | +64.5% / +78.6% | +56.8% / +66.3% | +56.7% / +76.2% | +104.3% / +80.9% |
| Canada (min) | tie / +11.2% | +6.4% / +5.8% | −5.9% / −4.6% | +5.3% / +21.4% | −8.3% / +5.5% |
| Canada (pretty) | tie / +4.7% | +3.2% / +4.2% | −6.5% / −6.4% | −7.5% / −8.1% | +11.2% / +2.3% |
| CitmCatalog (min) | +89.9% (n/c) / +90.7% | +64.2% / +73.9% | +45.2% / +47.8% | +55.0% / +61.4% | +56.1% / +84.3% |
| CitmCatalog (pretty) | +77.6% / +80.9% | +58.1% / +66.3% | +35.5% / +39.8% | +47.1% / +52.5% | +47.8% / +63.6% |
| Discord (min) | +301.8% / +430.6% (n/c) | +232.1% / +270.0% | +173.2% / +197.1% | +203.0% / +250.8% | +292.8% / +324.6% |
| Discord (pretty) | +314.7% / +371.5% (n/c) | +205.6% / +235.7% | +160.9% / +185.9% | +178.9% / +214.3% | +237.6% / +267.7% |
| Github Events (min) | +82.9% / +101.1% | +42.0% / +46.0% | +23.5% / +28.3% | +40.0% / +46.9% | +39.2% / +49.7% |
| Github Events (pretty) | +102.5% / +79.3% | +26.8% / +26.4% | +17.1% / +21.8% | +27.6% / +31.6% | +32.3% / +28.5% |
| Google Maps (min) | +49.4% (n/c) / +31.8% | +13.2% / +13.3% | +5.8% / +5.4% | +25.6% / +26.1% | −13.3% / +4.0% |
| Google Maps (pretty) | +36.1% / +45.6% (n/c) | +6.8% / +6.3% | −1.5% / −1.8% | +21.1% / +22.0% | +2.3% / −16.6% |
| Instruments (min) | +318.9% / +353.2% | +162.9% / +166.4% | +92.6% / +94.7% | +126.3% / +135.1% | +188.3% / +175.0% |
| Instruments (pretty) | +271.0% / +284.6% | +151.1% / +158.5% | +84.1% / +88.1% | +102.3% / +107.8% | +142.4% / +197.8% |
| Marine IK (min) | +46.3% / +46.9% | +49.9% / +55.4% | +34.5% / +38.5% | +43.4% / +47.9% | +53.6% / +68.0% (n/c) |
| Marine IK (pretty) | +48.7% (n/c) / +35.8% | +40.3% / +43.9% | +30.3% / +32.4% | +37.0% / +40.9% | +38.4% / +45.7% |
| Random (min) | +111.6% / +139.0% | +83.9% / +98.1% | +58.1% / +65.4% | +82.1% / +76.6% | +94.8% / +79.8% |
| Random (pretty) | +119.1% / +138.2% (n/c) | +75.0% / +82.4% | +47.3% / +50.4% | +68.6% / +89.7% | +98.4% / +113.5% |
| Twitter (min) | +324.6% / +350.9% | +140.2% / +147.0% | +105.8% / +108.8% | +119.4% / +139.0% | +189.6% / +187.0% |
| Twitter (pretty) | +284.1% / +327.1% (n/c) | +120.1% / +125.2% | +90.9% / +94.4% | +104.6% / +109.4% | +185.9% / +170.6% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +33.0% / +67.4% | +37.8% / +43.1% | +38.2% / +40.9% | +72.8% / +69.6% | +60.3% / +67.0% |
| Apache Builds (pretty) | +45.3% (n/c) / +48.0% | +23.5% / +29.4% | +22.2% / +28.1% | +34.3% / +49.6% | +4.3% / +14.0% |
| Canada (min) | +52.9% / +56.9% | +5.3% / +3.9% | +8.6% / +10.2% | +19.6% / +22.1% | +3.2% / +4.4% |
| Canada (pretty) | +90.8% / +91.2% | +8.0% / +7.8% | +10.0% / +14.4% | +13.2% / tie | −3.8% / +2.8% |
| CitmCatalog (min) | +41.4% / +48.1% | +8.7% / +9.1% | +12.4% / +11.2% | +18.7% / +19.2% | +8.5% / +5.1% |
| CitmCatalog (pretty) | +47.9% / +57.7% | +2.1% / +5.0% | +9.6% / +9.3% | +12.9% / +15.2% | +11.0% / +3.2% |
| Discord (min) | +27.1% / +38.7% | +13.5% / +14.9% | +7.5% / +10.5% | +24.9% / +28.8% | +10.6% / +14.2% |
| Discord (pretty) | +32.0% / +46.1% | +6.2% / +6.7% | +4.3% / +7.6% | +15.8% / +17.8% | tie / +1.9% |
| Github Events (min) | +49.5% / +60.8% | +11.0% / +12.4% | +9.3% / +9.7% | +29.5% / +32.9% | −5.3% / −5.0% |
| Github Events (pretty) | +54.3% / +63.3% | +37.9% / +9.8% | +7.9% / +10.0% | +17.7% / +20.2% | +9.3% / tie |
| Google Maps (min) | +42.1% / +46.4% | +13.7% / +11.4% | +14.7% / +13.6% | +23.8% / +23.2% | +9.1% / +40.3% |
| Google Maps (pretty) | +48.1% / +54.6% | +8.3% / +5.0% | +6.0% / +5.2% | +8.3% / +22.4% | +12.5% / +8.0% |
| Instruments (min) | +43.9% / +44.4% | +10.1% / +11.7% | +5.0% / +5.1% | +10.0% / tie | +16.2% / +14.5% |
| Instruments (pretty) | +46.6% / +50.4% | +3.6% / +4.9% | −1.2% / +0.6% | +5.1% / +5.7% | −6.3% / −4.5% |
| Marine IK (min) | +29.4% / +11.8% | −0.8% / −0.9% | +7.6% / +8.0% | −6.9% / +10.3% | −10.1% / +2.9% |
| Marine IK (pretty) | +69.7% / +63.7% | −5.8% / −6.0% | +9.9% / +10.2% | +11.7% / +12.0% | −1.8% / +5.6% |
| Random (min) | +23.6% / +37.1% | +22.9% / +21.9% | +15.3% / +15.0% | +29.1% / +35.4% | +37.2% / +6.3% |
| Random (pretty) | +22.2% / +38.1% | +11.0% / +15.3% | +10.1% / +9.6% | +22.5% / +26.2% | +32.6% / +6.4% |
| Twitter (min) | +28.2% / +31.5% (n/c) | +3.5% / +5.2% | +4.1% / +1.5% | +10.1% / −3.0% | +28.2% / +1.4% |
| Twitter (pretty) | +22.4% / +26.0% | −2.0% / −2.0% | +1.5% / +2.4% | +27.5% / −10.3% | −8.6% / tie |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +35.5% / +62.5% | +20.7% / +21.0% | +17.2% / +20.2% | +33.9% / +46.9% | +11.6% / +18.2% |
| Apache Builds (pretty) | +33.2% / +45.3% | +9.8% / +9.9% | +8.6% / +8.8% | +25.2% / +50.7% | +30.1% / tie |
| Canada (min) | +39.1% / +28.7% (n/c) | +6.5% / +2.8% | +4.4% / +5.5% | +15.2% / +16.0% | −3.5% / −2.9% |
| Canada (pretty) | +61.9% / +70.7% | −6.1% / −5.1% | +7.5% / +10.7% | +10.2% / +11.7% | tie / +7.3% |
| CitmCatalog (min) | +49.2% / +50.3% | +6.0% / +5.2% | +1.4% / +5.2% | +26.4% / +11.4% | +19.7% / +1.9% |
| CitmCatalog (pretty) | +48.8% / +71.6% (n/c) | tie / −1.4% | +1.2% / +2.0% | +9.2% / +9.7% | +0.7% / +3.4% |
| Discord (min) | +78.6% / +98.9% | +33.7% / +37.7% | +28.1% / +34.9% | +66.7% / +59.1% | +47.7% / +50.3% |
| Discord (pretty) | +66.9% / +96.3% | +28.4% / +32.4% | +23.0% / +26.8% | +34.8% / +41.5% | +14.7% / +25.6% |
| Github Events (min) | +71.7% / +87.3% | +24.8% / +24.1% | +17.9% / +20.5% | +33.4% / +36.8% | +23.7% / +18.4% |
| Github Events (pretty) | +65.3% / +78.1% | +13.0% / +15.3% | +14.8% / +16.6% | +27.0% / +28.6% | tie / +24.3% |
| Google Maps (min) | +45.9% / +43.1% | +2.1% / +2.8% | +1.1% / +2.2% | +17.8% / +17.6% | +6.1% / −10.6% |
| Google Maps (pretty) | +37.0% / +40.3% | −3.6% / −1.0% | −1.6% / −3.1% | +11.9% / −6.5% | +8.8% / tie |
| Instruments (min) | +73.7% / +75.6% | +30.2% / +29.3% | +20.7% / +18.0% | +23.6% / +26.7% | +33.1% / +24.1% |
| Instruments (pretty) | +77.2% (n/c) / +71.3% | +20.6% / +21.5% | +9.4% / +10.8% | +31.7% / +16.9% | +11.4% / tie |
| Marine IK (min) | +54.3% / +60.4% | +12.3% / +12.3% | +32.8% / +36.1% | +34.2% / +35.1% | +26.3% / +1.8% |
| Marine IK (pretty) | +59.5% / +70.3% | +18.3% / +17.8% | +26.6% / +28.0% | +26.5% / +27.3% | tie / +5.5% |
| Random (min) | +57.2% / +75.3% | +35.9% / +32.8% | +13.5% / +16.0% | +51.8% / +48.2% | +11.0% / +19.5% |
| Random (pretty) | +55.4% / +64.4% | +23.0% / +22.4% | +10.3% / +9.2% | +24.8% / +40.5% | +46.4% / +28.0% |
| Twitter (min) | +47.8% / +48.3% | +10.0% / +6.4% | +14.5% / +14.3% | +27.6% / +22.1% | +31.9% / tie |
| Twitter (pretty) | +46.7% / +51.0% | +2.3% / +4.3% | +11.0% / +9.7% | +19.7% / −10.0% | +4.6% / tie |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.6x to 4.8x faster at full size: +57.9% (Twitter pretty, macOS/GCC, fresh) up to +379.6% (Twitter min, MSVC, reused), and up to +349.8% outside MSVC (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +84.1% to +430.6%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −17.7% (prettified Google Maps on macOS/Clang, reused) to +25.5%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events rarely loses.** It accounts for 4 of the 116 single-document losses: prettified Github Events Small in order on Linux/GCC (−3.0% and −0.6%) and minified Github Events Small sparse on macOS/Clang (−5.3% and −5.0%). Read in reverse at full size it wins on every build (+81.1% to +278.3%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 192 of 200 full-size sparse-reverse results across both allocation modes (3 ties, 5 losses), and 179 of 200 small ones (9 ties, 12 losses). MSVC won all 80. The full-size losses are prettified Google Maps on Linux/Clang (−0.4% and −0.6%) and Linux/GCC (−1.2% and −0.9%), and minified CitmCatalog on macOS/Clang (reused, −14.4%). 11 of the 12 small losses are on Canada, CitmCatalog or Google Maps; the other is prettified Twitter Small on macOS/GCC (reused, −10.0%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 187.1 MB/s on MSVC against 285.8 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 664.2 and 930.4 MB/s. Every MSVC full-size sparse result is a win, by +17.4% to +93.3%, and every full-size sparse-reverse result by +19.2% to +114.1%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 180 of 200 full-size sparse results (6 ties, 14 losses) and 177 of 200 small ones (5 ties, 18 losses). MSVC and Linux/Clang won all 40 of their full-size results, and MSVC won all 40 small. The full-size losses are on macOS/Clang (5), Linux/GCC (5: minified Canada, fresh, −0.3%, and four Marine IK, −3.0% to −9.8%) and macOS/GCC (4: three Marine IK and prettified Instruments, reused). macOS/Clang holds 8 of the 18 small losses, Linux/Clang 6, macOS/GCC 3 and Linux/GCC 1.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 179, tied 7 and lost 14. Linux/Clang went 38-0-2, macOS/GCC 38-1-1, macOS/Clang 38-0-2, MSVC 33-4-3 and Linux/GCC 32-2-6 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 164, tied 7 and lost 29. MSVC went 37-2-1, macOS/GCC 36-2-2, Linux/Clang 35-0-5, macOS/Clang 29-3-8 and Linux/GCC 27-0-13. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −1.0% to −5.7%, led by CitmCatalog and Instruments, and Linux/GCC's from −0.4% to −11.2%, led by Canada and Marine IK. The largest is prettified CitmCatalog Small on macOS/Clang (reused, −18.1%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−4.3% to −8.7%) and prettified Marine IK in both runs (−2.5% and −3.1%). MSVC lost prettified Canada (fresh, −5.3%), minified Marine IK (fresh, −15.5%, n/c) and prettified Marine IK (reused, −7.2%), and macOS/GCC lost only minified Marine IK (fresh, −5.3%). Linux/Clang lost only prettified Instruments (−2.5% and −1.9%). macOS/Clang's two losses are both prettified and reused: CitmCatalog (−29.2%) and Canada (−4.3%, n/c). Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 45-0-5 on them. Bool, Double, Int64 and Uint64 won all 10 results each (+6.4% to +132.6%, +60.9% to +143.9%, +53.4% to +191.6% and +40.4% to +159.1%). String is the exception: 5 wins and 5 losses, on Linux/Clang in both runs (−3.7% and −5.7%), macOS/Clang in both runs (−14.7% and −21.4%) and Linux/GCC reused (−1.7%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 1.4% to 11.7% below its own NDJSON throughput on the same records, in all 18 build and stream pairs.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs from the 26 streaming tests that existed at the time are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`, before the Apache Builds streams were added.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 17 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 24 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 21 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 4-0-2 | 12-0-0 | 12-0-0 | 28-0-2 |
| Linux / GCC | 5-0-1 | 11-1-0 | 12-0-0 | 28-1-1 |
| macOS / GCC | 5-1-0 | 11-1-0 | 12-0-0 | 28-2-0 |
| macOS / Clang | 3-0-3 | 10-1-1 | 12-0-0 | 25-1-4 |
| **All** | **23-1-6** | **56-3-1** | **60-0-0** | **139-4-7** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +41.5% | +17.7% | +18.4% | +12.9% | +20.6% |
| Large Amazon Cellphones (NDJSON) | +14.1% | +9.7% | +3.5% | tie (n/c) | +9.5% |
| Stream Formats Small (NDJSON) | +70.0% | −5.6% | −0.6% | +13.5% | −12.2% |
| Stream Formats Small (comma) | +117.9% (n/c) | +81.3% | +102.8% | +94.3% | +43.8% |
| Stream Formats Large (NDJSON) | +115.7% | −13.7% | +21.3% | +24.3% | −10.9% |
| Stream Formats Large (comma) | +108.9% | +1.2% | +31.2% | +28.0% | −12.0% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +35.0% | +24.2% | +15.4% | +40.4% | +43.4% |
| Apache Builds (comma) | +37.8% (n/c) | +38.1% | +22.1% | +48.0% | +43.8% |
| CitmCatalog (NDJSON) | +26.2% (n/c) | +6.0% | +9.3% | +3.4% (n/c) | −5.0% |
| CitmCatalog (comma) | +35.5% | +28.9% | +32.8% | +27.7% | +43.0% |
| Google Maps (NDJSON) | +48.2% | +27.1% | +30.8% | +23.4% | +22.0% (n/c) |
| Google Maps (comma) | +49.3% | +43.5% | +46.5% | +39.4% | +22.0% (n/c) |
| Instruments (NDJSON) | +79.1% (n/c) | +11.4% | +15.0% | tie (n/c) | tie |
| Instruments (comma) | +108.2% | +36.4% | +59.4% | +54.2% | +22.5% |
| Random (NDJSON) | +44.5% | +22.5% | +27.8% | +42.9% | +45.8% (n/c) |
| Random (comma) | +46.1% | +34.5% | +48.7% | +48.2% | +50.5% |
| Twitter (NDJSON) | +32.6% (n/c) | +9.3% | tie | +27.3% | +12.9% |
| Twitter (comma) | +34.9% (n/c) | +19.0% | +17.2% | +37.7% | +18.2% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +44.7% (n/c) | +21.9% | +4.4% | +19.5% | +25.1% |
| Apache Builds (comma) | +43.6% (n/c) | +38.5% | +21.3% | +37.5% | +34.9% (n/c) |
| CitmCatalog (NDJSON) | +54.2% | +28.8% | +33.8% | +25.8% | +46.3% |
| CitmCatalog (comma) | +68.0% | +50.8% | +58.7% | +51.3% | +65.7% (n/c) |
| Google Maps (NDJSON) | +35.4% (n/c) | +10.6% | +11.6% | +13.5% | +8.3% (n/c) |
| Google Maps (comma) | +47.6% | +21.9% | +32.4% | +29.2% | +21.6% (n/c) |
| Instruments (NDJSON) | +117.8% | +38.6% | +20.8% | +21.6% | +33.1% |
| Instruments (comma) | +120.2% (n/c) | +63.2% | +49.6% | +80.0% (n/c) | +64.6% |
| Random (NDJSON) | +113.3% (n/c) | +76.9% | +50.1% | +78.0% (n/c) | +83.4% |
| Random (comma) | +141.7% | +91.4% | +71.1% | +94.2% | +82.4% (n/c) |
| Twitter (NDJSON) | +366.3% (n/c) | +158.7% | +155.8% | +103.2% (n/c) | +130.7% (n/c) |
| Twitter (comma) | +393.5% (n/c) | +172.0% | +177.2% | +118.6% | +188.1% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 23 of the 30 results in that group, tied 1 and lost 6. It won Stream Formats Small (comma) (+43.8% to +117.9%) and Amazon Cellphones (+12.9% to +41.5%) on all five builds, and Large Amazon Cellphones (+3.5% to +14.1%) on four, tying on macOS/GCC. All six losses are Stream Formats reads: Small as NDJSON on Linux/Clang (−5.6%), Linux/GCC (−0.6%) and macOS/Clang (−12.2%), Large as NDJSON on Linux/Clang (−13.7%) and macOS/Clang (−10.9%), and Large comma-separated on macOS/Clang (−12.0%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 30-0-0. macOS/GCC lost none (28-2-0), Linux/GCC lost one (28-1-1), Linux/Clang two (28-0-2), and macOS/Clang went 25-1-4. Every streaming loss but one is a Stream Formats read; the exception is CitmCatalog (NDJSON) on macOS/Clang.
- **In-order corpus streams lose once.** Of the 60 in-order corpus results, Jsonifier won 56, tied 3 and lost 1. The ties are Twitter (NDJSON) on Linux/GCC and Instruments (NDJSON) on macOS/GCC and macOS/Clang, and the loss is CitmCatalog (NDJSON) on macOS/Clang (−5.0%).
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +103.2% to +393.5%, while Google Maps rows, which have few, give +8.3% to +47.6%. Instruments, whose reverse streams have the most room to lose, wins on every build (+20.8% to +120.2%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 257.4 MB/s under MSVC and 471.7 MB/s under Linux/Clang, while Jsonifier reads it at 562.4 and 877.9 MB/s.

## 9. Limitations

- **Linux/GCC and macOS/Clang hold the most single-document losses:** 39 and 37 of the 116, against 22 on Linux/Clang, 14 on macOS/GCC and 4 on MSVC. 22 of Linux/GCC's are small documents, led by 13 small in-order, and 22 of macOS/Clang's, split evenly between small in-order and small sparse. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's prettified CitmCatalog read in order (reused, −29.2%), its String POD test (reused, −21.4%) and its prettified CitmCatalog Small read in order (reused, −18.1%); the largest x86 loss is MSVC's minified Marine IK read in order (fresh, −15.5%, n/c).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 63 of the 116: Canada 30 (15 on Linux/GCC, 8 on macOS/Clang), Google Maps 19 (7 each on Linux/GCC and macOS/Clang) and CitmCatalog 14 (7 on macOS/Clang, 4 on Linux/Clang). Marine IK, the other float-dense document, adds 26. Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 15 of its 39 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 70 of the 116 losses are small documents (22 each on Linux/GCC and macOS/Clang, 16 on Linux/Clang, 9 on macOS/GCC, 1 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats reads hold six of the seven streaming losses:** Small as NDJSON on Linux/Clang (−5.6%), Linux/GCC (−0.6%) and macOS/Clang (−12.2%), and Large on Linux/Clang (NDJSON, −13.7%) and macOS/Clang (NDJSON, −10.9%, and comma-separated, −12.0%). The only corpus record stream to lose is CitmCatalog (NDJSON) on macOS/Clang (−5.0%).
- **Some results did not converge.** 51 of the 1,650 single-document results did not converge: 40 on Windows/MSVC, 6 on macOS/Clang and 5 on macOS/GCC. 28 of the 150 streaming results did not converge: 13 on Windows/MSVC, 9 on macOS/Clang and 6 on macOS/GCC. Both Linux builds converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,490, tied 44 and lost 116. On reverse reads it won 374 of 400 at both sizes, and on Windows/MSVC it lost 4 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (742-23-60 fresh, 748-21-56 reused). The main remaining targets are the small-object documents (Canada, Google Maps and CitmCatalog, with 63 of the 116 losses), small documents in general, and the Linux/GCC and macOS/Clang builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 139, tied 4 and lost 7, and on the 120 corpus record-stream results it went 116-3-1. Stream Formats reads, which hold six of the seven losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.