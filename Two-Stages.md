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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 6, 2026, with Jsonifier [39b6202](https://github.com/nihilai-collective/jsonifier/commit/39b6202), Glaze [0a363d4](https://github.com/stephenberry/glaze/commit/0a363d40), simdjson [dfe02cf](https://github.com/simdjson/simdjson/commit/dfe02cfe3) and BenchmarkSuite [ed43ce8](https://github.com/nihilai-collective/benchmarksuite/commit/ed43ce8) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Every x86 result converged. Four freshly allocated full-parse rows contain non-converged results (two on macOS/GCC and two on macOS/Clang), and two reused rows do (one on each macOS build). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 168 tests, 7 are statistical ties, and the two-stage path wins 70. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 29 | 2 | 18 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 21 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 45 | 2 | 2 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 40 | 2 | 7 | 47 of 49 |
| **Aggregate** | **168** | **7** | **70** | **241 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 2 / 9 | 1 / 1 / 20 | 15 / 2 / 5 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 1 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 17 / 1 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 44 / 1 / 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **244 / 0 / 1** | **239 / 1 / 5** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 40 | 4 | 5 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 43 | 1 | 5 | 48 of 49 |
| **Aggregate** | **166** | **5** | **74** | **243 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 21 / 0 / 1 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 17 / 2 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 1 / 0 / 10 | 0 / 0 / 22 | 13 / 3 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 / 1 / 1 | 45 / 0 / 4 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **243 / 1 / 1** | **240 / 0 / 5** |

With objects reused, across 245 tests the fused path wins 166, ties 5 and loses 74 to the two-stage path, and the x86 split holds: the tape wins 31 of the 33 full-size prettified documents (the fused path takes Apache Builds under MSVC and Canada on Linux/GCC) and 32 of the 33 Small prettified ones (the fused path takes Apache Builds Small under MSVC), and the fused path wins all 33 Small minified documents and 32 of the 33 full-size minified ones (on Linux/GCC the tape takes Github Events, 2,257 against 2,156 MB/s). On the M2 Pro the fused path wins 43 of the 44 minified documents, with the tape taking Canada Small on macOS/GCC, and 30 of the 44 prettified ones, with 5 ties and 9 tape wins. Against simdjson, the faster Jsonifier path wins 243, ties 1 and loses 1, and the fused path alone wins 240, ties none and loses 5.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 460 | 261 | 70 | 1.76× | Fused | Win |
| Double (POD) | 612 | 492 | 147 | 1.24× | Fused | Win |
| Int64 (POD) | 1,464 | 882 | 308 | 1.66× | Fused | Win |
| String (POD) | 931 | 771 | 548 | 1.21× | Fused | Win |
| Uint64 (POD) | 1,564 | 929 | 309 | 1.68× | Fused | Win |
| Apache Builds (minified) | 901 | 780 | 662 | 1.15× | Fused | Win |
| Canada (minified) | 693 | 646 | 428 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 1,013 | 1,011 | 859 | 1.00× | Tie | Win |
| Canada (prettified) | 1,414 | 1,490 | 939 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 1,485 | 1,256 | 617 | 1.18× | Fused | Win |
| CitmCatalog (prettified) | 2,203 | 2,614 | 1,282 | 0.84× | Two-stage | Win |
| Discord (minified) | 1,053 | 1,006 | 662 | 1.05× | Fused | Win |
| Discord (prettified) | 1,218 | 1,416 | 949 | 0.86× | Two-stage | Win |
| Github Events (minified) | 974 | 900 | 626 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 1,366 | 1,149 | 520 | 1.19× | Fused | Win |
| Github Events (prettified) | 1,004 | 1,076 | 759 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 2,013 | 2,291 | 1,052 | 0.88× | Two-stage | Win |
| Instruments (minified) | 1,701 | 1,370 | 747 | 1.24× | Fused | Win |
| Instruments (prettified) | 1,797 | 2,245 | 1,203 | 0.80× | Two-stage | Win |
| Marine IK Reverse (minified) | 562 | 506 | 199 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 1,800 | 1,806 | 667 | 1.00× | Tie | Win |
| Marine IK (minified) | 574 | 513 | 349 | 1.12× | Fused | Win |
| Marine IK (prettified) | 1,799 | 1,831 | 1,218 | 0.98× | Two-stage | Win |
| Random (minified) | 769 | 748 | 554 | 1.03× | Fused | Win |
| Random (prettified) | 992 | 1,129 | 831 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,158 | 1,061 | 853 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,223 | 1,382 | 1,104 | 0.88× | Two-stage | Win |
| Apache Builds Small (minified) | 1,131 | 934 | 738 | 1.21× | Fused | Win |
| Canada Small (minified) | 624 | 588 | 347 | 1.06× | Fused | Win |
| Apache Builds Small (prettified) | 1,202 | 1,163 | 911 | 1.03× | Fused | Win |
| Canada Small (prettified) | 1,277 | 1,403 | 810 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 762 | 697 | 503 | 1.09× | Fused | Win |
| CitmCatalog Small (prettified) | 1,049 | 1,220 | 866 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 961 | 856 | 666 | 1.12× | Fused | Win |
| Discord Small (prettified) | 1,050 | 1,139 | 901 | 0.92× | Two-stage | Win |
| Github Events Small (minified) | 974 | 764 | 608 | 1.28× | Fused | Win |
| Google Maps Response Small (minified) | 1,165 | 1,058 | 500 | 1.10× | Fused | Win |
| Github Events Small (prettified) | 978 | 968 | 764 | 1.01× | Fused | Win |
| Google Maps Response Small (prettified) | 1,838 | 2,094 | 1,010 | 0.88× | Two-stage | Win |
| Instruments Small (minified) | 1,556 | 1,283 | 731 | 1.21× | Fused | Win |
| Instruments Small (prettified) | 1,651 | 2,107 | 1,162 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 444 | 413 | 154 | 1.08× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,355 | 1,520 | 575 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 476 | 435 | 302 | 1.09× | Fused | Win |
| Marine IK Small (prettified) | 1,430 | 1,593 | 1,076 | 0.90× | Two-stage | Win |
| Random Small (minified) | 847 | 787 | 522 | 1.08× | Fused | Win |
| Random Small (prettified) | 1,072 | 1,199 | 793 | 0.89× | Two-stage | Win |
| Twitter Small (minified) | 1,268 | 1,147 | 1,122 | 1.11× | Fused | Win |
| Twitter Small (prettified) | 1,263 | 1,448 | 1,406 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,890 | 612 | 277 | 3.09× | Fused | Win |
| Double (POD) | 1,269 | 730 | 351 | 1.74× | Fused | Win |
| Int64 (POD) | 2,999 | 1,652 | 770 | 1.82× | Fused | Win |
| String (POD) | 1,764 | 1,292 | 1,368 | 1.37× | Fused | Win |
| Uint64 (POD) | 3,188 | 1,590 | 706 | 2.00× | Fused | Win |
| Apache Builds (minified) | 1,640 | 1,492 | 1,164 | 1.10× | Fused | Win |
| Canada (minified) | 1,053 | 939 | 676 | 1.12× | Fused | Win |
| Apache Builds (prettified) | 1,672 | 1,843 | 1,494 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,031 | 2,071 | 1,538 | 0.98× | Two-stage | Win |
| CitmCatalog (minified) | 2,156 | 1,617 | 1,003 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 2,830 | 3,369 | 2,153 | 0.84× | Two-stage | Win |
| Discord (minified) | 2,245 | 1,732 | 941 | 1.30× | Fused | Win |
| Discord (prettified) | 1,762 | 2,427 | 1,364 | 0.73× | Two-stage | Win |
| Github Events (minified) | 1,616 | 1,536 | 949 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,400 | 1,835 | 1,028 | 1.31× | Fused | Win |
| Github Events (prettified) | 1,522 | 1,908 | 1,144 | 0.80× | Two-stage | Win |
| Google Maps Response (prettified) | 2,737 | 3,349 | 2,106 | 0.82× | Two-stage | Win |
| Instruments (minified) | 2,520 | 1,945 | 751 | 1.30× | Fused | Win |
| Instruments (prettified) | 2,144 | 2,893 | 1,245 | 0.74× | Two-stage | Win |
| Marine IK Reverse (minified) | 843 | 750 | 553 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,479 | 2,572 | 2,028 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 853 | 737 | 541 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,498 | 2,474 | 2,000 | 1.01× | Tie | Win |
| Random (minified) | 1,213 | 1,094 | 805 | 1.11× | Fused | Win |
| Random (prettified) | 1,386 | 1,651 | 1,217 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,842 | 1,666 | 915 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,531 | 2,153 | 1,210 | 0.71× | Two-stage | Win |
| Apache Builds Small (minified) | 2,474 | 2,174 | 1,379 | 1.14× | Fused | Win |
| Canada Small (minified) | 992 | 941 | 701 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 2,175 | 2,626 | 1,742 | 0.83× | Two-stage | Win |
| Canada Small (prettified) | 1,925 | 2,180 | 1,677 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,637 | 1,353 | 934 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,775 | 2,291 | 1,658 | 0.77× | Two-stage | Win |
| Discord Small (minified) | 1,930 | 1,686 | 746 | 1.15× | Fused | Win |
| Discord Small (prettified) | 1,571 | 2,235 | 1,022 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 1,943 | 1,771 | 1,346 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 2,194 | 1,649 | 1,033 | 1.33× | Fused | Win |
| Github Events Small (prettified) | 1,824 | 2,235 | 1,662 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,538 | 3,152 | 2,094 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,489 | 1,874 | 665 | 1.33× | Fused | Win |
| Instruments Small (prettified) | 2,093 | 2,852 | 1,088 | 0.73× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 817 | 702 | 496 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,108 | 2,463 | 1,837 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 874 | 772 | 504 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,181 | 2,648 | 1,866 | 0.82× | Two-stage | Win |
| Random Small (minified) | 1,682 | 1,475 | 805 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,665 | 2,258 | 1,267 | 0.74× | Two-stage | Win |
| Twitter Small (minified) | 2,445 | 2,137 | 989 | 1.14× | Fused | Win |
| Twitter Small (prettified) | 1,840 | 2,671 | 1,290 | 0.69× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,346 | 546 | 223 | 4.30× | Fused | Win |
| Double (POD) | 1,186 | 685 | 294 | 1.73× | Fused | Win |
| Int64 (POD) | 3,325 | 1,597 | 710 | 2.08× | Fused | Win |
| String (POD) | 1,818 | 1,227 | 1,236 | 1.48× | Fused | Win |
| Uint64 (POD) | 3,411 | 1,634 | 715 | 2.09× | Fused | Win |
| Apache Builds (minified) | 1,622 | 1,406 | 1,263 | 1.15× | Fused | Win |
| Canada (minified) | 1,110 | 971 | 819 | 1.14× | Fused | Win |
| Apache Builds (prettified) | 1,683 | 1,786 | 1,638 | 0.94× | Two-stage | Win |
| Canada (prettified) | 2,210 | 2,185 | 1,831 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,320 | 1,758 | 1,459 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 3,344 | 3,531 | 2,995 | 0.95× | Two-stage | Win |
| Discord (minified) | 2,228 | 1,822 | 1,352 | 1.22× | Fused | Win |
| Discord (prettified) | 2,213 | 2,537 | 1,929 | 0.87× | Two-stage | Win |
| Github Events (minified) | 1,599 | 1,741 | 1,422 | 0.92× | Two-stage | Win |
| Google Maps Response (minified) | 2,457 | 1,766 | 1,097 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,810 | 2,087 | 1,700 | 0.87× | Two-stage | Win |
| Google Maps Response (prettified) | 3,181 | 3,359 | 2,193 | 0.95× | Two-stage | Win |
| Instruments (minified) | 3,191 | 2,175 | 1,613 | 1.47× | Fused | Win |
| Instruments (prettified) | 2,894 | 3,186 | 2,579 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 834 | 701 | 558 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,459 | 2,569 | 2,114 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 839 | 716 | 579 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,496 | 2,583 | 2,061 | 0.97× | Two-stage | Win |
| Random (minified) | 1,180 | 1,068 | 1,034 | 1.10× | Fused | Win |
| Random (prettified) | 1,497 | 1,610 | 1,568 | 0.93× | Two-stage | Win |
| Twitter (minified) | 1,981 | 1,655 | 1,490 | 1.20× | Fused | Win |
| Twitter (prettified) | 1,942 | 2,161 | 1,941 | 0.90× | Two-stage | Win |
| Apache Builds Small (minified) | 2,334 | 2,049 | 1,668 | 1.14× | Fused | Win |
| Canada Small (minified) | 1,059 | 900 | 756 | 1.18× | Fused | Win |
| Apache Builds Small (prettified) | 2,287 | 2,391 | 2,069 | 0.96× | Two-stage | Win |
| Canada Small (prettified) | 2,092 | 2,131 | 1,793 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,717 | 1,393 | 1,242 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,096 | 2,396 | 2,136 | 0.87× | Two-stage | Win |
| Discord Small (minified) | 1,910 | 1,586 | 1,333 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,883 | 2,162 | 1,764 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,059 | 1,736 | 1,973 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 2,248 | 1,650 | 1,143 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 2,055 | 2,142 | 2,473 | 0.96× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,003 | 3,106 | 2,224 | 0.97× | Two-stage | Win |
| Instruments Small (minified) | 3,261 | 2,124 | 1,485 | 1.54× | Fused | Win |
| Instruments Small (prettified) | 2,867 | 3,199 | 2,397 | 0.90× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 847 | 725 | 545 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,283 | 2,576 | 1,989 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 935 | 755 | 551 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,439 | 2,635 | 2,007 | 0.93× | Two-stage | Win |
| Random Small (minified) | 1,569 | 1,455 | 1,153 | 1.08× | Fused | Win |
| Random Small (prettified) | 1,889 | 2,154 | 1,764 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,886 | 2,265 | 1,955 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,448 | 2,692 | 2,534 | 0.91× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,674 | 437 | 181 | 3.83× | Fused | Win |
| Double (POD) | 830 | 459 | 157 | 1.81× | Fused | Win |
| Int64 (POD) | 2,414 | 810 | 433 | 2.98× | Fused | Win |
| String (POD) | 1,101 | 674 | 663 | 1.64× | Fused | Win |
| Uint64 (POD) | 2,262 | 792 | 439 | 2.85× | Fused | Win |
| Apache Builds (minified) | 1,161 | 963 | 720 | 1.21× | Fused | Win |
| Canada (minified) | 800 | 705 | 500 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,265 | 1,192 | 915 | 1.06× | Fused | Win |
| Canada (prettified) | 1,711 | 1,497 | 1,141 | 1.14× | Fused | Win |
| CitmCatalog (minified) | 2,082 | 1,389 | 1,058 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | 2,678 | 2,495 | 2,006 | 1.07× | Fused | Win |
| Discord (minified) | 1,693 | 1,059 | 805 | 1.60× | Fused | Win |
| Discord (prettified) | 1,550 | 1,466 | 1,302 | 1.06× | Fused | Win |
| Github Events (minified) | 1,393 | 1,203 | 845 | 1.16× | Fused | Win |
| Google Maps Response (minified) | 1,675 | 1,082 | 661 | 1.55× | Fused | Win |
| Github Events (prettified) | 1,392 | 1,351 | 1,024 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,577 | 2,034 | 1,269 | 1.27× | Fused | Win |
| Instruments (minified) | 2,057 | 1,542 | 1,062 | 1.33× | Fused | Win |
| Instruments (prettified) | 2,391 | 1,995 | 1,611 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | 606 | 487 | 375 | 1.25× | Fused | Win |
| Marine IK Reverse (prettified) | 1,596 | 1,639 | 1,488 | 0.97× | Tie | Win |
| Marine IK (minified) | 605 | 542 | 488 (n/c) | 1.12× | Fused | Win |
| Marine IK (prettified) | 1,736 (n/c) | 1,793 | 1,601 | 0.97× | Two-stage | Win |
| Random (minified) | 892 | 813 | 618 | 1.10× | Fused | Win |
| Random (prettified) | 1,195 | 1,003 | 949 | 1.19× | Fused | Win |
| Twitter (minified) | 1,790 | 1,540 | 922 | 1.16× | Fused | Win |
| Twitter (prettified) | 1,874 | 1,706 | 1,182 | 1.10× | Fused | Win |
| Apache Builds Small (minified) | 1,337 | 1,264 | 791 | 1.06× | Fused | Win |
| Canada Small (minified) | 728 | 669 | 455 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 1,540 | 1,427 | 1,051 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,601 | 1,591 | 1,124 | 1.01× | Fused | Win |
| CitmCatalog Small (minified) | 1,295 | 1,024 | 863 | 1.26× | Fused | Win |
| CitmCatalog Small (prettified) | 1,535 | 1,412 | 1,449 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,536 | 1,304 | 935 | 1.18× | Fused | Win |
| Discord Small (prettified) | 1,586 | 1,506 | 1,206 | 1.05× | Fused | Win |
| Github Events Small (minified) | 1,446 | 1,294 | 1,111 | 1.12× | Fused | Win |
| Google Maps Response Small (minified) | 1,728 | 1,136 | 785 | 1.52× | Fused | Win |
| Github Events Small (prettified) | 1,535 | 1,438 | 1,418 | 1.07× | Fused | Win |
| Google Maps Response Small (prettified) | 2,541 | 2,307 | 1,406 | 1.10× | Fused | Win |
| Instruments Small (minified) | 2,602 | 1,472 | 1,136 | 1.77× | Fused | Win |
| Instruments Small (prettified) | 2,890 | 2,494 | 1,821 | 1.16× | Fused | Win |
| Marine IK Reverse Small (minified) | 617 | 532 | 403 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,704 | 1,598 | 1,408 | 1.07× | Fused | Win |
| Marine IK Small (minified) | 700 | 593 | 400 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 1,948 | 1,989 | 1,406 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,271 | 1,048 | 752 | 1.21× | Fused | Win |
| Random Small (prettified) | 1,608 | 1,386 | 1,132 | 1.16× | Fused | Win |
| Twitter Small (minified) | 2,311 | 1,871 | 1,145 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,226 | 2,246 | 1,474 | 0.99× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,122 | 321 | 190 | 3.49× | Fused | Win |
| Double (POD) | 927 | 354 | 212 | 2.62× | Fused | Win |
| Int64 (POD) | 1,682 | 759 | 433 | 2.22× | Fused | Win |
| String (POD) | 1,397 | 751 | 835 | 1.86× | Fused | Win |
| Uint64 (POD) | 2,350 | 845 | 476 | 2.78× | Fused | Win |
| Apache Builds (minified) | 1,570 | 1,104 | 870 | 1.42× | Fused | Win |
| Canada (minified) | 838 | 733 | 424 (n/c) | 1.14× | Fused | Win |
| Apache Builds (prettified) | 1,637 | 1,375 | 1,120 | 1.19× | Fused | Win |
| Canada (prettified) | 1,324 | 1,389 | 956 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 1,934 | 1,334 | 785 | 1.45× | Fused | Win |
| CitmCatalog (prettified) | 2,738 | 2,575 | 1,654 | 1.06× | Fused | Win |
| Discord (minified) | 2,361 | 1,248 | 672 | 1.89× | Fused | Win |
| Discord (prettified) | 2,091 | 1,658 | 1,074 | 1.26× | Fused | Win |
| Github Events (minified) | 1,672 | 1,310 | 792 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 1,718 | 1,031 | 762 | 1.67× | Fused | Win |
| Github Events (prettified) | 1,607 | 1,726 | 1,058 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 2,448 | 1,833 | 1,442 | 1.34× | Fused | Win |
| Instruments (minified) | 2,736 | 1,491 | 552 | 1.83× | Fused | Win |
| Instruments (prettified) | 2,203 | 2,242 | 930 | 0.98× | Two-stage | Win |
| Marine IK Reverse (minified) | 609 | 515 (n/c) | 419 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 1,810 | 1,787 | 1,344 | 1.01× | Tie | Win |
| Marine IK (minified) | 638 | 609 | 338 | 1.05× | Fused | Win |
| Marine IK (prettified) | 2,097 | 1,670 | 1,191 | 1.26× | Fused | Win |
| Random (minified) | 1,131 | 771 | 664 | 1.47× | Fused | Win |
| Random (prettified) | 1,296 | 1,143 | 915 | 1.13× | Fused | Win |
| Twitter (minified) | 1,984 | 1,683 | 686 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,955 | 2,003 | 1,061 | 0.98× | Two-stage | Win |
| Apache Builds Small (minified) | 1,336 | 1,347 | 875 | 0.99× | Tie | Win |
| Canada Small (minified) | 769 | 566 | 435 | 1.36× | Fused | Win |
| Apache Builds Small (prettified) | 1,681 | 1,632 | 1,126 | 1.03× | Fused | Win |
| Canada Small (prettified) | 1,502 | 1,280 | 1,009 | 1.17× | Fused | Win |
| CitmCatalog Small (minified) | 1,201 | 917 | 631 | 1.31× | Fused | Win |
| CitmCatalog Small (prettified) | 1,631 | 1,451 | 1,109 | 1.12× | Fused | Win |
| Discord Small (minified) | 1,842 | 1,044 | 519 | 1.76× | Fused | Win |
| Discord Small (prettified) | 1,627 | 1,239 | 675 | 1.31× | Fused | Win |
| Github Events Small (minified) | 1,656 | 1,176 | 1,021 | 1.41× | Fused | Win |
| Google Maps Response Small (minified) | 1,906 | 1,017 | 636 | 1.87× | Fused | Win |
| Github Events Small (prettified) | 1,603 | 1,198 | 1,239 | 1.34× | Fused | Win |
| Google Maps Response Small (prettified) | 2,564 | 2,150 | 1,320 | 1.19× | Fused | Win |
| Instruments Small (minified) | 2,673 | 1,672 | 437 | 1.60× | Fused | Win |
| Instruments Small (prettified) | 2,667 | 2,176 | 852 | 1.23× | Fused | Win |
| Marine IK Reverse Small (minified) | 511 | 470 | 310 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,644 | 1,829 | 942 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 658 | 490 | 318 | 1.34× | Fused | Win |
| Marine IK Small (prettified) | 1,869 | 1,890 | 1,097 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,430 | 1,007 | 617 | 1.42× | Fused | Win |
| Random Small (prettified) | 1,492 | 1,226 | 854 | 1.22× | Fused | Win |
| Twitter Small (minified) | 2,179 | 1,587 | 844 | 1.37× | Fused | Win |
| Twitter Small (prettified) | 2,154 | 2,163 | 1,039 | 1.00× | Two-stage | Win |

#### Reused objects

**Windows / MSVC 19.51 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 545 | 287 | 72 | 1.90× | Fused | Win |
| Double (POD) | 804 | 608 | 158 | 1.32× | Fused | Win |
| Int64 (POD) | 1,956 | 1,028 | 327 | 1.90× | Fused | Win |
| String (POD) | 1,597 | 1,175 | 713 | 1.36× | Fused | Win |
| Uint64 (POD) | 1,924 | 1,124 | 327 | 1.71× | Fused | Win |
| Apache Builds (minified) | 1,426 | 1,211 | 961 | 1.18× | Fused | Win |
| Canada (minified) | 833 | 783 | 481 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,581 | 1,556 | 1,221 | 1.02× | Fused | Win |
| Canada (prettified) | 1,626 | 1,737 | 1,052 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,736 | 1,443 | 646 | 1.20× | Fused | Win |
| CitmCatalog (prettified) | 2,441 | 2,973 | 1,328 | 0.82× | Two-stage | Win |
| Discord (minified) | 1,489 | 1,389 | 843 | 1.07× | Fused | Win |
| Discord (prettified) | 1,567 | 1,910 | 1,210 | 0.82× | Two-stage | Win |
| Github Events (minified) | 1,430 | 1,282 | 800 | 1.12× | Fused | Win |
| Google Maps Response (minified) | 1,553 | 1,288 | 572 | 1.21× | Fused | Win |
| Github Events (prettified) | 1,424 | 1,528 | 955 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 2,211 | 2,553 | 1,149 | 0.87× | Two-stage | Win |
| Instruments (minified) | 2,081 | 1,636 | 845 | 1.27× | Fused | Win |
| Instruments (prettified) | 2,030 | 2,628 | 1,338 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 627 | 544 | 207 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 1,905 | 1,980 | 679 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 627 | 554 | 374 | 1.13× | Fused | Win |
| Marine IK (prettified) | 1,937 | 1,996 | 1,318 | 0.97× | Two-stage | Win |
| Random (minified) | 1,000 | 971 | 668 | 1.03× | Fused | Win |
| Random (prettified) | 1,228 | 1,456 | 993 | 0.84× | Two-stage | Win |
| Twitter (minified) | 1,616 | 1,484 | 1,118 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,604 | 1,902 | 1,418 | 0.84× | Two-stage | Win |
| Apache Builds Small (minified) | 1,503 | 1,232 | 1,002 | 1.22× | Fused | Win |
| Canada Small (minified) | 788 | 739 | 387 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,568 | 1,503 | 1,222 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,557 | 1,717 | 888 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,004 | 895 | 595 | 1.12× | Fused | Win |
| CitmCatalog Small (prettified) | 1,320 | 1,533 | 1,007 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,292 | 1,183 | 923 | 1.09× | Fused | Win |
| Discord Small (prettified) | 1,303 | 1,552 | 1,214 | 0.84× | Two-stage | Win |
| Github Events Small (minified) | 1,334 | 1,041 | 768 | 1.28× | Fused | Win |
| Google Maps Response Small (minified) | 1,415 | 1,221 | 550 | 1.16× | Fused | Win |
| Github Events Small (prettified) | 1,253 | 1,315 | 959 | 0.95× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,094 | 2,496 | 1,095 | 0.84× | Two-stage | Win |
| Instruments Small (minified) | 1,757 | 1,396 | 829 | 1.26× | Fused | Win |
| Instruments Small (prettified) | 1,791 | 2,306 | 1,321 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 539 | 503 | 160 | 1.07× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,577 | 1,825 | 597 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 582 | 537 | 344 | 1.08× | Fused | Win |
| Marine IK Small (prettified) | 1,663 | 1,936 | 1,207 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,018 | 929 | 635 | 1.10× | Fused | Win |
| Random Small (prettified) | 1,225 | 1,437 | 977 | 0.85× | Two-stage | Win |
| Twitter Small (minified) | 1,629 | 1,457 | 1,334 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 1,531 | 1,840 | 1,671 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,082 | 642 | 279 | 3.24× | Fused | Win |
| Double (POD) | 1,389 | 786 | 362 | 1.77× | Fused | Win |
| Int64 (POD) | 3,367 | 1,789 | 780 | 1.88× | Fused | Win |
| String (POD) | 2,812 | 1,754 | 1,931 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,510 | 1,653 | 703 | 2.12× | Fused | Win |
| Apache Builds (minified) | 2,700 | 2,266 | 1,602 | 1.19× | Fused | Win |
| Canada (minified) | 1,292 | 1,143 | 789 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 2,406 | 2,781 | 2,068 | 0.86× | Two-stage | Win |
| Canada (prettified) | 2,390 | 2,517 | 1,783 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 2,649 | 1,898 | 1,135 | 1.40× | Fused | Win |
| CitmCatalog (prettified) | 3,135 | 3,910 | 2,429 | 0.80× | Two-stage | Win |
| Discord (minified) | 3,142 | 2,299 | 1,098 | 1.37× | Fused | Win |
| Discord (prettified) | 2,141 | 3,298 | 1,602 | 0.65× | Two-stage | Win |
| Github Events (minified) | 2,203 | 2,066 | 1,124 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 2,763 | 1,969 | 1,098 | 1.40× | Fused | Win |
| Github Events (prettified) | 1,926 | 2,518 | 1,338 | 0.76× | Two-stage | Win |
| Google Maps Response (prettified) | 2,933 | 3,630 | 2,234 | 0.81× | Two-stage | Win |
| Instruments (minified) | 2,933 | 2,185 | 785 | 1.34× | Fused | Win |
| Instruments (prettified) | 2,328 | 3,224 | 1,300 | 0.72× | Two-stage | Win |
| Marine IK Reverse (minified) | 936 | 819 | 606 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 2,704 | 2,811 | 2,220 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 955 | 821 | 606 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,652 | 2,850 | 2,172 | 0.93× | Two-stage | Win |
| Random (minified) | 1,730 | 1,465 | 1,011 | 1.18× | Fused | Win |
| Random (prettified) | 1,765 | 2,198 | 1,514 | 0.80× | Two-stage | Win |
| Twitter (minified) | 2,366 | 2,099 | 1,028 | 1.13× | Fused | Win |
| Twitter (prettified) | 1,772 | 2,686 | 1,358 | 0.66× | Two-stage | Win |
| Apache Builds Small (minified) | 2,945 | 2,496 | 1,609 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,286 | 1,184 | 809 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,328 | 3,091 | 2,040 | 0.75× | Two-stage | Win |
| Canada Small (prettified) | 2,312 | 2,629 | 1,912 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,982 | 1,556 | 1,052 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,006 | 2,656 | 1,820 | 0.76× | Two-stage | Win |
| Discord Small (minified) | 2,580 | 2,087 | 852 | 1.24× | Fused | Win |
| Discord Small (prettified) | 1,938 | 2,809 | 1,160 | 0.69× | Two-stage | Win |
| Github Events Small (minified) | 2,468 | 2,252 | 1,577 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 2,595 | 1,858 | 1,150 | 1.40× | Fused | Win |
| Github Events Small (prettified) | 2,270 | 2,727 | 1,968 | 0.83× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,719 | 3,498 | 2,314 | 0.78× | Two-stage | Win |
| Instruments Small (minified) | 2,776 | 2,001 | 687 | 1.39× | Fused | Win |
| Instruments Small (prettified) | 2,179 | 3,105 | 1,139 | 0.70× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 991 | 836 | 550 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,439 | 2,984 | 2,021 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 1,083 | 915 | 555 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 2,523 | 3,126 | 2,025 | 0.81× | Two-stage | Win |
| Random Small (minified) | 2,120 | 1,840 | 965 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,930 | 2,750 | 1,492 | 0.70× | Two-stage | Win |
| Twitter Small (minified) | 2,719 | 2,425 | 1,028 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 1,986 | 3,104 | 1,323 | 0.64× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,552 | 598 | 220 | 4.27× | Fused | Win |
| Double (POD) | 1,290 | 721 | 303 | 1.79× | Fused | Win |
| Int64 (POD) | 3,639 | 1,700 | 724 | 2.14× | Fused | Win |
| String (POD) | 2,785 | 1,598 | 1,626 | 1.74× | Fused | Win |
| Uint64 (POD) | 3,718 | 1,746 | 725 | 2.13× | Fused | Win |
| Apache Builds (minified) | 2,504 | 2,043 | 1,806 | 1.23× | Fused | Win |
| Canada (minified) | 1,392 | 1,183 | 977 | 1.18× | Fused | Win |
| Apache Builds (prettified) | 2,307 | 2,586 | 2,302 | 0.89× | Two-stage | Win |
| Canada (prettified) | 2,645 | 2,595 | 2,166 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,889 | 2,103 | 1,765 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 3,849 | 4,109 | 3,595 | 0.94× | Two-stage | Win |
| Discord (minified) | 3,249 | 2,384 | 1,676 | 1.36× | Fused | Win |
| Discord (prettified) | 2,759 | 3,253 | 2,357 | 0.85× | Two-stage | Win |
| Github Events (minified) | 2,156 | 2,257 | 1,741 | 0.96× | Two-stage | Win |
| Google Maps Response (minified) | 2,786 | 1,934 | 1,167 | 1.44× | Fused | Win |
| Github Events (prettified) | 2,202 | 2,679 | 2,062 | 0.82× | Two-stage | Win |
| Google Maps Response (prettified) | 3,416 | 3,617 | 2,316 | 0.94× | Two-stage | Win |
| Instruments (minified) | 3,840 | 2,470 | 1,748 | 1.55× | Fused | Win |
| Instruments (prettified) | 3,172 | 3,548 | 2,775 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 920 | 778 | 633 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,677 | 2,761 | 2,315 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 938 | 808 | 640 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,727 | 2,830 | 2,318 | 0.96× | Two-stage | Win |
| Random (minified) | 1,596 | 1,423 | 1,420 | 1.12× | Fused | Win |
| Random (prettified) | 1,901 | 2,098 | 2,114 | 0.91× | Two-stage | Tie |
| Twitter (minified) | 2,543 | 2,085 | 1,825 | 1.22× | Fused | Win |
| Twitter (prettified) | 2,334 | 2,708 | 2,358 | 0.86× | Two-stage | Win |
| Apache Builds Small (minified) | 2,812 | 2,382 | 1,944 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,351 | 1,115 | 853 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 2,561 | 2,842 | 2,347 | 0.90× | Two-stage | Win |
| Canada Small (prettified) | 2,538 | 2,579 | 2,028 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,092 | 1,650 | 1,406 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 2,393 | 2,775 | 2,421 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,605 | 2,009 | 1,614 | 1.30× | Fused | Win |
| Discord Small (prettified) | 2,378 | 2,659 | 2,145 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 2,570 | 2,065 | 2,389 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,597 | 1,840 | 1,260 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 2,450 | 2,601 | 2,944 | 0.94× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,280 | 3,439 | 2,429 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,603 | 2,308 | 1,604 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 3,004 | 3,433 | 2,580 | 0.88× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,055 | 866 | 611 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,691 | 3,021 | 2,207 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 1,162 | 912 | 606 | 1.27× | Fused | Win |
| Marine IK Small (prettified) | 2,821 | 3,122 | 2,220 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,910 | 1,770 | 1,393 | 1.08× | Fused | Win |
| Random Small (prettified) | 2,223 | 2,595 | 2,104 | 0.86× | Two-stage | Win |
| Twitter Small (minified) | 3,148 | 2,514 | 2,170 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,722 | 3,064 | 2,679 | 0.89× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,926 | 453 | 185 | 4.25× | Fused | Win |
| Double (POD) | 1,137 | 494 | 152 | 2.30× | Fused | Win |
| Int64 (POD) | 2,895 | 865 | 429 | 3.35× | Fused | Win |
| String (POD) | 2,023 | 892 | 922 | 2.27× | Fused | Win |
| Uint64 (POD) | 2,999 | 906 | 397 | 3.31× | Fused | Win |
| Apache Builds (minified) | 1,944 | 1,409 | 913 | 1.38× | Fused | Win |
| Canada (minified) | 1,022 | 846 | 565 | 1.21× | Fused | Win |
| Apache Builds (prettified) | 1,833 | 1,676 | 1,066 | 1.09× | Fused | Win |
| Canada (prettified) | 2,088 | 1,751 | 1,335 | 1.19× | Fused | Win |
| CitmCatalog (minified) | 2,583 | 1,303 | 1,139 | 1.98× | Fused | Win |
| CitmCatalog (prettified) | 3,184 | 2,434 | 1,984 | 1.31× | Fused | Win |
| Discord (minified) | 1,912 | 1,327 | 1,045 | 1.44× | Fused | Win |
| Discord (prettified) | 1,751 | 1,835 | 1,551 | 0.95× | Two-stage | Win |
| Github Events (minified) | 2,191 | 1,638 | 1,020 | 1.34× | Fused | Win |
| Google Maps Response (minified) | 2,005 | 1,265 | 725 | 1.58× | Fused | Win |
| Github Events (prettified) | 1,852 | 1,817 | 1,260 | 1.02× | Tie | Win |
| Google Maps Response (prettified) | 2,714 | 2,304 | 1,342 | 1.18× | Fused | Win |
| Instruments (minified) | 2,699 | 1,529 | 1,088 | 1.76× | Fused | Win |
| Instruments (prettified) | 2,945 | 2,379 | 1,768 | 1.24× | Fused | Win |
| Marine IK Reverse (minified) | 670 | 573 | 461 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 1,868 | 1,947 | 1,378 (n/c) | 0.96× | Two-stage | Win |
| Marine IK (minified) | 803 | 590 | 481 | 1.36× | Fused | Win |
| Marine IK (prettified) | 2,107 | 1,801 | 1,821 | 1.17× | Fused | Win |
| Random (minified) | 1,366 | 1,182 | 765 | 1.16× | Fused | Win |
| Random (prettified) | 1,638 | 1,488 | 1,182 | 1.10× | Fused | Win |
| Twitter (minified) | 2,577 | 1,874 | 1,022 | 1.38× | Fused | Win |
| Twitter (prettified) | 2,313 | 2,185 | 1,338 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 1,636 | 1,511 | 1,106 | 1.08× | Fused | Win |
| Canada Small (minified) | 798 | 827 | 485 | 0.96× | Two-stage | Win |
| Apache Builds Small (prettified) | 1,893 | 1,906 | 1,313 | 0.99× | Tie | Win |
| Canada Small (prettified) | 2,015 | 1,868 | 1,274 | 1.08× | Fused | Win |
| CitmCatalog Small (minified) | 1,803 | 1,362 | 1,091 | 1.32× | Fused | Win |
| CitmCatalog Small (prettified) | 2,235 | 1,692 | 1,771 | 1.32× | Fused | Win |
| Discord Small (minified) | 1,950 | 1,508 | 1,088 | 1.29× | Fused | Win |
| Discord Small (prettified) | 1,964 | 1,938 | 1,358 | 1.01× | Tie | Win |
| Github Events Small (minified) | 2,197 | 1,631 | 1,480 | 1.35× | Fused | Win |
| Google Maps Response Small (minified) | 2,040 | 1,502 | 883 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 2,257 | 1,973 | 1,678 | 1.14× | Fused | Win |
| Google Maps Response Small (prettified) | 2,935 | 2,661 | 1,561 | 1.10× | Fused | Win |
| Instruments Small (minified) | 2,949 | 1,781 | 1,286 | 1.66× | Fused | Win |
| Instruments Small (prettified) | 3,138 | 2,702 | 2,010 | 1.16× | Fused | Win |
| Marine IK Reverse Small (minified) | 788 | 648 | 465 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,984 | 2,154 | 1,614 | 0.92× | Two-stage | Win |
| Marine IK Small (minified) | 945 | 736 | 428 | 1.28× | Fused | Win |
| Marine IK Small (prettified) | 2,334 | 2,368 | 1,606 | 0.99× | Two-stage | Win |
| Random Small (minified) | 1,659 | 1,407 | 949 | 1.18× | Fused | Win |
| Random Small (prettified) | 1,986 | 1,853 | 1,411 | 1.07× | Fused | Win |
| Twitter Small (minified) | 2,839 | 2,235 | 1,155 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,612 | 2,647 | 1,572 | 0.99× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,245 | 335 | 181 | 3.72× | Fused | Win |
| Double (POD) | 1,101 | 400 | 221 | 2.75× | Fused | Win |
| Int64 (POD) | 2,555 | 718 | 498 | 3.56× | Fused | Win |
| String (POD) | 2,454 | 975 | 1,238 | 2.52× | Fused | Win |
| Uint64 (POD) | 2,819 | 785 | 494 | 3.59× | Fused | Win |
| Apache Builds (minified) | 2,317 | 1,498 | 1,079 | 1.55× | Fused | Win |
| Canada (minified) | 1,035 | 816 | 533 | 1.27× | Fused | Win |
| Apache Builds (prettified) | 2,252 | 1,826 | 1,370 | 1.23× | Fused | Win |
| Canada (prettified) | 1,769 | 1,489 | 1,108 | 1.19× | Fused | Win |
| CitmCatalog (minified) | 2,415 | 1,503 | 855 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | 3,102 | 2,928 | 1,747 | 1.06× | Fused | Win |
| Discord (minified) | 2,931 | 1,584 | 761 | 1.85× | Fused | Win |
| Discord (prettified) | 2,564 | 2,038 | 1,134 | 1.26× | Fused | Win |
| Github Events (minified) | 2,261 | 1,523 | 844 | 1.48× | Fused | Win |
| Google Maps Response (minified) | 1,977 | 1,205 | 792 | 1.64× | Fused | Win |
| Github Events (prettified) | 2,286 | 2,233 | 1,137 | 1.02× | Fused | Win |
| Google Maps Response (prettified) | 3,010 | 2,468 | 1,356 | 1.22× | Fused | Win |
| Instruments (minified) | 2,782 | 1,940 | 530 | 1.43× | Fused | Win |
| Instruments (prettified) | 2,782 | 2,441 | 984 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 659 | 588 | 420 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,129 | 1,925 | 1,454 | 1.11× | Fused | Win |
| Marine IK (minified) | 789 (n/c) | 620 | 366 | 1.27× | Fused | Win |
| Marine IK (prettified) | 1,995 | 2,020 | 1,479 | 0.99× | Tie | Win |
| Random (minified) | 1,461 | 1,352 | 756 | 1.08× | Fused | Win |
| Random (prettified) | 1,912 | 1,643 | 936 | 1.16× | Fused | Win |
| Twitter (minified) | 2,778 | 1,848 | 768 | 1.50× | Fused | Win |
| Twitter (prettified) | 2,265 | 2,561 | 1,062 | 0.88× | Two-stage | Win |
| Apache Builds Small (minified) | 2,092 | 1,737 | 951 | 1.20× | Fused | Win |
| Canada Small (minified) | 907 | 850 | 495 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 2,127 | 2,057 | 1,543 | 1.03× | Fused | Win |
| Canada Small (prettified) | 1,773 | 1,696 | 1,153 | 1.05× | Fused | Win |
| CitmCatalog Small (minified) | 1,838 | 1,086 | 851 | 1.69× | Fused | Win |
| CitmCatalog Small (prettified) | 1,750 | 2,039 | 1,225 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,542 | 1,406 | 671 | 1.81× | Fused | Win |
| Discord Small (prettified) | 2,228 | 1,910 | 862 | 1.17× | Fused | Win |
| Github Events Small (minified) | 2,170 | 1,681 | 1,160 | 1.29× | Fused | Win |
| Google Maps Response Small (minified) | 2,232 | 1,315 | 842 | 1.70× | Fused | Win |
| Github Events Small (prettified) | 1,873 | 2,025 | 1,320 | 0.93× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,828 | 2,289 | 1,428 | 1.24× | Fused | Win |
| Instruments Small (minified) | 2,638 | 1,875 | 479 | 1.41× | Fused | Win |
| Instruments Small (prettified) | 2,871 | 2,415 | 883 | 1.19× | Fused | Win |
| Marine IK Reverse Small (minified) | 640 | 583 | 305 | 1.10× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,847 | 2,197 | 1,171 | 0.84× | Two-stage | Win |
| Marine IK Small (minified) | 769 | 682 | 351 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 2,145 | 2,230 | 1,256 | 0.96× | Two-stage | Win |
| Random Small (minified) | 1,917 | 1,326 | 741 | 1.45× | Fused | Win |
| Random Small (prettified) | 1,862 | 1,591 | 1,017 | 1.17× | Fused | Win |
| Twitter Small (minified) | 2,718 | 2,185 | 838 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 2,523 | 2,507 | 1,078 | 1.01× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests, 32 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 29 of the 33 full-size prettified documents and 31 of the 33 Small prettified ones. MSVC follows the same split: all 22 minified documents go to the fused path, and of the 22 prettified ones the tape wins 18, the fused path 2 (Apache Builds Small and Github Events Small) and 2 are ties (Apache Builds and Marine IK Reverse). On the M2 Pro the fused path wins all POD input, 43 of the 44 minified documents (1 tie, none to the tape) and 32 of the 44 prettified documents (9 to the tape, 3 ties).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.21× (String, Windows/MSVC) to 4.30× (Bool, Linux/GCC) faster than the two-stage path, and 1.32× (Double, Windows/MSVC) to 4.27× (Bool, Linux/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, at both sizes.** The fused path wins 108 of the 110 minified documents: all 22 under MSVC, on Linux/Clang and on macOS/GCC, 21 of 22 on Linux/GCC, where the two-stage path wins minified Github Events by 9% (1,741 against 1,599 MB/s), and 21 of 22 on macOS/Clang, where minified Apache Builds Small is a tie (1,336 against 1,347). The fused path's margins run from 3% (Random under MSVC, 769 against 748 MB/s) to 1.89× (Discord on macOS/Clang, 2,361 against 1,248 MB/s) on the full-size documents and up to 1.87× on the Small ones (Google Maps Response Small on macOS/Clang, 1,906 against 1,017 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 60 of 66.** It wins 21 of 22 on Linux/Clang, where prettified Marine IK is a tie (2,498 MB/s fused against 2,474), 21 of 22 on Linux/GCC, where the fused path wins prettified Canada by 1% (2,210 against 2,185), and 18 of 22 under MSVC, where the fused path wins prettified Apache Builds Small and Github Events Small, by 3% and 1%, and prettified Apache Builds and Marine IK Reverse are ties (1,013 against 1,011 and 1,800 against 1,806). The tape's margins run from 2% (Marine IK under MSVC, 1,831 against 1,799 MB/s) to 41% (Twitter on Linux/Clang, 2,153 against 1,531) on the full-size documents and from 2% (Canada Small on Linux/GCC) to 45% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 63 of the 66 prettified x86 documents: the fused path wins Apache Builds and Apache Builds Small under MSVC and Canada on Linux/GCC, and there are no ties.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 32, the tape wins 9 and 3 are ties. On the 22 full-size documents the fused path wins 15, 2 are ties (Marine IK Reverse on both compilers) and the tape takes 5: prettified Marine IK on macOS/GCC, and Canada, Github Events, Instruments and Twitter on macOS/Clang, the widest of them Github Events at 1,726 against 1,607 MB/s (1.07×). The 22 Small copies split 17 / 1 / 4: the tape takes Marine IK Small on macOS/GCC and Marine IK Small, Marine IK Reverse Small and Twitter Small on macOS/Clang, its widest M2 Pro margin on Marine IK Reverse Small at 1,829 against 1,644 MB/s (1.11×), and Twitter Small on macOS/GCC is a tie. By compiler, the fused path wins 18 of 22 on macOS/GCC (2 to the tape, 2 ties) and 14 of 22 on macOS/Clang (7 to the tape, 1 tie). Reused, the fused path wins 30, the tape 9, and 5 are ties: 14, 4 and 4 on macOS/GCC, and 16, 5 and 1 on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Prettified Github Events Small on Linux/GCC is where Jsonifier is weakest.** It is the faster Jsonifier path's only freshly allocated loss to simdjson On Demand, by 13% (2,142 against 2,473 MB/s), and there are no ties. With objects reused it is again the only loss, by 12% (2,601 against 2,944), and prettified Random on Linux/GCC is a tie (2,098 against 2,114). We have not profiled what sets this document apart.

**MSVC slows all three by a similar median share.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 63% of its Linux/Clang throughput, the two-stage path 66% and simdjson 65%. The spread between documents is narrowest for the fused path (from 47% on Discord to 69% on CitmCatalog), wider for the two-stage path (from 52% on Apache Builds to 78% on CitmCatalog) and widest for simdjson, which keeps 51% of its Linux/Clang throughput on Google Maps Response and nearly all of it on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 244 of 245 tests with whichever path is faster, ties none and loses one** (prettified Github Events Small on Linux/GCC). The fused path alone wins 239, ties 1 and loses 5, all prettified: its four extra losses are three on Linux/GCC (Random, CitmCatalog Small and Twitter Small) and one under MSVC (Twitter Small), and its tie is prettified Twitter on Linux/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 199 MB/s, against 349 MB/s for the same document in declared order, so the faster Jsonifier path is 2.83× faster than simdjson freshly allocated (3.03× reused). The prettified form shows the same gap, at 667 MB/s against 1,218, and the faster Jsonifier path is 2.71× faster (2.92× reused). The Small copies show it too, at 154 MB/s minified and 575 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, or faster, on Linux and macOS/Clang, and up to about a quarter slower on macOS/GCC (375 against 488 MB/s minified), and the faster Jsonifier path is 1.10× to 1.61× faster than simdjson on the full-size reversed document, freshly allocated, and 1.19× to 1.57× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, minified input belongs to the fused path, and so do about three quarters of the prettified documents. Routing each test to its faster path would raise the record against simdjson from 239 wins, 1 tie and 5 losses to 244 wins, 0 ties and 1 loss. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,406 | 1,271 | 1.11× | Win |
| Canada (minified) | 971 | 808 | 1.20× | Win |
| Apache Builds (prettified) | 1,786 | 1,640 | 1.09× | Win |
| Canada (prettified) | 2,185 | 1,801 | 1.21× | Win |
| CitmCatalog (minified) | 1,758 | 1,390 | 1.27× | Win |
| CitmCatalog (prettified) | 3,531 | 2,905 | 1.22× | Win |
| Discord (minified) | 1,822 | 1,567 | 1.16× | Win |
| Discord (prettified) | 2,537 | 2,209 | 1.15× | Win |
| Github Events (minified) | 1,741 | 1,605 | 1.09× | Win |
| Google Maps Response (minified) | 1,766 | 1,337 | 1.32× | Win |
| Github Events (prettified) | 2,087 | 2,033 | 1.03× | Win |
| Google Maps Response (prettified) | 3,359 | 2,652 | 1.27× | Win |
| Instruments (minified) | 2,175 | 1,645 | 1.32× | Win |
| Instruments (prettified) | 3,186 | 2,612 | 1.22× | Win |
| Marine IK Reverse (minified) | 701 | 623 | 1.13× | Win |
| Marine IK Reverse (prettified) | 2,569 | 2,243 | 1.15× | Win |
| Marine IK (minified) | 716 | 606 | 1.18× | Win |
| Marine IK (prettified) | 2,583 | 2,225 | 1.16× | Win |
| Random (minified) | 1,068 | 1,109 | 0.96× | **Loss** |
| Random (prettified) | 1,610 | 1,648 | 0.98× | **Loss** |
| Twitter (minified) | 1,655 | 1,482 | 1.12× | Win |
| Twitter (prettified) | 2,161 | 1,943 | 1.11× | Win |
| Apache Builds Small (minified) | 2,049 | 1,740 | 1.18× | Win |
| Canada Small (minified) | 900 | 738 | 1.22× | Win |
| Apache Builds Small (prettified) | 2,391 | 2,179 | 1.10× | Win |
| Canada Small (prettified) | 2,131 | 1,730 | 1.23× | Win |
| CitmCatalog Small (minified) | 1,393 | 1,222 | 1.14× | Win |
| CitmCatalog Small (prettified) | 2,396 | 2,047 | 1.17× | Win |
| Discord Small (minified) | 1,586 | 1,479 | 1.07× | Win |
| Discord Small (prettified) | 2,162 | 1,974 | 1.10× | Win |
| Github Events Small (minified) | 1,736 | 1,783 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,650 | 1,371 | 1.20× | Win |
| Github Events Small (prettified) | 2,142 | 2,188 | 0.98× | **Loss** |
| Google Maps Response Small (prettified) | 3,106 | 2,675 | 1.16× | Win |
| Instruments Small (minified) | 2,124 | 1,626 | 1.31× | Win |
| Instruments Small (prettified) | 3,199 | 2,581 | 1.24× | Win |
| Marine IK Reverse Small (minified) | 725 | 642 | 1.13× | Win |
| Marine IK Reverse Small (prettified) | 2,576 | 2,307 | 1.12× | Win |
| Marine IK Small (minified) | 755 | 640 | 1.18× | Win |
| Marine IK Small (prettified) | 2,635 | 2,284 | 1.15× | Win |
| Random Small (minified) | 1,455 | 1,226 | 1.19× | Win |
| Random Small (prettified) | 2,154 | 1,823 | 1.18× | Win |
| Twitter Small (minified) | 2,265 | 1,861 | 1.22× | Win |
| Twitter Small (prettified) | 2,692 | 2,383 | 1.13× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 963 | 799 | 1.20× | Win |
| Canada (minified) | 705 | 572 | 1.23× | Win |
| Apache Builds (prettified) | 1,192 | 1,007 | 1.18× | Win |
| Canada (prettified) | 1,497 | 1,337 | 1.12× | Win |
| CitmCatalog (minified) | 1,389 | 1,021 | 1.36× | Win |
| CitmCatalog (prettified) | 2,495 | 1,898 | 1.31× | Win |
| Discord (minified) | 1,059 | 883 | 1.20× | Win |
| Discord (prettified) | 1,466 | 1,314 | 1.12× | Win |
| Github Events (minified) | 1,203 | 1,071 | 1.12× | Win |
| Google Maps Response (minified) | 1,082 | 859 | 1.26× | Win |
| Github Events (prettified) | 1,351 | 1,302 | 1.04× | Win |
| Google Maps Response (prettified) | 2,034 | 1,644 | 1.24× | Win |
| Instruments (minified) | 1,542 | 1,098 | 1.40× | Win |
| Instruments (prettified) | 1,995 | 1,693 | 1.18× | Win |
| Marine IK Reverse (minified) | 487 | 455 (n/c) | 1.07× | Win |
| Marine IK Reverse (prettified) | 1,639 | 1,693 | 0.97× | **Loss** |
| Marine IK (minified) | 542 | 484 (n/c) | 1.12× | Win |
| Marine IK (prettified) | 1,793 | 1,791 | 1.00× | Tie |
| Random (minified) | 813 | 645 | 1.26× | Win |
| Random (prettified) | 1,003 | 1,013 | 0.99× | Tie |
| Twitter (minified) | 1,540 | 1,167 | 1.32× | Win |
| Twitter (prettified) | 1,706 | 1,471 | 1.16× | Win |
| Apache Builds Small (minified) | 1,264 | 780 | 1.62× | Win |
| Canada Small (minified) | 669 | 510 | 1.31× | Win |
| Apache Builds Small (prettified) | 1,427 | 1,198 | 1.19× | Win |
| Canada Small (prettified) | 1,591 | 1,369 | 1.16× | Win |
| CitmCatalog Small (minified) | 1,024 | 661 | 1.55× | Win |
| CitmCatalog Small (prettified) | 1,412 | 1,186 | 1.19× | Win |
| Discord Small (minified) | 1,304 | 865 | 1.51× | Win |
| Discord Small (prettified) | 1,506 | 1,319 | 1.14× | Win |
| Github Events Small (minified) | 1,294 | 1,098 | 1.18× | Win |
| Google Maps Response Small (minified) | 1,136 | 972 | 1.17× | Win |
| Github Events Small (prettified) | 1,438 | 1,371 | 1.05× | Win |
| Google Maps Response Small (prettified) | 2,307 | 1,725 | 1.34× | Win |
| Instruments Small (minified) | 1,472 | 1,275 | 1.15× | Win |
| Instruments Small (prettified) | 2,494 | 1,978 | 1.26× | Win |
| Marine IK Reverse Small (minified) | 532 | 529 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 1,598 | 1,795 | 0.89× | **Loss** |
| Marine IK Small (minified) | 593 | 438 | 1.35× | Win |
| Marine IK Small (prettified) | 1,989 | 1,786 | 1.11× | Win |
| Random Small (minified) | 1,048 | 839 | 1.25× | Win |
| Random Small (prettified) | 1,386 | 1,127 | 1.23× | Win |
| Twitter Small (minified) | 1,871 | 1,150 | 1.63× | Win |
| Twitter Small (prettified) | 2,246 | 1,771 | 1.27× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,043 | 1,623 | 1.26× | Win |
| Canada (minified) | 1,183 | 949 | 1.25× | Win |
| Apache Builds (prettified) | 2,586 | 2,063 | 1.25× | Win |
| Canada (prettified) | 2,595 | 2,147 | 1.21× | Win |
| CitmCatalog (minified) | 2,103 | 1,639 | 1.28× | Win |
| CitmCatalog (prettified) | 4,109 | 3,342 | 1.23× | Win |
| Discord (minified) | 2,384 | 1,781 | 1.34× | Win |
| Discord (prettified) | 3,253 | 2,513 | 1.29× | Win |
| Github Events (minified) | 2,257 | 1,851 | 1.22× | Win |
| Google Maps Response (minified) | 1,934 | 1,409 | 1.37× | Win |
| Github Events (prettified) | 2,679 | 2,185 | 1.23× | Win |
| Google Maps Response (prettified) | 3,617 | 2,812 | 1.29× | Win |
| Instruments (minified) | 2,470 | 1,681 | 1.47× | Win |
| Instruments (prettified) | 3,548 | 2,674 | 1.33× | Win |
| Marine IK Reverse (minified) | 778 | 684 | 1.14× | Win |
| Marine IK Reverse (prettified) | 2,761 | 2,459 | 1.12× | Win |
| Marine IK (minified) | 808 | 677 | 1.19× | Win |
| Marine IK (prettified) | 2,830 | 2,458 | 1.15× | Win |
| Random (minified) | 1,423 | 1,440 | 0.99× | **Loss** |
| Random (prettified) | 2,098 | 2,145 | 0.98× | **Loss** |
| Twitter (minified) | 2,085 | 1,718 | 1.21× | Win |
| Twitter (prettified) | 2,708 | 2,232 | 1.21× | Win |
| Apache Builds Small (minified) | 2,382 | 1,892 | 1.26× | Win |
| Canada Small (minified) | 1,115 | 844 | 1.32× | Win |
| Apache Builds Small (prettified) | 2,842 | 2,315 | 1.23× | Win |
| Canada Small (prettified) | 2,579 | 1,988 | 1.30× | Win |
| CitmCatalog Small (minified) | 1,650 | 1,305 | 1.26× | Win |
| CitmCatalog Small (prettified) | 2,775 | 2,272 | 1.22× | Win |
| Discord Small (minified) | 2,009 | 1,804 | 1.11× | Win |
| Discord Small (prettified) | 2,659 | 2,367 | 1.12× | Win |
| Github Events Small (minified) | 2,065 | 2,011 | 1.03× | Win |
| Google Maps Response Small (minified) | 1,840 | 1,464 | 1.26× | Win |
| Github Events Small (prettified) | 2,601 | 2,504 | 1.04× | Win |
| Google Maps Response Small (prettified) | 3,439 | 2,864 | 1.20× | Win |
| Instruments Small (minified) | 2,308 | 1,676 | 1.38× | Win |
| Instruments Small (prettified) | 3,433 | 2,668 | 1.29× | Win |
| Marine IK Reverse Small (minified) | 866 | 717 | 1.21× | Win |
| Marine IK Reverse Small (prettified) | 3,021 | 2,555 | 1.18× | Win |
| Marine IK Small (minified) | 912 | 719 | 1.27× | Win |
| Marine IK Small (prettified) | 3,122 | 2,544 | 1.23× | Win |
| Random Small (minified) | 1,770 | 1,385 | 1.28× | Win |
| Random Small (prettified) | 2,595 | 2,087 | 1.24× | Win |
| Twitter Small (minified) | 2,514 | 2,001 | 1.26× | Win |
| Twitter Small (prettified) | 3,064 | 2,550 | 1.20× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,409 | 956 | 1.47× | Win |
| Canada (minified) | 846 | 684 | 1.24× | Win |
| Apache Builds (prettified) | 1,676 | 1,188 | 1.41× | Win |
| Canada (prettified) | 1,751 | 1,549 | 1.13× | Win |
| CitmCatalog (minified) | 1,303 | 1,073 | 1.21× | Win |
| CitmCatalog (prettified) | 2,434 | 2,304 | 1.06× | Win |
| Discord (minified) | 1,327 | 1,199 | 1.11× | Win |
| Discord (prettified) | 1,835 | 1,419 | 1.29× | Win |
| Github Events (minified) | 1,638 | 1,301 | 1.26× | Win |
| Google Maps Response (minified) | 1,265 | 1,043 | 1.21× | Win |
| Github Events (prettified) | 1,817 | 1,490 | 1.22× | Win |
| Google Maps Response (prettified) | 2,304 | 1,868 | 1.23× | Win |
| Instruments (minified) | 1,529 | 1,108 | 1.38× | Win |
| Instruments (prettified) | 2,379 | 1,761 | 1.35× | Win |
| Marine IK Reverse (minified) | 573 | 512 | 1.12× | Win |
| Marine IK Reverse (prettified) | 1,947 | 1,912 | 1.02× | Win |
| Marine IK (minified) | 590 | 550 (n/c) | 1.07× | Win |
| Marine IK (prettified) | 1,801 | 1,964 | 0.92× | **Loss** |
| Random (minified) | 1,182 | 785 | 1.50× | Win |
| Random (prettified) | 1,488 | 1,124 | 1.32× | Win |
| Twitter (minified) | 1,874 | 1,246 | 1.50× | Win |
| Twitter (prettified) | 2,185 | 1,720 | 1.27× | Win |
| Apache Builds Small (minified) | 1,511 | 1,105 | 1.37× | Win |
| Canada Small (minified) | 827 | 657 | 1.26× | Win |
| Apache Builds Small (prettified) | 1,906 | 1,532 | 1.24× | Win |
| Canada Small (prettified) | 1,868 | 1,584 | 1.18× | Win |
| CitmCatalog Small (minified) | 1,362 | 1,009 | 1.35× | Win |
| CitmCatalog Small (prettified) | 1,692 | 1,586 | 1.07× | Win |
| Discord Small (minified) | 1,508 | 1,135 | 1.33× | Win |
| Discord Small (prettified) | 1,938 | 1,607 | 1.21× | Win |
| Github Events Small (minified) | 1,631 | 1,296 | 1.26× | Win |
| Google Maps Response Small (minified) | 1,502 | 1,076 | 1.40× | Win |
| Github Events Small (prettified) | 1,973 | 1,540 | 1.28× | Win |
| Google Maps Response Small (prettified) | 2,661 | 2,009 | 1.32× | Win |
| Instruments Small (minified) | 1,781 | 1,111 | 1.60× | Win |
| Instruments Small (prettified) | 2,702 | 2,066 | 1.31× | Win |
| Marine IK Reverse Small (minified) | 648 | 638 | 1.02× | Win |
| Marine IK Reverse Small (prettified) | 2,154 | 1,960 | 1.10× | Win |
| Marine IK Small (minified) | 736 | 563 | 1.31× | Win |
| Marine IK Small (prettified) | 2,368 | 2,107 | 1.12× | Win |
| Random Small (minified) | 1,407 | 994 | 1.42× | Win |
| Random Small (prettified) | 1,853 | 1,463 | 1.27× | Win |
| Twitter Small (minified) | 2,235 | 1,325 | 1.69× | Win |
| Twitter Small (prettified) | 2,647 | 1,985 | 1.33× | Win |

In the freshly allocated run, two-stage Jsonifier wins 79 of these 88 tests, ties 3 and loses 6, by 2% to 11%. Four losses are on Linux/GCC: Random and Github Events Small, each in both forms (0.96× to 0.98×). The other two are on macOS/GCC: prettified Marine IK Reverse (0.97×) and prettified Marine IK Reverse Small, the widest at 0.89× (1,598 against 1,795 MB/s). The ties are all on macOS/GCC: prettified Marine IK and Random, and minified Marine IK Reverse Small. With the fused path instead, Jsonifier beats the reflection reader on 78, ties 1 and loses 9. With objects reused, two-stage Jsonifier wins 85, ties none and loses 3: minified and prettified Random on Linux/GCC by 1% and 2% (1,423 against 1,440 MB/s and 2,098 against 2,145) and prettified Marine IK on macOS/GCC by 8% (1,801 against 1,964). The fused path beats the reflection reader on 85, ties 1 and loses 2. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 33 of the 40 results; all 7 wider gaps are on the macOS builds, and the widest is reused prettified Twitter on macOS/Clang, where the "jsonifier" row runs at 4,405 MB/s against the two-stage row's 5,485. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,771 | 3,776 | 1.79× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,571 | 4,348 | 1.74× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,075 | 3,447 | 1.18× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 4,921 | 4,069 | 1.21× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,853 | 6,763 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,201 | 8,062 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,186 | 5,403 | 0.96× | Loss |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,157 | 6,647 | 0.93× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,702 | 6,867 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,692 | 8,125 | 1.07× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,507 | 5,509 | 1.00× | Tie |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,332 | 6,683 | 0.95× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,298 | 3,009 | 1.43× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,757 | 4,770 | 1.00× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,945 | 3,795 | 1.04× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,374 | 4,418 | 0.99× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,993 | 3,696 | 1.35× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,315 | 4,203 | 1.26× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,361 | 3,030 | 1.11× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,980 | 3,963 | 1.00× | Tie |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (minified) | 6,947 | 3,913 | 1.78× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter (prettified) | 7,745 | 4,486 | 1.73× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,406 | 3,740 | 1.18× | Win |
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,241 | 4,383 | 1.20× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,974 | 6,902 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,420 | 8,183 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,566 | 5,525 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,585 | 6,684 | 0.99× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,797 | 6,962 | 1.12× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,836 | 8,181 | 1.08× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,744 | 5,662 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,683 | 6,752 | 0.99× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,477 | 3,783 | 1.18× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,770 | 4,848 | 0.98× | Tie |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,275 | 3,970 | 1.08× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,558 | 4,581 | 0.99× | Tie |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,654 | 3,344 | 1.39× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,485 | 4,253 | 1.29× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,317 | 3,230 | 1.03× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,072 | 4,066 | 1.00× | Tie |

Jsonifier wins 28 of the 40 results, ties 7 and loses 5, and wins by up to 1.79× (minified Twitter under MSVC, freshly allocated). Freshly allocated it wins 13, ties 4 and loses 3: the losses are minified and prettified Twitter Small on Linux/Clang (0.96× and 0.93×) and prettified Twitter Small on Linux/GCC (0.95×); the ties are minified Twitter Small on Linux/GCC, prettified Twitter and Twitter Small on macOS/GCC, and prettified Twitter Small on macOS/Clang. The reused run has 15 wins, 3 ties and 2 losses: prettified Twitter Small on Linux/Clang and on Linux/GCC (0.99× each); the ties are prettified Twitter and Twitter Small on macOS/GCC and prettified Twitter Small on macOS/Clang. On the three x86 builds every full-size result is a win, by 1.07× to 1.79×; on the M2 Pro the full-size document wins 6 of its 8 results, tying prettified Twitter on macOS/GCC in both runs. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document on x86 this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.03× to 1.37×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.51 (i9-14900KF, AVX2) | 2,666 | 2,462 | 1.08× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,459 | 2,538 | 1.36× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,941 | 2,143 | 1.37× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,106 | 2,053 | 1.03× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,578 | 1,979 | 1.30× | Win |

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