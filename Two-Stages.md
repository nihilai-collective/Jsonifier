# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 105 of the 110 minified documents (the tape takes three, two of them under MSVC, and two are ties), the tape is faster on 64 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 33 of the 44 prettified documents, the tape 5 and 6 are ties. Three AVX-512 builds show the same split: the fused path wins 64 of their 66 minified documents and the tape 64 of their 66 prettified ones.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep [5] published November 17, 2026, with Jsonifier [79b3e67](https://github.com/nihilai-collective/jsonifier/commit/79b3e67), Glaze [a53ddd5](https://github.com/stephenberry/glaze/commit/a53ddd59), simdjson [36c5b3c](https://github.com/simdjson/simdjson/commit/36c5b3cfc) and BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Seven freshly allocated full-parse rows contain non-converged results (two on macOS/GCC and five on macOS/Clang), as do five reused rows (three on macOS/GCC and two on macOS/Clang); every x86 row converged. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 165 tests, 8 are statistical ties, and the two-stage path wins 72. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 4 | 3 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 43 | 4 | 2 | 44 of 49 |
| **Aggregate** | **165** | **8** | **72** | **238 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 21 / 0 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 18 / 3 / 1 |
| Small minified documents | 10 / 0 / 1 | 22 / 0 / 0 | 20 / 2 / 0 |
| Small prettified documents | 0 / 0 / 11 | 0 / 0 / 22 | 15 / 3 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 48 / 0 / 1 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 47 / 0 / 2 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 47 / 0 / 2 | 47 / 0 / 2 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **242 / 0 / 3** | **239 / 0 / 6** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 26 | 0 | 23 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 3 | 4 | 46 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 45 | 0 | 4 | 47 of 49 |
| **Aggregate** | **168** | **3** | **74** | **240 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 0 / 11 | 1 / 0 / 21 | 19 / 1 / 2 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 0 / 0 / 11 | 0 / 0 / 22 | 14 / 2 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 47 / 0 / 2 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 46 / 0 / 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **239 / 0 / 6** |

With objects reused, across 245 tests the fused path wins 168, ties 3 and loses 74 to the two-stage path, and the x86 split holds: the tape wins 32 of the 33 full-size prettified documents (the fused path takes Canada on Linux/GCC) and all 33 Small prettified ones, and the fused path wins 65 of the 66 minified documents, the tape taking Canada under MSVC. On the M2 Pro the fused path wins all 44 minified documents and 33 of the 44 prettified ones, with 8 tape wins and 3 ties. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins 239, ties none and loses 6.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 612 | 261 | 122 | 2.34× | Fused | Win |
| Double (POD) | 793 | 542 | 260 | 1.46× | Fused | Win |
| Int64 (POD) | 1,589 | 890 | 565 | 1.79× | Fused | Win |
| String (POD) | 834 | 701 | 672 | 1.19× | Fused | Win |
| Uint64 (POD) | 1,671 | 914 | 560 | 1.83× | Fused | Win |
| Apache Builds (minified) | 816 | 770 | 641 | 1.06× | Fused | Win |
| Canada (minified) | 544 | 562 | 373 | 0.97× | Two-stage | Win |
| Apache Builds (prettified) | 993 | 1,013 | 840 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,149 | 1,249 | 862 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,520 | 1,250 | 541 | 1.22× | Fused | Win |
| CitmCatalog (prettified) | 2,329 | 2,572 | 1,198 | 0.91× | Two-stage | Win |
| Discord (minified) | 1,188 | 968 | 701 | 1.23× | Fused | Win |
| Discord (prettified) | 1,330 | 1,426 | 1,032 | 0.93× | Two-stage | Win |
| Github Events (minified) | 955 | 859 | 668 | 1.11× | Fused | Win |
| Google Maps Response (minified) | 1,336 | 1,167 | 562 | 1.14× | Fused | Win |
| Github Events (prettified) | 1,002 | 1,023 | 808 | 0.98× | Two-stage | Win |
| Google Maps Response (prettified) | 2,205 | 2,317 | 1,143 | 0.95× | Two-stage | Win |
| Instruments (minified) | 1,997 | 1,454 | 836 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,096 | 2,251 | 1,364 | 0.93× | Two-stage | Win |
| Marine IK Reverse (minified) | 601 | 551 | 181 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 1,889 | 1,964 | 648 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 617 | 559 | 302 | 1.10× | Fused | Win |
| Marine IK (prettified) | 1,955 | 1,933 | 1,113 | 1.01× | Fused | Win |
| Random (minified) | 718 | 655 | 503 | 1.10× | Fused | Win |
| Random (prettified) | 931 | 1,018 | 769 | 0.92× | Two-stage | Win |
| Twitter (minified) | 1,124 | 987 | 845 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,324 | 1,400 | 1,184 | 0.95× | Two-stage | Win |
| Apache Builds Small (minified) | 1,119 | 1,159 | 665 | 0.97× | Two-stage | Win |
| Canada Small (minified) | 505 | 503 | 309 | 1.00× | Fused | Win |
| Apache Builds Small (prettified) | 1,194 | 1,393 | 837 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 1,115 | 1,189 | 748 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 817 | 687 | 483 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,096 | 1,199 | 848 | 0.91× | Two-stage | Win |
| Discord Small (minified) | 1,050 | 804 | 651 | 1.31× | Fused | Win |
| Discord Small (prettified) | 1,109 | 1,170 | 894 | 0.95× | Two-stage | Win |
| Github Events Small (minified) | 1,083 | 923 | 680 | 1.17× | Fused | Win |
| Google Maps Response Small (minified) | 1,173 | 1,013 | 543 | 1.16× | Fused | Win |
| Github Events Small (prettified) | 977 | 1,177 | 797 | 0.83× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,904 | 2,028 | 1,110 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 2,053 | 1,435 | 805 | 1.43× | Fused | Win |
| Instruments Small (prettified) | 1,972 | 2,180 | 1,305 | 0.90× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 413 | 406 | 144 | 1.02× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,241 | 1,478 | 554 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 472 | 436 | 260 | 1.08× | Fused | Win |
| Marine IK Small (prettified) | 1,417 | 1,571 | 986 | 0.90× | Two-stage | Win |
| Random Small (minified) | 820 | 733 | 478 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,032 | 1,132 | 739 | 0.91× | Two-stage | Win |
| Twitter Small (minified) | 1,676 | 1,339 | 1,281 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 1,525 | 1,634 | 1,658 | 0.93× | Two-stage | Loss |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,045 | 633 | 287 | 3.23× | Fused | Win |
| Double (POD) | 1,292 | 736 | 360 | 1.76× | Fused | Win |
| Int64 (POD) | 3,141 | 1,678 | 789 | 1.87× | Fused | Win |
| String (POD) | 1,846 | 1,368 | 1,421 | 1.35× | Fused | Win |
| Uint64 (POD) | 3,268 | 1,626 | 739 | 2.01× | Fused | Win |
| Apache Builds (minified) | 1,716 | 1,495 | 1,222 | 1.15× | Fused | Win |
| Canada (minified) | 950 | 866 | 795 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,756 | 1,902 | 1,573 | 0.92× | Two-stage | Win |
| Canada (prettified) | 1,771 | 1,863 | 1,801 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 2,256 | 1,665 | 1,129 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 2,900 | 3,449 | 2,387 | 0.84× | Two-stage | Win |
| Discord (minified) | 2,221 | 1,723 | 965 | 1.29× | Fused | Win |
| Discord (prettified) | 1,739 | 2,543 | 1,396 | 0.68× | Two-stage | Win |
| Github Events (minified) | 1,600 | 1,789 | 975 | 0.89× | Two-stage | Win |
| Google Maps Response (minified) | 2,482 | 1,825 | 1,054 | 1.36× | Fused | Win |
| Github Events (prettified) | 1,537 | 1,909 | 1,277 | 0.81× | Two-stage | Win |
| Google Maps Response (prettified) | 2,867 | 3,429 | 2,147 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,434 | 1,910 | 761 | 1.27× | Fused | Win |
| Instruments (prettified) | 2,168 | 2,816 | 1,259 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 845 | 722 | 579 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,446 | 2,507 | 2,126 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 856 | 734 | 579 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,487 | 2,543 | 2,098 | 0.98× | Two-stage | Win |
| Random (minified) | 1,270 | 1,100 | 828 | 1.15× | Fused | Win |
| Random (prettified) | 1,415 | 1,687 | 1,254 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,890 | 1,756 | 934 | 1.08× | Fused | Win |
| Twitter (prettified) | 1,576 | 2,194 | 1,221 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 2,525 | 2,171 | 1,370 | 1.16× | Fused | Win |
| Canada Small (minified) | 861 | 780 | 772 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,165 | 2,626 | 1,752 | 0.82× | Two-stage | Win |
| Canada Small (prettified) | 1,632 | 1,782 | 1,740 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,632 | 1,373 | 984 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,774 | 2,331 | 1,723 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 1,952 | 1,744 | 765 | 1.12× | Fused | Win |
| Discord Small (prettified) | 1,612 | 2,299 | 1,043 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 2,044 | 1,793 | 1,359 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 2,221 | 1,681 | 1,065 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 1,806 | 2,202 | 1,721 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,601 | 3,189 | 2,157 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,493 | 1,865 | 662 | 1.34× | Fused | Win |
| Instruments Small (prettified) | 2,098 | 2,832 | 1,097 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 756 | 663 | 508 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,044 | 2,360 | 1,866 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 837 | 728 | 512 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,147 | 2,525 | 1,889 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,715 | 1,530 | 843 | 1.12× | Fused | Win |
| Random Small (prettified) | 1,672 | 2,309 | 1,291 | 0.72× | Two-stage | Win |
| Twitter Small (minified) | 2,374 | 2,152 | 1,005 | 1.10× | Fused | Win |
| Twitter Small (prettified) | 1,790 | 2,639 | 1,303 | 0.68× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,219 | 616 | 229 | 3.60× | Fused | Win |
| Double (POD) | 1,263 | 711 | 302 | 1.78× | Fused | Win |
| Int64 (POD) | 3,383 | 1,635 | 733 | 2.07× | Fused | Win |
| String (POD) | 1,808 | 1,316 | 1,299 | 1.37× | Fused | Win |
| Uint64 (POD) | 3,528 | 1,623 | 724 | 2.17× | Fused | Win |
| Apache Builds (minified) | 1,665 | 1,446 | 1,362 | 1.15× | Fused | Win |
| Canada (minified) | 939 | 800 | 775 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 1,791 | 1,833 | 1,807 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,913 | 1,879 | 1,736 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,359 | 1,812 | 1,452 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,322 | 3,622 | 3,009 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,297 | 1,827 | 1,326 | 1.26× | Fused | Win |
| Discord (prettified) | 2,234 | 2,542 | 1,905 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,671 | 1,533 | 1,423 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,505 | 1,772 | 1,104 | 1.41× | Fused | Win |
| Github Events (prettified) | 1,731 | 1,858 | 1,596 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 3,282 | 3,319 | 2,234 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,210 | 2,160 | 1,441 | 1.49× | Fused | Win |
| Instruments (prettified) | 2,939 | 3,368 | 2,329 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 836 | 715 | 531 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,424 | 2,542 | 1,958 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 845 | 720 | 535 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,480 | 2,558 | 1,961 | 0.97× | Two-stage | Win |
| Random (minified) | 1,197 | 1,090 | 977 | 1.10× | Fused | Win |
| Random (prettified) | 1,500 | 1,629 | 1,473 | 0.92× | Two-stage | Win |
| Twitter (minified) | 2,019 | 1,707 | 1,142 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,929 | 2,203 | 1,510 | 0.88× | Two-stage | Win |
| Apache Builds Small (minified) | 2,409 | 2,054 | 1,635 | 1.17× | Fused | Win |
| Canada Small (minified) | 930 | 799 | 678 | 1.16× | Fused | Win |
| Apache Builds Small (prettified) | 2,306 | 2,500 | 2,041 | 0.92× | Two-stage | Win |
| Canada Small (prettified) | 1,857 | 1,905 | 1,626 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,668 | 1,452 | 1,181 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 2,086 | 2,384 | 2,027 | 0.87× | Two-stage | Win |
| Discord Small (minified) | 1,993 | 1,667 | 1,225 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,925 | 2,173 | 1,620 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,100 | 1,750 | 1,559 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 2,223 | 1,630 | 1,081 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 1,992 | 2,133 | 1,945 | 0.93× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,990 | 3,111 | 2,224 | 0.96× | Two-stage | Win |
| Instruments Small (minified) | 3,234 | 2,113 | 1,301 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 2,827 | 3,345 | 2,112 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 849 | 741 | 500 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,175 | 2,494 | 1,794 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 924 | 773 | 492 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,321 | 2,665 | 1,830 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,558 | 1,471 | 1,042 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,910 | 2,211 | 1,592 | 0.86× | Two-stage | Win |
| Twitter Small (minified) | 2,870 | 2,255 | 1,359 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,442 | 2,853 | 1,766 | 0.86× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,778 | 355 | 153 | 5.00× | Fused | Win |
| Double (POD) | 816 | 399 | 133 | 2.04× | Fused | Win |
| Int64 (POD) | 2,128 | 801 | 410 | 2.66× | Fused | Win |
| String (POD) | 1,176 | 663 | 687 | 1.77× | Fused | Win |
| Uint64 (POD) | 2,042 | 803 | 396 | 2.54× | Fused | Win |
| Apache Builds (minified) | 1,079 | 844 | 643 | 1.28× | Fused | Win |
| Canada (minified) | 701 | 503 | 542 | 1.39× | Fused | Win |
| Apache Builds (prettified) | 1,150 | 1,160 | 901 | 0.99× | Tie | Win |
| Canada (prettified) | 1,099 (n/c) | 1,101 | 1,147 | 1.00× | Tie | Loss |
| CitmCatalog (minified) | 1,853 | 1,335 | 1,024 | 1.39× | Fused | Win |
| CitmCatalog (prettified) | 2,841 | 2,422 | 1,698 | 1.17× | Fused | Win |
| Discord (minified) | 1,862 | 1,026 | 1,008 | 1.82× | Fused | Win |
| Discord (prettified) | 1,685 | 1,614 | 1,251 | 1.04× | Fused | Win |
| Github Events (minified) | 1,427 | 1,332 | 950 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 1,820 | 1,167 | 707 | 1.56× | Fused | Win |
| Github Events (prettified) | 1,542 | 1,450 | 1,082 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,308 | 2,141 | 1,241 | 1.08× | Fused | Win |
| Instruments (minified) | 2,594 | 1,610 | 1,139 | 1.61× | Fused | Win |
| Instruments (prettified) | 2,882 | 2,455 | 1,734 | 1.17× | Fused | Win |
| Marine IK Reverse (minified) | 659 | 530 | 468 | 1.24× | Fused | Win |
| Marine IK Reverse (prettified) | 1,822 | 1,664 | 1,399 | 1.10× | Fused | Win |
| Marine IK (minified) | 606 | 475 | 390 | 1.28× | Fused | Win |
| Marine IK (prettified) | 1,888 | 1,760 | 1,461 | 1.07× | Fused | Win |
| Random (minified) | 963 | 893 | 650 | 1.08× | Fused | Win |
| Random (prettified) | 1,287 | 1,135 | 975 | 1.13× | Fused | Win |
| Twitter (minified) | 1,886 | 1,548 | 857 | 1.22× | Fused | Win |
| Twitter (prettified) | 1,568 | 1,520 | 1,128 | 1.03× | Fused | Win |
| Apache Builds Small (minified) | 1,390 | 1,368 | 812 | 1.02× | Tie | Win |
| Canada Small (minified) | 662 | 590 | 439 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 1,424 | 1,681 (n/c) | 1,055 | 0.85× | Two-stage | Win |
| Canada Small (prettified) | 1,345 | 1,301 | 972 | 1.03× | Fused | Win |
| CitmCatalog Small (minified) | 1,158 | 965 | 796 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 1,797 | 1,655 | 1,436 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,560 | 1,199 | 836 | 1.30× | Fused | Win |
| Discord Small (prettified) | 1,430 | 1,221 | 923 | 1.17× | Fused | Win |
| Github Events Small (minified) | 1,434 | 944 | 1,160 | 1.52× | Fused | Win |
| Google Maps Response Small (minified) | 1,737 | 1,115 | 616 | 1.56× | Fused | Win |
| Github Events Small (prettified) | 1,328 | 1,193 | 1,360 | 1.11× | Fused | Loss |
| Google Maps Response Small (prettified) | 2,552 | 2,209 | 1,412 | 1.16× | Fused | Win |
| Instruments Small (minified) | 2,413 | 1,708 | 941 | 1.41× | Fused | Win |
| Instruments Small (prettified) | 2,272 | 1,844 | 1,419 | 1.23× | Fused | Win |
| Marine IK Reverse Small (minified) | 572 | 470 | 360 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,463 | 1,642 | 1,326 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 663 | 476 | 341 | 1.39× | Fused | Win |
| Marine IK Small (prettified) | 1,626 | 1,715 | 1,237 | 0.95× | Two-stage | Win |
| Random Small (minified) | 1,106 | 1,046 | 635 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,506 | 1,284 | 1,028 | 1.17× | Fused | Win |
| Twitter Small (minified) | 2,227 | 1,775 | 1,086 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,187 | 2,150 | 1,372 | 1.02× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,173 | 291 | 163 | 4.04× | Fused | Win |
| Double (POD) | 729 | 301 | 198 | 2.42× | Fused | Win |
| Int64 (POD) | 2,055 | 821 | 429 | 2.50× | Fused | Win |
| String (POD) | 1,298 | 612 | 782 | 2.12× | Fused | Win |
| Uint64 (POD) | 2,155 | 769 | 476 | 2.80× | Fused | Win |
| Apache Builds (minified) | 1,557 | 1,099 | 789 | 1.42× | Fused | Win |
| Canada (minified) | 637 | 502 (n/c) | 506 (n/c) | 1.27× | Fused | Win |
| Apache Builds (prettified) | 1,627 | 1,391 | 1,114 | 1.17× | Fused | Win |
| Canada (prettified) | 1,259 | 1,152 | 962 | 1.09× | Fused | Win |
| CitmCatalog (minified) | 2,031 | 1,365 | 802 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 2,480 | 2,217 | 1,545 | 1.12× | Fused | Win |
| Discord (minified) | 2,180 | 1,026 | 658 | 2.12× | Fused | Win |
| Discord (prettified) | 2,017 | 1,526 | 946 | 1.32× | Fused | Win |
| Github Events (minified) | 1,592 | 1,486 | 882 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 2,167 | 1,197 | 641 | 1.81× | Fused | Win |
| Github Events (prettified) | 1,582 (n/c) | 1,634 | 1,055 | 0.97× | Tie | Win |
| Google Maps Response (prettified) | 2,548 | 1,794 | 1,486 | 1.42× | Fused | Win |
| Instruments (minified) | 2,415 | 1,418 | 619 | 1.70× | Fused | Win |
| Instruments (prettified) | 2,708 | 2,382 | 999 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 624 | 538 | 390 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 1,939 | 1,795 (n/c) | 1,235 (n/c) | 1.08× | Fused | Win |
| Marine IK (minified) | 656 | 581 | 390 (n/c) | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,005 | 1,819 | 1,323 | 1.10× | Fused | Win |
| Random (minified) | 984 | 833 | 576 | 1.18× | Fused | Win |
| Random (prettified) | 1,625 | 1,248 | 992 | 1.30× | Fused | Win |
| Twitter (minified) | 1,777 | 1,637 | 811 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,838 | 2,047 (n/c) | 955 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 1,327 | 1,026 | 749 | 1.29× | Fused | Win |
| Canada Small (minified) | 585 | 588 | 410 | 1.00× | Tie | Win |
| Apache Builds Small (prettified) | 1,530 | 1,367 | 991 | 1.12× | Fused | Win |
| Canada Small (prettified) | 1,158 | 1,091 | 831 | 1.06× | Fused | Win |
| CitmCatalog Small (minified) | 1,074 | 894 | 658 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 1,448 | 1,246 | 1,129 | 1.16× | Fused | Win |
| Discord Small (minified) | 2,075 | 1,159 | 620 | 1.79× | Fused | Win |
| Discord Small (prettified) | 2,116 | 1,641 | 871 | 1.29× | Fused | Win |
| Github Events Small (minified) | 1,655 | 1,210 | 890 | 1.37× | Fused | Win |
| Google Maps Response Small (minified) | 1,994 | 1,145 | 718 | 1.74× | Fused | Win |
| Github Events Small (prettified) | 1,646 | 1,467 | 1,285 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,467 | 1,882 | 1,230 | 1.31× | Fused | Win |
| Instruments Small (minified) | 2,712 | 1,381 | 508 | 1.96× | Fused | Win |
| Instruments Small (prettified) | 2,848 | 2,498 | 821 | 1.14× | Fused | Win |
| Marine IK Reverse Small (minified) | 493 | 440 | 315 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,483 | 1,435 | 1,081 | 1.03× | Tie | Win |
| Marine IK Small (minified) | 676 | 441 | 289 | 1.53× | Fused | Win |
| Marine IK Small (prettified) | 1,682 | 1,662 | 1,072 | 1.01× | Fused | Win |
| Random Small (minified) | 1,148 | 1,036 | 639 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,317 | 1,329 | 966 | 0.99× | Tie | Win |
| Twitter Small (minified) | 2,139 | 1,581 | 793 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 1,807 | 2,168 | 1,058 | 0.83× | Two-stage | Win |

#### Reused objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 752 | 278 | 127 | 2.71× | Fused | Win |
| Double (POD) | 1,011 | 636 | 282 | 1.59× | Fused | Win |
| Int64 (POD) | 1,954 | 988 | 618 | 1.98× | Fused | Win |
| String (POD) | 1,299 | 986 | 950 | 1.32× | Fused | Win |
| Uint64 (POD) | 1,894 | 1,028 | 598 | 1.84× | Fused | Win |
| Apache Builds (minified) | 1,253 | 1,134 | 867 | 1.11× | Fused | Win |
| Canada (minified) | 628 | 653 | 412 | 0.96× | Two-stage | Win |
| Apache Builds (prettified) | 1,426 | 1,451 | 1,126 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,321 | 1,427 | 940 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,788 | 1,440 | 607 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,617 | 2,920 | 1,324 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,776 | 1,351 | 901 | 1.31× | Fused | Win |
| Discord (prettified) | 1,776 | 1,941 | 1,306 | 0.91× | Two-stage | Win |
| Github Events (minified) | 1,382 | 1,222 | 846 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 1,547 | 1,316 | 614 | 1.18× | Fused | Win |
| Github Events (prettified) | 1,373 | 1,444 | 1,028 | 0.95× | Two-stage | Win |
| Google Maps Response (prettified) | 2,445 | 2,597 | 1,248 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,624 | 1,755 | 945 | 1.50× | Fused | Win |
| Instruments (prettified) | 2,441 | 2,630 | 1,540 | 0.93× | Two-stage | Win |
| Marine IK Reverse (minified) | 679 | 593 | 187 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,042 | 2,097 | 658 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 681 | 610 | 323 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,116 | 2,158 | 1,221 | 0.98× | Two-stage | Win |
| Random (minified) | 967 | 862 | 614 | 1.12× | Fused | Win |
| Random (prettified) | 1,180 | 1,327 | 933 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,938 | 1,532 | 1,167 | 1.27× | Fused | Win |
| Twitter (prettified) | 1,785 | 1,932 | 1,505 | 0.92× | Two-stage | Win |
| Apache Builds Small (minified) | 1,491 | 1,454 | 1,042 | 1.03× | Fused | Win |
| Canada Small (minified) | 613 | 605 | 344 | 1.01× | Fused | Win |
| Apache Builds Small (prettified) | 1,538 | 1,757 | 1,298 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 1,275 | 1,400 | 821 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,076 | 864 | 595 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 1,362 | 1,499 | 1,054 | 0.91× | Two-stage | Win |
| Discord Small (minified) | 1,550 | 1,081 | 931 | 1.43× | Fused | Win |
| Discord Small (prettified) | 1,476 | 1,577 | 1,254 | 0.94× | Two-stage | Win |
| Github Events Small (minified) | 1,468 | 1,197 | 754 | 1.23× | Fused | Win |
| Google Maps Response Small (minified) | 1,399 | 1,175 | 613 | 1.19× | Fused | Win |
| Github Events Small (prettified) | 1,252 | 1,473 | 1,032 | 0.85× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,182 | 2,337 | 1,261 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,339 | 1,573 | 915 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,173 | 2,415 | 1,483 | 0.90× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 514 | 475 | 152 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,443 | 1,740 | 586 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 607 | 522 | 293 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 1,672 | 1,879 | 1,109 | 0.89× | Two-stage | Win |
| Random Small (minified) | 994 | 867 | 579 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,188 | 1,343 | 888 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,085 | 1,508 | 1,426 | 1.38× | Fused | Win |
| Twitter Small (prettified) | 1,779 | 1,955 | 1,848 | 0.91× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,212 | 643 | 290 | 3.44× | Fused | Win |
| Double (POD) | 1,390 | 782 | 370 | 1.78× | Fused | Win |
| Int64 (POD) | 3,445 | 1,763 | 802 | 1.95× | Fused | Win |
| String (POD) | 2,870 | 1,820 | 1,972 | 1.58× | Fused | Win |
| Uint64 (POD) | 3,593 | 1,676 | 734 | 2.14× | Fused | Win |
| Apache Builds (minified) | 2,795 | 2,296 | 1,683 | 1.22× | Fused | Win |
| Canada (minified) | 1,139 | 1,013 | 959 | 1.12× | Fused | Win |
| Apache Builds (prettified) | 2,505 | 2,868 | 2,132 | 0.87× | Two-stage | Win |
| Canada (prettified) | 2,037 | 2,169 | 2,124 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,800 | 1,945 | 1,295 | 1.44× | Fused | Win |
| CitmCatalog (prettified) | 3,251 | 4,009 | 2,714 | 0.81× | Two-stage | Win |
| Discord (minified) | 3,174 | 2,418 | 1,120 | 1.31× | Fused | Win |
| Discord (prettified) | 2,130 | 3,384 | 1,629 | 0.63× | Two-stage | Win |
| Github Events (minified) | 2,194 | 2,083 | 1,142 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,826 | 2,008 | 1,120 | 1.41× | Fused | Win |
| Github Events (prettified) | 1,947 | 2,511 | 1,367 | 0.78× | Two-stage | Win |
| Google Maps Response (prettified) | 3,070 | 3,709 | 2,281 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,856 | 2,147 | 790 | 1.33× | Fused | Win |
| Instruments (prettified) | 2,348 | 3,129 | 1,313 | 0.75× | Two-stage | Win |
| Marine IK Reverse (minified) | 938 | 796 | 636 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,640 | 2,727 | 2,323 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 962 | 813 | 639 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,710 | 2,772 | 2,306 | 0.98× | Two-stage | Win |
| Random (minified) | 1,794 | 1,494 | 1,028 | 1.20× | Fused | Win |
| Random (prettified) | 1,791 | 2,260 | 1,559 | 0.79× | Two-stage | Win |
| Twitter (minified) | 2,395 | 2,324 | 1,044 | 1.03× | Fused | Win |
| Twitter (prettified) | 1,816 | 2,736 | 1,381 | 0.66× | Two-stage | Win |
| Apache Builds Small (minified) | 2,940 | 2,481 | 1,635 | 1.19× | Fused | Win |
| Canada Small (minified) | 1,038 | 938 | 879 | 1.11× | Fused | Win |
| Apache Builds Small (prettified) | 2,368 | 3,038 | 2,059 | 0.78× | Two-stage | Win |
| Canada Small (prettified) | 1,965 | 2,166 | 2,098 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,036 | 1,620 | 1,119 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 2,009 | 2,717 | 1,950 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,669 | 2,163 | 879 | 1.23× | Fused | Win |
| Discord Small (prettified) | 1,935 | 2,840 | 1,201 | 0.68× | Two-stage | Win |
| Github Events Small (minified) | 2,510 | 2,194 | 1,578 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 2,634 | 1,872 | 1,142 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 2,176 | 2,718 | 1,999 | 0.80× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,825 | 3,538 | 2,318 | 0.80× | Two-stage | Win |
| Instruments Small (minified) | 2,679 | 1,975 | 691 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 2,170 | 3,036 | 1,148 | 0.71× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 938 | 795 | 566 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,338 | 2,747 | 2,079 | 0.85× | Two-stage | Win |
| Marine IK Small (minified) | 1,031 | 869 | 561 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 2,448 | 2,968 | 2,060 | 0.82× | Two-stage | Win |
| Random Small (minified) | 2,147 | 1,879 | 1,006 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,961 | 2,803 | 1,504 | 0.70× | Two-stage | Win |
| Twitter Small (minified) | 2,641 | 2,353 | 1,047 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 1,904 | 2,983 | 1,352 | 0.64× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,602 | 620 | 224 | 4.20× | Fused | Win |
| Double (POD) | 1,402 | 760 | 306 | 1.84× | Fused | Win |
| Int64 (POD) | 3,784 | 1,763 | 749 | 2.15× | Fused | Win |
| String (POD) | 2,839 | 1,738 | 1,690 | 1.63× | Fused | Win |
| Uint64 (POD) | 3,884 | 1,703 | 736 | 2.28× | Fused | Win |
| Apache Builds (minified) | 2,552 | 2,067 | 1,925 | 1.23× | Fused | Win |
| Canada (minified) | 1,113 | 921 | 928 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 2,384 | 2,626 | 2,436 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,218 | 2,160 | 2,044 | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,940 | 2,145 | 1,765 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 3,856 | 4,222 | 3,597 | 0.91× | Two-stage | Win |
| Discord (minified) | 3,231 | 2,408 | 1,618 | 1.34× | Fused | Win |
| Discord (prettified) | 2,773 | 3,278 | 2,326 | 0.85× | Two-stage | Win |
| Github Events (minified) | 2,269 | 2,043 | 1,627 | 1.11× | Fused | Win |
| Google Maps Response (minified) | 2,839 | 1,964 | 1,171 | 1.45× | Fused | Win |
| Github Events (prettified) | 2,265 | 2,371 | 1,940 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 3,498 | 3,542 | 2,358 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,814 | 2,429 | 1,545 | 1.57× | Fused | Win |
| Instruments (prettified) | 3,204 | 3,755 | 2,490 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 922 | 788 | 578 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,638 | 2,777 | 2,080 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 943 | 803 | 583 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,708 | 2,801 | 2,141 | 0.97× | Two-stage | Win |
| Random (minified) | 1,628 | 1,460 | 1,283 | 1.12× | Fused | Win |
| Random (prettified) | 1,899 | 2,152 | 1,959 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,607 | 2,171 | 1,335 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,316 | 2,732 | 1,763 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 2,856 | 2,367 | 1,956 | 1.21× | Fused | Win |
| Canada Small (minified) | 1,141 | 948 | 750 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 2,612 | 2,867 | 2,467 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 2,203 | 2,238 | 1,777 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,101 | 1,655 | 1,397 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,409 | 2,822 | 2,432 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,636 | 2,015 | 1,517 | 1.31× | Fused | Win |
| Discord Small (prettified) | 2,285 | 2,640 | 2,078 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,542 | 2,094 | 1,815 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,608 | 1,823 | 1,183 | 1.43× | Fused | Win |
| Github Events Small (prettified) | 2,398 | 2,576 | 2,243 | 0.93× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,274 | 3,404 | 2,392 | 0.96× | Two-stage | Win |
| Instruments Small (minified) | 3,556 | 2,289 | 1,414 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 3,007 | 3,486 | 2,296 | 0.86× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,041 | 882 | 547 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,457 | 2,933 | 1,981 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 1,133 | 925 | 538 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,622 | 3,136 | 1,971 | 0.84× | Two-stage | Win |
| Random Small (minified) | 1,893 | 1,773 | 1,275 | 1.07× | Fused | Win |
| Random Small (prettified) | 2,242 | 2,628 | 1,923 | 0.85× | Two-stage | Win |
| Twitter Small (minified) | 3,132 | 2,499 | 1,444 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,663 | 3,142 | 1,844 | 0.85× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,638 | 280 | 163 | 5.86× | Fused | Win |
| Double (POD) | 1,126 | 423 | 143 | 2.66× | Fused | Win |
| Int64 (POD) | 3,283 | 888 | 471 | 3.70× | Fused | Win |
| String (POD) | 2,398 | 987 | 1,007 | 2.43× | Fused | Win |
| Uint64 (POD) | 2,462 | 754 | 419 | 3.27× | Fused | Win |
| Apache Builds (minified) | 2,137 | 1,241 | 875 | 1.72× | Fused | Win |
| Canada (minified) | 927 | 690 | 611 | 1.34× | Fused | Win |
| Apache Builds (prettified) | 1,811 | 1,665 | 1,116 | 1.09× | Fused | Win |
| Canada (prettified) | 1,452 | 1,412 | 1,300 | 1.03× | Fused | Win |
| CitmCatalog (minified) | 2,295 | 1,448 | 1,301 | 1.58× | Fused | Win |
| CitmCatalog (prettified) | 3,241 | 2,774 | 2,276 | 1.17× | Fused | Win |
| Discord (minified) | 2,346 | 1,621 | 1,084 | 1.45× | Fused | Win |
| Discord (prettified) | 2,532 | 1,855 | 1,456 | 1.37× | Fused | Win |
| Github Events (minified) | 2,114 | 1,456 | 1,109 | 1.45× | Fused | Win |
| Google Maps Response (minified) | 2,080 | 1,314 | 693 | 1.58× | Fused | Win |
| Github Events (prettified) | 2,065 | 1,843 | 1,296 | 1.12× | Fused | Win |
| Google Maps Response (prettified) | 2,748 | 2,369 | 1,357 | 1.16× | Fused | Win |
| Instruments (minified) | 2,844 | 1,789 | 1,215 | 1.59× | Fused | Win |
| Instruments (prettified) | 3,213 | 2,599 | 1,839 | 1.24× | Fused | Win |
| Marine IK Reverse (minified) | 756 | 591 | 518 | 1.28× | Fused | Win |
| Marine IK Reverse (prettified) | 1,782 | 1,800 | 1,656 | 0.99× | Tie | Win |
| Marine IK (minified) | 712 | 583 | 465 | 1.22× | Fused | Win |
| Marine IK (prettified) | 2,115 | 1,925 | 1,566 | 1.10× | Fused | Win |
| Random (minified) | 1,416 | 1,301 | 812 | 1.09× | Fused | Win |
| Random (prettified) | 1,761 | 1,691 | 1,190 | 1.04× | Fused | Win |
| Twitter (minified) | 2,518 | 1,754 | 953 | 1.44× | Fused | Win |
| Twitter (prettified) | 2,070 | 1,932 | 1,185 (n/c) | 1.07× | Fused | Win |
| Apache Builds Small (minified) | 2,043 | 1,818 | 1,085 | 1.12× | Fused | Win |
| Canada Small (minified) | 815 | 726 | 481 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 1,878 | 2,097 | 1,176 | 0.90× | Two-stage | Win |
| Canada Small (prettified) | 1,593 | 1,584 | 1,251 | 1.01× | Tie | Win |
| CitmCatalog Small (minified) | 1,735 | 1,090 | 1,018 | 1.59× | Fused | Win |
| CitmCatalog Small (prettified) | 2,280 | 2,138 | 1,421 (n/c) | 1.07× | Fused | Win |
| Discord Small (minified) | 2,217 | 1,596 | 1,074 | 1.39× | Fused | Win |
| Discord Small (prettified) | 1,919 | 1,754 | 1,208 | 1.09× | Fused | Win |
| Github Events Small (minified) | 1,936 | 1,448 | 1,432 | 1.34× | Fused | Win |
| Google Maps Response Small (minified) | 2,101 | 1,505 | 860 | 1.40× | Fused | Win |
| Github Events Small (prettified) | 1,862 | 1,868 | 1,548 | 1.00× | Tie | Win |
| Google Maps Response Small (prettified) | 2,235 | 2,540 | 1,556 | 0.88× | Two-stage | Win |
| Instruments Small (minified) | 3,166 | 1,748 | 1,075 | 1.81× | Fused | Win |
| Instruments Small (prettified) | 2,644 | 2,433 | 1,903 | 1.09× | Fused | Win |
| Marine IK Reverse Small (minified) | 686 | 573 | 407 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,836 | 1,950 | 1,396 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 826 | 595 (n/c) | 390 | 1.39× | Fused | Win |
| Marine IK Small (prettified) | 1,879 | 2,186 | 1,141 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,403 | 1,343 | 854 | 1.05× | Fused | Win |
| Random Small (prettified) | 1,741 | 1,715 | 1,251 | 1.02× | Fused | Win |
| Twitter Small (minified) | 2,551 | 2,166 | 1,057 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,566 | 2,359 | 1,500 | 1.09× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,250 | 298 | 146 | 4.19× | Fused | Win |
| Double (POD) | 902 | 335 | 207 | 2.70× | Fused | Win |
| Int64 (POD) | 2,560 | 827 | 438 | 3.09× | Fused | Win |
| String (POD) | 2,145 | 890 | 1,064 | 2.41× | Fused | Win |
| Uint64 (POD) | 2,427 | 842 | 445 | 2.88× | Fused | Win |
| Apache Builds (minified) | 2,314 | 1,500 | 1,099 | 1.54× | Fused | Win |
| Canada (minified) | 779 | 709 | 611 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 2,044 | 1,844 | 1,397 | 1.11× | Fused | Win |
| Canada (prettified) | 1,596 | 1,401 | 1,174 | 1.14× | Fused | Win |
| CitmCatalog (minified) | 2,446 | 1,592 | 861 | 1.54× | Fused | Win |
| CitmCatalog (prettified) | 2,909 | 2,798 | 1,685 | 1.04× | Fused | Win |
| Discord (minified) | 2,865 | 1,417 | 729 | 2.02× | Fused | Win |
| Discord (prettified) | 2,459 | 1,911 | 985 | 1.29× | Fused | Win |
| Github Events (minified) | 2,107 | 1,773 | 891 | 1.19× | Fused | Win |
| Google Maps Response (minified) | 2,180 | 1,233 | 849 | 1.77× | Fused | Win |
| Github Events (prettified) | 1,911 | 2,065 | 974 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 2,291 | 1,960 | 1,627 | 1.17× | Fused | Win |
| Instruments (minified) | 2,542 | 1,643 | 708 | 1.55× | Fused | Win |
| Instruments (prettified) | 2,590 | 2,471 | 1,055 | 1.05× | Fused | Win |
| Marine IK Reverse (minified) | 718 | 600 | 412 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,301 | 2,005 | 1,377 (n/c) | 1.15× | Fused | Win |
| Marine IK (minified) | 812 | 644 | 381 | 1.26× | Fused | Win |
| Marine IK (prettified) | 2,191 | 2,053 | 1,429 | 1.07× | Fused | Win |
| Random (minified) | 1,546 | 1,229 | 646 | 1.26× | Fused | Win |
| Random (prettified) | 2,053 | 1,790 | 1,143 | 1.15× | Fused | Win |
| Twitter (minified) | 2,647 | 2,152 | 874 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,156 | 2,563 | 892 | 0.84× | Two-stage | Win |
| Apache Builds Small (minified) | 1,691 | 1,363 | 1,176 | 1.24× | Fused | Win |
| Canada Small (minified) | 717 | 649 | 455 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,060 | 1,976 | 1,461 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,280 | 1,327 | 1,006 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,856 | 1,242 | 770 | 1.49× | Fused | Win |
| CitmCatalog Small (prettified) | 2,094 | 1,970 | 1,437 | 1.06× | Fused | Win |
| Discord Small (minified) | 2,504 | 1,706 | 686 | 1.47× | Fused | Win |
| Discord Small (prettified) | 2,508 | 2,159 | 995 | 1.16× | Fused | Win |
| Github Events Small (minified) | 2,207 | 1,781 | 1,307 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,208 | 1,391 | 733 | 1.59× | Fused | Win |
| Github Events Small (prettified) | 2,094 | 1,982 | 1,306 | 1.06× | Fused | Win |
| Google Maps Response Small (prettified) | 2,959 | 2,405 | 1,454 | 1.23× | Fused | Win |
| Instruments Small (minified) | 2,500 (n/c) | 1,607 | 498 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 3,117 | 2,780 | 1,048 | 1.12× | Fused | Win |
| Marine IK Reverse Small (minified) | 661 | 561 | 359 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,234 | 1,924 | 1,249 | 1.16× | Fused | Win |
| Marine IK Small (minified) | 888 | 541 | 344 | 1.64× | Fused | Win |
| Marine IK Small (prettified) | 2,231 | 2,149 | 1,275 | 1.04× | Fused | Win |
| Random Small (minified) | 1,896 | 1,355 | 716 | 1.40× | Fused | Win |
| Random Small (prettified) | 2,017 | 1,575 | 1,070 | 1.28× | Fused | Win |
| Twitter Small (minified) | 2,934 | 2,117 | 872 | 1.39× | Fused | Win |
| Twitter Small (prettified) | 2,020 | 2,355 | 1,117 | 0.86× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and 63 of the 66 minified documents, full-size and Small, the other three going to the tape (two under MSVC, one on Linux/Clang), while the tape wins 31 of the 33 full-size prettified documents and all 33 Small prettified ones. The two prettified exceptions are Marine IK under MSVC and Canada on Linux/GCC, which the fused path wins. On the M2 Pro the fused path wins all POD input, 42 of the 44 minified documents (2 ties) and 33 of the 44 prettified documents (5 to the tape, 6 ties). §2.8 repeats the comparison on AVX-512.

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.19× (String, Windows/MSVC) to 5.00× (Bool, macOS/GCC) faster than the two-stage path, and 1.32× (String, Windows/MSVC) to 5.86× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, except for three, two of them under MSVC.** Freshly allocated, the fused path wins 105 of the 110 minified documents, ties 2 and loses 3 to the tape. Two of the losses are under MSVC: Canada (544 against 562 MB/s, 0.97×) and Apache Builds Small (1,119 against 1,159, 0.97×). The third is Github Events on Linux/Clang (1,600 against 1,789, 0.89×), and the ties are Apache Builds Small on macOS/GCC (1,390 against 1,368) and Canada Small on macOS/Clang (585 against 588). Its margins run from 6% (Apache Builds under MSVC, 816 against 770 MB/s) to 2.12× (Discord on macOS/Clang, 2,180 against 1,026 MB/s) on the full-size documents and from under 1% (Canada Small under MSVC, 505 against 503) to 1.96× (Instruments Small on macOS/Clang, 2,712 against 1,381 MB/s) on the Small ones. With objects reused it wins 109 and loses 1, minified Canada under MSVC (628 against 653 MB/s). Why the tape wins these has not been profiled. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 64 of 66.** It wins all 22 on Linux/Clang, 21 of 22 on Linux/GCC, where the fused path wins prettified Canada (1,913 MB/s against 1,879), and 21 of 22 under MSVC, where the fused path wins prettified Marine IK (1,955 against 1,933 MB/s). The tape's margins run from 1% (Google Maps Response on Linux/GCC, 3,319 against 3,282 MB/s) to 46% (Discord on Linux/Clang, 2,543 against 1,739) on the full-size documents and from 3% (Canada Small on Linux/GCC) to 47% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 65 of the 66 prettified x86 documents; the fused path takes prettified Canada on Linux/GCC (2,218 against 2,160 MB/s).

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 33, the tape wins 5 and 6 are ties. Of the 22 full-size documents the fused path wins 18, the tape takes Twitter on macOS/Clang (0.90×), and Apache Builds and Canada on macOS/GCC and Github Events on macOS/Clang are ties. On the Small copies the tape wins Apache Builds Small, Marine IK Reverse Small and Marine IK Small on macOS/GCC (0.85×, 0.89× and 0.95×) and Twitter Small on macOS/Clang (0.83×); Twitter Small on macOS/GCC and Marine IK Reverse Small and Random Small on macOS/Clang are ties. By compiler, the fused path wins 16 of 22 on macOS/GCC (3 to the tape, 3 ties) and 17 of 22 on macOS/Clang (2 to the tape, 3 ties). Reused, the fused path wins 33, the tape 8 and 3 are ties: 15, 4 and 3 on macOS/GCC and 18, 4 and none on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Three documents beat both Jsonifier paths, freshly allocated.** Against simdjson On Demand the faster Jsonifier path wins every test in both runs except three freshly allocated prettified ones: Twitter Small under MSVC (simdjson 1,658 MB/s against the tape's 1,634 and the fused path's 1,525, 0.99×), Canada on macOS/GCC (1,147 against the tape's 1,101, 0.96×) and Github Events Small on macOS/GCC (1,360 against the fused path's 1,328, 0.98×); reused, Jsonifier wins all three. The fused path alone also loses freshly allocated prettified Canada and Canada Small on Linux/Clang (1,771 against 1,801 and 1,632 against 1,740 MB/s) and prettified Apache Builds on Linux/GCC (1,791 against 1,807); the tape wins all three, at 1,863, 1,782 and 1,833 MB/s. With objects reused the fused path loses six prettified documents: Twitter Small under MSVC (1,779 against 1,848, 0.96×), Canada and Canada Small on Linux/Clang, and Apache Builds, Random and CitmCatalog Small on Linux/GCC (2,409 against 2,432 for the last), and the tape wins all six. We have not profiled what sets these documents apart.

**MSVC slows all three by about the same median.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 58% of its Linux/Clang throughput, the two-stage path 62% and simdjson 57%. The spread between documents is narrowest for the two-stage path (from 48% on Github Events to 76% on Marine IK), wider for the fused path (from 48% on Apache Builds to 82% on Instruments) and widest for simdjson, which keeps 47% of its Linux/Clang throughput on Canada and more than all of it (110%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 242 of 245 tests with whichever path is faster, and loses the other three.** The losses are prettified Twitter Small under MSVC and prettified Canada and Github Events Small on macOS/GCC. The fused path alone wins 239 and loses 6: those three and prettified Canada and Canada Small on Linux/Clang and Apache Builds on Linux/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 181 MB/s, against 302 MB/s for the same document in declared order, so the faster Jsonifier path is 3.33× faster than simdjson freshly allocated (3.63× reused). The prettified form shows the same gap, at 648 MB/s against 1,113, and the faster Jsonifier path is 3.03× faster (3.19× reused). The Small copies show it too, at 144 MB/s minified and 554 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document within about 20% of the declared one, sometimes faster, and the faster Jsonifier path is 1.18× to 1.60× faster than simdjson on the full-size reversed document, freshly allocated, and 1.09× to 1.74× reused. Why the rescan cost shows up mostly under MSVC has not been profiled; §2.8 shows it again, wider, on the AVX-512 MSVC build.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path, and under MSVC so do two minified documents; on the M2 Pro, minified input belongs to the fused path, and so do 33 of the 44 prettified documents. Routing each test to its faster path would raise the record against simdjson from 239 wins and 6 losses to 242 wins and 3 losses. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,446 | 1,408 | 1.03× | Win |
| Canada (minified) | 800 | 829 | 0.97× | **Loss** |
| Apache Builds (prettified) | 1,833 | 1,877 | 0.98× | **Loss** |
| Canada (prettified) | 1,879 | 1,859 | 1.01× | Win |
| CitmCatalog (minified) | 1,812 | 1,447 | 1.25× | Win |
| CitmCatalog (prettified) | 3,622 | 3,016 | 1.20× | Win |
| Discord (minified) | 1,827 | 1,572 | 1.16× | Win |
| Discord (prettified) | 2,542 | 2,240 | 1.13× | Win |
| Github Events (minified) | 1,533 | 1,565 | 0.98× | **Loss** |
| Google Maps Response (minified) | 1,772 | 1,498 | 1.18× | Win |
| Github Events (prettified) | 1,858 | 1,883 | 0.99× | **Loss** |
| Google Maps Response (prettified) | 3,319 | 2,932 | 1.13× | Win |
| Instruments (minified) | 2,160 | 1,665 | 1.30× | Win |
| Instruments (prettified) | 3,368 | 2,686 | 1.25× | Win |
| Marine IK Reverse (minified) | 715 | 620 | 1.15× | Win |
| Marine IK Reverse (prettified) | 2,542 | 2,246 | 1.13× | Win |
| Marine IK (minified) | 720 | 620 | 1.16× | Win |
| Marine IK (prettified) | 2,558 | 2,252 | 1.14× | Win |
| Random (minified) | 1,090 | 966 | 1.13× | Win |
| Random (prettified) | 1,629 | 1,471 | 1.11× | Win |
| Twitter (minified) | 1,707 | 1,587 | 1.08× | Win |
| Twitter (prettified) | 2,203 | 2,089 | 1.05× | Win |
| Apache Builds Small (minified) | 2,054 | 1,752 | 1.17× | Win |
| Canada Small (minified) | 799 | 770 | 1.04× | Win |
| Apache Builds Small (prettified) | 2,500 | 2,196 | 1.14× | Win |
| Canada Small (prettified) | 1,905 | 1,838 | 1.04× | Win |
| CitmCatalog Small (minified) | 1,452 | 1,200 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,384 | 2,074 | 1.15× | Win |
| Discord Small (minified) | 1,667 | 1,505 | 1.11× | Win |
| Discord Small (prettified) | 2,173 | 2,010 | 1.08× | Win |
| Github Events Small (minified) | 1,750 | 1,821 | 0.96× | **Loss** |
| Google Maps Response Small (minified) | 1,630 | 1,433 | 1.14× | Win |
| Github Events Small (prettified) | 2,133 | 2,250 | 0.95× | **Loss** |
| Google Maps Response Small (prettified) | 3,111 | 2,821 | 1.10× | Win |
| Instruments Small (minified) | 2,113 | 1,645 | 1.28× | Win |
| Instruments Small (prettified) | 3,345 | 2,660 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 741 | 621 | 1.19× | Win |
| Marine IK Reverse Small (prettified) | 2,494 | 2,292 | 1.09× | Win |
| Marine IK Small (minified) | 773 | 623 | 1.24× | Win |
| Marine IK Small (prettified) | 2,665 | 2,287 | 1.17× | Win |
| Random Small (minified) | 1,471 | 1,060 | 1.39× | Win |
| Random Small (prettified) | 2,211 | 1,604 | 1.38× | Win |
| Twitter Small (minified) | 2,255 | 1,998 | 1.13× | Win |
| Twitter Small (prettified) | 2,853 | 2,489 | 1.15× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 844 | 751 | 1.12× | Win |
| Canada (minified) | 503 | 575 | 0.88× | **Loss** |
| Apache Builds (prettified) | 1,160 | 1,061 | 1.09× | Win |
| Canada (prettified) | 1,101 | 1,395 | 0.79× | **Loss** |
| CitmCatalog (minified) | 1,335 | 1,129 | 1.18× | Win |
| CitmCatalog (prettified) | 2,422 | 2,042 | 1.19× | Win |
| Discord (minified) | 1,026 | 1,029 | 1.00× | Tie |
| Discord (prettified) | 1,614 | 1,373 | 1.18× | Win |
| Github Events (minified) | 1,332 | 1,121 | 1.19× | Win |
| Google Maps Response (minified) | 1,167 | 1,018 | 1.15× | Win |
| Github Events (prettified) | 1,450 | 1,230 | 1.18× | Win |
| Google Maps Response (prettified) | 2,141 | 1,899 | 1.13× | Win |
| Instruments (minified) | 1,610 | 1,262 | 1.28× | Win |
| Instruments (prettified) | 2,455 | 1,919 | 1.28× | Win |
| Marine IK Reverse (minified) | 530 | 523 | 1.01× | Win |
| Marine IK Reverse (prettified) | 1,664 | 1,475 | 1.13× | Win |
| Marine IK (minified) | 475 | 487 | 0.98× | **Loss** |
| Marine IK (prettified) | 1,760 | 1,566 | 1.12× | Win |
| Random (minified) | 893 | 742 | 1.20× | Win |
| Random (prettified) | 1,135 | 1,111 | 1.02× | Win |
| Twitter (minified) | 1,548 | 1,090 | 1.42× | Win |
| Twitter (prettified) | 1,520 | 1,397 | 1.09× | Win |
| Apache Builds Small (minified) | 1,368 | 1,057 | 1.29× | Win |
| Canada Small (minified) | 590 | 518 | 1.14× | Win |
| Apache Builds Small (prettified) | 1,681 (n/c) | 1,132 | 1.48× | Win |
| Canada Small (prettified) | 1,301 | 1,254 | 1.04× | Win |
| CitmCatalog Small (minified) | 965 | 824 | 1.17× | Win |
| CitmCatalog Small (prettified) | 1,655 | 1,296 | 1.28× | Win |
| Discord Small (minified) | 1,199 | 983 | 1.22× | Win |
| Discord Small (prettified) | 1,221 | 1,301 | 0.94× | **Loss** |
| Github Events Small (minified) | 944 | 906 | 1.04× | Win |
| Google Maps Response Small (minified) | 1,115 | 1,025 | 1.09× | Win |
| Github Events Small (prettified) | 1,193 | 1,310 | 0.91× | **Loss** |
| Google Maps Response Small (prettified) | 2,209 | 1,602 | 1.38× | Win |
| Instruments Small (minified) | 1,708 | 1,048 | 1.63× | Win |
| Instruments Small (prettified) | 1,844 | 1,643 | 1.12× | Win |
| Marine IK Reverse Small (minified) | 470 | 482 | 0.97× | **Loss** |
| Marine IK Reverse Small (prettified) | 1,642 | 1,514 | 1.08× | Win |
| Marine IK Small (minified) | 476 | 448 | 1.06× | Win |
| Marine IK Small (prettified) | 1,715 | 1,622 | 1.06× | Win |
| Random Small (minified) | 1,046 | 810 | 1.29× | Win |
| Random Small (prettified) | 1,284 | 1,185 | 1.08× | Win |
| Twitter Small (minified) | 1,775 | 1,150 | 1.54× | Win |
| Twitter Small (prettified) | 2,150 | 1,476 | 1.46× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,067 | 1,827 | 1.13× | Win |
| Canada (minified) | 921 | 971 | 0.95× | **Loss** |
| Apache Builds (prettified) | 2,626 | 2,333 | 1.13× | Win |
| Canada (prettified) | 2,160 | 2,145 | 1.01× | Win |
| CitmCatalog (minified) | 2,145 | 1,715 | 1.25× | Win |
| CitmCatalog (prettified) | 4,222 | 3,493 | 1.21× | Win |
| Discord (minified) | 2,408 | 1,798 | 1.34× | Win |
| Discord (prettified) | 3,278 | 2,558 | 1.28× | Win |
| Github Events (minified) | 2,043 | 1,898 | 1.08× | Win |
| Google Maps Response (minified) | 1,964 | 1,607 | 1.22× | Win |
| Github Events (prettified) | 2,371 | 2,235 | 1.06× | Win |
| Google Maps Response (prettified) | 3,542 | 3,165 | 1.12× | Win |
| Instruments (minified) | 2,429 | 1,708 | 1.42× | Win |
| Instruments (prettified) | 3,755 | 2,766 | 1.36× | Win |
| Marine IK Reverse (minified) | 788 | 686 | 1.15× | Win |
| Marine IK Reverse (prettified) | 2,777 | 2,431 | 1.14× | Win |
| Marine IK (minified) | 803 | 691 | 1.16× | Win |
| Marine IK (prettified) | 2,801 | 2,483 | 1.13× | Win |
| Random (minified) | 1,460 | 1,195 | 1.22× | Win |
| Random (prettified) | 2,152 | 1,813 | 1.19× | Win |
| Twitter (minified) | 2,171 | 1,868 | 1.16× | Win |
| Twitter (prettified) | 2,732 | 2,414 | 1.13× | Win |
| Apache Builds Small (minified) | 2,367 | 1,939 | 1.22× | Win |
| Canada Small (minified) | 948 | 895 | 1.06× | Win |
| Apache Builds Small (prettified) | 2,867 | 2,429 | 1.18× | Win |
| Canada Small (prettified) | 2,238 | 2,070 | 1.08× | Win |
| CitmCatalog Small (minified) | 1,655 | 1,306 | 1.27× | Win |
| CitmCatalog Small (prettified) | 2,822 | 2,273 | 1.24× | Win |
| Discord Small (minified) | 2,015 | 1,736 | 1.16× | Win |
| Discord Small (prettified) | 2,640 | 2,431 | 1.09× | Win |
| Github Events Small (minified) | 2,094 | 2,013 | 1.04× | Win |
| Google Maps Response Small (minified) | 1,823 | 1,581 | 1.15× | Win |
| Github Events Small (prettified) | 2,576 | 2,490 | 1.03× | Win |
| Google Maps Response Small (prettified) | 3,404 | 3,102 | 1.10× | Win |
| Instruments Small (minified) | 2,289 | 1,697 | 1.35× | Win |
| Instruments Small (prettified) | 3,486 | 2,749 | 1.27× | Win |
| Marine IK Reverse Small (minified) | 882 | 711 | 1.24× | Win |
| Marine IK Reverse Small (prettified) | 2,933 | 2,537 | 1.16× | Win |
| Marine IK Small (minified) | 925 | 696 | 1.33× | Win |
| Marine IK Small (prettified) | 3,136 | 2,548 | 1.23× | Win |
| Random Small (minified) | 1,773 | 1,159 | 1.53× | Win |
| Random Small (prettified) | 2,628 | 1,729 | 1.52× | Win |
| Twitter Small (minified) | 2,499 | 2,137 | 1.17× | Win |
| Twitter Small (prettified) | 3,142 | 2,698 | 1.16× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,241 | 970 | 1.28× | Win |
| Canada (minified) | 690 | 644 | 1.07× | Win |
| Apache Builds (prettified) | 1,665 | 1,336 | 1.25× | Win |
| Canada (prettified) | 1,412 | 1,418 | 1.00× | Tie |
| CitmCatalog (minified) | 1,448 | 1,275 | 1.14× | Win |
| CitmCatalog (prettified) | 2,774 | 2,150 | 1.29× | Win |
| Discord (minified) | 1,621 | 1,049 | 1.54× | Win |
| Discord (prettified) | 1,855 | 1,518 | 1.22× | Win |
| Github Events (minified) | 1,456 | 1,298 | 1.12× | Win |
| Google Maps Response (minified) | 1,314 | 1,103 | 1.19× | Win |
| Github Events (prettified) | 1,843 | 1,506 | 1.22× | Win |
| Google Maps Response (prettified) | 2,369 | 1,888 | 1.26× | Win |
| Instruments (minified) | 1,789 | 1,312 | 1.36× | Win |
| Instruments (prettified) | 2,599 | 1,982 | 1.31× | Win |
| Marine IK Reverse (minified) | 591 | 586 | 1.01× | Win |
| Marine IK Reverse (prettified) | 1,800 | 1,817 | 0.99× | Tie |
| Marine IK (minified) | 583 | 545 | 1.07× | Win |
| Marine IK (prettified) | 1,925 | 1,745 | 1.10× | Win |
| Random (minified) | 1,301 | 901 | 1.44× | Win |
| Random (prettified) | 1,691 | 1,319 | 1.28× | Win |
| Twitter (minified) | 1,754 | 1,156 | 1.52× | Win |
| Twitter (prettified) | 1,932 | 1,528 | 1.26× | Win |
| Apache Builds Small (minified) | 1,818 | 1,275 | 1.43× | Win |
| Canada Small (minified) | 726 | 708 | 1.03× | Win |
| Apache Builds Small (prettified) | 2,097 | 1,428 | 1.47× | Win |
| Canada Small (prettified) | 1,584 | 1,447 | 1.10× | Win |
| CitmCatalog Small (minified) | 1,090 | 1,015 | 1.07× | Win |
| CitmCatalog Small (prettified) | 2,138 | 1,499 | 1.43× | Win |
| Discord Small (minified) | 1,596 | 1,213 | 1.32× | Win |
| Discord Small (prettified) | 1,754 | 1,512 | 1.16× | Win |
| Github Events Small (minified) | 1,448 | 1,258 | 1.15× | Win |
| Google Maps Response Small (minified) | 1,505 | 1,046 | 1.44× | Win |
| Github Events Small (prettified) | 1,868 | 1,342 | 1.39× | Win |
| Google Maps Response Small (prettified) | 2,540 | 1,753 | 1.45× | Win |
| Instruments Small (minified) | 1,748 | 1,257 | 1.39× | Win |
| Instruments Small (prettified) | 2,433 | 1,596 | 1.52× | Win |
| Marine IK Reverse Small (minified) | 573 | 607 | 0.94× | **Loss** |
| Marine IK Reverse Small (prettified) | 1,950 | 1,878 | 1.04× | Win |
| Marine IK Small (minified) | 595 (n/c) | 565 | 1.05× | Win |
| Marine IK Small (prettified) | 2,186 | 2,039 | 1.07× | Win |
| Random Small (minified) | 1,343 | 947 | 1.42× | Win |
| Random Small (prettified) | 1,715 | 1,385 | 1.24× | Win |
| Twitter Small (minified) | 2,166 | 1,432 | 1.51× | Win |
| Twitter Small (prettified) | 2,359 | 1,750 | 1.35× | Win |

In the freshly allocated run, two-stage Jsonifier wins 75 of these 88 tests, ties 1 and loses 12, by up to 21%. Six of the losses are on Linux/GCC: prettified Apache Builds, minified Canada, minified and prettified Github Events, and minified and prettified Github Events Small (0.98×, 0.97×, 0.98×, 0.99×, 0.96× and 0.95×). The other six are on macOS/GCC: minified and prettified Canada, minified Marine IK, prettified Discord Small and Github Events Small, and minified Marine IK Reverse Small (0.88×, 0.79×, 0.98×, 0.94×, 0.91× and 0.97×). The tie is minified Discord on macOS/GCC (1.00×). With the fused path instead, Jsonifier beats the reflection reader on 76, ties 2 and loses 10. With objects reused, two-stage Jsonifier wins 84, ties 2 and loses 2: minified Canada on Linux/GCC (921 against 971 MB/s) and minified Marine IK Reverse Small on macOS/GCC (573 against 607); the ties are prettified Canada and Marine IK Reverse on macOS/GCC. The fused path beats the reflection reader on 80, ties 2 and loses 6. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a row near 1.00× can go either way: freshly allocated prettified Github Events on Linux/GCC (1,858 against 1,883 MB/s, 0.99×) is a loss, while reused prettified Marine IK Reverse on macOS/GCC (1,800 against 1,817) is a tie.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 34 of the 40 results; the widest gap is freshly allocated minified Twitter Small on macOS/Clang, where the two-stage row runs at 3,558 MB/s against the "jsonifier" row's 2,984. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 6,633 | 4,199 | 1.58× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,517 | 5,143 | 1.46× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,622 | 3,755 | 1.23× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,352 | 4,603 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,176 | 6,884 | 1.19× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,369 | 8,279 | 1.13× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,658 | 5,553 | 1.02× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,333 | 6,543 | 0.97× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,944 | 6,791 | 1.17× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,057 | 8,177 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,473 | 5,442 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,667 | 6,532 | 1.02× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,828 | 3,879 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,000 | 4,237 | 1.18× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,645 | 3,485 | 1.05× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,948 | 4,093 | 0.96× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,998 | 3,321 | 1.51× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,850 | 4,226 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,558 | 3,420 | 1.04× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,119 | 3,988 | 1.03× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 6,843 | 4,350 | 1.57× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,665 | 5,284 | 1.45× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,195 | 4,135 | 1.26× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,897 | 5,087 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,188 | 7,008 | 1.17× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,340 | 8,307 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,690 | 5,578 | 1.02× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,458 | 6,709 | 0.96× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,135 | 6,956 | 1.17× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,151 | 8,262 | 1.11× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,754 | 5,611 | 1.03× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,827 | 6,826 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,059 | 3,824 | 1.32× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,676 | 4,026 | 1.41× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,120 | 3,565 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,431 | 3,636 | 1.22× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,167 | 3,968 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,280 | 4,074 | 1.30× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,884 | 3,633 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,261 | 4,177 | 1.02× | Win |

Jsonifier wins 36 of the 40 results, ties 2 and loses 2, and wins by up to 1.58× (minified Twitter under MSVC, freshly allocated). Freshly allocated it wins 18, ties 1 and loses 1: the tie is prettified Twitter Small on macOS/GCC (0.96×), and the loss is prettified Twitter Small on Linux/Clang (0.97×). The reused run also has 18 wins, 1 tie and 1 loss: the tie is prettified Twitter Small on Linux/GCC (1.00×), and the loss is prettified Twitter Small on Linux/Clang (0.96×). Every full-size result is a win, by 1.11× to 1.58× on the three x86 builds and 1.05× to 1.51× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on four of the five builds, by 1.25× to 1.37×, and loses on macOS/GCC:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 3,000 | 2,398 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,579 | 2,616 | 1.37× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,983 | 2,173 | 1.37× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,798 | 1,942 | 0.93× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,259 | 1,796 | 1.26× | Win |

The narrowest win is under MSVC, 1.25× (3,000 against 2,398 MB/s), and the loss on macOS/GCC is 0.93× (1,798 against 1,942 MB/s).

### 2.8 AVX-512 builds

Every result above comes from AVX2 and NEON builds. The same tests also ran on three AVX-512 builds, Linux / Clang 24.0, Linux / GCC 16.2 and Windows / MSVC 19.51, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, from the Json-Performance branch `benchmarking-avx512-main` at the same commits as §2.1, on November 17, 2026. Jsonifier and both of its paths run their AVX-512 kernels, and simdjson runs its `icelake` kernel. Sampling, convergence limits (RSE under 5%, mean shift under 2.5%) and verdicts are as in §2.1. The Linux / GCC build also runs the reflection reader of §2.5. The runners are shared virtual machines, and the CPU, operating-system images and MSVC version differ from the AVX2 builds, so a result here cannot be set against the matching AVX2 result to isolate the instruction set; every comparison below is within one build.

- **The routing split holds.** Freshly allocated, the fused path wins 79 of the 147 tests, 2 are ties and the tape wins 66; reused, 78, 1 and 68. Freshly allocated, the fused path wins all 15 POD-type tests (1.36× for String under MSVC to 5.34× for Bool on Linux / GCC) and 64 of the 66 minified documents. The tape takes minified Github Events on Linux / Clang (0.97×) and Apache Builds Small on Linux / GCC (0.99×). The tape wins 64 of the 66 prettified documents, and the other two are ties, prettified Canada under MSVC and on Linux / Clang. Its margins reach 1.57× (Instruments on Linux / Clang, 3,370 against 2,146 MB/s) and 1.67× on the Small copy.
- **Against simdjson, the faster Jsonifier path wins 146 of 147 freshly allocated and 144 of 147 reused.** All of its non-wins are on Linux / GCC: prettified Random (0.98×) is the one loss freshly allocated; reused, prettified Random (0.98×) is a loss and prettified Apache Builds and minified Random are ties. The fused path alone wins 139, ties 2 and loses 6 freshly allocated, and wins 137, ties 3 and loses 7 reused; all of the freshly allocated non-wins are prettified documents, four of the six losses on Linux / GCC.
- **Stage 1 + reflection on Linux / GCC.** Against simdjson's reflection reader, two-stage Jsonifier wins 29 of the 44 corpus tests freshly allocated, ties 4 (prettified Discord, prettified Random Small and both forms of Twitter Small) and loses 11, by up to 16% (minified Github Events, 0.84×). Reused it wins 37 and loses 7, by up to 11% (minified Random, 0.89×). With the fused path instead, Jsonifier beats the reflection reader on 29 freshly allocated and 31 reused.
- **Reverse key order costs simdjson most under MSVC.** On this build simdjson parses minified Marine IK Reverse at 126 MB/s against 220 MB/s in declared order, and the faster Jsonifier path is 5.51× faster freshly allocated (6.00× reused); on the prettified form it is 4.44× faster (4.75× reused). On both Linux builds simdjson reads the full-size reversed document within 4% of the declared one, and the faster Jsonifier path is 1.36× (Linux / Clang) and 1.54× (Linux / GCC) faster on the minified form, and 1.29× and 1.31× on the prettified form.
- **Partial reading.** Freshly allocated, Jsonifier wins all four Twitter Partial results under MSVC (2.56× to 2.59× full-size, 1.73× to 1.83× Small) and both full-size results on Linux / Clang (1.27× and 1.33×); on Linux / GCC it wins prettified Twitter (1.14×) and ties minified Twitter (1.00×), and it loses both Small copies on both Linux builds (0.86× to 0.96×), for 7 wins, 1 tie and 4 losses. Reused it wins every full-size result (2.48× to 2.57× under MSVC, 1.27× to 1.41× on Linux / Clang, 1.06× to 1.13× on Linux / GCC), both Small copies under MSVC and prettified Twitter Small on Linux / Clang (1.03×), for 9 wins and 3 losses.
- **Validation.** Jsonifier wins on all three builds, by 1.23× under MSVC, 1.59× on Linux / Clang and 1.29× on Linux / GCC.

#### Freshly allocated objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 27 | 1 | 21 | 46 of 49 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 26 | 1 | 22 | 49 of 49 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 26 | 0 | 23 | 48 of 49 |
| **Aggregate** | **79** | **2** | **66** | **143 of 147** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 47 / 1 / 1 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 48 / 0 / 1 | 44 / 1 / 4 |
| **Aggregate** | **146 / 0 / 1** | **139 / 2 / 6** |

**Windows / MSVC 19.51 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 436 | 157 | 68 | 2.77× | Fused | Win |
| Double (POD) | 849 | 338 | 134 | 2.51× | Fused | Win |
| Int64 (POD) | 1,494 | 391 | 296 | 3.82× | Fused | Win |
| String (POD) | 864 | 635 | 518 | 1.36× | Fused | Win |
| Uint64 (POD) | 1,347 | 548 | 301 | 2.46× | Fused | Win |
| Apache Builds (minified) | 823 | 791 | 692 | 1.04× | Fused | Win |
| Canada (minified) | 592 | 554 | 194 (n/c) | 1.07× | Fused | Win |
| Apache Builds (prettified) | 962 | 1,019 | 867 | 0.94× | Two-stage | Win |
| Canada (prettified) | 1,269 | 1,253 | 440 (n/c) | 1.01× | Tie | Win |
| CitmCatalog (minified) | 1,325 | 1,139 | 573 | 1.16× | Fused | Win |
| CitmCatalog (prettified) | 2,151 | 2,425 | 1,234 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,211 | 950 | 700 | 1.27× | Fused | Win |
| Discord (prettified) | 1,316 | 1,386 | 1,012 | 0.95× | Two-stage | Win |
| Github Events (minified) | 1,007 | 970 | 610 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 1,282 | 1,084 | 533 | 1.18× | Fused | Win |
| Github Events (prettified) | 1,051 | 1,178 | 745 | 0.89× | Two-stage | Win |
| Google Maps Response (prettified) | 2,180 | 2,222 | 1,062 | 0.98× | Two-stage | Win |
| Instruments (minified) | 1,516 | 1,124 | 737 | 1.35× | Fused | Win |
| Instruments (prettified) | 1,653 | 1,829 | 1,129 | 0.90× | Two-stage | Win |
| Marine IK Reverse (minified) | 695 | 584 | 126 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,013 | 2,147 | 484 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 679 | 597 | 220 | 1.14× | Fused | Win |
| Marine IK (prettified) | 2,105 | 2,192 | 804 (n/c) | 0.96× | Two-stage | Win |
| Random (minified) | 709 | 633 | 501 | 1.12× | Fused | Win |
| Random (prettified) | 923 | 954 | 707 | 0.97× | Two-stage | Win |
| Twitter (minified) | 1,275 | 1,113 | 875 | 1.15× | Fused | Win |
| Twitter (prettified) | 1,280 | 1,499 | 1,163 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 964 | 921 | 640 | 1.05× | Fused | Win |
| Canada Small (minified) | 529 | 488 | 161 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,059 | 1,175 | 828 | 0.90× | Two-stage | Win |
| Canada Small (prettified) | 1,108 | 1,195 | 394 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 777 | 680 | 422 | 1.14× | Fused | Win |
| CitmCatalog Small (prettified) | 1,103 | 1,204 | 745 | 0.92× | Two-stage | Win |
| Discord Small (minified) | 963 | 793 | 645 | 1.21× | Fused | Win |
| Discord Small (prettified) | 1,065 | 1,094 | 853 | 0.97× | Two-stage | Win |
| Github Events Small (minified) | 910 | 828 | 540 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 1,142 | 950 | 491 | 1.20× | Fused | Win |
| Github Events Small (prettified) | 910 | 1,063 | 680 | 0.86× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,889 | 1,921 | 1,020 | 0.98× | Two-stage | Win |
| Instruments Small (minified) | 1,415 | 1,028 | 674 | 1.38× | Fused | Win |
| Instruments Small (prettified) | 1,546 | 1,722 | 1,093 | 0.90× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 438 | 428 | 97 | 1.02× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,280 | 1,589 | 354 | 0.81× | Two-stage | Win |
| Marine IK Small (minified) | 484 | 451 | 177 | 1.07× | Fused | Win |
| Marine IK Small (prettified) | 1,488 | 1,698 | 661 | 0.88× | Two-stage | Win |
| Random Small (minified) | 740 | 667 | 474 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,002 | 1,059 | 725 | 0.95× | Two-stage | Win |
| Twitter Small (minified) | 1,562 | 1,285 | 1,190 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 1,401 | 1,670 | 1,550 | 0.84× | Two-stage | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,770 | 493 | 251 | 3.59× | Fused | Win |
| Double (POD) | 1,245 | 442 | 292 | 2.82× | Fused | Win |
| Int64 (POD) | 2,720 | 834 | 644 | 3.26× | Fused | Win |
| String (POD) | 1,731 | 1,019 | 1,201 | 1.70× | Fused | Win |
| Uint64 (POD) | 2,824 | 839 | 606 | 3.37× | Fused | Win |
| Apache Builds (minified) | 1,561 | 1,433 | 1,036 | 1.09× | Fused | Win |
| Canada (minified) | 781 | 719 | 688 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,679 | 1,793 | 1,356 | 0.94× | Two-stage | Win |
| Canada (prettified) | 1,562 | 1,563 | 1,550 | 1.00× | Tie | Win |
| CitmCatalog (minified) | 1,993 | 1,744 | 1,051 | 1.14× | Fused | Win |
| CitmCatalog (prettified) | 2,949 | 3,585 | 2,235 | 0.82× | Two-stage | Win |
| Discord (minified) | 2,073 | 1,695 | 787 | 1.22× | Fused | Win |
| Discord (prettified) | 1,938 | 2,336 | 1,184 | 0.83× | Two-stage | Win |
| Github Events (minified) | 1,598 | 1,647 | 989 | 0.97× | Two-stage | Win |
| Google Maps Response (minified) | 2,286 | 1,752 | 899 | 1.31× | Fused | Win |
| Github Events (prettified) | 1,449 | 2,001 | 1,185 | 0.72× | Two-stage | Win |
| Google Maps Response (prettified) | 2,995 | 3,329 | 1,845 | 0.90× | Two-stage | Win |
| Instruments (minified) | 2,664 | 2,265 | 662 | 1.18× | Fused | Win |
| Instruments (prettified) | 2,146 | 3,370 | 1,126 | 0.64× | Two-stage | Win |
| Marine IK Reverse (minified) | 781 | 715 | 573 | 1.09× | Fused | Win |
| Marine IK Reverse (prettified) | 2,376 | 2,626 | 2,042 | 0.90× | Two-stage | Win |
| Marine IK (minified) | 783 | 744 | 577 | 1.05× | Fused | Win |
| Marine IK (prettified) | 2,406 | 2,445 | 2,082 | 0.98× | Two-stage | Win |
| Random (minified) | 1,161 | 1,116 | 796 | 1.04× | Fused | Win |
| Random (prettified) | 1,410 | 1,699 | 1,180 | 0.83× | Two-stage | Win |
| Twitter (minified) | 1,726 | 1,686 | 872 | 1.02× | Fused | Win |
| Twitter (prettified) | 1,648 | 2,118 | 1,129 | 0.78× | Two-stage | Win |
| Apache Builds Small (minified) | 2,404 | 2,342 | 1,453 | 1.03× | Fused | Win |
| Canada Small (minified) | 905 | 842 | 696 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 2,276 | 2,835 | 1,878 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 1,788 | 1,848 | 1,674 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,722 | 1,556 | 1,015 | 1.11× | Fused | Win |
| CitmCatalog Small (prettified) | 1,887 | 2,557 | 1,771 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 1,808 | 1,554 | 771 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,637 | 2,119 | 1,093 | 0.77× | Two-stage | Win |
| Github Events Small (minified) | 2,248 | 1,966 | 1,361 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 2,297 | 1,806 | 937 | 1.27× | Fused | Win |
| Github Events Small (prettified) | 1,511 | 2,402 | 1,726 | 0.63× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,897 | 3,401 | 1,928 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 2,635 | 2,196 | 624 | 1.20× | Fused | Win |
| Instruments Small (prettified) | 1,970 | 3,293 | 1,038 | 0.60× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 821 | 718 | 521 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,009 | 2,472 | 1,816 | 0.81× | Two-stage | Win |
| Marine IK Small (minified) | 863 | 802 | 509 | 1.08× | Fused | Win |
| Marine IK Small (prettified) | 2,198 | 2,767 | 1,821 | 0.79× | Two-stage | Win |
| Random Small (minified) | 1,679 | 1,511 | 864 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,759 | 2,214 | 1,318 | 0.79× | Two-stage | Win |
| Twitter Small (minified) | 2,070 | 2,024 | 969 | 1.02× | Fused | Win |
| Twitter Small (prettified) | 1,824 | 2,508 | 1,196 | 0.73× | Two-stage | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,416 | 452 | 211 | 5.34× | Fused | Win |
| Double (POD) | 1,414 | 457 | 210 | 3.09× | Fused | Win |
| Int64 (POD) | 3,031 | 821 | 421 | 3.69× | Fused | Win |
| String (POD) | 1,607 | 905 | 1,131 | 1.78× | Fused | Win |
| Uint64 (POD) | 2,804 | 795 | 415 | 3.53× | Fused | Win |
| Apache Builds (minified) | 1,535 | 1,336 | 1,317 | 1.15× | Fused | Win |
| Canada (minified) | 852 | 774 | 698 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,690 | 1,708 | 1,678 | 0.99× | Two-stage | Win |
| Canada (prettified) | 1,681 | 1,710 | 1,573 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 2,072 | 1,799 | 1,543 | 1.15× | Fused | Win |
| CitmCatalog (prettified) | 3,199 | 3,619 | 3,111 | 0.88× | Two-stage | Win |
| Discord (minified) | 1,974 | 1,752 | 1,437 | 1.13× | Fused | Win |
| Discord (prettified) | 2,068 | 2,371 | 2,018 | 0.87× | Two-stage | Win |
| Github Events (minified) | 1,666 | 1,635 | 1,302 | 1.02× | Fused | Win |
| Google Maps Response (minified) | 1,997 | 1,607 | 1,146 | 1.24× | Fused | Win |
| Github Events (prettified) | 1,755 | 2,037 | 1,557 | 0.86× | Two-stage | Win |
| Google Maps Response (prettified) | 3,048 | 3,058 | 2,338 | 1.00× | Two-stage | Win |
| Instruments (minified) | 3,003 | 2,392 | 1,065 | 1.26× | Fused | Win |
| Instruments (prettified) | 2,815 | 3,649 | 1,749 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 869 (n/c) | 747 | 565 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,500 | 2,668 | 2,037 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 840 | 741 | 553 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,536 | 2,593 | 1,970 | 0.98× | Two-stage | Win |
| Random (minified) | 1,105 | 1,028 | 1,064 | 1.07× | Fused | Win |
| Random (prettified) | 1,535 | 1,678 | 1,714 | 0.91× | Two-stage | Loss |
| Twitter (minified) | 1,956 | 1,768 | 987 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,733 | 2,124 | 1,261 | 0.82× | Two-stage | Win |
| Apache Builds Small (minified) | 2,048 | 2,076 | 1,700 | 0.99× | Two-stage | Win |
| Canada Small (minified) | 906 | 831 | 739 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,190 | 2,608 | 2,202 | 0.84× | Two-stage | Win |
| Canada Small (prettified) | 1,789 | 1,873 | 1,618 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,645 | 1,542 | 1,250 | 1.07× | Fused | Win |
| CitmCatalog Small (prettified) | 1,909 | 2,521 | 2,301 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 1,731 | 1,509 | 1,282 | 1.15× | Fused | Win |
| Discord Small (prettified) | 1,758 | 2,000 | 1,723 | 0.88× | Two-stage | Win |
| Github Events Small (minified) | 1,840 | 1,589 | 1,409 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,061 | 1,563 | 1,201 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 1,591 | 2,097 | 1,631 | 0.76× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,912 | 3,004 | 2,385 | 0.97× | Two-stage | Win |
| Instruments Small (minified) | 3,258 | 2,422 | 993 | 1.34× | Fused | Win |
| Instruments Small (prettified) | 2,695 | 3,690 | 1,607 | 0.73× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 874 | 770 | 566 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,318 | 2,629 | 1,990 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 970 | 829 | 545 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,478 | 2,903 | 1,964 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,496 | 1,377 | 1,221 | 1.09× | Fused | Win |
| Random Small (prettified) | 1,930 | 2,206 | 1,913 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,296 | 1,976 | 1,177 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 2,055 | 2,494 | 1,436 | 0.82× | Two-stage | Win |

#### Reused objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 27 | 0 | 22 | 47 of 49 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 25 | 1 | 23 | 48 of 49 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 26 | 0 | 23 | 49 of 49 |
| **Aggregate** | **78** | **1** | **68** | **144 of 147** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 46 / 2 / 1 | 41 / 3 / 5 |
| **Aggregate** | **144 / 2 / 1** | **137 / 3 / 7** |

**Windows / MSVC 19.51 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 536 | 165 | 67 | 3.25× | Fused | Win |
| Double (POD) | 1,180 | 379 | 145 | 3.11× | Fused | Win |
| Int64 (POD) | 1,898 | 623 | 325 | 3.05× | Fused | Win |
| String (POD) | 1,359 | 826 | 667 | 1.64× | Fused | Win |
| Uint64 (POD) | 1,718 | 592 | 314 | 2.90× | Fused | Win |
| Apache Builds (minified) | 1,262 | 1,161 | 978 | 1.09× | Fused | Win |
| Canada (minified) | 709 | 649 | 208 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 1,379 | 1,450 | 1,259 | 0.95× | Two-stage | Win |
| Canada (prettified) | 1,405 | 1,499 | 471 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,514 | 1,264 | 541 | 1.20× | Fused | Win |
| CitmCatalog (prettified) | 2,329 | 2,621 | 1,142 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,711 | 1,252 | 891 | 1.37× | Fused | Win |
| Discord (prettified) | 1,675 | 1,779 | 1,239 | 0.94× | Two-stage | Win |
| Github Events (minified) | 1,492 | 1,368 | 746 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 1,423 | 1,213 | 581 | 1.17× | Fused | Win |
| Github Events (prettified) | 1,426 | 1,616 | 893 | 0.88× | Two-stage | Win |
| Google Maps Response (prettified) | 2,444 | 2,472 | 1,205 | 0.99× | Two-stage | Win |
| Instruments (minified) | 1,805 | 1,303 | 780 | 1.39× | Fused | Win |
| Instruments (prettified) | 1,832 | 2,054 | 1,235 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 761 | 645 (n/c) | 127 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,261 | 2,332 | 491 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 749 | 678 | 228 (n/c) | 1.10× | Fused | Win |
| Marine IK (prettified) | 2,327 | 2,433 | 838 | 0.96× | Two-stage | Win |
| Random (minified) | 886 | 804 | 605 | 1.10× | Fused | Win |
| Random (prettified) | 1,123 | 1,188 | 866 | 0.95× | Two-stage | Win |
| Twitter (minified) | 1,797 | 1,502 | 1,102 | 1.20× | Fused | Win |
| Twitter (prettified) | 1,625 | 1,921 | 1,407 | 0.85× | Two-stage | Win |
| Apache Builds Small (minified) | 1,301 | 1,218 | 1,088 | 1.07× | Fused | Win |
| Canada Small (minified) | 681 | 589 | 173 | 1.16× | Fused | Win |
| Apache Builds Small (prettified) | 1,372 | 1,522 | 1,348 | 0.90× | Two-stage | Win |
| Canada Small (prettified) | 1,360 | 1,392 | 422 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 593 | 841 | 462 | 0.71× | Two-stage | Win |
| CitmCatalog Small (prettified) | 1,344 | 1,531 | 800 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,508 | 1,109 | 870 | 1.36× | Fused | Win |
| Discord Small (prettified) | 1,419 | 1,472 | 1,204 | 0.96× | Two-stage | Win |
| Github Events Small (minified) | 1,219 | 1,092 | 712 | 1.12× | Fused | Win |
| Google Maps Response Small (minified) | 1,349 | 1,055 | 561 | 1.28× | Fused | Win |
| Github Events Small (prettified) | 1,192 | 1,416 | 884 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,150 | 2,123 | 1,125 | 1.01× | Fused | Win |
| Instruments Small (minified) | 1,558 | 1,108 | 658 | 1.41× | Fused | Win |
| Instruments Small (prettified) | 1,611 | 1,817 | 1,076 | 0.89× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 564 | 545 | 104 | 1.03× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,586 | 1,991 | 385 | 0.80× | Two-stage | Win |
| Marine IK Small (minified) | 661 | 566 | 197 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 1,802 | 2,129 | 735 | 0.85× | Two-stage | Win |
| Random Small (minified) | 878 | 808 | 573 | 1.09× | Fused | Win |
| Random Small (prettified) | 1,170 | 1,276 | 865 | 0.92× | Two-stage | Win |
| Twitter Small (minified) | 1,846 | 1,498 | 1,317 | 1.23× | Fused | Win |
| Twitter Small (prettified) | 1,576 | 1,884 | 1,632 | 0.84× | Two-stage | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,768 | 479 | 246 | 3.69× | Fused | Win |
| Double (POD) | 1,319 | 463 | 288 | 2.85× | Fused | Win |
| Int64 (POD) | 2,756 | 849 | 662 | 3.25× | Fused | Win |
| String (POD) | 2,531 | 1,261 | 1,561 | 2.01× | Fused | Win |
| Uint64 (POD) | 3,036 | 853 | 619 | 3.56× | Fused | Win |
| Apache Builds (minified) | 2,316 | 1,962 | 1,343 | 1.18× | Fused | Win |
| Canada (minified) | 966 | 849 | 833 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 2,262 | 2,494 | 1,722 | 0.91× | Two-stage | Win |
| Canada (prettified) | 1,837 | 1,880 | 1,802 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 2,443 | 2,039 | 1,174 | 1.20× | Fused | Win |
| CitmCatalog (prettified) | 3,282 | 4,172 | 2,562 | 0.79× | Two-stage | Win |
| Discord (minified) | 2,741 | 2,140 | 904 | 1.28× | Fused | Win |
| Discord (prettified) | 2,334 | 3,019 | 1,350 | 0.77× | Two-stage | Win |
| Github Events (minified) | 2,142 | 2,194 | 1,064 | 0.98× | Two-stage | Win |
| Google Maps Response (minified) | 2,573 | 1,868 | 944 | 1.38× | Fused | Win |
| Github Events (prettified) | 1,852 | 2,654 | 1,263 | 0.70× | Two-stage | Win |
| Google Maps Response (prettified) | 3,106 | 3,577 | 1,914 | 0.87× | Two-stage | Win |
| Instruments (minified) | 3,070 | 2,530 | 676 | 1.21× | Fused | Win |
| Instruments (prettified) | 2,256 | 3,748 | 1,139 | 0.60× | Two-stage | Win |
| Marine IK Reverse (minified) | 868 | 813 | 646 | 1.07× | Fused | Win |
| Marine IK Reverse (prettified) | 2,603 | 2,741 | 2,180 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 873 | 814 (n/c) | 628 | 1.07× | Fused | Win |
| Marine IK (prettified) | 2,619 | 2,928 | 2,351 | 0.89× | Two-stage | Win |
| Random (minified) | 1,614 | 1,478 | 939 | 1.09× | Fused | Win |
| Random (prettified) | 1,802 | 2,198 | 1,536 | 0.82× | Two-stage | Win |
| Twitter (minified) | 2,100 | 2,074 | 958 | 1.01× | Tie | Win |
| Twitter (prettified) | 1,873 | 2,785 | 1,284 | 0.67× | Two-stage | Win |
| Apache Builds Small (minified) | 2,740 | 2,708 | 1,686 | 1.01× | Fused | Win |
| Canada Small (minified) | 1,061 | 967 | 783 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,400 | 3,390 | 2,095 | 0.71× | Two-stage | Win |
| Canada Small (prettified) | 2,006 | 2,247 | 1,928 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,172 | 1,798 | 1,161 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 2,103 | 2,928 | 1,984 | 0.72× | Two-stage | Win |
| Discord Small (minified) | 2,423 | 2,065 | 854 | 1.17× | Fused | Win |
| Discord Small (prettified) | 2,052 | 2,860 | 1,137 | 0.72× | Two-stage | Win |
| Github Events Small (minified) | 2,998 | 2,525 | 1,601 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 2,516 | 1,946 | 997 | 1.29× | Fused | Win |
| Github Events Small (prettified) | 1,969 | 3,095 | 2,005 | 0.64× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,369 | 3,820 | 1,986 | 0.88× | Two-stage | Win |
| Instruments Small (minified) | 2,901 | 2,386 | 638 | 1.22× | Fused | Win |
| Instruments Small (prettified) | 2,075 | 3,789 | 1,063 | 0.55× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 963 | 895 | 544 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,354 | 3,053 | 1,994 | 0.77× | Two-stage | Win |
| Marine IK Small (minified) | 1,059 | 950 | 549 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 2,560 | 3,236 | 2,021 | 0.79× | Two-stage | Win |
| Random Small (minified) | 2,026 | 1,829 | 982 | 1.11× | Fused | Win |
| Random Small (prettified) | 2,051 | 2,610 | 1,483 | 0.79× | Two-stage | Win |
| Twitter Small (minified) | 2,225 | 2,160 | 1,009 | 1.03× | Fused | Win |
| Twitter Small (prettified) | 1,884 | 2,725 | 1,291 | 0.69× | Two-stage | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,361 | 456 | 211 | 5.18× | Fused | Win |
| Double (POD) | 1,527 | 452 | 219 | 3.38× | Fused | Win |
| Int64 (POD) | 3,187 | 809 | 578 | 3.94× | Fused | Win |
| String (POD) | 2,171 | 1,087 | 1,366 | 2.00× | Fused | Win |
| Uint64 (POD) | 3,001 | 824 | 555 | 3.64× | Fused | Win |
| Apache Builds (minified) | 2,070 | 1,735 | 1,725 | 1.19× | Fused | Win |
| Canada (minified) | 995 | 879 | 812 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 2,174 | 2,210 | 2,209 | 0.98× | Two-stage | Tie |
| Canada (prettified) | 1,911 | 1,960 | 1,851 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 2,486 | 2,137 | 1,864 | 1.16× | Fused | Win |
| CitmCatalog (prettified) | 3,637 | 4,166 | 3,706 | 0.87× | Two-stage | Win |
| Discord (minified) | 2,481 | 2,060 | 1,676 | 1.20× | Fused | Win |
| Discord (prettified) | 2,367 | 2,836 | 2,366 | 0.83× | Two-stage | Win |
| Github Events (minified) | 2,071 | 2,052 | 1,441 | 1.01× | Fused | Win |
| Google Maps Response (minified) | 2,141 | 1,689 | 1,247 | 1.27× | Fused | Win |
| Github Events (prettified) | 2,009 | 2,542 | 1,731 | 0.79× | Two-stage | Win |
| Google Maps Response (prettified) | 3,143 | 3,075 | 2,438 | 1.02× | Fused | Win |
| Instruments (minified) | 3,502 | 2,682 | 1,169 | 1.31× | Fused | Win |
| Instruments (prettified) | 2,867 | 4,014 | 1,900 | 0.71× | Two-stage | Win |
| Marine IK Reverse (minified) | 931 | 842 | 625 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 2,730 | 2,847 | 2,174 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 940 | 812 | 606 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,771 | 2,802 | 2,146 | 0.99× | Two-stage | Win |
| Random (minified) | 1,412 | 1,305 | 1,405 | 1.08× | Fused | Tie |
| Random (prettified) | 1,885 | 2,101 | 2,135 | 0.90× | Two-stage | Loss |
| Twitter (minified) | 2,271 | 2,143 | 1,099 | 1.06× | Fused | Win |
| Twitter (prettified) | 1,970 | 2,677 | 1,412 | 0.74× | Two-stage | Win |
| Apache Builds Small (minified) | 2,231 | 2,246 | 1,977 | 0.99× | Two-stage | Win |
| Canada Small (minified) | 1,098 | 1,036 | 759 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 2,419 | 2,783 | 2,422 | 0.87× | Two-stage | Win |
| Canada Small (prettified) | 2,005 | 2,190 | 1,737 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,873 | 1,738 | 1,508 | 1.08× | Fused | Win |
| CitmCatalog Small (prettified) | 2,117 | 2,874 | 2,576 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,009 | 1,803 | 1,634 | 1.11× | Fused | Win |
| Discord Small (prettified) | 2,020 | 2,338 | 1,958 | 0.86× | Two-stage | Win |
| Github Events Small (minified) | 2,240 | 2,293 | 1,457 | 0.98× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,090 | 1,710 | 1,273 | 1.22× | Fused | Win |
| Github Events Small (prettified) | 1,948 | 2,757 | 1,873 | 0.71× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,139 | 3,228 | 2,488 | 0.97× | Two-stage | Win |
| Instruments Small (minified) | 3,778 | 2,639 | 1,027 | 1.43× | Fused | Win |
| Instruments Small (prettified) | 2,708 | 4,102 | 1,697 | 0.66× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,012 | 900 | 586 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,687 | 3,134 | 2,090 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,175 | 984 | 571 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 2,851 | 3,373 | 2,074 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,683 | 1,556 | 1,591 | 1.08× | Fused | Win |
| Random Small (prettified) | 2,172 | 2,496 | 2,455 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,532 | 2,253 | 1,195 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 2,221 | 2,699 | 1,514 | 0.82× | Two-stage | Win |

#### Freshly allocated objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,336 | 1,431 | 0.93× | **Loss** |
| Canada (minified) | 774 | 688 | 1.13× | Win |
| Apache Builds (prettified) | 1,708 | 1,822 | 0.94× | **Loss** |
| Canada (prettified) | 1,710 | 1,568 | 1.09× | Win |
| CitmCatalog (minified) | 1,799 | 1,513 | 1.19× | Win |
| CitmCatalog (prettified) | 3,619 | 3,056 | 1.18× | Win |
| Discord (minified) | 1,752 | 1,652 | 1.06× | Win |
| Discord (prettified) | 2,371 | 2,364 | 1.00× | Tie |
| Github Events (minified) | 1,635 | 1,952 | 0.84× | **Loss** |
| Google Maps Response (minified) | 1,607 | 1,441 | 1.12× | Win |
| Github Events (prettified) | 2,037 | 2,225 | 0.92× | **Loss** |
| Google Maps Response (prettified) | 3,058 | 2,834 | 1.08× | Win |
| Instruments (minified) | 2,392 | 1,576 | 1.52× | Win |
| Instruments (prettified) | 3,649 | 2,537 | 1.44× | Win |
| Marine IK Reverse (minified) | 747 | 621 | 1.20× | Win |
| Marine IK Reverse (prettified) | 2,668 | 2,194 | 1.22× | Win |
| Marine IK (minified) | 741 | 615 | 1.20× | Win |
| Marine IK (prettified) | 2,593 | 2,190 (n/c) | 1.18× | Win |
| Random (minified) | 1,028 | 1,136 | 0.90× | **Loss** |
| Random (prettified) | 1,678 | 1,704 | 0.99× | **Loss** |
| Twitter (minified) | 1,768 | 1,643 | 1.08× | Win |
| Twitter (prettified) | 2,124 | 2,066 | 1.03× | Win |
| Apache Builds Small (minified) | 2,076 | 2,070 | 1.00× | Win |
| Canada Small (minified) | 831 | 688 | 1.21× | Win |
| Apache Builds Small (prettified) | 2,608 | 2,458 | 1.06× | Win |
| Canada Small (prettified) | 1,873 | 1,649 | 1.14× | Win |
| CitmCatalog Small (minified) | 1,542 | 1,199 | 1.29× | Win |
| CitmCatalog Small (prettified) | 2,521 | 2,235 | 1.13× | Win |
| Discord Small (minified) | 1,509 | 1,677 | 0.90× | **Loss** |
| Discord Small (prettified) | 2,000 | 2,019 | 0.99× | **Loss** |
| Github Events Small (minified) | 1,589 | 1,770 | 0.90× | **Loss** |
| Google Maps Response Small (minified) | 1,563 | 1,406 | 1.11× | Win |
| Github Events Small (prettified) | 2,097 | 2,310 | 0.91× | **Loss** |
| Google Maps Response Small (prettified) | 3,004 | 2,787 | 1.08× | Win |
| Instruments Small (minified) | 2,422 | 1,488 | 1.63× | Win |
| Instruments Small (prettified) | 3,690 | 2,334 | 1.58× | Win |
| Marine IK Reverse Small (minified) | 770 | 665 | 1.16× | Win |
| Marine IK Reverse Small (prettified) | 2,629 | 2,314 | 1.14× | Win |
| Marine IK Small (minified) | 829 | 668 | 1.24× | Win |
| Marine IK Small (prettified) | 2,903 | 2,367 | 1.23× | Win |
| Random Small (minified) | 1,377 | 1,474 | 0.93× | **Loss** |
| Random Small (prettified) | 2,206 | 2,207 | 1.00× | Tie |
| Twitter Small (minified) | 1,976 | 1,973 | 1.00× | Tie |
| Twitter Small (prettified) | 2,494 | 2,488 | 1.00× | Tie |

#### Reused objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,735 | 1,806 | 0.96× | **Loss** |
| Canada (minified) | 879 | 782 | 1.12× | Win |
| Apache Builds (prettified) | 2,210 | 2,312 | 0.96× | **Loss** |
| Canada (prettified) | 1,960 | 1,785 | 1.10× | Win |
| CitmCatalog (minified) | 2,137 | 1,745 | 1.22× | Win |
| CitmCatalog (prettified) | 4,166 | 3,494 | 1.19× | Win |
| Discord (minified) | 2,060 | 1,897 | 1.09× | Win |
| Discord (prettified) | 2,836 | 2,696 | 1.05× | Win |
| Github Events (minified) | 2,052 | 2,019 | 1.02× | Win |
| Google Maps Response (minified) | 1,689 | 1,483 | 1.14× | Win |
| Github Events (prettified) | 2,542 | 2,347 | 1.08× | Win |
| Google Maps Response (prettified) | 3,075 | 2,964 | 1.04× | Win |
| Instruments (minified) | 2,682 | 1,582 | 1.70× | Win |
| Instruments (prettified) | 4,014 | 2,489 | 1.61× | Win |
| Marine IK Reverse (minified) | 842 | 690 | 1.22× | Win |
| Marine IK Reverse (prettified) | 2,847 | 2,427 | 1.17× | Win |
| Marine IK (minified) | 812 | 680 | 1.19× | Win |
| Marine IK (prettified) | 2,802 | 2,400 | 1.17× | Win |
| Random (minified) | 1,305 | 1,466 | 0.89× | **Loss** |
| Random (prettified) | 2,101 | 2,263 | 0.93× | **Loss** |
| Twitter (minified) | 2,143 | 1,966 | 1.09× | Win |
| Twitter (prettified) | 2,677 | 2,424 | 1.10× | Win |
| Apache Builds Small (minified) | 2,246 | 2,265 | 0.99× | **Loss** |
| Canada Small (minified) | 1,036 | 763 | 1.36× | Win |
| Apache Builds Small (prettified) | 2,783 | 2,721 | 1.02× | Win |
| Canada Small (prettified) | 2,190 | 1,773 | 1.24× | Win |
| CitmCatalog Small (minified) | 1,738 | 1,355 | 1.28× | Win |
| CitmCatalog Small (prettified) | 2,874 | 2,468 | 1.16× | Win |
| Discord Small (minified) | 1,803 | 1,721 | 1.05× | Win |
| Discord Small (prettified) | 2,338 | 2,494 | 0.94× | **Loss** |
| Github Events Small (minified) | 2,293 | 2,037 | 1.13× | Win |
| Google Maps Response Small (minified) | 1,710 | 1,518 | 1.13× | Win |
| Github Events Small (prettified) | 2,757 | 2,447 | 1.13× | Win |
| Google Maps Response Small (prettified) | 3,228 | 3,043 | 1.06× | Win |
| Instruments Small (minified) | 2,639 | 1,537 | 1.72× | Win |
| Instruments Small (prettified) | 4,102 | 2,284 | 1.80× | Win |
| Marine IK Reverse Small (minified) | 900 | 702 | 1.28× | Win |
| Marine IK Reverse Small (prettified) | 3,134 | 2,563 | 1.22× | Win |
| Marine IK Small (minified) | 984 | 746 | 1.32× | Win |
| Marine IK Small (prettified) | 3,373 | 2,615 | 1.29× | Win |
| Random Small (minified) | 1,556 | 1,590 | 0.98× | **Loss** |
| Random Small (prettified) | 2,496 | 2,393 | 1.04× | Win |
| Twitter Small (minified) | 2,253 | 2,079 | 1.08× | Win |
| Twitter Small (prettified) | 2,699 | 2,676 | 1.01× | Win |

#### Freshly allocated objects: partial reading and validation

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (minified) | 7,747 | 2,995 | 2.59× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,420 | 3,675 | 2.56× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,090 | 2,774 | 1.83× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 5,834 | 3,366 | 1.73× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 9,695 | 7,647 | 1.27× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 11,332 | 8,529 | 1.33× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,007 | 6,397 | 0.94× | Loss |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,089 | 7,408 | 0.96× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,113 | 8,089 | 1.00× | Tie |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,969 | 8,779 | 1.14× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,012 | 7,025 | 0.86× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,179 | 7,627 | 0.94× | Loss |

**Reused objects.**

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (minified) | 7,936 | 3,089 | 2.57× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,248 | 3,732 | 2.48× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,474 | 2,936 | 1.86× | Win |
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,211 | 3,486 | 1.78× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 10,500 | 7,445 | 1.41× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 11,078 | 8,755 | 1.27× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,366 | 6,503 | 0.98× | Loss |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,703 | 7,496 | 1.03× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,735 | 8,255 | 1.06× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 10,039 | 8,865 | 1.13× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,132 | 7,120 | 0.86× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,582 | 7,616 | 1.00× | Loss |

**Validation.**

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (EPYC 9V45, AVX-512) | 3,079 | 2,504 | 1.23× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 3,792 | 2,377 | 1.59× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 3,133 | 2,420 | 1.29× | Win |

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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 165 of 245 freshly allocated tests: all 25 POD-type tests, 105 of 110 minified documents and 33 of 44 prettified documents on the M2 Pro, while the two-stage path wins 64 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 242 of 245 tests freshly allocated and all 245 reused, its stage-1 + reflection path beats simdjson's reflection reader on 75 of 88, and on partial reading it wins 36 of 40. On the three AVX-512 builds the faster path beats simdjson On Demand on 146 of 147 tests freshly allocated and 144 of 147 reused, and its stage-1 + reflection path beats simdjson's reflection reader on 29 of 44 freshly allocated and 37 of 44 reused. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier [4]. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance [5].*

## References

1. G. Langdale, D. Lemire. "Parsing Gigabytes of JSON per Second." *The VLDB Journal*, 28(6), 2019.
2. simdjson. https://github.com/simdjson/simdjson (commit 36c5b3cfc).
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
