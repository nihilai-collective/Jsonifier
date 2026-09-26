# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader tuned for small documents;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Across five platform/compiler builds, Jsonifier won 289 of 348 converged single-document results against simdjson On Demand, tied 15 and lost 44. On the full-document reverse-order tests it won 69 of 76, tied 5 and lost 2; on sparse reverse reads it won 73 of 84. On Windows/MSVC it won 72 of 73 single-document results, losing only the String POD test, by −1.8%.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests that include ports of simdjson's own streaming benchmarks, Jsonifier won 84 of 97 converged results, tied 4 and lost 9. It won 22 of the 25 results on simdjson's own streaming benchmarks, including Amazon Cellphones on all five builds, and won every streaming test that converged on Windows/MSVC.

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
| macOS / Clang | Marine IK Reverse (min) | — | +55% | **−6%** | — |
| macOS / Clang | Random Reverse (min) | tie | — | +43% | **−27%** |
| macOS / GCC | Random Reverse (min) | +32% | +35% | — | **−38%** |

*"—" means the result did not converge in that run.*

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

*T* was not re-swept after the wrap gate was added, and the wrap count of 2 is fixed on every build; it was not swept.

## 6. Methodology

- **Harness:** Json-Performance, branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 9 documents each run in minified and prettified form under four access patterns: 77 single-document tests per build.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`. The measured commit, `49941f7`, differs from it only in the field-index trigger of §5.5 and in one bit-scan helper in the NEON stage-1 kernel; the comparison has not been re-run on `49941f7`.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results are dropped from all tallies. The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on a virtualized M1 and used RSE < 10% and shift < 5%.
- **Integer parsing:** Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time.
- **Builds:**

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 73 / 77 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 74 / 77 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 72 / 77 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 68 / 77 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 61 / 77 |

All builds ran Jsonifier `49941f7` against simdjson `610f14d`, with BenchmarkSuite `ced5b69`. The x86 builds ran on an Intel Core i9-14900KF.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "—" means the result did not converge.

### 7.1 Summary

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Total |
|---|---|---|---|---|---|---|
| Windows / MSVC | 4-0-1 | 16-0-0 | 16-0-0 | 18-0-0 | 18-0-0 | 72-0-1 |
| Linux / Clang | 5-0-0 | 16-1-1 | 14-2-0 | 13-1-3 | 15-2-1 | 63-6-5 |
| Linux / GCC | 5-0-0 | 8-1-8 | 15-0-2 | 15-0-2 | 16-0-0 | 59-1-12 |
| macOS / GCC | 3-0-1 | 13-1-2 | 14-0-0 | 16-0-0 | 16-0-2 | 62-1-5 |
| macOS / Clang | 2-0-2 | 5-2-8 | 10-3-0 | 8-1-6 | 8-1-5 | 33-7-21 |
| **All** | **19-0-4** | **58-5-19** | **69-5-2** | **70-2-11** | **73-3-8** | **289-15-44** |

Each cell is wins-ties-losses.

**Jsonifier lost two full-document reverse-order tests out of 76,** both Canada on Linux/GCC (−4.8% minified, −8.0% prettified). The five reverse ties are Google Maps (both forms) on Linux/Clang and macOS/Clang, and Canada (pretty) on macOS/Clang. Canada and Google Maps are made of small objects, which barely exercise out-of-order lookup. **Windows/MSVC lost one single-document test of 73,** the String POD test (−1.8%), and won every in-order, reverse, sparse and sparse-reverse test that converged.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +79.6% | +45.3% | +75.1% | +8.5% | — |
| Double | +35.7% | +63.9% | +76.3% | +90.7% | +85.8% |
| Int64 | +21.5% | +68.6% | +39.7% | +45.8% | +45.3% |
| String | −1.8% | +9.1% | +12.6% | −3.0% | −10.2% |
| Uint64 | +92.8% | +65.2% | +61.2% | — | −38.1% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | — | +6.3% | −10.3% | +4.5% | — |
| Canada (pretty) | — | +6.0% | −2.7% | +2.3% | — |
| CitmCatalog (min) | +24.7% | +4.4% | +11.9% | +5.7% | — |
| CitmCatalog (pretty) | +21.8% | +3.3% | +4.9% | — | +12.7% |
| Discord (min) | +36.7% | +17.4% | +13.0% | +8.8% | −5.3% |
| Discord (pretty) | +36.6% | +19.2% | +4.9% | +12.3% | −5.5% |
| Google Maps (min) | +9.3% | +9.0% | +11.6% | +12.4% | −9.3% |
| Google Maps (pretty) | +1.0% | +8.1% | +13.5% | +8.3% | −14.5% |
| Instruments (min) | +42.6% | +4.4% | −6.8% | +29.6% | +20.8% |
| Instruments (pretty) | +35.5% | +0.9% | −5.1% | +12.7% | −4.5% |
| Marine IK (min) | +9.1% | +4.0% | −8.2% | — | +6.5% |
| Marine IK (pretty) | +13.9% | tie | — | tie | tie |
| Mesh (min) | +36.8% | +1.0% | −1.5% | −3.0% | +10.9% |
| Mesh (pretty) | +31.3% | −0.8% | −8.9% | −4.7% | +8.6% |
| Random (min) | +9.3% | +22.4% | +33.1% | +22.8% | −4.9% |
| Random (pretty) | +10.3% | +20.7% | +31.5% | +20.0% | −2.0% |
| Twitter (min) | +26.4% | +16.1% | tie | +31.6% | −5.1% |
| Twitter (pretty) | +23.9% | +3.7% | −5.5% | +2.4% | tie |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +17.2% | +7.5% | −4.8% | +13.6% | — |
| Canada (pretty) | +18.7% | +3.0% | −8.0% | — | tie |
| CitmCatalog (min) | +308.1% | +78.6% | +75.2% | +109.2% | — |
| CitmCatalog (pretty) | +290.2% | +69.8% | +68.5% | +59.1% | +59.3% |
| Discord (min) | +1617.3% | +289.4% | +342.7% | +324.2% | +263.9% |
| Discord (pretty) | +1573.8% | +292.4% | +330.7% | +324.3% | +213.8% |
| Google Maps (min) | +105.5% | tie | +17.4% | +6.6% | tie |
| Google Maps (pretty) | +91.2% | tie | +14.4% | +10.1% | tie |
| Instruments (min) | +692.7% | +125.4% | +148.1% | — | +121.2% |
| Instruments (pretty) | +663.0% | +109.0% | +124.5% | +104.8% | +118.3% |
| Marine IK (min) | — | — | +69.3% | — | — |
| Marine IK (pretty) | — | — | — | — | — |
| Mesh (min) | +334.4% | +36.6% | +51.6% | +44.6% | +51.8% |
| Mesh (pretty) | +316.2% | +31.4% | +42.5% | +63.2% | +43.6% |
| Random (min) | +212.0% | +27.3% | +28.5% | +28.1% | +19.6% |
| Random (pretty) | +212.6% | +24.9% | +25.6% | +25.4% | +9.0% |
| Twitter (min) | +724.1% | +213.8% | +134.2% | +170.7% | +153.9% |
| Twitter (pretty) | +705.5% | +203.8% | +118.7% | +159.3% | — |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +51.1% | +5.3% | +6.5% | +37.7% | +25.9% |
| Canada (pretty) | +44.3% | +11.3% | — | +9.2% | tie |
| CitmCatalog (min) | +261.7% | +37.0% | +26.6% | +36.1% | +11.2% |
| CitmCatalog (pretty) | +189.4% | +28.6% | +18.3% | +20.9% | — |
| Discord (min) | +16.0% | −8.7% | +12.1% | +14.3% | −11.1% |
| Discord (pretty) | +15.6% | −11.5% | +8.0% | +8.8% | — |
| Google Maps (min) | +55.4% | +2.2% | −2.6% | +18.5% | +6.0% |
| Google Maps (pretty) | +40.7% | −6.7% | −1.5% | +12.5% | +5.8% |
| Instruments (min) | +32.0% | +9.8% | +12.3% | — | −21.1% |
| Instruments (pretty) | +34.9% | +2.2% | +9.2% | +8.5% | −14.7% |
| Marine IK (min) | +44.5% | +27.7% | +30.2% | — | — |
| Marine IK (pretty) | +46.7% | — | +12.2% | +17.2% | +16.3% |
| Mesh (min) | +159.0% | +6.7% | +14.1% | +10.5% | +14.4% |
| Mesh (pretty) | +143.4% | +5.6% | +7.3% | +4.1% | +9.6% |
| Random (min) | +24.4% | +8.7% | +16.4% | +16.2% | −9.7% |
| Random (pretty) | +24.6% | +10.6% | +7.0% | +14.9% | −6.0% |
| Twitter (min) | +145.0% | tie | +25.1% | +24.7% | +11.7% |
| Twitter (pretty) | +125.7% | +7.3% | +19.8% | +15.9% | −2.8% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +251.3% | tie | +14.5% | +26.4% | — |
| Canada (pretty) | +173.2% | +7.4% | — | +41.9% | — |
| CitmCatalog (min) | +231.6% | +22.6% | +38.2% | −9.6% | +30.1% |
| CitmCatalog (pretty) | +198.9% | +20.5% | +30.8% | +8.2% | +34.4% |
| Discord (min) | +133.5% | +1.3% | +22.3% | +8.6% | −2.4% |
| Discord (pretty) | +129.8% | −2.9% | +16.2% | +8.3% | tie |
| Google Maps (min) | +73.0% | +2.8% | — | +2.8% | — |
| Google Maps (pretty) | +61.3% | tie | +12.7% | +22.3% | −4.8% |
| Instruments (min) | +197.3% | +10.4% | +32.6% | +45.5% | +5.1% |
| Instruments (pretty) | +185.2% | +8.5% | +25.2% | +43.1% | −5.3% |
| Marine IK (min) | +185.7% | +36.6% | +60.2% | +33.9% | — |
| Marine IK (pretty) | +151.6% | +15.4% | +16.1% | +39.2% | +35.2% |
| Mesh (min) | +160.8% | +6.2% | +13.4% | +16.1% | +15.4% |
| Mesh (pretty) | +142.5% | +7.0% | +8.4% | +5.1% | +12.5% |
| Random (min) | +56.3% | +3.8% | +13.7% | +21.7% | −8.0% |
| Random (pretty) | +53.9% | +2.5% | +12.3% | +26.0% | −5.7% |
| Twitter (min) | +200.2% | +10.2% | +20.4% | +36.1% | +5.5% |
| Twitter (pretty) | +182.8% | +6.0% | +22.0% | −18.9% | +1.1% |

### 7.7 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.0x to 17x faster: +104.8% (Instruments pretty, macOS/GCC) up to +1617.3% (Discord min, MSVC). On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the margin runs from −8.0% to +17.4%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are now a strength.** Jsonifier won 73 of 84 converged sparse-reverse results, tied 3 and lost 8. Linux/GCC and MSVC won all 34 of theirs, and Random, Instruments and Discord, whose objects receive two or three lookups out of many more keys, win on every x86 build except one Discord result on Linux/Clang (−2.9%). This is the access pattern §5.5's wrap gate was built for: the object is large enough to cross *T*, but it rarely wraps twice, so it never pays for an index that few lookups would use. Five of the eight losses are on macOS/Clang.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty) runs at 32.4 MB/s on MSVC against 218.2 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 542.3 and 856.2 MB/s. Every MSVC reverse result is a win, by +91.2% or more outside Canada (+17.2% and +18.7%); every MSVC sparse result is a win, by +15.6% to +261.7%, and every sparse-reverse result by +53.9% to +251.3%.
- **Sparse reads in order are mostly wins.** Of 83 converged results, Jsonifier won 70, tied 2 and lost 11. MSVC and macOS/GCC won all of theirs. Six of the losses are on macOS/Clang; the rest are Discord and Google Maps (pretty) on Linux/Clang and Google Maps on Linux/GCC (−2.6% and −1.5%).
- **In-order full reads are mostly wins.** Across 82 in-order document results, Jsonifier won 58, tied 5 and lost 19. MSVC won all 16, and Linux/Clang lost one, Mesh (pretty) by −0.8%. Linux/GCC split its in-order tests 8-1-8, and macOS/Clang 5-2-8.
- **In-order losses cluster on number-heavy documents.** Mesh loses in order on Linux/GCC (−1.5% and −8.9%), Linux/Clang (pretty, −0.8%) and macOS/GCC (−3.0% and −4.7%); Canada and Marine IK lose on Linux/GCC. These documents are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. The other Linux/GCC losses are Instruments (−6.8% and −5.1%) and Twitter (pretty, −5.5%). macOS/Clang's eight losses are spread across Discord, Google Maps, Instruments, Random and Twitter, all by −2.0% to −14.5%; see §9.
- **The number POD tests win on every x86 build.** Double wins on all five builds (+35.7% to +90.7%) and Int64 on all five (+21.5% to +68.6%). Uint64 wins on the three x86 builds (+61.2% to +92.8%) and loses on macOS/Clang (−38.1%). The other three POD losses are String on MSVC (−1.8%), macOS/GCC (−3.0%) and macOS/Clang (−10.2%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 4.9% to 19.8% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **simdjson's own streaming benchmarks, ported.** *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from simdjson's repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses simdjson's `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 22 / 26 |
| Linux (WSL2) / Clang 24.0 | AVX2 | 23 / 26 |
| Linux (WSL2) / GCC 16.1 | AVX2 | 23 / 26 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 12 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 17 / 26 |

The commits, hosts and convergence limits are the same as in §6.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 6-0-0 | 22-0-0 |
| Linux / Clang | 4-0-1 | 8-1-0 | 8-0-1 | 20-1-2 |
| Linux / GCC | 5-1-0 | 7-0-2 | 6-0-2 | 18-1-4 |
| macOS / Clang | 5-0-1 | 4-0-1 | 6-0-0 | 15-0-2 |
| macOS / GCC | 2-0-0 | 5-2-0 | 2-0-1 | 9-2-1 |
| **All** | **22-1-2** | **34-3-3** | **28-0-4** | **84-4-9** |

**simdjson's own benchmarks:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones | +40.7% | +14.2% | +5.3% | +48.5% | +17.0% |
| Large Amazon Cellphones | +33.8% | — | tie | — | +9.6% |
| Stream Formats Small (NDJSON) | +22.1% | −2.5% | +16.5% | — | −13.6% |
| Stream Formats Small (comma) | +74.2% | +64.9% | +87.6% | — | +54.9% |
| Stream Formats Large (NDJSON) | +16.2% | +2.8% | +46.2% | +24.2% | +3.8% |
| Stream Formats Large (comma) | +22.6% | +5.8% | +36.1% | — | +10.2% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +25.2% | — | — | — | +1.6% |
| CitmCatalog (comma) | +26.1% | +16.1% | +19.9% | +29.7% | +19.2% |
| Google Maps (NDJSON) | +12.7% | +19.9% | +14.1% | +18.9% | −4.0% |
| Google Maps (comma) | +11.8% | +25.7% | +26.9% | +21.8% | — |
| Instruments (NDJSON) | +44.1% | +16.8% | −8.0% | +7.5% | — |
| Instruments (comma) | +45.1% | +23.4% | +20.2% | — | +7.7% |
| Random (NDJSON) | +11.1% | +19.1% | +25.5% | tie | +8.1% |
| Random (comma) | +10.3% | +27.3% | +37.5% | +25.5% | — |
| Twitter (NDJSON) | +24.2% | +6.7% | −7.4% | tie | — |
| Twitter (comma) | +22.2% | tie | +3.9% | — | — |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | — | +48.5% | +37.0% | — | +46.3% |
| CitmCatalog (comma) | — | +61.0% | +59.4% | — | +65.0% |
| Google Maps (NDJSON) | +47.3% | +4.9% | — | −13.4% | — |
| Google Maps (comma) | +43.4% | +9.3% | +17.7% | — | +8.2% |
| Instruments (NDJSON) | +58.3% | −14.9% | −19.8% | — | +3.3% |
| Instruments (comma) | +68.0% | +5.4% | −4.7% | +13.1% | +26.5% |
| Random (NDJSON) | +220.1% | +56.5% | — | — | +26.2% |
| Random (comma) | +224.4% | +61.6% | +61.5% | +51.1% | — |
| Twitter (NDJSON) | — | +231.5% | +141.3% | — | — |
| Twitter (comma) | — | — | +156.6% | — | — |

### 8.5 What the results show

- **Jsonifier wins simdjson's own streaming benchmarks.** It took 22 of the 25 converged results in that group and tied one. It won Amazon Cellphones on all five builds (+5.3% to +48.5%) and Stream Formats Small (comma) on every build where it converged (+54.9% to +87.6%). The two losses are both Stream Formats Small (NDJSON), on Linux/Clang (−2.5%) and macOS/Clang (−13.6%); the tie is Large Amazon Cellphones on Linux/GCC. Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test that converged,** 22-0-0.
- **In-order corpus streams are mostly wins.** Of the 40 converged in-order corpus results, Jsonifier won 34, tied 3 and lost 3. The comma-separated streams won 19 of their 20 in-order results and tied the other. All three losses are NDJSON: Instruments (−8.0%) and Twitter (−7.4%) on Linux/GCC, and Google Maps on macOS/Clang (−4.0%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 28 of 32 converged reverse stream results. The margins follow object size, as in §7.7: Twitter statuses, which have many fields, give +141.3% to +231.5%, while Google Maps rows, which have few, give −13.4% to +47.3%. Three of the four losses are Instruments: NDJSON on Linux/Clang (−14.9%) and Linux/GCC (−19.8%), and comma on Linux/GCC (−4.7%). The fourth is Google Maps (NDJSON) on macOS/GCC (−13.4%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 120.7 MB/s under MSVC and 417.0 MB/s under Linux/Clang, while Jsonifier reads it at 391.4 and 673.9 MB/s.

## 9. Limitations

- **macOS/Clang is the weakest build.** It holds 21 of the 44 single-document losses: eight in order (Discord, Google Maps and Random in both forms, Instruments pretty, Twitter min), six sparse, five sparse reverse and two POD tests, including the largest loss in the suite, Uint64 (−38.1%). It also converged least often among the single-document runs (61 of 77). It ran on a virtualized M1, so part of these results likely reflects host noise, but its losses are not confined to one access pattern.
- **Linux/GCC** is the weakest x86 build for full in-order reads, with eight losses: Canada ×2, Instruments ×2, Marine IK (min), Mesh ×2 and Twitter (pretty). The largest are Canada (min, −10.3%) and Mesh (pretty, −8.9%). It is also the only build that loses a full-document reverse test (Canada, both forms), while it won all 16 of its sparse-reverse results.
- **Number-heavy documents in order** remain a common loss. Mesh loses on three builds, and Marine IK and Canada on Linux/GCC. The cause is float-array throughput rather than out-of-order access, so it is out of scope for the field index.
- **Instruments reverse streams** lose on both Linux builds in NDJSON form (−14.9% and −19.8%), although the single-document Instruments Reverse tests win on every build that converged.
- **Convergence is uneven.** macOS/GCC met the convergence limits on 68 of its 77 single-document tests but only 12 of its 26 streaming tests, and macOS/Clang on 61 and 17, even with the looser NEON limits. Marine IK Reverse converged on only one build (Linux/GCC, +69.3%).
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the same benchmarks reported in §7, before the wrap gate was added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. On the x86 in-order corpus streams this leaves comma mode 4.9% to 19.8% slower than NDJSON mode on the same records. The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commit.** The byte-for-byte comparison in §6 ran on `3e7bf6d`, not on `49941f7`; the field-index trigger that differs between them changes which lookup path finds a key, not which value it returns.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 348 converged single-document results on five builds, Jsonifier won 289, tied 15 and lost 44. On full-document reverse reads it won 69 of 76 and lost two, both Canada on Linux/GCC; on sparse reverse reads it won 73 of 84; and on Windows/MSVC it lost one single-document test of 73, by −1.8%. Most of the x86 in-order losses are reads of number-heavy documents, which is number-parsing work rather than generic-layer work; macOS/Clang, with 21 of the 44 losses, is the main remaining target.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. The one lesson it added repeats §5's: finding every document's end up front makes an in-order reader pay for two passes over the tape, and letting the reader's own cursor report where each document ended removes that pass for whitespace-delimited streams. Across 97 converged streaming results, Jsonifier won 84, tied 4 and lost 9, winning 22 of 25 on simdjson's own streaming benchmarks and 28 of 32 with keys read in reverse order, and it won every converged streaming test on Windows/MSVC. Instruments reverse NDJSON streams on Linux are the main remaining streaming target.

The main engineering lesson is in §5. An index over the whole document made the target workload 2.3x *slower*, because most objects are small. The version that works builds the index lazily, per object, and only once that object has shown both that it is large and that it is being read out of order: enough skipped keys to clear *T*, and two lookups that had to wrap around. In short: make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.