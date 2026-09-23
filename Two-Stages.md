# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**
*Nihilai Collective Corp*
*July 2026, results updated September 2026 — Jsonifier*

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model — a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2) — has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization — so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying.

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

For full-document parsing into known types, this is pure overhead on most input. Jsonifier's measurements show the fused single-pass path outperforming its own two-stage path on POD-heavy and minified documents, often by a wide margin, which is why the library ships both and routes between them. §2.2 through §2.4 quantify that claim test by test, including the one input shape where it does not hold: heavily indented documents on x86.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.**

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 The forced two-stage comparison

Both comparisons in this section come from the same September 2026 sweep: Jsonifier c09c0d3 against simdjson 82d0b8e (On Demand), harness benchmarksuite 49d7727. The x86 targets ran on September 23 and the M1 targets on September 24. Both libraries parse fully into the target data structures and perform UTF-8 validation in both modes. The suite has 25 tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), nine corpus documents in minified and prettified form, and two "Marine IK Reverse" tests that request every key in the reverse of its document order. In every other test, both libraries receive keys in document order. Sampling is adaptive and ties are declared by Welch's t-test, as in the stage-1 companion paper; convergence requires RSE below 5% and epoch-over-epoch mean shift below 2.5% on x86, and 10% and 5% on the virtualized M1. Results that do not converge within 20 seconds are excluded, which is why some M1 rows have fewer than 25 tests.

The first mode forces Jsonifier's two-stage path via the harness's branch-switching configuration, so that both libraries execute the same stage-1-then-stage-2 architecture and the comparison isolates the stage implementations themselves rather than the routing decision. Against simdjson the forced two-stage record is:

| Platform / Compiler | Wins | Ties | Losses | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 0 | 0 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 23 | 2 | 0 | 25 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 16 | 3 | 6 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 19 | 2 | 3 | 24 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 15 | 2 | 5 | 22 of 25 |
| **Aggregate** | **98** | **9** | **14** | **121 of 125** |

### 2.2 The fused single-pass comparison

The second mode runs Jsonifier in its default routing — the fused single-pass path (the harness calls it "scalar structural iteration"), with no stage-1 tape — against the same simdjson On Demand configuration. This is the comparison that tests this paper's thesis directly:

| Platform / Compiler | Wins | Ties | Losses | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 24 | 1 | 0 | 25 of 25 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 21 | 2 | 2 | 25 of 25 |
| Linux / GCC 16.1 (i9-14900KF, AVX2) | 16 | 2 | 7 | 25 of 25 |
| macOS / GCC 16.2 (Apple M1, NEON) | 23 | 0 | 2 | 25 of 25 |
| macOS / Clang 23.1 (Apple M1, NEON) | 18 | 1 | 0 | 19 of 25 |
| **Aggregate** | **102** | **6** | **11** | **119 of 125** |

The two tallies are close in shape: 102 wins, 6 ties and 11 losses fused, against 98 wins, 9 ties and 14 losses forced two-stage. The tallies hide most of the story, however, because a win is a win whether it is by 1% or by 1,000%. The per-test deltas in §2.3 show where the fused path actually pulls away.

### 2.3 Per-test deltas, both modes

Each delta is Jsonifier's throughput over simdjson's, minus one, so a positive delta favors Jsonifier; ties are marked, and "n/c" means the test did not converge in that mode. The gap column is the fused delta minus the two-stage delta, in percentage points. A positive gap means the fused path beats simdjson by more than the two-stage path does.

One check makes the gap column meaningful. simdjson runs identically in both modes, so its throughput should not change between the two runs, and it does not: the median ratio of simdjson's throughput between the two runs is between 0.99 and 1.01 on every platform. The gap therefore measures what switching Jsonifier's path does, not drift in the competitor.

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused single-pass Δ | Forced two-stage Δ | Gap (fused − two-stage) |
|---|---|---|---|
| Double (POD) | +500.0% | +73.0% | +427.0 pp |
| Uint64 (POD) | +344.8% | +43.5% | +301.3 pp |
| Int64 (POD) | +433.9% | +39.2% | +394.7 pp |
| Bool (POD) | +1672.2% | +77.3% | +1594.9 pp |
| String (POD) | +121.2% | +39.1% | +82.2 pp |
| Canada (minified) | +56.4% | +61.4% | −5.0 pp |
| Canada (prettified) | +39.2% | +53.2% | −14.1 pp |
| CitmCatalog (minified) | +188.6% | +155.0% | +33.6 pp |
| CitmCatalog (prettified) | +67.3% | +140.1% | −72.8 pp |
| Discord (minified) | +102.8% | +78.4% | +24.5 pp |
| Discord (prettified) | +39.2% | +77.1% | −37.9 pp |
| Google Maps Response (minified) | +97.5% | +82.1% | +15.4 pp |
| Google Maps Response (prettified) | +55.2% | +70.0% | −14.8 pp |
| Instruments (minified) | +225.1% | +122.4% | +102.6 pp |
| Instruments (prettified) | +65.1% | +120.3% | −55.2 pp |
| Marine IK Reverse (minified) | +1581.9% | +1375.3% | +206.6 pp |
| Marine IK Reverse (prettified) | +1055.7% | +1210.9% | −155.2 pp |
| Marine IK (minified) | +82.5% | +71.6% | +10.9 pp |
| Marine IK (prettified) | +40.3% | +66.0% | −25.7 pp |
| Mesh (minified) | +48.8% | +29.7% | +19.1 pp |
| Mesh (prettified) | −4.9% (tie) | +22.1% | −27.0 pp |
| Random (minified) | +66.6% | +47.5% | +19.1 pp |
| Random (prettified) | +33.1% | +42.3% | −9.1 pp |
| Twitter (minified) | +84.8% | +56.3% | +28.6 pp |
| Twitter (prettified) | +38.1% | +56.1% | −18.0 pp |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused single-pass Δ | Forced two-stage Δ | Gap (fused − two-stage) |
|---|---|---|---|
| Double (POD) | +430.6% | +44.4% | +386.2 pp |
| Uint64 (POD) | +257.5% | +5.6% (tie) | +251.8 pp |
| Int64 (POD) | +295.9% | −1.8% (tie) | +297.7 pp |
| Bool (POD) | +1219.2% | +18.4% | +1200.8 pp |
| String (POD) | +193.2% | +7.2% | +186.1 pp |
| Canada (minified) | +36.4% | +20.8% | +15.6 pp |
| Canada (prettified) | +13.0% | +19.8% | −6.9 pp |
| CitmCatalog (minified) | +73.7% | +50.6% | +23.0 pp |
| CitmCatalog (prettified) | +36.9% | +45.3% | −8.4 pp |
| Discord (minified) | +38.1% | +21.1% | +17.0 pp |
| Discord (prettified) | +6.7% | +20.8% | −14.1 pp |
| Google Maps Response (minified) | +22.6% | +11.9% | +10.7 pp |
| Google Maps Response (prettified) | −2.7% (tie) | +13.2% | −15.9 pp |
| Instruments (minified) | +112.4% | +33.3% | +79.1 pp |
| Instruments (prettified) | +31.1% | +34.3% | −3.2 pp |
| Marine IK Reverse (minified) | +315.1% | +259.6% | +55.4 pp |
| Marine IK Reverse (prettified) | +243.3% | +253.9% | −10.7 pp |
| Marine IK (minified) | +47.7% | +24.7% | +23.0 pp |
| Marine IK (prettified) | +14.2% | +22.9% | −8.7 pp |
| Mesh (minified) | +20.7% | +4.9% | +15.9 pp |
| Mesh (prettified) | −9.1% | +2.9% | −11.9 pp |
| Random (minified) | +24.7% | +10.2% | +14.5 pp |
| Random (prettified) | +0.8% (tie) | +4.3% | −3.6 pp |
| Twitter (minified) | +26.2% | +8.6% | +17.6 pp |
| Twitter (prettified) | −5.2% | +6.3% | −11.6 pp |

**Linux / GCC 16.1 (i9-14900KF, AVX2)**

| Test | Fused single-pass Δ | Forced two-stage Δ | Gap (fused − two-stage) |
|---|---|---|---|
| Double (POD) | +408.6% | +56.6% | +352.0 pp |
| Uint64 (POD) | +252.2% | +11.8% | +240.4 pp |
| Int64 (POD) | +271.6% | +17.9% | +253.7 pp |
| Bool (POD) | +1309.2% | +47.0% | +1262.2 pp |
| String (POD) | +173.8% | −1.5% | +175.3 pp |
| Canada (minified) | +3.6% | +0.3% (tie) | +3.3 pp |
| Canada (prettified) | −8.5% | −1.0% (tie) | −7.5 pp |
| CitmCatalog (minified) | +34.4% | +10.9% | +23.5 pp |
| CitmCatalog (prettified) | −15.7% | +3.4% | −19.1 pp |
| Discord (minified) | +37.8% | +14.2% | +23.6 pp |
| Discord (prettified) | −1.6% (tie) | +12.6% | −14.2 pp |
| Google Maps Response (minified) | +16.1% | +6.7% | +9.4 pp |
| Google Maps Response (prettified) | −4.9% (tie) | +12.3% | −17.2 pp |
| Instruments (minified) | +104.1% | +29.4% | +74.8 pp |
| Instruments (prettified) | −4.3% | +18.8% | −23.0 pp |
| Marine IK Reverse (minified) | +372.5% | +285.2% | +87.3 pp |
| Marine IK Reverse (prettified) | +236.4% | +261.1% | −24.7 pp |
| Marine IK (minified) | +30.4% | +8.6% | +21.8 pp |
| Marine IK (prettified) | +2.8% | +4.7% | −1.9 pp |
| Mesh (minified) | −7.9% | −15.3% | +7.4 pp |
| Mesh (prettified) | −28.6% | −22.0% | −6.5 pp |
| Random (minified) | +8.7% | −5.7% | +14.4 pp |
| Random (prettified) | −14.4% | −10.5% | −3.9 pp |
| Twitter (minified) | +28.7% | +6.2% (tie) | +22.5 pp |
| Twitter (prettified) | −20.4% | −7.6% | −12.8 pp |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused single-pass Δ | Forced two-stage Δ | Gap (fused − two-stage) |
|---|---|---|---|
| Double (POD) | +570.1% | +67.8% | +502.4 pp |
| Uint64 (POD) | +319.7% | −1.4% (tie) | +321.1 pp |
| Int64 (POD) | +341.1% | +4.5% | +336.6 pp |
| Bool (POD) | +752.0% | −21.4% | +773.3 pp |
| String (POD) | +277.5% | +30.3% | +247.3 pp |
| Canada (minified) | +33.3% | +25.6% | +7.7 pp |
| Canada (prettified) | +14.9% | +8.9% | +6.0 pp |
| CitmCatalog (minified) | +84.8% | +32.9% | +51.9 pp |
| CitmCatalog (prettified) | +35.8% | +20.5% | +15.3 pp |
| Discord (minified) | +45.6% | +6.4% | +39.2 pp |
| Discord (prettified) | +13.2% | +11.1% | +2.2 pp |
| Google Maps Response (minified) | +53.1% | +28.0% | +25.1 pp |
| Google Maps Response (prettified) | +21.2% | +27.4% | −6.2 pp |
| Instruments (minified) | +96.4% | +31.1% | +65.3 pp |
| Instruments (prettified) | +37.6% | +32.4% | +5.2 pp |
| Marine IK Reverse (minified) | +401.2% | +309.7% | +91.5 pp |
| Marine IK Reverse (prettified) | +232.4% | n/c | — |
| Marine IK (minified) | +27.1% | +10.2% | +16.9 pp |
| Marine IK (prettified) | +16.8% | +8.6% | +8.3 pp |
| Mesh (minified) | −22.2% | −14.3% | −7.9 pp |
| Mesh (prettified) | −19.9% | −1.2% (tie) | −18.6 pp |
| Random (minified) | +18.4% | +4.7% | +13.7 pp |
| Random (prettified) | +8.7% | +3.5% | +5.2 pp |
| Twitter (minified) | +81.3% | +3.3% | +78.1 pp |
| Twitter (prettified) | +6.0% | −5.3% | +11.3 pp |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused single-pass Δ | Forced two-stage Δ | Gap (fused − two-stage) |
|---|---|---|---|
| Double (POD) | n/c | +40.7% | — |
| Uint64 (POD) | +279.8% | −18.4% | +298.2 pp |
| Int64 (POD) | n/c | +4.5% | — |
| Bool (POD) | +304.5% | −18.8% | +323.3 pp |
| String (POD) | +362.8% | +68.6% | +294.1 pp |
| Canada (minified) | +59.5% | +21.7% | +37.8 pp |
| Canada (prettified) | n/c | +16.7% | — |
| CitmCatalog (minified) | +117.4% | +30.9% | +86.5 pp |
| CitmCatalog (prettified) | +46.9% | n/c | — |
| Discord (minified) | +50.1% | +26.7% | +23.4 pp |
| Discord (prettified) | +16.4% | +3.9% | +12.5 pp |
| Google Maps Response (minified) | +31.4% | +0.4% (tie) | +31.0 pp |
| Google Maps Response (prettified) | +12.9% | +15.5% | −2.7 pp |
| Instruments (minified) | +106.7% | +23.1% | +83.6 pp |
| Instruments (prettified) | +33.4% | +19.0% | +14.4 pp |
| Marine IK Reverse (minified) | n/c | +280.6% | — |
| Marine IK Reverse (prettified) | +219.2% | +175.9% | +43.4 pp |
| Marine IK (minified) | +42.3% | +33.8% | +8.5 pp |
| Marine IK (prettified) | n/c | +13.2% | — |
| Mesh (minified) | +27.9% | n/c | — |
| Mesh (prettified) | +5.9% | −1.8% (tie) | +7.7 pp |
| Random (minified) | −0.6% (tie) | n/c | — |
| Random (prettified) | n/c | −10.5% | — |
| Twitter (minified) | +24.4% | −9.7% | +34.1 pp |
| Twitter (prettified) | +3.7% | −2.7% | +6.4 pp |

### 2.4 What the deltas show

Across the 115 tests that converged in both modes, the fused path's margin over simdjson is wider than the two-stage margin in 80 and narrower in 35. The split is not random; it follows the shape of the input.

**POD-type tests: the fused margin is several times wider, on every platform.** All 23 POD comparisons favor the fused path, and not narrowly. On Linux/Clang, Bool goes from +18.4% two-stage to +1219.2% fused; Double from +44.4% to +430.6%; Int64 from a −1.8% tie to +295.9%. On Windows/MSVC, Bool goes from +77.3% to +1672.2%. On the M1 the two-stage path actually loses some of these tests to simdjson (Bool on both compilers, Uint64 on Clang), and the fused path wins all of them by +278% to +752%. Jsonifier's own fused path runs these tests 1.5× to 11× faster than its two-stage path. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead.

**Minified corpus documents: wider on 29 of 30 x86 comparisons.** Across the three x86 targets, every minified document and the minified reverse-order test show a wider fused margin, except minified Canada under MSVC (−5.0 pp, both modes still winning). Typical gaps are +10 to +35 pp, and Instruments stands out at +75 to +103 pp. This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified corpus documents on x86: narrower on all 30 comparisons.** This is the result that cuts against the thesis, and it is consistent. On every x86 target, every prettified document shows a narrower fused margin, from −1.9 pp (Marine IK, GCC) to −72.8 pp (CitmCatalog, MSVC), and −155.2 pp on the prettified reverse-order test under MSVC. Jsonifier's own two-stage path is faster than its fused path on 28 of these 30, by up to 39% (prettified Instruments, MSVC). Eight of the nine fused-mode losses on x86 are prettified documents; the ninth is minified Mesh under GCC. The most likely explanation is the whitespace the fused path still skips one byte at a time. Stage 1 classifies every byte, whitespace included, in 64-byte vector blocks at a cost that does not depend on layout. The fused path vectorizes only the indentation after `{`, `[` and `,`, through the depth prediction of §3. The newline and indentation before every closing `}` and `]`, the space after every `:`, and any line whose indentation misses the prediction all go through the byte-at-a-time `skipWhitespaceScalar` loop. Heavily indented documents contain one such closing line per object and array, and one post-colon space per member. We have not isolated this, and state it as a hypothesis. If it holds, extending the prediction to closing brackets (whose indentation is one level shallower, and equally predictable) is the obvious fix.

**The M1: wider almost everywhere.** On macOS/GCC the fused margin is wider in 21 of 24 comparisons, and on macOS/Clang in 15 of 16, prettified documents included. The prettified pattern that dominates x86 largely disappears on NEON. We do not have an explanation for why, and have not isolated it.

**Reverse key order: both modes crush simdjson.** Requesting every key in reverse order forces simdjson's On Demand API into the rescanning behavior described in §3, and both of Jsonifier's paths win by +176% to +1582%. Under MSVC simdjson drops to 35 MB/s on minified Marine IK Reverse while Jsonifier holds 522–595 MB/s. The fused path is wider on minified input (every platform) and narrower on prettified input on x86, following the same whitespace pattern as the ordinary corpus.

The routing implication is sharper than §2's rule of thumb. The fused path is the right default for POD-heavy and minified input, which is most service-to-service traffic. For heavily indented input on x86, the measurements say Jsonifier's own stage-1 + stage-2 path is faster, and the router does not yet take that into account.

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

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSizeRoot` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `indentSize * currentDepth()`. It then verifies the whole predicted span in one call to `spanIsIndent`, which compares 32 bytes at a time on AVX2 and AVX-512 targets (16 bytes elsewhere) against the broadcast indent character. Any remainder is resolved through a size-class switch that uses overlapping loads, so no remainder length needs a byte loop. On our corpus of tests, `spanIsIndent` returns true on 100% of its roughly 799,000 calls, so the prediction never misses on that corpus and the scalar fallback is never taken for predicted indentation. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration. That same scalar loop also handles the whitespace the prediction does not cover: the newline and indentation before each closing `}` or `]`, and the space after each `:`.

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
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed (see §2.2) |
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

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2.3 shows what skipping the tape is worth, test by test. Across the 115 tests measured in both modes, the fused path beats simdjson by a wider margin than the two-stage path does in 80. On the POD-type tests the difference is an order of magnitude (+1219% against +18% for Bool on Linux/Clang), and minified documents on x86 widen in 29 of 30 comparisons. The same data shows the limit: on heavily indented documents on x86, Jsonifier's own two-stage path is the faster one, a routing refinement the measurements now call for. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*