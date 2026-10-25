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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,383 of 1,650 single-document results against simdjson On Demand, tied 30 and lost 237. On reverse-order reads it won 375 of 400. On Windows/MSVC it won 325 of 330 single-document results, tied 4 and lost 1.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 30 streaming tests, Jsonifier won 134 of 150 results, tied 6 and lost 10, and won every streaming test on Windows/MSVC.

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

Once an object has shown that it is being read out of order, tier 2 is replaced by a probe of the field index described in §5.

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

On macOS/Clang, 16 kept larger margins on the documents whose objects have many fields (Twitter, Discord, Mesh), while 8 won on Random, Google Maps and Instruments. Neither produced a reverse loss.

### 5.5 Requiring evidence of out-of-order access

A skipped-key count cannot tell a large object read out of order from a large object read sparsely in order. A sparse reader that asks for three fields of a 30-field object skips most of its keys on the way, so the count crosses *T* even though every lookup is found ahead of the cursor. Recording keys during those skips costs a hash and an insert per key for an index that no lookup needs.

The trigger in the measured code (§6) therefore asks for two kinds of evidence, both kept per object handle:

- **Size:** the skipped-key count has reached *T*, chosen per build as in §5.4. This is the gate that keeps small objects such as Marine IK's off the map.
- **Out-of-order access:** at least two lookups on this object have missed the expected field, scanned to the end of the object, and wrapped around to its start. A wrap is the event that only out-of-order reads produce. One wrap is not enough: a sparse reverse read of three fields often wraps once on its last lookup, whose key is first in the object, and an index built then would serve no further lookup.

Until both hold, lookups use scan-and-wrap and record nothing, so skips never pay for hashing. Once both hold, the next lookup that misses the expected field builds the object's index in one dedicated pass over the whole object, recording every key, and then probes it. Every later out-of-order lookup on that object is a single probe; a key the map does not hold falls through to the escape-aware retry of §3.4.

## 6. Methodology

- **Harness:** Json-Performance, branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 10 documents (Apache Builds, Canada, CitmCatalog, Discord, Github Events, Google Maps, Instruments, Marine IK, Random and Twitter) each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 170 single-document results per build at full size and another 160 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document, reused, Apache Builds and Github Events tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 56 of the 1,650 single-document results and 36 of the 150 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M2 Pro hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 152 / 170 | 159 / 160 | 311 / 330 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 170 / 170 | 160 / 160 | 330 / 330 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 145 / 170 | 155 / 160 | 300 / 330 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 164 / 170 | 159 / 160 | 323 / 330 |

All builds ran BenchmarkSuite `35ec835`, Jsonifier `d11b7d3` and simdjson `50f0739`. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on October 25, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 19-1-0 | 20-0-0 | 20-0-0 | 20-0-0 | 18-1-1 | 20-0-0 | 20-0-0 | 20-0-0 | 162-2-1 |
| Linux / Clang | 5-0-0 | 18-0-2 | 20-0-0 | 14-1-5 | 11-0-9 | 11-2-7 | 17-0-3 | 12-0-8 | 12-0-8 | 120-3-42 |
| Linux / GCC | 5-0-0 | 12-0-8 | 18-0-2 | 18-0-2 | 20-0-0 | 8-0-12 | 16-0-4 | 15-1-4 | 18-0-2 | 130-1-34 |
| macOS / GCC | 5-0-0 | 14-3-3 | 20-0-0 | 18-1-1 | 18-0-2 | 17-0-3 | 20-0-0 | 19-1-0 | 20-0-0 | 151-5-9 |
| macOS / Clang | 3-0-2 | 14-2-4 | 20-0-0 | 13-0-7 | 17-1-2 | 14-1-5 | 17-0-3 | 10-1-9 | 15-1-4 | 123-6-36 |
| **All** | **23-0-2** | **77-6-17** | **98-0-2** | **83-2-15** | **86-1-13** | **68-4-28** | **90-0-10** | **76-3-21** | **85-1-14** | **686-17-122** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 18-2-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 20-0-0 | 163-2-0 |
| Linux / Clang | 5-0-0 | 18-0-2 | 20-0-0 | 11-2-7 | 11-0-9 | 13-0-7 | 17-0-3 | 12-0-8 | 12-0-8 | 119-2-44 |
| Linux / GCC | 5-0-0 | 12-0-8 | 18-0-2 | 18-0-2 | 20-0-0 | 10-0-10 | 16-0-4 | 15-2-3 | 18-0-2 | 132-2-31 |
| macOS / GCC | 5-0-0 | 17-1-2 | 19-0-1 | 20-0-0 | 19-1-0 | 18-0-2 | 19-0-1 | 19-0-1 | 20-0-0 | 156-2-7 |
| macOS / Clang | 4-0-1 | 15-1-4 | 20-0-0 | 15-0-5 | 16-2-2 | 16-0-4 | 18-0-2 | 10-1-9 | 13-1-6 | 127-5-33 |
| **All** | **24-0-1** | **80-4-16** | **97-0-3** | **84-2-14** | **86-3-11** | **77-0-23** | **90-0-10** | **76-3-21** | **83-1-16** | **697-13-115** |


Each cell is wins-ties-losses.

Across all 1,650 single-document results (56 of them did not converge; see §9), Jsonifier won 1,383, tied 30 and lost 237: 686-17-122 with fresh output objects and 697-13-115 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 195 of 200 full-size reverse results (no ties, 5 losses) and 180 of 200 small reverse results (no ties, 20 losses). All five full-size losses are Canada: Linux/GCC in both forms and both runs (−1.3% to −5.7%) and macOS/GCC minified reused (−5.6%, n/c). Of the 20 small losses, 13 are Github Events Small, on every build except MSVC (−4.0% to −18.4%), 4 are Canada Small on Linux/GCC (−1.1% to −6.3%) and 3 are prettified Google Maps Small, on Linux/Clang in both runs (−2.9%) and macOS/Clang fresh (−0.8%). Linux/Clang and macOS/Clang won all 40 of their full-size reverse results.
- **Windows/MSVC lost one of its 330 single-document results.** The loss is prettified Canada Small read in order (fresh, −1.9%), and its 4 ties are all Canada read in order: three at full size and minified Canada Small (fresh). It won all 10 POD results and every other test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6.8–10.8% for Jsonifier and 4.7–7.7% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +0.6 to +4.2 percentage points on the non-MSVC builds and by +12.4 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +1550.7% to +1724.5%, and Apache Builds (pretty) from +25.6% to +65.3%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +60.7% / +74.1% | +37.6% / +37.5% | +33.9% / +27.7% | +22.5% / +24.2% | −5.0% / +6.5% |
| Double | +23.8% (n/c) / +20.3% | +53.9% / +54.4% | +47.5% / +53.7% | +64.1% / +85.9% | +51.1% / +36.9% |
| Int64 | +50.2% / +71.6% (n/c) | +69.6% / +75.3% | +33.8% / +35.0% | +44.0% / +63.1% | +24.3% / +25.9% |
| String | +7.4% / +7.7% | +4.0% / +6.2% | +4.3% / +8.4% | +13.7% / +14.6% | −22.4% / −17.3% |
| Uint64 | +59.1% / +67.4% | +62.2% / +65.1% | +53.2% / +52.6% | +45.1% / +53.2% | +28.8% / +27.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +19.4% / +53.9% | +13.7% / +21.6% | +11.4% / +18.4% | +30.2% / +37.7% | +7.8% / +11.1% |
| Apache Builds (pretty) | +25.6% / +65.3% | +13.2% / +19.5% | +11.3% / +16.2% | +36.1% / +41.1% | +7.7% / +10.1% |
| Canada (min) | tie (n/c) / tie | +4.4% / +4.8% | −5.7% / −6.7% | tie / +14.7% | +20.5% / +13.6% |
| Canada (pretty) | +6.2% (n/c) / tie | +3.6% / +4.3% | −6.8% / −6.7% | +7.5% / +6.7% (n/c) | +9.5% / +10.6% |
| CitmCatalog (min) | +29.0% / +41.6% | +4.6% / +4.5% | +5.1% / +4.5% | +20.9% / +22.1% | +1.1% / +5.1% |
| CitmCatalog (pretty) | +25.6% / +29.3% | +5.1% / +2.8% | +2.7% / +4.1% | +13.0% / +13.4% | +1.7% / +5.3% |
| Discord (min) | +53.6% / +75.9% | +15.4% / +19.9% | +2.0% / +8.0% | +24.1% / +32.7% | +7.0% / +7.4% |
| Discord (pretty) | +44.0% / +61.2% | +11.3% / +18.1% | +2.8% / +8.0% | −6.7% (n/c) / +23.0% | −8.1% / +3.8% |
| Github Events (min) | +95.2% / +125.4% | −9.5% / −17.4% | −11.9% / −12.5% | tie / +4.3% | −15.1% / −16.2% |
| Github Events (pretty) | +99.8% / +115.8% | −14.7% / −17.8% | −10.9% / −11.6% | tie / +7.7% | −16.3% / −18.7% |
| Google Maps (min) | +29.4% / +36.3% | +19.0% / +18.8% | +16.3% / +16.7% | +18.4% / +20.4% | +4.6% / +4.5% |
| Google Maps (pretty) | +39.0% / +31.6% | +11.6% / +9.8% | +13.0% / +13.2% | +12.9% / +33.4% | +2.5% / +1.5% |
| Instruments (min) | +53.6% / +69.2% | +10.7% / +13.1% | −2.5% / −2.9% | +9.2% / +16.9% | +2.9% / +3.2% |
| Instruments (pretty) | +67.7% / +63.8% | +8.6% / +9.9% | −4.5% / −5.9% | +5.3% / +8.6% | tie / −3.6% |
| Marine IK (min) | +14.6% / +19.3% | +0.8% / +0.4% | −2.7% / −4.0% | −1.4% / −8.9% | +2.9% / +6.9% |
| Marine IK (pretty) | +12.2% / +8.6% | +1.5% / +0.8% | −3.4% / −1.6% | −7.9% (n/c) / tie | +7.2% (n/c) / +34.0% (n/c) |
| Random (min) | +36.1% / +59.1% | +23.0% / +27.2% | +28.2% / +34.6% | +10.9% (n/c) / +71.7% (n/c) | +21.6% / +27.0% |
| Random (pretty) | +44.1% / +62.1% | +22.9% / +28.7% | +28.7% / +33.9% | +58.5% / +45.8% (n/c) | +20.6% / +23.5% |
| Twitter (min) | +36.5% / +59.6% | +9.4% / +15.6% | +7.7% / +9.9% | +34.4% (n/c) / +19.7% | tie / tie |
| Twitter (pretty) | +34.4% / +43.3% | +2.8% / +11.6% | +9.1% / +6.1% | +40.2% / −7.4% (n/c) | −2.3% / −2.0% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +395.7% / +507.8% | +70.7% / +87.0% | +86.9% / +109.4% | +119.5% (n/c) / +154.9% | +66.4% / +82.7% |
| Apache Builds (pretty) | +355.4% / +606.9% (n/c) | +69.7% / +87.8% | +84.5% / +105.9% | +70.8% (n/c) / +84.4% | +61.0% / +57.1% |
| Canada (min) | +18.6% / +21.3% | +2.6% / +2.7% | −1.3% / −1.4% | +22.7% (n/c) / −5.6% (n/c) | +8.0% / +11.9% |
| Canada (pretty) | +23.0% (n/c) / +24.5% (n/c) | +2.2% / +4.2% | −4.2% / −5.7% | +9.5% (n/c) / +10.1% | +4.7% / +14.2% |
| CitmCatalog (min) | +281.8% / +291.6% | +64.1% / +66.5% | +71.5% / +76.0% | +60.8% / +65.2% | +65.4% / +41.2% |
| CitmCatalog (pretty) | +277.2% / +301.2% (n/c) | +61.9% / +65.5% | +69.5% / +72.9% | +63.8% / +63.1% | +62.7% / +69.5% |
| Discord (min) | +1550.7% / +1724.5% | +235.6% / +263.8% | +309.2% / +337.5% | +297.2% (n/c) / +346.5% | +267.6% / +275.3% |
| Discord (pretty) | +1425.1% / +1658.2% | +232.8% / +256.1% | +305.4% / +338.1% | +251.5% (n/c) / +302.2% | +257.4% / +270.3% |
| Github Events (min) | +271.6% / +327.2% | +64.0% / +67.4% | +43.1% / +49.1% | +66.9% / +67.6% | +43.3% / +53.5% |
| Github Events (pretty) | +251.3% / +301.1% | +55.5% / +58.8% | +35.9% / +41.3% | +53.4% / +44.9% | +37.8% / +42.4% |
| Google Maps (min) | +103.5% / +94.0% | +2.7% / +4.1% | +16.8% / +17.4% | +21.0% / +31.1% | +8.2% / +9.6% |
| Google Maps (pretty) | +94.7% / +101.5% | +1.2% / +0.4% | +19.1% / +16.6% | +25.9% / +21.8% | +5.2% / +4.7% |
| Instruments (min) | +677.1% / +673.4% | +125.2% / +129.2% | +129.8% / +133.2% | +140.5% / +145.5% | +82.6% / +132.9% |
| Instruments (pretty) | +648.3% / +679.3% | +115.4% / +119.0% | +118.9% / +124.1% | +132.1% / +159.5% | +123.1% / +132.1% |
| Marine IK (min) | +339.5% (n/c) / +337.5% (n/c) | +33.0% / +33.3% | +67.8% / +70.5% | +63.8% (n/c) / +52.8% | +35.7% / +41.4% (n/c) |
| Marine IK (pretty) | +305.9% (n/c) / +324.9% (n/c) | +25.9% / +34.2% | +48.3% / +51.0% | +82.9% / +45.0% (n/c) | +34.4% (n/c) / +41.3% |
| Random (min) | +213.3% / +261.4% | +18.0% / +19.2% | +28.2% / +31.1% | +24.7% / +26.4% | +30.9% / +31.8% |
| Random (pretty) | +206.7% / +276.0% (n/c) | +18.1% / +17.7% | +26.0% / +30.2% | +71.4% / +25.7% | +27.5% / +31.2% |
| Twitter (min) | +717.4% / +799.0% | +192.5% / +205.7% | +133.6% / +143.0% | +80.6% (n/c) / +187.4% | +162.0% / +171.3% |
| Twitter (pretty) | +761.2% / +835.4% | +179.9% / +190.5% | +122.7% / +131.6% | +156.5% / +136.1% (n/c) | +159.2% / +155.5% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +45.4% / +68.2% | +13.4% / +21.9% | +23.2% / +24.3% | +12.3% (n/c) / +63.2% | +10.3% / +12.8% |
| Apache Builds (pretty) | +41.4% / +73.3% | +18.7% / +21.5% | +12.1% / +19.6% | +28.9% / +23.2% | +4.8% / +16.3% |
| Canada (min) | +50.5% / +50.1% | −4.1% / −3.9% | +17.8% / +17.5% | +18.6% / +30.2% | +12.1% (n/c) / +24.5% |
| Canada (pretty) | +62.9% / +71.2% (n/c) | +1.2% / +0.8% | +15.2% / +14.9% | +16.6% / +21.6% | +19.1% / +19.4% |
| CitmCatalog (min) | +286.0% / +255.9% | +2.3% / +2.5% | +15.9% / +15.9% | +16.7% / +17.2% | +2.9% / +15.1% |
| CitmCatalog (pretty) | +205.8% / +214.3% (n/c) | +2.9% / +3.1% | +16.3% / +16.4% | tie / +24.3% | +14.4% / +14.2% |
| Discord (min) | +29.4% / +41.7% | +0.4% / −1.0% | +5.6% / +5.8% | +20.5% / +27.7% | +4.2% / +13.0% |
| Discord (pretty) | +30.1% / +46.3% | +0.2% / tie | +7.7% / +5.2% | +15.6% / +13.1% | −0.9% / −2.3% |
| Github Events (min) | +144.9% / +190.9% | +3.1% / +3.1% | +20.2% / +22.4% | +67.0% / +55.3% | +10.5% / +10.2% |
| Github Events (pretty) | +131.8% / +163.1% | +1.2% / +1.7% | +12.2% / +15.4% | +27.7% / +25.9% | −15.4% / +3.6% |
| Google Maps (min) | +50.8% / +60.8% | +0.8% / −1.7% | −2.1% / −3.1% | +12.1% / +11.7% | −3.6% / −1.7% |
| Google Maps (pretty) | +55.8% / +58.8% | −2.7% / −1.6% | −2.9% / −3.5% | +20.5% / +20.1% | −4.5% / +1.9% |
| Instruments (min) | +43.1% / +56.6% | −1.7% / −1.6% | +1.9% / +2.2% | +14.8% / +11.6% | −2.9% / −3.9% |
| Instruments (pretty) | +47.8% / +59.6% | −1.0% / −1.5% | +0.6% / +0.6% | +15.5% / +20.9% | −10.2% / −10.4% |
| Marine IK (min) | +48.0% (n/c) / +44.2% (n/c) | +26.7% / +26.9% | +29.6% / +28.8% | +49.7% / +39.4% (n/c) | +29.5% / +31.1% |
| Marine IK (pretty) | +42.5% / +55.6% | +16.3% / +17.0% | +13.4% / +14.2% | +18.6% / +35.1% | +25.8% / +24.9% |
| Random (min) | +68.5% / +68.8% | +9.3% / +11.8% | +8.8% / +9.0% | +30.3% / +41.4% | +14.9% / +21.2% |
| Random (pretty) | +59.9% / +69.2% | +8.6% / +12.0% | +9.1% / +8.1% | +16.0% / +29.4% | +11.0% / +11.1% |
| Twitter (min) | +122.0% / +125.2% | tie / −0.1% | +10.9% / +12.9% | +19.8% / +26.7% | +3.1% / +3.2% |
| Twitter (pretty) | +108.4% / +112.6% | −0.3% / tie | +6.6% / +9.2% | −2.6% / +6.5% | −2.3% / −7.2% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +84.0% / +101.0% | +17.0% / +20.5% | +27.1% / +30.9% | +73.2% / +51.8% | +16.7% / +19.5% |
| Apache Builds (pretty) | +91.1% / +120.3% | +15.1% / +18.0% | +21.0% / +26.4% | −11.4% (n/c) / +42.5% | +10.8% / +12.2% |
| Canada (min) | +252.8% / +269.3% | −2.0% / −1.1% | +24.0% / +23.6% | +23.8% / +30.0% | +11.8% / +14.5% |
| Canada (pretty) | +189.7% / +200.8% | +1.4% / +1.6% | +13.9% / +14.3% | +21.3% / +15.0% | +10.4% (n/c) / +9.6% |
| CitmCatalog (min) | +243.8% / +213.3% | +13.2% / +13.7% | +32.4% / +32.5% | +18.5% / +23.8% | +7.9% / +11.9% |
| CitmCatalog (pretty) | +208.9% / +208.4% | +11.5% / +11.4% | +29.3% / +28.2% | +30.2% / +26.2% | +12.7% / +14.1% |
| Discord (min) | +127.8% / +140.2% | −0.3% / −0.4% | +13.2% / +16.0% | +23.9% / +28.8% | +9.6% / +8.4% |
| Discord (pretty) | +112.7% (n/c) / +139.3% | −2.4% / −1.3% | +11.2% / +13.5% | +26.7% / +18.2% | +2.9% / +3.7% |
| Github Events (min) | +187.5% / +200.0% | −9.9% / −9.6% | +28.5% / +28.3% | +31.0% / +39.3% | +4.3% / +6.1% |
| Github Events (pretty) | +173.4% / +187.4% | −11.7% / −9.5% | +21.0% / +22.0% | +21.9% / +29.4% | +4.3% / +11.3% |
| Google Maps (min) | +78.1% / +78.0% | −3.7% / −4.5% | +6.9% / +5.9% | +11.5% / +25.0% | −8.7% / −5.1% |
| Google Maps (pretty) | +85.5% / +73.1% | −6.5% / −5.8% | +6.3% / +7.1% | +18.4% / +12.5% | −1.6% / −3.4% |
| Instruments (min) | +200.6% / +214.4% | +10.0% / +10.3% | +20.1% / +20.9% | +28.7% / +28.5% | +7.7% / +6.6% |
| Instruments (pretty) | +186.9% / +198.8% | +9.7% / +8.7% | +17.1% / +18.6% | +25.9% / +25.0% | +2.1% / tie |
| Marine IK (min) | +208.9% / +230.1% | +20.7% / +21.3% | +62.9% / +64.2% | +70.7% / +38.5% | +32.7% / +33.7% |
| Marine IK (pretty) | +188.3% / +195.4% | +15.1% / +16.1% | +39.4% / +39.8% | +18.2% / +13.0% | +31.4% / +26.7% |
| Random (min) | +90.3% / +101.1% | +2.0% / +2.5% | +7.7% / +7.9% | +12.3% (n/c) / +25.6% (n/c) | +6.7% / +11.2% |
| Random (pretty) | +84.5% / +96.9% | +1.7% / +3.1% | +7.7% / +7.8% | +68.0% / +5.5% (n/c) | +7.5% / +8.2% |
| Twitter (min) | +197.3% / +204.7% | −1.7% / −0.9% | +16.1% / +15.9% | −5.5% / tie | +4.6% / +4.7% |
| Twitter (pretty) | +184.3% / +185.1% | −1.2% / −1.1% | +12.8% / +13.7% | +18.2% / +17.8% | tie / tie |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +35.7% / +56.2% | +13.5% / +13.0% | +12.6% / +22.6% | +28.5% / +49.1% | +13.8% / +9.3% |
| Apache Builds (pretty) | +29.1% / +60.8% | +8.3% / +13.8% | +14.0% / +17.2% | +19.3% / +43.5% | +12.6% / +11.2% |
| Canada (min) | tie / +2.1% | tie / −0.8% | −11.3% / −11.7% | +5.9% / +7.0% | +8.5% / +8.1% |
| Canada (pretty) | −1.9% / +0.9% | +3.0% / +1.4% | −11.0% / −12.3% | +24.8% / +4.0% | +8.5% / +4.8% |
| CitmCatalog (min) | +6.7% / +19.8% | −5.9% / −6.2% | −2.1% / −2.1% | +5.1% / +9.9% | −1.0% / +6.2% |
| CitmCatalog (pretty) | +6.3% / +19.2% | −8.5% / −6.5% | −2.5% / −2.2% | +5.8% / +7.5% | −4.2% / −2.0% |
| Discord (min) | +25.8% / +56.9% | +1.6% / +8.2% | −0.5% / +5.7% | +25.2% / +50.5% | +5.2% / +11.4% |
| Discord (pretty) | +27.3% / +56.5% | +1.0% / +4.9% | −0.8% / +7.3% | +11.4% / +23.1% (n/c) | +2.0% / +4.4% |
| Github Events (min) | +35.9% / +35.9% | −28.2% / −28.7% | −32.2% / −33.6% | −16.6% / −18.1% | −13.9% / −16.7% |
| Github Events (pretty) | +33.1% / +40.9% | −26.2% / −30.1% | −29.7% / −34.4% | −19.5% / −16.5% | −17.9% / −15.6% |
| Google Maps (min) | +28.0% / +32.5% | +18.4% / +18.0% | +13.3% / +16.2% | +16.4% / +28.6% | +11.3% / +7.0% |
| Google Maps (pretty) | +23.3% / +28.9% | +14.3% / +10.4% | +14.1% / +15.2% | +28.4% / +29.9% | +5.1% / +3.8% |
| Instruments (min) | +63.0% / +65.1% | +4.1% / +5.7% | −5.1% / −5.1% | +13.1% / +10.9% | +5.7% / +4.5% |
| Instruments (pretty) | +51.4% / +56.9% | tie / +1.9% | −6.9% / −7.0% | +4.3% / +5.2% | tie / −2.9% |
| Marine IK (min) | +5.2% / +2.3% | −0.6% / −0.4% | −1.8% / −3.9% | −6.6% / +8.4% | +5.5% / +6.5% |
| Marine IK (pretty) | +4.1% / +5.9% | −0.4% / −0.9% | −3.8% / −4.8% | +14.7% / +5.3% | +4.5% / +5.7% |
| Random (min) | +35.9% / +49.4% | +16.5% / +19.6% | +18.8% / +21.5% | +45.0% / +34.2% | +15.0% / +24.2% |
| Random (pretty) | +38.4% / +55.2% | +15.4% / +18.8% | +14.7% / +16.5% | +45.2% / +50.2% | +12.5% / +17.1% |
| Twitter (min) | +36.7% / +42.9% | −2.0% / +1.3% | +5.2% / +3.4% | +7.0% / +28.3% | +3.5% / +6.0% |
| Twitter (pretty) | +33.1% / +46.7% | +0.8% / +2.0% | +3.6% / +6.4% | +20.9% / +29.2% | −0.6% / +3.5% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +325.2% / +469.2% | +68.9% / +77.7% | +91.6% / +104.0% | +93.1% / +130.7% | +73.8% / +87.8% |
| Apache Builds (pretty) | +342.3% / +434.2% | +66.2% / +76.8% | +90.2% / +102.2% | +81.9% / +119.1% | +63.5% / +80.3% |
| Canada (min) | +18.8% / +16.9% | +3.9% / +1.7% | −1.1% / −6.3% | +1.3% / +10.5% | +5.3% / +4.5% |
| Canada (pretty) | +12.9% / +17.1% | +3.0% / +0.7% | −4.1% / −6.0% | +46.6% / +30.4% | +3.0% / +3.6% |
| CitmCatalog (min) | +206.8% / +220.6% | +46.2% / +50.2% | +64.2% / +72.1% | +27.6% / +31.9% | +49.0% / +56.8% |
| CitmCatalog (pretty) | +198.4% / +220.1% | +43.2% / +46.2% | +62.3% / +66.6% | +95.8% / +70.1% (n/c) | +42.4% / +60.9% |
| Discord (min) | +1202.3% / +1581.4% | +244.9% / +275.9% | +277.7% / +309.5% | +279.5% / +366.2% | +259.7% / +290.7% |
| Discord (pretty) | +1254.7% / +1441.9% | +248.8% / +274.1% | +277.2% / +310.5% | +259.0% / +438.5% (n/c) | +245.9% / +290.2% |
| Github Events (min) | +97.4% / +104.8% | −15.9% / −16.8% | −14.7% / −15.7% | +9.0% / −11.5% | −4.0% / −5.8% |
| Github Events (pretty) | +100.5% / +115.4% | −18.3% / −18.4% | −15.4% / −17.7% | +3.6% / +6.4% | −10.9% / −4.5% |
| Google Maps (min) | +98.5% / +114.3% | +4.4% / +2.4% | +17.8% / +16.6% | +20.2% / +37.2% | +13.5% / +13.7% |
| Google Maps (pretty) | +94.6% / +114.5% | −2.9% / −2.9% | +15.4% / +14.2% | +26.0% / +28.3% | −0.8% / +8.6% |
| Instruments (min) | +672.0% / +736.5% | +128.9% / +132.3% | +131.1% / +134.2% | +163.6% / +174.6% | +127.4% / +144.6% |
| Instruments (pretty) | +714.6% / +681.5% | +121.8% / +124.5% | +125.5% / +129.5% | +138.2% / +149.0% | +130.6% / +130.0% |
| Marine IK (min) | +188.8% / +190.8% | +27.6% / +28.6% | +51.3% / +53.9% | +38.6% / +56.7% | +30.7% / +33.1% |
| Marine IK (pretty) | +186.5% / +191.1% | +25.2% / +26.6% | +47.1% / +50.2% | +45.5% / +46.6% | +34.3% / +30.4% |
| Random (min) | +244.3% / +294.2% | +36.5% / +39.8% | +42.2% / +48.5% | +62.2% / +67.8% | +47.3% / +49.7% |
| Random (pretty) | +238.5% / +287.1% | +36.4% / +36.4% | +41.9% / +46.4% | +33.0% / +70.7% | +43.1% / +45.8% |
| Twitter (min) | +740.7% / +746.7% | +157.8% / +156.2% | +150.5% / +153.3% | +186.1% / +158.2% | +160.5% / +169.6% |
| Twitter (pretty) | +707.6% / +773.9% | +144.0% / +151.9% | +141.1% / +148.0% | +179.4% / +179.2% | +154.6% / +157.3% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +42.1% / +74.9% | +16.5% / +14.3% | +14.0% / +17.5% | +35.2% / +60.9% | +13.2% / +14.7% |
| Apache Builds (pretty) | +44.4% / +82.0% | +9.1% / +8.4% | +11.3% / +13.6% | +24.6% / +45.2% | +6.9% / +6.2% |
| Canada (min) | +22.2% / +23.4% | +14.3% / +14.8% | +18.2% / +16.0% | +37.5% / +41.8% | −8.6% / −6.2% |
| Canada (pretty) | +33.1% / +33.5% | +7.7% / +8.0% | +14.6% / +13.2% | +13.1% / +29.5% | −5.3% / −5.2% |
| CitmCatalog (min) | +152.0% / +166.7% | +3.2% / +4.9% | +17.5% / +18.1% | +64.1% (n/c) / +49.8% | +15.6% / +16.7% |
| CitmCatalog (pretty) | +130.1% / +149.0% | +2.3% / +1.1% | +16.5% / +14.8% | +49.6% / +80.0% | +6.7% / +10.5% |
| Discord (min) | +27.8% / +43.5% | +1.0% / +4.5% | +2.2% / +5.0% | +24.2% / +26.5% | +7.9% / +0.7% |
| Discord (pretty) | +25.7% / +40.3% | +0.6% / +0.5% | −1.3% / tie | tie / +5.6% | −9.6% / −20.9% |
| Github Events (min) | +100.1% / +126.6% | −8.6% / −10.1% | +6.8% / +4.6% | +17.3% / +45.2% (n/c) | +3.7% / +3.0% |
| Github Events (pretty) | +102.3% / +124.8% | −11.5% / −12.2% | +5.3% / +2.9% | +10.9% / +28.8% | −2.4% / −4.3% |
| Google Maps (min) | +56.8% / +81.6% | −1.1% / −1.3% | −5.5% / −4.3% | +14.6% / +7.3% | tie / tie |
| Google Maps (pretty) | +54.0% / +55.0% | −6.6% / −9.2% | −4.6% / −2.4% | +19.3% / +8.5% | −2.7% / +1.4% |
| Instruments (min) | +39.8% / +46.1% | −4.5% / −1.4% | tie / +0.5% | +17.3% / +16.9% | −2.5% / −68.5% |
| Instruments (pretty) | +27.4% / +40.7% | −5.4% / −6.1% | −3.0% / −3.7% | +3.9% / −5.0% | −8.1% / −10.1% |
| Marine IK (min) | +19.9% / +13.8% | +23.1% / +23.3% | +18.6% / +20.7% | +36.4% / +17.2% | +16.3% / +19.6% |
| Marine IK (pretty) | +24.5% / +22.0% | +10.8% / +12.1% | +13.9% / +14.8% | +14.0% / +26.4% | +8.4% / +8.1% |
| Random (min) | +42.3% / +45.4% | +7.1% / +8.6% | +3.0% / +4.0% | +35.1% / +20.7% | +4.4% / +6.7% |
| Random (pretty) | +44.8% / +45.4% | +5.5% / +6.1% | +2.4% / +1.1% | +27.3% / +33.9% | +4.2% / −9.7% |
| Twitter (min) | +106.1% / +113.1% | −13.1% / −12.5% | +5.7% / +5.1% | +15.8% / +29.0% | −0.8% / −0.7% |
| Twitter (pretty) | +101.4% / +97.6% | −10.2% / −11.7% | +1.6% / tie | +4.0% / +10.2% | −5.4% / −4.9% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (min) | +81.9% / +102.3% | +7.8% / +14.5% | +20.6% / +24.2% | +40.2% / +41.3% | +9.3% / +10.2% |
| Apache Builds (pretty) | +67.9% / +110.0% | +4.7% / +6.4% | +16.6% / +19.6% | +29.3% / +50.5% | +0.9% / +1.7% |
| Canada (min) | +136.9% / +157.5% | +22.7% / +21.5% | +27.0% / +22.4% | +33.4% / +19.8% | −9.6% / −9.7% |
| Canada (pretty) | +130.1% / +127.5% | +15.7% / +14.5% | +20.3% / +20.5% | +31.6% / +38.6% | −7.0% / −9.4% |
| CitmCatalog (min) | +162.6% / +150.3% | +11.5% / +12.3% | +25.7% / +24.9% | +58.0% / +2.4% | +9.8% / +11.2% |
| CitmCatalog (pretty) | +133.0% / +130.5% | +7.0% / +9.4% | +24.7% / +22.0% | +29.1% / +29.8% | +7.4% / +7.8% |
| Discord (min) | +138.3% / +157.4% | −1.0% / −1.2% | +8.0% / +7.5% | +7.7% / +15.6% | +7.4% / +8.4% |
| Discord (pretty) | +134.9% / +156.5% | −4.9% / −3.9% | +5.0% / +6.6% | +37.6% / +44.4% | +5.7% / +2.3% |
| Github Events (min) | +128.1% / +149.3% | −17.0% / −18.2% | −0.8% / −1.1% | +15.4% / +18.7% | +8.8% / +3.5% |
| Github Events (pretty) | +128.8% / +145.3% | −19.5% / −19.9% | −0.9% / −1.6% | +19.7% / +20.3% | tie / −3.4% |
| Google Maps (min) | +77.1% / +83.5% | −4.2% / −5.8% | +2.7% / +4.8% | +12.7% / +6.7% | −3.0% / −3.4% |
| Google Maps (pretty) | +69.6% / +70.8% | −8.1% / −7.7% | +2.0% / +2.7% | +14.9% / +6.9% | −3.2% / tie |
| Instruments (min) | +190.1% / +204.1% | +14.7% / +12.5% | +17.4% / +16.7% | +31.6% / +38.3% | +8.4% / +8.7% |
| Instruments (pretty) | +189.3% / +189.3% | +10.8% / +8.7% | +14.4% / +15.4% | +31.4% / +20.3% | +1.3% / +0.8% |
| Marine IK (min) | +113.2% / +120.9% | +17.0% / +17.3% | +36.9% / +38.0% | +43.9% / +39.8% | +25.5% / +23.8% |
| Marine IK (pretty) | +107.9% / +110.6% | +13.0% / +12.0% | +33.4% / +30.9% | +25.0% / +35.0% | +12.4% / +15.9% |
| Random (min) | +79.1% (n/c) / +88.6% | +2.7% / +2.5% | +6.3% / +6.0% | +18.9% / +22.0% | +190.5% / +6.9% |
| Random (pretty) | +72.0% / +86.6% | +2.2% / +3.2% | +5.6% / +4.3% | +30.8% / +25.7% | +4.1% / +2.4% |
| Twitter (min) | +167.1% / +179.4% | −10.2% / −9.2% | +7.5% / +6.6% | +14.0% / +24.0% | +1.0% / −0.8% |
| Twitter (pretty) | +157.1% / +157.7% | −9.2% / −5.6% | +4.8% / +4.7% | +18.5% / +19.1% | +3.2% (n/c) / −2.7% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.8x to 18.2x faster at full size: +80.6% (Twitter min, macOS/GCC, fresh, n/c) up to +1724.5% (Discord min, MSVC, reused), and up to +346.5% outside MSVC (Discord min, macOS/GCC, reused). The small documents show the same pattern, from +121.8% to +1581.4%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −5.7% (prettified Canada on Linux/GCC, reused) to +31.1%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Github Events is the new weak spot outside MSVC.** It accounts for 61 of the 237 single-document losses: 24 on Linux/Clang, 16 each on Linux/GCC and macOS/Clang, and 5 on macOS/GCC. Read in order it loses on both Linux builds and on macOS/Clang at both sizes, by −9.5% to −18.7% at full size and −13.9% to −34.4% small, and macOS/GCC loses all four of its small in-order results (−16.5% to −19.5%). Read in reverse at full size it wins on every build (+35.9% to +327.2%). On MSVC it wins all 32 of its results, by +33.1% to +327.2%.
- **Sparse reverse reads are a strength.** Jsonifier won 172 of 200 full-size sparse-reverse results across both allocation modes (4 ties, 24 losses), and 168 of 200 small ones (2 ties, 30 losses). MSVC and Linux/GCC each won all 40 of their full-size results, and MSVC and macOS/GCC all 40 small. Of the 24 full-size losses, 18 are on Linux/Clang, on Github Events (−9.5% to −11.7%), Google Maps (−3.7% to −6.5%), and Canada, Discord and Twitter by −2.4% or less; 4 are Google Maps on macOS/Clang (−1.6% to −8.7%) and 2 on macOS/GCC.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 29.9 MB/s on MSVC against 204.9 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 455.2 and 681.8 MB/s. Every MSVC full-size sparse result is a win, by +29.4% to +286.0%, and every full-size sparse-reverse result by +73.1% to +269.3%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 167 of 200 full-size sparse results (4 ties, 29 losses) and 152 of 200 small ones (6 ties, 42 losses). MSVC won all 80. Most losses are small: of the 29 at full size, all but 4 are under 5%, and 24 are on Linux/Clang and macOS/Clang, mostly Google Maps, Instruments and Twitter. The largest single-document loss in the suite is macOS/Clang's minified Instruments Small Sparse (reused, −68.5%), whose fresh run loses by −2.5%.
- **In-order full reads are mostly wins at full size.** Across 200 full-size in-order results, Jsonifier won 157, tied 10 and lost 33. MSVC went 37-3-0 and Linux/Clang 36-0-4, its four losses all Github Events. macOS/GCC went 31-4-5, macOS/Clang 29-3-8 and Linux/GCC 24-0-16 across the two modes.
- **Small documents read in order are the weakest category.** Across 200 results, Jsonifier won 145, tied 4 and lost 51. MSVC (38-1-1), macOS/GCC (35-0-5) and macOS/Clang (30-1-9) won most of theirs, but Linux/Clang went 24-2-14 and Linux/GCC 18-0-22. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. Outside Github Events, the Linux/Clang losses there are modest (−0.4% to −8.5%, on CitmCatalog, Marine IK, Canada and Twitter) and Linux/GCC's reach −12.3% on Canada, while the in-order small-document results on Apache Builds, Google Maps and Random on those two builds are all wins, from +8.3% to +22.6%.
- **In-order losses at full size cluster on Linux/GCC.** Linux/GCC lost Github Events, Canada, Instruments and Marine IK in both forms and both runs, by −1.6% to −12.5%. macOS/GCC lost five: Marine IK in three of its four results (−1.4% to −8.9%), prettified Discord (fresh, −6.7%, n/c) and prettified Twitter (reused, −7.4%, n/c). macOS/Clang lost Github Events in all four results (−15.1% to −18.7%), prettified Twitter in both runs (−2.0% and −2.3%), prettified Discord (fresh, −8.1%) and prettified Instruments (reused, −3.6%). Canada and Marine IK are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. MSVC lost none.
- **The POD tests almost never lose.** Jsonifier went 47-0-3 on them, and every build except macOS/Clang won all 10; macOS/Clang won 7 and lost String in both runs (−22.4% and −17.3%) and Bool fresh (−5.0%). Double, Int64 and Uint64 won all 10 results each (+20.3% to +85.9%, +24.3% to +75.3% and +27.2% to +67.4%), Bool 9 (up to +74.1%) and String 8 (+4.0% to +14.6%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 4.5% to 27.9% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 30 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of six of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: Apache Builds `jobs`, CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs from the 26 streaming tests that existed at the time are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`, before the Apache Builds streams were added.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 20 / 30 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 30 / 30 |
| Linux 7.0.0 / GCC 16.2.1 | AVX2 | 30 / 30 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2.0 | NEON | 20 / 30 |
| macOS 25.6 (M2 Pro, virtual) / Clang 23.1.4 | NEON | 14 / 30 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 12-0-0 | 12-0-0 | 30-0-0 |
| Linux / Clang | 4-0-2 | 12-0-0 | 10-0-2 | 26-0-4 |
| Linux / GCC | 5-0-1 | 11-0-1 | 10-0-2 | 26-0-4 |
| macOS / GCC | 6-0-0 | 12-0-0 | 11-0-1 | 29-0-1 |
| macOS / Clang | 3-2-1 | 9-3-0 | 11-1-0 | 23-6-1 |
| **All** | **24-2-4** | **56-3-1** | **54-1-5** | **134-6-10** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +52.2% | +19.5% | +4.7% | +29.7% | +14.5% |
| Large Amazon Cellphones (NDJSON) | +44.2% | +12.3% | +2.7% | +7.1% (n/c) | +5.3% |
| Stream Formats Small (NDJSON) | +52.3% | −7.4% | +8.9% | +16.8% | −18.7% |
| Stream Formats Small (comma) | +66.3% | +76.1% | +106.3% | +106.1% | +57.2% |
| Stream Formats Large (NDJSON) | +18.4% | −2.2% | −6.1% | +15.5% | tie |
| Stream Formats Large (comma) | +28.0% | +3.8% | +4.5% | +9.3% (n/c) | tie |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +53.7% | +17.1% | +9.3% | +59.5% (n/c) | +12.7% |
| Apache Builds (comma) | +53.0% | +28.1% | +24.8% | +64.9% | tie |
| CitmCatalog (NDJSON) | +35.7% | +5.2% | +2.2% | +12.7% | +8.2% (n/c) |
| CitmCatalog (comma) | +32.7% | +12.6% | +19.7% | +23.5% (n/c) | +60.6% (n/c) |
| Google Maps (NDJSON) | +38.8% | +36.4% | +18.3% | +19.7% (n/c) | +29.9% (n/c) |
| Google Maps (comma) | +29.2% (n/c) | +35.4% | +40.3% | +32.3% (n/c) | tie (n/c) |
| Instruments (NDJSON) | +103.6% | +21.7% | −3.6% | +18.5% | tie |
| Instruments (comma) | +78.1% (n/c) | +32.5% | +42.0% | +33.3% | +25.5% (n/c) |
| Random (NDJSON) | +63.8% | +26.0% | +30.1% | +64.5% (n/c) | +35.3% |
| Random (comma) | +50.2% | +31.2% | +53.7% | +58.9% | +42.8% (n/c) |
| Twitter (NDJSON) | +36.0% | +8.1% | +4.2% | +19.0% | +3.8% |
| Twitter (comma) | +38.2% | +15.6% | +20.5% | +28.5% | +23.4% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Apache Builds (NDJSON) | +67.4% | +6.4% | +8.3% | +35.1% | +16.2% (n/c) |
| Apache Builds (comma) | +70.2% | +23.1% | +24.8% | +42.7% | +28.2% (n/c) |
| CitmCatalog (NDJSON) | +209.1% (n/c) | +31.3% | +39.4% | +37.5% | +38.9% (n/c) |
| CitmCatalog (comma) | +228.4% (n/c) | +42.7% | +63.4% | +62.8% (n/c) | +42.3% (n/c) |
| Google Maps (NDJSON) | +60.6% (n/c) | +15.3% | +6.0% | +17.5% | +8.3% (n/c) |
| Google Maps (comma) | +51.8% (n/c) | +17.6% | +25.8% | +23.4% (n/c) | +8.2% |
| Instruments (NDJSON) | +81.5% (n/c) | −16.8% | −25.4% | −12.1% (n/c) | tie (n/c) |
| Instruments (comma) | +61.2% (n/c) | −0.9% | −4.4% | +12.0% | +29.7% (n/c) |
| Random (NDJSON) | +266.6% | +43.9% | +45.2% | +62.8% | +62.6% (n/c) |
| Random (comma) | +284.3% | +59.3% | +66.3% | +78.4% | +51.8% (n/c) |
| Twitter (NDJSON) | +801.3% (n/c) | +212.4% | +146.6% | +162.7% | +192.4% |
| Twitter (comma) | +815.7% (n/c) | +223.0% | +174.8% | +193.1% | +199.6% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 24 of the 30 results in that group, tied 2 and lost 4. It won Amazon Cellphones (+4.7% to +52.2%), Large Amazon Cellphones (+2.7% to +44.2%) and Stream Formats Small (comma) (+57.2% to +106.3%) on all five builds. The four losses are all Stream Formats in NDJSON form: Small on Linux/Clang (−7.4%) and macOS/Clang (−18.7%), and Large on Linux/Clang (−2.2%) and Linux/GCC (−6.1%). macOS/Clang ties both Large tests. Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 30-0-0; macOS/GCC and macOS/Clang lost one of 30 each, and Linux/Clang and Linux/GCC four each.
- **In-order corpus streams almost never lose.** Of the 60 in-order corpus results, Jsonifier won 56, tied 3 and lost 1. The loss is Instruments (NDJSON) on Linux/GCC (−3.6%), and the ties are all on macOS/Clang: Apache Builds (comma), Google Maps (comma) and Instruments (NDJSON).
- **Reverse order carries over from single documents to streams.** Jsonifier won 54 of 60 reverse stream results and tied 1. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +146.6% to +815.7%, while Google Maps rows, which have few, give +6.0% to +60.6%. All five losses are Instruments: NDJSON on Linux/Clang (−16.8%), Linux/GCC (−25.4%) and macOS/GCC (−12.1%, n/c), and comma-separated on Linux/Clang (−0.9%) and Linux/GCC (−4.4%). The tie is Instruments (NDJSON) on macOS/Clang.
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 116.0 MB/s under MSVC and 451.0 MB/s under Linux/Clang, while Jsonifier reads it at 445.7 and 718.4 MB/s.

## 9. Limitations

- **Linux/Clang holds the most single-document losses:** 86 of the 237, against 69 on macOS/Clang, 65 on Linux/GCC, 16 on macOS/GCC and 1 on MSVC. Linux/Clang's are spread across the sparse and small patterns, led by 18 full-size sparse reverse, 16 small sparse, 16 small sparse reverse and 14 small in-order; most are small, but Github Events accounts for 24 of them. The largest individual loss is macOS/Clang's minified Instruments Small Sparse (reused, −68.5%), whose fresh run loses by only −2.5%; both macOS builds ran on virtualized hosts, so part of their results likely reflects host noise. The largest x86 losses are Github Events Small read in order on Linux/GCC (−29.7% to −34.4%).
- **Github Events loses on every build except MSVC.** It accounts for 61 of the 237 losses, mostly in-order and small-document reads (§7.11), although its full-size reverse reads win everywhere. It is the main new target.
- **Linux/GCC loses mostly on reads in document order.** 38 of its 65 losses are in-order reads at either size (§7.11), on Github Events and on the float-heavy Canada, Instruments and Marine IK, where much of the cost is number parsing rather than field lookup.
- **Small documents lose on Linux.** At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Instruments streams read in reverse** lose on three builds: in NDJSON form on Linux/Clang (−16.8%), Linux/GCC (−25.4%) and macOS/GCC (−12.1%), and in comma form on Linux/Clang (−0.9%) and Linux/GCC (−4.4%), although the single-document Instruments Reverse tests win on every build at both sizes.
- **Some results did not converge.** 56 of the 1,650 single-document results did not converge: 30 on macOS/GCC, 19 on Windows/MSVC and 7 on macOS/Clang. 36 of the 150 streaming results did not converge: 16 on macOS/Clang, 10 on macOS/GCC and 10 on Windows/MSVC. Both Linux builds converged on every result.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
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

Across 1,650 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,383, tied 30 and lost 237. On reverse reads it won 375 of 400 at both sizes, and on Windows/MSVC it lost 1 of its 330 single-document results. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (686-17-122 fresh, 697-13-115 reused). The main remaining targets are Github Events, with 61 of the 237 losses, Linux/Clang's sparse and small-document reads, and small documents on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 150 streaming results, Jsonifier won 134, tied 6 and lost 10, and it won every streaming test on Windows/MSVC. Reverse Instruments streams and the NDJSON Stream Formats tests are the main remaining streaming targets.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.