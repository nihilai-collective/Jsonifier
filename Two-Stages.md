# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 106 of the 110 minified documents, the tape is faster on 62 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 25 of the 44 prettified documents, the tape 9 and 10 are ties, with 8 of the tape's 9 wins on the Small copies.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 5, 2026, with Jsonifier [86c028c](https://github.com/nihilai-collective/jsonifier/commit/86c028c), Glaze [00e6ce3](https://github.com/stephenberry/glaze/commit/00e6ce3), simdjson [37a1d72](https://github.com/simdjson/simdjson/commit/37a1d72) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Every x86 result converged. Three freshly allocated full-parse rows contain non-converged results (two on macOS/GCC and one on macOS/Clang, which has two), and three reused rows do (one on macOS/GCC and two on macOS/Clang, one of them with two). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 159 tests, 12 are statistical ties, and the two-stage path wins 74. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 1 | 18 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 36 | 6 | 7 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 40 | 5 | 4 | 48 of 49 |
| **Aggregate** | **159** | **12** | **74** | **242 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 20 / 1 / 1 |
| Prettified corpus documents | 1 / 1 / 9 | 0 / 0 / 22 | 17 / 4 / 1 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 8 / 6 / 8 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 45 / 1 / 3 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 43 / 0 / 6 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 47 / 0 / 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 / 0 / 2 | 47 / 0 / 2 |
| **Aggregate** | **238 / 1 / 6** | **223 / 0 / 22** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 40 | 3 | 6 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 36 | 6 | 7 | 47 of 49 |
| **Aggregate** | **159** | **9** | **77** | **242 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 20 / 2 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 0 / 0 / 22 | 14 / 5 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 19 / 1 / 2 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 13 / 1 / 8 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 / 1 / 1 | 37 / 0 / 12 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 45 / 0 / 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 47 / 0 / 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 48 / 1 / 0 |
| **Aggregate** | **241 / 2 / 2** | **225 / 1 / 19** |

With objects reused, across 245 tests the fused path wins 159, ties 9 and loses 77 to the two-stage path, and the x86 split holds: the tape wins 32 of the 33 full-size prettified documents (the fused path takes Apache Builds under MSVC) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 32 of the 33 full-size minified ones (on Linux/GCC the tape takes Canada, 1,255 against 1,177 MB/s). On the M2 Pro the fused path wins 39 of the 44 minified documents, with 3 ties and 2 tape wins (Canada Small and Marine IK Reverse Small on macOS/Clang), and 27 of the 44 prettified ones, with 6 ties and 11 tape wins. Against simdjson, the faster Jsonifier path wins 241, ties 2 and loses 2, and the fused path alone wins 225, ties 1 and loses 19.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 503 | 260 | 91 | 1.94× | Fused | Win |
| Double (POD) | 673 | 496 | 147 | 1.36× | Fused | Win |
| Int64 (POD) | 1,661 | 885 | 314 | 1.88× | Fused | Win |
| String (POD) | 991 | 777 | 548 | 1.28× | Fused | Win |
| Uint64 (POD) | 1,531 | 894 | 318 | 1.71× | Fused | Win |
| Apache Builds (minified) | 936 | 794 | 671 | 1.18× | Fused | Win |
| Canada (minified) | 692 | 656 | 430 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,059 | 1,027 | 880 | 1.03× | Fused | Win |
| Canada (prettified) | 1,399 | 1,478 | 950 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 1,641 | 1,305 | 589 | 1.26× | Fused | Win |
| CitmCatalog (prettified) | 2,354 | 2,690 | 1,234 | 0.88× | Two-stage | Win |
| Discord (minified) | 1,163 | 1,044 | 635 | 1.11× | Fused | Win |
| Discord (prettified) | 1,260 | 1,452 | 907 | 0.87× | Two-stage | Win |
| Github Events (minified) | 1,028 | 908 | 576 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 1,499 | 1,201 | 529 | 1.25× | Fused | Win |
| Github Events (prettified) | 1,054 | 1,089 | 686 | 0.97× | Two-stage | Win |
| Google Maps Response (prettified) | 2,261 | 2,377 | 1,054 | 0.95× | Two-stage | Win |
| Instruments (minified) | 1,851 | 1,491 | 704 | 1.24× | Fused | Win |
| Instruments (prettified) | 1,867 | 2,281 | 1,130 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 582 | 526 | 189 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 1,819 | 1,861 | 619 | 0.98× | Tie | Win |
| Marine IK (minified) | 599 | 532 | 344 | 1.13× | Fused | Win |
| Marine IK (prettified) | 1,844 | 1,903 | 1,210 | 0.97× | Two-stage | Win |
| Random (minified) | 779 | 710 | 536 | 1.10× | Fused | Win |
| Random (prettified) | 967 | 1,082 | 808 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,221 | 1,070 | 821 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,264 | 1,409 | 1,048 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 1,263 | 1,005 | 774 | 1.26× | Fused | Win |
| Canada Small (minified) | 650 | 613 | 351 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,352 | 1,217 | 974 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,365 | 1,434 | 822 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 856 | 751 | 507 | 1.14× | Fused | Win |
| CitmCatalog Small (prettified) | 1,154 | 1,344 | 879 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,090 | 929 | 679 | 1.17× | Fused | Win |
| Discord Small (prettified) | 1,120 | 1,222 | 900 | 0.92× | Two-stage | Win |
| Github Events Small (minified) | 1,217 | 842 | 582 | 1.45× | Fused | Win |
| Google Maps Response Small (minified) | 1,508 | 1,110 | 547 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 1,160 | 1,070 | 724 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,107 | 2,282 | 1,069 | 0.92× | Two-stage | Win |
| Instruments Small (minified) | 1,953 | 1,465 | 708 | 1.33× | Fused | Win |
| Instruments Small (prettified) | 1,808 | 2,288 | 1,157 | 0.79× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 472 | 439 | 142 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,447 | 1,591 | 536 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 507 | 464 | 298 | 1.09× | Fused | Win |
| Marine IK Small (prettified) | 1,512 | 1,675 | 1,060 | 0.90× | Two-stage | Win |
| Random Small (minified) | 922 | 795 | 530 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,145 | 1,212 | 803 | 0.94× | Two-stage | Win |
| Twitter Small (minified) | 1,423 | 1,275 | 1,170 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 1,430 | 1,644 | 1,488 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,013 | 644 | 289 | 3.13× | Fused | Win |
| Double (POD) | 1,258 | 690 | 373 | 1.82× | Fused | Win |
| Int64 (POD) | 3,078 | 1,612 | 791 | 1.91× | Fused | Win |
| String (POD) | 1,825 | 1,301 | 1,353 | 1.40× | Fused | Win |
| Uint64 (POD) | 3,234 | 1,588 | 775 | 2.04× | Fused | Win |
| Apache Builds (minified) | 1,734 | 1,552 | 1,239 | 1.12× | Fused | Win |
| Canada (minified) | 1,040 | 984 | 679 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,709 | 1,933 | 1,600 | 0.88× | Two-stage | Win |
| Canada (prettified) | 2,035 | 2,154 | 1,538 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,350 | 1,818 | 1,201 | 1.29× | Fused | Win |
| CitmCatalog (prettified) | 2,870 | 3,614 | 2,526 | 0.79× | Two-stage | Win |
| Discord (minified) | 2,244 | 1,842 | 1,455 | 1.22× | Fused | Win |
| Discord (prettified) | 1,833 | 2,602 | 2,080 | 0.70× | Two-stage | Win |
| Github Events (minified) | 1,646 | 1,607 | 1,540 | 1.02× | Fused | Win |
| Google Maps Response (minified) | 2,668 | 1,987 | 1,353 | 1.34× | Fused | Win |
| Github Events (prettified) | 1,574 | 1,886 | 2,106 | 0.83× | Two-stage | Loss |
| Google Maps Response (prettified) | 2,885 | 3,668 | 2,702 | 0.79× | Two-stage | Win |
| Instruments (minified) | 2,381 | 2,061 | 1,737 | 1.16× | Fused | Win |
| Instruments (prettified) | 2,267 | 2,968 | 2,759 | 0.76× | Two-stage | Win |
| Marine IK Reverse (minified) | 827 | 741 | 587 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,450 | 2,647 | 2,104 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 839 | 750 | 594 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,488 | 2,633 | 2,128 | 0.94× | Two-stage | Win |
| Random (minified) | 1,242 | 1,106 | 1,062 | 1.12× | Fused | Win |
| Random (prettified) | 1,401 | 1,666 | 1,608 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,889 | 1,731 | 1,697 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,707 | 2,181 | 2,212 | 0.78× | Two-stage | Loss |
| Apache Builds Small (minified) | 2,690 | 2,312 | 1,515 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,038 | 992 | 708 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 2,176 | 2,727 | 1,852 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 1,980 | 2,262 | 1,680 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,722 | 1,470 | 1,196 | 1.17× | Fused | Win |
| CitmCatalog Small (prettified) | 1,861 | 2,418 | 2,115 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 2,060 | 1,770 | 1,432 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,730 | 2,370 | 1,923 | 0.73× | Two-stage | Win |
| Github Events Small (minified) | 2,055 | 1,869 | 1,891 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 2,433 | 1,825 | 1,317 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,966 | 2,317 | 2,308 | 0.85× | Two-stage | Tie |
| Google Maps Response Small (prettified) | 2,673 | 3,406 | 2,591 | 0.78× | Two-stage | Win |
| Instruments Small (minified) | 2,473 | 2,026 | 1,598 | 1.22× | Fused | Win |
| Instruments Small (prettified) | 2,252 | 3,010 | 2,561 | 0.75× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 814 | 745 | 578 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,126 | 2,638 | 2,096 | 0.81× | Two-stage | Win |
| Marine IK Small (minified) | 915 | 806 | 593 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,295 | 2,830 | 2,135 | 0.81× | Two-stage | Win |
| Random Small (minified) | 1,728 | 1,573 | 1,133 | 1.10× | Fused | Win |
| Random Small (prettified) | 1,720 | 2,371 | 1,737 | 0.73× | Two-stage | Win |
| Twitter Small (minified) | 2,532 | 2,189 | 2,185 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 2,032 | 2,658 | 2,817 | 0.76× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,263 | 559 | 229 | 4.05× | Fused | Win |
| Double (POD) | 1,252 | 626 | 304 | 2.00× | Fused | Win |
| Int64 (POD) | 3,375 | 1,420 | 727 | 2.38× | Fused | Win |
| String (POD) | 1,803 | 1,243 | 1,286 | 1.45× | Fused | Win |
| Uint64 (POD) | 3,432 | 1,454 | 710 | 2.36× | Fused | Win |
| Apache Builds (minified) | 1,686 | 1,510 | 1,374 | 1.12× | Fused | Win |
| Canada (minified) | 980 | 1,033 | 854 | 0.95× | Two-stage | Win |
| Apache Builds (prettified) | 1,715 | 1,909 | 1,766 | 0.90× | Two-stage | Win |
| Canada (prettified) | 2,120 | 2,239 | 1,905 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 2,316 | 1,832 | 1,359 | 1.26× | Fused | Win |
| CitmCatalog (prettified) | 3,175 | 3,656 | 2,843 | 0.87× | Two-stage | Win |
| Discord (minified) | 2,263 | 1,796 | 1,432 | 1.26× | Fused | Win |
| Discord (prettified) | 2,174 | 2,591 | 2,020 | 0.84× | Two-stage | Win |
| Github Events (minified) | 1,628 | 1,557 | 1,543 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,500 | 1,914 | 1,073 | 1.31× | Fused | Win |
| Github Events (prettified) | 1,681 | 1,834 | 1,733 | 0.92× | Two-stage | Win |
| Google Maps Response (prettified) | 3,272 | 3,581 | 2,184 | 0.91× | Two-stage | Win |
| Instruments (minified) | 3,252 | 2,262 | 1,390 | 1.44× | Fused | Win |
| Instruments (prettified) | 2,926 | 3,395 | 2,261 | 0.86× | Two-stage | Win |
| Marine IK Reverse (minified) | 734 | 722 | 605 | 1.02× | Fused | Win |
| Marine IK Reverse (prettified) | 2,345 | 2,564 | 2,187 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 740 | 733 | 600 | 1.01× | Fused | Win |
| Marine IK (prettified) | 2,408 | 2,626 | 2,197 | 0.92× | Two-stage | Win |
| Random (minified) | 1,114 | 1,092 | 1,040 | 1.02× | Fused | Win |
| Random (prettified) | 1,408 | 1,701 | 1,577 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,954 | 1,667 | 1,328 | 1.17× | Fused | Win |
| Twitter (prettified) | 1,689 | 2,174 | 1,742 | 0.78× | Two-stage | Win |
| Apache Builds Small (minified) | 2,442 | 2,140 | 1,605 | 1.14× | Fused | Win |
| Canada Small (minified) | 1,073 | 967 | 780 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,273 | 2,629 | 1,990 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 2,098 | 2,216 | 1,820 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,776 | 1,489 | 1,159 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 2,030 | 2,502 | 2,021 | 0.81× | Two-stage | Win |
| Discord Small (minified) | 1,981 | 1,747 | 1,247 | 1.13× | Fused | Win |
| Discord Small (prettified) | 1,884 | 2,220 | 1,657 | 0.85× | Two-stage | Win |
| Github Events Small (minified) | 2,104 | 1,833 | 1,932 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,284 | 1,785 | 1,116 | 1.28× | Fused | Win |
| Github Events Small (prettified) | 2,018 | 2,257 | 2,355 | 0.89× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,973 | 3,307 | 2,200 | 0.90× | Two-stage | Win |
| Instruments Small (minified) | 3,315 | 2,272 | 1,291 | 1.46× | Fused | Win |
| Instruments Small (prettified) | 2,857 | 3,346 | 2,079 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 836 | 723 | 590 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,299 | 2,587 | 2,169 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 922 | 780 | 593 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 2,461 | 2,754 | 2,155 | 0.89× | Two-stage | Win |
| Random Small (minified) | 1,673 | 1,573 | 1,156 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,958 | 2,357 | 1,766 | 0.83× | Two-stage | Win |
| Twitter Small (minified) | 2,858 | 2,324 | 1,686 | 1.23× | Fused | Win |
| Twitter Small (prettified) | 2,095 | 2,937 | 2,185 | 0.71× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,694 | 453 | 159 | 3.74× | Fused | Win |
| Double (POD) | 902 | 400 | 152 | 2.25× | Fused | Win |
| Int64 (POD) | 2,669 | 965 | 465 | 2.77× | Fused | Win |
| String (POD) | 1,274 | 741 | 696 | 1.72× | Fused | Win |
| Uint64 (POD) | 2,182 | 877 | 365 | 2.49× | Fused | Win |
| Apache Builds (minified) | 1,085 | 938 | 623 | 1.16× | Fused | Win |
| Canada (minified) | 741 | 729 | 573 | 1.02× | Fused | Win |
| Apache Builds (prettified) | 1,056 | 1,024 | 771 | 1.03× | Fused | Win |
| Canada (prettified) | 1,754 | 1,360 | 1,122 | 1.29× | Fused | Win |
| CitmCatalog (minified) | 1,830 | 1,210 | 953 | 1.51× | Fused | Win |
| CitmCatalog (prettified) | 2,607 | 2,168 | 2,048 | 1.20× | Fused | Win |
| Discord (minified) | 1,584 | 1,229 | 1,017 | 1.29× | Fused | Win |
| Discord (prettified) | 1,825 | 1,604 | 1,418 | 1.14× | Fused | Win |
| Github Events (minified) | 1,639 | 888 | 938 | 1.85× | Fused | Win |
| Google Maps Response (minified) | 1,964 | 1,218 | 665 | 1.61× | Fused | Win |
| Github Events (prettified) | 1,149 | 1,637 | 1,181 | 0.70× | Two-stage | Win |
| Google Maps Response (prettified) | 2,689 | 2,413 | 1,500 | 1.11× | Fused | Win |
| Instruments (minified) | 2,530 | 1,576 | 1,283 | 1.61× | Fused | Win |
| Instruments (prettified) | 2,854 | 1,921 | 1,455 | 1.49× | Fused | Win |
| Marine IK Reverse (minified) | 686 | 475 | 399 | 1.44× | Fused | Win |
| Marine IK Reverse (prettified) | 2,020 | 1,995 | 1,675 | 1.01× | Fused | Win |
| Marine IK (minified) | 597 | 612 | 489 | 0.98× | Two-stage | Win |
| Marine IK (prettified) | 2,072 | 2,029 | 1,481 | 1.02× | Tie | Win |
| Random (minified) | 916 | 919 | 684 | 1.00× | Tie | Win |
| Random (prettified) | 1,116 | 884 | 906 | 1.26× | Fused | Win |
| Twitter (minified) | 2,050 | 1,711 | 1,063 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,039 | 2,047 | 1,363 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 1,510 | 1,259 (n/c) | 827 | 1.20× | Fused | Win |
| Canada Small (minified) | 754 | 709 | 377 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,441 | 1,556 | 941 | 0.93× | Two-stage | Win |
| Canada Small (prettified) | 1,515 | 1,532 | 1,065 | 0.99× | Tie | Win |
| CitmCatalog Small (minified) | 1,135 | 968 | 799 | 1.17× | Fused | Win |
| CitmCatalog Small (prettified) | 1,661 | 1,573 | 1,419 (n/c) | 1.06× | Fused | Win |
| Discord Small (minified) | 1,430 | 1,085 | 909 | 1.32× | Fused | Win |
| Discord Small (prettified) | 1,483 | 1,449 | 1,168 | 1.02× | Tie | Win |
| Github Events Small (minified) | 1,474 | 1,131 | 946 | 1.30× | Fused | Win |
| Google Maps Response Small (minified) | 1,603 | 1,100 | 670 | 1.46× | Fused | Win |
| Github Events Small (prettified) | 1,016 | 1,355 | 1,304 | 0.75× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,296 | 2,077 | 1,389 | 1.11× | Fused | Win |
| Instruments Small (minified) | 2,520 | 1,673 | 1,074 | 1.51× | Fused | Win |
| Instruments Small (prettified) | 2,488 | 2,139 | 1,681 | 1.16× | Fused | Win |
| Marine IK Reverse Small (minified) | 608 | 501 | 365 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,448 | 1,539 | 1,287 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 686 | 568 | 369 | 1.21× | Fused | Win |
| Marine IK Small (prettified) | 1,739 | 1,881 | 1,321 | 0.92× | Two-stage | Win |
| Random Small (minified) | 1,162 | 1,142 | 706 | 1.02× | Fused | Win |
| Random Small (prettified) | 1,305 | 1,281 | 1,049 | 1.02× | Tie | Win |
| Twitter Small (minified) | 1,861 | 1,667 | 1,092 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 1,893 | 1,986 | 1,413 | 0.95× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,198 | 316 | 103 | 3.79× | Fused | Win |
| Double (POD) | 890 | 337 | 149 | 2.64× | Fused | Win |
| Int64 (POD) | 2,203 | 836 | 303 | 2.64× | Fused | Win |
| String (POD) | 1,199 | 649 | 698 | 1.85× | Fused | Win |
| Uint64 (POD) | 2,152 | 774 | 347 | 2.78× | Fused | Win |
| Apache Builds (minified) | 1,444 | 1,021 | 890 | 1.41× | Fused | Win |
| Canada (minified) | 743 | 733 | 511 | 1.01× | Fused | Win |
| Apache Builds (prettified) | 1,593 | 1,310 | 1,192 | 1.22× | Fused | Win |
| Canada (prettified) | 1,538 | 1,430 (n/c) | 964 (n/c) | 1.08× | Fused | Win |
| CitmCatalog (minified) | 2,164 | 1,527 | 1,088 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 2,999 | 2,943 | 2,207 | 1.02× | Fused | Win |
| Discord (minified) | 2,562 | 1,325 | 1,387 | 1.93× | Fused | Win |
| Discord (prettified) | 2,455 | 1,822 | 1,972 | 1.35× | Fused | Win |
| Github Events (minified) | 1,868 | 1,539 | 1,590 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 2,344 | 1,281 | 921 | 1.83× | Fused | Win |
| Github Events (prettified) | 1,809 | 1,751 | 1,778 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 3,063 | 2,392 | 2,029 | 1.28× | Fused | Win |
| Instruments (minified) | 2,797 | 1,804 | 1,303 | 1.55× | Fused | Win |
| Instruments (prettified) | 2,462 | 2,339 | 1,922 | 1.05× | Fused | Win |
| Marine IK Reverse (minified) | 681 | 620 | 354 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 1,777 | 1,677 | 1,298 | 1.06× | Fused | Win |
| Marine IK (minified) | 630 | 582 | 448 | 1.08× | Fused | Win |
| Marine IK (prettified) | 2,090 | 2,089 | 1,566 | 1.00× | Tie | Win |
| Random (minified) | 1,165 | 1,013 | 750 | 1.15× | Fused | Win |
| Random (prettified) | 1,484 | 1,039 | 1,088 | 1.43× | Fused | Win |
| Twitter (minified) | 1,834 | 1,335 | 1,237 | 1.37× | Fused | Win |
| Twitter (prettified) | 1,751 | 1,744 | 1,586 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 1,241 | 1,293 | 766 | 0.96× | Two-stage | Win |
| Canada Small (minified) | 589 | 566 | 380 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 1,597 | 1,242 | 1,190 | 1.29× | Fused | Win |
| Canada Small (prettified) | 1,171 | 1,152 | 944 | 1.02× | Tie | Win |
| CitmCatalog Small (minified) | 950 | 735 | 639 | 1.29× | Fused | Win |
| CitmCatalog Small (prettified) | 1,316 | 1,207 | 1,075 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,578 | 962 | 1,024 | 1.64× | Fused | Win |
| Discord Small (prettified) | 1,684 | 1,290 | 1,373 | 1.31× | Fused | Win |
| Github Events Small (minified) | 1,242 | 957 | 1,179 | 1.30× | Fused | Win |
| Google Maps Response Small (minified) | 1,727 | 1,010 | 949 | 1.71× | Fused | Win |
| Github Events Small (prettified) | 1,348 | 1,312 | 1,452 | 1.03× | Tie | Loss |
| Google Maps Response Small (prettified) | 2,151 | 1,666 | 1,436 | 1.29× | Fused | Win |
| Instruments Small (minified) | 2,382 | 1,352 | 908 | 1.76× | Fused | Win |
| Instruments Small (prettified) | 2,072 | 2,402 | 1,467 | 0.86× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 574 | 425 | 315 | 1.35× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,578 | 1,686 | 1,269 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 535 | 448 | 282 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 1,531 | 1,572 | 1,048 | 0.97× | Two-stage | Win |
| Random Small (minified) | 1,303 | 788 | 676 | 1.65× | Fused | Win |
| Random Small (prettified) | 1,329 | 1,206 | 1,208 | 1.10× | Fused | Win |
| Twitter Small (minified) | 2,197 | 1,487 | 1,407 | 1.48× | Fused | Win |
| Twitter Small (prettified) | 2,013 | 1,989 | 2,068 | 1.01× | Tie | Loss |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 569 | 259 | 92 | 2.19× | Fused | Win |
| Double (POD) | 824 | 577 | 154 | 1.43× | Fused | Win |
| Int64 (POD) | 2,059 | 958 | 327 | 2.15× | Fused | Win |
| String (POD) | 1,679 | 1,150 | 700 | 1.46× | Fused | Win |
| Uint64 (POD) | 1,829 | 946 | 330 | 1.93× | Fused | Win |
| Apache Builds (minified) | 1,505 | 1,190 | 954 | 1.27× | Fused | Win |
| Canada (minified) | 834 | 780 | 484 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,601 | 1,526 | 1,201 | 1.05× | Fused | Win |
| Canada (prettified) | 1,610 | 1,750 | 1,034 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,943 | 1,502 | 617 | 1.29× | Fused | Win |
| CitmCatalog (prettified) | 2,633 | 3,046 | 1,281 | 0.86× | Two-stage | Win |
| Discord (minified) | 1,650 | 1,404 | 790 | 1.18× | Fused | Win |
| Discord (prettified) | 1,597 | 1,930 | 1,122 | 0.83× | Two-stage | Win |
| Github Events (minified) | 1,564 | 1,292 | 706 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 1,726 | 1,366 | 575 | 1.26× | Fused | Win |
| Github Events (prettified) | 1,484 | 1,552 | 843 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 2,503 | 2,600 | 1,136 | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,377 | 1,764 | 795 | 1.35× | Fused | Win |
| Instruments (prettified) | 2,147 | 2,704 | 1,264 | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 637 | 574 | 196 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 1,977 | 2,072 | 661 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 658 | 593 | 370 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,035 | 2,074 | 1,278 | 0.98× | Two-stage | Win |
| Random (minified) | 1,065 | 960 | 652 | 1.11× | Fused | Win |
| Random (prettified) | 1,257 | 1,456 | 982 | 0.86× | Two-stage | Win |
| Twitter (minified) | 1,700 | 1,479 | 1,039 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,633 | 1,944 | 1,310 | 0.84× | Two-stage | Win |
| Apache Builds Small (minified) | 1,588 | 1,226 | 988 | 1.30× | Fused | Win |
| Canada Small (minified) | 812 | 753 | 397 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,621 | 1,520 | 1,221 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,588 | 1,770 | 897 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,174 | 983 | 581 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,424 | 1,683 | 976 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 1,414 | 1,220 | 855 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,376 | 1,594 | 1,136 | 0.86× | Two-stage | Win |
| Github Events Small (minified) | 1,728 | 1,125 | 683 | 1.54× | Fused | Win |
| Google Maps Response Small (minified) | 1,763 | 1,266 | 579 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 1,490 | 1,430 | 858 | 1.04× | Fused | Win |
| Google Maps Response Small (prettified) | 2,338 | 2,583 | 1,177 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 2,193 | 1,586 | 779 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 1,916 | 2,505 | 1,260 | 0.77× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 584 | 542 | 147 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,666 | 1,971 | 552 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 628 | 573 | 335 | 1.10× | Fused | Win |
| Marine IK Small (prettified) | 1,750 | 2,100 | 1,176 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,115 | 947 | 636 | 1.18× | Fused | Win |
| Random Small (prettified) | 1,305 | 1,478 | 964 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 1,741 | 1,503 | 1,274 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 1,618 | 1,950 | 1,646 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,220 | 656 | 289 | 3.39× | Fused | Win |
| Double (POD) | 1,409 | 774 | 375 | 1.82× | Fused | Win |
| Int64 (POD) | 3,425 | 1,753 | 799 | 1.95× | Fused | Win |
| String (POD) | 2,921 | 1,770 | 1,923 | 1.65× | Fused | Win |
| Uint64 (POD) | 3,563 | 1,648 | 783 | 2.16× | Fused | Win |
| Apache Builds (minified) | 2,959 | 2,395 | 1,738 | 1.24× | Fused | Win |
| Canada (minified) | 1,264 | 1,186 | 790 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 2,436 | 2,877 | 2,239 | 0.85× | Two-stage | Win |
| Canada (prettified) | 2,409 | 2,571 | 1,772 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,913 | 2,164 | 1,408 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 3,178 | 4,271 | 2,927 | 0.74× | Two-stage | Win |
| Discord (minified) | 3,288 | 2,505 | 1,870 | 1.31× | Fused | Win |
| Discord (prettified) | 2,211 | 3,475 | 2,658 | 0.64× | Two-stage | Win |
| Github Events (minified) | 2,234 | 2,142 | 2,039 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 3,144 | 2,241 | 1,453 | 1.40× | Fused | Win |
| Github Events (prettified) | 1,988 | 2,501 | 2,430 | 0.79× | Two-stage | Win |
| Google Maps Response (prettified) | 3,144 | 4,041 | 2,898 | 0.78× | Two-stage | Win |
| Instruments (minified) | 2,762 | 2,344 | 1,915 | 1.18× | Fused | Win |
| Instruments (prettified) | 2,463 | 3,328 | 3,043 | 0.74× | Two-stage | Win |
| Marine IK Reverse (minified) | 917 | 812 | 643 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 2,677 | 2,813 | 2,335 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 938 | 830 | 647 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,694 | 2,906 | 2,339 | 0.93× | Two-stage | Win |
| Random (minified) | 1,726 | 1,507 | 1,473 | 1.15× | Fused | Win |
| Random (prettified) | 1,747 | 2,222 | 2,216 | 0.79× | Two-stage | Tie |
| Twitter (minified) | 2,426 | 2,289 | 2,136 | 1.06× | Fused | Win |
| Twitter (prettified) | 2,016 | 2,821 | 2,748 | 0.71× | Two-stage | Win |
| Apache Builds Small (minified) | 3,140 | 2,648 | 1,788 | 1.19× | Fused | Win |
| Canada Small (minified) | 1,305 | 1,251 | 816 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 2,409 | 3,144 | 2,321 | 0.77× | Two-stage | Win |
| Canada Small (prettified) | 2,340 | 2,775 | 1,935 | 0.84× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,140 | 1,753 | 1,370 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,123 | 2,874 | 2,358 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,775 | 2,183 | 1,846 | 1.27× | Fused | Win |
| Discord Small (prettified) | 2,089 | 2,915 | 2,470 | 0.72× | Two-stage | Win |
| Github Events Small (minified) | 2,610 | 2,260 | 2,290 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,796 | 2,081 | 1,491 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 2,335 | 2,788 | 2,833 | 0.84× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,891 | 3,811 | 2,936 | 0.76× | Two-stage | Win |
| Instruments Small (minified) | 2,636 | 2,176 | 1,748 | 1.21× | Fused | Win |
| Instruments Small (prettified) | 2,349 | 3,134 | 2,784 | 0.75× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,002 | 896 | 652 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,446 | 3,082 | 2,403 | 0.79× | Two-stage | Win |
| Marine IK Small (minified) | 1,132 | 993 | 666 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,634 | 3,377 | 2,376 | 0.78× | Two-stage | Win |
| Random Small (minified) | 2,190 | 1,940 | 1,452 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,987 | 2,884 | 2,150 | 0.69× | Two-stage | Win |
| Twitter Small (minified) | 2,780 | 2,484 | 2,355 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 2,184 | 3,034 | 2,991 | 0.72× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,459 | 572 | 227 | 4.30× | Fused | Win |
| Double (POD) | 1,341 | 668 | 303 | 2.01× | Fused | Win |
| Int64 (POD) | 3,763 | 1,520 | 743 | 2.48× | Fused | Win |
| String (POD) | 2,794 | 1,620 | 1,723 | 1.72× | Fused | Win |
| Uint64 (POD) | 3,838 | 1,519 | 716 | 2.53× | Fused | Win |
| Apache Builds (minified) | 2,661 | 2,238 | 1,944 | 1.19× | Fused | Win |
| Canada (minified) | 1,177 | 1,255 | 1,028 | 0.94× | Two-stage | Win |
| Apache Builds (prettified) | 2,424 | 2,742 | 2,493 | 0.88× | Two-stage | Win |
| Canada (prettified) | 2,474 | 2,714 | 2,280 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,864 | 2,203 | 1,623 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,615 | 4,294 | 3,375 | 0.84× | Two-stage | Win |
| Discord (minified) | 3,278 | 2,455 | 1,767 | 1.34× | Fused | Win |
| Discord (prettified) | 2,728 | 3,366 | 2,522 | 0.81× | Two-stage | Win |
| Github Events (minified) | 2,209 | 2,022 | 1,718 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,902 | 2,113 | 1,152 | 1.37× | Fused | Win |
| Github Events (prettified) | 2,059 | 2,600 | 2,051 | 0.79× | Two-stage | Win |
| Google Maps Response (prettified) | 3,545 | 3,875 | 2,318 | 0.91× | Two-stage | Win |
| Instruments (minified) | 3,869 | 2,580 | 1,472 | 1.50× | Fused | Win |
| Instruments (prettified) | 3,207 | 3,791 | 2,384 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 806 | 799 | 666 | 1.01× | Fused | Win |
| Marine IK Reverse (prettified) | 2,550 | 2,774 | 2,446 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 817 | 811 | 664 | 1.01× | Fused | Win |
| Marine IK (prettified) | 2,608 | 2,883 | 2,399 | 0.90× | Two-stage | Win |
| Random (minified) | 1,478 | 1,468 | 1,416 | 1.01× | Fused | Win |
| Random (prettified) | 1,777 | 2,254 | 2,110 | 0.79× | Two-stage | Win |
| Twitter (minified) | 2,515 | 2,113 | 1,593 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,980 | 2,732 | 2,117 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 2,890 | 2,485 | 1,831 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,359 | 1,215 | 911 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,562 | 3,044 | 2,266 | 0.84× | Two-stage | Win |
| Canada Small (prettified) | 2,497 | 2,769 | 2,127 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,146 | 1,749 | 1,332 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,309 | 2,936 | 2,296 | 0.79× | Two-stage | Win |
| Discord Small (minified) | 2,702 | 2,170 | 1,559 | 1.25× | Fused | Win |
| Discord Small (prettified) | 2,308 | 2,750 | 2,122 | 0.84× | Two-stage | Win |
| Github Events Small (minified) | 2,628 | 2,268 | 2,368 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,671 | 2,035 | 1,249 | 1.31× | Fused | Win |
| Github Events Small (prettified) | 2,438 | 2,661 | 2,871 | 0.92× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,228 | 3,678 | 2,441 | 0.88× | Two-stage | Win |
| Instruments Small (minified) | 3,604 | 2,426 | 1,357 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,999 | 3,536 | 2,205 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,036 | 878 | 652 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,652 | 3,074 | 2,361 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,152 | 941 | 637 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,852 | 3,228 | 2,305 | 0.88× | Two-stage | Win |
| Random Small (minified) | 2,076 | 1,925 | 1,434 | 1.08× | Fused | Win |
| Random Small (prettified) | 2,298 | 2,848 | 2,165 | 0.81× | Two-stage | Win |
| Twitter Small (minified) | 3,237 | 2,588 | 1,761 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,419 | 3,256 | 2,305 | 0.74× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,896 | 456 | 189 | 4.16× | Fused | Win |
| Double (POD) | 1,087 | 506 | 157 | 2.15× | Fused | Win |
| Int64 (POD) | 3,350 | 925 | 457 | 3.62× | Fused | Win |
| String (POD) | 2,152 | 961 | 811 | 2.24× | Fused | Win |
| Uint64 (POD) | 2,926 | 861 | 356 | 3.40× | Fused | Win |
| Apache Builds (minified) | 1,992 | 1,297 | 841 | 1.54× | Fused | Win |
| Canada (minified) | 988 | 978 | 609 | 1.01× | Tie | Win |
| Apache Builds (prettified) | 1,781 | 1,575 | 1,101 | 1.13× | Fused | Win |
| Canada (prettified) | 2,126 | 1,642 | 1,382 (n/c) | 1.29× | Fused | Win |
| CitmCatalog (minified) | 2,529 | 1,363 | 1,035 | 1.86× | Fused | Win |
| CitmCatalog (prettified) | 3,197 | 2,443 | 2,184 | 1.31× | Fused | Win |
| Discord (minified) | 2,108 | 1,635 | 1,227 | 1.29× | Fused | Win |
| Discord (prettified) | 2,210 | 1,964 | 1,432 | 1.12× | Fused | Win |
| Github Events (minified) | 2,232 | 1,210 | 1,090 | 1.85× | Fused | Win |
| Google Maps Response (minified) | 1,771 | 1,382 | 846 | 1.28× | Fused | Win |
| Github Events (prettified) | 1,364 | 2,117 | 1,443 | 0.64× | Two-stage | Win |
| Google Maps Response (prettified) | 2,936 | 2,723 | 1,586 | 1.08× | Fused | Win |
| Instruments (minified) | 3,081 | 1,790 | 1,393 | 1.72× | Fused | Win |
| Instruments (prettified) | 2,914 | 2,188 | 1,686 | 1.33× | Fused | Win |
| Marine IK Reverse (minified) | 673 | 543 | 497 | 1.24× | Fused | Win |
| Marine IK Reverse (prettified) | 2,232 | 2,204 | 1,826 | 1.01× | Fused | Win |
| Marine IK (minified) | 691 | 692 | 539 | 1.00× | Tie | Win |
| Marine IK (prettified) | 2,350 | 2,295 | 1,548 | 1.02× | Fused | Win |
| Random (minified) | 1,282 | 1,222 | 811 | 1.05× | Fused | Win |
| Random (prettified) | 1,530 | 1,688 | 1,124 | 0.91× | Two-stage | Win |
| Twitter (minified) | 2,444 | 2,209 | 1,208 | 1.11× | Fused | Win |
| Twitter (prettified) | 2,586 | 2,596 | 1,547 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 1,998 | 1,741 | 1,119 | 1.15× | Fused | Win |
| Canada Small (minified) | 972 | 792 | 429 | 1.23× | Fused | Win |
| Apache Builds Small (prettified) | 1,603 | 1,803 | 1,271 | 0.89× | Two-stage | Win |
| Canada Small (prettified) | 1,851 | 1,703 | 1,229 | 1.09× | Fused | Win |
| CitmCatalog Small (minified) | 1,567 | 1,297 | 1,027 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 2,117 | 1,943 | 1,386 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,113 | 1,415 | 1,041 | 1.49× | Fused | Win |
| Discord Small (prettified) | 1,873 | 1,717 | 1,284 | 1.09× | Fused | Win |
| Github Events Small (minified) | 1,752 | 1,383 | 1,261 | 1.27× | Fused | Win |
| Google Maps Response Small (minified) | 2,149 | 1,402 | 744 | 1.53× | Fused | Win |
| Github Events Small (prettified) | 1,209 | 1,868 | 1,731 | 0.65× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,490 | 2,392 | 1,571 | 1.04× | Fused | Win |
| Instruments Small (minified) | 2,785 | 1,706 | 1,212 | 1.63× | Fused | Win |
| Instruments Small (prettified) | 2,776 | 2,436 | 1,899 | 1.14× | Fused | Win |
| Marine IK Reverse Small (minified) | 764 | 617 | 424 | 1.24× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,700 | 2,098 | 1,343 | 0.81× | Two-stage | Win |
| Marine IK Small (minified) | 836 | 618 | 410 | 1.35× | Fused | Win |
| Marine IK Small (prettified) | 2,014 | 2,310 | 1,471 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,657 | 1,476 | 891 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,809 | 1,759 | 1,316 | 1.03× | Fused | Win |
| Twitter Small (minified) | 2,751 | 2,165 | 1,059 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,232 | 1,908 | 1,370 | 1.17× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,299 | 328 | 101 | 3.96× | Fused | Win |
| Double (POD) | 1,049 | 364 | 145 | 2.88× | Fused | Win |
| Int64 (POD) | 2,654 | 912 | 351 | 2.91× | Fused | Win |
| String (POD) | 2,293 | 796 | 709 | 2.88× | Fused | Win |
| Uint64 (POD) | 2,343 | 815 | 298 | 2.88× | Fused | Win |
| Apache Builds (minified) | 2,189 | 1,341 | 1,051 | 1.63× | Fused | Win |
| Canada (minified) | 1,064 | 845 | 579 | 1.26× | Fused | Win |
| Apache Builds (prettified) | 1,892 | 1,882 | 1,340 | 1.01× | Tie | Win |
| Canada (prettified) | 1,885 | 1,907 | 1,071 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,309 | 1,785 | 1,190 | 1.29× | Fused | Win |
| CitmCatalog (prettified) | 3,438 | 3,379 | 2,385 | 1.02× | Fused | Win |
| Discord (minified) | 3,196 | 1,717 | 1,619 | 1.86× | Fused | Win |
| Discord (prettified) | 2,844 | 2,299 | 2,298 | 1.24× | Fused | Win |
| Github Events (minified) | 2,575 | 2,034 | 2,048 | 1.27× | Fused | Win |
| Google Maps Response (minified) | 2,511 | 1,474 | 1,097 | 1.70× | Fused | Win |
| Github Events (prettified) | 2,361 | 2,145 | 2,213 | 1.10× | Fused | Win |
| Google Maps Response (prettified) | 3,164 | 2,691 | 2,148 | 1.18× | Fused | Win |
| Instruments (minified) | 3,341 | 2,001 | 1,406 | 1.67× | Fused | Win |
| Instruments (prettified) | 3,171 | 2,301 | 2,120 | 1.38× | Fused | Win |
| Marine IK Reverse (minified) | 783 | 690 | 473 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 2,125 | 2,083 | 1,404 | 1.02× | Tie | Win |
| Marine IK (minified) | 754 | 709 | 483 | 1.06× | Fused | Win |
| Marine IK (prettified) | 2,326 | 2,315 | 1,685 | 1.00× | Tie | Win |
| Random (minified) | 1,635 | 1,282 | 1,022 | 1.28× | Fused | Win |
| Random (prettified) | 1,818 | 1,524 | 1,362 | 1.19× | Fused | Win |
| Twitter (minified) | 2,340 | 1,804 | 1,492 | 1.30× | Fused | Win |
| Twitter (prettified) | 2,198 | 2,105 | 1,894 | 1.04× | Tie | Win |
| Apache Builds Small (minified) | 1,792 | 1,331 | 1,009 | 1.35× | Fused | Win |
| Canada Small (minified) | 752 | 799 | 391 | 0.94× | Two-stage | Win |
| Apache Builds Small (prettified) | 1,527 | 1,989 | 1,341 | 0.77× | Two-stage | Win |
| Canada Small (prettified) | 1,457 | 1,405 | 1,008 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 1,555 | 1,080 | 892 | 1.44× | Fused | Win |
| CitmCatalog Small (prettified) | 2,190 | 1,797 | 1,342 | 1.22× | Fused | Win |
| Discord Small (minified) | 2,482 | 1,508 | 1,226 | 1.65× | Fused | Win |
| Discord Small (prettified) | 2,204 | 1,794 | 1,642 | 1.23× | Fused | Win |
| Github Events Small (minified) | 1,884 (n/c) | 1,494 (n/c) | 1,392 | 1.26× | Fused | Win |
| Google Maps Response Small (minified) | 1,817 | 1,132 | 830 | 1.60× | Fused | Win |
| Github Events Small (prettified) | 1,825 | 1,613 | 1,782 | 1.13× | Fused | Tie |
| Google Maps Response Small (prettified) | 2,399 | 2,020 | 1,763 | 1.19× | Fused | Win |
| Instruments Small (minified) | 2,686 | 1,471 | 1,012 | 1.83× | Fused | Win |
| Instruments Small (prettified) | 2,338 | 2,110 | 1,790 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 632 | 656 | 338 | 0.96× | Two-stage | Win |
| Marine IK Reverse Small (prettified) | 1,874 | 2,002 | 1,330 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 657 | 587 | 324 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,857 | 1,900 | 1,135 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,521 | 1,011 | 872 | 1.50× | Fused | Win |
| Random Small (prettified) | 1,676 | 1,589 | 1,398 | 1.05× | Tie | Win |
| Twitter Small (minified) | 2,045 | 2,018 (n/c) | 1,662 | 1.01× | Tie | Win |
| Twitter Small (prettified) | 1,994 | 2,515 | 1,783 | 0.79× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86, with the Small prettified copies on the M2 Pro the one mixed case. On the three x86 builds the fused path wins all 15 POD-type tests, 32 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 31 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC follows the same split: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 18, the fused path 3 (Apache Builds, Apache Builds Small and Github Events Small) and 1 is a tie (Marine IK Reverse). On the M2 Pro the fused path wins all POD input, 41 of the 44 minified documents (1 tie, 2 to the tape) and 25 of the 44 prettified documents (9 to the tape, 10 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.28× (String, Windows/MSVC) to 4.05× (Bool, Linux/GCC) faster than the two-stage path, and 1.43× (Double, Windows/MSVC) to 4.30× (Bool, Linux/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** The fused path wins 106 of the 110 minified documents: all 22 under MSVC and on Linux/Clang, 21 of 22 on Linux/GCC, where the two-stage path wins minified Canada by 5% (1,033 against 980 MB/s), 21 of 22 on macOS/Clang, where the tape wins Apache Builds Small by 4% (1,293 against 1,241), and 20 of 22 on macOS/GCC, where the tape wins minified Marine IK by 3% (612 against 597) and minified Random is a tie (916 against 919). The fused path's margins run from under 1% (Marine IK on Linux/GCC, 740 against 733 MB/s) to 1.93× (Discord on macOS/Clang, 2,562 against 1,325 MB/s) on the full-size documents and up to 1.76× on the Small ones (Instruments Small on macOS/Clang, 2,382 against 1,352 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 62 of 66.** It wins all 22 on Linux/Clang and on Linux/GCC, and 18 of 22 under MSVC, where the fused path wins prettified Apache Builds, Apache Builds Small and Github Events Small, by 3%, 11% and 8%, and prettified Marine IK Reverse is a tie (1,819 MB/s fused against 1,861). The tape's margins run from 3% (Marine IK under MSVC, 1,903 against 1,844 MB/s) to 42% (Discord on Linux/Clang, 2,602 against 1,833) on the full-size documents and from 5% (Canada Small under MSVC) to 40% (Twitter Small on Linux/GCC) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 63 of the 66 prettified x86 documents: the fused path wins the same three under MSVC, and there are no ties.

**Prettified documents on the M2 Pro: the full-size copies are the fused path's.** Of the 44 prettified documents, the fused path wins 25, the tape wins 9 and 10 are ties, and the size of the document matters more than the compiler. On the 22 full-size documents the fused path wins 17, 4 are ties (Marine IK and Twitter on both compilers) and the tape takes only prettified Github Events on macOS/GCC, its widest M2 Pro margin at 1,637 against 1,149 MB/s (1.42×). The 22 Small copies split 8 / 6 / 8. On macOS/Clang the fused path wins 5 of 11, the tape takes Instruments Small, Marine IK Small and Marine IK Reverse Small, and Canada Small, Github Events Small and Twitter Small are ties; on macOS/GCC the fused path wins 3, the tape takes the Small copies of Apache Builds, Github Events, Marine IK, Marine IK Reverse and Twitter, and the Small copies of Canada, Discord and Random are ties. Reused, the fused path wins 27, the tape 11, and 6 are ties: 12, 5 and 5 on macOS/Clang, and 15, 6 and 1 on macOS/GCC. NEON escapes much of the prettified penalty seen on x86, most of all on the full-size documents, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified Github Events and Twitter are where Jsonifier is weakest.** All six of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are prettified copies of those two documents: on Linux/Clang Github Events by 10% (1,886 against 2,106 MB/s), Twitter by 1% (2,181 against 2,212) and Twitter Small by 6% (2,658 against 2,817), Github Events Small on Linux/GCC by 4% (2,257 against 2,355), and on macOS/Clang Github Events Small by 7% (1,348 against 1,452) and Twitter Small by 3% (2,013 against 2,068). The one tie is prettified Github Events Small on Linux/Clang (2,317 against 2,308). With objects reused the faster path loses two, Github Events Small on Linux/Clang by 2% (2,788 against 2,833) and on Linux/GCC by 7% (2,661 against 2,871), and ties prettified Random on Linux/Clang (2,222 against 2,216) and Github Events Small on macOS/Clang (1,825 against 1,782). We have not profiled what these documents have in common.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 64% of its Linux/Clang throughput, the two-stage path 63% and simdjson 49%. Because the two Jsonifier paths lose a similar share, MSVC routes input much as Linux/Clang does. The spread between documents is about the same for the two Jsonifier paths (the fused path from 52% on Discord to 78% on Instruments, the two-stage path from 51% on Apache Builds to 72% on Instruments) and widest for simdjson, which keeps 37% of its Linux/Clang throughput on Github Events and 63% on Canada. We have not profiled why.

**Against simdjson, Jsonifier wins 238 of 245 tests with whichever path is faster, ties one and loses six** (all prettified documents). The fused path alone wins 223, ties none and loses 22, all prettified, its sixteen extra losses being eight on Linux/Clang, five on Linux/GCC, two on macOS/GCC and one under MSVC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 189 MB/s, against 344 MB/s for the same document in declared order, so the faster Jsonifier path is 3.08× faster than simdjson freshly allocated (3.25× reused). The prettified form shows the same gap, at 619 MB/s against 1,210, and the faster Jsonifier path is 3.01× faster (3.13× reused). The Small copies show it too, at 142 MB/s minified and 536 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one on Linux and up to about a fifth slower on macOS, and the faster Jsonifier path is 1.17× to 1.92× faster than simdjson on the full-size reversed document, freshly allocated, and 1.13× to 1.66× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input and full-size prettified input belong to the fused path, while the Small prettified copies split about evenly. Routing each test to its faster path would raise the record against simdjson from 223 wins, 0 ties and 22 losses to 238 wins, 1 tie and 6 losses. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,510 | 1,348 | 1.12× | Win |
| Canada (minified) | 1,033 | 864 | 1.20× | Win |
| Apache Builds (prettified) | 1,909 | 1,752 | 1.09× | Win |
| Canada (prettified) | 2,239 | 1,918 | 1.17× | Win |
| CitmCatalog (minified) | 1,832 | 1,389 | 1.32× | Win |
| CitmCatalog (prettified) | 3,656 | 2,921 | 1.25× | Win |
| Discord (minified) | 1,796 | 1,555 | 1.15× | Win |
| Discord (prettified) | 2,591 | 2,166 | 1.20× | Win |
| Github Events (minified) | 1,557 | 1,819 | 0.86× | **Loss** |
| Google Maps Response (minified) | 1,914 | 1,296 | 1.48× | Win |
| Github Events (prettified) | 1,834 | 1,936 | 0.95× | **Loss** |
| Google Maps Response (prettified) | 3,581 | 2,543 | 1.41× | Win |
| Instruments (minified) | 2,262 | 1,819 | 1.24× | Win |
| Instruments (prettified) | 3,395 | 2,877 | 1.18× | Win |
| Marine IK Reverse (minified) | 722 | 712 | 1.01× | Win |
| Marine IK Reverse (prettified) | 2,564 | 2,537 | 1.01× | Win |
| Marine IK (minified) | 733 | 702 | 1.04× | Win |
| Marine IK (prettified) | 2,626 | 2,514 | 1.04× | Win |
| Random (minified) | 1,092 | 1,072 | 1.02× | Win |
| Random (prettified) | 1,701 | 1,618 | 1.05× | Win |
| Twitter (minified) | 1,667 | 1,730 | 0.96× | **Loss** |
| Twitter (prettified) | 2,174 | 2,248 | 0.97× | **Loss** |
| Apache Builds Small (minified) | 2,140 | 1,727 | 1.24× | Win |
| Canada Small (minified) | 967 | 927 | 1.04× | Win |
| Apache Builds Small (prettified) | 2,629 | 2,189 | 1.20× | Win |
| Canada Small (prettified) | 2,216 | 2,138 | 1.04× | Win |
| CitmCatalog Small (minified) | 1,489 | 1,242 | 1.20× | Win |
| CitmCatalog Small (prettified) | 2,502 | 2,064 | 1.21× | Win |
| Discord Small (minified) | 1,747 | 1,369 | 1.28× | Win |
| Discord Small (prettified) | 2,220 | 1,830 | 1.21× | Win |
| Github Events Small (minified) | 1,833 | 1,818 | 1.01× | Win |
| Google Maps Response Small (minified) | 1,785 | 1,285 | 1.39× | Win |
| Github Events Small (prettified) | 2,257 | 2,302 | 0.98× | **Loss** |
| Google Maps Response Small (prettified) | 3,307 | 2,558 | 1.29× | Win |
| Instruments Small (minified) | 2,272 | 1,740 | 1.31× | Win |
| Instruments Small (prettified) | 3,346 | 2,763 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 723 | 747 | 0.97× | **Loss** |
| Marine IK Reverse Small (prettified) | 2,587 | 2,667 | 0.97× | **Loss** |
| Marine IK Small (minified) | 780 | 749 | 1.04× | Win |
| Marine IK Small (prettified) | 2,754 | 2,675 | 1.03× | Win |
| Random Small (minified) | 1,573 | 1,251 | 1.26× | Win |
| Random Small (prettified) | 2,357 | 1,917 | 1.23× | Win |
| Twitter Small (minified) | 2,324 | 2,126 | 1.09× | Win |
| Twitter Small (prettified) | 2,937 | 2,710 | 1.08× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 938 | 736 | 1.27× | Win |
| Canada (minified) | 729 | 609 (n/c) | 1.20× | Win |
| Apache Builds (prettified) | 1,024 | 1,042 | 0.98× | Tie |
| Canada (prettified) | 1,360 | 1,286 | 1.06× | Win |
| CitmCatalog (minified) | 1,210 | 917 | 1.32× | Win |
| CitmCatalog (prettified) | 2,168 | 1,985 | 1.09× | Win |
| Discord (minified) | 1,229 | 910 | 1.35× | Win |
| Discord (prettified) | 1,604 | 1,239 | 1.30× | Win |
| Github Events (minified) | 888 | 1,178 | 0.75× | **Loss** |
| Google Maps Response (minified) | 1,218 | 1,021 | 1.19× | Win |
| Github Events (prettified) | 1,637 | 1,528 | 1.07× | Win |
| Google Maps Response (prettified) | 2,413 | 1,892 | 1.28× | Win |
| Instruments (minified) | 1,576 | 1,317 | 1.20× | Win |
| Instruments (prettified) | 1,921 | 1,616 | 1.19× | Win |
| Marine IK Reverse (minified) | 475 | 484 | 0.98× | **Loss** |
| Marine IK Reverse (prettified) | 1,995 | 1,768 | 1.13× | Win |
| Marine IK (minified) | 612 | 482 (n/c) | 1.27× | Win |
| Marine IK (prettified) | 2,029 | 1,489 | 1.36× | Win |
| Random (minified) | 919 | 651 | 1.41× | Win |
| Random (prettified) | 884 | 870 | 1.02× | Tie |
| Twitter (minified) | 1,711 | 1,226 | 1.40× | Win |
| Twitter (prettified) | 2,047 | 1,747 | 1.17× | Win |
| Apache Builds Small (minified) | 1,259 (n/c) | 1,024 | 1.23× | Win |
| Canada Small (minified) | 709 | 559 | 1.27× | Win |
| Apache Builds Small (prettified) | 1,556 | 1,238 | 1.26× | Win |
| Canada Small (prettified) | 1,532 | 1,331 | 1.15× | Win |
| CitmCatalog Small (minified) | 968 | 825 | 1.17× | Win |
| CitmCatalog Small (prettified) | 1,573 | 1,141 | 1.38× | Win |
| Discord Small (minified) | 1,085 | 999 | 1.09× | Win |
| Discord Small (prettified) | 1,449 | 1,250 | 1.16× | Win |
| Github Events Small (minified) | 1,131 | 1,003 | 1.13× | Win |
| Google Maps Response Small (minified) | 1,100 | 834 | 1.32× | Win |
| Github Events Small (prettified) | 1,355 | 1,240 | 1.09× | Win |
| Google Maps Response Small (prettified) | 2,077 | 1,741 | 1.19× | Win |
| Instruments Small (minified) | 1,673 | 1,130 | 1.48× | Win |
| Instruments Small (prettified) | 2,139 | 1,804 | 1.19× | Win |
| Marine IK Reverse Small (minified) | 501 | 483 | 1.04× | Win |
| Marine IK Reverse Small (prettified) | 1,539 | 1,452 | 1.06× | Win |
| Marine IK Small (minified) | 568 | 471 | 1.21× | Win |
| Marine IK Small (prettified) | 1,881 | 1,640 | 1.15× | Win |
| Random Small (minified) | 1,142 | 663 | 1.72× | Win |
| Random Small (prettified) | 1,281 | 1,197 | 1.07× | Win |
| Twitter Small (minified) | 1,667 | 1,232 | 1.35× | Win |
| Twitter Small (prettified) | 1,986 | 1,430 | 1.39× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,238 | 1,746 | 1.28× | Win |
| Canada (minified) | 1,255 | 1,021 | 1.23× | Win |
| Apache Builds (prettified) | 2,742 | 2,229 | 1.23× | Win |
| Canada (prettified) | 2,714 | 2,247 | 1.21× | Win |
| CitmCatalog (minified) | 2,203 | 1,638 | 1.35× | Win |
| CitmCatalog (prettified) | 4,294 | 3,368 | 1.27× | Win |
| Discord (minified) | 2,455 | 1,768 | 1.39× | Win |
| Discord (prettified) | 3,366 | 2,490 | 1.35× | Win |
| Github Events (minified) | 2,022 | 1,901 | 1.06× | Win |
| Google Maps Response (minified) | 2,113 | 1,369 | 1.54× | Win |
| Github Events (prettified) | 2,600 | 2,273 | 1.14× | Win |
| Google Maps Response (prettified) | 3,875 | 2,701 | 1.43× | Win |
| Instruments (minified) | 2,580 | 1,861 | 1.39× | Win |
| Instruments (prettified) | 3,791 | 2,959 | 1.28× | Win |
| Marine IK Reverse (minified) | 799 | 797 | 1.00× | Win |
| Marine IK Reverse (prettified) | 2,774 | 2,814 | 0.99× | **Loss** |
| Marine IK (minified) | 811 | 788 | 1.03× | Win |
| Marine IK (prettified) | 2,883 | 2,811 | 1.03× | Win |
| Random (minified) | 1,468 | 1,391 | 1.06× | Win |
| Random (prettified) | 2,254 | 2,086 | 1.08× | Win |
| Twitter (minified) | 2,113 | 2,023 | 1.04× | Win |
| Twitter (prettified) | 2,732 | 2,657 | 1.03× | Win |
| Apache Builds Small (minified) | 2,485 | 1,820 | 1.37× | Win |
| Canada Small (minified) | 1,215 | 1,111 | 1.09× | Win |
| Apache Builds Small (prettified) | 3,044 | 2,303 | 1.32× | Win |
| Canada Small (prettified) | 2,769 | 2,500 | 1.11× | Win |
| CitmCatalog Small (minified) | 1,749 | 1,346 | 1.30× | Win |
| CitmCatalog Small (prettified) | 2,936 | 2,301 | 1.28× | Win |
| Discord Small (minified) | 2,170 | 1,585 | 1.37× | Win |
| Discord Small (prettified) | 2,750 | 2,112 | 1.30× | Win |
| Github Events Small (minified) | 2,268 | 2,059 | 1.10× | Win |
| Google Maps Response Small (minified) | 2,035 | 1,409 | 1.44× | Win |
| Github Events Small (prettified) | 2,661 | 2,588 | 1.03× | Win |
| Google Maps Response Small (prettified) | 3,678 | 2,779 | 1.32× | Win |
| Instruments Small (minified) | 2,426 | 1,804 | 1.34× | Win |
| Instruments Small (prettified) | 3,536 | 2,865 | 1.23× | Win |
| Marine IK Reverse Small (minified) | 878 | 845 | 1.04× | Win |
| Marine IK Reverse Small (prettified) | 3,074 | 3,011 | 1.02× | Win |
| Marine IK Small (minified) | 941 | 856 | 1.10× | Win |
| Marine IK Small (prettified) | 3,228 | 3,045 | 1.06× | Win |
| Random Small (minified) | 1,925 | 1,433 | 1.34× | Win |
| Random Small (prettified) | 2,848 | 2,157 | 1.32× | Win |
| Twitter Small (minified) | 2,588 | 2,341 | 1.11× | Win |
| Twitter Small (prettified) | 3,256 | 3,007 | 1.08× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,297 | 957 | 1.36× | Win |
| Canada (minified) | 978 | 718 | 1.36× | Win |
| Apache Builds (prettified) | 1,575 | 1,263 | 1.25× | Win |
| Canada (prettified) | 1,642 | 1,489 | 1.10× | Win |
| CitmCatalog (minified) | 1,363 | 1,098 | 1.24× | Win |
| CitmCatalog (prettified) | 2,443 | 2,159 | 1.13× | Win |
| Discord (minified) | 1,635 | 1,070 | 1.53× | Win |
| Discord (prettified) | 1,964 | 1,468 | 1.34× | Win |
| Github Events (minified) | 1,210 | 1,413 | 0.86× | **Loss** |
| Google Maps Response (minified) | 1,382 | 1,104 | 1.25× | Win |
| Github Events (prettified) | 2,117 | 1,682 | 1.26× | Win |
| Google Maps Response (prettified) | 2,723 | 1,984 | 1.37× | Win |
| Instruments (minified) | 1,790 | 1,354 | 1.32× | Win |
| Instruments (prettified) | 2,188 | 2,050 | 1.07× | Win |
| Marine IK Reverse (minified) | 543 | 538 | 1.01× | Tie |
| Marine IK Reverse (prettified) | 2,204 | 1,936 | 1.14× | Win |
| Marine IK (minified) | 692 | 487 (n/c) | 1.42× | Win |
| Marine IK (prettified) | 2,295 | 1,728 | 1.33× | Win |
| Random (minified) | 1,222 | 798 | 1.53× | Win |
| Random (prettified) | 1,688 | 1,195 | 1.41× | Win |
| Twitter (minified) | 2,209 | 1,521 | 1.45× | Win |
| Twitter (prettified) | 2,596 | 1,923 | 1.35× | Win |
| Apache Builds Small (minified) | 1,741 | 1,053 | 1.65× | Win |
| Canada Small (minified) | 792 | 611 | 1.30× | Win |
| Apache Builds Small (prettified) | 1,803 | 1,359 | 1.33× | Win |
| Canada Small (prettified) | 1,703 | 1,542 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,297 | 1,017 | 1.28× | Win |
| CitmCatalog Small (prettified) | 1,943 | 1,655 | 1.17× | Win |
| Discord Small (minified) | 1,415 | 1,001 | 1.41× | Win |
| Discord Small (prettified) | 1,717 | 1,488 | 1.15× | Win |
| Github Events Small (minified) | 1,383 | 1,359 | 1.02× | Tie |
| Google Maps Response Small (minified) | 1,402 | 955 | 1.47× | Win |
| Github Events Small (prettified) | 1,868 | 1,373 | 1.36× | Win |
| Google Maps Response Small (prettified) | 2,392 | 1,706 | 1.40× | Win |
| Instruments Small (minified) | 1,706 | 1,222 | 1.40× | Win |
| Instruments Small (prettified) | 2,436 | 1,894 | 1.29× | Win |
| Marine IK Reverse Small (minified) | 617 | 571 | 1.08× | Win |
| Marine IK Reverse Small (prettified) | 2,098 | 1,829 | 1.15× | Win |
| Marine IK Small (minified) | 618 | 610 | 1.01× | Tie |
| Marine IK Small (prettified) | 2,310 | 1,957 | 1.18× | Win |
| Random Small (minified) | 1,476 | 984 | 1.50× | Win |
| Random Small (prettified) | 1,759 | 1,376 | 1.28× | Win |
| Twitter Small (minified) | 2,165 | 1,449 | 1.49× | Win |
| Twitter Small (prettified) | 1,908 | 1,985 | 0.96× | Tie |

In the freshly allocated run, two-stage Jsonifier wins 77 of these 88 tests, ties 2 and loses 9, by 2% to 25%. Seven losses are on Linux/GCC: minified and prettified Github Events and Twitter (the widest of these at 0.86×, on minified Github Events), prettified Github Events Small, and Marine IK Reverse Small in both forms. The other two are on macOS/GCC: minified Github Events, the widest overall at 0.75× (888 against 1,178 MB/s), and minified Marine IK Reverse, at 0.98×. The ties are prettified Apache Builds and Random on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 72, ties 1 and loses 15. With objects reused, two-stage Jsonifier wins 82, ties 4 and loses 2: prettified Marine IK Reverse on Linux/GCC by 1% (2,774 against 2,814 MB/s) and minified Github Events on macOS/GCC by 14% (1,210 against 1,413). The ties are all on macOS/GCC: minified Marine IK Reverse, Github Events Small and Marine IK Small, and prettified Twitter Small. The fused path beats the reflection reader on 75, ties 1 and loses 12. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 29 of the 40 results; all 11 wider gaps are on the macOS builds, and the widest is freshly allocated minified Twitter Small on macOS/GCC, where the "jsonifier" row runs at 2,610 MB/s against the two-stage row's 3,662. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,815 | 3,588 | 1.90× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,759 | 4,174 | 1.86× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,174 | 3,142 | 1.33× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,044 | 3,785 | 1.33× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,035 | 5,547 | 1.45× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,150 | 6,799 | 1.35× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,522 | 5,257 | 1.05× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,674 | 6,337 | 1.05× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,081 | 7,300 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,988 | 8,424 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,793 | 5,873 | 0.99× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,105 | 7,102 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,397 | 4,233 | 1.28× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,631 | 4,834 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,662 | 3,171 | 1.15× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,007 | 4,327 | 0.93× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 3,868 | 3,515 | 1.10× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,148 | 3,886 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 2,620 | 2,690 | 0.97× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,426 | 3,320 (n/c) | 1.03× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 7,048 | 3,679 | 1.92× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,986 | 4,290 | 1.86× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,554 | 3,331 | 1.37× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,355 | 3,934 | 1.36× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,127 | 5,632 | 1.44× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,349 | 6,882 | 1.36× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,796 | 5,350 | 1.08× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,839 | 6,505 | 1.05× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,185 | 7,369 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,126 | 8,469 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,188 | 6,002 | 1.03× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,416 | 7,347 | 1.01× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,510 | 4,341 | 1.27× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,864 | 4,906 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,746 (n/c) | 3,390 | 1.11× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,421 | 4,503 | 0.98× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 3,876 | 3,646 | 1.06× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,552 | 4,044 | 1.13× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,097 | 3,079 | 1.01× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,562 | 3,942 | 0.90× | Loss |

Jsonifier wins 32 of the 40 results, ties 3 and loses 5, and wins by up to 1.92× (minified Twitter under MSVC, reused). Freshly allocated it wins 16, ties 1 and loses 3: the losses are minified Twitter Small on Linux/GCC (0.99×) and macOS/Clang (0.97×) and prettified Twitter Small on macOS/GCC (0.93×); the tie is prettified Twitter Small on Linux/GCC. The reused run has 16 wins, 2 ties and 2 losses: prettified Twitter on macOS/GCC (0.99×) and prettified Twitter Small on macOS/Clang (0.90×); the ties are prettified Twitter Small on macOS/GCC and minified Twitter Small on macOS/Clang. On the three x86 builds every full-size result is a win, by 1.07× to 1.92×; on the M2 Pro the full-size document wins 7 of its 8 results, losing only reused prettified Twitter on macOS/GCC. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.05× to 1.34×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,711 | 2,593 | 1.05× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,569 | 2,656 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 3,030 | 2,530 | 1.20× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,108 | 1,650 | 1.28× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 1,852 | 1,558 | 1.19× | Win |

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 106 of 110 minified documents and on 17 of 22 full-size prettified documents on the M2 Pro; slower on 62 of 66 prettified x86 documents, and split on the M2 Pro's Small prettified copies (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 159 of 245 freshly allocated tests: all 25 POD-type tests, 106 of 110 minified documents and 17 of 22 full-size prettified documents on the M2 Pro, while the two-stage path wins 62 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 238 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 77 of 88, and on partial reading it wins 32 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*