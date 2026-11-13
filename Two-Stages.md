# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on all 25 POD-type tests and on 105 of the 110 minified documents (the tape takes the other five, all under MSVC), the tape is faster on 63 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 34 of the 44 prettified documents, the tape 9 and 1 is a tie.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published November 13, 2026, with Jsonifier [4919934](https://github.com/nihilai-collective/jsonifier/commit/4919934), Glaze [232e9f5](https://github.com/stephenberry/glaze/commit/232e9f50), simdjson [449f38e](https://github.com/simdjson/simdjson/commit/449f38e63) and BenchmarkSuite [27950a9](https://github.com/nihilai-collective/benchmarksuite/commit/27950a9) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Five freshly allocated full-parse rows contain non-converged results (three under MSVC, one on macOS/GCC and one on macOS/Clang), and five reused rows do (two under MSVC, one on macOS/GCC and two on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 166 tests, 2 are statistical ties, and the two-stage path wins 77. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 22 | 1 | 26 | 46 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 41 | 1 | 7 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 | 0 | 2 | 48 of 49 |
| **Aggregate** | **166** | **2** | **77** | **240 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 1 / 10 | 1 / 0 / 21 | 19 / 1 / 2 |
| Small minified documents | 7 / 0 / 4 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 0 / 0 / 11 | 1 / 0 / 21 | 15 / 0 / 7 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 48 / 1 / 0 | 47 / 1 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 47 / 0 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **244 / 1 / 0** | **241 / 1 / 3** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 24 | 0 | 25 | 47 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 22 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 29 | 0 | 20 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 43 | 0 | 6 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 43 | 2 | 4 | 47 of 49 |
| **Aggregate** | **166** | **2** | **77** | **240 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 1 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 1 / 0 / 10 | 1 / 0 / 21 | 18 / 1 / 3 |
| Small minified documents | 8 / 0 / 3 | 22 / 0 / 0 | 21 / 0 / 1 |
| Small prettified documents | 0 / 0 / 11 | 1 / 0 / 21 | 15 / 1 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 47 / 0 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **242 / 0 / 3** |

With objects reused, across 245 tests the fused path wins 166, ties 2 and loses 77 to the two-stage path, and the x86 split holds: the tape wins 31 of the 33 full-size prettified documents (the fused path takes Marine IK Reverse under MSVC and Canada on Linux/GCC) and 32 of the 33 Small prettified ones (the fused path takes Canada Small on Linux/GCC), and the fused path wins 62 of the 66 minified documents, the tape taking Marine IK Reverse and the Small copies of Apache Builds, Marine IK Reverse and Marine IK under MSVC. On the M2 Pro the fused path wins 43 of the 44 minified documents, with Random Small on macOS/GCC to the tape, and 33 of the 44 prettified ones, with 2 ties and 9 tape wins. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins 242, ties none and loses 3.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 667 | 268 | 125 | 2.49× | Fused | Win |
| Double (POD) | 914 | 558 | 258 | 1.64× | Fused | Win |
| Int64 (POD) | 1,597 | 883 | 574 | 1.81× | Fused | Win |
| String (POD) | 1,003 | 797 | 749 | 1.26× | Fused | Win |
| Uint64 (POD) | 1,697 | 919 | 574 | 1.85× | Fused | Win |
| Apache Builds (minified) | 938 | 880 | 739 | 1.07× | Fused | Win |
| Canada (minified) | 730 | 691 | 432 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,096 | 1,122 | 965 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,481 | 1,535 | 976 | 0.96× | Two-stage | Win |
| CitmCatalog (minified) | 1,624 | 1,306 | 628 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,355 | 2,654 | 1,339 | 0.89× | Two-stage | Win |
| Discord (minified) | 1,349 | 1,094 | 755 | 1.23× | Fused | Win |
| Discord (prettified) | 1,437 | 1,567 | 1,101 | 0.92× | Two-stage | Win |
| Github Events (minified) | 1,079 | 954 | 734 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 1,409 | 1,205 | 601 | 1.17× | Fused | Win |
| Github Events (prettified) | 1,125 | 1,174 | 800 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 2,205 | 2,336 | 1,207 | 0.94× | Two-stage | Win |
| Instruments (minified) | 2,222 | 1,493 | 873 | 1.49× | Fused | Win |
| Instruments (prettified) | 2,136 | 2,355 | 1,412 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 453 | 569 | 197 | 0.80× | Two-stage | Win |
| Marine IK Reverse (prettified) | 1,925 | 2,017 | 672 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 665 | 568 | 340 | 1.17× | Fused | Win |
| Marine IK (prettified) | 1,992 | 2,074 | 1,280 | 0.96× | Two-stage | Win |
| Random (minified) | 820 | 744 | 582 | 1.10× | Fused | Win |
| Random (prettified) | 1,032 | 1,139 | 875 | 0.91× | Two-stage | Win |
| Twitter (minified) | 1,066 (n/c) | 885 | 810 | 1.20× | Fused | Win |
| Twitter (prettified) | 756 (n/c) | 1,101 (n/c) | 901 (n/c) | 0.69× | Tie | Tie |
| Apache Builds Small (minified) | 1,271 | 1,349 | 771 | 0.94× | Two-stage | Win |
| Canada Small (minified) | 668 | 625 | 361 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 1,327 | 1,604 | 979 | 0.83× | Two-stage | Win |
| Canada Small (prettified) | 1,390 | 1,470 | 858 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 868 | 752 | 552 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 1,180 | 1,291 | 969 | 0.91× | Two-stage | Win |
| Discord Small (minified) | 1,201 | 960 | 714 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,208 | 1,302 | 963 | 0.93× | Two-stage | Win |
| Github Events Small (minified) | 1,019 | 1,043 | 791 | 0.98× | Two-stage | Win |
| Google Maps Response Small (minified) | 1,208 | 1,055 | 582 | 1.14× | Fused | Win |
| Github Events Small (prettified) | 1,091 (n/c) | 1,340 | 891 | 0.81× | Two-stage | Win |
| Google Maps Response Small (prettified) | 1,954 | 2,081 | 1,176 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 2,231 | 1,499 | 872 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,094 | 2,290 | 1,448 | 0.91× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 337 | 446 | 161 | 0.76× | Two-stage | Win |
| Marine IK Reverse Small (prettified) | 1,332 | 1,630 | 616 | 0.82× | Two-stage | Win |
| Marine IK Small (minified) | 455 | 477 | 310 | 0.95× | Two-stage | Win |
| Marine IK Small (prettified) | 1,549 | 1,758 | 1,151 | 0.88× | Two-stage | Win |
| Random Small (minified) | 960 | 849 | 554 | 1.13× | Fused | Win |
| Random Small (prettified) | 1,151 | 1,296 | 862 | 0.89× | Two-stage | Win |
| Twitter Small (minified) | 1,769 | 1,463 | 1,415 | 1.21× | Fused | Win |
| Twitter Small (prettified) | 1,598 | 1,846 | 1,821 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,986 | 631 | 283 | 3.15× | Fused | Win |
| Double (POD) | 1,280 | 750 | 337 | 1.71× | Fused | Win |
| Int64 (POD) | 3,081 | 1,638 | 775 | 1.88× | Fused | Win |
| String (POD) | 1,767 | 1,331 | 1,379 | 1.33× | Fused | Win |
| Uint64 (POD) | 3,200 | 1,579 | 721 | 2.03× | Fused | Win |
| Apache Builds (minified) | 1,693 | 1,457 | 1,208 | 1.16× | Fused | Win |
| Canada (minified) | 1,064 | 905 | 794 | 1.18× | Fused | Win |
| Apache Builds (prettified) | 1,743 | 1,842 | 1,558 | 0.95× | Two-stage | Win |
| Canada (prettified) | 2,000 | 2,122 | 1,795 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,197 | 1,664 | 1,125 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 2,869 | 3,356 | 2,382 | 0.85× | Two-stage | Win |
| Discord (minified) | 2,186 | 1,752 | 937 | 1.25× | Fused | Win |
| Discord (prettified) | 1,704 | 2,423 | 1,372 | 0.70× | Two-stage | Win |
| Github Events (minified) | 1,648 | 1,551 | 967 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 2,448 | 1,856 | 1,062 | 1.32× | Fused | Win |
| Github Events (prettified) | 1,532 | 1,923 | 1,173 | 0.80× | Two-stage | Win |
| Google Maps Response (prettified) | 2,910 | 3,447 | 2,164 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,450 | 1,822 | 761 | 1.34× | Fused | Win |
| Instruments (prettified) | 2,146 | 2,792 | 1,248 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 837 | 722 | 579 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,465 | 2,553 | 2,104 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 838 | 729 | 575 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,470 | 2,593 | 2,080 | 0.95× | Two-stage | Win |
| Random (minified) | 1,252 | 1,094 | 815 | 1.14× | Fused | Win |
| Random (prettified) | 1,388 | 1,690 | 1,243 | 0.82× | Two-stage | Win |
| Twitter (minified) | 1,893 | 1,765 | 955 | 1.07× | Fused | Win |
| Twitter (prettified) | 1,567 | 2,180 | 1,225 | 0.72× | Two-stage | Win |
| Apache Builds Small (minified) | 2,527 | 2,204 | 1,355 | 1.15× | Fused | Win |
| Canada Small (minified) | 984 | 900 | 744 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 2,151 | 2,541 | 1,669 | 0.85× | Two-stage | Win |
| Canada Small (prettified) | 1,843 | 2,102 | 1,739 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,611 | 1,347 | 973 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 1,794 | 2,301 | 1,716 | 0.78× | Two-stage | Win |
| Discord Small (minified) | 1,934 | 1,676 | 773 | 1.15× | Fused | Win |
| Discord Small (prettified) | 1,598 | 2,285 | 1,046 | 0.70× | Two-stage | Win |
| Github Events Small (minified) | 1,947 | 1,681 | 1,337 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,231 | 1,693 | 1,059 | 1.32× | Fused | Win |
| Github Events Small (prettified) | 1,791 | 2,186 | 1,632 | 0.82× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,664 | 3,205 | 2,152 | 0.83× | Two-stage | Win |
| Instruments Small (minified) | 2,491 | 1,797 | 653 | 1.39× | Fused | Win |
| Instruments Small (prettified) | 2,065 | 2,779 | 1,094 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 791 | 685 | 499 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,154 | 2,455 | 1,854 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 865 | 746 | 498 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 2,245 | 2,607 | 1,855 | 0.86× | Two-stage | Win |
| Random Small (minified) | 1,701 | 1,535 | 842 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,660 | 2,277 | 1,301 | 0.73× | Two-stage | Win |
| Twitter Small (minified) | 2,438 | 2,205 | 1,020 | 1.11× | Fused | Win |
| Twitter Small (prettified) | 1,829 | 2,656 | 1,309 | 0.69× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,334 | 623 | 228 | 3.75× | Fused | Win |
| Double (POD) | 1,288 | 717 | 307 | 1.80× | Fused | Win |
| Int64 (POD) | 3,408 | 1,636 | 717 | 2.08× | Fused | Win |
| String (POD) | 1,808 | 1,293 | 1,284 | 1.40× | Fused | Win |
| Uint64 (POD) | 3,466 | 1,618 | 726 | 2.14× | Fused | Win |
| Apache Builds (minified) | 1,666 | 1,430 | 1,357 | 1.17× | Fused | Win |
| Canada (minified) | 1,171 | 1,001 | 778 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 1,775 | 1,840 | 1,797 | 0.96× | Two-stage | Win |
| Canada (prettified) | 2,326 | 2,237 | 1,743 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,349 | 1,812 | 1,446 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,415 | 3,571 | 3,008 | 0.96× | Two-stage | Win |
| Discord (minified) | 2,274 | 1,845 | 1,344 | 1.23× | Fused | Win |
| Discord (prettified) | 2,175 | 2,552 | 1,925 | 0.85× | Two-stage | Win |
| Github Events (minified) | 1,672 | 1,538 | 1,425 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,463 | 1,807 | 1,110 | 1.36× | Fused | Win |
| Github Events (prettified) | 1,705 | 1,897 | 1,608 | 0.90× | Two-stage | Win |
| Google Maps Response (prettified) | 3,275 | 3,295 | 2,225 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,195 | 2,228 | 1,440 | 1.43× | Fused | Win |
| Instruments (prettified) | 2,837 | 3,324 | 2,324 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 855 | 735 | 523 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,542 | 2,558 | 1,953 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 852 | 737 | 531 | 1.16× | Fused | Win |
| Marine IK (prettified) | 2,679 | 2,710 | 1,975 | 0.99× | Two-stage | Win |
| Random (minified) | 1,201 | 1,089 | 974 | 1.10× | Fused | Win |
| Random (prettified) | 1,588 | 1,824 | 1,609 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,993 | 1,685 | 1,134 | 1.18× | Fused | Win |
| Twitter (prettified) | 1,947 | 2,143 | 1,499 | 0.91× | Two-stage | Win |
| Apache Builds Small (minified) | 2,435 | 2,078 | 1,632 | 1.17× | Fused | Win |
| Canada Small (minified) | 1,081 | 902 | 676 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 2,273 | 2,447 | 2,059 | 0.93× | Two-stage | Win |
| Canada Small (prettified) | 2,214 | 2,177 | 1,617 | 1.02× | Fused | Win |
| CitmCatalog Small (minified) | 1,715 | 1,408 | 1,155 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,150 | 2,385 | 1,976 | 0.90× | Two-stage | Win |
| Discord Small (minified) | 1,977 | 1,644 | 1,252 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,889 | 2,160 | 1,661 | 0.87× | Two-stage | Win |
| Github Events Small (minified) | 2,040 | 1,721 | 1,505 | 1.18× | Fused | Win |
| Google Maps Response Small (minified) | 2,257 | 1,682 | 1,087 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 2,012 | 2,188 | 1,911 | 0.92× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,003 | 3,162 | 2,202 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,234 | 2,173 | 1,326 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 2,791 | 3,288 | 2,135 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 853 | 752 | 492 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,295 | 2,537 | 1,786 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 926 | 767 | 492 | 1.21× | Fused | Win |
| Marine IK Small (prettified) | 2,473 | 2,668 | 1,794 | 0.93× | Two-stage | Win |
| Random Small (minified) | 1,553 | 1,498 | 1,050 | 1.04× | Fused | Win |
| Random Small (prettified) | 1,867 | 2,187 | 1,602 | 0.85× | Two-stage | Win |
| Twitter Small (minified) | 2,847 | 2,268 | 1,278 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,400 | 2,837 | 1,659 | 0.85× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,781 | 381 | 182 | 4.67× | Fused | Win |
| Double (POD) | 982 | 525 | 163 | 1.87× | Fused | Win |
| Int64 (POD) | 2,610 | 938 | 464 | 2.78× | Fused | Win |
| String (POD) | 1,090 | 679 | 635 | 1.60× | Fused | Win |
| Uint64 (POD) | 2,084 | 828 | 374 | 2.52× | Fused | Win |
| Apache Builds (minified) | 1,118 | 921 | 726 | 1.21× | Fused | Win |
| Canada (minified) | 834 | 678 | 555 | 1.23× | Fused | Win |
| Apache Builds (prettified) | 1,400 | 1,203 | 981 | 1.16× | Fused | Win |
| Canada (prettified) | 1,793 | 1,485 | 1,290 | 1.21× | Fused | Win |
| CitmCatalog (minified) | 2,097 | 1,279 | 1,171 | 1.64× | Fused | Win |
| CitmCatalog (prettified) | 2,525 | 2,318 | 2,075 | 1.09× | Fused | Win |
| Discord (minified) | 1,590 | 1,112 | 918 | 1.43× | Fused | Win |
| Discord (prettified) | 1,836 | 1,628 | 1,322 | 1.13× | Fused | Win |
| Github Events (minified) | 1,616 | 1,083 | 837 | 1.49× | Fused | Win |
| Google Maps Response (minified) | 1,910 | 1,273 | 621 | 1.50× | Fused | Win |
| Github Events (prettified) | 1,374 | 1,366 | 1,045 | 1.01× | Tie | Win |
| Google Maps Response (prettified) | 2,303 | 2,411 | 1,240 (n/c) | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,421 | 1,380 | 967 | 1.75× | Fused | Win |
| Instruments (prettified) | 2,437 | 2,335 | 1,693 | 1.04× | Fused | Win |
| Marine IK Reverse (minified) | 659 | 567 | 386 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 1,924 | 1,979 | 1,628 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 729 | 579 | 447 | 1.26× | Fused | Win |
| Marine IK (prettified) | 2,037 | 2,024 | 1,423 | 1.01× | Fused | Win |
| Random (minified) | 962 | 756 | 562 | 1.27× | Fused | Win |
| Random (prettified) | 1,294 | 1,107 | 890 | 1.17× | Fused | Win |
| Twitter (minified) | 1,875 | 1,521 | 936 | 1.23× | Fused | Win |
| Twitter (prettified) | 1,942 | 1,892 | 1,130 | 1.03× | Fused | Win |
| Apache Builds Small (minified) | 1,388 | 1,308 | 814 | 1.06× | Fused | Win |
| Canada Small (minified) | 778 | 620 | 422 | 1.25× | Fused | Win |
| Apache Builds Small (prettified) | 1,465 | 1,608 | 1,006 | 0.91× | Two-stage | Win |
| Canada Small (prettified) | 1,444 | 1,475 | 1,025 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,088 | 947 | 780 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 1,404 | 1,286 | 1,169 | 1.09× | Fused | Win |
| Discord Small (minified) | 1,269 | 1,154 | 818 | 1.10× | Fused | Win |
| Discord Small (prettified) | 1,477 | 1,391 | 1,253 | 1.06× | Fused | Win |
| Github Events Small (minified) | 1,527 | 1,274 | 1,176 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 1,789 | 1,271 | 755 | 1.41× | Fused | Win |
| Github Events Small (prettified) | 1,691 | 1,550 | 1,477 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,558 | 2,342 | 1,070 | 1.09× | Fused | Win |
| Instruments Small (minified) | 2,151 | 1,712 | 1,032 | 1.26× | Fused | Win |
| Instruments Small (prettified) | 2,525 | 1,840 | 1,381 | 1.37× | Fused | Win |
| Marine IK Reverse Small (minified) | 672 | 554 | 389 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,796 | 1,900 | 1,378 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 719 | 584 | 334 | 1.23× | Fused | Win |
| Marine IK Small (prettified) | 1,909 | 1,987 | 1,334 | 0.96× | Two-stage | Win |
| Random Small (minified) | 1,238 | 1,112 | 728 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,607 | 1,233 | 1,094 | 1.30× | Fused | Win |
| Twitter Small (minified) | 2,326 | 1,874 | 1,123 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 1,788 | 2,270 | 1,244 | 0.79× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,077 | 302 | 150 | 3.56× | Fused | Win |
| Double (POD) | 852 | 363 | 185 | 2.35× | Fused | Win |
| Int64 (POD) | 2,188 | 663 | 384 | 3.30× | Fused | Win |
| String (POD) | 1,054 | 547 | 663 | 1.93× | Fused | Win |
| Uint64 (POD) | 2,024 | 702 | 456 | 2.88× | Fused | Win |
| Apache Builds (minified) | 1,400 | 973 | 813 | 1.44× | Fused | Win |
| Canada (minified) | 736 | 616 | 485 | 1.20× | Fused | Win |
| Apache Builds (prettified) | 1,412 | 1,287 | 951 | 1.10× | Fused | Win |
| Canada (prettified) | 1,561 | 1,375 | 1,031 | 1.14× | Fused | Win |
| CitmCatalog (minified) | 2,008 | 1,338 | 695 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | 2,525 | 2,422 | 1,615 | 1.04× | Fused | Win |
| Discord (minified) | 2,454 | 1,102 | 643 | 2.23× | Fused | Win |
| Discord (prettified) | 2,180 | 1,578 | 969 | 1.38× | Fused | Win |
| Github Events (minified) | 1,558 | 1,289 | 745 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 2,132 | 1,123 | 761 | 1.90× | Fused | Win |
| Github Events (prettified) | 1,514 | 1,361 | 940 | 1.11× | Fused | Win |
| Google Maps Response (prettified) | 2,757 | 2,156 | 1,358 | 1.28× | Fused | Win |
| Instruments (minified) | 2,422 | 1,445 | 615 | 1.68× | Fused | Win |
| Instruments (prettified) | 2,826 | 2,529 | 1,011 | 1.12× | Fused | Win |
| Marine IK Reverse (minified) | 551 | 442 | 341 | 1.25× | Fused | Win |
| Marine IK Reverse (prettified) | 2,095 | 2,010 | 1,468 | 1.04× | Fused | Win |
| Marine IK (minified) | 711 | 612 | 376 | 1.16× | Fused | Win |
| Marine IK (prettified) | 1,992 | 1,846 | 1,304 | 1.08× | Fused | Win |
| Random (minified) | 1,178 | 902 | 600 | 1.31× | Fused | Win |
| Random (prettified) | 1,428 | 1,108 | 926 | 1.29× | Fused | Win |
| Twitter (minified) | 2,054 | 1,379 | 661 | 1.49× | Fused | Win |
| Twitter (prettified) | 1,824 | 1,755 | 793 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,401 | 1,257 | 849 | 1.11× | Fused | Win |
| Canada Small (minified) | 644 | 563 | 392 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 1,275 | 1,307 | 953 | 0.98× | Two-stage | Win |
| Canada Small (prettified) | 1,243 | 1,401 | 841 | 0.89× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,167 | 748 | 508 | 1.56× | Fused | Win |
| CitmCatalog Small (prettified) | 1,457 | 1,376 | 959 (n/c) | 1.06× | Fused | Win |
| Discord Small (minified) | 1,864 | 1,131 | 557 | 1.65× | Fused | Win |
| Discord Small (prettified) | 2,158 | 1,381 | 729 | 1.56× | Fused | Win |
| Github Events Small (minified) | 1,565 | 1,258 | 1,016 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,045 | 1,103 | 773 | 1.85× | Fused | Win |
| Github Events Small (prettified) | 1,736 | 1,420 | 1,263 | 1.22× | Fused | Win |
| Google Maps Response Small (prettified) | 2,072 | 1,574 | 1,491 | 1.32× | Fused | Win |
| Instruments Small (minified) | 2,529 | 1,455 | 522 | 1.74× | Fused | Win |
| Instruments Small (prettified) | 2,696 | 2,379 | 872 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 593 | 438 | 254 | 1.35× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,692 | 1,395 | 1,110 | 1.21× | Fused | Win |
| Marine IK Small (minified) | 620 | 517 | 262 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 1,993 | 1,867 | 1,025 | 1.07× | Fused | Win |
| Random Small (minified) | 1,263 | 975 | 541 | 1.30× | Fused | Win |
| Random Small (prettified) | 1,780 | 1,014 | 913 | 1.76× | Fused | Win |
| Twitter Small (minified) | 2,143 | 1,718 | 775 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,179 | 2,085 | 988 | 1.05× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 752 | 276 | 129 | 2.73× | Fused | Win |
| Double (POD) | 1,203 | 641 | 275 | 1.88× | Fused | Win |
| Int64 (POD) | 1,970 | 982 | 601 | 2.01× | Fused | Win |
| String (POD) | 1,736 | 1,231 | 1,103 | 1.41× | Fused | Win |
| Uint64 (POD) | 1,967 | 1,024 | 610 | 1.92× | Fused | Win |
| Apache Builds (minified) | 1,489 | 1,333 | 1,066 | 1.12× | Fused | Win |
| Canada (minified) | 896 | 833 | 487 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 1,659 | 1,692 | 1,379 | 0.98× | Two-stage | Win |
| Canada (prettified) | 1,475 (n/c) | 1,782 (n/c) | 1,094 | 0.83× | Two-stage | Win |
| CitmCatalog (minified) | 1,944 | 1,483 | 723 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 2,669 | 2,995 | 1,533 | 0.89× | Two-stage | Win |
| Discord (minified) | 2,114 | 1,537 | 985 | 1.38× | Fused | Win |
| Discord (prettified) | 1,936 | 2,202 | 1,439 | 0.88× | Two-stage | Win |
| Github Events (minified) | 1,673 | 1,411 | 959 | 1.19× | Fused | Win |
| Google Maps Response (minified) | 1,614 | 1,347 | 648 | 1.20× | Fused | Win |
| Github Events (prettified) | 1,609 | 1,680 | 1,032 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 2,451 | 2,576 | 1,328 | 0.95× | Two-stage | Win |
| Instruments (minified) | 2,880 | 1,780 | 980 | 1.62× | Fused | Win |
| Instruments (prettified) | 2,518 | 2,746 | 1,604 | 0.92× | Two-stage | Win |
| Marine IK Reverse (minified) | 490 | 620 | 206 | 0.79× | Two-stage | Win |
| Marine IK Reverse (prettified) | 2,107 | 1,889 | 618 (n/c) | 1.12× | Fused | Win |
| Marine IK (minified) | 733 | 636 | 373 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,173 | 2,250 | 1,363 | 0.97× | Two-stage | Win |
| Random (minified) | 1,170 | 1,010 | 728 | 1.16× | Fused | Win |
| Random (prettified) | 1,371 | 1,550 | 1,123 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,104 | 1,607 | 1,269 | 1.31× | Fused | Win |
| Twitter (prettified) | 1,813 | 2,039 | 1,648 | 0.89× | Two-stage | Win |
| Apache Builds Small (minified) | 1,635 | 1,693 | 1,251 | 0.97× | Two-stage | Win |
| Canada Small (minified) | 856 | 789 | 385 | 1.09× | Fused | Win |
| Apache Builds Small (prettified) | 1,690 | 2,101 | 1,590 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 1,699 | 1,762 | 936 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,207 | 949 | 717 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 1,461 | 1,669 | 1,240 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,916 | 1,311 | 1,018 | 1.46× | Fused | Win |
| Discord Small (prettified) | 1,714 | 1,776 | 1,441 | 0.97× | Two-stage | Win |
| Github Events Small (minified) | 1,514 | 1,454 | 919 | 1.04× | Fused | Win |
| Google Maps Response Small (minified) | 1,467 | 1,230 | 678 | 1.19× | Fused | Win |
| Github Events Small (prettified) | 1,427 | 1,811 | 1,166 | 0.79× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,187 | 2,445 | 1,328 | 0.89× | Two-stage | Win |
| Instruments Small (minified) | 2,633 | 1,651 | 966 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 2,278 | 2,541 | 1,558 | 0.90× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 403 | 535 | 169 | 0.75× | Two-stage | Win |
| Marine IK Reverse Small (prettified) | 1,595 | 1,915 | 640 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 585 | 589 | 357 | 0.99× | Two-stage | Win |
| Marine IK Small (prettified) | 1,868 | 2,121 | 1,305 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,211 | 1,031 | 698 | 1.17× | Fused | Win |
| Random Small (prettified) | 1,374 | 1,559 | 1,071 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 2,224 | 1,733 | 1,558 | 1.28× | Fused | Win |
| Twitter Small (prettified) | 1,881 | 2,163 | 1,999 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,168 | 648 | 286 | 3.35× | Fused | Win |
| Double (POD) | 1,391 | 801 | 366 | 1.74× | Fused | Win |
| Int64 (POD) | 3,443 | 1,770 | 793 | 1.94× | Fused | Win |
| String (POD) | 2,833 | 1,814 | 1,916 | 1.56× | Fused | Win |
| Uint64 (POD) | 3,545 | 1,635 | 723 | 2.17× | Fused | Win |
| Apache Builds (minified) | 2,786 | 2,297 | 1,660 | 1.21× | Fused | Win |
| Canada (minified) | 1,299 | 1,079 | 946 | 1.20× | Fused | Win |
| Apache Builds (prettified) | 2,461 | 2,839 | 2,124 | 0.87× | Two-stage | Win |
| Canada (prettified) | 2,330 | 2,525 | 2,124 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,663 | 1,957 | 1,288 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,198 | 3,901 | 2,695 | 0.82× | Two-stage | Win |
| Discord (minified) | 3,237 | 2,404 | 1,105 | 1.35× | Fused | Win |
| Discord (prettified) | 2,080 | 3,370 | 1,624 | 0.62× | Two-stage | Win |
| Github Events (minified) | 2,251 | 2,065 | 1,132 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,783 | 2,027 | 1,136 | 1.37× | Fused | Win |
| Github Events (prettified) | 1,946 | 2,516 | 1,366 | 0.77× | Two-stage | Win |
| Google Maps Response (prettified) | 3,099 | 3,725 | 2,296 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,862 | 2,035 | 790 | 1.41× | Fused | Win |
| Instruments (prettified) | 2,309 | 3,099 | 1,306 | 0.75× | Two-stage | Win |
| Marine IK Reverse (minified) | 934 | 791 | 633 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,649 | 2,793 | 2,308 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 948 | 812 | 632 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,696 | 2,852 | 2,276 | 0.95× | Two-stage | Win |
| Random (minified) | 1,754 | 1,482 | 1,010 | 1.18× | Fused | Win |
| Random (prettified) | 1,756 | 2,241 | 1,549 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,394 | 2,356 | 1,065 | 1.02× | Fused | Win |
| Twitter (prettified) | 1,809 | 2,727 | 1,378 | 0.66× | Two-stage | Win |
| Apache Builds Small (minified) | 2,921 | 2,524 | 1,583 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,233 | 1,097 | 857 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,427 | 3,051 | 1,916 | 0.80× | Two-stage | Win |
| Canada Small (prettified) | 2,198 | 2,508 | 2,030 | 0.88× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,995 | 1,609 | 1,091 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 1,998 | 2,679 | 1,893 | 0.75× | Two-stage | Win |
| Discord Small (minified) | 2,670 | 2,140 | 875 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,932 | 2,851 | 1,183 | 0.68× | Two-stage | Win |
| Github Events Small (minified) | 2,528 | 2,087 | 1,555 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,643 | 1,917 | 1,120 | 1.38× | Fused | Win |
| Github Events Small (prettified) | 2,127 | 2,726 | 1,943 | 0.78× | Two-stage | Win |
| Google Maps Response Small (prettified) | 2,910 | 3,576 | 2,243 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,685 | 1,965 | 684 | 1.37× | Fused | Win |
| Instruments Small (prettified) | 2,155 | 2,927 | 1,140 | 0.74× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 974 | 824 | 558 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,447 | 2,851 | 2,054 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,067 | 895 | 557 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 2,573 | 3,077 | 2,031 | 0.84× | Two-stage | Win |
| Random Small (minified) | 2,133 | 1,874 | 998 | 1.14× | Fused | Win |
| Random Small (prettified) | 1,926 | 2,800 | 1,488 | 0.69× | Two-stage | Win |
| Twitter Small (minified) | 2,738 | 2,367 | 1,073 | 1.16× | Fused | Win |
| Twitter Small (prettified) | 1,985 | 2,970 | 1,343 | 0.67× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,382 | 614 | 223 | 3.88× | Fused | Win |
| Double (POD) | 1,391 | 749 | 306 | 1.86× | Fused | Win |
| Int64 (POD) | 3,694 | 1,748 | 737 | 2.11× | Fused | Win |
| String (POD) | 2,789 | 1,714 | 1,675 | 1.63× | Fused | Win |
| Uint64 (POD) | 3,789 | 1,711 | 734 | 2.21× | Fused | Win |
| Apache Builds (minified) | 2,542 | 2,048 | 1,921 | 1.24× | Fused | Win |
| Canada (minified) | 1,451 | 1,213 | 932 | 1.20× | Fused | Win |
| Apache Builds (prettified) | 2,404 | 2,547 | 2,430 | 0.94× | Two-stage | Win |
| Canada (prettified) | 2,841 | 2,670 | 2,067 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,904 | 2,180 | 1,775 | 1.33× | Fused | Win |
| CitmCatalog (prettified) | 3,880 | 4,169 | 3,594 | 0.93× | Two-stage | Win |
| Discord (minified) | 3,157 | 2,449 | 1,621 | 1.29× | Fused | Win |
| Discord (prettified) | 2,688 | 3,303 | 2,315 | 0.81× | Two-stage | Win |
| Github Events (minified) | 2,238 | 2,017 | 1,630 | 1.11× | Fused | Win |
| Google Maps Response (minified) | 2,837 | 1,952 | 1,177 | 1.45× | Fused | Win |
| Github Events (prettified) | 2,261 | 2,372 | 1,920 | 0.95× | Two-stage | Win |
| Google Maps Response (prettified) | 3,482 | 3,564 | 2,352 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,849 | 2,507 | 1,531 | 1.53× | Fused | Win |
| Instruments (prettified) | 3,162 | 3,741 | 2,470 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 948 | 808 | 576 | 1.17× | Fused | Win |
| Marine IK Reverse (prettified) | 2,761 | 2,807 | 2,108 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 978 | 820 | 581 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,872 | 2,906 | 2,093 | 0.99× | Two-stage | Win |
| Random (minified) | 1,628 | 1,446 | 1,276 | 1.13× | Fused | Win |
| Random (prettified) | 2,022 | 2,419 | 1,996 | 0.84× | Two-stage | Win |
| Twitter (minified) | 2,727 | 2,123 | 1,312 | 1.28× | Fused | Win |
| Twitter (prettified) | 2,343 | 2,686 | 1,743 | 0.87× | Two-stage | Win |
| Apache Builds Small (minified) | 2,842 | 2,387 | 1,975 | 1.19× | Fused | Win |
| Canada Small (minified) | 1,377 | 1,108 | 750 | 1.24× | Fused | Win |
| Apache Builds Small (prettified) | 2,535 | 2,890 | 2,447 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,657 | 2,633 | 1,771 | 1.01× | Fused | Win |
| CitmCatalog Small (minified) | 2,031 | 1,654 | 1,429 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 2,383 | 2,758 | 2,430 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,562 | 2,025 | 1,541 | 1.27× | Fused | Win |
| Discord Small (prettified) | 2,262 | 2,644 | 2,003 | 0.86× | Two-stage | Win |
| Github Events Small (minified) | 2,598 | 2,084 | 1,772 | 1.25× | Fused | Win |
| Google Maps Response Small (minified) | 2,611 | 1,902 | 1,203 | 1.37× | Fused | Win |
| Github Events Small (prettified) | 2,426 | 2,653 | 2,264 | 0.91× | Two-stage | Win |
| Google Maps Response Small (prettified) | 3,304 | 3,491 | 2,386 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,611 | 2,315 | 1,418 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 2,955 | 3,514 | 2,284 | 0.84× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,055 | 869 | 534 | 1.21× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,684 | 3,020 | 1,969 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 1,150 | 937 | 526 | 1.23× | Fused | Win |
| Marine IK Small (prettified) | 2,846 | 3,120 | 1,954 | 0.91× | Two-stage | Win |
| Random Small (minified) | 1,905 | 1,783 | 1,255 | 1.07× | Fused | Win |
| Random Small (prettified) | 2,192 | 2,607 | 1,903 | 0.84× | Two-stage | Win |
| Twitter Small (minified) | 3,183 | 2,516 | 1,336 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,653 | 3,197 | 1,741 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,642 | 382 | 184 | 4.30× | Fused | Win |
| Double (POD) | 1,175 | 533 | 167 | 2.21× | Fused | Win |
| Int64 (POD) | 3,298 | 995 | 403 | 3.31× | Fused | Win |
| String (POD) | 2,071 | 1,009 | 916 | 2.05× | Fused | Win |
| Uint64 (POD) | 2,981 | 867 | 383 | 3.44× | Fused | Win |
| Apache Builds (minified) | 1,616 | 1,286 | 980 | 1.26× | Fused | Win |
| Canada (minified) | 939 | 873 | 686 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 1,714 | 1,857 | 1,245 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,185 | 1,958 | 1,480 | 1.12× | Fused | Win |
| CitmCatalog (minified) | 2,627 | 1,626 | 1,299 | 1.62× | Fused | Win |
| CitmCatalog (prettified) | 3,200 | 2,796 | 2,244 | 1.14× | Fused | Win |
| Discord (minified) | 2,326 | 1,496 | 1,134 | 1.55× | Fused | Win |
| Discord (prettified) | 2,297 | 2,025 | 1,546 | 1.13× | Fused | Win |
| Github Events (minified) | 1,996 | 1,486 | 1,065 | 1.34× | Fused | Win |
| Google Maps Response (minified) | 1,826 | 1,349 | 715 | 1.35× | Fused | Win |
| Github Events (prettified) | 2,037 | 1,744 | 1,295 | 1.17× | Fused | Win |
| Google Maps Response (prettified) | 2,832 | 2,178 | 1,414 | 1.30× | Fused | Win |
| Instruments (minified) | 2,670 | 1,542 | 1,075 | 1.73× | Fused | Win |
| Instruments (prettified) | 3,216 | 2,410 | 1,714 | 1.33× | Fused | Win |
| Marine IK Reverse (minified) | 813 | 572 | 463 | 1.42× | Fused | Win |
| Marine IK Reverse (prettified) | 2,324 | 2,192 | 1,762 | 1.06× | Fused | Win |
| Marine IK (minified) | 847 | 652 | 503 | 1.30× | Fused | Win |
| Marine IK (prettified) | 2,415 (n/c) | 2,237 | 1,716 | 1.08× | Fused | Win |
| Random (minified) | 1,265 | 1,112 | 789 | 1.14× | Fused | Win |
| Random (prettified) | 1,755 | 1,683 | 1,129 | 1.04× | Fused | Win |
| Twitter (minified) | 2,525 | 1,889 | 1,044 | 1.34× | Fused | Win |
| Twitter (prettified) | 2,614 | 2,307 | 1,376 | 1.13× | Fused | Win |
| Apache Builds Small (minified) | 1,985 | 1,745 | 941 | 1.14× | Fused | Win |
| Canada Small (minified) | 979 | 792 | 514 | 1.24× | Fused | Win |
| Apache Builds Small (prettified) | 1,815 | 2,099 | 1,318 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 1,872 | 1,754 | 1,197 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,696 | 1,217 | 969 | 1.39× | Fused | Win |
| CitmCatalog Small (prettified) | 1,851 | 1,652 | 1,409 | 1.12× | Fused | Win |
| Discord Small (minified) | 2,200 | 1,571 | 984 | 1.40× | Fused | Win |
| Discord Small (prettified) | 2,359 | 1,817 | 1,308 | 1.30× | Fused | Win |
| Github Events Small (minified) | 2,227 | 1,746 | 1,490 | 1.28× | Fused | Win |
| Google Maps Response Small (minified) | 2,231 | 1,504 | 847 | 1.48× | Fused | Win |
| Github Events Small (prettified) | 2,320 | 2,077 | 1,468 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,947 | 2,659 | 1,334 | 1.11× | Fused | Win |
| Instruments Small (minified) | 2,700 | 1,649 | 1,105 | 1.64× | Fused | Win |
| Instruments Small (prettified) | 2,511 | 2,684 | 1,544 | 0.94× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 866 | 683 | 449 | 1.27× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,134 | 2,278 | 1,548 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 951 | 730 | 430 | 1.30× | Fused | Win |
| Marine IK Small (prettified) | 2,367 | 2,418 | 1,506 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,407 | 1,434 | 691 | 0.98× | Two-stage | Win |
| Random Small (prettified) | 1,991 | 1,895 | 1,352 | 1.05× | Fused | Win |
| Twitter Small (minified) | 2,886 | 2,286 | 1,004 | 1.26× | Fused | Win |
| Twitter Small (prettified) | 2,734 | 2,665 | 1,438 | 1.03× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,112 | 256 | 144 | 4.34× | Fused | Win |
| Double (POD) | 1,031 | 406 | 198 | 2.54× | Fused | Win |
| Int64 (POD) | 2,248 | 676 | 362 | 3.32× | Fused | Win |
| String (POD) | 1,983 | 738 | 964 | 2.69× | Fused | Win |
| Uint64 (POD) | 2,819 | 755 | 406 | 3.73× | Fused | Win |
| Apache Builds (minified) | 1,934 | 1,279 | 976 | 1.51× | Fused | Win |
| Canada (minified) | 918 | 797 | 524 | 1.15× | Fused | Win |
| Apache Builds (prettified) | 2,172 | 1,637 | 1,127 | 1.33× | Fused | Win |
| Canada (prettified) | 1,868 | 1,583 | 1,202 | 1.18× | Fused | Win |
| CitmCatalog (minified) | 2,437 | 1,454 | 852 | 1.68× | Fused | Win |
| CitmCatalog (prettified) | 3,065 (n/c) | 2,702 | 1,614 | 1.13× | Fused | Win |
| Discord (minified) | 2,698 | 1,592 | 796 | 1.69× | Fused | Win |
| Discord (prettified) | 2,395 | 2,114 | 1,057 | 1.13× | Fused | Win |
| Github Events (minified) | 1,967 | 1,633 | 926 | 1.20× | Fused | Win |
| Google Maps Response (minified) | 2,245 | 1,307 | 691 | 1.72× | Fused | Win |
| Github Events (prettified) | 1,657 | 2,244 | 961 | 0.74× | Two-stage | Win |
| Google Maps Response (prettified) | 2,903 | 2,075 | 1,587 | 1.40× | Fused | Win |
| Instruments (minified) | 3,102 | 1,704 | 612 | 1.82× | Fused | Win |
| Instruments (prettified) | 2,792 | 2,612 | 997 | 1.07× | Fused | Win |
| Marine IK Reverse (minified) | 632 | 499 | 350 (n/c) | 1.27× | Fused | Win |
| Marine IK Reverse (prettified) | 2,388 | 2,224 | 1,573 | 1.07× | Fused | Win |
| Marine IK (minified) | 811 | 631 | 413 | 1.29× | Fused | Win |
| Marine IK (prettified) | 2,185 | 2,107 | 1,406 | 1.04× | Fused | Win |
| Random (minified) | 1,591 | 1,223 | 700 | 1.30× | Fused | Win |
| Random (prettified) | 1,513 | 1,660 | 1,062 | 0.91× | Two-stage | Win |
| Twitter (minified) | 2,386 | 1,699 | 702 | 1.40× | Fused | Win |
| Twitter (prettified) | 2,135 | 2,140 | 907 | 1.00× | Tie | Win |
| Apache Builds Small (minified) | 1,730 | 1,611 | 1,132 | 1.07× | Fused | Win |
| Canada Small (minified) | 799 | 704 | 422 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,642 | 1,676 | 1,210 | 0.98× | Two-stage | Win |
| Canada Small (prettified) | 1,597 | 1,592 | 895 | 1.00× | Tie | Win |
| CitmCatalog Small (minified) | 1,846 | 1,367 | 728 | 1.35× | Fused | Win |
| CitmCatalog Small (prettified) | 2,119 | 2,057 | 1,386 | 1.03× | Fused | Win |
| Discord Small (minified) | 2,102 | 1,305 | 643 | 1.61× | Fused | Win |
| Discord Small (prettified) | 1,957 | 1,801 | 920 | 1.09× | Fused | Win |
| Github Events Small (minified) | 2,383 | 1,598 | 893 | 1.49× | Fused | Win |
| Google Maps Response Small (minified) | 2,211 | 1,291 | 821 | 1.71× | Fused | Win |
| Github Events Small (prettified) | 2,259 | 1,968 | 1,483 | 1.15× | Fused | Win |
| Google Maps Response Small (prettified) | 2,733 | 2,413 | 1,516 | 1.13× | Fused | Win |
| Instruments Small (minified) | 2,328 | 1,633 | 609 | 1.43× | Fused | Win |
| Instruments Small (prettified) | 2,666 | 2,590 | 1,003 | 1.03× | Fused | Win |
| Marine IK Reverse Small (minified) | 770 | 650 | 289 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,869 | 1,915 | 1,173 | 0.98× | Two-stage | Win |
| Marine IK Small (minified) | 743 | 618 | 276 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,211 | 2,004 | 1,045 | 1.10× | Fused | Win |
| Random Small (minified) | 1,429 | 1,094 | 691 | 1.31× | Fused | Win |
| Random Small (prettified) | 1,929 | 1,445 | 1,096 | 1.33× | Fused | Win |
| Twitter Small (minified) | 2,636 | 2,050 | 835 | 1.29× | Fused | Win |
| Twitter Small (prettified) | 2,514 | 2,353 | 1,064 | 1.07× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented and whether the machine is x86. On the three x86 builds the fused path wins all 15 POD-type tests and 61 of the 66 minified documents, full-size and Small, the other five going to the tape under MSVC, while the tape wins 31 of the 33 full-size prettified documents and 32 of the 33 Small prettified ones. The three prettified exceptions are Twitter under MSVC, a tie, and Canada and Canada Small on Linux/GCC, which the fused path wins. On the M2 Pro the fused path wins all POD input, all 44 minified documents and 34 of the 44 prettified documents (9 to the tape, 1 tie).

**POD-type tests: the fused path wins all 25, freshly allocated and reused.** It runs them 1.26× (String, Windows/MSVC) to 4.67× (Bool, macOS/GCC) faster than the two-stage path, and 1.41× (String, Windows/MSVC) to 4.34× (Bool, macOS/Clang) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified documents: the fused path's, except for five under MSVC.** Freshly allocated, the fused path wins 105 of the 110 minified documents and loses 5 to the tape, all under MSVC: Marine IK Reverse (453 against 569 MB/s, 0.80×) and the Small copies of Apache Builds (1,271 against 1,349, 0.94×), Github Events (1,019 against 1,043, 0.98×), Marine IK Reverse (337 against 446, 0.76×) and Marine IK (455 against 477, 0.95×). Its margins run from 6% (Canada under MSVC, 730 against 691 MB/s) to 2.23× (Discord on macOS/Clang, 2,454 against 1,102 MB/s) on the full-size documents and from 4% (Random Small on Linux/GCC, 1,553 against 1,498) to 1.85× (Google Maps Response Small on macOS/Clang, 2,045 against 1,103 MB/s) on the Small ones. With objects reused it also wins 105 and loses 5: the same documents under MSVC except Github Events Small, which the fused path wins, plus Random Small on macOS/GCC (1,407 against 1,434 MB/s, 0.98×). Why the tape wins these under MSVC has not been profiled. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 63 of 66.** It wins all 22 on Linux/Clang, 20 of 22 on Linux/GCC, where the fused path wins prettified Canada and Canada Small (2,326 MB/s against 2,237, and 2,214 against 2,177), and 21 of 22 under MSVC, where prettified Twitter is a tie in which neither path converged (756 against 1,101 MB/s). The tape's margins run from under 1% (Marine IK Reverse on Linux/GCC, 2,558 against 2,542 MB/s) to 42% (Discord on Linux/Clang, 2,423 against 1,704) on the full-size documents and from 5% (Google Maps Response Small on Linux/GCC) to 45% (Twitter Small on Linux/Clang) on the Small ones. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape again wins 63 of the 66 prettified x86 documents; the fused path takes prettified Marine IK Reverse under MSVC and prettified Canada and Canada Small on Linux/GCC.

**Prettified documents on the M2 Pro: mostly the fused path's.** Of the 44 prettified documents, the fused path wins 34, the tape wins 9 and 1 is a tie. On the 22 full-size documents the fused path wins 19, the tape 2 and 1 is a tie: the tape takes Google Maps Response and Marine IK Reverse on macOS/GCC (0.96× and 0.97×), and Github Events on macOS/GCC is a tie. On the Small copies the tape wins Apache Builds Small, Canada Small, Marine IK Reverse Small, Marine IK Small and Twitter Small on macOS/GCC (0.91×, 0.98×, 0.94×, 0.96× and 0.79×), and Apache Builds Small and Canada Small on macOS/Clang (0.98× and 0.89×). By compiler, the fused path wins 14 of 22 on macOS/GCC (7 to the tape, 1 tie) and 20 of 22 on macOS/Clang (2 to the tape). Reused, the fused path wins 33, the tape 9, and 2 are ties: 17, 5 and 0 on macOS/GCC, and 16, 4 and 2 on macOS/Clang. NEON escapes much of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**No document beats both Jsonifier paths.** The faster Jsonifier path wins every test against simdjson On Demand in both runs except freshly allocated prettified Twitter under MSVC, a tie in which none of the three results converged. The fused path alone loses three freshly allocated: prettified Twitter Small under MSVC (1,598 against 1,821 MB/s, 0.88×), and prettified Apache Builds and Random on Linux/GCC (1,775 against 1,797, 0.99×, and 1,588 against 1,609, 0.99×). The tape wins all three, at 1,846, 1,840 and 1,824 MB/s. With objects reused the fused path loses prettified Twitter Small under MSVC (1,881 against 1,999, 0.94×), and prettified Apache Builds and CitmCatalog Small on Linux/GCC (2,404 against 2,430, 0.99×, and 2,383 against 2,430, 0.98×), and the tape wins those at 2,163, 2,547 and 2,758 MB/s. We have not profiled what sets these documents apart.

**MSVC slows all three, Jsonifier's two paths more than simdjson.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 58% of its Linux/Clang throughput, the two-stage path 59% and simdjson 66%. The spread between documents is narrowest for the two-stage path (from 52% on Twitter to 76% on CitmCatalog), wider for the fused path (from 53% on Google Maps Response to 87% on Instruments) and widest for simdjson, which keeps 48% of its Linux/Clang throughput on Google Maps Response and slightly more than all of it (103%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins 244 of 245 tests with whichever path is faster, and ties the other.** The tie is prettified Twitter under MSVC. The fused path alone wins 241, ties 1 and loses 3: prettified Twitter Small under MSVC and prettified Apache Builds and Random on Linux/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 197 MB/s, against 340 MB/s for the same document in declared order, so the faster Jsonifier path is 2.88× faster than simdjson freshly allocated (3.00× reused). The prettified form shows the same gap, at 672 MB/s against 1,280, and the faster Jsonifier path is 3.00× faster (3.41× reused). The Small copies show it too, at 161 MB/s minified and 616 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document within about 15% of the declared one, sometimes faster, and the faster Jsonifier path is 1.21× to 1.71× faster than simdjson on the full-size reversed document, freshly allocated, and 1.21× to 1.80× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path, and under MSVC so do five minified documents; on the M2 Pro, minified input belongs to the fused path, and so do about three quarters of the prettified documents. Routing each test to its faster path would raise the record against simdjson from 241 wins, 1 tie and 3 losses to 244 wins and 1 tie. The router does not yet take the architecture, the compiler or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,430 | 1,396 | 1.02× | Win |
| Canada (minified) | 1,001 | 824 | 1.21× | Win |
| Apache Builds (prettified) | 1,840 | 1,825 | 1.01× | Win |
| Canada (prettified) | 2,237 | 1,838 | 1.22× | Win |
| CitmCatalog (minified) | 1,812 | 1,425 | 1.27× | Win |
| CitmCatalog (prettified) | 3,571 | 2,978 | 1.20× | Win |
| Discord (minified) | 1,845 | 1,564 | 1.18× | Win |
| Discord (prettified) | 2,552 | 2,230 | 1.14× | Win |
| Github Events (minified) | 1,538 | 1,543 | 1.00× | **Loss** |
| Google Maps Response (minified) | 1,807 | 1,502 | 1.20× | Win |
| Github Events (prettified) | 1,897 | 1,886 | 1.01× | Win |
| Google Maps Response (prettified) | 3,295 | 2,950 | 1.12× | Win |
| Instruments (minified) | 2,228 | 1,682 | 1.32× | Win |
| Instruments (prettified) | 3,324 | 2,711 | 1.23× | Win |
| Marine IK Reverse (minified) | 735 | 621 | 1.18× | Win |
| Marine IK Reverse (prettified) | 2,558 | 2,252 | 1.14× | Win |
| Marine IK (minified) | 737 | 622 | 1.18× | Win |
| Marine IK (prettified) | 2,710 | 2,296 | 1.18× | Win |
| Random (minified) | 1,089 | 1,165 | 0.93× | **Loss** |
| Random (prettified) | 1,824 | 1,911 | 0.95× | **Loss** |
| Twitter (minified) | 1,685 | 1,586 | 1.06× | Win |
| Twitter (prettified) | 2,143 | 2,067 | 1.04× | Win |
| Apache Builds Small (minified) | 2,078 | 1,770 | 1.17× | Win |
| Canada Small (minified) | 902 | 755 | 1.20× | Win |
| Apache Builds Small (prettified) | 2,447 | 2,211 | 1.11× | Win |
| Canada Small (prettified) | 2,177 | 1,774 | 1.23× | Win |
| CitmCatalog Small (minified) | 1,408 | 1,199 | 1.17× | Win |
| CitmCatalog Small (prettified) | 2,385 | 2,051 | 1.16× | Win |
| Discord Small (minified) | 1,644 | 1,499 | 1.10× | Win |
| Discord Small (prettified) | 2,160 | 1,933 | 1.12× | Win |
| Github Events Small (minified) | 1,721 | 1,766 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,682 | 1,444 | 1.17× | Win |
| Github Events Small (prettified) | 2,188 | 2,215 | 0.99× | **Loss** |
| Google Maps Response Small (prettified) | 3,162 | 2,839 | 1.11× | Win |
| Instruments Small (minified) | 2,173 | 1,605 | 1.35× | Win |
| Instruments Small (prettified) | 3,288 | 2,572 | 1.28× | Win |
| Marine IK Reverse Small (minified) | 752 | 615 | 1.22× | Win |
| Marine IK Reverse Small (prettified) | 2,537 | 2,281 | 1.11× | Win |
| Marine IK Small (minified) | 767 | 630 | 1.22× | Win |
| Marine IK Small (prettified) | 2,668 | 2,297 | 1.16× | Win |
| Random Small (minified) | 1,498 | 1,261 | 1.19× | Win |
| Random Small (prettified) | 2,187 | 1,916 | 1.14× | Win |
| Twitter Small (minified) | 2,268 | 1,990 | 1.14× | Win |
| Twitter Small (prettified) | 2,837 | 2,577 | 1.10× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 921 | 934 | 0.99× | Tie |
| Canada (minified) | 678 | 643 | 1.05× | Win |
| Apache Builds (prettified) | 1,203 | 1,189 | 1.01× | Win |
| Canada (prettified) | 1,485 | 1,415 | 1.05× | Win |
| CitmCatalog (minified) | 1,279 | 1,152 | 1.11× | Win |
| CitmCatalog (prettified) | 2,318 | 1,902 | 1.22× | Win |
| Discord (minified) | 1,112 | 996 | 1.12× | Win |
| Discord (prettified) | 1,628 | 1,449 | 1.12× | Win |
| Github Events (minified) | 1,083 | 1,010 | 1.07× | Win |
| Google Maps Response (minified) | 1,273 | 907 | 1.40× | Win |
| Github Events (prettified) | 1,366 | 1,453 | 0.94× | **Loss** |
| Google Maps Response (prettified) | 2,411 | 1,770 | 1.36× | Win |
| Instruments (minified) | 1,380 | 1,325 | 1.04× | Win |
| Instruments (prettified) | 2,335 | 1,827 | 1.28× | Win |
| Marine IK Reverse (minified) | 567 | 459 | 1.23× | Win |
| Marine IK Reverse (prettified) | 1,979 | 1,797 (n/c) | 1.10× | Win |
| Marine IK (minified) | 579 | 505 | 1.15× | Win |
| Marine IK (prettified) | 2,024 | 1,783 | 1.13× | Win |
| Random (minified) | 756 | 722 | 1.05× | Win |
| Random (prettified) | 1,107 | 1,134 | 0.98× | **Loss** |
| Twitter (minified) | 1,521 | 1,173 | 1.30× | Win |
| Twitter (prettified) | 1,892 | 1,561 | 1.21× | Win |
| Apache Builds Small (minified) | 1,308 | 1,053 | 1.24× | Win |
| Canada Small (minified) | 620 | 570 | 1.09× | Win |
| Apache Builds Small (prettified) | 1,608 | 1,214 | 1.32× | Win |
| Canada Small (prettified) | 1,475 | 1,300 | 1.13× | Win |
| CitmCatalog Small (minified) | 947 | 815 | 1.16× | Win |
| CitmCatalog Small (prettified) | 1,286 | 1,380 | 0.93× | **Loss** |
| Discord Small (minified) | 1,154 | 951 | 1.21× | Win |
| Discord Small (prettified) | 1,391 | 1,302 | 1.07× | Win |
| Github Events Small (minified) | 1,274 | 1,133 | 1.12× | Win |
| Google Maps Response Small (minified) | 1,271 | 1,020 | 1.25× | Win |
| Github Events Small (prettified) | 1,550 | 1,407 | 1.10× | Win |
| Google Maps Response Small (prettified) | 2,342 | 1,849 | 1.27× | Win |
| Instruments Small (minified) | 1,712 | 1,002 | 1.71× | Win |
| Instruments Small (prettified) | 1,840 | 1,850 | 0.99× | Tie |
| Marine IK Reverse Small (minified) | 554 | 522 | 1.06× | Win |
| Marine IK Reverse Small (prettified) | 1,900 | 1,823 | 1.04× | Win |
| Marine IK Small (minified) | 584 | 540 | 1.08× | Win |
| Marine IK Small (prettified) | 1,987 | 1,843 | 1.08× | Win |
| Random Small (minified) | 1,112 | 862 | 1.29× | Win |
| Random Small (prettified) | 1,233 | 1,290 | 0.96× | **Loss** |
| Twitter Small (minified) | 1,874 | 1,378 | 1.36× | Win |
| Twitter Small (prettified) | 2,270 | 1,484 | 1.53× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,048 | 1,806 | 1.13× | Win |
| Canada (minified) | 1,213 | 964 | 1.26× | Win |
| Apache Builds (prettified) | 2,547 | 2,295 | 1.11× | Win |
| Canada (prettified) | 2,670 | 2,124 | 1.26× | Win |
| CitmCatalog (minified) | 2,180 | 1,700 | 1.28× | Win |
| CitmCatalog (prettified) | 4,169 | 3,467 | 1.20× | Win |
| Discord (minified) | 2,449 | 1,825 | 1.34× | Win |
| Discord (prettified) | 3,303 | 2,589 | 1.28× | Win |
| Github Events (minified) | 2,017 | 1,890 | 1.07× | Win |
| Google Maps Response (minified) | 1,952 | 1,604 | 1.22× | Win |
| Github Events (prettified) | 2,372 | 2,227 | 1.07× | Win |
| Google Maps Response (prettified) | 3,564 | 3,092 | 1.15× | Win |
| Instruments (minified) | 2,507 | 1,726 | 1.45× | Win |
| Instruments (prettified) | 3,741 | 2,769 | 1.35× | Win |
| Marine IK Reverse (minified) | 808 | 685 | 1.18× | Win |
| Marine IK Reverse (prettified) | 2,807 | 2,438 | 1.15× | Win |
| Marine IK (minified) | 820 | 691 | 1.19× | Win |
| Marine IK (prettified) | 2,906 | 2,490 | 1.17× | Win |
| Random (minified) | 1,446 | 1,542 | 0.94× | **Loss** |
| Random (prettified) | 2,419 | 2,340 | 1.03× | Win |
| Twitter (minified) | 2,123 | 1,865 | 1.14× | Win |
| Twitter (prettified) | 2,686 | 2,452 | 1.10× | Win |
| Apache Builds Small (minified) | 2,387 | 1,916 | 1.25× | Win |
| Canada Small (minified) | 1,108 | 871 | 1.27× | Win |
| Apache Builds Small (prettified) | 2,890 | 2,414 | 1.20× | Win |
| Canada Small (prettified) | 2,633 | 2,076 | 1.27× | Win |
| CitmCatalog Small (minified) | 1,654 | 1,283 | 1.29× | Win |
| CitmCatalog Small (prettified) | 2,758 | 2,230 | 1.24× | Win |
| Discord Small (minified) | 2,025 | 1,790 | 1.13× | Win |
| Discord Small (prettified) | 2,644 | 2,402 | 1.10× | Win |
| Github Events Small (minified) | 2,084 | 1,983 | 1.05× | Win |
| Google Maps Response Small (minified) | 1,902 | 1,528 | 1.24× | Win |
| Github Events Small (prettified) | 2,653 | 2,449 | 1.08× | Win |
| Google Maps Response Small (prettified) | 3,491 | 3,051 | 1.14× | Win |
| Instruments Small (minified) | 2,315 | 1,658 | 1.40× | Win |
| Instruments Small (prettified) | 3,514 | 2,654 | 1.32× | Win |
| Marine IK Reverse Small (minified) | 869 | 707 | 1.23× | Win |
| Marine IK Reverse Small (prettified) | 3,020 | 2,538 | 1.19× | Win |
| Marine IK Small (minified) | 937 | 740 | 1.27× | Win |
| Marine IK Small (prettified) | 3,120 | 2,580 | 1.21× | Win |
| Random Small (minified) | 1,783 | 1,427 | 1.25× | Win |
| Random Small (prettified) | 2,607 | 2,167 | 1.20× | Win |
| Twitter Small (minified) | 2,516 | 2,137 | 1.18× | Win |
| Twitter Small (prettified) | 3,197 | 2,689 | 1.19× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,286 | 1,169 | 1.10× | Win |
| Canada (minified) | 873 | 773 | 1.13× | Win |
| Apache Builds (prettified) | 1,857 | 1,475 | 1.26× | Win |
| Canada (prettified) | 1,958 | 1,642 | 1.19× | Win |
| CitmCatalog (minified) | 1,626 | 1,277 | 1.27× | Win |
| CitmCatalog (prettified) | 2,796 | 1,977 | 1.41× | Win |
| Discord (minified) | 1,496 | 1,095 | 1.37× | Win |
| Discord (prettified) | 2,025 | 1,623 | 1.25× | Win |
| Github Events (minified) | 1,486 | 1,187 | 1.25× | Win |
| Google Maps Response (minified) | 1,349 | 972 | 1.39× | Win |
| Github Events (prettified) | 1,744 | 1,569 | 1.11× | Win |
| Google Maps Response (prettified) | 2,178 | 1,872 | 1.16× | Win |
| Instruments (minified) | 1,542 | 1,372 | 1.12× | Win |
| Instruments (prettified) | 2,410 | 1,830 | 1.32× | Win |
| Marine IK Reverse (minified) | 572 | 481 | 1.19× | Win |
| Marine IK Reverse (prettified) | 2,192 | 1,981 | 1.11× | Win |
| Marine IK (minified) | 652 | 571 | 1.14× | Win |
| Marine IK (prettified) | 2,237 | 1,946 | 1.15× | Win |
| Random (minified) | 1,112 | 904 | 1.23× | Win |
| Random (prettified) | 1,683 | 1,309 | 1.29× | Win |
| Twitter (minified) | 1,889 | 1,339 | 1.41× | Win |
| Twitter (prettified) | 2,307 | 1,704 | 1.35× | Win |
| Apache Builds Small (minified) | 1,745 | 1,250 | 1.40× | Win |
| Canada Small (minified) | 792 | 661 | 1.20× | Win |
| Apache Builds Small (prettified) | 2,099 | 1,421 | 1.48× | Win |
| Canada Small (prettified) | 1,754 | 1,454 | 1.21× | Win |
| CitmCatalog Small (minified) | 1,217 | 994 | 1.22× | Win |
| CitmCatalog Small (prettified) | 1,652 | 1,416 | 1.17× | Win |
| Discord Small (minified) | 1,571 | 1,206 | 1.30× | Win |
| Discord Small (prettified) | 1,817 | 1,546 | 1.18× | Win |
| Github Events Small (minified) | 1,746 | 1,357 | 1.29× | Win |
| Google Maps Response Small (minified) | 1,504 | 1,147 | 1.31× | Win |
| Github Events Small (prettified) | 2,077 | 1,661 | 1.25× | Win |
| Google Maps Response Small (prettified) | 2,659 | 1,840 | 1.45× | Win |
| Instruments Small (minified) | 1,649 | 1,071 | 1.54× | Win |
| Instruments Small (prettified) | 2,684 | 1,910 | 1.41× | Win |
| Marine IK Reverse Small (minified) | 683 | 644 | 1.06× | Win |
| Marine IK Reverse Small (prettified) | 2,278 | 2,150 | 1.06× | Win |
| Marine IK Small (minified) | 730 | 655 | 1.11× | Win |
| Marine IK Small (prettified) | 2,418 | 2,158 | 1.12× | Win |
| Random Small (minified) | 1,434 | 1,042 | 1.38× | Win |
| Random Small (prettified) | 1,895 | 1,522 | 1.25× | Win |
| Twitter Small (minified) | 2,286 | 1,546 | 1.48× | Win |
| Twitter Small (prettified) | 2,665 | 1,816 | 1.47× | Win |

In the freshly allocated run, two-stage Jsonifier wins 77 of these 88 tests, ties 2 and loses 9, by up to 7%: on Linux/GCC, minified Github Events, minified and prettified Random, and minified and prettified Github Events Small (1.00×, 0.93×, 0.95×, 0.97× and 0.99×); on macOS/GCC, prettified Github Events, Random, CitmCatalog Small and Random Small (0.94×, 0.98×, 0.93× and 0.96×). The ties are minified Apache Builds and prettified Instruments Small on macOS/GCC (both 0.99×). With the fused path instead, Jsonifier beats the reflection reader on 76, ties 1 and loses 11. With objects reused, two-stage Jsonifier wins 87 and loses 1: minified Random on Linux/GCC (1,446 against 1,542 MB/s). The fused path beats the reflection reader on 82 and loses 6. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a 1.00× row can go either way: freshly allocated minified Github Events on Linux/GCC (1,538 against 1,543 MB/s) is a loss.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 37 of the 40 results; the widest gap is reused prettified Twitter on macOS/GCC, where the "jsonifier" row runs at 5,202 MB/s against the two-stage row's 4,746. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 6,960 | 4,240 | 1.64× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,677 | 5,114 | 1.50× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,835 | 4,094 | 1.18× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,811 | 4,582 | 1.27× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,080 | 6,776 | 1.19× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,254 | 8,142 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,537 | 5,529 | 1.00× | Tie |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,763 | 6,691 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,940 | 6,750 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,831 | 8,124 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,548 | 5,314 | 1.04× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,455 | 6,758 | 0.96× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,796 | 3,700 | 1.30× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,979 | 3,936 | 1.27× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,962 | 3,667 | 1.08× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,250 | 4,345 | 0.98× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,484 | 3,703 | 1.21× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,892 | 4,438 | 1.10× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,471 | 3,306 | 1.05× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,926 | 3,452 | 1.14× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 7,117 | 4,378 | 1.63× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 7,868 | 5,365 | 1.47× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,384 | 4,325 | 1.25× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,282 | 5,248 | 1.20× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,309 | 6,893 | 1.21× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,563 | 8,278 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,694 | 5,627 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,771 | 6,797 | 1.00× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,085 | 6,845 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,965 | 8,211 | 1.09× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,840 | 5,702 | 1.02× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,587 | 6,831 | 0.96× | Loss |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,886 | 3,653 | 1.34× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,746 | 4,282 | 1.11× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,257 | 3,916 | 1.09× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,641 | 4,517 | 1.03× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,188 | 3,801 | 1.10× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,023 | 4,494 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,807 | 3,503 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,587 | 4,210 | 1.09× | Win |

Jsonifier wins 35 of the 40 results, ties 1 and loses 4, and wins by up to 1.64× (minified Twitter under MSVC, freshly allocated). Freshly allocated it wins 17, ties 1 and loses 2: the tie is minified Twitter Small on Linux/Clang (1.00×), and the losses are prettified Twitter Small on Linux/GCC (0.96×) and on macOS/GCC (0.98×). The reused run has 18 wins and 2 losses: prettified Twitter Small on Linux/Clang (1.00×) and on Linux/GCC (0.96×). Every full-size result is a win, by 1.09× to 1.64× on the three x86 builds and 1.10× to 1.34× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.09× to 1.36×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 2,856 | 2,610 | 1.09× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,575 | 2,636 | 1.36× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,979 | 2,186 | 1.36× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,296 | 2,051 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,257 | 1,773 | 1.27× | Win |

The narrowest margin is under MSVC, 1.09× (2,856 against 2,610 MB/s).

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on all POD data, on 105 of 110 minified documents, and on the M2 Pro on 19 of 22 full-size and 15 of 22 Small prettified documents; slower on 63 of 66 prettified x86 documents (see §2.2) |
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

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 166 of 245 freshly allocated tests: all 25 POD-type tests, 105 of 110 minified documents and 34 of 44 prettified documents on the M2 Pro, while the two-stage path wins 63 of 66 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 244 of 245 tests and ties the other, its stage-1 + reflection path beats simdjson's reflection reader on 77 of 88, and on partial reading it wins 35 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*