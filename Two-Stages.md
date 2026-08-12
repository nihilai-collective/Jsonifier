# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**
*Chris M. (RealTimeChris)*
*July 2026 — Jsonifier*

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

For full-document parsing into known types, this is pure overhead. Jsonifier's measurements across its benchmark corpus consistently showed the fused single-pass path outperforming its own two-stage path on full-document workloads — which is precisely why the library ships both and routes between them. §2.2 quantifies that claim directly.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.**

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 The forced two-stage comparison

A note on benchmarking, to keep this paper consistent with its stage-1 companion: the head-to-head results reported here are measured with Jsonifier's two-stage path explicitly engaged via the harness's branch-switching configuration, so that both libraries execute the same stage-1-then-stage-2 architecture and the comparison isolates the stage implementations themselves rather than the routing decision. As of the July 2026 sweep, that record stands at **136 wins, 7 ties, 27 losses across 170 head-to-head tests against simdjson**, including a perfect 34-0-0 sweep on one of the five platform/compiler targets:

| Platform / Compiler | Wins | Ties | Losses |
|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 34 | 0 | 0 |
| Linux / Clang 23.0.0 (i9-14900KF, AVX2) | 30 | 2 | 2 |
| macOS / GCC 16.1.0 (Apple M1, NEON) | 27 | 2 | 5 |
| macOS / Clang 22.1.8 (Apple M1, NEON) | 24 | 2 | 8 |
| Linux / GCC 16.0.1 (i9-14900KF, AVX2) | 21 | 1 | 12 |
| **Aggregate** | **136** | **7** | **27** |

Linux/GCC is the visibly weaker platform in this table — nearly half the win count of Windows/MSVC, and the only row where losses (12) approach wins (21). We do not have a confirmed root cause for this specific gap; the per-compiler step-geometry divergence documented in §4.1 (GCC preferring `simdTapeStep = 1` against Clang's `simdTapeStep = 4` on identical AVX2 hardware) reflects a real, independently-verified difference in how the two compilers schedule the classification and drain loops, and is a plausible contributor, but we have not isolated it as the specific cause of this table's Linux/GCC margin and are not claiming that link as established.

The routing rule above describes the library's *default* behavior in production use; the benchmark deliberately overrides it for architectural comparability. Jsonifier's fused single-pass path is faster still on full-document workloads on most platforms — which is the point of the next section — and the harness's isolated-stage mode exists precisely to keep those two claims separately measurable. Per-platform tables and raw CSVs are maintained live at nihilai-collective.net; the figures above are a snapshot at time of writing.

### 2.2 Isolating the single-pass claim

§2.1's table answers "when both libraries run the same stage-1-then-stage-2 architecture, which implementation is faster?" It does not answer this paper's actual thesis — that Jsonifier's *default*, fused single-pass path outperforms two-stage parsing on full-document workloads. That comparison requires a separate harness: Jsonifier running in its default routing mode (the "scalar iteration" fused path, no stage-1 tape), against simdjson's On Demand API, on the same platform/compiler targets.

| Platform / Compiler | Wins | Ties | Losses |
|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 33 | 0 | 0 |
| Linux / Clang 24.0.0 (i9-14900KF, AVX2) | 30 | 2 | 1 |
| macOS / Clang 22.1.8 (Apple M1, NEON) | 28 | 0 | 4 |
| macOS / GCC 16.1.0 (Apple M1, NEON) | 23 | 3 | 6 |
| Linux / GCC 16.1.0 (i9-14900KF, AVX2) | 18 | 1 | 12 |
| **Aggregate** | **132** | **6** | **23** |

Windows/MSVC is again nearly a clean sweep at 33-0-0 — the strongest single result across either table. Linux/GCC is again the weakest platform, 18-1-12 — this is now the second independently-measured benchmark mode (production-routing aggregate and this forced-single-pass comparison both show it) in which Linux/GCC is Jsonifier's least favorable platform relative to simdjson. We flag the pattern without claiming a diagnosed cause for the same reason given in §2.1: it is consistent with the per-compiler step-geometry divergence in §4.1, but that link has not been isolated and confirmed here.

**Reading the two tables together.** Comparing win-to-loss ratios platform by platform against §2.1 gives a sharper picture than the aggregate totals alone:

| Platform / Compiler | §2.1 W/L ratio | §2.2 W/L ratio | Direction |
|---|---|---|---|
| Windows / MSVC | 34:0 (no losses) | 33:0 (no losses) | Unchanged — clean sweep both times |
| Linux / Clang | 30:2 → 15.0 | 30:1 → 30.0 | Stronger |
| macOS / Clang | 24:8 → 3.0 | 28:4 → 7.0 | Stronger |
| macOS / GCC | 27:5 → 5.4 | 23:6 → 3.8 | Weaker |
| Linux / GCC | 21:12 → 1.75 | 18:12 → 1.5 | Weaker |
| **Aggregate** | **136:27 → 5.0** | **132:23 → 5.7** | **Stronger** |

Three of the five platforms — Linux/Clang, macOS/Clang, and the aggregate — show a *stronger* relative standing against simdjson under the fused single-pass path than under the forced two-stage comparison in §2.1. The two GCC rows (Linux and macOS) move the other way, each showing a narrower margin under the fused path than under the forced two-stage comparison. Windows/MSVC is unchanged at a clean sweep in both modes.

We do not have a mechanism to offer for why the fused path widens Jsonifier's margin on three platforms while narrowing it on the two GCC targets specifically. The two tables are also not directly test-for-test comparable — they were run at different points against different upstream commits on both sides, and §2.1's harness explicitly forces the two-stage path while §2.2's does not — so this section reports the pattern as measured rather than assigning it a cause. What it does establish is narrower than "the fused path always wins more against simdjson": the fused path remains ahead of simdjson in aggregate on every platform measured, and the aggregate margin is wider under the fused path than under the forced two-stage comparison, but the GCC targets specifically buck that trend.

## 3. The single-pass path: `json_iterator` over raw text

The fused path is implemented as `json_iterator<parseOpts, string_view_ptr, string_buffer_type>` — an iterator holding a raw `const char*` cursor plus depth counters, walking the document exactly once. Several design decisions distinguish it:

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

**Whitespace memoization.** The non-minified specialization exploits the fact that pretty-printed JSON repeats its indentation pattern on nearly every line. `skipWhitespace` records the first whitespace run it sees (`wsStart`, `wsLength`) and thereafter skips subsequent runs by comparing 8 bytes at a time against the memoized pattern via `skipMatchingWs` — turning per-character whitespace scanning into a couple of `uint64_t` XORs per line.

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

Stage 2 is not a separate parser — it is a *specialization* of the same `json_iterator` interface over `structural_index_ptr` instead of `string_view_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the iterator concept; a `structural_context` trait switches the token-navigation primitives:

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

Jsonifier ships the same range-based algorithm simdjson uses (the `byte1High`/`byte1Low`/`byte2High` nibble-lookup classifier with the `carry`/`tooShort`/`tooLong`/`surrogate` error-bit algebra) in two deployments. The first, `utf8_checker_new::validateUtf8`, is the conventional standalone block validator, with the same ASCII fast path (`orAll` the block's registers, one test, skip the multibyte machinery). The second is the distributed one:

```cpp
} else if (hasByteLessThanValue<32>(simdValue)) [[unlikely]] {
	result = static_cast<basic_iterator02>(nullptr);
	return static_cast<basic_iterator01>(nullptr);
} else {
	validator.checkRegister(simdValue);
	string2 += bytesProcessed;
	string1Start += bytesProcessed;
}
```

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x41` (an innocuous ASCII byte) — so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator — the position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The stage-1 companion paper covers this mechanism in full [§6.1 there].

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes* — every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is enabled per-instantiation via `parse_options::validateUtf8` — one more compile-time routing decision, selecting between two `string_parser` specializations, so callers who trust their input pay literally nothing.

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

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2.2 shows that skipping the tape for full-document parsing keeps Jsonifier ahead of simdjson on every platform measured, and widens the aggregate margin over the forced two-stage comparison in §2.1 — though the two GCC targets specifically move the other way, a split-platform pattern §2.2 reports without a confirmed explanation. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*