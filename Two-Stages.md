# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across the 220 corpus tests of five builds, skipping the tape wins 109 of the 110 minified documents and 84 of the 110 prettified ones, with 1 tie; the tape's 26 wins come mostly from the float-heavy Marine IK documents on x86 and from prettified documents on the M2 Pro. Three AVX-512 builds show the same shape: the fused path wins 64 of their 66 minified documents, with the other 2 tied, and 44 of their 66 prettified ones.

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

For full-document parsing into known types, the tape is overhead in principle, and in practice skipping it is faster on nearly every document measured, indented or not, with the float-heavy exceptions §2.4 names. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying: workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One objection is that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads (schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents), this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns, parsing documents into concrete types the caller has defined, the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is legibility: as hand-written traversal the knowledge is opaque to the library, while as a reflection registration it becomes architecture (fused member headers, adaptive order recovery, and the routing rule above). It is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep [5] published November 27, 2026 (the two macOS builds finished on November 28), with Jsonifier [a1328b5](https://github.com/nihilai-collective/jsonifier/commit/a1328b5), Glaze [283c966](https://github.com/stephenberry/glaze/commit/283c9664), simdjson [4ba1cb3](https://github.com/simdjson/simdjson/commit/4ba1cb3ae) and BenchmarkSuite [ff0f871](https://github.com/nihilai-collective/benchmarksuite/commit/ff0f871) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. The sweep registers the two-stage path on the 44 corpus tests of each build but not on the five POD-type tests, so the fused-against-two-stage comparisons below cover corpus documents only. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Eight freshly allocated full-parse rows contain non-converged results, six under MSVC and one on each M2 Pro build, as do seven reused rows, four under MSVC, two on macOS/GCC and one on macOS/Clang; every Linux row converged. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head on the 220 corpus tests, the fused path wins 193, 1 is a statistical tie, and the two-stage path wins 26:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 37 | 0 | 7 | 38 of 44 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 39 | 0 | 5 | 44 of 44 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 38 | 0 | 6 | 43 of 44 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 39 | 1 | 4 | 43 of 44 |
| **Aggregate** | **193** | **1** | **26** | **212 of 220** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 8 / 0 / 3 | 18 / 0 / 4 | 19 / 0 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 7 / 0 / 4 | 17 / 0 / 5 | 15 / 1 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **244 / 1 / 0** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 38 | 0 | 6 | 40 of 44 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 39 | 0 | 5 | 44 of 44 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 1 | 1 | 42 of 44 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 39 | 0 | 5 | 43 of 44 |
| **Aggregate** | **198** | **1** | **21** | **213 of 220** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 8 / 0 / 3 | 18 / 0 / 4 | 20 / 1 / 1 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 8 / 0 / 3 | 17 / 0 / 5 | 18 / 0 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **245 / 0 / 0** |

With objects reused, across the 220 corpus tests the fused path wins 198, ties 1 and loses 21 to the two-stage path. On x86 the fused path wins all 66 minified documents and 51 of the 66 prettified ones; the tape's 15 wins are the Marine IK family plus prettified CitmCatalog under MSVC and Canada Small under MSVC and Linux/Clang. On the M2 Pro the fused path wins 43 of the 44 minified documents (the tape takes Twitter Small on macOS/Clang) and 38 of the 44 prettified ones, with 1 tie and 5 tape wins. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins all 245 too.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 642 | n/a | 183 | n/a | n/a | Win |
| Double (POD) | 762 | n/a | 330 | n/a | n/a | Win |
| Int64 (POD) | 1,010 | n/a | 325 | n/a | n/a | Win |
| String (POD) | 653 | n/a | 483 | n/a | n/a | Win |
| Uint64 (POD) | 994 | n/a | 288 | n/a | n/a | Win |
| Apache Builds (minified) | 857 | 765 | 682 | 1.12× | Fused | Win |
| Canada (minified) | 646 | 612 | 386 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,063 | 957 | 827 | 1.11× | Fused | Win |
| Canada (prettified) | 1,236 | 1,365 (n/c) | 962 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,508 | 1,129 (n/c) | 552 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 2,660 | 2,591 | 1,275 | 1.03× | Fused | Win |
| Discord (minified) | 1,260 | 1,012 | 768 | 1.25× | Fused | Win |
| Discord (prettified) | 1,681 | 1,453 | 996 | 1.16× | Fused | Win |
| Github Events (minified) | 948 | 868 | 676 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 1,229 | 1,010 | 571 | 1.22× | Fused | Win |
| Github Events (prettified) | 976 | 905 | 652 | 1.08× | Fused | Win |
| Google Maps Response (prettified) | 2,287 | 2,183 | 1,240 | 1.05× | Fused | Win |
| Instruments (minified) | 2,024 (n/c) | 1,375 | 857 (n/c) | 1.47× | Fused | Win |
| Instruments (prettified) | 2,574 | 2,272 | 1,456 | 1.13× | Fused | Win |
| Marine IK Reverse (minified) | 672 | 554 | 203 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | 1,869 (n/c) | 1,992 (n/c) | 681 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 668 | 523 | 357 | 1.28× | Fused | Win |
| Marine IK (prettified) | 1,955 | 2,021 | 1,354 | 0.97× | Two-stage | Win |
| Random (minified) | 882 (n/c) | 724 | 570 | 1.22× | Fused | Win |
| Random (prettified) | 1,268 | 1,084 | 875 | 1.17× | Fused | Win |
| Twitter (minified) | 1,390 | 1,057 | 886 | 1.32× | Fused | Win |
| Twitter (prettified) | 1,536 | 1,348 | 1,139 (n/c) | 1.14× | Fused | Win |
| Apache Builds Small (minified) | 1,426 | 1,238 | 842 | 1.15× | Fused | Win |
| Canada Small (minified) | 586 | 540 | 337 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,624 | 1,551 | 1,047 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,247 | 1,303 | 808 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 813 | 700 | 540 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 1,225 | 1,201 | 945 | 1.02× | Fused | Win |
| Discord Small (minified) | 1,287 | 1,008 | 740 | 1.28× | Fused | Win |
| Discord Small (prettified) | 1,551 | 1,339 | 1,013 | 1.16× | Fused | Win |
| Github Events Small (minified) | 1,209 | 977 | 774 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 1,140 | 977 | 599 | 1.17× | Fused | Win |
| Github Events Small (prettified) | 1,211 | 1,266 | 986 | 0.96× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,082 | 1,981 | 1,218 | 1.05× | Fused | Win |
| Instruments Small (minified) | 2,189 | 1,446 | 897 | 1.51× | Fused | Win |
| Instruments Small (prettified) | 2,679 | 2,339 | 1,456 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 462 | 435 | 158 | 1.06× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,327 | 1,579 | 592 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 511 | 446 | 306 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,472 | 1,642 | 1,095 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,008 | 833 | 534 | 1.21× | Fused | Win |
| Random Small (prettified) | 1,391 | 1,254 | 852 | 1.11× | Fused | Win |
| Twitter Small (minified) | 1,880 | 1,354 | 1,407 | 1.39× | Fused | Win |
| Twitter Small (prettified) | 1,898 | 1,743 | 1,842 | 1.09× | Fused | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,007 | n/a | 480 | n/a | n/a | Win |
| Double (POD) | 1,205 | n/a | 442 | n/a | n/a | Win |
| Int64 (POD) | 1,475 | n/a | 371 | n/a | n/a | Win |
| String (POD) | 1,238 | n/a | 949 | n/a | n/a | Win |
| Uint64 (POD) | 1,703 | n/a | 353 | n/a | n/a | Win |
| Apache Builds (minified) | 1,802 | 1,397 | 1,233 | 1.29× | Fused | Win |
| Canada (minified) | 884 | 857 | 803 | 1.03× | Fused | Win |
| Apache Builds (prettified) | 2,111 | 1,645 | 1,578 | 1.28× | Fused | Win |
| Canada (prettified) | 1,913 | 1,895 | 1,804 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,295 | 1,685 | 1,114 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,736 | 3,443 | 2,367 | 1.09× | Fused | Win |
| Discord (minified) | 2,374 | 1,744 | 933 | 1.36× | Fused | Win |
| Discord (prettified) | 3,007 | 2,463 | 1,348 | 1.22× | Fused | Win |
| Github Events (minified) | 1,759 | 1,456 | 961 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 2,527 | 1,714 | 1,074 | 1.47× | Fused | Win |
| Github Events (prettified) | 2,048 | 1,691 | 1,149 | 1.21× | Fused | Win |
| Google Maps Response (prettified) | 3,692 | 2,971 | 2,215 | 1.24× | Fused | Win |
| Instruments (minified) | 2,480 | 1,934 | 716 | 1.28× | Fused | Win |
| Instruments (prettified) | 3,541 | 2,975 | 1,174 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | 801 | 734 | 591 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,424 | 2,513 | 2,151 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 820 | 740 | 587 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,482 | 2,529 | 2,131 | 0.98× | Two-stage | Win |
| Random (minified) | 1,612 | 1,133 | 818 | 1.42× | Fused | Win |
| Random (prettified) | 2,219 | 1,667 | 1,234 | 1.33× | Fused | Win |
| Twitter (minified) | 1,988 | 1,614 | 924 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,506 | 1,987 | 1,226 | 1.26× | Fused | Win |
| Apache Builds Small (minified) | 2,949 | 2,021 | 1,384 | 1.46× | Fused | Win |
| Canada Small (minified) | 826 | 783 | 743 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 3,091 | 2,365 | 1,713 | 1.31× | Fused | Win |
| Canada Small (prettified) | 1,787 | 1,798 | 1,789 | 0.99× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,727 | 1,368 | 978 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 2,463 | 2,312 | 1,715 | 1.07× | Fused | Win |
| Discord Small (minified) | 2,215 | 1,678 | 767 | 1.32× | Fused | Win |
| Discord Small (prettified) | 2,602 | 2,180 | 1,041 | 1.19× | Fused | Win |
| Github Events Small (minified) | 2,210 | 1,730 | 1,357 | 1.28× | Fused | Win |
| Google Maps Response Small (minified) | 2,352 | 1,581 | 1,083 | 1.49× | Fused | Win |
| Github Events Small (prettified) | 2,557 | 2,117 | 1,717 | 1.21× | Fused | Win |
| Google Maps Response Small (prettified) | 3,489 | 2,789 | 2,213 | 1.25× | Fused | Win |
| Instruments Small (minified) | 2,519 | 1,904 | 657 | 1.32× | Fused | Win |
| Instruments Small (prettified) | 3,550 | 2,930 | 1,087 | 1.21× | Fused | Win |
| Marine IK Reverse Small (minified) | 751 | 674 | 497 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,092 | 2,381 | 1,836 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 819 | 728 | 511 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,261 | 2,546 | 1,894 | 0.89× | Two-stage | Win |
| Random Small (minified) | 2,084 | 1,442 | 849 | 1.45× | Fused | Win |
| Random Small (prettified) | 2,715 | 2,112 | 1,292 | 1.29× | Fused | Win |
| Twitter Small (minified) | 2,666 | 2,203 | 1,011 | 1.21× | Fused | Win |
| Twitter Small (prettified) | 3,168 | 2,628 | 1,302 | 1.21× | Fused | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,416 | n/a | 488 | n/a | n/a | Win |
| Double (POD) | 1,297 | n/a | 433 | n/a | n/a | Win |
| Int64 (POD) | 1,838 | n/a | 400 | n/a | n/a | Win |
| String (POD) | 1,377 | n/a | 918 | n/a | n/a | Win |
| Uint64 (POD) | 1,894 | n/a | 377 | n/a | n/a | Win |
| Apache Builds (minified) | 1,846 | 1,384 | 1,330 | 1.33× | Fused | Win |
| Canada (minified) | 942 | 804 | 768 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 2,139 | 1,656 | 1,653 | 1.29× | Fused | Win |
| Canada (prettified) | 1,993 | 1,898 | 1,714 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,563 | 1,924 | 1,543 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 4,063 | 3,679 | 3,010 | 1.10× | Fused | Win |
| Discord (minified) | 2,568 | 1,835 | 1,345 | 1.40× | Fused | Win |
| Discord (prettified) | 3,223 | 2,559 | 1,939 | 1.26× | Fused | Win |
| Github Events (minified) | 1,884 | 1,569 | 1,498 | 1.20× | Fused | Win |
| Google Maps Response (minified) | 2,578 | 1,678 | 1,226 | 1.54× | Fused | Win |
| Github Events (prettified) | 2,115 | 1,975 | 1,639 | 1.07× | Fused | Win |
| Google Maps Response (prettified) | 3,855 | 3,110 | 2,397 | 1.24× | Fused | Win |
| Instruments (minified) | 3,195 | 2,196 | 1,502 | 1.46× | Fused | Win |
| Instruments (prettified) | 4,041 | 3,358 | 2,415 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | 828 | 711 | 543 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,526 | 2,559 | 2,002 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 843 | 717 | 562 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,547 | 2,568 | 2,044 | 0.99× | Two-stage | Win |
| Random (minified) | 1,544 | 1,134 | 989 | 1.36× | Fused | Win |
| Random (prettified) | 2,159 | 1,601 | 1,492 | 1.35× | Fused | Win |
| Twitter (minified) | 2,271 | 1,635 | 1,149 | 1.39× | Fused | Win |
| Twitter (prettified) | 2,597 | 2,047 | 1,509 | 1.27× | Fused | Win |
| Apache Builds Small (minified) | 2,977 | 2,019 | 1,732 | 1.47× | Fused | Win |
| Canada Small (minified) | 928 | 783 | 666 | 1.19× | Fused | Win |
| Apache Builds Small (prettified) | 3,088 | 2,362 | 2,138 | 1.31× | Fused | Win |
| Canada Small (prettified) | 1,973 | 1,933 | 1,577 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,921 | 1,451 | 1,218 | 1.32× | Fused | Win |
| CitmCatalog Small (prettified) | 2,699 | 2,445 | 2,127 | 1.10× | Fused | Win |
| Discord Small (minified) | 2,355 | 1,665 | 1,306 | 1.41× | Fused | Win |
| Discord Small (prettified) | 2,601 | 2,115 | 1,757 | 1.23× | Fused | Win |
| Github Events Small (minified) | 2,382 | 1,724 | 1,539 | 1.38× | Fused | Win |
| Google Maps Response Small (minified) | 2,409 | 1,597 | 1,193 | 1.51× | Fused | Win |
| Github Events Small (prettified) | 2,579 | 2,001 | 1,960 | 1.29× | Fused | Win |
| Google Maps Response Small (prettified) | 3,692 | 2,998 | 2,393 | 1.23× | Fused | Win |
| Instruments Small (minified) | 3,236 | 2,178 | 1,382 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 3,942 | 3,316 | 2,236 | 1.19× | Fused | Win |
| Marine IK Reverse Small (minified) | 868 | 751 | 500 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,356 | 2,627 | 1,847 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 928 | 776 | 507 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,515 | 2,735 | 1,882 | 0.92× | Two-stage | Win |
| Random Small (minified) | 2,110 | 1,478 | 1,078 | 1.43× | Fused | Win |
| Random Small (prettified) | 2,741 | 2,214 | 1,650 | 1.24× | Fused | Win |
| Twitter Small (minified) | 3,199 | 2,228 | 1,383 | 1.44× | Fused | Win |
| Twitter Small (prettified) | 3,270 | 2,786 | 1,790 | 1.17× | Fused | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,653 | n/a | 381 | n/a | n/a | Win |
| Double (POD) | 795 | n/a | 247 | n/a | n/a | Win |
| Int64 (POD) | 1,047 | n/a | 269 | n/a | n/a | Win |
| String (POD) | 916 | n/a | 474 | n/a | n/a | Win |
| Uint64 (POD) | 1,215 | n/a | 294 | n/a | n/a | Win |
| Apache Builds (minified) | 1,160 | 1,056 | 767 | 1.10× | Fused | Win |
| Canada (minified) | 757 | 625 | 575 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 1,341 | 1,259 | 949 | 1.06× | Fused | Win |
| Canada (prettified) | 1,422 | 1,259 | 1,069 | 1.13× | Fused | Win |
| CitmCatalog (minified) | 2,083 | 1,327 | 916 | 1.57× | Fused | Win |
| CitmCatalog (prettified) | 2,757 | 2,659 | 2,125 | 1.04× | Fused | Win |
| Discord (minified) | 1,738 | 1,286 | 973 | 1.35× | Fused | Win |
| Discord (prettified) | 1,658 | 1,749 | 1,416 | 0.95× | Two-stage | Win |
| Github Events (minified) | 1,546 | 1,454 | 912 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 1,806 | 1,304 | 761 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,609 | 1,643 | 1,224 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,115 | 2,317 | 1,449 | 0.91× | Two-stage | Win |
| Instruments (minified) | 2,683 | 1,640 | 1,220 | 1.64× | Fused | Win |
| Instruments (prettified) | 2,489 | 2,418 | 1,782 | 1.03× | Fused | Win |
| Marine IK Reverse (minified) | 618 | 475 | 391 | 1.30× | Fused | Win |
| Marine IK Reverse (prettified) | 1,947 | 1,729 (n/c) | 1,645 | 1.13× | Fused | Win |
| Marine IK (minified) | 601 | 443 | 384 | 1.36× | Fused | Win |
| Marine IK (prettified) | 1,699 | 1,644 | 1,594 | 1.03× | Fused | Win |
| Random (minified) | 1,073 | 902 | 709 | 1.19× | Fused | Win |
| Random (prettified) | 1,543 | 1,368 | 1,059 | 1.13× | Fused | Win |
| Twitter (minified) | 2,203 | 1,456 | 975 | 1.51× | Fused | Win |
| Twitter (prettified) | 2,389 | 2,021 | 1,339 | 1.18× | Fused | Win |
| Apache Builds Small (minified) | 1,592 | 1,429 | 893 | 1.11× | Fused | Win |
| Canada Small (minified) | 744 | 614 | 483 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 1,637 | 1,667 | 1,109 | 0.98× | Two-stage | Win |
| Canada Small (prettified) | 1,541 | 1,426 | 1,115 | 1.08× | Fused | Win |
| CitmCatalog Small (minified) | 1,164 | 1,019 | 849 | 1.14× | Fused | Win |
| CitmCatalog Small (prettified) | 1,863 | 1,663 | 1,404 | 1.12× | Fused | Win |
| Discord Small (minified) | 1,763 | 1,376 | 903 | 1.28× | Fused | Win |
| Discord Small (prettified) | 1,887 | 1,761 | 1,249 | 1.07× | Fused | Win |
| Github Events Small (minified) | 1,306 | 1,272 | 1,178 | 1.03× | Fused | Win |
| Google Maps Response Small (minified) | 1,663 | 1,214 | 729 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 1,684 | 1,505 | 1,393 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,288 | 2,176 | 1,465 | 1.05× | Fused | Win |
| Instruments Small (minified) | 2,754 | 1,686 | 1,130 | 1.63× | Fused | Win |
| Instruments Small (prettified) | 2,388 | 2,492 | 1,744 | 0.96× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 646 | 533 | 388 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,747 | 1,886 | 1,367 | 0.93× | Two-stage | Win |
| Marine IK Small (minified) | 705 | 568 | 372 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,006 | 1,957 | 1,325 | 1.03× | Fused | Win |
| Random Small (minified) | 1,227 | 925 | 589 | 1.33× | Fused | Win |
| Random Small (prettified) | 1,552 | 1,429 | 979 | 1.09× | Fused | Win |
| Twitter Small (minified) | 2,390 | 1,861 | 1,133 | 1.28× | Fused | Win |
| Twitter Small (prettified) | 2,518 | 2,196 | 1,382 | 1.15× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,271 | n/a | 312 | n/a | n/a | Win |
| Double (POD) | 868 | n/a | 304 | n/a | n/a | Win |
| Int64 (POD) | 1,059 | n/a | 295 | n/a | n/a | Win |
| String (POD) | 1,023 | n/a | 714 | n/a | n/a | Win |
| Uint64 (POD) | 1,073 | n/a | 274 | n/a | n/a | Win |
| Apache Builds (minified) | 1,332 | 1,218 | 834 | 1.09× | Fused | Win |
| Canada (minified) | 735 | 644 | 540 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 1,501 | 1,399 | 1,116 | 1.07× | Fused | Win |
| Canada (prettified) | 1,529 | 1,415 | 1,195 | 1.08× | Fused | Win |
| CitmCatalog (minified) | 2,132 | 1,482 | 853 | 1.44× | Fused | Win |
| CitmCatalog (prettified) | 3,204 | 2,762 | 1,786 | 1.16× | Fused | Win |
| Discord (minified) | 2,377 | 1,684 | 765 | 1.41× | Fused | Win |
| Discord (prettified) | 2,412 | 1,976 | 1,113 | 1.22× | Fused | Win |
| Github Events (minified) | 1,745 | 1,578 | 888 | 1.11× | Fused | Win |
| Google Maps Response (minified) | 1,869 | 1,417 | 825 | 1.32× | Fused | Win |
| Github Events (prettified) | 1,843 | 1,771 | 1,066 | 1.04× | Fused | Win |
| Google Maps Response (prettified) | 2,730 | 2,328 | 1,647 | 1.17× | Fused | Win |
| Instruments (minified) | 2,681 | 1,767 | 687 | 1.52× | Fused | Win |
| Instruments (prettified) | 2,928 | 2,526 | 1,126 | 1.16× | Fused | Win |
| Marine IK Reverse (minified) | 694 | 508 | 400 | 1.37× | Fused | Win |
| Marine IK Reverse (prettified) | 2,002 | 1,854 | 1,242 | 1.08× | Fused | Win |
| Marine IK (minified) | 618 (n/c) | 496 | 373 | 1.25× | Fused | Win |
| Marine IK (prettified) | 1,875 | 1,818 | 1,352 | 1.03× | Fused | Win |
| Random (minified) | 1,108 | 916 | 631 | 1.21× | Fused | Win |
| Random (prettified) | 1,630 | 1,362 | 917 | 1.20× | Fused | Win |
| Twitter (minified) | 2,046 | 1,712 | 849 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,415 | 1,986 | 919 | 1.22× | Fused | Win |
| Apache Builds Small (minified) | 1,601 | 1,398 | 679 | 1.15× | Fused | Win |
| Canada Small (minified) | 576 | 547 | 411 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,436 | 1,442 | 936 | 1.00× | Tie | Win |
| Canada Small (prettified) | 1,489 | 1,267 | 968 | 1.18× | Fused | Win |
| CitmCatalog Small (minified) | 1,245 | 1,011 | 684 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 1,790 | 1,642 | 1,196 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,153 | 1,672 | 651 | 1.29× | Fused | Win |
| Discord Small (prettified) | 2,206 | 1,895 | 875 | 1.16× | Fused | Win |
| Github Events Small (minified) | 1,550 | 1,363 | 1,001 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 1,769 | 1,361 | 804 | 1.30× | Fused | Win |
| Github Events Small (prettified) | 1,718 | 1,636 | 1,276 | 1.05× | Fused | Win |
| Google Maps Response Small (prettified) | 1,905 | 2,212 | 1,622 | 0.86× | Two-stage | Win |
| Instruments Small (minified) | 2,783 | 1,443 | 625 | 1.93× | Fused | Win |
| Instruments Small (prettified) | 3,025 | 2,685 | 1,033 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 644 | 556 | 318 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,858 | 1,926 | 1,175 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 677 | 513 | 332 | 1.32× | Fused | Win |
| Marine IK Small (prettified) | 1,718 | 1,964 | 1,210 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,140 | 1,196 | 657 | 0.95× | Two-stage | Win |
| Random Small (prettified) | 1,887 | 1,688 | 1,008 | 1.12× | Fused | Win |
| Twitter Small (minified) | 2,329 | 1,881 | 847 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,482 | 2,290 | 1,095 | 1.08× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 736 | n/a | 182 | n/a | n/a | Win |
| Double (POD) | 981 | n/a | 365 | n/a | n/a | Win |
| Int64 (POD) | 1,472 | n/a | 368 | n/a | n/a | Win |
| String (POD) | 1,189 | n/a | 689 | n/a | n/a | Win |
| Uint64 (POD) | 1,459 | n/a | 325 | n/a | n/a | Win |
| Apache Builds (minified) | 1,386 | 1,210 | 1,065 | 1.15× | Fused | Win |
| Canada (minified) | 794 | 734 | 452 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 1,641 | 1,451 | 1,282 (n/c) | 1.13× | Fused | Win |
| Canada (prettified) | 1,658 | 1,581 | 1,051 (n/c) | 1.05× | Fused | Win |
| CitmCatalog (minified) | 1,805 | 1,290 | 631 | 1.40× | Fused | Win |
| CitmCatalog (prettified) | 3,046 | 3,078 | 1,457 | 0.99× | Two-stage | Win |
| Discord (minified) | 2,114 | 1,446 | 1,010 | 1.46× | Fused | Win |
| Discord (prettified) | 2,398 | 2,055 | 1,357 | 1.17× | Fused | Win |
| Github Events (minified) | 1,700 | 1,299 | 915 | 1.31× | Fused | Win |
| Google Maps Response (minified) | 1,433 | 1,233 | 611 | 1.16× | Fused | Win |
| Github Events (prettified) | 1,664 | 1,477 | 1,169 | 1.13× | Fused | Win |
| Google Maps Response (prettified) | 2,579 | 2,402 | 1,360 | 1.07× | Fused | Win |
| Instruments (minified) | 2,519 | 1,579 | 959 | 1.60× | Fused | Win |
| Instruments (prettified) | 3,070 | 2,637 | 1,658 | 1.16× | Fused | Win |
| Marine IK Reverse (minified) | 757 | 618 | 213 | 1.23× | Fused | Win |
| Marine IK Reverse (prettified) | 2,195 | 2,315 | 729 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 743 | 587 | 373 | 1.27× | Fused | Win |
| Marine IK (prettified) | 2,121 | 2,268 | 1,440 (n/c) | 0.94× | Two-stage | Win |
| Random (minified) | 1,334 | 952 | 699 | 1.40× | Fused | Win |
| Random (prettified) | 1,807 | 1,508 | 1,088 | 1.20× | Fused | Win |
| Twitter (minified) | 2,221 | 1,532 | 1,190 | 1.45× | Fused | Win |
| Twitter (prettified) | 2,176 | 1,915 | 1,590 | 1.14× | Fused | Win |
| Apache Builds Small (minified) | 1,894 | 1,603 | 1,185 | 1.18× | Fused | Win |
| Canada Small (minified) | 733 | 666 | 365 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,095 | 1,954 | 1,466 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,513 | 1,566 | 883 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,142 | 903 | 690 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 1,660 | 1,501 | 1,199 | 1.11× | Fused | Win |
| Discord Small (minified) | 1,960 | 1,347 | 1,081 | 1.46× | Fused | Win |
| Discord Small (prettified) | 2,132 | 1,645 (n/c) | 1,454 | 1.30× | Fused | Win |
| Github Events Small (minified) | 1,742 | 1,323 | 877 | 1.32× | Fused | Win |
| Google Maps Response Small (minified) | 1,355 | 1,142 | 667 | 1.19× | Fused | Win |
| Github Events Small (prettified) | 1,844 | 1,602 | 1,073 | 1.15× | Fused | Win |
| Google Maps Response Small (prettified) | 2,372 | 2,275 | 1,382 | 1.04× | Fused | Win |
| Instruments Small (minified) | 2,487 | 1,595 | 982 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 2,946 | 2,570 | 1,572 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 565 | 502 | 169 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,532 | 1,893 | 645 | 0.81× | Two-stage | Win |
| Marine IK Small (minified) | 632 | 560 | 324 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,772 | 2,063 | 1,280 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,299 | 1,022 | 717 | 1.27× | Fused | Win |
| Random Small (prettified) | 1,723 | 1,534 | 1,093 | 1.12× | Fused | Win |
| Twitter Small (minified) | 2,390 | 1,595 | 1,563 | 1.50× | Fused | Win |
| Twitter Small (prettified) | 2,280 | 2,073 | 2,029 | 1.10× | Fused | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,204 | n/a | 489 | n/a | n/a | Win |
| Double (POD) | 1,341 | n/a | 461 | n/a | n/a | Win |
| Int64 (POD) | 1,763 | n/a | 383 | n/a | n/a | Win |
| String (POD) | 1,827 | n/a | 1,221 | n/a | n/a | Win |
| Uint64 (POD) | 2,006 | n/a | 374 | n/a | n/a | Win |
| Apache Builds (minified) | 3,111 | 2,056 | 1,690 | 1.51× | Fused | Win |
| Canada (minified) | 1,043 | 1,007 | 957 | 1.03× | Fused | Win |
| Apache Builds (prettified) | 3,386 | 2,347 | 2,149 | 1.44× | Fused | Win |
| Canada (prettified) | 2,197 | 2,184 | 2,131 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,828 | 1,984 | 1,278 | 1.43× | Fused | Win |
| CitmCatalog (prettified) | 4,355 | 3,952 | 2,708 | 1.10× | Fused | Win |
| Discord (minified) | 3,621 | 2,302 | 1,103 | 1.57× | Fused | Win |
| Discord (prettified) | 4,225 | 3,173 | 1,599 | 1.33× | Fused | Win |
| Github Events (minified) | 2,462 | 1,920 | 1,139 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 2,925 | 1,889 | 1,147 | 1.55× | Fused | Win |
| Github Events (prettified) | 2,836 | 2,202 | 1,371 | 1.29× | Fused | Win |
| Google Maps Response (prettified) | 3,981 | 3,176 | 2,318 | 1.25× | Fused | Win |
| Instruments (minified) | 2,936 | 2,189 | 746 | 1.34× | Fused | Win |
| Instruments (prettified) | 4,013 | 3,305 | 1,226 | 1.21× | Fused | Win |
| Marine IK Reverse (minified) | 883 | 808 | 648 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,618 | 2,736 | 2,347 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 911 | 823 | 647 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,700 | 2,780 | 2,345 | 0.97× | Two-stage | Win |
| Random (minified) | 2,534 | 1,547 | 1,027 | 1.64× | Fused | Win |
| Random (prettified) | 3,268 | 2,217 | 1,562 | 1.47× | Fused | Win |
| Twitter (minified) | 2,595 | 2,013 | 1,043 | 1.29× | Fused | Win |
| Twitter (prettified) | 3,187 | 2,470 | 1,392 | 1.29× | Fused | Win |
| Apache Builds Small (minified) | 3,589 | 2,341 | 1,693 | 1.53× | Fused | Win |
| Canada Small (minified) | 985 | 930 | 863 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 3,625 | 2,683 | 2,081 | 1.35× | Fused | Win |
| Canada Small (prettified) | 2,124 | 2,145 | 2,095 | 0.99× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,214 | 1,608 | 1,105 | 1.38× | Fused | Win |
| CitmCatalog Small (prettified) | 2,951 | 2,678 | 1,946 | 1.10× | Fused | Win |
| Discord Small (minified) | 3,169 | 2,101 | 883 | 1.51× | Fused | Win |
| Discord Small (prettified) | 3,499 | 2,708 | 1,208 | 1.29× | Fused | Win |
| Github Events Small (minified) | 2,826 | 2,121 | 1,551 | 1.33× | Fused | Win |
| Google Maps Response Small (minified) | 2,732 | 1,758 | 1,165 | 1.55× | Fused | Win |
| Github Events Small (prettified) | 3,200 | 2,525 | 1,933 | 1.27× | Fused | Win |
| Google Maps Response Small (prettified) | 3,891 | 3,065 | 2,394 | 1.27× | Fused | Win |
| Instruments Small (minified) | 2,749 | 2,025 | 680 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 3,808 | 3,086 | 1,132 | 1.23× | Fused | Win |
| Marine IK Reverse Small (minified) | 919 | 788 | 557 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,386 | 2,738 | 2,022 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 991 | 865 | 549 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,594 | 2,967 | 2,025 | 0.87× | Two-stage | Win |
| Random Small (minified) | 2,802 | 1,782 | 993 | 1.57× | Fused | Win |
| Random Small (prettified) | 3,407 | 2,496 | 1,508 | 1.36× | Fused | Win |
| Twitter Small (minified) | 3,041 | 2,435 | 1,043 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 3,557 | 2,830 | 1,324 | 1.26× | Fused | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,643 | n/a | 490 | n/a | n/a | Win |
| Double (POD) | 1,386 | n/a | 441 | n/a | n/a | Win |
| Int64 (POD) | 2,219 | n/a | 413 | n/a | n/a | Win |
| String (POD) | 1,979 | n/a | 1,138 | n/a | n/a | Win |
| Uint64 (POD) | 2,192 | n/a | 378 | n/a | n/a | Win |
| Apache Builds (minified) | 3,022 | 1,995 | 1,983 | 1.51× | Fused | Win |
| Canada (minified) | 1,120 | 935 | 901 | 1.20× | Fused | Win |
| Apache Builds (prettified) | 3,391 | 2,239 | 2,531 | 1.51× | Fused | Win |
| Canada (prettified) | 2,317 | 2,209 | 2,013 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 3,122 | 2,255 | 1,780 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 4,700 | 4,216 | 3,566 | 1.11× | Fused | Win |
| Discord (minified) | 3,745 | 2,350 | 1,648 | 1.59× | Fused | Win |
| Discord (prettified) | 4,351 | 3,221 | 2,334 | 1.35× | Fused | Win |
| Github Events (minified) | 2,668 | 2,084 | 1,665 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 2,989 | 1,843 | 1,281 | 1.62× | Fused | Win |
| Github Events (prettified) | 2,872 | 2,531 | 2,014 | 1.13× | Fused | Win |
| Google Maps Response (prettified) | 4,210 | 3,380 | 2,538 | 1.25× | Fused | Win |
| Instruments (minified) | 3,790 | 2,445 | 1,619 | 1.55× | Fused | Win |
| Instruments (prettified) | 4,604 | 3,744 | 2,560 | 1.23× | Fused | Win |
| Marine IK Reverse (minified) | 926 | 779 | 592 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,752 | 2,772 | 2,166 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 939 | 797 | 624 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,777 | 2,835 | 2,237 | 0.98× | Two-stage | Win |
| Random (minified) | 2,405 | 1,525 | 1,328 | 1.58× | Fused | Win |
| Random (prettified) | 3,146 | 2,108 | 1,988 | 1.49× | Fused | Win |
| Twitter (minified) | 3,049 | 2,034 | 1,333 | 1.50× | Fused | Win |
| Twitter (prettified) | 3,289 | 2,524 | 1,750 | 1.30× | Fused | Win |
| Apache Builds Small (minified) | 3,544 | 2,336 | 2,104 | 1.52× | Fused | Win |
| Canada Small (minified) | 1,120 | 923 | 744 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 3,619 | 2,695 | 2,560 | 1.34× | Fused | Win |
| Canada Small (prettified) | 2,334 | 2,239 | 1,745 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 2,343 | 1,714 | 1,402 | 1.37× | Fused | Win |
| CitmCatalog Small (prettified) | 3,136 | 2,793 | 2,440 | 1.12× | Fused | Win |
| Discord Small (minified) | 3,191 | 2,034 | 1,578 | 1.57× | Fused | Win |
| Discord Small (prettified) | 3,414 | 2,665 | 2,129 | 1.28× | Fused | Win |
| Github Events Small (minified) | 3,009 | 2,099 | 1,809 | 1.43× | Fused | Win |
| Google Maps Response Small (minified) | 2,792 | 1,780 | 1,311 | 1.57× | Fused | Win |
| Github Events Small (prettified) | 3,311 | 2,530 | 2,245 | 1.31× | Fused | Win |
| Google Maps Response Small (prettified) | 4,103 | 3,264 | 2,616 | 1.26× | Fused | Win |
| Instruments Small (minified) | 3,613 | 2,327 | 1,534 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 4,261 | 3,531 | 2,437 | 1.21× | Fused | Win |
| Marine IK Reverse Small (minified) | 1,050 | 888 | 545 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,674 | 3,130 | 2,045 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,147 | 913 | 541 | 1.26× | Fused | Win |
| Marine IK Small (prettified) | 2,904 | 3,201 | 2,039 | 0.91× | Two-stage | Win |
| Random Small (minified) | 2,783 | 1,794 | 1,318 | 1.55× | Fused | Win |
| Random Small (prettified) | 3,483 | 2,668 | 2,016 | 1.31× | Fused | Win |
| Twitter Small (minified) | 3,641 | 2,494 | 1,422 | 1.46× | Fused | Win |
| Twitter Small (prettified) | 3,723 | 3,147 | 1,864 | 1.18× | Fused | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,779 | n/a | 354 | n/a | n/a | Win |
| Double (POD) | 991 | n/a | 277 | n/a | n/a | Win |
| Int64 (POD) | 1,495 | n/a | 305 | n/a | n/a | Win |
| String (POD) | 1,814 | n/a | 673 | n/a | n/a | Win |
| Uint64 (POD) | 1,727 | n/a | 312 | n/a | n/a | Win |
| Apache Builds (minified) | 1,875 | 1,648 | 979 | 1.14× | Fused | Win |
| Canada (minified) | 922 | 650 | 685 | 1.42× | Fused | Win |
| Apache Builds (prettified) | 2,019 | 1,842 | 1,242 | 1.10× | Fused | Win |
| Canada (prettified) | 1,536 | 1,318 | 1,211 | 1.17× | Fused | Win |
| CitmCatalog (minified) | 2,609 | 1,394 | 1,094 | 1.87× | Fused | Win |
| CitmCatalog (prettified) | 3,691 | 3,000 | 2,451 | 1.23× | Fused | Win |
| Discord (minified) | 2,381 | 1,576 | 1,161 | 1.51× | Fused | Win |
| Discord (prettified) | 2,378 | 2,145 | 1,626 | 1.11× | Fused | Win |
| Github Events (minified) | 2,314 | 1,708 | 1,089 | 1.35× | Fused | Win |
| Google Maps Response (minified) | 2,088 | 1,437 | 817 | 1.45× | Fused | Win |
| Github Events (prettified) | 2,370 | 2,158 | 1,453 | 1.10× | Fused | Win |
| Google Maps Response (prettified) | 2,930 | 2,521 | 1,551 | 1.16× | Fused | Win |
| Instruments (minified) | 3,150 | 1,814 | 1,319 | 1.74× | Fused | Win |
| Instruments (prettified) | 2,651 | 2,656 | 1,993 | 1.00× | Tie | Win |
| Marine IK Reverse (minified) | 683 (n/c) | 501 | 523 | 1.36× | Fused | Win |
| Marine IK Reverse (prettified) | 2,128 | 2,088 | 1,791 | 1.02× | Fused | Win |
| Marine IK (minified) | 663 | 464 | 399 | 1.43× | Fused | Win |
| Marine IK (prettified) | 2,044 | 1,825 | 1,507 (n/c) | 1.12× | Fused | Win |
| Random (minified) | 1,634 | 1,367 | 878 | 1.20× | Fused | Win |
| Random (prettified) | 2,205 | 1,874 | 1,293 | 1.18× | Fused | Win |
| Twitter (minified) | 2,651 | 1,856 | 1,073 | 1.43× | Fused | Win |
| Twitter (prettified) | 3,064 | 2,587 | 1,397 | 1.18× | Fused | Win |
| Apache Builds Small (minified) | 2,057 | 1,849 | 1,186 | 1.11× | Fused | Win |
| Canada Small (minified) | 913 | 730 | 540 | 1.25× | Fused | Win |
| Apache Builds Small (prettified) | 2,157 | 2,073 | 1,334 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,797 | 1,671 | 1,232 | 1.08× | Fused | Win |
| CitmCatalog Small (minified) | 1,886 | 1,301 | 1,031 | 1.45× | Fused | Win |
| CitmCatalog Small (prettified) | 2,474 | 2,107 | 1,781 | 1.17× | Fused | Win |
| Discord Small (minified) | 2,546 | 1,787 | 1,186 | 1.42× | Fused | Win |
| Discord Small (prettified) | 2,511 | 2,239 | 1,558 | 1.12× | Fused | Win |
| Github Events Small (minified) | 2,099 | 1,702 | 1,310 | 1.23× | Fused | Win |
| Google Maps Response Small (minified) | 2,047 | 1,409 | 869 | 1.45× | Fused | Win |
| Github Events Small (prettified) | 2,303 | 2,015 | 1,808 | 1.14× | Fused | Win |
| Google Maps Response Small (prettified) | 2,835 | 2,464 | 1,632 | 1.15× | Fused | Win |
| Instruments Small (minified) | 3,122 | 1,828 | 1,273 | 1.71× | Fused | Win |
| Instruments Small (prettified) | 2,738 | 2,667 | 1,965 | 1.03× | Fused | Win |
| Marine IK Reverse Small (minified) | 830 | 637 | 446 | 1.30× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,086 | 2,214 | 1,556 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 920 | 698 | 425 | 1.32× | Fused | Win |
| Marine IK Small (prettified) | 2,402 | 2,340 | 1,491 | 1.03× | Fused | Win |
| Random Small (minified) | 1,691 | 1,134 | 796 | 1.49× | Fused | Win |
| Random Small (prettified) | 2,070 | 1,815 | 1,105 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,961 | 2,180 | 1,215 | 1.36× | Fused | Win |
| Twitter Small (prettified) | 2,971 | 2,573 | 1,535 | 1.15× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,370 | n/a | 308 | n/a | n/a | Win |
| Double (POD) | 1,075 | n/a | 304 | n/a | n/a | Win |
| Int64 (POD) | 1,386 | n/a | 323 | n/a | n/a | Win |
| String (POD) | 1,635 | n/a | 852 | n/a | n/a | Win |
| Uint64 (POD) | 1,462 | n/a | 289 | n/a | n/a | Win |
| Apache Builds (minified) | 1,860 | 1,624 | 1,053 | 1.15× | Fused | Win |
| Canada (minified) | 891 | 774 | 619 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 1,972 | 1,774 | 1,344 | 1.11× | Fused | Win |
| Canada (prettified) | 1,788 | 1,647 | 1,345 | 1.09× | Fused | Win |
| CitmCatalog (minified) | 2,609 | 1,706 | 916 | 1.53× | Fused | Win |
| CitmCatalog (prettified) | 3,652 | 3,097 | 1,893 | 1.18× | Fused | Win |
| Discord (minified) | 2,913 | 1,930 | 832 | 1.51× | Fused | Win |
| Discord (prettified) | 2,742 | 2,496 | 1,205 | 1.10× | Fused | Win |
| Github Events (minified) | 2,365 | 2,061 | 1,021 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,964 | 1,471 | 875 | 1.34× | Fused | Win |
| Github Events (prettified) | 2,363 | 2,249 | 1,215 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,831 | 2,392 | 1,737 | 1.18× | Fused | Win |
| Instruments (minified) | 3,155 | 1,966 | 715 | 1.60× | Fused | Win |
| Instruments (prettified) | 3,238 | 2,792 | 1,180 | 1.16× | Fused | Win |
| Marine IK Reverse (minified) | 783 | 600 | 432 | 1.30× | Fused | Win |
| Marine IK Reverse (prettified) | 2,235 | 2,188 | 1,437 | 1.02× | Fused | Win |
| Marine IK (minified) | 675 | 608 | 416 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,051 | 2,081 | 1,476 | 0.99× | Two-stage | Win |
| Random (minified) | 1,714 | 1,116 | 752 | 1.54× | Fused | Win |
| Random (prettified) | 2,054 | 1,689 | 899 | 1.22× | Fused | Win |
| Twitter (minified) | 2,433 | 2,172 | 918 | 1.12× | Fused | Win |
| Twitter (prettified) | 2,709 | 2,135 | 1,119 (n/c) | 1.27× | Fused | Win |
| Apache Builds Small (minified) | 2,092 | 1,765 | 1,078 | 1.19× | Fused | Win |
| Canada Small (minified) | 717 | 642 | 437 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,084 | 1,819 | 1,314 | 1.15× | Fused | Win |
| Canada Small (prettified) | 1,374 | 1,432 | 1,098 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,922 | 1,422 | 872 | 1.35× | Fused | Win |
| CitmCatalog Small (prettified) | 2,491 | 2,233 | 1,473 | 1.12× | Fused | Win |
| Discord Small (minified) | 2,615 | 1,868 | 608 | 1.40× | Fused | Win |
| Discord Small (prettified) | 2,577 | 2,410 | 940 | 1.07× | Fused | Win |
| Github Events Small (minified) | 2,074 | 1,801 | 1,264 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 1,931 | 1,461 | 797 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 2,239 | 2,078 | 1,576 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,798 | 2,320 | 1,796 | 1.21× | Fused | Win |
| Instruments Small (minified) | 3,191 | 1,986 | 670 | 1.61× | Fused | Win |
| Instruments Small (prettified) | 3,292 | 2,873 | 1,105 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 824 | 689 | 361 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,209 | 2,282 | 1,309 | 0.97× | Two-stage | Win |
| Marine IK Small (minified) | 888 | 726 | 376 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,371 | 2,386 | 1,370 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,933 | 1,453 | 790 | 1.33× | Fused | Win |
| Random Small (prettified) | 2,288 | 2,015 | 1,180 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,190 | 2,228 | 908 | 0.98× | Two-stage | Win |
| Twitter Small (prettified) | 2,955 | 2,669 | 1,160 | 1.11× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends mostly on the document on x86, and more on the machine on the M2 Pro. On the three x86 builds the fused path wins all 66 minified documents, full-size and Small, and 50 of the 66 prettified ones; the tape's 16 wins are the four prettified Marine IK documents (Marine IK and Marine IK Reverse, full-size and Small) on each build, plus prettified Canada and Github Events Small under MSVC and Canada Small under MSVC and Linux/Clang. On the M2 Pro the fused path wins 43 of the 44 minified documents (the tape takes Random Small on macOS/Clang) and 34 of the 44 prettified ones, with 9 to the tape and 1 tie. §2.8 repeats the comparison on AVX-512.

**POD-type tests: outside the comparison.** The sweep does not register the two-stage path on the five POD-type tests, so they have no fused-against-tape verdict. Against simdjson On Demand the fused path wins all 25 in both runs. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape would be pure overhead.

**Minified documents: the fused path's.** Freshly allocated, the fused path wins 109 of the 110 minified documents; the tape takes Random Small on macOS/Clang (0.95×, 1,140 against 1,196 MB/s). The fused path's margins run from 3% (Canada on Linux/Clang, 884 against 857 MB/s) to 1.64× (Instruments on macOS/GCC, 2,683 against 1,640 MB/s) on the full-size documents and from 3% (Github Events Small on macOS/GCC, 1,306 against 1,272) to 1.93× (Instruments Small on macOS/Clang) on the Small ones. With objects reused it again wins 109, and the tape takes Twitter Small on macOS/Clang (0.98×). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the fused path wins 50 of 66, the tape keeps the Marine IK family.** The fused path wins all nine full-size documents outside the Marine IK family on both Linux builds and eight under MSVC, where the tape also takes prettified Canada; on the Small copies it wins those nine on Linux/GCC, eight on Linux/Clang (the tape takes Canada Small) and seven under MSVC (the tape takes Canada Small and Github Events Small). Its margins reach 1.35× (Random on Linux/GCC, 2,159 against 1,601 MB/s) on the full-size documents and 1.31× (Apache Builds Small on both Linux builds) on the Small ones. The tape's 16 wins are narrow: from under 1% (Marine IK on Linux/GCC, 2,568 against 2,547 MB/s) to 10% (Canada under MSVC, 1,365 against 1,236 MB/s, not converged) on the full-size documents and from under 1% (Canada Small on Linux/Clang, 1,798 against 1,787) to 19% (Marine IK Reverse Small under MSVC, 1,579 against 1,327) on the Small ones. Marine IK is almost entirely floating-point arrays, so its lines are short and numerous, which is where the fused path's per-line work (the newline test, the depth multiplication, the span check and the branch on its result) weighs most against stage 1's layout-independent vector classification. We have not isolated this, and state it as a hypothesis. With objects reused the fused path wins 51 and the tape 15: Canada and Github Events Small under MSVC move to the fused path, prettified CitmCatalog under MSVC moves to the tape, and the tape's widest margin is 1.24× (Marine IK Reverse Small under MSVC).

**Prettified documents on the M2 Pro: mostly the fused path's, less so freshly allocated on macOS/GCC.** Of the 44 prettified documents, the fused path wins 34, the tape wins 9 and 1 is a tie. On macOS/GCC the tape takes three full-size documents, Discord (0.95×), Github Events (0.98×) and Google Maps Response (0.91×), and three Small ones, Apache Builds Small (0.98×), Instruments Small (0.96×) and Marine IK Reverse Small (0.93×); on macOS/Clang it takes Google Maps Response Small (0.86×), Marine IK Small (0.87×) and Marine IK Reverse Small (0.96×), and Apache Builds Small is a tie. The fused path's margins reach 1.18× (Twitter on macOS/GCC) and 1.22× (Discord on macOS/Clang) on the full-size documents. Reused, the fused path wins 38, the tape 5 (Marine IK Reverse Small on macOS/GCC; Marine IK, Canada Small, Marine IK Small and Marine IK Reverse Small on macOS/Clang) and 1 is a tie (Instruments on macOS/GCC), so every macOS/GCC full-size tape win of the fresh run goes back to the fused path.

**One tie against simdjson for the fused path alone.** Against simdjson On Demand the fused path alone wins 244 of the 245 tests freshly allocated and all 245 reused. The exception is prettified Canada Small on Linux/Clang freshly allocated, a tie for the fused path (1,787 against simdjson's 1,789 MB/s) and a win for the tape (1,798), so taking the faster Jsonifier path changes exactly that one verdict.

**MSVC slows all three by about the same median.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 60% of its Linux/Clang throughput, the two-stage path 65% and simdjson 65%. The spread between documents is narrowest for the two-stage path (from 55% on Apache Builds to 71% on Canada), wider for the fused path (from 48% on Apache Builds to 82% on Instruments) and widest for simdjson, which keeps 48% of its Linux/Clang throughput on Canada and more than all of it (120%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins all 245 tests with whichever path is faster, and 244 with the fused path alone.**

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 203 MB/s, against 357 MB/s for the same document in declared order, so the faster Jsonifier path is 3.32× faster than simdjson freshly allocated (3.55× reused). The prettified form shows the same gap, at 681 MB/s against 1,354, and the faster Jsonifier path is 2.92× faster (3.18× reused). The Small copies show it too, at 158 MB/s minified and 592 MB/s prettified under MSVC. On the other builds simdjson parses the full-size reversed document within about 8% of the declared one, sometimes faster, and the faster Jsonifier path is 1.17× to 1.73× faster than simdjson on it, freshly allocated, and 1.17× to 1.81× reused. Why the rescan cost shows up mostly under MSVC has not been profiled; §2.8 shows it again, wider, on the AVX-512 MSVC build.

The routing implication supports §2's rule of thumb: on every build, minified input belongs to the fused path, and so does prettified input on x86 except the float-heavy Marine IK family, where the tape is ahead by at most 19%. The M2 Pro is less settled: freshly allocated, the tape takes three full-size prettified documents on macOS/GCC that it loses reused. Routing each test to its faster path would change one verdict against simdjson, prettified Canada Small on Linux/Clang, from a tie to a win. The router does not take the document's shape into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,384 | 1,418 | 0.98× | **Loss** |
| Canada (minified) | 804 | 844 | 0.95× | **Loss** |
| Apache Builds (prettified) | 1,656 | 1,802 | 0.92× | **Loss** |
| Canada (prettified) | 1,898 | 1,884 | 1.01× | Win |
| CitmCatalog (minified) | 1,924 | 1,546 | 1.24× | Win |
| CitmCatalog (prettified) | 3,679 | 3,049 | 1.21× | Win |
| Discord (minified) | 1,835 | 1,616 | 1.14× | Win |
| Discord (prettified) | 2,559 | 2,326 | 1.10× | Win |
| Github Events (minified) | 1,569 | 1,656 | 0.95× | **Loss** |
| Google Maps Response (minified) | 1,678 | 1,573 | 1.07× | Win |
| Github Events (prettified) | 1,975 | 1,901 | 1.04× | Win |
| Google Maps Response (prettified) | 3,110 | 3,059 | 1.02× | Win |
| Instruments (minified) | 2,196 | 1,758 | 1.25× | Win |
| Instruments (prettified) | 3,358 | 2,774 | 1.21× | Win |
| Marine IK Reverse (minified) | 711 | 670 | 1.06× | Win |
| Marine IK Reverse (prettified) | 2,559 | 2,426 | 1.05× | Win |
| Marine IK (minified) | 717 | 669 | 1.07× | Win |
| Marine IK (prettified) | 2,568 | 2,400 | 1.07× | Win |
| Random (minified) | 1,134 | 1,176 | 0.96× | **Loss** |
| Random (prettified) | 1,601 | 1,765 | 0.91× | **Loss** |
| Twitter (minified) | 1,635 | 1,587 | 1.03× | Win |
| Twitter (prettified) | 2,047 | 2,066 | 0.99× | **Loss** |
| Apache Builds Small (minified) | 2,019 | 1,786 | 1.13× | Win |
| Canada Small (minified) | 783 | 757 | 1.03× | Win |
| Apache Builds Small (prettified) | 2,362 | 2,198 | 1.07× | Win |
| Canada Small (prettified) | 1,933 | 1,758 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,451 | 1,212 | 1.20× | Win |
| CitmCatalog Small (prettified) | 2,445 | 2,172 | 1.13× | Win |
| Discord Small (minified) | 1,665 | 1,577 | 1.06× | Win |
| Discord Small (prettified) | 2,115 | 2,106 | 1.00× | Tie |
| Github Events Small (minified) | 1,724 | 1,779 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,597 | 1,532 | 1.04× | Win |
| Github Events Small (prettified) | 2,001 | 2,196 | 0.91× | **Loss** |
| Google Maps Response Small (prettified) | 2,998 | 3,069 | 0.98× | **Loss** |
| Instruments Small (minified) | 2,178 | 1,774 | 1.23× | Win |
| Instruments Small (prettified) | 3,316 | 2,827 | 1.17× | Win |
| Marine IK Reverse Small (minified) | 751 | 650 | 1.16× | Win |
| Marine IK Reverse Small (prettified) | 2,627 | 2,419 | 1.09× | Win |
| Marine IK Small (minified) | 776 | 636 | 1.22× | Win |
| Marine IK Small (prettified) | 2,735 | 2,373 | 1.15× | Win |
| Random Small (minified) | 1,478 | 1,297 | 1.14× | Win |
| Random Small (prettified) | 2,214 | 1,962 | 1.13× | Win |
| Twitter Small (minified) | 2,228 | 1,902 | 1.17× | Win |
| Twitter Small (prettified) | 2,786 | 2,473 | 1.13× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,056 | 937 | 1.13× | Win |
| Canada (minified) | 625 | 642 | 0.97× | **Loss** |
| Apache Builds (prettified) | 1,259 | 1,190 | 1.06× | Win |
| Canada (prettified) | 1,259 | 1,226 | 1.03× | Win |
| CitmCatalog (minified) | 1,327 | 1,050 | 1.26× | Win |
| CitmCatalog (prettified) | 2,659 | 2,261 | 1.18× | Win |
| Discord (minified) | 1,286 | 611 | 2.11× | Win |
| Discord (prettified) | 1,749 | 1,012 | 1.73× | Win |
| Github Events (minified) | 1,454 | 1,036 | 1.40× | Win |
| Google Maps Response (minified) | 1,304 | 1,192 | 1.09× | Win |
| Github Events (prettified) | 1,643 | 1,482 | 1.11× | Win |
| Google Maps Response (prettified) | 2,317 | 2,133 | 1.09× | Win |
| Instruments (minified) | 1,640 | 1,375 | 1.19× | Win |
| Instruments (prettified) | 2,418 | 2,089 | 1.16× | Win |
| Marine IK Reverse (minified) | 475 | 562 | 0.85× | **Loss** |
| Marine IK Reverse (prettified) | 1,729 (n/c) | 1,946 | 0.89× | **Loss** |
| Marine IK (minified) | 443 | 452 | 0.98× | Tie |
| Marine IK (prettified) | 1,644 | 1,899 | 0.87× | **Loss** |
| Random (minified) | 902 | 818 | 1.10× | Win |
| Random (prettified) | 1,368 | 1,206 | 1.13× | Win |
| Twitter (minified) | 1,456 | 1,352 | 1.08× | Win |
| Twitter (prettified) | 2,021 | 1,719 | 1.18× | Win |
| Apache Builds Small (minified) | 1,429 | 1,002 | 1.43× | Win |
| Canada Small (minified) | 614 | 592 | 1.04× | Win |
| Apache Builds Small (prettified) | 1,667 | 1,290 | 1.29× | Win |
| Canada Small (prettified) | 1,426 | 1,140 | 1.25× | Win |
| CitmCatalog Small (minified) | 1,019 | 887 | 1.15× | Win |
| CitmCatalog Small (prettified) | 1,663 | 1,490 | 1.12× | Win |
| Discord Small (minified) | 1,376 | 1,078 | 1.28× | Win |
| Discord Small (prettified) | 1,761 | 1,424 | 1.24× | Win |
| Github Events Small (minified) | 1,272 | 963 | 1.32× | Win |
| Google Maps Response Small (minified) | 1,214 | 1,068 | 1.14× | Win |
| Github Events Small (prettified) | 1,505 | 1,410 | 1.07× | Win |
| Google Maps Response Small (prettified) | 2,176 | 1,966 | 1.11× | Win |
| Instruments Small (minified) | 1,686 | 1,342 | 1.26× | Win |
| Instruments Small (prettified) | 2,492 | 2,066 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 533 | 531 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 1,886 | 1,803 | 1.05× | Win |
| Marine IK Small (minified) | 568 | 535 | 1.06× | Win |
| Marine IK Small (prettified) | 1,957 | 1,529 | 1.28× | Win |
| Random Small (minified) | 925 | 714 | 1.29× | Win |
| Random Small (prettified) | 1,429 | 1,098 | 1.30× | Win |
| Twitter Small (minified) | 1,861 | 1,396 | 1.33× | Win |
| Twitter Small (prettified) | 2,196 | 1,745 | 1.26× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,995 | 1,820 | 1.10× | Win |
| Canada (minified) | 935 | 997 | 0.94× | **Loss** |
| Apache Builds (prettified) | 2,239 | 2,331 | 0.96× | **Loss** |
| Canada (prettified) | 2,209 | 2,205 | 1.00× | Tie |
| CitmCatalog (minified) | 2,255 | 1,802 | 1.25× | Win |
| CitmCatalog (prettified) | 4,216 | 3,572 | 1.18× | Win |
| Discord (minified) | 2,350 | 1,902 | 1.24× | Win |
| Discord (prettified) | 3,221 | 2,623 | 1.23× | Win |
| Github Events (minified) | 2,084 | 1,931 | 1.08× | Win |
| Google Maps Response (minified) | 1,843 | 1,648 | 1.12× | Win |
| Github Events (prettified) | 2,531 | 2,314 | 1.09× | Win |
| Google Maps Response (prettified) | 3,380 | 3,227 | 1.05× | Win |
| Instruments (minified) | 2,445 | 1,808 | 1.35× | Win |
| Instruments (prettified) | 3,744 | 2,812 | 1.33× | Win |
| Marine IK Reverse (minified) | 779 | 749 | 1.04× | Win |
| Marine IK Reverse (prettified) | 2,772 | 2,662 | 1.04× | Win |
| Marine IK (minified) | 797 | 751 | 1.06× | Win |
| Marine IK (prettified) | 2,835 | 2,662 | 1.06× | Win |
| Random (minified) | 1,525 | 1,556 | 0.98× | **Loss** |
| Random (prettified) | 2,108 | 2,297 | 0.92× | **Loss** |
| Twitter (minified) | 2,034 | 1,875 | 1.08× | Win |
| Twitter (prettified) | 2,524 | 2,380 | 1.06× | Win |
| Apache Builds Small (minified) | 2,336 | 1,991 | 1.17× | Win |
| Canada Small (minified) | 923 | 850 | 1.09× | Win |
| Apache Builds Small (prettified) | 2,695 | 2,490 | 1.08× | Win |
| Canada Small (prettified) | 2,239 | 1,995 | 1.12× | Win |
| CitmCatalog Small (minified) | 1,714 | 1,381 | 1.24× | Win |
| CitmCatalog Small (prettified) | 2,793 | 2,380 | 1.17× | Win |
| Discord Small (minified) | 2,034 | 1,840 | 1.11× | Win |
| Discord Small (prettified) | 2,665 | 2,373 | 1.12× | Win |
| Github Events Small (minified) | 2,099 | 2,015 | 1.04× | Win |
| Google Maps Response Small (minified) | 1,780 | 1,692 | 1.05× | Win |
| Github Events Small (prettified) | 2,530 | 2,464 | 1.03× | Win |
| Google Maps Response Small (prettified) | 3,264 | 3,334 | 0.98× | **Loss** |
| Instruments Small (minified) | 2,327 | 1,830 | 1.27× | Win |
| Instruments Small (prettified) | 3,531 | 2,926 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 888 | 747 | 1.19× | Win |
| Marine IK Reverse Small (prettified) | 3,130 | 2,661 | 1.18× | Win |
| Marine IK Small (minified) | 913 | 731 | 1.25× | Win |
| Marine IK Small (prettified) | 3,201 | 2,671 | 1.20× | Win |
| Random Small (minified) | 1,794 | 1,470 | 1.22× | Win |
| Random Small (prettified) | 2,668 | 2,223 | 1.20× | Win |
| Twitter Small (minified) | 2,494 | 2,109 | 1.18× | Win |
| Twitter Small (prettified) | 3,147 | 2,704 | 1.16× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,648 | 1,166 | 1.41× | Win |
| Canada (minified) | 650 | 760 | 0.86× | **Loss** |
| Apache Builds (prettified) | 1,842 | 1,472 | 1.25× | Win |
| Canada (prettified) | 1,318 | 1,528 | 0.86× | **Loss** |
| CitmCatalog (minified) | 1,394 | 1,160 | 1.20× | Win |
| CitmCatalog (prettified) | 3,000 | 2,205 | 1.36× | Win |
| Discord (minified) | 1,576 | 1,213 | 1.30× | Win |
| Discord (prettified) | 2,145 | 1,545 | 1.39× | Win |
| Github Events (minified) | 1,708 | 1,322 | 1.29× | Win |
| Google Maps Response (minified) | 1,437 | 1,297 | 1.11× | Win |
| Github Events (prettified) | 2,158 | 1,735 | 1.24× | Win |
| Google Maps Response (prettified) | 2,521 | 2,290 | 1.10× | Win |
| Instruments (minified) | 1,814 | 1,432 | 1.27× | Win |
| Instruments (prettified) | 2,656 | 2,063 | 1.29× | Win |
| Marine IK Reverse (minified) | 501 | 660 | 0.76× | **Loss** |
| Marine IK Reverse (prettified) | 2,088 | 2,163 | 0.97× | **Loss** |
| Marine IK (minified) | 464 | 499 | 0.93× | **Loss** |
| Marine IK (prettified) | 1,825 | 1,922 | 0.95× | **Loss** |
| Random (minified) | 1,367 | 987 | 1.39× | Win |
| Random (prettified) | 1,874 | 1,454 | 1.29× | Win |
| Twitter (minified) | 1,856 | 1,498 | 1.24× | Win |
| Twitter (prettified) | 2,587 | 1,905 | 1.36× | Win |
| Apache Builds Small (minified) | 1,849 | 1,283 | 1.44× | Win |
| Canada Small (minified) | 730 | 689 | 1.06× | Win |
| Apache Builds Small (prettified) | 2,073 | 1,723 | 1.20× | Win |
| Canada Small (prettified) | 1,671 | 1,395 | 1.20× | Win |
| CitmCatalog Small (minified) | 1,301 | 1,103 | 1.18× | Win |
| CitmCatalog Small (prettified) | 2,107 | 1,803 | 1.17× | Win |
| Discord Small (minified) | 1,787 | 1,314 | 1.36× | Win |
| Discord Small (prettified) | 2,239 | 1,456 | 1.54× | Win |
| Github Events Small (minified) | 1,702 | 1,369 | 1.24× | Win |
| Google Maps Response Small (minified) | 1,409 | 1,209 | 1.17× | Win |
| Github Events Small (prettified) | 2,015 | 1,665 | 1.21× | Win |
| Google Maps Response Small (prettified) | 2,464 | 2,189 | 1.13× | Win |
| Instruments Small (minified) | 1,828 | 1,427 | 1.28× | Win |
| Instruments Small (prettified) | 2,667 | 2,192 | 1.22× | Win |
| Marine IK Reverse Small (minified) | 637 | 640 | 0.99× | Tie |
| Marine IK Reverse Small (prettified) | 2,214 | 2,047 | 1.08× | Win |
| Marine IK Small (minified) | 698 | 643 | 1.09× | Win |
| Marine IK Small (prettified) | 2,340 | 2,012 | 1.16× | Win |
| Random Small (minified) | 1,134 | 883 | 1.28× | Win |
| Random Small (prettified) | 1,815 | 1,376 | 1.32× | Win |
| Twitter Small (minified) | 2,180 | 1,556 | 1.40× | Win |
| Twitter Small (prettified) | 2,573 | 1,934 | 1.33× | Win |

In the freshly allocated run, two-stage Jsonifier wins 71 of these 88 tests, ties 3 and loses 14, by up to 15% (minified Marine IK Reverse on macOS/GCC, 475 against 562 MB/s). Ten of the losses are on Linux/GCC: both forms of Apache Builds and Random, minified Canada and Github Events, prettified Twitter, both forms of Github Events Small and prettified Google Maps Response Small. The other four are on macOS/GCC: minified Canada, prettified Marine IK and both forms of Marine IK Reverse (0.85× to 0.97×). The ties are prettified Discord Small on Linux/GCC and minified Marine IK and Marine IK Reverse Small on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 83, ties 2 and loses 3. With objects reused, two-stage Jsonifier wins 75, ties 2 and loses 11, five on Linux/GCC and six on macOS/GCC, by up to 24% (minified Marine IK Reverse on macOS/GCC, 501 against 660 MB/s); the ties are prettified Canada on Linux/GCC and minified Marine IK Reverse Small on macOS/GCC. The fused path beats the reflection reader on 86, ties 2 and loses none. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a row near 1.00× can go either way: freshly allocated prettified Twitter on Linux/GCC (2,047 against 2,066 MB/s, 0.99×) is a loss, while prettified Discord Small on the same build (2,115 against 2,106) is a tie.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 31 of the 40 results; the widest gap is reused prettified Twitter on macOS/Clang, where the two-stage row runs at 5,162 MB/s against the "jsonifier" row's 4,320. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 6,245 | 3,969 | 1.57× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,909 | 5,195 | 1.52× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,972 | 3,686 | 1.35× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,142 | 4,945 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,353 | 6,844 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,467 | 8,268 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,903 | 5,459 | 1.08× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,776 | 6,645 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,357 | 6,906 | 1.21× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,569 | 8,187 | 1.17× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,765 | 5,625 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,720 | 6,742 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,361 | 3,495 | 1.25× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,226 | 4,100 | 1.03× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,806 | 3,121 | 1.22× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,150 | 4,350 | 0.95× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,881 | 4,098 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,095 | 3,970 | 1.28× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,033 | 3,544 | 1.14× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,536 | 4,092 | 1.11× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 7,091 | 4,467 | 1.59× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 8,228 | 5,320 | 1.55× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,446 | 4,360 | 1.25× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,580 | 5,179 | 1.27× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,545 | 7,030 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,593 | 8,509 | 1.13× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,962 | 5,740 | 1.04× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,903 | 6,814 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,516 | 7,100 | 1.20× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,668 | 8,423 | 1.15× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,190 | 5,709 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,033 | 6,930 | 1.01× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,342 | 3,943 | 1.10× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,744 | 4,507 | 1.05× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,776 | 3,753 | 1.01× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,570 | 4,487 | 1.02× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,389 | 4,162 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,162 | 3,961 | 1.30× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,382 | 3,804 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,673 | 4,244 | 1.10× | Win |

Jsonifier wins 36 of the 40 results, ties 3 and loses 1, and wins by up to 1.59× (minified Twitter under MSVC, reused). Freshly allocated it wins 18, ties 1 and loses 1: prettified Twitter Small is a tie on Linux/GCC (1.00×) and a loss on macOS/GCC (0.95×). Reused it wins 18 and ties 2, both forms of Twitter Small on macOS/GCC. Every full-size result is a win, by 1.13× to 1.59× on the three x86 builds and 1.03× to 1.30× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.15× to 1.31×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 2,724 | 2,379 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,344 | 2,593 | 1.29× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,682 | 2,163 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,312 | 1,860 | 1.24× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,599 | 1,991 | 1.31× | Win |

The narrowest win is under MSVC, 1.15× (2,724 against 2,379 MB/s), and the widest on macOS/Clang, 1.31× (2,599 against 1,991 MB/s).

### 2.8 AVX-512 builds

Every result above comes from AVX2 and NEON builds. The same tests also ran on three AVX-512 builds, Linux / Clang 24.0, Linux / GCC 16.2 and Windows / MSVC 19.52, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, from the Json-Performance branch `benchmarking-avx512-main` at the same commits as §2.1, on November 27, 2026. Jsonifier and both of its paths run their AVX-512 kernels, and simdjson runs its `icelake` kernel. Sampling, convergence limits (RSE under 5%, mean shift under 2.5%) and verdicts are as in §2.1. The Linux / GCC build also runs the reflection reader of §2.5. The runners are shared virtual machines, and the CPU and operating-system images differ from the AVX2 builds, so a result here cannot be set against the matching AVX2 result to isolate the instruction set; every comparison below is within one build.

- **The routing split holds.** Freshly allocated, across the 132 corpus tests the fused path wins 108, 8 are ties and the tape wins 16; reused, 110, 3 and 19. Freshly allocated, the fused path wins 64 of the 66 minified documents and ties the other 2, minified Canada and Marine IK Reverse under MSVC. On prettified input the fused path wins 44 of 66, with 6 ties, by up to 1.21× (Discord under MSVC) on the full-size documents and 1.16× (Discord Small under MSVC) on the Small ones. The tape's 16 prettified wins sit in the Marine IK family and the number-heavy Canada and CitmCatalog documents, plus Instruments on Linux / GCC and Twitter Small on Linux / Clang (1.31×). Reused, the tape also takes one minified document, Google Maps Response Small under MSVC, by a wide 1.48× (1,101 against 742 MB/s).
- **Against simdjson, Jsonifier wins all 147 tests in both runs.** The faster Jsonifier path and the fused path alone each win all 147 freshly allocated and all 147 reused.
- **Stage 1 + reflection on Linux / GCC.** Against simdjson's reflection reader, two-stage Jsonifier wins 32 of the 44 corpus tests freshly allocated, ties 3 (minified Apache Builds and Twitter, prettified Apache Builds Small) and loses 9, by up to 20% (prettified Github Events Small, 0.80×). Reused it wins 38, ties 2 (prettified Apache Builds and Twitter) and loses 4, by up to 7% (prettified Github Events Small, 0.93×). With the fused path instead, Jsonifier beats the reflection reader on 42 freshly allocated and all 44 reused.
- **Reverse key order costs simdjson most under MSVC.** On this build simdjson parses minified Marine IK Reverse at 123 MB/s against 236 MB/s in declared order, and the faster Jsonifier path is 4.90× faster freshly allocated (5.37× reused); on the prettified form it is 4.45× faster (4.82× reused). On both Linux builds simdjson reads the full-size reversed document within 7% of the declared one, and the faster Jsonifier path is 1.54× (Linux / Clang) and 1.55× (Linux / GCC) faster on the minified form, and 1.23× and 1.33× on the prettified form.
- **Partial reading.** Freshly allocated, Jsonifier wins all four Twitter Partial results under MSVC (2.22× to 2.30× full-size, 1.73× to 1.82× Small) and all four on Linux / Clang (1.23× to 1.31× full-size, 1.02× to 1.03× Small); on Linux / GCC it wins both full-size results (1.07× and 1.15×) and loses both Small copies (0.95× and 0.86×), for 10 wins and 2 losses. Reused it wins every result except prettified Twitter Small on Linux / GCC (0.98×): 2.15× to 2.32× full-size under MSVC, 1.24× to 1.30× on Linux / Clang and 1.09× to 1.16× on Linux / GCC, for 11 wins and 1 loss.
- **Validation.** Jsonifier wins on all three builds, by 1.21× under MSVC, 1.53× on Linux / Clang and 1.24× on Linux / GCC.

#### Freshly allocated objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 35 | 5 | 4 | 37 of 44 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 37 | 0 | 7 | 44 of 44 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 36 | 3 | 5 | 44 of 44 |
| **Aggregate** | **108** | **8** | **16** | **125 of 132** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **147 / 0 / 0** | **147 / 0 / 0** |

**Windows / MSVC 19.52 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 502 | n/a | 152 | n/a | n/a | Win |
| Double (POD) | 798 | n/a | 292 | n/a | n/a | Win |
| Int64 (POD) | 884 | n/a | 279 | n/a | n/a | Win |
| String (POD) | 606 | n/a | 383 | n/a | n/a | Win |
| Uint64 (POD) | 798 | n/a | 261 | n/a | n/a | Win |
| Apache Builds (minified) | 774 | 703 | 622 | 1.10× | Fused | Win |
| Canada (minified) | 509 | 501 (n/c) | 185 | 1.02× | Tie | Win |
| Apache Builds (prettified) | 932 | 839 | 878 | 1.11× | Fused | Win |
| Canada (prettified) | 1,266 | 1,296 | 428 (n/c) | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,485 | 1,206 | 638 | 1.23× | Fused | Win |
| CitmCatalog (prettified) | 2,554 | 2,420 | 1,311 | 1.06× | Fused | Win |
| Discord (minified) | 1,272 | 966 | 741 | 1.32× | Fused | Win |
| Discord (prettified) | 1,632 | 1,349 | 1,065 | 1.21× | Fused | Win |
| Github Events (minified) | 1,087 | 962 | 634 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 1,171 | 1,014 | 562 | 1.16× | Fused | Win |
| Github Events (prettified) | 1,171 | 1,112 | 738 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,093 | 1,991 | 1,158 | 1.05× | Fused | Win |
| Instruments (minified) | 1,485 | 1,083 | 824 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,023 | 1,784 | 1,383 | 1.13× | Fused | Win |
| Marine IK Reverse (minified) | 583 (n/c) | 602 | 123 | 0.97× | Tie | Win |
| Marine IK Reverse (prettified) | 2,121 | 2,197 | 494 (n/c) | 0.97× | Two-stage | Win |
| Marine IK (minified) | 679 (n/c) | 570 | 236 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,148 | 2,149 | 839 (n/c) | 1.00× | Tie | Win |
| Random (minified) | 788 | 687 | 495 | 1.15× | Fused | Win |
| Random (prettified) | 1,182 | 1,022 | 737 | 1.16× | Fused | Win |
| Twitter (minified) | 1,375 | 1,108 (n/c) | 903 | 1.24× | Fused | Win |
| Twitter (prettified) | 1,580 | 1,395 | 1,204 | 1.13× | Fused | Win |
| Apache Builds Small (minified) | 1,131 | 1,038 | 654 | 1.09× | Fused | Win |
| Canada Small (minified) | 556 | 517 | 172 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,317 | 1,258 | 828 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,167 | 1,237 | 425 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 735 | 639 | 447 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 1,139 | 1,140 | 787 | 1.00× | Tie | Win |
| Discord Small (minified) | 1,153 | 914 | 607 | 1.26× | Fused | Win |
| Discord Small (prettified) | 1,385 | 1,196 | 878 | 1.16× | Fused | Win |
| Github Events Small (minified) | 1,166 | 973 | 620 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 1,097 | 956 | 544 | 1.15× | Fused | Win |
| Github Events Small (prettified) | 1,238 | 1,218 | 781 | 1.02× | Fused | Win |
| Google Maps Response Small (prettified) | 1,958 | 1,886 | 1,077 | 1.04× | Fused | Win |
| Instruments Small (minified) | 1,390 | 1,010 | 757 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 1,862 | 1,694 | 1,246 | 1.10× | Fused | Win |
| Marine IK Reverse Small (minified) | 447 | 429 | 101 | 1.04× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,367 | 1,550 | 388 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 506 | 456 | 184 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 1,442 | 1,668 | 706 | 0.86× | Two-stage | Win |
| Random Small (minified) | 879 | 777 | 428 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,258 | 1,112 | 652 | 1.13× | Fused | Win |
| Twitter Small (minified) | 1,469 | 1,119 | 1,070 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 1,600 | 1,434 | 1,354 | 1.12× | Fused | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,879 | n/a | 452 | n/a | n/a | Win |
| Double (POD) | 1,351 | n/a | 451 | n/a | n/a | Win |
| Int64 (POD) | 1,465 | n/a | 358 | n/a | n/a | Win |
| String (POD) | 1,356 | n/a | 808 | n/a | n/a | Win |
| Uint64 (POD) | 1,611 | n/a | 317 | n/a | n/a | Win |
| Apache Builds (minified) | 1,762 | 1,496 | 1,127 | 1.18× | Fused | Win |
| Canada (minified) | 863 | 800 | 759 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 2,038 | 1,755 | 1,446 | 1.16× | Fused | Win |
| Canada (prettified) | 1,755 | 1,779 | 1,708 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,165 | 1,850 | 1,047 | 1.17× | Fused | Win |
| CitmCatalog (prettified) | 3,814 | 3,563 | 2,219 | 1.07× | Fused | Win |
| Discord (minified) | 2,269 | 1,932 | 828 | 1.17× | Fused | Win |
| Discord (prettified) | 2,903 | 2,723 | 1,201 | 1.07× | Fused | Win |
| Github Events (minified) | 1,941 | 1,567 | 991 | 1.24× | Fused | Win |
| Google Maps Response (minified) | 2,233 | 1,845 | 1,008 | 1.21× | Fused | Win |
| Github Events (prettified) | 2,282 | 1,940 | 1,249 | 1.18× | Fused | Win |
| Google Maps Response (prettified) | 3,586 | 3,300 | 2,035 | 1.09× | Fused | Win |
| Instruments (minified) | 2,981 | 2,435 | 654 | 1.22× | Fused | Win |
| Instruments (prettified) | 3,810 | 3,588 | 1,081 | 1.06× | Fused | Win |
| Marine IK Reverse (minified) | 848 | 780 | 549 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,558 | 2,536 | 2,072 | 1.01× | Fused | Win |
| Marine IK (minified) | 811 | 723 | 555 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,659 | 2,805 | 2,227 | 0.95× | Two-stage | Win |
| Random (minified) | 1,481 | 1,191 | 770 | 1.24× | Fused | Win |
| Random (prettified) | 1,931 | 1,737 | 1,188 | 1.11× | Fused | Win |
| Twitter (minified) | 1,958 | 1,671 | 926 | 1.17× | Fused | Win |
| Twitter (prettified) | 2,486 | 2,199 | 1,225 | 1.13× | Fused | Win |
| Apache Builds Small (minified) | 3,013 | 2,482 | 1,532 | 1.21× | Fused | Win |
| Canada Small (minified) | 910 | 838 | 767 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 3,284 | 2,871 | 1,894 | 1.14× | Fused | Win |
| Canada Small (prettified) | 1,891 | 1,954 | 1,771 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,929 | 1,590 | 1,068 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 2,563 | 2,658 | 1,828 | 0.96× | Two-stage | Win |
| Discord Small (minified) | 2,196 | 1,789 | 796 | 1.23× | Fused | Win |
| Discord Small (prettified) | 2,619 | 2,352 | 1,087 | 1.11× | Fused | Win |
| Github Events Small (minified) | 2,530 | 1,907 | 1,451 | 1.33× | Fused | Win |
| Google Maps Response Small (minified) | 2,242 | 1,849 | 1,066 | 1.21× | Fused | Win |
| Github Events Small (prettified) | 2,672 | 2,385 | 1,809 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 3,609 | 3,329 | 2,140 | 1.08× | Fused | Win |
| Instruments Small (minified) | 3,004 | 2,342 | 622 | 1.28× | Fused | Win |
| Instruments Small (prettified) | 3,636 | 3,366 | 1,035 | 1.08× | Fused | Win |
| Marine IK Reverse Small (minified) | 862 | 774 | 528 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,334 | 2,765 | 1,912 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 937 | 833 | 530 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 2,538 | 2,893 | 1,925 | 0.88× | Two-stage | Win |
| Random Small (minified) | 2,121 | 1,737 | 907 | 1.22× | Fused | Win |
| Random Small (prettified) | 2,847 | 2,530 | 1,386 | 1.13× | Fused | Win |
| Twitter Small (minified) | 2,568 | 2,092 | 1,057 | 1.23× | Fused | Win |
| Twitter Small (prettified) | 2,015 | 2,643 | 1,357 | 0.76× | Two-stage | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,182 | n/a | 477 | n/a | n/a | Win |
| Double (POD) | 1,392 | n/a | 382 | n/a | n/a | Win |
| Int64 (POD) | 1,662 | n/a | 339 | n/a | n/a | Win |
| String (POD) | 1,382 | n/a | 687 | n/a | n/a | Win |
| Uint64 (POD) | 1,537 | n/a | 305 | n/a | n/a | Win |
| Apache Builds (minified) | 1,574 | 1,389 | 1,283 | 1.13× | Fused | Win |
| Canada (minified) | 824 | 779 | 705 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,808 | 1,697 | 1,668 | 1.06× | Fused | Win |
| Canada (prettified) | 1,684 | 1,665 | 1,550 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,018 | 1,781 | 1,534 | 1.13× | Fused | Win |
| CitmCatalog (prettified) | 3,564 | 3,631 | 3,140 | 0.98× | Two-stage | Win |
| Discord (minified) | 2,214 | 1,821 | 1,422 | 1.22× | Fused | Win |
| Discord (prettified) | 2,609 | 2,423 | 2,001 | 1.08× | Fused | Win |
| Github Events (minified) | 1,848 | 1,540 | 1,176 | 1.20× | Fused | Win |
| Google Maps Response (minified) | 1,907 | 1,539 | 1,201 | 1.24× | Fused | Win |
| Github Events (prettified) | 1,967 | 1,795 | 1,491 | 1.10× | Fused | Win |
| Google Maps Response (prettified) | 3,063 | 2,819 | 2,340 | 1.09× | Fused | Win |
| Instruments (minified) | 2,932 | 2,495 | 1,264 | 1.18× | Fused | Win |
| Instruments (prettified) | 3,516 | 3,568 | 2,033 | 0.99× | Two-stage | Win |
| Marine IK Reverse (minified) | 854 | 751 | 552 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 2,593 | 2,588 | 1,951 | 1.00× | Tie | Win |
| Marine IK (minified) | 835 | 747 | 563 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,620 | 2,615 | 2,016 | 1.00× | Tie | Win |
| Random (minified) | 1,254 | 1,101 | 1,058 | 1.14× | Fused | Win |
| Random (prettified) | 1,853 | 1,635 | 1,631 | 1.13× | Fused | Win |
| Twitter (minified) | 2,058 | 1,665 | 1,028 | 1.24× | Fused | Win |
| Twitter (prettified) | 2,237 | 2,034 | 1,292 | 1.10× | Fused | Win |
| Apache Builds Small (minified) | 2,316 | 2,002 | 1,724 | 1.16× | Fused | Win |
| Canada Small (minified) | 956 | 833 | 699 | 1.15× | Fused | Win |
| Apache Builds Small (prettified) | 2,511 | 2,362 | 2,079 | 1.06× | Fused | Win |
| Canada Small (prettified) | 1,917 | 1,904 | 1,713 | 1.01× | Tie | Win |
| CitmCatalog Small (minified) | 1,921 | 1,596 | 1,413 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 2,532 | 2,590 | 2,239 | 0.98× | Two-stage | Win |
| Discord Small (minified) | 1,937 | 1,565 | 1,339 | 1.24× | Fused | Win |
| Discord Small (prettified) | 2,222 | 2,122 | 1,785 | 1.05× | Fused | Win |
| Github Events Small (minified) | 2,197 | 1,590 | 1,295 | 1.38× | Fused | Win |
| Google Maps Response Small (minified) | 1,762 | 1,581 | 1,307 | 1.11× | Fused | Win |
| Github Events Small (prettified) | 2,144 | 1,909 | 1,635 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 3,132 | 2,867 | 2,440 | 1.09× | Fused | Win |
| Instruments Small (minified) | 3,190 | 2,449 | 1,143 | 1.30× | Fused | Win |
| Instruments Small (prettified) | 3,535 | 3,438 | 1,731 | 1.03× | Fused | Win |
| Marine IK Reverse Small (minified) | 877 | 746 | 527 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,556 | 2,699 | 1,824 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 951 | 770 | 505 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,615 | 2,671 | 1,794 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,962 | 1,603 | 1,265 | 1.22× | Fused | Win |
| Random Small (prettified) | 2,506 | 2,330 | 1,913 | 1.08× | Fused | Win |
| Twitter Small (minified) | 2,748 | 1,956 | 1,135 | 1.40× | Fused | Win |
| Twitter Small (prettified) | 2,734 | 2,494 | 1,470 | 1.10× | Fused | Win |

#### Reused objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 36 | 1 | 7 | 42 of 44 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 37 | 0 | 7 | 44 of 44 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 37 | 2 | 5 | 43 of 44 |
| **Aggregate** | **110** | **3** | **19** | **129 of 132** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **147 / 0 / 0** | **147 / 0 / 0** |

**Windows / MSVC 19.52 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 594 | n/a | 160 | n/a | n/a | Win |
| Double (POD) | 1,141 | n/a | 336 | n/a | n/a | Win |
| Int64 (POD) | 1,476 | n/a | 314 | n/a | n/a | Win |
| String (POD) | 1,010 | n/a | 512 | n/a | n/a | Win |
| Uint64 (POD) | 1,335 | n/a | 287 | n/a | n/a | Win |
| Apache Builds (minified) | 1,210 | 1,057 | 1,116 | 1.14× | Fused | Win |
| Canada (minified) | 649 | 623 | 204 | 1.04× | Fused | Win |
| Apache Builds (prettified) | 1,467 | 1,357 | 1,420 | 1.08× | Fused | Win |
| Canada (prettified) | 1,500 | 1,533 | 493 (n/c) | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,674 | 1,347 | 644 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,777 | 2,763 | 1,329 | 1.01× | Fused | Win |
| Discord (minified) | 1,813 | 1,280 | 1,040 | 1.42× | Fused | Win |
| Discord (prettified) | 2,222 | 1,751 | 1,404 | 1.27× | Fused | Win |
| Github Events (minified) | 1,652 | 1,306 | 795 | 1.26× | Fused | Win |
| Google Maps Response (minified) | 1,311 | 1,130 | 623 | 1.16× | Fused | Win |
| Github Events (prettified) | 1,707 | 1,568 | 913 | 1.09× | Fused | Win |
| Google Maps Response (prettified) | 2,367 | 2,217 | 1,250 | 1.07× | Fused | Win |
| Instruments (minified) | 1,722 | 1,237 | 942 | 1.39× | Fused | Win |
| Instruments (prettified) | 2,276 | 2,069 | 1,544 | 1.10× | Fused | Win |
| Marine IK Reverse (minified) | 712 | 671 | 133 | 1.06× | Fused | Win |
| Marine IK Reverse (prettified) | 2,348 (n/c) | 2,448 | 508 (n/c) | 0.96× | Two-stage | Win |
| Marine IK (minified) | 771 | 668 | 240 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,368 | 2,418 | 887 | 0.98× | Two-stage | Win |
| Random (minified) | 1,083 | 889 | 601 | 1.22× | Fused | Win |
| Random (prettified) | 1,526 | 1,289 | 889 | 1.18× | Fused | Win |
| Twitter (minified) | 1,956 | 1,446 | 1,196 | 1.35× | Fused | Win |
| Twitter (prettified) | 2,034 | 1,748 | 1,541 | 1.16× | Fused | Win |
| Apache Builds Small (minified) | 1,491 | 1,336 | 866 | 1.12× | Fused | Win |
| Canada Small (minified) | 659 | 643 | 181 | 1.03× | Fused | Win |
| Apache Builds Small (prettified) | 1,745 | 1,624 | 1,081 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,395 | 1,494 | 450 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,046 | 863 | 517 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,482 | 1,504 | 889 | 0.99× | Two-stage | Win |
| Discord Small (minified) | 1,684 | 1,215 | 886 | 1.39× | Fused | Win |
| Discord Small (prettified) | 1,887 | 1,608 | 1,197 | 1.17× | Fused | Win |
| Github Events Small (minified) | 1,544 | 1,275 | 715 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 742 | 1,101 | 574 | 0.67× | Two-stage | Win |
| Github Events Small (prettified) | 1,593 | 1,562 | 896 | 1.02× | Fused | Win |
| Google Maps Response Small (prettified) | 2,237 | 2,099 | 1,203 | 1.07× | Fused | Win |
| Instruments Small (minified) | 1,531 | 1,109 | 851 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 1,957 | 1,816 | 1,420 | 1.08× | Fused | Win |
| Marine IK Reverse Small (minified) | 545 | 518 | 104 | 1.05× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,586 | 1,882 | 392 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 642 | 563 | 200 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,848 | 2,008 | 788 | 0.92× | Two-stage | Win |
| Random Small (minified) | 1,093 | 915 | 500 | 1.19× | Fused | Win |
| Random Small (prettified) | 1,542 | 1,298 | 766 | 1.19× | Fused | Win |
| Twitter Small (minified) | 2,019 | 1,421 | 1,278 | 1.42× | Fused | Win |
| Twitter Small (prettified) | 2,028 | 1,789 | 1,664 | 1.13× | Fused | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,931 | n/a | 475 | n/a | n/a | Win |
| Double (POD) | 1,379 | n/a | 435 | n/a | n/a | Win |
| Int64 (POD) | 1,662 | n/a | 367 | n/a | n/a | Win |
| String (POD) | 2,094 | n/a | 922 | n/a | n/a | Win |
| Uint64 (POD) | 1,838 | n/a | 326 | n/a | n/a | Win |
| Apache Builds (minified) | 2,648 | 2,051 | 1,430 | 1.29× | Fused | Win |
| Canada (minified) | 1,044 | 953 | 916 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 2,823 | 2,349 | 1,842 | 1.20× | Fused | Win |
| Canada (prettified) | 2,049 | 2,088 | 2,027 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 2,789 | 2,275 | 1,188 | 1.23× | Fused | Win |
| CitmCatalog (prettified) | 4,396 | 4,232 | 2,643 | 1.04× | Fused | Win |
| Discord (minified) | 3,405 | 2,548 | 963 | 1.34× | Fused | Win |
| Discord (prettified) | 3,745 | 3,393 | 1,381 | 1.10× | Fused | Win |
| Github Events (minified) | 2,657 | 2,044 | 1,142 | 1.30× | Fused | Win |
| Google Maps Response (minified) | 2,465 | 1,980 | 1,054 | 1.24× | Fused | Win |
| Github Events (prettified) | 3,213 | 2,683 | 1,374 | 1.20× | Fused | Win |
| Google Maps Response (prettified) | 3,835 | 3,531 | 2,107 | 1.09× | Fused | Win |
| Instruments (minified) | 3,422 | 2,764 | 673 | 1.24× | Fused | Win |
| Instruments (prettified) | 4,235 | 4,028 | 1,114 | 1.05× | Fused | Win |
| Marine IK Reverse (minified) | 961 | 817 | 625 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,650 | 2,870 | 2,346 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 904 | 809 | 639 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,916 | 3,093 | 2,445 | 0.94× | Two-stage | Win |
| Random (minified) | 2,141 | 1,638 | 960 | 1.31× | Fused | Win |
| Random (prettified) | 2,660 | 2,316 | 1,429 | 1.15× | Fused | Win |
| Twitter (minified) | 2,426 | 2,017 | 1,026 | 1.20× | Fused | Win |
| Twitter (prettified) | 3,040 | 2,641 | 1,355 | 1.15× | Fused | Win |
| Apache Builds Small (minified) | 3,570 | 2,802 | 1,739 | 1.27× | Fused | Win |
| Canada Small (minified) | 1,099 | 1,002 | 844 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 3,721 | 3,195 | 2,081 | 1.16× | Fused | Win |
| Canada Small (prettified) | 2,197 | 2,265 | 1,978 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,462 | 1,927 | 1,235 | 1.28× | Fused | Win |
| CitmCatalog Small (prettified) | 3,113 | 3,237 | 2,062 | 0.96× | Two-stage | Win |
| Discord Small (minified) | 3,119 | 2,254 | 900 | 1.38× | Fused | Win |
| Discord Small (prettified) | 3,422 | 2,973 | 1,209 | 1.15× | Fused | Win |
| Github Events Small (minified) | 3,250 | 2,523 | 1,735 | 1.29× | Fused | Win |
| Google Maps Response Small (minified) | 2,609 | 2,078 | 1,094 | 1.26× | Fused | Win |
| Github Events Small (prettified) | 3,557 | 3,118 | 2,125 | 1.14× | Fused | Win |
| Google Maps Response Small (prettified) | 3,939 | 3,644 | 2,198 | 1.08× | Fused | Win |
| Instruments Small (minified) | 3,380 | 2,457 | 646 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 3,911 | 3,635 | 1,067 | 1.08× | Fused | Win |
| Marine IK Reverse Small (minified) | 1,044 | 910 | 562 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,664 | 3,125 | 2,047 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,154 | 965 | 589 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,869 | 3,407 | 2,131 | 0.84× | Two-stage | Win |
| Random Small (minified) | 2,650 | 2,088 | 1,042 | 1.27× | Fused | Win |
| Random Small (prettified) | 3,406 | 3,020 | 1,546 | 1.13× | Fused | Win |
| Twitter Small (minified) | 2,813 | 2,286 | 1,089 | 1.23× | Fused | Win |
| Twitter Small (prettified) | 3,325 | 2,853 | 1,403 | 1.17× | Fused | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,142 | n/a | 500 | n/a | n/a | Win |
| Double (POD) | 1,492 | n/a | 390 | n/a | n/a | Win |
| Int64 (POD) | 1,903 | n/a | 342 | n/a | n/a | Win |
| String (POD) | 1,788 | n/a | 812 | n/a | n/a | Win |
| Uint64 (POD) | 1,680 | n/a | 316 | n/a | n/a | Win |
| Apache Builds (minified) | 2,157 | 1,819 | 1,723 | 1.19× | Fused | Win |
| Canada (minified) | 949 | 882 | 803 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 2,361 | 2,162 | 2,180 | 1.09× | Fused | Win |
| Canada (prettified) | 1,903 | 1,907 | 1,771 | 1.00× | Tie | Win |
| CitmCatalog (minified) | 2,483 | 2,125 | 1,878 | 1.17× | Fused | Win |
| CitmCatalog (prettified) | 4,074 | 4,244 | 3,723 | 0.96× | Two-stage | Win |
| Discord (minified) | 2,820 | 2,263 | 1,720 | 1.25× | Fused | Win |
| Discord (prettified) | 3,243 | 3,083 | 2,468 | 1.05× | Fused | Win |
| Github Events (minified) | 2,522 | 1,898 | 1,383 | 1.33× | Fused | Win |
| Google Maps Response (minified) | 2,036 | 1,645 | 1,270 | 1.24× | Fused | Win |
| Github Events (prettified) | 2,530 | 2,168 | 1,658 | 1.17× | Fused | Win |
| Google Maps Response (prettified) | 3,373 | 2,941 | 2,501 | 1.15× | Fused | Win |
| Instruments (minified) | 3,366 | 2,739 | 1,331 | 1.23× | Fused | Win |
| Instruments (prettified) | 3,994 | 4,004 | 2,117 | 1.00× | Tie | Win |
| Marine IK Reverse (minified) | 944 | 824 | 609 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,780 | 2,801 | 2,179 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 944 | 821 | 609 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,837 | 2,755 | 2,295 (n/c) | 1.03× | Fused | Win |
| Random (minified) | 1,814 | 1,406 | 1,408 | 1.29× | Fused | Win |
| Random (prettified) | 2,359 | 2,038 | 2,140 | 1.16× | Fused | Win |
| Twitter (minified) | 2,554 | 2,047 | 1,072 | 1.25× | Fused | Win |
| Twitter (prettified) | 2,711 | 2,440 | 1,424 | 1.11× | Fused | Win |
| Apache Builds Small (minified) | 2,565 | 2,259 | 2,008 | 1.14× | Fused | Win |
| Canada Small (minified) | 1,142 | 977 | 794 | 1.17× | Fused | Win |
| Apache Builds Small (prettified) | 2,890 | 2,646 | 2,450 | 1.09× | Fused | Win |
| Canada Small (prettified) | 2,364 | 2,302 | 1,896 | 1.03× | Fused | Win |
| CitmCatalog Small (minified) | 2,234 | 1,931 | 1,603 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 2,782 | 2,961 | 2,536 | 0.94× | Two-stage | Win |
| Discord Small (minified) | 2,562 | 1,915 | 1,578 | 1.34× | Fused | Win |
| Discord Small (prettified) | 2,854 | 2,511 | 2,085 | 1.14× | Fused | Win |
| Github Events Small (minified) | 2,729 | 2,108 | 1,445 | 1.29× | Fused | Win |
| Google Maps Response Small (minified) | 2,000 | 1,678 | 1,317 | 1.19× | Fused | Win |
| Github Events Small (prettified) | 3,046 | 2,356 | 1,883 | 1.29× | Fused | Win |
| Google Maps Response Small (prettified) | 3,288 | 3,035 | 2,615 | 1.08× | Fused | Win |
| Instruments Small (minified) | 3,812 | 2,692 | 1,241 | 1.42× | Fused | Win |
| Instruments Small (prettified) | 3,857 | 3,818 | 1,942 | 1.01× | Fused | Win |
| Marine IK Reverse Small (minified) | 1,087 | 899 | 572 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,897 | 3,117 | 1,977 | 0.93× | Two-stage | Win |
| Marine IK Small (minified) | 1,167 | 921 | 523 | 1.27× | Fused | Win |
| Marine IK Small (prettified) | 2,808 | 3,166 | 1,807 | 0.89× | Two-stage | Win |
| Random Small (minified) | 2,305 | 1,794 | 1,551 | 1.28× | Fused | Win |
| Random Small (prettified) | 3,003 | 2,606 | 2,225 | 1.15× | Fused | Win |
| Twitter Small (minified) | 2,942 | 2,133 | 1,139 | 1.38× | Fused | Win |
| Twitter Small (prettified) | 3,107 | 2,721 | 1,496 | 1.14× | Fused | Win |

#### Freshly allocated objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,389 | 1,389 | 1.00× | Tie |
| Canada (minified) | 779 | 708 | 1.10× | Win |
| Apache Builds (prettified) | 1,697 | 1,770 | 0.96× | **Loss** |
| Canada (prettified) | 1,665 | 1,625 | 1.02× | Win |
| CitmCatalog (minified) | 1,781 | 1,518 | 1.17× | Win |
| CitmCatalog (prettified) | 3,631 | 3,051 | 1.19× | Win |
| Discord (minified) | 1,821 | 1,601 | 1.14× | Win |
| Discord (prettified) | 2,423 | 2,300 | 1.05× | Win |
| Github Events (minified) | 1,540 | 1,653 | 0.93× | **Loss** |
| Google Maps Response (minified) | 1,539 | 1,433 | 1.07× | Win |
| Github Events (prettified) | 1,795 | 1,989 | 0.90× | **Loss** |
| Google Maps Response (prettified) | 2,819 | 2,847 | 0.99× | **Loss** |
| Instruments (minified) | 2,495 | 1,612 | 1.55× | Win |
| Instruments (prettified) | 3,568 | 2,489 | 1.43× | Win |
| Marine IK Reverse (minified) | 751 | 653 | 1.15× | Win |
| Marine IK Reverse (prettified) | 2,588 | 2,307 | 1.12× | Win |
| Marine IK (minified) | 747 | 669 | 1.12× | Win |
| Marine IK (prettified) | 2,615 | 2,380 | 1.10× | Win |
| Random (minified) | 1,101 | 1,063 | 1.04× | Win |
| Random (prettified) | 1,635 | 1,623 | 1.01× | Win |
| Twitter (minified) | 1,665 | 1,667 | 1.00× | Tie |
| Twitter (prettified) | 2,034 | 2,096 | 0.97× | **Loss** |
| Apache Builds Small (minified) | 2,002 | 1,838 | 1.09× | Win |
| Canada Small (minified) | 833 | 714 | 1.17× | Win |
| Apache Builds Small (prettified) | 2,362 | 2,375 | 0.99× | Tie |
| Canada Small (prettified) | 1,904 | 1,691 | 1.13× | Win |
| CitmCatalog Small (minified) | 1,596 | 1,304 | 1.22× | Win |
| CitmCatalog Small (prettified) | 2,590 | 2,267 | 1.14× | Win |
| Discord Small (minified) | 1,565 | 1,595 | 0.98× | **Loss** |
| Discord Small (prettified) | 2,122 | 2,059 | 1.03× | Win |
| Github Events Small (minified) | 1,590 | 1,759 | 0.90× | **Loss** |
| Google Maps Response Small (minified) | 1,581 | 1,517 | 1.04× | Win |
| Github Events Small (prettified) | 1,909 | 2,385 | 0.80× | **Loss** |
| Google Maps Response Small (prettified) | 2,867 | 2,937 | 0.98× | **Loss** |
| Instruments Small (minified) | 2,449 | 1,658 | 1.48× | Win |
| Instruments Small (prettified) | 3,438 | 2,552 | 1.35× | Win |
| Marine IK Reverse Small (minified) | 746 | 660 | 1.13× | Win |
| Marine IK Reverse Small (prettified) | 2,699 | 2,343 | 1.15× | Win |
| Marine IK Small (minified) | 770 | 655 | 1.18× | Win |
| Marine IK Small (prettified) | 2,671 | 2,312 | 1.16× | Win |
| Random Small (minified) | 1,603 | 1,355 | 1.18× | Win |
| Random Small (prettified) | 2,330 | 1,940 | 1.20× | Win |
| Twitter Small (minified) | 1,956 | 1,853 | 1.06× | Win |
| Twitter Small (prettified) | 2,494 | 2,465 | 1.01× | Win |

#### Reused objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,819 | 1,746 | 1.04× | Win |
| Canada (minified) | 882 | 796 | 1.11× | Win |
| Apache Builds (prettified) | 2,162 | 2,171 | 1.00× | Tie |
| Canada (prettified) | 1,907 | 1,806 | 1.06× | Win |
| CitmCatalog (minified) | 2,125 | 1,797 | 1.18× | Win |
| CitmCatalog (prettified) | 4,244 | 3,515 | 1.21× | Win |
| Discord (minified) | 2,263 | 1,884 | 1.20× | Win |
| Discord (prettified) | 3,083 | 2,654 | 1.16× | Win |
| Github Events (minified) | 1,898 | 1,925 | 0.99× | **Loss** |
| Google Maps Response (minified) | 1,645 | 1,569 | 1.05× | Win |
| Github Events (prettified) | 2,168 | 2,240 | 0.97× | **Loss** |
| Google Maps Response (prettified) | 2,941 | 2,899 | 1.01× | Win |
| Instruments (minified) | 2,739 | 1,665 | 1.64× | Win |
| Instruments (prettified) | 4,004 | 2,619 | 1.53× | Win |
| Marine IK Reverse (minified) | 824 | 725 | 1.14× | Win |
| Marine IK Reverse (prettified) | 2,801 | 2,539 | 1.10× | Win |
| Marine IK (minified) | 821 | 754 | 1.09× | Win |
| Marine IK (prettified) | 2,755 | 2,588 | 1.06× | Win |
| Random (minified) | 1,406 | 1,362 | 1.03× | Win |
| Random (prettified) | 2,038 | 2,006 | 1.02× | Win |
| Twitter (minified) | 2,047 | 1,958 | 1.05× | Win |
| Twitter (prettified) | 2,440 | 2,455 | 0.99× | Tie |
| Apache Builds Small (minified) | 2,259 | 2,008 | 1.12× | Win |
| Canada Small (minified) | 977 | 822 | 1.19× | Win |
| Apache Builds Small (prettified) | 2,646 | 2,513 | 1.05× | Win |
| Canada Small (prettified) | 2,302 | 1,872 | 1.23× | Win |
| CitmCatalog Small (minified) | 1,931 | 1,398 | 1.38× | Win |
| CitmCatalog Small (prettified) | 2,961 | 2,434 | 1.22× | Win |
| Discord Small (minified) | 1,915 | 1,797 | 1.07× | Win |
| Discord Small (prettified) | 2,511 | 2,391 | 1.05× | Win |
| Github Events Small (minified) | 2,108 | 1,992 | 1.06× | Win |
| Google Maps Response Small (minified) | 1,678 | 1,553 | 1.08× | Win |
| Github Events Small (prettified) | 2,356 | 2,535 | 0.93× | **Loss** |
| Google Maps Response Small (prettified) | 3,035 | 3,085 | 0.98× | **Loss** |
| Instruments Small (minified) | 2,692 | 1,713 | 1.57× | Win |
| Instruments Small (prettified) | 3,818 | 2,744 | 1.39× | Win |
| Marine IK Reverse Small (minified) | 899 | 728 | 1.24× | Win |
| Marine IK Reverse Small (prettified) | 3,117 | 2,587 | 1.21× | Win |
| Marine IK Small (minified) | 921 | 759 | 1.21× | Win |
| Marine IK Small (prettified) | 3,166 | 2,502 | 1.27× | Win |
| Random Small (minified) | 1,794 | 1,486 | 1.21× | Win |
| Random Small (prettified) | 2,606 | 2,242 | 1.16× | Win |
| Twitter Small (minified) | 2,133 | 2,115 | 1.01× | Win |
| Twitter Small (prettified) | 2,721 | 2,699 | 1.01× | Win |

#### Freshly allocated objects: partial reading and validation

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (minified) | 7,599 | 3,307 | 2.30× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,156 | 4,116 | 2.22× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,463 | 3,003 | 1.82× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,698 | 3,866 | 1.73× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 10,318 | 7,858 | 1.31× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 11,616 | 9,464 | 1.23× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 7,047 | 6,923 | 1.02× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 8,413 | 8,140 | 1.03× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,735 | 7,570 | 1.15× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,451 | 8,874 | 1.07× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,729 | 7,086 | 0.95× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,960 | 8,086 | 0.86× | Loss |

**Reused objects.**

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,060 | 3,480 | 2.32× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,290 | 4,316 | 2.15× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,821 | 3,275 | 1.78× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,185 | 4,124 | 1.74× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 10,452 | 8,057 | 1.30× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 11,928 | 9,628 | 1.24× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 7,644 | 7,095 | 1.08× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 8,613 | 8,202 | 1.05× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,993 | 7,757 | 1.16× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,585 | 8,816 | 1.09× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 7,295 | 7,145 | 1.02× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,610 | 7,783 | 0.98× | Loss |

**Validation.**

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 2,948 | 2,433 | 1.21× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 4,034 | 2,640 | 1.53× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 3,036 | 2,451 | 1.24× | Win |

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
#if JSONIFIER_AVX512 && JSONIFIER_COMPILER_GCC
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
#elif JSONIFIER_AVX512 && JSONIFIER_COMPILER_MSVC
	inline constexpr uint64_t simdBlocksPerStep{ 2 };
#elif JSONIFIER_AVX512 && JSONIFIER_COMPILER_CLANG
	inline constexpr uint64_t simdBlocksPerStep{ 8 };
#elif JSONIFIER_NEON && JSONIFIER_COMPILER_GCC
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
	inline constexpr uint64_t simdTapeStep{ 4 };
#else
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
	inline constexpr uint64_t simdTapeStep{ 2 };
#endif
```

`simd/simd_config.hpp` defines `simdBlocksPerStep` and `simdTapeStep` directly in one `#if` chain keyed on ISA and compiler together. On AVX-512 the step is 8 blocks under Clang, 4 under GCC and 2 under MSVC; NEON under GCC takes 4 blocks with a tape step of 4; every other target, AVX2 and NEON under Clang and MSVC included, shares the final branch at 4 blocks and a tape step of 2. The AVX-512 cells define no `simdTapeStep`, since their compress drain has no stepped scalar writer.

These values were selected by parameter sweeps across the CI targets. That Clang and GCC want *different* geometry on the identical ISA reflects how each compiler schedules the unrolled block bodies. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing 2 to 8 blocks (128 to 512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a window of independent block computations up to 4x wider than simdjson's fixed 128-byte step, and never narrower. Past the last full step the reader halves the batch (8, 4, 2, then 1 block) rather than dropping to a block-at-a-time loop.

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

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit: a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional: `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNext`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path, freshly allocated, on 109 of 110 minified documents and 84 of 110 prettified ones (50 of 66 on x86, 34 of 44 on the M2 Pro); the tape keeps the Marine IK family on x86 (see §2.2) |
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

The last row is the root of every other difference. simdjson's core API can ship one x86 binary that picks its instruction set at run time; Jsonifier deliberately does not. Its founding constraint is that apart from the data, nothing is decided at run time. The schema, the compiler, the parse options and the instruction-set tier are all known at build time, so every routing decision in this paper (tape or no tape, whitespace lane or not, 2, 4 or 8 blocks, tape step of 2 or 4) is still resolved before the first byte is read.

The tier is chosen when CMake configures the project, by a detector run on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS` (and capped with `JSONIFIER_SIMD_TIER`), and the whole library is compiled once for it, so every call is a direct, inlinable call with no per-call check. simdjson dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time. Jsonifier keeps one tier per build because per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, which GCC and Clang then reject. The cost is portability: a binary for mixed hardware is configured for the lowest tier it must support.

## 8. Conclusion

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 193 of the 220 freshly allocated corpus tests: 109 of 110 minified documents and 84 of 110 prettified ones, while the two-stage path's 26 wins sit mostly in the float-heavy Marine IK family on x86 and in prettified documents on the M2 Pro. The fused path alone beats simdjson On Demand on 244 of 245 tests freshly allocated (the other is a tie) and all 245 reused, the stage-1 + reflection path beats simdjson's reflection reader on 71 of 88 freshly allocated and 75 of 88 reused, and on partial reading Jsonifier wins 36 of 40 and loses 1. On the three AVX-512 builds the faster path beats simdjson On Demand on all 147 tests in both runs, and its stage-1 + reflection path beats simdjson's reflection reader on 32 of 44 freshly allocated and 38 of 44 reused. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier [4]. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance [5].*

## References

1. G. Langdale, D. Lemire. "Parsing Gigabytes of JSON per Second." *The VLDB Journal*, 28(6), 2019.
2. simdjson. https://github.com/simdjson/simdjson (commit 4ba1cb3ae).
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
