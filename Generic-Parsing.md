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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,250 of 1,490 single-document results against simdjson On Demand, tied 67 and lost 173. On reverse-order reads it won 344 of 360. On Windows/MSVC it won 294 of 298 single-document results, tied the other 4 and lost none.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests, Jsonifier won 113 of 130 results, tied 7 and lost 10, and won every streaming test on Windows/MSVC.

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

Jsonifier compiles exactly one instruction-set tier per build, chosen when CMake configures the project, either by running a detector on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS`. Every call is then a direct, inlinable call into code built for that ISA, with no CPUID read and no per-call check. The reason is inlining: per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, so GCC and Clang reject multi-tier builds of that code. The cost is that a binary for mixed hardware is configured for the lowest tier it must support. simdjson instead dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time.

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
- **Tests:** 5 POD tests, plus 9 documents each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 154 single-document results per build at full size and another 144 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document and reused tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. 114 of the 1,490 single-document results and 40 of the 130 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple M1 hosts and used RSE < 10% and shift < 5%.
- **Integer parsing:** on every build except Windows/MSVC, Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time. The MSVC build uses an unrolled, bounds-checked digit parser instead.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 148 / 154 | 143 / 144 | 291 / 298 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 150 / 154 | 144 / 144 | 294 / 298 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 120 / 154 | 132 / 144 | 252 / 298 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 109 / 154 | 132 / 144 | 241 / 298 |

All builds ran BenchmarkSuite `ab75024` and Jsonifier `b975cd7`; simdjson is `70f83c9` on the two Linux builds, `957ddbc` on Windows/MSVC and macOS/Clang, and `2cc1e70` on macOS/GCC. Each build compiles one stage-1 tier, chosen at configure time (§3.5), and the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on October 23, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 147-2-0 |
| Linux / Clang | 5-0-0 | 18-0-0 | 18-0-0 | 16-0-2 | 17-0-1 | 11-1-6 | 18-0-0 | 10-1-7 | 17-0-1 | 130-2-17 |
| Linux / GCC | 5-0-0 | 8-1-9 | 16-0-2 | 15-0-3 | 18-0-0 | 6-1-11 | 16-0-2 | 11-2-5 | 18-0-0 | 113-4-32 |
| macOS / GCC | 5-0-0 | 10-3-5 | 18-0-0 | 15-1-2 | 15-1-2 | 16-0-2 | 16-0-2 | 16-1-1 | 17-1-0 | 128-7-14 |
| macOS / Clang | 1-4-0 | 14-4-0 | 15-1-2 | 8-6-4 | 12-1-5 | 11-2-5 | 18-0-0 | 8-1-9 | 11-1-6 | 98-20-31 |
| **All** | **21-4-0** | **66-10-14** | **85-1-4** | **72-7-11** | **80-2-8** | **62-4-24** | **86-0-4** | **63-5-22** | **81-2-7** | **616-35-94** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 16-2-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 18-0-0 | 147-2-0 |
| Linux / Clang | 5-0-0 | 18-0-0 | 18-0-0 | 16-0-2 | 17-0-1 | 12-0-6 | 18-0-0 | 9-2-7 | 17-0-1 | 130-2-17 |
| Linux / GCC | 5-0-0 | 9-2-7 | 16-0-2 | 16-0-2 | 18-0-0 | 8-0-10 | 16-0-2 | 13-1-4 | 18-0-0 | 119-3-27 |
| macOS / GCC | 5-0-0 | 11-2-5 | 17-1-0 | 15-1-2 | 14-2-2 | 15-0-3 | 18-0-0 | 17-1-0 | 17-1-0 | 129-8-12 |
| macOS / Clang | 2-3-0 | 16-0-2 | 16-1-1 | 10-3-5 | 10-2-6 | 16-1-1 | 18-0-0 | 10-3-5 | 11-4-3 | 109-17-23 |
| **All** | **22-3-0** | **70-6-14** | **85-2-3** | **75-4-11** | **77-4-9** | **69-1-20** | **88-0-2** | **67-7-16** | **81-5-4** | **634-32-79** |


Each cell is wins-ties-losses.

Across all 1,490 single-document results (114 of them did not converge; see §9), Jsonifier won 1,250, tied 67 and lost 173: 616-35-94 with fresh output objects and 634-32-79 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 170 of 180 full-size reverse results (3 ties, 7 losses) and 174 of 180 small reverse results (no ties, 6 losses). Ten of the 13 losses are on Canada: Linux/GCC lost Canada in both forms and both runs at full size (−4.0% to −4.9%) and Canada Small likewise (−4.2% to −7.2%), and macOS/Clang lost minified Canada (−57.8% fresh, −40.3% reused (n/c)). The other three are minified Random on macOS/Clang (fresh, −20.8% (n/c)) and CitmCatalog Small on macOS/GCC (fresh, −7.4% minified and −6.8% (n/c) prettified). All 3 ties are prettified Canada: on macOS/Clang in both runs and on macOS/GCC reused.
- **Windows/MSVC lost none of its 298 single-document results.** Its 4 ties are all full-size Canada read in order, minified and prettified, fresh and reused. It won all 10 POD results and every other test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6.3–10.1% for Jsonifier and 4.6–9.6% for simdjson, depending on the build. Jsonifier's margin over simdjson moves by a median +0.6 to +2.4 percentage points on the non-MSVC builds and by +9.2 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +1546.2% to +1689.3%, and Mesh Small Sparse (min) from +103.4% to +153.2%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +54.3% / +67.3% | +35.6% / +35.8% | +34.9% / +29.1% | +2.7% / +36.4% | tie (n/c) / tie (n/c) |
| Double | +27.1% / +32.3% | +54.1% / +54.2% | +50.3% / +46.7% | +64.0% / +166.5% | +71.3% (n/c) / +77.5% |
| Int64 | +54.7% / +58.9% | +67.8% / +68.6% | +32.1% / +32.1% | +31.0% / +23.7% | tie / +55.5% |
| String | +17.7% / +21.3% | +4.2% / +5.7% | +5.3% / +5.2% | +3.7% / +14.4% | tie (n/c) / tie (n/c) |
| Uint64 | +55.4% / +61.1% | +61.6% / +58.8% | +48.8% / +51.2% | +85.8% / +39.8% | tie / tie |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | tie / tie | +5.6% / +6.0% | −6.4% / −8.3% | +8.5% (n/c) / +4.9% | tie (n/c) / +74.1% (n/c) |
| Canada (pretty) | tie / tie | +5.0% / +5.4% | −6.6% / tie | tie (n/c) / −10.8% (n/c) | +20.4% (n/c) / +21.5% (n/c) |
| CitmCatalog (min) | +33.9% / +40.5% | +5.7% / +6.7% | +7.6% / +8.0% | tie (n/c) / tie (n/c) | +140.1% (n/c) / +110.4% (n/c) |
| CitmCatalog (pretty) | +33.9% / +35.2% | +5.1% / +5.5% | +5.2% / +6.1% | +16.5% / −8.0% (n/c) | tie / +102.4% |
| Discord (min) | +57.0% / +71.4% | +14.8% / +23.6% | −2.3% / tie | +19.1% / +30.4% | +10.9% / +11.5% |
| Discord (pretty) | +46.3% / +64.5% | +15.2% / +16.8% | tie / +40.9% | +23.6% / +32.3% | +22.1% (n/c) / +8.0% |
| Google Maps (min) | +35.8% / +38.5% | +19.6% / +20.3% | +13.5% / +13.9% | +17.8% / +31.9% | +6.3% / +6.8% |
| Google Maps (pretty) | +30.1% / +39.1% | +16.9% / +18.6% | +13.0% / +13.3% | +26.1% / +30.2% | +2.6% / +7.7% |
| Instruments (min) | +47.8% / +73.3% | +7.3% / +8.6% | −2.1% / −2.2% | +85.6% / +26.2% | tie / +5.7% |
| Instruments (pretty) | +55.1% / +69.5% | +4.5% / +4.8% | −2.6% / −4.0% | −31.8% (n/c) / +28.6% | +1.3% / −8.6% |
| Marine IK (min) | +15.4% / +14.0% | +4.4% / +3.2% | −4.5% / −4.8% | tie / tie | +12.9% / +8.1% |
| Marine IK (pretty) | +18.5% / +18.2% | +1.6% / +4.1% | −2.9% / −3.3% | −10.2% (n/c) / −25.7% (n/c) | +7.1% (n/c) / +9.4% |
| Mesh (min) | +51.3% / +61.5% | +1.3% / +1.3% | −2.6% / −2.3% | −7.8% / −13.3% | +9.3% / +11.0% |
| Mesh (pretty) | +50.9% / +56.2% | +1.4% / +1.5% | −5.1% / −6.1% | −4.5% (n/c) / −12.9% | +2.6% / −2.1% |
| Random (min) | +49.8% / +63.6% | +25.3% / +32.5% | +25.5% / +33.3% | +46.8% / +56.3% | +58.5% / +27.9% |
| Random (pretty) | +47.6% / +62.1% | +26.3% / +29.8% | +24.6% / +28.3% | +53.7% / +65.2% (n/c) | +14.6% (n/c) / +32.4% |
| Twitter (min) | +37.0% / +49.4% | +10.9% / +22.9% | +3.7% / +5.6% | −18.2% (n/c) / +25.3% | tie / +2.7% |
| Twitter (pretty) | +38.6% / +47.2% | +4.3% / +18.6% | +2.9% / +8.7% | +21.5% (n/c) / +24.3% | +4.2% / +6.4% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +17.5% (n/c) / +15.9% | +6.3% / +6.3% | −4.0% / −4.7% | +37.2% (n/c) / +12.5% | −57.8% / −40.3% (n/c) |
| Canada (pretty) | +16.9% / +17.2% | +3.0% / +5.1% | −4.1% / −4.9% | +6.7% / tie | tie (n/c) / tie (n/c) |
| CitmCatalog (min) | +294.5% / +304.5% | +65.9% / +71.5% | +78.0% / +98.3% (n/c) | +116.4% / +86.2% (n/c) | +103.3% (n/c) / +53.9% (n/c) |
| CitmCatalog (pretty) | +289.3% / +318.6% | +63.9% / +66.2% | +55.1% (n/c) / +76.0% | +38.7% / +62.0% | +157.7% (n/c) / +120.9% |
| Discord (min) | +1546.2% / +1689.3% | +241.6% / +264.2% | +299.9% / +335.2% | +350.9% / +353.3% | +262.5% / +297.3% |
| Discord (pretty) | +1459.3% / +1672.4% | +234.6% / +255.9% | +327.6% / +301.4% | +329.7% / +352.5% | +264.2% / +261.6% |
| Google Maps (min) | +96.4% / +111.3% | +5.4% / +6.0% | +20.2% / +20.3% | +11.1% / +24.2% | +0.7% / +8.9% |
| Google Maps (pretty) | +103.1% / +122.5% | +3.5% / +4.3% | +17.7% / +18.9% | +18.6% / +23.2% | +11.0% / +2.7% |
| Instruments (min) | +669.7% / +720.4% | +124.9% / +129.4% | +136.3% / +140.0% | +200.7% / +143.7% | +131.2% / +116.9% |
| Instruments (pretty) | +690.8% / +699.0% | +113.6% / +118.6% | +125.1% / +128.4% | +167.6% / +134.4% | +117.4% / +114.8% |
| Marine IK (min) | +331.8% (n/c) / +333.1% (n/c) | +37.1% / +38.0% | +67.3% / +72.3% | +99.4% (n/c) / +55.6% (n/c) | +48.3% (n/c) / +39.5% |
| Marine IK (pretty) | +307.6% (n/c) / +309.1% (n/c) | +30.1% / +33.2% | +49.1% / +52.0% | +38.0% (n/c) / +35.9% (n/c) | +31.4% / +46.8% (n/c) |
| Mesh (min) | +368.4% / +409.5% | +31.9% / +37.4% | +49.2% / +51.7% | +55.7% / +47.9% | +42.8% / +52.8% |
| Mesh (pretty) | +349.6% / +394.8% | +31.4% / +34.2% | +42.4% / +45.3% | +45.7% (n/c) / +50.7% | +65.6% / +55.8% |
| Random (min) | +235.5% / +270.7% | +6.8% / +7.2% | +23.4% / +27.0% | +47.3% / +40.1% (n/c) | −20.8% (n/c) / +18.5% |
| Random (pretty) | +238.3% / +252.6% | +9.3% / +8.2% | +23.4% / +24.1% | +46.2% / +47.0% (n/c) | +33.7% (n/c) / +24.3% |
| Twitter (min) | +698.9% / +792.3% | +195.4% / +210.4% | +131.6% / +139.8% | +281.5% / +269.2% (n/c) | +154.8% / +176.5% (n/c) |
| Twitter (pretty) | +696.5% / +775.5% | +178.7% / +188.8% | +120.9% / +128.2% | +120.1% / +92.7% (n/c) | +130.4% / +149.4% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +61.3% / +59.0% | +4.3% / +4.6% | +15.9% / +16.9% | +26.7% / +28.6% | tie (n/c) / +20.1% |
| Canada (pretty) | +52.3% / +47.5% | +14.9% / +13.8% | +9.8% / +10.0% | +25.1% / +9.2% | tie / tie (n/c) |
| CitmCatalog (min) | +234.9% (n/c) / +244.1% | +28.7% / +28.5% | +36.5% / +37.1% | +27.2% (n/c) / tie | +16.1% / tie |
| CitmCatalog (pretty) | +195.4% / +189.7% | +23.7% / +24.9% | +15.9% / +9.5% | +14.4% / +14.6% | +23.3% (n/c) / tie (n/c) |
| Discord (min) | +24.2% / +39.8% | −6.0% / −5.9% | −34.2% / +8.9% | +19.8% / +23.5% | tie / +7.2% |
| Discord (pretty) | +28.1% / +42.8% | −5.9% / −6.0% | +7.7% / +7.8% | +19.0% / +19.6% | −15.9% (n/c) / +17.8% (n/c) |
| Google Maps (min) | +61.1% / +66.0% | +3.2% / +1.6% | −4.1% / −3.8% | tie / +18.4% | tie / +0.7% |
| Google Maps (pretty) | +52.1% / +61.9% | +3.3% / +3.9% | −0.8% / −1.2% | +14.6% / +20.8% | +2.2% / −9.8% |
| Instruments (min) | +32.9% / +40.1% | +2.9% / +3.2% | +4.1% / +4.9% | +6.1% / +36.1% | −3.5% / −3.6% |
| Instruments (pretty) | +42.4% / +45.4% | +3.9% / +4.1% | +2.0% / +2.0% | +12.4% / +8.4% (n/c) | −2.8% / −6.6% |
| Marine IK (min) | +43.3% / +43.9% | +26.4% / +24.9% | +29.7% / +28.6% | +16.2% / +10.9% | +23.9% / +28.7% |
| Marine IK (pretty) | +44.8% / +44.0% | +16.6% / +20.9% | +16.6% / +15.0% | +34.7% / +46.6% | +16.1% / +19.1% |
| Mesh (min) | +288.8% / +307.3% | +8.8% / +10.4% | +20.0% / +19.0% | −0.9% / −8.5% | tie / +14.6% |
| Mesh (pretty) | +248.1% / +259.3% | +7.7% / +8.7% | +12.0% / +12.8% | −4.6% / −2.1% | +14.9% / +32.8% (n/c) |
| Random (min) | +54.5% / +60.4% | +13.0% / +12.5% | +3.2% / +2.9% | +31.2% / +49.5% | +4.3% (n/c) / +6.5% |
| Random (pretty) | +54.9% / +67.9% | +13.3% / +14.2% | +2.7% / +2.7% | +35.8% (n/c) / +15.2% | +23.0% / +7.3% |
| Twitter (min) | +136.6% / +145.3% | +5.8% / +6.3% | +8.1% / +8.2% | +17.5% / +12.2% | −6.6% / −6.2% |
| Twitter (pretty) | +115.6% / +125.7% | +4.1% / +3.6% | +4.3% / +4.9% | +27.4% / +24.3% | tie / −4.6% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +265.4% / +263.7% | −1.8% / −0.9% | +24.3% / +23.5% | tie / +61.2% (n/c) | +9.7% (n/c) / +22.0% (n/c) |
| Canada (pretty) | +159.6% / +166.9% | +8.8% / +9.2% | +8.5% / +9.2% | +11.9% / tie (n/c) | tie (n/c) / −11.6% |
| CitmCatalog (min) | +261.8% / +235.9% | +21.1% / +21.2% | +39.5% / +99.4% | +7.6% (n/c) / tie | +48.9% (n/c) / +31.3% (n/c) |
| CitmCatalog (pretty) | +208.9% / +200.9% | +18.4% / +17.8% | +35.5% (n/c) / +42.3% (n/c) | +16.4% / +20.7% | −48.2% (n/c) / −61.3% (n/c) |
| Discord (min) | +119.4% / +154.5% | +4.6% / +5.1% | +8.5% / +9.8% | +26.7% / +24.1% | +51.6% / +9.3% |
| Discord (pretty) | +114.7% / +156.9% | +4.0% / +5.0% | +6.0% / +8.2% | +31.7% / +22.9% | +6.1% / tie (n/c) |
| Google Maps (min) | +74.7% / +95.6% | +4.3% / +4.8% | +11.2% / +10.1% | +18.5% / +13.8% | −6.0% / −3.0% |
| Google Maps (pretty) | +84.0% / +85.5% | +2.2% / +5.5% | +8.8% / +9.1% | +20.9% / +11.3% (n/c) | −3.6% / −8.4% |
| Instruments (min) | +177.9% / +190.2% | +14.5% / +13.9% | +20.0% / +21.3% | +35.3% / +50.0% | +5.7% / +2.7% |
| Instruments (pretty) | +184.7% / +191.7% | +13.4% / +14.6% | +17.4% / +16.3% | +21.5% / +22.0% (n/c) | −1.6% / +4.0% |
| Marine IK (min) | +209.4% / +212.5% | +17.8% / +18.2% | +61.0% / +61.5% | +13.0% / +44.2% | +42.4% / +29.9% |
| Marine IK (pretty) | +171.1% / +174.1% | +21.8% / +15.5% | +29.7% / +30.8% | +34.6% / +48.6% | +37.6% (n/c) / +21.9% |
| Mesh (min) | +278.4% / +310.3% | +5.4% / +8.5% | +19.9% / +20.0% | −9.8% / −13.1% | +28.4% / +19.9% |
| Mesh (pretty) | +240.6% / +258.1% | +6.1% / +6.2% | +12.3% / +12.2% | −7.3% / −5.6% | +25.6% (n/c) / −11.3% (n/c) |
| Random (min) | +77.4% / +84.7% | +11.3% / +12.7% | +1.9% / +1.7% | +25.5% / +35.5% (n/c) | +2.7% / +12.8% |
| Random (pretty) | +77.0% / +86.5% | +9.5% / +10.2% | +1.5% / +2.5% | +25.4% / +41.4% | +22.8% / tie |
| Twitter (min) | +168.2% / +175.5% | +13.1% / +13.3% | +14.8% / +14.7% | +27.7% / +33.1% | +1.2% / +9.3% |
| Twitter (pretty) | +161.6% / +160.6% | +12.2% / +11.7% | +13.1% / +12.3% | +36.2% / +53.7% (n/c) | −18.9% (n/c) / −0.8% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +2.3% / +2.4% | +7.7% / +8.3% | −12.6% / −15.1% | +4.9% / +34.7% | +7.6% / +12.6% |
| Canada (pretty) | +4.4% / +3.0% | +6.7% / +6.2% | −12.8% / −15.5% | −5.8% / −8.8% | +5.5% / +1.5% |
| CitmCatalog (min) | +19.8% / +28.8% | −7.3% / −5.3% | −5.2% / −4.7% | +21.3% / +10.7% | +4.9% / tie |
| CitmCatalog (pretty) | +22.5% / +27.6% | −9.3% / −7.6% | −7.5% / −4.7% | +40.3% / +31.4% | −2.9% / +1.5% |
| Discord (min) | +29.6% / +70.9% | +3.5% / +7.6% | tie / +3.5% | +30.9% / +28.1% | +3.1% / +19.7% |
| Discord (pretty) | +27.6% / +62.6% | +3.4% / +8.7% | −0.8% / +3.1% | +33.4% / +37.5% | +2.7% / +14.3% |
| Google Maps (min) | +29.6% / +39.0% | +13.1% / +14.8% | +12.5% / +14.4% | +57.6% / +38.9% | +10.4% / +4.7% (n/c) |
| Google Maps (pretty) | +30.4% / +37.5% | +12.7% / +13.9% | +10.5% / +11.6% | +30.0% / +33.4% | +3.4% / +3.9% |
| Instruments (min) | +60.9% / +63.5% | −2.7% / −1.9% | −4.9% / −5.0% | +18.0% / +11.4% | +23.8% / +14.8% |
| Instruments (pretty) | +52.9% / +61.2% | −1.6% / −3.3% | −8.0% / −7.7% | +4.4% / +11.9% | tie / +3.3% |
| Marine IK (min) | +6.4% / +6.6% | +1.7% / +2.0% | −3.3% / −5.0% | +5.4% / +7.0% | −15.3% / +9.6% |
| Marine IK (pretty) | +7.4% / +7.2% | tie / +1.1% | −4.6% / −4.9% | −6.7% / +16.3% | tie / +3.4% |
| Mesh (min) | +10.7% / +39.3% | −4.9% / −5.3% | −9.2% / −10.3% | +38.2% / −24.1% | +15.7% (n/c) / +20.4% |
| Mesh (pretty) | +14.9% / +33.9% | −4.7% / −8.1% | −11.8% / −13.3% | +8.7% (n/c) / −7.9% | −4.3% (n/c) / −8.2% (n/c) |
| Random (min) | +44.6% / +53.0% | +11.3% / +15.2% | +19.0% / +24.8% | +45.5% / +47.3% | +18.3% / +17.3% |
| Random (pretty) | +42.2% / +50.3% | +11.3% / +14.4% | +16.8% / +21.1% | +43.5% / +38.2% | +17.5% / +36.3% |
| Twitter (min) | +37.7% / +49.6% | +5.2% / +7.1% | +2.8% / +6.0% | +11.8% / +29.8% | −7.4% (n/c) / +11.7% |
| Twitter (pretty) | +44.6% / +50.8% | +2.1% / +3.6% | +2.6% / +2.9% | +25.3% / +31.4% | −5.2% / +10.8% (n/c) |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +17.5% / +19.1% | +8.3% / +9.8% | −4.2% / −7.2% | +112.4% / +11.0% (n/c) | +1.2% / +14.1% |
| Canada (pretty) | +17.2% / +18.4% | +7.3% / +8.8% | −6.6% / −6.3% | +19.0% / +3.7% | +8.3% / +7.4% |
| CitmCatalog (min) | +205.6% / +224.3% | +44.5% / +49.6% | +68.5% / +73.3% | −7.4% / +55.1% | +48.9% / +72.3% |
| CitmCatalog (pretty) | +203.0% / +224.4% | +42.9% / +46.9% | +65.4% / +69.7% | −6.8% (n/c) / +100.8% | +54.6% / +59.4% |
| Discord (min) | +1228.6% / +1481.0% | +245.3% / +275.5% | +278.1% / +310.3% | +292.2% / +373.6% | +261.1% / +293.9% |
| Discord (pretty) | +1200.1% / +1509.0% | +234.7% / +263.7% | +266.7% / +298.9% | +350.1% / +422.9% | +265.4% / +286.2% |
| Google Maps (min) | +102.3% / +115.7% | +5.1% / +5.9% | +16.6% / +16.8% | +18.8% / +35.3% | +3.4% / +5.7% |
| Google Maps (pretty) | +94.2% / +114.8% | +3.7% / +4.0% | +16.9% / +17.8% | +26.7% / +16.8% | +28.8% (n/c) / +6.5% |
| Instruments (min) | +683.3% / +691.6% | +129.7% / +134.5% | +135.7% / +139.0% | +231.6% / +134.7% | +141.9% / +122.5% |
| Instruments (pretty) | +666.1% / +686.2% | +123.8% / +126.3% | +128.5% / +131.0% | +213.4% / +150.9% | +111.7% / +126.9% |
| Marine IK (min) | +188.8% / +194.1% | +31.6% / +34.9% | +52.6% / +54.8% | +64.5% / +58.1% | +18.7% / +33.8% |
| Marine IK (pretty) | +182.7% / +191.9% | +30.6% / +32.5% | +50.6% / +51.4% | +44.2% / +24.1% | +28.8% / +14.2% |
| Mesh (min) | +126.5% / +193.9% | +24.6% / +28.6% | +26.7% / +32.9% | +24.6% / +47.8% | +81.4% / +31.7% |
| Mesh (pretty) | +130.9% / +188.9% | +22.1% / +24.7% | +22.0% / +29.0% | +7.9% / +36.5% | +27.0% / +50.4% (n/c) |
| Random (min) | +257.2% / +293.8% | +23.7% / +24.6% | +43.8% / +47.0% | +89.7% / +59.5% | +53.6% / +70.1% |
| Random (pretty) | +248.6% / +289.9% | +21.8% / +23.3% | +39.6% / +44.0% | +77.6% / +73.4% | +50.2% (n/c) / +84.8% |
| Twitter (min) | +725.9% / +776.3% | +159.3% / +164.3% | +146.4% / +151.4% | +203.2% / +187.7% | +194.0% / +157.2% |
| Twitter (pretty) | +716.8% / +752.4% | +150.2% / +154.4% | +136.4% / +138.7% | +198.6% / +186.6% | +161.6% / +195.4% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +31.6% / +30.5% | +21.2% / +20.7% | +15.0% / +14.7% | +27.0% (n/c) / +34.6% | −16.4% / +7.3% |
| Canada (pretty) | +36.4% / +36.1% | +19.6% / +20.5% | +14.7% / +13.8% | +39.1% / +16.1% | −10.4% / tie |
| CitmCatalog (min) | +153.0% / +170.7% | +11.9% / +13.0% | +13.0% / +15.9% | +16.1% / +6.4% | +17.1% / +15.6% |
| CitmCatalog (pretty) | +136.0% / +150.7% | +8.3% / +8.7% | +10.9% / +13.0% | +16.6% / +30.3% | +6.8% / +15.3% |
| Discord (min) | +28.4% / +44.6% | −0.6% / tie | tie / +2.1% | +111.6% (n/c) / +24.9% (n/c) | −5.1% / +1.5% |
| Discord (pretty) | +26.3% / +40.9% | −1.1% / −3.9% | −2.1% / −0.8% | +10.5% / +37.7% (n/c) | −4.1% / −8.9% |
| Google Maps (min) | +63.5% / +68.1% | +0.4% / −0.9% | −3.4% / −3.4% | +10.0% / +24.1% | −4.7% / tie |
| Google Maps (pretty) | +55.9% / +61.2% | −0.8% / −1.8% | −2.0% / −2.0% | +12.7% / +9.9% | +5.6% / −1.5% |
| Instruments (min) | +32.4% / +41.2% | −5.8% / −3.6% | tie / tie | +11.4% / +11.7% | −2.5% / −4.3% |
| Instruments (pretty) | +31.5% / +39.5% | −4.3% / −3.8% | −0.4% / +0.2% | −20.8% / +10.4% (n/c) | −8.5% / −4.9% |
| Marine IK (min) | +29.0% / +12.3% | +13.8% / +14.4% | +18.8% / +19.1% | +9.3% / +22.7% | +15.1% / +25.9% |
| Marine IK (pretty) | +18.6% / +28.9% | +12.3% / +11.8% | +17.3% / +18.1% | tie / +24.5% | +16.7% / +21.4% |
| Mesh (min) | +103.4% / +153.2% | +6.3% / +6.4% | +15.4% / +18.1% | +7.9% / tie | tie / +17.2% |
| Mesh (pretty) | +88.4% / +133.7% | +4.4% / +5.8% | +10.9% / +13.1% | +4.2% / +16.2% | −7.7% / tie |
| Random (min) | +42.9% / +60.2% | +1.2% / tie | +4.0% / +4.0% | +18.9% / +32.4% | +3.3% / +15.5% |
| Random (pretty) | +41.8% / +47.5% | tie / +0.8% | +3.8% / +3.7% | +19.4% (n/c) / +20.0% | +4.8% / +11.5% |
| Twitter (min) | +101.7% / +114.4% | −12.0% / −11.4% | +1.5% / +1.3% | +15.1% / +29.4% | +3.1% / +2.3% |
| Twitter (pretty) | +91.2% / +94.0% | −13.1% / −13.2% | −2.0% / −2.9% | +15.2% / +25.4% (n/c) | −5.4% / −11.1% (n/c) |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +146.5% / +161.4% | +23.3% / +25.1% | +18.7% / +20.8% | +35.8% / +33.9% | −9.2% / −10.3% |
| Canada (pretty) | +126.2% / +136.7% | +24.9% / +21.6% | +17.0% / +17.7% | +31.8% / +29.8% | −7.6% / −6.3% |
| CitmCatalog (min) | +146.8% / +158.3% | +21.1% / +17.1% | +23.8% / +26.3% | +16.0% / +35.8% | +7.7% / +6.5% |
| CitmCatalog (pretty) | +145.7% / +147.3% | +13.4% / +14.1% | +22.7% / +23.4% | +8.4% / +26.6% | +13.1% / +9.3% |
| Discord (min) | +144.3% / +174.4% | +1.6% / +3.4% | +4.8% / +6.0% | +12.6% / +34.8% | +7.7% / +3.9% |
| Discord (pretty) | +140.4% / +168.9% | +1.3% / +1.1% | +2.1% / +5.0% | +15.1% / +10.6% | −5.2% / tie |
| Google Maps (min) | +78.4% / +92.6% | +0.6% / +1.8% | +6.9% / +5.7% | +59.9% (n/c) / tie | −7.4% / tie |
| Google Maps (pretty) | +71.4% / +75.3% | +1.2% / +2.0% | +6.3% / +3.5% | +11.3% / +14.5% | −8.3% / −4.8% |
| Instruments (min) | +210.2% / +197.2% | +8.6% / +9.7% | +18.7% / +20.2% | +29.6% / +29.0% | +1.7% / +12.0% |
| Instruments (pretty) | +190.4% / +192.8% | +8.7% / +9.5% | +16.1% / +17.1% | +38.8% / +20.6% | +3.6% / +13.0% |
| Marine IK (min) | +110.6% / +97.5% | +14.0% / +15.2% | +33.8% / +35.3% | +39.0% / +30.2% | +35.0% / +10.5% |
| Marine IK (pretty) | +104.5% / +90.9% | +13.2% / +13.2% | +31.1% / +31.9% | +30.7% / +41.2% | +20.7% / +46.1% (n/c) |
| Mesh (min) | +132.3% / +179.7% | +7.7% / +8.1% | +15.6% / +13.8% | tie / +6.5% | +8.0% / +1.5% |
| Mesh (pretty) | +97.8% (n/c) / +150.6% | +3.0% / +3.5% | +9.1% / +9.8% | +7.9% / +5.6% | +5.8% / tie |
| Random (min) | +78.3% / +74.8% | +3.8% / +3.0% | +9.3% / +8.7% | +24.7% / +13.8% | +186.8% / +8.6% |
| Random (pretty) | +62.6% / +71.9% | +5.9% / +3.7% | +8.3% / +7.0% | +22.5% / +32.3% | tie / tie |
| Twitter (min) | +148.3% / +145.5% | +0.7% / +1.6% | +8.0% / +8.5% | +19.8% / +24.4% | +170.3% (n/c) / +18.1% |
| Twitter (pretty) | +133.5% / +146.6% | −0.5% / −0.9% | +5.0% / +6.1% | +26.1% / +10.2% (n/c) | −5.1% / +1.8% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 17.9x faster at full size: +92.7% (Twitter pretty, macOS/GCC, reused, n/c) up to +1689.3% (Discord min, MSVC, reused), and up to +353.3% outside MSVC (Discord min, macOS/GCC, reused). The small documents show the same pattern, from +111.7% to +1509.0%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −57.8% (minified Canada on macOS/Clang, fresh) to +37.2%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are a strength.** Jsonifier won 157 of 180 full-size sparse-reverse results across both allocation modes (6 ties, 17 losses), and 162 of 180 small ones (7 ties, 11 losses). MSVC and Linux/GCC each won all 72 of theirs. Of the 17 full-size losses, 11 are on macOS/Clang, 4 on macOS/GCC (Mesh in both forms and both runs, −5.6% to −13.1%) and 2 on Linux/Clang (minified Canada, −1.8% fresh and −0.9% reused); 4 of macOS/Clang's 11 are Google Maps, and its largest are prettified CitmCatalog (−48.2% fresh and −61.3% reused, both n/c).
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 32.7 MB/s on MSVC against 234.3 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 509.2 and 784.0 MB/s. Every MSVC full-size sparse result is a win, by +24.2% to +307.3%, and every full-size sparse-reverse result by +74.7% to +310.3%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 147 of 180 full-size sparse results (11 ties, 22 losses) and 130 of 180 small ones (12 ties, 38 losses). MSVC won all 72.
- **In-order full reads are mostly wins at full size.** Across 180 full-size in-order results, Jsonifier won 136, tied 16 and lost 28. Linux/Clang went 36-0-0 and MSVC 32-4-0. macOS/Clang went 30-4-2, macOS/GCC 21-5-10 and Linux/GCC 17-3-16 across the two modes.
- **Small documents read in order are the weakest in-order category.** Across 180 results, Jsonifier won 131, tied 5 and lost 44. MSVC (36-0-0), macOS/GCC (31-0-5) and macOS/Clang (27-3-6) won most of theirs, but Linux/Clang went 23-1-12 and Linux/GCC 14-1-21. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The Linux/Clang losses there are modest (−1.6% to −9.3%, on CitmCatalog, Instruments and Mesh) and Linux/GCC's reach −15.5% on Canada, while the in-order small-document results on Google Maps and Random on those two builds are all wins, from +10.5% to +24.8%.
- **In-order losses at full size cluster on Linux/GCC and macOS/GCC.** Linux/GCC lost Instruments, Marine IK and Mesh in both forms and both runs, Canada in three of its four results (prettified reused is a tie) and minified Discord (fresh, −2.3%), all by −2.1% to −8.3%. macOS/GCC lost ten: Mesh in both forms and both runs (−4.5% to −13.3%), prettified Marine IK (−10.2% fresh and −25.7% reused), prettified Instruments (fresh, −31.8%), minified Twitter (fresh, −18.2%), prettified Canada (reused, −10.8%) and prettified CitmCatalog (reused, −8.0%); seven of those ten did not converge. macOS/Clang lost two, both prettified and reused: Instruments (−8.6%) and Mesh (−2.1%). Canada, Marine IK and Mesh are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. Linux/Clang and MSVC lost none.
- **The POD tests never lose.** Jsonifier went 43-7-0 on them, and every build except macOS/Clang won all 10; macOS/Clang won 3 and tied 7. Double won all 10 results (+27.1% to +166.5%), Int64 9 (+23.7% to +68.6%), and Bool (+2.7% to +67.3%), String (+3.7% to +21.3%) and Uint64 (+39.8% to +85.8%) 8 each, with the rest tied on macOS/Clang.

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 6.3% to 23.6% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 20 / 26 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 26 / 26 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 26 / 26 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 15 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 3 / 26 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 10-0-0 | 26-0-0 |
| Linux / Clang | 5-0-1 | 10-0-0 | 9-0-1 | 24-0-2 |
| Linux / GCC | 5-0-1 | 10-0-0 | 8-0-2 | 23-0-3 |
| macOS / GCC | 6-0-0 | 10-0-0 | 8-1-1 | 24-1-1 |
| macOS / Clang | 2-1-3 | 7-3-0 | 7-2-1 | 16-6-4 |
| **All** | **24-1-5** | **47-3-0** | **42-3-5** | **113-7-10** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +50.4% | +22.4% | +4.6% | +35.4% | +13.4% |
| Large Amazon Cellphones (NDJSON) | +43.3% | +13.2% | +4.1% | +20.1% | tie (n/c) |
| Stream Formats Small (NDJSON) | +44.9% (n/c) | −5.4% | +2.9% | +13.9% (n/c) | −17.4% (n/c) |
| Stream Formats Small (comma) | +68.9% | +69.5% | +98.8% | +91.4% (n/c) | +46.7% (n/c) |
| Stream Formats Large (NDJSON) | +20.0% | +1.8% | −4.4% | +15.2% | −20.5% (n/c) |
| Stream Formats Large (comma) | +25.2% | +8.6% | +8.1% | +14.1% | −7.6% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +34.2% | +6.2% | +6.1% | +10.9% | +24.7% (n/c) |
| CitmCatalog (comma) | +35.4% | +18.0% | +24.4% | +21.2% (n/c) | +33.7% (n/c) |
| Google Maps (NDJSON) | +42.9% | +26.8% | +23.6% | +26.1% (n/c) | tie (n/c) |
| Google Maps (comma) | +33.9% | +28.9% | +43.9% | +23.3% (n/c) | +21.6% (n/c) |
| Instruments (NDJSON) | +98.6% | +17.9% | +6.2% | +11.8% | +17.8% (n/c) |
| Instruments (comma) | +72.3% | +36.1% | +42.3% | +37.0% | tie (n/c) |
| Random (NDJSON) | +63.3% | +32.1% | +23.2% | +59.2% | +82.8% (n/c) |
| Random (comma) | +53.1% | +31.9% | +48.1% | +60.2% | +21.8% (n/c) |
| Twitter (NDJSON) | +40.1% (n/c) | +13.3% | +2.5% | +17.5% (n/c) | +28.4% (n/c) |
| Twitter (comma) | +39.6% | +16.9% | +18.8% | +29.8% | tie (n/c) |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +218.7% (n/c) | +38.7% | +42.3% | +33.9% | +72.4% (n/c) |
| CitmCatalog (comma) | +219.9% (n/c) | +54.0% | +63.3% | +60.0% | +17.1% (n/c) |
| Google Maps (NDJSON) | +59.3% | +7.4% | +11.1% | +12.2% (n/c) | −19.6% (n/c) |
| Google Maps (comma) | +46.7% | +9.3% | +29.0% | +30.8% (n/c) | tie (n/c) |
| Instruments (NDJSON) | +77.1% | −11.2% | −25.0% | −9.8% | tie (n/c) |
| Instruments (comma) | +83.2% | +2.9% | −4.0% | tie (n/c) | +23.4% |
| Random (NDJSON) | +292.8% | +28.3% | +42.6% | +57.3% | +35.9% (n/c) |
| Random (comma) | +286.5% | +37.2% | +64.0% | +94.6% (n/c) | +73.7% (n/c) |
| Twitter (NDJSON) | +736.1% (n/c) | +213.2% | +143.6% | +170.7% | +184.7% (n/c) |
| Twitter (comma) | +769.9% (n/c) | +224.4% | +174.0% | +218.1% (n/c) | +305.5% (n/c) |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 24 of the 30 results in that group, tied 1 and lost 5. It won Amazon Cellphones (+4.6% to +50.4%) and Stream Formats Small (comma) (+46.7% to +98.8%) on all five builds, and Large Amazon Cellphones (+4.1% to +43.3%) on four, tying on macOS/Clang. The five losses are all Stream Formats: Small (NDJSON) on Linux/Clang (−5.4%) and macOS/Clang (−17.4%), Large (NDJSON) on Linux/GCC (−4.4%) and macOS/Clang (−20.5%), and Large (comma) on macOS/Clang (−7.6%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 26-0-0; macOS/GCC lost one of 26, Linux/Clang two, Linux/GCC three and macOS/Clang four.
- **In-order corpus streams never lose.** Of the 50 in-order corpus results, Jsonifier won 47 and tied 3, all three on macOS/Clang: Google Maps (NDJSON), Instruments (comma) and Twitter (comma).
- **Reverse order carries over from single documents to streams.** Jsonifier won 42 of 50 reverse stream results and tied 3. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +143.6% to +769.9%, while Google Maps rows, which have few, give −19.6% to +59.3%. Four of the five losses are Instruments: NDJSON on Linux/Clang (−11.2%), Linux/GCC (−25.0%) and macOS/GCC (−9.8%), and comma-separated on Linux/GCC (−4.0%); the fifth is Google Maps NDJSON on macOS/Clang (−19.6%). The ties are Instruments in comma form on macOS/GCC, and Google Maps in comma form and Instruments in NDJSON form on macOS/Clang.
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 119.9 MB/s under MSVC and 446.7 MB/s under Linux/Clang, while Jsonifier reads it at 463.5 and 613.1 MB/s.

## 9. Limitations

- **Linux/GCC and macOS/Clang hold most of the single-document losses:** 59 and 54 of the 173, against 34 on Linux/Clang, 26 on macOS/GCC and none on MSVC. macOS/Clang's are spread across access patterns, led by 14 small sparse, 11 full-size sparse reverse, 9 full-size sparse and 9 small sparse reverse. The largest individual losses are on macOS/Clang: CitmCatalog Sparse Reverse (pretty) at −61.3% reused and −48.2% fresh (both n/c), and Canada Reverse (min) at −57.8% fresh. Both macOS builds ran on virtualized hosts, so part of these results likely reflects host noise. The largest x86 loss is minified Discord Sparse on Linux/GCC (fresh, −34.2%), whose reused run is a win.
- **Linux/GCC loses mostly on reads in document order.** 37 of its 59 losses are in-order reads (§7.11), many on the float-heavy Canada, Marine IK and Mesh, where the cost is number parsing rather than field lookup.
- **Small documents lose on Linux.** At 5 KiB per document, Jsonifier's fixed per-parse cost dominates (§7.11), and it is the next target.
- **Instruments streams read in reverse** lose on three builds: in NDJSON form on Linux/Clang (−11.2%), Linux/GCC (−25.0%) and macOS/GCC (−9.8%), and in comma form on Linux/GCC (−4.0%), although the single-document Instruments Reverse tests win on every build at both sizes and the in-order Instruments streams never lose.
- **Some results did not converge.** 114 of the 1,490 single-document results did not converge: 57 on macOS/Clang, 46 on macOS/GCC, 7 on Windows/MSVC and 4 on Linux/GCC. 40 of the 130 streaming results did not converge: 23 on macOS/Clang, 11 on macOS/GCC and 6 on Windows/MSVC. Most come from the two NEON builds, whose limits are looser.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. (§8.2 gives the cost). The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commits** (`3e7bf6d`; see §6).

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,490 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,250, tied 67 and lost 173. On reverse reads it won 344 of 360 at both sizes, and on Windows/MSVC it lost none of its 298 single-document results. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (616-35-94 fresh, 634-32-79 reused). The main remaining targets are Linux/GCC, with 59 of the 173 losses, macOS/Clang, with 54 on a virtualized host, and small documents on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. Letting the reader's own cursor report where each document ended removes the up-front boundary pass for whitespace-delimited streams. Across 130 streaming results, Jsonifier won 113, tied 7 and lost 10, and it won every streaming test on Windows/MSVC. Reverse Instruments streams and the Stream Formats tests on macOS/Clang are the main remaining streaming targets.

The main engineering lesson is in §5: an index over the whole document made the target workload 2.3x *slower*. Make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.