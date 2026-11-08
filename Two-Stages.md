# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 109 of the 110 minified documents (the other is a tie), the tape is faster on 64 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 36 of the 44 prettified documents, the tape 7 and 1 is a tie.

This paper describes both halves of that architecture: the single-pass primary path, and Jsonifier's stage-1 implementation, including its per-compiler-tuned step geometry, its fold-expression drain architecture for bitmask-to-index extraction, and its distributed UTF-8 validation strategy. Throughout, it compares both directly with simdjson's current design.

---

## 1. Background: the canonical two-stage model

simdjson's pipeline is well documented. Stage 1 sweeps the input in 64-byte blocks, computing per-block bitmasks: backslashes, odd-length escape terminations, quotes, an in-string range mask derived via carry-less multiplication (prefix XOR), structural characters (`{ } [ ] : ,`), whitespace, and pseudo-structural scalar starts. The surviving bits are converted to byte offsets, the *structural tape*, via a tzcnt/blsr extraction loop. Stage 2 then walks the tape to build a DOM or, in the On Demand API, lazily materializes only the values the caller touches.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 9, 2026, with Jsonifier [c80a6d5](https://github.com/nihilai-collective/jsonifier/commit/c80a6d5), Glaze [8283164](https://github.com/stephenberry/glaze/commit/82831644), simdjson [f0c2d2a](https://github.com/simdjson/simdjson/commit/f0c2d2ac2) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Five freshly allocated full-parse rows contain non-converged results (three on Windows/MSVC and one on each macOS build), and four reused rows do (three on Windows/MSVC and one on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 172 tests, 2 are statistical ties, and the two-stage path wins 71. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 44 | 1 | 4 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 46 | 0 | 3 | 47 of 49 |
| **Aggregate** | **172** | **2** | **71** | **242 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 19 / 0 / 3 |
| Small minified documents | 10 / 1 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 0 / 0 / 11 | 0 / 0 / 22 | 17 / 1 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 43 / 1 / 5 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 48 / 1 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **244 / 0 / 1** | **237 / 3 / 5** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 27 | 3 | 19 | 47 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 45 | 1 | 3 | 49 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 45 | 0 | 4 | 48 of 49 |
| **Aggregate** | **171** | **4** | **70** | **242 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 1 / 10 | 1 / 0 / 21 | 21 / 1 / 0 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 0 / 2 / 9 | 0 / 0 / 22 | 16 / 0 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 43 / 1 / 5 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **244 / 0 / 1** | **239 / 1 / 5** |

With objects reused, across 245 tests the fused path wins 171, ties 4 and loses 70 to the two-stage path, and the x86 split holds: the tape wins 31 of the 33 full-size prettified documents (the fused path takes Canada on Linux/GCC, and Github Events under MSVC is a tie) and 31 of the 33 Small prettified ones (Apache Builds Small and Discord Small under MSVC are ties), and the fused path wins 65 of the 66 minified documents, losing Github Events on Linux/GCC to the tape. On the M2 Pro the fused path wins 43 of the 44 minified documents, with Canada Small on macOS/Clang going to the tape, and 37 of the 44 prettified ones, with 1 tie and 6 tape wins. Against simdjson, the faster Jsonifier path wins 244, ties none and loses 1, and the fused path alone wins 239, ties 1 and loses 5.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 545 | 276 | 71 | 1.98× | Fused | Win |
| Double (POD) | 696 | 500 | 149 | 1.39× | Fused | Win |
| Int64 (POD) | 1,584 | 908 | 315 | 1.75× | Fused | Win |
| String (POD) | 1,007 | 799 | 545 | 1.26× | Fused | Win |
| Uint64 (POD) | 1,616 | 914 | 312 | 1.77× | Fused | Win |
| Apache Builds (minified) | 955 | 843 | 710 | 1.13× | Fused | Win |
| Canada (minified) | 688 | 654 | 430 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,097 | 1,077 | 899 | 1.02× | Fused | Win |
| Canada (prettified) | 1,362 | 1,488 | 940 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,564 | 1,265 | 589 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,283 | 2,547 | 1,240 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,241 | 1,023 | 670 | 1.21× | Fused | Win |
| Discord (prettified) | 1,325 | 1,474 | 944 | 0.90× | Two-stage | Win |
| Github Events (minified) | 942 | 871 | 620 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 1,380 | 1,183 | 533 | 1.17× | Fused | Win |
| Github Events (prettified) | 1,052 | 1,068 | 757 | 0.99× | Two-stage | Win |
| Google Maps Response (prettified) | 2,177 | 2,324 | 1,077 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,060 | 1,485 | 755 | 1.39× | Fused | Win |
| Instruments (prettified) | 2,129 | 2,308 | 1,194 | 0.92× | Two-stage | Win |
| Marine IK Reverse (minified) | 563 | 497 | 196 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 1,751 | 1,817 | 667 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 558 | 509 | 342 | 1.10× | Fused | Win |
| Marine IK (prettified) | 1,781 | 1,819 | 1,209 | 0.98× | Two-stage | Win |
| Random (minified) | 806 | 738 | 559 | 1.09× | Fused | Win |
| Random (prettified) | 1,046 | 1,118 | 844 | 0.94× | Two-stage | Win |
| Twitter (minified) | 1,175 | 999 | 822 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,366 | 1,432 | 1,088 | 0.95× | Two-stage | Win |
| Apache Builds Small (minified) | 1,110 | 982 | 723 | 1.13× | Fused | Win |
| Canada Small (minified) | 631 | 600 | 348 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,217 | 1,240 | 896 | 0.98× | Two-stage | Win |
| Canada Small (prettified) | 1,303 | 1,401 | 815 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 866 | 731 | 524 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,172 | 1,259 | 913 | 0.93× | Two-stage | Win |
| Discord Small (minified) | 1,092 | 888 | 669 | 1.23× | Fused | Win |
| Discord Small (prettified) | 1,138 | 1,179 | 876 | 0.97× | Two-stage | Win |
| Github Events Small (minified) | 982 | 987 | 613 | 0.99× | Tie | Win |
| Google Maps Response Small (minified) | 1,221 | 1,041 | 525 | 1.17× | Fused | Win |
| Github Events Small (prettified) | 1,043 | 1,241 | 735 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,944 | 2,086 | 1,048 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,206 | 1,480 | 761 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,128 | 2,295 | 1,225 | 0.93× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 445 | 423 | 151 | 1.05× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,307 | 1,552 | 570 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 489 | 458 | 311 | 1.07× | Fused | Win |
| Marine IK Small (prettified) | 1,517 | 1,636 | 1,080 | 0.93× | Two-stage | Win |
| Random Small (minified) | 867 | 820 | 515 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,096 | 1,224 | 797 | 0.90× | Two-stage | Win |
| Twitter Small (minified) | 1,597 | 1,250 | 1,191 | 1.28× | Fused | Win |
| Twitter Small (prettified) | 1,554 | 1,679 | 1,557 | 0.93× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,946 | 608 | 281 | 3.20× | Fused | Win |
| Double (POD) | 1,259 | 752 | 356 | 1.68× | Fused | Win |
| Int64 (POD) | 3,043 | 1,642 | 774 | 1.85× | Fused | Win |
| String (POD) | 1,796 | 1,346 | 1,393 | 1.33× | Fused | Win |
| Uint64 (POD) | 3,217 | 1,566 | 716 | 2.05× | Fused | Win |
| Apache Builds (minified) | 1,692 | 1,482 | 1,192 | 1.14× | Fused | Win |
| Canada (minified) | 1,053 | 963 | 688 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,680 | 1,904 | 1,518 | 0.88× | Two-stage | Win |
| Canada (prettified) | 2,053 | 2,113 | 1,573 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,205 | 1,642 | 1,021 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 2,840 | 3,400 | 2,182 | 0.84× | Two-stage | Win |
| Discord (minified) | 2,276 | 1,802 | 947 | 1.26× | Fused | Win |
| Discord (prettified) | 1,787 | 2,477 | 1,361 | 0.72× | Two-stage | Win |
| Github Events (minified) | 1,624 | 1,565 | 958 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 2,438 | 1,782 | 1,042 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,651 | 1,876 | 1,258 | 0.88× | Two-stage | Win |
| Google Maps Response (prettified) | 2,786 | 3,458 | 2,101 | 0.81× | Two-stage | Win |
| Instruments (minified) | 2,446 | 1,945 | 756 | 1.26× | Fused | Win |
| Instruments (prettified) | 2,167 | 3,000 | 1,256 | 0.72× | Two-stage | Win |
| Marine IK Reverse (minified) | 780 | 753 | 558 | 1.04× | Fused | Win |
| Marine IK Reverse (prettified) | 2,509 | 2,577 | 2,075 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 823 | 749 | 558 | 1.10× | Fused | Win |
| Marine IK (prettified) | 2,528 | 2,591 | 1,986 | 0.98× | Two-stage | Win |
| Random (minified) | 1,254 | 1,100 | 824 | 1.14× | Fused | Win |
| Random (prettified) | 1,388 | 1,677 | 1,243 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,864 | 1,685 | 918 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,561 | 2,172 | 1,220 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 2,593 | 2,182 | 1,420 | 1.19× | Fused | Win |
| Canada Small (minified) | 1,002 | 952 | 708 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 2,178 | 2,646 | 1,741 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 1,896 | 2,139 | 1,679 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,664 | 1,365 | 956 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 1,793 | 2,339 | 1,662 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 2,001 | 1,735 | 765 | 1.15× | Fused | Win |
| Discord Small (prettified) | 1,605 | 2,326 | 1,040 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,067 | 1,814 | 1,392 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 2,213 | 1,650 | 1,048 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 1,858 | 2,236 | 1,676 | 0.83× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,568 | 3,170 | 2,118 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,462 | 1,869 | 668 | 1.32× | Fused | Win |
| Instruments Small (prettified) | 2,075 | 2,976 | 1,114 | 0.70× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 779 | 716 | 494 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,125 | 2,459 | 1,833 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 861 | 762 | 497 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,206 | 2,660 | 1,836 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,716 | 1,523 | 840 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,640 | 2,317 | 1,281 | 0.71× | Two-stage | Win |
| Twitter Small (minified) | 2,456 | 2,130 | 1,003 | 1.15× | Fused | Win |
| Twitter Small (prettified) | 1,852 | 2,723 | 1,316 | 0.68× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,288 | 632 | 225 | 3.62× | Fused | Win |
| Double (POD) | 1,210 | 720 | 303 | 1.68× | Fused | Win |
| Int64 (POD) | 3,353 | 1,643 | 727 | 2.04× | Fused | Win |
| String (POD) | 1,793 | 1,303 | 1,282 | 1.38× | Fused | Win |
| Uint64 (POD) | 3,364 | 1,601 | 722 | 2.10× | Fused | Win |
| Apache Builds (minified) | 1,644 | 1,421 | 1,308 | 1.16× | Fused | Win |
| Canada (minified) | 1,132 | 1,000 | 839 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,683 | 1,811 | 1,683 | 0.93× | Two-stage | Win |
| Canada (prettified) | 2,262 | 2,253 | 1,882 | 1.00× | Fused | Win |
| CitmCatalog (minified) | 2,346 | 1,830 | 1,479 | 1.28× | Fused | Win |
| CitmCatalog (prettified) | 3,548 | 3,868 | 3,222 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,288 | 1,872 | 1,365 | 1.22× | Fused | Win |
| Discord (prettified) | 2,248 | 2,566 | 1,976 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,731 | 1,545 | 1,549 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 2,484 | 1,810 | 1,120 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,722 | 1,860 | 1,666 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 3,200 | 3,416 | 2,235 | 0.94× | Two-stage | Win |
| Instruments (minified) | 3,271 | 2,260 | 1,647 | 1.45× | Fused | Win |
| Instruments (prettified) | 2,964 | 3,290 | 2,644 | 0.90× | Two-stage | Win |
| Marine IK Reverse (minified) | 838 | 728 | 587 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,540 | 2,625 | 2,158 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 856 | 737 | 581 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,532 | 2,623 | 2,113 | 0.97× | Two-stage | Win |
| Random (minified) | 1,198 | 1,103 | 1,060 | 1.09× | Fused | Win |
| Random (prettified) | 1,563 | 1,859 | 1,707 | 0.84× | Two-stage | Win |
| Twitter (minified) | 2,008 | 1,697 | 1,488 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,910 | 2,136 | 1,959 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 2,397 | 2,117 | 1,679 | 1.13× | Fused | Win |
| Canada Small (minified) | 1,071 | 898 | 798 | 1.19× | Fused | Win |
| Apache Builds Small (prettified) | 2,200 | 2,423 | 2,080 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 2,114 | 2,196 | 1,860 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,695 | 1,463 | 1,251 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 2,102 | 2,425 | 2,131 | 0.87× | Two-stage | Win |
| Discord Small (minified) | 1,983 | 1,667 | 1,345 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,884 | 2,152 | 1,806 | 0.88× | Two-stage | Win |
| Github Events Small (minified) | 2,050 | 1,725 | 1,955 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 2,223 | 1,664 | 1,154 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 2,086 | 2,253 | 2,445 | 0.93× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,978 | 3,155 | 2,267 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,195 | 2,183 | 1,504 | 1.46× | Fused | Win |
| Instruments Small (prettified) | 2,894 | 3,239 | 2,434 | 0.89× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 853 | 740 | 548 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,325 | 2,572 | 2,011 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 949 | 788 | 540 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,441 | 2,649 | 2,048 | 0.92× | Two-stage | Win |
| Random Small (minified) | 1,541 | 1,496 | 1,157 | 1.03× | Fused | Win |
| Random Small (prettified) | 1,903 | 2,221 | 1,734 | 0.86× | Two-stage | Win |
| Twitter Small (minified) | 2,877 | 2,263 | 1,949 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,382 | 2,923 | 2,533 | 0.82× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,419 | 353 | 180 | 4.02× | Fused | Win |
| Double (POD) | 955 | 499 | 157 | 1.92× | Fused | Win |
| Int64 (POD) | 2,536 | 924 | 448 | 2.74× | Fused | Win |
| String (POD) | 1,304 | 749 | 755 | 1.74× | Fused | Win |
| Uint64 (POD) | 2,475 | 840 | 435 | 2.95× | Fused | Win |
| Apache Builds (minified) | 1,177 | 987 | 729 | 1.19× | Fused | Win |
| Canada (minified) | 838 | 721 | 540 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 1,337 | 1,227 | 935 | 1.09× | Fused | Win |
| Canada (prettified) | 1,737 | 1,574 | 1,222 | 1.10× | Fused | Win |
| CitmCatalog (minified) | 1,984 | 1,346 | 1,126 | 1.47× | Fused | Win |
| CitmCatalog (prettified) | 2,946 | 2,532 | 2,011 | 1.16× | Fused | Win |
| Discord (minified) | 1,795 | 1,223 | 969 | 1.47× | Fused | Win |
| Discord (prettified) | 1,969 | 1,688 | 1,338 | 1.17× | Fused | Win |
| Github Events (minified) | 1,599 | 1,398 | 1,003 | 1.14× | Fused | Win |
| Google Maps Response (minified) | 1,902 | 1,169 | 737 | 1.63× | Fused | Win |
| Github Events (prettified) | 1,690 | 1,572 | 1,248 | 1.08× | Fused | Win |
| Google Maps Response (prettified) | 2,660 | 2,238 | 1,400 | 1.19× | Fused | Win |
| Instruments (minified) | 2,325 | 1,576 | 1,240 | 1.47× | Fused | Win |
| Instruments (prettified) | 2,811 | 2,434 | 1,885 | 1.15× | Fused | Win |
| Marine IK Reverse (minified) | 707 | 588 | 488 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 1,867 | 1,799 | 1,519 | 1.04× | Fused | Win |
| Marine IK (minified) | 688 | 570 | 464 | 1.21× | Fused | Win |
| Marine IK (prettified) | 1,778 (n/c) | 1,722 | 1,557 (n/c) | 1.03× | Fused | Win |
| Random (minified) | 988 | 957 | 658 | 1.03× | Fused | Win |
| Random (prettified) | 1,158 | 1,229 | 1,079 | 0.94× | Two-stage | Win |
| Twitter (minified) | 2,094 | 1,706 | 1,054 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,179 | 2,037 | 1,373 | 1.07× | Fused | Win |
| Apache Builds Small (minified) | 1,578 | 1,471 | 896 | 1.07× | Fused | Win |
| Canada Small (minified) | 814 | 723 | 486 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,662 | 1,750 | 1,118 | 0.95× | Two-stage | Win |
| Canada Small (prettified) | 1,693 | 1,600 | 1,125 | 1.06× | Fused | Win |
| CitmCatalog Small (minified) | 1,299 | 913 | 863 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 1,802 | 1,655 | 1,434 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,706 | 1,333 | 940 | 1.28× | Fused | Win |
| Discord Small (prettified) | 1,810 | 1,703 | 1,233 | 1.06× | Fused | Win |
| Github Events Small (minified) | 1,556 | 1,272 | 1,045 | 1.22× | Fused | Win |
| Google Maps Response Small (minified) | 1,784 | 1,164 | 787 | 1.53× | Fused | Win |
| Github Events Small (prettified) | 1,525 | 1,535 | 1,480 | 0.99× | Tie | Win |
| Google Maps Response Small (prettified) | 2,546 | 2,320 | 1,479 | 1.10× | Fused | Win |
| Instruments Small (minified) | 2,399 | 1,695 | 1,156 | 1.42× | Fused | Win |
| Instruments Small (prettified) | 2,858 | 2,434 | 1,785 | 1.17× | Fused | Win |
| Marine IK Reverse Small (minified) | 656 | 566 | 398 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,783 | 1,896 | 1,421 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 715 | 602 | 382 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 1,988 | 2,002 | 1,399 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,259 | 1,137 | 747 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,606 | 1,409 | 1,121 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,364 | 1,884 | 1,164 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,350 | 2,254 | 1,477 | 1.04× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,102 | 318 | 152 | 3.47× | Fused | Win |
| Double (POD) | 864 | 346 | 193 | 2.50× | Fused | Win |
| Int64 (POD) | 2,134 | 783 | 434 | 2.73× | Fused | Win |
| String (POD) | 1,265 | 708 | 795 | 1.79× | Fused | Win |
| Uint64 (POD) | 2,177 | 772 | 439 | 2.82× | Fused | Win |
| Apache Builds (minified) | 1,400 | 999 | 774 | 1.40× | Fused | Win |
| Canada (minified) | 759 | 675 | 472 (n/c) | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,402 | 1,308 | 1,065 | 1.07× | Fused | Win |
| Canada (prettified) | 1,551 | 1,399 | 1,063 | 1.11× | Fused | Win |
| CitmCatalog (minified) | 1,662 | 1,249 | 792 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 2,462 | 2,580 | 1,699 | 0.95× | Two-stage | Win |
| Discord (minified) | 2,103 | 1,132 | 554 | 1.86× | Fused | Win |
| Discord (prettified) | 1,933 | 1,479 | 1,012 | 1.31× | Fused | Win |
| Github Events (minified) | 1,779 | 1,387 | 790 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 2,047 | 998 | 687 | 2.05× | Fused | Win |
| Github Events (prettified) | 1,827 | 1,716 | 918 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,461 | 1,998 | 1,313 | 1.23× | Fused | Win |
| Instruments (minified) | 2,379 | 1,390 | 555 | 1.71× | Fused | Win |
| Instruments (prettified) | 2,474 | 2,164 | 1,123 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 627 | 527 | 370 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 1,705 | 1,794 | 1,093 (n/c) | 0.95× | Two-stage | Win |
| Marine IK (minified) | 599 | 522 | 372 | 1.15× | Fused | Win |
| Marine IK (prettified) | 1,956 | 1,751 | 1,241 | 1.12× | Fused | Win |
| Random (minified) | 1,110 | 998 | 663 | 1.11× | Fused | Win |
| Random (prettified) | 1,605 | 1,249 | 1,002 | 1.28× | Fused | Win |
| Twitter (minified) | 1,947 | 1,584 | 738 | 1.23× | Fused | Win |
| Twitter (prettified) | 1,926 | 1,823 | 879 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 1,523 | 1,095 | 749 | 1.39× | Fused | Win |
| Canada Small (minified) | 633 | 544 | 361 | 1.16× | Fused | Win |
| Apache Builds Small (prettified) | 1,564 | 1,278 | 967 | 1.22× | Fused | Win |
| Canada Small (prettified) | 1,575 | 1,418 | 985 | 1.11× | Fused | Win |
| CitmCatalog Small (minified) | 1,194 | 939 | 647 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 1,624 | 1,341 | 1,109 | 1.21× | Fused | Win |
| Discord Small (minified) | 1,980 | 1,179 | 589 | 1.68× | Fused | Win |
| Discord Small (prettified) | 1,752 | 1,452 | 791 | 1.21× | Fused | Win |
| Github Events Small (minified) | 1,578 | 1,156 | 994 | 1.37× | Fused | Win |
| Google Maps Response Small (minified) | 2,125 | 1,140 | 778 | 1.86× | Fused | Win |
| Github Events Small (prettified) | 1,686 | 1,450 | 1,236 | 1.16× | Fused | Win |
| Google Maps Response Small (prettified) | 2,401 | 1,984 | 1,538 | 1.21× | Fused | Win |
| Instruments Small (minified) | 2,700 | 1,637 | 562 | 1.65× | Fused | Win |
| Instruments Small (prettified) | 2,746 | 2,472 | 971 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 628 | 544 | 319 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,832 | 1,676 | 1,091 | 1.09× | Fused | Win |
| Marine IK Small (minified) | 657 | 546 | 290 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,016 | 1,977 | 1,027 | 1.02× | Fused | Win |
| Random Small (minified) | 1,386 | 987 | 550 | 1.40× | Fused | Win |
| Random Small (prettified) | 1,423 | 1,202 | 897 | 1.18× | Fused | Win |
| Twitter Small (minified) | 1,996 | 1,460 | 688 | 1.37× | Fused | Win |
| Twitter Small (prettified) | 1,958 | 2,116 | 1,016 | 0.93× | Two-stage | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 561 (n/c) | 288 | 70 | 1.94× | Fused | Win |
| Double (POD) | 840 | 568 | 156 | 1.48× | Fused | Win |
| Int64 (POD) | 1,957 | 991 | 326 | 1.98× | Fused | Win |
| String (POD) | 1,678 | 1,165 | 710 | 1.44× | Fused | Win |
| Uint64 (POD) | 1,957 | 1,018 | 322 | 1.92× | Fused | Win |
| Apache Builds (minified) | 1,479 | 1,222 | 978 | 1.21× | Fused | Win |
| Canada (minified) | 825 | 761 | 481 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 1,456 | 1,504 | 1,227 | 0.97× | Two-stage | Win |
| Canada (prettified) | 1,595 | 1,741 | 1,043 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,848 | 1,437 | 641 | 1.29× | Fused | Win |
| CitmCatalog (prettified) | 2,543 | 2,935 | 1,335 | 0.87× | Two-stage | Win |
| Discord (minified) | 1,865 | 1,394 | 858 | 1.34× | Fused | Win |
| Discord (prettified) | 1,792 | 1,985 | 1,225 | 0.90× | Two-stage | Win |
| Github Events (minified) | 1,465 | 1,264 | 784 | 1.16× | Fused | Win |
| Google Maps Response (minified) | 1,577 | 1,329 | 579 | 1.19× | Fused | Win |
| Github Events (prettified) | 1,523 | 1,507 | 938 | 1.01× | Tie | Win |
| Google Maps Response (prettified) | 2,402 | 2,587 | 1,152 | 0.93× | Two-stage | Win |
| Instruments (minified) | 2,731 | 1,751 | 837 | 1.56× | Fused | Win |
| Instruments (prettified) | 2,478 | 2,688 | 1,318 | 0.92× | Two-stage | Win |
| Marine IK Reverse (minified) | 605 | 547 | 202 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 1,771 (n/c) | 1,813 | 605 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 624 | 545 | 369 | 1.15× | Fused | Win |
| Marine IK (prettified) | 1,912 | 1,983 | 1,295 | 0.96× | Two-stage | Win |
| Random (minified) | 1,078 | 960 | 676 | 1.12× | Fused | Win |
| Random (prettified) | 1,320 | 1,442 | 996 | 0.92× | Two-stage | Win |
| Twitter (minified) | 1,897 | 1,492 | 1,100 | 1.27× | Fused | Win |
| Twitter (prettified) | 1,813 | 1,973 | 1,393 | 0.92× | Two-stage | Win |
| Apache Builds Small (minified) | 1,415 | 1,268 | 1,096 | 1.12× | Fused | Win |
| Canada Small (minified) | 792 | 734 | 389 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,499 | 1,491 | 1,331 | 1.01× | Tie | Win |
| Canada Small (prettified) | 1,573 | 1,699 | 908 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,154 | 923 | 628 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,459 | 1,602 | 1,078 | 0.91× | Two-stage | Win |
| Discord Small (minified) | 1,591 | 1,175 | 890 | 1.35× | Fused | Win |
| Discord Small (prettified) | 1,541 | 1,552 | 1,216 | 0.99× | Tie | Win |
| Github Events Small (minified) | 1,442 | 1,336 | 767 | 1.08× | Fused | Win |
| Google Maps Response Small (minified) | 1,453 | 1,222 | 596 | 1.19× | Fused | Win |
| Github Events Small (prettified) | 1,414 | 1,633 | 951 | 0.87× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,221 | 2,398 | 1,200 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,514 | 1,585 | 809 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,304 | 2,495 | 1,282 | 0.92× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 551 | 517 | 158 | 1.07× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,552 | 1,844 | 600 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 631 | 552 | 348 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,754 | 1,991 | 1,222 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,050 | 991 | 665 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,314 | 1,441 | 1,000 | 0.91× | Two-stage | Win |
| Twitter Small (minified) | 1,985 | 1,473 | 1,339 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 1,831 | 1,951 | 1,666 | 0.94× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,160 | 638 | 286 | 3.39× | Fused | Win |
| Double (POD) | 1,373 | 800 | 364 | 1.72× | Fused | Win |
| Int64 (POD) | 3,441 | 1,754 | 783 | 1.96× | Fused | Win |
| String (POD) | 2,788 | 1,824 | 1,974 | 1.53× | Fused | Win |
| Uint64 (POD) | 3,555 | 1,666 | 720 | 2.13× | Fused | Win |
| Apache Builds (minified) | 2,761 | 2,275 | 1,649 | 1.21× | Fused | Win |
| Canada (minified) | 1,285 | 1,155 | 802 | 1.11× | Fused | Win |
| Apache Builds (prettified) | 2,398 | 2,875 | 2,113 | 0.83× | Two-stage | Win |
| Canada (prettified) | 2,413 | 2,484 | 1,793 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,665 | 1,932 | 1,145 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 3,192 | 3,924 | 2,451 | 0.81× | Two-stage | Win |
| Discord (minified) | 3,309 | 2,362 | 1,117 | 1.40× | Fused | Win |
| Discord (prettified) | 2,165 | 3,344 | 1,607 | 0.65× | Two-stage | Win |
| Github Events (minified) | 2,495 | 2,102 | 1,128 | 1.19× | Fused | Win |
| Google Maps Response (minified) | 2,805 | 1,975 | 1,103 | 1.42× | Fused | Win |
| Github Events (prettified) | 2,125 | 2,490 | 1,359 | 0.85× | Two-stage | Win |
| Google Maps Response (prettified) | 2,962 | 3,755 | 2,254 | 0.79× | Two-stage | Win |
| Instruments (minified) | 2,873 | 2,216 | 789 | 1.30× | Fused | Win |
| Instruments (prettified) | 2,352 | 3,367 | 1,305 | 0.70× | Two-stage | Win |
| Marine IK Reverse (minified) | 879 | 824 | 611 | 1.07× | Fused | Win |
| Marine IK Reverse (prettified) | 2,725 | 2,807 | 2,246 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 924 | 826 | 608 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,746 | 2,816 | 2,178 | 0.98× | Two-stage | Win |
| Random (minified) | 1,773 | 1,454 | 1,030 | 1.22× | Fused | Win |
| Random (prettified) | 1,728 | 2,250 | 1,560 | 0.77× | Two-stage | Win |
| Twitter (minified) | 2,462 | 2,325 | 1,052 | 1.06× | Fused | Win |
| Twitter (prettified) | 1,813 | 2,721 | 1,382 | 0.67× | Two-stage | Win |
| Apache Builds Small (minified) | 3,048 | 2,505 | 1,641 | 1.22× | Fused | Win |
| Canada Small (minified) | 1,254 | 1,171 | 800 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 2,412 | 3,051 | 1,934 | 0.79× | Two-stage | Win |
| Canada Small (prettified) | 2,247 | 2,576 | 1,926 | 0.87× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,010 | 1,610 | 1,067 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 2,006 | 2,749 | 1,843 | 0.73× | Two-stage | Win |
| Discord Small (minified) | 2,707 | 2,131 | 863 | 1.27× | Fused | Win |
| Discord Small (prettified) | 1,951 | 2,845 | 1,180 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,652 | 2,265 | 1,606 | 1.17× | Fused | Win |
| Google Maps Response Small (minified) | 2,644 | 1,829 | 1,136 | 1.45× | Fused | Win |
| Github Events Small (prettified) | 2,196 | 2,750 | 1,975 | 0.80× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,764 | 3,514 | 2,238 | 0.79× | Two-stage | Win |
| Instruments Small (minified) | 2,692 | 2,001 | 698 | 1.35× | Fused | Win |
| Instruments Small (prettified) | 2,172 | 3,130 | 1,161 | 0.69× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 944 | 835 | 547 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,466 | 2,860 | 2,017 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,068 | 920 | 552 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,554 | 3,094 | 2,019 | 0.83× | Two-stage | Win |
| Random Small (minified) | 2,170 | 1,833 | 1,001 | 1.18× | Fused | Win |
| Random Small (prettified) | 1,887 | 2,793 | 1,501 | 0.68× | Two-stage | Win |
| Twitter Small (minified) | 2,730 | 2,410 | 1,048 | 1.13× | Fused | Win |
| Twitter Small (prettified) | 1,980 | 2,968 | 1,346 | 0.67× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,520 | 645 | 222 | 3.91× | Fused | Win |
| Double (POD) | 1,293 | 752 | 308 | 1.72× | Fused | Win |
| Int64 (POD) | 3,653 | 1,745 | 740 | 2.09× | Fused | Win |
| String (POD) | 2,779 | 1,738 | 1,701 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,806 | 1,730 | 726 | 2.20× | Fused | Win |
| Apache Builds (minified) | 2,540 | 2,116 | 1,837 | 1.20× | Fused | Win |
| Canada (minified) | 1,395 | 1,212 | 1,013 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 2,398 | 2,591 | 2,322 | 0.93× | Two-stage | Win |
| Canada (prettified) | 2,748 | 2,723 | 2,239 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,872 | 2,189 | 1,789 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 3,647 | 4,030 | 3,619 | 0.91× | Two-stage | Win |
| Discord (minified) | 3,205 | 2,414 | 1,688 | 1.33× | Fused | Win |
| Discord (prettified) | 2,745 | 3,323 | 2,406 | 0.83× | Two-stage | Win |
| Github Events (minified) | 2,192 | 2,255 | 1,744 | 0.97× | Two-stage | Win |
| Google Maps Response (minified) | 2,877 | 2,024 | 1,191 | 1.42× | Fused | Win |
| Github Events (prettified) | 2,204 | 2,377 | 2,056 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 3,498 | 3,668 | 2,389 | 0.95× | Two-stage | Win |
| Instruments (minified) | 3,872 | 2,541 | 1,799 | 1.52× | Fused | Win |
| Instruments (prettified) | 3,289 | 3,640 | 2,859 | 0.90× | Two-stage | Win |
| Marine IK Reverse (minified) | 939 | 790 | 644 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,730 | 2,860 | 2,379 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 977 | 823 | 640 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,746 | 2,863 | 2,306 | 0.96× | Two-stage | Win |
| Random (minified) | 1,617 | 1,484 | 1,438 | 1.09× | Fused | Win |
| Random (prettified) | 1,988 | 2,459 | 2,129 | 0.81× | Two-stage | Win |
| Twitter (minified) | 2,612 | 2,163 | 1,832 | 1.21× | Fused | Win |
| Twitter (prettified) | 2,314 | 2,724 | 2,384 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 2,872 | 2,411 | 1,984 | 1.19× | Fused | Win |
| Canada Small (minified) | 1,361 | 1,128 | 911 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 2,467 | 2,811 | 2,443 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,563 | 2,669 | 2,130 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,085 | 1,690 | 1,438 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,396 | 2,814 | 2,421 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,641 | 2,050 | 1,632 | 1.29× | Fused | Win |
| Discord Small (prettified) | 2,326 | 2,710 | 2,152 | 0.86× | Two-stage | Win |
| Github Events Small (minified) | 2,540 | 2,094 | 2,391 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,582 | 1,855 | 1,252 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 2,477 | 2,577 | 2,996 | 0.96× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,244 | 3,472 | 2,477 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,637 | 2,411 | 1,609 | 1.51× | Fused | Win |
| Instruments Small (prettified) | 3,015 | 3,530 | 2,645 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,051 | 870 | 616 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,657 | 3,021 | 2,233 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 1,176 | 936 | 614 | 1.26× | Fused | Win |
| Marine IK Small (prettified) | 2,853 | 3,182 | 2,246 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,879 | 1,821 | 1,446 | 1.03× | Fused | Win |
| Random Small (prettified) | 2,194 | 2,660 | 2,158 | 0.82× | Two-stage | Win |
| Twitter Small (minified) | 3,175 | 2,427 | 2,057 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 2,626 | 3,208 | 2,662 | 0.82× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,903 | 372 | 180 | 5.11× | Fused | Win |
| Double (POD) | 1,136 | 549 | 152 | 2.07× | Fused | Win |
| Int64 (POD) | 3,138 | 954 | 440 | 3.29× | Fused | Win |
| String (POD) | 2,300 | 948 | 973 | 2.43× | Fused | Win |
| Uint64 (POD) | 3,124 | 867 | 395 | 3.60× | Fused | Win |
| Apache Builds (minified) | 2,056 | 1,460 | 948 | 1.41× | Fused | Win |
| Canada (minified) | 1,073 | 883 | 651 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 2,073 | 1,776 | 1,209 | 1.17× | Fused | Win |
| Canada (prettified) | 1,985 | 1,869 | 1,394 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,482 | 1,559 | 1,259 | 1.59× | Fused | Win |
| CitmCatalog (prettified) | 3,384 | 2,886 | 2,376 | 1.17× | Fused | Win |
| Discord (minified) | 2,437 | 1,555 | 1,155 | 1.57× | Fused | Win |
| Discord (prettified) | 2,409 | 2,057 | 1,585 | 1.17× | Fused | Win |
| Github Events (minified) | 2,276 | 1,833 | 1,183 | 1.24× | Fused | Win |
| Google Maps Response (minified) | 2,253 | 1,312 | 792 | 1.72× | Fused | Win |
| Github Events (prettified) | 2,381 | 1,754 | 1,485 | 1.36× | Fused | Win |
| Google Maps Response (prettified) | 2,818 | 2,570 | 1,502 | 1.10× | Fused | Win |
| Instruments (minified) | 3,036 | 1,859 | 1,338 | 1.63× | Fused | Win |
| Instruments (prettified) | 3,239 | 2,640 | 2,020 | 1.23× | Fused | Win |
| Marine IK Reverse (minified) | 817 | 660 | 541 | 1.24× | Fused | Win |
| Marine IK Reverse (prettified) | 2,017 | 1,867 | 1,587 | 1.08× | Fused | Win |
| Marine IK (minified) | 800 | 642 | 527 | 1.25× | Fused | Win |
| Marine IK (prettified) | 2,201 | 1,922 | 1,689 | 1.14× | Fused | Win |
| Random (minified) | 1,540 | 1,326 | 741 | 1.16× | Fused | Win |
| Random (prettified) | 1,819 | 1,836 | 1,339 | 0.99× | Tie | Win |
| Twitter (minified) | 2,808 | 2,233 | 1,209 | 1.26× | Fused | Win |
| Twitter (prettified) | 2,737 | 2,657 | 1,542 | 1.03× | Fused | Win |
| Apache Builds Small (minified) | 2,162 | 1,885 | 1,183 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,027 | 878 | 549 | 1.17× | Fused | Win |
| Apache Builds Small (prettified) | 2,115 | 2,217 | 1,470 | 0.95× | Two-stage | Win |
| Canada Small (prettified) | 2,022 | 1,895 | 1,258 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,859 | 1,367 | 1,094 | 1.36× | Fused | Win |
| CitmCatalog Small (prettified) | 2,328 | 2,162 | 1,807 | 1.08× | Fused | Win |
| Discord Small (minified) | 2,453 | 1,744 | 1,028 | 1.41× | Fused | Win |
| Discord Small (prettified) | 2,412 | 2,187 | 1,555 | 1.10× | Fused | Win |
| Github Events Small (minified) | 2,256 | 1,716 | 1,499 | 1.31× | Fused | Win |
| Google Maps Response Small (minified) | 2,247 | 1,355 | 884 | 1.66× | Fused | Win |
| Github Events Small (prettified) | 2,350 | 2,040 | 1,849 | 1.15× | Fused | Win |
| Google Maps Response Small (prettified) | 2,933 | 2,670 | 1,653 | 1.10× | Fused | Win |
| Instruments Small (minified) | 3,155 | 1,870 | 1,299 | 1.69× | Fused | Win |
| Instruments Small (prettified) | 3,150 | 2,641 | 1,992 | 1.19× | Fused | Win |
| Marine IK Reverse Small (minified) | 839 | 699 | 457 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,101 | 2,302 | 1,597 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 937 | 748 | 460 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,277 | 2,431 | 1,602 | 0.94× | Two-stage | Win |
| Random Small (minified) | 1,657 | 1,470 | 942 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,983 | 1,892 | 1,390 | 1.05× | Fused | Win |
| Twitter Small (minified) | 2,937 | 2,256 | 1,237 | 1.30× | Fused | Win |
| Twitter Small (prettified) | 2,782 | 2,654 | 1,570 | 1.05× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,248 | 326 | 169 | 3.83× | Fused | Win |
| Double (POD) | 1,048 | 369 | 204 | 2.84× | Fused | Win |
| Int64 (POD) | 2,556 | 824 | 471 | 3.10× | Fused | Win |
| String (POD) | 2,161 | 896 | 1,082 | 2.41× | Fused | Win |
| Uint64 (POD) | 2,632 | 818 | 449 | 3.22× | Fused | Win |
| Apache Builds (minified) | 2,093 | 1,310 | 962 | 1.60× | Fused | Win |
| Canada (minified) | 839 | 761 | 557 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,841 | 1,450 | 1,176 | 1.27× | Fused | Win |
| Canada (prettified) | 1,781 | 1,701 | 1,172 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,126 | 1,545 | 814 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 3,090 | 2,883 | 1,560 | 1.07× | Fused | Win |
| Discord (minified) | 2,733 | 1,510 | 732 | 1.81× | Fused | Win |
| Discord (prettified) | 2,549 | 2,063 | 1,157 | 1.24× | Fused | Win |
| Github Events (minified) | 2,324 | 1,836 | 899 | 1.27× | Fused | Win |
| Google Maps Response (minified) | 2,079 | 1,290 | 742 | 1.61× | Fused | Win |
| Github Events (prettified) | 2,137 | 2,083 | 1,070 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,610 | 2,225 | 1,302 | 1.17× | Fused | Win |
| Instruments (minified) | 2,695 | 1,662 | 539 | 1.62× | Fused | Win |
| Instruments (prettified) | 2,878 | 2,741 | 1,161 | 1.05× | Fused | Win |
| Marine IK Reverse (minified) | 724 | 609 | 416 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,030 | 1,914 | 1,281 | 1.06× | Fused | Win |
| Marine IK (minified) | 710 | 622 | 370 | 1.14× | Fused | Win |
| Marine IK (prettified) | 2,127 | 1,960 | 1,210 (n/c) | 1.09× | Fused | Win |
| Random (minified) | 1,522 | 1,362 | 760 | 1.12× | Fused | Win |
| Random (prettified) | 2,021 | 1,792 | 1,144 | 1.13× | Fused | Win |
| Twitter (minified) | 2,716 | 2,020 | 689 | 1.34× | Fused | Win |
| Twitter (prettified) | 2,344 | 2,146 | 836 | 1.09× | Fused | Win |
| Apache Builds Small (minified) | 1,840 | 1,598 | 1,040 | 1.15× | Fused | Win |
| Canada Small (minified) | 720 | 760 | 419 | 0.95× | Two-stage | Win |
| Apache Builds Small (prettified) | 1,702 | 1,912 | 1,328 | 0.89× | Two-stage | Win |
| Canada Small (prettified) | 1,794 | 1,541 | 991 | 1.16× | Fused | Win |
| CitmCatalog Small (minified) | 1,828 | 1,331 | 703 | 1.37× | Fused | Win |
| CitmCatalog Small (prettified) | 2,083 | 2,027 | 1,230 | 1.03× | Fused | Win |
| Discord Small (minified) | 2,549 | 1,588 | 662 | 1.61× | Fused | Win |
| Discord Small (prettified) | 2,312 | 2,054 | 898 | 1.13× | Fused | Win |
| Github Events Small (minified) | 2,393 | 1,728 | 1,152 | 1.38× | Fused | Win |
| Google Maps Response Small (minified) | 2,275 | 1,299 | 829 | 1.75× | Fused | Win |
| Github Events Small (prettified) | 2,182 | 2,006 | 1,510 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,771 | 2,476 | 1,699 | 1.12× | Fused | Win |
| Instruments Small (minified) | 3,114 | 1,868 | 634 | 1.67× | Fused | Win |
| Instruments Small (prettified) | 2,975 | 2,642 | 1,053 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 827 | 675 | 358 | 1.23× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,152 | 1,925 | 1,303 | 1.12× | Fused | Win |
| Marine IK Small (minified) | 889 | 592 | 287 | 1.50× | Fused | Win |
| Marine IK Small (prettified) | 2,211 | 2,320 | 1,257 | 0.95× | Two-stage | Win |
| Random Small (minified) | 1,765 | 1,164 | 605 | 1.52× | Fused | Win |
| Random Small (prettified) | 1,701 | 1,739 | 1,208 | 0.98× | Two-stage | Win |
| Twitter Small (minified) | 2,588 | 2,175 | 829 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 2,579 | 2,511 | 1,107 | 1.03× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and 65 of the 66 minified documents, full-size and Small, with 1 tie, while the tape wins 31 of the 33 full-size prettified documents and all 33 Small prettified ones. MSVC is where the split blurs most: of its 22 minified documents the fused path wins 21 and 1 is a tie (Github Events Small), and of the 22 prettified ones the tape wins 21 and the fused path 1 (Apache Builds). On the M2 Pro the fused path wins all POD input, all 44 minified documents and 36 of the 44 prettified documents (7 to the tape, 1 tie).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.26× (String, Windows/MSVC) to 4.02× (Bool, macOS/GCC) faster than the two-stage path, and 1.44× (String, Windows/MSVC) to 5.11× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** Freshly allocated, the fused path wins 109 of the 110 minified documents, and the other is a tie: Github Events Small under MSVC (982 against 987 MB/s). Its margins run from 3% (Random on macOS/GCC, 988 against 957 MB/s) to 2.05× (Google Maps Response on macOS/Clang, 2,047 against 998 MB/s) on the full-size documents and up to 1.86× on the Small ones (Google Maps Response Small on macOS/Clang, 2,125 against 1,140 MB/s). With objects reused it wins 108 and loses 2 to the tape: minified Github Events on Linux/GCC (2,192 against 2,255, 0.97×) and Canada Small on macOS/Clang (720 against 760, 0.95×). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 64 of 66.** It wins all 22 on Linux/Clang, 21 of 22 on Linux/GCC, where the fused path wins prettified Canada by a hair (2,262 MB/s against 2,253), and 21 of 22 under MSVC, where the fused path wins prettified Apache Builds by 2%. The tape's margins run from 2% (Github Events under MSVC, 1,068 against 1,052 MB/s) to 39% (Twitter on Linux/Clang, 2,172 against 1,561) on the full-size documents and from 2% (Apache Builds Small under MSVC) to 47% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 62 of the 66 prettified x86 documents: the fused path wins Canada on Linux/GCC, and Github Events, Apache Builds Small and Discord Small under MSVC are ties.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 36, the tape wins 7 and 1 is a tie. On the 22 full-size documents the fused path wins 19 and the tape 3: Random on macOS/GCC (0.94×), and CitmCatalog and Marine IK Reverse on macOS/Clang (0.95× each). On the Small copies the tape wins Apache Builds Small, Marine IK Reverse Small and Marine IK Small on macOS/GCC (0.95×, 0.94× and 0.99×) and Twitter Small on macOS/Clang (0.93×), and Github Events Small on macOS/GCC is a tie. By compiler, the fused path wins 17 of 22 on macOS/GCC (4 to the tape, 1 tie) and 19 of 22 on macOS/Clang (3 to the tape). Reused, the fused path wins 37, the tape 6, and 1 is a tie: 18, 3 and 1 on macOS/GCC, and 19, 3 and 0 on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Github Events Small on Linux/GCC is where Jsonifier is weakest.** Its prettified form is the faster Jsonifier path's only loss to simdjson On Demand in both runs, by 8% freshly allocated (2,253 against 2,445 MB/s) and 14% reused (2,577 against 2,996), and there are no ties. We have not profiled what sets this document apart.

**MSVC slows all three by a similar median share.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 64% of its Linux/Clang throughput, the two-stage path 67% and simdjson 64%. The spread between documents is narrowest for the two-stage path (from 56% on Github Events to 77% on CitmCatalog), wider for the fused path (from 55% on Discord to 84% on Instruments) and widest for simdjson, which keeps 51% of its Linux/Clang throughput on Google Maps Response and nearly all of it on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 244 of 245 tests with whichever path is faster, ties none and loses one** (prettified Github Events Small on Linux/GCC). The fused path alone wins 237, ties 3 and loses 5: beyond that loss, it loses prettified Random, Twitter, CitmCatalog Small and Twitter Small on Linux/GCC, and ties prettified Apache Builds on Linux/GCC, Twitter Small under MSVC and Github Events Small on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 196 MB/s, against 342 MB/s for the same document in declared order, so the faster Jsonifier path is 2.87× faster than simdjson freshly allocated (2.99× reused). The prettified form shows the same gap, at 667 MB/s against 1,209, and the faster Jsonifier path is 2.72× faster (2.99× reused). The Small copies show it too, at 151 MB/s minified and 570 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document within about 15% of the declared one, sometimes faster, and the faster Jsonifier path is 1.22× to 1.70× faster than simdjson on the full-size reversed document, freshly allocated, and 1.20× to 1.74× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so do about four fifths of the prettified documents. Routing each test to its faster path would raise the record against simdjson from 237 wins, 3 ties and 5 losses to 244 wins, 0 ties and 1 loss. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,421 | 1,295 | 1.10× | Win |
| Canada (minified) | 1,000 | 823 | 1.22× | Win |
| Apache Builds (prettified) | 1,811 | 1,649 | 1.10× | Win |
| Canada (prettified) | 2,253 | 1,848 | 1.22× | Win |
| CitmCatalog (minified) | 1,830 | 1,444 | 1.27× | Win |
| CitmCatalog (prettified) | 3,868 | 3,004 | 1.29× | Win |
| Discord (minified) | 1,872 | 1,560 | 1.20× | Win |
| Discord (prettified) | 2,566 | 2,210 | 1.16× | Win |
| Github Events (minified) | 1,545 | 1,492 | 1.04× | Win |
| Google Maps Response (minified) | 1,810 | 1,354 | 1.34× | Win |
| Github Events (prettified) | 1,860 | 1,851 | 1.00× | Win |
| Google Maps Response (prettified) | 3,416 | 2,680 | 1.27× | Win |
| Instruments (minified) | 2,260 | 1,659 | 1.36× | Win |
| Instruments (prettified) | 3,290 | 2,663 | 1.24× | Win |
| Marine IK Reverse (minified) | 728 | 613 | 1.19× | Win |
| Marine IK Reverse (prettified) | 2,625 | 2,234 | 1.18× | Win |
| Marine IK (minified) | 737 | 624 | 1.18× | Win |
| Marine IK (prettified) | 2,623 | 2,258 | 1.16× | Win |
| Random (minified) | 1,103 | 1,134 | 0.97× | **Loss** |
| Random (prettified) | 1,859 | 1,800 | 1.03× | Win |
| Twitter (minified) | 1,697 | 1,501 | 1.13× | Win |
| Twitter (prettified) | 2,136 | 1,962 | 1.09× | Win |
| Apache Builds Small (minified) | 2,117 | 1,727 | 1.23× | Win |
| Canada Small (minified) | 898 | 754 | 1.19× | Win |
| Apache Builds Small (prettified) | 2,423 | 2,179 | 1.11× | Win |
| Canada Small (prettified) | 2,196 | 1,798 | 1.22× | Win |
| CitmCatalog Small (minified) | 1,463 | 1,208 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,425 | 2,016 | 1.20× | Win |
| Discord Small (minified) | 1,667 | 1,514 | 1.10× | Win |
| Discord Small (prettified) | 2,152 | 2,016 | 1.07× | Win |
| Github Events Small (minified) | 1,725 | 1,777 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,664 | 1,333 | 1.25× | Win |
| Github Events Small (prettified) | 2,253 | 2,317 | 0.97× | **Loss** |
| Google Maps Response Small (prettified) | 3,155 | 2,611 | 1.21× | Win |
| Instruments Small (minified) | 2,183 | 1,624 | 1.34× | Win |
| Instruments Small (prettified) | 3,239 | 2,652 | 1.22× | Win |
| Marine IK Reverse Small (minified) | 740 | 619 | 1.19× | Win |
| Marine IK Reverse Small (prettified) | 2,572 | 2,270 | 1.13× | Win |
| Marine IK Small (minified) | 788 | 629 | 1.25× | Win |
| Marine IK Small (prettified) | 2,649 | 2,298 | 1.15× | Win |
| Random Small (minified) | 1,496 | 1,262 | 1.19× | Win |
| Random Small (prettified) | 2,221 | 1,884 | 1.18× | Win |
| Twitter Small (minified) | 2,263 | 1,850 | 1.22× | Win |
| Twitter Small (prettified) | 2,923 | 2,337 | 1.25× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 987 | 801 | 1.23× | Win |
| Canada (minified) | 721 | 614 | 1.17× | Win |
| Apache Builds (prettified) | 1,227 | 1,022 | 1.20× | Win |
| Canada (prettified) | 1,574 | 1,313 | 1.20× | Win |
| CitmCatalog (minified) | 1,346 | 1,102 | 1.22× | Win |
| CitmCatalog (prettified) | 2,532 | 2,133 | 1.19× | Win |
| Discord (minified) | 1,223 | 1,011 | 1.21× | Win |
| Discord (prettified) | 1,688 | 1,402 | 1.20× | Win |
| Github Events (minified) | 1,398 | 1,215 | 1.15× | Win |
| Google Maps Response (minified) | 1,169 | 961 | 1.22× | Win |
| Github Events (prettified) | 1,572 | 1,521 | 1.03× | Win |
| Google Maps Response (prettified) | 2,238 | 1,481 | 1.51× | Win |
| Instruments (minified) | 1,576 | 1,298 | 1.21× | Win |
| Instruments (prettified) | 2,434 | 1,962 | 1.24× | Win |
| Marine IK Reverse (minified) | 588 | 530 (n/c) | 1.11× | Win |
| Marine IK Reverse (prettified) | 1,799 | 1,604 | 1.12× | Win |
| Marine IK (minified) | 570 | 492 | 1.16× | Win |
| Marine IK (prettified) | 1,722 | 1,802 | 0.96× | **Loss** |
| Random (minified) | 957 | 665 | 1.44× | Win |
| Random (prettified) | 1,229 | 1,154 | 1.06× | Win |
| Twitter (minified) | 1,706 | 1,357 | 1.26× | Win |
| Twitter (prettified) | 2,037 | 1,733 | 1.18× | Win |
| Apache Builds Small (minified) | 1,471 | 924 | 1.59× | Win |
| Canada Small (minified) | 723 | 600 | 1.20× | Win |
| Apache Builds Small (prettified) | 1,750 | 1,142 | 1.53× | Win |
| Canada Small (prettified) | 1,600 | 1,370 | 1.17× | Win |
| CitmCatalog Small (minified) | 913 | 880 | 1.04× | Win |
| CitmCatalog Small (prettified) | 1,655 | 1,410 | 1.17× | Win |
| Discord Small (minified) | 1,333 | 1,053 | 1.27× | Win |
| Discord Small (prettified) | 1,703 | 1,382 | 1.23× | Win |
| Github Events Small (minified) | 1,272 | 1,142 | 1.11× | Win |
| Google Maps Response Small (minified) | 1,164 | 982 | 1.19× | Win |
| Github Events Small (prettified) | 1,535 | 1,427 | 1.08× | Win |
| Google Maps Response Small (prettified) | 2,320 | 1,840 | 1.26× | Win |
| Instruments Small (minified) | 1,695 | 1,273 | 1.33× | Win |
| Instruments Small (prettified) | 2,434 | 1,970 | 1.24× | Win |
| Marine IK Reverse Small (minified) | 566 | 526 | 1.08× | Win |
| Marine IK Reverse Small (prettified) | 1,896 | 1,787 | 1.06× | Win |
| Marine IK Small (minified) | 602 | 530 | 1.14× | Win |
| Marine IK Small (prettified) | 2,002 | 1,796 | 1.11× | Win |
| Random Small (minified) | 1,137 | 839 | 1.36× | Win |
| Random Small (prettified) | 1,409 | 908 | 1.55× | Win |
| Twitter Small (minified) | 1,884 | 1,426 | 1.32× | Win |
| Twitter Small (prettified) | 2,254 | 1,794 | 1.26× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,116 | 1,644 | 1.29× | Win |
| Canada (minified) | 1,212 | 968 | 1.25× | Win |
| Apache Builds (prettified) | 2,591 | 2,083 | 1.24× | Win |
| Canada (prettified) | 2,723 | 2,153 | 1.27× | Win |
| CitmCatalog (minified) | 2,189 | 1,685 | 1.30× | Win |
| CitmCatalog (prettified) | 4,030 | 3,388 | 1.19× | Win |
| Discord (minified) | 2,414 | 1,748 | 1.38× | Win |
| Discord (prettified) | 3,323 | 2,465 | 1.35× | Win |
| Github Events (minified) | 2,255 | 1,855 | 1.22× | Win |
| Google Maps Response (minified) | 2,024 | 1,448 | 1.40× | Win |
| Github Events (prettified) | 2,377 | 2,190 | 1.09× | Win |
| Google Maps Response (prettified) | 3,668 | 2,838 | 1.29× | Win |
| Instruments (minified) | 2,541 | 1,689 | 1.50× | Win |
| Instruments (prettified) | 3,640 | 2,706 | 1.35× | Win |
| Marine IK Reverse (minified) | 790 | 681 | 1.16× | Win |
| Marine IK Reverse (prettified) | 2,860 | 2,419 | 1.18× | Win |
| Marine IK (minified) | 823 | 688 | 1.20× | Win |
| Marine IK (prettified) | 2,863 | 2,475 | 1.16× | Win |
| Random (minified) | 1,484 | 1,475 | 1.01× | Win |
| Random (prettified) | 2,459 | 2,200 | 1.12× | Win |
| Twitter (minified) | 2,163 | 1,730 | 1.25× | Win |
| Twitter (prettified) | 2,724 | 2,291 | 1.19× | Win |
| Apache Builds Small (minified) | 2,411 | 1,835 | 1.31× | Win |
| Canada Small (minified) | 1,128 | 865 | 1.30× | Win |
| Apache Builds Small (prettified) | 2,811 | 2,310 | 1.22× | Win |
| Canada Small (prettified) | 2,669 | 2,039 | 1.31× | Win |
| CitmCatalog Small (minified) | 1,690 | 1,336 | 1.26× | Win |
| CitmCatalog Small (prettified) | 2,814 | 2,238 | 1.26× | Win |
| Discord Small (minified) | 2,050 | 1,790 | 1.15× | Win |
| Discord Small (prettified) | 2,710 | 2,349 | 1.15× | Win |
| Github Events Small (minified) | 2,094 | 1,956 | 1.07× | Win |
| Google Maps Response Small (minified) | 1,855 | 1,441 | 1.29× | Win |
| Github Events Small (prettified) | 2,577 | 2,527 | 1.02× | Win |
| Google Maps Response Small (prettified) | 3,472 | 2,841 | 1.22× | Win |
| Instruments Small (minified) | 2,411 | 1,691 | 1.43× | Win |
| Instruments Small (prettified) | 3,530 | 2,717 | 1.30× | Win |
| Marine IK Reverse Small (minified) | 870 | 711 | 1.22× | Win |
| Marine IK Reverse Small (prettified) | 3,021 | 2,541 | 1.19× | Win |
| Marine IK Small (minified) | 936 | 700 | 1.34× | Win |
| Marine IK Small (prettified) | 3,182 | 2,547 | 1.25× | Win |
| Random Small (minified) | 1,821 | 1,402 | 1.30× | Win |
| Random Small (prettified) | 2,660 | 2,169 | 1.23× | Win |
| Twitter Small (minified) | 2,427 | 1,994 | 1.22× | Win |
| Twitter Small (prettified) | 3,208 | 2,467 | 1.30× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,460 | 1,002 | 1.46× | Win |
| Canada (minified) | 883 | 517 | 1.71× | Win |
| Apache Builds (prettified) | 1,776 | 1,280 | 1.39× | Win |
| Canada (prettified) | 1,869 | 1,149 | 1.63× | Win |
| CitmCatalog (minified) | 1,559 | 1,211 | 1.29× | Win |
| CitmCatalog (prettified) | 2,886 | 2,325 | 1.24× | Win |
| Discord (minified) | 1,555 | 1,147 | 1.36× | Win |
| Discord (prettified) | 2,057 | 1,571 | 1.31× | Win |
| Github Events (minified) | 1,833 | 1,445 | 1.27× | Win |
| Google Maps Response (minified) | 1,312 | 1,039 | 1.26× | Win |
| Github Events (prettified) | 1,754 | 1,668 | 1.05× | Win |
| Google Maps Response (prettified) | 2,570 | 2,005 | 1.28× | Win |
| Instruments (minified) | 1,859 | 1,131 | 1.64× | Win |
| Instruments (prettified) | 2,640 | 2,033 | 1.30× | Win |
| Marine IK Reverse (minified) | 660 | 585 | 1.13× | Win |
| Marine IK Reverse (prettified) | 1,867 | 1,740 | 1.07× | Win |
| Marine IK (minified) | 642 | 531 | 1.21× | Win |
| Marine IK (prettified) | 1,922 | 1,969 | 0.98× | **Loss** |
| Random (minified) | 1,326 | 824 | 1.61× | Win |
| Random (prettified) | 1,836 | 1,362 | 1.35× | Win |
| Twitter (minified) | 2,233 | 1,501 | 1.49× | Win |
| Twitter (prettified) | 2,657 | 1,924 | 1.38× | Win |
| Apache Builds Small (minified) | 1,885 | 1,357 | 1.39× | Win |
| Canada Small (minified) | 878 | 521 | 1.69× | Win |
| Apache Builds Small (prettified) | 2,217 | 1,453 | 1.53× | Win |
| Canada Small (prettified) | 1,895 | 1,579 | 1.20× | Win |
| CitmCatalog Small (minified) | 1,367 | 1,094 | 1.25× | Win |
| CitmCatalog Small (prettified) | 2,162 | 1,796 | 1.20× | Win |
| Discord Small (minified) | 1,744 | 1,265 | 1.38× | Win |
| Discord Small (prettified) | 2,187 | 1,663 | 1.32× | Win |
| Github Events Small (minified) | 1,716 | 1,383 | 1.24× | Win |
| Google Maps Response Small (minified) | 1,355 | 1,103 | 1.23× | Win |
| Github Events Small (prettified) | 2,040 | 1,699 | 1.20× | Win |
| Google Maps Response Small (prettified) | 2,670 | 1,788 | 1.49× | Win |
| Instruments Small (minified) | 1,870 | 1,355 | 1.38× | Win |
| Instruments Small (prettified) | 2,641 | 2,072 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 699 | 636 | 1.10× | Win |
| Marine IK Reverse Small (prettified) | 2,302 | 1,911 | 1.20× | Win |
| Marine IK Small (minified) | 748 | 620 | 1.21× | Win |
| Marine IK Small (prettified) | 2,431 | 2,117 | 1.15× | Win |
| Random Small (minified) | 1,470 | 1,001 | 1.47× | Win |
| Random Small (prettified) | 1,892 | 1,426 | 1.33× | Win |
| Twitter Small (minified) | 2,256 | 1,597 | 1.41× | Win |
| Twitter Small (prettified) | 2,654 | 1,983 | 1.34× | Win |

In the freshly allocated run, two-stage Jsonifier wins 84 of these 88 tests and loses 4, each by 3% to 4%: minified Random and minified and prettified Github Events Small on Linux/GCC (0.97× each), and prettified Marine IK on macOS/GCC (0.96×). With the fused path instead, Jsonifier beats the reflection reader on 80, ties 2 and loses 6. With objects reused, two-stage Jsonifier wins 87 and loses 1: prettified Marine IK on macOS/GCC by 2% (1,922 against 1,969 MB/s). The fused path beats the reflection reader on 85 and loses 3. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 33 of the 40 results; the widest gap is reused prettified Twitter Small on macOS/Clang, where the "jsonifier" row runs at 4,581 MB/s against the two-stage row's 3,477. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,585 | 3,724 | 1.77× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 6,875 | 4,321 | 1.59× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,132 | 3,460 | 1.19× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,539 | 4,052 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,880 | 6,633 | 1.19× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,338 | 7,990 | 1.17× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,657 | 5,616 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,579 | 6,844 | 0.96× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,825 | 6,849 | 1.14× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,793 | 8,160 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,571 | 5,596 | 1.00× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,459 | 6,638 | 0.97× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,203 | 4,111 | 1.27× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,481 | 4,710 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,391 | 3,807 | 0.89× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,349 | 4,450 | 0.98× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,588 | 3,753 | 1.22× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,990 | 4,562 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,584 | 3,312 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,190 | 3,947 | 1.06× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,428 | 3,825 | 1.68× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,594 | 4,492 | 1.69× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,465 | 3,612 | 1.24× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,009 | 4,250 | 1.18× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,984 | 6,833 | 1.17× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,513 | 8,187 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,779 | 5,723 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,747 | 6,855 | 0.98× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,938 | 6,950 | 1.14× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,939 | 8,402 | 1.06× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,687 | 5,652 | 1.01× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,589 | 6,922 | 0.95× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,311 | 4,201 | 1.26× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,614 | 4,852 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,655 | 3,982 | 0.92× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,619 | 4,581 | 1.01× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,155 | 3,945 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,054 | 4,920 | 1.03× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,907 | 3,746 | 1.04× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,477 | 4,304 | 0.81× | Loss |

Jsonifier wins 29 of the 40 results, ties 3 and loses 8, and wins by up to 1.77× (minified Twitter under MSVC, freshly allocated). Freshly allocated it wins 15, ties 1 and loses 4: prettified Twitter Small on Linux/Clang (0.96×) and Linux/GCC (0.97×), and minified and prettified Twitter Small on macOS/GCC (0.89× and 0.98×); the tie is minified Twitter Small on Linux/GCC. The reused run has 14 wins, 2 ties and 4 losses: prettified Twitter Small on Linux/Clang (0.98×), Linux/GCC (0.95×) and macOS/Clang (0.81×), and minified Twitter Small on macOS/GCC (0.92×); the ties are minified Twitter Small on Linux/GCC and prettified Twitter Small on macOS/GCC. Every full-size result is a win, by 1.06× to 1.77× on the three x86 builds and 1.03× to 1.27× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.04× to 1.36×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,585 | 2,489 | 1.04× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,480 | 2,605 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,960 | 2,170 | 1.36× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,276 | 1,915 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,040 | 1,667 | 1.22× | Win |

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

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans and skips every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, turning an O(members) parse into an O(members²) crawl over structural positions, per object, per document, forever. The model has no memory; the millionth permuted document costs exactly what the first one did.

Jsonifier's failure mode for the same situation is: one hash lookup, one dispatch-table indirection, and a learned correction. The mismatch cost is paid once per key slot per thread, not once per object instance. A feed of a billion documents with keys in reversed order parses at effectively the same throughput as a feed in declared order, because after the first document the "expected order" *is* the observed order. The schema-directed model converts key order from a per-document runtime tax into a per-stream calibration.

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `context.indentSize * depth`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (the string parse loop, whose in-register UTF-8 validation §6 covers) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra with several architectural departures.

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

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte), so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator. The position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The companion paper *Batched Drain, Fused Scan* covers this mechanism in full in its §6.1.

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes*: every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 109 of 110 minified documents and on 19 of 22 full-size prettified documents on the M2 Pro; slower on 64 of 66 prettified x86 documents, and split on the M2 Pro's Small prettified copies (see §2.2) |
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

The last row is the root of every other difference. simdjson's core API can ship one x86 binary that picks its instruction set at run time; Jsonifier deliberately does not. Its founding constraint is that apart from the data, nothing is decided at run time. The schema, the compiler, the parse options and the instruction-set tier are all known at build time, so every routing decision in this paper (tape or no tape, whitespace lane or not, validation or not, 4 blocks or 8, burst of 1 or 4) is still resolved before the first byte is read.

The tier is chosen when CMake configures the project, by a detector run on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS` (and capped with `JSONIFIER_SIMD_TIER`), and the whole library is compiled once for it, so every call is a direct, inlinable call with no per-call check. simdjson dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time. Jsonifier keeps one tier per build because per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, which GCC and Clang then reject. The cost is portability: a binary for mixed hardware is configured for the lowest tier it must support.

## 8. Conclusion

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 172 of 245 freshly allocated tests: all 25 POD-type tests, 109 of 110 minified documents and 36 of 44 prettified documents on the M2 Pro, while the two-stage path wins 64 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 244 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 84 of 88, and on partial reading it wins 29 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*