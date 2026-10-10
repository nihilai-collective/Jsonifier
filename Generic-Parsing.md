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

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,187 of 1,417 converged single-document results against simdjson On Demand, tied 50 and lost 180. On reverse-order reads it won 310 of 324. On Windows/MSVC it won 284 of 294 single-document results, tied 6 and lost 4.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests, Jsonifier won 91 of 102 converged results, tied 3 and lost 8, and won every streaming test that converged on Windows/MSVC.

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

### 3.5 Runtime CPU selection

`jsonifier::generic` takes part in the same runtime tier selection as the rest of the library. On x86, one Jsonifier binary carries every instruction-set tier it was configured for and picks one at run time. The headers are compiled once per tier inside a target-attribute region (`backend_passes.hpp`: `#pragma GCC target` on GCC, `#pragma clang attribute` on Clang), which yields one complete `jsonifier_core_internal<backend>` per tier: AVX-512 (`avx512f`, `avx512bw`, `avx512vbmi2`), AVX2 and AVX. Each of those inherits its tier's parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call inside a tier is a direct, inlinable call into code built for that tier. On first use, `selectSupportedBackend` reads CPUID (leaves 0, 1 and 7, plus the extended leaves for `lzcnt`) and XCR0, so a tier counts only if both the CPU and the operating system support it, and caches the widest supported tier in a function-local `static`. Every public entry point then dispatches with a fold expression over the compiled tiers:

```cpp
static const jsonifier_backend backend{ selectedBackend() };
static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
```

That is a load of a cached enum and at most three compares, once per call. There is no function pointer and no vtable. simdjson's runtime dispatch, by contrast, goes through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its own On Demand documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target. In Jsonifier the same dispatch covers every API, including typed parsing, serialization and the generic On Demand-style iterator, and inside a tier nothing is virtual, so the per-tier code keeps all of its compile-time specialization.

The scope is x86. ARM builds compile one backend (NEON, or SVE2 when configured), as simdjson's ARM builds do. The set of x86 tiers compiled in is bounded by the configured tier (`JSONIFIER_CONFIGURED_AVX_TIER`), so a build configured for AVX2 carries AVX2 and AVX but not AVX-512. We have not separately measured the cost of the per-call check. This is the case simdjson's On Demand documentation leaves to the caller: a generic Jsonifier reader compiled once for x86 runs its AVX-512, AVX2 or AVX stage 1 depending on the machine, with no change to the calling code.

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
- **Tests:** 5 POD tests, plus 9 documents each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 154 single-document results per build at full size and another 144 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document and reused tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results are dropped from all tallies. The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on a virtualized M1 and used RSE < 10% and shift < 5%.
- **Integer parsing:** Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 150 / 154 | 144 / 144 | 294 / 298 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 145 / 154 | 144 / 144 | 289 / 298 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 121 / 154 | 144 / 144 | 265 / 298 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 128 / 154 | 143 / 144 | 271 / 298 |

All builds ran simdjson `7f6f8dc` with BenchmarkSuite `6196208`. Jsonifier was `12498b4` on Windows/MSVC, `5aa6104` on both Linux builds and `13785b6` on both macOS builds. Jsonifier selects its generic stage-1 kernel at run time from the detected CPU; the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on October 10, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "—" means the result did not converge. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 3-2-0 | 16-1-1 | 16-0-0 | 18-0-0 | 18-0-0 | 17-1-0 | 18-0-0 | 17-0-1 | 18-0-0 | 141-4-2 |
| Linux / Clang | 5-0-0 | 17-1-0 | 18-0-0 | 16-0-2 | 18-0-0 | 8-1-9 | 18-0-0 | 10-1-7 | 12-3-3 | 122-6-21 |
| Linux / GCC | 5-0-0 | 9-0-9 | 13-0-2 | 15-1-1 | 15-0-2 | 6-1-11 | 16-0-2 | 12-0-6 | 18-0-0 | 109-2-33 |
| macOS / GCC | 3-0-1 | 12-2-1 | 11-0-0 | 16-1-1 | 11-0-1 | 14-2-2 | 18-0-0 | 15-3-0 | 18-0-0 | 118-8-6 |
| macOS / Clang | 3-0-1 | 12-0-3 | 12-0-0 | 11-0-6 | 13-1-4 | 13-2-3 | 17-0-1 | 9-0-9 | 13-0-4 | 103-3-31 |
| **All** | **19-2-2** | **66-4-14** | **70-0-2** | **76-2-10** | **75-1-7** | **58-7-25** | **87-0-3** | **63-4-23** | **79-3-7** | **593-23-93** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 4-0-1 | 16-1-1 | 16-0-0 | 18-0-0 | 18-0-0 | 17-1-0 | 18-0-0 | 18-0-0 | 18-0-0 | 143-2-2 |
| Linux / Clang | 5-0-0 | 17-1-0 | 17-1-0 | 16-0-2 | 17-1-0 | 12-0-6 | 18-0-0 | 11-1-6 | 12-3-3 | 125-7-17 |
| Linux / GCC | 5-0-0 | 9-1-8 | 14-0-3 | 15-0-1 | 16-0-1 | 7-0-11 | 16-0-2 | 11-1-6 | 18-0-0 | 111-2-32 |
| macOS / GCC | 3-0-0 | 9-1-4 | 10-0-0 | 15-2-0 | 14-1-2 | 16-0-2 | 18-0-0 | 18-0-0 | 15-3-0 | 118-7-8 |
| macOS / Clang | 3-1-1 | 13-1-1 | 10-1-0 | 10-0-5 | 10-2-4 | 14-1-3 | 16-1-1 | 10-2-6 | 11-0-7 | 97-9-28 |
| **All** | **20-1-2** | **64-5-14** | **67-2-3** | **74-2-8** | **75-4-7** | **66-2-22** | **86-1-3** | **68-4-18** | **74-6-10** | **594-27-87** |


Each cell is wins-ties-losses.

Across all 1,417 converged single-document results, Jsonifier won 1,187, tied 50 and lost 180: 593-23-93 with fresh output objects and 594-27-87 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 137 of 144 converged full-size reverse results (2 ties, 5 losses) and 173 of 180 small reverse results (1 tie, 6 losses). All 11 of those losses are on Canada or Google Maps: nine on Linux/GCC (Canada at both sizes, −3.2% to −8.5%, and minified Google Maps reused, −4.1%) and two on macOS/Clang (−4.1% and −6.8%).
- **Windows/MSVC lost 4 of 294 single-document results:** Canada read in order (pretty, fresh, −2.8%; min, reused, −4.0%), Marine IK Small Sparse (min, fresh, −1.2%) and the String POD test (reused, −8.2%). Its 6 ties are Canada read in order (full size, min fresh and pretty reused), Canada Small (min) read in order in both modes, and Bool and String (fresh). It won every reverse and sparse-reverse test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6.1–10.4% for Jsonifier and 4.5–6.2% for simdjson, depending on the build. Jsonifier's median margin over simdjson moves by +0.8 to +3.8 percentage points on the non-MSVC builds and by +8.0 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +1527.9% to +1688.3%, and Mesh Small Sparse (min) from +129.2% to +167.6%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | tie / +1.9% | +59.2% / +60.9% | +45.1% / +43.8% | +41.6% / +27.7% | +17.5% / +1.6% |
| Double | +19.4% / +20.2% | +53.9% / +55.6% | +53.5% / +55.1% | — / — | +46.2% / +45.1% |
| Int64 | +15.0% / +6.1% | +58.6% / +58.0% | +38.8% / +41.7% | +36.9% / — | — / tie |
| String | tie / −8.2% | +13.2% / +16.4% | +4.6% / +5.1% | −3.3% / +18.4% | −23.0% / −17.9% |
| Uint64 | +7.2% / +11.3% | +56.3% / +53.6% | +54.0% / +58.8% | +39.7% / +32.9% | +5.3% / +19.2% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | tie / −4.0% | +9.5% / +5.4% | −6.6% / −6.7% | +27.8% / −17.4% | — / tie |
| Canada (pretty) | −2.8% / tie | +5.8% / +6.2% | −8.0% / −5.9% | — / — | — / — |
| CitmCatalog (min) | +33.1% / +34.1% | +4.2% / +3.7% | +3.7% / +5.8% | +25.3% / −6.4% | +15.2% / +5.3% |
| CitmCatalog (pretty) | +37.4% / +36.7% | +2.4% / +2.0% | +3.0% / +4.0% | tie / — | +2.8% / +6.0% |
| Discord (min) | +53.9% / +73.8% | +11.1% / +14.5% | +6.8% / +10.1% | +20.0% / +25.6% | +11.2% / +20.5% |
| Discord (pretty) | +49.5% / +70.8% | +13.4% / +15.7% | +4.3% / +5.8% | +20.1% / +24.4% | +4.9% / +7.9% |
| Google Maps (min) | +33.6% / +36.5% | +22.7% / +21.9% | +7.9% / −4.5% | +58.2% / +68.4% | +13.1% / +17.5% |
| Google Maps (pretty) | +30.4% / +35.3% | +13.3% / +15.3% | −4.1% / +9.0% | +34.4% / — | −2.9% / +12.4% |
| Instruments (min) | +65.2% / +72.4% | +5.8% / +7.4% | +12.8% / +11.2% | +7.8% / tie | +11.4% / −4.4% |
| Instruments (pretty) | +44.1% / +63.1% | +3.8% / +4.2% | −2.7% / tie | +6.9% / +9.1% | +13.9% / — |
| Marine IK (min) | +14.2% / +15.3% | +2.2% / tie | −48.6% / −46.1% | tie / — | — / +4.6% |
| Marine IK (pretty) | +12.5% / +15.3% | +2.6% / +1.4% | −4.3% / −5.4% | — / −8.2% | +3.4% / +8.4% |
| Mesh (min) | +53.1% / +63.2% | +1.8% / +2.4% | −3.1% / −2.8% | −8.6% / +12.4% | +7.9% / +26.9% |
| Mesh (pretty) | +49.9% / +60.2% | tie / +0.9% | −3.8% / −6.2% | — / −6.7% | −6.2% / — |
| Random (min) | +47.6% / +66.7% | +24.4% / +30.1% | +15.0% / +17.1% | +27.9% / +69.3% | −6.0% / +30.9% |
| Random (pretty) | +45.8% / +59.6% | +24.0% / +26.3% | +13.8% / +19.4% | +39.2% / +37.8% | +29.2% / +25.9% |
| Twitter (min) | +37.0% / +46.9% | +11.2% / +16.0% | −2.3% / +8.3% | +10.2% / +20.9% | +3.4% / +5.2% |
| Twitter (pretty) | +33.1% / +43.1% | +4.5% / +6.8% | +4.8% / −1.7% | +9.7% / +18.0% | +20.1% / +18.9% |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +18.6% / +20.6% | +6.1% / +5.8% | −3.2% / −8.0% | — / — | +10.3% / +5.2% |
| Canada (pretty) | +17.1% / +19.8% | +3.8% / tie | −8.5% / −5.8% | — / — | +2.5% / — |
| CitmCatalog (min) | +312.8% / +312.9% | +65.9% / +68.0% | +79.6% / +83.4% | — / — | — / — |
| CitmCatalog (pretty) | +298.9% / +309.3% | +63.8% / +66.3% | +74.0% / +79.6% | — / +64.8% | +39.0% / — |
| Discord (min) | +1527.9% / +1688.3% | +258.9% / +282.2% | +311.8% / +356.4% | — / +394.6% | +252.7% / +263.0% |
| Discord (pretty) | +1574.5% / +1770.9% | +256.1% / +275.3% | +313.0% / +347.3% | +317.3% / +263.1% | +257.5% / +273.5% |
| Google Maps (min) | +113.9% / +107.0% | +3.0% / +3.0% | +16.2% / −4.1% | +10.9% / +17.8% | — / +14.9% |
| Google Maps (pretty) | +110.9% / +114.9% | +3.9% / +3.7% | — / +16.7% | +8.4% / — | +11.0% / — |
| Instruments (min) | +696.9% / +693.3% | +118.3% / +118.2% | — / — | +138.6% / +136.9% | — / +139.3% |
| Instruments (pretty) | +648.2% / +684.5% | +106.1% / +108.6% | — / +92.5% | +127.8% / +142.4% | +100.6% / +121.1% |
| Marine IK (min) | — / — | +34.7% / +36.5% | +67.3% / +69.9% | +49.3% / — | +66.5% / — |
| Marine IK (pretty) | — / — | +32.2% / +34.3% | +49.8% / +51.3% | +54.7% / — | — / — |
| Mesh (min) | +383.3% / +419.5% | +34.7% / +36.9% | +48.9% / +53.5% | — / +84.1% | — / +38.3% |
| Mesh (pretty) | +333.8% / +384.4% | +31.4% / +35.3% | +41.9% / +44.5% | +118.1% / — | +33.8% / +49.2% |
| Random (min) | +228.8% / +260.3% | +21.9% / +23.9% | +18.9% / +22.0% | +42.2% / +43.7% | +47.1% / tie |
| Random (pretty) | +225.3% / +252.7% | +17.8% / +21.7% | +17.5% / +20.6% | +36.7% / +40.4% | +24.1% / +29.3% |
| Twitter (min) | +679.9% / +780.1% | +198.5% / +208.7% | +127.1% / +134.0% | +175.5% / +194.0% | — / — |
| Twitter (pretty) | +687.0% / +767.8% | +181.2% / +192.6% | +116.4% / +123.6% | — / — | +154.5% / +165.9% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +64.9% / +62.0% | +7.9% / +8.1% | +9.8% / +8.3% | +29.6% / +14.5% | +48.6% / +48.2% |
| Canada (pretty) | +42.0% / +41.4% | +14.9% / +14.6% | +9.5% / +10.4% | +28.3% / +18.2% | +11.9% / +42.3% |
| CitmCatalog (min) | +278.1% / +286.5% | +36.1% / +35.9% | +17.3% / +18.2% | +6.6% / +12.8% | — / +14.4% |
| CitmCatalog (pretty) | +203.3% / +207.6% | +27.5% / +24.9% | +11.6% / +15.8% | +13.1% / +15.0% | +20.8% / +15.4% |
| Discord (min) | +28.7% / +42.6% | −6.2% / −5.9% | +6.2% / +5.6% | +17.7% / +31.2% | +1.8% / +3.2% |
| Discord (pretty) | +34.9% / +40.0% | −2.9% / −5.6% | +88.5% / +61.2% | +32.7% / +52.8% | −4.2% / +2.1% |
| Google Maps (min) | +58.4% / +66.5% | +4.3% / +5.4% | — / — | +19.4% / +15.7% | −7.4% / −2.7% |
| Google Maps (pretty) | +53.2% / +57.9% | +4.5% / +4.9% | −7.8% / −5.5% | +7.7% / +10.0% | +10.4% / +33.3% |
| Instruments (min) | +38.8% / +44.0% | +3.1% / +2.5% | +63.5% / +11.5% | +31.8% / +11.2% | +2.2% / +8.1% |
| Instruments (pretty) | +42.7% / +48.4% | +3.2% / +2.5% | tie / — | +50.7% / +22.6% | −19.1% / −9.4% |
| Marine IK (min) | +29.7% / +26.8% | +19.1% / +17.8% | +31.6% / +30.2% | +6.1% / +12.2% | +18.5% / — |
| Marine IK (pretty) | +40.8% / +42.0% | +17.8% / +15.7% | +17.4% / +15.1% | +23.3% / — | −14.3% / — |
| Mesh (min) | +280.6% / +326.9% | +6.7% / +8.3% | +19.4% / +20.4% | tie / tie | +12.9% / +13.7% |
| Mesh (pretty) | +247.6% / +277.9% | +5.9% / +7.1% | +12.5% / +14.1% | −7.3% / tie | +54.2% / — |
| Random (min) | +56.1% / +63.2% | +12.7% / +14.2% | +8.3% / +9.4% | +27.6% / +33.7% | +14.4% / −5.7% |
| Random (pretty) | +54.0% / +65.4% | +10.0% / +13.2% | +8.5% / +8.6% | +31.5% / +33.7% | +19.2% / +23.4% |
| Twitter (min) | +121.3% / +130.1% | +3.7% / +4.0% | +5.5% / +5.9% | +13.8% / +19.4% | −11.0% / −3.9% |
| Twitter (pretty) | +112.7% / +114.7% | +2.1% / +1.6% | +2.1% / +3.3% | +22.0% / +10.5% | −1.5% / −3.0% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +265.0% / +268.8% | +4.1% / +3.9% | +20.7% / +23.3% | — / +16.8% | tie / +10.3% |
| Canada (pretty) | +180.8% / +176.5% | +10.0% / +10.4% | +10.3% / +9.7% | — / tie | −5.9% / −14.5% |
| CitmCatalog (min) | +272.4% / +273.0% | +25.4% / +26.3% | +36.3% / +36.0% | +10.3% / +18.4% | +6.2% / +10.6% |
| CitmCatalog (pretty) | +230.9% / +219.0% | +22.8% / +21.9% | +33.9% / +29.6% | +18.7% / +19.0% | +12.5% / +9.9% |
| Discord (min) | +119.4% / +150.3% | +6.5% / +9.0% | +16.5% / — | +26.4% / +39.5% | +17.4% / +9.1% |
| Discord (pretty) | +123.6% / +140.2% | +7.7% / +8.1% | −1.6% / +14.5% | +29.0% / +27.7% | +2.6% / +20.3% |
| Google Maps (min) | +78.0% / +83.0% | +2.0% / +1.3% | −25.2% / +33.6% | −13.2% / +10.2% | −19.1% / −21.3% |
| Google Maps (pretty) | +69.9% / +75.1% | +1.5% / +0.9% | — / −7.6% | +4.1% / −2.4% | +6.6% / −9.7% |
| Instruments (min) | +198.0% / +204.2% | +5.5% / +5.0% | +21.5% / +21.6% | +53.2% / +25.6% | +24.9% / tie |
| Instruments (pretty) | +183.4% / +195.9% | +7.2% / +5.7% | +18.8% / +18.8% | — / +25.1% | −7.2% / +12.5% |
| Marine IK (min) | +217.7% / +223.0% | +22.3% / +23.0% | +58.5% / +56.7% | +44.8% / — | +41.5% / +31.7% |
| Marine IK (pretty) | +178.2% / +178.2% | +17.3% / +17.9% | +27.0% / +27.6% | +44.0% / +30.9% | +31.1% / — |
| Mesh (min) | +285.2% / +318.1% | +6.7% / +8.1% | +19.1% / +19.9% | — / +6.3% | +25.6% / — |
| Mesh (pretty) | +260.5% / +261.1% | +5.6% / +6.7% | +11.8% / +13.3% | — / −13.2% | +18.4% / tie |
| Random (min) | +82.1% / +90.3% | +1.2% / +2.4% | +14.3% / +15.6% | +55.1% / +44.8% | −10.9% / +22.6% |
| Random (pretty) | +79.2% / +85.7% | +2.1% / tie | +16.4% / +15.5% | — / +54.4% | +3.0% / +7.7% |
| Twitter (min) | +191.4% / +199.8% | +10.4% / +11.0% | +14.8% / +16.0% | +7.6% / +27.2% | +3.6% / +3.0% |
| Twitter (pretty) | +172.6% / +184.7% | +9.0% / +9.0% | +10.7% / +11.8% | +44.7% / +48.7% | +4.1% / −3.2% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | tie / tie | +3.8% / +5.4% | −10.2% / −13.9% | +6.9% / +9.6% | +8.1% / +11.1% |
| Canada (pretty) | +1.3% / +1.8% | +5.2% / +7.9% | −11.1% / −14.3% | −5.4% / +21.5% | +4.3% / +3.7% |
| CitmCatalog (min) | +18.5% / +24.9% | −4.7% / −3.1% | −7.0% / −3.0% | +7.2% / +74.3% | tie / +14.2% |
| CitmCatalog (pretty) | +16.1% / +24.4% | −6.1% / −4.9% | −6.1% / −2.7% | +7.0% / +6.9% | −1.1% / +2.7% |
| Discord (min) | +16.6% / +47.3% | +1.6% / +3.0% | +2.1% / +9.0% | +25.2% / +18.5% | +7.0% / +14.8% |
| Discord (pretty) | +17.8% / +46.6% | −1.6% / +2.9% | +0.6% / +5.6% | +14.8% / +30.1% | −0.9% / +9.5% |
| Google Maps (min) | +31.1% / +36.7% | +19.3% / +17.8% | +8.2% / +7.8% | +13.8% / +20.6% | +4.6% / +7.0% |
| Google Maps (pretty) | +31.8% / +36.5% | +15.7% / +13.5% | +9.0% / +10.2% | +23.2% / +24.0% | −9.6% / +2.2% |
| Instruments (min) | +54.0% / +57.0% | −1.3% / +0.8% | −6.7% / −7.0% | +7.9% / +9.0% | +7.4% / +4.1% |
| Instruments (pretty) | +54.4% / +53.6% | −2.4% / −1.8% | −9.2% / −8.0% | tie / +5.8% | +2.0% / +3.3% |
| Marine IK (min) | +2.4% / +4.1% | +0.6% / +1.6% | −3.9% / −3.8% | +4.3% / +7.2% | +2.5% / +3.8% |
| Marine IK (pretty) | +7.2% / +7.8% | −1.0% / +0.5% | −4.6% / −3.8% | +5.1% / +6.2% | +8.5% / +1.8% |
| Mesh (min) | +18.4% / +42.9% | tie / −2.0% | −7.3% / −10.1% | −5.2% / −8.3% | tie / −5.9% |
| Mesh (pretty) | +14.4% / +35.4% | −0.9% / −5.4% | −10.6% / −14.6% | +6.9% / −8.5% | +9.4% / −15.3% |
| Random (min) | +43.4% / +51.7% | +13.3% / +16.2% | +12.4% / +20.0% | +40.0% / +87.3% | +12.9% / +22.1% |
| Random (pretty) | +30.9% / +46.9% | +13.6% / +16.5% | +12.8% / +17.9% | +40.2% / +45.9% | +16.2% / +24.5% |
| Twitter (min) | +49.4% / +49.0% | −0.5% / +3.1% | tie / +1.8% | +19.4% / +25.9% | +7.2% / tie |
| Twitter (pretty) | +37.6% / +47.9% | −1.7% / −0.7% | −2.1% / −0.5% | tie / +18.4% | +4.5% / −65.7% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +13.3% / +15.1% | +9.2% / +12.3% | −4.6% / −7.3% | +8.7% / +13.8% | +1.3% / −4.1% |
| Canada (pretty) | +14.5% / +16.9% | +8.1% / +8.0% | −5.2% / −6.6% | +9.4% / +6.0% | +3.0% / tie |
| CitmCatalog (min) | +205.2% / +231.7% | +50.8% / +54.4% | +68.6% / +75.9% | +55.9% / +60.5% | +56.3% / +51.6% |
| CitmCatalog (pretty) | +197.0% / +230.9% | +47.0% / +50.5% | +64.3% / +71.6% | +55.6% / +52.8% | +51.0% / +49.0% |
| Discord (min) | +1184.3% / +1453.2% | +250.3% / +289.1% | +274.0% / +316.4% | +314.6% / +359.3% | +257.0% / +279.7% |
| Discord (pretty) | +1159.6% / +1413.3% | +250.1% / +281.4% | +263.8% / +295.7% | +303.8% / +356.0% | +249.7% / +646.6% |
| Google Maps (min) | +101.8% / +112.0% | +2.9% / +2.7% | +18.2% / +19.3% | +16.5% / +24.0% | +6.0% / +7.3% |
| Google Maps (pretty) | +98.2% / +104.6% | +1.7% / +2.3% | +16.4% / +18.6% | +17.6% / +29.7% | −6.8% / +5.0% |
| Instruments (min) | +713.7% / +710.7% | +116.2% / +120.8% | +133.7% / +137.0% | +124.3% / +155.9% | +130.8% / +138.5% |
| Instruments (pretty) | +659.9% / +696.3% | +109.2% / +112.9% | +123.5% / +125.5% | +143.3% / +174.1% | +109.6% / +128.3% |
| Marine IK (min) | +200.5% / +212.0% | +32.3% / +32.3% | +54.8% / +59.7% | +44.4% / +48.6% | +36.7% / +35.0% |
| Marine IK (pretty) | +197.1% / +199.7% | +31.1% / +31.5% | +51.8% / +53.0% | +44.5% / +45.6% | +39.0% / +24.0% |
| Mesh (min) | +128.7% / +233.7% | +24.3% / +30.2% | +26.3% / +33.0% | +28.3% / +35.7% | +20.8% / +13.1% |
| Mesh (pretty) | +132.1% / +205.0% | +21.6% / +23.6% | +21.6% / +27.9% | +25.5% / +38.5% | +28.4% / +21.2% |
| Random (min) | +225.4% / +277.3% | +41.6% / +43.8% | +41.8% / +44.1% | +55.2% / +65.3% | +42.6% / +44.6% |
| Random (pretty) | +259.3% / +258.4% | +40.7% / +41.1% | +38.2% / +41.0% | +38.3% / +70.5% | +37.8% / +43.4% |
| Twitter (min) | +763.0% / +753.7% | +157.5% / +162.5% | +144.4% / +147.2% | +179.1% / +177.4% | +167.9% / +163.7% |
| Twitter (pretty) | +702.1% / +763.0% | +149.7% / +157.0% | +135.8% / +140.5% | +175.8% / +174.5% | +148.3% / +154.7% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +27.0% / +30.4% | +22.6% / +24.6% | +11.0% / +10.2% | +37.0% / +44.3% | +19.3% / +17.1% |
| Canada (pretty) | +44.4% / +38.3% | +24.1% / +23.9% | +8.7% / +8.5% | +21.3% / +26.0% | +9.7% / +6.4% |
| CitmCatalog (min) | +152.1% / +209.2% | +21.4% / +23.4% | +15.8% / +16.1% | +15.6% / +17.9% | +14.1% / +20.1% |
| CitmCatalog (pretty) | +142.3% / +155.4% | +16.6% / +17.5% | +13.1% / +10.2% | tie / +19.6% | −3.9% / +12.7% |
| Discord (min) | +28.7% / +41.4% | −2.0% / −2.0% | −1.8% / −0.6% | +19.3% / +21.1% | −2.7% / +1.4% |
| Discord (pretty) | +28.0% / +39.3% | −4.2% / −2.2% | −3.7% / −3.5% | +16.6% / +8.2% | −14.7% / −7.5% |
| Google Maps (min) | +57.3% / +67.7% | tie / tie | −2.6% / −4.4% | +9.7% / +9.2% | −2.0% / +2.8% |
| Google Maps (pretty) | +52.8% / +60.9% | −1.8% / −1.2% | +0.9% / −1.0% | +10.3% / +27.2% | −2.3% / −5.8% |
| Instruments (min) | +37.8% / +42.4% | −2.7% / +0.5% | −2.7% / −0.3% | tie / +3.0% | −4.3% / −1.0% |
| Instruments (pretty) | +29.6% / +33.8% | −4.3% / −1.6% | −0.7% / tie | +2.3% / +11.9% | −6.4% / −7.6% |
| Marine IK (min) | −1.2% / +4.1% | +11.0% / +11.3% | +20.9% / +20.2% | +17.3% / +17.9% | +12.0% / +14.4% |
| Marine IK (pretty) | +15.6% / +18.2% | +8.8% / +10.0% | +16.2% / +16.5% | +20.1% / +20.3% | +13.2% / +2.6% |
| Mesh (min) | +129.2% / +167.6% | +6.2% / +5.6% | +17.1% / +19.8% | +5.9% / +10.0% | +9.7% / +7.5% |
| Mesh (pretty) | +111.9% / +171.3% | +3.6% / +3.9% | +6.3% / +8.0% | tie / +14.0% | +3.6% / tie |
| Random (min) | +32.7% / +51.1% | +5.2% / +6.1% | +4.1% / +2.5% | +20.9% / +31.2% | +3.0% / +5.9% |
| Random (pretty) | +34.8% / +47.9% | +6.2% / +6.2% | +2.1% / +4.0% | +20.1% / +24.6% | +3.6% / tie |
| Twitter (min) | +95.4% / +103.4% | −11.0% / −8.4% | +1.7% / +1.6% | +13.6% / +25.2% | −2.4% / −2.5% |
| Twitter (pretty) | +91.6% / +98.9% | −9.8% / −10.7% | −2.2% / −2.2% | +20.6% / +7.4% | −11.8% / −5.4% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +144.4% / +159.0% | +23.2% / +23.8% | +18.6% / +15.7% | +31.5% / +32.8% | −11.6% / −9.8% |
| Canada (pretty) | +125.3% / +120.2% | +25.3% / +23.4% | +11.1% / +11.8% | +22.6% / +25.2% | −7.1% / −6.0% |
| CitmCatalog (min) | +159.7% / +161.5% | +19.3% / +17.2% | +27.3% / +29.3% | +29.3% / +8.0% | +4.8% / +9.4% |
| CitmCatalog (pretty) | +142.8% / +168.3% | +16.4% / +14.7% | +19.1% / +21.6% | +45.2% / +33.7% | +4.2% / +9.8% |
| Discord (min) | +143.4% / +172.2% | +2.6% / +4.2% | +6.3% / +8.1% | +25.3% / +36.0% | +10.5% / +7.4% |
| Discord (pretty) | +135.6% / +148.5% | +1.3% / +4.7% | +3.9% / +6.7% | +33.2% / +26.2% | — / −54.3% |
| Google Maps (min) | +77.5% / +80.3% | tie / tie | +8.1% / +8.0% | +8.6% / tie | −6.9% / −4.6% |
| Google Maps (pretty) | +72.5% / +72.8% | −2.8% / tie | +5.7% / +5.7% | +9.6% / +3.5% | −6.3% / −4.2% |
| Instruments (min) | +185.9% / +191.6% | +6.2% / +7.1% | +15.2% / +15.7% | +27.8% / +28.4% | +6.9% / +7.1% |
| Instruments (pretty) | +164.2% / +182.8% | +4.1% / +5.4% | +16.0% / +15.0% | +37.0% / +17.6% | +4.4% / +4.8% |
| Marine IK (min) | +125.9% / +97.1% | +15.5% / +17.4% | +30.6% / +31.9% | +15.7% / +27.8% | +16.0% / +18.7% |
| Marine IK (pretty) | +90.2% / +100.6% | +15.3% / +14.7% | +25.3% / +25.2% | +28.8% / +14.7% | +7.5% / +20.2% |
| Mesh (min) | +127.5% / +205.5% | −1.3% / tie | +12.9% / +14.0% | +15.0% / tie | +7.1% / +8.5% |
| Mesh (pretty) | +118.1% / +162.0% | tie / −2.2% | +6.5% / +7.9% | +3.4% / +8.1% | +4.1% / −2.4% |
| Random (min) | +77.6% / +82.3% | +4.9% / +5.6% | +12.5% / +8.6% | +23.1% / +40.3% | +3.5% / +4.4% |
| Random (pretty) | +66.0% / +99.1% | +4.9% / +4.9% | +8.6% / +8.5% | +21.4% / +27.8% | +5.0% / +3.0% |
| Twitter (min) | +145.7% / +155.1% | −2.5% / −2.3% | +6.0% / +7.4% | +24.3% / +25.3% | +7.8% / +19.4% |
| Twitter (pretty) | +146.7% / +166.8% | tie / −3.5% | +2.6% / +5.0% | +28.0% / tie | +1.8% / −5.2% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 1.9x to 18.7x faster at full size: +92.5% up to +1770.9% (Discord pretty, MSVC, reused). The small documents show the same pattern, from +109.2% to +1453.2%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −8.5% to +17.8%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are a strength.** Jsonifier won 150 of 169 converged full-size sparse-reverse results across both allocation modes (5 ties, 14 losses), and 153 of 179 small ones (9 ties, 17 losses). MSVC won every one of its 72, and Linux/GCC every one of its 36 small results. Of the 14 full-size losses, 8 are on macOS/Clang, 3 on macOS/GCC and 3 on Linux/GCC, and 7 of the 14 are Google Maps.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 32.5 MB/s on MSVC against 222.8 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 545.0 and 793.4 MB/s. Every MSVC reverse result is a win, by +107.0% or more outside Canada (+17.1% to +20.6%); every MSVC full-size sparse result is a win, by +26.8% to +326.9%, and every sparse-reverse result by +69.9% to +318.1%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 150 of 172 converged full-size sparse results (4 ties, 18 losses) and 131 of 180 small ones (8 ties, 41 losses). MSVC won all 36 full-size results and 35 of 36 small ones. On full-size documents, 11 of the 18 losses are on macOS/Clang (Twitter, Google Maps, Instruments and others), 4 are Discord on Linux/Clang, 2 are Google Maps on Linux/GCC and 1 is Mesh on macOS/GCC.
- **In-order full reads are mostly wins at full size.** Across 167 converged full-size in-order results, Jsonifier won 130, tied 9 and lost 28. MSVC went 32-2-2 and Linux/Clang 34-2-0. Linux/GCC split its in-order tests 18-1-17 across the two modes, macOS/GCC 21-3-5 and macOS/Clang 25-1-4.
- **Small documents read in order are the weakest category.** Across 180 converged results, Jsonifier won 124, tied 9 and lost 47. MSVC (34-2-0), macOS/GCC (30-2-4) and macOS/Clang (27-3-6) won most of theirs, but Linux/Clang went 20-1-15 and Linux/GCC 13-1-22. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The Linux/Clang losses there are modest (−0.5% to −6.1%) and Linux/GCC's reach −14.6% on Mesh, while the in-order small-document wins on the same builds on Google Maps and Random run from +7.8% to +20.0%.
- **In-order losses at full size cluster on Linux/GCC.** It lost Canada, Google Maps, Instruments, Marine IK, Mesh and Twitter in order, mostly by −1.7% to −8.0%, but minified Marine IK by −48.6% fresh and −46.1% reused. macOS/GCC lost five: minified Mesh (fresh, −8.6%), and minified Canada (−17.4%), minified CitmCatalog and prettified Marine IK and Mesh with reused objects. Canada, Marine IK and Mesh are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. Linux/Clang lost no full-size in-order test.
- **The POD tests win on most builds.** Jsonifier went 39-3-4 on them. Double won all 8 converged results (+19.4% to +55.6%), Uint64 all 10 (+5.3% to +58.8%) and Int64 7 of 8 (+6.1% to +58.6%, tying on macOS/Clang reused). Bool won 9 of 10 (+1.6% to +60.9%), tying on MSVC fresh. String is the weak POD test: it won 5 of 10 (+4.6% to +18.4%), tied 1 (MSVC, fresh) and lost 4 (MSVC reused, −8.2%; macOS/GCC fresh, −3.3%; macOS/Clang fresh, −23.0%, and reused, −17.9%).

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

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 6.4% to 23.3% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.44 | AVX2 | 22 / 26 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 26 / 26 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 26 / 26 |
| macOS 25.6 (M1, virtual) / GCC 16.2 | NEON | 13 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 15 / 26 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 6-0-0 | 22-0-0 |
| Linux / Clang | 4-0-2 | 10-0-0 | 9-0-1 | 23-0-3 |
| Linux / GCC | 6-0-0 | 8-0-2 | 8-0-2 | 22-0-4 |
| macOS / GCC | 5-0-0 | 3-1-0 | 4-0-0 | 12-1-0 |
| macOS / Clang | 3-1-1 | 6-1-0 | 3-0-0 | 12-2-1 |
| **All** | **24-1-3** | **37-2-2** | **30-0-3** | **91-3-8** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +46.0% | +7.4% | +5.4% | +29.0% | +11.4% |
| Large Amazon Cellphones (NDJSON) | +43.4% | +5.3% | +4.2% | +10.0% | tie |
| Stream Formats Small (NDJSON) | +29.2% | −5.7% | +6.6% | +4.0% | — |
| Stream Formats Small (comma) | +19.5% | +81.2% | +101.5% | +78.1% | +42.5% |
| Stream Formats Large (NDJSON) | +22.6% | −1.0% | +3.1% | +11.1% | −12.4% |
| Stream Formats Large (comma) | +22.4% | +5.8% | +11.4% | — | +8.0% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +39.8% | +1.7% | +3.6% | — | +15.6% |
| CitmCatalog (comma) | +36.7% | +15.8% | +23.9% | +16.8% | +15.0% |
| Google Maps (NDJSON) | +40.8% | +33.8% | +14.0% | — | — |
| Google Maps (comma) | +29.5% | +36.3% | +35.3% | — | tie |
| Instruments (NDJSON) | +73.5% | +20.6% | −10.1% | +6.3% | +4.0% |
| Instruments (comma) | +54.0% | +40.6% | +22.6% | +24.5% | +5.5% |
| Random (NDJSON) | +63.0% | +23.3% | +11.3% | — | — |
| Random (comma) | +52.1% | +40.4% | +34.4% | — | — |
| Twitter (NDJSON) | +37.7% | +5.6% | −5.6% | tie | +7.1% |
| Twitter (comma) | +30.0% | +12.8% | +10.3% | — | +28.8% |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | — | +35.6% | +44.3% | — | — |
| CitmCatalog (comma) | — | +49.9% | +67.6% | +45.8% | +54.1% |
| Google Maps (NDJSON) | +61.9% | +14.5% | +13.9% | +15.6% | +16.5% |
| Google Maps (comma) | +54.3% | +19.7% | +33.8% | — | +4.7% |
| Instruments (NDJSON) | +67.4% | −6.8% | −22.0% | — | — |
| Instruments (comma) | +72.0% | +9.7% | −1.8% | — | — |
| Random (NDJSON) | +248.5% | +51.1% | +34.5% | — | — |
| Random (comma) | +270.3% | +63.0% | +54.0% | +68.3% | — |
| Twitter (NDJSON) | — | +222.9% | +135.9% | — | — |
| Twitter (comma) | — | +234.2% | +160.7% | +185.5% | — |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 24 of the 28 converged results in that group, tied 1 and lost 3. It won Amazon Cellphones on all five builds (+5.4% to +46.0%), won Large Amazon Cellphones on the four builds other than macOS/Clang (+4.2% to +43.4%) and tied it there, and won Stream Formats Small (comma) on all five builds (+19.5% to +101.5%). All three losses are Stream Formats in NDJSON form: Small on Linux/Clang (−5.7%) and Large on Linux/Clang (−1.0%) and macOS/Clang (−12.4%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test that converged,** 22-0-0; Linux/Clang lost three of 26 and Linux/GCC four.
- **In-order corpus streams are mostly wins.** Of the 41 converged in-order corpus results, Jsonifier won 37, tied 2 and lost 2, both NDJSON on Linux/GCC: Instruments (−10.1%) and Twitter (−5.6%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 30 of 33 converged reverse stream results. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +135.9% to +234.2%, while Google Maps rows, which have few, give +4.7% to +61.9%. All three losses are Instruments: NDJSON on Linux/Clang (−6.8%) and Linux/GCC (−22.0%), and comma-separated on Linux/GCC (−1.8%).
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 118.9 MB/s under MSVC and 441.1 MB/s under Linux/Clang, while Jsonifier reads it at 440.4 and 718.9 MB/s.

## 9. Limitations

- **Linux/GCC and macOS/Clang hold most of the single-document losses:** 65 and 59 of the 180, against 38 on Linux/Clang, 14 on macOS/GCC and 4 on MSVC. macOS/Clang's are spread across access patterns and concentrated in sparse reads: 15 small sparse, 11 small sparse reverse, 11 full-size sparse and 8 full-size sparse reverse. It also holds the largest individual losses: Twitter Small read in order (pretty, reused) at −65.7% and Discord Small Sparse Reverse (pretty, reused) at −54.3%. It ran on a virtualized M1, so part of these results likely reflects host noise.
- **Linux/GCC loses mostly on reads in document order.** 39 of its 65 losses are in-order reads: 22 of its 36 converged small in-order results and 17 of its 36 full-size ones. At full size they are on Canada, Google Maps, Instruments, Marine IK, Mesh and Twitter, mostly −1.7% to −8.0%, with minified Marine IK at −48.6% (fresh) and −46.1% (reused); at small size they reach −14.6% (Mesh Small, pretty, reused). It also loses reverse reads of Canada at both sizes (−3.2% to −8.5%), while it won every small sparse-reverse result.
- **Small documents read in order lose on Linux.** Linux/Clang lost 15 of its 36 converged small in-order results and Linux/GCC 22 of 36, while Linux/Clang lost none of its 36 full-size in-order tests. Linux/Clang also lost 13 of its 36 small sparse in-order results and 6 of its 36 small sparse-reverse ones. At 5 KiB per document, Jsonifier's fixed per-parse cost is the next target.
- **Number-heavy documents in order** remain a common loss. Canada, Marine IK and Mesh lose in order on Linux/GCC, and Canada, Marine IK and Mesh on macOS/GCC. The cause is float-array throughput rather than out-of-order access, so it is out of scope for the field index.
- **Instruments streams** lose on Linux: read in order in NDJSON form on Linux/GCC (−10.1%), and in reverse in NDJSON form on Linux/Clang (−6.8%) and Linux/GCC (−22.0%) and in comma form on Linux/GCC (−1.8%), although the single-document Instruments Reverse tests win on every build that converged.
- **Convergence is uneven.** Linux/Clang converged on all 298 single-document results, MSVC on 294 and Linux/GCC on 289, but macOS/GCC met the limits on only 121 of its 154 full-size single-document results and macOS/Clang on 128, even with the looser NEON limits. The NEON builds converged on only 13 and 15 of the 26 streaming tests, and MSVC on 22.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. On the x86 in-order corpus streams this leaves comma mode 3.7% to 25.4% slower than NDJSON mode on the same records. The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commits.** The byte-for-byte comparison in §6 ran on `3e7bf6d`, not on the commits measured here, and it does not cover the small-document or reused tests.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,417 converged single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,187, tied 50 and lost 180. On reverse reads it won 310 of 324 at both sizes, and on Windows/MSVC it lost 4 single-document results of 294, by 1.2% to 8.2%. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (593-23-93 fresh, 594-27-87 reused). The main remaining targets are Linux/GCC, with 65 of the 180 losses, macOS/Clang, with 59 on a virtualized host, and small documents read in order on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. The one lesson it added repeats §5's: finding every document's end up front makes an in-order reader pay for two passes over the tape, and letting the reader's own cursor report where each document ended removes that pass for whitespace-delimited streams. Across 102 converged streaming results, Jsonifier won 91, tied 3 and lost 8, and it won every converged streaming test on Windows/MSVC. Instruments streams on Linux and the Stream Formats NDJSON tests are the main remaining streaming targets.

The main engineering lesson is in §5. An index over the whole document made the target workload 2.3x *slower*, because most objects are small. The version that works builds the index lazily, per object, and only once that object has shown both that it is large and that it is being read out of order: enough skipped keys to clear *T*, and two lookups that had to wrap around. In short: make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.