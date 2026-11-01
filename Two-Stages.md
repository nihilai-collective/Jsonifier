# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on 24 of the 25 POD-type tests and on 106 of the 110 minified documents, the tape is faster on 62 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 33 of the 44 prettified documents.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 1, 2026, with Jsonifier [c338fdb](https://github.com/nihilai-collective/jsonifier/commit/c338fdb), Glaze [01efb29](https://github.com/stephenberry/glaze/commit/01efb29) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build, and simdjson [a78278a](https://github.com/simdjson/simdjson/commit/a78278a) ([40a686a](https://github.com/simdjson/simdjson/commit/40a686a) under MSVC). The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Two freshly allocated full-parse rows contain non-converged results (one under MSVC, whose String row has two n/c cells, the fused path and simdjson, and one on macOS/GCC), and four reused rows contain one each (one under MSVC, two on macOS/GCC, one on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 167 tests, 6 are statistical ties, and the two-stage path wins 72. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 29 | 0 | 20 | 48 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 25 | 2 | 22 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 0 | 7 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 44 | 3 | 2 | 49 of 49 |
| **Aggregate** | **167** | **6** | **72** | **243 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 4 / 0 / 1 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 19 / 3 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 19 / 2 / 1 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 14 / 1 / 7 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 46 / 1 / 2 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 44 / 0 / 5 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 47 / 1 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 48 / 0 / 1 | 48 / 0 / 1 |
| **Aggregate** | **239 / 2 / 4** | **225 / 2 / 18** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 48 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 25 | 1 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 37 | 5 | 7 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 43 | 3 | 3 | 48 of 49 |
| **Aggregate** | **163** | **9** | **73** | **241 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 20 / 1 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 16 / 3 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 1 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 11 / 4 / 7 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 48 / 1 / 0 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 46 / 0 / 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 48 / 0 / 1 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 / 1 / 1 | 47 / 1 / 1 |
| **Aggregate** | **240 / 3 / 2** | **228 / 1 / 16** |

Across 245 tests the fused path wins 163, ties 9 and loses 73 to the two-stage path, and the x86 split holds: the tape wins 31 of the 33 full-size prettified documents (the fused path takes Apache Builds under MSVC and Marine IK Reverse on Linux/Clang) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 31 of the 33 full-size minified ones (the tape takes Random on Linux/GCC by 0.4%, 1,516 against 1,510 MB/s, a statistically significant gap that rounds to 1.00× in §2.3, and minified Canada is a tie there). On the M2 Pro the fused path wins 43 of the 44 minified documents (minified Canada Small is a tie on macOS/Clang) and 27 of the 44 prettified ones, with 7 ties and 10 tape wins. Against simdjson, the faster Jsonifier path wins 240, ties 3 and loses 2, and the fused path alone wins 228, ties 1 and loses 16.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 511 | 267 | 92 | 1.92× | Fused | Win |
| Double (POD) | 689 | 507 | 148 | 1.36× | Fused | Win |
| Int64 (POD) | 1,680 | 895 | 312 | 1.88× | Fused | Win |
| String (POD) | 539 (n/c) | 785 | 557 (n/c) | 0.69× | Two-stage | Win |
| Uint64 (POD) | 1,534 | 918 | 315 | 1.67× | Fused | Win |
| Apache Builds (minified) | 956 | 798 | 559 | 1.20× | Fused | Win |
| Canada (minified) | 699 | 657 | 436 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,093 | 1,038 | 716 | 1.05× | Fused | Win |
| Canada (prettified) | 1,397 | 1,506 | 962 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,804 | 1,269 | 476 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 2,402 | 2,691 | 1,028 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,181 | 1,038 | 523 | 1.14× | Fused | Win |
| Discord (prettified) | 1,275 | 1,479 | 753 | 0.86× | Two-stage | Win |
| Github Events (minified) | 1,019 | 885 | 507 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,606 | 1,191 | 443 | 1.35× | Fused | Win |
| Github Events (prettified) | 1,006 | 1,067 | 624 | 0.94× | Two-stage | Win |
| Google Maps Response (prettified) | 2,235 | 2,315 | 883 | 0.97× | Two-stage | Win |
| Instruments (minified) | 1,955 | 1,473 | 578 | 1.33× | Fused | Win |
| Instruments (prettified) | 1,953 | 2,280 | 935 | 0.86× | Two-stage | Win |
| Marine IK Reverse (minified) | 583 | 511 | 182 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 1,826 | 1,895 | 615 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 593 | 526 | 331 | 1.13× | Fused | Win |
| Marine IK (prettified) | 1,819 | 1,885 | 1,155 | 0.96× | Two-stage | Win |
| Random (minified) | 766 | 714 | 443 | 1.07× | Fused | Win |
| Random (prettified) | 999 | 1,083 | 669 | 0.92× | Two-stage | Win |
| Twitter (minified) | 1,219 | 1,111 | 724 | 1.10× | Fused | Win |
| Twitter (prettified) | 1,242 | 1,428 | 960 | 0.87× | Two-stage | Win |
| Apache Builds Small (minified) | 1,305 | 979 | 665 | 1.33× | Fused | Win |
| Canada Small (minified) | 621 | 590 | 335 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,352 | 1,241 | 815 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,266 | 1,403 | 787 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 883 | 749 | 464 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,158 | 1,312 | 795 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,114 | 924 | 554 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,133 | 1,231 | 751 | 0.92× | Two-stage | Win |
| Github Events Small (minified) | 1,249 | 840 | 521 | 1.49× | Fused | Win |
| Google Maps Response Small (minified) | 1,609 | 1,113 | 440 | 1.45× | Fused | Win |
| Github Events Small (prettified) | 1,185 | 1,085 | 666 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,053 | 2,268 | 890 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 2,094 | 1,464 | 598 | 1.43× | Fused | Win |
| Instruments Small (prettified) | 1,841 | 2,326 | 963 | 0.79× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 472 | 418 | 134 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,374 | 1,559 | 504 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 481 | 426 | 275 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,412 | 1,571 | 980 | 0.90× | Two-stage | Win |
| Random Small (minified) | 916 | 815 | 440 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,151 | 1,212 | 663 | 0.95× | Two-stage | Win |
| Twitter Small (minified) | 1,505 | 1,297 | 1,019 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 1,441 | 1,610 | 1,293 | 0.90× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,951 | 633 | 278 | 3.08× | Fused | Win |
| Double (POD) | 1,230 | 735 | 352 | 1.67× | Fused | Win |
| Int64 (POD) | 2,997 | 1,626 | 767 | 1.84× | Fused | Win |
| String (POD) | 1,817 | 1,284 | 1,350 | 1.41× | Fused | Win |
| Uint64 (POD) | 3,219 | 1,571 | 764 | 2.05× | Fused | Win |
| Apache Builds (minified) | 1,737 | 1,507 | 1,238 | 1.15× | Fused | Win |
| Canada (minified) | 1,078 | 977 | 675 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,736 | 1,843 | 1,574 | 0.94× | Two-stage | Win |
| Canada (prettified) | 2,072 | 2,136 | 1,551 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,411 | 1,776 | 1,211 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 2,878 | 3,619 | 2,573 | 0.80× | Two-stage | Win |
| Discord (minified) | 2,238 | 1,802 | 1,419 | 1.24× | Fused | Win |
| Discord (prettified) | 1,796 | 2,561 | 2,090 | 0.70× | Two-stage | Win |
| Github Events (minified) | 1,659 | 1,650 | 1,587 | 1.01× | Tie | Win |
| Google Maps Response (minified) | 2,590 | 1,960 | 1,336 | 1.32× | Fused | Win |
| Github Events (prettified) | 1,643 | 2,180 | 1,929 | 0.75× | Two-stage | Win |
| Google Maps Response (prettified) | 2,993 | 3,579 | 2,636 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,581 | 1,954 | 1,734 | 1.32× | Fused | Win |
| Instruments (prettified) | 2,226 | 2,976 | 2,772 | 0.75× | Two-stage | Win |
| Marine IK Reverse (minified) | 845 | 735 | 591 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,502 | 2,489 | 2,091 | 1.00× | Fused | Win |
| Marine IK (minified) | 861 | 752 | 591 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,520 | 2,606 | 2,106 | 0.97× | Two-stage | Win |
| Random (minified) | 1,265 | 1,104 | 1,058 | 1.15× | Fused | Win |
| Random (prettified) | 1,401 | 1,658 | 1,570 | 0.85× | Two-stage | Win |
| Twitter (minified) | 1,905 | 1,724 | 1,695 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,688 | 2,185 | 2,206 | 0.77× | Two-stage | Tie |
| Apache Builds Small (minified) | 2,616 | 2,211 | 1,519 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,046 | 970 | 708 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 2,176 | 2,703 | 1,831 | 0.81× | Two-stage | Win |
| Canada Small (prettified) | 1,943 | 2,189 | 1,674 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,754 | 1,420 | 1,179 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 1,861 | 2,434 | 2,054 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 2,014 | 1,763 | 1,434 | 1.14× | Fused | Win |
| Discord Small (prettified) | 1,745 | 2,368 | 1,940 | 0.74× | Two-stage | Win |
| Github Events Small (minified) | 2,106 | 1,826 | 1,873 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,400 | 1,806 | 1,316 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,816 | 2,278 | 2,295 | 0.80× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,746 | 3,488 | 2,640 | 0.79× | Two-stage | Win |
| Instruments Small (minified) | 2,601 | 1,966 | 1,597 | 1.32× | Fused | Win |
| Instruments Small (prettified) | 2,214 | 3,058 | 2,562 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 822 | 740 | 582 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,194 | 2,595 | 2,103 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 917 | 800 | 577 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,299 | 2,807 | 2,122 | 0.82× | Two-stage | Win |
| Random Small (minified) | 1,733 | 1,565 | 1,161 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,683 | 2,349 | 1,775 | 0.72× | Two-stage | Win |
| Twitter Small (minified) | 2,544 | 2,107 | 2,202 | 1.21× | Fused | Win |
| Twitter Small (prettified) | 2,007 | 2,669 | 2,812 | 0.75× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,228 | 640 | 250 | 3.48× | Fused | Win |
| Double (POD) | 1,235 | 709 | 298 | 1.74× | Fused | Win |
| Int64 (POD) | 3,393 | 1,692 | 735 | 2.01× | Fused | Win |
| String (POD) | 1,838 | 1,230 | 1,302 | 1.49× | Fused | Win |
| Uint64 (POD) | 3,509 | 1,677 | 726 | 2.09× | Fused | Win |
| Apache Builds (minified) | 1,667 | 1,546 | 1,395 | 1.08× | Fused | Win |
| Canada (minified) | 1,010 | 1,010 | 869 | 1.00× | Tie | Win |
| Apache Builds (prettified) | 1,750 | 1,911 | 1,770 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,137 | 2,270 | 1,919 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,459 | 1,851 | 1,377 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 3,403 | 3,700 | 2,897 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,298 | 1,847 | 1,450 | 1.24× | Fused | Win |
| Discord (prettified) | 2,320 | 2,623 | 2,101 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,606 | 1,551 | 1,415 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 2,661 | 1,931 | 1,104 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,580 | 2,099 | 1,716 | 0.75× | Two-stage | Win |
| Google Maps Response (prettified) | 3,456 | 3,575 | 2,214 | 0.97× | Two-stage | Win |
| Instruments (minified) | 3,404 | 2,311 | 1,409 | 1.47× | Fused | Win |
| Instruments (prettified) | 2,998 | 3,442 | 2,249 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 759 | 745 | 616 | 1.02× | Fused | Win |
| Marine IK Reverse (prettified) | 2,467 | 2,629 | 2,221 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 762 | 750 | 606 | 1.02× | Fused | Win |
| Marine IK (prettified) | 2,494 | 2,629 | 2,195 | 0.95× | Two-stage | Win |
| Random (minified) | 1,128 | 1,128 | 1,035 | 1.00× | Tie | Win |
| Random (prettified) | 1,433 | 1,683 | 1,577 | 0.85× | Two-stage | Win |
| Twitter (minified) | 1,937 | 1,680 | 1,356 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,763 | 2,138 | 1,780 | 0.82× | Two-stage | Win |
| Apache Builds Small (minified) | 2,537 | 2,209 | 1,657 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,068 | 999 | 780 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 2,352 | 2,660 | 2,027 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,081 | 2,219 | 1,839 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,855 | 1,512 | 1,194 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,143 | 2,527 | 2,031 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,059 | 1,723 | 1,270 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,958 | 2,242 | 1,709 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,148 | 1,848 | 2,024 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,453 | 1,821 | 1,124 | 1.35× | Fused | Win |
| Github Events Small (prettified) | 2,060 | 2,243 | 2,491 | 0.92× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,165 | 3,365 | 2,268 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,368 | 2,259 | 1,315 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,894 | 3,375 | 2,092 | 0.86× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 839 | 746 | 574 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,291 | 2,583 | 2,085 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 934 | 778 | 592 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,446 | 2,711 | 2,154 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,694 | 1,600 | 1,151 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,968 | 2,371 | 1,769 | 0.83× | Two-stage | Win |
| Twitter Small (minified) | 2,943 | 2,371 | 1,702 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,332 | 2,843 | 2,184 | 0.82× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,711 | 411 | 156 | 4.16× | Fused | Win |
| Double (POD) | 1,043 | 465 | 159 | 2.25× | Fused | Win |
| Int64 (POD) | 2,640 | 921 | 457 | 2.87× | Fused | Win |
| String (POD) | 1,415 | 803 | 779 | 1.76× | Fused | Win |
| Uint64 (POD) | 2,574 | 935 | 444 | 2.75× | Fused | Win |
| Apache Builds (minified) | 1,134 | 943 | 637 | 1.20× | Fused | Win |
| Canada (minified) | 825 | 751 | 591 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,183 | 1,098 | 911 | 1.08× | Fused | Win |
| Canada (prettified) | 1,746 | 1,662 | 1,266 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,151 | 1,415 | 1,179 | 1.52× | Fused | Win |
| CitmCatalog (prettified) | 3,039 | 2,669 | 2,265 | 1.14× | Fused | Win |
| Discord (minified) | 1,860 | 1,281 | 1,008 | 1.45× | Fused | Win |
| Discord (prettified) | 1,930 | 1,776 | 1,417 | 1.09× | Fused | Win |
| Github Events (minified) | 1,667 | 1,102 | 1,012 | 1.51× | Fused | Win |
| Google Maps Response (minified) | 1,977 | 1,316 | 788 | 1.50× | Fused | Win |
| Github Events (prettified) | 1,328 | 1,630 | 1,212 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 2,718 | 2,406 | 1,479 | 1.13× | Fused | Win |
| Instruments (minified) | 2,458 | 1,544 | 1,157 | 1.59× | Fused | Win |
| Instruments (prettified) | 2,246 | 2,167 | 1,815 | 1.04× | Fused | Win |
| Marine IK Reverse (minified) | 671 | 461 | 438 | 1.46× | Fused | Win |
| Marine IK Reverse (prettified) | 1,987 | 1,659 | 1,495 | 1.20× | Fused | Win |
| Marine IK (minified) | 647 | 541 | 435 | 1.19× | Fused | Win |
| Marine IK (prettified) | 1,894 | 1,725 | 1,361 | 1.10× | Fused | Win |
| Random (minified) | 823 | 772 | 660 | 1.07× | Fused | Win |
| Random (prettified) | 1,072 | 973 | 978 | 1.10× | Fused | Win |
| Twitter (minified) | 1,691 | 1,392 | 918 | 1.21× | Fused | Win |
| Twitter (prettified) | 1,788 | 1,648 | 1,147 | 1.08× | Fused | Win |
| Apache Builds Small (minified) | 1,367 | 1,153 | 761 | 1.19× | Fused | Win |
| Canada Small (minified) | 798 | 749 | 409 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,610 | 1,647 | 1,104 | 0.98× | Two-stage | Win |
| Canada Small (prettified) | 1,574 | 1,632 | 1,130 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,334 | 914 | 790 | 1.46× | Fused | Win |
| CitmCatalog Small (prettified) | 1,493 | 1,394 | 1,245 | 1.07× | Fused | Win |
| Discord Small (minified) | 1,662 | 1,232 | 793 | 1.35× | Fused | Win |
| Discord Small (prettified) | 1,622 | 1,501 | 1,036 | 1.08× | Fused | Win |
| Github Events Small (minified) | 1,411 | 1,089 | 929 | 1.30× | Fused | Win |
| Google Maps Response Small (minified) | 1,505 | 1,024 | 788 | 1.47× | Fused | Win |
| Github Events Small (prettified) | 1,153 | 1,341 | 1,394 | 0.86× | Two-stage | Tie |
| Google Maps Response Small (prettified) | 2,141 | 2,016 | 1,266 | 1.06× | Fused | Win |
| Instruments Small (minified) | 2,391 (n/c) | 1,453 | 1,006 | 1.65× | Fused | Win |
| Instruments Small (prettified) | 2,490 | 1,925 | 1,807 | 1.29× | Fused | Win |
| Marine IK Reverse Small (minified) | 641 | 458 | 353 | 1.40× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,491 | 1,876 | 1,350 | 0.79× | Two-stage | Win |
| Marine IK Small (minified) | 668 | 515 | 347 | 1.30× | Fused | Win |
| Marine IK Small (prettified) | 1,836 | 1,704 | 1,309 | 1.08× | Fused | Win |
| Random Small (minified) | 1,079 | 1,127 | 662 | 0.96× | Two-stage | Win |
| Random Small (prettified) | 1,164 | 1,416 | 1,143 | 0.82× | Two-stage | Win |
| Twitter Small (minified) | 1,963 | 1,662 | 1,084 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,049 | 1,883 | 1,239 | 1.09× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,301 | 367 | 189 | 3.55× | Fused | Win |
| Double (POD) | 958 | 315 | 216 | 3.04× | Fused | Win |
| Int64 (POD) | 2,083 | 865 | 493 | 2.41× | Fused | Win |
| String (POD) | 1,400 | 686 | 867 | 2.04× | Fused | Win |
| Uint64 (POD) | 2,364 | 867 | 484 | 2.73× | Fused | Win |
| Apache Builds (minified) | 1,608 | 1,122 | 915 | 1.43× | Fused | Win |
| Canada (minified) | 850 | 745 | 547 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 1,646 | 1,411 | 1,167 | 1.17× | Fused | Win |
| Canada (prettified) | 1,687 | 1,602 | 1,212 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,323 | 1,525 | 1,082 | 1.52× | Fused | Win |
| CitmCatalog (prettified) | 3,254 | 2,946 | 2,186 | 1.10× | Fused | Win |
| Discord (minified) | 2,595 | 1,335 | 1,377 | 1.94× | Fused | Win |
| Discord (prettified) | 2,439 | 1,812 | 1,981 | 1.35× | Fused | Win |
| Github Events (minified) | 1,873 | 1,205 | 1,477 | 1.55× | Fused | Win |
| Google Maps Response (minified) | 2,413 | 1,274 | 833 | 1.89× | Fused | Win |
| Github Events (prettified) | 1,626 | 1,637 | 1,875 | 0.99× | Tie | Loss |
| Google Maps Response (prettified) | 2,971 | 2,168 | 1,916 | 1.37× | Fused | Win |
| Instruments (minified) | 2,595 | 1,567 | 1,160 | 1.66× | Fused | Win |
| Instruments (prettified) | 2,830 | 2,597 | 1,658 | 1.09× | Fused | Win |
| Marine IK Reverse (minified) | 715 | 541 | 446 | 1.32× | Fused | Win |
| Marine IK Reverse (prettified) | 2,006 | 2,022 | 1,555 | 0.99× | Tie | Win |
| Marine IK (minified) | 727 | 576 | 449 | 1.26× | Fused | Win |
| Marine IK (prettified) | 2,179 | 2,094 | 1,562 | 1.04× | Fused | Win |
| Random (minified) | 1,298 | 1,010 | 900 | 1.28× | Fused | Win |
| Random (prettified) | 1,663 | 1,260 | 1,337 | 1.32× | Fused | Win |
| Twitter (minified) | 2,353 | 1,783 | 1,712 | 1.32× | Fused | Win |
| Twitter (prettified) | 2,318 | 2,206 | 2,184 | 1.05× | Fused | Win |
| Apache Builds Small (minified) | 1,471 | 1,232 | 997 | 1.19× | Fused | Win |
| Canada Small (minified) | 772 | 682 | 395 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,309 | 1,596 | 1,149 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 1,309 | 1,222 | 1,001 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,061 | 846 | 842 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,491 | 1,463 | 1,305 | 1.02× | Fused | Win |
| Discord Small (minified) | 1,805 | 1,239 | 1,233 | 1.46× | Fused | Win |
| Discord Small (prettified) | 1,912 | 1,423 | 1,474 | 1.34× | Fused | Win |
| Github Events Small (minified) | 1,435 | 1,138 | 1,337 | 1.26× | Fused | Win |
| Google Maps Response Small (minified) | 1,986 | 882 | 861 | 2.25× | Fused | Win |
| Github Events Small (prettified) | 1,737 | 1,497 | 1,375 | 1.16× | Fused | Win |
| Google Maps Response Small (prettified) | 2,522 | 1,947 | 1,727 | 1.30× | Fused | Win |
| Instruments Small (minified) | 2,469 | 1,686 | 1,121 | 1.46× | Fused | Win |
| Instruments Small (prettified) | 2,493 | 2,425 | 1,723 | 1.03× | Fused | Win |
| Marine IK Reverse Small (minified) | 570 | 479 | 329 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,575 | 1,651 | 1,148 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 597 | 558 | 312 | 1.07× | Fused | Win |
| Marine IK Small (prettified) | 1,720 | 1,691 | 1,113 | 1.02× | Fused | Win |
| Random Small (minified) | 1,289 | 1,058 | 779 | 1.22× | Fused | Win |
| Random Small (prettified) | 1,456 | 1,248 | 1,149 | 1.17× | Fused | Win |
| Twitter Small (minified) | 2,044 | 1,725 | 1,625 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 1,985 | 2,000 | 1,867 | 0.99× | Tie | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 580 | 280 | 93 | 2.07× | Fused | Win |
| Double (POD) | 841 | 589 | 157 | 1.43× | Fused | Win |
| Int64 (POD) | 2,055 | 965 | 161 (n/c) | 2.13× | Fused | Win |
| String (POD) | 1,687 | 1,143 | 713 | 1.48× | Fused | Win |
| Uint64 (POD) | 1,832 | 945 | 335 | 1.94× | Fused | Win |
| Apache Builds (minified) | 1,558 | 1,195 | 761 | 1.30× | Fused | Win |
| Canada (minified) | 842 | 796 | 487 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,664 | 1,549 | 966 | 1.07× | Fused | Win |
| Canada (prettified) | 1,622 | 1,777 | 1,073 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,187 | 1,464 | 501 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 2,724 | 3,052 | 1,057 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,694 | 1,404 | 626 | 1.21× | Fused | Win |
| Discord (prettified) | 1,637 | 1,977 | 895 | 0.83× | Two-stage | Win |
| Github Events (minified) | 1,574 | 1,273 | 620 | 1.24× | Fused | Win |
| Google Maps Response (minified) | 1,933 | 1,381 | 473 | 1.40× | Fused | Win |
| Github Events (prettified) | 1,447 | 1,481 | 755 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,465 | 2,619 | 955 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,535 | 1,690 | 639 | 1.50× | Fused | Win |
| Instruments (prettified) | 2,149 | 2,648 | 1,011 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 651 | 574 | 188 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 1,962 | 2,059 | 640 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 671 | 578 | 347 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,000 | 2,054 | 1,187 | 0.97× | Two-stage | Win |
| Random (minified) | 1,056 | 975 | 523 | 1.08× | Fused | Win |
| Random (prettified) | 1,296 | 1,496 | 781 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,787 | 1,529 | 917 | 1.17× | Fused | Win |
| Twitter (prettified) | 1,632 | 1,992 | 1,161 | 0.82× | Two-stage | Win |
| Apache Builds Small (minified) | 1,674 | 1,259 | 819 | 1.33× | Fused | Win |
| Canada Small (minified) | 783 | 736 | 373 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,666 | 1,544 | 1,021 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,557 | 1,671 | 874 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,279 | 957 | 499 | 1.34× | Fused | Win |
| CitmCatalog Small (prettified) | 1,447 | 1,694 | 862 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 1,541 | 1,224 | 692 | 1.26× | Fused | Win |
| Discord Small (prettified) | 1,372 | 1,619 | 931 | 0.85× | Two-stage | Win |
| Github Events Small (minified) | 1,771 | 1,122 | 592 | 1.58× | Fused | Win |
| Google Maps Response Small (minified) | 2,003 | 1,285 | 463 | 1.56× | Fused | Win |
| Github Events Small (prettified) | 1,525 | 1,467 | 754 | 1.04× | Fused | Win |
| Google Maps Response Small (prettified) | 2,385 | 2,507 | 934 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 2,325 | 1,618 | 665 | 1.44× | Fused | Win |
| Instruments Small (prettified) | 1,961 | 2,508 | 1,040 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 575 | 529 | 142 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,612 | 1,876 | 543 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 624 | 538 | 302 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 1,710 | 2,020 | 1,066 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,123 | 966 | 513 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,336 | 1,460 | 766 | 0.92× | Two-stage | Win |
| Twitter Small (minified) | 1,837 | 1,554 | 1,087 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 1,600 | 1,932 | 1,393 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,139 | 648 | 281 | 3.30× | Fused | Win |
| Double (POD) | 1,381 | 783 | 360 | 1.76× | Fused | Win |
| Int64 (POD) | 3,367 | 1,730 | 771 | 1.95× | Fused | Win |
| String (POD) | 2,893 | 1,755 | 1,880 | 1.65× | Fused | Win |
| Uint64 (POD) | 3,453 | 1,645 | 780 | 2.10× | Fused | Win |
| Apache Builds (minified) | 2,884 | 2,358 | 1,701 | 1.22× | Fused | Win |
| Canada (minified) | 1,333 | 1,172 | 784 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 2,484 | 2,840 | 2,175 | 0.87× | Two-stage | Win |
| Canada (prettified) | 2,431 | 2,527 | 1,784 | 0.96× | Two-stage | Win |
| CitmCatalog (minified) | 3,014 | 2,103 | 1,418 | 1.43× | Fused | Win |
| CitmCatalog (prettified) | 3,238 | 4,234 | 2,972 | 0.76× | Two-stage | Win |
| Discord (minified) | 3,231 | 2,485 | 1,943 | 1.30× | Fused | Win |
| Discord (prettified) | 2,207 | 3,434 | 2,630 | 0.64× | Two-stage | Win |
| Github Events (minified) | 2,284 | 2,230 | 2,040 | 1.02× | Fused | Win |
| Google Maps Response (minified) | 3,080 | 2,211 | 1,450 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,977 | 2,851 | 2,416 | 0.69× | Two-stage | Win |
| Google Maps Response (prettified) | 3,197 | 3,962 | 2,877 | 0.81× | Two-stage | Win |
| Instruments (minified) | 3,053 | 2,229 | 1,916 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,440 | 3,350 | 3,016 | 0.73× | Two-stage | Win |
| Marine IK Reverse (minified) | 936 | 810 | 647 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,716 | 2,667 | 2,275 | 1.02× | Fused | Win |
| Marine IK (minified) | 970 | 831 | 646 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,770 | 2,848 | 2,311 | 0.97× | Two-stage | Win |
| Random (minified) | 1,783 | 1,498 | 1,441 | 1.19× | Fused | Win |
| Random (prettified) | 1,755 | 2,187 | 2,124 | 0.80× | Two-stage | Win |
| Twitter (minified) | 2,602 | 2,183 | 2,134 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,972 | 2,757 | 2,734 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 3,152 | 2,568 | 1,845 | 1.23× | Fused | Win |
| Canada Small (minified) | 1,314 | 1,215 | 804 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 2,448 | 3,077 | 2,245 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 2,295 | 2,666 | 1,888 | 0.86× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,169 | 1,705 | 1,363 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,133 | 2,908 | 2,355 | 0.73× | Two-stage | Win |
| Discord Small (minified) | 2,720 | 2,179 | 1,875 | 1.25× | Fused | Win |
| Discord Small (prettified) | 2,076 | 2,891 | 2,491 | 0.72× | Two-stage | Win |
| Github Events Small (minified) | 2,650 | 2,299 | 2,266 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,845 | 2,066 | 1,486 | 1.38× | Fused | Win |
| Github Events Small (prettified) | 2,150 | 2,781 | 2,786 | 0.77× | Two-stage | Tie |
| Google Maps Response Small (prettified) | 3,014 | 3,870 | 2,925 | 0.78× | Two-stage | Win |
| Instruments Small (minified) | 2,795 | 2,096 | 1,740 | 1.33× | Fused | Win |
| Instruments Small (prettified) | 2,250 | 3,212 | 2,801 | 0.70× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,013 | 872 | 652 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,525 | 3,068 | 2,345 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 1,148 | 971 | 629 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 2,651 | 3,310 | 2,326 | 0.80× | Two-stage | Win |
| Random Small (minified) | 2,213 | 1,951 | 1,452 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,943 | 2,856 | 2,175 | 0.68× | Two-stage | Win |
| Twitter Small (minified) | 2,805 | 2,415 | 2,379 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 2,187 | 3,076 | 3,035 | 0.71× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,457 | 655 | 252 | 3.75× | Fused | Win |
| Double (POD) | 1,364 | 765 | 309 | 1.78× | Fused | Win |
| Int64 (POD) | 3,760 | 1,804 | 752 | 2.08× | Fused | Win |
| String (POD) | 2,750 | 1,636 | 1,687 | 1.68× | Fused | Win |
| Uint64 (POD) | 3,938 | 1,853 | 739 | 2.12× | Fused | Win |
| Apache Builds (minified) | 2,777 | 2,272 | 1,992 | 1.22× | Fused | Win |
| Canada (minified) | 1,224 | 1,211 | 1,043 | 1.01× | Tie | Win |
| Apache Builds (prettified) | 2,570 | 2,809 | 2,514 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,548 | 2,712 | 2,290 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 3,107 | 2,224 | 1,650 | 1.40× | Fused | Win |
| CitmCatalog (prettified) | 3,913 | 4,315 | 3,423 | 0.91× | Two-stage | Win |
| Discord (minified) | 3,335 | 2,505 | 1,813 | 1.33× | Fused | Win |
| Discord (prettified) | 2,883 | 3,412 | 2,645 | 0.85× | Two-stage | Win |
| Github Events (minified) | 2,178 | 2,053 | 1,761 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 3,095 | 2,167 | 1,180 | 1.43× | Fused | Win |
| Github Events (prettified) | 2,284 | 2,667 | 2,126 | 0.86× | Two-stage | Win |
| Google Maps Response (prettified) | 3,753 | 3,898 | 2,357 | 0.96× | Two-stage | Win |
| Instruments (minified) | 4,084 | 2,631 | 1,497 | 1.55× | Fused | Win |
| Instruments (prettified) | 3,308 | 3,799 | 2,397 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 839 | 821 | 686 | 1.02× | Fused | Win |
| Marine IK Reverse (prettified) | 2,666 | 2,868 | 2,433 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 844 | 836 | 672 | 1.01× | Fused | Win |
| Marine IK (prettified) | 2,729 | 2,903 | 2,407 | 0.94× | Two-stage | Win |
| Random (minified) | 1,510 | 1,516 | 1,393 | 1.00× | Two-stage | Win |
| Random (prettified) | 1,792 | 2,244 | 2,109 | 0.80× | Two-stage | Win |
| Twitter (minified) | 2,478 | 2,149 | 1,617 | 1.15× | Fused | Win |
| Twitter (prettified) | 2,101 | 2,693 | 2,119 | 0.78× | Two-stage | Win |
| Apache Builds Small (minified) | 3,015 | 2,583 | 1,813 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,359 | 1,246 | 900 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,634 | 3,102 | 2,211 | 0.85× | Two-stage | Win |
| Canada Small (prettified) | 2,524 | 2,721 | 2,113 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,292 | 1,760 | 1,345 | 1.30× | Fused | Win |
| CitmCatalog Small (prettified) | 2,419 | 2,909 | 2,343 | 0.83× | Two-stage | Win |
| Discord Small (minified) | 2,703 | 2,145 | 1,565 | 1.26× | Fused | Win |
| Discord Small (prettified) | 2,352 | 2,710 | 2,127 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,656 | 2,244 | 2,532 | 1.18× | Fused | Win |
| Google Maps Response Small (minified) | 2,872 | 2,035 | 1,234 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 2,499 | 2,699 | 3,038 | 0.93× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,477 | 3,739 | 2,464 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,705 | 2,416 | 1,361 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 3,063 | 3,604 | 2,207 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,048 | 896 | 632 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,673 | 3,044 | 2,303 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 1,165 | 953 | 639 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,815 | 3,202 | 2,356 | 0.88× | Two-stage | Win |
| Random Small (minified) | 2,126 | 1,975 | 1,434 | 1.08× | Fused | Win |
| Random Small (prettified) | 2,326 | 2,852 | 2,137 | 0.82× | Two-stage | Win |
| Twitter Small (minified) | 3,249 | 2,610 | 1,796 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,548 | 3,128 | 2,320 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,859 | 394 | 182 | 4.72× | Fused | Win |
| Double (POD) | 1,267 | 491 | 158 | 2.58× | Fused | Win |
| Int64 (POD) | 3,383 | 989 | 444 | 3.42× | Fused | Win |
| String (POD) | 2,466 | 1,049 | 998 | 2.35× | Fused | Win |
| Uint64 (POD) | 3,242 | 985 | 454 | 3.29× | Fused | Win |
| Apache Builds (minified) | 2,072 | 1,119 | 855 | 1.85× | Fused | Win |
| Canada (minified) | 1,054 | 963 | 609 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,566 | 1,580 | 1,166 | 0.99× | Tie | Win |
| Canada (prettified) | 2,129 | 1,919 | 1,476 (n/c) | 1.11× | Fused | Win |
| CitmCatalog (minified) | 2,698 | 1,636 | 1,307 | 1.65× | Fused | Win |
| CitmCatalog (prettified) | 3,499 | 3,036 | 2,474 | 1.15× | Fused | Win |
| Discord (minified) | 2,494 | 1,638 | 1,184 | 1.52× | Fused | Win |
| Discord (prettified) | 2,362 | 2,188 | 1,687 | 1.08× | Fused | Win |
| Github Events (minified) | 2,388 | 1,364 | 1,206 | 1.75× | Fused | Win |
| Google Maps Response (minified) | 2,381 | 1,492 | 834 | 1.60× | Fused | Win |
| Github Events (prettified) | 1,643 | 2,147 | 1,430 | 0.77× | Two-stage | Win |
| Google Maps Response (prettified) | 3,026 | 2,686 | 1,594 | 1.13× | Fused | Win |
| Instruments (minified) | 3,127 | 1,672 | 1,282 | 1.87× | Fused | Win |
| Instruments (prettified) | 2,692 | 2,397 | 1,979 | 1.12× | Fused | Win |
| Marine IK Reverse (minified) | 720 | 533 | 473 | 1.35× | Fused | Win |
| Marine IK Reverse (prettified) | 1,917 | 2,080 | 1,577 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 762 | 624 | 481 | 1.22× | Fused | Win |
| Marine IK (prettified) | 1,935 | 1,966 | 1,555 | 0.98× | Tie | Win |
| Random (minified) | 1,293 | 1,243 | 817 | 1.04× | Fused | Win |
| Random (prettified) | 1,447 | 1,639 | 1,145 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,329 | 1,980 | 1,013 | 1.18× | Fused | Win |
| Twitter (prettified) | 2,089 | 2,009 | 1,454 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,750 | 1,583 | 1,031 | 1.11× | Fused | Win |
| Canada Small (minified) | 978 | 822 | 475 | 1.19× | Fused | Win |
| Apache Builds Small (prettified) | 1,985 | 2,049 | 1,520 | 0.97× | Tie | Win |
| Canada Small (prettified) | 1,936 | 1,656 | 1,268 | 1.17× | Fused | Win |
| CitmCatalog Small (minified) | 1,923 | 1,336 | 1,050 | 1.44× | Fused | Win |
| CitmCatalog Small (prettified) | 2,237 | 1,882 | 1,681 | 1.19× | Fused | Win |
| Discord Small (minified) | 2,312 | 1,490 | 1,083 | 1.55× | Fused | Win |
| Discord Small (prettified) | 2,246 | 2,152 | 1,572 | 1.04× | Fused | Win |
| Github Events Small (minified) | 2,111 | 1,374 (n/c) | 1,480 | 1.54× | Fused | Win |
| Google Maps Response Small (minified) | 2,145 | 1,374 | 886 | 1.56× | Fused | Win |
| Github Events Small (prettified) | 1,363 | 1,809 | 1,812 | 0.75× | Two-stage | Tie |
| Google Maps Response Small (prettified) | 2,170 | 2,274 | 1,476 | 0.95× | Tie | Win |
| Instruments Small (minified) | 2,672 | 1,554 | 1,157 | 1.72× | Fused | Win |
| Instruments Small (prettified) | 2,489 | 2,452 | 1,721 | 1.01× | Tie | Win |
| Marine IK Reverse Small (minified) | 769 | 610 | 427 | 1.26× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,011 | 2,238 | 1,626 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 846 | 766 | 438 | 1.10× | Fused | Win |
| Marine IK Small (prettified) | 1,919 | 2,198 | 1,594 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,592 | 1,262 | 950 | 1.26× | Fused | Win |
| Random Small (prettified) | 1,682 | 1,878 | 1,278 | 0.90× | Two-stage | Win |
| Twitter Small (minified) | 2,674 | 1,761 | 1,157 | 1.52× | Fused | Win |
| Twitter Small (prettified) | 2,238 | 2,079 | 1,313 | 1.08× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,306 | 369 | 189 | 3.54× | Fused | Win |
| Double (POD) | 1,057 | 331 | 204 | 3.19× | Fused | Win |
| Int64 (POD) | 2,780 | 940 | 513 | 2.96× | Fused | Win |
| String (POD) | 2,490 | 960 | 1,217 | 2.59× | Fused | Win |
| Uint64 (POD) | 2,801 | 829 | 491 | 3.38× | Fused | Win |
| Apache Builds (minified) | 2,414 | 1,540 | 1,135 | 1.57× | Fused | Win |
| Canada (minified) | 1,065 | 914 | 631 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,270 | 1,899 | 1,439 | 1.20× | Fused | Win |
| Canada (prettified) | 2,030 | 1,911 | 1,299 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,925 | 1,777 | 1,185 | 1.65× | Fused | Win |
| CitmCatalog (prettified) | 3,675 | 3,388 | 2,357 | 1.08× | Fused | Win |
| Discord (minified) | 3,216 | 1,718 | 1,605 | 1.87× | Fused | Win |
| Discord (prettified) | 2,279 | 2,291 | 2,287 | 0.99× | Tie | Tie |
| Github Events (minified) | 2,592 | 1,949 | 1,840 | 1.33× | Fused | Win |
| Google Maps Response (minified) | 2,432 | 1,192 | 1,050 | 2.04× | Fused | Win |
| Github Events (prettified) | 1,999 | 1,948 | 2,179 | 1.03× | Fused | Loss |
| Google Maps Response (prettified) | 2,904 | 2,459 | 2,008 | 1.18× | Fused | Win |
| Instruments (minified) | 3,218 | 1,909 | 1,156 | 1.69× | Fused | Win |
| Instruments (prettified) | 3,143 | 2,372 | 2,067 | 1.33× | Fused | Win |
| Marine IK Reverse (minified) | 833 | 692 | 481 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,331 | 2,152 | 1,657 | 1.08× | Fused | Win |
| Marine IK (minified) | 785 (n/c) | 675 | 489 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,371 | 2,319 | 1,677 | 1.02× | Fused | Win |
| Random (minified) | 1,763 | 1,394 | 1,091 | 1.27× | Fused | Win |
| Random (prettified) | 2,094 | 1,828 | 1,604 | 1.15× | Fused | Win |
| Twitter (minified) | 3,040 | 2,248 | 1,980 | 1.35× | Fused | Win |
| Twitter (prettified) | 2,788 | 2,684 | 2,509 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 2,272 | 1,664 | 1,228 | 1.37× | Fused | Win |
| Canada Small (minified) | 730 | 726 | 478 | 1.01× | Tie | Win |
| Apache Builds Small (prettified) | 1,782 | 1,794 | 1,261 | 0.99× | Tie | Win |
| Canada Small (prettified) | 1,811 | 1,774 | 1,109 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,893 | 1,348 | 1,014 | 1.40× | Fused | Win |
| CitmCatalog Small (prettified) | 2,269 | 2,083 | 1,795 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,685 | 1,627 | 1,384 | 1.65× | Fused | Win |
| Discord Small (prettified) | 2,434 | 2,033 | 1,959 | 1.20× | Fused | Win |
| Github Events Small (minified) | 2,326 | 1,565 | 1,847 | 1.49× | Fused | Win |
| Google Maps Response Small (minified) | 2,288 | 1,250 | 951 | 1.83× | Fused | Win |
| Github Events Small (prettified) | 2,425 | 1,836 | 2,069 | 1.32× | Fused | Win |
| Google Maps Response Small (prettified) | 2,859 | 2,382 | 1,834 | 1.20× | Fused | Win |
| Instruments Small (minified) | 3,189 | 1,825 | 1,138 | 1.75× | Fused | Win |
| Instruments Small (prettified) | 2,574 | 2,646 | 1,839 | 0.97× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 787 | 587 | 379 | 1.34× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,877 | 2,053 | 1,343 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 817 | 668 | 415 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,288 | 2,124 | 1,388 | 1.08× | Fused | Win |
| Random Small (minified) | 1,696 | 1,220 | 1,070 | 1.39× | Fused | Win |
| Random Small (prettified) | 1,705 | 1,653 | 1,477 | 1.03× | Fused | Win |
| Twitter Small (minified) | 2,900 | 2,161 | 1,685 | 1.34× | Fused | Win |
| Twitter Small (prettified) | 2,302 | 2,549 | 2,068 | 0.90× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins 14 of the 15 POD-type tests, 30 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 31 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC follows the same split: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 19 and the fused path 3 (Apache Builds, Apache Builds Small and Github Events Small). On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (the tape takes minified Random Small on macOS/GCC) and 33 of the 44 prettified documents (eight to the tape, three ties).

**POD-type tests: the fused path wins 24 of 25 freshly allocated and all 25 reused.** It runs them 1.36× (Double, Windows/MSVC) to 4.16× (Bool, macOS/GCC) faster than the two-stage path, and 1.43× (Double, Windows/MSVC) to 4.72× (Bool, macOS/GCC) faster with objects reused. The exception is freshly allocated String under MSVC, where the two-stage path runs at 785 MB/s against 539; the fused result did not converge, and reused the fused path wins it. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** The fused path wins 106 of the 110 minified documents: all 22 under MSVC and on macOS/Clang, 21 of 22 on Linux/Clang, where minified Github Events is a tie, 21 of 22 on macOS/GCC, where the two-stage path wins minified Random Small by 4% (1,127 against 1,079 MB/s), and 20 of 22 on Linux/GCC, where minified Canada and Random are ties. The fused path's margins run from 2% (Marine IK on Linux/GCC, 762 against 750 MB/s) to 1.94× (Discord on macOS/Clang, 2,595 against 1,335 MB/s) on the full-size documents and up to 2.25× on the Small ones (Google Maps Response Small on macOS/Clang, 1,986 against 882 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 62 of 66.** It wins all 22 on Linux/GCC, 21 of 22 on Linux/Clang, and 19 of 22 under MSVC. The fused path wins prettified Marine IK Reverse on Linux/Clang, by 0.5%, and prettified Apache Builds, Apache Builds Small and Github Events Small under MSVC, by 5%, 9% and 9%. The tape's margins run from 3% (Canada on Linux/Clang, 2,136 against 2,072 MB/s) to 43% (Discord on Linux/Clang, 2,561 against 1,796) on the full-size documents and from 5% to 40% on the Small ones, widest on Random Small on Linux/Clang. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape again wins 62 of the 66 prettified x86 documents, and the fused path wins the same four.

**Prettified documents on the M2 Pro: mostly the fused path.** Of the 44 prettified documents, the fused path wins 33, the tape wins 8 and 3 are ties; the tape's wins are prettified Github Events, Apache Builds Small, Canada Small, Github Events Small, Marine IK Reverse Small and Random Small on macOS/GCC, and Apache Builds Small and Marine IK Reverse Small on macOS/Clang. Prettified Github Events, Marine IK Reverse and Twitter Small are ties on macOS/Clang. Reused, the fused path wins 27, the tape 10, and 7 are ties. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified Github Events and Twitter are where Jsonifier is weakest.** All four of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are prettified copies of those two documents: Github Events on macOS/Clang by 13% (1,637 against 1,875 MB/s), Github Events Small on Linux/GCC by 10% (2,243 against 2,491) and on Linux/Clang by 1% (2,278 against 2,295), and Twitter Small on Linux/Clang by 5% (2,669 against 2,812). The two ties are prettified Twitter on Linux/Clang and prettified Github Events Small on macOS/GCC. With objects reused the faster path loses two: Github Events Small on Linux/GCC by 11% (2,699 against 3,038) and Github Events on macOS/Clang by 8% (1,999 against 2,179). We have not profiled what these documents have in common.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 63% of its Linux/Clang throughput, the two-stage path 65% and simdjson 41%. Because the two Jsonifier paths lose a similar share, MSVC routes input much as Linux/Clang does. The spread between documents is about the same for the two Jsonifier paths (the fused path from 53% on Discord to 76% on Instruments, the two-stage path from 53% on Apache Builds to 75% on Instruments) and widest for simdjson, which keeps 32% of its Linux/Clang throughput on Github Events and 65% on Canada. We have not profiled why.

**Against simdjson, Jsonifier wins 239 of 245 tests with whichever path is faster, ties two and loses four** (all prettified documents). The fused path alone wins 225, ties 2 and loses 18, all prettified, its fourteen extra losses being nine on Linux/Clang, four on Linux/GCC and one on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 182 MB/s, against 331 MB/s for the same document in declared order, so the faster Jsonifier path is 3.20× faster than simdjson freshly allocated (3.45× reused). The prettified form shows the same gap, at 615 MB/s against 1,155, and the faster Jsonifier path is 3.08× faster (3.22× reused). The Small copies show it too, at 134 MB/s minified and 504 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.18× to 1.61× faster than simdjson on the full-size reversed document, freshly allocated, and 1.18× to 1.73× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, almost everything belongs to the fused path. Routing each test to its faster path would raise the record against simdjson from 225 wins, 2 ties and 18 losses to 239 wins, 2 ties and 4 losses. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,546 | 1,372 | 1.13× | Win |
| Canada (minified) | 1,010 | 873 | 1.16× | Win |
| Apache Builds (prettified) | 1,911 | 1,755 | 1.09× | Win |
| Canada (prettified) | 2,270 | 1,932 | 1.17× | Win |
| CitmCatalog (minified) | 1,851 | 1,424 | 1.30× | Win |
| CitmCatalog (prettified) | 3,700 | 2,974 | 1.24× | Win |
| Discord (minified) | 1,847 | 1,581 | 1.17× | Win |
| Discord (prettified) | 2,623 | 2,317 | 1.13× | Win |
| Github Events (minified) | 1,551 | 1,568 | 0.99× | **Loss** |
| Google Maps Response (minified) | 1,931 | 1,301 | 1.48× | Win |
| Github Events (prettified) | 2,099 | 2,246 | 0.93× | **Loss** |
| Google Maps Response (prettified) | 3,575 | 2,574 | 1.39× | Win |
| Instruments (minified) | 2,311 | 1,865 | 1.24× | Win |
| Instruments (prettified) | 3,442 | 2,945 | 1.17× | Win |
| Marine IK Reverse (minified) | 745 | 725 | 1.03× | Win |
| Marine IK Reverse (prettified) | 2,629 | 2,565 | 1.03× | Win |
| Marine IK (minified) | 750 | 698 | 1.07× | Win |
| Marine IK (prettified) | 2,629 | 2,496 | 1.05× | Win |
| Random (minified) | 1,128 | 1,088 | 1.04× | Win |
| Random (prettified) | 1,683 | 1,640 | 1.03× | Win |
| Twitter (minified) | 1,680 | 1,763 | 0.95× | **Loss** |
| Twitter (prettified) | 2,138 | 2,283 | 0.94× | **Loss** |
| Apache Builds Small (minified) | 2,209 | 1,755 | 1.26× | Win |
| Canada Small (minified) | 999 | 929 | 1.07× | Win |
| Apache Builds Small (prettified) | 2,660 | 2,208 | 1.20× | Win |
| Canada Small (prettified) | 2,219 | 2,158 | 1.03× | Win |
| CitmCatalog Small (minified) | 1,512 | 1,264 | 1.20× | Win |
| CitmCatalog Small (prettified) | 2,527 | 2,147 | 1.18× | Win |
| Discord Small (minified) | 1,723 | 1,540 | 1.12× | Win |
| Discord Small (prettified) | 2,242 | 2,074 | 1.08× | Win |
| Github Events Small (minified) | 1,848 | 1,874 | 0.99× | **Loss** |
| Google Maps Response Small (minified) | 1,821 | 1,284 | 1.42× | Win |
| Github Events Small (prettified) | 2,243 | 2,313 | 0.97× | **Loss** |
| Google Maps Response Small (prettified) | 3,365 | 2,538 | 1.33× | Win |
| Instruments Small (minified) | 2,259 | 1,748 | 1.29× | Win |
| Instruments Small (prettified) | 3,375 | 2,787 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 746 | 744 | 1.00× | Win |
| Marine IK Reverse Small (prettified) | 2,583 | 2,642 | 0.98× | **Loss** |
| Marine IK Small (minified) | 778 | 750 | 1.04× | Win |
| Marine IK Small (prettified) | 2,711 | 2,667 | 1.02× | Win |
| Random Small (minified) | 1,600 | 1,261 | 1.27× | Win |
| Random Small (prettified) | 2,371 | 1,923 | 1.23× | Win |
| Twitter Small (minified) | 2,371 | 2,144 | 1.11× | Win |
| Twitter Small (prettified) | 2,843 | 2,738 | 1.04× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 943 | 791 | 1.19× | Win |
| Canada (minified) | 751 | 654 | 1.15× | Win |
| Apache Builds (prettified) | 1,098 | 1,116 | 0.98× | **Loss** |
| Canada (prettified) | 1,662 | 1,351 | 1.23× | Win |
| CitmCatalog (minified) | 1,415 | 1,168 | 1.21× | Win |
| CitmCatalog (prettified) | 2,669 | 2,240 | 1.19× | Win |
| Discord (minified) | 1,281 | 1,090 | 1.18× | Win |
| Discord (prettified) | 1,776 | 1,507 | 1.18× | Win |
| Github Events (minified) | 1,102 | 1,310 | 0.84× | **Loss** |
| Google Maps Response (minified) | 1,316 | 847 | 1.55× | Win |
| Github Events (prettified) | 1,630 | 1,523 | 1.07× | Win |
| Google Maps Response (prettified) | 2,406 | 1,839 | 1.31× | Win |
| Instruments (minified) | 1,544 | 1,195 | 1.29× | Win |
| Instruments (prettified) | 2,167 | 1,987 | 1.09× | Win |
| Marine IK Reverse (minified) | 461 | 470 | 0.98× | Tie |
| Marine IK Reverse (prettified) | 1,659 | 1,591 | 1.04× | Win |
| Marine IK (minified) | 541 | 474 | 1.14× | Win |
| Marine IK (prettified) | 1,725 | 1,626 (n/c) | 1.06× | Win |
| Random (minified) | 772 | 673 | 1.15× | Win |
| Random (prettified) | 973 | 799 | 1.22× | Win |
| Twitter (minified) | 1,392 | 1,211 | 1.15× | Win |
| Twitter (prettified) | 1,648 | 1,437 | 1.15× | Win |
| Apache Builds Small (minified) | 1,153 | 984 | 1.17× | Win |
| Canada Small (minified) | 749 | 528 | 1.42× | Win |
| Apache Builds Small (prettified) | 1,647 | 1,290 | 1.28× | Win |
| Canada Small (prettified) | 1,632 | 1,384 | 1.18× | Win |
| CitmCatalog Small (minified) | 914 | 798 | 1.15× | Win |
| CitmCatalog Small (prettified) | 1,394 | 1,480 | 0.94× | **Loss** |
| Discord Small (minified) | 1,232 | 1,024 | 1.20× | Win |
| Discord Small (prettified) | 1,501 | 1,197 | 1.25× | Win |
| Github Events Small (minified) | 1,089 | 959 | 1.14× | Win |
| Google Maps Response Small (minified) | 1,024 | 982 | 1.04× | Win |
| Github Events Small (prettified) | 1,341 | 1,327 | 1.01× | Tie |
| Google Maps Response Small (prettified) | 2,016 | 1,532 | 1.32× | Win |
| Instruments Small (minified) | 1,453 | 1,088 | 1.34× | Win |
| Instruments Small (prettified) | 1,925 | 1,753 | 1.10× | Win |
| Marine IK Reverse Small (minified) | 458 | 495 | 0.93× | **Loss** |
| Marine IK Reverse Small (prettified) | 1,876 | 1,468 | 1.28× | Win |
| Marine IK Small (minified) | 515 | 439 | 1.17× | Win |
| Marine IK Small (prettified) | 1,704 | 1,578 | 1.08× | Win |
| Random Small (minified) | 1,127 | 858 | 1.31× | Win |
| Random Small (prettified) | 1,416 | 1,097 | 1.29× | Win |
| Twitter Small (minified) | 1,662 | 1,227 | 1.35× | Win |
| Twitter Small (prettified) | 1,883 | 1,423 | 1.32× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,272 | 1,780 | 1.28× | Win |
| Canada (minified) | 1,211 | 1,030 | 1.18× | Win |
| Apache Builds (prettified) | 2,809 | 2,280 | 1.23× | Win |
| Canada (prettified) | 2,712 | 2,270 | 1.20× | Win |
| CitmCatalog (minified) | 2,224 | 1,660 | 1.34× | Win |
| CitmCatalog (prettified) | 4,315 | 3,442 | 1.25× | Win |
| Discord (minified) | 2,505 | 1,845 | 1.36× | Win |
| Discord (prettified) | 3,412 | 2,622 | 1.30× | Win |
| Github Events (minified) | 2,053 | 1,936 | 1.06× | Win |
| Google Maps Response (minified) | 2,167 | 1,375 | 1.58× | Win |
| Github Events (prettified) | 2,667 | 2,321 | 1.15× | Win |
| Google Maps Response (prettified) | 3,898 | 2,726 | 1.43× | Win |
| Instruments (minified) | 2,631 | 1,925 | 1.37× | Win |
| Instruments (prettified) | 3,799 | 3,024 | 1.26× | Win |
| Marine IK Reverse (minified) | 821 | 814 | 1.01× | Win |
| Marine IK Reverse (prettified) | 2,868 | 2,855 | 1.00× | Tie |
| Marine IK (minified) | 836 | 799 | 1.05× | Win |
| Marine IK (prettified) | 2,903 | 2,814 | 1.03× | Win |
| Random (minified) | 1,516 | 1,430 | 1.06× | Win |
| Random (prettified) | 2,244 | 2,109 | 1.06× | Win |
| Twitter (minified) | 2,149 | 2,118 | 1.01× | Win |
| Twitter (prettified) | 2,693 | 2,688 | 1.00× | Tie |
| Apache Builds Small (minified) | 2,583 | 1,806 | 1.43× | Win |
| Canada Small (minified) | 1,246 | 1,108 | 1.12× | Win |
| Apache Builds Small (prettified) | 3,102 | 2,236 | 1.39× | Win |
| Canada Small (prettified) | 2,721 | 2,529 | 1.08× | Win |
| CitmCatalog Small (minified) | 1,760 | 1,358 | 1.30× | Win |
| CitmCatalog Small (prettified) | 2,909 | 2,341 | 1.24× | Win |
| Discord Small (minified) | 2,145 | 1,857 | 1.15× | Win |
| Discord Small (prettified) | 2,710 | 2,490 | 1.09× | Win |
| Github Events Small (minified) | 2,244 | 2,161 | 1.04× | Win |
| Google Maps Response Small (minified) | 2,035 | 1,373 | 1.48× | Win |
| Github Events Small (prettified) | 2,699 | 2,595 | 1.04× | Win |
| Google Maps Response Small (prettified) | 3,739 | 2,717 | 1.38× | Win |
| Instruments Small (minified) | 2,416 | 1,827 | 1.32× | Win |
| Instruments Small (prettified) | 3,604 | 2,902 | 1.24× | Win |
| Marine IK Reverse Small (minified) | 896 | 835 | 1.07× | Win |
| Marine IK Reverse Small (prettified) | 3,044 | 2,972 | 1.02× | Win |
| Marine IK Small (minified) | 953 | 855 | 1.11× | Win |
| Marine IK Small (prettified) | 3,202 | 3,021 | 1.06× | Win |
| Random Small (minified) | 1,975 | 1,438 | 1.37× | Win |
| Random Small (prettified) | 2,852 | 2,142 | 1.33× | Win |
| Twitter Small (minified) | 2,610 | 2,326 | 1.12× | Win |
| Twitter Small (prettified) | 3,128 | 2,977 | 1.05× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,119 | 992 | 1.13× | Win |
| Canada (minified) | 963 | 775 | 1.24× | Win |
| Apache Builds (prettified) | 1,580 | 1,284 | 1.23× | Win |
| Canada (prettified) | 1,919 | 1,652 | 1.16× | Win |
| CitmCatalog (minified) | 1,636 | 1,285 | 1.27× | Win |
| CitmCatalog (prettified) | 3,036 | 2,444 | 1.24× | Win |
| Discord (minified) | 1,638 | 1,224 | 1.34× | Win |
| Discord (prettified) | 2,188 | 1,687 | 1.30× | Win |
| Github Events (minified) | 1,364 | 1,556 | 0.88× | **Loss** |
| Google Maps Response (minified) | 1,492 | 1,065 | 1.40× | Win |
| Github Events (prettified) | 2,147 | 1,857 | 1.16× | Win |
| Google Maps Response (prettified) | 2,686 | 1,967 | 1.37× | Win |
| Instruments (minified) | 1,672 | 1,138 | 1.47× | Win |
| Instruments (prettified) | 2,397 | 2,058 | 1.16× | Win |
| Marine IK Reverse (minified) | 533 | 533 | 1.00× | Tie |
| Marine IK Reverse (prettified) | 2,080 | 1,731 | 1.20× | Win |
| Marine IK (minified) | 624 | 524 | 1.19× | Win |
| Marine IK (prettified) | 1,966 | 1,733 | 1.13× | Win |
| Random (minified) | 1,243 | 852 | 1.46× | Win |
| Random (prettified) | 1,639 | 1,196 | 1.37× | Win |
| Twitter (minified) | 1,980 | 1,340 | 1.48× | Win |
| Twitter (prettified) | 2,009 | 1,641 | 1.22× | Win |
| Apache Builds Small (minified) | 1,583 | 978 | 1.62× | Win |
| Canada Small (minified) | 822 | 609 | 1.35× | Win |
| Apache Builds Small (prettified) | 2,049 | 1,328 | 1.54× | Win |
| Canada Small (prettified) | 1,656 | 1,474 | 1.12× | Win |
| CitmCatalog Small (minified) | 1,336 | 1,019 | 1.31× | Win |
| CitmCatalog Small (prettified) | 1,882 | 1,425 | 1.32× | Win |
| Discord Small (minified) | 1,490 | 1,209 | 1.23× | Win |
| Discord Small (prettified) | 2,152 | 1,449 | 1.49× | Win |
| Github Events Small (minified) | 1,374 (n/c) | 1,198 | 1.15× | Win |
| Google Maps Response Small (minified) | 1,374 | 919 | 1.50× | Win |
| Github Events Small (prettified) | 1,809 | 1,676 | 1.08× | Win |
| Google Maps Response Small (prettified) | 2,274 | 1,790 | 1.27× | Win |
| Instruments Small (minified) | 1,554 | 1,250 | 1.24× | Win |
| Instruments Small (prettified) | 2,452 | 1,816 | 1.35× | Win |
| Marine IK Reverse Small (minified) | 610 | 538 | 1.14× | Win |
| Marine IK Reverse Small (prettified) | 2,238 | 1,886 | 1.19× | Win |
| Marine IK Small (minified) | 766 | 514 | 1.49× | Win |
| Marine IK Small (prettified) | 2,198 | 1,889 | 1.16× | Win |
| Random Small (minified) | 1,262 | 963 | 1.31× | Win |
| Random Small (prettified) | 1,878 | 1,287 | 1.46× | Win |
| Twitter Small (minified) | 1,761 | 1,500 | 1.17× | Win |
| Twitter Small (prettified) | 2,079 | 1,699 | 1.22× | Win |

In the freshly allocated run, two-stage Jsonifier wins 75 of these 88 tests, ties 2 and loses 11, every loss by 1% to 16%: seven on Linux/GCC (Github Events, Github Events Small and Twitter in both forms, and prettified Marine IK Reverse Small) and four on macOS/GCC (prettified Apache Builds and CitmCatalog Small, minified Marine IK Reverse Small, and minified Github Events, the widest at 0.84×). The ties are minified Marine IK Reverse and prettified Github Events Small on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 71, ties 3 and loses 14. With objects reused, two-stage Jsonifier wins 84, ties 3 and loses 1 (minified Github Events on macOS/GCC, by 12%), and the fused path beats the reflection reader on 74, ties 1 and loses 13. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 31 of the 40 results; all nine wider gaps are on the macOS builds, and the widest is reused minified Twitter Small on macOS/GCC, where the two-stage row runs at 4,269 MB/s against the "jsonifier" row's 3,364. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,408 | 3,551 | 1.80× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,587 | 4,111 | 1.85× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,125 | 3,196 | 1.29× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,781 | 3,839 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,095 | 7,073 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,185 | 8,401 | 1.09× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,496 | 5,792 | 0.95× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,434 | 6,963 | 0.92× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,102 | 7,237 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,155 | 8,411 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,987 | 5,985 | 1.00× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,047 | 7,133 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,047 | 3,264 (n/c) | 1.24× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,074 | 4,317 | 1.18× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,668 | 3,240 | 1.13× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,459 | 3,881 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,896 | 4,272 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,460 | 5,038 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,579 | 3,633 | 0.99× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,868 | 3,779 | 1.02× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,809 | 3,716 | 1.83× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,834 | 4,287 | 1.83× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,453 | 3,439 | 1.29× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,257 | 4,008 | 1.31× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,239 | 7,179 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,313 | 8,501 | 1.10× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,794 | 5,966 | 0.97× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,782 | 7,014 | 0.97× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,266 | 7,357 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,180 | 8,511 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,300 | 6,246 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,386 | 7,425 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,272 | 3,669 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,806 | 4,378 | 1.10× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,269 | 2,857 | 1.49× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,618 | 3,867 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 5,041 | 4,379 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,531 | 5,118 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,712 | 3,548 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,324 | 4,363 | 0.99× | Loss |

Jsonifier wins 31 of the 40 results, ties 1 and loses 8, and wins by up to 1.85× (prettified Twitter under MSVC, freshly allocated). Every loss is on the Small copy. Freshly allocated, the losses are Twitter Small on Linux/Clang in both forms (0.95× minified, 0.92× prettified), prettified on Linux/GCC (0.99×) and minified on macOS/Clang (0.99×), and the tie is minified Twitter Small on Linux/GCC. The reused run has 16 wins and 4 losses (0.97× to 0.99×): Twitter Small on Linux/Clang in both forms, and prettified on Linux/GCC and macOS/Clang. All 20 full-size results are wins. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,840 | 2,553 | 1.11× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,629 | 2,684 | 1.35× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 3,044 | 2,471 | 1.23× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,948 | 1,774 | 1.10× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,206 | 1,777 | 1.24× | Win |

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on 106 of 110 minified documents and on 33 of 44 prettified documents on the M2 Pro; slower on 62 of 66 prettified x86 documents (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 167 of 245 freshly allocated tests: 24 of 25 POD-type tests, 106 of 110 minified documents and 33 of 44 prettified documents on the M2 Pro, while the two-stage path wins 62 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 239 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 75 of 88, and on partial reading it wins 31 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*