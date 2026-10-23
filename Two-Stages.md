# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on POD-type data on every build and on 101 of the 110 minified documents, the tape is faster on 56 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 34 of the 44 prettified documents.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 28-29, 2026, with Jsonifier [9f4382a](https://github.com/nihilai-collective/jsonifier/commit/9f4382a) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build, Glaze [f67610b](https://github.com/stephenberry/glaze/commit/f67610b) ([8ae6d0d](https://github.com/stephenberry/glaze/commit/8ae6d0d) on the two macOS builds), and simdjson [41ad75c](https://github.com/simdjson/simdjson/commit/41ad75c) on Linux, [9262482](https://github.com/simdjson/simdjson/commit/9262482) under MSVC and [ab504c9](https://github.com/simdjson/simdjson/commit/ab504c9) on macOS. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Seventeen freshly allocated full-parse rows did not converge (fifteen under MSVC, two on macOS/GCC), and fifteen reused ones (thirteen under MSVC, one on macOS/GCC, one on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 163 tests, 11 are statistical ties, and the two-stage path wins 71. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 26 | 8 | 15 | 34 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 0 | 25 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 40 | 3 | 6 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 | 0 | 2 | 49 of 49 |
| **Aggregate** | **163** | **11** | **71** | **228 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 8 / 1 / 2 | 17 / 0 / 5 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 7 / 4 | 1 / 0 / 21 | 17 / 2 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 1 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 17 / 0 / 5 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 1 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 45 / 1 / 3 | 37 / 0 / 12 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 46 / 1 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 48 / 1 / 0 |
| **Aggregate** | **240 / 2 / 3** | **228 / 3 / 14** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 27 | 5 | 17 | 36 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 | 0 | 26 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 41 | 4 | 4 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 | 0 | 2 | 48 of 49 |
| **Aggregate** | **165** | **10** | **70** | **230 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 9 / 1 / 1 | 17 / 0 / 5 | 21 / 1 / 0 |
| Prettified corpus documents | 0 / 4 / 7 | 1 / 1 / 20 | 20 / 0 / 2 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 15 / 3 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 / 0 / 2 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 47 / 0 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **242 / 0 / 3** | **232 / 0 / 13** |

Across 245 tests the fused path wins 165, ties 10 and loses 70 to the two-stage path, and the x86 split holds: the tape wins 27 of the 33 full-size prettified documents (5 ties; the fused path takes Google Maps Response on Linux/GCC) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 26 of the 33 full-size minified ones (the tape takes Github Events under MSVC and Canada, Github Events, Marine IK, Marine IK Reverse and Random on Linux/GCC, and minified Canada is a tie under MSVC). On the M2 Pro the fused path wins 43 of the 44 minified documents (minified Random is a tie on macOS/GCC) and 35 of the 44 prettified ones, with 3 ties and 6 tape wins. Against simdjson, the faster Jsonifier path wins 242 and loses 3, and the fused path alone wins 232 and loses 13.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 522 | 271 | 93 | 1.93× | Fused | Win |
| Double (POD) | 695 | 533 | 145 (n/c) | 1.31× | Fused | Win |
| Int64 (POD) | 1,705 | 890 | 322 | 1.92× | Fused | Win |
| String (POD) | 988 | 774 | 561 | 1.28× | Fused | Win |
| Uint64 (POD) | 1,558 | 896 | 316 | 1.74× | Fused | Win |
| Apache Builds (minified) | 443 (n/c) | 389 (n/c) | 274 (n/c) | 1.14× | Tie | Win |
| Canada (minified) | 338 | 317 (n/c) | 213 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 528 (n/c) | 495 (n/c) | 351 (n/c) | 1.07× | Tie | Win |
| Canada (prettified) | 670 | 730 | 465 | 0.92× | Tie | Win |
| CitmCatalog (minified) | 886 (n/c) | 644 (n/c) | 239 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 1,198 (n/c) | 1,323 (n/c) | 509 (n/c) | 0.91× | Tie | Win |
| Discord (minified) | 616 (n/c) | 501 (n/c) | 255 (n/c) | 1.23× | Fused | Win |
| Discord (prettified) | 623 (n/c) | 735 (n/c) | 374 (n/c) | 0.85× | Tie | Win |
| Github Events (minified) | 493 (n/c) | 880 | 515 | 0.56× | Two-stage | Win |
| Google Maps Response (minified) | 1,660 | 1,171 | 215 (n/c) | 1.42× | Fused | Win |
| Github Events (prettified) | 516 (n/c) | 515 (n/c) | 304 (n/c) | 1.00× | Tie | Win |
| Google Maps Response (prettified) | 2,221 | 2,416 | 428 (n/c) | 0.92× | Two-stage | Win |
| Instruments (minified) | 705 (n/c) | 1,421 (n/c) | 326 (n/c) | 0.50× | Two-stage | Win |
| Instruments (prettified) | 969 | 1,094 (n/c) | 455 | 0.89× | Tie | Win |
| Marine IK Reverse (minified) | 281 | 250 | 90 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 868 (n/c) | 901 | 281 (n/c) | 0.96× | Tie | Win |
| Marine IK (minified) | 297 | 261 | 159 | 1.14× | Fused | Win |
| Marine IK (prettified) | 1,871 | 1,924 | 1,165 | 0.97× | Two-stage | Win |
| Random (minified) | 791 | 712 | 440 | 1.11× | Fused | Win |
| Random (prettified) | 1,027 | 1,126 | 679 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,225 | 1,075 | 735 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,249 | 1,400 | 950 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 1,323 | 997 | 649 | 1.33× | Fused | Win |
| Canada Small (minified) | 653 | 612 | 350 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,362 | 1,195 | 828 | 1.14× | Fused | Win |
| Canada Small (prettified) | 1,329 | 1,434 | 815 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 881 | 755 | 460 | 1.17× | Fused | Win |
| CitmCatalog Small (prettified) | 1,172 | 1,323 | 798 | 0.89× | Two-stage | Win |
| Discord Small (minified) | 1,070 | 869 | 547 | 1.23× | Fused | Win |
| Discord Small (prettified) | 1,071 | 1,182 | 737 | 0.91× | Two-stage | Win |
| Github Events Small (minified) | 1,211 | 844 | 525 | 1.44× | Fused | Win |
| Google Maps Response Small (minified) | 1,580 | 1,106 | 451 | 1.43× | Fused | Win |
| Github Events Small (prettified) | 1,157 | 1,072 | 666 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,049 | 2,246 | 904 | 0.91× | Two-stage | Win |
| Instruments Small (minified) | 2,096 | 1,493 | 595 | 1.40× | Fused | Win |
| Instruments Small (prettified) | 1,850 | 2,254 | 969 | 0.82× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 485 | 433 | 138 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,443 | 1,603 | 522 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 517 | 455 | 279 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,520 | 1,672 | 994 | 0.91× | Two-stage | Win |
| Random Small (minified) | 927 | 817 | 430 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,161 | 1,237 | 669 | 0.94× | Two-stage | Win |
| Twitter Small (minified) | 1,500 | 1,258 | 1,023 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 1,482 | 1,643 | 1,296 | 0.90× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,902 | 619 | 288 | 3.07× | Fused | Win |
| Double (POD) | 1,309 | 758 | 335 | 1.73× | Fused | Win |
| Int64 (POD) | 3,139 | 1,598 | 788 | 1.96× | Fused | Win |
| String (POD) | 1,816 | 1,320 | 1,389 | 1.38× | Fused | Win |
| Uint64 (POD) | 3,325 | 1,586 | 776 | 2.10× | Fused | Win |
| Apache Builds (minified) | 1,750 | 1,512 | 1,205 | 1.16× | Fused | Win |
| Canada (minified) | 1,060 | 995 | 682 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,736 | 1,883 | 1,558 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,102 | 2,128 | 1,555 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,386 | 1,818 | 1,209 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 3,159 | 3,589 | 2,540 | 0.88× | Two-stage | Win |
| Discord (minified) | 2,238 | 1,890 | 1,520 | 1.18× | Fused | Win |
| Discord (prettified) | 1,907 | 2,593 | 2,183 | 0.74× | Two-stage | Win |
| Github Events (minified) | 1,660 | 1,867 | 1,833 | 0.89× | Two-stage | Win |
| Google Maps Response (minified) | 2,681 | 1,991 | 1,303 | 1.35× | Fused | Win |
| Github Events (prettified) | 1,634 | 1,912 | 1,933 | 0.85× | Two-stage | Loss |
| Google Maps Response (prettified) | 3,183 | 3,744 | 2,604 | 0.85× | Two-stage | Win |
| Instruments (minified) | 2,387 | 1,969 | 1,751 | 1.21× | Fused | Win |
| Instruments (prettified) | 2,350 | 2,957 | 2,802 | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 840 | 758 | 596 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 2,553 | 2,587 | 2,166 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 845 | 763 | 584 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,558 | 2,615 | 2,131 | 0.98× | Two-stage | Win |
| Random (minified) | 1,243 | 1,115 | 1,085 | 1.11× | Fused | Win |
| Random (prettified) | 1,446 | 1,648 | 1,633 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,915 | 1,725 | 1,693 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,741 | 2,195 | 2,190 | 0.79× | Two-stage | Tie |
| Apache Builds Small (minified) | 2,649 | 2,231 | 1,515 | 1.19× | Fused | Win |
| Canada Small (minified) | 979 | 956 | 707 | 1.02× | Fused | Win |
| Apache Builds Small (prettified) | 2,227 | 2,692 | 1,903 | 0.83× | Two-stage | Win |
| Canada Small (prettified) | 1,957 | 2,154 | 1,671 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,767 | 1,416 | 1,182 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,952 | 2,416 | 2,042 | 0.81× | Two-stage | Win |
| Discord Small (minified) | 2,001 | 1,744 | 1,502 | 1.15× | Fused | Win |
| Discord Small (prettified) | 1,687 | 2,331 | 1,982 | 0.72× | Two-stage | Win |
| Github Events Small (minified) | 2,130 | 1,833 | 1,912 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,425 | 1,804 | 1,324 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 1,990 | 2,230 | 2,337 | 0.89× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,879 | 3,429 | 2,577 | 0.84× | Two-stage | Win |
| Instruments Small (minified) | 2,449 | 1,929 | 1,566 | 1.27× | Fused | Win |
| Instruments Small (prettified) | 2,302 | 2,958 | 2,524 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 800 | 738 | 577 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,229 | 2,596 | 2,094 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 895 | 791 | 595 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,329 | 2,771 | 2,166 | 0.84× | Two-stage | Win |
| Random Small (minified) | 1,749 | 1,557 | 1,152 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,741 | 2,303 | 1,752 | 0.76× | Two-stage | Win |
| Twitter Small (minified) | 2,503 | 2,132 | 2,158 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 2,082 | 2,691 | 2,772 | 0.77× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,242 | 556 | 247 | 4.03× | Fused | Win |
| Double (POD) | 1,221 | 624 | 292 | 1.96× | Fused | Win |
| Int64 (POD) | 3,346 | 1,432 | 724 | 2.34× | Fused | Win |
| String (POD) | 1,821 | 1,227 | 1,285 | 1.48× | Fused | Win |
| Uint64 (POD) | 3,452 | 1,410 | 716 | 2.45× | Fused | Win |
| Apache Builds (minified) | 1,650 | 1,505 | 1,385 | 1.10× | Fused | Win |
| Canada (minified) | 989 | 1,013 | 858 | 0.98× | Two-stage | Win |
| Apache Builds (prettified) | 1,720 | 1,890 | 1,754 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,093 | 2,236 | 1,908 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,414 | 1,862 | 1,363 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,276 | 3,678 | 2,829 | 0.89× | Two-stage | Win |
| Discord (minified) | 2,386 | 1,821 | 1,440 | 1.31× | Fused | Win |
| Discord (prettified) | 2,174 | 2,559 | 2,048 | 0.85× | Two-stage | Win |
| Github Events (minified) | 1,613 | 1,700 | 1,350 | 0.95× | Two-stage | Win |
| Google Maps Response (minified) | 2,638 | 1,932 | 1,060 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,772 | 2,028 | 1,650 | 0.87× | Two-stage | Win |
| Google Maps Response (prettified) | 3,547 | 3,506 | 2,121 | 1.01× | Fused | Win |
| Instruments (minified) | 3,283 | 2,240 | 1,391 | 1.47× | Fused | Win |
| Instruments (prettified) | 3,002 | 3,386 | 2,256 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 734 | 739 | 613 | 0.99× | Two-stage | Win |
| Marine IK Reverse (prettified) | 2,411 | 2,579 | 2,218 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 739 | 746 | 610 | 0.99× | Two-stage | Win |
| Marine IK (prettified) | 2,421 | 2,593 | 2,187 | 0.93× | Two-stage | Win |
| Random (minified) | 1,110 | 1,104 | 1,030 | 1.01× | Fused | Win |
| Random (prettified) | 1,379 | 1,689 | 1,559 | 0.82× | Two-stage | Win |
| Twitter (minified) | 1,932 | 1,673 | 1,326 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,753 | 2,165 | 1,750 | 0.81× | Two-stage | Win |
| Apache Builds Small (minified) | 2,473 | 2,149 | 1,643 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,070 | 968 | 774 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,313 | 2,436 | 2,019 | 0.95× | Two-stage | Win |
| Canada Small (prettified) | 2,053 | 2,259 | 1,853 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,809 | 1,484 | 1,146 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,075 | 2,480 | 1,936 | 0.84× | Two-stage | Win |
| Discord Small (minified) | 2,036 | 1,624 | 1,243 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,883 | 2,098 | 1,690 | 0.90× | Two-stage | Win |
| Github Events Small (minified) | 2,072 | 1,773 | 1,558 | 1.17× | Fused | Win |
| Google Maps Response Small (minified) | 2,350 | 1,781 | 1,093 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 2,005 | 2,278 | 1,944 | 0.88× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,127 | 3,321 | 2,173 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,307 | 2,204 | 1,264 | 1.50× | Fused | Win |
| Instruments Small (prettified) | 2,904 | 3,352 | 2,044 | 0.87× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 815 | 723 | 581 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,246 | 2,574 | 2,112 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 928 | 772 | 595 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,451 | 2,687 | 2,154 | 0.91× | Two-stage | Win |
| Random Small (minified) | 1,667 | 1,565 | 1,148 | 1.07× | Fused | Win |
| Random Small (prettified) | 1,890 | 2,289 | 1,761 | 0.83× | Two-stage | Win |
| Twitter Small (minified) | 2,907 | 2,297 | 1,699 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,315 | 2,789 | 2,140 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,364 | 398 | 162 | 3.42× | Fused | Win |
| Double (POD) | 886 | 463 | 135 | 1.91× | Fused | Win |
| Int64 (POD) | 2,363 | 968 | 436 | 2.44× | Fused | Win |
| String (POD) | 1,314 | 764 | 717 | 1.72× | Fused | Win |
| Uint64 (POD) | 2,207 | 816 | 398 | 2.71× | Fused | Win |
| Apache Builds (minified) | 1,157 | 889 | 630 | 1.30× | Fused | Win |
| Canada (minified) | 788 | 695 (n/c) | 520 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,054 | 1,141 | 884 | 0.92× | Two-stage | Win |
| Canada (prettified) | 1,442 | 1,432 | 1,053 | 1.01× | Tie | Win |
| CitmCatalog (minified) | 2,033 | 1,384 | 992 | 1.47× | Fused | Win |
| CitmCatalog (prettified) | 2,460 | 2,313 | 1,931 | 1.06× | Fused | Win |
| Discord (minified) | 1,696 | 1,178 | 897 | 1.44× | Fused | Win |
| Discord (prettified) | 1,830 | 1,677 | 1,233 | 1.09× | Fused | Win |
| Github Events (minified) | 1,562 | 1,100 | 877 | 1.42× | Fused | Win |
| Google Maps Response (minified) | 1,697 | 1,090 | 684 | 1.56× | Fused | Win |
| Github Events (prettified) | 1,427 | 1,626 | 938 | 0.88× | Two-stage | Win |
| Google Maps Response (prettified) | 2,613 | 1,991 | 1,367 | 1.31× | Fused | Win |
| Instruments (minified) | 2,476 | 1,695 | 1,196 | 1.46× | Fused | Win |
| Instruments (prettified) | 2,475 | 2,083 | 1,694 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | 612 | 528 | 433 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 1,694 | 1,866 | 1,492 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 686 | 575 | 485 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,071 | 2,030 | 1,372 | 1.02× | Fused | Win |
| Random (minified) | 845 | 822 | 575 | 1.03× | Fused | Win |
| Random (prettified) | 1,185 | 1,155 | 981 | 1.03× | Fused | Win |
| Twitter (minified) | 1,899 | 1,626 | 887 (n/c) | 1.17× | Fused | Win |
| Twitter (prettified) | 1,660 | 1,663 | 1,206 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 1,393 | 1,391 | 858 | 1.00× | Tie | Win |
| Canada Small (minified) | 848 | 691 | 451 | 1.23× | Fused | Win |
| Apache Builds Small (prettified) | 1,514 | 1,602 | 991 | 0.94× | Two-stage | Win |
| Canada Small (prettified) | 1,618 | 1,370 | 900 | 1.18× | Fused | Win |
| CitmCatalog Small (minified) | 1,237 | 991 | 828 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,688 | 1,592 | 1,339 | 1.06× | Fused | Win |
| Discord Small (minified) | 1,549 | 1,233 | 903 | 1.26× | Fused | Win |
| Discord Small (prettified) | 1,649 | 1,549 | 1,180 | 1.06× | Fused | Win |
| Github Events Small (minified) | 1,417 | 1,221 | 1,083 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 1,729 | 1,163 | 748 | 1.49× | Fused | Win |
| Github Events Small (prettified) | 1,592 | 1,370 | 1,394 | 1.16× | Fused | Win |
| Google Maps Response Small (prettified) | 2,332 | 2,076 | 1,443 | 1.12× | Fused | Win |
| Instruments Small (minified) | 2,539 | 1,550 | 1,110 | 1.64× | Fused | Win |
| Instruments Small (prettified) | 2,553 | 2,301 | 1,709 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 593 | 495 | 370 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,456 | 1,666 | 1,170 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 690 | 497 | 355 | 1.39× | Fused | Win |
| Marine IK Small (prettified) | 1,804 | 1,693 | 1,099 | 1.07× | Fused | Win |
| Random Small (minified) | 1,099 | 897 | 621 | 1.23× | Fused | Win |
| Random Small (prettified) | 1,333 | 778 | 871 | 1.71× | Fused | Win |
| Twitter Small (minified) | 1,686 | 1,409 | 735 | 1.20× | Fused | Win |
| Twitter Small (prettified) | 1,650 | 1,863 | 1,169 | 0.89× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,308 | 344 | 193 | 3.80× | Fused | Win |
| Double (POD) | 947 | 354 | 206 | 2.67× | Fused | Win |
| Int64 (POD) | 2,293 | 881 | 496 | 2.60× | Fused | Win |
| String (POD) | 1,384 | 734 | 878 | 1.89× | Fused | Win |
| Uint64 (POD) | 2,363 | 851 | 488 | 2.78× | Fused | Win |
| Apache Builds (minified) | 1,605 | 1,126 | 910 | 1.43× | Fused | Win |
| Canada (minified) | 847 | 746 | 547 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,647 | 1,408 | 1,166 | 1.17× | Fused | Win |
| Canada (prettified) | 1,684 | 1,604 | 1,215 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,343 | 1,533 | 1,082 | 1.53× | Fused | Win |
| CitmCatalog (prettified) | 3,273 | 2,939 | 2,194 | 1.11× | Fused | Win |
| Discord (minified) | 2,580 | 1,323 | 1,375 | 1.95× | Fused | Win |
| Discord (prettified) | 2,478 | 1,805 | 1,979 | 1.37× | Fused | Win |
| Github Events (minified) | 1,897 | 1,538 | 1,591 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,426 | 1,283 | 1,037 | 1.89× | Fused | Win |
| Github Events (prettified) | 1,873 | 1,703 | 1,877 | 1.10× | Fused | Tie |
| Google Maps Response (prettified) | 3,141 | 2,390 | 2,037 | 1.31× | Fused | Win |
| Instruments (minified) | 2,812 | 1,771 | 1,298 | 1.59× | Fused | Win |
| Instruments (prettified) | 2,832 | 2,574 | 2,077 | 1.10× | Fused | Win |
| Marine IK Reverse (minified) | 720 | 616 | 446 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,106 | 2,077 | 1,553 | 1.01× | Fused | Win |
| Marine IK (minified) | 723 | 621 | 446 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,183 | 2,083 | 1,548 | 1.05× | Fused | Win |
| Random (minified) | 1,313 | 1,006 | 902 | 1.30× | Fused | Win |
| Random (prettified) | 1,659 | 1,259 | 1,332 | 1.32× | Fused | Win |
| Twitter (minified) | 2,364 | 1,788 | 1,700 | 1.32× | Fused | Win |
| Twitter (prettified) | 2,316 | 2,217 | 2,165 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,684 | 1,379 | 1,040 | 1.22× | Fused | Win |
| Canada Small (minified) | 784 | 715 | 470 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 1,699 | 1,650 | 1,293 | 1.03× | Fused | Win |
| Canada Small (prettified) | 1,590 | 1,557 | 1,093 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,290 | 990 | 855 | 1.30× | Fused | Win |
| CitmCatalog Small (prettified) | 1,745 | 1,626 | 1,464 | 1.07× | Fused | Win |
| Discord Small (minified) | 2,223 | 1,308 | 1,306 | 1.70× | Fused | Win |
| Discord Small (prettified) | 2,210 | 1,668 | 1,735 | 1.32× | Fused | Win |
| Github Events Small (minified) | 1,760 | 1,324 | 1,548 | 1.33× | Fused | Win |
| Google Maps Response Small (minified) | 2,348 | 1,254 | 1,027 | 1.87× | Fused | Win |
| Github Events Small (prettified) | 1,900 | 1,635 | 1,885 | 1.16× | Fused | Win |
| Google Maps Response Small (prettified) | 2,984 | 2,347 | 1,691 | 1.27× | Fused | Win |
| Instruments Small (minified) | 2,821 | 1,816 | 1,208 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 2,823 | 2,339 | 1,929 | 1.21× | Fused | Win |
| Marine IK Reverse Small (minified) | 677 | 576 | 379 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,868 | 1,943 | 1,364 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 630 | 606 | 380 | 1.04× | Fused | Win |
| Marine IK Small (prettified) | 2,027 | 2,029 | 1,370 | 1.00× | Two-stage | Win |
| Random Small (minified) | 1,548 | 1,139 | 945 | 1.36× | Fused | Win |
| Random Small (prettified) | 1,826 | 1,403 | 1,409 | 1.30× | Fused | Win |
| Twitter Small (minified) | 2,472 | 1,850 | 1,776 | 1.34× | Fused | Win |
| Twitter Small (prettified) | 2,370 | 2,271 | 2,212 | 1.04× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 596 | 279 | 92 | 2.14× | Fused | Win |
| Double (POD) | 850 | 582 | 153 | 1.46× | Fused | Win |
| Int64 (POD) | 2,132 | 937 | 328 | 2.28× | Fused | Win |
| String (POD) | 1,743 | 1,193 | 725 | 1.46× | Fused | Win |
| Uint64 (POD) | 1,844 | 934 | 329 | 1.97× | Fused | Win |
| Apache Builds (minified) | 766 (n/c) | 598 (n/c) | 395 (n/c) | 1.28× | Fused | Win |
| Canada (minified) | 422 (n/c) | 397 | 231 | 1.06× | Tie | Win |
| Apache Builds (prettified) | 794 (n/c) | 768 (n/c) | 482 (n/c) | 1.03× | Tie | Win |
| Canada (prettified) | 809 | 877 | 523 (n/c) | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,050 (n/c) | 739 | 247 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 1,300 (n/c) | 3,056 | 518 | 0.43× | Two-stage | Win |
| Discord (minified) | 867 (n/c) | 695 (n/c) | 312 (n/c) | 1.25× | Fused | Win |
| Discord (prettified) | 853 (n/c) | 1,942 (n/c) | 454 (n/c) | 0.44× | Two-stage | Win |
| Github Events (minified) | 766 (n/c) | 1,260 | 635 | 0.61× | Two-stage | Win |
| Google Maps Response (minified) | 1,936 | 1,379 | 474 | 1.40× | Fused | Win |
| Github Events (prettified) | 653 (n/c) | 704 (n/c) | 375 (n/c) | 0.93× | Tie | Win |
| Google Maps Response (prettified) | 2,543 | 2,705 | 929 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,476 | 1,802 | 637 | 1.37× | Fused | Win |
| Instruments (prettified) | 1,047 (n/c) | 1,326 (n/c) | 515 (n/c) | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 334 (n/c) | 283 | 91 (n/c) | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 959 | 983 | 300 | 0.98× | Tie | Win |
| Marine IK (minified) | 319 (n/c) | 290 (n/c) | 176 (n/c) | 1.10× | Fused | Win |
| Marine IK (prettified) | 2,040 | 2,064 | 1,231 | 0.99× | Tie | Win |
| Random (minified) | 1,062 | 986 | 515 | 1.08× | Fused | Win |
| Random (prettified) | 1,305 | 1,464 | 770 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,781 | 1,495 | 915 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,671 | 1,925 | 1,165 | 0.87× | Two-stage | Win |
| Apache Builds Small (minified) | 1,641 | 1,261 | 786 | 1.30× | Fused | Win |
| Canada Small (minified) | 799 | 756 | 385 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,679 | 1,509 | 1,012 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,653 | 1,785 | 898 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,262 | 1,013 | 519 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,504 | 1,706 | 874 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,475 | 1,207 | 702 | 1.22× | Fused | Win |
| Discord Small (prettified) | 1,345 | 1,626 | 936 | 0.83× | Two-stage | Win |
| Github Events Small (minified) | 1,729 | 1,152 | 611 | 1.50× | Fused | Win |
| Google Maps Response Small (minified) | 2,052 | 1,304 | 480 | 1.57× | Fused | Win |
| Github Events Small (prettified) | 1,535 | 1,449 | 765 | 1.06× | Fused | Win |
| Google Maps Response Small (prettified) | 2,304 | 2,721 | 950 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 2,369 | 1,649 | 642 | 1.44× | Fused | Win |
| Instruments Small (prettified) | 1,993 | 2,485 | 1,042 | 0.80× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 599 | 541 | 144 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,680 | 1,927 | 537 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 649 | 586 | 310 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 1,814 | 2,073 | 1,094 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,129 | 974 | 508 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,386 | 1,490 | 770 | 0.93× | Two-stage | Win |
| Twitter Small (minified) | 1,776 | 1,486 | 1,106 | 1.20× | Fused | Win |
| Twitter Small (prettified) | 1,639 | 1,952 | 1,391 | 0.84× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,050 | 639 | 291 | 3.21× | Fused | Win |
| Double (POD) | 1,438 | 812 | 348 | 1.77× | Fused | Win |
| Int64 (POD) | 3,465 | 1,729 | 798 | 2.00× | Fused | Win |
| String (POD) | 2,843 | 1,801 | 1,938 | 1.58× | Fused | Win |
| Uint64 (POD) | 3,710 | 1,684 | 787 | 2.20× | Fused | Win |
| Apache Builds (minified) | 2,921 | 2,410 | 1,697 | 1.21× | Fused | Win |
| Canada (minified) | 1,297 | 1,200 | 794 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 2,509 | 2,818 | 2,157 | 0.89× | Two-stage | Win |
| Canada (prettified) | 2,469 | 2,534 | 1,782 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,936 | 2,157 | 1,401 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,563 | 4,176 | 2,923 | 0.85× | Two-stage | Win |
| Discord (minified) | 3,292 | 2,485 | 1,935 | 1.32× | Fused | Win |
| Discord (prettified) | 2,305 | 3,447 | 2,733 | 0.67× | Two-stage | Win |
| Github Events (minified) | 2,309 | 2,133 | 2,059 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 3,099 | 2,210 | 1,423 | 1.40× | Fused | Win |
| Github Events (prettified) | 2,095 | 2,527 | 2,447 | 0.83× | Two-stage | Win |
| Google Maps Response (prettified) | 3,373 | 4,052 | 2,823 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,794 | 2,243 | 1,919 | 1.25× | Fused | Win |
| Instruments (prettified) | 2,557 | 3,324 | 3,086 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 931 | 834 | 654 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,793 | 2,797 | 2,375 | 1.00× | Tie | Win |
| Marine IK (minified) | 948 | 847 | 646 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,824 | 2,866 | 2,299 | 0.99× | Two-stage | Win |
| Random (minified) | 1,731 | 1,511 | 1,499 | 1.15× | Fused | Win |
| Random (prettified) | 1,834 | 2,207 | 2,230 | 0.83× | Two-stage | Loss |
| Twitter (minified) | 2,600 | 2,287 | 2,108 | 1.14× | Fused | Win |
| Twitter (prettified) | 2,048 | 2,880 | 2,726 | 0.71× | Two-stage | Win |
| Apache Builds Small (minified) | 3,150 | 2,555 | 1,825 | 1.23× | Fused | Win |
| Canada Small (minified) | 1,252 | 1,215 | 814 | 1.03× | Fused | Win |
| Apache Builds Small (prettified) | 2,503 | 3,042 | 2,338 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 2,284 | 2,649 | 1,904 | 0.86× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,155 | 1,677 | 1,374 | 1.29× | Fused | Win |
| CitmCatalog Small (prettified) | 2,220 | 2,835 | 2,361 | 0.78× | Two-stage | Win |
| Discord Small (minified) | 2,719 | 2,176 | 1,905 | 1.25× | Fused | Win |
| Discord Small (prettified) | 2,092 | 2,858 | 2,549 | 0.73× | Two-stage | Win |
| Github Events Small (minified) | 2,645 | 2,252 | 2,345 | 1.17× | Fused | Win |
| Google Maps Response Small (minified) | 2,880 | 2,055 | 1,467 | 1.40× | Fused | Win |
| Github Events Small (prettified) | 2,390 | 2,742 | 2,822 | 0.87× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,054 | 3,745 | 2,894 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,664 | 2,049 | 1,732 | 1.30× | Fused | Win |
| Instruments Small (prettified) | 2,392 | 3,137 | 2,777 | 0.76× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,000 | 888 | 650 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,555 | 3,019 | 2,343 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,109 | 957 | 658 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,734 | 3,315 | 2,336 | 0.82× | Two-stage | Win |
| Random Small (minified) | 2,191 | 1,912 | 1,483 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,999 | 2,817 | 2,155 | 0.71× | Two-stage | Win |
| Twitter Small (minified) | 2,807 | 2,417 | 2,373 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 2,288 | 3,026 | 2,967 | 0.76× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,455 | 582 | 251 | 4.21× | Fused | Win |
| Double (POD) | 1,337 | 661 | 299 | 2.02× | Fused | Win |
| Int64 (POD) | 3,699 | 1,509 | 739 | 2.45× | Fused | Win |
| String (POD) | 2,752 | 1,572 | 1,722 | 1.75× | Fused | Win |
| Uint64 (POD) | 3,813 | 1,500 | 728 | 2.54× | Fused | Win |
| Apache Builds (minified) | 2,720 | 2,230 | 1,994 | 1.22× | Fused | Win |
| Canada (minified) | 1,191 | 1,251 | 1,033 | 0.95× | Two-stage | Win |
| Apache Builds (prettified) | 2,530 | 2,736 | 2,508 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,468 | 2,665 | 2,275 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 3,013 | 2,238 | 1,627 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 3,747 | 4,314 | 3,336 | 0.87× | Two-stage | Win |
| Discord (minified) | 3,359 | 2,448 | 1,806 | 1.37× | Fused | Win |
| Discord (prettified) | 2,762 | 3,329 | 2,574 | 0.83× | Two-stage | Win |
| Github Events (minified) | 2,182 | 2,196 | 1,666 | 0.99× | Two-stage | Win |
| Google Maps Response (minified) | 3,041 | 2,121 | 1,152 | 1.43× | Fused | Win |
| Github Events (prettified) | 2,216 | 2,630 | 2,005 | 0.84× | Two-stage | Win |
| Google Maps Response (prettified) | 3,814 | 3,792 | 2,294 | 1.01× | Fused | Win |
| Instruments (minified) | 3,898 | 2,518 | 1,477 | 1.55× | Fused | Win |
| Instruments (prettified) | 3,310 | 3,785 | 2,364 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 803 | 812 | 675 | 0.99× | Two-stage | Win |
| Marine IK Reverse (prettified) | 2,592 | 2,840 | 2,431 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 821 | 828 | 667 | 0.99× | Two-stage | Win |
| Marine IK (prettified) | 2,656 | 2,843 | 2,403 | 0.93× | Two-stage | Win |
| Random (minified) | 1,486 | 1,501 | 1,397 | 0.99× | Two-stage | Win |
| Random (prettified) | 1,726 | 2,236 | 2,073 | 0.77× | Two-stage | Win |
| Twitter (minified) | 2,505 | 2,100 | 1,589 | 1.19× | Fused | Win |
| Twitter (prettified) | 2,146 | 2,736 | 2,085 | 0.78× | Two-stage | Win |
| Apache Builds Small (minified) | 2,865 | 2,469 | 1,944 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,371 | 1,205 | 899 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,601 | 2,912 | 2,398 | 0.89× | Two-stage | Win |
| Canada Small (prettified) | 2,456 | 2,794 | 2,112 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,196 | 1,784 | 1,318 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,392 | 2,865 | 2,209 | 0.83× | Two-stage | Win |
| Discord Small (minified) | 2,694 | 2,051 | 1,526 | 1.31× | Fused | Win |
| Discord Small (prettified) | 2,296 | 2,622 | 2,025 | 0.88× | Two-stage | Win |
| Github Events Small (minified) | 2,581 | 2,130 | 2,361 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,732 | 1,997 | 1,195 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 2,428 | 2,696 | 2,852 | 0.90× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,426 | 3,641 | 2,340 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,671 | 2,390 | 1,335 | 1.54× | Fused | Win |
| Instruments Small (prettified) | 3,050 | 3,576 | 2,158 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,008 | 864 | 635 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,619 | 3,002 | 2,327 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 1,146 | 920 | 643 | 1.25× | Fused | Win |
| Marine IK Small (prettified) | 2,844 | 3,152 | 2,331 | 0.90× | Two-stage | Win |
| Random Small (minified) | 2,040 | 1,894 | 1,415 | 1.08× | Fused | Win |
| Random Small (prettified) | 2,210 | 2,820 | 2,160 | 0.78× | Two-stage | Win |
| Twitter Small (minified) | 3,187 | 2,535 | 1,791 | 1.26× | Fused | Win |
| Twitter Small (prettified) | 2,509 | 3,160 | 2,286 | 0.79× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,697 | 406 | 127 | 4.18× | Fused | Win |
| Double (POD) | 1,092 | 516 | 165 | 2.12× | Fused | Win |
| Int64 (POD) | 3,025 | 1,025 | 441 | 2.95× | Fused | Win |
| String (POD) | 2,153 | 1,004 | 926 | 2.14× | Fused | Win |
| Uint64 (POD) | 2,717 | 841 | 410 | 3.23× | Fused | Win |
| Apache Builds (minified) | 2,172 | 1,286 | 810 | 1.69× | Fused | Win |
| Canada (minified) | 985 | 883 | 683 | 1.12× | Fused | Win |
| Apache Builds (prettified) | 1,571 | 1,481 | 1,122 | 1.06× | Fused | Win |
| Canada (prettified) | 1,709 (n/c) | 1,652 | 1,233 | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,682 | 1,436 | 1,114 | 1.87× | Fused | Win |
| CitmCatalog (prettified) | 3,018 | 2,360 | 2,451 | 1.28× | Fused | Win |
| Discord (minified) | 2,309 | 1,315 | 1,007 | 1.76× | Fused | Win |
| Discord (prettified) | 2,186 | 1,640 | 1,595 | 1.33× | Fused | Win |
| Github Events (minified) | 2,215 | 1,572 | 1,075 | 1.41× | Fused | Win |
| Google Maps Response (minified) | 2,203 | 1,234 | 652 | 1.79× | Fused | Win |
| Github Events (prettified) | 2,241 | 1,943 | 1,223 | 1.15× | Fused | Win |
| Google Maps Response (prettified) | 2,744 | 2,297 | 1,352 | 1.19× | Fused | Win |
| Instruments (minified) | 2,917 | 1,711 | 1,272 | 1.71× | Fused | Win |
| Instruments (prettified) | 2,739 | 2,297 | 1,725 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | 712 | 581 | 476 | 1.23× | Fused | Win |
| Marine IK Reverse (prettified) | 2,042 | 2,002 | 1,699 | 1.02× | Fused | Win |
| Marine IK (minified) | 792 | 648 | 491 | 1.22× | Fused | Win |
| Marine IK (prettified) | 2,340 | 2,049 | 1,747 | 1.14× | Fused | Win |
| Random (minified) | 1,155 | 1,148 | 788 | 1.01× | Tie | Win |
| Random (prettified) | 1,411 | 1,622 | 1,225 | 0.87× | Two-stage | Win |
| Twitter (minified) | 2,645 | 1,661 | 1,075 | 1.59× | Fused | Win |
| Twitter (prettified) | 2,207 | 2,451 | 1,190 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 2,217 | 1,607 | 1,090 | 1.38× | Fused | Win |
| Canada Small (minified) | 1,106 | 873 | 457 | 1.27× | Fused | Win |
| Apache Builds Small (prettified) | 2,008 | 2,021 | 1,522 | 0.99× | Tie | Win |
| Canada Small (prettified) | 1,849 | 1,697 | 1,094 | 1.09× | Fused | Win |
| CitmCatalog Small (minified) | 1,745 | 1,207 | 1,007 | 1.45× | Fused | Win |
| CitmCatalog Small (prettified) | 2,086 | 2,061 | 1,725 | 1.01× | Tie | Win |
| Discord Small (minified) | 2,316 | 1,537 | 1,151 | 1.51× | Fused | Win |
| Discord Small (prettified) | 2,108 | 1,969 | 1,372 | 1.07× | Fused | Win |
| Github Events Small (minified) | 2,101 | 1,656 | 1,428 | 1.27× | Fused | Win |
| Google Maps Response Small (minified) | 2,214 | 1,368 | 821 | 1.62× | Fused | Win |
| Github Events Small (prettified) | 2,233 | 1,828 | 1,629 | 1.22× | Fused | Win |
| Google Maps Response Small (prettified) | 2,671 | 2,494 | 1,539 | 1.07× | Fused | Win |
| Instruments Small (minified) | 2,729 | 1,712 | 1,236 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,811 | 2,480 | 1,728 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 703 | 622 | 434 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,823 | 2,019 | 1,400 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 852 | 561 | 378 | 1.52× | Fused | Win |
| Marine IK Small (prettified) | 2,116 | 1,934 | 1,264 | 1.09× | Fused | Win |
| Random Small (minified) | 1,483 | 1,090 | 765 | 1.36× | Fused | Win |
| Random Small (prettified) | 1,481 | 1,493 | 1,223 | 0.99× | Tie | Win |
| Twitter Small (minified) | 1,800 | 1,691 | 1,064 | 1.06× | Fused | Win |
| Twitter Small (prettified) | 2,045 | 2,223 | 1,113 | 0.92× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,384 | 336 | 184 (n/c) | 4.13× | Fused | Win |
| Double (POD) | 1,124 | 382 | 210 | 2.94× | Fused | Win |
| Int64 (POD) | 2,802 | 947 | 513 | 2.96× | Fused | Win |
| String (POD) | 2,468 | 970 | 1,191 | 2.55× | Fused | Win |
| Uint64 (POD) | 2,827 | 937 | 500 | 3.02× | Fused | Win |
| Apache Builds (minified) | 2,398 | 1,542 | 1,122 | 1.56× | Fused | Win |
| Canada (minified) | 1,056 | 914 | 629 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,262 | 1,891 | 1,421 | 1.20× | Fused | Win |
| Canada (prettified) | 2,027 | 1,909 | 1,385 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,915 | 1,784 | 1,179 | 1.63× | Fused | Win |
| CitmCatalog (prettified) | 3,756 | 3,368 | 2,360 | 1.12× | Fused | Win |
| Discord (minified) | 3,236 | 1,709 | 1,609 | 1.89× | Fused | Win |
| Discord (prettified) | 2,816 | 2,286 | 2,280 | 1.23× | Fused | Win |
| Github Events (minified) | 2,631 | 2,028 | 2,038 | 1.30× | Fused | Win |
| Google Maps Response (minified) | 2,591 | 1,470 | 1,105 | 1.76× | Fused | Win |
| Github Events (prettified) | 2,469 | 2,271 | 2,356 | 1.09× | Fused | Win |
| Google Maps Response (prettified) | 3,265 | 2,674 | 2,148 | 1.22× | Fused | Win |
| Instruments (minified) | 3,341 | 1,994 | 1,415 | 1.68× | Fused | Win |
| Instruments (prettified) | 3,134 | 2,825 | 2,230 | 1.11× | Fused | Win |
| Marine IK Reverse (minified) | 834 | 699 | 483 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,333 | 2,302 | 1,678 | 1.01× | Fused | Win |
| Marine IK (minified) | 837 | 705 | 488 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,427 | 2,322 | 1,688 | 1.04× | Fused | Win |
| Random (minified) | 1,789 | 1,385 | 1,089 | 1.29× | Fused | Win |
| Random (prettified) | 2,112 | 1,811 | 1,608 | 1.17× | Fused | Win |
| Twitter (minified) | 3,038 | 2,216 | 1,996 | 1.37× | Fused | Win |
| Twitter (prettified) | 2,790 | 2,681 | 2,502 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 2,264 | 1,781 | 1,427 | 1.27× | Fused | Win |
| Canada Small (minified) | 968 | 872 | 530 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,174 | 2,099 | 1,746 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,885 | 1,851 | 1,224 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 2,034 | 1,434 | 1,121 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 2,421 | 2,296 | 1,888 | 1.05× | Fused | Win |
| Discord Small (minified) | 2,902 | 1,768 | 1,682 | 1.64× | Fused | Win |
| Discord Small (prettified) | 2,325 | 2,179 | 2,221 | 1.07× | Fused | Win |
| Github Events Small (minified) | 2,580 | 1,826 | 1,981 | 1.41× | Fused | Win |
| Google Maps Response Small (minified) | 2,682 | 1,481 | 1,142 | 1.81× | Fused | Win |
| Github Events Small (prettified) | 2,610 | 2,143 | 2,453 | 1.22× | Fused | Win |
| Google Maps Response Small (prettified) | 3,200 | 2,713 | 2,216 | 1.18× | Fused | Win |
| Instruments Small (minified) | 3,385 | 1,992 | 1,417 | 1.70× | Fused | Win |
| Instruments Small (prettified) | 3,072 | 2,853 | 2,251 | 1.08× | Fused | Win |
| Marine IK Reverse Small (minified) | 877 | 724 | 433 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,248 | 2,336 | 1,568 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 956 | 770 | 437 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,437 | 2,473 | 1,555 | 0.99× | Two-stage | Win |
| Random Small (minified) | 2,015 | 1,474 | 1,175 | 1.37× | Fused | Win |
| Random Small (prettified) | 2,190 | 1,909 | 1,729 | 1.15× | Fused | Win |
| Twitter Small (minified) | 2,652 | 2,251 | 1,983 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,787 | 2,709 | 2,499 | 1.03× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins all POD input, 25 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 25 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC splits less cleanly: 19 of the 22 minified documents go to the fused path (the tape takes Github Events and Instruments, and Apache Builds is a tie), but of the 22 prettified ones the tape wins 13, the fused path 2 (Apache Builds Small and Github Events Small) and 7 are ties, all seven full-size. On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (minified Apache Builds Small is a tie on macOS/GCC) and 34 of the 44 prettified documents (eight to the tape, two ties).

**POD-type tests: the fused path wins all 25 freshly allocated and all 25 reused, on every build.** It runs them 1.28× (String, Windows/MSVC) to 4.03× (Bool, Linux/GCC) faster than the two-stage path, and 1.46× (String, Windows/MSVC) to 4.21× (Bool, Linux/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the platform.

**Minified documents: the fused path's, at both sizes.** The fused path wins 101 of the 110 minified documents: all 22 on macOS/Clang, 21 of 22 on Linux/Clang, where the two-stage path wins minified Github Events by 12% (1,867 against 1,660 MB/s), 21 of 22 on macOS/GCC, where minified Apache Builds Small is a tie, 19 of 22 under MSVC, where the two-stage path wins Github Events (880 against 493 MB/s) and Instruments (1,421 against 705) and Apache Builds is a tie, and 18 of 22 on Linux/GCC, where the two-stage path wins minified Canada (1,013 against 989 MB/s), Github Events (1,700 against 1,613), Marine IK (746 against 739) and Marine IK Reverse (739 against 734), all by 5% or less. The fused path's margins run from 0.5% (Random on Linux/GCC, 1,110 against 1,104 MB/s) to 1.95× (Discord on macOS/Clang, 2,580 against 1,323 MB/s) on the full-size documents and up to 1.87× on the Small ones (Google Maps Response Small on macOS/Clang, 2,348 against 1,254 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 56 of 66.** It wins all 11 at each size on Linux/Clang, all 11 Small prettified documents and 10 of the 11 full-size ones on Linux/GCC, and 4 of the 11 full-size and 9 of the 11 Small prettified documents under MSVC. The fused path wins prettified Google Maps Response on Linux/GCC, by 1%, and prettified Apache Builds Small and Github Events Small under MSVC, by 14% and 8%; the other seven full-size prettified documents under MSVC are ties. The tape's margins run from 1% (Canada on Linux/Clang, 2,128 against 2,102 MB/s) to 36% (Discord on Linux/Clang, 2,593 against 1,907) on the full-size documents and from 5% to 38% on the Small ones, widest on Discord Small on Linux/Clang. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 57 of the 66 prettified x86 documents; the fused path wins prettified Canada, Marine IK and Marine IK Reverse on Linux/Clang and prettified Apache Builds, Marine IK, Apache Builds Small and Github Events Small under MSVC, and prettified Canada and Marine IK Reverse are ties under MSVC.

**Prettified documents on the M2 Pro: mostly the fused path.** Of the 44 prettified documents, the fused path wins 34, the tape wins 8 and 2 are ties; the tape's wins are prettified Apache Builds, Github Events and Marine IK Reverse, and Apache Builds Small, Marine IK Reverse Small and Twitter Small, on macOS/GCC, and Marine IK Small and Marine IK Reverse Small on macOS/Clang. Prettified Canada and Twitter are ties on macOS/GCC. Reused, the fused path wins 35, the tape 6, and 3 are ties. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified input on Linux/Clang is where Jsonifier is weakest.** All three of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are prettified documents on Linux/Clang: Github Events by 1% (1,912 against 1,933 MB/s), Github Events Small by 5% (2,230 against 2,337) and Twitter Small by 3% (2,691 against 2,772). The two ties are prettified Twitter on Linux/Clang and prettified Github Events on macOS/Clang. With objects reused the faster path again loses three, all prettified: Random on Linux/Clang by 1% (2,207 against 2,230), and Github Events Small on Linux/Clang by 3% (2,742 against 2,822) and on Linux/GCC by 5% (2,696 against 2,852). We have not profiled what these documents have in common.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 34% of its Linux/Clang throughput, the two-stage path 41% and simdjson 25%. Because the two Jsonifier paths lose a similar share, MSVC routes minified input much as Linux/Clang does, though most of its full-size prettified documents end as ties. The spread between documents is about the same for the two Jsonifier paths (the fused path from 25% on Apache Builds to 64% on Twitter, the two-stage path from 26% on Apache Builds to 72% on Instruments) and widest for simdjson, which keeps 16% of its Linux/Clang throughput on Google Maps Response and 43% on Twitter. We have not profiled why.

**Against simdjson, Jsonifier wins 240 of 245 tests with whichever path is faster, ties two and loses three** (all prettified documents on Linux/Clang). The fused path alone wins 228, ties 3 and loses 14, its eleven extra losses all on x86: nine on Linux/Clang (eight prettified documents and minified Github Events) and prettified Apache Builds and Random on Linux/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 90 MB/s, against 159 MB/s for the same document in declared order, so the faster Jsonifier path is 3.12× faster than simdjson freshly allocated (3.68× reused). The prettified form shows the same gap, at 281 MB/s against 1,165, and the faster Jsonifier path is 3.21× faster (3.28× reused). The Small copies show it too, at 138 MB/s minified and 522 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.16× to 1.61× faster than simdjson on the full-size reversed document, freshly allocated, and 1.17× to 1.72× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, almost everything belongs to the fused path. Routing each test to its faster path would raise the record against simdjson from 228 wins, 3 ties and 14 losses to 240 wins, 2 ties and 3 losses. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,505 | 1,354 | 1.11× | Win |
| Canada (minified) | 1,013 | 926 | 1.09× | Win |
| Apache Builds (prettified) | 1,890 | 1,705 | 1.11× | Win |
| Canada (prettified) | 2,236 | 2,062 | 1.08× | Win |
| CitmCatalog (minified) | 1,862 | 1,421 | 1.31× | Win |
| CitmCatalog (prettified) | 3,678 | 2,953 | 1.25× | Win |
| Discord (minified) | 1,821 | 1,575 | 1.16× | Win |
| Discord (prettified) | 2,559 | 2,280 | 1.12× | Win |
| Github Events (minified) | 1,700 | 1,790 | 0.95× | **Loss** |
| Google Maps Response (minified) | 1,932 | 1,281 | 1.51× | Win |
| Github Events (prettified) | 2,028 | 1,908 | 1.06× | Win |
| Google Maps Response (prettified) | 3,506 | 2,524 | 1.39× | Win |
| Instruments (minified) | 2,240 | 1,842 | 1.22× | Win |
| Instruments (prettified) | 3,386 | 2,954 | 1.15× | Win |
| Marine IK Reverse (minified) | 739 | 713 | 1.04× | Win |
| Marine IK Reverse (prettified) | 2,579 | 2,543 | 1.01× | Win |
| Marine IK (minified) | 746 | 688 | 1.08× | Win |
| Marine IK (prettified) | 2,593 | 2,495 | 1.04× | Win |
| Random (minified) | 1,104 | 1,089 | 1.01× | Win |
| Random (prettified) | 1,689 | 1,649 | 1.02× | Win |
| Twitter (minified) | 1,673 | 1,739 | 0.96× | **Loss** |
| Twitter (prettified) | 2,165 | 2,253 | 0.96× | **Loss** |
| Apache Builds Small (minified) | 2,149 | 1,682 | 1.28× | Win |
| Canada Small (minified) | 968 | 934 | 1.04× | Win |
| Apache Builds Small (prettified) | 2,436 | 2,099 | 1.16× | Win |
| Canada Small (prettified) | 2,259 | 2,208 | 1.02× | Win |
| CitmCatalog Small (minified) | 1,484 | 1,219 | 1.22× | Win |
| CitmCatalog Small (prettified) | 2,480 | 2,062 | 1.20× | Win |
| Discord Small (minified) | 1,624 | 1,523 | 1.07× | Win |
| Discord Small (prettified) | 2,098 | 2,023 | 1.04× | Win |
| Github Events Small (minified) | 1,773 | 1,799 | 0.99× | **Loss** |
| Google Maps Response Small (minified) | 1,781 | 1,266 | 1.41× | Win |
| Github Events Small (prettified) | 2,278 | 2,233 | 1.02× | Win |
| Google Maps Response Small (prettified) | 3,321 | 2,498 | 1.33× | Win |
| Instruments Small (minified) | 2,204 | 1,705 | 1.29× | Win |
| Instruments Small (prettified) | 3,352 | 2,729 | 1.23× | Win |
| Marine IK Reverse Small (minified) | 723 | 728 | 0.99× | **Loss** |
| Marine IK Reverse Small (prettified) | 2,574 | 2,598 | 0.99× | **Loss** |
| Marine IK Small (minified) | 772 | 718 | 1.07× | Win |
| Marine IK Small (prettified) | 2,687 | 2,589 | 1.04× | Win |
| Random Small (minified) | 1,565 | 1,201 | 1.30× | Win |
| Random Small (prettified) | 2,289 | 1,837 | 1.25× | Win |
| Twitter Small (minified) | 2,297 | 2,105 | 1.09× | Win |
| Twitter Small (prettified) | 2,789 | 2,707 | 1.03× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 889 | 760 | 1.17× | Win |
| Canada (minified) | 695 (n/c) | 363 | 1.91× | Win |
| Apache Builds (prettified) | 1,141 | 993 | 1.15× | Win |
| Canada (prettified) | 1,432 | 684 (n/c) | 2.09× | Win |
| CitmCatalog (minified) | 1,384 | 696 | 1.99× | Win |
| CitmCatalog (prettified) | 2,313 | 1,595 | 1.45× | Win |
| Discord (minified) | 1,178 | 997 | 1.18× | Win |
| Discord (prettified) | 1,677 | 1,350 | 1.24× | Win |
| Github Events (minified) | 1,100 | 1,081 | 1.02× | Tie |
| Google Maps Response (minified) | 1,090 | 777 | 1.40× | Win |
| Github Events (prettified) | 1,626 | 1,300 | 1.25× | Win |
| Google Maps Response (prettified) | 1,991 | 1,599 | 1.24× | Win |
| Instruments (minified) | 1,695 | 1,208 | 1.40× | Win |
| Instruments (prettified) | 2,083 | 1,674 | 1.24× | Win |
| Marine IK Reverse (minified) | 528 | 458 | 1.15× | Win |
| Marine IK Reverse (prettified) | 1,866 | 1,690 | 1.10× | Win |
| Marine IK (minified) | 575 | 486 | 1.18× | Win |
| Marine IK (prettified) | 2,030 | 1,541 | 1.32× | Win |
| Random (minified) | 822 | 696 | 1.18× | Win |
| Random (prettified) | 1,155 | 1,053 | 1.10× | Win |
| Twitter (minified) | 1,626 | 1,192 | 1.36× | Win |
| Twitter (prettified) | 1,663 | 1,327 | 1.25× | Win |
| Apache Builds Small (minified) | 1,391 | 867 | 1.60× | Win |
| Canada Small (minified) | 691 | 331 | 2.08× | Win |
| Apache Builds Small (prettified) | 1,602 | 1,253 | 1.28× | Win |
| Canada Small (prettified) | 1,370 | 1,261 | 1.09× | Win |
| CitmCatalog Small (minified) | 991 | 795 | 1.25× | Win |
| CitmCatalog Small (prettified) | 1,592 | 1,258 | 1.27× | Win |
| Discord Small (minified) | 1,233 | 1,029 | 1.20× | Win |
| Discord Small (prettified) | 1,549 | 1,314 | 1.18× | Win |
| Github Events Small (minified) | 1,221 | 1,057 | 1.16× | Win |
| Google Maps Response Small (minified) | 1,163 | 907 | 1.28× | Win |
| Github Events Small (prettified) | 1,370 | 1,269 | 1.08× | Win |
| Google Maps Response Small (prettified) | 2,076 | 1,774 | 1.17× | Win |
| Instruments Small (minified) | 1,550 | 1,211 | 1.28× | Win |
| Instruments Small (prettified) | 2,301 | 1,797 | 1.28× | Win |
| Marine IK Reverse Small (minified) | 495 | 493 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 1,666 | 1,636 | 1.02× | Win |
| Marine IK Small (minified) | 497 | 443 | 1.12× | Win |
| Marine IK Small (prettified) | 1,693 | 1,406 | 1.20× | Win |
| Random Small (minified) | 897 | 711 | 1.26× | Win |
| Random Small (prettified) | 778 | 976 | 0.80× | **Loss** |
| Twitter Small (minified) | 1,409 | 1,084 | 1.30× | Win |
| Twitter Small (prettified) | 1,863 | 1,279 | 1.46× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,230 | 1,726 | 1.29× | Win |
| Canada (minified) | 1,251 | 1,118 | 1.12× | Win |
| Apache Builds (prettified) | 2,736 | 2,202 | 1.24× | Win |
| Canada (prettified) | 2,665 | 2,443 | 1.09× | Win |
| CitmCatalog (minified) | 2,238 | 1,671 | 1.34× | Win |
| CitmCatalog (prettified) | 4,314 | 3,415 | 1.26× | Win |
| Discord (minified) | 2,448 | 1,832 | 1.34× | Win |
| Discord (prettified) | 3,329 | 2,616 | 1.27× | Win |
| Github Events (minified) | 2,196 | 1,886 | 1.16× | Win |
| Google Maps Response (minified) | 2,121 | 1,355 | 1.56× | Win |
| Github Events (prettified) | 2,630 | 2,244 | 1.17× | Win |
| Google Maps Response (prettified) | 3,792 | 2,678 | 1.42× | Win |
| Instruments (minified) | 2,518 | 1,901 | 1.32× | Win |
| Instruments (prettified) | 3,785 | 3,002 | 1.26× | Win |
| Marine IK Reverse (minified) | 812 | 797 | 1.02× | Win |
| Marine IK Reverse (prettified) | 2,840 | 2,809 | 1.01× | Win |
| Marine IK (minified) | 828 | 792 | 1.05× | Win |
| Marine IK (prettified) | 2,843 | 2,775 | 1.02× | Win |
| Random (minified) | 1,501 | 1,426 | 1.05× | Win |
| Random (prettified) | 2,236 | 2,105 | 1.06× | Win |
| Twitter (minified) | 2,100 | 2,042 | 1.03× | Win |
| Twitter (prettified) | 2,736 | 2,639 | 1.04× | Win |
| Apache Builds Small (minified) | 2,469 | 1,850 | 1.33× | Win |
| Canada Small (minified) | 1,205 | 1,104 | 1.09× | Win |
| Apache Builds Small (prettified) | 2,912 | 2,306 | 1.26× | Win |
| Canada Small (prettified) | 2,794 | 2,547 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,784 | 1,342 | 1.33× | Win |
| CitmCatalog Small (prettified) | 2,865 | 2,272 | 1.26× | Win |
| Discord Small (minified) | 2,051 | 1,810 | 1.13× | Win |
| Discord Small (prettified) | 2,622 | 2,381 | 1.10× | Win |
| Github Events Small (minified) | 2,130 | 2,075 | 1.03× | Win |
| Google Maps Response Small (minified) | 1,997 | 1,375 | 1.45× | Win |
| Github Events Small (prettified) | 2,696 | 2,491 | 1.08× | Win |
| Google Maps Response Small (prettified) | 3,641 | 2,693 | 1.35× | Win |
| Instruments Small (minified) | 2,390 | 1,766 | 1.35× | Win |
| Instruments Small (prettified) | 3,576 | 2,816 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 864 | 834 | 1.04× | Win |
| Marine IK Reverse Small (prettified) | 3,002 | 2,958 | 1.01× | Win |
| Marine IK Small (minified) | 920 | 824 | 1.12× | Win |
| Marine IK Small (prettified) | 3,152 | 2,984 | 1.06× | Win |
| Random Small (minified) | 1,894 | 1,377 | 1.38× | Win |
| Random Small (prettified) | 2,820 | 2,064 | 1.37× | Win |
| Twitter Small (minified) | 2,535 | 2,290 | 1.11× | Win |
| Twitter Small (prettified) | 3,160 | 2,902 | 1.09× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,286 | 893 | 1.44× | Win |
| Canada (minified) | 883 | 778 | 1.13× | Win |
| Apache Builds (prettified) | 1,481 | 1,201 | 1.23× | Win |
| Canada (prettified) | 1,652 | 1,298 | 1.27× | Win |
| CitmCatalog (minified) | 1,436 | 1,223 | 1.17× | Win |
| CitmCatalog (prettified) | 2,360 | 2,443 | 0.97× | **Loss** |
| Discord (minified) | 1,315 | 1,019 | 1.29× | Win |
| Discord (prettified) | 1,640 | 1,575 | 1.04× | Win |
| Github Events (minified) | 1,572 | 1,419 | 1.11× | Win |
| Google Maps Response (minified) | 1,234 | 917 | 1.34× | Win |
| Github Events (prettified) | 1,943 | 1,469 | 1.32× | Win |
| Google Maps Response (prettified) | 2,297 | 1,791 | 1.28× | Win |
| Instruments (minified) | 1,711 | 1,278 | 1.34× | Win |
| Instruments (prettified) | 2,297 | 1,769 | 1.30× | Win |
| Marine IK Reverse (minified) | 581 | 517 | 1.12× | Win |
| Marine IK Reverse (prettified) | 2,002 | 1,632 | 1.23× | Win |
| Marine IK (minified) | 648 | 516 | 1.26× | Win |
| Marine IK (prettified) | 2,049 | 1,710 | 1.20× | Win |
| Random (minified) | 1,148 | 804 | 1.43× | Win |
| Random (prettified) | 1,622 | 1,255 | 1.29× | Win |
| Twitter (minified) | 1,661 | 1,285 | 1.29× | Win |
| Twitter (prettified) | 2,451 | 1,638 | 1.50× | Win |
| Apache Builds Small (minified) | 1,607 | 1,325 | 1.21× | Win |
| Canada Small (minified) | 873 | 602 | 1.45× | Win |
| Apache Builds Small (prettified) | 2,021 | 1,587 | 1.27× | Win |
| Canada Small (prettified) | 1,697 | 1,343 | 1.26× | Win |
| CitmCatalog Small (minified) | 1,207 | 1,010 | 1.20× | Win |
| CitmCatalog Small (prettified) | 2,061 | 1,654 | 1.25× | Win |
| Discord Small (minified) | 1,537 | 1,229 | 1.25× | Win |
| Discord Small (prettified) | 1,969 | 1,649 | 1.19× | Win |
| Github Events Small (minified) | 1,656 | 1,277 | 1.30× | Win |
| Google Maps Response Small (minified) | 1,368 | 981 | 1.39× | Win |
| Github Events Small (prettified) | 1,828 | 1,440 | 1.27× | Win |
| Google Maps Response Small (prettified) | 2,494 | 1,826 | 1.37× | Win |
| Instruments Small (minified) | 1,712 | 1,169 | 1.46× | Win |
| Instruments Small (prettified) | 2,480 | 1,883 | 1.32× | Win |
| Marine IK Reverse Small (minified) | 622 | 609 | 1.02× | Win |
| Marine IK Reverse Small (prettified) | 2,019 | 1,885 | 1.07× | Win |
| Marine IK Small (minified) | 561 | 507 | 1.11× | Win |
| Marine IK Small (prettified) | 1,934 | 1,787 | 1.08× | Win |
| Random Small (minified) | 1,090 | 836 | 1.30× | Win |
| Random Small (prettified) | 1,493 | 1,228 | 1.22× | Win |
| Twitter Small (minified) | 1,691 | 1,470 | 1.15× | Win |
| Twitter Small (prettified) | 2,223 | 1,374 | 1.62× | Win |

In the freshly allocated run, two-stage Jsonifier wins 79 of these 88 tests, ties 2 and loses 7, every loss by 1% to 20%: six on Linux/GCC (minified Github Events, Github Events Small and Twitter, prettified Twitter, and Marine IK Reverse Small in both forms) and one on macOS/GCC (prettified Random Small, the widest at 0.80×). The ties are minified Github Events and minified Marine IK Reverse Small on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 73, ties 1 and loses 14. With objects reused, two-stage Jsonifier wins 87 and loses 1 (prettified CitmCatalog on macOS/GCC, by 3%), and the fused path beats the reflection reader on 76 and loses 12. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 3% on 33 of the 40 results; all seven wider gaps are on macOS/GCC, and the widest is freshly allocated prettified Twitter Small, where the two-stage row runs at 4,205 MB/s against the "jsonifier" row's 3,522. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,652 | 3,521 | 1.89× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,580 | 4,132 | 1.83× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 3,975 | 3,122 | 1.27× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,810 | 3,706 | 1.30× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,119 | 7,005 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,299 | 8,336 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,338 | 5,948 | 0.90× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,563 | 7,062 | 0.93× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,072 | 7,277 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,874 | 8,221 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,770 | 5,652 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,727 | 6,816 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,717 (n/c) | 3,967 | 1.19× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,629 | 4,552 | 1.02× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,793 | 2,823 | 1.34× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,205 | 3,855 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,949 | 4,286 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,451 | 5,042 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,830 | 3,844 | 1.00× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,381 | 4,614 | 0.95× | Loss |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,957 | 3,692 | 1.88× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,846 | 4,274 | 1.84× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,356 | 3,381 | 1.29× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,116 | 4,003 | 1.28× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,274 | 7,113 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,545 | 8,515 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,668 | 5,986 | 0.95× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,871 | 7,289 | 0.94× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,171 | 7,352 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,010 | 8,225 | 1.10× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,070 | 5,901 | 1.03× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,963 | 6,997 | 1.00× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,890 | 3,855 | 1.27× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,860 | 4,762 | 1.02× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,375 | 3,230 | 1.04× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,805 | 3,598 | 1.06× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 5,006 | 4,384 | 1.14× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,538 | 5,152 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,138 | 4,120 | 1.00× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,645 | 4,884 | 0.95× | Loss |

Jsonifier wins 30 of the 40 results, ties 2 and loses 8, and wins by up to 1.89× (minified Twitter under MSVC, freshly allocated). Every loss is on the Small copy. Freshly allocated, the losses are minified Twitter Small on Linux/Clang (0.90×) and prettified Twitter Small on Linux/Clang (0.93×), Linux/GCC (0.99×) and macOS/Clang (0.95×), and the ties are prettified Twitter on macOS/GCC and minified Twitter Small on macOS/Clang. The reused run has 16 wins and 4 losses, on the same four Small results (0.94× to 1.00×). Of the 20 full-size results, 19 are wins and one a tie. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,785 | 2,611 | 1.07× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,600 | 2,628 | 1.37× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,940 | 2,444 | 1.20× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,987 | 1,823 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,512 | 1,995 | 1.26× | Win |

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

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (string unescaping, discussed in §6) rather than as a mandatory preprocessing pass.

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
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step: up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, write_structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains; each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

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

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte), so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator. The position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The stage-1 companion paper covers this mechanism in full [§6.1 there].

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes*: every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on 107 of 110 minified documents and on 32 of 44 prettified documents on the M2 Pro; slower on 57 of 66 prettified x86 documents (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 168 of 245 freshly allocated tests: all POD-type tests, 103 of 110 minified documents and 34 of 44 prettified documents on the M2 Pro, while the two-stage path wins 57 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 237 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 70 of 88, and on partial reading it wins 37 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*