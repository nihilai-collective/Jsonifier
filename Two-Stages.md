# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, November 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across the 220 corpus tests of five builds, skipping the tape wins all 110 minified documents and 91 of the 110 prettified ones, with 5 ties; the tape's 14 wins come almost entirely from the float-heavy Marine IK documents on x86. Three AVX-512 builds show the same shape: the fused path wins 64 of their 66 minified documents and 45 of their 66 prettified ones.

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

For full-document parsing into known types, the tape is overhead in principle, and in practice skipping it is faster on nearly every document measured, indented or not, with the float-heavy exceptions §2.4 names. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying: workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One objection is that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads (schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents), this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns, parsing documents into concrete types the caller has defined, the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is legibility: as hand-written traversal the knowledge is opaque to the library, while as a reflection registration it becomes architecture (fused member headers, adaptive order recovery, and the routing rule above). It is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep [5] published November 23, 2026, with Jsonifier [4f52e70](https://github.com/nihilai-collective/jsonifier/commit/4f52e70), Glaze [fc0b48a](https://github.com/stephenberry/glaze/commit/fc0b48a0), simdjson [3448984](https://github.com/simdjson/simdjson/commit/344898409) and BenchmarkSuite [71afc62](https://github.com/nihilai-collective/benchmarksuite/commit/71afc62) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. The sweep registers the two-stage path on the 44 corpus tests of each build but not on the five POD-type tests, so the fused-against-two-stage comparisons below cover corpus documents only. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Two freshly allocated full-parse rows contain non-converged results, both on macOS/Clang, as does one reused row, also on macOS/Clang; every x86 row converged. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head on the 220 corpus tests, the fused path wins 201, 5 are statistical ties, and the two-stage path wins 14:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 38 | 2 | 4 | 44 of 44 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 1 | 1 | 44 of 44 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 41 | 2 | 1 | 42 of 44 |
| **Aggregate** | **201** | **5** | **14** | **218 of 220** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 8 / 2 / 1 | 18 / 0 / 4 | 21 / 1 / 0 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 8 / 0 / 3 | 18 / 0 / 4 | 18 / 2 / 2 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **245 / 0 / 0** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 39 | 1 | 4 | 44 of 44 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 40 | 0 | 4 | 44 of 44 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 41 | 2 | 1 | 44 of 44 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 40 | 2 | 2 | 43 of 44 |
| **Aggregate** | **200** | **5** | **15** | **219 of 220** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| Minified corpus documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Prettified corpus documents | 9 / 1 / 1 | 18 / 0 / 4 | 21 / 1 / 0 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 20 / 2 / 0 |
| Small prettified documents | 8 / 0 / 3 | 18 / 0 / 4 | 18 / 1 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **245 / 0 / 0** | **245 / 0 / 0** |

With objects reused, across the 220 corpus tests the fused path wins 200, ties 5 and loses 15 to the two-stage path. On x86 the fused path wins all 66 minified documents and 53 of the 66 prettified ones, with 1 tie; the tape's 12 wins are the Marine IK family plus prettified Canada Small under MSVC. On the M2 Pro the fused path wins 42 of the 44 minified documents (2 ties) and 39 of the 44 prettified ones, with 2 ties and 3 tape wins. Against simdjson, the faster Jsonifier path wins all 245, and the fused path alone wins all 245 too.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput. Ratios here and in §2.4 are computed from unrounded throughput, so one recomputed from the rounded columns can differ in the last digit.

#### Freshly allocated objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 620 | n/a | 177 | n/a | n/a | Win |
| Double (POD) | 827 | n/a | 343 | n/a | n/a | Win |
| Int64 (POD) | 1,011 | n/a | 323 | n/a | n/a | Win |
| String (POD) | 729 | n/a | 487 | n/a | n/a | Win |
| Uint64 (POD) | 985 | n/a | 282 | n/a | n/a | Win |
| Apache Builds (minified) | 934 | 846 | 724 | 1.10× | Fused | Win |
| Canada (minified) | 654 | 627 | 398 | 1.04× | Fused | Win |
| Apache Builds (prettified) | 1,140 | 1,073 | 933 | 1.06× | Fused | Win |
| Canada (prettified) | 1,422 | 1,404 | 950 | 1.01× | Tie | Win |
| CitmCatalog (minified) | 1,617 | 1,287 | 625 | 1.26× | Fused | Win |
| CitmCatalog (prettified) | 2,716 | 2,653 | 1,418 | 1.02× | Fused | Win |
| Discord (minified) | 1,345 | 1,052 | 769 | 1.28× | Fused | Win |
| Discord (prettified) | 1,712 | 1,377 | 1,116 | 1.24× | Fused | Win |
| Github Events (minified) | 1,087 | 944 | 734 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,296 | 1,126 | 598 | 1.15× | Fused | Win |
| Github Events (prettified) | 1,210 | 1,137 | 889 | 1.06× | Fused | Win |
| Google Maps Response (prettified) | 2,292 | 2,099 | 1,209 | 1.09× | Fused | Win |
| Instruments (minified) | 2,119 | 1,447 | 899 | 1.46× | Fused | Win |
| Instruments (prettified) | 2,679 | 2,339 | 1,451 | 1.15× | Fused | Win |
| Marine IK Reverse (minified) | 675 | 585 | 199 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,086 | 2,102 | 687 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 699 | 591 | 367 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,137 | 2,128 | 1,355 | 1.00× | Tie | Win |
| Random (minified) | 928 | 760 | 580 | 1.22× | Fused | Win |
| Random (prettified) | 1,275 | 1,138 | 882 | 1.12× | Fused | Win |
| Twitter (minified) | 1,466 | 1,128 | 977 | 1.30× | Fused | Win |
| Twitter (prettified) | 1,635 | 1,410 | 1,264 | 1.16× | Fused | Win |
| Apache Builds Small (minified) | 1,294 | 1,130 | 772 | 1.14× | Fused | Win |
| Canada Small (minified) | 601 | 570 | 348 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,438 | 1,380 | 961 | 1.04× | Fused | Win |
| Canada Small (prettified) | 1,294 | 1,329 | 800 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 872 | 737 | 554 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,255 | 1,247 | 954 | 1.01× | Fused | Win |
| Discord Small (minified) | 1,267 | 993 | 699 | 1.28× | Fused | Win |
| Discord Small (prettified) | 1,444 | 1,239 | 992 | 1.17× | Fused | Win |
| Github Events Small (minified) | 1,091 | 879 | 688 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 1,129 | 981 | 589 | 1.15× | Fused | Win |
| Github Events Small (prettified) | 1,187 | 1,094 | 875 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,050 | 1,854 | 1,145 | 1.11× | Fused | Win |
| Instruments Small (minified) | 2,142 | 1,446 | 906 | 1.48× | Fused | Win |
| Instruments Small (prettified) | 2,621 | 2,311 | 1,454 | 1.13× | Fused | Win |
| Marine IK Reverse Small (minified) | 469 | 440 | 162 | 1.07× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,385 | 1,600 | 613 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 530 | 473 | 315 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,554 | 1,696 | 1,166 | 0.92× | Two-stage | Win |
| Random Small (minified) | 1,029 | 846 | 558 | 1.22× | Fused | Win |
| Random Small (prettified) | 1,383 | 1,266 | 847 | 1.09× | Fused | Win |
| Twitter Small (minified) | 1,898 | 1,380 | 1,388 | 1.38× | Fused | Win |
| Twitter Small (prettified) | 1,956 | 1,737 | 1,800 | 1.13× | Fused | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,968 | n/a | 488 | n/a | n/a | Win |
| Double (POD) | 1,248 | n/a | 424 | n/a | n/a | Win |
| Int64 (POD) | 1,522 | n/a | 373 | n/a | n/a | Win |
| String (POD) | 1,283 | n/a | 933 | n/a | n/a | Win |
| Uint64 (POD) | 1,741 | n/a | 363 | n/a | n/a | Win |
| Apache Builds (minified) | 1,822 | 1,397 | 1,226 | 1.30× | Fused | Win |
| Canada (minified) | 913 | 839 | 802 | 1.09× | Fused | Win |
| Apache Builds (prettified) | 2,120 | 1,645 | 1,581 | 1.29× | Fused | Win |
| Canada (prettified) | 1,906 | 1,894 | 1,806 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,294 | 1,668 | 1,102 | 1.38× | Fused | Win |
| CitmCatalog (prettified) | 3,755 | 3,456 | 2,331 | 1.09× | Fused | Win |
| Discord (minified) | 2,426 | 1,694 | 933 | 1.43× | Fused | Win |
| Discord (prettified) | 2,935 | 2,389 | 1,366 | 1.23× | Fused | Win |
| Github Events (minified) | 1,801 | 1,720 | 948 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,525 | 1,689 | 1,079 | 1.49× | Fused | Win |
| Github Events (prettified) | 2,095 | 1,772 | 1,147 | 1.18× | Fused | Win |
| Google Maps Response (prettified) | 3,685 | 2,922 | 2,158 | 1.26× | Fused | Win |
| Instruments (minified) | 2,401 | 1,885 | 709 | 1.27× | Fused | Win |
| Instruments (prettified) | 3,606 | 2,965 | 1,164 | 1.22× | Fused | Win |
| Marine IK Reverse (minified) | 811 | 703 | 600 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,451 | 2,551 | 2,181 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 820 | 714 | 590 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,478 | 2,555 | 2,150 | 0.97× | Two-stage | Win |
| Random (minified) | 1,624 | 1,148 | 829 | 1.41× | Fused | Win |
| Random (prettified) | 2,196 | 1,658 | 1,271 | 1.32× | Fused | Win |
| Twitter (minified) | 1,991 | 1,628 | 923 | 1.22× | Fused | Win |
| Twitter (prettified) | 2,533 | 2,016 | 1,225 | 1.26× | Fused | Win |
| Apache Builds Small (minified) | 2,925 | 2,031 | 1,377 | 1.44× | Fused | Win |
| Canada Small (minified) | 811 | 778 | 755 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 3,065 | 2,381 | 1,660 | 1.29× | Fused | Win |
| Canada Small (prettified) | 1,806 | 1,785 | 1,790 | 1.01× | Fused | Win |
| CitmCatalog Small (minified) | 1,740 | 1,328 | 956 | 1.31× | Fused | Win |
| CitmCatalog Small (prettified) | 2,450 | 2,284 | 1,699 | 1.07× | Fused | Win |
| Discord Small (minified) | 2,112 | 1,571 | 781 | 1.34× | Fused | Win |
| Discord Small (prettified) | 2,543 | 2,112 | 1,067 | 1.20× | Fused | Win |
| Github Events Small (minified) | 2,276 | 1,771 | 1,343 | 1.29× | Fused | Win |
| Google Maps Response Small (minified) | 2,340 | 1,571 | 1,078 | 1.49× | Fused | Win |
| Github Events Small (prettified) | 2,462 | 2,048 | 1,667 | 1.20× | Fused | Win |
| Google Maps Response Small (prettified) | 3,514 | 2,857 | 2,201 | 1.23× | Fused | Win |
| Instruments Small (minified) | 2,462 | 1,869 | 649 | 1.32× | Fused | Win |
| Instruments Small (prettified) | 3,495 | 2,971 | 1,084 | 1.18× | Fused | Win |
| Marine IK Reverse Small (minified) | 763 | 658 | 512 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,087 | 2,382 | 1,875 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 815 | 714 | 510 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,262 | 2,534 | 1,881 | 0.89× | Two-stage | Win |
| Random Small (minified) | 1,953 | 1,401 | 862 | 1.39× | Fused | Win |
| Random Small (prettified) | 2,705 | 2,153 | 1,321 | 1.26× | Fused | Win |
| Twitter Small (minified) | 2,596 | 2,142 | 1,025 | 1.21× | Fused | Win |
| Twitter Small (prettified) | 3,090 | 2,627 | 1,332 | 1.18× | Fused | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,417 | n/a | 467 | n/a | n/a | Win |
| Double (POD) | 1,269 | n/a | 394 | n/a | n/a | Win |
| Int64 (POD) | 1,891 | n/a | 397 | n/a | n/a | Win |
| String (POD) | 1,393 | n/a | 905 | n/a | n/a | Win |
| Uint64 (POD) | 1,810 | n/a | 364 | n/a | n/a | Win |
| Apache Builds (minified) | 1,877 | 1,351 | 1,394 | 1.39× | Fused | Win |
| Canada (minified) | 940 | 811 | 764 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,186 | 1,666 | 1,788 | 1.31× | Fused | Win |
| Canada (prettified) | 1,991 | 1,892 | 1,719 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,535 | 1,891 | 1,504 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 3,941 | 3,637 | 3,019 | 1.08× | Fused | Win |
| Discord (minified) | 2,524 | 1,814 | 1,360 | 1.39× | Fused | Win |
| Discord (prettified) | 3,116 | 2,603 | 1,938 | 1.20× | Fused | Win |
| Github Events (minified) | 1,882 | 1,463 | 1,363 | 1.29× | Fused | Win |
| Google Maps Response (minified) | 2,641 | 1,674 | 1,197 | 1.58× | Fused | Win |
| Github Events (prettified) | 2,361 | 1,781 | 1,679 | 1.33× | Fused | Win |
| Google Maps Response (prettified) | 3,975 | 3,203 | 2,417 | 1.24× | Fused | Win |
| Instruments (minified) | 3,175 | 2,186 | 1,533 | 1.45× | Fused | Win |
| Instruments (prettified) | 3,991 | 3,423 | 2,456 | 1.17× | Fused | Win |
| Marine IK Reverse (minified) | 860 | 730 | 553 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,441 | 2,550 | 2,003 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 843 | 719 | 557 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,573 | 2,583 | 2,041 | 1.00× | Two-stage | Win |
| Random (minified) | 1,605 | 1,136 | 1,000 | 1.41× | Fused | Win |
| Random (prettified) | 2,202 | 1,675 | 1,520 | 1.31× | Fused | Win |
| Twitter (minified) | 2,346 | 1,637 | 1,153 | 1.43× | Fused | Win |
| Twitter (prettified) | 2,581 | 2,037 | 1,499 | 1.27× | Fused | Win |
| Apache Builds Small (minified) | 2,899 | 2,020 | 1,699 | 1.44× | Fused | Win |
| Canada Small (minified) | 928 | 774 | 663 | 1.20× | Fused | Win |
| Apache Builds Small (prettified) | 3,010 | 2,375 | 2,137 | 1.27× | Fused | Win |
| Canada Small (prettified) | 1,948 | 1,895 | 1,590 | 1.03× | Fused | Win |
| CitmCatalog Small (minified) | 1,917 | 1,451 | 1,207 | 1.32× | Fused | Win |
| CitmCatalog Small (prettified) | 2,575 | 2,356 | 2,098 | 1.09× | Fused | Win |
| Discord Small (minified) | 2,333 | 1,661 | 1,333 | 1.40× | Fused | Win |
| Discord Small (prettified) | 2,584 | 2,131 | 1,778 | 1.21× | Fused | Win |
| Github Events Small (minified) | 2,213 | 1,682 | 1,547 | 1.32× | Fused | Win |
| Google Maps Response Small (minified) | 2,415 | 1,593 | 1,170 | 1.52× | Fused | Win |
| Github Events Small (prettified) | 2,486 | 1,999 | 1,939 | 1.24× | Fused | Win |
| Google Maps Response Small (prettified) | 3,736 | 2,994 | 2,365 | 1.25× | Fused | Win |
| Instruments Small (minified) | 3,150 | 2,101 | 1,358 | 1.50× | Fused | Win |
| Instruments Small (prettified) | 3,791 | 3,192 | 2,212 | 1.19× | Fused | Win |
| Marine IK Reverse Small (minified) | 797 | 709 | 503 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,247 | 2,553 | 1,846 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 937 | 779 | 507 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,473 | 2,752 | 1,895 | 0.90× | Two-stage | Win |
| Random Small (minified) | 2,113 | 1,509 | 1,078 | 1.40× | Fused | Win |
| Random Small (prettified) | 2,728 | 2,181 | 1,664 | 1.25× | Fused | Win |
| Twitter Small (minified) | 3,326 | 2,257 | 1,363 | 1.47× | Fused | Win |
| Twitter Small (prettified) | 3,337 | 2,772 | 1,802 | 1.20× | Fused | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,758 | n/a | 438 | n/a | n/a | Win |
| Double (POD) | 878 | n/a | 309 | n/a | n/a | Win |
| Int64 (POD) | 1,218 | n/a | 278 | n/a | n/a | Win |
| String (POD) | 1,010 | n/a | 541 | n/a | n/a | Win |
| Uint64 (POD) | 1,267 | n/a | 293 | n/a | n/a | Win |
| Apache Builds (minified) | 1,157 | 1,054 | 760 | 1.10× | Fused | Win |
| Canada (minified) | 764 | 625 | 588 | 1.22× | Fused | Win |
| Apache Builds (prettified) | 1,356 | 1,253 | 974 | 1.08× | Fused | Win |
| Canada (prettified) | 1,482 | 1,372 | 1,285 | 1.08× | Fused | Win |
| CitmCatalog (minified) | 2,081 | 1,399 | 1,161 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 3,166 | 2,689 | 2,235 | 1.18× | Fused | Win |
| Discord (minified) | 1,833 | 1,349 | 1,013 | 1.36× | Fused | Win |
| Discord (prettified) | 2,060 | 1,834 | 1,404 | 1.12× | Fused | Win |
| Github Events (minified) | 1,665 | 1,449 | 1,045 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,782 | 1,320 | 766 | 1.35× | Fused | Win |
| Github Events (prettified) | 1,766 | 1,654 | 1,242 | 1.07× | Fused | Win |
| Google Maps Response (prettified) | 2,623 | 2,328 | 1,459 | 1.13× | Fused | Win |
| Instruments (minified) | 2,661 | 1,667 | 1,216 | 1.60× | Fused | Win |
| Instruments (prettified) | 2,951 | 2,436 | 1,850 | 1.21× | Fused | Win |
| Marine IK Reverse (minified) | 686 | 546 | 478 | 1.26× | Fused | Win |
| Marine IK Reverse (prettified) | 2,006 | 1,905 | 1,658 | 1.05× | Fused | Win |
| Marine IK (minified) | 673 | 534 | 442 | 1.26× | Fused | Win |
| Marine IK (prettified) | 1,931 | 1,853 | 1,540 | 1.04× | Fused | Win |
| Random (minified) | 1,173 | 962 | 705 | 1.22× | Fused | Win |
| Random (prettified) | 1,550 | 1,362 | 1,061 | 1.14× | Fused | Win |
| Twitter (minified) | 2,083 | 1,674 | 1,046 | 1.24× | Fused | Win |
| Twitter (prettified) | 2,166 | 2,028 | 1,350 | 1.07× | Fused | Win |
| Apache Builds Small (minified) | 1,574 | 1,435 | 883 | 1.10× | Fused | Win |
| Canada Small (minified) | 738 | 612 | 479 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 1,699 | 1,673 | 1,103 | 1.02× | Fused | Win |
| Canada Small (prettified) | 1,521 | 1,390 | 1,101 | 1.09× | Fused | Win |
| CitmCatalog Small (minified) | 1,317 | 1,026 | 848 | 1.28× | Fused | Win |
| CitmCatalog Small (prettified) | 1,886 | 1,691 | 1,429 | 1.11× | Fused | Win |
| Discord Small (minified) | 1,704 | 1,373 | 940 | 1.24× | Fused | Win |
| Discord Small (prettified) | 1,963 | 1,713 | 1,239 | 1.15× | Fused | Win |
| Github Events Small (minified) | 1,448 | 1,275 | 1,197 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 1,640 | 1,228 | 771 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 1,648 | 1,471 | 1,493 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,433 | 2,187 | 1,483 | 1.11× | Fused | Win |
| Instruments Small (minified) | 2,720 | 1,691 | 1,128 | 1.61× | Fused | Win |
| Instruments Small (prettified) | 2,848 | 2,484 | 1,769 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 640 | 534 | 386 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,752 | 1,873 | 1,373 | 0.94× | Two-stage | Win |
| Marine IK Small (minified) | 693 | 559 | 376 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 1,979 | 1,948 | 1,318 | 1.02× | Fused | Win |
| Random Small (minified) | 1,341 | 1,117 | 729 | 1.20× | Fused | Win |
| Random Small (prettified) | 1,785 | 1,612 | 1,088 | 1.11× | Fused | Win |
| Twitter Small (minified) | 2,367 | 1,838 | 1,146 | 1.29× | Fused | Win |
| Twitter Small (prettified) | 2,231 | 2,211 | 1,457 | 1.01× | Tie | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,200 | n/a | 294 | n/a | n/a | Win |
| Double (POD) | 852 | n/a | 306 | n/a | n/a | Win |
| Int64 (POD) | 967 | n/a | 282 | n/a | n/a | Win |
| String (POD) | 955 | n/a | 581 | n/a | n/a | Win |
| Uint64 (POD) | 1,009 | n/a | 233 | n/a | n/a | Win |
| Apache Builds (minified) | 1,203 | 1,122 | 793 | 1.07× | Fused | Win |
| Canada (minified) | 616 | 516 | 454 | 1.19× | Fused | Win |
| Apache Builds (prettified) | 1,253 | 1,144 | 927 | 1.10× | Fused | Win |
| Canada (prettified) | 1,183 | 1,131 | 900 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 1,829 | 1,182 | 647 | 1.55× | Fused | Win |
| CitmCatalog (prettified) | 2,586 | 2,137 | 1,355 | 1.21× | Fused | Win |
| Discord (minified) | 1,797 | 1,308 | 587 | 1.37× | Fused | Win |
| Discord (prettified) | 1,861 | 1,617 | 830 | 1.15× | Fused | Win |
| Github Events (minified) | 1,314 | 1,205 | 653 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 1,672 | 1,036 | 597 | 1.61× | Fused | Win |
| Github Events (prettified) | 1,469 | 1,235 | 802 | 1.19× | Fused | Win |
| Google Maps Response (prettified) | 2,530 | 1,790 | 1,321 | 1.41× | Fused | Win |
| Instruments (minified) | 2,084 | 1,329 | 603 | 1.57× | Fused | Win |
| Instruments (prettified) | 2,285 | 2,193 | 1,115 | 1.04× | Fused | Win |
| Marine IK Reverse (minified) | 577 | 467 | 350 (n/c) | 1.24× | Fused | Win |
| Marine IK Reverse (prettified) | 1,565 | 1,538 | 1,141 | 1.02× | Tie | Win |
| Marine IK (minified) | 581 | 503 | 378 | 1.16× | Fused | Win |
| Marine IK (prettified) | 1,800 | 1,674 | 1,304 | 1.08× | Fused | Win |
| Random (minified) | 1,237 | 944 | 599 | 1.31× | Fused | Win |
| Random (prettified) | 1,611 | 1,262 | 840 | 1.28× | Fused | Win |
| Twitter (minified) | 1,888 | 1,424 (n/c) | 665 | 1.33× | Fused | Win |
| Twitter (prettified) | 2,176 | 1,962 | 1,123 | 1.11× | Fused | Win |
| Apache Builds Small (minified) | 1,317 | 1,291 | 727 | 1.02× | Fused | Win |
| Canada Small (minified) | 610 | 564 | 386 | 1.08× | Fused | Win |
| Apache Builds Small (prettified) | 1,445 | 1,496 | 948 | 0.97× | Two-stage | Win |
| Canada Small (prettified) | 1,375 | 1,288 | 994 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,250 | 1,022 | 695 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 1,796 | 1,683 | 1,192 | 1.07× | Fused | Win |
| Discord Small (minified) | 2,173 | 1,596 | 620 | 1.36× | Fused | Win |
| Discord Small (prettified) | 2,083 | 1,602 | 810 | 1.30× | Fused | Win |
| Github Events Small (minified) | 1,194 | 1,107 | 967 | 1.08× | Fused | Win |
| Google Maps Response Small (minified) | 1,820 | 1,095 | 644 | 1.66× | Fused | Win |
| Github Events Small (prettified) | 1,394 | 1,380 | 946 | 1.01× | Tie | Win |
| Google Maps Response Small (prettified) | 2,651 | 2,237 | 1,263 | 1.19× | Fused | Win |
| Instruments Small (minified) | 2,265 | 1,678 | 590 | 1.35× | Fused | Win |
| Instruments Small (prettified) | 2,773 | 2,481 | 873 | 1.12× | Fused | Win |
| Marine IK Reverse Small (minified) | 577 | 548 | 333 | 1.05× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,808 | 1,769 | 1,110 | 1.02× | Fused | Win |
| Marine IK Small (minified) | 598 | 482 | 264 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 1,594 | 1,440 | 999 | 1.11× | Fused | Win |
| Random Small (minified) | 1,119 | 914 | 533 | 1.22× | Fused | Win |
| Random Small (prettified) | 1,557 | 1,289 | 770 | 1.21× | Fused | Win |
| Twitter Small (minified) | 1,649 | 1,504 | 696 | 1.10× | Fused | Win |
| Twitter Small (prettified) | 2,084 | 1,925 | 915 | 1.08× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.52 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 702 | n/a | 183 | n/a | n/a | Win |
| Double (POD) | 1,029 | n/a | 373 | n/a | n/a | Win |
| Int64 (POD) | 1,515 | n/a | 371 | n/a | n/a | Win |
| String (POD) | 1,260 | n/a | 713 | n/a | n/a | Win |
| Uint64 (POD) | 1,495 | n/a | 321 | n/a | n/a | Win |
| Apache Builds (minified) | 1,467 | 1,252 | 1,054 | 1.17× | Fused | Win |
| Canada (minified) | 782 | 736 | 480 | 1.06× | Fused | Win |
| Apache Builds (prettified) | 1,682 | 1,566 | 1,355 | 1.07× | Fused | Win |
| Canada (prettified) | 1,672 | 1,646 | 1,077 | 1.02× | Tie | Win |
| CitmCatalog (minified) | 1,936 | 1,490 | 729 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,090 | 3,009 | 1,541 | 1.03× | Fused | Win |
| Discord (minified) | 2,083 | 1,464 | 1,019 | 1.42× | Fused | Win |
| Discord (prettified) | 2,449 | 1,803 | 1,461 | 1.36× | Fused | Win |
| Github Events (minified) | 1,703 | 1,385 | 964 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 1,492 | 1,259 | 634 | 1.18× | Fused | Win |
| Github Events (prettified) | 1,799 | 1,638 | 1,165 | 1.10× | Fused | Win |
| Google Maps Response (prettified) | 2,540 | 2,365 | 1,286 | 1.07× | Fused | Win |
| Instruments (minified) | 2,769 | 1,712 | 1,008 | 1.62× | Fused | Win |
| Instruments (prettified) | 3,242 | 2,740 | 1,663 | 1.18× | Fused | Win |
| Marine IK Reverse (minified) | 764 | 649 | 210 | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 2,194 | 2,267 | 722 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 795 | 651 | 396 | 1.22× | Fused | Win |
| Marine IK (prettified) | 2,372 | 2,295 | 1,455 | 1.03× | Fused | Win |
| Random (minified) | 1,356 | 1,039 | 733 | 1.30× | Fused | Win |
| Random (prettified) | 1,799 | 1,538 | 1,103 | 1.17× | Fused | Win |
| Twitter (minified) | 2,287 | 1,568 | 1,290 | 1.46× | Fused | Win |
| Twitter (prettified) | 2,383 | 1,957 | 1,681 | 1.22× | Fused | Win |
| Apache Builds Small (minified) | 1,741 | 1,450 | 1,251 | 1.20× | Fused | Win |
| Canada Small (minified) | 747 | 713 | 397 | 1.05× | Fused | Win |
| Apache Builds Small (prettified) | 1,884 | 1,736 | 1,542 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,586 | 1,614 | 935 | 0.98× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,228 | 968 | 702 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 1,655 | 1,609 | 1,222 | 1.03× | Fused | Win |
| Discord Small (minified) | 1,988 | 1,409 | 1,084 | 1.41× | Fused | Win |
| Discord Small (prettified) | 2,139 | 1,717 | 1,449 | 1.25× | Fused | Win |
| Github Events Small (minified) | 1,472 | 1,178 | 947 | 1.25× | Fused | Win |
| Google Maps Response Small (minified) | 1,366 | 1,155 | 701 | 1.18× | Fused | Win |
| Github Events Small (prettified) | 1,609 | 1,440 | 1,175 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,292 | 2,161 | 1,368 | 1.06× | Fused | Win |
| Instruments Small (minified) | 2,486 | 1,599 | 1,013 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 2,916 | 2,534 | 1,627 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 603 | 546 | 171 | 1.10× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,649 | 1,984 | 651 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 703 | 599 | 364 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 1,872 | 2,134 | 1,356 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,361 | 1,007 | 701 | 1.35× | Fused | Win |
| Random Small (prettified) | 1,724 | 1,508 | 1,075 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,416 | 1,634 | 1,569 | 1.48× | Fused | Win |
| Twitter Small (prettified) | 2,387 | 2,037 | 2,032 | 1.17× | Fused | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,152 | n/a | 510 | n/a | n/a | Win |
| Double (POD) | 1,369 | n/a | 428 | n/a | n/a | Win |
| Int64 (POD) | 1,774 | n/a | 375 | n/a | n/a | Win |
| String (POD) | 1,960 | n/a | 1,235 | n/a | n/a | Win |
| Uint64 (POD) | 2,080 | n/a | 375 | n/a | n/a | Win |
| Apache Builds (minified) | 3,122 | 2,037 | 1,698 | 1.53× | Fused | Win |
| Canada (minified) | 1,086 | 984 | 955 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 3,337 | 2,323 | 2,158 | 1.44× | Fused | Win |
| Canada (prettified) | 2,211 | 2,193 | 2,139 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 2,807 | 1,958 | 1,262 | 1.43× | Fused | Win |
| CitmCatalog (prettified) | 4,346 | 3,984 | 2,656 | 1.09× | Fused | Win |
| Discord (minified) | 3,618 | 2,276 | 1,113 | 1.59× | Fused | Win |
| Discord (prettified) | 4,161 | 3,172 | 1,612 | 1.31× | Fused | Win |
| Github Events (minified) | 2,542 | 2,245 | 1,123 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 2,908 | 1,882 | 1,132 | 1.55× | Fused | Win |
| Github Events (prettified) | 2,918 | 2,291 | 1,347 | 1.27× | Fused | Win |
| Google Maps Response (prettified) | 3,994 | 3,132 | 2,300 | 1.28× | Fused | Win |
| Instruments (minified) | 2,796 | 2,135 | 738 | 1.31× | Fused | Win |
| Instruments (prettified) | 4,148 | 3,319 | 1,217 | 1.25× | Fused | Win |
| Marine IK Reverse (minified) | 894 | 771 | 658 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 2,636 | 2,755 | 2,369 | 0.96× | Two-stage | Win |
| Marine IK (minified) | 910 | 793 | 653 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,696 | 2,807 | 2,358 | 0.96× | Two-stage | Win |
| Random (minified) | 2,576 | 1,565 | 1,028 | 1.65× | Fused | Win |
| Random (prettified) | 3,254 | 2,214 | 1,583 | 1.47× | Fused | Win |
| Twitter (minified) | 2,576 | 2,050 | 1,045 | 1.26× | Fused | Win |
| Twitter (prettified) | 3,243 | 2,464 | 1,382 | 1.32× | Fused | Win |
| Apache Builds Small (minified) | 3,523 | 2,339 | 1,563 | 1.51× | Fused | Win |
| Canada Small (minified) | 973 | 934 | 891 | 1.04× | Fused | Win |
| Apache Builds Small (prettified) | 3,594 | 2,696 | 1,908 | 1.33× | Fused | Win |
| Canada Small (prettified) | 2,122 | 2,101 | 2,085 | 1.01× | Fused | Win |
| CitmCatalog Small (minified) | 2,219 | 1,588 | 1,085 | 1.40× | Fused | Win |
| CitmCatalog Small (prettified) | 2,904 | 2,650 | 1,897 | 1.10× | Fused | Win |
| Discord Small (minified) | 3,030 | 2,087 | 901 | 1.45× | Fused | Win |
| Discord Small (prettified) | 3,482 | 2,815 | 1,213 | 1.24× | Fused | Win |
| Github Events Small (minified) | 2,952 | 2,154 | 1,567 | 1.37× | Fused | Win |
| Google Maps Response Small (minified) | 2,729 | 1,751 | 1,155 | 1.56× | Fused | Win |
| Github Events Small (prettified) | 3,135 | 2,523 | 1,963 | 1.24× | Fused | Win |
| Google Maps Response Small (prettified) | 3,926 | 3,074 | 2,327 | 1.28× | Fused | Win |
| Instruments Small (minified) | 2,678 | 1,969 | 676 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 3,806 | 3,113 | 1,142 | 1.22× | Fused | Win |
| Marine IK Reverse Small (minified) | 915 | 770 | 562 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,389 | 2,774 | 2,064 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 990 | 843 | 558 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,588 | 2,974 | 2,026 | 0.87× | Two-stage | Win |
| Random Small (minified) | 2,549 | 1,687 | 989 | 1.51× | Fused | Win |
| Random Small (prettified) | 3,368 | 2,517 | 1,503 | 1.34× | Fused | Win |
| Twitter Small (minified) | 2,915 | 2,399 | 1,065 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 3,652 | 2,889 | 1,376 | 1.26× | Fused | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,643 | n/a | 477 | n/a | n/a | Win |
| Double (POD) | 1,401 | n/a | 426 | n/a | n/a | Win |
| Int64 (POD) | 2,220 | n/a | 408 | n/a | n/a | Win |
| String (POD) | 2,017 | n/a | 1,116 | n/a | n/a | Win |
| Uint64 (POD) | 2,218 | n/a | 385 | n/a | n/a | Win |
| Apache Builds (minified) | 3,121 | 1,962 | 1,987 | 1.59× | Fused | Win |
| Canada (minified) | 1,118 | 943 | 896 | 1.19× | Fused | Win |
| Apache Builds (prettified) | 3,332 | 2,347 | 2,518 | 1.42× | Fused | Win |
| Canada (prettified) | 2,325 | 2,189 | 2,000 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 3,178 | 2,261 | 1,763 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 4,630 | 4,217 | 3,569 | 1.10× | Fused | Win |
| Discord (minified) | 3,722 | 2,316 | 1,641 | 1.61× | Fused | Win |
| Discord (prettified) | 4,181 | 3,302 | 2,344 | 1.27× | Fused | Win |
| Github Events (minified) | 2,710 | 1,935 | 1,674 | 1.40× | Fused | Win |
| Google Maps Response (minified) | 3,030 | 1,834 | 1,287 | 1.65× | Fused | Win |
| Github Events (prettified) | 3,167 | 2,308 | 2,028 | 1.37× | Fused | Win |
| Google Maps Response (prettified) | 4,305 | 3,444 | 2,581 | 1.25× | Fused | Win |
| Instruments (minified) | 3,788 | 2,475 | 1,625 | 1.53× | Fused | Win |
| Instruments (prettified) | 4,529 | 3,826 | 2,607 | 1.18× | Fused | Win |
| Marine IK Reverse (minified) | 958 | 800 | 608 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,637 | 2,794 | 2,158 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 946 | 799 | 615 | 1.18× | Fused | Win |
| Marine IK (prettified) | 2,818 | 2,851 | 2,238 | 0.99× | Two-stage | Win |
| Random (minified) | 2,470 | 1,527 | 1,342 | 1.62× | Fused | Win |
| Random (prettified) | 3,175 | 2,179 | 2,006 | 1.46× | Fused | Win |
| Twitter (minified) | 3,440 | 2,048 | 1,340 | 1.68× | Fused | Win |
| Twitter (prettified) | 3,546 | 2,491 | 1,763 | 1.42× | Fused | Win |
| Apache Builds Small (minified) | 3,449 | 2,363 | 2,115 | 1.46× | Fused | Win |
| Canada Small (minified) | 1,133 | 929 | 750 | 1.22× | Fused | Win |
| Apache Builds Small (prettified) | 3,479 | 2,771 | 2,618 | 1.26× | Fused | Win |
| Canada Small (prettified) | 2,329 | 2,225 | 1,798 | 1.05× | Fused | Win |
| CitmCatalog Small (minified) | 2,337 | 1,698 | 1,407 | 1.38× | Fused | Win |
| CitmCatalog Small (prettified) | 3,050 | 2,786 | 2,443 | 1.09× | Fused | Win |
| Discord Small (minified) | 3,191 | 2,059 | 1,543 | 1.55× | Fused | Win |
| Discord Small (prettified) | 3,388 | 2,667 | 2,143 | 1.27× | Fused | Win |
| Github Events Small (minified) | 2,853 | 2,085 | 1,804 | 1.37× | Fused | Win |
| Google Maps Response Small (minified) | 2,751 | 1,756 | 1,300 | 1.57× | Fused | Win |
| Github Events Small (prettified) | 3,127 | 2,423 | 2,274 | 1.29× | Fused | Win |
| Google Maps Response Small (prettified) | 4,141 | 3,232 | 2,609 | 1.28× | Fused | Win |
| Instruments Small (minified) | 3,477 | 2,267 | 1,482 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 4,061 | 3,483 | 2,385 | 1.17× | Fused | Win |
| Marine IK Reverse Small (minified) | 970 | 848 | 542 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,574 | 2,997 | 2,012 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,153 | 915 | 544 | 1.26× | Fused | Win |
| Marine IK Small (prettified) | 2,836 | 3,227 | 2,025 | 0.88× | Two-stage | Win |
| Random Small (minified) | 2,790 | 1,835 | 1,311 | 1.52× | Fused | Win |
| Random Small (prettified) | 3,405 | 2,636 | 1,994 | 1.29× | Fused | Win |
| Twitter Small (minified) | 3,687 | 2,479 | 1,437 | 1.49× | Fused | Win |
| Twitter Small (prettified) | 3,651 | 3,139 | 1,857 | 1.16× | Fused | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,985 | n/a | 440 | n/a | n/a | Win |
| Double (POD) | 1,146 | n/a | 324 | n/a | n/a | Win |
| Int64 (POD) | 1,652 | n/a | 346 | n/a | n/a | Win |
| String (POD) | 1,925 | n/a | 731 | n/a | n/a | Win |
| Uint64 (POD) | 1,767 | n/a | 311 | n/a | n/a | Win |
| Apache Builds (minified) | 1,900 | 1,633 | 972 | 1.16× | Fused | Win |
| Canada (minified) | 942 | 737 | 682 | 1.28× | Fused | Win |
| Apache Builds (prettified) | 2,051 | 1,862 | 1,223 | 1.10× | Fused | Win |
| Canada (prettified) | 1,729 | 1,597 | 1,468 | 1.08× | Fused | Win |
| CitmCatalog (minified) | 2,591 | 1,616 | 1,286 | 1.60× | Fused | Win |
| CitmCatalog (prettified) | 3,587 | 3,055 | 2,444 | 1.17× | Fused | Win |
| Discord (minified) | 2,468 | 1,688 | 1,210 | 1.46× | Fused | Win |
| Discord (prettified) | 2,550 | 2,235 | 1,680 | 1.14× | Fused | Win |
| Github Events (minified) | 2,356 | 1,961 | 1,253 | 1.20× | Fused | Win |
| Google Maps Response (minified) | 2,075 | 1,456 | 820 | 1.42× | Fused | Win |
| Github Events (prettified) | 2,416 | 2,128 | 1,488 | 1.14× | Fused | Win |
| Google Maps Response (prettified) | 2,890 | 2,523 | 1,539 | 1.15× | Fused | Win |
| Instruments (minified) | 3,148 | 1,842 | 1,313 | 1.71× | Fused | Win |
| Instruments (prettified) | 3,255 | 2,670 | 1,982 | 1.22× | Fused | Win |
| Marine IK Reverse (minified) | 786 | 609 | 527 | 1.29× | Fused | Win |
| Marine IK Reverse (prettified) | 2,202 | 2,096 | 1,797 | 1.05× | Fused | Win |
| Marine IK (minified) | 777 | 598 | 488 | 1.30× | Fused | Win |
| Marine IK (prettified) | 2,220 | 2,036 | 1,666 | 1.09× | Fused | Win |
| Random (minified) | 1,834 | 1,378 | 874 | 1.33× | Fused | Win |
| Random (prettified) | 2,207 | 1,868 | 1,292 | 1.18× | Fused | Win |
| Twitter (minified) | 2,914 | 2,129 | 1,188 | 1.37× | Fused | Win |
| Twitter (prettified) | 2,704 | 2,591 | 1,523 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,856 | 1,845 | 1,165 | 1.01× | Tie | Win |
| Canada Small (minified) | 901 | 724 | 544 | 1.24× | Fused | Win |
| Apache Builds Small (prettified) | 2,164 | 2,126 | 1,458 | 1.02× | Tie | Win |
| Canada Small (prettified) | 1,802 | 1,641 | 1,245 | 1.10× | Fused | Win |
| CitmCatalog Small (minified) | 1,888 | 1,338 | 1,075 | 1.41× | Fused | Win |
| CitmCatalog Small (prettified) | 2,505 | 2,156 | 1,770 | 1.16× | Fused | Win |
| Discord Small (minified) | 2,403 | 1,814 | 1,166 | 1.32× | Fused | Win |
| Discord Small (prettified) | 2,528 | 2,264 | 1,518 | 1.12× | Fused | Win |
| Github Events Small (minified) | 2,061 | 1,725 | 1,497 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 2,027 | 1,427 | 856 | 1.42× | Fused | Win |
| Github Events Small (prettified) | 2,317 | 2,061 | 1,792 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,824 | 2,475 | 1,646 | 1.14× | Fused | Win |
| Instruments Small (minified) | 3,139 | 1,867 | 1,264 | 1.68× | Fused | Win |
| Instruments Small (prettified) | 3,263 | 2,662 | 1,958 | 1.23× | Fused | Win |
| Marine IK Reverse Small (minified) | 819 | 641 | 445 | 1.28× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,045 | 2,216 | 1,563 | 0.92× | Two-stage | Win |
| Marine IK Small (minified) | 895 | 697 | 428 | 1.28× | Fused | Win |
| Marine IK Small (prettified) | 2,369 | 2,314 | 1,480 | 1.02× | Fused | Win |
| Random Small (minified) | 1,858 | 1,414 | 920 | 1.31× | Fused | Win |
| Random Small (prettified) | 2,252 | 1,365 | 1,355 | 1.65× | Fused | Win |
| Twitter Small (minified) | 2,857 | 2,182 | 1,236 | 1.31× | Fused | Win |
| Twitter Small (prettified) | 2,638 | 2,571 | 1,550 | 1.03× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,304 | n/a | 295 | n/a | n/a | Win |
| Double (POD) | 1,053 | n/a | 302 | n/a | n/a | Win |
| Int64 (POD) | 1,273 | n/a | 303 | n/a | n/a | Win |
| String (POD) | 1,175 | n/a | 678 | n/a | n/a | Win |
| Uint64 (POD) | 1,354 | n/a | 244 | n/a | n/a | Win |
| Apache Builds (minified) | 1,917 | 1,415 | 860 | 1.35× | Fused | Win |
| Canada (minified) | 726 | 658 | 492 | 1.10× | Fused | Win |
| Apache Builds (prettified) | 1,685 | 1,436 | 1,096 | 1.17× | Fused | Win |
| Canada (prettified) | 1,418 | 1,263 | 1,064 | 1.12× | Fused | Win |
| CitmCatalog (minified) | 2,242 | 1,295 | 708 | 1.73× | Fused | Win |
| CitmCatalog (prettified) | 3,021 | 2,183 | 1,502 | 1.38× | Fused | Win |
| Discord (minified) | 2,335 | 1,535 | 663 | 1.52× | Fused | Win |
| Discord (prettified) | 2,145 | 1,875 | 922 | 1.14× | Fused | Win |
| Github Events (minified) | 1,780 | 1,480 | 730 | 1.20× | Fused | Win |
| Google Maps Response (minified) | 1,478 | 1,063 | 640 | 1.39× | Fused | Win |
| Github Events (prettified) | 1,977 | 1,724 | 973 | 1.15× | Fused | Win |
| Google Maps Response (prettified) | 2,191 | 1,979 | 1,413 | 1.11× | Fused | Win |
| Instruments (minified) | 2,520 | 1,485 | 707 | 1.70× | Fused | Win |
| Instruments (prettified) | 2,765 | 2,605 | 1,021 | 1.06× | Fused | Win |
| Marine IK Reverse (minified) | 654 | 533 | 362 | 1.23× | Fused | Win |
| Marine IK Reverse (prettified) | 1,794 | 1,714 | 1,401 | 1.05× | Fused | Win |
| Marine IK (minified) | 686 (n/c) | 667 | 390 | 1.03× | Fused | Win |
| Marine IK (prettified) | 1,943 | 1,975 | 1,404 | 0.98× | Tie | Win |
| Random (minified) | 1,717 | 1,132 | 673 | 1.52× | Fused | Win |
| Random (prettified) | 1,966 | 1,541 | 1,028 | 1.28× | Fused | Win |
| Twitter (minified) | 2,182 | 1,835 | 685 | 1.19× | Fused | Win |
| Twitter (prettified) | 2,463 | 2,318 | 1,109 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 1,472 | 1,414 | 993 | 1.04× | Tie | Win |
| Canada Small (minified) | 732 | 605 | 467 | 1.21× | Fused | Win |
| Apache Builds Small (prettified) | 2,047 | 1,839 | 1,385 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,390 | 1,497 | 1,182 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,920 | 1,415 | 877 | 1.36× | Fused | Win |
| CitmCatalog Small (prettified) | 2,490 | 2,224 | 1,517 | 1.12× | Fused | Win |
| Discord Small (minified) | 2,566 | 1,851 | 676 | 1.39× | Fused | Win |
| Discord Small (prettified) | 2,457 | 2,218 | 916 | 1.11× | Fused | Win |
| Github Events Small (minified) | 1,772 | 1,698 | 1,122 | 1.04× | Fused | Win |
| Google Maps Response Small (minified) | 2,011 | 1,282 | 789 | 1.57× | Fused | Win |
| Github Events Small (prettified) | 2,144 | 1,936 | 1,462 | 1.11× | Fused | Win |
| Google Maps Response Small (prettified) | 2,497 | 2,119 | 1,704 | 1.18× | Fused | Win |
| Instruments Small (minified) | 2,990 | 1,778 | 595 | 1.68× | Fused | Win |
| Instruments Small (prettified) | 3,073 | 2,661 | 1,049 | 1.16× | Fused | Win |
| Marine IK Reverse Small (minified) | 795 | 674 | 350 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,161 | 2,217 | 1,250 | 0.97× | Two-stage | Win |
| Marine IK Small (minified) | 737 | 670 | 311 | 1.10× | Fused | Win |
| Marine IK Small (prettified) | 1,881 | 1,811 | 1,190 | 1.04× | Fused | Win |
| Random Small (minified) | 1,314 | 1,113 | 659 | 1.18× | Fused | Win |
| Random Small (prettified) | 1,875 | 1,587 | 915 | 1.18× | Fused | Win |
| Twitter Small (minified) | 2,343 | 1,979 | 737 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,842 | 2,177 | 1,071 | 1.31× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster now depends mostly on the document, not on indentation or the machine. On the three x86 builds the fused path wins all 66 minified documents, full-size and Small, and 52 of the 66 prettified ones, with 2 ties; the tape's 12 wins are the four prettified Marine IK documents (Marine IK and Marine IK Reverse, full-size and Small) on each Linux build, and prettified Marine IK Reverse, Marine IK Small, Marine IK Reverse Small and Canada Small under MSVC. On the M2 Pro the fused path wins all 44 minified documents and 39 of the 44 prettified ones (2 to the tape, 3 ties). §2.8 repeats the comparison on AVX-512.

**POD-type tests: outside the comparison.** The sweep does not register the two-stage path on the five POD-type tests, so they have no fused-against-tape verdict. Against simdjson On Demand the fused path wins all 25 in both runs. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape would be pure overhead.

**Minified documents: all the fused path's.** Freshly allocated, the fused path wins all 110 minified documents. Its margins run from 4% (Canada under MSVC, 654 against 627 MB/s) to 1.61× (Google Maps Response on macOS/Clang, 1,672 against 1,036 MB/s) on the full-size documents and from 2% (Apache Builds Small on macOS/Clang, 1,317 against 1,291) to 1.66× (Google Maps Response Small on macOS/Clang) on the Small ones. With objects reused it wins 108 and ties 2, minified Apache Builds Small on both M2 Pro builds. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the fused path wins 52 of 66, the tape keeps the Marine IK family.** The fused path wins all nine full-size documents outside the Marine IK family on both Linux builds and eight under MSVC, where prettified Canada and Marine IK are ties; on the Small copies it wins the same nine on each Linux build and eight under MSVC. Its margins reach 1.33× (Github Events on Linux/GCC, 2,361 against 1,781 MB/s) on the full-size documents and 1.29× (Apache Builds Small on Linux/Clang) on the Small ones. The tape's 12 wins are all Marine IK documents except prettified Canada Small under MSVC, and they are narrow: from under 1% (Marine IK on Linux/GCC, 2,583 against 2,573 MB/s) to 4% (Marine IK Reverse on Linux/GCC) on the full-size documents and from 3% (Canada Small under MSVC) to 15% (Marine IK Reverse Small under MSVC) on the Small ones. Marine IK is almost entirely floating-point arrays, so its lines are short and numerous, which is where the fused path's per-line work (the newline test, the depth multiplication, the span check and the branch on its result) weighs most against stage 1's layout-independent vector classification. We have not isolated this, and state it as a hypothesis. With objects reused the split is the same: the fused path wins 53, ties 1 (Canada under MSVC) and the tape wins the same 12.

**Prettified documents on the M2 Pro: the fused path's.** Of the 44 prettified documents, the fused path wins 39, the tape wins 2 and 3 are ties. The tape takes Marine IK Reverse Small on macOS/GCC (0.94×) and Apache Builds Small on macOS/Clang (0.97×); the ties are Twitter Small on macOS/GCC and Marine IK Reverse and Github Events Small on macOS/Clang. The fused path's margins reach 1.21× (Instruments on macOS/GCC) and 1.41× (Google Maps Response on macOS/Clang) on the full-size documents. Reused, the fused path wins 39, the tape 3 (Marine IK Reverse Small on macOS/GCC, Canada Small and Marine IK Reverse Small on macOS/Clang) and 2 are ties.

**No document beats the fused path.** Against simdjson On Demand the fused path alone wins all 245 tests in both runs, so taking the faster Jsonifier path changes no verdict.

**MSVC slows all three by about the same median.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 65% of its Linux/Clang throughput, the two-stage path 68% and simdjson 66%. The spread between documents is narrowest for the two-stage path (from 55% on Github Events to 83% on Marine IK), wider for the fused path (from 51% on Apache Builds to 88% on Instruments) and widest for simdjson, which keeps 50% of its Linux/Clang throughput on Canada and more than all of it (127%) on Instruments. We have not profiled why.

**Against simdjson, Jsonifier wins all 245 tests with whichever path is faster, and with the fused path alone.**

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it still shows: simdjson parses minified Marine IK Reverse at 199 MB/s, against 367 MB/s for the same document in declared order, so the faster Jsonifier path is 3.40× faster than simdjson freshly allocated (3.65× reused). The prettified form shows the same gap, at 687 MB/s against 1,355, and the faster Jsonifier path is 3.06× faster (3.14× reused). The Small copies show it too, at 162 MB/s minified and 613 MB/s prettified under MSVC. On the other builds simdjson parses the full-size reversed document within about 15% of the declared one, sometimes faster, and the faster Jsonifier path is 1.17× to 1.65× faster than simdjson on it, freshly allocated, and 1.16× to 1.80× reused. Why the rescan cost shows up mostly under MSVC has not been profiled; §2.8 shows it again, wider, on the AVX-512 MSVC build.

The routing implication supports §2's rule of thumb: on every build, minified input belongs to the fused path, and so does prettified input except the float-heavy Marine IK family on x86, where the tape is ahead by at most 15%. Routing each test to its faster path would not change the record against simdjson, since the fused path alone already wins all 245. The router does not take the document's shape into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,351 | 1,414 | 0.96× | **Loss** |
| Canada (minified) | 811 | 844 | 0.96× | **Loss** |
| Apache Builds (prettified) | 1,666 | 1,807 | 0.92× | **Loss** |
| Canada (prettified) | 1,892 | 1,903 | 0.99× | **Loss** |
| CitmCatalog (minified) | 1,891 | 1,579 | 1.20× | Win |
| CitmCatalog (prettified) | 3,637 | 3,087 | 1.18× | Win |
| Discord (minified) | 1,814 | 1,604 | 1.13× | Win |
| Discord (prettified) | 2,603 | 2,274 | 1.14× | Win |
| Github Events (minified) | 1,463 | 1,537 | 0.95× | **Loss** |
| Google Maps Response (minified) | 1,674 | 1,568 | 1.07× | Win |
| Github Events (prettified) | 1,781 | 1,993 | 0.89× | **Loss** |
| Google Maps Response (prettified) | 3,203 | 3,132 | 1.02× | Win |
| Instruments (minified) | 2,186 | 1,768 | 1.24× | Win |
| Instruments (prettified) | 3,423 | 2,823 | 1.21× | Win |
| Marine IK Reverse (minified) | 730 | 696 | 1.05× | Win |
| Marine IK Reverse (prettified) | 2,550 | 2,419 | 1.05× | Win |
| Marine IK (minified) | 719 | 667 | 1.08× | Win |
| Marine IK (prettified) | 2,583 | 2,404 | 1.07× | Win |
| Random (minified) | 1,136 | 1,183 | 0.96× | **Loss** |
| Random (prettified) | 1,675 | 1,756 | 0.95× | **Loss** |
| Twitter (minified) | 1,637 | 1,600 | 1.02× | Win |
| Twitter (prettified) | 2,037 | 2,066 | 0.99× | **Loss** |
| Apache Builds Small (minified) | 2,020 | 1,801 | 1.12× | Win |
| Canada Small (minified) | 774 | 748 | 1.03× | Win |
| Apache Builds Small (prettified) | 2,375 | 2,246 | 1.06× | Win |
| Canada Small (prettified) | 1,895 | 1,777 | 1.07× | Win |
| CitmCatalog Small (minified) | 1,451 | 1,216 | 1.19× | Win |
| CitmCatalog Small (prettified) | 2,356 | 2,100 | 1.12× | Win |
| Discord Small (minified) | 1,661 | 1,544 | 1.08× | Win |
| Discord Small (prettified) | 2,131 | 2,060 | 1.03× | Win |
| Github Events Small (minified) | 1,682 | 1,729 | 0.97× | **Loss** |
| Google Maps Response Small (minified) | 1,593 | 1,510 | 1.05× | Win |
| Github Events Small (prettified) | 1,999 | 2,191 | 0.91× | **Loss** |
| Google Maps Response Small (prettified) | 2,994 | 3,035 | 0.99× | **Loss** |
| Instruments Small (minified) | 2,101 | 1,746 | 1.20× | Win |
| Instruments Small (prettified) | 3,192 | 2,808 | 1.14× | Win |
| Marine IK Reverse Small (minified) | 709 | 649 | 1.09× | Win |
| Marine IK Reverse Small (prettified) | 2,553 | 2,361 | 1.08× | Win |
| Marine IK Small (minified) | 779 | 645 | 1.21× | Win |
| Marine IK Small (prettified) | 2,752 | 2,355 | 1.17× | Win |
| Random Small (minified) | 1,509 | 1,293 | 1.17× | Win |
| Random Small (prettified) | 2,181 | 1,976 | 1.10× | Win |
| Twitter Small (minified) | 2,257 | 1,961 | 1.15× | Win |
| Twitter Small (prettified) | 2,772 | 2,516 | 1.10× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,054 | 921 | 1.14× | Win |
| Canada (minified) | 625 | 639 | 0.98× | **Loss** |
| Apache Builds (prettified) | 1,253 | 1,177 | 1.06× | Win |
| Canada (prettified) | 1,372 | 1,395 | 0.98× | **Loss** |
| CitmCatalog (minified) | 1,399 | 1,155 | 1.21× | Win |
| CitmCatalog (prettified) | 2,689 | 2,245 | 1.20× | Win |
| Discord (minified) | 1,349 | 1,156 | 1.17× | Win |
| Discord (prettified) | 1,834 | 1,600 | 1.15× | Win |
| Github Events (minified) | 1,449 | 1,251 | 1.16× | Win |
| Google Maps Response (minified) | 1,320 | 1,169 | 1.13× | Win |
| Github Events (prettified) | 1,654 | 1,475 | 1.12× | Win |
| Google Maps Response (prettified) | 2,328 | 2,102 | 1.11× | Win |
| Instruments (minified) | 1,667 | 1,365 | 1.22× | Win |
| Instruments (prettified) | 2,436 | 2,056 | 1.18× | Win |
| Marine IK Reverse (minified) | 546 | 583 | 0.94× | **Loss** |
| Marine IK Reverse (prettified) | 1,905 | 1,948 | 0.98× | **Loss** |
| Marine IK (minified) | 534 | 557 | 0.96× | **Loss** |
| Marine IK (prettified) | 1,853 | 1,876 | 0.99× | **Loss** |
| Random (minified) | 962 | 808 | 1.19× | Win |
| Random (prettified) | 1,362 | 1,196 | 1.14× | Win |
| Twitter (minified) | 1,674 | 1,351 | 1.24× | Win |
| Twitter (prettified) | 2,028 | 1,722 | 1.18× | Win |
| Apache Builds Small (minified) | 1,435 | 1,102 | 1.30× | Win |
| Canada Small (minified) | 612 | 587 | 1.04× | Win |
| Apache Builds Small (prettified) | 1,673 | 1,366 | 1.22× | Win |
| Canada Small (prettified) | 1,390 | 1,339 | 1.04× | Win |
| CitmCatalog Small (minified) | 1,026 | 877 | 1.17× | Win |
| CitmCatalog Small (prettified) | 1,691 | 1,481 | 1.14× | Win |
| Discord Small (minified) | 1,373 | 1,080 | 1.27× | Win |
| Discord Small (prettified) | 1,713 | 1,426 | 1.20× | Win |
| Github Events Small (minified) | 1,275 | 1,152 | 1.11× | Win |
| Google Maps Response Small (minified) | 1,228 | 1,048 | 1.17× | Win |
| Github Events Small (prettified) | 1,471 | 1,343 | 1.10× | Win |
| Google Maps Response Small (prettified) | 2,187 | 1,956 | 1.12× | Win |
| Instruments Small (minified) | 1,691 | 1,334 | 1.27× | Win |
| Instruments Small (prettified) | 2,484 | 2,078 | 1.20× | Win |
| Marine IK Reverse Small (minified) | 534 | 528 | 1.01× | Win |
| Marine IK Reverse Small (prettified) | 1,873 | 1,785 | 1.05× | Win |
| Marine IK Small (minified) | 559 | 529 | 1.06× | Win |
| Marine IK Small (prettified) | 1,948 | 1,795 | 1.09× | Win |
| Random Small (minified) | 1,117 | 878 | 1.27× | Win |
| Random Small (prettified) | 1,612 | 1,313 | 1.23× | Win |
| Twitter Small (minified) | 1,838 | 1,401 | 1.31× | Win |
| Twitter Small (prettified) | 2,211 | 1,754 | 1.26× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,962 | 1,809 | 1.08× | Win |
| Canada (minified) | 943 | 992 | 0.95× | **Loss** |
| Apache Builds (prettified) | 2,347 | 2,299 | 1.02× | Win |
| Canada (prettified) | 2,189 | 2,223 | 0.98× | **Loss** |
| CitmCatalog (minified) | 2,261 | 1,801 | 1.26× | Win |
| CitmCatalog (prettified) | 4,217 | 3,626 | 1.16× | Win |
| Discord (minified) | 2,316 | 1,830 | 1.27× | Win |
| Discord (prettified) | 3,302 | 2,603 | 1.27× | Win |
| Github Events (minified) | 1,935 | 1,917 | 1.01× | Win |
| Google Maps Response (minified) | 1,834 | 1,659 | 1.11× | Win |
| Github Events (prettified) | 2,308 | 2,315 | 1.00× | **Loss** |
| Google Maps Response (prettified) | 3,444 | 3,317 | 1.04× | Win |
| Instruments (minified) | 2,475 | 1,810 | 1.37× | Win |
| Instruments (prettified) | 3,826 | 2,870 | 1.33× | Win |
| Marine IK Reverse (minified) | 800 | 763 | 1.05× | Win |
| Marine IK Reverse (prettified) | 2,794 | 2,641 | 1.06× | Win |
| Marine IK (minified) | 799 | 752 | 1.06× | Win |
| Marine IK (prettified) | 2,851 | 2,677 | 1.06× | Win |
| Random (minified) | 1,527 | 1,550 | 0.99× | **Loss** |
| Random (prettified) | 2,179 | 2,275 | 0.96× | **Loss** |
| Twitter (minified) | 2,048 | 1,870 | 1.10× | Win |
| Twitter (prettified) | 2,491 | 2,406 | 1.04× | Win |
| Apache Builds Small (minified) | 2,363 | 1,983 | 1.19× | Win |
| Canada Small (minified) | 929 | 854 | 1.09× | Win |
| Apache Builds Small (prettified) | 2,771 | 2,497 | 1.11× | Win |
| Canada Small (prettified) | 2,225 | 2,003 | 1.11× | Win |
| CitmCatalog Small (minified) | 1,698 | 1,362 | 1.25× | Win |
| CitmCatalog Small (prettified) | 2,786 | 2,344 | 1.19× | Win |
| Discord Small (minified) | 2,059 | 1,813 | 1.14× | Win |
| Discord Small (prettified) | 2,667 | 2,436 | 1.09× | Win |
| Github Events Small (minified) | 2,085 | 1,988 | 1.05× | Win |
| Google Maps Response Small (minified) | 1,756 | 1,677 | 1.05× | Win |
| Github Events Small (prettified) | 2,423 | 2,433 | 1.00× | Tie |
| Google Maps Response Small (prettified) | 3,232 | 3,256 | 0.99× | **Loss** |
| Instruments Small (minified) | 2,267 | 1,813 | 1.25× | Win |
| Instruments Small (prettified) | 3,483 | 2,945 | 1.18× | Win |
| Marine IK Reverse Small (minified) | 848 | 736 | 1.15× | Win |
| Marine IK Reverse Small (prettified) | 2,997 | 2,633 | 1.14× | Win |
| Marine IK Small (minified) | 915 | 739 | 1.24× | Win |
| Marine IK Small (prettified) | 3,227 | 2,663 | 1.21× | Win |
| Random Small (minified) | 1,835 | 1,460 | 1.26× | Win |
| Random Small (prettified) | 2,636 | 2,226 | 1.18× | Win |
| Twitter Small (minified) | 2,479 | 2,130 | 1.16× | Win |
| Twitter Small (prettified) | 3,139 | 2,709 | 1.16× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,633 | 1,148 | 1.42× | Win |
| Canada (minified) | 737 | 762 | 0.97× | **Loss** |
| Apache Builds (prettified) | 1,862 | 1,458 | 1.28× | Win |
| Canada (prettified) | 1,597 | 1,631 | 0.98× | **Loss** |
| CitmCatalog (minified) | 1,616 | 1,294 | 1.25× | Win |
| CitmCatalog (prettified) | 3,055 | 2,455 | 1.24× | Win |
| Discord (minified) | 1,688 | 1,305 | 1.29× | Win |
| Discord (prettified) | 2,235 | 1,791 | 1.25× | Win |
| Github Events (minified) | 1,961 | 1,486 | 1.32× | Win |
| Google Maps Response (minified) | 1,456 | 1,279 | 1.14× | Win |
| Github Events (prettified) | 2,128 | 1,745 | 1.22× | Win |
| Google Maps Response (prettified) | 2,523 | 2,264 | 1.11× | Win |
| Instruments (minified) | 1,842 | 1,419 | 1.30× | Win |
| Instruments (prettified) | 2,670 | 2,139 | 1.25× | Win |
| Marine IK Reverse (minified) | 609 | 657 | 0.93× | **Loss** |
| Marine IK Reverse (prettified) | 2,096 | 2,147 | 0.98× | **Loss** |
| Marine IK (minified) | 598 | 629 | 0.95× | **Loss** |
| Marine IK (prettified) | 2,036 | 2,063 | 0.99× | **Loss** |
| Random (minified) | 1,378 | 982 | 1.40× | Win |
| Random (prettified) | 1,868 | 1,431 | 1.30× | Win |
| Twitter (minified) | 2,129 | 1,495 | 1.42× | Win |
| Twitter (prettified) | 2,591 | 1,905 | 1.36× | Win |
| Apache Builds Small (minified) | 1,845 | 1,397 | 1.32× | Win |
| Canada Small (minified) | 724 | 663 | 1.09× | Win |
| Apache Builds Small (prettified) | 2,126 | 1,707 | 1.25× | Win |
| Canada Small (prettified) | 1,641 | 1,541 | 1.06× | Win |
| CitmCatalog Small (minified) | 1,338 | 1,103 | 1.21× | Win |
| CitmCatalog Small (prettified) | 2,156 | 1,760 | 1.23× | Win |
| Discord Small (minified) | 1,814 | 1,322 | 1.37× | Win |
| Discord Small (prettified) | 2,264 | 1,717 | 1.32× | Win |
| Github Events Small (minified) | 1,725 | 1,335 | 1.29× | Win |
| Google Maps Response Small (minified) | 1,427 | 1,198 | 1.19× | Win |
| Github Events Small (prettified) | 2,061 | 1,674 | 1.23× | Win |
| Google Maps Response Small (prettified) | 2,475 | 2,159 | 1.15× | Win |
| Instruments Small (minified) | 1,867 | 1,411 | 1.32× | Win |
| Instruments Small (prettified) | 2,662 | 2,190 | 1.22× | Win |
| Marine IK Reverse Small (minified) | 641 | 640 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 2,216 | 2,122 | 1.04× | Win |
| Marine IK Small (minified) | 697 | 650 | 1.07× | Win |
| Marine IK Small (prettified) | 2,314 | 2,073 | 1.12× | Win |
| Random Small (minified) | 1,414 | 1,032 | 1.37× | Win |
| Random Small (prettified) | 1,365 | 1,548 | 0.88× | **Loss** |
| Twitter Small (minified) | 2,182 | 1,582 | 1.38× | Win |
| Twitter Small (prettified) | 2,571 | 1,953 | 1.32× | Win |

In the freshly allocated run, two-stage Jsonifier wins 70 of these 88 tests and loses 18, by up to 11% (prettified Github Events on Linux/GCC, 1,781 against 1,993 MB/s). Twelve of the losses are on Linux/GCC: both forms of Apache Builds, Canada, Github Events and Random, prettified Twitter, both forms of Github Events Small and prettified Google Maps Response Small. The other six are on macOS/GCC: both forms of Canada, Marine IK and Marine IK Reverse (0.94× to 0.99×). With the fused path instead, Jsonifier beats the reflection reader on 86 and loses 2. With objects reused, two-stage Jsonifier wins 73, ties 2 and loses 13, six on Linux/GCC and seven on macOS/GCC, by up to 12% (prettified Random Small on macOS/GCC, 1,365 against 1,548 MB/s); the ties are prettified Github Events Small on Linux/GCC and minified Marine IK Reverse Small on macOS/GCC. The fused path beats the reflection reader on 85, ties 1 and loses 2. We have not profiled these losses. Verdicts are Welch's t-tests, not ratio thresholds, so a row near 1.00× can go either way: reused prettified Github Events on Linux/GCC (2,308 against 2,315 MB/s, 1.00×) is a loss, while reused prettified Github Events Small on the same build (2,423 against 2,433) is a tie.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 4% on 33 of the 40 results; the widest gap is freshly allocated minified Twitter Small on macOS/Clang, where the two-stage row runs at 2,981 MB/s against the "jsonifier" row's 3,500. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 7,049 | 4,184 | 1.68× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 8,081 | 5,254 | 1.54× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,311 | 3,958 | 1.34× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,141 | 4,921 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,135 | 6,954 | 1.17× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,325 | 8,287 | 1.13× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,628 | 5,587 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,480 | 6,786 | 0.95× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,498 | 7,061 | 1.20× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,451 | 8,292 | 1.14× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,928 | 5,599 | 1.06× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,816 | 6,717 | 1.01× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,055 | 3,855 | 1.31× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,300 | 4,443 | 1.19× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,087 | 3,733 | 1.09× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,498 | 4,343 | 1.04× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,564 | 2,909 | 1.57× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,036 | 3,933 | 1.03× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 2,981 | 3,334 | 0.89× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,120 | 4,060 | 1.01× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (minified) | 7,116 | 4,435 | 1.60× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter (prettified) | 8,335 | 5,400 | 1.54× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,756 | 4,339 | 1.33× | Win |
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,583 | 5,311 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,277 | 7,091 | 1.17× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,622 | 8,434 | 1.14× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,877 | 5,778 | 1.02× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,627 | 6,933 | 0.96× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,606 | 7,170 | 1.20× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,722 | 8,483 | 1.15× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,122 | 5,757 | 1.06× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,107 | 7,017 | 1.01× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 5,143 | 3,926 | 1.31× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,416 | 4,607 | 1.18× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,282 | 3,933 | 1.09× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,678 | 4,496 | 1.04× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,009 | 3,035 | 1.32× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,197 | 3,558 | 1.18× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,891 | 2,991 | 1.30× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,320 | 3,458 | 1.25× | Win |

Jsonifier wins 37 of the 40 results and loses 3, and wins by up to 1.68× (minified Twitter under MSVC, freshly allocated). Freshly allocated it wins 18 and loses 2: prettified Twitter Small on Linux/Clang (0.95×) and minified Twitter Small on macOS/Clang (0.89×). Reused it wins 19 and loses 1, prettified Twitter Small on Linux/Clang (0.96×). Every full-size result is a win, by 1.13× to 1.68× on the three x86 builds and 1.03× to 1.57× on the M2 Pro. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds, by 1.09× to 1.27×:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (i9-14900KF, AVX2) | 2,961 | 2,559 | 1.16× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,224 | 2,615 | 1.23× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,658 | 2,194 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 2,228 | 2,040 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 1,931 | 1,524 | 1.27× | Win |

The narrowest win is on macOS/GCC, 1.09× (2,228 against 2,040 MB/s), and the widest on macOS/Clang, 1.27× (1,931 against 1,524 MB/s).

### 2.8 AVX-512 builds

Every result above comes from AVX2 and NEON builds. The same tests also ran on three AVX-512 builds, Linux / Clang 24.0, Linux / GCC 16.2 and Windows / MSVC 19.52, on cloud-hosted CI runners with an AMD EPYC 9V45 (Zen 5) processor, from the Json-Performance branch `benchmarking-avx512-main` at the same commits as §2.1, on November 23, 2026. Jsonifier and both of its paths run their AVX-512 kernels, and simdjson runs its `icelake` kernel. Sampling, convergence limits (RSE under 5%, mean shift under 2.5%) and verdicts are as in §2.1. The Linux / GCC build also runs the reflection reader of §2.5. The runners are shared virtual machines, and the CPU and operating-system images differ from the AVX2 builds, so a result here cannot be set against the matching AVX2 result to isolate the instruction set; every comparison below is within one build.

- **The routing split holds.** Freshly allocated, across the 132 corpus tests the fused path wins 109, 6 are ties and the tape wins 17; reused, 108, 6 and 18. Freshly allocated, the fused path wins 64 of the 66 minified documents; minified Canada under MSVC is a tie and the tape takes Apache Builds Small under MSVC (0.98×). On prettified input the fused path wins 45 of 66, with 5 ties, by up to 1.18× (Github Events on Linux / GCC) on the full-size documents and 1.49× (Marine IK Reverse Small under MSVC) on the Small ones. The tape's 16 prettified wins sit in the Marine IK family and the number-heavy Canada and CitmCatalog documents, plus Instruments Small on Linux / GCC.
- **Against simdjson, the faster Jsonifier path wins 145 of 147 freshly allocated and 144 of 147 reused.** All of its non-wins are under MSVC except one: freshly allocated, prettified Twitter Small (0.98×) is the one loss and prettified Apache Builds a tie; reused, prettified Apache Builds (0.95×) and Apache Builds Small (0.97×) are losses and prettified Canada on Linux / Clang is a tie. The fused path alone has the same record in both runs.
- **Stage 1 + reflection on Linux / GCC.** Against simdjson's reflection reader, two-stage Jsonifier wins 30 of the 44 corpus tests freshly allocated, ties 1 (prettified Twitter Small) and loses 13, by up to 15% (minified Github Events Small, 0.85×). Reused it wins 33 and loses 11, by up to 9% (prettified Github Events Small, 0.91×). With the fused path instead, Jsonifier beats the reflection reader on 41 freshly allocated and all 44 reused.
- **Reverse key order costs simdjson most under MSVC.** On this build simdjson parses minified Marine IK Reverse at 125 MB/s (n/c) against 217 MB/s in declared order, and the faster Jsonifier path is 5.10× faster freshly allocated (5.77× reused); on the prettified form it is 4.55× faster (4.53× reused). On both Linux builds simdjson reads the full-size reversed document within 4% of the declared one, and the faster Jsonifier path is 1.39× (Linux / Clang) and 1.51× (Linux / GCC) faster on the minified form, and 1.25× and 1.28× on the prettified form.
- **Partial reading.** Freshly allocated, Jsonifier wins all four Twitter Partial results under MSVC (2.04× to 2.43× full-size, 1.61× to 2.52× Small) and all four on Linux / Clang (1.25× to 1.35× full-size, 1.06× to 1.10× Small); on Linux / GCC it wins both full-size results (1.02× and 1.05×) and loses both Small copies (0.90×), for 10 wins and 2 losses. Reused it wins every result except prettified Twitter Small on Linux / GCC (0.96×): 2.06× to 2.25× full-size under MSVC, 1.28× to 1.37× on Linux / Clang and 1.04× to 1.05× on Linux / GCC, for 11 wins and 1 loss.
- **Validation.** Jsonifier wins on all three builds, by 1.27× under MSVC, 1.56× on Linux / Clang and 1.27× on Linux / GCC.

#### Freshly allocated objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 34 | 6 | 4 | 42 of 44 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 37 | 0 | 7 | 44 of 44 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 38 | 0 | 6 | 43 of 44 |
| **Aggregate** | **109** | **6** | **17** | **129 of 132** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 47 / 1 / 1 | 47 / 1 / 1 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **145 / 1 / 1** | **145 / 1 / 1** |

**Windows / MSVC 19.52 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 521 | n/a | 155 | n/a | n/a | Win |
| Double (POD) | 844 | n/a | 279 | n/a | n/a | Win |
| Int64 (POD) | 876 | n/a | 277 | n/a | n/a | Win |
| String (POD) | 584 | n/a | 389 | n/a | n/a | Win |
| Uint64 (POD) | 796 | n/a | 259 | n/a | n/a | Win |
| Apache Builds (minified) | 825 | 734 | 734 | 1.12× | Fused | Win |
| Canada (minified) | 570 | 558 | 204 | 1.02× | Tie | Win |
| Apache Builds (prettified) | 982 | 962 | 967 | 1.02× | Tie | Tie |
| Canada (prettified) | 1,234 | 1,253 | 449 | 0.98× | Tie | Win |
| CitmCatalog (minified) | 1,411 | 1,171 | 607 | 1.20× | Fused | Win |
| CitmCatalog (prettified) | 2,433 | 2,411 | 1,379 | 1.01× | Fused | Win |
| Discord (minified) | 1,290 | 973 | 801 | 1.33× | Fused | Win |
| Discord (prettified) | 1,652 | 1,451 | 1,161 | 1.14× | Fused | Win |
| Github Events (minified) | 1,074 | 998 | 673 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 1,207 | 1,122 | 616 | 1.08× | Fused | Win |
| Github Events (prettified) | 1,220 | 1,196 | 796 | 1.02× | Fused | Win |
| Google Maps Response (prettified) | 2,151 | 2,038 | 1,303 | 1.06× | Fused | Win |
| Instruments (minified) | 1,476 | 1,160 | 790 | 1.27× | Fused | Win |
| Instruments (prettified) | 2,050 | 1,795 | 1,343 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 638 | 590 | 125 (n/c) | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 2,040 | 2,171 (n/c) | 477 | 0.94× | Two-stage | Win |
| Marine IK (minified) | 666 | 616 | 217 | 1.08× | Fused | Win |
| Marine IK (prettified) | 2,229 | 2,150 | 861 | 1.04× | Fused | Win |
| Random (minified) | 844 | 701 | 530 | 1.20× | Fused | Win |
| Random (prettified) | 1,140 | 1,101 | 764 | 1.04× | Fused | Win |
| Twitter (minified) | 1,375 | 1,091 | 991 | 1.26× | Fused | Win |
| Twitter (prettified) | 1,544 | 1,454 | 1,300 | 1.06× | Fused | Win |
| Apache Builds Small (minified) | 922 | 945 | 731 | 0.98× | Two-stage | Win |
| Canada Small (minified) | 553 | 492 | 171 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 1,176 | 1,173 | 926 | 1.00× | Tie | Win |
| Canada Small (prettified) | 1,232 | 1,238 | 439 | 1.00× | Two-stage | Win |
| CitmCatalog Small (minified) | 742 | 697 | 454 | 1.06× | Fused | Win |
| CitmCatalog Small (prettified) | 1,205 | 1,206 | 784 | 1.00× | Tie | Win |
| Discord Small (minified) | 1,140 | 956 | 677 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,314 | 1,276 | 933 | 1.03× | Fused | Win |
| Github Events Small (minified) | 968 | 861 | 603 | 1.12× | Fused | Win |
| Google Maps Response Small (minified) | 1,077 | 974 | 608 | 1.11× | Fused | Win |
| Github Events Small (prettified) | 1,070 | 1,081 | 793 | 0.99× | Tie | Win |
| Google Maps Response Small (prettified) | 1,971 | 1,930 | 1,198 | 1.02× | Fused | Win |
| Instruments Small (minified) | 1,506 | 1,083 | 841 | 1.39× | Fused | Win |
| Instruments Small (prettified) | 1,951 | 1,778 | 1,391 | 1.10× | Fused | Win |
| Marine IK Reverse Small (minified) | 431 | 423 | 103 | 1.02× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,398 | 936 | 399 | 1.49× | Fused | Win |
| Marine IK Small (minified) | 491 | 439 | 197 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,466 | 1,582 | 745 | 0.93× | Two-stage | Win |
| Random Small (minified) | 894 | 804 | 515 | 1.11× | Fused | Win |
| Random Small (prettified) | 1,248 | 1,181 | 783 | 1.06× | Fused | Win |
| Twitter Small (minified) | 1,688 | 1,301 | 1,371 | 1.30× | Fused | Win |
| Twitter Small (prettified) | 1,706 | 1,656 | 1,737 | 1.03× | Fused | Loss |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,742 | n/a | 356 | n/a | n/a | Win |
| Double (POD) | 1,174 | n/a | 398 | n/a | n/a | Win |
| Int64 (POD) | 1,283 | n/a | 323 | n/a | n/a | Win |
| String (POD) | 1,251 | n/a | 725 | n/a | n/a | Win |
| Uint64 (POD) | 1,408 | n/a | 281 | n/a | n/a | Win |
| Apache Builds (minified) | 1,548 | 1,360 | 1,010 | 1.14× | Fused | Win |
| Canada (minified) | 750 | 664 | 677 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,814 | 1,604 | 1,286 | 1.13× | Fused | Win |
| Canada (prettified) | 1,508 | 1,553 | 1,459 | 0.97× | Two-stage | Win |
| CitmCatalog (minified) | 1,962 | 1,661 | 980 | 1.18× | Fused | Win |
| CitmCatalog (prettified) | 3,287 | 3,421 | 2,101 | 0.96× | Two-stage | Win |
| Discord (minified) | 2,041 | 1,668 | 723 | 1.22× | Fused | Win |
| Discord (prettified) | 2,574 | 2,354 | 1,035 | 1.09× | Fused | Win |
| Github Events (minified) | 1,649 | 1,393 | 872 | 1.18× | Fused | Win |
| Google Maps Response (minified) | 2,045 | 1,670 | 916 | 1.22× | Fused | Win |
| Github Events (prettified) | 1,914 | 1,724 | 1,092 | 1.11× | Fused | Win |
| Google Maps Response (prettified) | 3,159 | 2,926 | 1,822 | 1.08× | Fused | Win |
| Instruments (minified) | 2,483 | 2,174 | 583 | 1.14× | Fused | Win |
| Instruments (prettified) | 3,305 | 3,267 | 975 | 1.01× | Fused | Win |
| Marine IK Reverse (minified) | 781 | 695 | 560 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,421 | 2,307 | 1,929 | 1.05× | Fused | Win |
| Marine IK (minified) | 771 | 694 | 543 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,413 | 2,450 | 1,935 | 0.98× | Two-stage | Win |
| Random (minified) | 1,329 | 1,111 | 715 | 1.20× | Fused | Win |
| Random (prettified) | 1,830 | 1,606 | 1,077 | 1.14× | Fused | Win |
| Twitter (minified) | 1,681 | 1,524 | 810 | 1.10× | Fused | Win |
| Twitter (prettified) | 2,106 | 1,908 | 1,092 | 1.10× | Fused | Win |
| Apache Builds Small (minified) | 2,533 | 2,139 | 1,315 | 1.18× | Fused | Win |
| Canada Small (minified) | 831 | 730 | 684 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,738 | 2,463 | 1,640 | 1.11× | Fused | Win |
| Canada Small (prettified) | 1,663 | 1,717 | 1,569 | 0.97× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,610 | 1,340 | 910 | 1.20× | Fused | Win |
| CitmCatalog Small (prettified) | 2,158 | 2,329 | 1,588 | 0.93× | Two-stage | Win |
| Discord Small (minified) | 1,617 | 1,544 | 696 | 1.05× | Fused | Win |
| Discord Small (prettified) | 2,204 | 2,017 | 927 | 1.09× | Fused | Win |
| Github Events Small (minified) | 1,989 | 1,660 | 1,216 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 1,968 | 1,584 | 917 | 1.24× | Fused | Win |
| Github Events Small (prettified) | 2,258 | 2,105 | 1,584 | 1.07× | Fused | Win |
| Google Maps Response Small (prettified) | 3,032 | 2,846 | 1,824 | 1.07× | Fused | Win |
| Instruments Small (minified) | 2,269 | 1,956 | 545 | 1.16× | Fused | Win |
| Instruments Small (prettified) | 3,090 | 3,013 | 901 | 1.03× | Fused | Win |
| Marine IK Reverse Small (minified) | 771 | 660 | 464 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,099 | 2,381 | 1,712 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 839 | 750 | 485 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 2,211 | 2,463 | 1,728 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,929 | 1,568 | 789 | 1.23× | Fused | Win |
| Random Small (prettified) | 2,375 | 2,178 | 1,197 | 1.09× | Fused | Win |
| Twitter Small (minified) | 2,215 | 1,857 | 919 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 2,636 | 2,327 | 1,205 | 1.13× | Fused | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,342 | n/a | 538 | n/a | n/a | Win |
| Double (POD) | 1,426 | n/a | 418 | n/a | n/a | Win |
| Int64 (POD) | 1,775 | n/a | 340 | n/a | n/a | Win |
| String (POD) | 1,430 | n/a | 744 | n/a | n/a | Win |
| Uint64 (POD) | 1,658 | n/a | 325 | n/a | n/a | Win |
| Apache Builds (minified) | 1,627 | 1,440 | 1,372 | 1.13× | Fused | Win |
| Canada (minified) | 883 | 798 | 735 | 1.11× | Fused | Win |
| Apache Builds (prettified) | 1,903 | 1,762 | 1,744 | 1.08× | Fused | Win |
| Canada (prettified) | 1,814 | 1,771 | 1,644 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,154 | 1,890 | 1,626 | 1.14× | Fused | Win |
| CitmCatalog (prettified) | 3,633 | 3,570 | 3,334 | 1.02× | Fused | Win |
| Discord (minified) | 2,251 | 1,834 | 1,510 | 1.23× | Fused | Win |
| Discord (prettified) | 2,760 | 2,662 | 2,144 | 1.04× | Fused | Win |
| Github Events (minified) | 1,969 | 1,630 | 1,290 | 1.21× | Fused | Win |
| Google Maps Response (minified) | 1,874 | 1,599 | 1,282 | 1.17× | Fused | Win |
| Github Events (prettified) | 2,231 | 1,887 | 1,521 | 1.18× | Fused | Win |
| Google Maps Response (prettified) | 3,210 | 2,927 | 2,524 | 1.10× | Fused | Win |
| Instruments (minified) | 2,942 | 2,508 | 1,292 | 1.17× | Fused | Win |
| Instruments (prettified) | 3,876 | 3,810 | 2,104 | 1.02× | Fused | Win |
| Marine IK Reverse (minified) | 872 | 774 | 577 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 2,581 | 2,650 (n/c) | 2,076 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 878 | 775 | 591 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,718 | 2,731 | 2,109 | 1.00× | Two-stage | Win |
| Random (minified) | 1,348 | 1,183 | 1,127 | 1.14× | Fused | Win |
| Random (prettified) | 1,904 | 1,771 | 1,711 | 1.08× | Fused | Win |
| Twitter (minified) | 2,181 | 1,706 | 1,074 | 1.28× | Fused | Win |
| Twitter (prettified) | 2,334 | 2,179 | 1,376 | 1.07× | Fused | Win |
| Apache Builds Small (minified) | 2,377 | 2,081 | 1,852 | 1.14× | Fused | Win |
| Canada Small (minified) | 978 | 861 | 744 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,669 | 2,553 | 2,143 | 1.05× | Fused | Win |
| Canada Small (prettified) | 2,046 | 1,966 | 1,734 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 1,889 | 1,631 | 1,394 | 1.16× | Fused | Win |
| CitmCatalog Small (prettified) | 2,595 | 2,697 | 2,374 | 0.96× | Two-stage | Win |
| Discord Small (minified) | 2,081 | 1,713 | 1,399 | 1.22× | Fused | Win |
| Discord Small (prettified) | 2,301 | 2,270 | 1,839 | 1.01× | Fused | Win |
| Github Events Small (minified) | 2,023 | 1,590 | 1,294 | 1.27× | Fused | Win |
| Google Maps Response Small (minified) | 1,855 | 1,620 | 1,286 | 1.14× | Fused | Win |
| Github Events Small (prettified) | 2,226 | 2,115 | 1,649 | 1.05× | Fused | Win |
| Google Maps Response Small (prettified) | 3,220 | 2,960 | 2,613 | 1.09× | Fused | Win |
| Instruments Small (minified) | 3,375 | 2,417 | 1,093 | 1.40× | Fused | Win |
| Instruments Small (prettified) | 3,505 | 3,570 | 1,692 | 0.98× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 908 | 746 | 505 | 1.22× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,428 | 2,673 | 1,884 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 914 | 780 | 514 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,582 | 2,645 | 1,845 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,917 | 1,637 | 1,293 | 1.17× | Fused | Win |
| Random Small (prettified) | 2,507 | 2,267 | 1,875 | 1.11× | Fused | Win |
| Twitter Small (minified) | 2,866 | 1,974 | 1,190 | 1.45× | Fused | Win |
| Twitter Small (prettified) | 2,796 | 2,403 | 1,431 | 1.16× | Fused | Win |

#### Reused objects

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 33 | 6 | 5 | 42 of 44 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 36 | 0 | 8 | 44 of 44 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 39 | 0 | 5 | 43 of 44 |
| **Aggregate** | **108** | **6** | **18** | **129 of 132** |

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 47 / 0 / 2 | 47 / 0 / 2 |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 48 / 1 / 0 | 48 / 1 / 0 |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 49 / 0 / 0 | 49 / 0 / 0 |
| **Aggregate** | **144 / 1 / 2** | **144 / 1 / 2** |

**Windows / MSVC 19.52 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 595 | n/a | 159 | n/a | n/a | Win |
| Double (POD) | 1,142 | n/a | 328 | n/a | n/a | Win |
| Int64 (POD) | 1,470 | n/a | 317 | n/a | n/a | Win |
| String (POD) | 929 | n/a | 503 | n/a | n/a | Win |
| Uint64 (POD) | 1,335 | n/a | 297 | n/a | n/a | Win |
| Apache Builds (minified) | 1,213 | 1,095 | 1,153 | 1.11× | Fused | Win |
| Canada (minified) | 750 (n/c) | 642 | 210 | 1.17× | Fused | Win |
| Apache Builds (prettified) | 1,408 | 1,363 | 1,484 | 1.03× | Fused | Loss |
| Canada (prettified) | 1,470 | 1,451 | 500 | 1.01× | Tie | Win |
| CitmCatalog (minified) | 1,578 | 1,294 | 611 | 1.22× | Fused | Win |
| CitmCatalog (prettified) | 2,852 | 2,727 | 1,321 | 1.05× | Fused | Win |
| Discord (minified) | 1,844 | 1,309 | 1,056 | 1.41× | Fused | Win |
| Discord (prettified) | 2,276 | 1,730 | 1,513 | 1.32× | Fused | Win |
| Github Events (minified) | 1,638 | 1,345 | 851 | 1.22× | Fused | Win |
| Google Maps Response (minified) | 748 | 1,242 | 675 | 0.60× | Two-stage | Win |
| Github Events (prettified) | 1,744 | 1,597 | 1,004 | 1.09× | Fused | Win |
| Google Maps Response (prettified) | 2,404 | 2,266 | 1,409 | 1.06× | Fused | Win |
| Instruments (minified) | 1,756 | 1,160 | 918 | 1.51× | Fused | Win |
| Instruments (prettified) | 2,267 | 1,983 | 1,519 | 1.14× | Fused | Win |
| Marine IK Reverse (minified) | 735 | 670 | 127 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,329 | 2,346 | 518 | 0.99× | Tie | Win |
| Marine IK (minified) | 745 | 720 | 224 | 1.03× | Fused | Win |
| Marine IK (prettified) | 2,370 | 2,396 | 898 (n/c) | 0.99× | Tie | Win |
| Random (minified) | 1,124 | 933 | 609 | 1.20× | Fused | Win |
| Random (prettified) | 1,615 | 1,419 | 972 | 1.14× | Fused | Win |
| Twitter (minified) | 2,005 | 1,424 | 1,269 | 1.41× | Fused | Win |
| Twitter (prettified) | 2,064 | 1,990 | 1,566 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,264 | 1,262 | 1,238 | 1.00× | Tie | Win |
| Canada Small (minified) | 690 | 609 | 191 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,459 | 1,515 | 1,564 | 0.96× | Two-stage | Loss |
| Canada Small (prettified) | 1,461 | 1,486 | 464 | 0.98× | Tie | Win |
| CitmCatalog Small (minified) | 1,022 | 892 | 517 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 1,413 | 1,524 | 923 | 0.93× | Two-stage | Win |
| Discord Small (minified) | 1,784 | 1,316 | 1,036 | 1.36× | Fused | Win |
| Discord Small (prettified) | 1,957 | 1,756 | 1,437 | 1.11× | Fused | Win |
| Github Events Small (minified) | 1,278 | 1,100 | 768 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 1,265 | 1,118 | 691 | 1.13× | Fused | Win |
| Github Events Small (prettified) | 1,376 | 1,387 | 931 | 0.99× | Tie | Win |
| Google Maps Response Small (prettified) | 2,240 | 2,129 | 1,414 | 1.05× | Fused | Win |
| Instruments Small (minified) | 1,550 | 1,162 | 874 | 1.33× | Fused | Win |
| Instruments Small (prettified) | 2,100 | 1,900 | 1,433 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 574 | 536 | 109 | 1.07× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,686 | 1,886 | 423 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 677 | 584 | 205 | 1.16× | Fused | Win |
| Marine IK Small (prettified) | 1,893 | 2,160 | 803 | 0.88× | Two-stage | Win |
| Random Small (minified) | 1,102 | 958 | 617 | 1.15× | Fused | Win |
| Random Small (prettified) | 1,574 | 1,427 | 880 | 1.10× | Fused | Win |
| Twitter Small (minified) | 2,117 | 1,561 | 1,431 | 1.36× | Fused | Win |
| Twitter Small (prettified) | 1,999 | 1,925 | 1,865 | 1.04× | Fused | Win |

**Linux / Clang 24.0 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,729 | n/a | 387 | n/a | n/a | Win |
| Double (POD) | 1,278 | n/a | 406 | n/a | n/a | Win |
| Int64 (POD) | 1,386 | n/a | 326 | n/a | n/a | Win |
| String (POD) | 1,767 | n/a | 845 | n/a | n/a | Win |
| Uint64 (POD) | 1,569 | n/a | 291 | n/a | n/a | Win |
| Apache Builds (minified) | 2,302 | 1,887 | 1,289 | 1.22× | Fused | Win |
| Canada (minified) | 898 | 829 | 803 | 1.08× | Fused | Win |
| Apache Builds (prettified) | 2,557 | 2,134 | 1,620 | 1.20× | Fused | Win |
| Canada (prettified) | 1,767 | 1,792 | 1,777 | 0.99× | Two-stage | Tie |
| CitmCatalog (minified) | 2,465 | 2,019 | 1,104 | 1.22× | Fused | Win |
| CitmCatalog (prettified) | 3,852 | 3,929 | 2,294 | 0.98× | Two-stage | Win |
| Discord (minified) | 2,743 | 2,144 | 845 | 1.28× | Fused | Win |
| Discord (prettified) | 3,315 | 2,896 | 1,185 | 1.14× | Fused | Win |
| Github Events (minified) | 2,242 | 1,827 | 1,001 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,246 | 1,861 | 929 | 1.21× | Fused | Win |
| Github Events (prettified) | 2,664 | 2,233 | 1,225 | 1.19× | Fused | Win |
| Google Maps Response (prettified) | 3,471 | 3,211 | 1,885 | 1.08× | Fused | Win |
| Instruments (minified) | 2,816 | 2,455 | 605 | 1.15× | Fused | Win |
| Instruments (prettified) | 3,709 | 3,586 | 1,000 | 1.03× | Fused | Win |
| Marine IK Reverse (minified) | 871 | 791 | 616 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,559 | 2,590 | 2,164 | 0.99× | Two-stage | Win |
| Marine IK (minified) | 843 | 783 | 618 | 1.08× | Fused | Win |
| Marine IK (prettified) | 2,606 | 2,657 | 2,176 | 0.98× | Two-stage | Win |
| Random (minified) | 1,886 | 1,520 | 898 | 1.24× | Fused | Win |
| Random (prettified) | 2,534 | 2,187 | 1,352 | 1.16× | Fused | Win |
| Twitter (minified) | 2,113 | 1,869 | 902 | 1.13× | Fused | Win |
| Twitter (prettified) | 2,627 | 2,319 | 1,194 | 1.13× | Fused | Win |
| Apache Builds Small (minified) | 2,920 | 2,524 | 1,525 | 1.16× | Fused | Win |
| Canada Small (minified) | 963 | 856 | 739 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 3,059 | 2,809 | 1,936 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,910 | 2,012 | 1,735 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,107 | 1,701 | 1,035 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 2,518 | 2,735 | 1,772 | 0.92× | Two-stage | Win |
| Discord Small (minified) | 2,115 | 1,940 | 773 | 1.09× | Fused | Win |
| Discord Small (prettified) | 2,932 | 2,536 | 1,028 | 1.16× | Fused | Win |
| Github Events Small (minified) | 2,532 | 2,206 | 1,494 | 1.15× | Fused | Win |
| Google Maps Response Small (minified) | 2,219 | 1,747 | 968 | 1.27× | Fused | Win |
| Github Events Small (prettified) | 2,944 | 2,731 | 1,856 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 3,356 | 3,132 | 1,909 | 1.07× | Fused | Win |
| Instruments Small (minified) | 2,547 | 2,076 | 559 | 1.23× | Fused | Win |
| Instruments Small (prettified) | 3,300 | 3,162 | 941 | 1.04× | Fused | Win |
| Marine IK Reverse Small (minified) | 963 | 816 | 505 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,460 | 2,814 | 1,806 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 1,022 | 875 | 511 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,522 | 2,929 | 1,798 | 0.86× | Two-stage | Win |
| Random Small (minified) | 2,425 | 1,826 | 881 | 1.33× | Fused | Win |
| Random Small (prettified) | 2,869 | 2,619 | 1,371 | 1.10× | Fused | Win |
| Twitter Small (minified) | 2,503 | 1,990 | 955 | 1.26× | Fused | Win |
| Twitter Small (prettified) | 2,935 | 2,591 | 1,246 | 1.13× | Fused | Win |

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,328 | n/a | 541 | n/a | n/a | Win |
| Double (POD) | 1,571 | n/a | 398 | n/a | n/a | Win |
| Int64 (POD) | 1,995 | n/a | 361 | n/a | n/a | Win |
| String (POD) | 1,923 | n/a | 867 | n/a | n/a | Win |
| Uint64 (POD) | 1,833 | n/a | 335 | n/a | n/a | Win |
| Apache Builds (minified) | 2,262 | 1,900 | 1,788 | 1.19× | Fused | Win |
| Canada (minified) | 1,043 | 923 | 859 | 1.13× | Fused | Win |
| Apache Builds (prettified) | 2,518 | 2,282 | 2,292 | 1.10× | Fused | Win |
| Canada (prettified) | 2,077 | 2,040 | 1,903 | 1.02× | Fused | Win |
| CitmCatalog (minified) | 2,557 | 2,216 | 1,867 | 1.15× | Fused | Win |
| CitmCatalog (prettified) | 4,080 | 4,384 | 3,889 | 0.93× | Two-stage | Win |
| Discord (minified) | 2,905 | 2,246 | 1,731 | 1.29× | Fused | Win |
| Discord (prettified) | 3,351 | 3,268 | 2,567 | 1.03× | Fused | Win |
| Github Events (minified) | 2,619 | 2,063 | 1,512 | 1.27× | Fused | Win |
| Google Maps Response (minified) | 2,060 | 1,694 | 1,349 | 1.22× | Fused | Win |
| Github Events (prettified) | 2,820 | 2,356 | 1,796 | 1.20× | Fused | Win |
| Google Maps Response (prettified) | 3,385 | 3,085 | 2,631 | 1.10× | Fused | Win |
| Instruments (minified) | 3,474 | 2,783 | 1,370 | 1.25× | Fused | Win |
| Instruments (prettified) | 4,265 | 4,188 | 2,218 | 1.02× | Fused | Win |
| Marine IK Reverse (minified) | 980 | 861 | 640 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 2,860 | 2,944 | 2,254 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 983 | 822 (n/c) | 651 | 1.20× | Fused | Win |
| Marine IK (prettified) | 2,986 | 2,932 | 2,280 | 1.02× | Fused | Win |
| Random (minified) | 1,847 | 1,507 | 1,470 | 1.23× | Fused | Win |
| Random (prettified) | 2,474 | 2,255 | 2,209 | 1.10× | Fused | Win |
| Twitter (minified) | 2,726 | 2,039 | 1,159 | 1.34× | Fused | Win |
| Twitter (prettified) | 2,861 | 2,484 | 1,533 | 1.15× | Fused | Win |
| Apache Builds Small (minified) | 2,698 | 2,355 | 2,232 | 1.15× | Fused | Win |
| Canada Small (minified) | 1,193 | 1,024 | 838 | 1.17× | Fused | Win |
| Apache Builds Small (prettified) | 3,072 | 2,742 | 2,577 | 1.12× | Fused | Win |
| Canada Small (prettified) | 2,400 | 2,305 | 1,908 | 1.04× | Fused | Win |
| CitmCatalog Small (minified) | 2,257 | 1,907 | 1,572 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 2,971 | 3,150 | 2,751 | 0.94× | Two-stage | Win |
| Discord Small (minified) | 2,726 | 2,053 | 1,666 | 1.33× | Fused | Win |
| Discord Small (prettified) | 2,908 | 2,731 | 2,117 | 1.06× | Fused | Win |
| Github Events Small (minified) | 2,626 | 2,192 | 1,530 | 1.20× | Fused | Win |
| Google Maps Response Small (minified) | 2,059 | 1,655 | 1,413 | 1.24× | Fused | Win |
| Github Events Small (prettified) | 3,023 | 2,578 | 1,885 | 1.17× | Fused | Win |
| Google Maps Response Small (prettified) | 3,409 | 3,221 | 2,757 | 1.06× | Fused | Win |
| Instruments Small (minified) | 3,859 | 2,622 | 1,166 | 1.47× | Fused | Win |
| Instruments Small (prettified) | 4,006 | 3,736 | 1,906 | 1.07× | Fused | Win |
| Marine IK Reverse Small (minified) | 1,032 | 881 | 551 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,844 | 2,968 | 2,077 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 1,119 | 916 | 552 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 2,931 | 3,074 | 2,017 | 0.95× | Two-stage | Win |
| Random Small (minified) | 2,343 | 1,915 | 1,582 | 1.22× | Fused | Win |
| Random Small (prettified) | 2,888 | 2,634 | 2,240 | 1.10× | Fused | Win |
| Twitter Small (minified) | 3,197 | 2,247 | 1,157 | 1.42× | Fused | Win |
| Twitter Small (prettified) | 3,019 | 2,556 | 1,511 | 1.18× | Fused | Win |

#### Freshly allocated objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,440 | 1,515 | 0.95× | **Loss** |
| Canada (minified) | 798 | 737 | 1.08× | Win |
| Apache Builds (prettified) | 1,762 | 1,936 | 0.91× | **Loss** |
| Canada (prettified) | 1,771 | 1,626 | 1.09× | Win |
| CitmCatalog (minified) | 1,890 | 1,631 | 1.16× | Win |
| CitmCatalog (prettified) | 3,570 | 3,328 | 1.07× | Win |
| Discord (minified) | 1,834 | 1,787 | 1.03× | Win |
| Discord (prettified) | 2,662 | 2,485 | 1.07× | Win |
| Github Events (minified) | 1,630 | 1,823 | 0.89× | **Loss** |
| Google Maps Response (minified) | 1,599 | 1,563 | 1.02× | Win |
| Github Events (prettified) | 1,887 | 2,199 | 0.86× | **Loss** |
| Google Maps Response (prettified) | 2,927 | 3,020 | 0.97× | **Loss** |
| Instruments (minified) | 2,508 | 1,749 | 1.43× | Win |
| Instruments (prettified) | 3,810 | 2,730 | 1.40× | Win |
| Marine IK Reverse (minified) | 774 | 696 | 1.11× | Win |
| Marine IK Reverse (prettified) | 2,650 (n/c) | 2,467 | 1.07× | Win |
| Marine IK (minified) | 775 | 704 | 1.10× | Win |
| Marine IK (prettified) | 2,731 | 2,484 | 1.10× | Win |
| Random (minified) | 1,183 | 1,209 | 0.98× | **Loss** |
| Random (prettified) | 1,771 | 1,828 | 0.97× | **Loss** |
| Twitter (minified) | 1,706 | 1,742 | 0.98× | **Loss** |
| Twitter (prettified) | 2,179 | 2,267 | 0.96× | **Loss** |
| Apache Builds Small (minified) | 2,081 | 2,073 | 1.00× | Win |
| Canada Small (minified) | 861 | 723 | 1.19× | Win |
| Apache Builds Small (prettified) | 2,553 | 2,475 | 1.03× | Win |
| Canada Small (prettified) | 1,966 | 1,701 | 1.16× | Win |
| CitmCatalog Small (minified) | 1,631 | 1,267 | 1.29× | Win |
| CitmCatalog Small (prettified) | 2,697 | 2,374 | 1.14× | Win |
| Discord Small (minified) | 1,713 | 1,724 | 0.99× | **Loss** |
| Discord Small (prettified) | 2,270 | 2,281 | 1.00× | **Loss** |
| Github Events Small (minified) | 1,590 | 1,870 | 0.85× | **Loss** |
| Google Maps Response Small (minified) | 1,620 | 1,509 | 1.07× | Win |
| Github Events Small (prettified) | 2,115 | 2,462 | 0.86× | **Loss** |
| Google Maps Response Small (prettified) | 2,960 | 2,951 | 1.00× | Win |
| Instruments Small (minified) | 2,417 | 1,580 | 1.53× | Win |
| Instruments Small (prettified) | 3,570 | 2,599 | 1.37× | Win |
| Marine IK Reverse Small (minified) | 746 | 674 | 1.11× | Win |
| Marine IK Reverse Small (prettified) | 2,673 | 2,454 | 1.09× | Win |
| Marine IK Small (minified) | 780 | 670 | 1.16× | Win |
| Marine IK Small (prettified) | 2,645 | 2,339 | 1.13× | Win |
| Random Small (minified) | 1,637 | 1,463 | 1.12× | Win |
| Random Small (prettified) | 2,267 | 2,106 | 1.08× | Win |
| Twitter Small (minified) | 1,974 | 1,922 | 1.03× | Win |
| Twitter Small (prettified) | 2,403 | 2,408 | 1.00× | Tie |

#### Reused objects: stage 1 + reflection

**Linux / GCC 16.2 (EPYC 9V45, AVX-512)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,900 | 1,921 | 0.99× | **Loss** |
| Canada (minified) | 923 | 860 | 1.07× | Win |
| Apache Builds (prettified) | 2,282 | 2,425 | 0.94× | **Loss** |
| Canada (prettified) | 2,040 | 1,892 | 1.08× | Win |
| CitmCatalog (minified) | 2,216 | 1,921 | 1.15× | Win |
| CitmCatalog (prettified) | 4,384 | 3,851 | 1.14× | Win |
| Discord (minified) | 2,246 | 2,020 | 1.11× | Win |
| Discord (prettified) | 3,268 | 2,867 | 1.14× | Win |
| Github Events (minified) | 2,063 | 2,112 | 0.98× | **Loss** |
| Google Maps Response (minified) | 1,694 | 1,620 | 1.05× | Win |
| Github Events (prettified) | 2,356 | 2,472 | 0.95× | **Loss** |
| Google Maps Response (prettified) | 3,085 | 3,131 | 0.99× | **Loss** |
| Instruments (minified) | 2,783 | 1,808 | 1.54× | Win |
| Instruments (prettified) | 4,188 | 2,664 | 1.57× | Win |
| Marine IK Reverse (minified) | 861 | 774 | 1.11× | Win |
| Marine IK Reverse (prettified) | 2,944 | 2,732 | 1.08× | Win |
| Marine IK (minified) | 822 (n/c) | 789 | 1.04× | Win |
| Marine IK (prettified) | 2,932 | 2,749 | 1.07× | Win |
| Random (minified) | 1,507 | 1,554 | 0.97× | **Loss** |
| Random (prettified) | 2,255 | 2,307 | 0.98× | **Loss** |
| Twitter (minified) | 2,039 | 2,049 | 0.99× | **Loss** |
| Twitter (prettified) | 2,484 | 2,599 | 0.96× | **Loss** |
| Apache Builds Small (minified) | 2,355 | 2,167 | 1.09× | Win |
| Canada Small (minified) | 1,024 | 722 | 1.42× | Win |
| Apache Builds Small (prettified) | 2,742 | 2,575 | 1.06× | Win |
| Canada Small (prettified) | 2,305 | 1,808 | 1.27× | Win |
| CitmCatalog Small (minified) | 1,907 | 1,469 | 1.30× | Win |
| CitmCatalog Small (prettified) | 3,150 | 2,548 | 1.24× | Win |
| Discord Small (minified) | 2,053 | 1,949 | 1.05× | Win |
| Discord Small (prettified) | 2,731 | 2,548 | 1.07× | Win |
| Github Events Small (minified) | 2,192 | 2,154 | 1.02× | Win |
| Google Maps Response Small (minified) | 1,655 | 1,634 | 1.01× | Win |
| Github Events Small (prettified) | 2,578 | 2,834 | 0.91× | **Loss** |
| Google Maps Response Small (prettified) | 3,221 | 3,104 | 1.04× | Win |
| Instruments Small (minified) | 2,622 | 1,734 | 1.51× | Win |
| Instruments Small (prettified) | 3,736 | 2,713 | 1.38× | Win |
| Marine IK Reverse Small (minified) | 881 | 709 | 1.24× | Win |
| Marine IK Reverse Small (prettified) | 2,968 | 2,645 | 1.12× | Win |
| Marine IK Small (minified) | 916 | 728 | 1.26× | Win |
| Marine IK Small (prettified) | 3,074 | 2,628 | 1.17× | Win |
| Random Small (minified) | 1,915 | 1,590 | 1.20× | Win |
| Random Small (prettified) | 2,634 | 2,195 | 1.20× | Win |
| Twitter Small (minified) | 2,247 | 2,155 | 1.04× | Win |
| Twitter Small (prettified) | 2,556 | 2,648 | 0.97× | **Loss** |

#### Freshly allocated objects: partial reading and validation

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,186 | 3,370 | 2.43× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (prettified) | 8,955 | 4,383 | 2.04× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,411 | 3,370 | 1.61× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 6,686 | 2,651 | 2.52× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,884 | 6,594 | 1.35× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,981 | 7,968 | 1.25× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,492 | 6,107 | 1.06× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,741 | 7,028 | 1.10× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,313 | 8,133 | 1.02× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,977 | 9,462 | 1.05× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 5,915 (n/c) | 6,597 | 0.90× | Loss |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,244 | 8,025 | 0.90× | Loss |

**Reused objects.**

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,161 | 3,627 | 2.25× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,374 | 4,561 | 2.06× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,450 | 3,459 | 1.86× | Win |
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,454 | 4,220 | 1.77× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (minified) | 9,160 | 6,687 | 1.37× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter (prettified) | 10,349 | 8,068 | 1.28× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 7,013 | 6,294 | 1.11× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 8,168 | 7,124 | 1.15× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (minified) | 8,659 | 8,303 | 1.04× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter (prettified) | 9,939 | 9,467 | 1.05× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (minified) | 6,510 | 6,348 | 1.03× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | Twitter Small (prettified) | 7,284 | 7,622 | 0.96× | Loss |

**Validation.**

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.52 (EPYC 9V45, AVX-512) | 3,172 | 2,499 | 1.27× | Win |
| Linux / Clang 24.0 (EPYC 9V45, AVX-512) | 3,609 | 2,312 | 1.56× | Win |
| Linux / GCC 16.2 (EPYC 9V45, AVX-512) | 3,004 | 2,366 | 1.27× | Win |

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
#if JSONIFIER_AVX512 && JSONIFIER_COMPILER_GCC
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
#elif JSONIFIER_AVX512 && JSONIFIER_COMPILER_MSVC
	inline constexpr uint64_t simdBlocksPerStep{ 2 };
#elif JSONIFIER_AVX512 && JSONIFIER_COMPILER_CLANG
	inline constexpr uint64_t simdBlocksPerStep{ 8 };
#elif JSONIFIER_NEON && JSONIFIER_COMPILER_GCC
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
	inline constexpr uint64_t simdTapeStep{ 4 };
#else
	inline constexpr uint64_t simdBlocksPerStep{ 4 };
	inline constexpr uint64_t simdTapeStep{ 2 };
#endif
```

`simd/simd_config.hpp` defines `simdBlocksPerStep` and `simdTapeStep` directly in one `#if` chain keyed on ISA and compiler together. On AVX-512 the step is 8 blocks under Clang, 4 under GCC and 2 under MSVC; NEON under GCC takes 4 blocks with a tape step of 4; every other target, AVX2 and NEON under Clang and MSVC included, shares the final branch at 4 blocks and a tape step of 2. The AVX-512 cells define no `simdTapeStep`, since their compress drain has no stepped scalar writer.

These values were selected by parameter sweeps across the CI targets. That Clang and GCC want *different* geometry on the identical ISA reflects how each compiler schedules the unrolled block bodies. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing 2 to 8 blocks (128 to 512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a window of independent block computations up to 4x wider than simdjson's fixed 128-byte step, and never narrower. Past the last full step the reader halves the batch (8, 4, 2, then 1 block) rather than dropping to a block-at-a-time loop.

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

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit: a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional: `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNext`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

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

The last row is the root of every other difference. simdjson's core API can ship one x86 binary that picks its instruction set at run time; Jsonifier deliberately does not. Its founding constraint is that apart from the data, nothing is decided at run time. The schema, the compiler, the parse options and the instruction-set tier are all known at build time, so every routing decision in this paper (tape or no tape, whitespace lane or not, 2, 4 or 8 blocks, tape step of 2 or 4) is still resolved before the first byte is read.

The tier is chosen when CMake configures the project, by a detector run on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS` (and capped with `JSONIFIER_SIMD_TIER`), and the whole library is compiled once for it, so every call is a direct, inlinable call with no per-call check. simdjson dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time. Jsonifier keeps one tier per build because per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, which GCC and Clang then reject. The cost is portability: a binary for mixed hardware is configured for the lowest tier it must support.

## 8. Conclusion

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 201 of the 220 freshly allocated corpus tests: all 110 minified documents and 91 of 110 prettified ones, while the two-stage path's 14 wins sit almost entirely in the float-heavy Marine IK family on x86. The fused path alone beats simdjson On Demand on all 245 tests in both runs, the stage-1 + reflection path beats simdjson's reflection reader on 70 of 88 freshly allocated and 73 of 88 reused, and on partial reading Jsonifier wins 37 of 40. On the three AVX-512 builds the faster path beats simdjson On Demand on 145 of 147 tests freshly allocated and 144 of 147 reused, and its stage-1 + reflection path beats simdjson's reflection reader on 30 of 44 freshly allocated and 33 of 44 reused. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier [4]. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance [5].*

## References

1. G. Langdale, D. Lemire. "Parsing Gigabytes of JSON per Second." *The VLDB Journal*, 28(6), 2019.
2. simdjson. https://github.com/simdjson/simdjson (commit 344898409).
3. Nihilai Collective Corp. "Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1." Nihilai Collective Corp, 2026.
4. Jsonifier repository. https://github.com/nihilai-collective/Jsonifier
5. Json-Performance benchmark suite and sweep data. https://github.com/nihilai-collective/Json-Performance
