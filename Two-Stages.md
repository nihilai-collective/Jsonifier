# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 105 of the 110 minified documents, the tape is faster on 62 of the 66 prettified documents on x86, and on the Apple M2 Pro the prettified documents split by compiler: the fused path wins 18 of 22 under Clang, while under GCC it wins 11, the tape 9 and 2 are ties.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 4, 2026, with Jsonifier [28b18c4](https://github.com/nihilai-collective/jsonifier/commit/28b18c4), Glaze [1717bb0](https://github.com/stephenberry/glaze/commit/1717bb0), simdjson [ebd28cb](https://github.com/simdjson/simdjson/commit/ebd28cb) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Every x86 result converged. Six freshly allocated full-parse rows contain non-converged results (four on macOS/GCC, one of them with two, and two on macOS/Clang), and seven reused rows contain one each (five on macOS/GCC, two on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 162 tests, 7 are statistical ties, and the two-stage path wins 76. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 | 2 | 24 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 37 | 3 | 9 | 45 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 45 | 1 | 3 | 47 of 49 |
| **Aggregate** | **162** | **7** | **76** | **239 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 18 / 2 / 2 | 21 / 1 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 0 / 1 / 21 | 14 / 2 / 6 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 15 / 1 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 46 / 1 / 2 | 37 / 0 / 12 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 44 / 0 / 5 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 48 / 0 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 / 0 / 2 | 47 / 0 / 2 |
| **Aggregate** | **239 / 1 / 5** | **224 / 0 / 21** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 1 | 24 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 35 | 7 | 7 | 44 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 44 | 0 | 5 | 47 of 49 |
| **Aggregate** | **160** | **8** | **77** | **238 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 19 / 1 / 2 | 20 / 1 / 1 |
| Prettified corpus documents | 1 / 0 / 10 | 0 / 0 / 22 | 15 / 3 / 4 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 1 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 13 / 2 / 7 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 46 / 0 / 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 48 / 0 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 48 / 0 / 1 | 48 / 0 / 1 |
| **Aggregate** | **242 / 1 / 2** | **228 / 1 / 16** |

Across 245 tests the fused path wins 160, ties 8 and loses 77 to the two-stage path, and the x86 split holds: the tape wins 32 of the 33 full-size prettified documents (the fused path takes Apache Builds under MSVC) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 30 of the 33 full-size minified ones (on Linux/GCC the tape takes Canada, 1,256 against 1,207 MB/s, and Github Events, 2,297 against 2,201, and Marine IK Reverse is a tie). On the M2 Pro the fused path wins 41 of the 44 minified documents, with 2 ties and 1 tape win (Random on macOS/GCC), and 28 of the 44 prettified ones, with 5 ties and 11 tape wins. Against simdjson, the faster Jsonifier path wins 242, ties 1 and loses 2, and the fused path alone wins 228, ties 1 and loses 16.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 525 | 257 | 92 | 2.04× | Fused | Win |
| Double (POD) | 695 | 514 | 149 | 1.35× | Fused | Win |
| Int64 (POD) | 1,692 | 898 | 320 | 1.89× | Fused | Win |
| String (POD) | 1,012 | 798 | 557 | 1.27× | Fused | Win |
| Uint64 (POD) | 1,544 | 908 | 320 | 1.70× | Fused | Win |
| Apache Builds (minified) | 945 | 793 | 676 | 1.19× | Fused | Win |
| Canada (minified) | 699 | 656 | 432 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,086 | 1,037 | 877 | 1.05× | Fused | Win |
| Canada (prettified) | 1,414 | 1,504 | 953 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,803 | 1,279 | 591 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 2,429 | 2,693 | 1,246 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,172 | 1,041 | 628 | 1.13× | Fused | Win |
| Discord (prettified) | 1,280 | 1,465 | 907 | 0.87× | Two-stage | Win |
| Github Events (minified) | 1,006 | 875 | 554 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,577 | 1,188 | 499 | 1.33× | Fused | Win |
| Github Events (prettified) | 1,042 | 1,071 | 674 | 0.97× | Two-stage | Win |
| Google Maps Response (prettified) | 2,309 | 2,374 | 1,004 | 0.97× | Two-stage | Win |
| Instruments (minified) | 2,019 | 1,450 | 706 | 1.39× | Fused | Win |
| Instruments (prettified) | 1,954 | 2,294 | 1,139 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 597 | 533 | 187 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 1,842 | 1,900 | 635 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 605 | 533 | 342 | 1.14× | Fused | Win |
| Marine IK (prettified) | 1,889 | 1,916 | 1,163 | 0.99× | Two-stage | Win |
| Random (minified) | 768 | 718 | 529 | 1.07× | Fused | Win |
| Random (prettified) | 1,008 | 1,107 | 792 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,236 | 1,090 | 811 | 1.13× | Fused | Win |
| Twitter (prettified) | 1,254 | 1,420 | 1,042 | 0.88× | Two-stage | Win |
| Apache Builds Small (minified) | 1,322 | 978 | 760 | 1.35× | Fused | Win |
| Canada Small (minified) | 651 | 612 | 346 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,337 | 1,222 | 952 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,343 | 1,440 | 818 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 876 | 727 | 509 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,140 | 1,324 | 880 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,119 | 927 | 650 | 1.21× | Fused | Win |
| Discord Small (prettified) | 1,149 | 1,229 | 898 | 0.93× | Two-stage | Win |
| Github Events Small (minified) | 1,243 | 854 | 574 | 1.46× | Fused | Win |
| Google Maps Response Small (minified) | 1,570 | 1,131 | 528 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 1,180 | 1,073 | 717 | 1.10× | Fused | Win |
| Google Maps Response Small (prettified) | 2,083 | 2,249 | 1,061 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,110 | 1,482 | 721 | 1.42× | Fused | Win |
| Instruments Small (prettified) | 1,866 | 2,277 | 1,154 | 0.82× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 487 | 439 | 142 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,440 | 1,596 | 536 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 521 | 456 | 297 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,516 | 1,668 | 1,050 | 0.91× | Two-stage | Win |
| Random Small (minified) | 919 | 792 | 519 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,155 | 1,229 | 792 | 0.94× | Two-stage | Win |
| Twitter Small (minified) | 1,495 | 1,280 | 1,172 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 1,417 | 1,664 | 1,454 | 0.85× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,180 | 702 | 287 | 3.11× | Fused | Win |
| Double (POD) | 1,402 | 803 | 373 | 1.75× | Fused | Win |
| Int64 (POD) | 3,379 | 1,699 | 801 | 1.99× | Fused | Win |
| String (POD) | 1,982 | 1,364 | 1,419 | 1.45× | Fused | Win |
| Uint64 (POD) | 3,503 | 1,713 | 788 | 2.05× | Fused | Win |
| Apache Builds (minified) | 1,788 | 1,553 | 1,254 | 1.15× | Fused | Win |
| Canada (minified) | 1,099 | 933 | 682 | 1.18× | Fused | Win |
| Apache Builds (prettified) | 1,698 | 2,000 | 1,601 | 0.85× | Two-stage | Win |
| Canada (prettified) | 2,088 | 2,142 | 1,522 | 0.98× | Tie | Win |
| CitmCatalog (minified) | 2,375 | 1,804 | 1,210 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 2,940 | 3,731 | 2,574 | 0.79× | Two-stage | Win |
| Discord (minified) | 2,240 | 1,854 | 1,433 | 1.21× | Fused | Win |
| Discord (prettified) | 1,822 | 2,681 | 2,074 | 0.68× | Two-stage | Win |
| Github Events (minified) | 1,691 | 1,636 | 1,608 | 1.03× | Fused | Win |
| Google Maps Response (minified) | 2,756 | 2,053 | 1,384 | 1.34× | Fused | Win |
| Github Events (prettified) | 1,557 | 1,959 | 2,193 | 0.79× | Two-stage | Loss |
| Google Maps Response (prettified) | 3,089 | 3,741 | 2,738 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,437 | 1,995 | 1,740 | 1.22× | Fused | Win |
| Instruments (prettified) | 2,240 | 3,113 | 2,788 | 0.72× | Two-stage | Win |
| Marine IK Reverse (minified) | 833 | 740 | 592 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 2,534 | 2,615 | 2,155 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 847 | 761 | 595 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,531 | 2,666 | 2,153 | 0.95× | Two-stage | Win |
| Random (minified) | 1,227 | 1,077 | 1,069 | 1.14× | Fused | Win |
| Random (prettified) | 1,398 | 1,690 | 1,576 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,889 | 1,738 | 1,705 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,705 | 2,214 | 2,222 | 0.77× | Two-stage | Tie |
| Apache Builds Small (minified) | 2,748 | 2,387 | 1,671 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,079 | 979 | 716 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,030 | 2,915 | 2,106 | 0.70× | Two-stage | Win |
| Canada Small (prettified) | 2,098 | 2,260 | 1,728 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,890 | 1,554 | 1,277 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 1,958 | 2,660 | 2,184 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,206 | 1,875 | 1,556 | 1.18× | Fused | Win |
| Discord Small (prettified) | 1,808 | 2,535 | 2,091 | 0.71× | Two-stage | Win |
| Github Events Small (minified) | 2,365 | 2,132 | 2,100 | 1.11× | Fused | Win |
| Google Maps Response Small (minified) | 2,668 | 2,023 | 1,394 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 2,012 | 2,630 | 2,581 | 0.77× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,979 | 3,806 | 2,779 | 0.78× | Two-stage | Win |
| Instruments Small (minified) | 2,651 | 2,082 | 1,670 | 1.27× | Fused | Win |
| Instruments Small (prettified) | 2,277 | 3,289 | 2,680 | 0.69× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 839 | 760 | 592 | 1.10× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,239 | 2,681 | 2,139 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 944 | 823 | 595 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,371 | 2,896 | 2,155 | 0.82× | Two-stage | Win |
| Random Small (minified) | 1,922 | 1,684 | 1,207 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,786 | 2,585 | 1,825 | 0.69× | Two-stage | Win |
| Twitter Small (minified) | 2,706 | 2,390 | 2,367 | 1.13× | Fused | Win |
| Twitter Small (prettified) | 2,124 | 2,827 | 2,984 | 0.75× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,224 | 567 | 226 | 3.92× | Fused | Win |
| Double (POD) | 1,236 | 628 | 300 | 1.97× | Fused | Win |
| Int64 (POD) | 3,336 | 1,445 | 703 | 2.31× | Fused | Win |
| String (POD) | 1,795 | 1,228 | 1,269 | 1.46× | Fused | Win |
| Uint64 (POD) | 3,496 | 1,428 | 715 | 2.45× | Fused | Win |
| Apache Builds (minified) | 1,744 | 1,500 | 1,399 | 1.16× | Fused | Win |
| Canada (minified) | 1,000 | 1,032 | 856 | 0.97× | Two-stage | Win |
| Apache Builds (prettified) | 1,731 | 1,935 | 1,778 | 0.89× | Two-stage | Win |
| Canada (prettified) | 2,121 | 2,271 | 1,944 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,456 | 1,897 | 1,387 | 1.29× | Fused | Win |
| CitmCatalog (prettified) | 3,291 | 3,638 | 2,875 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,264 | 1,835 | 1,420 | 1.23× | Fused | Win |
| Discord (prettified) | 2,250 | 2,546 | 2,076 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,624 | 1,773 | 1,442 | 0.92× | Two-stage | Win |
| Google Maps Response (minified) | 2,648 | 1,941 | 1,109 | 1.36× | Fused | Win |
| Github Events (prettified) | 1,625 | 2,102 | 1,735 | 0.77× | Two-stage | Win |
| Google Maps Response (prettified) | 3,517 | 3,591 | 2,202 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,277 | 2,300 | 1,399 | 1.42× | Fused | Win |
| Instruments (prettified) | 3,044 | 3,403 | 2,266 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 742 | 743 | 605 | 1.00× | Tie | Win |
| Marine IK Reverse (prettified) | 2,408 | 2,620 | 2,216 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 752 | 748 | 601 | 1.01× | Fused | Win |
| Marine IK (prettified) | 2,466 | 2,645 | 2,196 | 0.93× | Two-stage | Win |
| Random (minified) | 1,152 | 1,153 | 1,049 | 1.00× | Tie | Win |
| Random (prettified) | 1,369 | 1,746 | 1,575 | 0.78× | Two-stage | Win |
| Twitter (minified) | 1,962 | 1,691 | 1,362 | 1.16× | Fused | Win |
| Twitter (prettified) | 1,773 | 2,153 | 1,795 | 0.82× | Two-stage | Win |
| Apache Builds Small (minified) | 2,534 | 2,196 | 1,660 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,081 | 986 | 780 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,300 | 2,606 | 2,057 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,106 | 2,290 | 1,873 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,825 | 1,501 | 1,167 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,115 | 2,486 | 2,027 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,031 | 1,689 | 1,274 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,919 | 2,214 | 1,706 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,146 | 1,768 | 1,943 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,410 | 1,820 | 1,130 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 2,109 | 2,282 | 2,419 | 0.92× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,216 | 3,392 | 2,248 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,296 | 2,222 | 1,273 | 1.48× | Fused | Win |
| Instruments Small (prettified) | 2,873 | 3,363 | 2,084 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 845 | 752 | 578 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,331 | 2,644 | 2,081 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 949 | 784 | 592 | 1.21× | Fused | Win |
| Marine IK Small (prettified) | 2,400 | 2,768 | 2,161 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,704 | 1,583 | 1,168 | 1.08× | Fused | Win |
| Random Small (prettified) | 1,862 | 2,369 | 1,762 | 0.79× | Two-stage | Win |
| Twitter Small (minified) | 2,932 | 2,228 | 1,714 | 1.32× | Fused | Win |
| Twitter Small (prettified) | 2,353 | 2,806 | 2,156 | 0.84× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,760 | 381 | 176 | 4.62× | Fused | Win |
| Double (POD) | 981 | 429 | 122 | 2.29× | Fused | Win |
| Int64 (POD) | 2,647 | 765 | 459 | 3.46× | Fused | Win |
| String (POD) | 1,404 | 702 | 650 | 2.00× | Fused | Win |
| Uint64 (POD) | 2,407 | 898 | 455 | 2.68× | Fused | Win |
| Apache Builds (minified) | 1,255 | 968 | 758 | 1.30× | Fused | Win |
| Canada (minified) | 780 | 757 | 531 | 1.03× | Tie | Win |
| Apache Builds (prettified) | 1,175 | 1,248 | 806 | 0.94× | Two-stage | Win |
| Canada (prettified) | 1,523 | 1,546 | 1,156 | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,828 | 1,231 | 1,119 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 2,754 | 2,675 | 1,974 | 1.03× | Fused | Win |
| Discord (minified) | 1,837 | 1,291 | 1,017 | 1.42× | Fused | Win |
| Discord (prettified) | 1,929 | 1,603 | 1,363 | 1.20× | Fused | Win |
| Github Events (minified) | 1,241 | 1,104 | 832 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 1,805 | 1,256 | 701 | 1.44× | Fused | Win |
| Github Events (prettified) | 1,309 | 1,415 | 1,170 | 0.92× | Two-stage | Win |
| Google Maps Response (prettified) | 2,215 (n/c) | 2,253 | 1,073 | 0.98× | Tie | Win |
| Instruments (minified) | 2,529 | 1,510 | 1,127 | 1.68× | Fused | Win |
| Instruments (prettified) | 2,601 | 2,230 | 1,871 | 1.17× | Fused | Win |
| Marine IK Reverse (minified) | 629 | 532 (n/c) | 407 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 1,695 (n/c) | 1,822 | 1,627 (n/c) | 0.93× | Two-stage | Win |
| Marine IK (minified) | 637 | 501 | 407 | 1.27× | Fused | Win |
| Marine IK (prettified) | 1,731 | 1,906 | 1,600 (n/c) | 0.91× | Two-stage | Win |
| Random (minified) | 995 | 834 | 648 | 1.19× | Fused | Win |
| Random (prettified) | 1,070 | 1,204 | 943 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,900 | 1,389 | 995 | 1.37× | Fused | Win |
| Twitter (prettified) | 1,772 | 2,057 | 1,368 | 0.86× | Two-stage | Win |
| Apache Builds Small (minified) | 1,646 | 1,468 | 787 | 1.12× | Fused | Win |
| Canada Small (minified) | 803 | 715 | 403 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 1,663 | 1,524 | 1,014 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,554 | 1,401 | 989 | 1.11× | Fused | Win |
| CitmCatalog Small (minified) | 1,113 | 766 | 720 | 1.45× | Fused | Win |
| CitmCatalog Small (prettified) | 1,469 | 1,290 | 1,101 | 1.14× | Fused | Win |
| Discord Small (minified) | 1,552 | 1,220 | 879 | 1.27× | Fused | Win |
| Discord Small (prettified) | 1,664 | 1,600 | 1,112 | 1.04× | Fused | Win |
| Github Events Small (minified) | 1,347 | 1,159 | 1,051 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 1,704 | 1,199 | 754 | 1.42× | Fused | Win |
| Github Events Small (prettified) | 1,075 | 1,485 | 1,347 | 0.72× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,274 | 2,194 | 1,446 | 1.04× | Fused | Win |
| Instruments Small (minified) | 2,648 | 1,592 | 1,097 | 1.66× | Fused | Win |
| Instruments Small (prettified) | 2,516 | 2,260 | 1,766 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 600 | 486 | 371 | 1.23× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,556 | 1,719 | 1,333 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 673 | 552 | 367 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 1,542 | 1,822 | 1,307 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,046 | 975 | 724 | 1.07× | Fused | Win |
| Random Small (prettified) | 1,351 | 1,319 | 977 | 1.02× | Fused | Win |
| Twitter Small (minified) | 2,114 | 1,604 | 1,087 | 1.32× | Fused | Win |
| Twitter Small (prettified) | 2,019 | 1,838 | 1,246 | 1.10× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,242 | 270 | 158 | 4.60× | Fused | Win |
| Double (POD) | 762 | 319 | 166 | 2.39× | Fused | Win |
| Int64 (POD) | 1,950 | 777 | 407 | 2.51× | Fused | Win |
| String (POD) | 1,191 | 559 | 779 | 2.13× | Fused | Win |
| Uint64 (POD) | 1,656 | 568 | 398 | 2.92× | Fused | Win |
| Apache Builds (minified) | 1,320 | 982 | 877 | 1.34× | Fused | Win |
| Canada (minified) | 851 | 752 | 486 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,536 | 1,393 | 1,042 | 1.10× | Fused | Win |
| Canada (prettified) | 1,520 | 1,440 | 1,094 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,075 | 1,370 | 925 | 1.51× | Fused | Win |
| CitmCatalog (prettified) | 2,941 | 2,660 | 1,991 | 1.11× | Fused | Win |
| Discord (minified) | 2,431 | 1,276 | 1,052 | 1.91× | Fused | Win |
| Discord (prettified) | 2,292 | 1,484 | 1,804 | 1.54× | Fused | Win |
| Github Events (minified) | 1,789 | 1,382 | 1,514 | 1.29× | Fused | Win |
| Google Maps Response (minified) | 2,202 | 1,147 | 782 | 1.92× | Fused | Win |
| Github Events (prettified) | 1,697 | 1,596 | 1,686 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 3,117 | 2,390 | 2,066 | 1.30× | Fused | Win |
| Instruments (minified) | 2,532 | 1,454 | 1,301 | 1.74× | Fused | Win |
| Instruments (prettified) | 2,719 | 2,417 | 1,965 | 1.12× | Fused | Win |
| Marine IK Reverse (minified) | 721 | 621 | 447 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,113 | 2,074 | 1,253 (n/c) | 1.02× | Fused | Win |
| Marine IK (minified) | 729 | 616 | 448 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,186 | 1,619 | 1,194 | 1.35× | Fused | Win |
| Random (minified) | 1,222 | 923 | 823 | 1.32× | Fused | Win |
| Random (prettified) | 1,582 | 1,183 | 1,240 | 1.34× | Fused | Win |
| Twitter (minified) | 2,138 | 1,683 | 1,585 | 1.27× | Fused | Win |
| Twitter (prettified) | 2,111 | 2,064 | 2,077 | 1.02× | Fused | Win |
| Apache Builds Small (minified) | 1,521 | 1,258 | 964 | 1.21× | Fused | Win |
| Canada Small (minified) | 770 | 715 | 466 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,517 | 1,618 | 1,052 | 0.94× | Two-stage | Win |
| Canada Small (prettified) | 1,585 | 1,379 | 947 | 1.15× | Fused | Win |
| CitmCatalog Small (minified) | 1,298 | 989 | 777 | 1.31× | Fused | Win |
| CitmCatalog Small (prettified) | 1,751 | 1,501 | 1,466 | 1.17× | Fused | Win |
| Discord Small (minified) | 2,021 | 1,072 | 1,244 | 1.89× | Fused | Win |
| Discord Small (prettified) | 2,111 | 1,562 | 1,675 | 1.35× | Fused | Win |
| Github Events Small (minified) | 1,483 | 1,356 | 1,361 | 1.09× | Fused | Win |
| Google Maps Response Small (minified) | 2,340 | 1,201 | 871 | 1.95× | Fused | Win |
| Github Events Small (prettified) | 1,878 | 1,458 | 1,940 | 1.29× | Fused | Loss |
| Google Maps Response Small (prettified) | 2,755 | 2,170 | 1,888 | 1.27× | Fused | Win |
| Instruments Small (minified) | 2,387 | 1,440 | 1,003 | 1.66× | Fused | Win |
| Instruments Small (prettified) | 2,455 | 2,426 | 1,805 | 1.01× | Tie | Win |
| Marine IK Reverse Small (minified) | 565 | 459 | 329 | 1.23× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,744 | 1,864 | 1,313 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 629 | 549 | 343 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 1,949 | 1,738 | 1,295 | 1.12× | Fused | Win |
| Random Small (minified) | 1,552 | 1,136 | 952 | 1.37× | Fused | Win |
| Random Small (prettified) | 1,652 | 1,309 | 1,312 | 1.26× | Fused | Win |
| Twitter Small (minified) | 2,286 | 1,742 | 1,243 (n/c) | 1.31× | Fused | Win |
| Twitter Small (prettified) | 1,787 | 1,971 | 2,083 | 0.91× | Two-stage | Loss |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 549 | 268 | 93 | 2.04× | Fused | Win |
| Double (POD) | 855 | 588 | 156 | 1.45× | Fused | Win |
| Int64 (POD) | 2,086 | 968 | 334 | 2.15× | Fused | Win |
| String (POD) | 1,726 | 1,192 | 731 | 1.45× | Fused | Win |
| Uint64 (POD) | 1,843 | 962 | 335 | 1.92× | Fused | Win |
| Apache Builds (minified) | 1,581 | 1,197 | 956 | 1.32× | Fused | Win |
| Canada (minified) | 842 | 787 | 484 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,649 | 1,542 | 1,202 | 1.07× | Fused | Win |
| Canada (prettified) | 1,628 | 1,788 | 1,054 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,188 | 1,483 | 622 | 1.48× | Fused | Win |
| CitmCatalog (prettified) | 2,709 | 3,062 | 1,258 | 0.88× | Two-stage | Win |
| Discord (minified) | 1,719 | 1,403 | 778 | 1.23× | Fused | Win |
| Discord (prettified) | 1,652 | 1,992 | 1,108 | 0.83× | Two-stage | Win |
| Github Events (minified) | 1,565 | 1,293 | 686 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 1,930 | 1,326 | 541 | 1.46× | Fused | Win |
| Github Events (prettified) | 1,487 | 1,523 | 820 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,539 | 2,635 | 1,089 | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,539 | 1,795 | 795 | 1.41× | Fused | Win |
| Instruments (prettified) | 2,188 | 2,692 | 1,269 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 650 | 565 | 195 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,011 | 2,080 | 650 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 656 | 589 | 355 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,022 | 2,091 | 1,221 | 0.97× | Two-stage | Win |
| Random (minified) | 1,053 | 965 | 655 | 1.09× | Fused | Win |
| Random (prettified) | 1,300 | 1,483 | 972 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,772 | 1,542 | 1,020 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,672 | 1,976 | 1,321 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 1,725 | 1,224 | 980 | 1.41× | Fused | Win |
| Canada Small (minified) | 815 | 756 | 390 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,651 | 1,544 | 1,220 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,605 | 1,769 | 910 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,234 | 952 | 577 | 1.30× | Fused | Win |
| CitmCatalog Small (prettified) | 1,443 | 1,689 | 991 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 1,495 | 1,214 | 851 | 1.23× | Fused | Win |
| Discord Small (prettified) | 1,378 | 1,623 | 1,134 | 0.85× | Two-stage | Win |
| Github Events Small (minified) | 1,791 | 1,163 | 653 | 1.54× | Fused | Win |
| Google Maps Response Small (minified) | 1,933 | 1,301 | 576 | 1.49× | Fused | Win |
| Github Events Small (prettified) | 1,565 | 1,412 | 828 | 1.11× | Fused | Win |
| Google Maps Response Small (prettified) | 2,381 | 2,618 | 1,125 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 2,423 | 1,651 | 771 | 1.47× | Fused | Win |
| Instruments Small (prettified) | 1,985 | 2,464 | 1,245 | 0.81× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 605 | 531 | 145 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,688 | 1,966 | 550 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 646 | 569 | 331 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,764 | 2,037 | 1,161 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,111 | 939 | 636 | 1.18× | Fused | Win |
| Random Small (prettified) | 1,346 | 1,461 | 960 | 0.92× | Two-stage | Win |
| Twitter Small (minified) | 1,808 | 1,522 | 1,261 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 1,603 | 1,911 | 1,603 | 0.84× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,362 | 705 | 286 | 3.35× | Fused | Win |
| Double (POD) | 1,482 | 844 | 379 | 1.76× | Fused | Win |
| Int64 (POD) | 3,592 | 1,800 | 805 | 2.00× | Fused | Win |
| String (POD) | 3,051 | 1,854 | 1,980 | 1.65× | Fused | Win |
| Uint64 (POD) | 3,805 | 1,768 | 793 | 2.15× | Fused | Win |
| Apache Builds (minified) | 3,002 | 2,443 | 1,742 | 1.23× | Fused | Win |
| Canada (minified) | 1,348 | 1,112 | 794 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 2,399 | 2,965 | 2,208 | 0.81× | Two-stage | Win |
| Canada (prettified) | 2,491 | 2,560 | 1,744 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,977 | 2,160 | 1,410 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 3,284 | 4,362 | 2,971 | 0.75× | Two-stage | Win |
| Discord (minified) | 3,259 | 2,519 | 1,882 | 1.29× | Fused | Win |
| Discord (prettified) | 2,194 | 3,511 | 2,608 | 0.62× | Two-stage | Win |
| Github Events (minified) | 2,337 | 2,198 | 2,050 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 3,160 | 2,277 | 1,483 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,987 | 2,598 | 2,441 | 0.76× | Two-stage | Win |
| Google Maps Response (prettified) | 3,269 | 4,063 | 2,946 | 0.80× | Two-stage | Win |
| Instruments (minified) | 2,883 | 2,287 | 1,911 | 1.26× | Fused | Win |
| Instruments (prettified) | 2,426 | 3,546 | 3,041 | 0.68× | Two-stage | Win |
| Marine IK Reverse (minified) | 925 | 821 | 646 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 2,735 | 2,865 | 2,317 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 955 | 837 | 641 | 1.14× | Fused | Win |
| Marine IK (prettified) | 2,776 | 2,908 | 2,322 | 0.95× | Two-stage | Win |
| Random (minified) | 1,717 | 1,489 | 1,472 | 1.15× | Fused | Win |
| Random (prettified) | 1,762 | 2,249 | 2,197 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,436 | 2,315 | 2,154 | 1.05× | Fused | Win |
| Twitter (prettified) | 2,004 | 2,905 | 2,776 | 0.69× | Two-stage | Win |
| Apache Builds Small (minified) | 3,393 | 3,068 | 1,979 | 1.11× | Fused | Win |
| Canada Small (minified) | 1,367 | 1,214 | 821 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 2,589 | 3,574 | 2,541 | 0.72× | Two-stage | Win |
| Canada Small (prettified) | 2,481 | 2,826 | 1,949 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,367 | 1,862 | 1,456 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,233 | 3,124 | 2,532 | 0.71× | Two-stage | Win |
| Discord Small (minified) | 2,996 | 2,397 | 1,945 | 1.25× | Fused | Win |
| Discord Small (prettified) | 2,164 | 3,144 | 2,575 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 3,072 | 2,737 | 2,598 | 1.12× | Fused | Win |
| Google Maps Response Small (minified) | 3,060 | 2,270 | 1,495 | 1.35× | Fused | Win |
| Github Events Small (prettified) | 2,520 | 3,299 | 3,208 | 0.76× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,175 | 4,248 | 2,979 | 0.75× | Two-stage | Win |
| Instruments Small (minified) | 2,863 | 2,210 | 1,793 | 1.30× | Fused | Win |
| Instruments Small (prettified) | 2,373 | 3,453 | 2,920 | 0.69× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,032 | 902 | 675 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,594 | 3,156 | 2,411 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 1,171 | 998 | 674 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,726 | 3,454 | 2,388 | 0.79× | Two-stage | Win |
| Random Small (minified) | 2,441 | 2,087 | 1,531 | 1.17× | Fused | Win |
| Random Small (prettified) | 2,098 | 3,176 | 2,285 | 0.66× | Two-stage | Win |
| Twitter Small (minified) | 2,979 | 2,719 | 2,498 | 1.10× | Fused | Win |
| Twitter Small (prettified) | 2,286 | 3,294 | 3,233 | 0.69× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,488 | 574 | 229 | 4.33× | Fused | Win |
| Double (POD) | 1,331 | 661 | 306 | 2.01× | Fused | Win |
| Int64 (POD) | 3,762 | 1,527 | 739 | 2.46× | Fused | Win |
| String (POD) | 2,724 | 1,573 | 1,712 | 1.73× | Fused | Win |
| Uint64 (POD) | 3,846 | 1,503 | 722 | 2.56× | Fused | Win |
| Apache Builds (minified) | 2,754 | 2,241 | 1,949 | 1.23× | Fused | Win |
| Canada (minified) | 1,207 | 1,256 | 1,041 | 0.96× | Two-stage | Win |
| Apache Builds (prettified) | 2,530 | 2,783 | 2,509 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,500 | 2,714 | 2,293 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 3,106 | 2,272 | 1,666 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 3,738 | 4,246 | 3,437 | 0.88× | Two-stage | Win |
| Discord (minified) | 3,351 | 2,381 | 1,788 | 1.41× | Fused | Win |
| Discord (prettified) | 2,821 | 3,367 | 2,604 | 0.84× | Two-stage | Win |
| Github Events (minified) | 2,201 | 2,297 | 1,754 | 0.96× | Two-stage | Win |
| Google Maps Response (minified) | 3,094 | 2,175 | 1,168 | 1.42× | Fused | Win |
| Github Events (prettified) | 2,194 | 2,704 | 2,077 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 3,836 | 3,865 | 2,318 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,925 | 2,617 | 1,499 | 1.50× | Fused | Win |
| Instruments (prettified) | 3,331 | 3,789 | 2,397 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 820 | 823 | 671 | 1.00× | Tie | Win |
| Marine IK Reverse (prettified) | 2,603 | 2,861 | 2,433 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 836 | 828 | 675 | 1.01× | Fused | Win |
| Marine IK (prettified) | 2,662 | 2,899 | 2,414 | 0.92× | Two-stage | Win |
| Random (minified) | 1,557 | 1,548 | 1,405 | 1.01× | Fused | Win |
| Random (prettified) | 1,709 | 2,310 | 2,116 | 0.74× | Two-stage | Win |
| Twitter (minified) | 2,551 | 2,119 | 1,642 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,102 | 2,726 | 2,140 | 0.77× | Two-stage | Win |
| Apache Builds Small (minified) | 2,991 | 2,532 | 1,921 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,393 | 1,246 | 901 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,624 | 3,042 | 2,327 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 2,523 | 2,808 | 2,144 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,248 | 1,781 | 1,353 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 2,416 | 2,918 | 2,280 | 0.83× | Two-stage | Win |
| Discord Small (minified) | 2,678 | 2,107 | 1,586 | 1.27× | Fused | Win |
| Discord Small (prettified) | 2,403 | 2,696 | 2,131 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,698 | 2,213 | 2,391 | 1.22× | Fused | Win |
| Google Maps Response Small (minified) | 2,795 | 2,047 | 1,243 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 2,545 | 2,818 | 2,956 | 0.90× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,520 | 3,718 | 2,480 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,695 | 2,403 | 1,347 | 1.54× | Fused | Win |
| Instruments Small (prettified) | 3,122 | 3,583 | 2,192 | 0.87× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,033 | 896 | 656 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,670 | 3,115 | 2,307 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,197 | 956 | 639 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,831 | 3,278 | 2,329 | 0.86× | Two-stage | Win |
| Random Small (minified) | 2,140 | 1,941 | 1,442 | 1.10× | Fused | Win |
| Random Small (prettified) | 2,202 | 2,852 | 2,121 | 0.77× | Two-stage | Win |
| Twitter Small (minified) | 3,309 | 2,541 | 1,825 | 1.30× | Fused | Win |
| Twitter Small (prettified) | 2,553 | 3,155 | 2,310 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,988 | 342 | 169 | 5.82× | Fused | Win |
| Double (POD) | 1,177 | 353 | 166 | 3.34× | Fused | Win |
| Int64 (POD) | 3,345 | 994 | 349 | 3.37× | Fused | Win |
| String (POD) | 2,522 | 986 | 958 | 2.56× | Fused | Win |
| Uint64 (POD) | 2,683 | 965 | 372 | 2.78× | Fused | Win |
| Apache Builds (minified) | 2,136 | 1,296 | 979 | 1.65× | Fused | Win |
| Canada (minified) | 962 | 948 | 587 | 1.02× | Tie | Win |
| Apache Builds (prettified) | 1,573 | 1,572 | 1,051 | 1.00× | Tie | Win |
| Canada (prettified) | 1,867 | 1,821 | 1,244 | 1.03× | Tie | Win |
| CitmCatalog (minified) | 2,440 | 1,522 | 1,175 | 1.60× | Fused | Win |
| CitmCatalog (prettified) | 2,936 | 2,542 | 2,123 | 1.16× | Fused | Win |
| Discord (minified) | 2,492 | 1,642 | 1,188 | 1.52× | Fused | Win |
| Discord (prettified) | 2,046 | 2,216 | 1,638 | 0.92× | Two-stage | Win |
| Github Events (minified) | 2,065 | 1,304 | 1,046 | 1.58× | Fused | Win |
| Google Maps Response (minified) | 2,360 | 1,510 | 837 | 1.56× | Fused | Win |
| Github Events (prettified) | 1,621 | 1,622 | 1,244 | 1.00× | Tie | Win |
| Google Maps Response (prettified) | 2,738 | 2,610 | 1,467 | 1.05× | Fused | Win |
| Instruments (minified) | 2,924 | 1,875 | 1,245 | 1.56× | Fused | Win |
| Instruments (prettified) | 3,094 | 2,263 | 1,902 (n/c) | 1.37× | Fused | Win |
| Marine IK Reverse (minified) | 742 | 603 (n/c) | 465 | 1.23× | Fused | Win |
| Marine IK Reverse (prettified) | 2,098 (n/c) | 1,838 | 1,544 | 1.14× | Fused | Win |
| Marine IK (minified) | 735 | 567 | 449 | 1.30× | Fused | Win |
| Marine IK (prettified) | 1,966 | 2,189 (n/c) | 1,573 | 0.90× | Two-stage | Win |
| Random (minified) | 1,294 | 1,327 | 874 | 0.98× | Two-stage | Win |
| Random (prettified) | 1,573 | 1,511 | 1,275 | 1.04× | Fused | Win |
| Twitter (minified) | 2,173 | 2,049 | 1,131 | 1.06× | Fused | Win |
| Twitter (prettified) | 2,378 | 2,591 | 1,554 | 0.92× | Two-stage | Win |
| Apache Builds Small (minified) | 2,225 | 1,895 | 1,248 | 1.17× | Fused | Win |
| Canada Small (minified) | 898 | 887 | 484 | 1.01× | Tie | Win |
| Apache Builds Small (prettified) | 2,012 | 1,968 | 1,162 | 1.02× | Tie | Win |
| Canada Small (prettified) | 1,950 | 1,464 | 1,040 | 1.33× | Fused | Win |
| CitmCatalog Small (minified) | 1,570 | 1,232 | 894 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,187 | 1,870 | 1,686 | 1.17× | Fused | Win |
| Discord Small (minified) | 2,213 | 1,605 | 1,158 | 1.38× | Fused | Win |
| Discord Small (prettified) | 2,166 | 1,966 | 1,510 | 1.10× | Fused | Win |
| Github Events Small (minified) | 1,737 | 1,640 | 1,035 | 1.06× | Fused | Win |
| Google Maps Response Small (minified) | 2,192 | 1,410 | 819 | 1.55× | Fused | Win |
| Github Events Small (prettified) | 1,275 | 1,748 | 1,753 | 0.73× | Two-stage | Tie |
| Google Maps Response Small (prettified) | 2,772 | 2,563 | 1,515 | 1.08× | Fused | Win |
| Instruments Small (minified) | 2,982 | 1,726 | 1,219 | 1.73× | Fused | Win |
| Instruments Small (prettified) | 2,738 | 2,426 | 1,922 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 787 | 620 | 426 | 1.27× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,807 | 2,033 | 1,448 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 897 | 693 | 398 | 1.29× | Fused | Win |
| Marine IK Small (prettified) | 2,098 | 2,275 | 1,532 | 0.92× | Two-stage | Win |
| Random Small (minified) | 1,555 | 1,146 | 871 | 1.36× | Fused | Win |
| Random Small (prettified) | 1,859 | 1,615 | 1,183 | 1.15× | Fused | Win |
| Twitter Small (minified) | 2,673 | 2,026 | 1,056 | 1.32× | Fused | Win |
| Twitter Small (prettified) | 1,943 | 2,015 (n/c) | 1,234 | 0.96× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,112 | 288 | 151 | 3.86× | Fused | Win |
| Double (POD) | 1,002 | 304 | 183 | 3.30× | Fused | Win |
| Int64 (POD) | 2,132 (n/c) | 724 | 396 | 2.95× | Fused | Win |
| String (POD) | 2,188 | 827 | 909 | 2.65× | Fused | Win |
| Uint64 (POD) | 2,288 | 751 | 387 | 3.05× | Fused | Win |
| Apache Builds (minified) | 1,933 | 1,335 | 1,113 | 1.45× | Fused | Win |
| Canada (minified) | 1,065 | 914 | 631 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 2,244 | 1,822 | 1,454 | 1.23× | Fused | Win |
| Canada (prettified) | 1,851 | 1,689 | 1,237 | 1.10× | Fused | Win |
| CitmCatalog (minified) | 2,601 | 1,580 | 1,078 | 1.65× | Fused | Win |
| CitmCatalog (prettified) | 3,359 | 3,021 | 2,094 | 1.11× | Fused | Win |
| Discord (minified) | 3,076 | 1,473 | 1,501 | 2.09× | Fused | Win |
| Discord (prettified) | 2,475 | 2,116 | 2,026 | 1.17× | Fused | Win |
| Github Events (minified) | 2,516 | 1,806 | 1,818 | 1.39× | Fused | Win |
| Google Maps Response (minified) | 2,483 | 1,322 | 1,009 | 1.88× | Fused | Win |
| Github Events (prettified) | 2,358 | 2,133 | 2,259 | 1.11× | Fused | Win |
| Google Maps Response (prettified) | 3,225 | 2,440 | 2,086 | 1.32× | Fused | Win |
| Instruments (minified) | 3,064 | 1,788 | 1,409 | 1.71× | Fused | Win |
| Instruments (prettified) | 2,601 | 2,691 | 2,240 | 0.97× | Two-stage | Win |
| Marine IK Reverse (minified) | 836 | 704 | 488 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,348 | 2,303 | 1,676 | 1.02× | Fused | Win |
| Marine IK (minified) | 845 | 704 | 490 | 1.20× | Fused | Win |
| Marine IK (prettified) | 2,240 | 2,141 | 1,370 (n/c) | 1.05× | Fused | Win |
| Random (minified) | 1,642 | 1,270 | 1,015 | 1.29× | Fused | Win |
| Random (prettified) | 1,944 | 1,707 | 1,467 | 1.14× | Fused | Win |
| Twitter (minified) | 2,611 | 2,028 | 1,838 | 1.29× | Fused | Win |
| Twitter (prettified) | 2,601 | 2,537 | 2,379 | 1.03× | Fused | Win |
| Apache Builds Small (minified) | 2,131 | 1,677 | 1,399 | 1.27× | Fused | Win |
| Canada Small (minified) | 925 | 774 | 526 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 1,946 | 2,136 | 1,674 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 1,879 | 1,632 | 1,060 | 1.15× | Fused | Win |
| CitmCatalog Small (minified) | 2,025 | 1,430 | 1,052 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 2,274 | 1,995 | 1,813 | 1.14× | Fused | Win |
| Discord Small (minified) | 2,735 | 1,686 | 1,611 | 1.62× | Fused | Win |
| Discord Small (prettified) | 2,435 | 1,607 | 2,120 | 1.51× | Fused | Win |
| Github Events Small (minified) | 2,461 | 1,805 | 2,014 | 1.36× | Fused | Win |
| Google Maps Response Small (minified) | 2,568 | 1,421 | 1,112 | 1.81× | Fused | Win |
| Github Events Small (prettified) | 2,062 | 2,133 | 2,328 | 0.97× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,985 | 2,460 | 2,041 | 1.21× | Fused | Win |
| Instruments Small (minified) | 2,468 | 1,601 | 1,023 | 1.54× | Fused | Win |
| Instruments Small (prettified) | 2,892 | 2,502 | 2,100 | 1.16× | Fused | Win |
| Marine IK Reverse Small (minified) | 747 | 559 | 374 | 1.34× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,874 | 2,148 | 1,222 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 954 | 701 | 405 | 1.36× | Fused | Win |
| Marine IK Small (prettified) | 2,082 | 2,180 | 1,465 | 0.95× | Two-stage | Win |
| Random Small (minified) | 1,956 | 1,463 | 1,128 | 1.34× | Fused | Win |
| Random Small (prettified) | 2,021 | 1,745 | 1,575 | 1.16× | Fused | Win |
| Twitter Small (minified) | 2,763 | 2,104 | 1,852 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 2,686 | 2,570 | 2,395 | 1.05× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on three things: whether the document is indented, whether the machine is x86, and, on the M2 Pro, the compiler. On the three x86 builds the fused path wins all 15 POD-type tests, 29 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 31 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC follows the same split: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 19 and the fused path 3 (Apache Builds, Apache Builds Small and Github Events Small). On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (minified Canada on macOS/GCC is a tie) and 29 of the 44 prettified documents (12 to the tape, 3 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.27× (String, Windows/MSVC) to 4.62× (Bool, macOS/GCC) faster than the two-stage path, and 1.45× (String, Windows/MSVC) to 5.82× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** The fused path wins 105 of the 110 minified documents: all 22 under MSVC, on Linux/Clang and on macOS/Clang, 21 of 22 on macOS/GCC, where minified Canada is a tie (780 against 757 MB/s), and 18 of 22 on Linux/GCC, where the two-stage path wins minified Canada by 3% (1,032 against 1,000 MB/s) and minified Github Events by 9% (1,773 against 1,624), and minified Marine IK Reverse and Random are ties. The fused path's margins run from under 1% (Marine IK on Linux/GCC, 752 against 748 MB/s) to 1.92× (Google Maps Response on macOS/Clang, 2,202 against 1,147 MB/s) on the full-size documents and up to 1.95× on the Small ones (Google Maps Response Small on macOS/Clang, 2,340 against 1,201 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 62 of 66.** It wins all 22 on Linux/GCC, 21 of 22 on Linux/Clang, where prettified Canada is a tie (2,088 MB/s fused against 2,142), and 19 of 22 under MSVC, where the fused path wins prettified Apache Builds, Apache Builds Small and Github Events Small, by 5%, 9% and 10%. The tape's margins run from 1% (Marine IK under MSVC, 1,916 against 1,889 MB/s) to 47% (Discord on Linux/Clang, 2,681 against 1,822) on the full-size documents and from 5% (Google Maps Response Small on Linux/GCC) to 45% (Random Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 63 of the 66 prettified x86 documents: the fused path wins the same three under MSVC, and there are no ties.

**Prettified documents on the M2 Pro: split by compiler.** Of the 44 prettified documents, the fused path wins 29, the tape wins 12 and 3 are ties, but the two compilers disagree. On macOS/Clang the fused path wins 18 of 22; the tape takes the Small copies of Apache Builds, Marine IK Reverse and Twitter, and Instruments Small is a tie. On macOS/GCC the fused path wins only 11 of 22, the tape wins 9 (prettified Apache Builds, Github Events, Marine IK, Marine IK Reverse, Random and Twitter, and the Small copies of Github Events, Marine IK and Marine IK Reverse) and 2 are ties (prettified Canada and Google Maps Response). The tape's widest M2 Pro margin is prettified Github Events Small on macOS/GCC, 1,485 against 1,075 MB/s (1.38×). Reused, the fused path wins 28, the tape 11, and 5 are ties: 17, 5 and 0 on macOS/Clang, and 11, 6 and 5 on macOS/GCC. NEON escapes much of the prettified penalty seen on x86, most of all under Clang, and we do not have an explanation for why, or for why GCC keeps less of that advantage. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified Github Events and Twitter are where Jsonifier is weakest.** All five of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are prettified copies of those two documents: Github Events on Linux/Clang by 11% (1,959 against 2,193 MB/s), Twitter Small on Linux/Clang by 5% (2,827 against 2,984), Github Events Small on Linux/GCC by 6% (2,282 against 2,419), and on macOS/Clang Github Events Small by 3% (1,878 against 1,940) and Twitter Small by 5% (1,971 against 2,083). The one tie is prettified Twitter on Linux/Clang (2,214 against 2,222). With objects reused the faster path loses two, Github Events Small on Linux/GCC by 5% (2,818 against 2,956) and on macOS/Clang by 8% (2,133 against 2,328), and ties Github Events Small on macOS/GCC (1,748 against 1,753). We have not profiled what these documents have in common.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 64% of its Linux/Clang throughput, the two-stage path 66% and simdjson 48%. Because the two Jsonifier paths lose a similar share, MSVC routes input much as Linux/Clang does. The spread between documents is about the same for the two Jsonifier paths (the fused path from 53% on Discord to 79% on CitmCatalog, the two-stage path from 52% on Apache Builds to 76% on Instruments) and widest for simdjson, which keeps 38% of its Linux/Clang throughput on Github Events and 65% on Canada. We have not profiled why.

**Against simdjson, Jsonifier wins 239 of 245 tests with whichever path is faster, ties one and loses five** (all prettified documents). The fused path alone wins 224, ties none and loses 21, all prettified, its sixteen extra losses being ten on Linux/Clang, four on Linux/GCC and one each under MSVC and on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 187 MB/s, against 342 MB/s for the same document in declared order, so the faster Jsonifier path is 3.19× faster than simdjson freshly allocated (3.34× reused). The prettified form shows the same gap, at 635 MB/s against 1,163, and the faster Jsonifier path is 2.99× faster (3.20× reused). The Small copies show it too, at 142 MB/s minified and 536 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.12× to 1.69× faster than simdjson on the full-size reversed document, freshly allocated, and 1.18× to 1.71× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so does most prettified input under Clang, while prettified input under GCC splits about evenly. Routing each test to its faster path would raise the record against simdjson from 224 wins, 0 ties and 21 losses to 239 wins, 1 tie and 5 losses. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,500 | 1,379 | 1.09× | Win |
| Canada (minified) | 1,032 | 869 | 1.19× | Win |
| Apache Builds (prettified) | 1,935 | 1,766 | 1.10× | Win |
| Canada (prettified) | 2,271 | 1,944 | 1.17× | Win |
| CitmCatalog (minified) | 1,897 | 1,415 | 1.34× | Win |
| CitmCatalog (prettified) | 3,638 | 2,942 | 1.24× | Win |
| Discord (minified) | 1,835 | 1,561 | 1.18× | Win |
| Discord (prettified) | 2,546 | 2,229 | 1.14× | Win |
| Github Events (minified) | 1,773 | 1,568 | 1.13× | Win |
| Google Maps Response (minified) | 1,941 | 1,288 | 1.51× | Win |
| Github Events (prettified) | 2,102 | 1,936 | 1.09× | Win |
| Google Maps Response (prettified) | 3,591 | 2,564 | 1.40× | Win |
| Instruments (minified) | 2,300 | 1,847 | 1.25× | Win |
| Instruments (prettified) | 3,403 | 2,921 | 1.17× | Win |
| Marine IK Reverse (minified) | 743 | 714 | 1.04× | Win |
| Marine IK Reverse (prettified) | 2,620 | 2,578 | 1.02× | Win |
| Marine IK (minified) | 748 | 705 | 1.06× | Win |
| Marine IK (prettified) | 2,645 | 2,536 | 1.04× | Win |
| Random (minified) | 1,153 | 1,106 | 1.04× | Win |
| Random (prettified) | 1,746 | 1,669 | 1.05× | Win |
| Twitter (minified) | 1,691 | 1,749 | 0.97× | **Loss** |
| Twitter (prettified) | 2,153 | 2,281 | 0.94× | **Loss** |
| Apache Builds Small (minified) | 2,196 | 1,754 | 1.25× | Win |
| Canada Small (minified) | 986 | 922 | 1.07× | Win |
| Apache Builds Small (prettified) | 2,606 | 2,197 | 1.19× | Win |
| Canada Small (prettified) | 2,290 | 2,203 | 1.04× | Win |
| CitmCatalog Small (minified) | 1,501 | 1,212 | 1.24× | Win |
| CitmCatalog Small (prettified) | 2,486 | 2,096 | 1.19× | Win |
| Discord Small (minified) | 1,689 | 1,370 | 1.23× | Win |
| Discord Small (prettified) | 2,214 | 1,816 | 1.22× | Win |
| Github Events Small (minified) | 1,768 | 1,850 | 0.96× | **Loss** |
| Google Maps Response Small (minified) | 1,820 | 1,268 | 1.44× | Win |
| Github Events Small (prettified) | 2,282 | 2,288 | 1.00× | Tie |
| Google Maps Response Small (prettified) | 3,392 | 2,519 | 1.35× | Win |
| Instruments Small (minified) | 2,222 | 1,731 | 1.28× | Win |
| Instruments Small (prettified) | 3,363 | 2,780 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 752 | 750 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 2,644 | 2,667 | 0.99× | **Loss** |
| Marine IK Small (minified) | 784 | 751 | 1.04× | Win |
| Marine IK Small (prettified) | 2,768 | 2,665 | 1.04× | Win |
| Random Small (minified) | 1,583 | 1,257 | 1.26× | Win |
| Random Small (prettified) | 2,369 | 1,906 | 1.24× | Win |
| Twitter Small (minified) | 2,228 | 2,132 | 1.05× | Win |
| Twitter Small (prettified) | 2,806 | 2,718 | 1.03× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 968 | 838 | 1.15× | Win |
| Canada (minified) | 757 | 630 (n/c) | 1.20× | Win |
| Apache Builds (prettified) | 1,248 | 1,076 | 1.16× | Win |
| Canada (prettified) | 1,546 | 1,304 | 1.19× | Win |
| CitmCatalog (minified) | 1,231 | 976 | 1.26× | Win |
| CitmCatalog (prettified) | 2,675 | 2,254 | 1.19× | Win |
| Discord (minified) | 1,291 | 987 | 1.31× | Win |
| Discord (prettified) | 1,603 | 1,487 | 1.08× | Win |
| Github Events (minified) | 1,104 | 1,092 | 1.01× | Tie |
| Google Maps Response (minified) | 1,256 | 1,017 | 1.23× | Win |
| Github Events (prettified) | 1,415 | 1,393 | 1.02× | Tie |
| Google Maps Response (prettified) | 2,253 | 1,862 | 1.21× | Win |
| Instruments (minified) | 1,510 | 1,308 | 1.15× | Win |
| Instruments (prettified) | 2,230 | 1,737 | 1.28× | Win |
| Marine IK Reverse (minified) | 532 (n/c) | 433 | 1.23× | Win |
| Marine IK Reverse (prettified) | 1,822 | 1,556 | 1.17× | Win |
| Marine IK (minified) | 501 | 441 | 1.14× | Win |
| Marine IK (prettified) | 1,906 | 1,709 | 1.12× | Win |
| Random (minified) | 834 | 557 | 1.50× | Win |
| Random (prettified) | 1,204 | 965 | 1.25× | Win |
| Twitter (minified) | 1,389 | 1,357 | 1.02× | Win |
| Twitter (prettified) | 2,057 | 1,551 | 1.33× | Win |
| Apache Builds Small (minified) | 1,468 | 1,048 | 1.40× | Win |
| Canada Small (minified) | 715 | 570 | 1.26× | Win |
| Apache Builds Small (prettified) | 1,524 | 1,212 | 1.26× | Win |
| Canada Small (prettified) | 1,401 | 1,190 | 1.18× | Win |
| CitmCatalog Small (minified) | 766 | 761 | 1.01× | Tie |
| CitmCatalog Small (prettified) | 1,290 | 1,117 | 1.16× | Win |
| Discord Small (minified) | 1,220 | 861 | 1.42× | Win |
| Discord Small (prettified) | 1,600 | 1,281 | 1.25× | Win |
| Github Events Small (minified) | 1,159 | 1,116 | 1.04× | Win |
| Google Maps Response Small (minified) | 1,199 | 933 | 1.28× | Win |
| Github Events Small (prettified) | 1,485 | 1,335 | 1.11× | Win |
| Google Maps Response Small (prettified) | 2,194 | 1,714 | 1.28× | Win |
| Instruments Small (minified) | 1,592 | 1,101 | 1.45× | Win |
| Instruments Small (prettified) | 2,260 | 1,805 | 1.25× | Win |
| Marine IK Reverse Small (minified) | 486 | 490 | 0.99× | Tie |
| Marine IK Reverse Small (prettified) | 1,719 | 1,729 | 0.99× | Tie |
| Marine IK Small (minified) | 552 | 488 | 1.13× | Win |
| Marine IK Small (prettified) | 1,822 | 1,663 | 1.10× | Win |
| Random Small (minified) | 975 | 606 | 1.61× | Win |
| Random Small (prettified) | 1,319 | 1,227 | 1.08× | Win |
| Twitter Small (minified) | 1,604 | 1,109 | 1.45× | Win |
| Twitter Small (prettified) | 1,838 | 1,576 | 1.17× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,241 | 1,773 | 1.26× | Win |
| Canada (minified) | 1,256 | 1,026 | 1.22× | Win |
| Apache Builds (prettified) | 2,783 | 2,234 | 1.25× | Win |
| Canada (prettified) | 2,714 | 2,283 | 1.19× | Win |
| CitmCatalog (minified) | 2,272 | 1,666 | 1.36× | Win |
| CitmCatalog (prettified) | 4,246 | 3,382 | 1.26× | Win |
| Discord (minified) | 2,381 | 1,781 | 1.34× | Win |
| Discord (prettified) | 3,367 | 2,523 | 1.33× | Win |
| Github Events (minified) | 2,297 | 1,927 | 1.19× | Win |
| Google Maps Response (minified) | 2,175 | 1,358 | 1.60× | Win |
| Github Events (prettified) | 2,704 | 2,307 | 1.17× | Win |
| Google Maps Response (prettified) | 3,865 | 2,694 | 1.43× | Win |
| Instruments (minified) | 2,617 | 1,887 | 1.39× | Win |
| Instruments (prettified) | 3,789 | 2,995 | 1.27× | Win |
| Marine IK Reverse (minified) | 823 | 808 | 1.02× | Win |
| Marine IK Reverse (prettified) | 2,861 | 2,848 | 1.00× | Win |
| Marine IK (minified) | 828 | 802 | 1.03× | Win |
| Marine IK (prettified) | 2,899 | 2,820 | 1.03× | Win |
| Random (minified) | 1,548 | 1,432 | 1.08× | Win |
| Random (prettified) | 2,310 | 2,122 | 1.09× | Win |
| Twitter (minified) | 2,119 | 2,072 | 1.02× | Win |
| Twitter (prettified) | 2,726 | 2,663 | 1.02× | Win |
| Apache Builds Small (minified) | 2,532 | 1,859 | 1.36× | Win |
| Canada Small (minified) | 1,246 | 1,095 | 1.14× | Win |
| Apache Builds Small (prettified) | 3,042 | 2,254 | 1.35× | Win |
| Canada Small (prettified) | 2,808 | 2,557 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,781 | 1,328 | 1.34× | Win |
| CitmCatalog Small (prettified) | 2,918 | 2,270 | 1.29× | Win |
| Discord Small (minified) | 2,107 | 1,576 | 1.34× | Win |
| Discord Small (prettified) | 2,696 | 2,119 | 1.27× | Win |
| Github Events Small (minified) | 2,213 | 2,054 | 1.08× | Win |
| Google Maps Response Small (minified) | 2,047 | 1,350 | 1.52× | Win |
| Github Events Small (prettified) | 2,818 | 2,639 | 1.07× | Win |
| Google Maps Response Small (prettified) | 3,718 | 2,684 | 1.39× | Win |
| Instruments Small (minified) | 2,403 | 1,784 | 1.35× | Win |
| Instruments Small (prettified) | 3,583 | 2,869 | 1.25× | Win |
| Marine IK Reverse Small (minified) | 896 | 848 | 1.06× | Win |
| Marine IK Reverse Small (prettified) | 3,115 | 2,984 | 1.04× | Win |
| Marine IK Small (minified) | 956 | 846 | 1.13× | Win |
| Marine IK Small (prettified) | 3,278 | 2,992 | 1.10× | Win |
| Random Small (minified) | 1,941 | 1,425 | 1.36× | Win |
| Random Small (prettified) | 2,852 | 2,181 | 1.31× | Win |
| Twitter Small (minified) | 2,541 | 2,308 | 1.10× | Win |
| Twitter Small (prettified) | 3,155 | 2,954 | 1.07× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,296 | 1,042 | 1.24× | Win |
| Canada (minified) | 948 | 764 | 1.24× | Win |
| Apache Builds (prettified) | 1,572 | 1,335 | 1.18× | Win |
| Canada (prettified) | 1,821 | 1,467 | 1.24× | Win |
| CitmCatalog (minified) | 1,522 | 1,134 | 1.34× | Win |
| CitmCatalog (prettified) | 2,542 | 2,451 | 1.04× | Win |
| Discord (minified) | 1,642 | 1,216 | 1.35× | Win |
| Discord (prettified) | 2,216 | 1,430 | 1.55× | Win |
| Github Events (minified) | 1,304 | 1,294 | 1.01× | Tie |
| Google Maps Response (minified) | 1,510 | 1,112 | 1.36× | Win |
| Github Events (prettified) | 1,622 | 1,746 | 0.93× | **Loss** |
| Google Maps Response (prettified) | 2,610 | 1,787 (n/c) | 1.46× | Win |
| Instruments (minified) | 1,875 | 1,201 | 1.56× | Win |
| Instruments (prettified) | 2,263 | 1,702 | 1.33× | Win |
| Marine IK Reverse (minified) | 603 (n/c) | 575 | 1.05× | Win |
| Marine IK Reverse (prettified) | 1,838 | 1,727 (n/c) | 1.06× | Win |
| Marine IK (minified) | 567 | 484 | 1.17× | Win |
| Marine IK (prettified) | 2,189 (n/c) | 1,714 (n/c) | 1.28× | Win |
| Random (minified) | 1,327 | 825 | 1.61× | Win |
| Random (prettified) | 1,511 | 1,238 | 1.22× | Win |
| Twitter (minified) | 2,049 | 1,508 | 1.36× | Win |
| Twitter (prettified) | 2,591 | 1,727 | 1.50× | Win |
| Apache Builds Small (minified) | 1,895 | 1,152 | 1.65× | Win |
| Canada Small (minified) | 887 | 572 | 1.55× | Win |
| Apache Builds Small (prettified) | 1,968 | 1,496 | 1.32× | Win |
| Canada Small (prettified) | 1,464 | 1,428 | 1.03× | Win |
| CitmCatalog Small (minified) | 1,232 | 1,007 | 1.22× | Win |
| CitmCatalog Small (prettified) | 1,870 | 1,786 | 1.05× | Win |
| Discord Small (minified) | 1,605 | 1,217 | 1.32× | Win |
| Discord Small (prettified) | 1,966 | 1,558 | 1.26× | Win |
| Github Events Small (minified) | 1,640 | 1,283 | 1.28× | Win |
| Google Maps Response Small (minified) | 1,410 | 1,066 | 1.32× | Win |
| Github Events Small (prettified) | 1,748 | 1,514 | 1.15× | Win |
| Google Maps Response Small (prettified) | 2,563 | 1,919 | 1.34× | Win |
| Instruments Small (minified) | 1,726 | 1,220 | 1.41× | Win |
| Instruments Small (prettified) | 2,426 | 1,926 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 620 | 582 | 1.06× | Win |
| Marine IK Reverse Small (prettified) | 2,033 | 1,955 | 1.04× | Win |
| Marine IK Small (minified) | 693 | 583 | 1.19× | Win |
| Marine IK Small (prettified) | 2,275 | 1,959 | 1.16× | Win |
| Random Small (minified) | 1,146 | 971 | 1.18× | Win |
| Random Small (prettified) | 1,615 | 1,501 | 1.08× | Win |
| Twitter Small (minified) | 2,026 | 1,206 | 1.68× | Win |
| Twitter Small (prettified) | 2,015 (n/c) | 1,674 | 1.20× | Win |

In the freshly allocated run, two-stage Jsonifier wins 77 of these 88 tests, ties 7 and loses 4, every loss by 1% to 6% and all four on Linux/GCC: minified and prettified Twitter (the widest, at 0.94× prettified), minified Github Events Small, and prettified Marine IK Reverse Small, the narrowest at 0.99×. The ties are prettified Github Events Small and minified Marine IK Reverse Small on Linux/GCC, and on macOS/GCC minified and prettified Github Events, minified CitmCatalog Small and Marine IK Reverse Small in both forms. With the fused path instead, Jsonifier beats the reflection reader on 71, ties 1 and loses 16. With objects reused, two-stage Jsonifier wins 86, ties 1 and loses 1, both on macOS/GCC: it loses prettified Github Events by 7% (1,622 against 1,746 MB/s) and ties minified Github Events (1,304 against 1,294). The fused path beats the reflection reader on 75 and loses 13. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 27 of the 40 results; all 13 wider gaps are on the macOS builds, and the widest is reused minified Twitter on macOS/GCC, where the "jsonifier" row runs at 3,897 MB/s against the two-stage row's 4,827. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,571 | 3,562 | 1.84× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,650 | 4,155 | 1.84× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,155 | 3,223 | 1.29× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,975 | 3,751 | 1.33× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,065 | 7,091 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,176 | 8,357 | 1.10× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,149 | 6,424 | 0.96× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,479 | 7,948 | 0.94× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,050 | 7,190 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,071 | 8,338 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,481 | 5,788 | 0.95× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,017 | 6,876 | 1.02× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,486 | 3,465 | 1.29× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,276 | 4,375 | 0.98× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,729 | 3,631 | 1.03× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,003 | 3,935 | 1.02× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,295 | 3,566 | 1.20× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,648 | 5,066 | 0.92× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,193 | 2,702 | 1.18× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,063 | 3,287 | 1.24× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,828 | 3,682 | 1.85× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,877 | 4,248 | 1.85× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,535 | 3,413 | 1.33× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,346 | 4,013 | 1.33× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,238 | 7,171 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,359 | 8,538 | 1.10× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,567 | 6,755 | 0.97× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,998 | 8,047 | 0.99× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,217 | 7,328 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,145 | 8,552 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,441 | 6,018 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,381 | 7,259 | 1.02× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,827 | 3,851 | 1.25× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,847 | 4,952 | 0.98× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,974 | 3,843 | 1.03× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,495 | 4,174 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,381 | 4,350 | 1.01× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,789 | 5,158 | 0.93× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,884 | 2,939 | 1.32× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,787 | 4,442 | 0.85× | Loss |

Jsonifier wins 26 of the 40 results, ties 4 and loses 10, and wins by up to 1.85× (Twitter under MSVC, reused, in both forms). Freshly allocated it wins 13, ties 2 and loses 5: the losses are Twitter Small on Linux/Clang in both forms (0.96× minified, 0.94× prettified), minified Twitter Small on Linux/GCC (0.95×), and full-size prettified Twitter on macOS/GCC (0.98×) and macOS/Clang (0.92×); the ties are Twitter Small on macOS/GCC in both forms. The reused run also has 13 wins, 2 ties and 5 losses: Twitter Small on Linux/Clang in both forms (0.97× and 0.99×), prettified Twitter on macOS/GCC (0.98×), and prettified Twitter (0.93×) and Twitter Small (0.85×) on macOS/Clang; the ties are minified Twitter Small on macOS/GCC and minified Twitter on macOS/Clang. On the three x86 builds every full-size result is a win, by 1.07× to 1.85×; on the M2 Pro the full-size document is not a clear win, with prettified Twitter lost on both macOS builds in both runs. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.06× to 1.35×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,835 | 2,684 | 1.06× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,708 | 2,742 | 1.35× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 3,067 | 2,500 | 1.23× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,043 | 1,674 | 1.22× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,273 | 1,773 | 1.28× | Win |

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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 162 of 245 freshly allocated tests: all 25 POD-type tests, 105 of 110 minified documents and 18 of 22 prettified documents on macOS/Clang, while the two-stage path wins 62 of 66 prettified documents on x86 and 9 of 22 on macOS/GCC. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 239 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 77 of 88, and on partial reading it wins 26 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*