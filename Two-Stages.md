# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 108 of the 110 minified documents (the other two are ties), the tape is faster on 63 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 30 of the 44 prettified documents, the tape 8 and 6 are ties.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 12, 2026, with Jsonifier [55d73f0](https://github.com/nihilai-collective/jsonifier/commit/55d73f0), Glaze [7ef5e1a](https://github.com/stephenberry/glaze/commit/7ef5e1ab), simdjson [05da81d](https://github.com/simdjson/simdjson/commit/05da81d45) and BenchmarkSuite [0aaa9ac](https://github.com/nihilai-collective/benchmarksuite/commit/0aaa9ac) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Five freshly allocated full-parse rows contain non-converged results (three under MSVC, two on macOS/GCC), and ten reused rows do (three under MSVC, four on macOS/GCC and three on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 167 tests, 6 are statistical ties, and the two-stage path wins 72. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 28 | 0 | 21 | 46 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 38 | 4 | 7 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 45 | 2 | 2 | 49 of 49 |
| **Aggregate** | **167** | **6** | **72** | **240 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 19 / 1 / 2 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 19 / 0 / 3 |
| Small prettified documents | 0 / 0 / 11 | 1 / 0 / 21 | 13 / 5 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **243 / 0 / 2** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 27 | 2 | 20 | 46 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 5 | 2 | 45 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 | 1 | 1 | 46 of 49 |
| **Aggregate** | **171** | **8** | **66** | **235 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 2 / 9 | 1 / 0 / 21 | 21 / 0 / 1 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 1 / 0 |
| Small prettified documents | 0 / 0 / 11 | 0 / 0 / 22 | 15 / 5 / 2 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 47 / 0 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **242 / 0 / 3** |

With objects reused, across 245 tests the fused path wins 171, ties 8 and loses 66 to the two-stage path, and the x86 split holds: the tape wins 30 of the 33 full-size prettified documents (the fused path takes Canada on Linux/GCC, and Apache Builds and Github Events under MSVC are ties) and all 33 Small prettified ones, and the fused path wins all 66 minified documents. On the M2 Pro the fused path wins 43 of the 44 minified documents, with Apache Builds Small on macOS/GCC a tie, and 36 of the 44 prettified ones, with 5 ties and 3 tape wins. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins 242, ties none and loses 3.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 495 | 274 | 113 | 1.80× | Fused | Win |
| Double (POD) | 643 | 489 | 185 | 1.31× | Fused | Win |
| Int64 (POD) | 1,678 | 977 | 415 | 1.72× | Fused | Win |
| String (POD) | 935 | 828 | 740 | 1.13× | Fused | Win |
| Uint64 (POD) | 1,684 | 1,016 | 426 | 1.66× | Fused | Win |
| Apache Builds (minified) | 911 | 789 | 736 | 1.16× | Fused | Win |
| Canada (minified) | 620 | 579 | 397 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 980 | 945 | 886 | 1.04× | Fused | Win |
| Canada (prettified) | 1,217 | 1,344 | 964 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,570 | 1,267 | 546 (n/c) | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,290 | 2,639 | 1,238 | 0.87× | Two-stage | Win |
| Discord (minified) | 1,245 | 1,034 | 703 | 1.20× | Fused | Win |
| Discord (prettified) | 1,350 | 1,521 | 1,016 | 0.89× | Two-stage | Win |
| Github Events (minified) | 1,056 | 909 | 680 | 1.16× | Fused | Win |
| Google Maps Response (minified) | 1,284 | 1,174 | 509 | 1.09× | Fused | Win |
| Github Events (prettified) | 1,047 | 1,074 | 816 | 0.97× | Two-stage | Win |
| Google Maps Response (prettified) | 1,954 | 2,355 | 1,035 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,129 | 1,417 | 771 (n/c) | 1.50× | Fused | Win |
| Instruments (prettified) | 2,037 | 2,278 | 1,209 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 467 | 431 | 197 | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 1,475 | 1,533 | 665 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 481 (n/c) | 411 | 353 (n/c) | 1.17× | Fused | Win |
| Marine IK (prettified) | 1,522 | 1,582 | 1,241 | 0.96× | Two-stage | Win |
| Random (minified) | 774 | 724 | 580 | 1.07× | Fused | Win |
| Random (prettified) | 963 | 1,112 | 882 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,048 | 893 | 787 | 1.17× | Fused | Win |
| Twitter (prettified) | 1,338 | 1,432 | 1,156 | 0.93× | Two-stage | Win |
| Apache Builds Small (minified) | 1,167 | 1,069 | 740 | 1.09× | Fused | Win |
| Canada Small (minified) | 546 | 515 | 337 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,240 | 1,426 | 934 | 0.87× | Two-stage | Win |
| Canada Small (prettified) | 1,169 | 1,246 | 814 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 866 | 727 | 516 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,161 | 1,278 | 942 | 0.91× | Two-stage | Win |
| Discord Small (minified) | 1,117 | 915 | 673 | 1.22× | Fused | Win |
| Discord Small (prettified) | 1,158 | 1,234 | 920 | 0.94× | Two-stage | Win |
| Github Events Small (minified) | 995 | 925 | 633 | 1.08× | Fused | Win |
| Google Maps Response Small (minified) | 1,135 | 1,048 | 536 | 1.08× | Fused | Win |
| Github Events Small (prettified) | 1,006 | 1,222 | 801 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,905 | 2,021 | 1,092 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 2,176 | 1,464 | 787 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 1,945 | 2,272 | 1,299 | 0.86× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 401 | 378 | 153 | 1.06× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,222 | 1,415 | 567 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 423 | 381 | 297 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 1,286 | 1,417 | 1,102 | 0.91× | Two-stage | Win |
| Random Small (minified) | 893 | 780 | 558 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,122 | 1,210 | 869 | 0.93× | Two-stage | Win |
| Twitter Small (minified) | 1,629 | 1,318 | 1,316 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 1,532 | 1,724 | 1,678 | 0.89× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,965 | 623 | 282 | 3.15× | Fused | Win |
| Double (POD) | 1,318 | 761 | 346 | 1.73× | Fused | Win |
| Int64 (POD) | 3,166 | 1,670 | 790 | 1.90× | Fused | Win |
| String (POD) | 1,856 | 1,346 | 1,401 | 1.38× | Fused | Win |
| Uint64 (POD) | 3,239 | 1,615 | 719 | 2.01× | Fused | Win |
| Apache Builds (minified) | 1,692 | 1,488 | 1,227 | 1.14× | Fused | Win |
| Canada (minified) | 1,087 | 973 | 799 | 1.12× | Fused | Win |
| Apache Builds (prettified) | 1,678 | 1,890 | 1,569 | 0.89× | Two-stage | Win |
| Canada (prettified) | 2,009 | 2,149 | 1,783 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,197 | 1,660 | 1,118 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 2,891 | 3,501 | 2,385 | 0.83× | Two-stage | Win |
| Discord (minified) | 2,244 | 1,764 | 944 | 1.27× | Fused | Win |
| Discord (prettified) | 1,769 | 2,568 | 1,370 | 0.69× | Two-stage | Win |
| Github Events (minified) | 1,643 | 1,561 | 975 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,410 | 1,803 | 1,053 | 1.34× | Fused | Win |
| Github Events (prettified) | 1,556 | 1,920 | 1,173 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 2,914 | 3,423 | 2,147 | 0.85× | Two-stage | Win |
| Instruments (minified) | 2,442 | 1,953 | 747 | 1.25× | Fused | Win |
| Instruments (prettified) | 2,208 | 3,008 | 1,238 | 0.73× | Two-stage | Win |
| Marine IK Reverse (minified) | 804 | 729 | 569 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,501 | 2,610 | 2,096 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 827 | 740 | 570 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,503 | 2,638 | 2,081 | 0.95× | Two-stage | Win |
| Random (minified) | 1,259 | 1,100 | 835 | 1.14× | Fused | Win |
| Random (prettified) | 1,417 | 1,683 | 1,259 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,841 | 1,702 | 940 | 1.08× | Fused | Win |
| Twitter (prettified) | 1,576 | 2,171 | 1,244 | 0.73× | Two-stage | Win |
| Apache Builds Small (minified) | 2,455 | 2,162 | 1,392 | 1.14× | Fused | Win |
| Canada Small (minified) | 1,029 | 937 | 754 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,098 | 2,560 | 1,697 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 1,943 | 2,166 | 1,794 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,672 | 1,361 | 996 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 1,836 | 2,352 | 1,757 | 0.78× | Two-stage | Win |
| Discord Small (minified) | 1,984 | 1,705 | 773 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,596 | 2,289 | 1,050 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 1,983 | 1,790 | 1,347 | 1.11× | Fused | Win |
| Google Maps Response Small (minified) | 2,233 | 1,685 | 1,057 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,820 | 2,170 | 1,717 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,661 | 3,257 | 2,106 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,487 | 1,924 | 655 | 1.29× | Fused | Win |
| Instruments Small (prettified) | 2,155 | 2,927 | 1,094 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 791 | 700 | 508 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,145 | 2,514 | 1,849 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 874 | 762 | 507 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,228 | 2,682 | 1,879 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,701 | 1,539 | 858 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,693 | 2,330 | 1,326 | 0.73× | Two-stage | Win |
| Twitter Small (minified) | 2,515 | 2,112 | 1,035 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 1,837 | 2,671 | 1,328 | 0.69× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,359 | 606 | 224 | 3.89× | Fused | Win |
| Double (POD) | 1,194 | 734 | 302 | 1.63× | Fused | Win |
| Int64 (POD) | 3,292 | 1,647 | 718 | 2.00× | Fused | Win |
| String (POD) | 1,807 | 1,284 | 1,279 | 1.41× | Fused | Win |
| Uint64 (POD) | 3,404 | 1,607 | 711 | 2.12× | Fused | Win |
| Apache Builds (minified) | 1,640 | 1,414 | 1,351 | 1.16× | Fused | Win |
| Canada (minified) | 1,128 | 999 | 774 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,687 | 1,810 | 1,743 | 0.93× | Two-stage | Win |
| Canada (prettified) | 2,243 | 2,214 | 1,748 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,327 | 1,753 | 1,441 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 3,343 | 3,620 | 2,993 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,281 | 1,819 | 1,300 | 1.25× | Fused | Win |
| Discord (prettified) | 2,236 | 2,524 | 1,885 | 0.89× | Two-stage | Win |
| Github Events (minified) | 1,778 | 1,499 | 1,462 | 1.19× | Fused | Win |
| Google Maps Response (minified) | 2,466 | 1,792 | 1,090 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,704 | 1,854 | 1,584 | 0.92× | Two-stage | Win |
| Google Maps Response (prettified) | 3,146 | 3,341 | 2,213 | 0.94× | Two-stage | Win |
| Instruments (minified) | 3,241 | 2,199 | 1,450 | 1.47× | Fused | Win |
| Instruments (prettified) | 2,906 | 3,390 | 2,322 | 0.86× | Two-stage | Win |
| Marine IK Reverse (minified) | 850 | 741 | 518 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,515 | 2,597 | 1,947 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 862 | 741 | 522 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,488 | 2,595 | 1,907 | 0.96× | Two-stage | Win |
| Random (minified) | 1,193 | 1,065 | 968 | 1.12× | Fused | Win |
| Random (prettified) | 1,635 | 1,784 | 1,549 | 0.92× | Two-stage | Win |
| Twitter (minified) | 1,987 | 1,639 | 1,106 | 1.21× | Fused | Win |
| Twitter (prettified) | 1,914 | 2,192 | 1,456 | 0.87× | Two-stage | Win |
| Apache Builds Small (minified) | 2,430 | 2,010 | 1,648 | 1.21× | Fused | Win |
| Canada Small (minified) | 1,059 | 927 | 678 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,310 | 2,462 | 2,081 | 0.94× | Two-stage | Win |
| Canada Small (prettified) | 2,180 | 2,154 | 1,613 | 1.01× | Fused | Win |
| CitmCatalog Small (minified) | 1,721 | 1,463 | 1,251 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 2,131 | 2,375 | 2,121 | 0.90× | Two-stage | Win |
| Discord Small (minified) | 1,970 | 1,699 | 1,268 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,956 | 2,205 | 1,729 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,165 | 1,768 | 1,576 | 1.22× | Fused | Win |
| Google Maps Response Small (minified) | 2,262 | 1,700 | 1,093 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 2,013 | 2,209 | 1,942 | 0.91× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,005 | 3,121 | 2,190 | 0.96× | Two-stage | Win |
| Instruments Small (minified) | 3,331 | 2,182 | 1,341 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 2,893 | 3,289 | 2,137 | 0.88× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 862 | 767 | 499 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,302 | 2,554 | 1,782 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 946 | 760 | 484 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,411 | 2,684 | 1,846 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,586 | 1,485 | 1,055 | 1.07× | Fused | Win |
| Random Small (prettified) | 1,992 | 2,199 | 1,607 | 0.91× | Two-stage | Win |
| Twitter Small (minified) | 2,927 | 2,310 | 1,364 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,470 | 2,892 | 1,749 | 0.85× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,707 | 324 | 158 | 5.28× | Fused | Win |
| Double (POD) | 869 | 511 | 161 | 1.70× | Fused | Win |
| Int64 (POD) | 2,366 | 769 | 378 | 3.08× | Fused | Win |
| String (POD) | 1,083 | 662 | 766 | 1.64× | Fused | Win |
| Uint64 (POD) | 2,291 | 878 | 408 | 2.61× | Fused | Win |
| Apache Builds (minified) | 1,071 | 1,016 | 679 | 1.05× | Fused | Win |
| Canada (minified) | 934 | 772 | 459 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 1,396 | 1,264 | 986 | 1.10× | Fused | Win |
| Canada (prettified) | 1,610 | 1,279 | 1,083 (n/c) | 1.26× | Fused | Win |
| CitmCatalog (minified) | 2,097 | 1,417 | 926 | 1.48× | Fused | Win |
| CitmCatalog (prettified) | 2,821 | 2,480 | 2,142 | 1.14× | Fused | Win |
| Discord (minified) | 1,659 | 1,083 | 921 | 1.53× | Fused | Win |
| Discord (prettified) | 1,918 | 1,683 | 1,352 | 1.14× | Fused | Win |
| Github Events (minified) | 1,401 | 1,349 | 872 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 1,802 | 1,310 | 755 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,479 | 1,581 | 1,145 | 0.94× | Two-stage | Win |
| Google Maps Response (prettified) | 2,400 | 2,390 | 1,417 | 1.00× | Tie | Win |
| Instruments (minified) | 2,709 | 1,517 | 1,116 | 1.79× | Fused | Win |
| Instruments (prettified) | 2,329 | 2,459 | 1,858 | 0.95× | Two-stage | Win |
| Marine IK Reverse (minified) | 700 | 579 | 471 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | 2,105 | 1,859 | 1,632 | 1.13× | Fused | Win |
| Marine IK (minified) | 728 | 589 | 459 | 1.24× | Fused | Win |
| Marine IK (prettified) | 2,164 | 1,984 | 1,589 | 1.09× | Fused | Win |
| Random (minified) | 1,052 | 967 | 716 | 1.09× | Fused | Win |
| Random (prettified) | 1,391 | 1,228 | 1,068 | 1.13× | Fused | Win |
| Twitter (minified) | 2,100 | 1,703 | 1,041 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,164 | 2,045 | 1,336 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 1,328 | 1,373 | 796 | 0.97× | Two-stage | Win |
| Canada Small (minified) | 769 | 573 | 431 | 1.34× | Fused | Win |
| Apache Builds Small (prettified) | 1,602 | 1,603 | 1,021 | 1.00× | Tie | Win |
| Canada Small (prettified) | 1,476 | 1,586 | 1,112 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,326 | 1,048 | 854 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 1,613 | 1,685 | 1,426 | 0.96× | Two-stage | Win |
| Discord Small (minified) | 1,691 | 1,311 | 939 | 1.29× | Fused | Win |
| Discord Small (prettified) | 1,715 | 1,702 | 1,235 | 1.01× | Tie | Win |
| Github Events Small (minified) | 1,549 | 1,245 | 1,184 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 1,787 | 1,167 | 761 | 1.53× | Fused | Win |
| Github Events Small (prettified) | 1,638 | 1,462 | 1,105 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,315 | 1,943 | 1,356 | 1.19× | Fused | Win |
| Instruments Small (minified) | 2,638 | 1,600 | 1,082 | 1.65× | Fused | Win |
| Instruments Small (prettified) | 2,435 | 2,200 | 1,518 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 660 | 534 | 393 | 1.24× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,727 | 1,758 | 1,161 | 0.98× | Two-stage | Win |
| Marine IK Small (minified) | 657 | 558 | 343 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,826 | 1,901 | 1,241 | 0.96× | Two-stage | Win |
| Random Small (minified) | 1,211 | 844 | 656 | 1.44× | Fused | Win |
| Random Small (prettified) | 1,264 | 1,216 (n/c) | 1,043 | 1.04× | Fused | Win |
| Twitter Small (minified) | 2,269 | 1,746 | 944 | 1.30× | Fused | Win |
| Twitter Small (prettified) | 2,093 | 2,185 | 1,310 | 0.96× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,215 | 348 | 166 | 3.49× | Fused | Win |
| Double (POD) | 881 | 346 | 200 | 2.55× | Fused | Win |
| Int64 (POD) | 2,145 | 791 | 465 | 2.71× | Fused | Win |
| String (POD) | 1,294 | 686 | 823 | 1.89× | Fused | Win |
| Uint64 (POD) | 2,162 | 790 | 412 | 2.74× | Fused | Win |
| Apache Builds (minified) | 1,403 | 1,018 | 809 | 1.38× | Fused | Win |
| Canada (minified) | 771 | 700 | 485 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,410 | 1,203 | 1,131 | 1.17× | Fused | Win |
| Canada (prettified) | 1,471 | 1,307 | 1,069 | 1.13× | Fused | Win |
| CitmCatalog (minified) | 1,741 | 1,224 | 861 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 3,008 | 2,816 | 1,791 | 1.07× | Fused | Win |
| Discord (minified) | 2,502 | 1,296 | 763 | 1.93× | Fused | Win |
| Discord (prettified) | 2,393 | 1,786 | 1,114 | 1.34× | Fused | Win |
| Github Events (minified) | 1,572 | 1,517 | 886 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 2,203 | 1,218 | 818 | 1.81× | Fused | Win |
| Github Events (prettified) | 1,837 | 1,743 | 1,056 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,909 | 2,305 | 1,631 | 1.26× | Fused | Win |
| Instruments (minified) | 2,736 | 1,743 | 681 | 1.57× | Fused | Win |
| Instruments (prettified) | 2,290 | 2,028 | 1,124 | 1.13× | Fused | Win |
| Marine IK Reverse (minified) | 690 | 601 | 415 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 1,960 | 1,863 | 1,347 | 1.05× | Fused | Win |
| Marine IK (minified) | 651 | 529 | 348 | 1.23× | Fused | Win |
| Marine IK (prettified) | 1,863 | 1,753 | 1,204 | 1.06× | Fused | Win |
| Random (minified) | 1,293 | 994 | 656 | 1.30× | Fused | Win |
| Random (prettified) | 1,436 | 989 | 897 | 1.45× | Fused | Win |
| Twitter (minified) | 1,945 | 1,483 | 758 | 1.31× | Fused | Win |
| Twitter (prettified) | 2,267 | 1,771 | 902 | 1.28× | Fused | Win |
| Apache Builds Small (minified) | 1,641 | 1,355 | 933 | 1.21× | Fused | Win |
| Canada Small (minified) | 543 | 684 | 454 | 0.79× | Two-stage | Win |
| Apache Builds Small (prettified) | 1,717 | 1,632 | 1,177 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,545 | 1,515 | 1,054 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,245 | 973 | 694 | 1.28× | Fused | Win |
| CitmCatalog Small (prettified) | 1,712 | 1,572 | 1,190 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,135 | 1,260 | 647 | 1.69× | Fused | Win |
| Discord Small (prettified) | 2,103 | 1,634 | 877 | 1.29× | Fused | Win |
| Github Events Small (minified) | 1,704 | 1,320 | 1,072 | 1.29× | Fused | Win |
| Google Maps Response Small (minified) | 2,095 | 1,178 | 817 | 1.78× | Fused | Win |
| Github Events Small (prettified) | 1,819 | 1,573 | 1,350 | 1.16× | Fused | Win |
| Google Maps Response Small (prettified) | 2,801 | 2,123 | 1,626 | 1.32× | Fused | Win |
| Instruments Small (minified) | 2,468 | 1,711 | 592 | 1.44× | Fused | Win |
| Instruments Small (prettified) | 2,710 | 2,473 | 882 | 1.10× | Fused | Win |
| Marine IK Reverse Small (minified) | 564 | 515 | 318 | 1.10× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,745 | 1,739 | 1,132 | 1.00× | Tie | Win |
| Marine IK Small (minified) | 637 | 539 | 316 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,899 | 1,806 | 1,133 | 1.05× | Fused | Win |
| Random Small (minified) | 1,399 | 912 | 606 | 1.53× | Fused | Win |
| Random Small (prettified) | 1,516 | 1,267 | 944 | 1.20× | Fused | Win |
| Twitter Small (minified) | 1,695 | 1,782 | 710 | 0.95× | Two-stage | Win |
| Twitter Small (prettified) | 2,179 | 2,174 | 945 | 1.00× | Tie | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 519 | 291 | 115 | 1.79× | Fused | Win |
| Double (POD) | 754 | 556 | 208 | 1.36× | Fused | Win |
| Int64 (POD) | 2,104 | 1,107 | 434 (n/c) | 1.90× | Fused | Win |
| String (POD) | 1,748 | 1,201 | 1,055 | 1.45× | Fused | Win |
| Uint64 (POD) | 1,774 | 1,106 | 407 | 1.60× | Fused | Win |
| Apache Builds (minified) | 1,228 | 1,057 | 1,027 | 1.16× | Fused | Win |
| Canada (minified) | 757 | 665 | 442 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 1,334 | 1,337 | 1,288 | 1.00× | Tie | Win |
| Canada (prettified) | 1,424 | 1,584 | 1,104 | 0.90× | Two-stage | Win |
| CitmCatalog (minified) | 1,847 | 1,339 | 629 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 2,516 | 2,974 | 1,390 | 0.85× | Two-stage | Win |
| Discord (minified) | 1,924 | 1,486 | 907 | 1.29× | Fused | Win |
| Discord (prettified) | 1,763 | 2,086 | 1,303 | 0.84× | Two-stage | Win |
| Github Events (minified) | 1,595 | 1,312 | 883 | 1.22× | Fused | Win |
| Google Maps Response (minified) | 1,472 | 1,372 | 550 | 1.07× | Fused | Win |
| Github Events (prettified) | 1,422 | 1,470 | 1,068 | 0.97× | Tie | Win |
| Google Maps Response (prettified) | 2,364 | 2,586 | 1,163 | 0.91× | Two-stage | Win |
| Instruments (minified) | 2,695 | 1,717 | 882 | 1.57× | Fused | Win |
| Instruments (prettified) | 2,327 | 2,640 | 1,322 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 520 | 464 | 204 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 1,528 | 1,609 | 661 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 530 | 469 | 362 (n/c) | 1.13× | Fused | Win |
| Marine IK (prettified) | 1,629 | 1,698 | 1,298 | 0.96× | Two-stage | Win |
| Random (minified) | 1,060 | 972 | 730 | 1.09× | Fused | Win |
| Random (prettified) | 1,290 | 1,495 | 1,096 | 0.86× | Two-stage | Win |
| Twitter (minified) | 1,894 | 1,454 | 1,193 | 1.30× | Fused | Win |
| Twitter (prettified) | 1,830 | 2,049 | 1,520 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 1,542 | 1,468 | 1,165 | 1.05× | Fused | Win |
| Canada Small (minified) | 664 | 623 | 326 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,558 | 1,828 | 1,426 | 0.85× | Two-stage | Win |
| Canada Small (prettified) | 1,351 | 1,465 | 892 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,162 | 924 | 622 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 1,456 | 1,641 | 1,101 | 0.89× | Two-stage | Win |
| Discord Small (minified) | 1,720 | 1,288 | 964 | 1.34× | Fused | Win |
| Discord Small (prettified) | 1,566 | 1,774 | 1,283 | 0.88× | Two-stage | Win |
| Github Events Small (minified) | 1,419 | 1,322 | 832 | 1.07× | Fused | Win |
| Google Maps Response Small (minified) | 1,363 | 1,238 | 611 | 1.10× | Fused | Win |
| Github Events Small (prettified) | 1,321 | 1,631 | 1,033 | 0.81× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,197 | 2,412 | 1,236 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 2,493 | 1,565 | 869 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,040 | 2,460 | 1,446 | 0.83× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 485 | 448 | 166 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,396 | 1,644 | 634 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 545 | 469 | 334 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 1,557 | 1,744 | 1,202 (n/c) | 0.89× | Two-stage | Win |
| Random Small (minified) | 1,074 | 943 | 658 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,290 | 1,487 | 1,055 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,025 | 1,559 | 1,455 | 1.30× | Fused | Win |
| Twitter Small (prettified) | 1,805 | 2,075 | 1,850 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,188 | 652 | 285 | 3.36× | Fused | Win |
| Double (POD) | 1,433 | 799 | 353 | 1.79× | Fused | Win |
| Int64 (POD) | 3,575 | 1,817 | 801 | 1.97× | Fused | Win |
| String (POD) | 2,888 | 1,803 | 1,959 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,590 | 1,691 | 722 | 2.12× | Fused | Win |
| Apache Builds (minified) | 2,757 | 2,283 | 1,683 | 1.21× | Fused | Win |
| Canada (minified) | 1,324 | 1,170 | 956 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 2,400 | 2,844 | 2,164 | 0.84× | Two-stage | Win |
| Canada (prettified) | 2,332 | 2,539 | 2,123 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,658 | 1,975 | 1,297 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 3,236 | 4,042 | 2,720 | 0.80× | Two-stage | Win |
| Discord (minified) | 3,244 | 2,392 | 1,109 | 1.36× | Fused | Win |
| Discord (prettified) | 2,148 | 3,414 | 1,616 | 0.63× | Two-stage | Win |
| Github Events (minified) | 2,260 | 2,076 | 1,144 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,789 | 1,991 | 1,120 | 1.40× | Fused | Win |
| Github Events (prettified) | 1,970 | 2,514 | 1,378 | 0.78× | Two-stage | Win |
| Google Maps Response (prettified) | 3,148 | 3,692 | 2,274 | 0.85× | Two-stage | Win |
| Instruments (minified) | 2,877 | 2,207 | 778 | 1.30× | Fused | Win |
| Instruments (prettified) | 2,384 | 3,378 | 1,282 | 0.71× | Two-stage | Win |
| Marine IK Reverse (minified) | 901 | 805 | 626 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,675 | 2,828 | 2,289 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 928 | 832 | 630 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,747 | 2,911 | 2,297 | 0.94× | Two-stage | Win |
| Random (minified) | 1,746 | 1,486 | 1,044 | 1.17× | Fused | Win |
| Random (prettified) | 1,778 | 2,249 | 1,578 | 0.79× | Two-stage | Win |
| Twitter (minified) | 2,336 | 2,251 | 1,066 | 1.04× | Fused | Win |
| Twitter (prettified) | 1,892 | 2,817 | 1,411 | 0.67× | Two-stage | Win |
| Apache Builds Small (minified) | 2,902 | 2,483 | 1,693 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,303 | 1,173 | 873 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,321 | 3,034 | 2,033 | 0.77× | Two-stage | Win |
| Canada Small (prettified) | 2,286 | 2,635 | 2,040 | 0.87× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,996 | 1,610 | 1,114 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 2,073 | 2,704 | 1,950 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 2,695 | 2,195 | 875 | 1.23× | Fused | Win |
| Discord Small (prettified) | 1,961 | 2,852 | 1,194 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,501 | 2,212 | 1,597 | 1.13× | Fused | Win |
| Google Maps Response Small (minified) | 2,618 | 1,906 | 1,135 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 2,194 | 2,677 | 1,961 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,898 | 3,571 | 2,311 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,657 | 2,031 | 686 | 1.31× | Fused | Win |
| Instruments Small (prettified) | 2,281 | 3,085 | 1,139 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 967 | 834 | 559 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,446 | 2,941 | 2,023 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 1,071 | 916 | 557 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,561 | 3,185 | 2,037 | 0.80× | Two-stage | Win |
| Random Small (minified) | 2,128 | 1,892 | 1,003 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,945 | 2,820 | 1,543 | 0.69× | Two-stage | Win |
| Twitter Small (minified) | 2,736 | 2,361 | 1,066 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 1,955 | 3,017 | 1,407 | 0.65× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,597 | 630 | 228 | 4.12× | Fused | Win |
| Double (POD) | 1,254 | 763 | 307 | 1.64× | Fused | Win |
| Int64 (POD) | 3,717 | 1,763 | 733 | 2.11× | Fused | Win |
| String (POD) | 2,830 | 1,716 | 1,716 | 1.65× | Fused | Win |
| Uint64 (POD) | 3,730 | 1,722 | 720 | 2.17× | Fused | Win |
| Apache Builds (minified) | 2,533 | 2,082 | 1,909 | 1.22× | Fused | Win |
| Canada (minified) | 1,393 | 1,203 | 930 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,402 | 2,612 | 2,420 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,706 | 2,633 | 2,061 | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,906 | 2,122 | 1,759 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 3,805 | 4,260 | 3,553 | 0.89× | Two-stage | Win |
| Discord (minified) | 3,255 | 2,358 | 1,565 | 1.38× | Fused | Win |
| Discord (prettified) | 2,769 | 3,382 | 2,240 | 0.82× | Two-stage | Win |
| Github Events (minified) | 2,387 | 1,974 | 1,660 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 2,841 | 1,969 | 1,161 | 1.44× | Fused | Win |
| Github Events (prettified) | 2,320 | 2,373 | 1,936 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 3,460 | 3,641 | 2,343 | 0.95× | Two-stage | Win |
| Instruments (minified) | 3,861 | 2,494 | 1,556 | 1.55× | Fused | Win |
| Instruments (prettified) | 3,183 | 3,758 | 2,485 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 946 | 809 | 581 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,690 | 2,817 | 2,104 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 971 | 816 | 577 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,716 | 2,848 | 2,092 | 0.95× | Two-stage | Win |
| Random (minified) | 1,610 | 1,412 | 1,274 | 1.14× | Fused | Win |
| Random (prettified) | 2,075 | 2,328 | 1,895 | 0.89× | Two-stage | Win |
| Twitter (minified) | 2,585 | 2,063 | 1,290 | 1.25× | Fused | Win |
| Twitter (prettified) | 2,295 | 2,738 | 1,699 | 0.84× | Two-stage | Win |
| Apache Builds Small (minified) | 2,830 | 2,367 | 1,887 | 1.20× | Fused | Win |
| Canada Small (minified) | 1,351 | 1,120 | 760 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 2,556 | 2,950 | 2,271 | 0.87× | Two-stage | Win |
| Canada Small (prettified) | 2,599 | 2,622 | 1,835 | 0.99× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,059 | 1,677 | 1,417 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,419 | 2,800 | 2,447 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,626 | 2,069 | 1,572 | 1.27× | Fused | Win |
| Discord Small (prettified) | 2,335 | 2,688 | 2,069 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,624 | 2,163 | 1,800 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,682 | 1,901 | 1,203 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 2,458 | 2,632 | 2,273 | 0.93× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,273 | 3,516 | 2,391 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,686 | 2,375 | 1,423 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 3,058 | 3,526 | 2,386 | 0.87× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,054 | 883 | 539 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,631 | 3,024 | 1,991 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 1,154 | 927 | 542 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,788 | 3,186 | 2,002 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,956 | 1,829 | 1,275 | 1.07× | Fused | Win |
| Random Small (prettified) | 2,356 | 2,648 | 1,945 | 0.89× | Two-stage | Win |
| Twitter Small (minified) | 3,291 | 2,514 | 1,412 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 2,722 | 3,186 | 1,850 | 0.85× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,910 | 311 (n/c) | 188 | 6.15× | Fused | Win |
| Double (POD) | 1,063 | 485 | 169 | 2.19× | Fused | Win |
| Int64 (POD) | 2,677 | 833 | 429 | 3.22× | Fused | Win |
| String (POD) | 2,230 | 999 | 1,013 | 2.23× | Fused | Win |
| Uint64 (POD) | 2,547 | 843 | 422 | 3.02× | Fused | Win |
| Apache Builds (minified) | 1,663 | 1,501 | 986 | 1.11× | Fused | Win |
| Canada (minified) | 1,182 | 840 | 604 | 1.41× | Fused | Win |
| Apache Builds (prettified) | 2,145 | 1,853 | 1,255 | 1.16× | Fused | Win |
| Canada (prettified) | 1,703 | 1,482 (n/c) | 1,371 | 1.15× | Fused | Win |
| CitmCatalog (minified) | 2,623 | 1,469 | 1,111 (n/c) | 1.79× | Fused | Win |
| CitmCatalog (prettified) | 3,254 | 2,832 | 2,238 | 1.15× | Fused | Win |
| Discord (minified) | 2,083 | 1,459 | 1,051 | 1.43× | Fused | Win |
| Discord (prettified) | 2,366 | 2,074 | 1,534 | 1.14× | Fused | Win |
| Github Events (minified) | 2,186 | 1,777 | 1,138 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,314 | 1,257 | 810 | 1.84× | Fused | Win |
| Github Events (prettified) | 2,188 | 1,957 | 1,377 | 1.12× | Fused | Win |
| Google Maps Response (prettified) | 2,534 | 2,682 | 1,549 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,867 | 1,422 | 1,153 | 2.02× | Fused | Win |
| Instruments (prettified) | 3,003 | 2,683 | 1,966 | 1.12× | Fused | Win |
| Marine IK Reverse (minified) | 818 | 647 | 519 | 1.26× | Fused | Win |
| Marine IK Reverse (prettified) | 2,346 | 2,152 | 1,770 | 1.09× | Fused | Win |
| Marine IK (minified) | 844 | 664 | 496 | 1.27× | Fused | Win |
| Marine IK (prettified) | 2,424 | 2,187 | 1,711 | 1.11× | Fused | Win |
| Random (minified) | 1,544 | 1,394 | 889 | 1.11× | Fused | Win |
| Random (prettified) | 1,916 | 1,821 | 1,307 | 1.05× | Fused | Win |
| Twitter (minified) | 2,843 | 2,229 | 1,170 | 1.27× | Fused | Win |
| Twitter (prettified) | 2,759 | 2,497 | 1,510 | 1.11× | Fused | Win |
| Apache Builds Small (minified) | 1,769 | 1,780 | 1,003 | 0.99× | Tie | Win |
| Canada Small (minified) | 821 | 780 | 445 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,940 | 1,940 | 1,219 | 1.00× | Tie | Win |
| Canada Small (prettified) | 2,004 | 1,875 | 1,243 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,894 | 1,377 | 1,089 | 1.38× | Fused | Win |
| CitmCatalog Small (prettified) | 2,347 | 1,723 | 1,758 | 1.36× | Fused | Win |
| Discord Small (minified) | 2,419 | 1,729 | 1,174 | 1.40× | Fused | Win |
| Discord Small (prettified) | 2,410 | 1,843 | 1,532 | 1.31× | Fused | Win |
| Github Events Small (minified) | 1,954 | 1,658 | 1,494 | 1.18× | Fused | Win |
| Google Maps Response Small (minified) | 1,807 | 1,421 | 752 | 1.27× | Fused | Win |
| Github Events Small (prettified) | 2,318 | 2,026 | 1,621 (n/c) | 1.14× | Fused | Win |
| Google Maps Response Small (prettified) | 2,701 | 2,330 | 1,518 | 1.16× | Fused | Win |
| Instruments Small (minified) | 3,051 | 1,654 | 1,129 | 1.85× | Fused | Win |
| Instruments Small (prettified) | 2,451 | 2,386 | 1,488 | 1.03× | Tie | Win |
| Marine IK Reverse Small (minified) | 851 | 686 | 439 | 1.24× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,916 | 2,152 | 1,352 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 911 | 675 | 396 | 1.35× | Fused | Win |
| Marine IK Small (prettified) | 2,205 | 2,198 | 1,442 | 1.00× | Tie | Win |
| Random Small (minified) | 1,233 | 1,207 | 790 | 1.02× | Fused | Win |
| Random Small (prettified) | 1,752 | 1,784 | 1,218 | 0.98× | Tie | Win |
| Twitter Small (minified) | 2,708 | 1,989 | 1,152 | 1.36× | Fused | Win |
| Twitter Small (prettified) | 2,669 | 2,532 | 1,462 | 1.05× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,243 | 348 | 169 | 3.58× | Fused | Win |
| Double (POD) | 934 | 375 | 184 | 2.49× | Fused | Win |
| Int64 (POD) | 2,590 | 858 | 456 | 3.02× | Fused | Win |
| String (POD) | 2,280 | 886 | 1,082 | 2.57× | Fused | Win |
| Uint64 (POD) | 2,710 | 825 | 456 | 3.28× | Fused | Win |
| Apache Builds (minified) | 2,147 | 1,385 | 984 | 1.55× | Fused | Win |
| Canada (minified) | 995 | 804 | 543 | 1.24× | Fused | Win |
| Apache Builds (prettified) | 1,889 | 1,535 | 1,115 | 1.23× | Fused | Win |
| Canada (prettified) | 1,826 | 1,615 | 1,180 | 1.13× | Fused | Win |
| CitmCatalog (minified) | 2,150 | 1,417 | 921 | 1.52× | Fused | Win |
| CitmCatalog (prettified) | 3,359 | 3,206 | 1,907 | 1.05× | Fused | Win |
| Discord (minified) | 3,127 | 1,688 | 835 | 1.85× | Fused | Win |
| Discord (prettified) | 2,713 | 2,262 | 1,213 | 1.20× | Fused | Win |
| Github Events (minified) | 2,341 | 2,037 | 1,011 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 2,033 | 1,389 | 864 | 1.46× | Fused | Win |
| Github Events (prettified) | 2,431 | 2,254 | 1,212 | 1.08× | Fused | Win |
| Google Maps Response (prettified) | 3,038 | 2,583 | 1,729 | 1.18× | Fused | Win |
| Instruments (minified) | 3,222 | 1,938 | 709 | 1.66× | Fused | Win |
| Instruments (prettified) | 2,944 | 2,310 | 976 | 1.27× | Fused | Win |
| Marine IK Reverse (minified) | 798 | 680 | 452 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,197 | 1,875 | 1,481 | 1.17× | Fused | Win |
| Marine IK (minified) | 767 (n/c) | 655 | 383 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,031 | 1,909 | 1,289 | 1.06× | Fused | Win |
| Random (minified) | 1,744 | 1,354 | 751 | 1.29× | Fused | Win |
| Random (prettified) | 1,864 | 1,492 | 989 | 1.25× | Fused | Win |
| Twitter (minified) | 2,777 | 1,986 | 730 (n/c) | 1.40× | Fused | Win |
| Twitter (prettified) | 2,747 | 2,291 | 1,109 | 1.20× | Fused | Win |
| Apache Builds Small (minified) | 2,196 | 1,743 | 1,235 | 1.26× | Fused | Win |
| Canada Small (minified) | 877 | 732 | 510 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 2,155 | 2,072 | 1,451 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,828 | 1,780 | 1,180 | 1.03× | Fused | Win |
| CitmCatalog Small (minified) | 1,959 | 1,393 | 881 | 1.41× | Fused | Win |
| CitmCatalog Small (prettified) | 2,355 | 2,245 | 1,524 | 1.05× | Fused | Win |
| Discord Small (minified) | 2,766 | 1,708 | 742 | 1.62× | Fused | Win |
| Discord Small (prettified) | 2,516 | 2,149 | 995 | 1.17× | Fused | Win |
| Github Events Small (minified) | 2,441 | 1,381 | 1,055 | 1.77× | Fused | Win |
| Google Maps Response Small (minified) | 2,370 | 1,400 | 887 | 1.69× | Fused | Win |
| Github Events Small (prettified) | 2,440 | 2,082 | 1,595 | 1.17× | Fused | Win |
| Google Maps Response Small (prettified) | 2,686 | 2,342 | 1,646 | 1.15× | Fused | Win |
| Instruments Small (minified) | 2,980 | 1,872 | 642 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,890 | 2,304 | 893 | 1.25× | Fused | Win |
| Marine IK Reverse Small (minified) | 700 | 616 | 359 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,016 | 2,050 | 1,291 | 0.98× | Two-stage | Win |
| Marine IK Small (minified) | 898 | 688 | 324 (n/c) | 1.31× | Fused | Win |
| Marine IK Small (prettified) | 2,281 | 2,204 | 1,197 | 1.03× | Fused | Win |
| Random Small (minified) | 1,784 | 1,324 | 705 | 1.35× | Fused | Win |
| Random Small (prettified) | 1,967 | 1,722 | 1,044 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,188 | 2,139 | 806 | 1.02× | Fused | Win |
| Twitter Small (prettified) | 2,563 | 2,569 | 1,016 | 1.00× | Tie | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and all 66 minified documents, full-size and Small, while the tape wins 31 of the 33 full-size prettified documents and 32 of the 33 Small prettified ones. The three exceptions are prettified Apache Builds under MSVC and prettified Canada and Canada Small on Linux/GCC. On the M2 Pro the fused path wins all POD input, 41 of the 44 minified documents (3 to the tape) and 32 of the 44 prettified documents (6 to the tape, 6 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.13× (String, Windows/MSVC) to 5.28× (Bool, macOS/GCC) faster than the two-stage path, and 1.36× (Double, Windows/MSVC) to 6.15× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** Freshly allocated, the fused path wins 107 of the 110 minified documents and loses 3 to the tape, all of them Small copies on the M2 Pro: Apache Builds Small on macOS/GCC (1,328 against 1,373 MB/s, 0.97×), and Canada Small and Twitter Small on macOS/Clang (543 against 684, 0.79×, and 1,695 against 1,782, 0.95×). Its margins run from 4% (Github Events on macOS/Clang, 1,572 against 1,517 MB/s) to 1.93× (Discord on macOS/Clang, 2,502 against 1,296 MB/s) on the full-size documents and up to 1.78× on the Small ones (Google Maps Response Small on macOS/Clang, 2,095 against 1,178 MB/s). With objects reused it wins 109 and ties 1 (Apache Builds Small on macOS/GCC, 1,769 against 1,780 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 63 of 66.** It wins all 22 on Linux/Clang, 20 of 22 on Linux/GCC, where the fused path wins prettified Canada and Canada Small by a hair (2,243 MB/s against 2,214, and 2,180 against 2,154), and 21 of 22 under MSVC, where the fused path wins prettified Apache Builds by 4% (980 against 945). The tape's margins run from 3% (Github Events under MSVC, 1,074 against 1,047 MB/s) to 45% (Discord on Linux/Clang, 2,568 against 1,769) on the full-size documents and from 4% (Google Maps Response Small on Linux/GCC) to 45% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape again wins 63 of the 66 prettified x86 documents; the fused path takes prettified Canada on Linux/GCC, and prettified Apache Builds and Github Events under MSVC are ties.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 32, the tape wins 6 and 6 are ties. On the 22 full-size documents the fused path wins 19, the tape 2 and 1 is a tie: the tape takes Github Events and Instruments on macOS/GCC (0.94× and 0.95×), and Google Maps Response on macOS/GCC is a tie. On the Small copies the tape wins Canada Small, CitmCatalog Small, Marine IK Reverse Small and Marine IK Small on macOS/GCC (0.93×, 0.96×, 0.98× and 0.96×), and Apache Builds Small, Discord Small and Twitter Small on macOS/GCC and Marine IK Reverse Small and Twitter Small on macOS/Clang are ties. By compiler, the fused path wins 12 of 22 on macOS/GCC (6 to the tape, 4 ties) and 20 of 22 on macOS/Clang (2 ties). Reused, the fused path wins 36, the tape 3, and 5 are ties: 16, 2 and 4 on macOS/GCC, and 20, 1 and 1 on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**No document beats both Jsonifier paths.** The faster Jsonifier path wins every test against simdjson On Demand in both runs, and there are no ties. The fused path alone loses two freshly allocated, prettified Twitter Small under MSVC (1,532 against 1,678 MB/s, 0.91×) and prettified Apache Builds on Linux/GCC (1,687 against 1,743, 0.97×), and with objects reused it also loses prettified CitmCatalog Small on Linux/GCC (2,419 against 2,447, 0.99×). The tape wins all three, at 1,724, 1,810 and 2,800 MB/s. We have not profiled what sets these documents apart.

**MSVC slows all three, Jsonifier's two paths more than simdjson.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 58% of its Linux/Clang throughput, the two-stage path 59% and simdjson 66%. The spread between documents is narrowest for the two-stage path (from 52% on Twitter to 76% on CitmCatalog), wider for the fused path (from 53% on Google Maps Response to 87% on Instruments) and widest for simdjson, which keeps 48% of its Linux/Clang throughput on Google Maps Response and slightly more than all of it (103%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins all 245 tests with whichever path is faster.** The fused path alone wins 243, ties none and loses 2: prettified Twitter Small under MSVC and prettified Apache Builds on Linux/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 197 MB/s, against 353 MB/s for the same document in declared order, so the faster Jsonifier path is 2.38× faster than simdjson freshly allocated (2.55× reused). The prettified form shows the same gap, at 665 MB/s against 1,241, and the faster Jsonifier path is 2.30× faster (2.43× reused). The Small copies show it too, at 153 MB/s minified and 567 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document within about 20% of the declared one, sometimes faster, and the faster Jsonifier path is 1.24× to 1.66× faster than simdjson on the full-size reversed document, freshly allocated, and 1.24× to 1.76× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so do nearly three quarters of the prettified documents. Routing each test to its faster path would raise the record against simdjson from 243 wins, 0 ties and 2 losses to all 245 wins. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,414 | 1,304 | 1.08× | Win |
| Canada (minified) | 999 | 813 | 1.23× | Win |
| Apache Builds (prettified) | 1,810 | 1,644 | 1.10× | Win |
| Canada (prettified) | 2,214 | 1,820 | 1.22× | Win |
| CitmCatalog (minified) | 1,753 | 1,419 | 1.24× | Win |
| CitmCatalog (prettified) | 3,620 | 2,944 | 1.23× | Win |
| Discord (minified) | 1,819 | 1,527 | 1.19× | Win |
| Discord (prettified) | 2,524 | 2,208 | 1.14× | Win |
| Github Events (minified) | 1,499 | 1,490 | 1.01× | Win |
| Google Maps Response (minified) | 1,792 | 1,372 | 1.31× | Win |
| Github Events (prettified) | 1,854 | 1,866 | 0.99× | **Loss** |
| Google Maps Response (prettified) | 3,341 | 2,713 | 1.23× | Win |
| Instruments (minified) | 2,199 | 1,655 | 1.33× | Win |
| Instruments (prettified) | 3,390 | 2,648 | 1.28× | Win |
| Marine IK Reverse (minified) | 741 | 615 | 1.21× | Win |
| Marine IK Reverse (prettified) | 2,597 | 2,223 | 1.17× | Win |
| Marine IK (minified) | 741 | 608 | 1.22× | Win |
| Marine IK (prettified) | 2,595 | 2,204 | 1.18× | Win |
| Random (minified) | 1,065 | 1,107 | 0.96× | **Loss** |
| Random (prettified) | 1,784 | 1,764 | 1.01× | Win |
| Twitter (minified) | 1,639 | 1,522 | 1.08× | Win |
| Twitter (prettified) | 2,192 | 1,963 | 1.12× | Win |
| Apache Builds Small (minified) | 2,010 | 1,708 | 1.18× | Win |
| Canada Small (minified) | 927 | 766 | 1.21× | Win |
| Apache Builds Small (prettified) | 2,462 | 2,137 | 1.15× | Win |
| Canada Small (prettified) | 2,154 | 1,840 | 1.17× | Win |
| CitmCatalog Small (minified) | 1,463 | 1,218 | 1.20× | Win |
| CitmCatalog Small (prettified) | 2,375 | 2,078 | 1.14× | Win |
| Discord Small (minified) | 1,699 | 1,477 | 1.15× | Win |
| Discord Small (prettified) | 2,205 | 1,968 | 1.12× | Win |
| Github Events Small (minified) | 1,768 | 1,819 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,700 | 1,322 | 1.29× | Win |
| Github Events Small (prettified) | 2,209 | 2,241 | 0.99× | **Loss** |
| Google Maps Response Small (prettified) | 3,121 | 2,663 | 1.17× | Win |
| Instruments Small (minified) | 2,182 | 1,631 | 1.34× | Win |
| Instruments Small (prettified) | 3,289 | 2,599 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 767 | 607 | 1.26× | Win |
| Marine IK Reverse Small (prettified) | 2,554 | 2,292 | 1.11× | Win |
| Marine IK Small (minified) | 760 | 620 | 1.23× | Win |
| Marine IK Small (prettified) | 2,684 | 2,333 | 1.15× | Win |
| Random Small (minified) | 1,485 | 1,196 | 1.24× | Win |
| Random Small (prettified) | 2,199 | 1,870 | 1.18× | Win |
| Twitter Small (minified) | 2,310 | 1,875 | 1.23× | Win |
| Twitter Small (prettified) | 2,892 | 2,384 | 1.21× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,016 | 698 | 1.46× | Win |
| Canada (minified) | 772 | 569 | 1.36× | Win |
| Apache Builds (prettified) | 1,264 | 1,101 | 1.15× | Win |
| Canada (prettified) | 1,279 | 1,363 | 0.94× | **Loss** |
| CitmCatalog (minified) | 1,417 | 1,033 | 1.37× | Win |
| CitmCatalog (prettified) | 2,480 | 2,028 | 1.22× | Win |
| Discord (minified) | 1,083 | 1,045 | 1.04× | Win |
| Discord (prettified) | 1,683 | 1,403 | 1.20× | Win |
| Github Events (minified) | 1,349 | 1,128 | 1.20× | Win |
| Google Maps Response (minified) | 1,310 | 1,004 | 1.30× | Win |
| Github Events (prettified) | 1,581 | 1,355 | 1.17× | Win |
| Google Maps Response (prettified) | 2,390 | 1,850 | 1.29× | Win |
| Instruments (minified) | 1,517 | 1,311 | 1.16× | Win |
| Instruments (prettified) | 2,459 | 1,994 | 1.23× | Win |
| Marine IK Reverse (minified) | 579 | 523 | 1.11× | Win |
| Marine IK Reverse (prettified) | 1,859 | 1,813 | 1.03× | Win |
| Marine IK (minified) | 589 | 523 | 1.13× | Win |
| Marine IK (prettified) | 1,984 | 1,525 | 1.30× | Win |
| Random (minified) | 967 | 773 | 1.25× | Win |
| Random (prettified) | 1,228 | 1,159 | 1.06× | Win |
| Twitter (minified) | 1,703 | 1,317 | 1.29× | Win |
| Twitter (prettified) | 2,045 | 1,673 | 1.22× | Win |
| Apache Builds Small (minified) | 1,373 | 918 | 1.50× | Win |
| Canada Small (minified) | 573 | 478 | 1.20× | Win |
| Apache Builds Small (prettified) | 1,603 | 1,063 | 1.51× | Win |
| Canada Small (prettified) | 1,586 | 1,362 | 1.16× | Win |
| CitmCatalog Small (minified) | 1,048 | 878 | 1.19× | Win |
| CitmCatalog Small (prettified) | 1,685 | 1,467 | 1.15× | Win |
| Discord Small (minified) | 1,311 | 1,082 | 1.21× | Win |
| Discord Small (prettified) | 1,702 | 1,414 | 1.20× | Win |
| Github Events Small (minified) | 1,245 | 1,127 | 1.10× | Win |
| Google Maps Response Small (minified) | 1,167 | 790 | 1.48× | Win |
| Github Events Small (prettified) | 1,462 | 1,234 | 1.18× | Win |
| Google Maps Response Small (prettified) | 1,943 | 1,645 | 1.18× | Win |
| Instruments Small (minified) | 1,600 | 1,031 | 1.55× | Win |
| Instruments Small (prettified) | 2,200 | 1,564 | 1.41× | Win |
| Marine IK Reverse Small (minified) | 534 | 526 | 1.02× | Win |
| Marine IK Reverse Small (prettified) | 1,758 | 1,507 | 1.17× | Win |
| Marine IK Small (minified) | 558 | 479 | 1.17× | Win |
| Marine IK Small (prettified) | 1,901 | 1,674 | 1.14× | Win |
| Random Small (minified) | 844 | 655 | 1.29× | Win |
| Random Small (prettified) | 1,216 (n/c) | 1,267 | 0.96× | **Loss** |
| Twitter Small (minified) | 1,746 | 1,304 | 1.34× | Win |
| Twitter Small (prettified) | 2,185 | 1,358 | 1.61× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,082 | 1,668 | 1.25× | Win |
| Canada (minified) | 1,203 | 949 | 1.27× | Win |
| Apache Builds (prettified) | 2,612 | 2,110 | 1.24× | Win |
| Canada (prettified) | 2,633 | 2,137 | 1.23× | Win |
| CitmCatalog (minified) | 2,122 | 1,682 | 1.26× | Win |
| CitmCatalog (prettified) | 4,260 | 3,416 | 1.25× | Win |
| Discord (minified) | 2,358 | 1,743 | 1.35× | Win |
| Discord (prettified) | 3,382 | 2,501 | 1.35× | Win |
| Github Events (minified) | 1,974 | 1,846 | 1.07× | Win |
| Google Maps Response (minified) | 1,969 | 1,451 | 1.36× | Win |
| Github Events (prettified) | 2,373 | 2,182 | 1.09× | Win |
| Google Maps Response (prettified) | 3,641 | 2,920 | 1.25× | Win |
| Instruments (minified) | 2,494 | 1,703 | 1.46× | Win |
| Instruments (prettified) | 3,758 | 2,699 | 1.39× | Win |
| Marine IK Reverse (minified) | 809 | 678 | 1.19× | Win |
| Marine IK Reverse (prettified) | 2,817 | 2,388 | 1.18× | Win |
| Marine IK (minified) | 816 | 680 | 1.20× | Win |
| Marine IK (prettified) | 2,848 | 2,434 | 1.17× | Win |
| Random (minified) | 1,412 | 1,440 | 0.98× | **Loss** |
| Random (prettified) | 2,328 | 2,132 | 1.09× | Win |
| Twitter (minified) | 2,063 | 1,765 | 1.17× | Win |
| Twitter (prettified) | 2,738 | 2,284 | 1.20× | Win |
| Apache Builds Small (minified) | 2,367 | 1,806 | 1.31× | Win |
| Canada Small (minified) | 1,120 | 898 | 1.25× | Win |
| Apache Builds Small (prettified) | 2,950 | 2,224 | 1.33× | Win |
| Canada Small (prettified) | 2,622 | 2,112 | 1.24× | Win |
| CitmCatalog Small (minified) | 1,677 | 1,347 | 1.25× | Win |
| CitmCatalog Small (prettified) | 2,800 | 2,313 | 1.21× | Win |
| Discord Small (minified) | 2,069 | 1,768 | 1.17× | Win |
| Discord Small (prettified) | 2,688 | 2,368 | 1.14× | Win |
| Github Events Small (minified) | 2,163 | 1,995 | 1.08× | Win |
| Google Maps Response Small (minified) | 1,901 | 1,461 | 1.30× | Win |
| Github Events Small (prettified) | 2,632 | 2,499 | 1.05× | Win |
| Google Maps Response Small (prettified) | 3,516 | 2,948 | 1.19× | Win |
| Instruments Small (minified) | 2,375 | 1,704 | 1.39× | Win |
| Instruments Small (prettified) | 3,526 | 2,688 | 1.31× | Win |
| Marine IK Reverse Small (minified) | 883 | 703 | 1.26× | Win |
| Marine IK Reverse Small (prettified) | 3,024 | 2,515 | 1.20× | Win |
| Marine IK Small (minified) | 927 | 710 | 1.30× | Win |
| Marine IK Small (prettified) | 3,186 | 2,603 | 1.22× | Win |
| Random Small (minified) | 1,829 | 1,369 | 1.34× | Win |
| Random Small (prettified) | 2,648 | 2,076 | 1.28× | Win |
| Twitter Small (minified) | 2,514 | 2,071 | 1.21× | Win |
| Twitter Small (prettified) | 3,186 | 2,584 | 1.23× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,501 | 1,034 | 1.45× | Win |
| Canada (minified) | 840 | 638 | 1.32× | Win |
| Apache Builds (prettified) | 1,853 | 1,324 | 1.40× | Win |
| Canada (prettified) | 1,482 (n/c) | 1,652 | 0.90× | **Loss** |
| CitmCatalog (minified) | 1,469 | 1,075 | 1.37× | Win |
| CitmCatalog (prettified) | 2,832 | 2,223 | 1.27× | Win |
| Discord (minified) | 1,459 | 1,042 | 1.40× | Win |
| Discord (prettified) | 2,074 | 1,561 | 1.33× | Win |
| Github Events (minified) | 1,777 | 1,412 | 1.26× | Win |
| Google Maps Response (minified) | 1,257 | 1,078 | 1.17× | Win |
| Github Events (prettified) | 1,957 | 1,595 | 1.23× | Win |
| Google Maps Response (prettified) | 2,682 | 1,855 | 1.45× | Win |
| Instruments (minified) | 1,422 | 1,362 | 1.04× | Win |
| Instruments (prettified) | 2,683 | 2,056 | 1.31× | Win |
| Marine IK Reverse (minified) | 647 | 586 | 1.10× | Win |
| Marine IK Reverse (prettified) | 2,152 | 1,978 | 1.09× | Win |
| Marine IK (minified) | 664 | 577 | 1.15× | Win |
| Marine IK (prettified) | 2,187 | 1,938 | 1.13× | Win |
| Random (minified) | 1,394 | 933 | 1.49× | Win |
| Random (prettified) | 1,821 | 1,365 | 1.33× | Win |
| Twitter (minified) | 2,229 | 1,447 | 1.54× | Win |
| Twitter (prettified) | 2,497 | 1,831 | 1.36× | Win |
| Apache Builds Small (minified) | 1,780 | 1,118 | 1.59× | Win |
| Canada Small (minified) | 780 | 600 | 1.30× | Win |
| Apache Builds Small (prettified) | 1,940 | 1,663 | 1.17× | Win |
| Canada Small (prettified) | 1,875 | 1,579 | 1.19× | Win |
| CitmCatalog Small (minified) | 1,377 | 1,084 | 1.27× | Win |
| CitmCatalog Small (prettified) | 1,723 | 1,787 | 0.96× | **Loss** |
| Discord Small (minified) | 1,729 | 1,313 | 1.32× | Win |
| Discord Small (prettified) | 1,843 | 1,650 | 1.12× | Win |
| Github Events Small (minified) | 1,658 | 1,069 | 1.55× | Win |
| Google Maps Response Small (minified) | 1,421 | 965 | 1.47× | Win |
| Github Events Small (prettified) | 2,026 | 1,430 | 1.42× | Win |
| Google Maps Response Small (prettified) | 2,330 | 1,839 | 1.27× | Win |
| Instruments Small (minified) | 1,654 | 1,263 | 1.31× | Win |
| Instruments Small (prettified) | 2,386 | 1,910 | 1.25× | Win |
| Marine IK Reverse Small (minified) | 686 | 633 | 1.08× | Win |
| Marine IK Reverse Small (prettified) | 2,152 | 1,964 | 1.10× | Win |
| Marine IK Small (minified) | 675 | 625 | 1.08× | Win |
| Marine IK Small (prettified) | 2,198 | 1,994 | 1.10× | Win |
| Random Small (minified) | 1,207 | 792 | 1.53× | Win |
| Random Small (prettified) | 1,784 | 1,281 | 1.39× | Win |
| Twitter Small (minified) | 1,989 | 1,323 | 1.50× | Win |
| Twitter Small (prettified) | 2,532 | 1,638 | 1.55× | Win |

In the freshly allocated run, two-stage Jsonifier wins 82 of these 88 tests and loses 6, by 1% to 6%: prettified Github Events, minified Random, and minified and prettified Github Events Small on Linux/GCC (0.99×, 0.96×, 0.97× and 0.99×), and prettified Canada and Random Small on macOS/GCC (0.94× and 0.96×). With the fused path instead, Jsonifier beats the reflection reader on 82, ties 1 and loses 5. With objects reused, two-stage Jsonifier wins 85 and loses 3: minified Random on Linux/GCC (1,412 against 1,440 MB/s), and prettified Canada (1,482 against 1,652) and CitmCatalog Small (1,723 against 1,787) on macOS/GCC. The fused path beats the reflection reader on 83, ties 2 and loses 3. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a 1.01× row can be a win: freshly allocated minified Github Events on Linux/GCC (1,499 against 1,490 MB/s) has RSEs of 0.10% and 0.08%, tight enough for the test to separate the two.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 34 of the 40 results; the widest gap is reused prettified Twitter Small on macOS/Clang, where the "jsonifier" row runs at 3,198 MB/s against the two-stage row's 4,051. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 6,890 | 5,575 | 1.24× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,862 | 6,574 | 1.20× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,138 | 4,536 | 1.13× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,053 | 5,385 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,153 | 6,924 | 1.18× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,271 | 8,221 | 1.13× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,634 | 5,469 | 1.03× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,680 | 6,553 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,060 | 6,540 | 1.23× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,807 | 7,738 | 1.14× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,777 | 5,747 | 1.01× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,805 | 6,524 | 1.04× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,187 | 4,043 | 1.28× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,465 | 4,677 | 1.17× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,599 | 3,433 | 1.05× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,972 | 4,071 | 0.98× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,348 | 3,751 | 1.16× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,836 | 4,290 | 1.13× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,517 | 2,737 (n/c) | 1.29× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,057 | 3,475 | 1.17× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 7,196 | 5,870 | 1.23× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 8,328 | 6,884 | 1.21× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,594 | 4,810 | 1.16× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,412 | 5,747 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,192 | 6,911 | 1.19× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,381 | 8,347 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,520 | 5,582 | 0.99× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,735 | 6,735 | 1.00× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,939 | 6,650 | 1.19× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,960 | 7,969 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,899 | 5,652 | 1.04× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,853 | 6,418 | 1.07× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,284 | 4,143 | 1.28× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,549 | 4,783 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,832 | 3,547 | 1.08× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,174 | 4,262 | 0.98× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 5,118 | 3,975 | 1.29× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,843 | 4,467 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,965 | 3,540 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,051 | 3,675 | 1.10× | Win |

Jsonifier wins 35 of the 40 results, ties 4 and loses 1, and wins by up to 1.29× (minified Twitter on macOS/Clang, reused). Freshly allocated it wins 18 and ties 2: minified Twitter Small on Linux/GCC (1.01×) and prettified Twitter Small on macOS/GCC (0.98×). The reused run has 17 wins, 2 ties and 1 loss: the loss is minified Twitter Small on Linux/Clang (0.99×), and the ties are prettified Twitter Small on Linux/Clang (1.00×) and on macOS/GCC (0.98×). Every full-size result is a win, by 1.12× to 1.24× on the three x86 builds and 1.08× to 1.29× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.02× to 1.36×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 2,904 | 2,312 | 1.26× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,443 | 2,661 | 1.29× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,938 | 2,160 | 1.36× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,864 | 1,834 | 1.02× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,427 | 1,833 | 1.32× | Win |

The macOS/GCC win is narrow (1.02×, 1,864 against 1,834 MB/s), but with RSEs of 0.30% and 0.39% Welch's test separates the two.

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

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans and skips every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, so in the worst case an O(members) parse becomes an O(members²) crawl over structural positions, per object, per document. That is a bound, not a measurement: in this sweep reversed key order costs simdjson about half its throughput on minified Marine IK Reverse under MSVC, but on the other four builds it stays within about 20% of declared order (§2.4). The model has no memory; the millionth permuted document costs exactly what the first one did.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 107 of 110 minified documents, and on the M2 Pro on 19 of 22 full-size and 13 of 22 Small prettified documents; slower on 63 of 66 prettified x86 documents (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 167 of 245 freshly allocated tests: all 25 POD-type tests, 107 of 110 minified documents and 32 of 44 prettified documents on the M2 Pro, while the two-stage path wins 63 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on all 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 82 of 88, and on partial reading it wins 35 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*