# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model, a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2), has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization, so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 225 tests on five builds, skipping the tape wins on POD-type data on every build and on 97 of the 100 minified documents, the tape is faster on 57 of the 60 prettified documents on x86, and on the Apple M1 the fused path wins 29 of the 40 prettified documents.

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

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 23, 2026, with Jsonifier [b975cd7](https://github.com/nihilai-collective/jsonifier/commit/b975cd7) and BenchmarkSuite [ab75024](https://github.com/nihilai-collective/benchmarksuite/commit/ab75024) on every build. Glaze is [628bec1](https://github.com/stephenberry/glaze/commit/628bec1) on every build except macOS/Clang, which ran [1f8adf7](https://github.com/stephenberry/glaze/commit/1f8adf7). simdjson is [70f83c9](https://github.com/simdjson/simdjson/commit/70f83c9) on the two Linux builds, [2cc1e70](https://github.com/simdjson/simdjson/commit/2cc1e70) on Windows/MSVC and macOS/GCC, and [957ddbc](https://github.com/simdjson/simdjson/commit/957ddbc) on macOS/Clang. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 45 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), ten corpus documents in minified and prettified form, and "Small" cut-down copies of the same ten documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the ten documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M1), and non-converged results stay in the rankings and tallies, marked n/c in the tables. All fifteen non-converged full-parse rows are on the two macOS builds. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Keeping the quietest epoch raises absolute throughput somewhat, identically for every library, and resolves small differences as wins or losses more often than a first-to-converge rule would, which is why ties are rare.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 154 tests, 5 are statistical ties, and the two-stage path wins 66. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 0 | 20 | 45 of 45 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 0 | 18 | 45 of 45 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 25 | 0 | 20 | 45 of 45 |
| macOS / GCC 16.2 (Apple M1, NEON) | 38 | 4 | 3 | 38 of 45 |
| macOS / Clang 23.1 (Apple M1, NEON) | 39 | 1 | 5 | 42 of 45 |
| **Aggregate** | **154** | **5** | **66** | **215 of 225** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 19 / 0 / 1 | 19 / 1 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 3 / 0 / 17 | 15 / 3 / 2 |
| Small minified documents | 10 / 0 / 0 | 20 / 0 / 0 | 19 / 1 / 0 |
| Small prettified documents | 0 / 0 / 10 | 0 / 0 / 20 | 14 / 0 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 45 / 0 / 0 | 45 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 45 / 0 / 0 | 37 / 0 / 8 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 43 / 0 / 2 | 43 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 45 / 0 / 0 | 43 / 2 / 0 |
| macOS / Clang 23.1 (Apple M1, NEON) | 43 / 1 / 1 | 42 / 2 / 1 |
| **Aggregate** | **221 / 1 / 3** | **210 / 4 / 11** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 0 | 20 | 45 of 45 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 | 2 | 18 | 45 of 45 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 | 1 | 21 | 45 of 45 |
| macOS / GCC 16.2 (Apple M1, NEON) | 40 | 4 | 1 | 40 of 45 |
| macOS / Clang 23.1 (Apple M1, NEON) | 37 | 1 | 7 | 45 of 45 |
| **Aggregate** | **150** | **8** | **67** | **220 of 225** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 17 / 0 / 3 | 20 / 0 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 1 / 3 / 16 | 15 / 3 / 2 |
| Small minified documents | 10 / 0 / 0 | 20 / 0 / 0 | 20 / 0 / 0 |
| Small prettified documents | 0 / 0 / 10 | 0 / 0 / 20 | 12 / 2 / 6 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 45 / 0 / 0 | 45 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 44 / 0 / 1 | 36 / 1 / 8 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 42 / 0 / 3 | 42 / 0 / 3 |
| macOS / GCC 16.2 (Apple M1, NEON) | 44 / 0 / 1 | 44 / 0 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 44 / 0 / 1 | 43 / 1 / 1 |
| **Aggregate** | **219 / 0 / 6** | **210 / 2 / 13** |

Across 225 tests the fused path wins 150, ties 8 and loses 67 to the two-stage path, and the x86 split holds: the tape wins 26 of the 30 full-size prettified documents (3 ties, and the fused path takes Marine IK Reverse on Linux/Clang) and all 30 Small prettified ones, and the fused path wins all 30 Small minified documents and 27 of the 30 full-size minified ones (the tape takes Twitter on Linux/Clang and Canada and Mesh on Linux/GCC). On the M1 the fused path wins all 40 minified documents and 27 of the 40 prettified ones, with 5 ties and 8 tape wins. Against simdjson, the faster Jsonifier path wins 219 and loses 6, and the fused path alone wins 210, ties 2 and loses 13.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 462 | 133 | 123 | 3.48× | Fused | Win |
| Double (POD) | 539 | 303 | 166 | 1.78× | Fused | Win |
| Int64 (POD) | 1,531 | 556 | 378 | 2.75× | Fused | Win |
| String (POD) | 974 | 699 | 638 | 1.39× | Fused | Win |
| Uint64 (POD) | 1,585 | 576 | 376 | 2.75× | Fused | Win |
| Canada (minified) | 589 | 567 | 372 | 1.04× | Fused | Win |
| Canada (prettified) | 1,564 | 1,699 | 1,122 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 1,859 | 1,329 | 517 | 1.40× | Fused | Win |
| CitmCatalog (prettified) | 2,946 | 3,215 | 1,384 | 0.92× | Two-stage | Win |
| Discord (minified) | 1,225 | 1,078 | 606 | 1.14× | Fused | Win |
| Discord (prettified) | 1,464 | 1,706 | 988 | 0.86× | Two-stage | Win |
| Google Maps Response (minified) | 1,511 | 1,199 | 503 | 1.26× | Fused | Win |
| Google Maps Response (prettified) | 2,440 | 2,846 | 1,243 | 0.86× | Two-stage | Win |
| Instruments (minified) | 2,082 | 1,476 | 679 | 1.41× | Fused | Win |
| Instruments (prettified) | 2,225 | 2,669 | 1,261 | 0.83× | Two-stage | Win |
| Marine IK Reverse (minified) | 494 | 456 | 37 | 1.08× | Fused | Win |
| Marine IK Reverse (prettified) | 2,019 | 2,188 | 195 | 0.92× | Two-stage | Win |
| Marine IK (minified) | 513 | 466 | 261 | 1.10× | Fused | Win |
| Marine IK (prettified) | 2,052 | 2,230 | 1,283 | 0.92× | Two-stage | Win |
| Mesh (minified) | 935 | 817 | 448 | 1.14× | Fused | Win |
| Mesh (prettified) | 1,205 | 1,473 | 833 | 0.82× | Two-stage | Win |
| Random (minified) | 967 | 898 | 519 | 1.08× | Fused | Win |
| Random (prettified) | 1,391 | 1,589 | 950 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,380 | 1,213 | 814 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,488 | 1,742 | 1,188 | 0.85× | Two-stage | Win |
| Canada Small (minified) | 575 | 546 | 337 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,514 | 1,609 | 1,005 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 997 | 845 | 475 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,590 | 1,767 | 1,048 | 0.90× | Two-stage | Win |
| Discord Small (minified) | 1,147 | 975 | 634 | 1.18× | Fused | Win |
| Discord Small (prettified) | 1,291 | 1,371 | 942 | 0.94× | Two-stage | Win |
| Google Maps Response Small (minified) | 1,347 | 1,071 | 495 | 1.26× | Fused | Win |
| Google Maps Response Small (prettified) | 2,227 | 2,597 | 1,246 | 0.86× | Two-stage | Win |
| Instruments Small (minified) | 2,090 | 1,421 | 650 | 1.47× | Fused | Win |
| Instruments Small (prettified) | 2,030 | 2,532 | 1,211 | 0.80× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 442 | 398 | 39 | 1.11× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,766 | 1,914 | 204 | 0.92× | Two-stage | Win |
| Marine IK Small (minified) | 475 | 423 | 250 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 1,868 | 2,000 | 1,225 | 0.93× | Two-stage | Win |
| Mesh Small (minified) | 701 | 644 | 241 | 1.09× | Fused | Win |
| Mesh Small (prettified) | 1,049 | 1,240 | 493 | 0.85× | Two-stage | Win |
| Random Small (minified) | 1,143 | 975 | 507 | 1.17× | Fused | Win |
| Random Small (prettified) | 1,561 | 1,778 | 943 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 1,576 | 1,339 | 948 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 1,652 | 1,858 | 1,353 | 0.89× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,954 | 317 | 226 | 6.16× | Fused | Win |
| Double (POD) | 1,225 | 406 | 263 | 3.02× | Fused | Win |
| Int64 (POD) | 3,088 | 898 | 586 | 3.44× | Fused | Win |
| String (POD) | 1,817 | 1,135 | 1,102 | 1.60× | Fused | Win |
| Uint64 (POD) | 3,396 | 931 | 548 | 3.65× | Fused | Win |
| Canada (minified) | 977 | 892 | 651 | 1.10× | Fused | Win |
| Canada (prettified) | 2,604 | 2,624 | 1,909 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,471 | 1,819 | 1,215 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 3,780 | 4,276 | 3,059 | 0.88× | Two-stage | Win |
| Discord (minified) | 2,236 | 1,795 | 1,438 | 1.25× | Fused | Win |
| Discord (prettified) | 2,128 | 2,808 | 2,240 | 0.76× | Two-stage | Win |
| Google Maps Response (minified) | 2,693 | 1,970 | 1,341 | 1.37× | Fused | Win |
| Google Maps Response (prettified) | 3,627 | 4,313 | 3,172 | 0.84× | Two-stage | Win |
| Instruments (minified) | 2,698 | 1,995 | 1,695 | 1.35× | Fused | Win |
| Instruments (prettified) | 2,761 | 3,450 | 3,115 | 0.80× | Two-stage | Win |
| Marine IK Reverse (minified) | 851 | 738 | 615 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 3,400 | 3,284 | 2,879 | 1.04× | Fused | Win |
| Marine IK (minified) | 861 | 748 | 612 | 1.15× | Fused | Win |
| Marine IK (prettified) | 3,364 | 3,280 | 2,768 | 1.03× | Fused | Win |
| Mesh (minified) | 1,386 | 1,197 | 1,169 | 1.16× | Fused | Win |
| Mesh (prettified) | 1,934 | 2,224 | 2,206 | 0.87× | Two-stage | Win |
| Random (minified) | 1,705 | 1,422 | 1,074 | 1.20× | Fused | Win |
| Random (prettified) | 2,195 | 2,413 | 1,935 | 0.91× | Two-stage | Win |
| Twitter (minified) | 2,034 | 1,873 | 1,755 | 1.09× | Fused | Win |
| Twitter (prettified) | 1,982 | 2,609 | 2,502 | 0.76× | Two-stage | Win |
| Canada Small (minified) | 1,015 | 940 | 719 | 1.08× | Fused | Win |
| Canada Small (prettified) | 2,562 | 2,675 | 2,067 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,920 | 1,536 | 1,232 | 1.25× | Fused | Win |
| CitmCatalog Small (prettified) | 2,628 | 3,116 | 2,597 | 0.84× | Two-stage | Win |
| Discord Small (minified) | 2,086 | 1,792 | 1,458 | 1.16× | Fused | Win |
| Discord Small (prettified) | 1,999 | 2,556 | 2,132 | 0.78× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,471 | 1,864 | 1,332 | 1.33× | Fused | Win |
| Google Maps Response Small (prettified) | 3,334 | 4,085 | 3,106 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,763 | 1,901 | 1,591 | 1.45× | Fused | Win |
| Instruments Small (prettified) | 2,659 | 3,415 | 2,919 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 817 | 732 | 592 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,021 | 3,254 | 2,764 | 0.93× | Two-stage | Win |
| Marine IK Small (minified) | 910 | 798 | 596 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 3,127 | 3,524 | 2,791 | 0.89× | Two-stage | Win |
| Mesh Small (minified) | 985 | 889 | 790 | 1.11× | Fused | Win |
| Mesh Small (prettified) | 1,515 | 1,732 | 1,568 | 0.87× | Two-stage | Win |
| Random Small (minified) | 2,254 | 1,803 | 1,142 | 1.25× | Fused | Win |
| Random Small (prettified) | 2,571 | 3,196 | 2,087 | 0.80× | Two-stage | Win |
| Twitter Small (minified) | 2,651 | 2,334 | 2,266 | 1.14× | Fused | Win |
| Twitter Small (prettified) | 2,310 | 3,176 | 3,130 | 0.73× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,180 | 274 | 194 | 7.95× | Fused | Win |
| Double (POD) | 1,175 | 373 | 232 | 3.15× | Fused | Win |
| Int64 (POD) | 3,338 | 821 | 537 | 4.07× | Fused | Win |
| String (POD) | 1,759 | 1,015 | 1,035 | 1.73× | Fused | Win |
| Uint64 (POD) | 3,420 | 840 | 536 | 4.07× | Fused | Win |
| Canada (minified) | 911 | 905 | 772 | 1.01× | Fused | Win |
| Canada (prettified) | 2,389 | 2,636 | 2,273 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,442 | 1,847 | 1,365 | 1.32× | Fused | Win |
| CitmCatalog (prettified) | 3,880 | 4,332 | 3,368 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,356 | 1,853 | 1,447 | 1.27× | Fused | Win |
| Discord (prettified) | 2,462 | 2,845 | 2,251 | 0.87× | Two-stage | Win |
| Google Maps Response (minified) | 2,679 | 1,944 | 1,102 | 1.38× | Fused | Win |
| Google Maps Response (prettified) | 4,248 | 4,258 | 2,607 | 1.00× | Two-stage | Win |
| Instruments (minified) | 3,382 | 2,333 | 1,385 | 1.45× | Fused | Win |
| Instruments (prettified) | 3,479 | 3,886 | 2,535 | 0.90× | Two-stage | Win |
| Marine IK Reverse (minified) | 751 | 716 | 589 | 1.05× | Fused | Win |
| Marine IK Reverse (prettified) | 3,225 | 3,198 | 2,729 | 1.01× | Fused | Win |
| Marine IK (minified) | 762 | 725 | 596 | 1.05× | Fused | Win |
| Marine IK (prettified) | 3,279 | 3,300 | 2,790 | 0.99× | Two-stage | Win |
| Mesh (minified) | 1,073 | 1,151 | 1,209 | 0.93× | Two-stage | Loss |
| Mesh (prettified) | 1,775 | 2,031 | 2,222 | 0.87× | Two-stage | Loss |
| Random (minified) | 1,450 | 1,368 | 845 | 1.06× | Fused | Win |
| Random (prettified) | 2,010 | 2,426 | 1,536 | 0.83× | Two-stage | Win |
| Twitter (minified) | 2,041 | 1,748 | 1,332 | 1.17× | Fused | Win |
| Twitter (prettified) | 2,096 | 2,478 | 1,925 | 0.85× | Two-stage | Win |
| Canada Small (minified) | 1,051 | 955 | 789 | 1.10× | Fused | Win |
| Canada Small (prettified) | 2,530 | 2,694 | 2,263 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,870 | 1,569 | 1,198 | 1.19× | Fused | Win |
| CitmCatalog Small (prettified) | 2,709 | 3,192 | 2,533 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 1,908 | 1,632 | 1,283 | 1.17× | Fused | Win |
| Discord Small (prettified) | 2,001 | 2,324 | 1,867 | 0.86× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,404 | 1,814 | 1,124 | 1.33× | Fused | Win |
| Google Maps Response Small (prettified) | 3,665 | 3,893 | 2,649 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,331 | 2,239 | 1,248 | 1.49× | Fused | Win |
| Instruments Small (prettified) | 3,315 | 3,742 | 2,370 | 0.89× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 828 | 715 | 567 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,983 | 3,317 | 2,683 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 914 | 761 | 571 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 3,184 | 3,476 | 2,658 | 0.92× | Two-stage | Win |
| Mesh Small (minified) | 1,008 | 888 | 737 | 1.14× | Fused | Win |
| Mesh Small (prettified) | 1,518 | 1,711 | 1,512 | 0.89× | Two-stage | Win |
| Random Small (minified) | 2,213 | 1,712 | 903 | 1.29× | Fused | Win |
| Random Small (prettified) | 2,822 | 3,032 | 1,641 | 0.93× | Two-stage | Win |
| Twitter Small (minified) | 2,973 | 2,151 | 1,628 | 1.38× | Fused | Win |
| Twitter Small (prettified) | 2,517 | 2,995 | 2,280 | 0.84× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,516 | 218 | 184 | 6.96× | Fused | Win |
| Double (POD) | 886 | 272 | 143 | 3.25× | Fused | Win |
| Int64 (POD) | 2,146 | 580 | 433 | 3.70× | Fused | Win |
| String (POD) | 1,123 | 601 | 736 | 1.87× | Fused | Win |
| Uint64 (POD) | 2,104 | 571 | 416 | 3.68× | Fused | Win |
| Canada (minified) | 619 | 600 | 436 | 1.03× | Tie | Win |
| Canada (prettified) | 1,630 | 1,589 (n/c) | 1,137 (n/c) | 1.03× | Tie | Win |
| CitmCatalog (minified) | 1,751 | 1,154 | 977 | 1.52× | Fused | Win |
| CitmCatalog (prettified) | 2,935 | 2,576 | 1,963 (n/c) | 1.14× | Fused | Win |
| Discord (minified) | 1,490 | 1,039 | 819 | 1.43× | Fused | Win |
| Discord (prettified) | 1,904 | 1,609 | 1,302 | 1.18× | Fused | Win |
| Google Maps Response (minified) | 1,679 | 761 | 649 | 2.21× | Fused | Win |
| Google Maps Response (prettified) | 2,668 | 1,977 | 1,492 | 1.35× | Fused | Win |
| Instruments (minified) | 2,354 | 1,396 | 1,079 | 1.69× | Fused | Win |
| Instruments (prettified) | 2,738 | 2,299 | 1,833 | 1.19× | Fused | Win |
| Marine IK Reverse (minified) | 530 (n/c) | 464 | 405 | 1.14× | Fused | Win |
| Marine IK Reverse (prettified) | 1,526 | 1,732 (n/c) | 1,533 | 0.88× | Tie | Win |
| Marine IK (minified) | 508 | 434 | 386 | 1.17× | Fused | Win |
| Marine IK (prettified) | 2,291 | 2,165 | 1,869 | 1.06× | Fused | Win |
| Mesh (minified) | 823 | 725 (n/c) | 696 | 1.14× | Fused | Win |
| Mesh (prettified) | 1,213 | 1,361 | 1,227 | 0.89× | Two-stage | Win |
| Random (minified) | 981 | 761 | 537 | 1.29× | Fused | Win |
| Random (prettified) | 1,398 | 1,090 (n/c) | 973 | 1.28× | Fused | Win |
| Twitter (minified) | 1,594 | 1,475 | 1,091 | 1.08× | Fused | Win |
| Twitter (prettified) | 2,016 | 1,751 | 1,368 | 1.15× | Fused | Win |
| Canada Small (minified) | 724 | 632 | 421 | 1.15× | Fused | Win |
| Canada Small (prettified) | 1,687 | 1,763 | 1,193 | 0.96× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,213 | 862 | 815 | 1.41× | Fused | Win |
| CitmCatalog Small (prettified) | 1,880 | 1,802 | 1,634 | 1.04× | Fused | Win |
| Discord Small (minified) | 1,352 | 1,127 | 772 | 1.20× | Fused | Win |
| Discord Small (prettified) | 1,563 | 1,425 | 1,118 | 1.10× | Fused | Win |
| Google Maps Response Small (minified) | 1,490 | 757 | 677 | 1.97× | Fused | Win |
| Google Maps Response Small (prettified) | 2,540 | 1,913 | 1,525 | 1.33× | Fused | Win |
| Instruments Small (minified) | 2,499 | 1,382 | 1,018 | 1.81× | Fused | Win |
| Instruments Small (prettified) | 2,986 | 2,489 | 1,776 | 1.20× | Fused | Win |
| Marine IK Reverse Small (minified) | 438 (n/c) | 449 | 333 | 0.98× | Tie | Win |
| Marine IK Reverse Small (prettified) | 2,008 | 1,905 | 1,620 | 1.05× | Fused | Win |
| Marine IK Small (minified) | 618 | 498 | 342 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 2,270 | 1,941 | 1,347 | 1.17× | Fused | Win |
| Mesh Small (minified) | 775 | 643 | 448 | 1.21× | Fused | Win |
| Mesh Small (prettified) | 1,190 | 1,233 | 893 | 0.97× | Two-stage | Win |
| Random Small (minified) | 1,419 | 949 | 622 | 1.49× | Fused | Win |
| Random Small (prettified) | 1,890 | 1,428 | 1,181 | 1.32× | Fused | Win |
| Twitter Small (minified) | 2,107 | 1,510 | 1,132 | 1.40× | Fused | Win |
| Twitter Small (prettified) | 2,310 | 1,919 | 1,676 | 1.20× | Fused | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,213 | 246 | 172 | 4.92× | Fused | Win |
| Double (POD) | 954 | 326 | 198 | 2.92× | Fused | Win |
| Int64 (POD) | 1,670 | 463 | 499 | 3.61× | Fused | Win |
| String (POD) | 1,234 | 623 | 884 | 1.98× | Fused | Win |
| Uint64 (POD) | 2,340 | 651 | 386 | 3.59× | Fused | Win |
| Canada (minified) | 647 | 562 | 453 | 1.15× | Fused | Win |
| Canada (prettified) | 1,752 (n/c) | 1,760 | 1,247 | 1.00× | Tie | Win |
| CitmCatalog (minified) | 2,006 | 1,399 | 958 | 1.43× | Fused | Win |
| CitmCatalog (prettified) | 3,512 | 2,927 | 1,873 (n/c) | 1.20× | Fused | Win |
| Discord (minified) | 2,197 | 1,198 | 1,241 | 1.83× | Fused | Win |
| Discord (prettified) | 2,177 | 2,020 | 2,187 | 1.08× | Fused | Tie |
| Google Maps Response (minified) | 2,254 | 1,046 | 809 | 2.16× | Fused | Win |
| Google Maps Response (prettified) | 3,748 | 2,345 | 1,931 | 1.60× | Fused | Win |
| Instruments (minified) | 2,326 | 1,572 | 1,051 | 1.48× | Fused | Win |
| Instruments (prettified) | 3,000 | 2,573 | 2,355 | 1.17× | Fused | Win |
| Marine IK Reverse (minified) | 580 | 527 | 393 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,464 | 2,365 | 1,749 | 1.04× | Fused | Win |
| Marine IK (minified) | 681 | 546 | 407 | 1.25× | Fused | Win |
| Marine IK (prettified) | 2,613 | 2,560 | 1,810 | 1.02× | Fused | Win |
| Mesh (minified) | 1,097 | 990 | 836 | 1.11× | Fused | Win |
| Mesh (prettified) | 1,377 | 1,713 | 1,331 | 0.80× | Two-stage | Win |
| Random (minified) | 1,821 | 904 | 857 | 2.01× | Fused | Win |
| Random (prettified) | 2,310 | 1,353 | 1,519 | 1.71× | Fused | Win |
| Twitter (minified) | 2,019 | 1,417 | 1,548 | 1.43× | Fused | Win |
| Twitter (prettified) | 2,261 | 2,026 | 2,080 | 1.12× | Fused | Win |
| Canada Small (minified) | 783 | 699 | 473 | 1.12× | Fused | Win |
| Canada Small (prettified) | 1,592 | 1,403 | 941 | 1.13× | Fused | Win |
| CitmCatalog Small (minified) | 1,344 | 1,015 | 741 | 1.32× | Fused | Win |
| CitmCatalog Small (prettified) | 1,969 | 1,737 | 1,433 | 1.13× | Fused | Win |
| Discord Small (minified) | 1,897 | 1,189 | 1,249 | 1.59× | Fused | Win |
| Discord Small (prettified) | 1,855 | 1,559 | 1,611 | 1.19× | Fused | Win |
| Google Maps Response Small (minified) | 1,935 | 1,147 | 934 | 1.69× | Fused | Win |
| Google Maps Response Small (prettified) | 2,814 | 1,766 | 1,828 | 1.59× | Fused | Win |
| Instruments Small (minified) | 2,746 | 1,481 | 1,037 | 1.85× | Fused | Win |
| Instruments Small (prettified) | 2,790 | 2,630 | 2,075 | 1.06× | Fused | Win |
| Marine IK Reverse Small (minified) | 676 | 566 | 379 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,740 | 1,798 | 1,286 | 0.97× | Two-stage | Win |
| Marine IK Small (minified) | 609 | 537 | 350 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,629 | 1,822 | 1,333 | 0.89× | Two-stage | Win |
| Mesh Small (minified) | 863 | 745 | 522 | 1.16× | Fused | Win |
| Mesh Small (prettified) | 1,164 | 1,213 | 925 | 0.96× | Two-stage | Win |
| Random Small (minified) | 2,304 | 1,170 | 1,041 | 1.97× | Fused | Win |
| Random Small (prettified) | 2,521 | 1,467 | 1,620 | 1.72× | Fused | Win |
| Twitter Small (minified) | 2,342 | 1,741 | 1,619 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 1,909 (n/c) | 2,084 | 2,239 | 0.92× | Two-stage | Loss |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 535 | 142 | 131 | 3.78× | Fused | Win |
| Double (POD) | 685 | 345 | 180 | 1.98× | Fused | Win |
| Int64 (POD) | 2,013 | 621 | 404 | 3.24× | Fused | Win |
| String (POD) | 1,685 | 1,035 | 881 | 1.63× | Fused | Win |
| Uint64 (POD) | 2,013 | 639 | 410 | 3.15× | Fused | Win |
| Canada (minified) | 711 | 660 | 409 | 1.08× | Fused | Win |
| Canada (prettified) | 1,833 | 1,944 | 1,230 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 2,271 | 1,554 | 552 | 1.46× | Fused | Win |
| CitmCatalog (prettified) | 3,282 | 3,627 | 1,443 | 0.90× | Two-stage | Win |
| Discord (minified) | 1,794 | 1,503 | 755 | 1.19× | Fused | Win |
| Discord (prettified) | 1,897 | 2,311 | 1,209 | 0.82× | Two-stage | Win |
| Google Maps Response (minified) | 1,721 | 1,350 | 534 | 1.27× | Fused | Win |
| Google Maps Response (prettified) | 2,702 | 3,253 | 1,322 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,671 | 1,753 | 747 | 1.52× | Fused | Win |
| Instruments (prettified) | 2,524 | 3,103 | 1,385 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 543 | 492 | 37 | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,183 | 2,352 | 196 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 555 | 498 | 272 | 1.11× | Fused | Win |
| Marine IK (prettified) | 2,221 | 2,395 | 1,342 | 0.93× | Two-stage | Win |
| Mesh (minified) | 980 | 855 | 479 | 1.15× | Fused | Win |
| Mesh (prettified) | 1,245 | 1,550 | 888 | 0.80× | Two-stage | Win |
| Random (minified) | 1,445 | 1,261 | 633 | 1.15× | Fused | Win |
| Random (prettified) | 1,853 | 2,190 | 1,147 | 0.85× | Two-stage | Win |
| Twitter (minified) | 1,961 | 1,623 | 1,002 | 1.21× | Fused | Win |
| Twitter (prettified) | 1,902 | 2,347 | 1,449 | 0.81× | Two-stage | Win |
| Canada Small (minified) | 716 | 679 | 378 | 1.05× | Fused | Win |
| Canada Small (prettified) | 1,805 | 1,970 | 1,104 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,342 | 1,053 | 537 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 1,916 | 2,181 | 1,176 | 0.88× | Two-stage | Win |
| Discord Small (minified) | 1,650 | 1,313 | 826 | 1.26× | Fused | Win |
| Discord Small (prettified) | 1,561 | 1,905 | 1,224 | 0.82× | Two-stage | Win |
| Google Maps Response Small (minified) | 1,666 | 1,240 | 550 | 1.34× | Fused | Win |
| Google Maps Response Small (prettified) | 2,509 | 3,076 | 1,363 | 0.82× | Two-stage | Win |
| Instruments Small (minified) | 2,396 | 1,563 | 730 | 1.53× | Fused | Win |
| Instruments Small (prettified) | 2,144 | 2,751 | 1,397 | 0.78× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 541 | 480 | 39 | 1.13× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,065 | 2,323 | 207 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 576 | 506 | 274 | 1.14× | Fused | Win |
| Marine IK Small (prettified) | 2,175 | 2,432 | 1,339 | 0.89× | Two-stage | Win |
| Mesh Small (minified) | 894 | 761 | 402 | 1.17× | Fused | Win |
| Mesh Small (prettified) | 1,261 | 1,521 | 815 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,391 | 1,196 | 619 | 1.16× | Fused | Win |
| Random Small (prettified) | 1,825 | 2,103 | 1,142 | 0.87× | Two-stage | Win |
| Twitter Small (minified) | 2,209 | 1,806 | 1,094 | 1.22× | Fused | Win |
| Twitter Small (prettified) | 1,954 | 2,393 | 1,540 | 0.82× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,153 | 321 | 228 | 6.70× | Fused | Win |
| Double (POD) | 1,331 | 416 | 265 | 3.20× | Fused | Win |
| Int64 (POD) | 3,464 | 928 | 593 | 3.73× | Fused | Win |
| String (POD) | 2,870 | 1,494 | 1,413 | 1.92× | Fused | Win |
| Uint64 (POD) | 3,721 | 948 | 556 | 3.93× | Fused | Win |
| Canada (minified) | 1,214 | 1,068 | 761 | 1.14× | Fused | Win |
| Canada (prettified) | 3,101 | 3,105 | 2,221 | 1.00× | Tie | Win |
| CitmCatalog (minified) | 3,059 | 2,161 | 1,408 | 1.42× | Fused | Win |
| CitmCatalog (prettified) | 4,270 | 4,910 | 3,504 | 0.87× | Two-stage | Win |
| Discord (minified) | 3,267 | 2,496 | 1,784 | 1.31× | Fused | Win |
| Discord (prettified) | 2,581 | 3,689 | 2,809 | 0.70× | Two-stage | Win |
| Google Maps Response (minified) | 3,106 | 2,165 | 1,458 | 1.43× | Fused | Win |
| Google Maps Response (prettified) | 3,833 | 4,659 | 3,387 | 0.82× | Two-stage | Win |
| Instruments (minified) | 3,190 | 2,283 | 1,905 | 1.40× | Fused | Win |
| Instruments (prettified) | 2,989 | 3,883 | 3,465 | 0.77× | Two-stage | Win |
| Marine IK Reverse (minified) | 947 | 823 | 676 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 3,703 | 3,604 | 3,136 | 1.03× | Fused | Win |
| Marine IK (minified) | 968 | 827 | 673 | 1.17× | Fused | Win |
| Marine IK (prettified) | 3,632 | 3,555 | 3,017 | 1.02× | Tie | Win |
| Mesh (minified) | 1,496 | 1,282 | 1,312 | 1.17× | Fused | Win |
| Mesh (prettified) | 2,070 | 2,365 | 2,447 | 0.88× | Two-stage | Loss |
| Random (minified) | 2,627 | 2,028 | 1,396 | 1.30× | Fused | Win |
| Random (prettified) | 2,906 | 3,332 | 2,461 | 0.87× | Two-stage | Win |
| Twitter (minified) | 2,590 | 2,595 | 2,165 | 1.00× | Two-stage | Win |
| Twitter (prettified) | 2,293 | 3,227 | 3,083 | 0.71× | Two-stage | Win |
| Canada Small (minified) | 1,252 | 1,160 | 836 | 1.08× | Fused | Win |
| Canada Small (prettified) | 3,066 | 3,253 | 2,395 | 0.94× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,361 | 1,833 | 1,437 | 1.29× | Fused | Win |
| CitmCatalog Small (prettified) | 2,989 | 3,643 | 2,994 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 2,754 | 2,247 | 1,933 | 1.23× | Fused | Win |
| Discord Small (prettified) | 2,383 | 3,161 | 2,795 | 0.75× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,921 | 2,071 | 1,502 | 1.41× | Fused | Win |
| Google Maps Response Small (prettified) | 3,645 | 4,554 | 3,514 | 0.80× | Two-stage | Win |
| Instruments Small (minified) | 2,991 | 2,045 | 1,737 | 1.46× | Fused | Win |
| Instruments Small (prettified) | 2,815 | 3,669 | 3,180 | 0.77× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,002 | 875 | 675 | 1.14× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,467 | 3,786 | 3,095 | 0.92× | Two-stage | Win |
| Marine IK Small (minified) | 1,133 | 968 | 677 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 3,599 | 4,147 | 3,075 | 0.87× | Two-stage | Win |
| Mesh Small (minified) | 1,211 | 1,057 | 1,044 | 1.15× | Fused | Win |
| Mesh Small (prettified) | 1,755 | 2,054 | 2,040 | 0.85× | Two-stage | Win |
| Random Small (minified) | 2,817 | 2,146 | 1,395 | 1.31× | Fused | Win |
| Random Small (prettified) | 2,931 | 3,720 | 2,500 | 0.79× | Two-stage | Win |
| Twitter Small (minified) | 2,923 | 2,665 | 2,479 | 1.10× | Fused | Win |
| Twitter Small (prettified) | 2,476 | 3,506 | 3,364 | 0.71× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,393 | 280 | 195 | 8.55× | Fused | Win |
| Double (POD) | 1,286 | 384 | 235 | 3.35× | Fused | Win |
| Int64 (POD) | 3,706 | 842 | 538 | 4.40× | Fused | Win |
| String (POD) | 2,706 | 1,268 | 1,287 | 2.13× | Fused | Win |
| Uint64 (POD) | 3,788 | 865 | 541 | 4.38× | Fused | Win |
| Canada (minified) | 1,087 | 1,106 | 933 | 0.98× | Two-stage | Win |
| Canada (prettified) | 2,800 | 3,117 | 2,678 | 0.90× | Two-stage | Win |
| CitmCatalog (minified) | 3,052 | 2,227 | 1,659 | 1.37× | Fused | Win |
| CitmCatalog (prettified) | 4,356 | 5,005 | 4,002 | 0.87× | Two-stage | Win |
| Discord (minified) | 3,205 | 2,448 | 1,815 | 1.31× | Fused | Win |
| Discord (prettified) | 3,116 | 3,653 | 2,925 | 0.85× | Two-stage | Win |
| Google Maps Response (minified) | 3,084 | 2,117 | 1,188 | 1.46× | Fused | Win |
| Google Maps Response (prettified) | 4,559 | 4,581 | 2,747 | 1.00× | Two-stage | Win |
| Instruments (minified) | 4,018 | 2,653 | 1,484 | 1.51× | Fused | Win |
| Instruments (prettified) | 3,830 | 4,349 | 2,692 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 827 | 781 | 658 | 1.06× | Fused | Win |
| Marine IK Reverse (prettified) | 3,517 | 3,507 | 2,977 | 1.00× | Tie | Win |
| Marine IK (minified) | 847 | 801 | 656 | 1.06× | Fused | Win |
| Marine IK (prettified) | 3,528 | 3,577 | 2,969 | 0.99× | Two-stage | Win |
| Mesh (minified) | 1,125 | 1,202 | 1,331 | 0.94× | Two-stage | Loss |
| Mesh (prettified) | 1,850 | 2,162 | 2,408 | 0.86× | Two-stage | Loss |
| Random (minified) | 2,124 | 1,941 | 1,042 | 1.09× | Fused | Win |
| Random (prettified) | 2,627 | 3,351 | 1,900 | 0.78× | Two-stage | Win |
| Twitter (minified) | 2,628 | 2,183 | 1,555 | 1.20× | Fused | Win |
| Twitter (prettified) | 2,521 | 3,033 | 2,230 | 0.83× | Two-stage | Win |
| Canada Small (minified) | 1,346 | 1,182 | 898 | 1.14× | Fused | Win |
| Canada Small (prettified) | 3,028 | 3,241 | 2,562 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,266 | 1,865 | 1,349 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 3,017 | 3,678 | 2,812 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 2,617 | 2,038 | 1,569 | 1.28× | Fused | Win |
| Discord Small (prettified) | 2,551 | 2,885 | 2,277 | 0.88× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,807 | 2,034 | 1,245 | 1.38× | Fused | Win |
| Google Maps Response Small (prettified) | 4,026 | 4,313 | 2,920 | 0.93× | Two-stage | Win |
| Instruments Small (minified) | 3,723 | 2,406 | 1,356 | 1.55× | Fused | Win |
| Instruments Small (prettified) | 3,443 | 3,952 | 2,466 | 0.87× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,010 | 854 | 628 | 1.18× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,478 | 3,907 | 2,936 | 0.89× | Two-stage | Win |
| Marine IK Small (minified) | 1,151 | 913 | 628 | 1.26× | Fused | Win |
| Marine IK Small (prettified) | 3,670 | 4,057 | 2,951 | 0.90× | Two-stage | Win |
| Mesh Small (minified) | 1,210 | 1,026 | 1,002 | 1.18× | Fused | Win |
| Mesh Small (prettified) | 1,715 | 1,956 | 2,020 | 0.88× | Two-stage | Loss |
| Random Small (minified) | 2,726 | 2,035 | 1,052 | 1.34× | Fused | Win |
| Random Small (prettified) | 3,154 | 3,496 | 1,901 | 0.90× | Two-stage | Win |
| Twitter Small (minified) | 3,348 | 2,447 | 1,714 | 1.37× | Fused | Win |
| Twitter Small (prettified) | 2,751 | 3,394 | 2,390 | 0.81× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,519 | 220 | 190 | 6.89× | Fused | Win |
| Double (POD) | 1,060 | 314 | 146 | 3.37× | Fused | Win |
| Int64 (POD) | 2,698 | 610 | 417 | 4.42× | Fused | Win |
| String (POD) | 1,770 | 861 | 1,009 | 2.06× | Fused | Win |
| Uint64 (POD) | 2,645 | 597 | 427 | 4.43× | Fused | Win |
| Canada (minified) | 797 | 736 | 512 | 1.08× | Fused | Win |
| Canada (prettified) | 1,967 | 1,829 | 1,357 | 1.08× | Fused | Win |
| CitmCatalog (minified) | 2,249 | 1,336 | 1,097 | 1.68× | Fused | Win |
| CitmCatalog (prettified) | 3,442 | 2,904 | 2,344 | 1.19× | Fused | Win |
| Discord (minified) | 1,998 | 1,313 | 733 | 1.52× | Fused | Win |
| Discord (prettified) | 2,122 | 2,017 | 1,547 | 1.05× | Fused | Win |
| Google Maps Response (minified) | 1,971 | 1,132 | 656 | 1.74× | Fused | Win |
| Google Maps Response (prettified) | 2,841 | 2,420 | 1,602 | 1.17× | Fused | Win |
| Instruments (minified) | 2,768 | 1,533 | 1,180 | 1.81× | Fused | Win |
| Instruments (prettified) | 3,047 | 2,533 | 1,978 | 1.20× | Fused | Win |
| Marine IK Reverse (minified) | 658 | 516 | 434 | 1.28× | Fused | Win |
| Marine IK Reverse (prettified) | 2,065 | 1,921 | 1,703 (n/c) | 1.07× | Tie | Win |
| Marine IK (minified) | 656 | 529 | 330 (n/c) | 1.24× | Fused | Win |
| Marine IK (prettified) | 2,430 (n/c) | 2,514 | 1,771 | 0.97× | Tie | Win |
| Mesh (minified) | 883 | 747 | 780 | 1.18× | Fused | Win |
| Mesh (prettified) | 1,348 | 1,285 | 1,475 | 1.05× | Fused | Loss |
| Random (minified) | 1,629 | 1,116 | 703 | 1.46× | Fused | Win |
| Random (prettified) | 1,785 | 1,667 (n/c) | 1,150 | 1.07× | Fused | Win |
| Twitter (minified) | 2,336 | 1,684 | 1,079 | 1.39× | Fused | Win |
| Twitter (prettified) | 2,554 | 2,121 (n/c) | 1,558 | 1.20× | Fused | Win |
| Canada Small (minified) | 918 | 795 | 430 | 1.16× | Fused | Win |
| Canada Small (prettified) | 2,146 | 2,002 | 1,330 | 1.07× | Fused | Win |
| CitmCatalog Small (minified) | 1,765 | 1,147 | 893 | 1.54× | Fused | Win |
| CitmCatalog Small (prettified) | 2,299 | 2,363 | 1,961 | 0.97× | Two-stage | Win |
| Discord Small (minified) | 2,194 | 1,493 | 1,066 | 1.47× | Fused | Win |
| Discord Small (prettified) | 2,076 | 1,834 | 1,416 | 1.13× | Fused | Win |
| Google Maps Response Small (minified) | 1,993 | 1,214 | 757 | 1.64× | Fused | Win |
| Google Maps Response Small (prettified) | 2,995 | 2,267 | 1,803 | 1.32× | Fused | Win |
| Instruments Small (minified) | 2,832 | 1,483 | 1,049 | 1.91× | Fused | Win |
| Instruments Small (prettified) | 3,247 | 2,624 | 2,043 | 1.24× | Fused | Win |
| Marine IK Reverse Small (minified) | 593 | 495 | 356 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,412 | 2,053 | 1,628 | 1.18× | Fused | Win |
| Marine IK Small (minified) | 833 | 621 | 379 | 1.34× | Fused | Win |
| Marine IK Small (prettified) | 2,739 | 2,593 | 1,713 | 1.06× | Fused | Win |
| Mesh Small (minified) | 967 | 764 | 591 | 1.27× | Fused | Win |
| Mesh Small (prettified) | 1,434 | 1,420 | 1,159 | 1.01× | Tie | Win |
| Random Small (minified) | 1,885 | 1,208 | 755 | 1.56× | Fused | Win |
| Random Small (prettified) | 2,239 | 1,846 | 1,459 | 1.21× | Fused | Win |
| Twitter Small (minified) | 2,547 | 1,828 | 1,273 | 1.39× | Fused | Win |
| Twitter Small (prettified) | 2,618 | 2,512 | 1,659 | 1.04× | Tie | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,130 | 256 | 184 | 4.41× | Fused | Win |
| Double (POD) | 1,141 | 354 | 187 | 3.22× | Fused | Win |
| Int64 (POD) | 2,042 | 578 | 460 | 3.54× | Fused | Win |
| String (POD) | 2,415 | 862 | 1,032 | 2.80× | Fused | Win |
| Uint64 (POD) | 2,817 | 651 | 501 | 4.33× | Fused | Win |
| Canada (minified) | 866 | 731 | 496 | 1.18× | Fused | Win |
| Canada (prettified) | 2,099 | 1,736 | 1,427 | 1.21× | Fused | Win |
| CitmCatalog (minified) | 2,617 | 1,756 | 974 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 4,152 | 3,733 | 2,184 | 1.11× | Fused | Win |
| Discord (minified) | 2,804 | 1,181 | 1,493 | 2.37× | Fused | Win |
| Discord (prettified) | 2,869 | 1,848 | 2,375 | 1.55× | Fused | Win |
| Google Maps Response (minified) | 2,381 | 1,280 | 1,114 | 1.86× | Fused | Win |
| Google Maps Response (prettified) | 3,881 | 2,345 | 2,295 | 1.66× | Fused | Win |
| Instruments (minified) | 3,053 | 1,673 | 1,318 | 1.82× | Fused | Win |
| Instruments (prettified) | 3,408 | 2,955 | 2,134 | 1.15× | Fused | Win |
| Marine IK Reverse (minified) | 768 | 583 | 444 | 1.32× | Fused | Win |
| Marine IK Reverse (prettified) | 2,354 | 2,436 | 1,897 | 0.97× | Tie | Win |
| Marine IK (minified) | 784 | 641 | 453 | 1.22× | Fused | Win |
| Marine IK (prettified) | 2,978 | 2,652 | 1,906 | 1.12× | Fused | Win |
| Mesh (minified) | 1,294 | 963 | 875 | 1.34× | Fused | Win |
| Mesh (prettified) | 1,494 | 1,580 | 1,643 | 0.95× | Two-stage | Loss |
| Random (minified) | 2,397 | 1,183 | 897 | 2.03× | Fused | Win |
| Random (prettified) | 3,045 | 1,845 | 1,633 | 1.65× | Fused | Win |
| Twitter (minified) | 2,594 | 2,062 | 1,796 | 1.26× | Fused | Win |
| Twitter (prettified) | 2,506 | 2,625 | 2,211 | 0.95× | Two-stage | Win |
| Canada Small (minified) | 970 | 842 | 532 | 1.15× | Fused | Win |
| Canada Small (prettified) | 1,894 | 1,491 | 1,184 | 1.27× | Fused | Win |
| CitmCatalog Small (minified) | 1,899 | 1,382 | 1,050 | 1.37× | Fused | Win |
| CitmCatalog Small (prettified) | 2,419 | 1,976 | 1,779 | 1.22× | Fused | Win |
| Discord Small (minified) | 2,619 | 1,452 | 1,515 | 1.80× | Fused | Win |
| Discord Small (prettified) | 2,079 | 1,693 | 2,021 | 1.23× | Fused | Win |
| Google Maps Response Small (minified) | 2,574 | 1,249 | 1,084 | 2.06× | Fused | Win |
| Google Maps Response Small (prettified) | 2,460 | 2,539 | 2,017 | 0.97× | Two-stage | Win |
| Instruments Small (minified) | 2,550 | 1,868 | 1,534 | 1.36× | Fused | Win |
| Instruments Small (prettified) | 3,015 | 2,857 | 2,423 | 1.06× | Fused | Win |
| Marine IK Reverse Small (minified) | 870 | 695 | 440 | 1.25× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,078 | 2,175 | 1,430 | 0.96× | Two-stage | Win |
| Marine IK Small (minified) | 846 | 704 | 403 | 1.20× | Fused | Win |
| Marine IK Small (prettified) | 2,238 | 2,333 | 1,584 | 0.96× | Two-stage | Win |
| Mesh Small (minified) | 1,073 | 895 | 788 | 1.20× | Fused | Win |
| Mesh Small (prettified) | 1,370 | 1,434 | 1,370 | 0.95× | Two-stage | Win |
| Random Small (minified) | 2,633 | 1,491 | 1,201 | 1.77× | Fused | Win |
| Random Small (prettified) | 2,789 | 1,813 | 1,529 | 1.54× | Fused | Win |
| Twitter Small (minified) | 2,883 | 2,100 | 1,821 | 1.37× | Fused | Win |
| Twitter Small (prettified) | 2,562 | 2,743 | 2,490 | 0.93× | Two-stage | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins all POD input, 29 of the 30 full-size minified documents and all 30 Small minified ones, while the tape wins 27 of the 30 full-size prettified documents and all 30 Small prettified ones. MSVC splits cleanly: all 20 minified documents to the fused path, all 20 prettified ones to the tape. On the M1 the fused path wins all POD input, 38 of the 40 minified documents (the other two are ties) and 29 of the 40 prettified documents (eight to the tape, three ties).

**POD-type tests: the fused path wins all 25 freshly allocated and all 25 reused, on every build.** It runs them 1.39× (String, Windows/MSVC) to 7.95× (Bool, Linux/GCC) faster than the two-stage path, and 1.63× (String, Windows/MSVC) to 8.55× (Bool, Linux/GCC) faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the platform.

**Minified documents: the fused path's, at both sizes.** The fused path wins 97 of the 100 minified documents: all 20 under MSVC, on Linux/Clang and on macOS/Clang, 19 of 20 on Linux/GCC, where the two-stage path wins minified Mesh by 7% (1,151 against 1,073 MB/s), and 18 of 20 on macOS/GCC, where minified Canada and Marine IK Reverse Small are ties. The fused path's margins run from 1% (Canada on Linux/GCC, 911 against 905 MB/s) to 2.21× (Google Maps Response on macOS/GCC, 1,679 against 761 MB/s) on the full-size documents and up to 1.97× on the Small ones (Random Small on macOS/Clang, 2,304 against 1,170 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 57 of 60.** It wins all 10 under MSVC at each size, all 10 Small prettified documents on Linux/Clang and on Linux/GCC, 9 of the 10 full-size ones on Linux/GCC and 8 of 10 on Linux/Clang. The fused path wins prettified Marine IK and Marine IK Reverse on Linux/Clang and prettified Marine IK Reverse on Linux/GCC, each by 1% to 4%. The tape's margins run from under 1% (Google Maps Response on Linux/GCC, 4,258 against 4,248 MB/s) to 32% (Discord on Linux/Clang, 2,808 against 2,128) on the full-size documents and from 4% to 38% on the Small ones, widest on Twitter Small on Linux/Clang. The two Marine IK documents, which are mostly numbers, are among the closest on both Linux builds, and Discord, Twitter and Random are among the widest. The fused path's depth-predicted indentation (§3) keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 56 of the 60 prettified x86 documents; the fused path wins Marine IK Reverse on Linux/Clang, and prettified Canada and Marine IK on Linux/Clang and Marine IK Reverse on Linux/GCC are ties.

**Prettified documents on the M1: mostly the fused path.** Of the 40 prettified documents, the fused path wins 29, the tape wins 8 and 3 are ties; the tape's wins are Mesh on both builds plus a few Small documents, mostly number-heavy ones. Reused, the fused path wins 27, the tape 8, and 5 are ties. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor may be that stage 1 is comparatively more expensive there, since NEON has no `movemask`, but we have not isolated it.

**Mesh is where Jsonifier is weakest.** The two-stage path wins prettified Mesh on all three x86 builds and on macOS/Clang in both runs and on macOS/GCC freshly allocated, prettified Mesh Small on all three x86 builds and on macOS/Clang in both runs, and minified Mesh on Linux/GCC in both runs. Mesh is also where the faster Jsonifier path loses most often to simdjson On Demand. Freshly allocated, it loses Mesh on Linux/GCC in both forms, by 5% minified (1,151 against 1,209 MB/s) and 9% prettified (2,031 against 2,222); the only other loss is prettified Twitter Small on macOS/Clang, by 7% (2,084 against 2,239). With objects reused the faster path loses six results, all on Mesh or Mesh Small: Linux/GCC in both forms by 10%, prettified Mesh on Linux/Clang by 3%, on macOS/GCC by 9% and on macOS/Clang by 4%, and prettified Mesh Small on Linux/GCC by 3%. The fused path alone also ties prettified Mesh with simdjson on both macOS builds freshly allocated. We have not profiled what is particular about this document.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 9 full-size minified documents in declared key order, the fused path reaches a median 60% of its Linux/Clang throughput, the two-stage path 64% and simdjson 43%. Because the two Jsonifier paths lose a similar share, MSVC routes as Linux/Clang does: minified to the fused path, prettified to the tape. The spread between documents is wider for the fused path (55% on Discord to 77% on Instruments) than for the two-stage path (60% to 74%). That fits the fused path being branch-heavy code whose performance rests on the optimizer while stage 1 is branch-free intrinsic code whose shape is fixed by the source, but we have not isolated it.

**Against simdjson, Jsonifier wins 221 of 225 tests with whichever path is faster, ties one and loses three** (Mesh in both forms on Linux/GCC and prettified Twitter Small on macOS/Clang). The fused path alone wins 210, ties 4 and loses 11, its eight extra losses all prettified documents on Linux/Clang. Under MSVC both win all 45.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it does: simdjson parses minified Marine IK Reverse at 37 MB/s, against 261 MB/s for the same document in declared order, so the faster Jsonifier path is 13.4× faster than simdjson freshly allocated (14.8× reused). The prettified form shows the same gap, at 195 MB/s against 1,283, and the faster Jsonifier path is 11.2× faster (12.0× reused). The Small copies show it too, at 39 MB/s minified and 204 MB/s prettified under MSVC. On the other builds simdjson parses the reversed document about as fast as the declared one, and the faster Jsonifier path is 1.13× to 1.48× faster than simdjson on the full-size reversed document, freshly allocated, and 1.18× to 1.73× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb: on x86, prettified input belongs to the two-stage path; on the M1, almost everything belongs to the fused path. Routing each test to its faster path would raise the record against simdjson from 210 wins, 4 ties and 11 losses to 221 wins, 1 tie and 3 losses. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 905 | 868 | 1.04× | Win |
| Canada (prettified) | 2,636 | 2,502 | 1.05× | Win |
| CitmCatalog (minified) | 1,847 | 1,417 | 1.30× | Win |
| CitmCatalog (prettified) | 4,332 | 3,520 | 1.23× | Win |
| Discord (minified) | 1,853 | 1,542 | 1.20× | Win |
| Discord (prettified) | 2,845 | 2,462 | 1.16× | Win |
| Google Maps Response (minified) | 1,944 | 1,284 | 1.51× | Win |
| Google Maps Response (prettified) | 4,258 | 3,039 | 1.40× | Win |
| Instruments (minified) | 2,333 | 1,867 | 1.25× | Win |
| Instruments (prettified) | 3,886 | 3,345 | 1.16× | Win |
| Marine IK Reverse (minified) | 716 | 720 | 1.00× | Tie |
| Marine IK Reverse (prettified) | 3,198 | 3,201 | 1.00× | Tie |
| Marine IK (minified) | 725 | 716 | 1.01× | Win |
| Marine IK (prettified) | 3,300 | 3,251 | 1.02× | Win |
| Mesh (minified) | 1,151 | 1,188 | 0.97× | **Loss** |
| Mesh (prettified) | 2,031 | 2,122 | 0.96× | **Loss** |
| Random (minified) | 1,368 | 1,150 | 1.19× | Win |
| Random (prettified) | 2,426 | 2,033 | 1.19× | Win |
| Twitter (minified) | 1,748 | 1,786 | 0.98× | **Loss** |
| Twitter (prettified) | 2,478 | 2,563 | 0.97× | **Loss** |
| Canada Small (minified) | 955 | 937 | 1.02× | Win |
| Canada Small (prettified) | 2,694 | 2,743 | 0.98× | **Loss** |
| CitmCatalog Small (minified) | 1,569 | 1,251 | 1.25× | Win |
| CitmCatalog Small (prettified) | 3,192 | 2,672 | 1.19× | Win |
| Discord Small (minified) | 1,632 | 1,342 | 1.22× | Win |
| Discord Small (prettified) | 2,324 | 1,954 | 1.19× | Win |
| Google Maps Response Small (minified) | 1,814 | 1,269 | 1.43× | Win |
| Google Maps Response Small (prettified) | 3,893 | 2,984 | 1.30× | Win |
| Instruments Small (minified) | 2,239 | 1,759 | 1.27× | Win |
| Instruments Small (prettified) | 3,742 | 3,139 | 1.19× | Win |
| Marine IK Reverse Small (minified) | 715 | 745 | 0.96× | **Loss** |
| Marine IK Reverse Small (prettified) | 3,317 | 3,394 | 0.98× | **Loss** |
| Marine IK Small (minified) | 761 | 752 | 1.01× | Win |
| Marine IK Small (prettified) | 3,476 | 3,463 | 1.00× | Win |
| Mesh Small (minified) | 888 | 872 | 1.02× | Win |
| Mesh Small (prettified) | 1,711 | 1,780 | 0.96× | **Loss** |
| Random Small (minified) | 1,712 | 1,273 | 1.34× | Win |
| Random Small (prettified) | 3,032 | 2,277 | 1.33× | Win |
| Twitter Small (minified) | 2,151 | 2,122 | 1.01× | Win |
| Twitter Small (prettified) | 2,995 | 2,948 | 1.02× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 600 | 497 (n/c) | 1.21× | Win |
| Canada (prettified) | 1,589 (n/c) | 1,357 (n/c) | 1.17× | Win |
| CitmCatalog (minified) | 1,154 | 930 | 1.24× | Win |
| CitmCatalog (prettified) | 2,576 | 2,102 | 1.23× | Win |
| Discord (minified) | 1,039 | 765 | 1.36× | Win |
| Discord (prettified) | 1,609 | 1,360 | 1.18× | Win |
| Google Maps Response (minified) | 761 | 798 | 0.95× | **Loss** |
| Google Maps Response (prettified) | 1,977 | 1,879 | 1.05× | Win |
| Instruments (minified) | 1,396 | 599 (n/c) | 2.33× | Win |
| Instruments (prettified) | 2,299 | 1,797 | 1.28× | Win |
| Marine IK Reverse (minified) | 464 | 435 (n/c) | 1.07× | Win |
| Marine IK Reverse (prettified) | 1,732 (n/c) | 1,633 | 1.06× | Tie |
| Marine IK (minified) | 434 | 434 | 1.00× | Tie |
| Marine IK (prettified) | 2,165 | 1,945 | 1.11× | Win |
| Mesh (minified) | 725 (n/c) | 550 | 1.32× | Win |
| Mesh (prettified) | 1,361 | 903 | 1.51× | Win |
| Random (minified) | 761 | 585 | 1.30× | Win |
| Random (prettified) | 1,090 (n/c) | 875 (n/c) | 1.24× | Win |
| Twitter (minified) | 1,475 | 1,062 | 1.39× | Win |
| Twitter (prettified) | 1,751 | 1,374 | 1.27× | Win |
| Canada Small (minified) | 632 | 481 | 1.31× | Win |
| Canada Small (prettified) | 1,763 | 1,487 | 1.19× | Win |
| CitmCatalog Small (minified) | 862 | 486 | 1.77× | Win |
| CitmCatalog Small (prettified) | 1,802 | 1,084 | 1.66× | Win |
| Discord Small (minified) | 1,127 | 899 | 1.25× | Win |
| Discord Small (prettified) | 1,425 | 1,322 | 1.08× | Win |
| Google Maps Response Small (minified) | 757 | 855 | 0.89× | **Loss** |
| Google Maps Response Small (prettified) | 1,913 | 1,967 | 0.97× | **Loss** |
| Instruments Small (minified) | 1,382 | 1,142 | 1.21× | Win |
| Instruments Small (prettified) | 2,489 | 1,896 | 1.31× | Win |
| Marine IK Reverse Small (minified) | 449 | 379 | 1.18× | Win |
| Marine IK Reverse Small (prettified) | 1,905 | 1,910 | 1.00× | Tie |
| Marine IK Small (minified) | 498 | 423 | 1.18× | Win |
| Marine IK Small (prettified) | 1,941 | 1,856 | 1.05× | Tie |
| Mesh Small (minified) | 643 | 442 | 1.45× | Win |
| Mesh Small (prettified) | 1,233 | 929 | 1.33× | Win |
| Random Small (minified) | 949 | 737 | 1.29× | Win |
| Random Small (prettified) | 1,428 | 1,456 | 0.98× | **Loss** |
| Twitter Small (minified) | 1,510 | 1,261 | 1.20× | Win |
| Twitter Small (prettified) | 1,919 | 1,471 | 1.30× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,106 | 1,031 | 1.07× | Win |
| Canada (prettified) | 3,117 | 2,986 | 1.04× | Win |
| CitmCatalog (minified) | 2,227 | 1,665 | 1.34× | Win |
| CitmCatalog (prettified) | 5,005 | 4,059 | 1.23× | Win |
| Discord (minified) | 2,448 | 1,795 | 1.36× | Win |
| Discord (prettified) | 3,653 | 2,815 | 1.30× | Win |
| Google Maps Response (minified) | 2,117 | 1,338 | 1.58× | Win |
| Google Maps Response (prettified) | 4,581 | 3,147 | 1.46× | Win |
| Instruments (minified) | 2,653 | 1,928 | 1.38× | Win |
| Instruments (prettified) | 4,349 | 3,442 | 1.26× | Win |
| Marine IK Reverse (minified) | 781 | 815 | 0.96× | **Loss** |
| Marine IK Reverse (prettified) | 3,507 | 3,598 | 0.97× | **Loss** |
| Marine IK (minified) | 801 | 811 | 0.99× | **Loss** |
| Marine IK (prettified) | 3,577 | 3,604 | 0.99× | Tie |
| Mesh (minified) | 1,202 | 1,263 | 0.95× | **Loss** |
| Mesh (prettified) | 2,162 | 2,317 | 0.93× | **Loss** |
| Random (minified) | 1,941 | 1,412 | 1.38× | Win |
| Random (prettified) | 3,351 | 2,497 | 1.34× | Win |
| Twitter (minified) | 2,183 | 2,211 | 0.99× | **Loss** |
| Twitter (prettified) | 3,033 | 2,910 | 1.04× | Win |
| Canada Small (minified) | 1,182 | 1,117 | 1.06× | Win |
| Canada Small (prettified) | 3,241 | 3,157 | 1.03× | Win |
| CitmCatalog Small (minified) | 1,865 | 1,394 | 1.34× | Win |
| CitmCatalog Small (prettified) | 3,678 | 2,853 | 1.29× | Win |
| Discord Small (minified) | 2,038 | 1,587 | 1.28× | Win |
| Discord Small (prettified) | 2,885 | 2,326 | 1.24× | Win |
| Google Maps Response Small (minified) | 2,034 | 1,344 | 1.51× | Win |
| Google Maps Response Small (prettified) | 4,313 | 3,127 | 1.38× | Win |
| Instruments Small (minified) | 2,406 | 1,812 | 1.33× | Win |
| Instruments Small (prettified) | 3,952 | 3,225 | 1.23× | Win |
| Marine IK Reverse Small (minified) | 854 | 856 | 1.00× | **Loss** |
| Marine IK Reverse Small (prettified) | 3,907 | 3,840 | 1.02× | Win |
| Marine IK Small (minified) | 913 | 852 | 1.07× | Win |
| Marine IK Small (prettified) | 4,057 | 3,862 | 1.05× | Win |
| Mesh Small (minified) | 1,026 | 1,031 | 0.99× | **Loss** |
| Mesh Small (prettified) | 1,956 | 2,090 | 0.94× | **Loss** |
| Random Small (minified) | 2,035 | 1,400 | 1.45× | Win |
| Random Small (prettified) | 3,496 | 2,495 | 1.40× | Win |
| Twitter Small (minified) | 2,447 | 2,297 | 1.06× | Win |
| Twitter Small (prettified) | 3,394 | 3,166 | 1.07× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 736 | 575 | 1.28× | Win |
| Canada (prettified) | 1,829 | 1,597 | 1.15× | Win |
| CitmCatalog (minified) | 1,336 | 1,065 | 1.25× | Win |
| CitmCatalog (prettified) | 2,904 | 2,364 | 1.23× | Win |
| Discord (minified) | 1,313 | 902 | 1.46× | Win |
| Discord (prettified) | 2,017 | 1,563 | 1.29× | Win |
| Google Maps Response (minified) | 1,132 | 941 | 1.20× | Win |
| Google Maps Response (prettified) | 2,420 | 1,898 | 1.27× | Win |
| Instruments (minified) | 1,533 | 1,132 | 1.35× | Win |
| Instruments (prettified) | 2,533 | 1,916 | 1.32× | Win |
| Marine IK Reverse (minified) | 516 | 468 | 1.10× | Win |
| Marine IK Reverse (prettified) | 1,921 | 1,674 | 1.15× | Win |
| Marine IK (minified) | 529 | 417 | 1.27× | Win |
| Marine IK (prettified) | 2,514 | 1,792 | 1.40× | Win |
| Mesh (minified) | 747 | 561 | 1.33× | Win |
| Mesh (prettified) | 1,285 | 1,000 | 1.28× | Win |
| Random (minified) | 1,116 | 738 | 1.51× | Win |
| Random (prettified) | 1,667 (n/c) | 1,365 | 1.22× | Win |
| Twitter (minified) | 1,684 | 966 | 1.74× | Win |
| Twitter (prettified) | 2,121 (n/c) | 1,498 | 1.42× | Win |
| Canada Small (minified) | 795 | 585 | 1.36× | Win |
| Canada Small (prettified) | 2,002 | 1,706 | 1.17× | Win |
| CitmCatalog Small (minified) | 1,147 | 918 | 1.25× | Win |
| CitmCatalog Small (prettified) | 2,363 | 1,747 | 1.35× | Win |
| Discord Small (minified) | 1,493 | 1,133 | 1.32× | Win |
| Discord Small (prettified) | 1,834 | 1,509 | 1.22× | Win |
| Google Maps Response Small (minified) | 1,214 | 919 | 1.32× | Win |
| Google Maps Response Small (prettified) | 2,267 | 699 | 3.24× | Win |
| Instruments Small (minified) | 1,483 | 1,124 | 1.32× | Win |
| Instruments Small (prettified) | 2,624 | 1,874 | 1.40× | Win |
| Marine IK Reverse Small (minified) | 495 | 495 | 1.00× | Tie |
| Marine IK Reverse Small (prettified) | 2,053 | 2,067 | 0.99× | Tie |
| Marine IK Small (minified) | 621 | 540 | 1.15× | Win |
| Marine IK Small (prettified) | 2,593 | 2,123 | 1.22× | Win |
| Mesh Small (minified) | 764 | 549 | 1.39× | Win |
| Mesh Small (prettified) | 1,420 | 1,192 | 1.19× | Win |
| Random Small (minified) | 1,208 | 946 | 1.28× | Win |
| Random Small (prettified) | 1,846 | 1,514 | 1.22× | Win |
| Twitter Small (minified) | 1,828 | 1,330 | 1.37× | Win |
| Twitter Small (prettified) | 2,512 | 1,823 | 1.38× | Win |

In the freshly allocated run, two-stage Jsonifier wins 62 of these 80 tests, ties 6 and loses 12, every loss by 2% to 11%, mostly on Linux/GCC (Mesh, Twitter, Marine IK Reverse Small) and on Google Maps Response on macOS/GCC. With the fused path instead, Jsonifier beats the reflection reader on 68, ties 3 and loses 9. With objects reused, two-stage Jsonifier wins 68, ties 3 and loses 9, all nine on Linux/GCC by under 1% to 7%. We have not profiled these losses.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 8% on 34 of the 40 results; all six wider gaps are on the macOS builds, and the widest is freshly allocated minified Twitter on macOS/GCC, where the two-stage row runs at 2,727 MB/s against the "jsonifier" row's 3,312. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 6,026 | 1,946 | 3.10× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,542 | 2,752 | 2.74× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,946 | 1,993 | 2.48× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,373 | 2,789 | 2.29× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 6,995 | 5,618 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,073 | 7,418 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,238 | 5,239 | 1.00× | Tie |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,715 | 6,458 | 1.04× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,337 | 5,411 | 1.36× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,867 | 7,071 | 1.25× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,599 | 4,692 | 1.19× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,120 | 6,238 | 1.14× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (minified) | 2,727 | 2,806 | 0.97× | Loss |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (prettified) | 4,765 | 3,241 | 1.47× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (minified) | 3,201 | 2,647 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (prettified) | 4,016 | 3,341 | 1.20× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (minified) | 3,697 | 3,015 | 1.23× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (prettified) | 4,767 | 4,258 | 1.12× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (minified) | 3,232 | 3,265 | 0.99× | Loss |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (prettified) | 3,904 | 3,844 | 1.02× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 6,318 | 2,001 | 3.16× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,897 | 2,826 | 2.79× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,262 | 2,044 | 2.57× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,106 | 2,900 | 2.45× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,167 | 5,757 | 1.24× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,272 | 7,533 | 1.23× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,675 | 5,225 | 1.09× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,022 | 6,729 | 1.04× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,523 | 5,503 | 1.37× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,040 | 7,170 | 1.26× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,225 | 4,908 | 1.27× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,626 | 6,388 | 1.19× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (minified) | 3,307 | 3,018 | 1.10× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (prettified) | 4,266 | 3,708 | 1.15× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (minified) | 3,751 | 3,094 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (prettified) | 3,855 | 3,810 | 1.01× | Tie |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (minified) | 4,272 | 3,605 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (prettified) | 4,537 | 4,282 | 1.06× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (minified) | 3,692 | 3,399 | 1.09× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (prettified) | 4,251 | 4,027 | 1.06× | Win |

Jsonifier wins 36 of the 40 results, ties 2 and loses 2, by up to 3.16× (minified Twitter under MSVC, reused). The reused run has 19 wins and 1 tie (prettified Twitter Small on macOS/GCC). Freshly allocated, the losses are minified Twitter on macOS/GCC (0.97×) and minified Twitter Small on macOS/Clang (0.99×), and minified Twitter Small on Linux/Clang is a tie; 19 of the 20 full-size results are wins. Throughput is counted over the whole document, so these figures run well above the full-parse ones: unrequested bytes are skipped through the tape instead of being walked. On the full-size document this is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 2,964 | 2,411 | 1.23× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,622 | 2,619 | 1.38× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 2,999 | 2,438 | 1.23× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | 1,995 | 1,765 | 1.13× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | 1,884 | 1,731 | 1.09× | Win |

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on 97 of 100 minified documents and on 29 of 40 prettified documents on the M1; slower on 57 of 60 prettified x86 documents (see §2.2) |
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

The tier is chosen when CMake configures the project, by a detector run on the build machine or from `JSONIFIER_CPU_INSTRUCTIONS`, and the whole library is compiled once for it, so every call is a direct, inlinable call with no per-call check. simdjson dispatches its core and DOM API through virtual functions per ISA, while its On Demand front end is mostly fixed at compile time. Jsonifier keeps one tier per build because per-function target attributes do not reach force-inlined callables passed into standard-library templates such as `resize_and_overwrite`, which GCC and Clang then reject. The cost is portability: a binary for mixed hardware is configured for the lowest tier it must support.

## 8. Conclusion

The two-stage model is a genuinely great algorithm. Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. With both paths in one binary, the fused path wins 154 of 225 freshly allocated tests: all POD-type tests, 97 of 100 minified documents and 29 of 40 prettified documents on the M1, while the two-stage path wins 57 of 60 prettified documents on x86. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 221 of 225 tests, its stage-1 + reflection path beats simdjson's reflection reader on 62 of 80, and on partial reading it wins 36 of 40. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*