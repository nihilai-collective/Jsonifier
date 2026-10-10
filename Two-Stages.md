# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model — a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2) — has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization — so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary, skipping the tape wins on POD-type data on every build and on every minified document but two on Linux/GCC and one tie on macOS/Clang, and on x86 the tape is faster on every prettified document but one tie; on the Apple M1 the fused path wins 12 of the 15 prettified documents that converged.

This paper describes both halves of that architecture: the single-pass primary path, and Jsonifier's stage-1 implementation — including its per-compiler-tuned step geometry, its fold-expression drain architecture for bitmask-to-index extraction, and its distributed UTF-8 validation strategy — with direct comparisons to simdjson's current design throughout.

---

## 1. Background: the canonical two-stage model

simdjson's pipeline is well documented. Stage 1 sweeps the input in 64-byte blocks, computing per-block bitmasks: backslashes, odd-length escape terminations, quotes, an in-string range mask derived via carry-less multiplication (prefix XOR), structural characters (`{ } [ ] : ,`), whitespace, and pseudo-structural scalar starts. The surviving bits are converted to byte offsets — the *structural tape* — via a tzcnt/blsr extraction loop. Stage 2 then walks the tape to build a DOM or, in the On Demand API, lazily materializes only the values the caller touches.

The model's strength is that stage 1 is branch-free and data-parallel; its cost is that it is unconditional. Every byte of every document is classified and indexed before a single value is parsed, regardless of whether the caller's access pattern needs the index at all.

## 2. The Jsonifier position: pay for the tape only when the tape pays for you

Jsonifier's primary API parses JSON directly into user-defined structs registered through compile-time reflection (`jsonifier::core<T>`). In that setting the parser already knows, at compile time, the expected shape of the document: which keys exist, their serialized order, their types. The parse loop is a schema-directed walk, not a blind tree construction.

Consider what the structural tape provides: the positions of every brace, bracket, colon, comma, quote, and scalar start. Now consider what a schema-directed single-pass parser does at each of those same positions: it is *already there*, with its iterator parked on that exact byte, consuming it as part of matching a known key literal or delimiting a value. The tape's information is a strict subset of what the fused pass discovers for free in program order. Building it first means that, for a full-document parse in which every value is materialized:

- every byte of the input is loaded twice (once in stage 1, once in stage 2),
- the tape itself is written and re-read (cache traffic proportional to structural density), and
- stage 2's control flow is driven by indirect loads through the index array rather than by a pointer walking linearly through memory the prefetcher already understands.

(We scope the double-load claim deliberately: a two-stage consumer that *skips* content — simdjson's On Demand API skipping unrequested fields — does not reload the skipped bytes in its second pass. That is exactly the workload where the tape earns its keep, and exactly why Jsonifier retains the two-stage machinery for partial reading. The accounting above describes the full-materialization case, where nothing is skipped and the tape's information is fully redundant with the walk.)

For full-document parsing into known types, the tape is overhead in principle, and in practice whether skipping it is faster comes down to two questions: is the input indented, and is the machine x86? §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. In the freshly allocated run, the fused path wins all 25 POD-type tests and 40 of the 42 converged minified documents across all five builds; the other two, both on Linux/GCC, are a tie and a two-stage win. The two-stage path wins 29 of the 30 prettified documents on x86, and on the M1 prettified documents split: 7 to the fused path, 4 to the tape and one tie.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 10, 2026, with simdjson [7f6f8dc](https://github.com/simdjson/simdjson/commit/7f6f8dc) and BenchmarkSuite [6196208](https://github.com/nihilai-collective/benchmarksuite/commit/6196208) on every build. Jsonifier is [13785b6](https://github.com/nihilai-collective/jsonifier/commit/13785b6) on Windows/MSVC and both macOS builds, and [5aa6104](https://github.com/nihilai-collective/jsonifier/commit/5aa6104) on both Linux builds. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep but is left out here, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3 and §2.5 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document, which §2.6 covers on its own, and "Small" cut-down copies of each document (at most 5 KiB minified) that measure per-call overhead; neither is counted in §2.2 to §2.5.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M1), and a test is ranked only if every library in it converges, which is why some platforms have fewer than 25 tests. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to every library. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced two ties between the two Jsonifier paths across 116 freshly allocated tests and two across 116 reused ones.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 81 tests, 2 are statistical ties, and the two-stage path wins 33. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 15 | 0 | 9 | 24 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 15 | 0 | 10 | 25 of 25 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 13 | 1 | 11 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 21 | 0 | 3 | 24 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 17 | 1 | 0 | 18 of 25 |
| **Aggregate** | **81** | **2** | **33** | **116 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 18 / 0 / 2 | 16 / 1 / 0 |
| Prettified corpus documents | 0 / 0 / 9 | 0 / 1 / 19 | 12 / 0 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 24 / 0 / 0 | 24 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 25 / 0 / 0 | 21 / 0 / 4 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 / 0 / 2 | 23 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 24 / 0 / 0 | 22 / 0 / 2 |
| macOS / Clang 23.1 (Apple M1, NEON) | 18 / 0 / 0 | 18 / 0 / 0 |
| **Aggregate** | **114 / 0 / 2** | **108 / 0 / 8** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 15 | 0 | 9 | 24 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 14 | 0 | 11 | 25 of 25 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 13 | 1 | 11 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 22 | 0 | 2 | 24 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 15 | 1 | 2 | 18 of 25 |
| **Aggregate** | **79** | **2** | **35** | **116 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 17 / 1 / 2 | 14 / 0 / 1 |
| Prettified corpus documents | 0 / 0 / 9 | 0 / 0 / 20 | 13 / 1 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 24 / 0 / 0 | 24 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 24 / 0 / 1 | 21 / 0 / 4 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 / 0 / 2 | 23 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 23 / 0 / 1 | 23 / 0 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 18 / 0 / 0 | 17 / 1 / 0 |
| **Aggregate** | **112 / 0 / 4** | **108 / 1 / 7** |

Across 116 converged tests the fused path wins 79, ties 2 and loses 35 to the two-stage path, and the x86 split holds: every prettified document goes to the tape, and so do minified Mesh on Linux/GCC and minified Twitter on Linux/Clang, by 1%. On the M1 the tape takes prettified Mesh on both builds, prettified Twitter on macOS/GCC and minified Instruments on macOS/Clang, and prettified Twitter on macOS/Clang is a tie. Against simdjson, the faster Jsonifier path wins 112 and loses 4, and the fused path alone wins 108, ties 1 and loses 7. Taking allocation out of the timed region helps every library, so it moves individual margins; on x86 it changes the verdict between the two paths on three tests: minified Canada on Linux/GCC goes from the tape to a tie, prettified Marine IK on Linux/GCC from a tie to the tape, and minified Twitter on Linux/Clang from the fused path to the tape.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput, and "n/c" means the test did not converge for at least one library.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 469 | 132 | 119 | 3.56× | Fused | Win |
| Double (POD) | 513 | 295 | 165 | 1.74× | Fused | Win |
| Int64 (POD) | 1,489 | 540 | 374 | 2.76× | Fused | Win |
| String (POD) | 818 | 628 | 620 | 1.30× | Fused | Win |
| Uint64 (POD) | 1,505 | 569 | 367 | 2.65× | Fused | Win |
| Canada (minified) | 579 | 551 | 356 | 1.05× | Fused | Win |
| Canada (prettified) | 1,539 | 1,653 | 1,095 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,454 | 1,196 | 493 | 1.22× | Fused | Win |
| CitmCatalog (prettified) | 2,558 | 2,980 | 1,342 | 0.86× | Two-stage | Win |
| Discord (minified) | 1,142 | 1,000 | 612 | 1.14× | Fused | Win |
| Discord (prettified) | 1,377 | 1,646 | 942 | 0.84× | Two-stage | Win |
| Google Maps Response (minified) | 1,193 | 1,075 | 505 | 1.11× | Fused | Win |
| Google Maps Response (prettified) | 2,203 | 2,641 | 1,238 | 0.83× | Two-stage | Win |
| Instruments (minified) | 1,920 | 1,312 | 660 | 1.46× | Fused | Win |
| Instruments (prettified) | 2,038 | 2,485 | 1,211 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 489 | 450 | 36 | 1.09× | Fused | Win |
| Marine IK (minified) | 497 | 442 | 254 | 1.12× | Fused | Win |
| Marine IK (prettified) | 2,025 | 2,175 | 1,274 | 0.93× | Two-stage | Win |
| Mesh (minified) | 897 | 804 | 421 | 1.12× | Fused | Win |
| Mesh (prettified) | 1,169 | 1,439 | 775 | 0.81× | Two-stage | Win |
| Random (minified) | 889 | 833 | 499 | 1.07× | Fused | Win |
| Random (prettified) | 1,351 | 1,529 | 929 | 0.88× | Two-stage | Win |
| Twitter (minified) | 1,209 | 1,117 | 774 | 1.08× | Fused | Win |
| Twitter (prettified) | 1,372 | 1,621 | 1,103 | 0.85× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,779 | 363 | 232 | 4.90× | Fused | Win |
| Double (POD) | 1,230 | 449 | 271 | 2.74× | Fused | Win |
| Int64 (POD) | 2,958 | 1,044 | 557 | 2.83× | Fused | Win |
| String (POD) | 1,775 | 1,150 | 1,120 | 1.54× | Fused | Win |
| Uint64 (POD) | 3,200 | 1,033 | 576 | 3.10× | Fused | Win |
| Canada (minified) | 969 | 934 | 651 | 1.04× | Fused | Win |
| Canada (prettified) | 2,531 | 2,648 | 1,955 | 0.96× | Two-stage | Win |
| CitmCatalog (minified) | 2,464 | 1,821 | 1,227 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 3,888 | 4,327 | 3,125 | 0.90× | Two-stage | Win |
| Discord (minified) | 2,233 | 1,780 | 1,471 | 1.25× | Fused | Win |
| Discord (prettified) | 2,137 | 2,886 | 2,356 | 0.74× | Two-stage | Win |
| Google Maps Response (minified) | 2,644 | 1,946 | 1,381 | 1.36× | Fused | Win |
| Google Maps Response (prettified) | 3,754 | 4,348 | 3,246 | 0.86× | Two-stage | Win |
| Instruments (minified) | 2,656 | 1,983 | 1,734 | 1.34× | Fused | Win |
| Instruments (prettified) | 2,821 | 3,377 | 3,166 | 0.84× | Two-stage | Win |
| Marine IK Reverse (minified) | 865 | 770 | 603 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 3,349 | 3,431 | 2,794 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 870 | 778 | 605 | 1.12× | Fused | Win |
| Marine IK (prettified) | 3,386 | 3,493 | 2,857 | 0.97× | Two-stage | Win |
| Mesh (minified) | 1,414 | 1,215 | 1,155 | 1.16× | Fused | Win |
| Mesh (prettified) | 1,934 | 2,217 | 2,080 | 0.87× | Two-stage | Win |
| Random (minified) | 1,668 | 1,403 | 1,098 | 1.19× | Fused | Win |
| Random (prettified) | 2,155 | 2,412 | 1,961 | 0.89× | Two-stage | Win |
| Twitter (minified) | 1,987 | 1,839 | 1,714 | 1.08× | Fused | Win |
| Twitter (prettified) | 2,044 | 2,636 | 2,501 | 0.78× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,580 | 260 | 180 | 6.07× | Fused | Win |
| Double (POD) | 1,138 | 324 | 227 | 3.51× | Fused | Win |
| Int64 (POD) | 3,363 | 820 | 516 | 4.10× | Fused | Win |
| String (POD) | 1,726 | 1,032 | 999 | 1.67× | Fused | Win |
| Uint64 (POD) | 3,409 | 849 | 517 | 4.01× | Fused | Win |
| Canada (minified) | 906 | 916 | 771 | 0.99× | Two-stage | Win |
| Canada (prettified) | 2,417 | 2,642 | 2,280 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 2,447 | 1,873 | 1,375 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 4,014 | 4,379 | 3,418 | 0.92× | Two-stage | Win |
| Discord (minified) | 2,232 | 1,812 | 1,445 | 1.23× | Fused | Win |
| Discord (prettified) | 2,478 | 2,836 | 2,264 | 0.87× | Two-stage | Win |
| Google Maps Response (minified) | 2,705 | 1,916 | 1,108 | 1.41× | Fused | Win |
| Google Maps Response (prettified) | 4,139 | 4,168 | 2,625 | 0.99× | Two-stage | Win |
| Instruments (minified) | 3,301 | 2,289 | 1,398 | 1.44× | Fused | Win |
| Instruments (prettified) | 3,493 | 3,847 | 2,582 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 764 | 732 | 614 | 1.04× | Fused | Win |
| Marine IK Reverse (prettified) | 3,288 | 3,362 | 2,912 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 771 | 721 | 599 | 1.07× | Fused | Win |
| Marine IK (prettified) | 3,280 | 3,283 | 2,813 | 1.00× | Tie | Win |
| Mesh (minified) | 1,074 | 1,146 | 1,172 | 0.94× | Two-stage | Loss |
| Mesh (prettified) | 1,786 | 2,062 | 2,203 | 0.87× | Two-stage | Loss |
| Random (minified) | 1,449 | 1,409 | 855 | 1.03× | Fused | Win |
| Random (prettified) | 2,109 | 2,442 | 1,570 | 0.86× | Two-stage | Win |
| Twitter (minified) | 1,988 | 1,797 | 1,330 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,981 | 2,567 | 1,923 | 0.77× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,617 | 248 | 179 | 6.51× | Fused | Win |
| Double (POD) | 841 | 309 | 143 | 2.72× | Fused | Win |
| Int64 (POD) | 2,187 | 677 | 470 | 3.23× | Fused | Win |
| String (POD) | 1,139 | 719 | 759 | 1.58× | Fused | Win |
| Uint64 (POD) | 2,230 | 673 | 442 | 3.31× | Fused | Win |
| Canada (minified) | 654 | 608 | 457 | 1.08× | Fused | Win |
| Canada (prettified) | 1,824 | 1,649 | 1,358 | 1.11× | Fused | Win |
| CitmCatalog (minified) | 1,943 | 1,259 | 1,067 | 1.54× | Fused | Win |
| CitmCatalog (prettified) | 3,276 | 2,825 | 2,436 | 1.16× | Fused | Win |
| Discord (minified) | 1,683 | 1,136 | 946 | 1.48× | Fused | Win |
| Discord (prettified) | 1,981 | 1,739 | 1,449 | 1.14× | Fused | Win |
| Google Maps Response (minified) | 1,905 | 1,140 | 727 | 1.67× | Fused | Win |
| Google Maps Response (prettified) | 2,925 | 2,112 | 1,649 | 1.39× | Fused | Win |
| Instruments (minified) | 2,521 | 1,497 | 1,161 | 1.68× | Fused | Win |
| Instruments (prettified) | 1,385 | 2,479 | 1,982 | 0.56× | Two-stage | Win |
| Marine IK Reverse (minified) | 621 | 512 | 443 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | 2,429 | 2,248 | 1,923 | 1.08× | Fused | Win |
| Marine IK (minified) | 658 | 551 | 443 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,541 | 2,353 | 1,922 | 1.08× | Fused | Win |
| Mesh (minified) | 998 | 878 | 842 | 1.14× | Fused | Win |
| Mesh (prettified) | 1,385 | 1,525 | 1,498 | 0.91× | Two-stage | Win |
| Random (minified) | 1,167 | 860 | 638 | 1.36× | Fused | Win |
| Random (prettified) | 1,690 | 1,292 | 1,139 | 1.31× | Fused | Win |
| Twitter (prettified) | 1,860 | 2,040 | 1,470 | 0.91× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,067 | 241 | 183 | 4.43× | Fused | Win |
| Double (POD) | 860 | 274 | 168 | 3.14× | Fused | Win |
| Int64 (POD) | 2,077 | 613 | 453 | 3.39× | Fused | Win |
| String (POD) | 1,161 | 653 | 822 | 1.78× | Fused | Win |
| Uint64 (POD) | 2,012 | 594 | 448 | 3.39× | Fused | Win |
| Canada (minified) | 727 | 645 | 441 | 1.13× | Fused | Win |
| Canada (prettified) | 1,848 | 1,722 | 1,315 | 1.07× | Fused | Win |
| CitmCatalog (minified) | 2,078 | 1,391 | 981 | 1.49× | Fused | Win |
| CitmCatalog (prettified) | 3,670 | 3,104 | 2,343 | 1.18× | Fused | Win |
| Discord (minified) | 2,357 | 1,186 | 1,275 | 1.99× | Fused | Win |
| Discord (prettified) | 2,530 | 1,776 | 1,996 | 1.42× | Fused | Win |
| Google Maps Response (minified) | 2,195 | 1,149 | 872 | 1.91× | Fused | Win |
| Google Maps Response (prettified) | 3,355 | 2,139 | 2,222 | 1.57× | Fused | Win |
| Instruments (minified) | 1,431 | 1,420 | 1,009 | 1.01× | Tie | Win |
| Marine IK (minified) | 597 | 558 | 363 | 1.07× | Fused | Win |
| Marine IK (prettified) | 2,423 | 2,032 | 1,605 | 1.19× | Fused | Win |
| Random (minified) | 1,630 | 831 | 696 | 1.96× | Fused | Win |
| Twitter (minified) | 1,912 | 1,177 | 1,289 | 1.62× | Fused | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 590 | 137 | 129 | 4.30× | Fused | Win |
| Double (POD) | 687 | 330 | 179 | 2.08× | Fused | Win |
| Int64 (POD) | 1,987 | 607 | 400 | 3.27× | Fused | Win |
| String (POD) | 1,638 | 939 | 857 | 1.74× | Fused | Win |
| Uint64 (POD) | 2,014 | 636 | 403 | 3.17× | Fused | Win |
| Canada (minified) | 691 | 622 | 401 | 1.11× | Fused | Win |
| Canada (prettified) | 1,756 | 1,928 | 1,227 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 1,764 | 1,425 | 538 | 1.24× | Fused | Win |
| CitmCatalog (prettified) | 2,891 | 3,412 | 1,417 | 0.85× | Two-stage | Win |
| Discord (minified) | 1,558 | 1,397 | 725 | 1.12× | Fused | Win |
| Discord (prettified) | 1,862 | 2,185 | 1,173 | 0.85× | Two-stage | Win |
| Google Maps Response (minified) | 1,342 | 1,303 | 543 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,361 | 2,929 | 1,335 | 0.81× | Two-stage | Win |
| Instruments (minified) | 2,484 | 1,569 | 726 | 1.58× | Fused | Win |
| Instruments (prettified) | 2,341 | 2,851 | 1,348 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 534 | 474 | 36 | 1.13× | Fused | Win |
| Marine IK (minified) | 548 | 477 | 264 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,161 | 2,358 | 1,318 | 0.92× | Two-stage | Win |
| Mesh (minified) | 958 | 835 | 450 | 1.15× | Fused | Win |
| Mesh (prettified) | 1,223 | 1,518 | 833 | 0.81× | Two-stage | Win |
| Random (minified) | 1,240 | 1,167 | 616 | 1.06× | Fused | Win |
| Random (prettified) | 1,796 | 2,072 | 1,135 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,729 | 1,510 | 949 | 1.14× | Fused | Win |
| Twitter (prettified) | 1,754 | 2,027 | 1,332 | 0.87× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,927 | 372 | 232 | 5.19× | Fused | Win |
| Double (POD) | 1,353 | 461 | 276 | 2.94× | Fused | Win |
| Int64 (POD) | 3,284 | 1,065 | 561 | 3.08× | Fused | Win |
| String (POD) | 2,732 | 1,497 | 1,501 | 1.83× | Fused | Win |
| Uint64 (POD) | 3,600 | 1,056 | 582 | 3.41× | Fused | Win |
| Canada (minified) | 1,190 | 1,143 | 751 | 1.04× | Fused | Win |
| Canada (prettified) | 2,985 | 3,117 | 2,237 | 0.96× | Two-stage | Win |
| CitmCatalog (minified) | 3,065 | 2,169 | 1,409 | 1.41× | Fused | Win |
| CitmCatalog (prettified) | 4,416 | 5,010 | 3,580 | 0.88× | Two-stage | Win |
| Discord (minified) | 3,198 | 2,475 | 1,915 | 1.29× | Fused | Win |
| Discord (prettified) | 2,599 | 3,732 | 2,987 | 0.70× | Two-stage | Win |
| Google Maps Response (minified) | 3,093 | 2,183 | 1,503 | 1.42× | Fused | Win |
| Google Maps Response (prettified) | 4,009 | 4,686 | 3,483 | 0.86× | Two-stage | Win |
| Instruments (minified) | 3,160 | 2,261 | 1,916 | 1.40× | Fused | Win |
| Instruments (prettified) | 3,092 | 3,809 | 3,494 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 960 | 849 | 660 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 3,602 | 3,726 | 3,051 | 0.97× | Two-stage | Win |
| Marine IK (minified) | 982 | 869 | 669 | 1.13× | Fused | Win |
| Marine IK (prettified) | 3,650 | 3,810 | 3,098 | 0.96× | Two-stage | Win |
| Mesh (minified) | 1,511 | 1,282 | 1,291 | 1.18× | Fused | Win |
| Mesh (prettified) | 2,007 | 2,345 | 2,362 | 0.86× | Two-stage | Loss |
| Random (minified) | 2,593 | 2,013 | 1,431 | 1.29× | Fused | Win |
| Random (prettified) | 2,810 | 3,271 | 2,534 | 0.86× | Two-stage | Win |
| Twitter (minified) | 2,499 | 2,520 | 2,150 | 0.99× | Two-stage | Win |
| Twitter (prettified) | 2,362 | 3,480 | 3,056 | 0.68× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,737 | 264 | 188 | 6.57× | Fused | Win |
| Double (POD) | 1,239 | 331 | 232 | 3.75× | Fused | Win |
| Int64 (POD) | 3,651 | 839 | 523 | 4.35× | Fused | Win |
| String (POD) | 2,617 | 1,294 | 1,248 | 2.02× | Fused | Win |
| Uint64 (POD) | 3,786 | 876 | 523 | 4.32× | Fused | Win |
| Canada (minified) | 1,082 | 1,083 | 924 | 1.00× | Tie | Win |
| Canada (prettified) | 2,804 | 3,088 | 2,692 | 0.91× | Two-stage | Win |
| CitmCatalog (minified) | 3,037 | 2,241 | 1,624 | 1.36× | Fused | Win |
| CitmCatalog (prettified) | 4,451 | 5,059 | 3,961 | 0.88× | Two-stage | Win |
| Discord (minified) | 3,277 | 2,405 | 1,813 | 1.36× | Fused | Win |
| Discord (prettified) | 3,135 | 3,635 | 2,849 | 0.86× | Two-stage | Win |
| Google Maps Response (minified) | 3,014 | 2,083 | 1,186 | 1.45× | Fused | Win |
| Google Maps Response (prettified) | 4,429 | 4,517 | 2,794 | 0.98× | Two-stage | Win |
| Instruments (minified) | 3,936 | 2,586 | 1,506 | 1.52× | Fused | Win |
| Instruments (prettified) | 3,818 | 4,275 | 2,752 | 0.89× | Two-stage | Win |
| Marine IK Reverse (minified) | 837 | 796 | 672 | 1.05× | Fused | Win |
| Marine IK Reverse (prettified) | 3,566 | 3,639 | 3,161 | 0.98× | Two-stage | Win |
| Marine IK (minified) | 853 | 796 | 663 | 1.07× | Fused | Win |
| Marine IK (prettified) | 3,582 | 3,612 | 3,036 | 0.99× | Two-stage | Win |
| Mesh (minified) | 1,140 | 1,199 | 1,317 | 0.95× | Two-stage | Loss |
| Mesh (prettified) | 1,876 | 2,167 | 2,428 | 0.87× | Two-stage | Loss |
| Random (minified) | 2,072 | 1,990 | 1,067 | 1.04× | Fused | Win |
| Random (prettified) | 2,719 | 3,338 | 1,921 | 0.81× | Two-stage | Win |
| Twitter (minified) | 2,554 | 2,251 | 1,572 | 1.13× | Fused | Win |
| Twitter (prettified) | 2,295 | 3,130 | 2,251 | 0.73× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,725 | 247 | 196 | 6.99× | Fused | Win |
| Double (POD) | 984 | 342 | 141 | 2.88× | Fused | Win |
| Int64 (POD) | 2,702 | 712 | 488 | 3.80× | Fused | Win |
| String (POD) | 1,982 | 955 | 1,066 | 2.08× | Fused | Win |
| Uint64 (POD) | 2,727 | 697 | 452 | 3.91× | Fused | Win |
| Canada (minified) | 801 | 730 | 553 | 1.10× | Fused | Win |
| Canada (prettified) | 2,175 | 1,907 | 1,553 | 1.14× | Fused | Win |
| CitmCatalog (minified) | 2,439 | 1,451 | 1,192 | 1.68× | Fused | Win |
| CitmCatalog (prettified) | 3,745 | 3,173 | 2,656 | 1.18× | Fused | Win |
| Discord (minified) | 2,291 | 1,442 | 1,144 | 1.59× | Fused | Win |
| Discord (prettified) | 2,437 | 2,153 | 1,736 | 1.13× | Fused | Win |
| Google Maps Response (minified) | 2,227 | 1,286 | 785 | 1.73× | Fused | Win |
| Google Maps Response (prettified) | 3,203 | 2,564 | 1,762 | 1.25× | Fused | Win |
| Instruments (minified) | 3,005 | 1,646 | 1,256 | 1.83× | Fused | Win |
| Instruments (prettified) | 3,312 | 2,700 | 2,121 | 1.23× | Fused | Win |
| Marine IK Reverse (minified) | 713 | 576 | 489 | 1.24× | Fused | Win |
| Marine IK Reverse (prettified) | 2,683 | 2,460 | 2,082 | 1.09× | Fused | Win |
| Marine IK (minified) | 761 | 621 | 487 | 1.23× | Fused | Win |
| Marine IK (prettified) | 2,818 | 2,582 | 2,077 | 1.09× | Fused | Win |
| Mesh (minified) | 1,064 | 920 | 919 | 1.16× | Fused | Win |
| Mesh (prettified) | 1,433 | 1,591 | 1,637 | 0.90× | Two-stage | Loss |
| Random (minified) | 1,866 | 1,232 | 762 | 1.51× | Fused | Win |
| Random (prettified) | 2,353 | 1,902 | 1,337 | 1.24× | Fused | Win |
| Twitter (prettified) | 2,178 | 2,431 | 1,796 | 0.90× | Two-stage | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,132 | 241 | 182 | 4.70× | Fused | Win |
| Double (POD) | 1,030 | 293 | 192 | 3.52× | Fused | Win |
| Int64 (POD) | 2,533 | 629 | 470 | 4.02× | Fused | Win |
| String (POD) | 2,079 | 866 | 1,182 | 2.40× | Fused | Win |
| Uint64 (POD) | 2,451 | 616 | 463 | 3.98× | Fused | Win |
| Canada (minified) | 907 | 787 | 505 | 1.15× | Fused | Win |
| CitmCatalog (minified) | 2,587 | 1,605 | 1,077 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | 4,182 | 3,540 | 2,517 | 1.18× | Fused | Win |
| Discord (minified) | 2,982 | 1,558 | 1,503 | 1.91× | Fused | Win |
| Discord (prettified) | 2,938 | 2,298 | 2,338 | 1.28× | Fused | Win |
| Google Maps Response (minified) | 2,339 | 1,318 | 1,022 | 1.77× | Fused | Win |
| Google Maps Response (prettified) | 3,495 | 2,633 | 2,341 | 1.33× | Fused | Win |
| Instruments (minified) | 1,340 | 1,522 | 1,075 | 0.88× | Two-stage | Win |
| Instruments (prettified) | 2,573 | 2,282 | 1,918 | 1.13× | Fused | Win |
| Marine IK Reverse (prettified) | 2,486 | 2,296 | 1,803 | 1.08× | Fused | Win |
| Mesh (minified) | 1,039 | 936 | 777 | 1.11× | Fused | Win |
| Mesh (prettified) | 1,429 | 1,621 | 1,430 | 0.88× | Two-stage | Win |
| Twitter (prettified) | 2,556 | 2,541 | 2,172 | 1.01× | Tie | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins all POD input and 28 of the 30 minified documents, and 28 of the 29 converged prettified documents go to the tape, with the last a tie. MSVC and Linux/Clang split the same way: minified to the fused path, prettified to the tape. On the M1 the fused path wins all POD input, 16 of the 17 converged minified documents with one tie, and 12 of the 15 converged prettified documents.

**POD-type tests: the fused path wins all 25 freshly allocated and all 25 reused, on every build.** It runs them 1.3× (String, Windows/MSVC) to 6.5× (Bool, macOS/GCC) faster than the two-stage path, and 1.7× to 7.0× faster with objects reused. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the platform.

**Minified documents: the fused path's everywhere but two documents on Linux/GCC.** The fused path wins 44 of the 47 converged minified documents: all 10 under MSVC, all 10 on Linux/Clang, 8 of 10 on Linux/GCC and 16 of the 17 that converged on macOS. On Linux/GCC the two-stage path wins minified Canada by 1% (916 against 906 MB/s) and minified Mesh by 7%; on macOS/Clang minified Instruments is a tie (1,431 against 1,420 MB/s). The fused path's margins run from 3% (Random on Linux/GCC) to 99% (Discord on macOS/Clang, 2,357 MB/s against the two-stage path's 1,186; Random there is 96%); on Linux/GCC they are widest on Google Maps (41%) and Instruments (44%), and under MSVC on CitmCatalog (22%) and Instruments (46%). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 28 of 29.** It wins all 9 that converged under MSVC, by 7% (Canada and Marine IK) to 23% (Mesh); all 10 on Linux/Clang, by 2% (Marine IK Reverse) to 35% (Discord); and 9 of 10 on Linux/GCC, by 1% (Google Maps) to 30% (Twitter). The exception is prettified Marine IK on Linux/GCC, a tie at 3,280 against 3,283 MB/s freshly allocated; with objects reused the tape takes it by 1%, and takes all 29 prettified x86 documents. The two Marine IK documents, which are mostly numbers, are among the closest on every x86 build (0% to 7%), and Twitter is among the widest on every build (18% to 30%). The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That keeps the number-heavy documents close, but on x86 it does not win them. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis.

**Prettified documents on the M1: mostly the fused path.** Of the 15 prettified documents that converged freshly allocated, the fused path wins 12 and the tape wins 3, all on macOS/GCC. There the fused path wins seven, by 8% (both Marine IK documents) to 39% (Google Maps), and the tape wins Mesh (10%), Twitter (10%) and Instruments (79%, 2,479 against 1,385 MB/s). On macOS/Clang the fused path wins all five that converged, by 7% (Canada) to 57% (Google Maps). The Instruments result on macOS/GCC does not survive reuse: with objects reused the fused path runs it at 3,312 MB/s and wins by 23%. Reused, the fused path wins 13 of the 17 converged prettified documents on the M1, the tape takes Mesh on both builds (11% on macOS/GCC, 13% on macOS/Clang) and Twitter on macOS/GCC (12%), and Twitter on macOS/Clang is a tie. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it. Five prettified documents on macOS/Clang did not converge freshly allocated, so the M1 split rests on fewer tests than the x86 one.

**Mesh is where Jsonifier is weakest.** The two-stage path wins prettified Mesh on all three x86 builds, by 15% (Linux/Clang and Linux/GCC) to 23% (Windows/MSVC), and minified Mesh on Linux/GCC by 7%. Mesh on Linux/GCC is also the only document on which both Jsonifier paths lose to simdjson On Demand freshly allocated, in both forms, by 2% minified and 6% prettified; with objects reused, prettified Mesh on Linux/Clang (by under 1%) and macOS/GCC (by 3%) joins it. We have not profiled what is particular about this document.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 9 minified documents in declared key order, the fused path reaches a median 59% of its Linux/Clang throughput, the two-stage path 59% and simdjson 42%. Because the two Jsonifier paths lose about the same share, MSVC routes exactly as Linux/Clang does: minified to the fused path, prettified to the tape. The spread between documents is wider for the fused path (45% on Google Maps to 72% on Instruments) than for the two-stage path (55% to 66%). That fits the fused path being branch-heavy code whose performance rests on the optimizer while stage 1 is branch-free intrinsic code whose shape is fixed by the source, but we have not isolated it.

**Against simdjson, Jsonifier wins 114 of 116 tests with whichever path is faster.** It loses two, both Mesh on Linux/GCC. The fused path alone wins 108 and loses 8: those two, prettified Discord, Instruments, Mesh and Twitter on Linux/Clang, and prettified Instruments and Mesh on macOS/GCC.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it does: simdjson parses minified Marine IK Reverse at 36 MB/s, against 254 MB/s for the same document in declared order, so the faster Jsonifier path is 13.6× faster than simdjson freshly allocated (14.8× reused). The prettified form did not converge under MSVC. On the other builds simdjson parses the reversed and declared-order documents within 4% of each other, and the faster Jsonifier path is 1.15× to 1.43× faster than simdjson freshly allocated and 1.15× to 1.46× reused. Why the rescan cost shows up only under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb. On x86 the fused path is right for POD and minified input, apart from Mesh and Canada on Linux/GCC, and prettified input belongs to the two-stage path on all three compilers. On the M1 the fused path is right for POD and minified input and for most prettified documents, but not all of them. Routing each test to its faster path would raise the record against simdjson from 108 wins and 8 losses to 114 wins and 2 losses. Six tests change hands, all prettified: four on Linux/Clang and two on macOS/GCC. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents, not the POD-type tests. The tables list every test where both converged.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 916 | 872 | 1.05× | Win |
| Canada (prettified) | 2,642 | 2,503 | 1.06× | Win |
| CitmCatalog (minified) | 1,873 | 1,439 | 1.30× | Win |
| CitmCatalog (prettified) | 4,379 | 3,579 | 1.22× | Win |
| Discord (minified) | 1,812 | 1,549 | 1.17× | Win |
| Discord (prettified) | 2,836 | 2,490 | 1.14× | Win |
| Google Maps Response (minified) | 1,916 | 1,316 | 1.46× | Win |
| Google Maps Response (prettified) | 4,168 | 3,036 | 1.37× | Win |
| Instruments (minified) | 2,289 | 1,856 | 1.23× | Win |
| Instruments (prettified) | 3,847 | 3,358 | 1.15× | Win |
| Marine IK Reverse (minified) | 732 | 735 | 1.00× | **Loss** |
| Marine IK Reverse (prettified) | 3,362 | 3,308 | 1.02× | Win |
| Marine IK (minified) | 721 | 723 | 1.00× | **Loss** |
| Marine IK (prettified) | 3,283 | 3,264 | 1.01× | Tie |
| Mesh (minified) | 1,146 | 1,149 | 1.00× | Tie |
| Mesh (prettified) | 2,062 | 2,145 | 0.96× | **Loss** |
| Random (minified) | 1,409 | 1,184 | 1.19× | Win |
| Random (prettified) | 2,442 | 2,111 | 1.16× | Win |
| Twitter (minified) | 1,797 | 1,626 | 1.10× | Win |
| Twitter (prettified) | 2,567 | 2,335 | 1.10× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 608 | 536 | 1.13× | Win |
| Canada (prettified) | 1,649 | 1,514 | 1.09× | Win |
| CitmCatalog (minified) | 1,259 | 1,045 | 1.20× | Win |
| CitmCatalog (prettified) | 2,825 | 2,392 | 1.18× | Win |
| Discord (minified) | 1,136 | 1,010 | 1.12× | Win |
| Discord (prettified) | 1,739 | 1,548 | 1.12× | Win |
| Google Maps Response (minified) | 1,140 | 939 | 1.21× | Win |
| Google Maps Response (prettified) | 2,112 | 2,056 | 1.03× | Win |
| Instruments (minified) | 1,497 | 1,199 | 1.25× | Win |
| Instruments (prettified) | 2,479 | 2,018 | 1.23× | Win |
| Marine IK Reverse (minified) | 512 | 475 | 1.08× | Win |
| Marine IK Reverse (prettified) | 2,248 | 2,028 | 1.11× | Win |
| Marine IK (minified) | 551 | 469 | 1.18× | Win |
| Marine IK (prettified) | 2,353 | 2,024 | 1.16× | Win |
| Mesh (minified) | 878 | 639 | 1.37× | Win |
| Mesh (prettified) | 1,525 | 1,158 | 1.32× | Win |
| Random (minified) | 860 | 716 | 1.20× | Win |
| Random (prettified) | 1,292 | 1,257 | 1.03× | Win |
| Twitter (prettified) | 2,040 | 1,591 | 1.28× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,083 | 1,044 | 1.04× | Win |
| Canada (prettified) | 3,088 | 2,952 | 1.05× | Win |
| CitmCatalog (minified) | 2,241 | 1,698 | 1.32× | Win |
| CitmCatalog (prettified) | 5,059 | 4,120 | 1.23× | Win |
| Discord (minified) | 2,405 | 1,815 | 1.33× | Win |
| Discord (prettified) | 3,635 | 2,845 | 1.28× | Win |
| Google Maps Response (minified) | 2,083 | 1,384 | 1.51× | Win |
| Google Maps Response (prettified) | 4,517 | 3,183 | 1.42× | Win |
| Instruments (minified) | 2,586 | 1,923 | 1.35× | Win |
| Instruments (prettified) | 4,275 | 3,466 | 1.23× | Win |
| Marine IK Reverse (minified) | 796 | 824 | 0.97× | **Loss** |
| Marine IK Reverse (prettified) | 3,639 | 3,587 | 1.01× | Win |
| Marine IK (minified) | 796 | 816 | 0.97× | **Loss** |
| Marine IK (prettified) | 3,612 | 3,653 | 0.99× | **Loss** |
| Mesh (minified) | 1,199 | 1,224 | 0.98× | **Loss** |
| Mesh (prettified) | 2,167 | 2,267 | 0.96× | **Loss** |
| Random (minified) | 1,990 | 1,461 | 1.36× | Win |
| Random (prettified) | 3,338 | 2,567 | 1.30× | Win |
| Twitter (minified) | 2,251 | 1,952 | 1.15× | Win |
| Twitter (prettified) | 3,130 | 2,614 | 1.20× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 730 | 636 | 1.15× | Win |
| Canada (prettified) | 1,907 | 1,754 | 1.09× | Win |
| CitmCatalog (minified) | 1,451 | 1,153 | 1.26× | Win |
| CitmCatalog (prettified) | 3,173 | 2,577 | 1.23× | Win |
| Discord (minified) | 1,442 | 1,139 | 1.27× | Win |
| Discord (prettified) | 2,153 | 1,727 | 1.25× | Win |
| Google Maps Response (minified) | 1,286 | 1,009 | 1.27× | Win |
| Google Maps Response (prettified) | 2,564 | 2,169 | 1.18× | Win |
| Instruments (minified) | 1,646 | 1,238 | 1.33× | Win |
| Instruments (prettified) | 2,700 | 2,078 | 1.30× | Win |
| Marine IK Reverse (minified) | 576 | 525 | 1.10× | Win |
| Marine IK Reverse (prettified) | 2,460 | 2,195 | 1.12× | Win |
| Marine IK (minified) | 621 | 524 | 1.18× | Win |
| Marine IK (prettified) | 2,582 | 2,191 | 1.18× | Win |
| Mesh (minified) | 920 | 661 | 1.39× | Win |
| Mesh (prettified) | 1,591 | 1,196 | 1.33× | Win |
| Random (minified) | 1,232 | 815 | 1.51× | Win |
| Random (prettified) | 1,902 | 1,417 | 1.34× | Win |
| Twitter (prettified) | 2,431 | 1,724 | 1.41× | Win |

In the freshly allocated run, two-stage Jsonifier wins 34 of these 39 tests, ties 2 and loses 3. On Linux/GCC it wins 15 of 20, by 2% (Marine IK Reverse, prettified) to 46% (Google Maps, minified), ties prettified Marine IK and minified Mesh, and is slower on minified Marine IK and Marine IK Reverse (by under 1%) and prettified Mesh (by 4%). Mesh is also where Jsonifier is weakest against simdjson On Demand on that build (§2.4), so that loss looks like a property of the document under GCC on x86 rather than of the tape; we have not profiled it. On macOS/GCC it wins all 19, by 3% (Google Maps and Random, prettified) to 37% (Mesh, minified). With the fused path instead of the two-stage one, Jsonifier beats the reflection reader on 12 of 20 tests on Linux/GCC (two ties) and on 18 of 19 on macOS/GCC. With objects reused, two-stage Jsonifier wins 34 of 39: on Linux/GCC it wins 15 of 20 and is slower on both Marine IK documents in minified form (by 3% and 2%), prettified Marine IK (by 1%) and Mesh in both forms (by 2% minified and 4% prettified); on macOS/GCC it wins all 19, by 9% (Canada, prettified) to 51% (Google Maps and Random, minified).

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 5% on all 20 results, and within 2% on all but one; the widest gap is reused minified Twitter under MSVC, where the "jsonifier" row runs 4% slower (5,874 against 6,139 MB/s). Every build converged both forms in both runs.

#### Freshly allocated objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 5,622 | 1,920 | 2.93× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 7,100 | 2,678 | 2.65× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 6,973 | 5,737 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | 8,960 | 7,573 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | minified | 7,324 | 5,427 | 1.35× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | prettified | 8,845 | 7,139 | 1.24× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 4,135 | 3,093 | 1.34× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | 4,785 | 3,898 | 1.23× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | 3,951 | 3,370 | 1.17× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,742 | 4,284 | 1.11× | Win |

#### Reused objects

| Platform / Compiler | Document form | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | minified | 6,139 | 1,957 | 3.14× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | prettified | 7,256 | 2,725 | 2.66× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | minified | 7,152 | 5,796 | 1.23× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | prettified | 9,024 | 7,632 | 1.18× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | minified | 7,498 | 5,528 | 1.36× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | prettified | 9,000 | 7,205 | 1.25× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | minified | 4,263 | 3,186 | 1.34× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | prettified | 4,971 | 4,003 | 1.24× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | minified | 4,114 | 3,453 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | prettified | 4,879 | 4,003 | 1.22× | Win |

Jsonifier wins all 20 results, by 1.11× (prettified on macOS/Clang, freshly allocated) to 3.14× (minified under MSVC, reused). Throughput here is counted over the whole document, so these figures run well above the full-parse ones: on minified Twitter the fused path parses at 1,209 MB/s under MSVC and 1,987 MB/s on Linux/Clang when it materializes every value, against 5,622 and 6,973 MB/s here, because the bytes the caller does not ask for are skipped through the tape instead of being walked. The margin over simdjson is 2.65× to 3.14× under MSVC and 1.11× to 1.36× elsewhere. On minified Twitter in the freshly allocated run, simdjson On Demand under MSVC reaches 33% of its Linux/Clang throughput and Jsonifier's stage 1 + 2 path reaches 81%. This is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

## 3. The single-pass path: `json_cursor` over raw text

The fused path is implemented as `json_cursor<parseOpts, read_buffer_ptr>` — a set of static navigation primitives over a raw `read_buffer_ptr` cursor, with the end pointer, the detected indentation and the string buffer held in a `parse_context` and the nesting depth passed down the call chain, walking the document exactly once. Several design decisions distinguish it:

**Compile-time key fusion.** For each reflected member, the expected token is baked into the binary at compile time as a fused literal. In minified known-order mode, an entire member header — leading comma, quoted key, and colon — collapses into a single constant:

```cpp
template<uint64_t index, typename literal_type> JSONIFIER_INLINE static constexpr auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
	if constexpr (index > 0) {
		return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	} else {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}
}
```

When the document arrives in the expected order — the overwhelmingly common case for machine-generated JSON — matching `,"name":` is one constant-length comparison, and the cursor jumps the entire header in a single add. No tokenizer, no tape, no per-character state machine.

**Adaptive out-of-order recovery.** When the known-order guess misses, the parser falls back to a compile-time hash map over the type's keys (`hash_map<value_type>::findIndex`), dispatches through a generated table, and — crucially — *learns*: a thread-local `antiHashStatesNew` array records which index actually matched at each slot, so a document stream with a consistently permuted key order pays the hash cost once and then rides the corrected fast path.

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans — and skips — every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, turning an O(members) parse into an O(members²) crawl over structural positions — per object, per document, forever. The model has no memory; the millionth permuted document costs exactly what the first one did.

Jsonifier's failure mode for the same situation is: one hash lookup, one dispatch-table indirection, and a learned correction. The mismatch cost is paid once per key slot per thread, not once per object instance. A feed of a billion documents with keys in reversed order parses at effectively the same throughput as a feed in declared order, because after the first document the "expected order" *is* the observed order. The schema-directed model converts key order from a per-document runtime tax into a per-stream calibration.

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `context.indentSize * depth`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (string unescaping, discussed in §6) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra with several architectural departures.

### 4.1 Step geometry as a per-compiler compile-time constant

simdjson classifies input per 64-byte block, and its generic indexer steps through the buffer 128 bytes per iteration — two 64-byte blocks, software-pipelined one block deep, a geometry it retains even on its widest kernel (the Icelake implementation invokes `index<128>`). Jsonifier generalizes the unit of work to a *step* of `simdBlocksPerStep` 64-byte blocks, with the constant chosen per ISA **and per compiler**:

```cpp
template<> struct backend_traits<jsonifier_backend::avx2> : compiler_step_tuning<4, 4, 4, 8, 4, 8> {
	static constexpr const char* name{ "AVX2" };
	using simd_int_t = jsonifier_simd_int_256;
};
```

`compiler_step_tuning` takes (tape step, blocks per step) pairs for Clang, GCC and MSVC in that order and resolves to the current compiler's pair, so the AVX2 traits above carry 4 blocks per step on Clang and 8 on GCC and MSVC, with a tape step of 4 on all three. `simdBlocksPerStep` and `simdTapeStep` are read from the configured backend's traits.

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA reflects real differences in how each compiler schedules the unrolled block bodies: on AVX2 Clang takes four blocks (256 bytes) per step where GCC and MSVC take eight (512 bytes), and on NEON both compilers take four blocks per step with a drain burst of 8. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations — a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(const simd_array_t in_01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNext() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit — a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional — `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNextInString`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4` — the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm — `followsNonquoteScalar` carries the cross-block bit — so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
	const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
	const uint64_t op		   = simd::op_collector::impl(in_01, opTable, spaceMask);
	const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
	const uint64_t scalarStart = scalar & ~follows;
	return op | simd::rope_detector<rope_block>::quotes | scalarStart;
}
```

Note the overload pair: the minified variant omits the whitespace collector entirely — an entire classification lane deleted at compile time when the caller declares the input minified. This is the same philosophy as the compiler-specific step constants: every fact known before runtime is burned into the instruction stream.

### 4.4 The drain architecture: bits → indices

Extraction — converting each 64-bit structural mask into byte offsets on the tape — is where naive implementations serialize hard, because the classic loop (`tzcnt`, store, `blsr`, repeat) is a loop-carried dependency chain of length popcount.

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

**AVX2/AVX/NEON: the folded stepped drain.** Without byte-compress, extraction must use the tzcnt chain — but the chain's *structure* is still a compile-time decision. `write_indices_functor` emits one extract-advance pair per index; `write_indices_stepped_functor` groups them into unconditional bursts of `simdTapeStep` writes:

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

The `functor_runner`'s `implAnd` expands a stepped range sequence `<0, 64, simdTapeStep>` through an `&&`-fold: each group writes `simdTapeStep` indices *unconditionally* (over-writing garbage past the true count is harmless — the tape cursor only advances by the real popcount), and the fold short-circuits the moment a group's start index reaches the count. One predictable branch per `simdTapeStep` extractions instead of one per extraction.

Credit where due: simdjson's current generic `bit_indexer` implements the same stepped-burst pattern — `write_indexes_stepped<START, END, STEP>` via recursive template expansion, unconditional bursts, `simdjson_unlikely` short-circuit checks at each group boundary — with the burst size exposed as a build macro (`SIMDJSON_STRUCTURAL_INDEXER_STEP`, default 4). The two libraries have converged on the extraction inner loop itself. The divergences are in everything around it:

- **Coverage.** simdjson's stepped expansion runs to a fixed `STEP_UNTIL` of 24 set bits and falls back to a plain scalar loop for denser blocks; Jsonifier's fold covers the full 0..64 range at the same stride.
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep`, because the two constants interact. AVX2 uses a burst of 4 on all three compilers, with 4 blocks per step on Clang and 8 on GCC and MSVC; NEON uses a burst of 8 on both Clang and GCC, at 4 blocks per step (§4.1).
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step — up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, write_structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains — each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser — it is a *specialization* of the same `json_cursor` interface over `write_structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the cursor interface: `cursor_t<options, context_type>` picks the specialization from the context's `iterator_type`, and the `json_cursor<parseOpts, write_structural_index_ptr>` specialization supplies the token-navigation primitives:

```cpp
template<typename context_type> JSONIFIER_INLINE static bool skipValue(write_structural_index_ptr& iter, context_type& context) noexcept {
	if (iter >= context.endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>(iter, context);
	}
	const char first = static_cast<char>(*valuePtr(iter, context));
	if (first == '{' || first == '[') {
		int64_t depth{};
		while (iter < context.endIter) {
			depth += nestingDeltaTable[static_cast<uint8_t>(context.stringRoot[*iter])];
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

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per structural entry — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer cursor, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

The asymmetry defines the routing rule. When the caller wants *every* value (full-document parse into a reflected type), skips are rare and the tape is overhead. When the caller wants a *few* values from a large document (partial reading), or wants only the structure itself (prettify/minify, where the transformation is literally "copy bytes, adjusting whitespace at structural positions"), skips dominate and the tape converts O(bytes) navigation into O(structurals).

The same routing logic appears in stage 1's own entry point: `reset<minified>` selects between the whitespace-aware and whitespace-free classification pipelines, and the tail-block handling pads with `0x20`, indexes the pad, then retroactively pops any tape entries pointing past the true document length — branchless main loop, exact tape.

## 6. Distributed UTF-8 validation

Let's be precise about what simdjson does, because it is also an in-register scheme: `json_structural_indexer::next` feeds every 64-byte block into `utf8_checker::check_next_input` using the very registers the classifier just loaded. Validation is fused into stage 1, costs no extra pass over memory, and covers the entire input. (The standalone `generic_validate_utf8` exists too, for the buffer-validation API.) On Demand then additionally defers some string-level checks to traversal.

So the distinction is not "in-register versus dedicated pass" — both libraries validate in registers already in flight. The distinction is **scope and location**:

- **simdjson** validates *all input bytes*, in *stage 1*. Elegant and total, but it means the range checker runs over structural characters, whitespace, numbers, and literals — bytes that a JSON parser's own grammar already constrains to ASCII. For a document that is 70% non-string content, 70% of the validation work confirms what token matching would prove for free.
- **Jsonifier** validates *string bytes only*, in the *string parse loop*. Everything outside strings is implicitly ASCII-constrained: structural characters are matched literally, numbers pass through a digit-table parser, `true`/`false`/`null` are compared as packed integer constants — any non-ASCII byte in those positions is a parse error by construction, no range checker required. The only place arbitrary bytes can legally appear is inside strings, and that is exactly — and only — where `utf8_register_validator` runs.

One accounting qualifier belongs in the main text rather than a footnote, because it bounds the size of the claim: simdjson's checker leads with an ASCII fast path — a block whose registers OR to an ASCII-only result skips the multibyte carry chain entirely and pays roughly one reduction and one test. For ASCII-heavy input, then, "validation work proportional to input bytes" is proportional with a small constant, not with the full cost of the multibyte machinery. The scope distinction survives the qualifier — Jsonifier runs *no* validation instructions of any kind over non-string bytes, fast-path or otherwise, and the two approaches still separate cleanly on documents dense in non-ASCII string content — but the honest comparison states the fast path up front rather than burying it.

Jsonifier ships the same range-based algorithm simdjson uses (the `byte1High`/`byte1Low`/`byte2High` nibble-lookup classifier with the `carry`/`tooShort`/`tooLong`/`surrogate` error-bit algebra) in two deployments. The first, the standalone `validateUtf8` built on `utf8_checker`, is the conventional block validator, with the same ASCII fast path (`orAll` the block's registers, one test, skip the multibyte machinery). The second is the distributed one:

```cpp
if (delimiters == static_cast<integer_type>(0)) {
	validator.checkRegister(simdValue);
	string1Start += bytesProcessed;
	string2 += bytesProcessed;
	continue;
}
```

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte) — so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator — the position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The stage-1 companion paper covers this mechanism in full [§6.1 there].

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes* — every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on 44 of 47 minified documents and on 12 of 15 prettified documents on the M1; slower on 28 of 29 prettified x86 documents, with one tie (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared cursor interface |
| Out-of-order keys | Forward scan + wrap-around rescan per lookup, per object — no memory across documents | One hash fallback, then learned per-slot order correction (thread-local, per stream) |
| Schema knowledge (known-type workloads) | Exists in caller's traversal code — invisible to the library | Declared once via reflection — consumed by the architecture |
| Step size | 128 bytes (two 64-byte blocks), pipelined one block deep | 256–512 bytes, per-(ISA × compiler) constant |
| Bit extraction | Stepped tzcnt bursts (global STEP macro, scalar tail past 24 bits); VBMI2 compress on Icelake | Same primitives, but full-range folded bursts with per-toolchain-swept stride |
| Drain scheduling | Previous block drained during current block's classification | Whole-step deferred batch drain, lane offsets precomputed from popcounts |
| Escape short-circuit | Yes (zero-backslash fast exit) | Yes (same) |
| In-string prefix-XOR | Computed every block | Skipped for quoteless blocks |
| UTF-8 validation | In-register, fused into stage 1, over all input bytes (ASCII fast path per block); verdict at EOF | In-register, fused into string parsing, over string bytes only, carried across the width cascade; verdict per string |
| Specialization axis | Runtime CPU dispatch through virtual functions (core/DOM API); On Demand mostly fixed at compile time | Runtime tier selection by a cached enum fold over per-tier CRTP instantiations, no virtual calls; everything inside a tier is compile-time (Cathedral Architecture) |

The last row is the root of every other difference. Both libraries can ship one x86 binary that picks its instruction set at run time; they differ in what that choice costs and how much of the code it reaches. Jsonifier's founding constraint is that apart from the data, the only thing decided at run time is the tier, and that it is decided once. The schema, the compiler and the parse options are known at build time, and each ISA tier is a complete compile-time instantiation, so every routing decision in this paper (tape or no tape, whitespace lane or not, validation or not, 4 blocks or 8, burst of 1 or 4) is still resolved before the first byte is read.

On x86, one Jsonifier binary carries every instruction-set tier it was configured for and picks one at run time. The headers are compiled once per tier inside a target-attribute region (`backend_passes.hpp`: `#pragma GCC target` on GCC, `#pragma clang attribute` on Clang), which yields one complete `jsonifier_core_internal<backend>` per tier: AVX-512 (`avx512f`, `avx512bw`, `avx512vbmi2`), AVX2 and AVX. Each of those inherits its tier's parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call inside a tier is a direct, inlinable call into code built for that tier. On first use, `selectSupportedBackend` reads CPUID (leaves 0, 1 and 7, plus the extended leaves for `lzcnt`) and XCR0, so a tier counts only if both the CPU and the operating system support it, and caches the widest supported tier in a function-local `static`. Every public entry point then dispatches with a fold expression over the compiled tiers:

```cpp
static const jsonifier_backend backend{ selectedBackend() };
static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
```

That is a load of a cached enum and at most three compares, once per call. There is no function pointer and no vtable. simdjson's runtime dispatch, by contrast, goes through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its own On Demand documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target. In Jsonifier the same dispatch covers every API, including typed parsing, serialization and the generic On Demand-style iterator, and inside a tier nothing is virtual, so the per-tier code keeps all of its compile-time specialization.

The scope is x86. ARM builds compile one backend (NEON, or SVE2 when configured), as simdjson's ARM builds do. The set of x86 tiers compiled in is bounded by the configured tier (`JSONIFIER_CONFIGURED_AVX_TIER`), so a build configured for AVX2 carries AVX2 and AVX but not AVX-512. We have not separately measured the cost of the per-call check.

## 8. Conclusion

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head, on both freshly allocated and reused target objects. Across 116 freshly allocated tests on five platforms the fused path wins 81, ties 2 and loses 33, and where it loses is a matter of layout and architecture more than of principle. It wins all 25 POD-type tests, by up to 6.5× (Bool on macOS/GCC), 44 of the 47 minified documents, and 12 of the 15 prettified documents that converged on the M1. The two-stage path wins 28 of the 29 prettified documents on x86, on all three compilers, and ties the last. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 114 of those 116 tests, its own stage-1 + reflection path beats simdjson's stage-1 + reflection reader on 34 of 39, and on the partial-reading test, where the index is the right tool, it beats simdjson On Demand on all 20 results. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*