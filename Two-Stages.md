# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 107 of the 110 minified documents, the tape is faster on 62 of the 66 prettified documents on x86, and on the Apple M2 Pro the prettified documents split by compiler: the fused path wins 17 of 22 under Clang, while under GCC the tape wins 10 and 6 are ties.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 2, 2026, with Jsonifier [2773d05](https://github.com/nihilai-collective/jsonifier/commit/2773d05), Glaze [03a1bab](https://github.com/stephenberry/glaze/commit/03a1bab), simdjson [171066d](https://github.com/simdjson/simdjson/commit/171066d) and BenchmarkSuite [79cbad3](https://github.com/nihilai-collective/benchmarksuite/commit/79cbad3) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Every x86 result converged. Six freshly allocated full-parse rows contain one non-converged result each (three on macOS/GCC, three on macOS/Clang), and two reused rows contain one each (one on each macOS build). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 158 tests, 9 are statistical ties, and the two-stage path wins 78. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 25 | 1 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 32 | 6 | 11 | 46 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 44 | 1 | 4 | 46 of 49 |
| **Aggregate** | **158** | **9** | **78** | **239 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 20 / 1 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 0 / 1 / 21 | 11 / 4 / 7 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 12 / 3 / 7 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 45 / 1 / 3 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 44 / 1 / 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 47 / 1 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 / 1 / 1 | 45 / 0 / 4 |
| **Aggregate** | **238 / 2 / 5** | **222 / 2 / 21** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 2 | 20 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 37 | 2 | 10 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 42 | 2 | 5 | 48 of 49 |
| **Aggregate** | **162** | **6** | **77** | **243 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 0 / 2 / 20 | 13 / 3 / 6 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 12 / 1 / 9 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 48 / 0 / 1 | 37 / 0 / 12 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 45 / 1 / 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 48 / 0 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 48 / 0 / 1 |
| **Aggregate** | **243 / 0 / 2** | **227 / 1 / 17** |

Across 245 tests the fused path wins 162, ties 6 and loses 77 to the two-stage path, and the x86 split holds: the tape wins 30 of the 33 full-size prettified documents (the fused path takes Apache Builds under MSVC, and Canada and Marine IK Reverse are ties on Linux/Clang) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 32 of the 33 full-size minified ones (the tape takes Canada on Linux/GCC, 1,250 against 1,183 MB/s). On the M2 Pro the fused path wins all 44 minified documents and 25 of the 44 prettified ones, with 4 ties and 15 tape wins. Against simdjson, the faster Jsonifier path wins 243, ties none and loses 2, and the fused path alone wins 227, ties 1 and loses 17.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 494 | 252 | 91 | 1.96× | Fused | Win |
| Double (POD) | 644 | 476 | 146 | 1.35× | Fused | Win |
| Int64 (POD) | 1,571 | 844 | 315 | 1.86× | Fused | Win |
| String (POD) | 963 | 761 | 543 | 1.26× | Fused | Win |
| Uint64 (POD) | 1,442 | 878 | 314 | 1.64× | Fused | Win |
| Apache Builds (minified) | 916 | 798 | 680 | 1.15× | Fused | Win |
| Canada (minified) | 712 | 675 | 439 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,077 | 1,040 | 882 | 1.04× | Fused | Win |
| Canada (prettified) | 1,432 | 1,540 | 967 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,809 | 1,278 | 570 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 2,465 | 2,727 | 1,196 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,187 | 1,045 | 641 | 1.14× | Fused | Win |
| Discord (prettified) | 1,325 | 1,492 | 922 | 0.89× | Two-stage | Win |
| Github Events (minified) | 1,045 | 911 | 585 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,615 | 1,211 | 536 | 1.33× | Fused | Win |
| Github Events (prettified) | 1,057 | 1,087 | 702 | 0.97× | Two-stage | Win |
| Google Maps Response (prettified) | 2,179 | 2,317 | 1,071 | 0.94× | Two-stage | Win |
| Instruments (minified) | 1,998 | 1,487 | 714 | 1.34× | Fused | Win |
| Instruments (prettified) | 1,977 | 2,361 | 1,142 | 0.84× | Two-stage | Win |
| Marine IK Reverse (minified) | 610 | 534 | 190 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 1,891 | 1,951 | 649 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 616 | 542 | 346 | 1.14× | Fused | Win |
| Marine IK (prettified) | 1,891 | 1,954 | 1,237 | 0.97× | Two-stage | Win |
| Random (minified) | 789 | 747 | 549 | 1.06× | Fused | Win |
| Random (prettified) | 1,025 | 1,131 | 825 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,248 | 1,115 | 831 | 1.12× | Fused | Win |
| Twitter (prettified) | 1,279 | 1,441 | 1,056 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 1,217 | 918 | 739 | 1.33× | Fused | Win |
| Canada Small (minified) | 650 | 610 | 353 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,261 | 1,178 | 928 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,314 | 1,435 | 837 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 870 | 727 | 492 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 1,128 | 1,288 | 853 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,081 | 904 | 658 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,126 | 1,196 | 883 | 0.94× | Two-stage | Win |
| Github Events Small (minified) | 1,053 | 778 | 554 | 1.35× | Fused | Win |
| Google Maps Response Small (minified) | 1,503 | 1,103 | 537 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 1,093 | 979 | 698 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 1,966 | 2,155 | 1,059 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 1,928 | 1,419 | 699 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 1,811 | 2,265 | 1,122 | 0.80× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 492 | 432 | 143 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,426 | 1,607 | 538 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 517 | 458 | 300 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,488 | 1,685 | 1,059 | 0.88× | Two-stage | Win |
| Random Small (minified) | 917 | 824 | 529 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,175 | 1,228 | 807 | 0.96× | Two-stage | Win |
| Twitter Small (minified) | 1,384 | 1,189 | 1,089 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 1,334 | 1,513 | 1,389 | 0.88× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,965 | 635 | 277 | 3.10× | Fused | Win |
| Double (POD) | 1,237 | 743 | 360 | 1.66× | Fused | Win |
| Int64 (POD) | 3,109 | 1,618 | 787 | 1.92× | Fused | Win |
| String (POD) | 1,770 | 1,298 | 1,321 | 1.36× | Fused | Win |
| Uint64 (POD) | 3,200 | 1,618 | 769 | 1.98× | Fused | Win |
| Apache Builds (minified) | 1,725 | 1,530 | 1,233 | 1.13× | Fused | Win |
| Canada (minified) | 1,043 | 977 | 680 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,743 | 1,883 | 1,575 | 0.93× | Two-stage | Win |
| Canada (prettified) | 2,027 | 2,073 | 1,498 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 2,290 | 1,782 | 1,195 | 1.28× | Fused | Win |
| CitmCatalog (prettified) | 2,916 | 3,627 | 2,532 | 0.80× | Two-stage | Win |
| Discord (minified) | 2,258 | 1,783 | 1,407 | 1.27× | Fused | Win |
| Discord (prettified) | 1,843 | 2,578 | 2,078 | 0.71× | Two-stage | Win |
| Github Events (minified) | 1,667 | 1,591 | 1,540 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,591 | 1,974 | 1,322 | 1.31× | Fused | Win |
| Github Events (prettified) | 1,564 | 1,857 | 1,861 | 0.84× | Two-stage | Tie |
| Google Maps Response (prettified) | 2,865 | 3,572 | 2,645 | 0.80× | Two-stage | Win |
| Instruments (minified) | 2,558 | 1,952 | 1,705 | 1.31× | Fused | Win |
| Instruments (prettified) | 2,246 | 3,056 | 2,737 | 0.73× | Two-stage | Win |
| Marine IK Reverse (minified) | 821 | 747 | 575 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,502 | 2,495 | 2,119 | 1.00× | Tie | Win |
| Marine IK (minified) | 852 | 767 | 590 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,533 | 2,566 | 2,148 | 0.99× | Two-stage | Win |
| Random (minified) | 1,272 | 1,104 | 1,078 | 1.15× | Fused | Win |
| Random (prettified) | 1,387 | 1,676 | 1,605 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,935 | 1,732 | 1,730 | 1.12× | Fused | Win |
| Twitter (prettified) | 1,732 | 2,229 | 2,247 | 0.78× | Two-stage | Loss |
| Apache Builds Small (minified) | 2,615 | 2,227 | 1,519 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,008 | 972 | 713 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 2,219 | 2,686 | 1,917 | 0.83× | Two-stage | Win |
| Canada Small (prettified) | 1,956 | 2,160 | 1,687 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,770 | 1,468 | 1,199 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,910 | 2,481 | 2,036 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 2,032 | 1,777 | 1,491 | 1.14× | Fused | Win |
| Discord Small (prettified) | 1,751 | 2,299 | 2,040 | 0.76× | Two-stage | Win |
| Github Events Small (minified) | 2,148 | 1,921 | 1,952 | 1.12× | Fused | Win |
| Google Maps Response Small (minified) | 2,392 | 1,821 | 1,356 | 1.31× | Fused | Win |
| Github Events Small (prettified) | 1,916 | 2,262 | 2,410 | 0.85× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,701 | 3,440 | 2,654 | 0.79× | Two-stage | Win |
| Instruments Small (minified) | 2,599 | 1,945 | 1,610 | 1.34× | Fused | Win |
| Instruments Small (prettified) | 2,244 | 3,134 | 2,614 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 852 | 762 | 587 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,223 | 2,616 | 2,133 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 947 | 819 | 593 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,333 | 2,797 | 2,173 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,775 | 1,582 | 1,144 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,713 | 2,382 | 1,759 | 0.72× | Two-stage | Win |
| Twitter Small (minified) | 2,591 | 2,156 | 2,276 | 1.20× | Fused | Win |
| Twitter Small (prettified) | 2,049 | 2,675 | 2,798 | 0.77× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,223 | 556 | 246 | 4.00× | Fused | Win |
| Double (POD) | 1,219 | 628 | 291 | 1.94× | Fused | Win |
| Int64 (POD) | 3,308 | 1,434 | 714 | 2.31× | Fused | Win |
| String (POD) | 1,781 | 1,173 | 1,283 | 1.52× | Fused | Win |
| Uint64 (POD) | 3,428 | 1,436 | 704 | 2.39× | Fused | Win |
| Apache Builds (minified) | 1,647 | 1,472 | 1,367 | 1.12× | Fused | Win |
| Canada (minified) | 990 | 1,016 | 853 | 0.97× | Two-stage | Win |
| Apache Builds (prettified) | 1,690 | 1,842 | 1,715 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,098 | 2,255 | 1,914 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,423 | 1,844 | 1,363 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 3,371 | 3,648 | 2,833 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,269 | 1,839 | 1,429 | 1.23× | Fused | Win |
| Discord (prettified) | 2,291 | 2,558 | 2,018 | 0.90× | Two-stage | Win |
| Github Events (minified) | 1,630 | 1,537 | 1,410 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 2,637 | 1,923 | 1,081 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,578 | 2,047 | 1,705 | 0.77× | Two-stage | Win |
| Google Maps Response (prettified) | 3,454 | 3,546 | 2,169 | 0.97× | Two-stage | Win |
| Instruments (minified) | 3,284 | 2,265 | 1,392 | 1.45× | Fused | Win |
| Instruments (prettified) | 3,019 | 3,383 | 2,242 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 736 | 720 | 603 | 1.02× | Fused | Win |
| Marine IK Reverse (prettified) | 2,383 | 2,584 | 2,184 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 737 | 739 | 598 | 1.00× | Tie | Win |
| Marine IK (prettified) | 2,474 | 2,543 | 2,102 | 0.97× | Two-stage | Win |
| Random (minified) | 1,135 | 1,114 | 1,022 | 1.02× | Fused | Win |
| Random (prettified) | 1,366 | 1,646 | 1,554 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,877 | 1,630 | 1,354 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,759 | 2,169 | 1,766 | 0.81× | Two-stage | Win |
| Apache Builds Small (minified) | 2,457 | 2,123 | 1,640 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,054 | 965 | 772 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,271 | 2,535 | 1,993 | 0.90× | Two-stage | Win |
| Canada Small (prettified) | 2,077 | 2,287 | 1,794 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,807 | 1,471 | 1,147 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,101 | 2,448 | 1,971 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,052 | 1,706 | 1,278 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,936 | 2,193 | 1,709 | 0.88× | Two-stage | Win |
| Github Events Small (minified) | 2,108 | 1,783 | 1,870 | 1.18× | Fused | Win |
| Google Maps Response Small (minified) | 2,403 | 1,791 | 1,100 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 2,012 | 2,236 | 2,345 | 0.90× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,102 | 3,349 | 2,202 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,287 | 2,201 | 1,249 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,855 | 3,366 | 2,035 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 823 | 740 | 562 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,223 | 2,612 | 2,083 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 925 | 772 | 586 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,419 | 2,701 | 2,110 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,669 | 1,564 | 1,143 | 1.07× | Fused | Win |
| Random Small (prettified) | 1,854 | 2,319 | 1,738 | 0.80× | Two-stage | Win |
| Twitter Small (minified) | 2,874 | 2,311 | 1,702 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,269 | 2,886 | 2,163 | 0.79× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,627 | 418 | 153 | 3.89× | Fused | Win |
| Double (POD) | 977 | 438 | 155 | 2.23× | Fused | Win |
| Int64 (POD) | 2,523 | 819 | 416 | 3.08× | Fused | Win |
| String (POD) | 1,240 | 759 | 731 | 1.63× | Fused | Win |
| Uint64 (POD) | 2,116 | 865 | 392 | 2.45× | Fused | Win |
| Apache Builds (minified) | 1,168 | 875 | 689 | 1.33× | Fused | Win |
| Canada (minified) | 791 | 701 | 510 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,125 | 1,114 | 925 | 1.01× | Tie | Win |
| Canada (prettified) | 1,319 | 1,340 | 1,265 | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,945 | 1,138 | 989 | 1.71× | Fused | Win |
| CitmCatalog (prettified) | 2,802 | 2,196 | 1,943 | 1.28× | Fused | Win |
| Discord (minified) | 1,563 | 929 | 856 | 1.68× | Fused | Win |
| Discord (prettified) | 1,759 | 1,673 | 1,368 | 1.05× | Fused | Win |
| Github Events (minified) | 1,218 | 956 | 768 | 1.27× | Fused | Win |
| Google Maps Response (minified) | 1,743 | 1,325 | 773 | 1.32× | Fused | Win |
| Github Events (prettified) | 993 | 1,374 | 898 | 0.72× | Two-stage | Win |
| Google Maps Response (prettified) | 2,371 | 2,421 | 1,299 | 0.98× | Two-stage | Win |
| Instruments (minified) | 2,238 | 1,349 | 1,071 | 1.66× | Fused | Win |
| Instruments (prettified) | 2,373 | 2,444 | 1,944 | 0.97× | Two-stage | Win |
| Marine IK Reverse (minified) | 627 | 530 | 446 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 1,860 | 1,841 | 1,571 | 1.01× | Tie | Win |
| Marine IK (minified) | 656 | 553 | 436 | 1.19× | Fused | Win |
| Marine IK (prettified) | 1,840 | 1,861 | 1,457 | 0.99× | Tie | Win |
| Random (minified) | 880 | 785 | 666 | 1.12× | Fused | Win |
| Random (prettified) | 1,032 | 1,090 | 950 | 0.95× | Two-stage | Win |
| Twitter (minified) | 1,930 | 1,437 | 974 | 1.34× | Fused | Win |
| Twitter (prettified) | 1,936 | 2,037 | 1,212 (n/c) | 0.95× | Two-stage | Win |
| Apache Builds Small (minified) | 1,301 | 1,455 | 816 | 0.89× | Two-stage | Win |
| Canada Small (minified) | 715 | 670 | 426 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,281 | 1,526 | 982 | 0.84× | Two-stage | Win |
| Canada Small (prettified) | 1,437 | 1,607 | 1,072 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,063 | 926 | 708 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 1,483 | 1,418 | 1,296 | 1.05× | Fused | Win |
| Discord Small (minified) | 1,374 | 1,096 | 867 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,537 | 1,321 | 1,180 | 1.16× | Fused | Win |
| Github Events Small (minified) | 1,326 | 1,071 | 998 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 1,546 | 1,088 | 665 | 1.42× | Fused | Win |
| Github Events Small (prettified) | 1,035 | 1,257 | 1,220 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,904 (n/c) | 1,882 | 1,234 | 1.01× | Tie | Win |
| Instruments Small (minified) | 2,198 | 1,547 | 1,031 | 1.42× | Fused | Win |
| Instruments Small (prettified) | 2,233 | 2,140 | 1,578 (n/c) | 1.04× | Fused | Win |
| Marine IK Reverse Small (minified) | 498 | 427 | 263 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,596 | 1,660 | 1,280 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 638 | 543 | 379 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,941 | 1,991 | 1,423 | 0.97× | Two-stage | Win |
| Random Small (minified) | 1,116 | 1,044 | 646 | 1.07× | Fused | Win |
| Random Small (prettified) | 1,320 | 1,284 | 886 | 1.03× | Fused | Win |
| Twitter Small (minified) | 2,319 | 1,714 | 1,119 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 2,068 | 2,113 | 1,406 | 0.98× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,222 | 314 | 180 | 3.89× | Fused | Win |
| Double (POD) | 803 | 312 | 185 | 2.57× | Fused | Win |
| Int64 (POD) | 1,857 | 712 | 397 | 2.61× | Fused | Win |
| String (POD) | 1,219 | 658 | 796 | 1.85× | Fused | Win |
| Uint64 (POD) | 2,027 | 791 | 459 | 2.56× | Fused | Win |
| Apache Builds (minified) | 1,507 | 989 | 830 | 1.52× | Fused | Win |
| Canada (minified) | 778 | 671 | 472 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 1,539 | 1,166 | 960 | 1.32× | Fused | Win |
| Canada (prettified) | 1,576 (n/c) | 1,440 | 1,008 | 1.09× | Fused | Win |
| CitmCatalog (minified) | 2,195 | 1,417 | 912 | 1.55× | Fused | Win |
| CitmCatalog (prettified) | 3,021 | 2,484 | 1,921 | 1.22× | Fused | Win |
| Discord (minified) | 2,143 | 1,225 | 1,135 | 1.75× | Fused | Win |
| Discord (prettified) | 2,199 | 1,524 | 1,641 | 1.44× | Fused | Win |
| Github Events (minified) | 1,733 | 1,259 | 1,284 | 1.38× | Fused | Win |
| Google Maps Response (minified) | 2,265 | 1,126 | 918 | 2.01× | Fused | Win |
| Github Events (prettified) | 1,529 | 1,590 | 1,615 | 0.96× | Two-stage | Tie |
| Google Maps Response (prettified) | 2,807 | 2,218 | 1,701 | 1.27× | Fused | Win |
| Instruments (minified) | 2,443 | 1,541 | 1,174 | 1.59× | Fused | Win |
| Instruments (prettified) | 2,307 | 2,276 | 1,842 | 1.01× | Fused | Win |
| Marine IK Reverse (minified) | 713 (n/c) | 614 | 448 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 1,997 | 1,890 | 1,352 | 1.06× | Fused | Win |
| Marine IK (minified) | 655 | 618 | 395 (n/c) | 1.06× | Fused | Win |
| Marine IK (prettified) | 1,925 | 1,857 | 1,204 | 1.04× | Fused | Win |
| Random (minified) | 1,284 | 831 | 731 | 1.55× | Fused | Win |
| Random (prettified) | 1,492 | 1,074 | 1,141 | 1.39× | Fused | Win |
| Twitter (minified) | 2,052 | 1,719 | 1,714 | 1.19× | Fused | Win |
| Twitter (prettified) | 2,072 | 2,202 | 2,172 | 0.94× | Two-stage | Win |
| Apache Builds Small (minified) | 1,682 | 1,400 | 1,025 | 1.20× | Fused | Win |
| Canada Small (minified) | 765 | 709 | 468 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,657 | 1,496 | 1,292 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,357 | 1,287 | 959 | 1.05× | Fused | Win |
| CitmCatalog Small (minified) | 1,148 | 810 | 728 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 1,742 | 1,556 | 1,457 | 1.12× | Fused | Win |
| Discord Small (minified) | 2,238 | 1,253 | 951 | 1.79× | Fused | Win |
| Discord Small (prettified) | 2,055 | 1,674 | 1,737 | 1.23× | Fused | Win |
| Github Events Small (minified) | 1,857 | 1,389 | 1,477 | 1.34× | Fused | Win |
| Google Maps Response Small (minified) | 2,354 | 1,106 | 907 | 2.13× | Fused | Win |
| Github Events Small (prettified) | 1,852 | 1,388 | 1,882 | 1.33× | Fused | Loss |
| Google Maps Response Small (prettified) | 2,993 | 2,330 | 2,031 | 1.28× | Fused | Win |
| Instruments Small (minified) | 2,974 | 1,621 | 1,206 | 1.83× | Fused | Win |
| Instruments Small (prettified) | 2,877 | 2,649 | 1,938 | 1.09× | Fused | Win |
| Marine IK Reverse Small (minified) | 675 | 552 | 378 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,658 | 1,752 | 1,236 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 569 | 514 | 336 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 1,905 | 1,883 | 1,279 | 1.01× | Tie | Win |
| Random Small (minified) | 1,416 | 1,133 | 946 | 1.25× | Fused | Win |
| Random Small (prettified) | 1,410 | 1,314 | 1,230 | 1.07× | Fused | Win |
| Twitter Small (minified) | 2,394 | 1,532 | 1,647 | 1.56× | Fused | Win |
| Twitter Small (prettified) | 1,966 | 2,176 | 2,142 | 0.90× | Two-stage | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 584 | 280 | 94 | 2.09× | Fused | Win |
| Double (POD) | 848 | 597 | 154 | 1.42× | Fused | Win |
| Int64 (POD) | 2,111 | 959 | 334 | 2.20× | Fused | Win |
| String (POD) | 1,691 | 1,138 | 721 | 1.49× | Fused | Win |
| Uint64 (POD) | 1,838 | 976 | 335 | 1.88× | Fused | Win |
| Apache Builds (minified) | 1,554 | 1,216 | 987 | 1.28× | Fused | Win |
| Canada (minified) | 861 | 795 | 492 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 1,651 | 1,553 | 1,249 | 1.06× | Fused | Win |
| Canada (prettified) | 1,661 | 1,799 | 1,081 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,198 | 1,459 | 594 | 1.51× | Fused | Win |
| CitmCatalog (prettified) | 2,754 | 3,110 | 1,214 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,695 | 1,428 | 805 | 1.19× | Fused | Win |
| Discord (prettified) | 1,694 | 2,005 | 1,141 | 0.84× | Two-stage | Win |
| Github Events (minified) | 1,590 | 1,306 | 719 | 1.22× | Fused | Win |
| Google Maps Response (minified) | 1,908 | 1,362 | 593 | 1.40× | Fused | Win |
| Github Events (prettified) | 1,498 | 1,529 | 857 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,410 | 2,588 | 1,169 | 0.93× | Two-stage | Win |
| Instruments (minified) | 2,548 | 1,756 | 803 | 1.45× | Fused | Win |
| Instruments (prettified) | 2,224 | 2,749 | 1,271 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 672 | 586 | 197 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,039 | 2,105 | 674 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 681 | 600 | 373 | 1.14× | Fused | Win |
| Marine IK (prettified) | 2,061 | 2,111 | 1,301 | 0.98× | Two-stage | Win |
| Random (minified) | 1,087 | 1,012 | 678 | 1.07× | Fused | Win |
| Random (prettified) | 1,331 | 1,511 | 1,008 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,806 | 1,568 | 1,056 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,692 | 1,988 | 1,333 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 1,684 | 1,235 | 1,023 | 1.36× | Fused | Win |
| Canada Small (minified) | 842 | 760 | 392 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 1,655 | 1,532 | 1,245 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,636 | 1,771 | 914 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,212 | 960 | 570 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 1,449 | 1,687 | 970 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,474 | 1,207 | 866 | 1.22× | Fused | Win |
| Discord Small (prettified) | 1,387 | 1,649 | 1,159 | 0.84× | Two-stage | Win |
| Github Events Small (minified) | 1,515 | 1,059 | 695 | 1.43× | Fused | Win |
| Google Maps Response Small (minified) | 1,866 | 1,263 | 593 | 1.48× | Fused | Win |
| Github Events Small (prettified) | 1,433 | 1,330 | 865 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,219 | 2,486 | 1,174 | 0.89× | Two-stage | Win |
| Instruments Small (minified) | 2,202 | 1,577 | 781 | 1.40× | Fused | Win |
| Instruments Small (prettified) | 1,971 | 2,475 | 1,261 | 0.80× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 601 | 541 | 149 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,654 | 1,933 | 558 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 640 | 573 | 340 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,776 | 2,059 | 1,174 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,133 | 985 | 657 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,363 | 1,465 | 993 | 0.93× | Two-stage | Win |
| Twitter Small (minified) | 1,813 | 1,484 | 1,273 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 1,629 | 1,920 | 1,620 | 0.85× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,179 | 636 | 281 | 3.43× | Fused | Win |
| Double (POD) | 1,361 | 788 | 367 | 1.73× | Fused | Win |
| Int64 (POD) | 3,383 | 1,767 | 796 | 1.91× | Fused | Win |
| String (POD) | 2,841 | 1,755 | 1,864 | 1.62× | Fused | Win |
| Uint64 (POD) | 3,504 | 1,685 | 781 | 2.08× | Fused | Win |
| Apache Builds (minified) | 2,903 | 2,323 | 1,715 | 1.25× | Fused | Win |
| Canada (minified) | 1,270 | 1,186 | 788 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 2,513 | 2,866 | 2,181 | 0.88× | Two-stage | Win |
| Canada (prettified) | 2,398 | 2,431 | 1,749 | 0.99× | Tie | Win |
| CitmCatalog (minified) | 2,865 | 2,120 | 1,392 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 3,268 | 4,220 | 2,921 | 0.77× | Two-stage | Win |
| Discord (minified) | 3,292 | 2,503 | 1,836 | 1.32× | Fused | Win |
| Discord (prettified) | 2,209 | 3,407 | 2,621 | 0.65× | Two-stage | Win |
| Github Events (minified) | 2,299 | 2,152 | 2,021 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 3,033 | 2,186 | 1,442 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,984 | 2,454 | 2,404 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 3,065 | 3,856 | 2,858 | 0.79× | Two-stage | Win |
| Instruments (minified) | 3,049 | 2,219 | 1,887 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,434 | 3,462 | 2,999 | 0.70× | Two-stage | Win |
| Marine IK Reverse (minified) | 920 | 821 | 633 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,685 | 2,715 | 2,333 | 0.99× | Tie | Win |
| Marine IK (minified) | 965 | 851 | 653 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,788 | 2,872 | 2,362 | 0.97× | Two-stage | Win |
| Random (minified) | 1,794 | 1,502 | 1,481 | 1.19× | Fused | Win |
| Random (prettified) | 1,753 | 2,243 | 2,236 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,677 | 2,196 | 2,166 | 1.22× | Fused | Win |
| Twitter (prettified) | 2,025 | 2,933 | 2,830 | 0.69× | Two-stage | Win |
| Apache Builds Small (minified) | 3,130 | 2,478 | 1,834 | 1.26× | Fused | Win |
| Canada Small (minified) | 1,270 | 1,220 | 822 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 2,509 | 3,100 | 2,206 | 0.81× | Two-stage | Win |
| Canada Small (prettified) | 2,340 | 2,618 | 1,947 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,175 | 1,738 | 1,364 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 2,166 | 2,911 | 2,374 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,815 | 2,206 | 1,891 | 1.28× | Fused | Win |
| Discord Small (prettified) | 2,137 | 2,910 | 2,554 | 0.73× | Two-stage | Win |
| Github Events Small (minified) | 2,664 | 2,319 | 2,307 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,823 | 2,066 | 1,513 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 2,215 | 2,771 | 2,792 | 0.80× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,956 | 3,872 | 2,996 | 0.76× | Two-stage | Win |
| Instruments Small (minified) | 2,821 | 2,067 | 1,767 | 1.37× | Fused | Win |
| Instruments Small (prettified) | 2,379 | 3,310 | 2,833 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,046 | 915 | 666 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,574 | 3,025 | 2,410 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,189 | 1,015 | 676 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,688 | 3,331 | 2,423 | 0.81× | Two-stage | Win |
| Random Small (minified) | 2,253 | 1,945 | 1,458 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,972 | 2,911 | 2,226 | 0.68× | Two-stage | Win |
| Twitter Small (minified) | 2,923 | 2,458 | 2,495 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 2,255 | 3,116 | 3,030 | 0.72× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,444 | 569 | 247 | 4.29× | Fused | Win |
| Double (POD) | 1,302 | 660 | 295 | 1.97× | Fused | Win |
| Int64 (POD) | 3,695 | 1,509 | 735 | 2.45× | Fused | Win |
| String (POD) | 2,731 | 1,555 | 1,705 | 1.76× | Fused | Win |
| Uint64 (POD) | 3,775 | 1,518 | 724 | 2.49× | Fused | Win |
| Apache Builds (minified) | 2,701 | 2,209 | 1,945 | 1.22× | Fused | Win |
| Canada (minified) | 1,183 | 1,250 | 1,031 | 0.95× | Two-stage | Win |
| Apache Builds (prettified) | 2,507 | 2,702 | 2,475 | 0.93× | Two-stage | Win |
| Canada (prettified) | 2,456 | 2,711 | 2,274 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 3,024 | 2,221 | 1,630 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,867 | 4,267 | 3,357 | 0.91× | Two-stage | Win |
| Discord (minified) | 3,286 | 2,413 | 1,781 | 1.36× | Fused | Win |
| Discord (prettified) | 2,854 | 3,349 | 2,515 | 0.85× | Two-stage | Win |
| Github Events (minified) | 2,218 | 2,032 | 1,746 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 3,062 | 2,140 | 1,150 | 1.43× | Fused | Win |
| Github Events (prettified) | 2,029 | 2,668 | 2,044 | 0.76× | Two-stage | Win |
| Google Maps Response (prettified) | 3,702 | 3,842 | 2,335 | 0.96× | Two-stage | Win |
| Instruments (minified) | 3,930 | 2,562 | 1,482 | 1.53× | Fused | Win |
| Instruments (prettified) | 3,311 | 3,809 | 2,356 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 802 | 792 | 667 | 1.01× | Fused | Win |
| Marine IK Reverse (prettified) | 2,601 | 2,791 | 2,396 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 822 | 818 | 660 | 1.00× | Fused | Win |
| Marine IK (prettified) | 2,621 | 2,777 | 2,311 | 0.94× | Two-stage | Win |
| Random (minified) | 1,519 | 1,507 | 1,386 | 1.01× | Fused | Win |
| Random (prettified) | 1,717 | 2,197 | 2,066 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,442 | 2,118 | 1,620 | 1.15× | Fused | Win |
| Twitter (prettified) | 2,099 | 2,678 | 2,101 | 0.78× | Two-stage | Win |
| Apache Builds Small (minified) | 2,890 | 2,479 | 1,833 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,343 | 1,220 | 901 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,576 | 2,977 | 2,248 | 0.87× | Two-stage | Win |
| Canada Small (prettified) | 2,483 | 2,779 | 2,048 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,188 | 1,725 | 1,297 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,357 | 2,849 | 2,231 | 0.83× | Two-stage | Win |
| Discord Small (minified) | 2,680 | 2,081 | 1,580 | 1.29× | Fused | Win |
| Discord Small (prettified) | 2,334 | 2,637 | 2,090 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,609 | 2,193 | 2,309 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 2,810 | 2,023 | 1,205 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 2,424 | 2,755 | 2,842 | 0.88× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,426 | 3,727 | 2,416 | 0.92× | Two-stage | Win |
| Instruments Small (minified) | 3,680 | 2,338 | 1,329 | 1.57× | Fused | Win |
| Instruments Small (prettified) | 3,010 | 3,561 | 2,166 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,016 | 884 | 627 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,559 | 3,094 | 2,305 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 1,152 | 940 | 632 | 1.23× | Fused | Win |
| Marine IK Small (prettified) | 2,781 | 3,184 | 2,279 | 0.87× | Two-stage | Win |
| Random Small (minified) | 2,084 | 1,911 | 1,405 | 1.09× | Fused | Win |
| Random Small (prettified) | 2,184 | 2,794 | 2,135 | 0.78× | Two-stage | Win |
| Twitter Small (minified) | 3,190 | 2,555 | 1,804 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,551 | 3,141 | 2,289 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,807 | 425 | 167 | 4.25× | Fused | Win |
| Double (POD) | 1,193 | 474 | 165 | 2.52× | Fused | Win |
| Int64 (POD) | 2,995 | 879 | 415 | 3.41× | Fused | Win |
| String (POD) | 2,196 | 969 | 1,018 | 2.27× | Fused | Win |
| Uint64 (POD) | 2,598 | 948 | 387 | 2.74× | Fused | Win |
| Apache Builds (minified) | 1,926 | 1,322 | 841 | 1.46× | Fused | Win |
| Canada (minified) | 1,011 | 895 | 568 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,716 | 1,708 | 1,041 | 1.00× | Tie | Win |
| Canada (prettified) | 1,748 | 1,850 | 1,418 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,494 | 1,354 | 1,214 | 1.84× | Fused | Win |
| CitmCatalog (prettified) | 2,895 | 2,486 | 2,115 | 1.16× | Fused | Win |
| Discord (minified) | 2,469 | 1,439 | 1,106 | 1.72× | Fused | Win |
| Discord (prettified) | 2,271 | 2,082 | 1,424 | 1.09× | Fused | Win |
| Github Events (minified) | 2,135 | 1,073 | 1,013 | 1.99× | Fused | Win |
| Google Maps Response (minified) | 2,376 | 1,372 | 836 | 1.73× | Fused | Win |
| Github Events (prettified) | 1,476 | 1,890 | 1,078 | 0.78× | Two-stage | Win |
| Google Maps Response (prettified) | 2,623 | 2,388 | 1,245 | 1.10× | Fused | Win |
| Instruments (minified) | 2,781 | 1,551 | 1,198 | 1.79× | Fused | Win |
| Instruments (prettified) | 2,601 | 2,665 | 2,016 | 0.98× | Two-stage | Win |
| Marine IK Reverse (minified) | 673 | 614 | 504 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,061 | 2,019 | 1,664 | 1.02× | Fused | Win |
| Marine IK (minified) | 772 | 634 | 511 | 1.22× | Fused | Win |
| Marine IK (prettified) | 2,051 | 1,974 | 1,391 | 1.04× | Fused | Win |
| Random (minified) | 1,248 | 1,199 | 796 | 1.04× | Fused | Win |
| Random (prettified) | 1,568 | 1,564 | 1,196 | 1.00× | Tie | Win |
| Twitter (minified) | 2,607 | 1,836 | 1,008 | 1.42× | Fused | Win |
| Twitter (prettified) | 2,389 | 2,626 | 1,035 | 0.91× | Two-stage | Win |
| Apache Builds Small (minified) | 2,210 | 1,710 | 986 | 1.29× | Fused | Win |
| Canada Small (minified) | 1,000 | 845 | 516 | 1.18× | Fused | Win |
| Apache Builds Small (prettified) | 1,807 | 1,901 | 1,372 | 0.95× | Two-stage | Win |
| Canada Small (prettified) | 1,776 | 1,809 | 1,146 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,500 | 1,080 | 906 | 1.39× | Fused | Win |
| CitmCatalog Small (prettified) | 1,973 | 1,547 | 1,524 | 1.28× | Fused | Win |
| Discord Small (minified) | 2,297 | 1,614 | 1,108 | 1.42× | Fused | Win |
| Discord Small (prettified) | 1,907 | 1,855 | 1,515 | 1.03× | Fused | Win |
| Github Events Small (minified) | 1,895 (n/c) | 1,299 | 1,232 | 1.46× | Fused | Win |
| Google Maps Response Small (minified) | 2,010 | 1,312 | 781 | 1.53× | Fused | Win |
| Github Events Small (prettified) | 1,110 | 1,601 | 1,402 | 0.69× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,399 | 2,189 | 1,337 | 1.10× | Fused | Win |
| Instruments Small (minified) | 2,781 | 1,507 | 1,095 | 1.85× | Fused | Win |
| Instruments Small (prettified) | 2,422 | 2,193 | 1,758 | 1.10× | Fused | Win |
| Marine IK Reverse Small (minified) | 635 | 506 | 385 | 1.26× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,812 | 2,023 | 1,456 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 901 | 712 | 432 | 1.27× | Fused | Win |
| Marine IK Small (prettified) | 2,329 | 2,437 | 1,615 | 0.96× | Two-stage | Win |
| Random Small (minified) | 1,419 | 1,182 | 726 | 1.20× | Fused | Win |
| Random Small (prettified) | 1,751 | 1,653 | 1,175 | 1.06× | Fused | Win |
| Twitter Small (minified) | 2,785 | 2,100 | 1,179 | 1.33× | Fused | Win |
| Twitter Small (prettified) | 2,413 | 2,552 | 1,531 | 0.95× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,344 | 308 | 171 | 4.36× | Fused | Win |
| Double (POD) | 1,045 | 358 | 171 | 2.92× | Fused | Win |
| Int64 (POD) | 2,589 | 809 | 450 | 3.20× | Fused | Win |
| String (POD) | 2,239 | 888 | 1,098 | 2.52× | Fused | Win |
| Uint64 (POD) | 2,618 | 854 | 420 | 3.06× | Fused | Win |
| Apache Builds (minified) | 2,179 | 1,249 | 1,042 | 1.74× | Fused | Win |
| Canada (minified) | 878 | 823 | 578 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 2,101 | 1,681 | 1,240 | 1.25× | Fused | Win |
| Canada (prettified) | 1,856 | 1,797 | 1,191 (n/c) | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,658 | 1,493 | 952 | 1.78× | Fused | Win |
| CitmCatalog (prettified) | 2,782 | 2,952 | 2,048 | 0.94× | Two-stage | Win |
| Discord (minified) | 2,787 | 1,451 | 1,483 | 1.92× | Fused | Win |
| Discord (prettified) | 2,464 | 2,045 | 1,969 | 1.20× | Fused | Win |
| Github Events (minified) | 2,224 | 1,583 | 1,861 | 1.40× | Fused | Win |
| Google Maps Response (minified) | 2,334 | 1,189 | 984 | 1.96× | Fused | Win |
| Github Events (prettified) | 2,117 | 1,841 | 2,065 | 1.15× | Fused | Win |
| Google Maps Response (prettified) | 3,115 | 2,570 | 1,787 | 1.21× | Fused | Win |
| Instruments (minified) | 3,069 | 1,753 | 1,291 | 1.75× | Fused | Win |
| Instruments (prettified) | 2,630 | 2,650 | 2,228 | 0.99× | Two-stage | Win |
| Marine IK Reverse (minified) | 761 | 697 | 486 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,109 | 1,883 | 1,492 | 1.12× | Fused | Win |
| Marine IK (minified) | 783 | 697 | 400 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,232 | 2,073 | 1,422 | 1.08× | Fused | Win |
| Random (minified) | 1,624 | 1,216 | 1,014 | 1.34× | Fused | Win |
| Random (prettified) | 1,924 | 1,676 | 1,522 | 1.15× | Fused | Win |
| Twitter (minified) | 3,003 | 1,878 | 1,997 | 1.60× | Fused | Win |
| Twitter (prettified) | 2,699 | 2,702 | 2,522 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 2,236 | 1,786 | 1,401 | 1.25× | Fused | Win |
| Canada Small (minified) | 944 | 783 | 526 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 1,641 | 1,935 | 1,751 | 0.85× | Two-stage | Win |
| Canada Small (prettified) | 1,702 | 1,842 | 1,093 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,877 | 1,317 | 864 | 1.43× | Fused | Win |
| CitmCatalog Small (prettified) | 2,396 | 2,207 | 1,773 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,256 | 1,647 | 1,715 | 1.37× | Fused | Win |
| Discord Small (prettified) | 2,490 | 2,144 | 2,230 | 1.16× | Fused | Win |
| Github Events Small (minified) | 2,608 | 1,734 | 2,037 | 1.50× | Fused | Win |
| Google Maps Response Small (minified) | 2,666 | 1,479 | 1,150 | 1.80× | Fused | Win |
| Github Events Small (prettified) | 2,502 | 2,054 | 2,388 | 1.22× | Fused | Win |
| Google Maps Response Small (prettified) | 3,200 | 2,688 | 2,240 | 1.19× | Fused | Win |
| Instruments Small (minified) | 3,349 | 1,992 | 1,412 | 1.68× | Fused | Win |
| Instruments Small (prettified) | 3,025 | 2,862 | 2,258 | 1.06× | Fused | Win |
| Marine IK Reverse Small (minified) | 866 | 712 | 435 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,987 | 2,119 | 1,278 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 783 | 687 | 419 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,301 | 2,178 | 1,264 | 1.06× | Fused | Win |
| Random Small (minified) | 1,924 | 1,469 | 1,167 | 1.31× | Fused | Win |
| Random Small (prettified) | 1,989 | 1,906 | 1,729 | 1.04× | Tie | Win |
| Twitter Small (minified) | 2,764 | 2,111 | 1,853 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 2,623 | 2,526 | 2,327 | 1.04× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on three things: whether the document is indented, whether the machine is x86, and, on the M2 Pro, the compiler. On the three x86 builds the fused path wins all 15 POD-type tests, 31 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 31 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC follows the same split: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 19 and the fused path 3 (Apache Builds, Apache Builds Small and Github Events Small). On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (the tape takes minified Apache Builds Small on macOS/GCC) and 23 of the 44 prettified documents (14 to the tape, 7 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.26× (String, Windows/MSVC) to 4.00× (Bool, Linux/GCC) faster than the two-stage path, and 1.42× (Double, Windows/MSVC) to 4.36× (Bool, macOS/Clang) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** The fused path wins 107 of the 110 minified documents: all 22 under MSVC, on Linux/Clang and on macOS/Clang, 21 of 22 on macOS/GCC, where the two-stage path wins minified Apache Builds Small by 12% (1,455 against 1,301 MB/s), and 20 of 22 on Linux/GCC, where the two-stage path wins minified Canada by 3% (1,016 against 990 MB/s) and minified Marine IK is a tie. The fused path's margins run from 2% (Random on Linux/GCC, 1,135 against 1,114 MB/s) to 2.01× (Google Maps Response on macOS/Clang, 2,265 against 1,126 MB/s) on the full-size documents and up to 2.13× on the Small ones (Google Maps Response Small on macOS/Clang, 2,354 against 1,106 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 62 of 66.** It wins all 22 on Linux/GCC, 21 of 22 on Linux/Clang, where prettified Marine IK Reverse is a tie (2,502 MB/s fused against 2,495), and 19 of 22 under MSVC, where the fused path wins prettified Apache Builds, Apache Builds Small and Github Events Small, by 4%, 7% and 12%. The tape's margins run from 1% (Marine IK on Linux/Clang, 2,566 against 2,533 MB/s) to 40% (Discord on Linux/Clang, 2,578 against 1,843) on the full-size documents and from 5% (Random Small under MSVC) to 40% (Instruments Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 61 of the 66 prettified x86 documents: the fused path wins the same three under MSVC, and prettified Canada and Marine IK Reverse are ties on Linux/Clang.

**Prettified documents on the M2 Pro: split by compiler.** Of the 44 prettified documents, the fused path wins 23, the tape wins 14 and 7 are ties, but the two compilers disagree. On macOS/Clang the fused path wins 17 of 22; the tape takes prettified Github Events and Twitter and the Small copies of Marine IK Reverse and Twitter, and Marine IK Small is a tie. On macOS/GCC the fused path wins only 6 of 22, the tape wins 10 (prettified Github Events, Google Maps Response, Instruments, Random and Twitter, and the Small copies of Apache Builds, Canada, Github Events, Marine IK and Marine IK Reverse) and 6 are ties. The tape's widest M2 Pro margin is prettified Github Events on macOS/GCC, 1,374 against 993 MB/s (1.38×). Reused, the fused path wins 25, the tape 15, and 4 are ties: 15, 5 and 2 on macOS/Clang, and 10, 10 and 2 on macOS/GCC. NEON escapes much of the prettified penalty seen on x86, most of all under Clang, and we do not have an explanation for why, or for why GCC keeps less of that advantage. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified Github Events and Twitter are where Jsonifier is weakest.** All five of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are prettified copies of those two documents: Twitter on Linux/Clang by 1% (2,229 against 2,247 MB/s), Twitter Small on Linux/Clang by 4% (2,675 against 2,798), and Github Events Small on Linux/Clang by 6% (2,262 against 2,410), on Linux/GCC by 5% (2,236 against 2,345) and on macOS/Clang by 2% (1,852 against 1,882). The two ties are prettified Github Events on Linux/Clang and on macOS/Clang. With objects reused the faster path loses two: Github Events Small on Linux/GCC by 3% (2,755 against 2,842) and on Linux/Clang by 1% (2,771 against 2,792). We have not profiled what these documents have in common.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 64% of its Linux/Clang throughput, the two-stage path 66% and simdjson 48%. Because the two Jsonifier paths lose a similar share, MSVC routes input much as Linux/Clang does. The spread between documents is about the same for the two Jsonifier paths (the fused path from 53% on Discord to 79% on CitmCatalog, the two-stage path from 52% on Apache Builds to 76% on Instruments) and widest for simdjson, which keeps 38% of its Linux/Clang throughput on Github Events and 65% on Canada. We have not profiled why.

**Against simdjson, Jsonifier wins 238 of 245 tests with whichever path is faster, ties two and loses five** (all prettified documents). The fused path alone wins 222, ties 2 and loses 21, all prettified, its sixteen extra losses being eight on Linux/Clang, three on Linux/GCC, three on macOS/Clang and one each under MSVC and on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 190 MB/s, against 346 MB/s for the same document in declared order, so the faster Jsonifier path is 3.21× faster than simdjson freshly allocated (3.41× reused). The prettified form shows the same gap, at 649 MB/s against 1,237, and the faster Jsonifier path is 3.01× faster (3.12× reused). The Small copies show it too, at 143 MB/s minified and 538 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.18× to 1.59× faster than simdjson on the full-size reversed document, freshly allocated, and 1.16× to 1.57× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so does most prettified input under Clang, while prettified input under GCC leans to the tape. Routing each test to its faster path would raise the record against simdjson from 222 wins, 2 ties and 21 losses to 238 wins, 2 ties and 5 losses. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,472 | 1,352 | 1.09× | Win |
| Canada (minified) | 1,016 | 861 | 1.18× | Win |
| Apache Builds (prettified) | 1,842 | 1,731 | 1.06× | Win |
| Canada (prettified) | 2,255 | 1,921 | 1.17× | Win |
| CitmCatalog (minified) | 1,844 | 1,390 | 1.33× | Win |
| CitmCatalog (prettified) | 3,648 | 2,907 | 1.25× | Win |
| Discord (minified) | 1,839 | 1,538 | 1.20× | Win |
| Discord (prettified) | 2,558 | 2,191 | 1.17× | Win |
| Github Events (minified) | 1,537 | 1,544 | 1.00× | **Loss** |
| Google Maps Response (minified) | 1,923 | 1,279 | 1.50× | Win |
| Github Events (prettified) | 2,047 | 1,925 | 1.06× | Win |
| Google Maps Response (prettified) | 3,546 | 2,564 | 1.38× | Win |
| Instruments (minified) | 2,265 | 1,739 | 1.30× | Win |
| Instruments (prettified) | 3,383 | 2,790 | 1.21× | Win |
| Marine IK Reverse (minified) | 720 | 703 | 1.02× | Win |
| Marine IK Reverse (prettified) | 2,584 | 2,547 | 1.01× | Win |
| Marine IK (minified) | 739 | 696 | 1.06× | Win |
| Marine IK (prettified) | 2,543 | 2,478 | 1.03× | Win |
| Random (minified) | 1,114 | 1,089 | 1.02× | Win |
| Random (prettified) | 1,646 | 1,610 | 1.02× | Win |
| Twitter (minified) | 1,630 | 1,736 | 0.94× | **Loss** |
| Twitter (prettified) | 2,169 | 2,210 | 0.98× | **Loss** |
| Apache Builds Small (minified) | 2,123 | 1,697 | 1.25× | Win |
| Canada Small (minified) | 965 | 919 | 1.05× | Win |
| Apache Builds Small (prettified) | 2,535 | 2,096 | 1.21× | Win |
| Canada Small (prettified) | 2,287 | 2,181 | 1.05× | Win |
| CitmCatalog Small (minified) | 1,471 | 1,213 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,448 | 2,078 | 1.18× | Win |
| Discord Small (minified) | 1,706 | 1,355 | 1.26× | Win |
| Discord Small (prettified) | 2,193 | 1,787 | 1.23× | Win |
| Github Events Small (minified) | 1,783 | 1,770 | 1.01× | Win |
| Google Maps Response Small (minified) | 1,791 | 1,256 | 1.43× | Win |
| Github Events Small (prettified) | 2,236 | 2,229 | 1.00× | Tie |
| Google Maps Response Small (prettified) | 3,349 | 2,473 | 1.35× | Win |
| Instruments Small (minified) | 2,201 | 1,656 | 1.33× | Win |
| Instruments Small (prettified) | 3,366 | 2,669 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 740 | 733 | 1.01× | Win |
| Marine IK Reverse Small (prettified) | 2,612 | 2,607 | 1.00× | Tie |
| Marine IK Small (minified) | 772 | 738 | 1.05× | Win |
| Marine IK Small (prettified) | 2,701 | 2,627 | 1.03× | Win |
| Random Small (minified) | 1,564 | 1,237 | 1.26× | Win |
| Random Small (prettified) | 2,319 | 1,840 | 1.26× | Win |
| Twitter Small (minified) | 2,311 | 2,086 | 1.11× | Win |
| Twitter Small (prettified) | 2,886 | 2,641 | 1.09× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 875 | 761 | 1.15× | Win |
| Canada (minified) | 701 | 587 | 1.19× | Win |
| Apache Builds (prettified) | 1,114 | 1,000 | 1.11× | Win |
| Canada (prettified) | 1,340 | 1,219 | 1.10× | Win |
| CitmCatalog (minified) | 1,138 | 1,171 | 0.97× | **Loss** |
| CitmCatalog (prettified) | 2,196 | 2,062 | 1.07× | Win |
| Discord (minified) | 929 | 939 | 0.99× | Tie |
| Discord (prettified) | 1,673 | 1,264 | 1.32× | Win |
| Github Events (minified) | 956 | 1,156 | 0.83× | **Loss** |
| Google Maps Response (minified) | 1,325 | 1,025 | 1.29× | Win |
| Github Events (prettified) | 1,374 | 1,299 | 1.06× | Win |
| Google Maps Response (prettified) | 2,421 | 1,770 | 1.37× | Win |
| Instruments (minified) | 1,349 | 1,267 | 1.06× | Win |
| Instruments (prettified) | 2,444 | 1,615 | 1.51× | Win |
| Marine IK Reverse (minified) | 530 | 503 | 1.05× | Win |
| Marine IK Reverse (prettified) | 1,841 | 1,680 | 1.10× | Win |
| Marine IK (minified) | 553 | 491 | 1.13× | Win |
| Marine IK (prettified) | 1,861 | 1,618 | 1.15× | Win |
| Random (minified) | 785 | 702 | 1.12× | Win |
| Random (prettified) | 1,090 | 1,062 | 1.03× | Win |
| Twitter (minified) | 1,437 | 1,296 | 1.11× | Win |
| Twitter (prettified) | 2,037 | 1,522 | 1.34× | Win |
| Apache Builds Small (minified) | 1,455 | 821 | 1.77× | Win |
| Canada Small (minified) | 670 | 564 | 1.19× | Win |
| Apache Builds Small (prettified) | 1,526 | 1,245 | 1.23× | Win |
| Canada Small (prettified) | 1,607 | 1,250 | 1.29× | Win |
| CitmCatalog Small (minified) | 926 | 812 | 1.14× | Win |
| CitmCatalog Small (prettified) | 1,418 | 1,333 | 1.06× | Win |
| Discord Small (minified) | 1,096 | 946 | 1.16× | Win |
| Discord Small (prettified) | 1,321 | 1,323 | 1.00× | Tie |
| Github Events Small (minified) | 1,071 | 983 | 1.09× | Win |
| Google Maps Response Small (minified) | 1,088 | 848 | 1.28× | Win |
| Github Events Small (prettified) | 1,257 | 1,136 | 1.11× | Win |
| Google Maps Response Small (prettified) | 1,882 | 1,746 | 1.08× | Win |
| Instruments Small (minified) | 1,547 | 987 | 1.57× | Win |
| Instruments Small (prettified) | 2,140 | 1,690 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 427 | 476 | 0.90× | **Loss** |
| Marine IK Reverse Small (prettified) | 1,660 | 1,623 | 1.02× | Win |
| Marine IK Small (minified) | 543 | 503 | 1.08× | Win |
| Marine IK Small (prettified) | 1,991 | 1,803 | 1.10× | Win |
| Random Small (minified) | 1,044 | 730 | 1.43× | Win |
| Random Small (prettified) | 1,284 | 1,199 | 1.07× | Win |
| Twitter Small (minified) | 1,714 | 1,378 | 1.24× | Win |
| Twitter Small (prettified) | 2,113 | 1,725 | 1.23× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,209 | 1,730 | 1.28× | Win |
| Canada (minified) | 1,250 | 1,013 | 1.23× | Win |
| Apache Builds (prettified) | 2,702 | 2,200 | 1.23× | Win |
| Canada (prettified) | 2,711 | 2,227 | 1.22× | Win |
| CitmCatalog (minified) | 2,221 | 1,638 | 1.36× | Win |
| CitmCatalog (prettified) | 4,267 | 3,354 | 1.27× | Win |
| Discord (minified) | 2,413 | 1,746 | 1.38× | Win |
| Discord (prettified) | 3,349 | 2,498 | 1.34× | Win |
| Github Events (minified) | 2,032 | 1,902 | 1.07× | Win |
| Google Maps Response (minified) | 2,140 | 1,362 | 1.57× | Win |
| Github Events (prettified) | 2,668 | 2,241 | 1.19× | Win |
| Google Maps Response (prettified) | 3,842 | 2,689 | 1.43× | Win |
| Instruments (minified) | 2,562 | 1,784 | 1.44× | Win |
| Instruments (prettified) | 3,809 | 2,790 | 1.36× | Win |
| Marine IK Reverse (minified) | 792 | 783 | 1.01× | Win |
| Marine IK Reverse (prettified) | 2,791 | 2,800 | 1.00× | Tie |
| Marine IK (minified) | 818 | 780 | 1.05× | Win |
| Marine IK (prettified) | 2,777 | 2,772 | 1.00× | Tie |
| Random (minified) | 1,507 | 1,427 | 1.06× | Win |
| Random (prettified) | 2,197 | 2,090 | 1.05× | Win |
| Twitter (minified) | 2,118 | 2,121 | 1.00× | Tie |
| Twitter (prettified) | 2,678 | 2,633 | 1.02× | Win |
| Apache Builds Small (minified) | 2,479 | 1,751 | 1.42× | Win |
| Canada Small (minified) | 1,220 | 1,085 | 1.12× | Win |
| Apache Builds Small (prettified) | 2,977 | 2,167 | 1.37× | Win |
| Canada Small (prettified) | 2,779 | 2,532 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,725 | 1,313 | 1.31× | Win |
| CitmCatalog Small (prettified) | 2,849 | 2,213 | 1.29× | Win |
| Discord Small (minified) | 2,081 | 1,530 | 1.36× | Win |
| Discord Small (prettified) | 2,637 | 2,071 | 1.27× | Win |
| Github Events Small (minified) | 2,193 | 2,041 | 1.07× | Win |
| Google Maps Response Small (minified) | 2,023 | 1,386 | 1.46× | Win |
| Github Events Small (prettified) | 2,755 | 2,517 | 1.09× | Win |
| Google Maps Response Small (prettified) | 3,727 | 2,701 | 1.38× | Win |
| Instruments Small (minified) | 2,338 | 1,764 | 1.33× | Win |
| Instruments Small (prettified) | 3,561 | 2,833 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 884 | 837 | 1.06× | Win |
| Marine IK Reverse Small (prettified) | 3,094 | 2,952 | 1.05× | Win |
| Marine IK Small (minified) | 940 | 838 | 1.12× | Win |
| Marine IK Small (prettified) | 3,184 | 2,964 | 1.07× | Win |
| Random Small (minified) | 1,911 | 1,393 | 1.37× | Win |
| Random Small (prettified) | 2,794 | 2,095 | 1.33× | Win |
| Twitter Small (minified) | 2,555 | 2,281 | 1.12× | Win |
| Twitter Small (prettified) | 3,141 | 2,919 | 1.08× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,322 | 1,059 | 1.25× | Win |
| Canada (minified) | 895 | 662 | 1.35× | Win |
| Apache Builds (prettified) | 1,708 | 1,138 | 1.50× | Win |
| Canada (prettified) | 1,850 | 1,369 | 1.35× | Win |
| CitmCatalog (minified) | 1,354 | 1,293 | 1.05× | Win |
| CitmCatalog (prettified) | 2,486 | 2,101 | 1.18× | Win |
| Discord (minified) | 1,439 | 1,048 | 1.37× | Win |
| Discord (prettified) | 2,082 | 1,473 | 1.41× | Win |
| Github Events (minified) | 1,073 | 1,457 | 0.74× | **Loss** |
| Google Maps Response (minified) | 1,372 | 1,121 | 1.22× | Win |
| Github Events (prettified) | 1,890 | 1,537 | 1.23× | Win |
| Google Maps Response (prettified) | 2,388 | 1,877 | 1.27× | Win |
| Instruments (minified) | 1,551 | 1,194 | 1.30× | Win |
| Instruments (prettified) | 2,665 | 1,755 | 1.52× | Win |
| Marine IK Reverse (minified) | 614 | 557 | 1.10× | Win |
| Marine IK Reverse (prettified) | 2,019 | 1,820 | 1.11× | Win |
| Marine IK (minified) | 634 | 562 | 1.13× | Win |
| Marine IK (prettified) | 1,974 | 1,807 | 1.09× | Win |
| Random (minified) | 1,199 | 859 | 1.39× | Win |
| Random (prettified) | 1,564 | 1,231 | 1.27× | Win |
| Twitter (minified) | 1,836 | 1,373 | 1.34× | Win |
| Twitter (prettified) | 2,626 | 1,674 | 1.57× | Win |
| Apache Builds Small (minified) | 1,710 | 1,030 | 1.66× | Win |
| Canada Small (minified) | 845 | 665 | 1.27× | Win |
| Apache Builds Small (prettified) | 1,901 | 1,442 | 1.32× | Win |
| Canada Small (prettified) | 1,809 | 1,490 | 1.21× | Win |
| CitmCatalog Small (minified) | 1,080 | 819 | 1.32× | Win |
| CitmCatalog Small (prettified) | 1,547 | 1,696 | 0.91× | **Loss** |
| Discord Small (minified) | 1,614 | 1,202 | 1.34× | Win |
| Discord Small (prettified) | 1,855 | 1,593 | 1.16× | Win |
| Github Events Small (minified) | 1,299 | 1,151 | 1.13× | Win |
| Google Maps Response Small (minified) | 1,312 | 950 | 1.38× | Win |
| Github Events Small (prettified) | 1,601 | 1,306 | 1.23× | Win |
| Google Maps Response Small (prettified) | 2,189 | 1,632 | 1.34× | Win |
| Instruments Small (minified) | 1,507 | 1,118 | 1.35× | Win |
| Instruments Small (prettified) | 2,193 | 1,621 | 1.35× | Win |
| Marine IK Reverse Small (minified) | 506 | 529 | 0.96× | **Loss** |
| Marine IK Reverse Small (prettified) | 2,023 | 1,913 | 1.06× | Win |
| Marine IK Small (minified) | 712 | 593 | 1.20× | Win |
| Marine IK Small (prettified) | 2,437 | 2,110 | 1.15× | Win |
| Random Small (minified) | 1,182 | 816 | 1.45× | Win |
| Random Small (prettified) | 1,653 | 1,362 | 1.21× | Win |
| Twitter Small (minified) | 2,100 | 1,380 | 1.52× | Win |
| Twitter Small (prettified) | 2,552 | 2,001 | 1.28× | Win |

In the freshly allocated run, two-stage Jsonifier wins 78 of these 88 tests, ties 4 and loses 6, every loss by 0.5% to 17%: three on Linux/GCC (minified Github Events, the narrowest, and Twitter in both forms) and three on macOS/GCC (minified CitmCatalog, minified Marine IK Reverse Small, and minified Github Events, the widest at 0.83×). The ties are prettified Github Events Small and Marine IK Reverse Small on Linux/GCC, and minified Discord and prettified Discord Small on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 72, ties 2 and loses 14. With objects reused, two-stage Jsonifier wins 82, ties 3 and loses 3, all on macOS/GCC (minified Github Events by 26%, prettified CitmCatalog Small by 9% and minified Marine IK Reverse Small by 4%), and the fused path beats the reflection reader on 75 and loses 13. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 27 of the 40 results; all 13 wider gaps are on the macOS builds, and the widest is freshly allocated minified Twitter Small on macOS/Clang, where the "jsonifier" row runs at 3,601 MB/s against the two-stage row's 2,812. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,723 | 3,626 | 1.85× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,636 | 4,225 | 1.81× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,013 | 3,327 | 1.21× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,887 | 3,942 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,058 | 7,018 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 8,854 | 8,406 | 1.05× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,548 | 5,922 | 0.94× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,378 | 7,024 | 0.91× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,062 | 7,283 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,808 | 8,244 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,815 | 5,712 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,798 | 6,880 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,421 | 4,196 | 1.05× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,642 | 4,529 | 1.03× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,745 | 3,642 | 1.03× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,153 | 4,301 | 0.97× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,338 | 4,043 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,356 | 4,683 | 0.93× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 2,812 | 3,528 | 0.80× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,788 | 3,812 | 0.99× | Tie |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,971 | 3,747 | 1.86× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,910 | 4,340 | 1.82× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,330 | 3,596 | 1.20× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,233 | 4,223 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,215 | 7,189 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 8,965 | 8,538 | 1.05× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,840 | 6,229 | 0.94× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,732 | 7,410 | 0.91× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,191 | 7,350 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,869 | 8,257 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,240 | 5,950 | 1.05× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,088 | 7,077 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,194 | 4,323 | 1.20× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,071 | 4,704 | 1.08× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,189 | 3,777 | 1.11× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,591 | 4,519 | 1.02× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,752 | 4,138 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,759 | 5,122 | 0.93× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,072 | 3,102 | 0.99× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,326 | 4,012 | 0.83× | Loss |

Jsonifier wins 24 of the 40 results, ties 6 and loses 10, and wins by up to 1.86× (minified Twitter under MSVC, reused). Freshly allocated it wins 11, ties 3 and loses 6: the losses are Twitter Small on Linux/Clang in both forms (0.94× minified, 0.91× prettified), prettified Twitter Small on Linux/GCC (0.99×) and macOS/GCC (0.97×), and on macOS/Clang minified Twitter Small (0.80×) and full-size prettified Twitter (0.93×); the ties are prettified Twitter and minified Twitter Small on macOS/GCC, and prettified Twitter Small on macOS/Clang. The reused run has 13 wins, 3 ties and 4 losses: Twitter Small on Linux/Clang in both forms (0.94× and 0.91×), and prettified Twitter (0.93×) and Twitter Small (0.83×) on macOS/Clang; the ties are prettified Twitter Small on Linux/GCC and macOS/GCC, and minified Twitter Small on macOS/Clang. On the three x86 builds every full-size result is a win, by 1.05× to 1.86×; on the M2 Pro the full-size document is not a clear win, with prettified Twitter lost on macOS/Clang in both runs and tied on macOS/GCC freshly allocated. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on four of the five builds and ties Glaze on macOS/GCC (1,879 against 1,865 MB/s):

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,841 | 2,523 | 1.13× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,639 | 2,687 | 1.35× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,964 | 2,461 | 1.20× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,879 | 1,865 | 1.01× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,111 | 1,727 | 1.22× | Win |

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 107 of 110 minified documents and on 17 of 22 prettified documents on macOS/Clang; slower on 62 of 66 prettified x86 documents, and split on macOS/GCC (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 158 of 245 freshly allocated tests: all 25 POD-type tests, 107 of 110 minified documents and 17 of 22 prettified documents on macOS/Clang, while the two-stage path wins 62 of 66 prettified documents on x86 and 10 of 22 on macOS/GCC. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 238 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 78 of 88, and on partial reading it wins 24 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*