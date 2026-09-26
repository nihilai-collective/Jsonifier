# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model — a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2) — has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization — so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary, skipping the tape wins on POD-type data on every build, but for real documents the faster path depends on the compiler and on whether the input is indented.

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

For full-document parsing into known types, the tape is overhead in principle, but whether skipping it is faster in practice turns out to depend on the compiler and on the shape of the input. §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. The fused path wins every POD-type test and almost every minified document on Linux and macOS, and nearly everything on the M1. The two-stage path wins most prettified documents on x86, and half of the minified documents under MSVC.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from a single sweep run on October 1, 2026, with simdjson 610f14d (On Demand), BenchmarkSuite ced5b69, and Jsonifier e2e111b. The harness registers three libraries in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All three parse fully into the target data structures and perform UTF-8 validation. Each library parses into one output object that lives for the whole test: its vectors, maps and strings are cleared or overwritten between iterations rather than freed and reallocated, as in simdjson's own benchmarks, so no iteration pays for allocating a fresh result. After each test the three outputs are serialized and compared against each other and against the source document. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, all libraries receive keys in document order.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on x86 (10% and 5% on the virtualized M1), and a test is ranked only if all three libraries converge, which is why some platforms have fewer than 25 tests. Ties are declared by Welch's t-test on the kept epoch. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to all three libraries. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced five ties between the two Jsonifier paths across 116 tests.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 78 tests, five are statistical ties, and the two-stage path wins 33. The split is almost entirely a matter of platform:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 10 | 0 | 14 | 24 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 14 | 0 | 9 | 23 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 16 | 2 | 7 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 16 | 3 | 2 | 21 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 22 | 0 | 1 | 23 of 25 |
| **Aggregate** | **78** | **5** | **33** | **116 of 125** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 9 / 0 / 0 | 8 / 0 / 0 |
| Minified corpus documents | 5 / 0 / 5 | 19 / 0 / 1 | 17 / 1 / 0 |
| Prettified corpus documents | 0 / 0 / 9 | 2 / 2 / 15 | 13 / 2 / 3 |

Against simdjson, counting a test for Jsonifier when either of its paths ranks above simdjson:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 | 22 / 0 / 2 | 21 / 0 / 3 |
| Linux / Clang 24.0 | 22 / 1 / 0 | 19 / 0 / 4 |
| Linux / GCC 16.1 | 22 / 0 / 3 | 22 / 0 / 3 |
| macOS / GCC 16.2 | 21 / 0 / 0 | 20 / 0 / 1 |
| macOS / Clang 23.1 | 23 / 0 / 0 | 23 / 0 / 0 |
| **Aggregate** | **110 / 1 / 5** | **105 / 0 / 11** |

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two ranked higher, and "n/c" means the test did not converge for at least one library.

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 539 | 176 | 126 | 3.07× | Fused | Win |
| Double (POD) | 674 | 330 | 177 | 2.05× | Fused | Win |
| Int64 (POD) | 2,257 | 731 | 413 | 3.09× | Fused | Win |
| String (POD) | 1,650 | 996 | 858 | 1.66× | Fused | Win |
| Uint64 (POD) | 2,871 | 809 | 421 | 3.55× | Fused | Win |
| Canada (minified) | 672 | 629 | 397 | 1.07× | Fused | Win |
| Canada (prettified) | 1,765 | 1,883 | 1,171 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 725 | 765 | 552 | 0.95× | Two-stage | Win |
| CitmCatalog (prettified) | 1,674 | 1,950 | 1,461 | 0.86× | Two-stage | Win |
| Discord (minified) | 858 | 805 | 763 | 1.07× | Fused | Win |
| Discord (prettified) | 1,242 | 1,302 | 1,217 | 0.95× | Two-stage | Win |
| Google Maps Response (minified) | 781 | 764 | 556 | 1.02× | Fused | Win |
| Google Maps Response (prettified) | 1,658 | 1,734 | 1,365 | 0.96× | Two-stage | Win |
| Instruments (minified) | 718 | 737 | 783 | 0.97× | Two-stage | **Loss** |
| Instruments (prettified) | 1,153 | 1,333 | 1,434 | 0.87× | Two-stage | **Loss** |
| Marine IK Reverse (minified) | 366 | 375 | 35 | 0.98× | Two-stage | Win |
| Marine IK Reverse (prettified) | n/c | n/c | n/c | — | — | — |
| Marine IK (minified) | 378 | 383 | 268 | 0.99× | Two-stage | Win |
| Marine IK (prettified) | 1,556 | 1,895 | 1,343 | 0.82× | Two-stage | Win |
| Mesh (minified) | 501 | 572 | 458 | 0.88× | Two-stage | Win |
| Mesh (prettified) | 747 | 1,040 | 858 | 0.72× | Two-stage | Win |
| Random (minified) | 780 | 746 | 620 | 1.05× | Fused | Win |
| Random (prettified) | 1,279 | 1,393 | 1,167 | 0.92× | Two-stage | Win |
| Twitter (minified) | 1,198 | 1,081 | 1,014 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,545 | 1,582 | 1,475 | 0.98× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,145 | 337 | 215 | 6.36× | Fused | Win |
| Double (POD) | n/c | n/c | n/c | — | — | — |
| Int64 (POD) | 3,361 | 1,029 | 599 | 3.27× | Fused | Win |
| String (POD) | 2,557 | 1,444 | 1,462 | 1.77× | Fused | Win |
| Uint64 (POD) | 3,923 | 1,000 | 592 | 3.92× | Fused | Win |
| Canada (minified) | 1,233 | 1,033 | 732 | 1.19× | Fused | Win |
| Canada (prettified) | 2,853 | 3,115 | 2,159 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,063 | 1,869 | 1,186 | 1.10× | Fused | Win |
| CitmCatalog (prettified) | 3,976 | 4,503 | 3,001 | 0.88× | Two-stage | Win |
| Discord (minified) | 2,311 | 1,945 | 1,480 | 1.19× | Fused | Win |
| Discord (prettified) | 2,255 | 2,956 | 2,360 | 0.76× | Two-stage | Win |
| Google Maps Response (minified) | 1,915 | 1,709 | 1,138 | 1.12× | Fused | Win |
| Google Maps Response (prettified) | 3,266 | 3,760 | 2,766 | 0.87× | Two-stage | Win |
| Instruments (minified) | 2,952 | 2,174 | 1,608 | 1.36× | Fused | Win |
| Instruments (prettified) | 2,906 | 3,856 | 2,954 | 0.75× | Two-stage | Win |
| Marine IK Reverse (minified) | 949 | 793 | 198 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 3,375 | 3,561 | 906 | 0.95× | Two-stage | Win |
| Marine IK (minified) | 976 | 815 | 615 | 1.20× | Fused | Win |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 1,463 | 1,229 | 1,265 | 1.19× | Fused | Win |
| Mesh (prettified) | 1,864 | 2,324 | 2,312 | 0.80× | Two-stage | Tie |
| Random (minified) | 1,866 | 1,553 | 1,098 | 1.20× | Fused | Win |
| Random (prettified) | 2,075 | 2,697 | 1,918 | 0.77× | Two-stage | Win |
| Twitter (minified) | 2,506 | 1,949 | 1,840 | 1.29× | Fused | Win |
| Twitter (prettified) | 2,421 | 2,739 | 2,610 | 0.88× | Two-stage | Win |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,243 | 367 | 183 | 6.11× | Fused | Win |
| Double (POD) | 1,321 | 434 | 228 | 3.04× | Fused | Win |
| Int64 (POD) | 3,124 | 836 | 534 | 3.74× | Fused | Win |
| String (POD) | 2,797 | 1,247 | 1,284 | 2.24× | Fused | Win |
| Uint64 (POD) | 3,577 | 1,044 | 610 | 3.43× | Fused | Win |
| Canada (minified) | 1,003 | 957 | 850 | 1.05× | Fused | Win |
| Canada (prettified) | 2,561 | 2,835 | 2,485 | 0.90× | Two-stage | Win |
| CitmCatalog (minified) | 2,020 | 1,812 | 1,172 | 1.11× | Fused | Win |
| CitmCatalog (prettified) | 4,051 | 4,077 | 2,988 | 0.99× | Tie | Win |
| Discord (minified) | 2,264 | 1,826 | 1,422 | 1.24× | Fused | Win |
| Discord (prettified) | 2,588 | 2,909 | 2,200 | 0.89× | Two-stage | Win |
| Google Maps Response (minified) | 1,697 | 1,567 | 1,121 | 1.08× | Fused | Win |
| Google Maps Response (prettified) | 3,619 | 3,874 | 2,712 | 0.93× | Two-stage | Win |
| Instruments (minified) | 3,071 | 2,178 | 1,627 | 1.41× | Fused | Win |
| Instruments (prettified) | 3,394 | 3,715 | 2,897 | 0.91× | Two-stage | Win |
| Marine IK Reverse (minified) | 805 | 700 | 164 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 3,178 | 3,055 | 801 | 1.04× | Fused | Win |
| Marine IK (minified) | 851 | 746 | 613 | 1.14× | Fused | Win |
| Marine IK (prettified) | 3,417 | 3,314 | 2,849 | 1.03× | Fused | Win |
| Mesh (minified) | 1,088 | 1,157 | 1,316 | 0.94× | Two-stage | **Loss** |
| Mesh (prettified) | 1,662 | 2,043 | 2,442 | 0.81× | Two-stage | **Loss** |
| Random (minified) | 1,632 | 1,311 | 1,078 | 1.25× | Fused | Win |
| Random (prettified) | 2,206 | 2,246 | 1,938 | 0.98× | Two-stage | Win |
| Twitter (minified) | 2,373 | 1,633 | 1,881 | 1.45× | Fused | Win |
| Twitter (prettified) | 2,167 | 2,162 | 2,667 | 1.00× | Tie | **Loss** |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | n/c | n/c | n/c | — | — | — |
| Double (POD) | 970 | 324 | 161 | 2.99× | Fused | Win |
| Int64 (POD) | 2,670 | 620 | 370 | 4.31× | Fused | Win |
| String (POD) | 1,723 | 721 | 1,009 | 2.39× | Fused | Win |
| Uint64 (POD) | 2,662 | 567 | 411 | 4.70× | Fused | Win |
| Canada (minified) | 714 | 659 | 495 | 1.08× | Fused | Win |
| Canada (prettified) | 1,688 | 1,819 | 1,397 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,939 | 1,292 | 810 | 1.50× | Fused | Win |
| CitmCatalog (prettified) | n/c | n/c | n/c | — | — | — |
| Discord (minified) | 1,655 | 1,069 | 922 | 1.55× | Fused | Win |
| Discord (prettified) | 1,878 | 1,740 | 1,406 | 1.08× | Fused | Win |
| Google Maps Response (minified) | 1,012 | 1,039 | 599 | 0.97× | Tie | Win |
| Google Maps Response (prettified) | 2,380 | 2,314 | 1,408 | 1.03× | Tie | Win |
| Instruments (minified) | 2,313 | 1,486 | 1,013 | 1.56× | Fused | Win |
| Instruments (prettified) | 2,836 | 2,417 | 1,756 | 1.17× | Fused | Win |
| Marine IK Reverse (minified) | n/c | n/c | n/c | — | — | — |
| Marine IK Reverse (prettified) | 2,216 | 2,183 | 517 | 1.01× | Tie | Win |
| Marine IK (minified) | 611 | 522 | 399 | 1.17× | Fused | Win |
| Marine IK (prettified) | n/c | n/c | n/c | — | — | — |
| Mesh (minified) | 886 | 721 | 837 | 1.23× | Fused | Win |
| Mesh (prettified) | 1,264 | 1,495 | 1,356 | 0.85× | Two-stage | Win |
| Random (minified) | 1,059 | 818 | 580 | 1.29× | Fused | Win |
| Random (prettified) | 1,526 | 1,404 | 1,059 | 1.09× | Fused | Win |
| Twitter (minified) | 1,848 | 1,349 | 1,297 | 1.37× | Fused | Win |
| Twitter (prettified) | 2,166 | 1,878 | 1,809 | 1.15× | Fused | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | n/c | n/c | n/c | — | — | — |
| Double (POD) | 828 | 337 | 175 | 2.46× | Fused | Win |
| Int64 (POD) | 2,354 | 636 | 447 | 3.70× | Fused | Win |
| String (POD) | 1,752 | 745 | 1,078 | 2.35× | Fused | Win |
| Uint64 (POD) | 2,410 | 625 | 423 | 3.86× | Fused | Win |
| Canada (minified) | n/c | n/c | n/c | — | — | — |
| Canada (prettified) | 2,039 | 1,943 | 1,323 | 1.05× | Fused | Win |
| CitmCatalog (minified) | 2,277 | 1,378 | 839 | 1.65× | Fused | Win |
| CitmCatalog (prettified) | 3,639 | 3,098 | 2,011 | 1.17× | Fused | Win |
| Discord (minified) | 2,258 | 1,542 | 1,041 | 1.46× | Fused | Win |
| Discord (prettified) | 2,463 | 2,137 | 1,657 | 1.15× | Fused | Win |
| Google Maps Response (minified) | 1,987 | 1,294 | 716 | 1.54× | Fused | Win |
| Google Maps Response (prettified) | 3,082 | 2,377 | 1,666 | 1.30× | Fused | Win |
| Instruments (minified) | 2,355 | 1,442 | 1,084 | 1.63× | Fused | Win |
| Instruments (prettified) | 3,171 | 2,576 | 1,942 | 1.23× | Fused | Win |
| Marine IK Reverse (minified) | 645 | 562 | 128 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 2,638 | 2,354 | 587 | 1.12× | Fused | Win |
| Marine IK (minified) | 766 | 605 | 385 | 1.27× | Fused | Win |
| Marine IK (prettified) | 2,877 | 2,304 | 1,818 | 1.25× | Fused | Win |
| Mesh (minified) | 1,102 | 873 | 782 | 1.26× | Fused | Win |
| Mesh (prettified) | 1,531 | 1,571 | 1,444 | 0.97× | Two-stage | Win |
| Random (minified) | 1,464 | 1,126 | 793 | 1.30× | Fused | Win |
| Random (prettified) | 2,178 | 1,936 | 1,385 | 1.13× | Fused | Win |
| Twitter (minified) | 2,362 | 1,673 | 1,502 | 1.41× | Fused | Win |
| Twitter (prettified) | 2,633 | 2,256 | 2,174 | 1.17× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: the compiler, and whether the document is indented. On the M1 the fused path wins nearly everything. On Linux it wins POD and minified input and loses most prettified input. Under MSVC it splits minified documents evenly with the two-stage path and loses every prettified one.

**POD-type tests: the fused path wins all 22, on every build.** It runs them 1.7× (String, Windows/MSVC) to 6.4× (Bool, Linux/Clang) faster than the two-stage path. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the compiler.

**Minified documents: the fused path's on Linux and the M1, split under MSVC.** Outside MSVC the fused path wins 36 of 38 converged minified documents, ties one (Google Maps on macOS/GCC) and loses one (Mesh on Linux/GCC, by 6%). Its margins run from 5% (Canada on Linux/GCC) to 65% (CitmCatalog on macOS/Clang), and Instruments and Twitter, the documents with the most keys to match, are 29% to 45% faster on both Linux compilers. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order. Under MSVC the ten minified documents divide evenly. The fused path wins Canada, Discord, Google Maps, Random and Twitter, by 2% to 11%; the two-stage path wins CitmCatalog, Instruments, Marine IK, Marine IK Reverse and Mesh, by 1% (Marine IK) to 14% (Mesh).

**Prettified documents on x86: the two-stage path wins 24 of 28.** It wins all nine under MSVC, by 2% (Twitter) to 39% (Mesh); all nine under Linux/Clang, by 6% (Marine IK Reverse) to 33% (Instruments); and six of ten under Linux/GCC, by 2% (Random) to 23% (Mesh). The fused path wins two, Marine IK Reverse (4%) and Marine IK (3%) on Linux/GCC, and CitmCatalog and Twitter on Linux/GCC are ties. The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That keeps it close, but it is not enough on x86. Stage 1 classifies every byte, whitespace included, in 64-byte vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis.

**Prettified documents on the M1: mostly the fused path's.** It wins 13 of 18, by 5% (Canada on macOS/Clang) to 30% (Google Maps on macOS/Clang), and Google Maps and Marine IK Reverse on macOS/GCC are ties. The two-stage path takes Mesh on both compilers (18% on GCC, 3% on Clang) and Canada on GCC (8%). We do not have an explanation for why NEON largely escapes the prettified penalty. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it.

**MSVC slows the fused path more than anything else it compiles.** All three libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. On minified Instruments the fused path reaches 24% of its Linux/Clang throughput, while the two-stage path reaches 34% and simdjson 49%; on minified CitmCatalog the figures are 35%, 41% and 47%, and on minified Mesh 34%, 47% and 36%. The two-stage path's stage 1 is branch-free intrinsic code whose shape is fixed by the source, while the fused path is deeply inlined, branch-heavy template code whose performance rests on the optimizer. We take that difference in exposure to the compiler to be why MSVC moves the balance so far toward the tape, but we have not isolated it.

**Against simdjson, Jsonifier wins 110 of 116 tests with whichever path is faster.** It ties one, prettified Mesh on Linux/Clang, and loses five. The fused path alone wins 105 and loses 11. Neither M1 build loses a test with either path. The five losses are Instruments in both forms on Windows/MSVC, and Mesh in both forms and prettified Twitter on Linux/GCC; Mesh accounts for two of them and for the one tie.

**Reverse key order: both paths crush simdjson.** Requesting every key in reverse order forces simdjson's On Demand API into the rescanning behavior described in §3. Under MSVC simdjson manages 35 MB/s on minified Marine IK Reverse against 375 MB/s for the two-stage path, 10.6× slower. On the other builds the faster Jsonifier path is 3.9× to 5.0× faster than simdjson on every converged version.

The routing implication is sharper than §2's rule of thumb. On the M1, the fused path is the right default for everything. On Linux it is right for POD and minified input, and prettified input should go to the two-stage path. Under MSVC it is right for POD-type data and about half of minified documents, and prettified input should go to the two-stage path. Routing each test to its faster path would raise the record against simdjson from 105 wins, 0 ties and 11 losses to 110 wins, 1 tie and 5 losses; all six tests that change hands are prettified documents, and five of them are on x86. The router does not yet take the compiler or the indentation into account.

## 3. The single-pass path: `json_iterator` over raw text

The fused path is implemented as `json_iterator<parseOpts, read_buffer_ptr, string_buffer_type>` — an iterator holding a raw `read_buffer_ptr` cursor plus depth counters, walking the document exactly once. Several design decisions distinguish it:

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

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSizeRoot` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `indentSize * currentDepth()`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (string unescaping, discussed in §6) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra with several architectural departures.

### 4.1 Step geometry as a per-compiler compile-time constant

simdjson classifies input per 64-byte block, and its generic indexer steps through the buffer 128 bytes per iteration — two 64-byte blocks, software-pipelined one block deep, a geometry it retains even on its widest kernel (the Icelake implementation invokes `index<128>`). Jsonifier generalizes the unit of work to a *step* of `simdBlocksPerStep` 64-byte blocks, with the constant chosen per ISA **and per compiler**:

```cpp
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
using jsonifier_simd_int_t			= __m256i;
	#if JSONIFIER_COMPILER_CLANG
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 4;
	#elif JSONIFIER_COMPILER_GCC
static constexpr uint64_t simdTapeStep	   = 1;
static constexpr uint64_t simdBlocksPerStep = 8;
	#else
static constexpr uint64_t simdTapeStep	   = 4;
static constexpr uint64_t simdBlocksPerStep = 8;
	#endif
#endif
```

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA — Clang preferring narrower steps with wider tape strides, GCC the reverse — reflects real differences in how each compiler schedules the unrolled block bodies, and is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations — a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(simd_array_t in_01, jsonifier_simd_int_t bsRegister, jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNextInString() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit — a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional — `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString`. Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4` — the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm — `followsNonquoteScalar` carries the cross-block bit — so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(simd_array_t in_01, jsonifier_simd_int_t opTable, jsonifier_simd_int_t spaceMask, jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace	  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
	const uint64_t op			  = simd::op_collector::impl(in_01, opTable, spaceMask);
	const uint64_t scalar		  = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t nonquoteScalar = scalar & ~simd::rope_detector<rope_block>::quotes;
	const uint64_t follows		  = simd::rope_detector<rope_block>::followsNonquoteScalar(nonquoteScalar);
	const uint64_t scalarStart	  = scalar & ~follows;
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

	template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& bits, structural_index_ptr tape) noexcept {
		tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
		bits									  = simd::tape_writer_op::advance(bits);
	}
};

template<uint64_t step> struct write_indices_stepped_functor {
	using size_type = uint64_t;
	template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& bits, structural_index_ptr tape, uint64_t cnt) noexcept {
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
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep` — the Clang-vs-GCC AVX2 split in §4.1 (stride 4 vs stride 1) exists precisely because the two constants interact.
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step — up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(array<uint64_t, simdBlocksPerStep> bitsArr, array<uint64_t, simdBlocksPerStep> cnts, structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains — each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser — it is a *specialization* of the same `json_iterator` interface over `structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the iterator concept; a `structural_context` trait switches the token-navigation primitives:

```cpp
JSONIFIER_INLINE bool skipValue() noexcept {
	if (iter >= endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>();
	}
	const char first = static_cast<char>(*currentPtr());
	if (first == '{' || first == '[') {
		uint64_t depth{};
		while (iter < endIter) {
			const char c = static_cast<char>(stringRootIter[*iter]);
			++iter;
			if (c == '{' || c == '[') {
				++depth;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					return true;
				}
			}
		}
		return reject<parse_statuses::unexpected_string_end>();
	}
	++iter;
	return true;
}
```

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per brace/bracket — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer iterator, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on minified documents outside MSVC and on most documents on the M1; slower on prettified x86 input and on half of minified documents under MSVC (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared iterator concept |
| Out-of-order keys | Forward scan + wrap-around rescan per lookup, per object — no memory across documents | One hash fallback, then learned per-slot order correction (thread-local, per stream) |
| Schema knowledge (known-type workloads) | Exists in caller's traversal code — invisible to the library | Declared once via reflection — consumed by the architecture |
| Step size | 128 bytes (two 64-byte blocks), pipelined one block deep | 256–512 bytes, per-(ISA × compiler) constant |
| Bit extraction | Stepped tzcnt bursts (global STEP macro, scalar tail past 24 bits); VBMI2 compress on Icelake | Same primitives, but full-range folded bursts with per-toolchain-swept stride |
| Drain scheduling | Previous block drained during current block's classification | Whole-step deferred batch drain, lane offsets precomputed from popcounts |
| Escape short-circuit | Yes (zero-backslash fast exit) | Yes (same) |
| In-string prefix-XOR | Computed every block | Skipped for quoteless blocks |
| UTF-8 validation | In-register, fused into stage 1, over all input bytes (ASCII fast path per block); verdict at EOF | In-register, fused into string parsing, over string bytes only, carried across the width cascade; verdict per string |
| Specialization axis | Runtime CPU dispatch | Compile-time everything (Cathedral Architecture) |

The last row is the root of every other difference. simdjson must ship one binary that runs well everywhere, so its kernels are shaped for runtime selection among a fixed set. Jsonifier's founding constraint — only the *data* is a runtime variable; the schema, the ISA, the compiler, the parse options are all known at build time — lets every routing decision in this paper (tape or no tape, whitespace lane or not, validation or not, 4 blocks or 8, burst of 1 or 4) be resolved before the first byte is read.

## 8. Conclusion

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head. Across 116 tests on five platforms the fused path wins 78, ties 5 and loses 33, and where it loses is a matter of platform more than of principle. It wins all 22 POD-type tests, by up to 6.4× (Bool on Linux/Clang), 36 of 38 minified documents outside MSVC, and 13 of 18 prettified documents on the M1. The two-stage path wins 24 of 28 prettified documents on x86, and under MSVC it also takes half of the minified documents. The tape, in other words, is not only an index for partial reads: on some compilers and some layouts it is also the faster way to read the whole book, and the router should learn which. Taking whichever path is faster, Jsonifier beats simdjson on 110 of those 116 tests and ties one. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*