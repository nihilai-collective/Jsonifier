# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 104 of the 110 minified documents (the tape takes five, four of them under MSVC, and one is a tie), the tape is faster on 63 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 37 of the 44 prettified documents, the tape 5 and 2 are ties. Three AVX-512 builds show the same split: the fused path wins 61 of their 66 minified documents and the tape 64 of their 66 prettified ones.

This paper describes both halves of that architecture: the single-pass primary path, and Jsonifier's stage-1 implementation, including its per-compiler-tuned step geometry, its fold-expression drain architecture for bitmask-to-index extraction, and its distributed UTF-8 validation strategy. Throughout, it compares both directly with simdjson's current design.

---

## 1. Background: the canonical two-stage model

simdjson's pipeline [1, 2] is well documented. Stage 1 sweeps the input in 64-byte blocks, computing per-block bitmasks: backslashes, odd-length escape terminations, quotes, an in-string range mask derived via carry-less multiplication (prefix XOR), structural characters (`{ } [ ] : ,`), whitespace, and pseudo-structural scalar starts. The surviving bits are converted to byte offsets, the *structural tape*, via a tzcnt/blsr extraction loop. Stage 2 then walks the tape to build a DOM or, in the On Demand API, lazily materializes only the values the caller touches.

The model's strength is that stage 1 is branch-free and data-parallel; its cost is that it is unconditional. Every byte of every document is classified and indexed before a single value is parsed, regardless of whether the caller's access pattern needs the index at all.

## 2. The Jsonifier position: pay for the tape only when the tape pays for you

Jsonifier's primary API parses JSON directly into user-defined structs registered through compile-time reflection (`jsonifier::core<T>`). In that setting the parser already knows, at compile time, the expected shape of the document: which keys exist, their serialized order, their types. The parse loop is a schema-directed walk, not a blind tree construction.

Consider what the structural tape provides: the positions of every brace, bracket, colon, comma, quote, and scalar start. Now consider what a schema-directed single-pass parser does at each of those same positions: it is *already there*, with its iterator parked on that exact byte, consuming it as part of matching a known key literal or delimiting a value. The tape's information is a strict subset of what the fused pass discovers for free in program order. Building it first means that, for a full-document parse in which every value is materialized:

- every byte of the input is loaded twice (once in stage 1, once in stage 2),
- the tape itself is written and re-read (cache traffic proportional to structural density), and
- stage 2's control flow is driven by indirect loads through the index array rather than by a pointer walking linearly through memory the prefetcher already understands.

(We scope the double-load claim deliberately: a two-stage consumer that *skips* content, such as simdjson's On Demand API skipping unrequested fields, does not reload the skipped bytes in its second pass. That is exactly the workload where the tape earns its keep, and exactly why Jsonifier retains the two-stage machinery for partial reading. The accounting above describes the full-materialization case, where nothing is skipped and the tape's information is fully redundant with the walk.)

For full-document parsing into known types, the tape is overhead in principle, and in practice whether skipping it is faster comes down to two questions: is the input indented, and is the machine x86? §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying: workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One objection is that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads (schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents), this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns, parsing documents into concrete types the caller has defined, the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is legibility: as hand-written traversal the knowledge is opaque to the library, while as a reflection registration it becomes architecture (fused member headers, adaptive order recovery, and the routing rule above). It is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep [5] published November 14, 2026, with Jsonifier [d4727ec](https://github.com/nihilai-collective/jsonifier/commit/d4727ec), Glaze [a53ddd5](https://github.com/stephenberry/glaze/commit/a53ddd59), simdjson [449f38e](https://github.com/simdjson/simdjson/commit/449f38e63) and BenchmarkSuite [27950a9](https://github.com/nihilai-collective/benchmarksuite/commit/27950a9) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Three freshly allocated full-parse rows contain non-converged results (one each under MSVC, on macOS/GCC and on macOS/Clang), and every reused row converged. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 169 tests, 3 are statistical ties, and the two-stage path wins 73. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 24 | 0 | 25 | 48 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 44 | 2 | 3 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 46 | 1 | 2 | 48 of 49 |
| **Aggregate** | **169** | **3** | **73** | **242 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 21 / 0 / 1 | 21 / 1 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 22 / 0 / 0 |
| Small minified documents | 8 / 0 / 3 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 0 / 0 / 11 | 1 / 0 / 21 | 15 / 2 / 5 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 48 / 0 / 1 | 48 / 0 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **244 / 0 / 1** | **242 / 1 / 2** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 23 | 1 | 25 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 46 | 0 | 3 | 49 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 46 | 0 | 3 | 49 of 49 |
| **Aggregate** | **171** | **1** | **73** | **245 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 1 / 10 | 1 / 0 / 21 | 22 / 0 / 0 |
| Small minified documents | 8 / 0 / 3 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 0 / 0 / 11 | 1 / 0 / 21 | 16 / 0 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **243 / 0 / 2** |

With objects reused, across 245 tests the fused path wins 171, ties 1 and loses 73 to the two-stage path, and the x86 split holds: the tape wins 31 of the 33 full-size prettified documents (the fused path takes Canada on Linux/GCC, and Marine IK Reverse under MSVC is a tie) and 32 of the 33 Small prettified ones (the fused path takes Canada Small on Linux/GCC), and the fused path wins 62 of the 66 minified documents, the tape taking Marine IK Reverse and the Small copies of Apache Builds, Marine IK Reverse and Marine IK under MSVC. On the M2 Pro the fused path wins all 44 minified documents and 38 of the 44 prettified ones, with 6 tape wins. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins 243, ties none and loses 2.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 660 | 265 | 121 | 2.49× | Fused | Win |
| Double (POD) | 946 | 544 | 261 | 1.74× | Fused | Win |
| Int64 (POD) | 1,590 | 877 | 459 | 1.81× | Fused | Win |
| String (POD) | 1,012 | 790 | 752 | 1.28× | Fused | Win |
| Uint64 (POD) | 1,686 | 916 | 458 | 1.84× | Fused | Win |
| Apache Builds (minified) | 911 | 842 | 726 | 1.08× | Fused | Win |
| Canada (minified) | 723 | 689 | 432 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,081 | 1,068 | 953 | 1.01× | Fused | Win |
| Canada (prettified) | 1,486 | 1,567 | 998 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 1,636 | 1,309 | 629 | 1.25× | Fused | Win |
| CitmCatalog (prettified) | 2,376 | 2,671 | 1,361 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,354 | 1,093 | 769 | 1.24× | Fused | Win |
| Discord (prettified) | 1,440 | 1,574 | 1,115 | 0.92× | Two-stage | Win |
| Github Events (minified) | 1,081 | 963 | 726 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 1,372 | 1,185 | 599 | 1.16× | Fused | Win |
| Github Events (prettified) | 1,085 | 1,162 | 900 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 2,209 | 2,318 | 1,212 | 0.95× | Two-stage | Win |
| Instruments (minified) | 2,232 | 1,502 | 877 | 1.49× | Fused | Win |
| Instruments (prettified) | 2,162 | 2,364 | 1,428 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 459 | 562 | 197 | 0.82× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,959 | 2,047 | 700 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 669 | 581 | 352 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,009 | 2,087 | 1,318 | 0.96× | Two-stage | Win |
| Random (minified) | 843 | 751 | 585 | 1.12× | Fused | Win |
| Random (prettified) | 1,053 | 1,157 | 893 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,233 | 994 (n/c) | 901 (n/c) | 1.24× | Fused | Win |
| Twitter (prettified) | 1,391 | 1,487 | 1,248 | 0.94× | Two-stage | Win |
| Apache Builds Small (minified) | 1,267 | 1,310 | 762 | 0.97× | Two-stage | Win |
| Canada Small (minified) | 680 | 642 | 362 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,345 | 1,663 | 967 | 0.81× | Two-stage | Win |
| Canada Small (prettified) | 1,421 | 1,497 | 859 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 884 | 750 | 561 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,183 | 1,328 | 978 | 0.89× | Two-stage | Win |
| Discord Small (minified) | 1,207 | 965 | 725 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,202 | 1,303 | 983 | 0.92× | Two-stage | Win |
| Github Events Small (minified) | 1,186 | 897 | 759 | 1.32× | Fused | Win |
| Google Maps Response Small (minified) | 1,227 | 1,071 | 590 | 1.15× | Fused | Win |
| Github Events Small (prettified) | 1,079 | 1,327 | 875 | 0.81× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,955 | 2,114 | 1,193 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,313 | 1,479 | 881 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 2,129 | 2,337 | 1,402 | 0.91× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 353 | 463 | 165 | 0.76× | Two-stage | Win |
| Marine IK Reverse Small (prettified) | 1,396 | 1,656 | 629 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 475 | 493 | 317 | 0.96× | Two-stage | Win |
| Marine IK Small (prettified) | 1,566 | 1,805 | 1,179 | 0.87× | Two-stage | Win |
| Random Small (minified) | 971 | 852 | 554 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,177 | 1,313 | 856 | 0.90× | Two-stage | Win |
| Twitter Small (minified) | 1,793 | 1,431 | 1,363 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 1,553 | 1,799 | 1,694 | 0.86× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,990 | 624 | 290 | 3.19× | Fused | Win |
| Double (POD) | 1,318 | 750 | 360 | 1.76× | Fused | Win |
| Int64 (POD) | 3,134 | 1,659 | 780 | 1.89× | Fused | Win |
| String (POD) | 1,789 | 1,343 | 1,412 | 1.33× | Fused | Win |
| Uint64 (POD) | 3,216 | 1,592 | 719 | 2.02× | Fused | Win |
| Apache Builds (minified) | 1,719 | 1,472 | 1,201 | 1.17× | Fused | Win |
| Canada (minified) | 1,028 | 897 | 793 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 1,744 | 1,877 | 1,560 | 0.93× | Two-stage | Win |
| Canada (prettified) | 1,979 | 2,115 | 1,774 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,206 | 1,646 | 1,119 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 2,868 | 3,317 | 2,336 | 0.86× | Two-stage | Win |
| Discord (minified) | 2,221 | 1,730 | 949 | 1.28× | Fused | Win |
| Discord (prettified) | 1,694 | 2,519 | 1,379 | 0.67× | Two-stage | Win |
| Github Events (minified) | 1,654 | 1,772 | 1,012 | 0.93× | Two-stage | Win |
| Google Maps Response (minified) | 2,466 | 1,860 | 1,063 | 1.33× | Fused | Win |
| Github Events (prettified) | 1,540 | 1,913 | 1,267 | 0.80× | Two-stage | Win |
| Google Maps Response (prettified) | 2,924 | 3,415 | 2,174 | 0.86× | Two-stage | Win |
| Instruments (minified) | 2,464 | 1,829 | 764 | 1.35× | Fused | Win |
| Instruments (prettified) | 2,159 | 2,806 | 1,271 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 831 | 722 | 575 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,463 | 2,532 | 2,078 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 845 | 733 | 561 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,461 | 2,601 | 2,072 | 0.95× | Two-stage | Win |
| Random (minified) | 1,266 | 1,102 | 825 | 1.15× | Fused | Win |
| Random (prettified) | 1,392 | 1,700 | 1,254 | 0.82× | Two-stage | Win |
| Twitter (minified) | 1,836 | 1,693 | 933 | 1.08× | Fused | Win |
| Twitter (prettified) | 1,578 | 2,189 | 1,234 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 2,436 | 2,172 | 1,369 | 1.12× | Fused | Win |
| Canada Small (minified) | 1,012 | 906 | 749 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,095 | 2,617 | 1,721 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 1,912 | 2,171 | 1,781 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,639 | 1,370 | 975 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 1,803 | 2,343 | 1,728 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 2,020 | 1,776 | 772 | 1.14× | Fused | Win |
| Discord Small (prettified) | 1,643 | 2,341 | 1,049 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 2,015 | 1,766 | 1,382 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 2,275 | 1,716 | 1,074 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,863 | 2,226 | 1,747 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,673 | 3,229 | 2,160 | 0.83× | Two-stage | Win |
| Instruments Small (minified) | 2,482 | 1,862 | 659 | 1.33× | Fused | Win |
| Instruments Small (prettified) | 2,082 | 2,810 | 1,097 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 808 | 704 | 502 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,163 | 2,471 | 1,866 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 880 | 756 | 511 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,256 | 2,655 | 1,878 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,714 | 1,539 | 840 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,689 | 2,333 | 1,292 | 0.72× | Two-stage | Win |
| Twitter Small (minified) | 2,447 | 2,175 | 1,029 | 1.13× | Fused | Win |
| Twitter Small (prettified) | 1,854 | 2,647 | 1,310 | 0.70× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,268 | 599 | 228 | 3.79× | Fused | Win |
| Double (POD) | 1,272 | 712 | 303 | 1.79× | Fused | Win |
| Int64 (POD) | 3,399 | 1,633 | 726 | 2.08× | Fused | Win |
| String (POD) | 1,875 | 1,363 | 1,303 | 1.38× | Fused | Win |
| Uint64 (POD) | 3,500 | 1,611 | 722 | 2.17× | Fused | Win |
| Apache Builds (minified) | 1,655 | 1,435 | 1,355 | 1.15× | Fused | Win |
| Canada (minified) | 1,170 | 1,013 | 749 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 1,785 | 1,835 | 1,762 | 0.97× | Two-stage | Win |
| Canada (prettified) | 2,286 | 2,188 | 1,717 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,370 | 1,791 | 1,430 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 3,420 | 3,574 | 3,024 | 0.96× | Two-stage | Win |
| Discord (minified) | 2,292 | 1,847 | 1,340 | 1.24× | Fused | Win |
| Discord (prettified) | 2,227 | 2,577 | 1,906 | 0.86× | Two-stage | Win |
| Github Events (minified) | 1,674 | 1,558 | 1,433 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 2,494 | 1,808 | 1,115 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,713 | 1,878 | 1,618 | 0.91× | Two-stage | Win |
| Google Maps Response (prettified) | 3,298 | 3,369 | 2,264 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,206 | 2,237 | 1,447 | 1.43× | Fused | Win |
| Instruments (prettified) | 2,900 | 3,354 | 2,339 | 0.86× | Two-stage | Win |
| Marine IK Reverse (minified) | 856 | 723 | 516 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,575 | 2,624 | 1,963 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 861 | 737 | 524 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,608 | 2,644 | 1,936 | 0.99× | Two-stage | Win |
| Random (minified) | 1,194 | 1,090 | 978 | 1.10× | Fused | Win |
| Random (prettified) | 1,548 | 1,721 | 1,544 | 0.90× | Two-stage | Win |
| Twitter (minified) | 2,010 | 1,700 | 1,139 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,948 | 2,159 | 1,491 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 2,394 | 2,039 | 1,648 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,105 | 916 | 672 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 2,283 | 2,516 | 2,071 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 2,216 | 2,145 | 1,606 | 1.03× | Fused | Win |
| CitmCatalog Small (minified) | 1,732 | 1,433 | 1,151 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 2,159 | 2,423 | 1,993 | 0.89× | Two-stage | Win |
| Discord Small (minified) | 2,025 | 1,682 | 1,258 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,975 | 2,172 | 1,710 | 0.91× | Two-stage | Win |
| Github Events Small (minified) | 2,144 | 1,725 | 1,569 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,264 | 1,699 | 1,103 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,979 | 2,124 | 1,896 | 0.93× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,041 | 3,198 | 2,222 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,283 | 2,217 | 1,341 | 1.48× | Fused | Win |
| Instruments Small (prettified) | 2,822 | 3,375 | 2,166 | 0.84× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 865 | 753 | 498 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,349 | 2,611 | 1,840 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 951 | 781 | 486 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,505 | 2,705 | 1,849 | 0.93× | Two-stage | Win |
| Random Small (minified) | 1,563 | 1,487 | 1,061 | 1.05× | Fused | Win |
| Random Small (prettified) | 1,920 | 2,199 | 1,611 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,894 | 2,312 | 1,315 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,376 | 2,914 | 1,711 | 0.82× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,309 | 320 | 150 | 4.09× | Fused | Win |
| Double (POD) | 902 | 481 | 140 | 1.87× | Fused | Win |
| Int64 (POD) | 2,362 | 862 | 387 | 2.74× | Fused | Win |
| String (POD) | 926 | 673 | 706 | 1.38× | Fused | Win |
| Uint64 (POD) | 1,819 | 671 | 416 | 2.71× | Fused | Win |
| Apache Builds (minified) | 1,060 | 848 | 599 | 1.25× | Fused | Win |
| Canada (minified) | 842 | 584 | 514 | 1.44× | Fused | Win |
| Apache Builds (prettified) | 1,180 | 1,166 | 894 | 1.01× | Fused | Win |
| Canada (prettified) | 1,437 | 1,318 | 1,155 | 1.09× | Fused | Win |
| CitmCatalog (minified) | 1,913 | 1,097 (n/c) | 922 | 1.74× | Fused | Win |
| CitmCatalog (prettified) | 2,764 | 2,406 | 2,037 | 1.15× | Fused | Win |
| Discord (minified) | 1,692 | 1,165 | 918 | 1.45× | Fused | Win |
| Discord (prettified) | 1,850 | 1,601 | 1,279 | 1.16× | Fused | Win |
| Github Events (minified) | 1,532 | 1,334 | 953 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,817 | 1,207 | 685 | 1.51× | Fused | Win |
| Github Events (prettified) | 1,595 | 1,510 | 1,137 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,433 | 2,153 | 1,307 | 1.13× | Fused | Win |
| Instruments (minified) | 2,407 | 1,398 | 1,114 | 1.72× | Fused | Win |
| Instruments (prettified) | 2,653 | 2,039 | 1,695 | 1.30× | Fused | Win |
| Marine IK Reverse (minified) | 634 | 503 | 387 | 1.26× | Fused | Win |
| Marine IK Reverse (prettified) | 1,854 | 1,764 | 1,431 | 1.05× | Fused | Win |
| Marine IK (minified) | 669 | 535 | 417 | 1.25× | Fused | Win |
| Marine IK (prettified) | 1,986 | 1,623 | 1,466 | 1.22× | Fused | Win |
| Random (minified) | 946 | 821 | 598 | 1.15× | Fused | Win |
| Random (prettified) | 1,270 | 1,100 | 905 | 1.15× | Fused | Win |
| Twitter (minified) | 1,713 | 1,384 | 941 | 1.24× | Fused | Win |
| Twitter (prettified) | 2,179 | 1,899 | 1,275 | 1.15× | Fused | Win |
| Apache Builds Small (minified) | 1,518 | 1,380 | 852 | 1.10× | Fused | Win |
| Canada Small (minified) | 755 | 623 | 411 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 1,523 | 1,673 | 979 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 1,584 | 1,529 | 965 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 1,166 | 989 | 824 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,700 | 1,589 | 1,307 | 1.07× | Fused | Win |
| Discord Small (minified) | 1,497 | 1,262 | 888 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,606 | 1,634 | 1,192 | 0.98× | Tie | Win |
| Github Events Small (minified) | 1,392 | 1,101 | 1,128 | 1.26× | Fused | Win |
| Google Maps Response Small (minified) | 1,700 | 1,200 | 722 | 1.42× | Fused | Win |
| Github Events Small (prettified) | 1,248 | 1,348 | 1,402 | 0.93× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,421 | 2,215 | 1,359 | 1.09× | Fused | Win |
| Instruments Small (minified) | 2,619 | 1,614 | 1,072 | 1.62× | Fused | Win |
| Instruments Small (prettified) | 2,697 | 2,357 | 1,526 | 1.14× | Fused | Win |
| Marine IK Reverse Small (minified) | 639 | 529 | 340 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,708 | 1,802 | 1,319 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 691 | 586 | 361 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,973 | 1,832 | 1,233 | 1.08× | Fused | Win |
| Random Small (minified) | 1,143 | 1,035 | 658 | 1.10× | Fused | Win |
| Random Small (prettified) | 1,551 | 1,184 | 917 | 1.31× | Fused | Win |
| Twitter Small (minified) | 2,140 | 1,744 | 1,055 | 1.23× | Fused | Win |
| Twitter Small (prettified) | 2,102 | 2,088 | 1,355 | 1.01× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,267 | 346 | 185 | 3.66× | Fused | Win |
| Double (POD) | 912 | 375 | 194 | 2.43× | Fused | Win |
| Int64 (POD) | 2,026 | 863 | 495 | 2.35× | Fused | Win |
| String (POD) | 1,398 | 749 | 877 | 1.87× | Fused | Win |
| Uint64 (POD) | 2,357 | 845 | 472 | 2.79× | Fused | Win |
| Apache Builds (minified) | 1,572 | 1,089 | 869 | 1.44× | Fused | Win |
| Canada (minified) | 833 | 740 | 537 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,644 | 1,359 | 1,112 | 1.21× | Fused | Win |
| Canada (prettified) | 1,669 | 1,586 | 1,190 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,169 | 1,442 | 856 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | 3,014 | 2,826 | 1,785 | 1.07× | Fused | Win |
| Discord (minified) | 2,552 | 1,303 | 762 | 1.96× | Fused | Win |
| Discord (prettified) | 2,380 | 1,785 | 1,108 | 1.33× | Fused | Win |
| Github Events (minified) | 1,855 | 1,483 | 877 | 1.25× | Fused | Win |
| Google Maps Response (minified) | 2,212 | 1,206 | 806 | 1.83× | Fused | Win |
| Github Events (prettified) | 1,825 | 1,730 | 1,054 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,940 | 2,307 | 1,632 | 1.27× | Fused | Win |
| Instruments (minified) | 2,739 | 1,723 | 678 | 1.59× | Fused | Win |
| Instruments (prettified) | 2,828 | 2,528 | 1,117 | 1.12× | Fused | Win |
| Marine IK Reverse (minified) | 689 | 599 | 416 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,130 | 2,029 | 1,238 | 1.05× | Fused | Win |
| Marine IK (minified) | 605 | 609 | 417 | 0.99× | Tie | Win |
| Marine IK (prettified) | 2,159 | 1,954 (n/c) | 1,466 | 1.10× | Fused | Win |
| Random (minified) | 1,294 | 989 | 658 | 1.31× | Fused | Win |
| Random (prettified) | 1,608 | 1,240 | 992 | 1.30× | Fused | Win |
| Twitter (minified) | 2,276 | 1,756 | 839 | 1.30× | Fused | Win |
| Twitter (prettified) | 2,276 | 2,188 | 1,096 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,613 | 1,351 | 942 | 1.19× | Fused | Win |
| Canada Small (minified) | 755 | 687 | 453 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 1,700 | 1,636 | 1,174 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,581 | 1,524 | 1,059 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 1,264 | 973 | 704 | 1.30× | Fused | Win |
| CitmCatalog Small (prettified) | 1,731 | 1,603 | 1,228 | 1.08× | Fused | Win |
| Discord Small (minified) | 2,167 | 1,269 | 640 | 1.71× | Fused | Win |
| Discord Small (prettified) | 2,129 | 1,650 | 872 | 1.29× | Fused | Win |
| Github Events Small (minified) | 1,712 | 1,257 | 1,067 | 1.36× | Fused | Win |
| Google Maps Response Small (minified) | 2,130 | 1,183 | 805 | 1.80× | Fused | Win |
| Github Events Small (prettified) | 1,820 | 1,560 | 1,306 | 1.17× | Fused | Win |
| Google Maps Response Small (prettified) | 2,780 | 2,242 | 1,606 | 1.24× | Fused | Win |
| Instruments Small (minified) | 2,852 | 1,763 | 614 | 1.62× | Fused | Win |
| Instruments Small (prettified) | 2,868 | 2,581 | 1,019 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 660 | 548 | 332 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,890 | 1,903 | 1,207 | 0.99× | Two-stage | Win |
| Marine IK Small (minified) | 705 | 580 | 328 | 1.21× | Fused | Win |
| Marine IK Small (prettified) | 1,937 | 1,957 | 1,219 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,500 | 1,098 | 671 | 1.37× | Fused | Win |
| Random Small (prettified) | 1,779 | 1,324 | 1,017 | 1.34× | Fused | Win |
| Twitter Small (minified) | 2,326 | 1,846 | 848 | 1.26× | Fused | Win |
| Twitter Small (prettified) | 2,343 | 2,238 | 1,097 | 1.05× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 740 | 273 | 123 | 2.71× | Fused | Win |
| Double (POD) | 1,204 | 630 | 283 | 1.91× | Fused | Win |
| Int64 (POD) | 1,994 | 989 | 463 | 2.02× | Fused | Win |
| String (POD) | 1,737 | 1,175 | 1,081 | 1.48× | Fused | Win |
| Uint64 (POD) | 1,947 | 1,020 | 471 | 1.91× | Fused | Win |
| Apache Builds (minified) | 1,480 | 1,332 | 1,074 | 1.11× | Fused | Win |
| Canada (minified) | 891 | 831 | 486 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,660 | 1,695 | 1,377 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,761 | 1,865 | 1,100 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,947 | 1,498 | 726 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 2,659 | 3,024 | 1,548 | 0.88× | Two-stage | Win |
| Discord (minified) | 2,100 | 1,546 | 999 | 1.36× | Fused | Win |
| Discord (prettified) | 1,927 | 2,188 | 1,435 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,682 | 1,433 | 957 | 1.17× | Fused | Win |
| Google Maps Response (minified) | 1,594 | 1,345 | 652 | 1.19× | Fused | Win |
| Github Events (prettified) | 1,620 | 1,710 | 1,145 | 0.95× | Two-stage | Win |
| Google Maps Response (prettified) | 2,445 | 2,545 | 1,333 | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,929 | 1,777 | 992 | 1.65× | Fused | Win |
| Instruments (prettified) | 2,544 | 2,780 | 1,598 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 497 | 624 | 204 | 0.80× | Two-stage | Win |
| Marine IK Reverse (prettified) | 2,042 | 2,020 | 650 | 1.01× | Tie | Win |
| Marine IK (minified) | 743 | 642 | 379 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,219 | 2,285 | 1,401 | 0.97× | Two-stage | Win |
| Random (minified) | 1,186 | 1,020 | 739 | 1.16× | Fused | Win |
| Random (prettified) | 1,377 | 1,567 | 1,111 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,142 | 1,657 | 1,267 | 1.29× | Fused | Win |
| Twitter (prettified) | 1,886 | 2,086 | 1,637 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 1,681 | 1,699 | 1,213 | 0.99× | Two-stage | Win |
| Canada Small (minified) | 859 | 799 | 395 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,700 | 2,068 | 1,548 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 1,725 | 1,792 | 969 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,213 | 977 | 717 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 1,482 | 1,703 | 1,247 | 0.87× | Two-stage | Win |
| Discord Small (minified) | 1,915 | 1,309 | 1,064 | 1.46× | Fused | Win |
| Discord Small (prettified) | 1,692 | 1,809 | 1,399 | 0.94× | Two-stage | Win |
| Github Events Small (minified) | 1,781 | 1,457 | 852 | 1.22× | Fused | Win |
| Google Maps Response Small (minified) | 1,476 | 1,257 | 679 | 1.17× | Fused | Win |
| Github Events Small (prettified) | 1,460 | 1,776 | 1,150 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,217 | 2,416 | 1,375 | 0.92× | Two-stage | Win |
| Instruments Small (minified) | 2,678 | 1,660 | 968 | 1.61× | Fused | Win |
| Instruments Small (prettified) | 2,267 | 2,553 | 1,590 | 0.89× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 410 | 561 | 175 | 0.73× | Two-stage | Win |
| Marine IK Reverse Small (prettified) | 1,651 | 1,988 | 667 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 583 | 608 | 369 | 0.96× | Two-stage | Win |
| Marine IK Small (prettified) | 1,900 | 2,171 | 1,366 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,209 | 1,034 | 700 | 1.17× | Fused | Win |
| Random Small (prettified) | 1,401 | 1,588 | 1,077 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,243 | 1,666 | 1,514 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 1,815 | 2,127 | 1,932 | 0.85× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,124 | 639 | 289 | 3.32× | Fused | Win |
| Double (POD) | 1,444 | 808 | 369 | 1.79× | Fused | Win |
| Int64 (POD) | 3,474 | 1,778 | 793 | 1.95× | Fused | Win |
| String (POD) | 2,872 | 1,813 | 1,961 | 1.58× | Fused | Win |
| Uint64 (POD) | 3,536 | 1,663 | 724 | 2.13× | Fused | Win |
| Apache Builds (minified) | 2,801 | 2,300 | 1,649 | 1.22× | Fused | Win |
| Canada (minified) | 1,246 | 1,070 | 951 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 2,484 | 2,854 | 2,109 | 0.87× | Two-stage | Win |
| Canada (prettified) | 2,338 | 2,487 | 2,105 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,683 | 1,937 | 1,291 | 1.39× | Fused | Win |
| CitmCatalog (prettified) | 3,167 | 3,832 | 2,685 | 0.83× | Two-stage | Win |
| Discord (minified) | 3,175 | 2,389 | 1,112 | 1.33× | Fused | Win |
| Discord (prettified) | 2,082 | 3,386 | 1,619 | 0.61× | Two-stage | Win |
| Github Events (minified) | 2,273 | 2,080 | 1,141 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,813 | 2,048 | 1,129 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,965 | 2,504 | 1,365 | 0.78× | Two-stage | Win |
| Google Maps Response (prettified) | 3,097 | 3,675 | 2,300 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,896 | 2,047 | 796 | 1.41× | Fused | Win |
| Instruments (prettified) | 2,328 | 3,124 | 1,320 | 0.75× | Two-stage | Win |
| Marine IK Reverse (minified) | 921 | 768 | 633 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,637 | 2,762 | 2,318 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 949 | 808 | 625 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,670 | 2,819 | 2,292 | 0.95× | Two-stage | Win |
| Random (minified) | 1,767 | 1,493 | 1,025 | 1.18× | Fused | Win |
| Random (prettified) | 1,749 | 2,281 | 1,550 | 0.77× | Two-stage | Win |
| Twitter (minified) | 2,318 | 2,253 | 1,056 | 1.03× | Fused | Win |
| Twitter (prettified) | 1,885 | 2,736 | 1,385 | 0.69× | Two-stage | Win |
| Apache Builds Small (minified) | 2,915 | 2,542 | 1,618 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,273 | 1,119 | 858 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,349 | 3,044 | 1,983 | 0.77× | Two-stage | Win |
| Canada Small (prettified) | 2,303 | 2,628 | 2,010 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,985 | 1,621 | 1,117 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,031 | 2,687 | 1,926 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 2,624 | 2,165 | 865 | 1.21× | Fused | Win |
| Discord Small (prettified) | 1,966 | 2,846 | 1,171 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,537 | 2,092 | 1,590 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,676 | 1,936 | 1,157 | 1.38× | Fused | Win |
| Github Events Small (prettified) | 2,260 | 2,703 | 1,979 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,876 | 3,571 | 2,315 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,708 | 1,942 | 688 | 1.39× | Fused | Win |
| Instruments Small (prettified) | 2,170 | 2,975 | 1,142 | 0.73× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 994 | 837 | 557 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,480 | 2,883 | 2,077 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,105 | 904 | 550 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,586 | 3,131 | 2,023 | 0.83× | Two-stage | Win |
| Random Small (minified) | 2,168 | 1,882 | 1,005 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,938 | 2,798 | 1,512 | 0.69× | Two-stage | Win |
| Twitter Small (minified) | 2,701 | 2,381 | 1,049 | 1.13× | Fused | Win |
| Twitter Small (prettified) | 2,000 | 3,010 | 1,355 | 0.66× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,464 | 601 | 223 | 4.10× | Fused | Win |
| Double (POD) | 1,385 | 735 | 309 | 1.88× | Fused | Win |
| Int64 (POD) | 3,671 | 1,743 | 728 | 2.11× | Fused | Win |
| String (POD) | 2,785 | 1,741 | 1,684 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,818 | 1,726 | 733 | 2.21× | Fused | Win |
| Apache Builds (minified) | 2,542 | 2,068 | 1,919 | 1.23× | Fused | Win |
| Canada (minified) | 1,460 | 1,191 | 903 | 1.23× | Fused | Win |
| Apache Builds (prettified) | 2,385 | 2,602 | 2,436 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,792 | 2,672 | 2,023 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,929 | 2,150 | 1,734 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,844 | 4,144 | 3,477 | 0.93× | Two-stage | Win |
| Discord (minified) | 3,204 | 2,453 | 1,633 | 1.31× | Fused | Win |
| Discord (prettified) | 2,768 | 3,308 | 2,320 | 0.84× | Two-stage | Win |
| Github Events (minified) | 2,253 | 2,048 | 1,628 | 1.10× | Fused | Win |
| Google Maps Response (minified) | 2,902 | 1,985 | 1,182 | 1.46× | Fused | Win |
| Github Events (prettified) | 2,262 | 2,384 | 1,944 | 0.95× | Two-stage | Win |
| Google Maps Response (prettified) | 3,532 | 3,615 | 2,373 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,827 | 2,510 | 1,552 | 1.52× | Fused | Win |
| Instruments (prettified) | 3,173 | 3,743 | 2,495 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 952 | 810 | 578 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,797 | 2,864 | 2,138 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 976 | 824 | 572 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,854 | 2,902 | 2,094 | 0.98× | Two-stage | Win |
| Random (minified) | 1,613 | 1,455 | 1,286 | 1.11× | Fused | Win |
| Random (prettified) | 2,027 | 2,383 | 1,912 | 0.85× | Two-stage | Win |
| Twitter (minified) | 2,603 | 2,146 | 1,326 | 1.21× | Fused | Win |
| Twitter (prettified) | 2,327 | 2,722 | 1,741 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 2,867 | 2,383 | 2,000 | 1.20× | Fused | Win |
| Canada Small (minified) | 1,421 | 1,135 | 747 | 1.25× | Fused | Win |
| Apache Builds Small (prettified) | 2,592 | 2,946 | 2,508 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,735 | 2,610 | 1,784 | 1.05× | Fused | Win |
| CitmCatalog Small (minified) | 2,127 | 1,661 | 1,425 | 1.28× | Fused | Win |
| CitmCatalog Small (prettified) | 2,455 | 2,788 | 2,429 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 2,665 | 2,060 | 1,544 | 1.29× | Fused | Win |
| Discord Small (prettified) | 2,406 | 2,684 | 2,084 | 0.90× | Two-stage | Win |
| Github Events Small (minified) | 2,655 | 2,134 | 1,828 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,616 | 1,887 | 1,214 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 2,444 | 2,688 | 2,246 | 0.91× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,321 | 3,471 | 2,439 | 0.96× | Two-stage | Win |
| Instruments Small (minified) | 3,639 | 2,353 | 1,444 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 2,992 | 3,634 | 2,339 | 0.82× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,067 | 893 | 543 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,717 | 3,059 | 1,998 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 1,182 | 947 | 536 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,901 | 3,178 | 1,990 | 0.91× | Two-stage | Win |
| Random Small (minified) | 1,929 | 1,810 | 1,300 | 1.07× | Fused | Win |
| Random Small (prettified) | 2,258 | 2,654 | 1,948 | 0.85× | Two-stage | Win |
| Twitter Small (minified) | 3,193 | 2,576 | 1,382 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,727 | 3,280 | 1,770 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,500 | 329 | 148 | 4.56× | Fused | Win |
| Double (POD) | 903 | 441 | 135 | 2.05× | Fused | Win |
| Int64 (POD) | 2,904 | 901 | 407 | 3.22× | Fused | Win |
| String (POD) | 1,719 | 842 | 913 | 2.04× | Fused | Win |
| Uint64 (POD) | 2,969 | 834 | 454 | 3.56× | Fused | Win |
| Apache Builds (minified) | 1,985 | 1,290 | 880 | 1.54× | Fused | Win |
| Canada (minified) | 951 | 827 | 582 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 1,811 | 1,663 | 1,137 | 1.09× | Fused | Win |
| Canada (prettified) | 1,783 | 1,606 | 1,271 | 1.11× | Fused | Win |
| CitmCatalog (minified) | 2,357 | 1,340 | 1,053 | 1.76× | Fused | Win |
| CitmCatalog (prettified) | 3,143 | 2,719 | 2,216 | 1.16× | Fused | Win |
| Discord (minified) | 2,300 | 1,480 | 990 | 1.55× | Fused | Win |
| Discord (prettified) | 2,292 | 2,000 | 1,522 | 1.15× | Fused | Win |
| Github Events (minified) | 2,175 | 1,731 | 1,128 | 1.26× | Fused | Win |
| Google Maps Response (minified) | 2,036 | 1,384 | 745 | 1.47× | Fused | Win |
| Github Events (prettified) | 2,173 | 1,954 | 1,347 | 1.11× | Fused | Win |
| Google Maps Response (prettified) | 2,742 | 2,304 | 1,369 | 1.19× | Fused | Win |
| Instruments (minified) | 2,576 | 1,730 | 1,175 | 1.49× | Fused | Win |
| Instruments (prettified) | 2,786 | 2,440 | 1,777 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 654 | 543 | 471 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | 2,117 | 1,857 | 1,611 | 1.14× | Fused | Win |
| Marine IK (minified) | 787 | 602 | 427 | 1.31× | Fused | Win |
| Marine IK (prettified) | 2,192 | 1,865 | 1,540 | 1.18× | Fused | Win |
| Random (minified) | 1,396 | 1,172 | 790 | 1.19× | Fused | Win |
| Random (prettified) | 1,749 | 1,520 | 1,057 | 1.15× | Fused | Win |
| Twitter (minified) | 2,286 | 1,730 | 1,061 | 1.32× | Fused | Win |
| Twitter (prettified) | 2,655 | 2,513 | 1,437 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 2,042 | 1,795 | 1,118 | 1.14× | Fused | Win |
| Canada Small (minified) | 1,001 | 801 | 525 | 1.25× | Fused | Win |
| Apache Builds Small (prettified) | 1,924 | 2,124 | 1,411 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 1,919 | 1,687 | 1,034 | 1.14× | Fused | Win |
| CitmCatalog Small (minified) | 1,800 | 1,289 | 1,032 | 1.40× | Fused | Win |
| CitmCatalog Small (prettified) | 2,217 | 2,071 | 1,649 | 1.07× | Fused | Win |
| Discord Small (minified) | 2,320 | 1,498 | 1,108 | 1.55× | Fused | Win |
| Discord Small (prettified) | 2,243 | 2,092 | 1,492 | 1.07× | Fused | Win |
| Github Events Small (minified) | 1,966 | 1,615 | 1,454 | 1.22× | Fused | Win |
| Google Maps Response Small (minified) | 2,158 | 1,441 | 689 | 1.50× | Fused | Win |
| Github Events Small (prettified) | 2,146 | 1,984 | 1,726 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,806 | 2,595 | 1,540 | 1.08× | Fused | Win |
| Instruments Small (minified) | 3,045 | 1,788 | 1,160 | 1.70× | Fused | Win |
| Instruments Small (prettified) | 2,959 | 2,582 | 1,836 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 822 | 634 | 427 | 1.30× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,008 | 2,111 | 1,492 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 915 | 695 | 391 | 1.32× | Fused | Win |
| Marine IK Small (prettified) | 2,383 | 2,288 | 1,441 | 1.04× | Fused | Win |
| Random Small (minified) | 1,434 | 1,246 | 813 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,574 | 1,620 | 1,160 | 0.97× | Two-stage | Win |
| Twitter Small (minified) | 2,793 | 2,109 | 1,155 | 1.32× | Fused | Win |
| Twitter Small (prettified) | 2,673 | 2,469 | 1,485 | 1.08× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,362 | 352 | 175 | 3.87× | Fused | Win |
| Double (POD) | 1,078 | 406 | 204 | 2.66× | Fused | Win |
| Int64 (POD) | 2,775 | 938 | 482 | 2.96× | Fused | Win |
| String (POD) | 2,419 | 975 | 1,226 | 2.48× | Fused | Win |
| Uint64 (POD) | 2,845 | 896 | 474 | 3.17× | Fused | Win |
| Apache Builds (minified) | 2,336 | 1,471 | 1,066 | 1.59× | Fused | Win |
| Canada (minified) | 1,038 | 896 | 618 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,257 | 1,826 | 1,351 | 1.24× | Fused | Win |
| Canada (prettified) | 2,008 | 1,893 | 1,347 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,651 | 1,663 | 916 | 1.59× | Fused | Win |
| CitmCatalog (prettified) | 3,365 | 3,207 | 1,897 | 1.05× | Fused | Win |
| Discord (minified) | 3,196 | 1,680 | 831 | 1.90× | Fused | Win |
| Discord (prettified) | 2,759 | 2,259 | 1,212 | 1.22× | Fused | Win |
| Github Events (minified) | 2,574 | 2,004 | 1,012 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 2,354 | 1,385 | 855 | 1.70× | Fused | Win |
| Github Events (prettified) | 2,385 | 2,259 | 1,196 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 3,045 | 2,572 | 1,708 | 1.18× | Fused | Win |
| Instruments (minified) | 3,238 | 1,917 | 705 | 1.69× | Fused | Win |
| Instruments (prettified) | 3,128 | 2,778 | 1,165 | 1.13× | Fused | Win |
| Marine IK Reverse (minified) | 795 | 677 | 450 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,373 | 2,252 | 1,311 | 1.05× | Fused | Win |
| Marine IK (minified) | 820 | 687 | 453 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,406 | 2,270 | 1,573 | 1.06× | Fused | Win |
| Random (minified) | 1,745 | 1,343 | 749 | 1.30× | Fused | Win |
| Random (prettified) | 2,043 | 1,778 | 1,131 | 1.15× | Fused | Win |
| Twitter (minified) | 2,908 | 2,185 | 907 | 1.33× | Fused | Win |
| Twitter (prettified) | 2,703 | 2,674 | 1,191 | 1.01× | Fused | Win |
| Apache Builds Small (minified) | 2,184 | 1,737 | 1,225 | 1.26× | Fused | Win |
| Canada Small (minified) | 926 | 731 | 506 | 1.27× | Fused | Win |
| Apache Builds Small (prettified) | 2,152 | 2,072 | 1,547 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,827 | 1,789 | 1,175 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,957 | 1,383 | 881 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 2,347 | 2,235 | 1,512 | 1.05× | Fused | Win |
| Discord Small (minified) | 2,777 | 1,708 | 729 | 1.63× | Fused | Win |
| Discord Small (prettified) | 2,546 | 2,153 | 987 | 1.18× | Fused | Win |
| Github Events Small (minified) | 2,421 | 1,782 | 1,283 | 1.36× | Fused | Win |
| Google Maps Response Small (minified) | 2,379 | 1,379 | 783 | 1.73× | Fused | Win |
| Github Events Small (prettified) | 2,502 | 2,113 | 1,600 | 1.18× | Fused | Win |
| Google Maps Response Small (prettified) | 3,005 | 2,555 | 1,765 | 1.18× | Fused | Win |
| Instruments Small (minified) | 3,222 | 1,934 | 670 | 1.67× | Fused | Win |
| Instruments Small (prettified) | 3,083 | 2,785 | 1,101 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 844 | 699 | 377 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,211 | 2,282 | 1,373 | 0.97× | Two-stage | Win |
| Marine IK Small (minified) | 923 | 739 | 377 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,265 | 2,411 | 1,363 | 0.94× | Two-stage | Win |
| Random Small (minified) | 1,920 | 1,425 | 787 | 1.35× | Fused | Win |
| Random Small (prettified) | 2,134 | 1,875 | 1,198 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,882 | 2,251 | 895 | 1.28× | Fused | Win |
| Twitter Small (prettified) | 2,636 | 2,674 | 1,163 | 0.99× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and 61 of the 66 minified documents, full-size and Small, the other five going to the tape (four under MSVC, one on Linux/Clang), while the tape wins 31 of the 33 full-size prettified documents and 32 of the 33 Small prettified ones. The three prettified exceptions are Apache Builds under MSVC and Canada and Canada Small on Linux/GCC, which the fused path wins. On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (1 tie) and 37 of the 44 prettified documents (5 to the tape, 2 ties). §2.8 repeats the comparison on AVX-512.

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.28× (String, Windows/MSVC) to 4.09× (Bool, macOS/GCC) faster than the two-stage path, and 1.48× (String, Windows/MSVC) to 4.56× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, except for five, four of them under MSVC.** Freshly allocated, the fused path wins 104 of the 110 minified documents, ties 1 and loses 5 to the tape. Four of the losses are under MSVC: Marine IK Reverse (459 against 562 MB/s, 0.82×) and the Small copies of Apache Builds (1,267 against 1,310, 0.97×), Marine IK Reverse (353 against 463, 0.76×) and Marine IK (475 against 493, 0.96×). The fifth is Github Events on Linux/Clang (1,654 against 1,772, 0.93×), and the tie is Marine IK on macOS/Clang (605 against 609). Its margins run from 5% (Canada under MSVC, 723 against 689 MB/s) to 1.96× (Discord on macOS/Clang, 2,552 against 1,303 MB/s) on the full-size documents and from 5% (Random Small on Linux/GCC, 1,563 against 1,487) to 1.80× (Google Maps Response Small on macOS/Clang, 2,130 against 1,183 MB/s) on the Small ones. With objects reused it wins 106 and loses 4, the same four documents under MSVC. Why the tape wins these under MSVC has not been profiled. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 63 of 66.** It wins all 22 on Linux/Clang, 20 of 22 on Linux/GCC, where the fused path wins prettified Canada and Canada Small (2,286 MB/s against 2,188, and 2,216 against 2,145), and 21 of 22 under MSVC, where the fused path wins prettified Apache Builds (1,081 against 1,068 MB/s). The tape's margins run from 1% (Marine IK on Linux/GCC, 2,644 against 2,608 MB/s) to 49% (Discord on Linux/Clang, 2,519 against 1,694) on the full-size documents and from 5% (Google Maps Response Small on Linux/GCC) to 43% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape again wins 63 of the 66 prettified x86 documents; the fused path takes prettified Canada and Canada Small on Linux/GCC, and prettified Marine IK Reverse under MSVC is a tie.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 37, the tape wins 5 and 2 are ties. The fused path wins all 22 full-size documents. On the Small copies the tape wins Apache Builds Small, Github Events Small and Marine IK Reverse Small on macOS/GCC (0.91×, 0.93× and 0.95×), and Marine IK Reverse Small and Marine IK Small on macOS/Clang (0.99× each); Discord Small and Twitter Small on macOS/GCC are ties. By compiler, the fused path wins 17 of 22 on macOS/GCC (3 to the tape, 2 ties) and 20 of 22 on macOS/Clang (2 to the tape). Reused, the fused path wins 38 and the tape 6: 19 and 3 on each compiler. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**One document beats both Jsonifier paths, freshly allocated.** Against simdjson On Demand the faster Jsonifier path wins every test in both runs except freshly allocated prettified Github Events Small on macOS/GCC, where simdjson runs 1,402 MB/s against the tape's 1,348 and the fused path's 1,248 (0.96×); reused, Jsonifier wins it. The fused path alone also loses freshly allocated prettified Twitter Small under MSVC (1,553 against 1,694 MB/s, 0.92×) and ties prettified Random on Linux/GCC (1,548 against 1,544); the tape wins both, at 1,799 and 1,721 MB/s. With objects reused the fused path loses prettified Twitter Small under MSVC (1,815 against 1,932, 0.94×) and prettified Apache Builds on Linux/GCC (2,385 against 2,436, 0.98×), and the tape wins both, at 2,127 and 2,602 MB/s. We have not profiled what sets these documents apart.

**MSVC slows all three by about the same median.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 67% of its Linux/Clang throughput, the two-stage path 66% and simdjson 67%. The spread between documents is narrowest for the two-stage path (from 54% on Github Events to 82% on Instruments), wider for the fused path (from 53% on Apache Builds to 91% on Instruments) and widest for simdjson, which keeps 55% of its Linux/Clang throughput on Canada and more than all of it (115%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 244 of 245 tests with whichever path is faster, and loses the other.** The loss is prettified Github Events Small on macOS/GCC. The fused path alone wins 242, ties 1 and loses 2: prettified Twitter Small under MSVC and prettified Github Events Small on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 197 MB/s, against 352 MB/s for the same document in declared order, so the faster Jsonifier path is 2.85× faster than simdjson freshly allocated (3.06× reused). The prettified form shows the same gap, at 700 MB/s against 1,318, and the faster Jsonifier path is 2.92× faster (3.14× reused). The Small copies show it too, at 165 MB/s minified and 629 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document within about 17% of the declared one, sometimes faster, and the faster Jsonifier path is 1.22× to 1.72× faster than simdjson on the full-size reversed document, freshly allocated, and 1.19× to 1.81× reused. Why the rescan cost shows up mostly under MSVC has not been profiled; §2.8 shows it again, wider, on the AVX-512 MSVC build.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path, and under MSVC so do four minified documents; on the M2 Pro, minified input belongs to the fused path, and so do 37 of the 44 prettified documents. Routing each test to its faster path would raise the record against simdjson from 242 wins, 1 tie and 2 losses to 244 wins and 1 loss. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,435 | 1,399 | 1.03× | Win |
| Canada (minified) | 1,013 | 815 | 1.24× | Win |
| Apache Builds (prettified) | 1,835 | 1,822 | 1.01× | Win |
| Canada (prettified) | 2,188 | 1,798 | 1.22× | Win |
| CitmCatalog (minified) | 1,791 | 1,404 | 1.28× | Win |
| CitmCatalog (prettified) | 3,574 | 2,951 | 1.21× | Win |
| Discord (minified) | 1,847 | 1,574 | 1.17× | Win |
| Discord (prettified) | 2,577 | 2,255 | 1.14× | Win |
| Github Events (minified) | 1,558 | 1,569 | 0.99× | **Loss** |
| Google Maps Response (minified) | 1,808 | 1,502 | 1.20× | Win |
| Github Events (prettified) | 1,878 | 1,890 | 0.99× | **Loss** |
| Google Maps Response (prettified) | 3,369 | 2,971 | 1.13× | Win |
| Instruments (minified) | 2,237 | 1,687 | 1.33× | Win |
| Instruments (prettified) | 3,354 | 2,715 | 1.24× | Win |
| Marine IK Reverse (minified) | 723 | 621 | 1.16× | Win |
| Marine IK Reverse (prettified) | 2,624 | 2,264 | 1.16× | Win |
| Marine IK (minified) | 737 | 624 | 1.18× | Win |
| Marine IK (prettified) | 2,644 | 2,270 | 1.16× | Win |
| Random (minified) | 1,090 | 1,170 | 0.93× | **Loss** |
| Random (prettified) | 1,721 | 1,802 | 0.95× | **Loss** |
| Twitter (minified) | 1,700 | 1,599 | 1.06× | Win |
| Twitter (prettified) | 2,159 | 2,061 | 1.05× | Win |
| Apache Builds Small (minified) | 2,039 | 1,771 | 1.15× | Win |
| Canada Small (minified) | 916 | 750 | 1.22× | Win |
| Apache Builds Small (prettified) | 2,516 | 2,232 | 1.13× | Win |
| Canada Small (prettified) | 2,145 | 1,781 | 1.20× | Win |
| CitmCatalog Small (minified) | 1,433 | 1,231 | 1.16× | Win |
| CitmCatalog Small (prettified) | 2,423 | 2,101 | 1.15× | Win |
| Discord Small (minified) | 1,682 | 1,510 | 1.11× | Win |
| Discord Small (prettified) | 2,172 | 2,044 | 1.06× | Win |
| Github Events Small (minified) | 1,725 | 1,829 | 0.94× | **Loss** |
| Google Maps Response Small (minified) | 1,699 | 1,463 | 1.16× | Win |
| Github Events Small (prettified) | 2,124 | 2,261 | 0.94× | **Loss** |
| Google Maps Response Small (prettified) | 3,198 | 2,883 | 1.11× | Win |
| Instruments Small (minified) | 2,217 | 1,667 | 1.33× | Win |
| Instruments Small (prettified) | 3,375 | 2,636 | 1.28× | Win |
| Marine IK Reverse Small (minified) | 753 | 625 | 1.20× | Win |
| Marine IK Reverse Small (prettified) | 2,611 | 2,312 | 1.13× | Win |
| Marine IK Small (minified) | 781 | 638 | 1.23× | Win |
| Marine IK Small (prettified) | 2,705 | 2,343 | 1.15× | Win |
| Random Small (minified) | 1,487 | 1,268 | 1.17× | Win |
| Random Small (prettified) | 2,199 | 1,910 | 1.15× | Win |
| Twitter Small (minified) | 2,312 | 1,924 | 1.20× | Win |
| Twitter Small (prettified) | 2,914 | 2,558 | 1.14× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 848 | 824 | 1.03× | Win |
| Canada (minified) | 584 | 563 | 1.04× | Win |
| Apache Builds (prettified) | 1,166 | 1,075 | 1.08× | Win |
| Canada (prettified) | 1,318 | 1,131 (n/c) | 1.17× | Win |
| CitmCatalog (minified) | 1,097 (n/c) | 1,030 | 1.07× | Win |
| CitmCatalog (prettified) | 2,406 | 2,042 | 1.18× | Win |
| Discord (minified) | 1,165 | 1,044 | 1.12× | Win |
| Discord (prettified) | 1,601 | 1,433 | 1.12× | Win |
| Github Events (minified) | 1,334 | 1,124 | 1.19× | Win |
| Google Maps Response (minified) | 1,207 | 1,012 | 1.19× | Win |
| Github Events (prettified) | 1,510 | 1,342 | 1.13× | Win |
| Google Maps Response (prettified) | 2,153 | 1,809 | 1.19× | Win |
| Instruments (minified) | 1,398 | 1,086 | 1.29× | Win |
| Instruments (prettified) | 2,039 | 1,809 | 1.13× | Win |
| Marine IK Reverse (minified) | 503 | 488 | 1.03× | Win |
| Marine IK Reverse (prettified) | 1,764 | 1,618 | 1.09× | Win |
| Marine IK (minified) | 535 | 446 | 1.20× | Win |
| Marine IK (prettified) | 1,623 | 1,658 | 0.98× | Tie |
| Random (minified) | 821 | 727 | 1.13× | Win |
| Random (prettified) | 1,100 | 1,059 | 1.04× | Win |
| Twitter (minified) | 1,384 | 1,208 | 1.15× | Win |
| Twitter (prettified) | 1,899 | 1,623 | 1.17× | Win |
| Apache Builds Small (minified) | 1,380 | 1,077 | 1.28× | Win |
| Canada Small (minified) | 623 | 575 | 1.08× | Win |
| Apache Builds Small (prettified) | 1,673 | 1,285 | 1.30× | Win |
| Canada Small (prettified) | 1,529 | 1,210 | 1.26× | Win |
| CitmCatalog Small (minified) | 989 | 850 | 1.16× | Win |
| CitmCatalog Small (prettified) | 1,589 | 1,367 | 1.16× | Win |
| Discord Small (minified) | 1,262 | 998 | 1.26× | Win |
| Discord Small (prettified) | 1,634 | 1,357 | 1.20× | Win |
| Github Events Small (minified) | 1,101 | 1,086 | 1.01× | Tie |
| Google Maps Response Small (minified) | 1,200 | 962 | 1.25× | Win |
| Github Events Small (prettified) | 1,348 | 1,353 | 1.00× | Tie |
| Google Maps Response Small (prettified) | 2,215 | 1,806 | 1.23× | Win |
| Instruments Small (minified) | 1,614 | 1,187 | 1.36× | Win |
| Instruments Small (prettified) | 2,357 | 1,917 | 1.23× | Win |
| Marine IK Reverse Small (minified) | 529 | 458 | 1.15× | Win |
| Marine IK Reverse Small (prettified) | 1,802 | 1,762 | 1.02× | Win |
| Marine IK Small (minified) | 586 | 510 | 1.15× | Win |
| Marine IK Small (prettified) | 1,832 | 1,649 | 1.11× | Win |
| Random Small (minified) | 1,035 | 780 | 1.33× | Win |
| Random Small (prettified) | 1,184 | 1,128 | 1.05× | Win |
| Twitter Small (minified) | 1,744 | 1,205 | 1.45× | Win |
| Twitter Small (prettified) | 2,088 | 1,600 | 1.31× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,068 | 1,808 | 1.14× | Win |
| Canada (minified) | 1,191 | 962 | 1.24× | Win |
| Apache Builds (prettified) | 2,602 | 2,310 | 1.13× | Win |
| Canada (prettified) | 2,672 | 2,113 | 1.26× | Win |
| CitmCatalog (minified) | 2,150 | 1,683 | 1.28× | Win |
| CitmCatalog (prettified) | 4,144 | 3,413 | 1.21× | Win |
| Discord (minified) | 2,453 | 1,814 | 1.35× | Win |
| Discord (prettified) | 3,308 | 2,582 | 1.28× | Win |
| Github Events (minified) | 2,048 | 1,875 | 1.09× | Win |
| Google Maps Response (minified) | 1,985 | 1,605 | 1.24× | Win |
| Github Events (prettified) | 2,384 | 2,224 | 1.07× | Win |
| Google Maps Response (prettified) | 3,615 | 3,168 | 1.14× | Win |
| Instruments (minified) | 2,510 | 1,719 | 1.46× | Win |
| Instruments (prettified) | 3,743 | 2,770 | 1.35× | Win |
| Marine IK Reverse (minified) | 810 | 691 | 1.17× | Win |
| Marine IK Reverse (prettified) | 2,864 | 2,477 | 1.16× | Win |
| Marine IK (minified) | 824 | 694 | 1.19× | Win |
| Marine IK (prettified) | 2,902 | 2,486 | 1.17× | Win |
| Random (minified) | 1,455 | 1,526 | 0.95× | **Loss** |
| Random (prettified) | 2,383 | 2,351 | 1.01× | Win |
| Twitter (minified) | 2,146 | 1,853 | 1.16× | Win |
| Twitter (prettified) | 2,722 | 2,353 | 1.16× | Win |
| Apache Builds Small (minified) | 2,383 | 1,979 | 1.20× | Win |
| Canada Small (minified) | 1,135 | 869 | 1.31× | Win |
| Apache Builds Small (prettified) | 2,946 | 2,516 | 1.17× | Win |
| Canada Small (prettified) | 2,610 | 2,002 | 1.30× | Win |
| CitmCatalog Small (minified) | 1,661 | 1,342 | 1.24× | Win |
| CitmCatalog Small (prettified) | 2,788 | 2,341 | 1.19× | Win |
| Discord Small (minified) | 2,060 | 1,768 | 1.17× | Win |
| Discord Small (prettified) | 2,684 | 2,346 | 1.14× | Win |
| Github Events Small (minified) | 2,134 | 2,054 | 1.04× | Win |
| Google Maps Response Small (minified) | 1,887 | 1,603 | 1.18× | Win |
| Github Events Small (prettified) | 2,688 | 2,543 | 1.06× | Win |
| Google Maps Response Small (prettified) | 3,471 | 3,173 | 1.09× | Win |
| Instruments Small (minified) | 2,353 | 1,712 | 1.37× | Win |
| Instruments Small (prettified) | 3,634 | 2,701 | 1.35× | Win |
| Marine IK Reverse Small (minified) | 893 | 721 | 1.24× | Win |
| Marine IK Reverse Small (prettified) | 3,059 | 2,626 | 1.16× | Win |
| Marine IK Small (minified) | 947 | 714 | 1.32× | Win |
| Marine IK Small (prettified) | 3,178 | 2,619 | 1.21× | Win |
| Random Small (minified) | 1,810 | 1,406 | 1.29× | Win |
| Random Small (prettified) | 2,654 | 2,109 | 1.26× | Win |
| Twitter Small (minified) | 2,576 | 2,163 | 1.19× | Win |
| Twitter Small (prettified) | 3,280 | 2,767 | 1.19× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,290 | 1,018 | 1.27× | Win |
| Canada (minified) | 827 | 691 | 1.20× | Win |
| Apache Builds (prettified) | 1,663 | 1,303 | 1.28× | Win |
| Canada (prettified) | 1,606 | 1,442 | 1.11× | Win |
| CitmCatalog (minified) | 1,340 | 1,163 | 1.15× | Win |
| CitmCatalog (prettified) | 2,719 | 2,218 | 1.23× | Win |
| Discord (minified) | 1,480 | 1,092 | 1.36× | Win |
| Discord (prettified) | 2,000 | 1,606 | 1.25× | Win |
| Github Events (minified) | 1,731 | 1,345 | 1.29× | Win |
| Google Maps Response (minified) | 1,384 | 1,095 | 1.26× | Win |
| Github Events (prettified) | 1,954 | 1,576 | 1.24× | Win |
| Google Maps Response (prettified) | 2,304 | 1,944 | 1.19× | Win |
| Instruments (minified) | 1,730 | 1,230 | 1.41× | Win |
| Instruments (prettified) | 2,440 | 1,562 | 1.56× | Win |
| Marine IK Reverse (minified) | 543 | 538 | 1.01× | Tie |
| Marine IK Reverse (prettified) | 1,857 | 1,810 | 1.03× | Win |
| Marine IK (minified) | 602 | 491 | 1.22× | Win |
| Marine IK (prettified) | 1,865 | 1,609 | 1.16× | Win |
| Random (minified) | 1,172 | 895 | 1.31× | Win |
| Random (prettified) | 1,520 | 1,298 | 1.17× | Win |
| Twitter (minified) | 1,730 | 1,347 | 1.28× | Win |
| Twitter (prettified) | 2,513 | 1,757 | 1.43× | Win |
| Apache Builds Small (minified) | 1,795 | 1,345 | 1.33× | Win |
| Canada Small (minified) | 801 | 670 | 1.19× | Win |
| Apache Builds Small (prettified) | 2,124 | 1,582 | 1.34× | Win |
| Canada Small (prettified) | 1,687 | 1,400 | 1.21× | Win |
| CitmCatalog Small (minified) | 1,289 | 1,062 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,071 | 1,676 | 1.24× | Win |
| Discord Small (minified) | 1,498 | 1,123 | 1.33× | Win |
| Discord Small (prettified) | 2,092 | 1,377 | 1.52× | Win |
| Github Events Small (minified) | 1,615 | 1,325 | 1.22× | Win |
| Google Maps Response Small (minified) | 1,441 | 1,096 | 1.32× | Win |
| Github Events Small (prettified) | 1,984 | 1,594 | 1.24× | Win |
| Google Maps Response Small (prettified) | 2,595 | 2,007 | 1.29× | Win |
| Instruments Small (minified) | 1,788 | 1,320 | 1.35× | Win |
| Instruments Small (prettified) | 2,582 | 2,031 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 634 | 616 | 1.03× | Win |
| Marine IK Reverse Small (prettified) | 2,111 | 1,909 | 1.11× | Win |
| Marine IK Small (minified) | 695 | 610 | 1.14× | Win |
| Marine IK Small (prettified) | 2,288 | 1,839 | 1.24× | Win |
| Random Small (minified) | 1,246 | 948 | 1.31× | Win |
| Random Small (prettified) | 1,620 | 1,474 | 1.10× | Win |
| Twitter Small (minified) | 2,109 | 1,197 | 1.76× | Win |
| Twitter Small (prettified) | 2,469 | 1,820 | 1.36× | Win |

In the freshly allocated run, two-stage Jsonifier wins 79 of these 88 tests, ties 3 and loses 6, by up to 7%, all on Linux/GCC: minified and prettified Github Events, minified and prettified Random, and minified and prettified Github Events Small (0.99×, 0.99×, 0.93×, 0.95×, 0.94× and 0.94×). The ties are on macOS/GCC: prettified Marine IK and minified and prettified Github Events Small (0.98×, 1.01× and 1.00×). With the fused path instead, Jsonifier beats the reflection reader on 78 and loses 10. With objects reused, two-stage Jsonifier wins 86, ties 1 and loses 1: minified Random on Linux/GCC (1,455 against 1,526 MB/s); the tie is minified Marine IK Reverse on macOS/GCC. The fused path beats the reflection reader on 84 and loses 4. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a 0.99× row can go either way: freshly allocated minified Github Events on Linux/GCC (1,558 against 1,569 MB/s) is a loss.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 34 of the 40 results; the widest gap is reused prettified Twitter Small on macOS/GCC, where the two-stage row runs at 4,480 MB/s against the "jsonifier" row's 3,971. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 6,452 | 4,248 | 1.52× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,647 | 5,199 | 1.47× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,052 | 3,973 | 1.27× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,543 | 4,807 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,202 | 6,819 | 1.20× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,372 | 8,089 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,565 | 5,578 | 1.00× | Tie |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,689 | 6,824 | 0.98× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,916 | 6,684 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,903 | 8,172 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,712 | 5,625 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,558 | 6,286 | 1.04× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,833 | 3,697 | 1.31× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,873 | 4,041 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,890 | 3,383 | 1.15× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,205 | 3,159 | 1.33× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,944 | 4,074 | 1.21× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,380 | 4,833 | 1.11× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,740 | 3,510 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,225 | 4,315 | 0.98× | Loss |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 7,106 | 4,157 | 1.71× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,923 | 5,360 | 1.48× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,374 | 4,172 | 1.29× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,086 | 4,992 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,327 | 6,922 | 1.20× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,642 | 8,295 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,783 | 5,716 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,741 | 6,854 | 0.98× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,080 | 6,819 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,991 | 8,111 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,008 | 5,803 | 1.04× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,653 | 6,855 | 0.97× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,736 | 3,816 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,191 | 4,579 | 1.13× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,837 | 3,558 | 1.08× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,480 | 3,513 | 1.28× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 5,014 | 4,189 | 1.20× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,487 | 4,918 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,109 | 3,747 | 1.10× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,573 | 4,579 | 1.00× | Tie |

Jsonifier wins 34 of the 40 results, ties 2 and loses 4, and wins by up to 1.71× (minified Twitter under MSVC, reused). Freshly allocated it wins 17, ties 1 and loses 2: the tie is minified Twitter Small on Linux/Clang (1.00×), and the losses are prettified Twitter Small on Linux/Clang and on macOS/Clang (0.98× each). The reused run has 17 wins, 1 tie and 2 losses: the tie is prettified Twitter Small on macOS/Clang (1.00×), and the losses are prettified Twitter Small on Linux/Clang (0.98×) and on Linux/GCC (0.97×). Every full-size result is a win, by 1.09× to 1.71× on the three x86 builds and 1.11× to 1.31× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.16× to 1.38×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 2,933 | 2,522 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,536 | 2,635 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,987 | 2,172 | 1.38× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,244 | 1,736 | 1.29× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,529 | 1,988 | 1.27× | Win |

The narrowest margin is under MSVC, 1.16× (2,933 against 2,522 MB/s).

### 2.8 AVX-512 builds

Every result above comes from AVX2 and NEON builds. The same tests also ran on three AVX-512 builds, Linux / Clang 24.0, Linux / GCC 16.2 and Windows / MSVC 19.51, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, from the Json-Performance branch `benchmarking-avx512-main` at the same commits as §2.1, on November 15, 2026. Jsonifier and both of its paths run their AVX-512 kernels, and simdjson runs its `icelake` kernel. Sampling, convergence limits (RSE under 5%, mean shift under 2.5%) and verdicts are as in §2.1. The Linux / GCC build also runs the reflection reader of §2.5. The runners are shared virtual machines, and the CPU, operating-system images and MSVC version differ from the AVX2 builds, so a result here cannot be set against the matching AVX2 result to isolate the instruction set; every comparison below is within one build.

- **The routing split holds.** Freshly allocated, the fused path wins 76 of the 147 tests, 3 are ties and the tape wins 68; reused, 79, 2 and 66. Freshly allocated, the fused path wins all 15 POD-type tests (1.40× for String under MSVC to 5.24× for Bool on Linux / GCC) and 61 of the 66 minified documents. The tape takes minified Apache Builds (0.98×) and Google Maps Response Small (0.65×) under MSVC and minified Github Events (0.97×) and Random Small (0.99×) on Linux / GCC; minified Github Events on Linux / Clang is a tie. The tape wins 64 of the 66 prettified documents, and the other two are ties, prettified Canada under MSVC and Apache Builds on Linux / GCC. Its margins reach 1.58× (Instruments on Linux / Clang, 3,395 against 2,152 MB/s) and 1.57× on the Small copy.
- **Against simdjson, the faster Jsonifier path wins 145 of 147 freshly allocated and 144 of 147 reused.** All of its non-wins are on Linux / GCC: prettified Apache Builds (0.98×) and Random (0.98×) are losses freshly allocated; reused, minified Random (0.99×) and prettified Random (0.94×) are losses and prettified Apache Builds is a tie. The fused path alone wins 137, ties 1 and loses 9 freshly allocated, and wins 135 and loses 12 reused; seven of the nine freshly allocated losses are prettified documents on Linux / GCC.
- **Stage 1 + reflection on Linux / GCC.** Against simdjson's reflection reader, two-stage Jsonifier wins 28 of the 44 corpus tests freshly allocated, ties 1 (prettified Discord) and loses 15, by up to 10% (minified Apache Builds and Github Events Small, 0.90×). Reused it wins 36, ties 1 (minified Github Events Small) and loses 7, by up to 8% (prettified Apache Builds and minified Random, 0.92×). With the fused path instead, Jsonifier beats the reflection reader on 30 freshly allocated and 32 reused.
- **Reverse key order costs simdjson most under MSVC.** On this build simdjson parses minified Marine IK Reverse at 117 MB/s against 213 MB/s in declared order, and the faster Jsonifier path is 4.70× faster freshly allocated (5.32× reused); on the prettified form it is 4.10× faster (4.51× reused). On both Linux builds simdjson reads the full-size reversed document within 3% of the declared one, and the faster Jsonifier path is 1.31× (Linux / Clang) and 1.53× (Linux / GCC) faster on the minified form, and 1.19× and 1.33× on the prettified form.
- **Partial reading.** Jsonifier wins all four full-size Twitter Partial results on each build in both runs, by 2.59× to 2.67× under MSVC, 1.28× to 1.35× on Linux / Clang and 1.08× to 1.10× on Linux / GCC, and the Small copies under MSVC by 1.79× to 1.88×. Freshly allocated it loses both Small copies on both Linux builds (0.89× to 0.95×), for 8 wins and 4 losses. Reused it wins prettified Twitter Small on Linux / Clang (1.00×) and minified Twitter Small on Linux / GCC (1.25×), for 10 wins and 2 losses.
- **Validation.** Jsonifier wins on all three builds, by 1.19× under MSVC, 1.61× on Linux / Clang and 1.33× on Linux / GCC.

#### Freshly allocated objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 25 | 1 | 23 | 42 of 49 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 26 | 1 | 22 | 49 of 49 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 25 | 1 | 23 | 49 of 49 |
| **Aggregate** | **76** | **3** | **68** | **140 of 147** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 47 / 0 / 2 | 41 / 1 / 7 |
| **Aggregate** | **145 / 0 / 2** | **137 / 1 / 9** |

**Windows / MSVC 19.51 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 459 | 158 | 67 | 2.90× | Fused | Win |
| Double (POD) | 941 | 373 | 136 | 2.52× | Fused | Win |
| Int64 (POD) | 1,458 | 577 | 299 | 2.53× | Fused | Win |
| String (POD) | 811 | 579 | 485 | 1.40× | Fused | Win |
| Uint64 (POD) | 1,348 | 558 | 310 | 2.41× | Fused | Win |
| Apache Builds (minified) | 778 (n/c) | 797 | 687 | 0.98× | Two-stage | Win |
| Canada (minified) | 620 | 587 (n/c) | 190 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 969 | 1,026 (n/c) | 862 | 0.94× | Two-stage | Win |
| Canada (prettified) | 1,301 | 1,314 | 424 | 0.99× | Tie | Win |
| CitmCatalog (minified) | 1,302 | 1,117 | 554 | 1.17× | Fused | Win |
| CitmCatalog (prettified) | 2,103 | 2,355 | 1,199 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,229 | 954 | 704 | 1.29× | Fused | Win |
| Discord (prettified) | 1,360 | 1,425 | 1,023 | 0.95× | Two-stage | Win |
| Github Events (minified) | 1,031 | 970 | 619 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 1,275 | 1,087 | 537 | 1.17× | Fused | Win |
| Github Events (prettified) | 1,016 | 1,139 | 736 | 0.89× | Two-stage | Win |
| Google Maps Response (prettified) | 2,133 | 2,158 | 1,071 | 0.99× | Two-stage | Win |
| Instruments (minified) | 1,496 | 1,063 | 721 | 1.41× | Fused | Win |
| Instruments (prettified) | 1,589 | 1,793 | 1,123 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 549 (n/c) | 528 | 117 | 1.04× | Fused | Win |
| Marine IK Reverse (prettified) | 1,832 (n/c) | 1,959 | 478 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 596 | 556 | 213 | 1.07× | Fused | Win |
| Marine IK (prettified) | 1,863 (n/c) | 2,062 | 770 | 0.90× | Two-stage | Win |
| Random (minified) | 654 | 617 | 496 | 1.06× | Fused | Win |
| Random (prettified) | 894 | 988 | 721 (n/c) | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,222 | 1,070 | 842 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,251 | 1,412 | 1,127 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 989 | 922 | 655 | 1.07× | Fused | Win |
| Canada Small (minified) | 599 | 555 | 170 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,061 | 1,144 | 810 | 0.93× | Two-stage | Win |
| Canada Small (prettified) | 1,292 | 1,376 | 415 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 690 | 617 | 400 | 1.12× | Fused | Win |
| CitmCatalog Small (prettified) | 1,009 | 1,080 | 713 | 0.93× | Two-stage | Win |
| Discord Small (minified) | 944 | 756 | 633 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,012 | 1,048 | 860 | 0.97× | Two-stage | Win |
| Github Events Small (minified) | 854 | 811 | 528 | 1.05× | Fused | Win |
| Google Maps Response Small (minified) | 621 | 956 | 504 | 0.65× | Two-stage | Win |
| Github Events Small (prettified) | 906 | 1,015 | 673 | 0.89× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,850 | 1,946 | 1,026 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 1,429 | 1,070 | 670 | 1.34× | Fused | Win |
| Instruments Small (prettified) | 1,531 | 1,723 | 1,097 | 0.89× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 452 | 420 | 98 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,308 | 1,554 | 377 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 499 | 444 | 183 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,475 | 1,669 | 677 | 0.88× | Two-stage | Win |
| Random Small (minified) | 736 | 431 | 470 | 1.71× | Fused | Win |
| Random Small (prettified) | 924 | 1,064 | 721 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 1,397 | 1,310 | 1,093 | 1.07× | Fused | Win |
| Twitter Small (prettified) | 1,295 | 1,691 | 1,396 | 0.77× | Two-stage | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,971 | 495 | 253 | 3.98× | Fused | Win |
| Double (POD) | 1,305 | 401 | 285 | 3.25× | Fused | Win |
| Int64 (POD) | 2,723 | 852 | 658 | 3.20× | Fused | Win |
| String (POD) | 1,802 | 995 | 1,262 | 1.81× | Fused | Win |
| Uint64 (POD) | 2,847 | 836 | 615 | 3.41× | Fused | Win |
| Apache Builds (minified) | 1,652 | 1,425 | 1,053 | 1.16× | Fused | Win |
| Canada (minified) | 956 | 877 | 701 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,681 | 1,821 | 1,360 | 0.92× | Two-stage | Win |
| Canada (prettified) | 1,873 | 1,965 | 1,587 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 2,057 | 1,789 | 1,053 | 1.15× | Fused | Win |
| CitmCatalog (prettified) | 2,951 | 3,552 | 2,226 | 0.83× | Two-stage | Win |
| Discord (minified) | 2,174 | 1,732 | 820 | 1.26× | Fused | Win |
| Discord (prettified) | 1,969 | 2,411 | 1,229 | 0.82× | Two-stage | Win |
| Github Events (minified) | 1,657 | 1,648 | 966 | 1.01× | Tie | Win |
| Google Maps Response (minified) | 2,263 | 1,710 | 913 | 1.32× | Fused | Win |
| Github Events (prettified) | 1,615 | 2,141 | 1,133 | 0.75× | Two-stage | Win |
| Google Maps Response (prettified) | 2,895 | 3,359 | 1,820 | 0.86× | Two-stage | Win |
| Instruments (minified) | 2,782 | 2,265 | 650 | 1.23× | Fused | Win |
| Instruments (prettified) | 2,152 | 3,395 | 1,106 | 0.63× | Two-stage | Win |
| Marine IK Reverse (minified) | 773 | 739 | 589 | 1.05× | Fused | Win |
| Marine IK Reverse (prettified) | 2,432 | 2,517 | 2,108 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 798 | 740 | 587 | 1.08× | Fused | Win |
| Marine IK (prettified) | 2,489 | 2,541 | 2,074 | 0.98× | Two-stage | Win |
| Random (minified) | 1,182 | 1,097 | 772 | 1.08× | Fused | Win |
| Random (prettified) | 1,456 | 1,689 | 1,186 | 0.86× | Two-stage | Win |
| Twitter (minified) | 1,754 | 1,662 | 878 | 1.06× | Fused | Win |
| Twitter (prettified) | 1,569 | 2,129 | 1,151 | 0.74× | Two-stage | Win |
| Apache Builds Small (minified) | 2,422 | 2,189 | 1,394 | 1.11× | Fused | Win |
| Canada Small (minified) | 1,009 | 932 | 702 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 2,119 | 2,772 | 1,745 | 0.76× | Two-stage | Win |
| Canada Small (prettified) | 1,931 | 2,102 | 1,676 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,658 | 1,477 | 1,024 | 1.12× | Fused | Win |
| CitmCatalog Small (prettified) | 1,934 | 2,648 | 1,823 | 0.73× | Two-stage | Win |
| Discord Small (minified) | 1,933 | 1,627 | 754 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,709 | 2,126 | 1,017 | 0.80× | Two-stage | Win |
| Github Events Small (minified) | 2,328 | 1,939 | 1,317 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 2,337 | 1,791 | 973 | 1.30× | Fused | Win |
| Github Events Small (prettified) | 1,573 | 2,443 | 1,686 | 0.64× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,878 | 3,368 | 1,911 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 2,688 | 2,191 | 620 | 1.23× | Fused | Win |
| Instruments Small (prettified) | 2,066 | 3,234 | 1,039 | 0.64× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 867 | 777 | 541 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,238 | 2,725 | 1,841 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 913 | 842 | 522 | 1.08× | Fused | Win |
| Marine IK Small (prettified) | 2,409 | 2,965 | 1,863 | 0.81× | Two-stage | Win |
| Random Small (minified) | 1,727 | 1,530 | 892 | 1.13× | Fused | Win |
| Random Small (prettified) | 2,019 | 2,288 | 1,328 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,127 | 2,067 | 1,032 | 1.03× | Fused | Win |
| Twitter Small (prettified) | 1,930 | 2,525 | 1,316 | 0.76× | Two-stage | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,443 | 466 | 217 | 5.24× | Fused | Win |
| Double (POD) | 1,419 | 465 | 251 | 3.05× | Fused | Win |
| Int64 (POD) | 3,073 | 846 | 598 | 3.63× | Fused | Win |
| String (POD) | 1,736 | 926 | 1,157 | 1.88× | Fused | Win |
| Uint64 (POD) | 3,061 | 845 | 568 | 3.62× | Fused | Win |
| Apache Builds (minified) | 1,597 | 1,374 | 1,364 | 1.16× | Fused | Win |
| Canada (minified) | 1,060 | 965 | 742 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,743 | 1,735 | 1,770 | 1.00× | Tie | Loss |
| Canada (prettified) | 2,128 | 2,157 | 1,662 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,200 | 1,897 | 1,650 | 1.16× | Fused | Win |
| CitmCatalog (prettified) | 3,431 | 3,881 | 3,396 | 0.88× | Two-stage | Win |
| Discord (minified) | 2,075 | 1,748 | 1,472 | 1.19× | Fused | Win |
| Discord (prettified) | 2,077 | 2,435 | 2,074 | 0.85× | Two-stage | Win |
| Github Events (minified) | 1,734 | 1,779 | 1,334 | 0.97× | Two-stage | Win |
| Google Maps Response (minified) | 1,946 | 1,618 | 1,198 | 1.20× | Fused | Win |
| Github Events (prettified) | 1,729 | 2,130 | 1,578 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 3,136 | 3,171 | 2,413 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,198 | 2,554 | 1,082 | 1.25× | Fused | Win |
| Instruments (prettified) | 2,916 | 3,849 | 1,759 | 0.76× | Two-stage | Win |
| Marine IK Reverse (minified) | 868 | 795 | 568 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,629 | 2,768 | 2,084 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 869 | 798 | 581 | 1.09× | Fused | Win |
| Marine IK (prettified) | 2,609 | 2,768 | 2,107 | 0.94× | Two-stage | Win |
| Random (minified) | 1,156 | 1,094 | 1,112 | 1.06× | Fused | Win |
| Random (prettified) | 1,529 | 1,684 | 1,718 | 0.91× | Two-stage | Loss |
| Twitter (minified) | 1,943 | 1,712 | 1,052 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,822 | 2,292 | 1,395 | 0.80× | Two-stage | Win |
| Apache Builds Small (minified) | 2,076 | 2,054 | 1,862 | 1.01× | Fused | Win |
| Canada Small (minified) | 1,076 | 969 | 723 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,214 | 2,649 | 2,278 | 0.84× | Two-stage | Win |
| Canada Small (prettified) | 2,102 | 2,282 | 1,746 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,756 | 1,602 | 1,417 | 1.10× | Fused | Win |
| CitmCatalog Small (prettified) | 2,213 | 2,694 | 2,438 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 1,724 | 1,481 | 1,362 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,731 | 1,995 | 1,836 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,176 | 1,896 | 1,446 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,092 | 1,696 | 1,292 | 1.23× | Fused | Win |
| Github Events Small (prettified) | 1,723 | 2,380 | 1,758 | 0.72× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,172 | 3,215 | 2,516 | 0.99× | Two-stage | Win |
| Instruments Small (minified) | 3,433 | 2,516 | 955 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 2,729 | 3,815 | 1,537 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 903 | 818 | 556 | 1.10× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,409 | 2,831 | 1,997 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,007 | 876 | 544 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,591 | 3,008 | 1,943 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,465 | 1,474 | 1,285 | 0.99× | Two-stage | Win |
| Random Small (prettified) | 1,915 | 2,202 | 1,955 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,424 | 2,011 | 1,181 | 1.21× | Fused | Win |
| Twitter Small (prettified) | 2,106 | 2,567 | 1,480 | 0.82× | Two-stage | Win |

#### Reused objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 28 | 0 | 21 | 48 of 49 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 26 | 1 | 22 | 48 of 49 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 25 | 1 | 23 | 49 of 49 |
| **Aggregate** | **79** | **2** | **66** | **145 of 147** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 47 / 0 / 2 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 46 / 1 / 2 | 40 / 0 / 9 |
| **Aggregate** | **144 / 1 / 2** | **135 / 0 / 12** |

**Windows / MSVC 19.51 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 541 | 165 | 69 | 3.28× | Fused | Win |
| Double (POD) | 1,427 | 410 | 139 | 3.48× | Fused | Win |
| Int64 (POD) | 1,879 | 642 | 310 | 2.92× | Fused | Win |
| String (POD) | 1,262 | 814 | 641 | 1.55× | Fused | Win |
| Uint64 (POD) | 1,751 | 600 | 313 | 2.92× | Fused | Win |
| Apache Builds (minified) | 1,208 | 1,147 | 1,021 | 1.05× | Fused | Win |
| Canada (minified) | 803 | 737 | 210 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,346 | 1,452 | 1,294 | 0.93× | Two-stage | Win |
| Canada (prettified) | 1,521 | 1,622 | 466 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,553 | 1,269 | 567 | 1.22× | Fused | Win |
| CitmCatalog (prettified) | 2,307 | 2,666 | 1,157 | 0.87× | Two-stage | Win |
| Discord (minified) | 1,748 | 1,242 | 892 | 1.41× | Fused | Win |
| Discord (prettified) | 1,744 | 1,837 | 1,203 | 0.95× | Two-stage | Win |
| Github Events (minified) | 1,505 | 1,397 | 722 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 1,408 | 1,222 | 580 | 1.15× | Fused | Win |
| Github Events (prettified) | 1,379 | 1,614 | 869 | 0.85× | Two-stage | Win |
| Google Maps Response (prettified) | 2,359 | 2,332 | 1,157 | 1.01× | Fused | Win |
| Instruments (minified) | 1,736 | 1,213 | 808 | 1.43× | Fused | Win |
| Instruments (prettified) | 1,762 | 2,050 | 1,276 | 0.86× | Two-stage | Win |
| Marine IK Reverse (minified) | 671 | 618 | 126 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,006 | 2,203 | 488 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 693 | 637 | 224 | 1.09× | Fused | Win |
| Marine IK (prettified) | 2,111 | 2,292 | 807 (n/c) | 0.92× | Two-stage | Win |
| Random (minified) | 870 | 782 | 580 | 1.11× | Fused | Win |
| Random (prettified) | 1,118 | 1,265 | 924 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,700 | 1,431 | 1,055 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,533 | 1,946 | 1,388 | 0.79× | Two-stage | Win |
| Apache Builds Small (minified) | 1,297 | 1,201 | 1,128 | 1.08× | Fused | Win |
| Canada Small (minified) | 770 | 680 | 179 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,338 | 1,510 | 1,372 | 0.89× | Two-stage | Win |
| Canada Small (prettified) | 1,590 | 1,667 | 424 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 974 | 821 | 474 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,250 | 1,387 | 787 | 0.90× | Two-stage | Win |
| Discord Small (minified) | 1,414 | 1,058 | 912 | 1.34× | Fused | Win |
| Discord Small (prettified) | 1,288 | 1,410 | 1,192 | 0.91× | Two-stage | Win |
| Github Events Small (minified) | 1,177 | 1,078 | 635 | 1.09× | Fused | Win |
| Google Maps Response Small (minified) | 1,287 | 1,111 | 567 | 1.16× | Fused | Win |
| Github Events Small (prettified) | 1,123 | 1,355 | 810 | 0.83× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,201 | 2,251 | 1,109 | 0.98× | Two-stage | Win |
| Instruments Small (minified) | 1,555 | 1,104 | 712 | 1.41× | Fused | Win |
| Instruments Small (prettified) | 1,581 | 1,798 | 1,154 | 0.88× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 565 | 519 | 105 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,553 | 1,953 | 390 | 0.80× | Two-stage | Win |
| Marine IK Small (minified) | 676 | 570 | 203 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 1,791 | 2,106 | 741 | 0.85× | Two-stage | Win |
| Random Small (minified) | 888 | 799 | 581 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,081 | 1,240 | 858 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 1,766 | 1,512 | 1,283 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 1,607 | 1,937 | 1,649 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,850 | 493 (n/c) | 271 | 3.75× | Fused | Win |
| Double (POD) | 1,409 | 398 | 301 | 3.54× | Fused | Win |
| Int64 (POD) | 2,837 | 898 | 674 | 3.16× | Fused | Win |
| String (POD) | 2,531 | 1,296 | 1,607 | 1.95× | Fused | Win |
| Uint64 (POD) | 3,060 | 855 | 623 | 3.58× | Fused | Win |
| Apache Builds (minified) | 2,383 | 2,027 | 1,356 | 1.18× | Fused | Win |
| Canada (minified) | 1,182 | 1,050 | 847 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 2,310 | 2,528 | 1,729 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,202 | 2,281 | 1,889 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,394 | 2,112 | 1,220 | 1.13× | Fused | Win |
| CitmCatalog (prettified) | 3,307 | 4,119 | 2,510 | 0.80× | Two-stage | Win |
| Discord (minified) | 2,854 | 2,133 | 929 | 1.34× | Fused | Win |
| Discord (prettified) | 2,338 | 3,009 | 1,377 | 0.78× | Two-stage | Win |
| Github Events (minified) | 2,139 | 2,144 | 1,056 | 1.00× | Tie | Win |
| Google Maps Response (minified) | 2,547 | 1,887 | 958 | 1.35× | Fused | Win |
| Github Events (prettified) | 1,949 | 2,758 | 1,281 | 0.71× | Two-stage | Win |
| Google Maps Response (prettified) | 3,117 | 3,534 | 1,919 | 0.88× | Two-stage | Win |
| Instruments (minified) | 3,179 | 2,541 | 677 | 1.25× | Fused | Win |
| Instruments (prettified) | 2,331 | 3,778 | 1,125 | 0.62× | Two-stage | Win |
| Marine IK Reverse (minified) | 886 | 822 | 663 | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 2,669 | 2,791 | 2,340 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 887 | 827 | 650 | 1.07× | Fused | Win |
| Marine IK (prettified) | 2,686 | 2,827 | 2,338 | 0.95× | Two-stage | Win |
| Random (minified) | 1,585 | 1,466 | 971 | 1.08× | Fused | Win |
| Random (prettified) | 1,833 | 2,184 | 1,469 | 0.84× | Two-stage | Win |
| Twitter (minified) | 2,135 | 1,984 | 981 | 1.08× | Fused | Win |
| Twitter (prettified) | 1,806 | 2,617 | 1,277 | 0.69× | Two-stage | Win |
| Apache Builds Small (minified) | 2,734 | 2,586 | 1,526 | 1.06× | Fused | Win |
| Canada Small (minified) | 1,293 | 1,150 | 804 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,363 | 3,189 | 1,980 | 0.74× | Two-stage | Win |
| Canada Small (prettified) | 2,343 | 2,574 | 1,813 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,051 | 1,715 | 1,176 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 2,171 | 3,061 | 1,981 | 0.71× | Two-stage | Win |
| Discord Small (minified) | 2,528 | 2,006 | 843 | 1.26× | Fused | Win |
| Discord Small (prettified) | 2,013 | 2,656 | 1,161 | 0.76× | Two-stage | Win |
| Github Events Small (minified) | 2,946 | 2,462 | 1,523 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 2,640 | 1,995 | 984 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 1,977 | 3,013 | 2,031 | 0.66× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,165 | 3,727 | 1,975 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 3,075 | 2,340 | 646 | 1.31× | Fused | Win |
| Instruments Small (prettified) | 2,148 | 3,632 | 1,090 | 0.59× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,045 | 939 | 583 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,612 | 3,199 | 2,028 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 1,160 | 1,027 | 564 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,786 | 3,436 | 2,107 | 0.81× | Two-stage | Win |
| Random Small (minified) | 2,145 | 1,864 | 1,039 | 1.15× | Fused | Win |
| Random Small (prettified) | 2,225 | 2,739 | 1,467 | 0.81× | Two-stage | Win |
| Twitter Small (minified) | 2,357 | 2,297 | 1,080 | 1.03× | Fused | Win |
| Twitter Small (prettified) | 2,085 | 2,774 | 1,354 | 0.75× | Two-stage | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,371 | 471 | 220 | 5.03× | Fused | Win |
| Double (POD) | 1,523 | 471 | 253 | 3.23× | Fused | Win |
| Int64 (POD) | 3,299 | 855 | 596 | 3.86× | Fused | Win |
| String (POD) | 2,359 | 1,125 | 1,439 | 2.10× | Fused | Win |
| Uint64 (POD) | 3,214 | 844 | 569 | 3.81× | Fused | Win |
| Apache Builds (minified) | 2,140 | 1,811 | 1,802 | 1.18× | Fused | Win |
| Canada (minified) | 1,290 | 1,166 | 869 | 1.11× | Fused | Win |
| Apache Builds (prettified) | 2,256 | 2,297 | 2,298 | 0.98× | Two-stage | Tie |
| Canada (prettified) | 2,477 | 2,543 | 1,926 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,610 | 2,215 | 1,983 | 1.18× | Fused | Win |
| CitmCatalog (prettified) | 3,902 | 4,359 | 3,973 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,586 | 2,065 | 1,742 | 1.25× | Fused | Win |
| Discord (prettified) | 2,399 | 2,916 | 2,466 | 0.82× | Two-stage | Win |
| Github Events (minified) | 2,226 | 2,224 | 1,502 | 1.00× | Tie | Win |
| Google Maps Response (minified) | 2,200 | 1,718 | 1,273 | 1.28× | Fused | Win |
| Github Events (prettified) | 2,045 | 2,655 | 1,807 | 0.77× | Two-stage | Win |
| Google Maps Response (prettified) | 3,293 | 3,365 | 2,543 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,582 | 2,821 | 1,116 | 1.27× | Fused | Win |
| Instruments (prettified) | 3,105 | 4,216 | 1,831 | 0.74× | Two-stage | Win |
| Marine IK Reverse (minified) | 978 | 872 | 624 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,795 | 3,040 | 2,250 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 972 | 881 | 638 | 1.10× | Fused | Win |
| Marine IK (prettified) | 2,819 | 3,010 | 2,293 | 0.94× | Two-stage | Win |
| Random (minified) | 1,472 | 1,369 | 1,487 | 1.07× | Fused | Loss |
| Random (prettified) | 1,877 | 2,109 | 2,235 | 0.89× | Two-stage | Loss |
| Twitter (minified) | 2,359 | 2,124 | 1,180 | 1.11× | Fused | Win |
| Twitter (prettified) | 2,097 | 2,755 | 1,552 | 0.76× | Two-stage | Win |
| Apache Builds Small (minified) | 2,331 | 2,245 | 2,083 | 1.04× | Fused | Win |
| Canada Small (minified) | 1,327 | 1,151 | 813 | 1.15× | Fused | Win |
| Apache Builds Small (prettified) | 2,408 | 2,873 | 2,589 | 0.84× | Two-stage | Win |
| Canada Small (prettified) | 2,468 | 2,720 | 1,918 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,001 | 1,853 | 1,645 | 1.08× | Fused | Win |
| CitmCatalog Small (prettified) | 2,488 | 3,066 | 2,693 | 0.81× | Two-stage | Win |
| Discord Small (minified) | 2,151 | 1,829 | 1,618 | 1.18× | Fused | Win |
| Discord Small (prettified) | 2,051 | 2,458 | 2,157 | 0.83× | Two-stage | Win |
| Github Events Small (minified) | 2,522 | 2,366 | 1,664 | 1.07× | Fused | Win |
| Google Maps Response Small (minified) | 2,276 | 1,838 | 1,370 | 1.24× | Fused | Win |
| Github Events Small (prettified) | 2,086 | 2,886 | 2,029 | 0.72× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,412 | 3,451 | 2,670 | 0.99× | Two-stage | Win |
| Instruments Small (minified) | 3,873 | 2,809 | 1,014 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 2,884 | 4,176 | 1,651 | 0.69× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,106 | 977 | 586 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,742 | 3,270 | 2,088 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 1,231 | 1,041 | 565 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 2,939 | 3,449 | 2,028 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,645 | 1,670 | 1,546 | 0.99× | Two-stage | Win |
| Random Small (prettified) | 2,177 | 2,500 | 2,338 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,700 | 2,218 | 1,245 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 2,263 | 2,824 | 1,586 | 0.80× | Two-stage | Win |

#### Freshly allocated objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,374 | 1,521 | 0.90× | **Loss** |
| Canada (minified) | 965 | 727 | 1.33× | Win |
| Apache Builds (prettified) | 1,735 | 1,936 | 0.90× | **Loss** |
| Canada (prettified) | 2,157 | 1,650 | 1.31× | Win |
| CitmCatalog (minified) | 1,897 | 1,585 | 1.20× | Win |
| CitmCatalog (prettified) | 3,881 | 3,283 | 1.18× | Win |
| Discord (minified) | 1,748 | 1,721 | 1.02× | Win |
| Discord (prettified) | 2,435 | 2,436 | 1.00× | Tie |
| Github Events (minified) | 1,779 | 1,916 | 0.93× | **Loss** |
| Google Maps Response (minified) | 1,618 | 1,472 | 1.10× | Win |
| Github Events (prettified) | 2,130 | 2,285 | 0.93× | **Loss** |
| Google Maps Response (prettified) | 3,171 | 2,870 | 1.10× | Win |
| Instruments (minified) | 2,554 | 1,695 | 1.51× | Win |
| Instruments (prettified) | 3,849 | 2,650 | 1.45× | Win |
| Marine IK Reverse (minified) | 795 | 630 | 1.26× | Win |
| Marine IK Reverse (prettified) | 2,768 | 2,278 | 1.21× | Win |
| Marine IK (minified) | 798 | 644 | 1.24× | Win |
| Marine IK (prettified) | 2,768 | 2,312 | 1.20× | Win |
| Random (minified) | 1,094 | 1,172 | 0.93× | **Loss** |
| Random (prettified) | 1,684 | 1,796 | 0.94× | **Loss** |
| Twitter (minified) | 1,712 | 1,736 | 0.99× | **Loss** |
| Twitter (prettified) | 2,292 | 2,263 | 1.01× | Win |
| Apache Builds Small (minified) | 2,054 | 2,180 | 0.94× | **Loss** |
| Canada Small (minified) | 969 | 730 | 1.33× | Win |
| Apache Builds Small (prettified) | 2,649 | 2,697 | 0.98× | **Loss** |
| Canada Small (prettified) | 2,282 | 1,735 | 1.32× | Win |
| CitmCatalog Small (minified) | 1,602 | 1,359 | 1.18× | Win |
| CitmCatalog Small (prettified) | 2,694 | 2,304 | 1.17× | Win |
| Discord Small (minified) | 1,481 | 1,597 | 0.93× | **Loss** |
| Discord Small (prettified) | 1,995 | 2,188 | 0.91× | **Loss** |
| Github Events Small (minified) | 1,896 | 2,108 | 0.90× | **Loss** |
| Google Maps Response Small (minified) | 1,696 | 1,543 | 1.10× | Win |
| Github Events Small (prettified) | 2,380 | 2,525 | 0.94× | **Loss** |
| Google Maps Response Small (prettified) | 3,215 | 2,968 | 1.08× | Win |
| Instruments Small (minified) | 2,516 | 1,570 | 1.60× | Win |
| Instruments Small (prettified) | 3,815 | 2,529 | 1.51× | Win |
| Marine IK Reverse Small (minified) | 818 | 652 | 1.26× | Win |
| Marine IK Reverse Small (prettified) | 2,831 | 2,307 | 1.23× | Win |
| Marine IK Small (minified) | 876 | 664 | 1.32× | Win |
| Marine IK Small (prettified) | 3,008 | 2,341 | 1.29× | Win |
| Random Small (minified) | 1,474 | 1,414 | 1.04× | Win |
| Random Small (prettified) | 2,202 | 2,114 | 1.04× | Win |
| Twitter Small (minified) | 2,011 | 2,081 | 0.97× | **Loss** |
| Twitter Small (prettified) | 2,567 | 2,651 | 0.97× | **Loss** |

#### Reused objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,811 | 1,544 | 1.17× | Win |
| Canada (minified) | 1,166 | 840 | 1.39× | Win |
| Apache Builds (prettified) | 2,297 | 2,488 | 0.92× | **Loss** |
| Canada (prettified) | 2,543 | 1,869 | 1.36× | Win |
| CitmCatalog (minified) | 2,215 | 1,867 | 1.19× | Win |
| CitmCatalog (prettified) | 4,359 | 3,772 | 1.16× | Win |
| Discord (minified) | 2,065 | 1,930 | 1.07× | Win |
| Discord (prettified) | 2,916 | 2,748 | 1.06× | Win |
| Github Events (minified) | 2,224 | 2,106 | 1.06× | Win |
| Google Maps Response (minified) | 1,718 | 1,511 | 1.14× | Win |
| Github Events (prettified) | 2,655 | 2,502 | 1.06× | Win |
| Google Maps Response (prettified) | 3,365 | 2,962 | 1.14× | Win |
| Instruments (minified) | 2,821 | 1,737 | 1.62× | Win |
| Instruments (prettified) | 4,216 | 2,716 | 1.55× | Win |
| Marine IK Reverse (minified) | 872 | 702 | 1.24× | Win |
| Marine IK Reverse (prettified) | 3,040 | 2,497 | 1.22× | Win |
| Marine IK (minified) | 881 | 719 | 1.22× | Win |
| Marine IK (prettified) | 3,010 | 2,551 | 1.18× | Win |
| Random (minified) | 1,369 | 1,490 | 0.92× | **Loss** |
| Random (prettified) | 2,109 | 2,253 | 0.94× | **Loss** |
| Twitter (minified) | 2,124 | 2,064 | 1.03× | Win |
| Twitter (prettified) | 2,755 | 2,685 | 1.03× | Win |
| Apache Builds Small (minified) | 2,245 | 2,213 | 1.01× | Win |
| Canada Small (minified) | 1,151 | 798 | 1.44× | Win |
| Apache Builds Small (prettified) | 2,873 | 2,765 | 1.04× | Win |
| Canada Small (prettified) | 2,720 | 1,876 | 1.45× | Win |
| CitmCatalog Small (minified) | 1,853 | 1,527 | 1.21× | Win |
| CitmCatalog Small (prettified) | 3,066 | 2,519 | 1.22× | Win |
| Discord Small (minified) | 1,829 | 1,865 | 0.98× | **Loss** |
| Discord Small (prettified) | 2,458 | 2,491 | 0.99× | **Loss** |
| Github Events Small (minified) | 2,366 | 2,367 | 1.00× | Tie |
| Google Maps Response Small (minified) | 1,838 | 1,609 | 1.14× | Win |
| Github Events Small (prettified) | 2,886 | 2,802 | 1.03× | Win |
| Google Maps Response Small (prettified) | 3,451 | 3,113 | 1.11× | Win |
| Instruments Small (minified) | 2,809 | 1,612 | 1.74× | Win |
| Instruments Small (prettified) | 4,176 | 2,573 | 1.62× | Win |
| Marine IK Reverse Small (minified) | 977 | 699 | 1.40× | Win |
| Marine IK Reverse Small (prettified) | 3,270 | 2,490 | 1.31× | Win |
| Marine IK Small (minified) | 1,041 | 727 | 1.43× | Win |
| Marine IK Small (prettified) | 3,449 | 2,530 | 1.36× | Win |
| Random Small (minified) | 1,670 | 1,555 | 1.07× | Win |
| Random Small (prettified) | 2,500 | 2,266 | 1.10× | Win |
| Twitter Small (minified) | 2,218 | 2,262 | 0.98× | **Loss** |
| Twitter Small (prettified) | 2,824 | 2,871 | 0.98× | **Loss** |

#### Freshly allocated objects: partial reading and validation

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (minified) | 7,881 | 2,991 | 2.64× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (prettified) | 8,982 | 3,453 | 2.60× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 4,892 | 2,736 | 1.79× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,192 | 3,339 | 1.85× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 9,917 | 7,666 | 1.29× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 10,757 | 8,396 | 1.28× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,886 | 6,532 | 0.90× | Loss |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,270 | 7,661 | 0.95× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,708 | 7,983 | 1.09× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 10,250 | 9,337 | 1.10× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,214 | 7,015 | 0.89× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,266 | 8,196 | 0.89× | Loss |

**Reused objects.**

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,282 | 3,104 | 2.67× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,324 | 3,604 | 2.59× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,285 | 2,896 | 1.82× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,552 | 3,488 | 1.88× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 10,409 | 7,707 | 1.35× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 11,329 | 8,579 | 1.32× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,311 | 6,804 | 0.93× | Loss |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,801 | 7,779 | 1.00× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,819 | 8,139 | 1.08× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 10,456 | 9,479 | 1.10× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,387 | 5,096 | 1.25× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,614 | 8,338 | 0.91× | Loss |

**Validation.**

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 3,004 | 2,520 | 1.19× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 3,947 | 2,457 | 1.61× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 3,223 | 2,428 | 1.33× | Win |

## 3. The single-pass path: `json_cursor` over raw text

The fused path is implemented as `json_cursor<parseOpts, read_buffer_ptr>`: a set of static navigation primitives over a raw `read_buffer_ptr` cursor, with the end pointer, the detected indentation and the string buffer held in a `parse_context` and the nesting depth passed down the call chain, walking the document exactly once. Several design decisions distinguish it:

**Compile-time key fusion.** For each reflected member, the expected token is baked into the binary at compile time as a fused literal. In minified known-order mode, an entire member header (leading comma, quoted key, and colon) collapses into a single constant:

```cpp
template<uint64_t index, typename literal_type> JSONIFIER_INLINE static constexpr auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
	if constexpr (index > 0) {
		return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	} else {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}
}
```

When the document arrives in the expected order, the overwhelmingly common case for machine-generated JSON, matching `,"name":` is one constant-length comparison, and the cursor jumps the entire header in a single add. No tokenizer, no tape, no per-character state machine.

**Adaptive out-of-order recovery.** When the known-order guess misses, the parser falls back to a compile-time hash map over the type's keys (`hash_map<value_type>::findIndex`), dispatches through a generated table, and, crucially, *learns*: a thread-local `antiHashStatesNew` array records which index actually matched at each slot, so a document stream with a consistently permuted key order pays the hash cost once and then rides the corrected fast path.

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans and skips every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, so in the worst case an O(members) parse becomes an O(members²) crawl over structural positions, per object, per document. That is a bound, not a measurement: in this sweep reversed key order costs simdjson 44% of its throughput on minified Marine IK Reverse under MSVC (45% on the AVX-512 MSVC build of §2.8), but on the other four AVX2 and NEON builds it stays within about 17% of declared order (§2.4). The model has no memory; the millionth permuted document costs exactly what the first one did.

Jsonifier's failure mode for the same situation is: one hash lookup, one dispatch-table indirection, and a learned correction. The mismatch cost is paid once per key slot per thread, not once per object instance. A feed of a billion documents with keys in reversed order parses at effectively the same throughput as a feed in declared order, because after the first document the "expected order" *is* the observed order. The schema-directed model converts key order from a per-document runtime tax into a per-stream calibration.

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `context.indentSize * depth`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (the string parse loop, whose in-register UTF-8 validation §6 covers) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra [1] with several architectural departures.

### 4.1 Step geometry as a per-compiler compile-time constant

simdjson classifies input per 64-byte block, and its generic indexer steps through the buffer 128 bytes per iteration: two 64-byte blocks, software-pipelined one block deep, a geometry it retains even on its widest kernel (the Icelake implementation invokes `index<128>`). Jsonifier generalizes the unit of work to a *step* of `simdBlocksPerStep` 64-byte blocks, with the constant chosen per ISA **and per compiler**:

```cpp
#if JSONIFIER_AVX512
	inline constexpr uint64_t simdBlocksPerStep{ 8 };
#elif JSONIFIER_AVX2
	inline constexpr uint64_t simdTapeStep{ 4 };
	#if JSONIFIER_COMPILER_CLANG
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
	#else
	inline constexpr uint64_t simdBlocksPerStep{ 8 };
	#endif
#elif JSONIFIER_NEON
	inline constexpr uint64_t simdTapeStep{ 8 };
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
#else
	inline constexpr uint64_t simdTapeStep{ 8 };
	inline constexpr uint64_t simdBlocksPerStep{ 8 };
#endif
```

`simd/simd_config.hpp` defines `simdBlocksPerStep` and `simdTapeStep` directly in a per-ISA `#if` chain, and only the AVX2 cell branches on the compiler: 4 blocks per step on Clang and 8 on GCC and MSVC, with a tape step of 4 on all three. AVX, SVE2 and the scalar fallback share the final branch. The AVX-512 cell defines no `simdTapeStep`, since its compress drain has no stepped scalar writer.

These values were selected by parameter sweeps across the CI targets. That Clang and GCC want *different* geometry on the identical ISA reflects how each compiler schedules the unrolled block bodies. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations, a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(const simd_array_t in01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNext() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit: a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional: `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNextInString`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4`; the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm (`followsNonquoteScalar` carries the cross-block bit), so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
	const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace  = simd::ws_collector::impl(in01, whitespaceTableLocal);
	const uint64_t op		   = simd::op_collector::impl(in01, opTable, spaceMask);
	const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
	const uint64_t scalarStart = scalar & ~follows;
	return op | simd::rope_detector<rope_block>::quotes | scalarStart;
}
```

Note the overload pair: the minified variant omits the whitespace collector entirely: an entire classification lane deleted at compile time when the caller declares the input minified. This is the same philosophy as the compiler-specific step constants: every fact known before runtime is burned into the instruction stream.

### 4.4 The drain architecture: bits → indices

Extraction, converting each 64-bit structural mask into byte offsets on the tape, is where naive implementations serialize hard, because the classic loop (`tzcnt`, store, `blsr`, repeat) is a loop-carried dependency chain of length popcount.

Jsonifier's answer differs by ISA.

**AVX-512: fully vectorized drain.** With `VBMI2` available, the bitmask never enters a scalar loop at all. `_mm512_maskz_compress_epi8` compresses a constant 0..63 byte ramp under the structural mask, producing the set-bit positions as packed bytes; four `cvtepu8_epi32` widenings plus a broadcast base-add stream up to 64 indices to the tape in a handful of instructions:

```cpp
const __m512i indexes			= _mm512_maskz_compress_epi8(bits,
			  _mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, 0x37363534, 0x33323130, 0x2f2e2d2c, 0x2b2a2928, 0x27262524, 0x23222120, 0x1f1e1d1c, 0x1b1a1918, 0x17161514, 0x13121110,
				  0x0f0e0d0c, 0x0b0a0908, 0x07060504, 0x03020100));
const __m512i startIndexLocal = _mm512_set1_epi32(base);
__m512i t0					  = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes));
_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
```

The widening cascade is guarded by the lane's precomputed popcount (`count > 16`, `> 32`, `> 48`), so sparse blocks pay for one store, not four. simdjson employs the same family of trick: its generic indexer exposes a `CUSTOM_BIT_INDEXER` hook, and the Icelake kernel fills it with its own `VBMI2` compress-based extractor. On this ISA the two libraries again share the inner mechanism, and the performance separation comes from the surrounding step architecture (§4.1, §4.4 drain scheduling below) rather than the extraction primitive.

**AVX2/AVX/NEON: the folded stepped drain.** Without byte-compress, extraction must use the tzcnt chain, but the chain's *structure* is still a compile-time decision. `write_indices_functor` emits one extract-advance pair per index; `write_indices_stepped_functor` groups them into unconditional bursts of `simdTapeStep` writes:

```cpp
template<auto...> struct write_indices_functor {
	using size_type = uint64_t;

	template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& bits, write_structural_index_ptr tape) noexcept {
		tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
		bits									  = simd::tape_writer_op::advance(bits);
	}
};

template<uint64_t step> struct write_indices_stepped_functor {
	using size_type = uint64_t;
	template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& bits, write_structural_index_ptr tape, uint64_t cnt) noexcept {
		if constexpr (index > 0) {
			if ((index < cnt)) [[unlikely]] {
				functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
				return true;
			} else {
				return false;
			}
		} else {
			functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
			return true;
		}
	}
};
```

The `functor_runner`'s `implAnd` expands a stepped range sequence `<0, 64, simdTapeStep>` through an `&&`-fold: each group writes `simdTapeStep` indices *unconditionally* (over-writing garbage past the true count is harmless, since the tape cursor only advances by the real popcount), and the fold short-circuits the moment a group's start index reaches the count. One predictable branch per `simdTapeStep` extractions instead of one per extraction.

Credit where due: simdjson's current generic `bit_indexer` implements the same stepped-burst pattern (`write_indexes_stepped<START, END, STEP>` via recursive template expansion, unconditional bursts, `simdjson_unlikely` short-circuit checks at each group boundary), with the burst size exposed as a build macro (`SIMDJSON_STRUCTURAL_INDEXER_STEP`, default 4). The two libraries have converged on the extraction inner loop itself. The divergences are in everything around it:

- **Coverage.** simdjson's stepped expansion runs to a fixed `STEP_UNTIL` of 24 set bits and falls back to a plain scalar loop for denser blocks; Jsonifier's fold covers the full 0..64 range at the same stride.
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep`, because the two constants interact. (§4.1).
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step: up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. One independent tzcnt chain per block (four or eight, per §4.1) with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, write_structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains; each lane knows its destination offset up front. The classification of the step's later blocks and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser; it is a *specialization* of the same `json_cursor` interface over `write_structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the cursor interface: `cursor_t<options, context_type>` picks the specialization from the context's `iterator_type`, and the `json_cursor<parseOpts, write_structural_index_ptr>` specialization supplies the token-navigation primitives:

```cpp
template<typename context_type> JSONIFIER_INLINE static bool skipValue(write_structural_index_ptr& iter, context_type& context) noexcept {
	if (iter >= context.endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>(iter, context);
	}
	const char first = static_cast<char>(*valuePtr(iter, context));
	if (first == '{' || first == '[') {
		int64_t depth{};
		while (iter < context.endIter) {
			depth += nestingDeltaTable[static_cast<uint64_t>(context.stringRoot[*iter])];
			++iter;
			if (depth == 0) {
				return true;
			}
		}
		return reject<parse_statuses::unexpected_string_end>(iter, context);
	}
	++iter;
	return true;
}
```

`nestingDeltaTable` maps `{` and `[` to +1, `}` and `]` to −1 and every other byte to 0, so the loop needs one table load and one add per tape entry and a single branch on the running depth.

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters, one indexed byte load per structural entry, and never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer cursor, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

The asymmetry defines the routing rule. When the caller wants *every* value (full-document parse into a reflected type), skips are rare and the tape is overhead. When the caller wants a *few* values from a large document (partial reading), or wants only the structure itself (prettify/minify, where the transformation is literally "copy bytes, adjusting whitespace at structural positions"), skips dominate and the tape converts O(bytes) navigation into O(structurals).

The same routing logic appears in stage 1's own entry point: `reset<minified>` selects between the whitespace-aware and whitespace-free classification pipelines, and the tail-block handling pads with `0x20`, indexes the pad, then retroactively pops any tape entries pointing past the true document length: branchless main loop, exact tape.

## 6. Distributed UTF-8 validation

Let's be precise about what simdjson does, because it is also an in-register scheme: `json_structural_indexer::next` feeds every 64-byte block into `utf8_checker::check_next_input` using the very registers the classifier just loaded. Validation is fused into stage 1, costs no extra pass over memory, and covers the entire input. (The standalone `generic_validate_utf8` exists too, for the buffer-validation API.) On Demand then additionally defers some string-level checks to traversal.

So the distinction is not "in-register versus dedicated pass"; both libraries validate in registers already in flight. The distinction is **scope and location**:

- **simdjson** validates *all input bytes*, in *stage 1*. Elegant and total, but it means the range checker runs over structural characters, whitespace, numbers, and literals, bytes that a JSON parser's own grammar already constrains to ASCII. For a document that is 70% non-string content, 70% of the validation work confirms what token matching would prove for free.
- **Jsonifier** validates *string bytes only*, in the *string parse loop*. Everything outside strings is implicitly ASCII-constrained: structural characters are matched literally, numbers pass through a digit-table parser, `true`/`false`/`null` are compared as packed integer constants; any non-ASCII byte in those positions is a parse error by construction, no range checker required. The only place arbitrary bytes can legally appear is inside strings, and that is exactly, and only, where `utf8_register_validator` runs.

One qualifier bounds the claim: simdjson's checker has an ASCII fast path, so on ASCII-heavy input its whole-buffer cost is roughly one reduction and one test per block. The scope distinction survives it, since Jsonifier runs no validation instructions at all over non-string bytes.

Jsonifier ships the same range-based algorithm simdjson uses (the `byte1High`/`byte1Low`/`byte2High` nibble-lookup classifier with the `carry`/`tooShort`/`tooLong`/`surrogate` error-bit algebra) in two deployments. The first, the standalone `validateUtf8` built on `utf8_checker`, is the conventional block validator, with the same ASCII fast path (`orAll` the block's registers, one test, skip the multibyte machinery). The second is the distributed one:

```cpp
if (delimiters == static_cast<integer_type>(0)) {
	validator.checkRegister(simdValue);
	string1Start += bytesProcessed;
	string2 += bytesProcessed;
	continue;
}
```

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte), so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator. The position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The companion paper *Batched Drain, Fused Scan* [3] covers this mechanism in full in its §6.1.

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes*: every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 104 of 110 minified documents, and on the M2 Pro on 22 of 22 full-size and 15 of 22 Small prettified documents; slower on 63 of 66 prettified x86 documents (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared cursor interface |
| Out-of-order keys | Forward scan + wrap-around rescan per lookup, per object; no memory across documents | One hash fallback, then learned per-slot order correction (thread-local, per stream) |
| Schema knowledge (known-type workloads) | Exists in caller's traversal code, invisible to the library | Declared once via reflection, consumed by the architecture |
| Step size | 128 bytes (two 64-byte blocks), pipelined one block deep | 256–512 bytes, per-(ISA × compiler) constant |
| Bit extraction | Stepped tzcnt bursts (global STEP macro, scalar tail past 24 bits); VBMI2 compress on Icelake | Same primitives, but full-range folded bursts with per-toolchain-swept stride |
| Drain scheduling | Previous block drained during current block's classification | Whole-step deferred batch drain, lane offsets precomputed from popcounts |
| Escape short-circuit | Yes (zero-backslash fast exit) | Yes (same) |
| In-string prefix-XOR | Computed every block | Skipped for quoteless blocks |
| UTF-8 validation | In-register, fused into stage 1, over all input bytes (ASCII fast path per block); verdict at EOF | In-register, fused into string parsing, over string bytes only, carried across the width cascade; verdict per string |
| Specialization axis | Runtime CPU dispatch through virtual functions (core/DOM API); On Demand mostly fixed at compile time | One ISA tier per build, chosen at configure time; no virtual calls and no runtime dispatch, so everything is compile-time (Cathedral Architecture) |

The last row is the root of every other difference. simdjson's core API can ship one x86 binary that picks its instruction set at run time; Jsonifier deliberately does not. Its founding constraint is that apart from the data, nothing is decided at run time. The schema, the compiler, the parse options and the instruction-set tier are all known at build time, so every routing decision in this paper (tape or no tape, whitespace lane or not, 4 blocks or 8, tape step of 4 or 8) is still resolved before the first byte is read.

The tier is chosen when CMake configures the project, by a detector run on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS` (and capped with `JSONIFIER_SIMD_TIER`), and the whole library is compiled once for it, so every call is a direct, inlinable call with no per-call check. simdjson dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time. Jsonifier keeps one tier per build because per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, which GCC and Clang then reject. The cost is portability: a binary for mixed hardware is configured for the lowest tier it must support.

## 8. Conclusion

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 169 of 245 freshly allocated tests: all 25 POD-type tests, 104 of 110 minified documents and 37 of 44 prettified documents on the M2 Pro, while the two-stage path wins 63 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 244 of 245 tests and loses the other, its stage-1 + reflection path beats simdjson's reflection reader on 79 of 88, and on partial reading it wins 34 of 40. On the three AVX-512 builds the faster path beats simdjson On Demand on 145 of 147 tests freshly allocated and 144 of 147 reused, and its stage-1 + reflection path beats simdjson's reflection reader on 28 of 44 freshly allocated and 36 of 44 reused. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier [4]. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance [5].*

## References

1. G. Langdale, D. Lemire. "Parsing Gigabytes of JSON per Second." *The VLDB Journal*, 28(6), 2019.
2. simdjson. https://github.com/simdjson/simdjson (commit 449f38e63).
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
