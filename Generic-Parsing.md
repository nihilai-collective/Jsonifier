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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,519 of 1,650 single-document results against simdjson On Demand, tied 36 and lost 95. On reverse-order reads it won 385 of 400. On Windows/MSVC it won 319 of 330 single-document results, tied 4 and lost 7.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 144 of 150 results, tied 4 and lost 2, and it won 118 of the 120 corpus record-stream results and tied 2.

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
- **Correctness:** on the measured commit, the stage-1 structural tape Jsonifier builds for each of the 25 full-size single-document inputs, including Apache Builds and Github Events, was compared position by position with simdjson's, using a simdjson branch (nihilai-collective/simdjson `7002cd3`) that exposes its tape. All 2,634,960 positions match on each of the 5 builds (`correctness-validation-main`, Jsonifier `3e5ab01`, November 7, 2026).
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 8 of the 1,650 single-document results and 23 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 169 / 170 | 160 / 160 | 329 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 167 / 170 | 159 / 160 | 326 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 168 / 170 | 159 / 160 | 327 / 330 |

Every build ran the same commits of every library: BenchmarkSuite `ed43ce8`, Jsonifier `3e5ab01` and simdjson `dfe02cfe3`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. Every build ran on November 7, 2026, and the results were published to `generic-parsing-main` the same day.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 19-1-0 | 20-0-0 | 20-0-0 | 19-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 160-2-3 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 17-0-3 | 20-0-0 | 18-0-2 | 19-0-1 | 158-0-7 |
| Linux / GCC | 5-0-0 | 17-0-3 | 18-1-1 | 17-0-3 | 19-0-1 | 14-1-5 | 18-0-2 | 18-2-0 | 20-0-0 | 146-4-15 |
| macOS / GCC | 4-0-1 | 18-0-2 | 19-1-0 | 19-0-1 | 19-1-0 | 19-0-1 | 19-1-0 | 16-2-2 | 17-3-0 | 150-8-7 |
| macOS / Clang | 3-0-2 | 17-1-2 | 20-0-0 | 19-0-1 | 20-0-0 | 17-1-2 | 20-0-0 | 15-1-4 | 16-1-3 | 147-4-14 |
| **All** | **21-0-4** | **89-2-9** | **96-3-1** | **95-0-5** | **98-1-1** | **86-2-12** | **97-1-2** | **87-5-8** | **92-4-4** | **761-18-46** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 17-1-2 | 19-1-0 | 20-0-0 | 20-0-0 | 19-0-1 | 19-0-1 | 20-0-0 | 20-0-0 | 159-2-4 |
| Linux / Clang | 4-0-1 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 18-0-2 | 20-0-0 | 17-1-2 | 20-0-0 | 159-1-5 |
| Linux / GCC | 5-0-0 | 16-1-3 | 18-0-2 | 17-0-3 | 18-1-1 | 15-2-3 | 18-0-2 | 20-0-0 | 20-0-0 | 147-4-14 |
| macOS / GCC | 5-0-0 | 15-2-3 | 20-0-0 | 19-0-1 | 18-1-1 | 19-0-1 | 19-1-0 | 18-0-2 | 16-2-2 | 149-6-10 |
| macOS / Clang | 4-0-1 | 18-0-2 | 20-0-0 | 19-0-1 | 20-0-0 | 15-1-4 | 19-1-0 | 12-2-6 | 17-1-2 | 144-5-16 |
| **All** | **23-0-2** | **86-4-10** | **97-1-2** | **95-0-5** | **96-2-2** | **86-3-11** | **95-2-3** | **87-3-10** | **93-3-4** | **758-18-49** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (8 of them did not converge; see §9), Jsonifier won 1,519, tied 36 and lost 95: 761-18-46 with fresh output objects and 758-18-49 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 193 of 200 full-size reverse results (4 ties, 3 losses) and 192 of 200 small reverse results (3 ties, 5 losses). Linux/Clang won all 80, and macOS/Clang lost none. Every loss is on Canada. The 3 full-size losses are all on Linux/GCC: minified Canada (−7.4% fresh, −8.4% reused) and prettified Canada (reused, −1.2%). Of the 5 small losses, 4 are Canada Small on Linux/GCC (both forms and both runs, −5.0% to −10.0%) and 1 is minified Canada Small on MSVC (reused, −1.6%).
- **Windows/MSVC lost seven of its 330 single-document results,** all on Canada or Marine IK: minified Canada read in order (−1.9% fresh, −2.8% reused), minified Marine IK read in order (−5.4% and −6.4%), minified Marine IK Small read in order (−3.0% and −1.3%) and minified Canada Small read in reverse (reused, −1.6%). It won all 10 POD results and all 160 of its sparse and sparse-reverse results.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 7.7–12.9% for Jsonifier and 6.2–9.5% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +1.4 to +3.9 percentage points on the non-MSVC builds and by +5.0 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +202.0% to +236.6%, and Apache Builds (pretty) from +16.2% to +26.1%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +262.5% / +282.5% | +88.1% / +93.2% | +139.3% / +146.0% | +43.6% / +46.7% | −4.7% / +4.6% |
| Double | +182.4% / +206.7% | +84.4% / +90.3% | +93.1% / +93.7% | +98.3% / +94.7% | +59.2% / +72.0% |
| Int64 | +163.3% / +175.0% | +89.5% / +95.9% | +88.5% / +94.4% | +61.9% / +62.6% | +50.7% / +55.9% |
| String | +48.8% / +57.4% | −6.5% / −10.8% | +3.9% / +4.8% | −9.1% / +5.5% | −18.2% / −26.4% |
| Uint64 | +163.5% / +166.8% | +114.1% / +117.9% | +92.2% / +96.8% | +51.0% (n/c) / +74.7% | +42.0% / +43.6% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +8.9% / +25.5% | +21.4% / +27.5% | +21.1% / +34.9% | +40.8% / +62.9% | +25.0% / +32.6% |
| Apache Builds (pretty) | +16.2% / +26.1% | +20.0% / +26.9% | +22.0% / +33.0% | +52.2% / +60.4% | +6.3% / +30.2% |
| Canada (min) | −1.9% / −2.8% | +6.0% / +5.6% | −11.5% / −12.9% | +13.2% / +26.3% | −6.8% / +6.3% |
| Canada (pretty) | +3.2% / +2.5% | +4.4% / +4.5% | −6.1% / −4.1% | +6.3% / tie | +4.6% / −6.2% |
| CitmCatalog (min) | +14.0% / +13.8% | +9.6% / +10.8% | +10.8% / +12.7% | +24.3% / +15.4% | +10.7% / +11.7% |
| CitmCatalog (pretty) | +16.1% / +17.4% | +5.7% / +6.5% | +9.8% / +9.5% | +5.6% / −6.0% | +9.5% / +10.5% |
| Discord (min) | +16.8% / +24.8% | +20.0% / +26.6% | +11.2% / +13.0% | +26.1% / +22.2% | +20.1% / +22.3% |
| Discord (pretty) | +18.2% / +24.6% | +10.1% / +21.0% | +13.7% / +20.5% | +26.8% / +50.1% | +13.6% / +16.1% |
| Github Events (min) | +30.6% / +44.4% | +34.8% / +40.0% | +28.7% / +34.0% | +45.3% / +54.5% | +45.4% / +56.1% |
| Github Events (pretty) | +31.3% / +41.2% | +29.6% / +33.9% | +24.5% / +28.7% | +35.5% / +42.9% | +38.1% / +42.3% |
| Google Maps (min) | +24.0% / +24.5% | +23.3% / +24.3% | +26.3% / +29.7% | +31.4% / +59.7% | +15.8% / +14.4% |
| Google Maps (pretty) | +23.4% / +25.6% | +13.0% / +13.0% | +21.8% / +23.0% | +11.6% / +6.9% | +10.2% / +11.4% |
| Instruments (min) | +37.5% / +41.6% | +8.4% / +9.1% | +5.8% / +5.9% | +0.7% / tie | +7.1% / +8.0% |
| Instruments (pretty) | +35.0% / +41.9% | +1.7% / +2.5% | +3.4% / +1.9% | +9.1% / +12.0% | −1.9% / −2.5% |
| Marine IK (min) | −5.4% / −6.4% | +7.5% / +7.2% | −5.3% / −6.9% | −7.2% (n/c) / −0.4% | +5.2% / +5.6% |
| Marine IK (pretty) | tie / tie | +5.5% / +8.5% | +2.5% / tie | −1.3% / −2.5% | +6.1% / +6.6% |
| Random (min) | +26.9% / +33.0% | +21.7% / +25.6% | +32.6% / +38.6% | +56.2% / +71.4% | +34.2% / +38.9% |
| Random (pretty) | +26.3% / +33.1% | +23.5% / +29.2% | +30.2% / +39.6% | +51.4% / +64.6% | +30.1% / +33.5% |
| Twitter (min) | +24.4% / +30.4% | +11.0% / +20.0% | +8.9% / +22.7% | +43.7% / +53.6% | tie / +10.0% |
| Twitter (pretty) | +22.0% / +32.7% | +6.4% / +14.4% | +4.8% / +7.7% | +12.2% / +6.5% | +1.9% / +5.8% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +39.1% / +65.2% | +57.5% / +82.5% | +61.0% / +74.8% | +101.4% / +96.5% | +29.5% / +88.0% |
| Apache Builds (pretty) | +43.9% / +61.5% | +51.6% / +67.2% | +57.2% / +74.0% | +67.0% / +90.3% | +24.5% / +77.3% |
| Canada (min) | tie / tie | +7.5% / +8.2% | −7.4% / −8.4% | +6.2% / +19.9% | +7.8% / +9.7% |
| Canada (pretty) | +10.4% / +10.4% (n/c) | +9.0% / +8.5% | tie / −1.2% | tie / +6.3% | +7.1% / +8.6% |
| CitmCatalog (min) | +61.8% / +61.7% | +67.3% / +72.0% | +60.7% / +65.1% | +73.2% / +76.8% | +73.9% / +77.4% |
| CitmCatalog (pretty) | +59.2% / +60.1% | +57.5% / +61.3% | +54.8% / +59.1% | +96.8% / +65.2% | +68.3% / +72.5% |
| Discord (min) | +202.0% / +236.6% | +274.9% / +298.0% | +235.8% / +260.9% | +339.8% / +308.5% | +321.1% / +343.3% |
| Discord (pretty) | +166.2% / +192.3% | +234.2% / +255.2% | +212.2% / +232.5% | +187.7% / +214.1% | +277.3% / +291.5% |
| Github Events (min) | +142.0% / +184.6% | +147.1% / +170.3% | +118.0% / +136.6% | +106.6% / +127.9% | +165.0% / +195.0% |
| Github Events (pretty) | +130.3% / +167.3% | +122.4% / +142.6% | +98.7% / +113.4% | +85.8% / +97.5% | +123.8% / +159.2% |
| Google Maps (min) | +26.0% / +24.0% | +14.1% / +13.9% | +19.0% / +20.5% | +26.9% / +27.1% | +18.1% / +19.5% |
| Google Maps (pretty) | +23.9% / +25.5% | +8.8% / +9.6% | +14.0% / +14.9% | +22.2% / +23.5% | +12.0% / +11.7% |
| Instruments (min) | +189.7% / +202.9% | +188.8% / +195.6% | +153.7% / +161.9% | +149.1% / +154.9% | +188.0% / +218.9% |
| Instruments (pretty) | +162.7% / +174.9% | +161.7% / +168.1% | +136.7% / +138.3% | +121.9% / +125.2% | +166.8% / +191.7% |
| Marine IK (min) | +24.3% / +26.6% | +51.0% / +54.5% | +40.5% / +44.0% | +51.6% (n/c) / +54.7% | +52.3% / +55.6% (n/c) |
| Marine IK (pretty) | +33.5% / +32.1% | +54.2% / +47.8% | +45.3% / +45.5% | +50.5% / +54.0% | +55.2% / +54.4% |
| Random (min) | +70.3% / +82.3% | +97.4% / +111.2% | +93.2% / +105.4% | +93.1% / +110.3% | +115.7% / +127.6% |
| Random (pretty) | +63.2% / +73.9% | +86.7% / +97.9% | +77.7% / +88.3% | +104.3% / +106.9% | +126.8% / +124.4% |
| Twitter (min) | +223.5% / +256.9% | +173.0% / +181.0% | +133.5% / +143.0% | +129.5% / +171.9% | +208.5% / +214.6% |
| Twitter (pretty) | +189.7% / +213.7% | +138.2% / +147.1% | +102.8% / +109.2% | +93.3% / +125.5% | +212.1% / +227.9% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +24.6% / +40.1% | +38.9% / +58.2% | +36.8% / +50.8% | +34.0% / +71.5% | +34.2% / +40.6% |
| Apache Builds (pretty) | +24.8% / +34.1% | +25.1% / +36.2% | +26.8% / +39.9% | +26.6% / +44.5% | +23.7% / +28.2% |
| Canada (min) | +59.7% / +58.7% | +14.3% / +11.3% | −3.1% / −5.2% | +18.3% / +12.2% | +34.6% (n/c) / +62.3% |
| Canada (pretty) | +84.0% / +81.0% | +12.0% / +8.8% | +17.0% / +15.8% | +16.4% / +16.7% | +23.5% / +31.3% |
| CitmCatalog (min) | +29.6% / +31.0% | +5.9% / +5.9% | +6.5% / +6.5% | +7.6% / +14.1% | +6.2% / +6.5% |
| CitmCatalog (pretty) | +47.3% / +47.5% | +2.5% / +1.3% | +6.3% / +6.3% | +16.2% / +16.0% | +8.2% / +8.8% |
| Discord (min) | +17.6% / +22.0% | +8.0% / +7.5% | +24.2% / +28.2% | +25.8% / +32.8% | +19.0% / +19.0% |
| Discord (pretty) | +19.1% / +26.3% | +5.7% / +4.2% | +20.4% / +24.4% | +22.8% / +27.6% | +8.6% / +9.7% |
| Github Events (min) | +52.5% / +61.5% | +17.3% / +22.3% | +22.4% / +21.3% | +35.0% / +39.4% | +18.5% / +18.7% |
| Github Events (pretty) | +49.3% / +56.8% | +13.9% / +15.9% | +13.6% / +15.5% | +19.2% / +22.7% | +6.7% / +6.8% |
| Google Maps (min) | +25.2% / +29.6% | +16.0% / +16.6% | +19.1% / +20.8% | +21.3% / +21.4% | +15.3% / +14.5% |
| Google Maps (pretty) | +29.3% / +32.8% | +9.8% / +10.7% | +13.7% / +14.7% | +21.1% / +21.2% | +9.1% / +11.8% |
| Instruments (min) | +33.7% / +40.5% | +12.4% / +9.4% | +9.1% / +12.1% | +10.0% / +20.2% | +11.0% / +10.7% |
| Instruments (pretty) | +32.8% / +40.6% | +9.1% / +8.3% | +7.9% / +6.9% | +9.2% / +16.9% | −2.1% / −0.7% |
| Marine IK (min) | +29.7% / +28.8% | +12.4% / +8.3% | −7.4% / −9.1% | −1.5% / −1.4% | +19.8% / +20.2% |
| Marine IK (pretty) | +59.6% / +61.6% | +7.0% / +4.7% | −8.4% / −8.3% | +8.2% / +8.2% | +17.5% / +23.4% |
| Random (min) | +31.9% / +33.5% | +24.6% / +29.9% | +23.5% / +25.1% | +30.6% / +34.5% | +21.3% / +61.2% |
| Random (pretty) | +28.4% / +31.6% | +16.8% / +20.6% | +11.3% / +14.8% | +35.9% / +40.5% | +24.5% / +27.8% |
| Twitter (min) | +56.7% / +61.1% | +18.2% / +19.3% | +16.1% / +16.4% | +27.0% / +15.6% | +20.7% / +14.1% |
| Twitter (pretty) | +52.9% / +55.9% | +5.9% / +6.0% | +15.7% / +16.6% | +5.7% / +26.0% | +17.8% / +8.1% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +19.2% / +27.3% | +16.3% / +18.7% | +18.7% / +22.3% | +41.2% / +53.4% | +25.7% / +29.1% |
| Apache Builds (pretty) | +19.1% / +24.6% | +7.5% / +8.1% | +14.7% / +17.8% | +31.4% / +39.0% | +18.4% / +20.3% |
| Canada (min) | +40.6% / +43.3% | +9.5% / +8.0% | −2.6% / −3.4% | tie / +5.3% | +25.2% / +35.7% |
| Canada (pretty) | +61.9% / +62.3% | +9.7% / +10.4% | +14.3% / +13.3% | +7.4% / −3.0% | +24.7% / +25.4% |
| CitmCatalog (min) | +19.6% / +18.4% | +8.1% / +10.0% | +6.2% / +3.3% | +17.6% / +12.1% | +5.5% / +5.9% |
| CitmCatalog (pretty) | +33.7% / +33.6% | +5.7% / +7.9% | +3.5% / +2.8% | +20.8% / +18.1% | +7.0% / +6.9% |
| Discord (min) | +41.4% / +49.4% | +47.7% / +55.4% | +45.1% / +55.7% | +45.1% / +52.9% | +70.6% / +76.5% |
| Discord (pretty) | +39.4% / +48.0% | +33.9% / +44.7% | +36.6% / +43.1% | +37.3% / +42.7% | +52.5% / +57.9% |
| Github Events (min) | +65.0% / +76.9% | +44.6% / +49.1% | +44.3% / +46.6% | +46.7% / +50.9% | +47.2% / +48.6% |
| Github Events (pretty) | +59.6% / +67.2% | +36.9% / +36.7% | +39.7% / +41.2% | +32.9% / +35.7% | +33.6% / +33.8% |
| Google Maps (min) | +25.9% / +29.4% | +8.1% / +8.9% | +6.3% / +6.0% | +9.8% / +9.6% | +6.0% / +9.8% |
| Google Maps (pretty) | +24.9% / +27.9% | +1.7% / +1.9% | +8.0% / +8.2% | +11.4% / +11.8% | +7.2% / +6.4% |
| Instruments (min) | +43.0% / +47.8% | +28.7% / +27.6% | +22.7% / +22.9% | +26.4% / +26.5% | +36.9% / +37.7% |
| Instruments (pretty) | +43.6% / +47.8% | +15.9% / +15.7% | +22.1% / +23.0% | +19.6% / +21.5% | +23.6% / +24.3% |
| Marine IK (min) | +46.5% / +48.0% | +38.0% / +39.8% | +22.5% / +21.8% | +25.7% / +29.7% | +52.3% / +40.0% |
| Marine IK (pretty) | +69.1% / +68.5% | +34.3% / +27.3% | +11.1% / tie | +27.9% / +27.1% | +33.1% / +32.5% |
| Random (min) | +40.1% / +44.2% | +31.1% / +32.3% | +27.1% / +28.5% | +53.8% / +58.7% | +85.9% / +78.5% |
| Random (pretty) | +37.4% / +41.3% | +18.1% / +18.5% | +18.1% / +23.3% | +21.9% / +30.3% | +20.9% / +36.5% |
| Twitter (min) | +61.8% / +64.2% | +32.4% / +30.1% | +25.0% / +25.0% | +29.8% / +17.4% | +29.4% / +31.2% |
| Twitter (pretty) | +52.2% / +54.7% | +21.4% / +20.8% | +24.1% / +25.0% | +13.7% / tie | +22.8% / +23.7% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +12.0% / +18.4% | +17.7% / +22.1% | +24.5% / +31.6% | +16.2% / +50.8% | +33.0% / +36.3% |
| Apache Builds (pretty) | +11.9% / +20.6% | +16.8% / +19.1% | +23.0% / +33.3% | +21.3% / +38.7% | +28.3% / +32.1% |
| Canada (min) | +1.5% / +0.4% | +1.5% / +2.3% | −8.7% / −12.4% | +10.5% / −5.8% | +6.4% / −8.5% |
| Canada (pretty) | +3.4% / +3.7% | +4.1% / +1.2% | −10.0% / −11.0% | +1.3% / +5.2% | +4.4% / −1.5% |
| CitmCatalog (min) | +6.0% / +15.0% | −1.4% / +1.9% | −1.9% / +0.5% | +12.8% / +7.3% | +18.3% / +19.8% |
| CitmCatalog (pretty) | +11.6% / +17.4% | −4.7% / −2.0% | tie / tie | −14.3% / +10.3% | +10.7% / +1.7% |
| Discord (min) | +8.0% / +15.5% | +7.5% / +14.9% | +5.4% / +15.8% | +16.4% / +41.5% | +10.6% / +30.8% |
| Discord (pretty) | +8.7% / +16.8% | +6.2% / +13.1% | +6.2% / +13.5% | +25.2% / +23.1% | +5.0% / +10.5% |
| Github Events (min) | +22.9% / +30.5% | +15.9% / +15.2% | +10.6% / +9.4% | +7.0% / +29.7% | +18.1% / +18.7% |
| Github Events (pretty) | +17.9% / +20.3% | +6.5% / +9.8% | +8.9% / +5.8% | +39.8% / +24.5% | +10.1% / +32.6% |
| Google Maps (min) | +22.8% / +23.9% | +21.8% / +21.2% | +24.9% / +26.6% | +19.2% / +40.7% | +17.6% / +10.3% |
| Google Maps (pretty) | +26.1% / +26.1% | +10.3% / +11.7% | +19.0% / +21.8% | +27.7% / +34.1% | −3.3% / +8.6% |
| Instruments (min) | +36.6% / +42.9% | +1.2% / +1.9% | +2.5% / +1.3% | +11.1% / +10.9% | +25.6% / +2.9% |
| Instruments (pretty) | +36.6% / +37.0% | −3.7% / −2.6% | −1.3% / tie | +2.2% / +13.7% | −15.5% / −6.7% |
| Marine IK (min) | −3.0% / −1.3% | +3.3% / +3.8% | −2.3% / −0.9% | +29.3% / +4.7% | +10.9% / tie |
| Marine IK (pretty) | +3.0% / +5.3% | +4.5% / +2.2% | +3.8% / +1.0% | +8.1% / +28.0% | tie / −4.6% |
| Random (min) | +25.2% / +30.3% | +16.4% / +17.5% | +23.1% / +31.0% | +56.3% / +64.9% | +17.0% / +54.4% |
| Random (pretty) | +25.1% / +28.5% | +16.6% / +21.5% | +20.1% / +25.4% | +25.0% / +40.3% | +24.1% / +19.4% |
| Twitter (min) | +19.6% / +24.7% | +3.1% / +5.7% | +9.0% / +10.3% | +32.8% / +30.7% | +6.2% / +7.6% |
| Twitter (pretty) | +20.3% / +26.8% | +3.3% / +6.8% | +10.4% / +11.3% | +19.2% / +21.3% | +3.5% / +4.4% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +56.8% / +87.2% | +76.9% / +84.1% | +81.7% / +94.0% | +82.0% / +113.3% | +36.6% / +109.3% |
| Apache Builds (pretty) | +46.3% / +71.5% | +62.2% / +75.5% | +72.8% / +80.4% | +79.6% / +73.3% | +39.4% / +79.7% |
| Canada (min) | +1.7% / −1.6% | +6.3% / +6.2% | −6.7% / −10.0% | tie / tie | +8.3% / tie |
| Canada (pretty) | +6.2% / +5.0% | +4.8% / +5.1% | −5.0% / −6.3% | +4.7% / +4.8% | +9.1% / +16.2% |
| CitmCatalog (min) | +56.3% / +57.0% | +58.7% / +67.9% | +59.6% / +66.9% | +80.5% / +78.6% | +58.5% / +103.9% |
| CitmCatalog (pretty) | +54.6% / +49.4% | +48.5% / +55.6% | +54.7% / +60.9% | +57.1% / +62.8% | +81.1% / +66.4% |
| Discord (min) | +196.3% / +255.0% | +241.3% / +281.3% | +211.2% / +239.2% | +306.1% / +312.4% | +306.3% / +398.6% |
| Discord (pretty) | +185.1% / +225.5% | +213.5% / +249.2% | +195.1% / +222.1% | +236.7% / +246.6% | +272.1% / +290.2% |
| Github Events (min) | +44.7% / +56.3% | +48.2% / +54.1% | +33.4% / +38.8% | +45.4% / +70.2% | +64.7% / +62.7% |
| Github Events (pretty) | +37.9% / +44.1% | +34.0% / +34.7% | +22.3% / +30.3% | +25.2% / +58.2% | +58.4% / +75.2% |
| Google Maps (min) | +22.4% / +26.8% | +14.6% / +14.3% | +15.9% / +15.5% | +26.9% / +26.5% | +5.9% / +18.9% |
| Google Maps (pretty) | +21.9% / +27.3% | +6.1% / +6.4% | +10.9% / +11.8% | +22.1% / +10.3% | +18.5% / +13.6% |
| Instruments (min) | +163.1% / +175.3% | +175.7% / +189.4% | +148.7% / +154.8% | +164.5% / +164.3% | +220.8% / +270.8% |
| Instruments (pretty) | +142.9% / +152.5% | +165.2% / +163.3% | +133.7% / +135.5% | +100.3% / +123.5% | +174.7% / +169.4% |
| Marine IK (min) | +28.3% / +30.6% | +45.3% / +46.8% | +37.3% / +41.4% | +24.7% / +62.3% | +50.6% / +55.3% |
| Marine IK (pretty) | +28.3% / +30.9% | +37.7% / +44.6% | +35.7% / +39.4% | +17.1% / +58.1% | +44.4% / +49.1% |
| Random (min) | +66.3% / +84.4% | +88.8% / +101.2% | +85.5% / +92.8% | +78.0% / +97.5% | +84.4% / +176.8% |
| Random (pretty) | +56.9% / +69.6% | +74.1% / +82.0% | +71.9% / +79.0% | +110.5% / +70.0% | +105.3% / +85.4% |
| Twitter (min) | +166.2% / +185.1% | +173.9% / +176.5% | +132.5% / +141.4% | +140.9% / +119.7% | +224.7% / +241.4% |
| Twitter (pretty) | +145.1% / +164.3% | +138.8% / +151.4% | +120.6% / +118.9% | +94.4% / +139.8% | +209.5% / +222.0% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +28.0% / +41.9% | +31.7% / +34.2% | +32.5% / +40.3% | +42.1% / +33.4% | +23.3% / +33.8% |
| Apache Builds (pretty) | +27.4% / +40.4% | +24.5% / +27.8% | +29.0% / +31.3% | +29.3% / +72.7% | +24.2% / +36.1% |
| Canada (min) | +65.9% / +69.0% | +8.0% / +10.2% | +22.8% / +22.7% | +5.1% (n/c) / −9.1% | +6.2% / +21.0% |
| Canada (pretty) | +88.5% / +92.1% | +14.3% / +13.2% | +19.3% / +20.7% | +7.4% / +3.4% | −5.2% / +7.1% |
| CitmCatalog (min) | +28.0% / +33.6% | +7.5% / +6.0% | +7.6% / +8.8% | +7.4% / +22.7% | +24.2% / +32.9% |
| CitmCatalog (pretty) | +40.6% / +43.6% | −2.1% / tie | +8.9% / +7.7% | +30.1% / +15.8% | +23.8% / −8.5% |
| Discord (min) | +23.2% / +30.7% | +11.6% / +11.6% | +13.1% / +16.7% | +45.9% / +36.8% | +2.2% / −14.1% |
| Discord (pretty) | +24.5% / +33.4% | +5.2% / +7.0% | +12.1% / +13.4% | +14.5% / +10.2% | +20.6% / −4.6% |
| Github Events (min) | +35.3% / +45.1% | +11.0% / +11.5% | +14.8% / +16.6% | tie / +20.3% | tie / +6.8% |
| Github Events (pretty) | +35.1% / +43.8% | +9.1% / +10.0% | +11.5% / +12.9% | +19.0% / +21.1% | −7.3% / −4.3% |
| Google Maps (min) | +29.9% / +30.2% | +12.0% / +13.0% | +17.1% / +22.3% | +11.9% / +47.0% | +19.9% / +31.9% |
| Google Maps (pretty) | +29.1% / +29.3% | +7.5% / +6.7% | +9.4% / +9.0% | +49.8% / +7.6% | +14.6% / +17.7% |
| Instruments (min) | +24.6% / +34.6% | +5.1% / +6.3% | tie / +4.0% | tie / +11.4% | +11.0% / −18.0% |
| Instruments (pretty) | +31.8% / +31.6% | +4.0% / +1.5% | tie / +0.8% | −5.6% / −9.3% | +35.9% / tie |
| Marine IK (min) | +35.2% / +40.0% | −10.5% / −11.2% | +1.8% / +4.1% | +4.1% / +14.2% | +5.9% / +5.0% |
| Marine IK (pretty) | +59.6% / +61.2% | +4.2% / +4.8% | +8.4% / +8.5% | +14.0% / +10.5% | −27.4% / −15.3% |
| Random (min) | +23.3% / +28.9% | +23.3% / +27.5% | +20.4% / +19.9% | +22.9% / +48.4% | +27.8% / +13.2% |
| Random (pretty) | +19.9% / +22.9% | +15.5% / +15.0% | +14.7% / +13.6% | +13.8% / +32.3% | +15.3% / +23.0% |
| Twitter (min) | +18.2% / +20.2% | +15.2% / +8.7% | +7.8% / +5.9% | +17.7% / +25.2% | −5.2% / +7.7% |
| Twitter (pretty) | +19.5% / +18.6% | +0.9% / −1.3% | +9.3% / +8.4% | −3.5% / +7.3% | +0.8% / tie |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +26.7% / +36.2% | +12.7% / +11.5% | +18.7% / +22.0% | +31.3% / +42.1% | +7.5% / +13.0% |
| Apache Builds (pretty) | +23.1% / +29.6% | +3.0% / +4.4% | +14.4% / +14.2% | +6.8% / +31.9% | tie / tie |
| Canada (min) | +50.0% / +50.0% | +4.0% / +5.9% | +28.5% / +21.3% | +22.1% / tie | −4.9% (n/c) / −9.5% |
| Canada (pretty) | +74.5% / +69.4% | +7.0% / +7.5% | +29.2% / +28.4% | tie / +8.3% | +18.4% / +7.9% |
| CitmCatalog (min) | +24.1% / +23.8% | +10.4% / +10.2% | +5.5% / +7.2% | +26.6% / −9.8% | +19.9% / +22.6% |
| CitmCatalog (pretty) | +29.4% / +32.1% | +4.8% / +1.8% | +4.7% / +2.3% | tie / −6.3% | −5.2% / +29.3% |
| Discord (min) | +43.6% / +55.9% | +49.5% / +52.5% | +43.4% / +50.2% | +41.4% / +44.0% | +80.1% / +61.2% |
| Discord (pretty) | +44.7% / +50.2% | +36.6% / +38.6% | +36.6% / +40.8% | +50.3% / +37.5% | +28.5% / +60.9% |
| Github Events (min) | +44.7% / +63.4% | +23.8% / +29.5% | +33.2% / +28.9% | +28.6% / +38.7% | +16.2% / +46.9% |
| Github Events (pretty) | +41.8% / +47.9% | +19.9% / +25.8% | +25.1% / +27.7% | +17.5% / +20.4% | +13.4% / +23.2% |
| Google Maps (min) | +24.7% / +25.2% | +7.0% / +6.9% | +6.2% / +6.3% | tie / +17.2% | +23.7% / +6.0% |
| Google Maps (pretty) | +22.1% / +23.8% | −1.3% / +0.3% | +0.7% / +2.9% | +8.7% / tie | −2.6% / −0.6% |
| Instruments (min) | +34.5% / +40.9% | +23.4% / +32.0% | +25.2% / +21.2% | +8.6% / +18.8% | +31.5% / +51.1% |
| Instruments (pretty) | +36.3% / +45.6% | +20.5% / +21.5% | +22.7% / +26.6% | +42.0% / +13.9% | +27.0% / +37.8% |
| Marine IK (min) | +38.0% / +38.8% | +30.3% / +31.6% | +51.0% / +47.6% | +36.7% / +42.9% | +49.8% / +23.3% |
| Marine IK (pretty) | +47.3% / +52.0% | +20.8% / +22.9% | +38.0% / +40.8% | +54.0% / +39.1% | +24.6% / +4.3% |
| Random (min) | +36.3% / +41.3% | +37.6% / +32.5% | +24.4% / +25.4% | +32.6% / +37.2% | +36.3% / +46.3% |
| Random (pretty) | +30.1% / +36.8% | +22.4% / +19.3% | +23.2% / +21.2% | +31.4% / +23.2% | +32.0% / +34.6% |
| Twitter (min) | +29.5% / +30.3% | +20.9% / +19.1% | +15.5% / +11.4% | +14.9% / +14.9% | +21.8% / +20.7% |
| Twitter (pretty) | +22.1% / +20.0% | +11.4% / +9.0% | +14.8% / +15.1% | +11.1% / +8.8% | +11.6% / +11.0% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 4.4x faster at full size: +93.3% (Twitter pretty, macOS/GCC, fresh) up to +343.3% (Discord min, macOS/Clang, reused). The small documents show the same pattern, from +94.4% to +398.6%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and the full-size margin runs from −8.4% (minified Canada on Linux/GCC, reused) to +27.1%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events rarely loses.** It accounts for 2 of the 95 single-document losses, both prettified sparse reads of Github Events Small on macOS/Clang (−7.3% fresh, −4.3% reused). Read in reverse at full size it wins on every build (+85.8% to +195.0%), and on MSVC it wins all 32 of its results.
- **Sparse reverse reads are a strength.** Jsonifier won 194 of 200 full-size sparse-reverse results across both allocation modes (3 ties, 3 losses), and 185 of 200 small ones (7 ties, 8 losses). MSVC won all 80. Every loss is on Canada, CitmCatalog or Google Maps. The full-size losses are minified Canada on Linux/GCC (−2.6% fresh, −3.4% reused) and prettified Canada on macOS/GCC (reused, −3.0%). Of the 8 small losses, 5 are on macOS/Clang (−0.6% to −9.5%), 2 on macOS/GCC (CitmCatalog Small, reused, −9.8% and −6.3%) and 1 on Linux/Clang (prettified Google Maps Small, fresh, −1.3%).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are slower than on the other builds: Discord Reverse (pretty, fresh) runs at 237.2 MB/s on MSVC against 271.5 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 631.5 and 907.5 MB/s. Every MSVC full-size sparse result is a win, by +17.6% to +84.0%, and every full-size sparse-reverse result by +18.4% to +76.9%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 190 of 200 full-size sparse results (no ties, 10 losses) and 174 of 200 small ones (8 ties, 18 losses). MSVC won all 40 of its full-size results and all 40 small. The full-size losses are on Linux/GCC (6: minified Canada, −3.1% and −5.2%, and Marine IK, −7.4% to −9.1%), macOS/Clang (2: prettified Instruments, −2.1% fresh, −0.7% reused) and macOS/GCC (2: minified Marine IK, −1.5% and −1.4%). macOS/Clang holds 10 of the 18 small losses, Linux/Clang 4 and macOS/GCC 4.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 175, tied 6 and lost 19. Linux/Clang went 40-0-0, macOS/Clang 35-1-4, MSVC 34-2-4, Linux/GCC 33-1-6 and macOS/GCC 33-2-5 across the two modes.
- **Small documents read in order take the most losses of any category.** Across 200 results, Jsonifier won 172, tied 5 and lost 23. MSVC went 38-0-2, macOS/GCC 38-0-2, Linux/Clang 35-0-5, macOS/Clang 32-2-6 and Linux/GCC 29-3-8. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Most losses are modest: Linux/Clang's run from −1.4% to −4.7%, led by CitmCatalog and Instruments, and Linux/GCC's from −0.9% to −12.4%, led by Canada. The largest is prettified Instruments Small on macOS/Clang (fresh, −15.5%).
- **In-order losses at full size fall on a few documents per build.** Linux/GCC lost Canada in both forms and both runs (−4.1% to −12.9%) and minified Marine IK in both runs (−5.3% and −6.9%). MSVC lost minified Canada (−1.9% and −2.8%) and minified Marine IK (−5.4% and −6.4%) in both runs. macOS/GCC lost Marine IK four times (−0.4% to −7.2%, one n/c) and prettified CitmCatalog (reused, −6.0%). macOS/Clang lost Canada twice (minified fresh, −6.8%; prettified reused, −6.2%) and prettified Instruments in both runs (−1.9% and −2.5%). Linux/Clang lost none. Canada and Marine IK, which hold 16 of the 19, are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup.
- **The POD tests mostly win.** Jsonifier went 44-0-6 on them. Double, Int64 and Uint64 won all 10 results each (+59.2% to +206.7%, +50.7% to +175.0% and +42.0% to +166.8%), and Bool won 9 (+4.6% to +282.5%), losing only on macOS/Clang (fresh, −4.7%). String is the exception: 5 wins and 5 losses, on Linux/Clang (−6.5% and −10.8%) and macOS/Clang (−18.2% and −26.4%) in both runs, and on macOS/GCC (fresh, −9.1%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on the three x86 builds, Jsonifier's comma-separated throughput is 3.6% to 14.8% below its own NDJSON throughput on the same records in 17 of the 18 build and stream pairs; the exception is CitmCatalog on MSVC, where comma mode is 3.6% faster.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 20 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 20 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 27 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 5-1-0 | 12-0-0 | 12-0-0 | 29-1-0 |
| Linux / GCC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| macOS / GCC | 5-0-1 | 10-2-0 | 12-0-0 | 27-2-1 |
| macOS / Clang | 4-1-1 | 12-0-0 | 12-0-0 | 28-1-1 |
| **All** | **26-2-2** | **58-2-0** | **60-0-0** | **144-4-2** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +32.7% | +16.4% | +14.3% | +26.4% | +20.5% |
| Large Amazon Cellphones (NDJSON) | +15.3% | +11.8% | +3.5% | +7.1% | +16.2% (n/c) |
| Stream Formats Small (NDJSON) | +38.7% | +16.1% | +28.9% | −9.3% (n/c) | +9.5% |
| Stream Formats Small (comma) | +77.6% | +113.3% | +95.9% | +86.9% (n/c) | +61.7% |
| Stream Formats Large (NDJSON) | +87.9% | tie | +24.9% | +13.3% | tie |
| Stream Formats Large (comma) | +97.8% | +5.9% | +38.1% | +32.7% | −3.4% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +22.7% | +30.4% | +26.4% | +40.2% | +31.1% |
| Apache Builds (comma) | +33.2% (n/c) | +45.3% | +42.2% | +60.1% (n/c) | +40.7% |
| CitmCatalog (NDJSON) | +10.2% (n/c) | +6.6% | +12.2% | tie | +15.7% |
| CitmCatalog (comma) | +27.7% (n/c) | +32.2% | +36.0% | +29.2% | +29.7% |
| Google Maps (NDJSON) | +27.5% | +28.6% | +27.8% | +29.2% (n/c) | +8.0% |
| Google Maps (comma) | +37.9% | +48.6% | +46.1% | +32.1% | +19.2% |
| Instruments (NDJSON) | +47.3% (n/c) | +14.1% | +16.5% | tie | +9.3% |
| Instruments (comma) | +51.7% (n/c) | +45.5% | +48.2% | +41.8% | +50.2% |
| Random (NDJSON) | +29.5% | +30.7% | +29.4% | +61.9% | +47.6% |
| Random (comma) | +34.1% | +47.9% | +47.0% | +86.2% (n/c) | +55.8% (n/c) |
| Twitter (NDJSON) | +17.2% | +17.5% | +2.3% | +20.4% (n/c) | +12.8% |
| Twitter (comma) | +30.9% | +31.8% | +19.5% | +13.9% (n/c) | +21.8% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +26.0% (n/c) | +16.9% | +12.8% | +47.0% | +25.0% |
| Apache Builds (comma) | +43.0% | +36.1% | +32.9% | +54.6% | +37.8% |
| CitmCatalog (NDJSON) | +31.7% | +32.3% | +31.2% | +35.0% | +41.3% |
| CitmCatalog (comma) | +49.1% | +57.6% | +50.8% | +64.2% | +61.0% |
| Google Maps (NDJSON) | +23.2% | +11.6% | +8.8% | +17.6% | +10.6% |
| Google Maps (comma) | +28.6% (n/c) | +26.3% | +21.3% | +30.2% | +22.0% |
| Instruments (NDJSON) | +63.3% (n/c) | +51.9% | +17.9% | +29.2% | +43.3% |
| Instruments (comma) | +64.4% | +77.3% | +41.8% | +50.6% | +53.3% (n/c) |
| Random (NDJSON) | +65.0% | +90.0% | +79.6% | +96.5% (n/c) | +109.9% |
| Random (comma) | +73.3% | +109.6% | +91.6% | +94.2% | +122.9% |
| Twitter (NDJSON) | +235.5% (n/c) | +166.1% | +146.4% | +133.9% (n/c) | +242.5% |
| Twitter (comma) | +242.4% (n/c) | +192.9% | +157.0% | +140.3% (n/c) | +237.7% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 26 of the 30 results in that group, tied 2 and lost 2. It won Stream Formats Small (comma) (+61.7% to +113.3%), Amazon Cellphones (+14.3% to +32.7%) and Large Amazon Cellphones (+3.5% to +16.2%) on all five builds, and Stream Formats Small (NDJSON) on four (+9.5% to +38.7%), losing on macOS/GCC (−9.3%). Stream Formats Large (comma) won on four builds and lost on macOS/Clang (−3.4%); Stream Formats Large (NDJSON) won on three and tied on Linux/Clang (+0.0%) and macOS/Clang (−1.1%).
- **Windows/MSVC and Linux/GCC won every streaming test,** 30-0-0 each. Linux/Clang went 29-1-0, macOS/Clang 28-1-1 and macOS/GCC 27-2-1. Both streaming losses are Stream Formats reads on the macOS builds.
- **In-order corpus streams never lose.** Of the 60 in-order corpus results, Jsonifier won 58 and tied 2, CitmCatalog (NDJSON, +2.7%) and Instruments (NDJSON, +1.2%), both on macOS/GCC.
- **Reverse order carries over from single documents to streams.** Jsonifier won all 60 reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +133.9% to +242.5%, while Google Maps rows, which have few, give +8.8% to +30.2%. Instruments, whose reverse streams have the most room to lose, wins on every build (+17.9% to +77.3%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 327.6 MB/s under MSVC and 414.4 MB/s under Linux/Clang, while Jsonifier reads it at 567.6 and 868.5 MB/s.

## 9. Limitations

- **macOS/Clang and Linux/GCC hold the most single-document losses:** 30 and 29 of the 95, against 17 on macOS/GCC, 12 on Linux/Clang and 7 on MSVC. 21 of macOS/Clang's are small documents, led by 10 small sparse, and 12 of Linux/GCC's, led by 8 small in-order. Both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest single losses are macOS/Clang's prettified Marine IK Small sparse read (fresh, −27.4%) and String POD test (reused, −26.4%). The largest x86 loss is Linux/GCC's minified Canada read in order (reused, −12.9%).
- **Canada, Google Maps and CitmCatalog hold most of the losses.** Together they account for 47 of the 95: Canada 32 (19 on Linux/GCC, 7 on macOS/Clang, 3 on macOS/GCC, 3 on MSVC), CitmCatalog 11 (4 on Linux/Clang, 4 on macOS/GCC, 2 on macOS/Clang) and Google Maps 4 (3 on macOS/Clang). Marine IK, the other float-dense document, adds 23. Their objects are small, so the field index and the key trail have little to save, and Canada is dominated by float parsing.
- **Linux/GCC loses mostly on Canada.** 19 of its 29 losses are Canada, at both sizes, where much of the cost is number parsing rather than field lookup.
- **Small documents carry most of the losses.** 54 of the 95 losses are small documents (21 on macOS/Clang, 12 on Linux/GCC, 10 on Linux/Clang, 8 on macOS/GCC, 3 on MSVC). At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Stream Formats reads hold both streaming losses:** Stream Formats Small (NDJSON) on macOS/GCC (−9.3%) and Stream Formats Large (comma-separated) on macOS/Clang (−3.4%). No corpus record stream loses.
- **Some results did not converge.** 8 of the 1,650 single-document results did not converge: 4 on macOS/GCC, 3 on macOS/Clang and 1 on Windows/MSVC. 23 of the 150 streaming results did not converge: 10 on Windows/MSVC, 10 on macOS/GCC and 3 on macOS/Clang. Linux/Clang and Linux/GCC converged on every result.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,519, tied 36 and lost 95. On reverse reads it won 385 of 400 at both sizes, and on Windows/MSVC it lost 7 of its 330 single-document results. Reusing the output objects speeds up both libraries and leaves the verdicts largely unchanged (761-18-46 fresh, 758-18-49 reused). The main remaining targets are the small-object and float-heavy documents (Canada, Google Maps, CitmCatalog and Marine IK, with 70 of the 95 losses), small documents in general, and the macOS/Clang and Linux/GCC builds.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 144, tied 4 and lost 2, and on the 120 corpus record-stream results it went 118-2-0. The Stream Formats reads on the macOS builds, which hold both losses, are the main remaining streaming target.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.