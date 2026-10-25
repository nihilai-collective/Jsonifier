# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 245 tests on five builds, skipping the tape wins on POD-type data on every build and on 103 of the 110 minified documents, the tape is faster on 57 of the 66 prettified documents on x86, and on the Apple M2 Pro the fused path wins 34 of the 44 prettified documents.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 26, 2026, with Jsonifier [c4f0510](https://github.com/nihilai-collective/jsonifier/commit/c4f0510), Glaze [1476d6b](https://github.com/stephenberry/glaze/commit/1476d6b), simdjson [50f0739](https://github.com/simdjson/simdjson/commit/50f0739) and BenchmarkSuite [35ec835](https://github.com/nihilai-collective/benchmarksuite/commit/35ec835) on every build. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 49 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), eleven corpus documents in minified and prettified form, and "Small" cut-down copies of the same eleven documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M2 Pro), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Eighteen freshly allocated full-parse rows did not converge (fourteen under MSVC, two on macOS/GCC, two on macOS/Clang), and sixteen reused ones (thirteen under MSVC, one on macOS/GCC, two on macOS/Clang). Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 168 tests, 9 are statistical ties, and the two-stage path wins 68. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 29 | 4 | 16 | 35 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 1 | 24 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 40 | 2 | 7 | 47 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 45 | 2 | 2 | 47 of 49 |
| **Aggregate** | **168** | **9** | **68** | **227 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 1 / 0 | 19 / 1 / 2 | 21 / 1 / 0 |
| Prettified corpus documents | 0 / 2 / 9 | 3 / 0 / 19 | 17 / 1 / 4 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 20 / 0 / 2 |
| Small prettified documents | 3 / 1 / 7 | 0 / 0 / 22 | 17 / 2 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 48 / 0 / 1 | 47 / 0 / 2 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 46 / 1 / 2 | 38 / 1 / 10 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 47 / 0 / 2 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 48 / 1 / 0 | 47 / 2 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 47 / 0 / 2 | 47 / 0 / 2 |
| **Aggregate** | **237 / 2 / 6** | **226 / 3 / 16** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 29 | 0 | 20 | 36 of 49 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 30 | 0 | 19 | 49 of 49 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 2 | 23 | 49 of 49 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 42 | 2 | 5 | 48 of 49 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 44 | 1 | 4 | 47 of 49 |
| **Aggregate** | **169** | **5** | **71** | **229 of 245** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 11 / 0 / 0 | 19 / 1 / 2 | 22 / 0 / 0 |
| Prettified corpus documents | 0 / 0 / 11 | 3 / 1 / 18 | 16 / 3 / 3 |
| Small minified documents | 11 / 0 / 0 | 22 / 0 / 0 | 22 / 0 / 0 |
| Small prettified documents | 2 / 0 / 9 | 0 / 0 / 22 | 16 / 0 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 48 / 0 / 1 | 48 / 0 / 1 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 / 0 / 2 | 38 / 0 / 11 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 48 / 0 / 1 | 46 / 0 / 3 |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 49 / 0 / 0 | 49 / 0 / 0 |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 48 / 0 / 1 | 47 / 1 / 1 |
| **Aggregate** | **240 / 0 / 5** | **228 / 1 / 16** |

Across 245 tests the fused path wins 169, ties 5 and loses 71 to the two-stage path, and the x86 split holds: the tape wins 29 of the 33 full-size prettified documents (1 tie; the fused path takes Canada, Marine IK and Marine IK Reverse on Linux/Clang) and 31 of the 33 Small prettified ones (the fused path takes Apache Builds Small and Github Events Small under MSVC), and the fused path wins all 33 Small minified documents and 30 of the 33 full-size minified ones (the tape takes Canada and Random on Linux/GCC, and Marine IK Reverse is a tie there). On the M2 Pro the fused path wins all 44 minified documents and 32 of the 44 prettified ones, with 3 ties and 9 tape wins. Against simdjson, the faster Jsonifier path wins 240 and loses 5, and the fused path alone wins 228, ties 1 and loses 16.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 503 | 136 | 130 | 3.70× | Fused | Win |
| Double (POD) | 599 | 313 | 172 | 1.91× | Fused | Win |
| Int64 (POD) | 1,732 | 565 | 388 | 3.06× | Fused | Win |
| String (POD) | 1,027 | 643 | 643 | 1.60× | Fused | Win |
| Uint64 (POD) | 1,705 | 589 | 391 | 2.90× | Fused | Win |
| Apache Builds (minified) | 860 | 687 | 520 | 1.25× | Fused | Win |
| Canada (minified) | 613 | 549 | 361 | 1.12× | Fused | Win |
| Apache Builds (prettified) | 860 | 838 | 698 | 1.03× | Tie | Win |
| Canada (prettified) | 1,249 | 1,301 | 811 | 0.96× | Tie | Win |
| CitmCatalog (minified) | 1,800 | 1,339 | 466 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 2,155 | 2,408 | 1,061 (n/c) | 0.89× | Two-stage | Win |
| Discord (minified) | 1,098 | 1,091 | 559 | 1.01× | Tie | Win |
| Discord (prettified) | 1,289 | 1,526 | 813 (n/c) | 0.84× | Two-stage | Win |
| Github Events (minified) | 917 | 796 | 544 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,309 | 1,172 | 517 | 1.12× | Fused | Win |
| Github Events (prettified) | 905 | 1,075 | 698 | 0.84× | Two-stage | Win |
| Google Maps Response (prettified) | 2,095 | 2,383 (n/c) | 1,033 | 0.88× | Two-stage | Win |
| Instruments (minified) | 2,041 | 1,460 | 617 | 1.40× | Fused | Win |
| Instruments (prettified) | 1,843 | 2,337 | 1,031 | 0.79× | Two-stage | Win |
| Marine IK Reverse (minified) | 485 | 410 | 33 (n/c) | 1.18× | Fused | Win |
| Marine IK Reverse (prettified) | 1,393 | 1,600 (n/c) | 134 (n/c) | 0.87× | Two-stage | Win |
| Marine IK (minified) | 435 | 408 | 227 | 1.07× | Fused | Win |
| Marine IK (prettified) | 1,429 | 1,496 (n/c) | 868 | 0.96× | Two-stage | Win |
| Random (minified) | 701 | 640 | 519 (n/c) | 1.10× | Fused | Win |
| Random (prettified) | 875 | 987 | 761 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,066 | 893 | 657 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,016 | 1,183 | 935 | 0.86× | Two-stage | Win |
| Apache Builds Small (minified) | 1,398 | 1,074 | 743 | 1.30× | Fused | Win |
| Canada Small (minified) | 589 | 527 | 325 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 1,399 | 1,298 | 940 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,022 (n/c) | 1,261 | 796 | 0.81× | Two-stage | Win |
| CitmCatalog Small (minified) | 870 | 762 | 357 (n/c) | 1.14× | Fused | Win |
| CitmCatalog Small (prettified) | 1,151 | 1,334 | 879 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,117 | 893 | 570 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,163 | 1,108 | 839 | 1.05× | Tie | Win |
| Github Events Small (minified) | 1,175 | 859 | 1,000 | 1.37× | Fused | Win |
| Google Maps Response Small (minified) | 1,422 | 1,136 | 520 (n/c) | 1.25× | Fused | Win |
| Github Events Small (prettified) | 1,121 | 1,050 | 1,205 | 1.07× | Fused | Loss |
| Google Maps Response Small (prettified) | 1,912 (n/c) | 1,756 (n/c) | 1,091 | 1.09× | Fused | Win |
| Instruments Small (minified) | 2,148 | 1,401 | 692 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 1,718 | 2,377 | 1,135 | 0.72× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 457 | 395 | 34 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,212 (n/c) | 1,453 | 139 | 0.83× | Two-stage | Win |
| Marine IK Small (minified) | 464 | 414 | 268 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,406 | 1,573 | 999 | 0.89× | Two-stage | Win |
| Random Small (minified) | 890 | 818 (n/c) | 541 | 1.09× | Fused | Win |
| Random Small (prettified) | 1,116 (n/c) | 1,253 | 725 | 0.89× | Two-stage | Win |
| Twitter Small (minified) | 1,627 | 1,368 | 1,104 | 1.19× | Fused | Win |
| Twitter Small (prettified) | 1,438 | 1,772 | 1,463 | 0.81× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,922 | 326 | 234 | 5.89× | Fused | Win |
| Double (POD) | 1,317 | 405 | 263 | 3.26× | Fused | Win |
| Int64 (POD) | 3,167 | 905 | 593 | 3.50× | Fused | Win |
| String (POD) | 1,811 | 1,157 | 1,094 | 1.57× | Fused | Win |
| Uint64 (POD) | 3,408 | 925 | 561 | 3.69× | Fused | Win |
| Apache Builds (minified) | 1,760 | 1,550 | 1,228 | 1.14× | Fused | Win |
| Canada (minified) | 1,041 | 989 | 690 | 1.05× | Fused | Win |
| Apache Builds (prettified) | 1,732 | 1,956 | 1,572 | 0.89× | Two-stage | Win |
| Canada (prettified) | 2,127 | 2,055 | 1,570 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,430 | 1,809 | 1,231 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 3,143 | 3,592 | 2,580 | 0.87× | Two-stage | Win |
| Discord (minified) | 2,313 | 1,803 | 1,490 | 1.28× | Fused | Win |
| Discord (prettified) | 1,875 | 2,652 | 2,143 | 0.71× | Two-stage | Win |
| Github Events (minified) | 1,669 | 1,560 | 1,530 | 1.07× | Fused | Win |
| Google Maps Response (minified) | 2,540 | 1,963 | 1,358 | 1.29× | Fused | Win |
| Github Events (prettified) | 1,587 | 1,846 | 1,870 | 0.86× | Two-stage | Loss |
| Google Maps Response (prettified) | 3,114 | 3,729 | 2,715 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,627 | 1,994 | 1,726 | 1.32× | Fused | Win |
| Instruments (prettified) | 2,364 | 2,968 | 2,763 | 0.80× | Two-stage | Win |
| Marine IK Reverse (minified) | 818 | 744 | 596 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,634 | 2,479 | 2,164 | 1.06× | Fused | Win |
| Marine IK (minified) | 850 | 750 | 587 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,591 | 2,536 | 2,155 | 1.02× | Fused | Win |
| Random (minified) | 1,268 | 1,104 | 1,067 | 1.15× | Fused | Win |
| Random (prettified) | 1,457 | 1,663 | 1,604 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,911 | 1,692 | 1,695 | 1.13× | Fused | Win |
| Twitter (prettified) | 1,753 | 2,200 | 2,205 | 0.80× | Two-stage | Tie |
| Apache Builds Small (minified) | 2,634 | 2,242 | 1,513 | 1.18× | Fused | Win |
| Canada Small (minified) | 1,012 | 983 | 726 | 1.03× | Fused | Win |
| Apache Builds Small (prettified) | 2,259 | 2,629 | 1,904 | 0.86× | Two-stage | Win |
| Canada Small (prettified) | 2,030 | 2,119 | 1,673 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,748 | 1,446 | 1,211 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,986 | 2,455 | 2,113 | 0.81× | Two-stage | Win |
| Discord Small (minified) | 2,097 | 1,794 | 1,498 | 1.17× | Fused | Win |
| Discord Small (prettified) | 1,786 | 2,351 | 2,008 | 0.76× | Two-stage | Win |
| Github Events Small (minified) | 2,102 | 1,817 | 1,924 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,435 | 1,853 | 1,365 | 1.31× | Fused | Win |
| Github Events Small (prettified) | 1,898 | 2,246 | 2,429 | 0.84× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 2,889 | 3,487 | 2,703 | 0.83× | Two-stage | Win |
| Instruments Small (minified) | 2,683 | 1,979 | 1,577 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 2,301 | 2,995 | 2,529 | 0.77× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 833 | 754 | 593 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,264 | 2,551 | 2,135 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 947 | 795 | 592 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 2,400 | 2,771 | 2,153 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,752 | 1,586 | 1,116 | 1.10× | Fused | Win |
| Random Small (prettified) | 1,760 | 2,347 | 1,759 | 0.75× | Two-stage | Win |
| Twitter Small (minified) | 2,576 | 2,183 | 2,185 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 2,058 | 2,736 | 2,709 | 0.75× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,150 | 277 | 193 | 7.75× | Fused | Win |
| Double (POD) | 1,186 | 373 | 231 | 3.18× | Fused | Win |
| Int64 (POD) | 3,379 | 823 | 536 | 4.11× | Fused | Win |
| String (POD) | 1,801 | 1,014 | 1,019 | 1.78× | Fused | Win |
| Uint64 (POD) | 3,389 | 843 | 546 | 4.02× | Fused | Win |
| Apache Builds (minified) | 1,640 | 1,496 | 1,433 | 1.10× | Fused | Win |
| Canada (minified) | 971 | 1,003 | 847 | 0.97× | Two-stage | Win |
| Apache Builds (prettified) | 1,741 | 1,895 | 1,834 | 0.92× | Two-stage | Win |
| Canada (prettified) | 2,049 | 2,194 | 1,911 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,399 | 1,838 | 1,348 | 1.30× | Fused | Win |
| CitmCatalog (prettified) | 3,337 | 3,628 | 2,778 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,354 | 1,859 | 1,434 | 1.27× | Fused | Win |
| Discord (prettified) | 2,268 | 2,528 | 2,063 | 0.90× | Two-stage | Win |
| Github Events (minified) | 1,605 | 1,533 | 1,533 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 2,585 | 1,906 | 1,113 | 1.36× | Fused | Win |
| Github Events (prettified) | 1,755 | 1,832 | 1,655 | 0.96× | Two-stage | Win |
| Google Maps Response (prettified) | 3,453 | 3,511 | 2,201 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,350 | 2,265 | 1,386 | 1.48× | Fused | Win |
| Instruments (prettified) | 3,032 | 3,425 | 2,268 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 729 | 729 | 600 | 1.00× | Tie | Win |
| Marine IK Reverse (prettified) | 2,369 | 2,562 | 2,170 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 747 | 729 | 591 | 1.02× | Fused | Win |
| Marine IK (prettified) | 2,417 | 2,618 | 2,183 | 0.92× | Two-stage | Win |
| Random (minified) | 1,102 | 1,139 | 873 | 0.97× | Two-stage | Win |
| Random (prettified) | 1,344 | 1,710 | 1,323 | 0.79× | Two-stage | Win |
| Twitter (minified) | 1,959 | 1,678 | 1,341 | 1.17× | Fused | Win |
| Twitter (prettified) | 1,776 | 2,185 | 1,753 | 0.81× | Two-stage | Win |
| Apache Builds Small (minified) | 2,427 | 2,152 | 1,675 | 1.13× | Fused | Win |
| Canada Small (minified) | 1,068 | 973 | 783 | 1.10× | Fused | Win |
| Apache Builds Small (prettified) | 2,330 | 2,535 | 2,059 | 0.92× | Two-stage | Win |
| Canada Small (prettified) | 2,072 | 2,252 | 1,856 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,823 | 1,491 | 1,148 | 1.22× | Fused | Win |
| CitmCatalog Small (prettified) | 2,107 | 2,461 | 1,997 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 2,104 | 1,677 | 1,262 | 1.25× | Fused | Win |
| Discord Small (prettified) | 1,962 | 2,208 | 1,702 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 1,948 | 1,760 | 1,875 | 1.11× | Fused | Win |
| Google Maps Response Small (minified) | 2,407 | 1,799 | 1,135 | 1.34× | Fused | Win |
| Github Events Small (prettified) | 1,960 | 2,251 | 2,347 | 0.87× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,153 | 3,320 | 2,255 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 3,364 | 2,212 | 1,268 | 1.52× | Fused | Win |
| Instruments Small (prettified) | 2,899 | 3,358 | 2,053 | 0.86× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 834 | 748 | 589 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,262 | 2,599 | 2,118 | 0.87× | Two-stage | Win |
| Marine IK Small (minified) | 921 | 762 | 593 | 1.21× | Fused | Win |
| Marine IK Small (prettified) | 2,427 | 2,708 | 2,168 | 0.90× | Two-stage | Win |
| Random Small (minified) | 1,694 | 1,591 | 928 | 1.06× | Fused | Win |
| Random Small (prettified) | 1,833 | 2,351 | 1,429 | 0.78× | Two-stage | Win |
| Twitter Small (minified) | 2,837 | 2,267 | 1,704 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,381 | 2,865 | 2,184 | 0.83× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,725 | 235 | 202 | 7.33× | Fused | Win |
| Double (POD) | 1,018 | 369 | 159 | 2.76× | Fused | Win |
| Int64 (POD) | 2,642 | 782 | 522 | 3.38× | Fused | Win |
| String (POD) | 1,440 | 830 | 881 | 1.73× | Fused | Win |
| Uint64 (POD) | 2,583 | 786 | 471 | 3.28× | Fused | Win |
| Apache Builds (minified) | 1,261 | 1,029 | 747 | 1.23× | Fused | Win |
| Canada (minified) | 882 | 795 | 505 | 1.11× | Fused | Win |
| Apache Builds (prettified) | 1,299 | 1,278 | 964 | 1.02× | Fused | Win |
| Canada (prettified) | 1,753 | 1,680 | 1,278 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 2,158 | 1,425 | 1,203 | 1.51× | Fused | Win |
| CitmCatalog (prettified) | 3,052 | 2,684 | 2,306 | 1.14× | Fused | Win |
| Discord (minified) | 1,637 | 1,228 | 991 | 1.33× | Fused | Win |
| Discord (prettified) | 1,628 | 1,768 | 1,421 | 0.92× | Two-stage | Win |
| Github Events (minified) | 1,570 | 1,444 | 1,012 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 2,048 | 1,195 | 722 | 1.71× | Fused | Win |
| Github Events (prettified) | 1,658 | 1,620 | 1,203 | 1.02× | Fused | Win |
| Google Maps Response (prettified) | 2,297 | 1,730 | 1,373 | 1.33× | Fused | Win |
| Instruments (minified) | 2,647 | 1,634 | 1,153 | 1.62× | Fused | Win |
| Instruments (prettified) | 2,811 | 1,997 | 1,792 | 1.41× | Fused | Win |
| Marine IK Reverse (minified) | 579 | 518 | 436 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 1,662 | 1,887 | 1,675 | 0.88× | Two-stage | Win |
| Marine IK (minified) | 607 | 548 | 382 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,089 | 2,064 | 1,429 (n/c) | 1.01× | Fused | Win |
| Random (minified) | 862 | 879 | 666 | 0.98× | Tie | Win |
| Random (prettified) | 1,079 | 1,064 | 1,093 | 1.01× | Tie | Tie |
| Twitter (minified) | 1,891 | 1,613 | 841 | 1.17× | Fused | Win |
| Twitter (prettified) | 2,068 | 1,980 | 1,163 | 1.04× | Fused | Win |
| Apache Builds Small (minified) | 1,365 | 1,162 | 697 | 1.18× | Fused | Win |
| Canada Small (minified) | 723 | 761 | 489 | 0.95× | Two-stage | Win |
| Apache Builds Small (prettified) | 1,296 | 1,386 | 858 | 0.94× | Two-stage | Win |
| Canada Small (prettified) | 1,631 | 1,375 | 856 | 1.19× | Fused | Win |
| CitmCatalog Small (minified) | 939 (n/c) | 1,005 | 678 | 0.93× | Two-stage | Win |
| CitmCatalog Small (prettified) | 1,759 | 1,668 | 1,346 | 1.05× | Fused | Win |
| Discord Small (minified) | 1,417 | 1,126 | 760 | 1.26× | Fused | Win |
| Discord Small (prettified) | 1,588 | 1,407 | 1,204 | 1.13× | Fused | Win |
| Github Events Small (minified) | 1,424 | 1,247 | 903 | 1.14× | Fused | Win |
| Google Maps Response Small (minified) | 1,697 | 1,142 | 708 | 1.49× | Fused | Win |
| Github Events Small (prettified) | 1,561 | 1,491 | 1,368 | 1.05× | Fused | Win |
| Google Maps Response Small (prettified) | 2,180 | 2,304 | 1,406 | 0.95× | Two-stage | Win |
| Instruments Small (minified) | 2,272 | 1,397 | 1,084 | 1.63× | Fused | Win |
| Instruments Small (prettified) | 2,376 | 2,282 | 1,544 | 1.04× | Fused | Win |
| Marine IK Reverse Small (minified) | 556 | 508 | 373 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,295 | 1,830 | 1,203 | 0.71× | Two-stage | Win |
| Marine IK Small (minified) | 535 | 497 | 339 | 1.08× | Fused | Win |
| Marine IK Small (prettified) | 1,810 | 1,517 | 1,281 | 1.19× | Fused | Win |
| Random Small (minified) | 1,274 | 1,000 | 642 | 1.27× | Fused | Win |
| Random Small (prettified) | 1,538 | 879 | 1,030 | 1.75× | Fused | Win |
| Twitter Small (minified) | 2,332 | 1,869 | 1,134 | 1.25× | Fused | Win |
| Twitter Small (prettified) | 2,213 | 1,959 | 1,475 | 1.13× | Fused | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 942 | 216 | 151 | 4.37× | Fused | Win |
| Double (POD) | 912 | 325 | 187 | 2.80× | Fused | Win |
| Int64 (POD) | 2,028 | 618 | 398 | 3.28× | Fused | Win |
| String (POD) | 1,173 | 655 | 826 | 1.79× | Fused | Win |
| Uint64 (POD) | 1,945 | 528 | 421 | 3.68× | Fused | Win |
| Apache Builds (minified) | 1,242 | 999 | 794 | 1.24× | Fused | Win |
| Canada (minified) | 764 | 619 | 488 | 1.23× | Fused | Win |
| Apache Builds (prettified) | 1,474 | 1,247 | 997 | 1.18× | Fused | Win |
| Canada (prettified) | 1,279 | 1,502 | 1,022 | 0.85× | Two-stage | Win |
| CitmCatalog (minified) | 2,021 | 1,235 | 1,085 | 1.64× | Fused | Win |
| CitmCatalog (prettified) | 2,877 | 2,943 | 2,202 | 0.98× | Two-stage | Win |
| Discord (minified) | 2,595 | 1,309 | 1,391 | 1.98× | Fused | Win |
| Discord (prettified) | 2,464 | 1,590 | 1,975 | 1.55× | Fused | Win |
| Github Events (minified) | 1,873 | 1,524 | 1,604 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,153 | 1,265 | 1,049 | 1.70× | Fused | Win |
| Github Events (prettified) | 1,836 | 1,736 | 1,899 | 1.06× | Fused | Loss |
| Google Maps Response (prettified) | 3,135 | 2,394 | 2,081 | 1.31× | Fused | Win |
| Instruments (minified) | 2,834 | 1,767 | 1,261 | 1.60× | Fused | Win |
| Instruments (prettified) | 2,843 | 2,595 | 2,196 | 1.10× | Fused | Win |
| Marine IK Reverse (minified) | 719 | 620 | 450 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 1,839 | 1,724 | 1,315 | 1.07× | Fused | Win |
| Marine IK (minified) | 728 | 624 | 413 | 1.17× | Fused | Win |
| Marine IK (prettified) | 1,965 | 1,683 (n/c) | 1,316 | 1.17× | Fused | Win |
| Random (minified) | 1,120 | 1,007 | 909 | 1.11× | Fused | Win |
| Random (prettified) | 1,402 | 1,042 | 1,213 | 1.35× | Fused | Win |
| Twitter (minified) | 2,016 | 1,602 | 1,717 | 1.26× | Fused | Win |
| Twitter (prettified) | 2,039 | 1,795 | 1,887 | 1.14× | Fused | Win |
| Apache Builds Small (minified) | 1,587 | 1,216 | 945 | 1.31× | Fused | Win |
| Canada Small (minified) | 730 | 601 | 369 | 1.22× | Fused | Win |
| Apache Builds Small (prettified) | 1,511 | 1,402 | 1,022 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,306 | 1,179 | 930 | 1.11× | Fused | Win |
| CitmCatalog Small (minified) | 1,099 | 778 | 666 | 1.41× | Fused | Win |
| CitmCatalog Small (prettified) | 1,464 | 1,233 | 1,190 | 1.19× | Fused | Win |
| Discord Small (minified) | 1,844 | 1,033 | 972 | 1.78× | Fused | Win |
| Discord Small (prettified) | 2,049 | 1,526 | 1,368 | 1.34× | Fused | Win |
| Github Events Small (minified) | 1,266 | 1,071 | 1,499 | 1.18× | Fused | Loss |
| Google Maps Response Small (minified) | 1,799 | 1,152 | 809 | 1.56× | Fused | Win |
| Github Events Small (prettified) | 1,647 | 1,469 | 1,344 | 1.12× | Fused | Win |
| Google Maps Response Small (prettified) | 2,408 | 1,955 | 1,865 | 1.23× | Fused | Win |
| Instruments Small (minified) | 2,364 | 1,639 | 1,090 | 1.44× | Fused | Win |
| Instruments Small (prettified) | 2,331 | 2,269 | 1,922 | 1.03× | Tie | Win |
| Marine IK Reverse Small (minified) | 601 | 501 (n/c) | 339 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,692 | 1,683 | 1,137 | 1.01× | Tie | Win |
| Marine IK Small (minified) | 675 | 534 | 310 | 1.26× | Fused | Win |
| Marine IK Small (prettified) | 1,876 | 1,852 | 1,248 | 1.01× | Fused | Win |
| Random Small (minified) | 1,448 | 981 | 878 | 1.48× | Fused | Win |
| Random Small (prettified) | 1,703 | 1,350 | 1,342 | 1.26× | Fused | Win |
| Twitter Small (minified) | 2,365 | 1,357 | 1,636 | 1.74× | Fused | Win |
| Twitter Small (prettified) | 2,141 | 2,109 | 2,082 | 1.02× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 514 | 140 | 131 | 3.67× | Fused | Win |
| Double (POD) | 703 | 335 | 179 | 2.10× | Fused | Win |
| Int64 (POD) | 2,093 | 620 | 408 | 3.38× | Fused | Win |
| String (POD) | 1,718 | 953 | 888 | 1.80× | Fused | Win |
| Uint64 (POD) | 2,006 | 638 | 408 | 3.14× | Fused | Win |
| Apache Builds (minified) | 1,369 (n/c) | 1,109 | 736 | 1.23× | Fused | Win |
| Canada (minified) | 730 | 656 (n/c) | 405 | 1.11× | Fused | Win |
| Apache Builds (prettified) | 1,356 (n/c) | 1,422 | 1,094 | 0.95× | Two-stage | Win |
| Canada (prettified) | 1,447 | 1,521 (n/c) | 896 | 0.95× | Two-stage | Win |
| CitmCatalog (minified) | 2,259 | 1,404 (n/c) | 504 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | 2,471 | 3,012 | 1,195 | 0.82× | Two-stage | Win |
| Discord (minified) | 1,589 | 1,373 | 709 | 1.16× | Fused | Win |
| Discord (prettified) | 1,510 | 2,090 | 1,017 | 0.72× | Two-stage | Win |
| Github Events (minified) | 1,399 | 1,196 | 624 | 1.17× | Fused | Win |
| Google Maps Response (minified) | 1,774 | 1,336 | 564 | 1.33× | Fused | Win |
| Github Events (prettified) | 1,482 | 1,566 | 846 | 0.95× | Two-stage | Win |
| Google Maps Response (prettified) | 2,371 | 2,471 | 1,019 | 0.96× | Two-stage | Win |
| Instruments (minified) | 2,652 | 1,739 | 769 | 1.53× | Fused | Win |
| Instruments (prettified) | 2,069 | 2,438 | 1,088 | 0.85× | Two-stage | Win |
| Marine IK Reverse (minified) | 480 | 450 (n/c) | 33 (n/c) | 1.07× | Fused | Win |
| Marine IK Reverse (prettified) | 1,510 | 1,653 | 131 (n/c) | 0.91× | Two-stage | Win |
| Marine IK (minified) | 491 | 432 | 247 | 1.14× | Fused | Win |
| Marine IK (prettified) | 1,539 | 1,767 (n/c) | 902 | 0.87× | Two-stage | Win |
| Random (minified) | 976 | 890 | 653 (n/c) | 1.10× | Fused | Win |
| Random (prettified) | 1,137 | 1,341 | 949 | 0.85× | Two-stage | Win |
| Twitter (minified) | 1,657 | 1,375 | 882 | 1.21× | Fused | Win |
| Twitter (prettified) | 1,375 | 1,716 | 1,131 | 0.80× | Two-stage | Win |
| Apache Builds Small (minified) | 1,806 | 1,330 | 937 | 1.36× | Fused | Win |
| Canada Small (minified) | 726 | 641 | 366 | 1.13× | Fused | Win |
| Apache Builds Small (prettified) | 1,737 | 1,710 | 1,145 | 1.02× | Fused | Win |
| Canada Small (prettified) | 1,374 | 1,575 (n/c) | 870 | 0.87× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,213 | 1,016 | 563 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 1,413 | 1,685 | 961 | 0.84× | Two-stage | Win |
| Discord Small (minified) | 1,466 | 1,235 | 810 | 1.19× | Fused | Win |
| Discord Small (prettified) | 1,439 | 1,615 | 1,087 | 0.89× | Two-stage | Win |
| Github Events Small (minified) | 1,621 | 1,126 | 1,168 | 1.44× | Fused | Win |
| Google Maps Response Small (minified) | 1,747 | 1,342 | 563 | 1.30× | Fused | Win |
| Github Events Small (prettified) | 1,475 | 1,412 | 1,562 | 1.05× | Fused | Loss |
| Google Maps Response Small (prettified) | 2,338 | 2,630 | 1,027 | 0.89× | Two-stage | Win |
| Instruments Small (minified) | 2,472 | 1,680 | 748 | 1.47× | Fused | Win |
| Instruments Small (prettified) | 1,931 | 2,489 | 1,185 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 542 | 416 | 39 (n/c) | 1.30× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,515 | 1,585 (n/c) | 154 (n/c) | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 579 | 520 | 286 | 1.11× | Fused | Win |
| Marine IK Small (prettified) | 1,621 | 1,858 | 1,092 | 0.87× | Two-stage | Win |
| Random Small (minified) | 1,178 | 984 | 648 | 1.20× | Fused | Win |
| Random Small (prettified) | 1,343 | 1,496 | 1,015 | 0.90× | Two-stage | Win |
| Twitter Small (minified) | 1,986 | 1,698 | 1,197 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 1,699 | 2,140 | 1,579 (n/c) | 0.79× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,104 | 321 | 236 | 6.55× | Fused | Win |
| Double (POD) | 1,458 | 421 | 266 | 3.47× | Fused | Win |
| Int64 (POD) | 3,491 | 927 | 603 | 3.77× | Fused | Win |
| String (POD) | 2,811 | 1,521 | 1,408 | 1.85× | Fused | Win |
| Uint64 (POD) | 3,735 | 941 | 557 | 3.97× | Fused | Win |
| Apache Builds (minified) | 2,940 | 2,379 | 1,711 | 1.24× | Fused | Win |
| Canada (minified) | 1,262 | 1,183 | 804 | 1.07× | Fused | Win |
| Apache Builds (prettified) | 2,479 | 2,991 | 2,188 | 0.83× | Two-stage | Win |
| Canada (prettified) | 2,512 | 2,416 | 1,792 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 3,024 | 2,139 | 1,414 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 3,595 | 4,175 | 2,947 | 0.86× | Two-stage | Win |
| Discord (minified) | 3,315 | 2,494 | 1,906 | 1.33× | Fused | Win |
| Discord (prettified) | 2,317 | 3,476 | 2,719 | 0.67× | Two-stage | Win |
| Github Events (minified) | 2,296 | 2,107 | 2,062 | 1.09× | Fused | Win |
| Google Maps Response (minified) | 3,013 | 2,190 | 1,477 | 1.38× | Fused | Win |
| Github Events (prettified) | 2,032 | 2,450 | 2,468 | 0.83× | Two-stage | Loss |
| Google Maps Response (prettified) | 3,354 | 4,062 | 3,008 | 0.83× | Two-stage | Win |
| Instruments (minified) | 3,121 | 2,278 | 1,886 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,576 | 3,315 | 3,013 | 0.78× | Two-stage | Win |
| Marine IK Reverse (minified) | 911 | 813 | 651 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 2,842 | 2,772 | 2,367 | 1.02× | Fused | Win |
| Marine IK (minified) | 936 | 833 | 656 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,831 | 2,768 | 2,363 | 1.02× | Fused | Win |
| Random (minified) | 1,759 | 1,498 | 1,456 | 1.17× | Fused | Win |
| Random (prettified) | 1,845 | 2,201 | 2,173 | 0.84× | Two-stage | Win |
| Twitter (minified) | 2,450 | 2,139 | 2,129 | 1.15× | Fused | Win |
| Twitter (prettified) | 2,062 | 2,741 | 2,701 | 0.75× | Two-stage | Win |
| Apache Builds Small (minified) | 3,159 | 2,592 | 1,841 | 1.22× | Fused | Win |
| Canada Small (minified) | 1,301 | 1,220 | 838 | 1.07× | Fused | Win |
| Apache Builds Small (prettified) | 2,532 | 3,133 | 2,269 | 0.81× | Two-stage | Win |
| Canada Small (prettified) | 2,422 | 2,606 | 1,952 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,220 | 1,718 | 1,392 | 1.29× | Fused | Win |
| CitmCatalog Small (prettified) | 2,310 | 2,866 | 2,400 | 0.81× | Two-stage | Win |
| Discord Small (minified) | 2,786 | 2,248 | 1,869 | 1.24× | Fused | Win |
| Discord Small (prettified) | 2,120 | 2,920 | 2,493 | 0.73× | Two-stage | Win |
| Github Events Small (minified) | 2,620 | 2,157 | 2,336 | 1.21× | Fused | Win |
| Google Maps Response Small (minified) | 2,866 | 2,101 | 1,537 | 1.36× | Fused | Win |
| Github Events Small (prettified) | 2,358 | 2,721 | 2,959 | 0.87× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,124 | 3,877 | 3,018 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,947 | 2,092 | 1,737 | 1.41× | Fused | Win |
| Instruments Small (prettified) | 2,433 | 3,191 | 2,751 | 0.76× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,034 | 904 | 663 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,622 | 2,989 | 2,390 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 1,160 | 973 | 673 | 1.19× | Fused | Win |
| Marine IK Small (prettified) | 2,794 | 3,251 | 2,385 | 0.86× | Two-stage | Win |
| Random Small (minified) | 2,229 | 1,943 | 1,419 | 1.15× | Fused | Win |
| Random Small (prettified) | 2,041 | 2,865 | 2,176 | 0.71× | Two-stage | Win |
| Twitter Small (minified) | 2,820 | 2,461 | 2,268 | 1.15× | Fused | Win |
| Twitter Small (prettified) | 2,328 | 3,161 | 2,895 | 0.74× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,380 | 278 | 191 | 8.57× | Fused | Win |
| Double (POD) | 1,305 | 384 | 233 | 3.40× | Fused | Win |
| Int64 (POD) | 3,701 | 836 | 536 | 4.43× | Fused | Win |
| String (POD) | 2,681 | 1,257 | 1,275 | 2.13× | Fused | Win |
| Uint64 (POD) | 3,780 | 863 | 539 | 4.38× | Fused | Win |
| Apache Builds (minified) | 2,674 | 2,197 | 2,031 | 1.22× | Fused | Win |
| Canada (minified) | 1,167 | 1,231 | 1,038 | 0.95× | Two-stage | Win |
| Apache Builds (prettified) | 2,513 | 2,765 | 2,588 | 0.91× | Two-stage | Win |
| Canada (prettified) | 2,449 | 2,638 | 2,263 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,982 | 2,177 | 1,639 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 3,844 | 4,268 | 3,282 | 0.90× | Two-stage | Win |
| Discord (minified) | 3,279 | 2,438 | 1,799 | 1.35× | Fused | Win |
| Discord (prettified) | 2,765 | 3,353 | 2,541 | 0.82× | Two-stage | Win |
| Github Events (minified) | 2,167 | 2,044 | 1,691 | 1.06× | Fused | Win |
| Google Maps Response (minified) | 3,032 | 2,110 | 1,180 | 1.44× | Fused | Win |
| Github Events (prettified) | 2,195 | 2,353 | 2,012 | 0.93× | Two-stage | Win |
| Google Maps Response (prettified) | 3,802 | 3,795 | 2,343 | 1.00× | Tie | Win |
| Instruments (minified) | 3,978 | 2,561 | 1,480 | 1.55× | Fused | Win |
| Instruments (prettified) | 3,372 | 3,777 | 2,371 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 800 | 802 | 663 | 1.00× | Tie | Win |
| Marine IK Reverse (prettified) | 2,578 | 2,829 | 2,370 | 0.91× | Two-stage | Win |
| Marine IK (minified) | 823 | 806 | 652 | 1.02× | Fused | Win |
| Marine IK (prettified) | 2,626 | 2,862 | 2,386 | 0.92× | Two-stage | Win |
| Random (minified) | 1,475 | 1,527 | 1,138 | 0.97× | Two-stage | Win |
| Random (prettified) | 1,662 | 2,274 | 1,710 | 0.73× | Two-stage | Win |
| Twitter (minified) | 2,536 | 2,142 | 1,608 | 1.18× | Fused | Win |
| Twitter (prettified) | 2,103 | 2,723 | 2,096 | 0.77× | Two-stage | Win |
| Apache Builds Small (minified) | 2,843 | 2,450 | 1,931 | 1.16× | Fused | Win |
| Canada Small (minified) | 1,371 | 1,206 | 896 | 1.14× | Fused | Win |
| Apache Builds Small (prettified) | 2,631 | 2,973 | 2,409 | 0.88× | Two-stage | Win |
| Canada Small (prettified) | 2,466 | 2,703 | 2,131 | 0.91× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,185 | 1,762 | 1,291 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 2,387 | 2,876 | 2,233 | 0.83× | Two-stage | Win |
| Discord Small (minified) | 2,699 | 2,066 | 1,567 | 1.31× | Fused | Win |
| Discord Small (prettified) | 2,404 | 2,648 | 2,109 | 0.91× | Two-stage | Win |
| Github Events Small (minified) | 2,454 | 2,111 | 2,198 | 1.16× | Fused | Win |
| Google Maps Response Small (minified) | 2,798 | 2,016 | 1,265 | 1.39× | Fused | Win |
| Github Events Small (prettified) | 2,300 | 2,649 | 2,725 | 0.87× | Two-stage | Loss |
| Google Maps Response Small (prettified) | 3,462 | 3,681 | 2,457 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,728 | 2,370 | 1,339 | 1.57× | Fused | Win |
| Instruments Small (prettified) | 3,039 | 3,563 | 2,181 | 0.85× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,032 | 883 | 649 | 1.17× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,601 | 3,024 | 2,315 | 0.86× | Two-stage | Win |
| Marine IK Small (minified) | 1,146 | 899 | 641 | 1.27× | Fused | Win |
| Marine IK Small (prettified) | 2,793 | 3,177 | 2,306 | 0.88× | Two-stage | Win |
| Random Small (minified) | 2,123 | 1,949 | 1,117 | 1.09× | Fused | Win |
| Random Small (prettified) | 2,132 | 2,872 | 1,708 | 0.74× | Two-stage | Win |
| Twitter Small (minified) | 3,177 | 2,509 | 1,813 | 1.27× | Fused | Win |
| Twitter Small (prettified) | 2,517 | 3,175 | 2,272 | 0.79× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,611 | 264 | 204 | 6.11× | Fused | Win |
| Double (POD) | 1,234 | 400 | 165 | 3.08× | Fused | Win |
| Int64 (POD) | 3,368 | 812 | 540 | 4.15× | Fused | Win |
| String (POD) | 2,464 | 1,111 | 1,187 | 2.22× | Fused | Win |
| Uint64 (POD) | 3,279 | 813 | 482 | 4.03× | Fused | Win |
| Apache Builds (minified) | 2,162 | 1,508 | 985 | 1.43× | Fused | Win |
| Canada (minified) | 1,101 | 972 | 600 (n/c) | 1.13× | Fused | Win |
| Apache Builds (prettified) | 1,916 | 1,852 | 1,248 | 1.03× | Fused | Win |
| Canada (prettified) | 2,125 | 1,998 | 1,430 | 1.06× | Fused | Win |
| CitmCatalog (minified) | 2,710 | 1,651 | 1,337 | 1.64× | Fused | Win |
| CitmCatalog (prettified) | 3,484 | 3,064 | 2,516 | 1.14× | Fused | Win |
| Discord (minified) | 2,211 | 1,452 | 1,222 | 1.52× | Fused | Win |
| Discord (prettified) | 2,239 | 2,194 | 1,684 | 1.02× | Fused | Win |
| Github Events (minified) | 2,356 | 1,904 | 1,213 | 1.24× | Fused | Win |
| Google Maps Response (minified) | 2,276 | 1,365 | 774 | 1.67× | Fused | Win |
| Github Events (prettified) | 2,192 | 2,119 | 1,370 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,498 | 2,324 | 1,578 | 1.08× | Fused | Win |
| Instruments (minified) | 3,164 | 1,719 | 1,031 | 1.84× | Fused | Win |
| Instruments (prettified) | 2,406 | 2,348 | 2,084 | 1.02× | Tie | Win |
| Marine IK Reverse (minified) | 682 | 599 | 435 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 2,089 | 2,103 | 1,815 | 0.99× | Tie | Win |
| Marine IK (minified) | 701 | 612 | 433 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,338 | 2,318 | 1,546 | 1.01× | Fused | Win |
| Random (minified) | 1,305 | 1,258 | 835 | 1.04× | Fused | Win |
| Random (prettified) | 1,496 | 1,696 | 1,346 | 0.88× | Two-stage | Win |
| Twitter (minified) | 2,386 | 1,939 | 956 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,463 | 2,058 | 1,406 | 1.20× | Fused | Win |
| Apache Builds Small (minified) | 1,642 | 1,474 | 950 | 1.11× | Fused | Win |
| Canada Small (minified) | 1,113 | 780 | 507 | 1.43× | Fused | Win |
| Apache Builds Small (prettified) | 1,832 | 1,598 | 1,160 | 1.15× | Fused | Win |
| Canada Small (prettified) | 2,028 | 1,754 | 943 | 1.16× | Fused | Win |
| CitmCatalog Small (minified) | 1,536 | 1,086 | 938 | 1.41× | Fused | Win |
| CitmCatalog Small (prettified) | 2,143 | 1,723 | 1,628 | 1.24× | Fused | Win |
| Discord Small (minified) | 2,146 | 1,658 | 1,103 | 1.29× | Fused | Win |
| Discord Small (prettified) | 2,010 | 2,185 | 1,593 | 0.92× | Two-stage | Win |
| Github Events Small (minified) | 1,605 | 1,261 | 1,183 | 1.27× | Fused | Win |
| Google Maps Response Small (minified) | 2,165 | 1,165 | 901 | 1.86× | Fused | Win |
| Github Events Small (prettified) | 2,124 | 1,988 | 1,840 | 1.07× | Fused | Win |
| Google Maps Response Small (prettified) | 2,520 | 2,311 | 1,561 | 1.09× | Fused | Win |
| Instruments Small (minified) | 2,927 | 1,685 | 1,091 | 1.74× | Fused | Win |
| Instruments Small (prettified) | 2,681 | 2,543 | 1,835 | 1.05× | Fused | Win |
| Marine IK Reverse Small (minified) | 778 | 620 | 389 | 1.25× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,913 | 1,736 | 1,240 | 1.10× | Fused | Win |
| Marine IK Small (minified) | 872 | 633 | 382 | 1.38× | Fused | Win |
| Marine IK Small (prettified) | 1,880 | 2,347 | 1,454 | 0.80× | Two-stage | Win |
| Random Small (minified) | 1,708 | 1,357 | 964 | 1.26× | Fused | Win |
| Random Small (prettified) | 1,787 | 1,924 | 1,266 | 0.93× | Two-stage | Win |
| Twitter Small (minified) | 2,320 | 1,988 | 1,260 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 2,027 | 2,639 | 1,423 | 0.77× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M2 Pro, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,101 | 195 (n/c) | 195 | 5.63× | Fused | Win |
| Double (POD) | 1,047 | 347 | 192 | 3.01× | Fused | Win |
| Int64 (POD) | 2,651 | 721 | 485 | 3.68× | Fused | Win |
| String (POD) | 2,082 | 851 | 1,244 | 2.45× | Fused | Win |
| Uint64 (POD) | 2,780 | 722 | 427 | 3.85× | Fused | Win |
| Apache Builds (minified) | 2,096 | 1,395 | 994 | 1.50× | Fused | Win |
| Canada (minified) | 926 | 801 | 566 | 1.16× | Fused | Win |
| Apache Builds (prettified) | 2,072 | 1,660 | 1,169 | 1.25× | Fused | Win |
| Canada (prettified) | 1,603 | 1,728 | 1,207 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 2,398 | 1,653 | 1,177 | 1.45× | Fused | Win |
| CitmCatalog (prettified) | 3,468 | 3,361 | 2,124 | 1.03× | Fused | Win |
| Discord (minified) | 3,230 | 1,710 | 1,643 | 1.89× | Fused | Win |
| Discord (prettified) | 2,814 | 2,293 | 2,321 | 1.23× | Fused | Win |
| Github Events (minified) | 2,554 | 1,966 | 2,069 | 1.30× | Fused | Win |
| Google Maps Response (minified) | 2,614 | 1,463 | 1,117 | 1.79× | Fused | Win |
| Github Events (prettified) | 2,400 | 1,954 | 2,413 | 1.23× | Fused | Loss |
| Google Maps Response (prettified) | 3,264 | 2,688 | 2,172 | 1.21× | Fused | Win |
| Instruments (minified) | 3,358 | 1,985 | 1,491 | 1.69× | Fused | Win |
| Instruments (prettified) | 3,144 | 2,845 | 2,366 | 1.10× | Fused | Win |
| Marine IK Reverse (minified) | 836 | 700 | 494 | 1.19× | Fused | Win |
| Marine IK Reverse (prettified) | 2,033 | 1,898 | 1,451 | 1.07× | Fused | Win |
| Marine IK (minified) | 839 | 615 | 403 | 1.36× | Fused | Win |
| Marine IK (prettified) | 2,050 | 2,022 | 1,662 | 1.01× | Tie | Win |
| Random (minified) | 1,476 | 1,387 | 1,107 | 1.06× | Fused | Win |
| Random (prettified) | 1,930 | 1,514 | 1,481 | 1.27× | Fused | Win |
| Twitter (minified) | 2,655 | 1,940 | 1,669 | 1.37× | Fused | Win |
| Twitter (prettified) | 2,142 | 2,552 | 2,168 | 0.84× | Two-stage | Win |
| Apache Builds Small (minified) | 2,156 | 1,341 | 1,296 | 1.61× | Fused | Win |
| Canada Small (minified) | 785 | 700 | 461 | 1.12× | Fused | Win |
| Apache Builds Small (prettified) | 2,064 | 1,750 | 1,680 | 1.18× | Fused | Win |
| Canada Small (prettified) | 1,875 | 1,659 | 1,079 | 1.13× | Fused | Win |
| CitmCatalog Small (minified) | 1,670 | 1,177 | 962 | 1.42× | Fused | Win |
| CitmCatalog Small (prettified) | 1,898 | 1,703 | 1,614 | 1.11× | Fused | Win |
| Discord Small (minified) | 2,533 | 1,636 | 1,296 | 1.55× | Fused | Win |
| Discord Small (prettified) | 1,972 | 1,640 | 1,780 | 1.20× | Fused | Win |
| Github Events Small (minified) | 2,023 | 1,469 | 1,900 | 1.38× | Fused | Win |
| Google Maps Response Small (minified) | 2,261 | 1,129 | 965 | 2.00× | Fused | Win |
| Github Events Small (prettified) | 2,317 | 1,888 | 2,071 | 1.23× | Fused | Win |
| Google Maps Response Small (prettified) | 2,721 | 2,209 | 2,105 | 1.23× | Fused | Win |
| Instruments Small (minified) | 2,641 | 1,466 | 1,418 | 1.80× | Fused | Win |
| Instruments Small (prettified) | 2,831 | 2,574 | 1,878 (n/c) | 1.10× | Fused | Win |
| Marine IK Reverse Small (minified) | 757 | 611 | 368 | 1.24× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,037 | 2,101 | 1,353 | 0.97× | Two-stage | Win |
| Marine IK Small (minified) | 833 | 707 | 389 | 1.18× | Fused | Win |
| Marine IK Small (prettified) | 2,282 | 2,325 | 1,434 | 0.98× | Two-stage | Win |
| Random Small (minified) | 1,746 | 1,406 | 986 | 1.24× | Fused | Win |
| Random Small (prettified) | 1,989 | 1,750 | 1,600 | 1.14× | Fused | Win |
| Twitter Small (minified) | 2,841 | 1,951 | 1,935 | 1.46× | Fused | Win |
| Twitter Small (prettified) | 2,630 | 2,526 | 2,370 | 1.04× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins all POD input, 29 of the 33 full-size minified documents and all 33 Small minified ones, while the tape wins 28 of the 33 full-size prettified documents and 29 of the 33 Small prettified ones. MSVC splits almost cleanly: 21 of the 22 minified documents go to the fused path (minified Discord is a tie) and 16 of the 22 prettified ones to the tape, with Apache Builds Small, Github Events Small and Google Maps Response Small to the fused path and three ties. On the M2 Pro the fused path wins all POD input, 41 of the 44 minified documents (the tape takes Canada Small and CitmCatalog Small on macOS/GCC, and minified Random is a tie there) and 34 of the 44 prettified documents (seven to the tape, three ties).

**POD-type tests: the fused path wins all 25 freshly allocated and all 25 reused, on every build.** It runs them 1.57× (String, Linux/Clang) to 7.75× (Bool, Linux/GCC) faster than the two-stage path, and 1.80× (String, Windows/MSVC) to 8.57× (Bool, Linux/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the platform.

**Minified documents: the fused path's, at both sizes.** The fused path wins 103 of the 110 minified documents: all 22 on Linux/Clang and on macOS/Clang, 21 of 22 under MSVC, where minified Discord is a tie, 19 of 22 on Linux/GCC, where the two-stage path wins minified Canada by 3% (1,003 against 971 MB/s) and minified Random by 3% (1,139 against 1,102) and minified Marine IK Reverse is a tie, and 19 of 22 on macOS/GCC, where the two-stage path wins Canada Small by 5% (761 against 723 MB/s) and CitmCatalog Small by 7% (1,005 against 939) and minified Random is a tie. The fused path's margins run from 2.5% (Marine IK on Linux/GCC, 747 against 729 MB/s) to 1.98× (Discord on macOS/Clang, 2,595 against 1,309 MB/s) on the full-size documents and up to 1.78× on the Small ones (Discord Small on macOS/Clang, 1,844 against 1,033 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 57 of 66.** It wins all 11 at each size on Linux/GCC, all 11 Small prettified documents and 8 of the 11 full-size ones on Linux/Clang, and 9 of the 11 full-size and 7 of the 11 Small prettified documents under MSVC. The fused path wins prettified Canada, Marine IK and Marine IK Reverse on Linux/Clang, by 2% to 6%, and prettified Apache Builds Small, Github Events Small and Google Maps Response Small under MSVC, by 7% to 9%; prettified Apache Builds, Canada and Discord Small are ties under MSVC. The tape's margins run from 2% (Google Maps Response on Linux/GCC, 3,511 against 3,453 MB/s) to 41% (Discord on Linux/Clang, 2,652 against 1,875) on the full-size documents and from 4% to 38% on the Small ones, widest on Instruments Small under MSVC. The documents the fused path takes on Linux/Clang, Canada and the two Marine IK documents, are the most number-heavy in the corpus, and Discord is the widest full-size tape win. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 57 of the 66 prettified x86 documents; the fused path wins prettified Canada, Marine IK and Marine IK Reverse on Linux/Clang and prettified Apache Builds, Marine IK, Apache Builds Small and Github Events Small under MSVC, and prettified Canada and Marine IK Reverse are ties under MSVC.

**Prettified documents on the M2 Pro: mostly the fused path.** Of the 44 prettified documents, the fused path wins 34, the tape wins 7 and 3 are ties; the tape's wins are prettified Discord and Marine IK Reverse on macOS/GCC, prettified Canada and CitmCatalog on macOS/Clang, and Apache Builds Small, Google Maps Response Small and Marine IK Reverse Small on macOS/GCC. Reused, the fused path wins 32, the tape 9, and 3 are ties. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Github Events is where Jsonifier is weakest.** All six of the faster Jsonifier path's freshly allocated losses to simdjson On Demand are on Github Events or its Small copy: prettified Github Events on Linux/Clang by 1% (1,846 against 1,870 MB/s) and on macOS/Clang by 3% (1,836 against 1,899), prettified Github Events Small under MSVC by 7% (1,121 against 1,205), on Linux/Clang by 8% (2,246 against 2,429) and on Linux/GCC by 4% (2,251 against 2,347), and minified Github Events Small on macOS/Clang by 16% (1,266 against 1,499). The two ties are prettified Twitter on Linux/Clang and prettified Random on macOS/GCC. With objects reused the faster path loses five results, all on Github Events again: prettified Github Events on Linux/Clang by 1% (2,450 against 2,468) and on macOS/Clang by 1% (2,400 against 2,413), and prettified Github Events Small under MSVC by 6% (1,475 against 1,562), on Linux/Clang by 8% (2,721 against 2,959) and on Linux/GCC by 3% (2,649 against 2,725). We have not profiled what is particular about this document.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 10 full-size minified documents in declared key order, the fused path reaches a median 55% of its Linux/Clang throughput, the two-stage path 57% and simdjson 40%. Because the two Jsonifier paths lose a similar share, MSVC routes much as Linux/Clang does: minified to the fused path, prettified mostly to the tape. The spread between documents is about the same for the two Jsonifier paths (the fused path from 49% on Apache Builds to 73% on CitmCatalog, the two-stage path from 45% to 71% on the same two documents) and widest for simdjson, which keeps 12% of its Linux/Clang throughput on Github Events and 53% on Canada. We have not profiled why.

**Against simdjson, Jsonifier wins 237 of 245 tests with whichever path is faster, ties two and loses six** (all on Github Events or Github Events Small). The fused path alone wins 226, ties 3 and loses 16, its ten extra losses all prettified documents on x86: eight on Linux/Clang, prettified Apache Builds on Linux/GCC and prettified Twitter Small under MSVC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it does: simdjson parses minified Marine IK Reverse at 33 MB/s, against 227 MB/s for the same document in declared order, so the faster Jsonifier path is 14.8× faster than simdjson freshly allocated (14.5× reused). The prettified form shows the same gap, at 134 MB/s against 868, and the faster Jsonifier path is 12.0× faster (12.6× reused). The Small copies show it too, at 34 MB/s minified and 139 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.13× to 1.60× faster than simdjson on the full-size reversed document, freshly allocated, and 1.16× to 1.69× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M2 Pro, almost everything belongs to the fused path. Routing each test to its faster path would raise the record against simdjson from 226 wins, 3 ties and 16 losses to 237 wins, 2 ties and 6 losses. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,496 | 1,354 | 1.10× | Win |
| Canada (minified) | 1,003 | 919 | 1.09× | Win |
| Apache Builds (prettified) | 1,895 | 1,745 | 1.09× | Win |
| Canada (prettified) | 2,194 | 2,040 | 1.08× | Win |
| CitmCatalog (minified) | 1,838 | 1,419 | 1.30× | Win |
| CitmCatalog (prettified) | 3,628 | 2,920 | 1.24× | Win |
| Discord (minified) | 1,859 | 1,542 | 1.21× | Win |
| Discord (prettified) | 2,528 | 2,210 | 1.14× | Win |
| Github Events (minified) | 1,533 | 1,530 | 1.00× | Tie |
| Google Maps Response (minified) | 1,906 | 1,293 | 1.47× | Win |
| Github Events (prettified) | 1,832 | 1,920 | 0.95× | **Loss** |
| Google Maps Response (prettified) | 3,511 | 2,556 | 1.37× | Win |
| Instruments (minified) | 2,265 | 1,841 | 1.23× | Win |
| Instruments (prettified) | 3,425 | 2,931 | 1.17× | Win |
| Marine IK Reverse (minified) | 729 | 723 | 1.01× | Tie |
| Marine IK Reverse (prettified) | 2,562 | 2,521 | 1.02× | Tie |
| Marine IK (minified) | 729 | 699 | 1.04× | Win |
| Marine IK (prettified) | 2,618 | 2,526 | 1.04× | Win |
| Random (minified) | 1,139 | 1,091 | 1.04× | Win |
| Random (prettified) | 1,710 | 1,657 | 1.03× | Win |
| Twitter (minified) | 1,678 | 1,756 | 0.96× | **Loss** |
| Twitter (prettified) | 2,185 | 2,274 | 0.96× | **Loss** |
| Apache Builds Small (minified) | 2,152 | 1,693 | 1.27× | Win |
| Canada Small (minified) | 973 | 939 | 1.04× | Win |
| Apache Builds Small (prettified) | 2,535 | 2,104 | 1.20× | Win |
| Canada Small (prettified) | 2,252 | 2,222 | 1.01× | Win |
| CitmCatalog Small (minified) | 1,491 | 1,203 | 1.24× | Win |
| CitmCatalog Small (prettified) | 2,461 | 2,075 | 1.19× | Win |
| Discord Small (minified) | 1,677 | 1,382 | 1.21× | Win |
| Discord Small (prettified) | 2,208 | 1,857 | 1.19× | Win |
| Github Events Small (minified) | 1,760 | 1,849 | 0.95× | **Loss** |
| Google Maps Response Small (minified) | 1,799 | 1,300 | 1.38× | Win |
| Github Events Small (prettified) | 2,251 | 2,317 | 0.97× | **Loss** |
| Google Maps Response Small (prettified) | 3,320 | 2,562 | 1.30× | Win |
| Instruments Small (minified) | 2,212 | 1,738 | 1.27× | Win |
| Instruments Small (prettified) | 3,358 | 2,770 | 1.21× | Win |
| Marine IK Reverse Small (minified) | 748 | 764 | 0.98× | **Loss** |
| Marine IK Reverse Small (prettified) | 2,599 | 2,702 | 0.96× | **Loss** |
| Marine IK Small (minified) | 762 | 746 | 1.02× | Win |
| Marine IK Small (prettified) | 2,708 | 2,649 | 1.02× | Win |
| Random Small (minified) | 1,591 | 1,231 | 1.29× | Win |
| Random Small (prettified) | 2,351 | 1,886 | 1.25× | Win |
| Twitter Small (minified) | 2,267 | 2,156 | 1.05× | Win |
| Twitter Small (prettified) | 2,865 | 2,665 | 1.08× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,029 | 883 | 1.17× | Win |
| Canada (minified) | 795 | 531 | 1.50× | Win |
| Apache Builds (prettified) | 1,278 | 799 (n/c) | 1.60× | Win |
| Canada (prettified) | 1,680 | 1,426 | 1.18× | Win |
| CitmCatalog (minified) | 1,425 | 722 | 1.97× | Win |
| CitmCatalog (prettified) | 2,684 | 1,597 | 1.68× | Win |
| Discord (minified) | 1,228 | 1,005 | 1.22× | Win |
| Discord (prettified) | 1,768 | 1,514 | 1.17× | Win |
| Github Events (minified) | 1,444 | 1,288 | 1.12× | Win |
| Google Maps Response (minified) | 1,195 | 922 | 1.30× | Win |
| Github Events (prettified) | 1,620 | 1,387 | 1.17× | Win |
| Google Maps Response (prettified) | 1,730 | 1,459 | 1.19× | Win |
| Instruments (minified) | 1,634 | 1,180 | 1.38× | Win |
| Instruments (prettified) | 1,997 | 1,983 | 1.01× | Tie |
| Marine IK Reverse (minified) | 518 | 449 | 1.15× | Win |
| Marine IK Reverse (prettified) | 1,887 | 1,773 | 1.06× | Win |
| Marine IK (minified) | 548 | 443 (n/c) | 1.24× | Win |
| Marine IK (prettified) | 2,064 | 1,554 (n/c) | 1.33× | Win |
| Random (minified) | 879 | 682 | 1.29× | Win |
| Random (prettified) | 1,064 | 1,176 | 0.90× | **Loss** |
| Twitter (minified) | 1,613 | 1,126 | 1.43× | Win |
| Twitter (prettified) | 1,980 | 1,642 | 1.21× | Win |
| Apache Builds Small (minified) | 1,162 | 773 | 1.50× | Win |
| Canada Small (minified) | 761 | 484 | 1.57× | Win |
| Apache Builds Small (prettified) | 1,386 | 981 | 1.41× | Win |
| Canada Small (prettified) | 1,375 | 1,117 | 1.23× | Win |
| CitmCatalog Small (minified) | 1,005 | 669 | 1.50× | Win |
| CitmCatalog Small (prettified) | 1,668 | 1,388 | 1.20× | Win |
| Discord Small (minified) | 1,126 | 983 | 1.15× | Win |
| Discord Small (prettified) | 1,407 | 1,367 | 1.03× | Win |
| Github Events Small (minified) | 1,247 | 920 | 1.36× | Win |
| Google Maps Response Small (minified) | 1,142 | 1,027 | 1.11× | Win |
| Github Events Small (prettified) | 1,491 | 1,450 | 1.03× | Tie |
| Google Maps Response Small (prettified) | 2,304 | 1,616 | 1.43× | Win |
| Instruments Small (minified) | 1,397 | 1,186 | 1.18× | Win |
| Instruments Small (prettified) | 2,282 | 1,673 | 1.36× | Win |
| Marine IK Reverse Small (minified) | 508 | 534 | 0.95× | **Loss** |
| Marine IK Reverse Small (prettified) | 1,830 | 1,817 | 1.01× | Tie |
| Marine IK Small (minified) | 497 | 516 | 0.96× | **Loss** |
| Marine IK Small (prettified) | 1,517 | 1,649 | 0.92× | **Loss** |
| Random Small (minified) | 1,000 | 815 | 1.23× | Win |
| Random Small (prettified) | 879 | 1,031 | 0.85× | **Loss** |
| Twitter Small (minified) | 1,869 | 1,403 | 1.33× | Win |
| Twitter Small (prettified) | 1,959 | 1,501 | 1.31× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 2,197 | 1,722 | 1.28× | Win |
| Canada (minified) | 1,231 | 1,116 | 1.10× | Win |
| Apache Builds (prettified) | 2,765 | 2,175 | 1.27× | Win |
| Canada (prettified) | 2,638 | 2,403 | 1.10× | Win |
| CitmCatalog (minified) | 2,177 | 1,633 | 1.33× | Win |
| CitmCatalog (prettified) | 4,268 | 3,346 | 1.28× | Win |
| Discord (minified) | 2,438 | 1,762 | 1.38× | Win |
| Discord (prettified) | 3,353 | 2,508 | 1.34× | Win |
| Github Events (minified) | 2,044 | 1,909 | 1.07× | Win |
| Google Maps Response (minified) | 2,110 | 1,370 | 1.54× | Win |
| Github Events (prettified) | 2,353 | 2,272 | 1.04× | Win |
| Google Maps Response (prettified) | 3,795 | 2,717 | 1.40× | Win |
| Instruments (minified) | 2,561 | 1,895 | 1.35× | Win |
| Instruments (prettified) | 3,777 | 3,000 | 1.26× | Win |
| Marine IK Reverse (minified) | 802 | 814 | 0.99× | **Loss** |
| Marine IK Reverse (prettified) | 2,829 | 2,782 | 1.02× | Win |
| Marine IK (minified) | 806 | 809 | 1.00× | Tie |
| Marine IK (prettified) | 2,862 | 2,824 | 1.01× | Win |
| Random (minified) | 1,527 | 1,422 | 1.07× | Win |
| Random (prettified) | 2,274 | 2,124 | 1.07× | Win |
| Twitter (minified) | 2,142 | 2,054 | 1.04× | Win |
| Twitter (prettified) | 2,723 | 2,689 | 1.01× | Win |
| Apache Builds Small (minified) | 2,450 | 1,858 | 1.32× | Win |
| Canada Small (minified) | 1,206 | 1,131 | 1.07× | Win |
| Apache Builds Small (prettified) | 2,973 | 2,298 | 1.29× | Win |
| Canada Small (prettified) | 2,703 | 2,633 | 1.03× | Win |
| CitmCatalog Small (minified) | 1,762 | 1,319 | 1.34× | Win |
| CitmCatalog Small (prettified) | 2,876 | 2,230 | 1.29× | Win |
| Discord Small (minified) | 2,066 | 1,579 | 1.31× | Win |
| Discord Small (prettified) | 2,648 | 2,118 | 1.25× | Win |
| Github Events Small (minified) | 2,111 | 2,153 | 0.98× | **Loss** |
| Google Maps Response Small (minified) | 2,016 | 1,425 | 1.41× | Win |
| Github Events Small (prettified) | 2,649 | 2,607 | 1.02× | Win |
| Google Maps Response Small (prettified) | 3,681 | 2,778 | 1.33× | Win |
| Instruments Small (minified) | 2,370 | 1,788 | 1.33× | Win |
| Instruments Small (prettified) | 3,563 | 2,857 | 1.25× | Win |
| Marine IK Reverse Small (minified) | 883 | 869 | 1.02× | Win |
| Marine IK Reverse Small (prettified) | 3,024 | 3,015 | 1.00× | Tie |
| Marine IK Small (minified) | 899 | 859 | 1.05× | Win |
| Marine IK Small (prettified) | 3,177 | 3,002 | 1.06× | Win |
| Random Small (minified) | 1,949 | 1,417 | 1.38× | Win |
| Random Small (prettified) | 2,872 | 2,146 | 1.34× | Win |
| Twitter Small (minified) | 2,509 | 2,253 | 1.11× | Win |
| Twitter Small (prettified) | 3,175 | 2,916 | 1.09× | Win |

**macOS / GCC 16.2 (Apple M2 Pro, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Apache Builds (minified) | 1,508 | 1,067 | 1.41× | Win |
| Canada (minified) | 972 | 695 | 1.40× | Win |
| Apache Builds (prettified) | 1,852 | 1,344 | 1.38× | Win |
| Canada (prettified) | 1,998 | 1,558 | 1.28× | Win |
| CitmCatalog (minified) | 1,651 | 1,306 | 1.26× | Win |
| CitmCatalog (prettified) | 3,064 | 2,460 | 1.25× | Win |
| Discord (minified) | 1,452 | 1,107 | 1.31× | Win |
| Discord (prettified) | 2,194 | 1,690 | 1.30× | Win |
| Github Events (minified) | 1,904 | 1,571 | 1.21× | Win |
| Google Maps Response (minified) | 1,365 | 995 | 1.37× | Win |
| Github Events (prettified) | 2,119 | 1,585 | 1.34× | Win |
| Google Maps Response (prettified) | 2,324 | 1,451 | 1.60× | Win |
| Instruments (minified) | 1,719 | 1,143 | 1.50× | Win |
| Instruments (prettified) | 2,348 | 2,035 | 1.15× | Win |
| Marine IK Reverse (minified) | 599 | 507 | 1.18× | Win |
| Marine IK Reverse (prettified) | 2,103 | 1,947 | 1.08× | Win |
| Marine IK (minified) | 612 | 504 | 1.21× | Win |
| Marine IK (prettified) | 2,318 | 1,536 | 1.51× | Win |
| Random (minified) | 1,258 | 867 | 1.45× | Win |
| Random (prettified) | 1,696 | 1,210 | 1.40× | Win |
| Twitter (minified) | 1,939 | 1,430 | 1.36× | Win |
| Twitter (prettified) | 2,058 | 1,619 | 1.27× | Win |
| Apache Builds Small (minified) | 1,474 | 1,074 | 1.37× | Win |
| Canada Small (minified) | 780 | 606 | 1.29× | Win |
| Apache Builds Small (prettified) | 1,598 | 1,257 | 1.27× | Win |
| Canada Small (prettified) | 1,754 | 1,359 | 1.29× | Win |
| CitmCatalog Small (minified) | 1,086 | 1,104 | 0.98× | Tie |
| CitmCatalog Small (prettified) | 1,723 | 1,491 | 1.16× | Win |
| Discord Small (minified) | 1,658 | 1,188 | 1.39× | Win |
| Discord Small (prettified) | 2,185 | 1,649 | 1.32× | Win |
| Github Events Small (minified) | 1,261 | 1,096 | 1.15× | Win |
| Google Maps Response Small (minified) | 1,165 | 1,075 | 1.08× | Win |
| Github Events Small (prettified) | 1,988 | 1,719 | 1.16× | Win |
| Google Maps Response Small (prettified) | 2,311 | 1,867 | 1.24× | Win |
| Instruments Small (minified) | 1,685 | 1,282 | 1.31× | Win |
| Instruments Small (prettified) | 2,543 | 1,669 | 1.52× | Win |
| Marine IK Reverse Small (minified) | 620 | 565 | 1.10× | Win |
| Marine IK Reverse Small (prettified) | 1,736 | 1,717 | 1.01× | Tie |
| Marine IK Small (minified) | 633 | 530 | 1.20× | Win |
| Marine IK Small (prettified) | 2,347 | 2,122 | 1.11× | Win |
| Random Small (minified) | 1,357 | 971 | 1.40× | Win |
| Random Small (prettified) | 1,924 | 1,215 | 1.58× | Win |
| Twitter Small (minified) | 1,988 | 1,606 | 1.24× | Win |
| Twitter Small (prettified) | 2,639 | 1,966 | 1.34× | Win |

In the freshly allocated run, two-stage Jsonifier wins 70 of these 88 tests, ties 6 and loses 12, every loss by 2% to 15%: seven on Linux/GCC (prettified Github Events, plus Github Events Small, Twitter and Marine IK Reverse Small in both forms) and five on macOS/GCC (prettified Random, prettified Random Small, the widest at 0.85×, Marine IK Small in both forms and minified Marine IK Reverse Small). With the fused path instead, Jsonifier beats the reflection reader on 71 and loses 15. With objects reused, two-stage Jsonifier wins 82, ties 4 and loses 2, both on Linux/GCC (minified Marine IK Reverse by 1% and minified Github Events Small by 2%), and the fused path beats the reflection reader on 73 and loses 13. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 8% on 35 of the 40 results; all five wider gaps are reused results on the macOS builds, and the widest is reused minified Twitter Small on macOS/Clang, where the two-stage row runs at 2,906 MB/s against the "jsonifier" row's 3,783. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 6,339 | 1,654 | 3.83× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,359 | 2,086 | 3.53× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,140 | 2,080 (n/c) | 2.47× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 5,941 | 2,661 | 2.23× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,104 | 6,235 | 1.30× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,373 | 7,475 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,557 | 5,250 | 1.06× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,372 | 6,565 | 0.97× | Loss |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,366 | 5,944 | 1.41× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,297 | 7,255 | 1.28× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,042 | 4,942 | 1.22× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,973 | 5,926 | 1.18× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,808 | 3,226 | 1.49× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,342 | 3,851 | 1.39× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,816 | 3,282 | 1.16× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,199 | 3,732 | 1.13× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,517 | 3,574 | 1.26× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,784 | 3,807 | 1.26× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 3,538 | 3,189 | 1.11× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 3,984 | 4,054 | 0.98× | Loss |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 6,545 | 1,809 | 3.62× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,444 | 2,245 | 3.32× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,616 | 2,215 | 2.54× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,435 | 2,756 | 2.33× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 8,237 | 6,322 | 1.30× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,520 | 7,555 | 1.26× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,815 | 5,383 | 1.08× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,828 | 6,730 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 8,491 | 5,959 | 1.42× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,402 | 7,301 | 1.29× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,281 | 5,081 | 1.24× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,325 | 6,271 | 1.17× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (minified) | 4,924 | 3,316 | 1.48× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter (prettified) | 5,047 | 3,996 | 1.26× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (minified) | 4,064 | 3,245 | 1.25× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,571 | 3,976 | 1.15× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (minified) | 4,556 | 3,546 | 1.28× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter (prettified) | 4,298 | 3,967 | 1.08× | Win |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (minified) | 2,906 | 3,663 | 0.79× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | Twitter Small (prettified) | 4,356 | 4,238 | 1.03× | Win |

Jsonifier wins 37 of the 40 results and loses 3, by up to 3.83× (minified Twitter under MSVC, freshly allocated). Freshly allocated, the losses are prettified Twitter Small on Linux/Clang (0.97×) and on macOS/Clang (0.98×). The reused run has 19 wins and 1 loss (minified Twitter Small on macOS/Clang, 0.79×). All 20 full-size results are wins. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on four of the five builds and loses on macOS/GCC (0.92×):

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 2,642 | 2,307 | 1.15× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,506 | 2,707 | 1.30× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 3,022 | 2,446 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M2 Pro, NEON) | 1,854 | 2,012 | 0.92× | Loss |
| macOS / Clang 23.1 (Apple M2 Pro, NEON) | 2,221 | 1,735 | 1.28× | Win |

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