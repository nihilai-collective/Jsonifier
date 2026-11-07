# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on all 110 minified documents, the tape is faster on 56 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 36 of the 44 prettified documents, the tape 5 and 3 are ties, with 3 of the tape's 5 wins on the Small copies.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 7, 2026, with Jsonifier [3e5ab01](https://github.com/nihilai-collective/jsonifier/commit/3e5ab01), Glaze [0a363d4](https://github.com/stephenberry/glaze/commit/0a363d40), simdjson [dfe02cf](https://github.com/simdjson/simdjson/commit/dfe02cfe3) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Five freshly allocated full-parse rows contain non-converged results (three on Windows/MSVC and one on each macOS build), and four reused rows do (three on Windows/MSVC and one on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 177 tests, 7 are statistical ties, and the two-stage path wins 61. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 32 | 3 | 14 | 46 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 28 | 0 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 46 | 1 | 2 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 44 | 2 | 3 | 48 of 49 |
| **Aggregate** | **177** | **7** | **61** | **240 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 2 / 3 / 6 | 1 / 1 / 20 | 19 / 1 / 2 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 3 / 0 / 8 | 0 / 0 / 22 | 17 / 2 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 / 0 / 2 | 43 / 0 / 6 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **243 / 0 / 2** | **239 / 0 / 6** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 31 | 0 | 18 | 46 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 26 | 1 | 22 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 46 | 0 | 3 | 49 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 43 | 2 | 4 | 48 of 49 |
| **Aggregate** | **173** | **3** | **69** | **241 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 20 / 0 / 2 | 21 / 1 / 0 |
| Prettified corpus documents | 2 / 0 / 9 | 1 / 1 / 20 | 20 / 1 / 1 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 16 / 0 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 46 / 1 / 2 | 44 / 0 / 5 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **242 / 1 / 2** | **240 / 0 / 5** |

With objects reused, across 245 tests the fused path wins 173, ties 3 and loses 69 to the two-stage path, and the x86 split holds: the tape wins 29 of the 33 full-size prettified documents (the fused path takes Apache Builds and Github Events under MSVC and Canada on Linux/Clang, and Canada on Linux/GCC is a tie) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 31 of the 33 full-size minified ones (the tape takes Github Events on Linux/Clang, 2,450 against 2,244 MB/s, and on Linux/GCC, 2,305 against 2,174). On the M2 Pro the fused path wins 43 of the 44 minified documents, with Marine IK on macOS/Clang a tie, and 36 of the 44 prettified ones, with 1 tie and 7 tape wins. Against simdjson, the faster Jsonifier path wins 242, ties 1 and loses 2, and the fused path alone wins 240, ties none and loses 5.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 478 | 267 | 69 | 1.79× | Fused | Win |
| Double (POD) | 638 | 477 | 148 | 1.34× | Fused | Win |
| Int64 (POD) | 1,461 | 850 | 307 | 1.72× | Fused | Win |
| String (POD) | 983 | 788 | 554 | 1.25× | Fused | Win |
| Uint64 (POD) | 1,550 | 900 | 310 | 1.72× | Fused | Win |
| Apache Builds (minified) | 870 | 782 | 671 | 1.11× | Fused | Win |
| Canada (minified) | 693 | 658 | 435 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,035 | 1,013 | 815 | 1.02× | Tie | Win |
| Canada (prettified) | 1,415 | 1,507 | 962 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,485 | 1,227 | 576 | 1.21× | Fused | Win |
| CitmCatalog (prettified) | 2,368 | 2,513 (n/c) | 1,212 | 0.94× | Two-stage | Win |
| Discord (minified) | 1,288 | 1,042 | 678 | 1.24× | Fused | Win |
| Discord (prettified) | 1,360 | 1,436 | 972 | 0.95× | Two-stage | Win |
| Github Events (minified) | 1,069 | 919 | 640 | 1.16× | Fused | Win |
| Google Maps Response (minified) | 1,355 | 1,207 | 533 | 1.12× | Fused | Win |
| Github Events (prettified) | 1,040 | 1,005 | 722 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,142 | 2,354 | 1,050 | 0.91× | Two-stage | Win |
| Instruments (minified) | 2,116 | 1,425 | 741 | 1.49× | Fused | Win |
| Instruments (prettified) | 2,111 | 2,277 | 1,186 | 0.93× | Two-stage | Win |
| Marine IK Reverse (minified) | 583 | 516 | 194 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 1,734 | 1,883 | 675 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 567 | 489 | 343 | 1.16× | Fused | Win |
| Marine IK (prettified) | 1,713 (n/c) | 1,749 | 1,162 | 0.98× | Tie | Win |
| Random (minified) | 820 | 716 | 561 | 1.15× | Fused | Win |
| Random (prettified) | 1,048 (n/c) | 1,025 | 828 | 1.02× | Fused | Win |
| Twitter (minified) | 1,222 | 1,122 | 855 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,403 | 1,389 | 1,096 | 1.01× | Tie | Win |
| Apache Builds Small (minified) | 1,143 | 925 | 732 | 1.24× | Fused | Win |
| Canada Small (minified) | 624 | 592 | 344 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,235 | 1,159 | 914 | 1.07× | Fused | Win |
| Canada Small (prettified) | 1,305 | 1,381 | 750 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 794 | 685 | 495 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 1,143 | 1,200 | 868 | 0.95× | Two-stage | Win |
| Discord Small (minified) | 1,177 | 906 | 672 | 1.30× | Fused | Win |
| Discord Small (prettified) | 1,221 | 1,211 | 876 | 1.01× | Fused | Win |
| Github Events Small (minified) | 1,069 | 771 | 611 | 1.39× | Fused | Win |
| Google Maps Response Small (minified) | 1,205 | 1,085 | 528 | 1.11× | Fused | Win |
| Github Events Small (prettified) | 1,089 | 988 | 743 | 1.10× | Fused | Win |
| Google Maps Response Small (prettified) | 1,997 | 2,161 | 1,041 | 0.92× | Two-stage | Win |
| Instruments Small (minified) | 2,041 | 1,399 | 671 | 1.46× | Fused | Win |
| Instruments Small (prettified) | 2,036 | 2,197 | 1,145 | 0.93× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 442 | 414 | 154 | 1.07× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,305 | 1,513 | 587 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 489 | 432 | 302 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,495 | 1,605 | 1,067 | 0.93× | Two-stage | Win |
| Random Small (minified) | 907 | 789 | 479 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,165 | 1,207 | 794 | 0.96× | Two-stage | Win |
| Twitter Small (minified) | 1,462 | 1,198 | 1,121 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 1,481 | 1,560 | 1,426 | 0.95× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,966 | 624 | 283 | 3.15× | Fused | Win |
| Double (POD) | 1,299 | 739 | 353 | 1.76× | Fused | Win |
| Int64 (POD) | 3,074 | 1,671 | 785 | 1.84× | Fused | Win |
| String (POD) | 1,831 | 1,317 | 1,392 | 1.39× | Fused | Win |
| Uint64 (POD) | 3,246 | 1,588 | 714 | 2.04× | Fused | Win |
| Apache Builds (minified) | 1,666 | 1,462 | 1,192 | 1.14× | Fused | Win |
| Canada (minified) | 1,037 | 902 | 688 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 1,697 | 1,801 | 1,545 | 0.94× | Two-stage | Win |
| Canada (prettified) | 2,042 | 1,972 | 1,582 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,215 | 1,660 | 1,023 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 2,817 | 3,459 | 2,206 | 0.81× | Two-stage | Win |
| Discord (minified) | 2,201 | 1,707 | 949 | 1.29× | Fused | Win |
| Discord (prettified) | 1,740 | 2,492 | 1,385 | 0.70× | Two-stage | Win |
| Github Events (minified) | 1,634 | 1,557 | 965 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,397 | 1,803 | 1,060 | 1.33× | Fused | Win |
| Github Events (prettified) | 1,533 | 1,928 | 1,258 | 0.79× | Two-stage | Win |
| Google Maps Response (prettified) | 2,896 | 3,447 | 2,159 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,465 | 1,965 | 772 | 1.25× | Fused | Win |
| Instruments (prettified) | 2,160 | 2,968 | 1,280 | 0.73× | Two-stage | Win |
| Marine IK Reverse (minified) | 845 | 725 | 565 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,462 | 2,600 | 2,070 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 833 | 722 | 553 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,425 | 2,546 | 1,928 | 0.95× | Two-stage | Win |
| Random (minified) | 1,277 | 1,111 | 811 | 1.15× | Fused | Win |
| Random (prettified) | 1,388 | 1,651 | 1,240 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,876 | 1,676 | 913 | 1.12× | Fused | Win |
| Twitter (prettified) | 1,533 | 2,176 | 1,235 | 0.70× | Two-stage | Win |
| Apache Builds Small (minified) | 2,520 | 2,174 | 1,401 | 1.16× | Fused | Win |
| Canada Small (minified) | 984 | 900 | 673 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,121 | 2,468 | 1,718 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 1,900 | 2,091 | 1,673 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,600 | 1,324 | 938 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,756 | 2,297 | 1,666 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 1,987 | 1,673 | 758 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,601 | 2,287 | 1,032 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 1,994 | 1,818 | 1,364 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 2,241 | 1,686 | 1,053 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,906 | 2,245 | 1,713 | 0.85× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,630 | 3,109 | 2,138 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 2,469 | 1,933 | 668 | 1.28× | Fused | Win |
| Instruments Small (prettified) | 2,073 | 2,895 | 1,132 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 783 | 689 | 495 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,125 | 2,527 | 1,812 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 866 | 755 | 486 | 1.15× | Fused | Win |
| Marine IK Small (prettified) | 2,163 | 2,642 | 1,786 | 0.82× | Two-stage | Win |
| Random Small (minified) | 1,676 | 1,516 | 832 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,663 | 2,331 | 1,304 | 0.71× | Two-stage | Win |
| Twitter Small (minified) | 2,497 | 2,110 | 1,019 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 1,822 | 2,712 | 1,325 | 0.67× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,364 | 609 | 224 | 3.88× | Fused | Win |
| Double (POD) | 1,213 | 715 | 295 | 1.70× | Fused | Win |
| Int64 (POD) | 3,307 | 1,606 | 725 | 2.06× | Fused | Win |
| String (POD) | 1,798 | 1,286 | 1,235 | 1.40× | Fused | Win |
| Uint64 (POD) | 3,357 | 1,618 | 705 | 2.07× | Fused | Win |
| Apache Builds (minified) | 1,615 | 1,403 | 1,302 | 1.15× | Fused | Win |
| Canada (minified) | 1,133 | 982 | 823 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 1,727 | 1,752 | 1,653 | 0.99× | Two-stage | Win |
| Canada (prettified) | 2,196 | 2,212 | 1,836 | 0.99× | Tie | Win |
| CitmCatalog (minified) | 2,304 | 1,776 | 1,430 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,262 | 3,574 | 3,028 | 0.91× | Two-stage | Win |
| Discord (minified) | 2,314 | 1,847 | 1,353 | 1.25× | Fused | Win |
| Discord (prettified) | 2,239 | 2,511 | 1,915 | 0.89× | Two-stage | Win |
| Github Events (minified) | 1,619 | 1,581 | 1,414 | 1.02× | Fused | Win |
| Google Maps Response (minified) | 2,429 | 1,783 | 1,111 | 1.36× | Fused | Win |
| Github Events (prettified) | 1,652 | 1,931 | 1,693 | 0.86× | Two-stage | Win |
| Google Maps Response (prettified) | 3,212 | 3,396 | 2,228 | 0.95× | Two-stage | Win |
| Instruments (minified) | 3,203 | 2,242 | 1,625 | 1.43× | Fused | Win |
| Instruments (prettified) | 2,917 | 3,298 | 2,585 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 840 | 732 | 567 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,487 | 2,577 | 2,136 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 851 | 735 | 581 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,503 | 2,596 | 2,105 | 0.96× | Two-stage | Win |
| Random (minified) | 1,170 | 1,087 | 1,050 | 1.08× | Fused | Win |
| Random (prettified) | 1,525 | 1,619 | 1,568 | 0.94× | Two-stage | Win |
| Twitter (minified) | 2,003 | 1,673 | 1,501 | 1.20× | Fused | Win |
| Twitter (prettified) | 1,878 | 2,186 | 1,943 | 0.86× | Two-stage | Win |
| Apache Builds Small (minified) | 2,294 | 2,076 | 1,680 | 1.11× | Fused | Win |
| Canada Small (minified) | 1,053 | 889 | 755 | 1.18× | Fused | Win |
| Apache Builds Small (prettified) | 2,231 | 2,447 | 2,076 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 2,070 | 2,130 | 1,777 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,692 | 1,434 | 1,239 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 2,106 | 2,363 | 2,089 | 0.89× | Two-stage | Win |
| Discord Small (minified) | 1,956 | 1,648 | 1,326 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,948 | 2,157 | 1,751 | 0.90× | Two-stage | Win |
| Github Events Small (minified) | 1,893 | 1,726 | 1,983 | 1.10× | Fused | Loss |
| Google Maps Response Small (minified) | 2,279 | 1,669 | 1,134 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 1,991 | 2,186 | 2,344 | 0.91× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,014 | 3,227 | 2,285 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,273 | 2,191 | 1,469 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,850 | 3,283 | 2,379 | 0.87× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 841 | 720 | 549 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,249 | 2,561 | 1,998 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 919 | 768 | 563 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,363 | 2,659 | 2,085 | 0.89× | Two-stage | Win |
| Random Small (minified) | 1,551 | 1,506 | 1,147 | 1.03× | Fused | Win |
| Random Small (prettified) | 1,961 | 2,217 | 1,758 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,828 | 2,324 | 2,022 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 2,376 | 2,943 | 2,546 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,596 | 357 (n/c) | 186 | 4.48× | Fused | Win |
| Double (POD) | 951 | 528 | 162 | 1.80× | Fused | Win |
| Int64 (POD) | 2,645 | 965 | 465 | 2.74× | Fused | Win |
| String (POD) | 1,380 | 769 | 777 | 1.80× | Fused | Win |
| Uint64 (POD) | 2,600 | 791 | 453 | 3.29× | Fused | Win |
| Apache Builds (minified) | 1,263 | 1,031 | 773 | 1.23× | Fused | Win |
| Canada (minified) | 908 | 776 | 589 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 1,406 | 1,287 | 993 | 1.09× | Fused | Win |
| Canada (prettified) | 1,809 | 1,588 | 1,277 | 1.14× | Fused | Win |
| CitmCatalog (minified) | 2,101 | 1,415 | 1,180 | 1.48× | Fused | Win |
| CitmCatalog (prettified) | 3,077 | 2,682 | 2,271 | 1.15× | Fused | Win |
| Discord (minified) | 1,867 | 1,277 | 1,011 | 1.46× | Fused | Win |
| Discord (prettified) | 2,042 | 1,758 | 1,417 | 1.16× | Fused | Win |
| Github Events (minified) | 1,689 | 1,454 | 1,048 | 1.16× | Fused | Win |
| Google Maps Response (minified) | 1,931 | 1,312 | 773 | 1.47× | Fused | Win |
| Github Events (prettified) | 1,761 | 1,660 | 1,242 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,792 | 1,877 | 1,476 | 1.49× | Fused | Win |
| Instruments (minified) | 2,705 | 1,675 | 1,239 | 1.62× | Fused | Win |
| Instruments (prettified) | 2,906 | 2,431 | 1,889 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | 705 | 586 | 429 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,119 | 1,974 | 1,678 | 1.07× | Fused | Win |
| Marine IK (minified) | 717 | 594 | 485 | 1.21× | Fused | Win |
| Marine IK (prettified) | 2,185 | 1,982 | 1,669 | 1.10× | Fused | Win |
| Random (minified) | 1,051 | 957 | 731 | 1.10× | Fused | Win |
| Random (prettified) | 1,395 | 1,231 | 1,075 | 1.13× | Fused | Win |
| Twitter (minified) | 2,086 | 1,711 | 1,053 | 1.22× | Fused | Win |
| Twitter (prettified) | 2,170 | 2,040 | 1,362 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 1,586 | 1,288 | 910 | 1.23× | Fused | Win |
| Canada Small (minified) | 819 | 720 | 486 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 1,677 | 1,737 | 1,132 | 0.97× | Two-stage | Win |
| Canada Small (prettified) | 1,693 | 1,551 | 984 | 1.09× | Fused | Win |
| CitmCatalog Small (minified) | 1,303 | 1,043 | 858 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,822 | 1,676 | 1,439 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,713 | 1,327 | 936 | 1.29× | Fused | Win |
| Discord Small (prettified) | 1,820 | 1,700 | 1,252 | 1.07× | Fused | Win |
| Github Events Small (minified) | 1,541 | 1,306 | 1,184 | 1.18× | Fused | Win |
| Google Maps Response Small (minified) | 1,796 | 1,272 | 781 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 1,693 | 1,547 | 1,476 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,489 | 2,323 | 1,501 | 1.07× | Fused | Win |
| Instruments Small (minified) | 2,792 | 1,732 | 1,166 | 1.61× | Fused | Win |
| Instruments Small (prettified) | 2,938 | 2,488 | 1,794 | 1.18× | Fused | Win |
| Marine IK Reverse Small (minified) | 655 | 568 | 401 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,798 | 1,886 | 1,416 | 0.95× | Two-stage | Win |
| Marine IK Small (minified) | 701 | 593 | 399 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,972 | 1,986 | 1,411 | 0.99× | Tie | Win |
| Random Small (minified) | 1,272 | 1,130 | 752 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,605 | 1,413 | 1,008 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,338 | 1,891 | 1,158 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,307 | 2,223 | 1,460 | 1.04× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,271 | 372 | 177 | 3.42× | Fused | Win |
| Double (POD) | 926 | 377 | 212 | 2.46× | Fused | Win |
| Int64 (POD) | 2,278 | 850 | 488 | 2.68× | Fused | Win |
| String (POD) | 1,373 | 748 | 885 | 1.84× | Fused | Win |
| Uint64 (POD) | 2,342 | 832 | 477 | 2.82× | Fused | Win |
| Apache Builds (minified) | 1,539 | 1,104 | 887 | 1.39× | Fused | Win |
| Canada (minified) | 838 | 742 | 536 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,656 | 1,379 | 1,130 | 1.20× | Fused | Win |
| Canada (prettified) | 1,681 | 1,590 | 1,197 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,146 | 1,452 | 743 | 1.48× | Fused | Win |
| CitmCatalog (prettified) | 2,509 | 2,495 | 1,551 | 1.01× | Tie | Win |
| Discord (minified) | 1,992 | 1,177 | 645 | 1.69× | Fused | Win |
| Discord (prettified) | 2,371 | 1,460 | 951 | 1.62× | Fused | Win |
| Github Events (minified) | 1,857 | 1,494 | 886 | 1.24× | Fused | Win |
| Google Maps Response (minified) | 1,949 | 1,169 | 802 | 1.67× | Fused | Win |
| Github Events (prettified) | 1,824 | 1,725 | 1,060 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,541 | 2,129 | 1,513 | 1.19× | Fused | Win |
| Instruments (minified) | 2,720 | 1,475 | 558 | 1.84× | Fused | Win |
| Instruments (prettified) | 2,615 | 2,317 | 973 | 1.13× | Fused | Win |
| Marine IK Reverse (minified) | 580 | 497 | 324 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 1,692 | 1,844 | 1,179 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 613 | 510 | 386 | 1.20× | Fused | Win |
| Marine IK (prettified) | 2,048 | 2,066 | 1,472 | 0.99× | Two-stage | Win |
| Random (minified) | 1,285 | 995 | 665 | 1.29× | Fused | Win |
| Random (prettified) | 1,616 | 1,240 | 1,002 | 1.30× | Fused | Win |
| Twitter (minified) | 2,313 | 1,773 | 845 | 1.30× | Fused | Win |
| Twitter (prettified) | 2,248 | 1,749 | 983 | 1.29× | Fused | Win |
| Apache Builds Small (minified) | 1,386 | 1,253 | 858 | 1.11× | Fused | Win |
| Canada Small (minified) | 728 | 634 | 433 | 1.15× | Fused | Win |
| Apache Builds Small (prettified) | 1,560 | 1,526 | 1,079 | 1.02× | Fused | Win |
| Canada Small (prettified) | 1,473 | 1,393 | 973 | 1.06× | Fused | Win |
| CitmCatalog Small (minified) | 1,169 | 864 | 615 | 1.35× | Fused | Win |
| CitmCatalog Small (prettified) | 1,575 | 1,525 | 1,126 | 1.03× | Fused | Win |
| Discord Small (minified) | 2,066 | 1,137 | 623 | 1.82× | Fused | Win |
| Discord Small (prettified) | 2,020 | 1,536 | 801 | 1.31× | Fused | Win |
| Github Events Small (minified) | 1,400 | 1,165 | 942 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 2,021 | 1,136 | 768 | 1.78× | Fused | Win |
| Github Events Small (prettified) | 1,535 | 1,414 | 1,215 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,575 | 1,875 | 1,511 | 1.37× | Fused | Win |
| Instruments Small (minified) | 2,669 | 1,614 | 557 | 1.65× | Fused | Win |
| Instruments Small (prettified) | 2,734 | 2,397 | 880 | 1.14× | Fused | Win |
| Marine IK Reverse Small (minified) | 635 | 570 | 332 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,738 | 1,759 | 1,138 | 0.99× | Two-stage | Win |
| Marine IK Small (minified) | 629 | 552 | 283 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 1,889 | 1,778 | 1,139 | 1.06× | Fused | Win |
| Random Small (minified) | 1,499 | 1,120 | 670 | 1.34× | Fused | Win |
| Random Small (prettified) | 1,776 | 1,383 | 1,026 | 1.28× | Fused | Win |
| Twitter Small (minified) | 2,217 | 1,723 | 771 | 1.29× | Fused | Win |
| Twitter Small (prettified) | 1,857 (n/c) | 1,870 | 908 | 0.99× | Tie | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 540 | 287 | 72 | 1.88× | Fused | Win |
| Double (POD) | 838 | 579 | 157 | 1.45× | Fused | Win |
| Int64 (POD) | 1,973 | 994 | 326 | 1.98× | Fused | Win |
| String (POD) | 1,678 | 1,182 | 724 | 1.42× | Fused | Win |
| Uint64 (POD) | 1,841 | 1,049 | 326 | 1.75× | Fused | Win |
| Apache Builds (minified) | 1,420 | 1,199 | 982 | 1.18× | Fused | Win |
| Canada (minified) | 845 | 798 | 486 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,611 | 1,551 | 1,238 | 1.04× | Fused | Win |
| Canada (prettified) | 1,616 | 1,773 | 1,066 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,798 | 1,449 | 604 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,653 | 2,940 (n/c) | 1,204 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,905 | 1,423 | 851 | 1.34× | Fused | Win |
| Discord (prettified) | 1,828 | 1,990 (n/c) | 1,176 | 0.92× | Two-stage | Win |
| Github Events (minified) | 1,637 | 1,308 | 806 | 1.25× | Fused | Win |
| Google Maps Response (minified) | 1,516 | 1,339 | 566 | 1.13× | Fused | Win |
| Github Events (prettified) | 1,567 | 1,494 | 945 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,375 | 2,462 | 1,155 | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,580 | 1,769 | 839 | 1.46× | Fused | Win |
| Instruments (prettified) | 2,544 | 2,680 | 1,329 | 0.95× | Two-stage | Win |
| Marine IK Reverse (minified) | 637 | 559 | 201 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 1,935 | 2,014 | 712 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 646 | 541 | 357 | 1.19× | Fused | Win |
| Marine IK (prettified) | 1,851 | 1,965 | 1,315 | 0.94× | Two-stage | Win |
| Random (minified) | 1,088 | 932 | 644 | 1.17× | Fused | Win |
| Random (prettified) | 1,324 | 1,456 | 980 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,892 | 1,568 | 1,101 | 1.21× | Fused | Win |
| Twitter (prettified) | 1,877 | 1,975 | 1,381 | 0.95× | Two-stage | Win |
| Apache Builds Small (minified) | 1,582 | 1,215 | 990 | 1.30× | Fused | Win |
| Canada Small (minified) | 787 | 738 | 383 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,633 | 1,476 | 1,227 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,550 | 1,721 | 887 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,131 | 893 | 572 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 1,443 | 1,531 | 998 | 0.94× | Two-stage | Win |
| Discord Small (minified) | 1,625 | 1,256 | 892 | 1.29× | Fused | Win |
| Discord Small (prettified) | 1,519 | 1,632 | 1,178 | 0.93× | Two-stage | Win |
| Github Events Small (minified) | 1,512 | 1,060 | 757 | 1.43× | Fused | Win |
| Google Maps Response Small (minified) | 1,461 | 1,274 | 588 | 1.15× | Fused | Win |
| Github Events Small (prettified) | 1,462 | 1,339 | 945 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,240 | 2,421 | 1,156 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 2,400 | 1,555 | 796 | 1.54× | Fused | Win |
| Instruments Small (prettified) | 2,185 | 2,286 | 1,272 | 0.96× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 536 | 492 | 163 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,539 | 1,862 | 593 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 612 | 539 | 345 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,753 | 1,962 | 1,198 | 0.89× | Two-stage | Win |
| Random Small (minified) | 1,107 | 972 | 640 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,357 | 1,449 | 969 | 0.94× | Two-stage | Win |
| Twitter Small (minified) | 2,007 | 1,496 | 1,312 (n/c) | 1.34× | Fused | Win |
| Twitter Small (prettified) | 1,818 | 1,937 | 1,632 | 0.94× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,171 | 656 | 279 | 3.31× | Fused | Win |
| Double (POD) | 1,430 | 804 | 361 | 1.78× | Fused | Win |
| Int64 (POD) | 3,451 | 1,779 | 792 | 1.94× | Fused | Win |
| String (POD) | 2,884 | 1,801 | 1,923 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,539 | 1,664 | 723 | 2.13× | Fused | Win |
| Apache Builds (minified) | 2,756 | 2,307 | 1,665 | 1.19× | Fused | Win |
| Canada (minified) | 1,270 | 1,064 | 802 | 1.19× | Fused | Win |
| Apache Builds (prettified) | 2,412 | 2,805 | 2,145 | 0.86× | Two-stage | Win |
| Canada (prettified) | 2,397 | 2,341 | 1,839 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,704 | 1,944 | 1,159 | 1.39× | Fused | Win |
| CitmCatalog (prettified) | 3,129 | 4,007 | 2,465 | 0.78× | Two-stage | Win |
| Discord (minified) | 3,117 | 2,353 | 1,121 | 1.32× | Fused | Win |
| Discord (prettified) | 2,123 | 3,293 | 1,616 | 0.64× | Two-stage | Win |
| Github Events (minified) | 2,244 | 2,450 | 1,151 | 0.92× | Two-stage | Win |
| Google Maps Response (minified) | 2,763 | 1,986 | 1,114 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,952 | 2,481 | 1,385 | 0.79× | Two-stage | Win |
| Google Maps Response (prettified) | 3,113 | 3,739 | 2,277 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,938 | 2,214 | 805 | 1.33× | Fused | Win |
| Instruments (prettified) | 2,353 | 3,384 | 1,340 | 0.70× | Two-stage | Win |
| Marine IK Reverse (minified) | 942 | 795 | 618 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,653 | 2,761 | 2,268 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 935 | 799 | 593 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,651 | 2,741 | 2,138 | 0.97× | Two-stage | Win |
| Random (minified) | 1,803 | 1,505 | 1,004 | 1.20× | Fused | Win |
| Random (prettified) | 1,745 | 2,210 | 1,503 | 0.79× | Two-stage | Win |
| Twitter (minified) | 2,383 | 2,172 | 1,035 | 1.10× | Fused | Win |
| Twitter (prettified) | 1,801 | 2,749 | 1,391 | 0.66× | Two-stage | Win |
| Apache Builds Small (minified) | 2,926 | 2,470 | 1,652 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,251 | 1,114 | 797 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,380 | 2,926 | 2,036 | 0.81× | Two-stage | Win |
| Canada Small (prettified) | 2,251 | 2,501 | 1,876 | 0.90× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,992 | 1,588 | 1,076 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 1,999 | 2,684 | 1,856 | 0.74× | Two-stage | Win |
| Discord Small (minified) | 2,687 | 2,111 | 867 | 1.27× | Fused | Win |
| Discord Small (prettified) | 1,937 | 2,792 | 1,176 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,626 | 2,266 | 1,616 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,619 | 1,891 | 1,176 | 1.38× | Fused | Win |
| Github Events Small (prettified) | 2,257 | 2,698 | 1,988 | 0.84× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,841 | 3,457 | 2,331 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,705 | 2,021 | 697 | 1.34× | Fused | Win |
| Instruments Small (prettified) | 2,220 | 3,119 | 1,153 | 0.71× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 987 | 824 | 562 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,440 | 2,921 | 2,002 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 1,069 | 923 | 541 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,497 | 3,119 | 2,032 | 0.80× | Two-stage | Win |
| Random Small (minified) | 2,144 | 1,889 | 976 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,900 | 2,803 | 1,505 | 0.68× | Two-stage | Win |
| Twitter Small (minified) | 2,805 | 2,370 | 1,061 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,024 | 2,975 | 1,363 | 0.68× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,625 | 621 | 221 | 4.23× | Fused | Win |
| Double (POD) | 1,314 | 752 | 306 | 1.75× | Fused | Win |
| Int64 (POD) | 3,650 | 1,724 | 744 | 2.12× | Fused | Win |
| String (POD) | 2,761 | 1,717 | 1,638 | 1.61× | Fused | Win |
| Uint64 (POD) | 3,703 | 1,716 | 717 | 2.16× | Fused | Win |
| Apache Builds (minified) | 2,519 | 2,080 | 1,811 | 1.21× | Fused | Win |
| Canada (minified) | 1,423 | 1,201 | 995 | 1.18× | Fused | Win |
| Apache Builds (prettified) | 2,437 | 2,587 | 2,313 | 0.94× | Two-stage | Win |
| Canada (prettified) | 2,633 | 2,643 | 2,194 | 1.00× | Tie | Win |
| CitmCatalog (minified) | 2,874 | 2,143 | 1,750 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 3,729 | 4,155 | 3,616 | 0.90× | Two-stage | Win |
| Discord (minified) | 3,282 | 2,489 | 1,691 | 1.32× | Fused | Win |
| Discord (prettified) | 2,803 | 3,367 | 2,377 | 0.83× | Two-stage | Win |
| Github Events (minified) | 2,174 | 2,305 | 1,750 | 0.94× | Two-stage | Win |
| Google Maps Response (minified) | 2,837 | 1,953 | 1,181 | 1.45× | Fused | Win |
| Github Events (prettified) | 2,329 | 2,525 | 2,077 | 0.92× | Two-stage | Win |
| Google Maps Response (prettified) | 3,464 | 3,685 | 2,363 | 0.94× | Two-stage | Win |
| Instruments (minified) | 3,853 | 2,529 | 1,772 | 1.52× | Fused | Win |
| Instruments (prettified) | 3,231 | 3,696 | 2,785 | 0.87× | Two-stage | Win |
| Marine IK Reverse (minified) | 940 | 787 | 640 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,710 | 2,823 | 2,337 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 964 | 810 | 646 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,741 | 2,801 | 2,325 | 0.98× | Two-stage | Win |
| Random (minified) | 1,588 | 1,449 | 1,418 | 1.10× | Fused | Win |
| Random (prettified) | 1,939 | 2,113 | 2,111 | 0.92× | Two-stage | Tie |
| Twitter (minified) | 2,590 | 2,099 | 1,837 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,256 | 2,727 | 2,362 | 0.83× | Two-stage | Win |
| Apache Builds Small (minified) | 2,682 | 2,407 | 1,961 | 1.11× | Fused | Win |
| Canada Small (minified) | 1,361 | 1,100 | 866 | 1.24× | Fused | Win |
| Apache Builds Small (prettified) | 2,528 | 2,855 | 2,363 | 0.89× | Two-stage | Win |
| Canada Small (prettified) | 2,488 | 2,610 | 2,023 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,055 | 1,637 | 1,400 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 2,389 | 2,803 | 2,369 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,576 | 2,047 | 1,599 | 1.26× | Fused | Win |
| Discord Small (prettified) | 2,378 | 2,679 | 2,092 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,331 | 2,044 | 2,368 | 1.14× | Fused | Loss |
| Google Maps Response Small (minified) | 2,669 | 1,907 | 1,245 | 1.40× | Fused | Win |
| Github Events Small (prettified) | 2,402 | 2,645 | 2,866 | 0.91× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,300 | 3,508 | 2,482 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,541 | 2,326 | 1,593 | 1.52× | Fused | Win |
| Instruments Small (prettified) | 2,975 | 3,513 | 2,543 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,047 | 859 | 617 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,608 | 3,000 | 2,245 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 1,181 | 911 | 615 | 1.30× | Fused | Win |
| Marine IK Small (prettified) | 2,740 | 3,118 | 2,214 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,917 | 1,812 | 1,419 | 1.06× | Fused | Win |
| Random Small (prettified) | 2,280 | 2,654 | 2,157 | 0.86× | Two-stage | Win |
| Twitter Small (minified) | 3,161 | 2,528 | 2,151 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,666 | 3,217 | 2,758 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,764 | 350 | 189 | 5.05× | Fused | Win |
| Double (POD) | 1,184 | 584 | 165 | 2.03× | Fused | Win |
| Int64 (POD) | 3,346 | 1,032 | 476 | 3.24× | Fused | Win |
| String (POD) | 2,378 | 1,012 | 1,025 | 2.35× | Fused | Win |
| Uint64 (POD) | 3,250 | 934 | 464 | 3.48× | Fused | Win |
| Apache Builds (minified) | 2,113 | 1,520 | 984 | 1.39× | Fused | Win |
| Canada (minified) | 1,158 | 948 | 684 | 1.22× | Fused | Win |
| Apache Builds (prettified) | 2,156 | 1,863 | 1,255 | 1.16× | Fused | Win |
| Canada (prettified) | 2,222 | 1,903 | 1,476 | 1.17× | Fused | Win |
| CitmCatalog (minified) | 2,625 | 1,628 | 1,309 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | 3,536 | 3,041 | 2,469 | 1.16× | Fused | Win |
| Discord (minified) | 2,543 | 1,630 | 1,215 | 1.56× | Fused | Win |
| Discord (prettified) | 2,523 | 2,196 | 1,682 | 1.15× | Fused | Win |
| Github Events (minified) | 2,409 | 1,963 | 1,244 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,305 | 1,519 | 839 | 1.52× | Fused | Win |
| Github Events (prettified) | 2,373 | 2,125 | 1,476 | 1.12× | Fused | Win |
| Google Maps Response (prettified) | 3,046 | 2,598 | 1,567 | 1.17× | Fused | Win |
| Instruments (minified) | 3,201 | 1,868 | 1,332 | 1.71× | Fused | Win |
| Instruments (prettified) | 3,229 | 2,657 | 2,019 | 1.22× | Fused | Win |
| Marine IK Reverse (minified) | 812 | 660 | 536 | 1.23× | Fused | Win |
| Marine IK Reverse (prettified) | 2,352 | 2,174 | 1,805 | 1.08× | Fused | Win |
| Marine IK (minified) | 833 | 667 | 533 | 1.25× | Fused | Win |
| Marine IK (prettified) | 2,437 | 2,206 | 1,822 | 1.10× | Fused | Win |
| Random (minified) | 1,552 | 1,398 | 911 | 1.11× | Fused | Win |
| Random (prettified) | 1,925 | 1,824 | 1,343 | 1.06× | Fused | Win |
| Twitter (minified) | 2,781 | 2,152 | 1,203 | 1.29× | Fused | Win |
| Twitter (prettified) | 2,715 | 2,560 | 1,533 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 2,100 | 1,877 | 1,211 | 1.12× | Fused | Win |
| Canada Small (minified) | 1,032 | 882 | 552 | 1.17× | Fused | Win |
| Apache Builds Small (prettified) | 2,119 | 2,195 | 1,506 | 0.97× | Two-stage | Win |
| Canada Small (prettified) | 2,035 | 1,845 | 1,131 | 1.10× | Fused | Win |
| CitmCatalog Small (minified) | 1,870 | 1,382 | 1,098 | 1.35× | Fused | Win |
| CitmCatalog Small (prettified) | 2,337 | 2,170 | 1,808 | 1.08× | Fused | Win |
| Discord Small (minified) | 2,458 | 1,738 | 1,146 | 1.41× | Fused | Win |
| Discord Small (prettified) | 2,408 | 2,184 | 1,558 | 1.10× | Fused | Win |
| Github Events Small (minified) | 2,284 | 1,552 | 1,454 | 1.47× | Fused | Win |
| Google Maps Response Small (minified) | 2,251 | 1,490 | 777 | 1.51× | Fused | Win |
| Github Events Small (prettified) | 2,313 | 2,031 | 1,595 | 1.14× | Fused | Win |
| Google Maps Response Small (prettified) | 2,908 | 2,668 | 1,682 | 1.09× | Fused | Win |
| Instruments Small (minified) | 3,149 | 1,850 | 1,293 | 1.70× | Fused | Win |
| Instruments Small (prettified) | 3,109 | 2,667 | 2,006 | 1.17× | Fused | Win |
| Marine IK Reverse Small (minified) | 840 | 698 | 464 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,103 | 2,256 | 1,608 | 0.93× | Two-stage | Win |
| Marine IK Small (minified) | 913 | 747 | 464 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,362 | 2,391 | 1,603 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,667 | 1,474 | 950 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,943 | 1,630 | 1,238 | 1.19× | Fused | Win |
| Twitter Small (minified) | 2,917 | 2,266 | 1,246 | 1.29× | Fused | Win |
| Twitter Small (prettified) | 2,769 | 2,616 | 1,557 | 1.06× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,372 | 334 | 173 | 4.10× | Fused | Win |
| Double (POD) | 1,103 | 404 | 220 | 2.73× | Fused | Win |
| Int64 (POD) | 2,744 | 919 | 500 | 2.99× | Fused | Win |
| String (POD) | 2,412 | 971 | 1,175 | 2.48× | Fused | Win |
| Uint64 (POD) | 2,824 | 889 | 483 | 3.18× | Fused | Win |
| Apache Builds (minified) | 2,320 | 1,500 | 1,072 | 1.55× | Fused | Win |
| Canada (minified) | 1,037 | 898 | 626 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 2,251 | 1,839 | 1,367 | 1.22× | Fused | Win |
| Canada (prettified) | 2,017 | 1,907 | 1,365 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,634 | 1,673 | 733 | 1.57× | Fused | Win |
| CitmCatalog (prettified) | 2,965 | 2,949 | 1,622 | 1.01× | Tie | Win |
| Discord (minified) | 2,794 | 1,473 | 798 | 1.90× | Fused | Win |
| Discord (prettified) | 2,703 | 1,820 | 1,058 (n/c) | 1.49× | Fused | Win |
| Github Events (minified) | 2,593 | 1,990 | 1,016 | 1.30× | Fused | Win |
| Google Maps Response (minified) | 2,092 | 1,256 | 841 | 1.67× | Fused | Win |
| Github Events (prettified) | 2,404 | 2,254 | 1,215 | 1.07× | Fused | Win |
| Google Maps Response (prettified) | 3,037 | 2,374 | 1,657 | 1.28× | Fused | Win |
| Instruments (minified) | 2,651 | 1,715 | 613 | 1.55× | Fused | Win |
| Instruments (prettified) | 2,907 | 2,426 | 982 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | 641 | 540 | 338 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 1,915 | 2,011 | 1,416 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 676 | 671 | 430 | 1.01× | Tie | Win |
| Marine IK (prettified) | 2,388 | 2,269 | 1,574 | 1.05× | Fused | Win |
| Random (minified) | 1,732 | 1,355 | 752 | 1.28× | Fused | Win |
| Random (prettified) | 2,039 | 1,787 | 1,144 | 1.14× | Fused | Win |
| Twitter (minified) | 2,962 | 2,207 | 911 | 1.34× | Fused | Win |
| Twitter (prettified) | 2,288 | 2,197 | 1,061 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 2,040 | 1,618 | 1,178 | 1.26× | Fused | Win |
| Canada Small (minified) | 906 | 756 | 475 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 1,969 | 1,882 | 1,406 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,739 | 1,697 | 1,125 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,814 | 1,278 | 745 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 2,161 | 2,091 | 1,415 | 1.03× | Fused | Win |
| Discord Small (minified) | 2,376 | 1,598 | 682 | 1.49× | Fused | Win |
| Discord Small (prettified) | 2,323 | 2,002 | 925 | 1.16× | Fused | Win |
| Github Events Small (minified) | 2,302 | 1,706 | 1,257 | 1.35× | Fused | Win |
| Google Maps Response Small (minified) | 2,185 | 1,297 | 829 | 1.68× | Fused | Win |
| Github Events Small (prettified) | 2,300 | 1,924 | 1,483 | 1.20× | Fused | Win |
| Google Maps Response Small (prettified) | 2,849 | 2,396 | 1,678 | 1.19× | Fused | Win |
| Instruments Small (minified) | 2,973 | 1,872 | 602 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,998 | 2,667 | 949 | 1.12× | Fused | Win |
| Marine IK Reverse Small (minified) | 849 | 705 | 341 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,138 | 2,149 | 1,015 | 0.99× | Two-stage | Win |
| Marine IK Small (minified) | 793 | 714 | 325 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 2,058 | 2,190 | 1,294 | 0.94× | Two-stage | Win |
| Random Small (minified) | 1,922 | 1,429 | 801 | 1.34× | Fused | Win |
| Random Small (prettified) | 2,128 | 1,871 | 1,200 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,729 | 2,090 | 832 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 1,983 | 2,244 | 971 | 0.88× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and all 66 minified documents, full-size and Small, while the tape wins 26 of the 33 full-size prettified documents and 30 of the 33 Small prettified ones. MSVC is where the split blurs: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 14, the fused path 5 (Github Events, Random, Apache Builds Small, Discord Small and Github Events Small) and 3 are ties (Apache Builds, Marine IK and Twitter). On the M2 Pro the fused path wins all POD input, all 44 minified documents and 36 of the 44 prettified documents (5 to the tape, 3 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.25× (String, Windows/MSVC) to 4.48× (Bool, macOS/GCC) faster than the two-stage path, and 1.42× (String, Windows/MSVC) to 5.05× (Bool, macOS/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** Freshly allocated, the fused path wins all 110 minified documents, on every build. Its margins run from 2% (Github Events on Linux/GCC, 1,619 against 1,581 MB/s) to 1.84× (Instruments on macOS/Clang, 2,720 against 1,475 MB/s) on the full-size documents and up to 1.82× on the Small ones (Discord Small on macOS/Clang, 2,066 against 1,137 MB/s). With objects reused it wins 107 and ties 1: the two-stage path takes minified Github Events on Linux/Clang (2,450 against 2,244 MB/s) and on Linux/GCC (2,305 against 2,174), and minified Marine IK on macOS/Clang is a tie (676 against 671). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 56 of 66.** It wins 21 of 22 on Linux/Clang, where the fused path wins prettified Canada by 4% (2,042 MB/s against 1,972), 21 of 22 on Linux/GCC, where prettified Canada is a tie (2,196 fused against 2,212), and 14 of 22 under MSVC, where the fused path wins prettified Github Events, Random, Apache Builds Small, Discord Small and Github Events Small, by 1% to 10%, and prettified Apache Builds, Marine IK and Twitter are ties. The tape's margins run from 1% (Apache Builds on Linux/GCC, 1,752 against 1,727 MB/s) to 43% (Discord on Linux/Clang, 2,492 against 1,740) on the full-size documents and from 3% (Canada Small on Linux/GCC) to 49% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 60 of the 66 prettified x86 documents: the fused path wins Apache Builds, Github Events, Apache Builds Small and Github Events Small under MSVC and Canada on Linux/Clang, and prettified Canada on Linux/GCC is a tie.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 36, the tape wins 5 and 3 are ties. On the 22 full-size documents the fused path wins 19, 1 is a tie (CitmCatalog on macOS/Clang) and the tape takes 2, both on macOS/Clang: prettified Marine IK Reverse, its widest M2 Pro margin at 1,844 against 1,692 MB/s (1.09×), and Marine IK (1.01×). The 22 Small copies split 17 / 2 / 3: the tape takes Apache Builds Small and Marine IK Reverse Small on macOS/GCC (1.04× and 1.05×) and Marine IK Reverse Small on macOS/Clang (1.01×), and Marine IK Small on macOS/GCC and Twitter Small on macOS/Clang are ties. By compiler, the fused path wins 19 of 22 on macOS/GCC (2 to the tape, 1 tie) and 17 of 22 on macOS/Clang (3 to the tape, 2 ties). Reused, the fused path wins 36, the tape 7, and 1 is a tie: 19, 3 and 0 on macOS/GCC, and 17, 4 and 1 on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Github Events Small on Linux/GCC is where Jsonifier is weakest.** Its two forms are the faster Jsonifier path's only freshly allocated losses to simdjson On Demand: prettified by 7% (2,186 against 2,344 MB/s) and minified by 5% (1,893 against 1,983), and there are no ties. With objects reused they are again the only losses, by 8% (2,645 against 2,866) and 2% (2,331 against 2,368), and prettified Random on Linux/GCC is a tie (2,113 against 2,111). We have not profiled what sets this document apart.

**MSVC slows all three by a similar median share.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 65% of its Linux/Clang throughput, the two-stage path 67% and simdjson 65%. The spread between documents is narrowest for the two-stage path (from 53% on Apache Builds to 74% on CitmCatalog), wider for the fused path (from 52% on Apache Builds to 86% on Instruments) and widest for simdjson, which keeps 50% of its Linux/Clang throughput on Google Maps Response and 96% on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 243 of 245 tests with whichever path is faster, ties none and loses two** (Github Events Small on Linux/GCC, minified and prettified). The fused path alone wins 239, ties none and loses 6: its four extra losses are all prettified on Linux/GCC (Github Events, Random, Twitter and Twitter Small).

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 194 MB/s, against 343 MB/s for the same document in declared order, so the faster Jsonifier path is 3.00× faster than simdjson freshly allocated (3.16× reused). The prettified form shows the same gap, at 675 MB/s against 1,162, and the faster Jsonifier path is 2.79× faster (2.83× reused). The Small copies show it too, at 154 MB/s minified and 587 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one on Linux and macOS/GCC, and up to about a fifth slower on macOS/Clang (1,179 against 1,472 MB/s prettified), and the faster Jsonifier path is 1.21× to 1.79× faster than simdjson on the full-size reversed document, freshly allocated, and 1.21× to 1.89× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so do about four fifths of the prettified documents. Routing each test to its faster path would raise the record against simdjson from 239 wins, 0 ties and 6 losses to 243 wins, 0 ties and 2 losses. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,403 | 1,269 | 1.11× | Win |
| Canada (minified) | 982 | 809 | 1.21× | Win |
| Apache Builds (prettified) | 1,752 | 1,599 | 1.10× | Win |
| Canada (prettified) | 2,212 | 1,822 | 1.21× | Win |
| CitmCatalog (minified) | 1,776 | 1,430 | 1.24× | Win |
| CitmCatalog (prettified) | 3,574 | 2,989 | 1.20× | Win |
| Discord (minified) | 1,847 | 1,512 | 1.22× | Win |
| Discord (prettified) | 2,511 | 2,161 | 1.16× | Win |
| Github Events (minified) | 1,581 | 1,522 | 1.04× | Win |
| Google Maps Response (minified) | 1,783 | 1,383 | 1.29× | Win |
| Github Events (prettified) | 1,931 | 1,846 | 1.05× | Win |
| Google Maps Response (prettified) | 3,396 | 2,708 | 1.25× | Win |
| Instruments (minified) | 2,242 | 1,633 | 1.37× | Win |
| Instruments (prettified) | 3,298 | 2,630 | 1.25× | Win |
| Marine IK Reverse (minified) | 732 | 623 | 1.18× | Win |
| Marine IK Reverse (prettified) | 2,577 | 2,259 | 1.14× | Win |
| Marine IK (minified) | 735 | 612 | 1.20× | Win |
| Marine IK (prettified) | 2,596 | 2,219 | 1.17× | Win |
| Random (minified) | 1,087 | 1,109 | 0.98× | **Loss** |
| Random (prettified) | 1,619 | 1,691 | 0.96× | **Loss** |
| Twitter (minified) | 1,673 | 1,482 | 1.13× | Win |
| Twitter (prettified) | 2,186 | 1,929 | 1.13× | Win |
| Apache Builds Small (minified) | 2,076 | 1,736 | 1.20× | Win |
| Canada Small (minified) | 889 | 752 | 1.18× | Win |
| Apache Builds Small (prettified) | 2,447 | 2,193 | 1.12× | Win |
| Canada Small (prettified) | 2,130 | 1,763 | 1.21× | Win |
| CitmCatalog Small (minified) | 1,434 | 1,179 | 1.22× | Win |
| CitmCatalog Small (prettified) | 2,363 | 2,013 | 1.17× | Win |
| Discord Small (minified) | 1,648 | 1,497 | 1.10× | Win |
| Discord Small (prettified) | 2,157 | 1,992 | 1.08× | Win |
| Github Events Small (minified) | 1,726 | 1,791 | 0.96× | **Loss** |
| Google Maps Response Small (minified) | 1,669 | 1,369 | 1.22× | Win |
| Github Events Small (prettified) | 2,186 | 2,199 | 0.99× | Tie |
| Google Maps Response Small (prettified) | 3,227 | 2,689 | 1.20× | Win |
| Instruments Small (minified) | 2,191 | 1,572 | 1.39× | Win |
| Instruments Small (prettified) | 3,283 | 2,560 | 1.28× | Win |
| Marine IK Reverse Small (minified) | 720 | 628 | 1.15× | Win |
| Marine IK Reverse Small (prettified) | 2,561 | 2,241 | 1.14× | Win |
| Marine IK Small (minified) | 768 | 631 | 1.22× | Win |
| Marine IK Small (prettified) | 2,659 | 2,267 | 1.17× | Win |
| Random Small (minified) | 1,506 | 1,225 | 1.23× | Win |
| Random Small (prettified) | 2,217 | 1,870 | 1.19× | Win |
| Twitter Small (minified) | 2,324 | 1,840 | 1.26× | Win |
| Twitter Small (prettified) | 2,943 | 2,354 | 1.25× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,031 | 863 | 1.19× | Win |
| Canada (minified) | 776 | 568 | 1.37× | Win |
| Apache Builds (prettified) | 1,287 | 1,108 | 1.16× | Win |
| Canada (prettified) | 1,588 | 1,382 | 1.15× | Win |
| CitmCatalog (minified) | 1,415 | 1,155 | 1.22× | Win |
| CitmCatalog (prettified) | 2,682 | 2,230 | 1.20× | Win |
| Discord (minified) | 1,277 | 1,068 | 1.20× | Win |
| Discord (prettified) | 1,758 | 1,486 | 1.18× | Win |
| Github Events (minified) | 1,454 | 1,278 | 1.14× | Win |
| Google Maps Response (minified) | 1,312 | 1,018 | 1.29× | Win |
| Github Events (prettified) | 1,660 | 1,507 | 1.10× | Win |
| Google Maps Response (prettified) | 1,877 | 1,876 | 1.00× | Tie |
| Instruments (minified) | 1,675 | 1,286 | 1.30× | Win |
| Instruments (prettified) | 2,431 | 1,965 | 1.24× | Win |
| Marine IK Reverse (minified) | 586 | 524 | 1.12× | Win |
| Marine IK Reverse (prettified) | 1,974 | 1,781 | 1.11× | Win |
| Marine IK (minified) | 594 | 526 | 1.13× | Win |
| Marine IK (prettified) | 1,982 | 1,793 | 1.11× | Win |
| Random (minified) | 957 | 768 | 1.25× | Win |
| Random (prettified) | 1,231 | 1,147 | 1.07× | Win |
| Twitter (minified) | 1,711 | 1,352 | 1.27× | Win |
| Twitter (prettified) | 2,040 | 1,728 | 1.18× | Win |
| Apache Builds Small (minified) | 1,288 | 1,078 | 1.20× | Win |
| Canada Small (minified) | 720 | 596 | 1.21× | Win |
| Apache Builds Small (prettified) | 1,737 | 1,339 | 1.30× | Win |
| Canada Small (prettified) | 1,551 | 1,250 | 1.24× | Win |
| CitmCatalog Small (minified) | 1,043 | 870 | 1.20× | Win |
| CitmCatalog Small (prettified) | 1,676 | 1,460 | 1.15× | Win |
| Discord Small (minified) | 1,327 | 1,051 | 1.26× | Win |
| Discord Small (prettified) | 1,700 | 1,389 | 1.22× | Win |
| Github Events Small (minified) | 1,306 | 1,150 | 1.14× | Win |
| Google Maps Response Small (minified) | 1,272 | 875 | 1.45× | Win |
| Github Events Small (prettified) | 1,547 | 1,404 | 1.10× | Win |
| Google Maps Response Small (prettified) | 2,323 | 1,848 | 1.26× | Win |
| Instruments Small (minified) | 1,732 | 1,264 | 1.37× | Win |
| Instruments Small (prettified) | 2,488 | 1,972 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 568 | 523 | 1.09× | Win |
| Marine IK Reverse Small (prettified) | 1,886 | 1,798 | 1.05× | Win |
| Marine IK Small (minified) | 593 | 516 | 1.15× | Win |
| Marine IK Small (prettified) | 1,986 | 1,806 | 1.10× | Win |
| Random Small (minified) | 1,130 | 839 | 1.35× | Win |
| Random Small (prettified) | 1,413 | 1,127 | 1.25× | Win |
| Twitter Small (minified) | 1,891 | 1,415 | 1.34× | Win |
| Twitter Small (prettified) | 2,223 | 1,569 | 1.42× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,080 | 1,610 | 1.29× | Win |
| Canada (minified) | 1,201 | 951 | 1.26× | Win |
| Apache Builds (prettified) | 2,587 | 2,016 | 1.28× | Win |
| Canada (prettified) | 2,643 | 2,136 | 1.24× | Win |
| CitmCatalog (minified) | 2,143 | 1,658 | 1.29× | Win |
| CitmCatalog (prettified) | 4,155 | 3,459 | 1.20× | Win |
| Discord (minified) | 2,489 | 1,748 | 1.42× | Win |
| Discord (prettified) | 3,367 | 2,463 | 1.37× | Win |
| Github Events (minified) | 2,305 | 1,882 | 1.22× | Win |
| Google Maps Response (minified) | 1,953 | 1,476 | 1.32× | Win |
| Github Events (prettified) | 2,525 | 2,217 | 1.14× | Win |
| Google Maps Response (prettified) | 3,685 | 2,877 | 1.28× | Win |
| Instruments (minified) | 2,529 | 1,684 | 1.50× | Win |
| Instruments (prettified) | 3,696 | 2,694 | 1.37× | Win |
| Marine IK Reverse (minified) | 787 | 697 | 1.13× | Win |
| Marine IK Reverse (prettified) | 2,823 | 2,474 | 1.14× | Win |
| Marine IK (minified) | 810 | 683 | 1.19× | Win |
| Marine IK (prettified) | 2,801 | 2,463 | 1.14× | Win |
| Random (minified) | 1,449 | 1,461 | 0.99× | **Loss** |
| Random (prettified) | 2,113 | 2,174 | 0.97× | **Loss** |
| Twitter (minified) | 2,099 | 1,733 | 1.21× | Win |
| Twitter (prettified) | 2,727 | 2,239 | 1.22× | Win |
| Apache Builds Small (minified) | 2,407 | 1,902 | 1.27× | Win |
| Canada Small (minified) | 1,100 | 865 | 1.27× | Win |
| Apache Builds Small (prettified) | 2,855 | 2,329 | 1.23× | Win |
| Canada Small (prettified) | 2,610 | 2,004 | 1.30× | Win |
| CitmCatalog Small (minified) | 1,637 | 1,350 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,803 | 2,284 | 1.23× | Win |
| Discord Small (minified) | 2,047 | 1,752 | 1.17× | Win |
| Discord Small (prettified) | 2,679 | 2,301 | 1.16× | Win |
| Github Events Small (minified) | 2,044 | 1,989 | 1.03× | Win |
| Google Maps Response Small (minified) | 1,907 | 1,496 | 1.27× | Win |
| Github Events Small (prettified) | 2,645 | 2,453 | 1.08× | Win |
| Google Maps Response Small (prettified) | 3,508 | 2,884 | 1.22× | Win |
| Instruments Small (minified) | 2,326 | 1,610 | 1.45× | Win |
| Instruments Small (prettified) | 3,513 | 2,656 | 1.32× | Win |
| Marine IK Reverse Small (minified) | 859 | 714 | 1.20× | Win |
| Marine IK Reverse Small (prettified) | 3,000 | 2,529 | 1.19× | Win |
| Marine IK Small (minified) | 911 | 713 | 1.28× | Win |
| Marine IK Small (prettified) | 3,118 | 2,540 | 1.23× | Win |
| Random Small (minified) | 1,812 | 1,376 | 1.32× | Win |
| Random Small (prettified) | 2,654 | 2,105 | 1.26× | Win |
| Twitter Small (minified) | 2,528 | 2,036 | 1.24× | Win |
| Twitter Small (prettified) | 3,217 | 2,613 | 1.23× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,520 | 1,051 | 1.45× | Win |
| Canada (minified) | 948 | 753 | 1.26× | Win |
| Apache Builds (prettified) | 1,863 | 1,325 | 1.41× | Win |
| Canada (prettified) | 1,903 | 1,607 | 1.18× | Win |
| CitmCatalog (minified) | 1,628 | 1,278 | 1.27× | Win |
| CitmCatalog (prettified) | 3,041 | 2,405 | 1.26× | Win |
| Discord (minified) | 1,630 | 1,201 | 1.36× | Win |
| Discord (prettified) | 2,196 | 1,657 | 1.33× | Win |
| Github Events (minified) | 1,963 | 1,522 | 1.29× | Win |
| Google Maps Response (minified) | 1,519 | 1,101 | 1.38× | Win |
| Github Events (prettified) | 2,125 | 1,812 | 1.17× | Win |
| Google Maps Response (prettified) | 2,598 | 1,999 | 1.30× | Win |
| Instruments (minified) | 1,868 | 1,339 | 1.40× | Win |
| Instruments (prettified) | 2,657 | 2,027 | 1.31× | Win |
| Marine IK Reverse (minified) | 660 | 587 | 1.13× | Win |
| Marine IK Reverse (prettified) | 2,174 | 1,957 | 1.11× | Win |
| Marine IK (minified) | 667 | 587 | 1.14× | Win |
| Marine IK (prettified) | 2,206 | 1,959 | 1.13× | Win |
| Random (minified) | 1,398 | 922 | 1.52× | Win |
| Random (prettified) | 1,824 | 1,362 | 1.34× | Win |
| Twitter (minified) | 2,152 | 1,503 | 1.43× | Win |
| Twitter (prettified) | 2,560 | 1,920 | 1.33× | Win |
| Apache Builds Small (minified) | 1,877 | 1,344 | 1.40× | Win |
| Canada Small (minified) | 882 | 711 | 1.24× | Win |
| Apache Builds Small (prettified) | 2,195 | 1,648 | 1.33× | Win |
| Canada Small (prettified) | 1,845 | 1,585 | 1.16× | Win |
| CitmCatalog Small (minified) | 1,382 | 1,083 | 1.28× | Win |
| CitmCatalog Small (prettified) | 2,170 | 1,788 | 1.21× | Win |
| Discord Small (minified) | 1,738 | 1,277 | 1.36× | Win |
| Discord Small (prettified) | 2,184 | 1,663 | 1.31× | Win |
| Github Events Small (minified) | 1,552 | 1,377 | 1.13× | Win |
| Google Maps Response Small (minified) | 1,490 | 995 | 1.50× | Win |
| Github Events Small (prettified) | 2,031 | 1,675 | 1.21× | Win |
| Google Maps Response Small (prettified) | 2,668 | 2,037 | 1.31× | Win |
| Instruments Small (minified) | 1,850 | 1,344 | 1.38× | Win |
| Instruments Small (prettified) | 2,667 | 2,057 | 1.30× | Win |
| Marine IK Reverse Small (minified) | 698 | 632 | 1.11× | Win |
| Marine IK Reverse Small (prettified) | 2,256 | 2,088 | 1.08× | Win |
| Marine IK Small (minified) | 747 | 634 | 1.18× | Win |
| Marine IK Small (prettified) | 2,391 | 2,098 | 1.14× | Win |
| Random Small (minified) | 1,474 | 1,002 | 1.47× | Win |
| Random Small (prettified) | 1,630 | 1,462 | 1.11× | Win |
| Twitter Small (minified) | 2,266 | 1,579 | 1.44× | Win |
| Twitter Small (prettified) | 2,616 | 1,934 | 1.35× | Win |

In the freshly allocated run, two-stage Jsonifier wins 83 of these 88 tests, ties 2 and loses 3, by 2% to 4%, all on Linux/GCC: minified and prettified Random (0.98× and 0.96×) and minified Github Events Small (0.96×). The ties are prettified Github Events Small on Linux/GCC and prettified Google Maps Response on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 82, ties 1 and loses 5. With objects reused, two-stage Jsonifier wins 86 and loses 2: minified and prettified Random on Linux/GCC by 1% and 3% (1,449 against 1,461 MB/s and 2,113 against 2,174). The fused path also beats the reflection reader on 86, ties none and loses 2. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 39 of the 40 results; the one wider gap is reused minified Twitter Small on macOS/Clang, where the "jsonifier" row runs at 3,708 MB/s against the two-stage row's 3,977. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,421 | 3,740 | 1.72× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,335 | 4,034 | 1.82× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 3,908 | 3,402 | 1.15× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,798 | 4,060 | 1.18× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,745 | 6,689 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 8,916 | 7,978 | 1.12× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,338 | 5,475 | 0.97× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,180 | 6,634 | 0.93× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,814 | 6,947 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,800 | 8,085 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,529 | 5,485 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,368 | 6,618 | 0.96× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,203 | 3,827 | 1.36× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,546 | 4,726 | 1.17× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,976 | 3,757 | 1.06× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,314 | 4,424 | 0.98× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,976 | 4,063 | 1.22× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,415 | 4,818 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,223 | 3,243 | 0.99× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,938 | 3,360 | 1.17× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,701 | 3,831 | 1.75× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,425 | 4,202 | 1.77× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,308 | 3,680 | 1.17× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,182 | 4,239 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,885 | 6,802 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,039 | 8,265 | 1.09× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,584 | 5,644 | 0.99× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,411 | 6,902 | 0.93× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,957 | 7,040 | 1.13× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,741 | 8,141 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,721 | 5,638 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,604 | 6,604 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,313 | 4,207 | 1.26× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,613 | 4,824 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,256 | 3,988 | 1.07× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,484 | 4,596 | 0.98× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 5,092 | 4,132 | 1.23× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,529 | 4,891 | 1.13× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,977 | 3,468 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,272 | 4,204 | 1.02× | Win |

Jsonifier wins 31 of the 40 results, ties 2 and loses 7, and wins by up to 1.82× (prettified Twitter under MSVC, freshly allocated). Freshly allocated it wins 15, ties 1 and loses 4: the losses are minified and prettified Twitter Small on Linux/Clang (0.97× and 0.93×), prettified Twitter Small on Linux/GCC (0.96×) and prettified Twitter Small on macOS/GCC (0.98×); the tie is minified Twitter Small on macOS/Clang. The reused run has 16 wins, 1 tie and 3 losses: minified and prettified Twitter Small on Linux/Clang (0.99× and 0.93×) and prettified Twitter Small on macOS/GCC (0.98×); the tie is prettified Twitter Small on Linux/GCC. Every full-size result is a win, by 1.07× to 1.82× on the three x86 builds and 1.12× to 1.36× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.03× to 1.36×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,627 | 2,559 | 1.03× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,456 | 2,582 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,924 | 2,154 | 1.36× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,320 | 2,030 | 1.14× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,545 | 1,987 | 1.28× | Win |

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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 177 of 245 freshly allocated tests: all 25 POD-type tests, all 110 minified documents and 36 of 44 prettified documents on the M2 Pro, while the two-stage path wins 56 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 243 of 245 tests, its stage-1 + reflection path beats simdjson's reflection reader on 83 of 88, and on partial reading it wins 31 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*