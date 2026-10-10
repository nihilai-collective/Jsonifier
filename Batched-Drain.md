# Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1

**Nihilai Collective Corp — Technical Whitepaper**  
**Author: Nihilai Collective Corp**  
**July 2026 (AVX2 and NEON results October 9–10, 2026; AVX-512 results September 27 and October 4, 2026)**  

---

## Abstract

Structural indexing — the transformation of raw JSON text into a compact tape of structural character positions — has been the foundational pass of every high-performance JSON parser since simdjson established the SIMD two-stage paradigm in 2019. This paper documents the architecture of Jsonifier's stage-1 implementation and contrasts it point-by-point against simdjson's generic stage 1 (json_structural_indexer.h, master branch, retrieved July 2026). We identify six architectural divergences: (1) a batched bit-drain that decouples mask production from tape emission; (2) per-compiler step geometry ranging from 256 bytes to 512 bytes per iteration; (3) precomputed popcount accumulation replacing incremental tail advancement; (4) UTF-8 validation fused register-by-register into string unescaping itself, riding the same loads the unescape loop already performs, and carried *across SIMD width transitions* via a compact three-byte validation state, so the unescaping loop's full width cascade — widest register down to narrowest — validates continuously rather than restarting per width, where simdjson validates the entire buffer in a dedicated fixed-width stage-1 pass; (5) a compile-time minified specialization that deletes whitespace classification from the scan entirely; and (6) an AVX-512 compress-based index extraction path that operates as the batched architecture's structurally native drain, fed by scan-phase popcounts, rather than as a kernel-specific override retrofitted into a word-interleaved drain. We further argue that validation placement should follow access patterns: Jsonifier deliberately places unescaped-control-character validation in stage 2, where string bytes are already resident in registers, rather than in stage 1, and we present full conformance results across an 8-configuration test matrix as evidence that this placement loses no correctness. Benchmark results across eight platform/compiler combinations — five AVX2/NEON targets plus three AVX-512 targets that exercise the compress drain of (6) on real silicon — validate the design empirically, including perfect sweeps against simdjson on two of the eight, and a structural-tape comparison across eight builds, three of them AVX-512, confirms that Jsonifier's stage 1 emits exactly the same structural positions as simdjson's on every document of the corpus — 2,864,904 positions per build — with zero mismatches.

---

## 1. Introduction

In 2019, Langdale and Lemire demonstrated that JSON parsing need not be a byte-at-a-time state machine. simdjson's two-stage architecture — a SIMD-heavy first pass that locates every structural character and validates UTF-8, followed by a second pass that materializes values using the resulting index — established the design vocabulary that every subsequent high-performance parser has worked within. Jsonifier is no exception: its stage 1 is a direct descendant of that lineage. The odd-backslash escape-resolution arithmetic, the prefix-XOR in-string masking, the pseudo-structural character definition, and the lookup4 UTF-8 validation tables all trace their ancestry to simdjson, and Jsonifier's source acknowledges this debt explicitly.

This paper is not about the shared foundation. It is about the divergences — the places where Jsonifier's stage 1 departs from simdjson's design, why those departures were made, and what they are worth empirically. Some of the divergences are microarchitectural (how bit masks are drained to the tape), some are build-system-level (per-compiler tuning of loop geometry), and one is philosophical (where in the pipeline validation belongs). Together they produce a stage 1 that wins the large majority of head-to-head benchmark comparisons across eight platform/compiler combinations, emits a structural tape identical to simdjson's on every tested document, and passes the complete JSON conformance suite in all eight of Jsonifier's tested parse-option configurations.

The scope of this paper is stage 1 only: the pass that consumes raw bytes and produces the structural index tape. The overall fused parse architecture, on-demand key handling, out-of-order key resolution, and stage-2 value materialization are covered in the companion paper, "Two Stages, On Demand" [5]. Where a design decision in stage 1 only makes sense in light of a stage-2 property, we state the property and defer the details.

The remainder of this paper proceeds as follows. Section 2 briefly establishes the shared foundation to make the divergences legible. Sections 3 through 8 present the six divergences in order of impact: the batched drain (§3), the AVX-512 compress drain (§4), per-compiler step geometry (§5), UTF-8 validation fused into string unescaping and carried across width transitions (§6), the minified specialization (§7), and tape accounting (§8). Section 9 presents a case study in validation placement — the placement of control-character validation in stage 2 rather than stage 1 — with conformance results. Section 10 presents AVX2 and NEON benchmark results and structural-tape equivalence against simdjson. Section 11 does the same for AVX-512, where the compress drain of §4 runs. Section 12 states limitations honestly. Section 13 concludes.

---

## 2. Shared Foundation

Both parsers define stage 1's job identically: given a buffer of bytes, emit the position of every *pseudo-structural* character. A character is pseudo-structural if it is a structural operator (`{`, `}`, `[`, `]`, `:`, `,`), a quote delimiting a string, or the first character of a scalar token (a number, `true`, `false`, or `null`) — that is, a scalar character whose predecessor is whitespace, a structural character, or a closing quote. Everything inside strings is opaque to stage 1 except for the machinery needed to know *where the strings are*.

That machinery is the string-scanning core, and it is where the two implementations are most alike, because the underlying arithmetic is close to canonical. Backslash escape resolution uses the odd-bits trick: given a 64-bit mask of backslash positions, the expression

```cpp
JSONIFIER_INLINE uint64_t nextEscapeAndTerminalCodeImpl(const uint64_t potentialEscape) noexcept {
	static constexpr uint64_t oddBits{ 0xAAAAAAAAAAAAAAAAULL };
	const uint64_t maybeEscaped              = potentialEscape << 1;
	const uint64_t maybeEscapedAndOddBits    = maybeEscaped | oddBits;
	const uint64_t evenSeriesCodesAndOddBits = maybeEscapedAndOddBits - potentialEscape;
	return evenSeriesCodesAndOddBits ^ oddBits;
}
```

computes, in four scalar operations with a single carry-propagating subtraction, which characters are escaped by an odd-length run of preceding backslashes. A one-bit `nextIsEscaped` carry threads runs of backslashes across 64-bit word boundaries. Jsonifier's `rope_detector::nextEscapeAndTerminalCode` is structurally the same computation as simdjson's `json_string_scanner`, including the zero-backslash fast path that consumes only the carry.

In-string masking uses prefix XOR over the mask of unescaped quotes: every byte position between an opening and closing quote receives a 1 bit. On x86, Jsonifier computes this with a carry-less multiply against all-ones (`_mm_clmulepi64_si128`), the same PCLMULQDQ trick simdjson uses. On NEON and in the scalar fallback, a six-step Hillis-Steele XOR scan (`bitmask ^= bitmask << 1; ... << 32;`) produces the identical result. A sign-extension of the final bit (`static_cast<int64_t>(inString) >> 63`) carries the in-string state to the next word.

One structural refinement worth noting even in this "shared" section: Jsonifier's `rope_detector::next` branches on whether the word contains any quotes at all —

```cpp
return quotes ? finishNext() : finishNextNoInString();
```

— and the no-quote path skips the prefix-XOR entirely, simply propagating `prevInString`. For documents with long quote-free stretches (numeric arrays such as canada.json and mesh.json), this deletes the carry-less multiply from the majority of iterations. The branch is well-predicted precisely because such documents are homogeneous.

The UTF-8 validation tables are likewise shared ancestry: the three-shuffle lookup4 classification (byte1High, byte1Low, byte2High) with saturating-subtraction continuation checks is simdjson's algorithm. Jsonifier replicates the 16-entry tables to full register width (16, 32, or 64 bytes) at compile time via constexpr generator lambdas, so the same table constants serve SSE, AVX2, AVX-512, NEON, and SVE2 without per-ISA table definitions. Where the *placement* of validation differs, §6 covers it.

Having established what is shared, everything that follows is divergence.

---

## 3. Divergence 1: The Batched Drain

The single largest architectural difference between the two stage-1 implementations is *when tape emission happens relative to mask production*.

### 3.1 simdjson: interleaved word-granular drain

simdjson's inner loop processes 128 bytes per step as two 64-byte blocks. For each block, `json_structural_indexer::next` computes the structural mask, then immediately calls `bit_indexer::write` on the *previous* iteration's mask — a one-iteration software pipeline. `write` is the drain: it early-exits on a zero mask, popcounts, then extracts indices via a stepped unroll (`write_indexes_stepped` with STEP=4 up to a threshold of 24 set bits, falling back to a scalar loop beyond). Each extraction iteration is a serial dependency chain: trailing-zero count, store, clear-lowest-bit, repeat. Mask production and tape emission are interleaved at 64-bit-word granularity throughout the entire parse.

The consequence is that the drain's serial chain sits directly on the critical path between consecutive scan iterations. simdjson's in-source performance notes describe the mitigation: pipe two inputs at once so the scan of block N+1 can proceed while the drain of block N executes. This works, but it caps the overlap window at exactly one block, and it hard-codes that pipelining decision into the structure of the code.

### 3.2 Jsonifier: batch, then drain

Jsonifier separates the two phases completely. `processBlocks` runs the scan for `simdBlocksPerStep` consecutive 64-byte blocks — 4 or 8 blocks, i.e. 256 or 512 bytes, on every configured target (§5); the unrolled `if constexpr` chain supports at most 8 blocks per step — storing each block's structural mask and its popcount into two stack arrays:

```cpp
array<uint64_t, simdBlocksPerStep> bitsArr;
array<uint64_t, simdBlocksPerStep> cntsArr;
processBlocksImpl<0>(bitsArr, cntsArr, blockPtr, ...);
processBlocksImpl<1>(bitsArr, cntsArr, blockPtr, ...);
...
add_tape_values<default_backend, make_integer_sequence<simdBlocksPerStep>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);
```

Only after the entire step's masks exist does `add_tape_values` drain them all, with a fold expression walking the lanes and accumulating tape offsets from the precomputed counts. The generic (non-AVX-512) drain — the one every target in §10's sweep executes — is:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& __restrict bitsArr, const array<uint64_t, blocksPerStep>& __restrict cnts,
	write_structural_index_ptr __restrict tape, size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

(The AVX-512 `add_tape_values` specialization of §4 has an identical `impl`; it differs only in its per-lane extraction body, which adds a zero-mask early exit ahead of the compress sequence.)

The performance argument is about the out-of-order window. Each lane's drain is still a serial tzcnt/blsr chain internally, but *adjacent lanes' chains are independent of each other*: lane 3's extraction does not consume lane 2's extracted values, only its precomputed count for the offset — and all counts were computed during the scan phase. A modern out-of-order core can therefore overlap the drain chains of multiple lanes with each other, and overlap the entire drain phase with the loads and shuffles of the *next* step's scan, without the source code dictating a fixed pipeline depth. Where simdjson hand-schedules a two-block pipeline, Jsonifier hands the scheduler a wide field of independent work and lets the hardware find the overlap.

The batch structure also creates the precondition for the AVX-512 result in the next section: with masks and counts materialized in arrays, the drain becomes a pluggable unit that can be replaced wholesale per ISA, rather than a routine welded into the scan loop.

---

## 4. Divergence 2: The AVX-512 Compress Drain in a Batched Context

In May 2022, Lemire published "Fast bitset decoding using Intel AVX-512" and opened simdjson issue #1812, proposing that the AVX-512 kernel's index extraction be rebuilt around the compress instructions rather than scalar tzcnt loops. simdjson's AVX-512 kernel realizes this: its generic stage 1 keeps the scalar stepped drain as the default and exposes the `SIMDJSON_GENERIC_JSON_STRUCTURAL_INDEXER_CUSTOM_BIT_INDEXER` hook, through which the AVX-512 kernel substitutes a compress-based `bit_indexer::write`. Both libraries therefore use compress-instruction extraction on AVX-512; the technique itself is Lemire's, and this paper claims no priority on it.

The divergence is *where the technique sits in the surrounding architecture*, and it is a direct corollary of §3. In simdjson, the compress drain is a kernel-specific override slotted into a drain that remains interleaved with the scan at 64-bit-word granularity: `write` is still invoked per word from within `next`, still popcounts at drain time, and still advances the tail incrementally, with the compress instructions replacing only the innermost extraction loop of that structure. In Jsonifier, the batched-drain separation makes the compress path the structurally native drain: an entire step's masks and popcounts already exist in `bitsArr`/`cntsArr` when the drain begins, so the compress extraction runs over all lanes back-to-back, its per-lane tape offsets are fold-expression sums of counts computed during the scan phase, and none of its branches depend on drain-time work. The per-lane drain is:

```cpp
template<uint64_t index> JSONIFIER_INLINE static void drainLane(const array<uint64_t, blocksPerStep>& __restrict bitsArr,
	const array<uint64_t, blocksPerStep>& __restrict cnts, write_structural_index_ptr __restrict tape, size_type strIdx) noexcept {
	uint64_t bits = bitsArr[tag<index>{}];
	if (!bits) [[unlikely]] {
		return;
	}
	const uint64_t count = cnts[tag<index>{}];
	static constexpr size_type bitTotal{ index * 64ull };
	const int32_t base    = static_cast<int32_t>(bitTotal + strIdx);
	const __m512i indexes = _mm512_maskz_compress_epi8(bits,
		_mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, /* ... identity bytes 0..63 ... */ 0x03020100));
	const __m512i startIndexLocal = _mm512_set1_epi32(base);
	__m512i t0 = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes));
	_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
	if (count > 16) {
		const __m512i t1 = _mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(indexes, 1));
		_mm512_storeu_si512(tape + 16, _mm512_add_epi32(t1, startIndexLocal));
		if (count > 32) { /* quarters 2 and 3, identically gated */ }
	}
}
```

The mechanism: a 512-bit register holds the identity byte sequence 0..63. `_mm512_maskz_compress_epi8`, masked by the structural bitmask, compacts the byte indices of every set bit to the front of the register in one instruction. The compacted bytes are then widened to 32-bit indices sixteen at a time (`_mm512_cvtepu8_epi32`), offset by the block's base position, and stored directly to the tape. There is no tzcnt, no blsr, and no per-bit loop anywhere in the hot path — the entire 64-bit mask drains in a handful of vector instructions whose count depends only on the popcount quartile, not on the popcount itself.

The count-gating (`count > 16`, `> 32`, `> 48`) deserves a note: real-world JSON rarely exceeds 16 structural characters per 64 bytes. Densely structural content — minified arrays of small numbers — approaches one structural per 2 bytes only pathologically; typical documents (twitter.json, citm_catalog.json) sit well under the first gate. The branches are therefore heavily biased and cheap, and the common case executes exactly one compress, one widen, one add, one store per 64-byte block.

Stated precisely for verifiability against both codebases: the extraction technique is common to both libraries on AVX-512, and strikingly so — simdjson's icelake `bit_indexer::write` and Jsonifier's `drainLane` are nearly instruction-identical in the extraction body, down to the same 0..63 identity constant, the same compress → cvtepu8 → add → store sequence, and the same 16/32/48 count gating, as both descend from the same 2022 blog post. This convergence is not a weakness of the comparison but its controlled variable: with the instruction sequence held essentially constant on both sides, any measured difference in drain throughput is attributable entirely to the surrounding architecture — batched versus word-interleaved invocation, scan-phase counts versus drain-time `count_ones`, fold-expression lane offsets versus incremental `tail += count`. Jsonifier's contribution is that context: a drain phase already decoupled from the scan, already batched across the step, and already fed by scan-phase counts, into which compress extraction fits without a hook, an override macro, or any per-word re-entry into the scan loop. Adjacent lanes' compress/widen/store sequences are independent and free to overlap in the out-of-order window (the §3 argument, now applied to a fully vectorized drain), and the count-gating branches consume values whose computation completed an entire phase earlier. Where simdjson retrofits the instruction into its interleaved structure, the batched architecture is the structure the instruction wants.

---

## 5. Divergence 3: Per-Compiler Step Geometry

simdjson compiles one generic `step<128>` — two 64-byte blocks per iteration — for every compiler, on every ISA. The 128-byte figure was chosen, per the in-source performance notes, to soak available execution capacity on the microarchitectures of the time, and it is a reasonable universal default.

Jsonifier rejects the premise that a universal default exists. The step geometry — how many 64-byte blocks are scanned per batch, and (in the scalar-extraction fallback) how deep the stepped index writer unrolls — is selected per (ISA × compiler) pair at compile time:

| ISA | Clang | GCC | MSVC/other |
|---|---|---|---|
| AVX-512 | 8 blocks (512 B) | 4 blocks (256 B) | 4 blocks (256 B) |
| AVX2 | 4 blocks, tape step 4 | 8 blocks, tape step 4 | 8 blocks, tape step 4 |
| AVX/SSE | 4 blocks, tape step 2 | 8 blocks, tape step 2 | 4 blocks, tape step 1 |
| NEON | 4 blocks, tape step 8 | 4 blocks, tape step 8 | 4 blocks, tape step 1 |

(Values from the per-backend `backend_traits` specializations; `blocksPerStep` and `tapeStep` respectively, surfaced for the configured backend as `simdBlocksPerStep` and `simdTapeStep`. AVX-512 has no tape-step cell because its drain is the fully vectorized compress path of §4, which has no stepped scalar writer to tune.)

The rationale is empirical rather than theoretical: the fully-unrolled scan body of §3 places materially different register-allocation and scheduling demands on each compiler. GCC's scheduler profits from wider batches on x86, sustaining 8-block steps that keep its generated code dense; Clang's regalloc under the same 8-block unroll spills, and 4 blocks is its sweet spot; MSVC's behavior differs from both, particularly in how deeply the stepped tape writer should unroll before branch density outweighs extraction throughput. Each cell in the table is the winner of a measured sweep on the benchmark corpus, not a guess.

The contrast is sharpest precisely where the vectors are widest. simdjson's icelake kernel classifies each 64-byte block as a single 512-bit chunk and retains the universal `index<128>` step — on their widest ISA, exactly two vector registers of scan work separate consecutive drains. Jsonifier's AVX-512 steps batch 4 such blocks (256 B) between drain phases under GCC and MSVC and 8 (512 B) under Clang, a 2x to 4x wider scan window on the hardware with the most execution capacity to fill.

The engineering cost of this divergence is essentially zero — the geometry constants gate `if constexpr` chains that the compiler resolves at instantiation — but it encodes a position: at this performance tier, the compiler is part of the microarchitecture, and tuning that ignores it leaves measurable throughput on the table on at least one major toolchain. The cross-compiler variance in §10's results (compare the GCC and Clang columns on identical hardware) is the direct evidence.

### 5.1 Selecting the tier at run time

The table is resolved at compile time per tier, not per binary. On x86, one Jsonifier binary carries every instruction-set tier it was configured for and picks one at run time. The headers are compiled once per tier inside a target-attribute region (`backend_passes.hpp`: `#pragma GCC target` on GCC, `#pragma clang attribute` on Clang), which yields one complete `jsonifier_core_internal<backend>` per tier: AVX-512 (`avx512f`, `avx512bw`, `avx512vbmi2`), AVX2 and AVX. Each of those inherits its tier's parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call inside a tier is a direct, inlinable call into code built for that tier. On first use, `selectSupportedBackend` reads CPUID (leaves 0, 1 and 7, plus the extended leaves for `lzcnt`) and XCR0, so a tier counts only if both the CPU and the operating system support it, and caches the widest supported tier in a function-local `static`. Every public entry point then dispatches with a fold expression over the compiled tiers:

```cpp
static const jsonifier_backend backend{ selectedBackend() };
static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
```

That is a load of a cached enum and at most three compares, once per call. There is no function pointer and no vtable. simdjson's runtime dispatch, by contrast, goes through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its own On Demand documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target. In Jsonifier the same dispatch covers every API, including typed parsing, serialization and the generic On Demand-style iterator, and inside a tier nothing is virtual, so the per-tier code keeps all of its compile-time specialization.

The scope is x86. ARM builds compile one backend (NEON, or SVE2 when configured), as simdjson's ARM builds do. The set of x86 tiers compiled in is bounded by the configured tier (`JSONIFIER_CONFIGURED_AVX_TIER`), so a build configured for AVX2 carries AVX2 and AVX but not AVX-512. We have not separately measured the cost of the per-call check. Each tier's row of the table above is baked into its own instantiation, so runtime selection picks the AVX-512, AVX2 or AVX geometry with the kernel, and no step constant is ever read at run time.

---

## 6. Divergence 4: UTF-8 Validation Fused into String Unescaping

Both parsers descend from the same lookup4 validation algorithm (§2). The divergence here is not placement within stage 1 — it is *whether whole-buffer validation belongs in stage 1 at all*, and, more specifically than a first pass at this comparison suggested, *how tightly the validator is threaded into the one loop that is guaranteed to touch every string byte*.

simdjson validates the entire input buffer during stage 1: `json_structural_indexer::next` calls `checker.check_next_input(in)` on every 64-byte block (under the default-enabled SIMDJSON_UTF8VALIDATION guard), deliberately positioned as filler work for pipeline slack between the scan's serial segments, with `checker.check_eof()` finalizing the incomplete-sequence carry at the end of the document. Every byte of the input, string or not, passes through the UTF-8 checker, as a pass separate from string unescaping.

Jsonifier does not validate UTF-8 as a stage-1 pass at all, and does not run it as a separate pass over strings either. The check is compiled directly into the string-unescaping loop and cannot be disabled: `string_scanner`'s copy-and-unescape loop interleaves validator calls at every register boundary:

```cpp
utf8_register_validator<simd_type_wrapper<simd_type>> validator{ scanState.validationState };
/* ... broadcast '\\', '"', and 32 into simdValues00/01/02 ... */
while (string1Start < stringEndNew) {
	const auto registerStart		= string1Start;
	const simd_type_local simdValue = simd::gatherValuesU<simd_type_local>(string1Start);
	simd::storeU(simdValue, string2);
	const integer_type delimiters = static_cast<integer_type>(simd::opBitMask(
		simd::opOr(simd::opOr(simd::opCmpEqRaw(simdValue, simdValues00), simd::opCmpEqRaw(simdValue, simdValues01)), simd::opCmpLtRaw(simdValue, simdValues02))));
	if (delimiters == static_cast<integer_type>(0)) {
		validator.checkRegister(simdValue);
		string1Start += bytesProcessed;
		string2 += bytesProcessed;
		continue;
	}
	const uint64_t offset	= static_cast<uint64_t>(simd::countrZero(delimiters));
	const uint8_t foundChar = static_cast<uint8_t>(string1Start[offset]);
	if (foundChar < 32) [[unlikely]] { /* reject: unescaped control character */ }
	validator.checkPartial(registerStart, offset);
	string1Start += offset;
	string2 += offset;
	if (validator.errors()) [[unlikely]] { /* reject: invalid UTF-8 */ }
	if (foundChar == '"') { /* record raw and output lengths, done */ }
	/* ... bounds check, handleEscape ... */
	validator.reset();
}
validator.flush();
```

A single delimiter mask — backslash, quote, or any byte below 0x20 — is built from the register the loop has just loaded and stored to the output; `validator.checkRegister(simdValue)` consumes that same load, so a plain (delimiter-free) register of string content is validated with zero additional memory traffic — the bytes are already resident because unescaping needed them anyway, and validation piggybacks on the load rather than issuing its own. When a register is cut short by a quote or backslash mid-register, `validator.checkPartial` covers exactly the consumed prefix, and `validator.errors()` — which includes the pending incomplete-sequence carry — is checked before the escape is processed, so no byte of string content is skipped regardless of how densely escapes are distributed. Because an escape sequence is pure ASCII and the carry has just been checked, the validator is then reset before scanning resumes. The trailing sub-register remainder is handled by a scalar epilogue inside the narrowest-width step of the cascade (§6.1), which calls `checkPartial` over each run of raw bytes as it reaches a quote or backslash. Coverage is therefore total over the string, assembled from several call sites rather than one continuous scan — but every one of those call sites is already inside the unescaping loop, at a point where the relevant bytes are already in a register for an unrelated reason.

The validator instance itself, `utf8_register_validator<simd_type_wrapper<simd_type>>`, is deliberately register-granular rather than block-granular — its unit of work is `sizeof(simd_type)` bytes (16, 32, or 64 depending on target), matching the width the unescaping loop already operates at, not a fixed 64-byte block independent of ISA:

```cpp
JSONIFIER_INLINE void checkRegister(simd_type input) noexcept {
	if (simd::isAscii(input)) {
		error              = simd::opOr(error, incompleteRegister);
		prevInput          = input;
		incompleteRegister = simd_type{};
		return;
	}
	const simd_type sc = checkSpecialCases(input, simd::opPrev<15>(input, prevInput));
	error	  = simd::opOr(error, simd::opXor(simd::opAnd(mustBe23Continuation(simd::opPrev<14>(input, prevInput), simd::opPrev<13>(input, prevInput)), maskNibble01), sc));
	prevInput = input;
	incompleteRegister = simd::opSubs(input, incompleteMax);
}
```

(`maskNibble01` and `incompleteMax` are register constants built once per validator: a 0x80 broadcast and the width-appropriate tail of the `isIncompleteMax` table.) The ASCII fast path, the lookup4 special-case classification (`checkSpecialCases`), the 2-3-byte continuation check, and the incomplete-sequence carry via `isIncompleteMax` are all structurally the same lookup4 machinery as §2's shared ancestry — the divergence is exclusively in what unit of work the validator is built to consume and where it is invoked from, not in the underlying byte-classification algorithm. `checkPartial` handles sub-register remainders the same way the standalone block-level validator handles its tail: the partial content is memcpy'd into a scratch buffer padded with neutral ASCII (0x20 here; the standalone validator uses 0x41) and fed through the same `checkRegister` path, so no separate scalar validation code exists.

### 6.1 Validation Across Width Transitions

The description above holds for a single register width, but Jsonifier's string-unescaping loop is not single-width: it is a compile-time cascade over the target's full SIMD width list (`make_ascending_range<start_index, list_size>`), draining a string through progressively narrower registers — 64-byte steps while the remaining length supports them, then 32, then 16 — before the scalar epilogue. (The cascade is 64→32→16 on AVX-512 and 32→16 on AVX2; SSE, NEON, and SVE2 have a single 16-byte width.) A validator whose carry state (`prevInput`, `incompleteRegister`) is a register of one fixed width cannot survive this cascade: naively resetting it at each width transition would sever the incomplete-sequence carry exactly where a multi-byte UTF-8 sequence straddles the boundary between a wide-register chunk and a narrow-register chunk.

The resolution follows from an observation about the lookup4 algorithm itself: the continuation checks consume at most the trailing three bytes of the previous register (`opPrev<15/14/13>` — alignr operations pulling 1, 2, and 3 tail bytes respectively), and the incomplete-sequence carry is fully determined by whether those same trailing bytes begin an unterminated sequence (≥ 0xC0 in the last position, ≥ 0xE0 in the second-to-last, ≥ 0xF0 in the third-to-last). The width-agnostic validation state is therefore three bytes, one incomplete flag, and one sticky error bit:

```cpp
struct utf8_validation_state {
	uint8_t prevBytes[3];
	bool prevIncomplete;
	bool error;
};
```

Each width's loop constructs a local, register-resident `utf8_register_validator` seeded from this state — the three carried bytes placed in the tail positions of a zeroed register are semantically indistinguishable from carrying the previous full register, regardless of what width that register was — runs its hot loop with the validator entirely in registers exactly as described above, and flushes back to the state (one store, three scalar compares, one reduction) only when the loop exits. The flush executes at most once per width traversed; the per-register cost within each width is unchanged from the single-width design. The flush's incomplete-flag computation applies the position-dependent thresholds above — the last carried byte against 0xC0, the second-to-last against 0xE0, the third-to-last against 0xF0 — so the carry re-seeded into the next width is exactly the carry the register-resident `incompleteRegister` would have held. Copy construction and assignment of the register validator are explicitly deleted, converting any accidental duplication of in-flight carry state into a compile error rather than a silently reseeded validator with stale carry bytes.

The consequence is that validation coverage is continuous across the entire cascade: a 4-byte UTF-8 sequence whose lead byte lands in the final wide register of a string and whose continuations land in the subsequent narrower register is validated correctly, as is a sequence truncated by the true end of the string — caught by the padded `checkPartial` in whichever width sees the truncation, including the scalar epilogue, which reuses the narrowest width's validator and therefore the same carried state. The width-transition test family exercises both directions of this edge: multi-byte sequences deliberately straddling a transition (which must pass), and dangling lead bytes in each of the three carry positions at a transition followed by pure-ASCII content (which must fail — the ASCII fast path's only cross-boundary guard is the seeded incomplete carry, making the flush thresholds load-bearing). simdjson's fixed 64-byte-block checker has no analogous problem to solve — its stage-1 pass is width-homogeneous by construction — which is precisely the point: fusing validation into a multi-width unescaping loop requires the carry state to be width-portable, and three bytes suffice.

The performance consequence of the cascade is that mid-length strings — the dominant string population in service payloads such as twitter.json and discord.json — drain through progressively narrower validated widths instead of falling from the widest register directly to the scalar epilogue, keeping vector-granular validation in play for string tails that a single-width design would validate byte-at-a-time. §10's results isolate this effect.

### 6.2 The Accounting

The accounting of the divergence is stark, and it is worth being precise about exactly what is and is not free. Both parsers ride register loads that were already happening for another reason — that much is common ground, and simdjson's own performance notes say as much, characterizing its stage-1 UTF-8 pass as filler work for pipeline slack rather than a load issued for validation's sake. The divergence is not whether the load is free; it is what the load was free for. simdjson's stage-1 loop loads every 64-byte block of the entire buffer in order to build the structural index — the validator rides that traversal, but the traversal itself covers punctuation, whitespace, and numeric content that has no UTF-8 content to speak of, because the structural scan needs those bytes regardless of what they contain. Jsonifier's stage-2 unescaping loop, by contrast, only ever loads a register because it is about to copy or unescape string content — a load scoped to exactly the bytes validation cares about, with nothing structural or numeric riding along. The two parsers both validate on borrowed loads; only one of them borrows loads that were headed toward the relevant bytes in the first place. For documents that are structurally dense and string-light (canada.json, mesh.json — coordinate arrays), that scoping difference means the validated fraction of the buffer approaches zero for Jsonifier while remaining total for simdjson; even for string-heavy documents (twitter.json, discord.json) the validation cost is not a separate pass but a handful of additional vector instructions per register already in flight for string-handling reasons. simdjson's placement is, again, forced by its access pattern rather than by any inattention to cost: an on-demand consumer may never read a string's bytes at all, so stage 1 is the only pass guaranteed to see them, and validating the whole buffer in a dedicated pass — accepting that most of what it loads is not string content — is the price of that guarantee.

The UTF-8 unit suite has two halves. 147 cases exercise `validateUtf8`, the standalone whole-buffer API for arbitrary byte buffers, which is built on the block-granular `utf8_checker` rather than the register-granular string-scanner validator. They cover dangling leads at chunk, block, and step boundaries; surrogates; overlongs in all widths; and continuation-position errors crossing chunk seams. 51 more cases drive the string-fused path through a full parse, including multi-byte sequences deliberately straddling width-transition boundaries within the cascade and dangling leads in each carry position at a transition (per §6.1). Width-transition, unaligned-pointer, and page-boundary sweeps round it out. The conformance corpus separately confirms the string-fused path end-to-end: fail34.json (a raw 0x80 byte inside a string) is rejected with `invalid_string_characters` by `checkRegister`/`checkPartial` during unescaping. Both the standalone validator and the fused string-scanner path are exercised under the project's AddressSanitizer and UndefinedBehaviorSanitizer builds across all five platform/compiler targets, and pass cleanly — meaningful here specifically because register-granular validators carrying cross-call and now cross-width state (`utf8_validation_state`, the seeded `prevInput`/`incompleteRegister` members) are exactly the shape of code where an off-by-one in the padding, seeding, or carry logic would manifest as a sanitizer-visible read past a buffer's end rather than merely a wrong answer.

---

## 7. Divergence 5: The Minified Specialization

Stage 1's whitespace classification exists to separate scalars from padding. But if the caller knows the input is minified — machine-generated JSON with no interstitial whitespace, which describes virtually all service-to-service traffic — the classification is pure waste: the only whitespace-adjacent character that matters is the space character's role in the op-table trick, and the general whitespace shuffle table contributes nothing.

Jsonifier makes this a compile-time template parameter. `reset<minified>` instantiates two distinct scan loops. The prettified path computes, per block:

```cpp
const uint64_t whitespace = simd::ws_collector::impl(in_01, whitespaceTableLocal);
const uint64_t op         = simd::op_collector::impl(in_01, opTable, spaceMask);
const uint64_t scalar     = ~(op | whitespace | quotes);
```

The minified path deletes the first line and its inputs entirely — the whitespace table register is never even loaded — and the scalar definition tightens to `~(op | quotes)`:

```cpp
const uint64_t op     = simd::op_collector::impl(in_01, opTable, spaceMask);
const uint64_t scalar = ~(op | quotes);
```

Per 64-byte block, this removes one shuffle and one full-width compare per register — on a 128-bit target, four shuffles and four compares per block; over an 8-block GCC step, sixty-four eliminated vector operations per 512-byte step — plus the register pressure of holding the whitespace table live through the loop. The remainder trim (§8) guarantees correctness at the padded tail in both variants.

simdjson has no equivalent. Its json_minifier is an output transformation (produce minified text), not a parse-time fast path; its structural indexer always classifies whitespace. The distinction reflects the two libraries' relationship to compile-time knowledge: simdjson selects among precompiled kernels at runtime by CPU features, while Jsonifier's interface propagates caller knowledge about the *input's shape* into the instantiation itself. This is the stage-1 expression of the compile-time-knowledge thesis developed at length in the companion paper.

Every corpus test in §9's matrix runs in both Minified and Prettified variants; both paths pass identically.

---

## 8. Divergence 6: Tape Accounting and Remainder Handling

Three smaller mechanical divergences complete the inventory.

**Count computation.** simdjson recomputes the popcount inside `write` at drain time and advances the tail pointer incrementally per word. Jsonifier computes every popcount once, during the scan phase (`correctedPopcount` in `processBlocksImpl`), stores it in `cntsArr`, and both the drain's lane offsets and the final `tapeCount` advancement are pure additions over precomputed values. The popcounts thus execute in the scan's instruction mix, where port pressure differs from the drain's, and the drain's offset arithmetic has no dependence on extraction results.

**NEON index extraction.** On NEON, the 64-bit structural mask is synthesized from four 16-byte comparison results via the paired-add (`vpaddq_u8`) bitmask reduction, yielding one bit per byte, so the stepped writer extracts indices with a plain trailing-zero count (`countrZero`) and `blsr`, exactly as on x86. (The nibble-granularity `vshrn_n_u16` masks and the matching `postCmpTzcnt` — a trailing-zero count right-shifted by 2 — are used by the string-scanning and comparison code, not by stage 1.) Unlike simdjson's SIMDJSON_PREFER_REVERSE_BITS option, Jsonifier has no bit-reversed extraction path.

**Convergent stepped writers.** In the non-AVX-512 fallback, Jsonifier's `write_indices_stepped_functor` — extract a fixed step of indices unconditionally, branch only on whether the count demands another step — is structurally identical to simdjson's `write_indexes_stepped`. We note this as convergent design rather than divergence, with one delta: simdjson's step is a single compile-time constant (STEP=4, overridable by macro), while Jsonifier's `simdTapeStep` is a cell in the per-compiler table of §5, ranging from 1 to 8.

**Remainder handling.** The two libraries pad opposite problems. simdjson pads *forward*: after the final block, it writes sentinel indices (the document length, repeated) beyond the last real structural, so that on-demand parsing which runs past the end lands on positions that provably cannot extend a valid document — the "repeated `[`" argument documented in their finish routine. Jsonifier pads *backward*: the tail is copied into a 0x20-filled (space) step-sized buffer, scanned through the identical vector path, and then over-emitted indices are trimmed:

```cpp
const uint64_t excess = stepBytes - remaining;
while (excess > 0 && tapeCount > 0 && tape[tapeCount - 1] >= string_block_reader::length) {
	--tapeCount;
}
```

Space padding generates no structural bits by construction, so the trim loop's work is bounded by pathological cases (a document ending mid-token); `begin()` then writes a single terminating index equal to the document length. The bounds-truncation test family in §9 — every corpus document truncated at every step-boundary-adjacent length, across all eight option configurations — exists specifically to police this edge, and passes in full.

---

## 9. Case Study: Validation Placement Follows Access Patterns

This section examines a single validation rule — the rejection of unescaped control characters inside strings — as a case study in where validation belongs, because the two libraries place the identical check in different stages, and each placement is correct for its own architecture.

### 9.1 The rule and simdjson's placement

simdjson's stage 1 does one piece of *content* validation beyond UTF-8: it accumulates a mask of unescaped control characters (bytes below 0x20) and ANDs it against the in-string mask, because a raw control character inside a string is invalid JSON (RFC 8259 §7). This check lives in stage 1 for a structural reason: simdjson's on-demand stage 2 may *never touch a string's bytes again* — a document consumer that skips a field skips its bytes — so if stage 1 does not catch the raw tab, nothing will.

### 9.2 Jsonifier's placement

Jsonifier places the same validation in stage 2, during string materialization, byte-adjacent to the unescaping work it must do anyway — visible in the §6 listing as the below-0x20 comparison folded into the delimiter mask of every register the unescaping loop processes. The conformance suite confirms the placement end-to-end: the canonical failure cases for this exact rule — fail25.json (raw tab characters inside a string) and fail27.json (a raw line break inside a string) — are rejected with `invalid_string_characters`, with the rejection originating in the stage-2 json_iterator, not in any stage-1 mask.

This is the architecturally consistent placement *for this library*, and the same distinction §6.2 draws for UTF-8 applies here without modification. Jsonifier's stage 2, unlike simdjson's on-demand consumer, walks every string it materializes as a matter of course — the load that exposes each byte to the control-character check is a load that unescaping was always going to issue, not one recruited from an unrelated whole-buffer pass. Stage-1 accumulation would be redundant work performed on every block of every document — including the non-string bytes simdjson's equivalent check necessarily passes over on its way to the string bytes it actually cares about — to catch an error that stage 2 catches for free on the path it already executes. The same validation, placed where each architecture's access pattern makes it cheapest: simdjson validates in stage 1 because its stage 2 might not look, and accepts a whole-buffer pass as the cost of that guarantee; Jsonifier validates in stage 2 because its stage 2 always looks, and the check rides a load already scoped to string content rather than one scoped to the entire document.

The placement is ratified by the full conformance matrix: 77 fail-case documents (correctly rejected), 27 pass-case documents (correctly accepted), 27 round-trip documents, the 35-case string-pass and 26-case string-fail suites, the float/uint/int numeric suites, the UTF-8 boundary suite (§6.2), the full corpus parse tests in minified and prettified variants, and the bounds-truncation family — each executed under all eight parse-option configurations (Partial-Reading × Known-Order × Null-Terminated, all toggles). Every test passes.

### 9.3 The principle

We generalize the comparison into the design rule this section is named for: **validation belongs where the validated bytes are already resident.** A parser whose second stage is guaranteed to walk string content should validate string content there; a parser whose second stage may skip content must validate it in the pass that is guaranteed to see it. Neither placement is universally correct — the access pattern decides. Copying a competitor's validation placement without copying their access pattern imports their costs without their necessity.

The principle is not foreign to simdjson itself. Their AVX-512 classifier's `| 0x20` op-table trick knowingly misclassifies the control characters 0x0C and 0x1A as operators, and the in-source comment notes that "this gets caught in stage 2, which checks the actual character" — a deliberate acceptance of stage-1 imprecision on exactly the grounds argued here: stage 2 will look at those bytes anyway, so stage 1 need not be exact about them. The two libraries differ only in how far they extend the same logic.

---

## 10. Empirical Results: AVX2 and NEON

### 10.1 Methodology

Jsonifier's benchmark harness runs at nihilai-collective.net with live CSV result fetching from the repository's CI, branch-switching for isolated stage comparisons, and per-test win/tie/loss adjudication against simdjson (on-demand). The sweep ran on October 9 (x86 targets) and October 10, 2026 (NEON targets), with the harness at benchmarksuite 6196208 on every target. Jsonifier and simdjson are pinned per target: Jsonifier 35b277d with simdjson 9a0e3b2 on Linux/Clang and Linux/GCC, Jsonifier 35b277d with simdjson 7f6f8dc on macOS/GCC, and Jsonifier e2f2df8 with simdjson 7f6f8dc on Windows/MSVC and macOS/Clang. The two Jsonifier commits differ in `include/` only in the runtime backend-selection and generic-parser headers; the stage-1 kernels, the drain and the step tuning are identical. Every target runs the §5 step geometry exactly as printed. The sweep covers 23 head-to-head tests per platform/compiler combination: five POD-type tests (Bool, Double, Int64, String, Uint64), each running stage 1 separately on 200 single-value inputs of at most 64 bytes, plus nine corpus documents (Canada, CitmCatalog, Discord, Google Maps Response, Instruments, Marine IK, Mesh, Random, Twitter), each in minified and prettified form. Both libraries execute stage 1 only — structural indexing into a tape — and **neither performs UTF-8 validation** in these runs, so the comparison isolates the scan and drain machinery of §§3–5, 7, and 8; the fused validation of §6 is not exercised on either side.

Sampling is adaptive: iterations begin at 100 and double each epoch up to a 100,000-iteration cap, with caches cleared before every iteration. Each epoch evaluates a trailing window of max(iterations/10, 30) samples. Sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap is reached, every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is retained. A result counts as converged only if that epoch has RSE below 5% *and* mean shift below 2.5% (10% and 10% on the virtualized Apple Silicon targets, whose timing noise floor is higher); non-converged results are excluded from all tallies. Variance is Bessel-corrected, and ties are declared by Welch's t-test rather than by a fixed throughput band.

The hardware is an Intel i9-14900KF (Raptor Lake, AVX-512 fused off) under Windows/MSVC 19.44, Linux/Clang 24.0, and Linux/GCC 16.2, all under Linux 7.0.0 or Windows on the same machine, plus virtualized Apple M1 hosts under macOS/GCC 16.2 and macOS/Clang 23.1. On x86 all three builds share one host; the two NEON builds share a host type, but because they are virtual machines, a GCC-versus-Clang comparison on NEON may still mix compiler and host-noise effects. AVX-512 hardware is covered separately in §11.

Throughout this section, results are reported per test as a percentage delta: Jsonifier's throughput over simdjson's, minus one. A positive delta favors Jsonifier. With 23 tests per target there is no need to collapse them into a single mean; every number is shown.

### 10.2 Headline results (AVX2 and NEON)

On this sweep, Jsonifier's record against simdjson (on-demand) across the five AVX2/NEON platform/compiler combinations is **89 wins, 3 ties, 20 losses across 112 head-to-head tests**, including a perfect 23-0-0 sweep on Windows/MSVC. Three tests did not converge for both libraries, all on macOS/Clang (the Int64 POD-type test, prettified Marine IK and minified Random):

| Platform / Compiler | Wins | Ties | Losses |
|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 23 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 18 | 0 | 5 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 19 | 0 | 4 |
| macOS / GCC 16.2 (Apple M1, NEON) | 18 | 0 | 5 |
| macOS / Clang 23.1 (Apple M1, NEON) | 11 | 3 | 6 |
| **Aggregate** | **89** | **3** | **20** |

Per-test throughput deltas (Jsonifier over simdjson, minus one; a negative delta is a loss unless marked as a tie; "n/c" means the test did not converge):

| Test | Windows / MSVC | Linux / Clang | Linux / GCC | macOS / GCC | macOS / Clang |
|---|---|---|---|---|---|
| Bool (POD) | +74.2% | −20.3% | +41.2% | +6.2% | −47.4% |
| Double (POD) | +64.8% | −21.0% | +45.3% | +20.9% | −54.5% |
| Int64 (POD) | +81.9% | −21.3% | +40.0% | +11.8% | n/c |
| String (POD) | +55.7% | −21.4% | +36.5% | −16.4% | −50.0% |
| Uint64 (POD) | +91.7% | −20.7% | +44.7% | +6.0% | −44.4% |
| Canada (minified) | +30.7% | +28.9% | −5.3% | +13.8% | +75.4% |
| Canada (prettified) | +33.1% | +17.6% | −1.0% | −4.7% | +50.7% |
| CitmCatalog (minified) | +18.4% | +13.4% | +6.4% | −12.7% | −7.2% (tie) |
| CitmCatalog (prettified) | +8.7% | +9.8% | +16.4% | +10.2% | +26.9% |
| Discord (minified) | +15.1% | +11.5% | +7.6% | +4.0% | +16.5% |
| Discord (prettified) | +8.7% | +5.6% | +3.2% | +28.6% | −26.0% |
| Google Maps Response (minified) | +13.7% | +9.7% | +7.7% | +3.9% | +14.6% |
| Google Maps Response (prettified) | +11.8% | +9.1% | +9.4% | +11.8% | +0.2% (tie) |
| Instruments (minified) | +11.7% | +12.4% | +6.8% | +7.8% | +9.8% |
| Instruments (prettified) | +7.6% | +9.5% | +5.8% | +10.1% | +25.1% |
| Marine IK (minified) | +36.8% | +28.1% | −7.7% | −13.8% | +22.9% |
| Marine IK (prettified) | +30.8% | +14.8% | −10.4% | +1.5% | n/c |
| Mesh (minified) | +21.3% | +26.7% | +29.6% | +18.8% | +21.4% |
| Mesh (prettified) | +11.6% | +16.1% | +11.1% | −2.6% | −15.7% |
| Random (minified) | +13.6% | +12.6% | +8.2% | +20.5% | n/c |
| Random (prettified) | +9.0% | +9.4% | +9.3% | +13.5% | +13.8% |
| Twitter (minified) | +17.5% | +14.6% | +15.2% | +16.4% | +4.3% |
| Twitter (prettified) | +7.6% | +6.2% | +9.5% | +5.4% | −0.1% (tie) |

On the i9-14900KF, the three x86 builds record 60 wins, no ties and 9 losses across 69 tests. MSVC wins all 23, by +7.6% (prettified Instruments and prettified Twitter) up to +91.7% (Uint64 array); on the corpus documents alone its range is +7.6% to +36.8% (minified Marine IK). Clang wins all 18 corpus documents, by +5.6% (prettified Discord) to +28.9% (minified Canada), and loses all five POD-type tests by a nearly uniform −20.3% to −21.4%. MSVC and GCC win the same five tests by +36.5% to +91.7%. Each POD-type test runs stage 1 on 200 separate inputs of at most 64 bytes, so fixed per-call cost dominates in both libraries; these tests measure small-input dispatch and setup overhead more than the steady-state scan loop, and the uniformity of the Clang deficit across five different value types points at that per-call path rather than at classification or drain. We have not profiled it.

The losses localize to specific toolchains rather than spreading evenly. Linux/GCC's four losses are the two float-dense documents in both forms: Canada (−5.3% minified, −1.0% prettified) and Marine IK (−7.7% minified, −10.4% prettified). All four are wins for both MSVC and Clang on the same hardware, and every Linux/Clang loss is a win for both MSVC and GCC, so every x86 loss is a win for a sibling compiler's build on the same host. On NEON, macOS/GCC's five losses are the String POD-type test (−16.4%), minified CitmCatalog (−12.7%), minified Marine IK (−13.8%), prettified Canada (−4.7%) and prettified Mesh (−2.6%). macOS/Clang loses four POD-type tests by −44.4% to −54.5%, plus prettified Discord (−26.0%) and prettified Mesh (−15.7%), and ties minified CitmCatalog, prettified Google Maps Response and prettified Twitter. It also posts the largest corpus margins in the sweep on float-dense content: +75.4% on minified Canada and +50.7% on prettified Canada. The String POD-type test and prettified Mesh lose on both NEON builds; every other NEON loss is a win or a tie for the other compiler's build. On x86, where the hardware is shared, the same source code has different weak spots under GCC than under Clang, and neither under MSVC. This is the compiler-as-microarchitecture point of §5.

Per-platform tables, per-document plots, and the raw CSVs are served at nihilai-collective.net directly from the repository's committed CSV artifacts — the rendered figures are client-side fetches of the same files a reproducing reader would generate — and should be consulted for current numbers; the figures above are a snapshot at time of writing. The site additionally maintains live aggregate tallies for the broader cross-library campaign (void-numerics, void-containers, and rtc-digit-count against their respective competitors); those aggregates are scoped per suite and evolve per sweep, so we cite only this paper's own stage-1 record here and defer all cross-library totals to the live site. Readers are encouraged to reproduce: the harness (benchmarksuite commit 6196208), corpus, and competitor versions are pinned by commit hash.

### 10.3 Attribution of gains

Isolated-stage benchmarking (the harness's branch-switching mode) attributes the stage-1 margin primarily to three of the six divergences: the batched drain (§3) on all targets; the per-compiler geometry (§5) as the explanation of why the margin *holds across* toolchains rather than appearing on one and vanishing on another; and the minified specialization (§7) on the minified half of the corpus. The tape accounting details (§8) are individually small and collectively present.

With UTF-8 validation disabled on both sides, these margins exclude the contribution of §6 entirely. That makes them a clean test of the scan-and-drain architecture: simdjson's dedicated stage-1 validation pass, which Jsonifier does not run at all, does not count toward Jsonifier's margin. The width-transition carry of §6.1 belongs to string unescaping in stage 2 and likewise plays no part in these numbers.

None of the §10.2 targets exercises the AVX-512 compress drain of §4: the i9-14900KF is a Raptor Lake part with AVX-512 fused off, so all three x86 targets run the AVX2 kernel and its stepped scalar drain, and the NEON targets are 16-byte by construction. Every §10.2 margin on x86 is therefore earned by the batched architecture around the *scalar* stepped drain, which if anything strengthens the §3 argument, since the batching wins without its most favorable extraction instruction in play. The compress drain is measured separately in §11.

### 10.4 Structural-tape equivalence

Throughput is meaningful only if both libraries compute the same thing. To verify this directly, a branch of simdjson (nihilai-collective/simdjson, commit 7002cd3) exposes its stage-1 structural tape, and the harness compares it position-by-position against Jsonifier's (commit 3e7bf6d) on every test of the suite: the nine corpus documents in both variants plus the five POD-type tests. The comparison ran on five AVX2 and NEON builds, and every structural position matches on all five:

| Build | Jsonifier kernel | simdjson kernel |
|---|---|---|
| Linux / Clang 24.0 (Azure) | AVX2 | haswell |
| Linux / GCC 16.0.1 (Azure) | AVX2 | haswell |
| Windows / MSVC 19.51 | AVX2 | haswell |
| macOS / Clang 23.1 (Apple M1) | NEON | arm64 |
| macOS / GCC 16.2 (Apple M1) | NEON | arm64 |

| Document | Structurals (each variant) |
|---|---|
| Marine IK | 643,012 |
| Canada | 334,373 |
| Mesh | 153,274 |
| CitmCatalog | 135,990 |
| Random | 88,017 |
| Twitter | 33,981 |
| Instruments | 27,173 |
| Discord | 13,015 |
| Google Maps Response | 3,117 |
| **Corpus total** | **1,431,952** |
| Bool (POD, 200 elements) | 200 |
| Double (POD, 200 elements) | 200 |
| Int64 (POD, 200 elements) | 200 |
| String (POD, 200 elements) | 200 |
| Uint64 (POD, 200 elements) | 200 |
| **POD total** | **1,000** |

That is 2,863,904 corpus positions per build across the minified and prettified variants, plus 1,000 from the POD-type tests, for 2,864,904 positions per build and 14,324,520 across the five builds, with zero mismatches. Each corpus document yields the same count in its minified and prettified form, as the pseudo-structural definition of §2 requires: whitespace is never structural.

The equivalence rules out the most obvious alternative explanation for §10.2, namely that Jsonifier is faster because it emits less. It does not: on this suite its tape is simdjson's tape. One scope limit applies. The suite contains none of the control-character edge cases discussed in §9.3, where both libraries accept deliberate stage-1 imprecision that stage 2 resolves, so the result is a statement about this suite rather than about every possible input.

The check ran on 3e7bf6d, not on the commits swept in §10.2 (35b277d and e2f2df8). The §5 tape-step cells for AVX2 under GCC and for NEON under Clang changed between them, so the equivalence result has not been re-established on the exact code measured in §10.2. A tape step changes how many indices the stepped writer extracts per unconditional burst, not which positions it writes, so we expect the result to carry over, but the comparison has not been re-run.

---

## 11. AVX-512: Performance and Correctness

The AVX-512 kernel gets its own section because it is the only kernel that runs the compress drain of §4. None of §10's hardware can run it: the i9-14900KF has AVX-512 fused off, and NEON registers are 16 bytes wide. This section measures that drain for speed and checks it for correctness, on hardware where it actually executes.

### 11.1 Hardware and methodology

The AVX-512 throughput targets are an Intel Xeon Platinum 8370C (Ice Lake-SP) under Windows/MSVC 19.51, an AMD EPYC 9V45 under Linux/Clang 24.0, and an Intel Xeon Platinum 8573C under Linux/GCC 16.0.1, all Azure instances. On every target Jsonifier runs its AVX-512 kernel and simdjson runs its icelake kernel. Each target runs the same 23 tests, POD-type tests included, but the three were swept at different commits:

| Platform / Compiler | Sweep date | Jsonifier | simdjson | benchmarksuite |
|---|---|---|---|---|
| Windows / MSVC 19.51 | September 27, 2026 | 13fff52 | 645e5c8 | 343f572 |
| Linux / Clang 24.0 | September 27, 2026 | 13fff52 | 645e5c8 | 343f572 |
| Linux / GCC 16.0.1 | October 4, 2026 | 9249e11 | c903072 | 4e7c701 |

None of these is a §10 commit: the AVX-512 targets have not been swept on 35b277d or e2f2df8, so the §10 and §11 tallies measure different revisions of stage 1 and should not be summed. Within §11, the GCC build also measures a different Jsonifier revision and a different simdjson revision from the other two. Sampling and tie detection follow §10.1. The Windows host uses the same 5% RSE and 2.5% mean-shift convergence thresholds as §10's x86 targets. The two Linux hosts use the looser 10% and 10% thresholds that §10.1 applies to the virtualized M1, because their timing noise floor is higher. Every test converged on every target.

### 11.2 Throughput

Across the three AVX-512 combinations Jsonifier records **61 wins, 1 tie, 7 losses across 69 tests**. Linux/Clang sweeps all 23 tests; Linux/GCC loses a single test, and all seven losses fall on prettified documents:

| Platform / Compiler | Step (§5) | Wins | Ties | Losses |
|---|---|---|---|---|
| Windows / MSVC 19.51 (Xeon Platinum 8370C, AVX-512) | 4 blocks (256 B) | 17 | 0 | 6 |
| Linux / Clang 24.0 (AMD EPYC 9V45, AVX-512) | 8 blocks (512 B) | 23 | 0 | 0 |
| Linux / GCC 16.0.1 (Xeon Platinum 8573C, AVX-512) | 4 blocks (256 B) | 21 | 1 | 1 |
| **Aggregate** | | **61** | **1** | **7** |

Per-test throughput deltas (Jsonifier over simdjson, minus one; a negative delta is a loss unless marked as a tie):

| Test | Linux / Clang | Linux / GCC | Windows / MSVC |
|---|---|---|---|
| Bool (POD) | +5.5% | +17.7% | +92.3% |
| Double (POD) | +6.6% | +23.7% | +97.8% |
| Int64 (POD) | +4.8% | +25.6% | +93.0% |
| String (POD) | +11.6% | +20.1% | +54.8% |
| Uint64 (POD) | +4.1% | +24.3% | +93.9% |
| Canada (minified) | +39.6% | +30.1% | +20.7% |
| Canada (prettified) | +23.7% | +24.6% | +12.6% |
| CitmCatalog (minified) | +22.9% | +21.1% | +2.3% |
| CitmCatalog (prettified) | +3.8% | +2.8% | −3.8% |
| Discord (minified) | +10.0% | +14.9% | +4.3% |
| Discord (prettified) | +10.0% | +8.0% | −2.0% |
| Google Maps Response (minified) | +12.2% | +34.8% | +7.9% |
| Google Maps Response (prettified) | +6.0% | +1.0% (tie) | −3.3% |
| Instruments (minified) | +16.8% | +10.8% | +3.8% |
| Instruments (prettified) | +9.6% | +2.6% | −0.9% |
| Marine IK (minified) | +22.5% | +14.8% | +3.8% |
| Marine IK (prettified) | +12.2% | +3.8% | +2.8% |
| Mesh (minified) | +35.5% | +27.4% | +19.7% |
| Mesh (prettified) | +25.9% | +19.9% | +12.2% |
| Random (minified) | +33.6% | +19.1% | +5.1% |
| Random (prettified) | +9.1% | −1.5% | −1.8% |
| Twitter (minified) | +12.1% | +25.4% | +7.8% |
| Twitter (prettified) | +17.0% | +10.5% | −2.0% |

Four observations follow.

First, the result bears directly on §4's controlled-variable argument. On AVX-512 both libraries extract indices with an essentially identical compress → widen → add → store sequence, so the extraction instructions cancel out of the comparison. What remains is the surrounding architecture — batched versus word-interleaved invocation, scan-phase versus drain-time popcounts, fold-expression versus incremental tape offsets. On the corpus documents that architecture is worth up to +39.6% (Clang, minified Canada) and +35.5% (Clang, minified Mesh), and under Clang every one of the 23 tests is a win.

Second, the Clang and GCC builds split by test type. Each POD input is at most 64 bytes, so it fits in a single block and never reaches the step geometry of §5. On those five tests the 4-block GCC build posts the larger delta every time: +17.7% to +25.6%, against +4.1% to +11.6% for the 8-block Clang build. On the 18 corpus documents, where step width does apply, the Clang build posts the larger delta on 14. The four exceptions are prettified Canada and the minified variants of Discord, Google Maps Response and Twitter, where GCC leads. Compiler, step width and host (an AMD EPYC 9V45 for Clang, an Intel Xeon Platinum 8573C for GCC) all change together here, so this cannot separate codegen from geometry or from hardware. It is, however, consistent with §5's claim that wider steps pay off only where there are enough blocks to batch. Sweeping the AVX-512 GCC and MSVC cells at 8 blocks is the natural next experiment.

Third, the minified specialization of §7 is visible on every AVX-512 target. For 26 of the 27 document/target pairs, the minified variant's delta exceeds the prettified variant's; the one exception is Twitter on Clang. The losses follow the same line. Under MSVC every minified document is a win (+2.3% to +20.7%) and all six losses are prettified documents, each by 1% to 4%. Under GCC every minified document is a win (+10.8% to +34.8%); the single loss is prettified Random (−1.5%), and the only tie is prettified Google Maps Response (+1.0%). Both 4-block builds lose only on prettified input and the 8-block Clang build does not lose at all, but with compiler, step width and hardware all varying at once, we do not attribute that to geometry.

Fourth, the POD-type margins under MSVC are the largest in the paper, +54.8% to +97.8%. As in §10.2, these tests run stage 1 on 200 separate inputs of at most 64 bytes, so they measure small-input dispatch and per-call cost more than the steady-state scan loop.

### 11.3 Correctness: structural-tape equivalence on AVX-512

Throughput on AVX-512 means something only if the compress drain emits the right tape. This check is separate from the AVX2 and NEON check of §10.4 because it covers different code. The AVX-512 kernel replaces the whole extraction loop, including the tzcnt/blsr chain, with compress, widen, add and store, and it computes each lane's tape offset from counts taken during the scan (§4). None of that runs on the §10.4 builds.

The check uses the same tape-exposing simdjson branch as §10.4, on three builds, all run on September 25, 2026:

| Build | Jsonifier | simdjson |
|---|---|---|
| Linux / Clang 24.0 (Azure) | 7a91dd0, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |
| Linux / GCC 16.0.1 (Azure) | 7a91dd0, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |
| Windows / MSVC 19.51 | 7a91dd0, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |

The harness compared the two tapes position by position on every test of the suite, and the counts below held identically on all three builds:

| Test | Structurals compared | Mismatches |
|---|---|---|
| Marine IK (minified + prettified) | 643,012 × 2 | 0 |
| Canada (minified + prettified) | 334,373 × 2 | 0 |
| Mesh (minified + prettified) | 153,274 × 2 | 0 |
| CitmCatalog (minified + prettified) | 135,990 × 2 | 0 |
| Random (minified + prettified) | 88,017 × 2 | 0 |
| Twitter (minified + prettified) | 33,981 × 2 | 0 |
| Instruments (minified + prettified) | 27,173 × 2 | 0 |
| Discord (minified + prettified) | 13,015 × 2 | 0 |
| Google Maps Response (minified + prettified) | 3,117 × 2 | 0 |
| Bool (POD, 200 elements) | 200 | 0 |
| Double (POD, 200 elements) | 200 | 0 |
| Int64 (POD, 200 elements) | 200 | 0 |
| String (POD, 200 elements) | 200 | 0 |
| Uint64 (POD, 200 elements) | 200 | 0 |
| **Total** | **2,864,904** | **0** |

Every one of the 2,864,904 positions matches on each of the three builds, 8,594,712 in all. An error in the compress extraction, the widening, the base-offset add, or the count-gated stores would show up as a wrong or missing tape position; none does. The per-document counts are also identical to those of the five AVX2 and NEON builds in §10.4, so the same document produces the same tape on all three ISAs and under all three compilers on x86. Across all eight builds that is 22,919,232 positions with zero mismatches.

The check was run on 7a91dd0, while the Clang and MSVC throughput sweeps of §11 ran on 13fff52. The stage-1 kernel files (`avx_stage1.hpp`, `neon_stage1.hpp`, `sve2_stage1.hpp`, `add_tape_values.hpp`) are identical across those two commits; the remaining stage-1 differences are type aliases, identifier renames and license headers, so the equivalence result carries over to the swept Clang and MSVC AVX-512 code. The GCC throughput sweep ran on 9249e11, a commit the tape comparison was not run against. The §10 sweep ran on 35b277d and e2f2df8.

The check completes the controlled-variable argument of §4 and §11.2. The extraction instructions are essentially the same in both libraries, and the tapes are now shown to be identical. So the AVX-512 throughput differences in §11.2 come from the surrounding architecture, not from doing less work.

One scope limit applies, the same one as in §10.4. The suite contains none of the control-character edge cases discussed in §9.3, where both libraries accept deliberate stage-1 imprecision that stage 2 resolves, so the result is a statement about this suite rather than about every possible input.

---

## 12. Limitations and Non-Goals

Honesty about scope: simdjson's stage 1 provides capabilities Jsonifier's does not, and the comparison is complete only with them stated. When this paper was first written, that list included all of simdjson's multi-document streaming. Jsonifier has since added it for its schema-free parser: `jsonifier::generic::parser::iterateMany` runs this paper's stage 1 over fixed-size windows of a multi-document buffer, recovers document boundaries at window edges, accepts whitespace-separated (NDJSON) and comma-separated streams, and reports truncated trailing documents, as described and benchmarked against simdjson's `iterate_many` in the companion paper on generic parsing [8], §8. The remaining gaps are narrower. simdjson additionally supports RFC 7464 JSON text sequences and single-array (`comma_delimited_array`) streams, partial-UTF-8 tail trimming for chunked network input, a threaded mode that indexes the next window on a second thread, and the sentinel-padding scheme that makes its on-demand API safe against truncated garbage; Jsonifier has none of these. Neither library resumes a single document split across separate input buffers: both require each document to be complete within one window. Jsonifier's typed parser remains aimed at whole-document parsing into caller-defined types, and its Partial-Reading option addresses selective field extraction, not streaming.

The remaining benchmark losses are likewise stated plainly. On AVX2, Linux/GCC retains four losses, both forms of the float-dense Canada and Marine IK documents, and Linux/Clang loses all five POD-type tests by about 21%. Each is a win for both sibling compilers' builds on the same hardware, and MSVC has no losses or ties. On NEON, macOS/GCC retains five losses and macOS/Clang six losses and three ties, on virtualized M1 hosts; four of macOS/Clang's losses are POD-type tests, at −44.4% to −54.5%. The String POD-type test and prettified Mesh lose on both NEON builds. On AVX-512 (§11.1), Windows/MSVC, measured at 13fff52, retains six losses, all on prettified documents, and Linux/GCC, measured at 9249e11, retains one loss (prettified Random) and one tie (prettified Google Maps Response); Linux/Clang has none. Each is an open item. On AVX2, every x86 loss is a win for a sibling compiler's build on the same host. On AVX-512 every loss is a win for the Clang build, though the three AVX-512 builds ran on different hosts.

Additionally, the conformance evidence in §9 covers RFC 8259 document-level validity as exercised by the standard fail/pass corpus plus Jsonifier's extended suites; it is not a claim of byte-identical error *positions* with any other parser, and error-reporting granularity (Jsonifier reports global index, line, and local index with source context) is a feature comparison outside this paper's scope.

---

## 13. Conclusion

Six divergences separate Jsonifier's stage 1 from its simdjson ancestry: a batched drain that hands the out-of-order scheduler independent work instead of a hand-built pipeline; a compress-based AVX-512 index extraction operating as the batched architecture's native drain rather than as a kernel override retrofitted into an interleaved one; step geometry tuned per compiler on the position that the toolchain is part of the microarchitecture; UTF-8 validation eliminated from stage 1 entirely, fused register-by-register into the string-unescaping loop itself, and carried across the loop's SIMD width cascade through a three-byte validation state — riding loads the loop already performs at every width it occupies; a compile-time minified specialization that deletes whitespace classification when the caller knows it is unnecessary; and tape accounting built on precomputed counts. A seventh finding — the placement of control-character validation in stage 2 rather than stage 1, ratified by the full conformance matrix — yields the general principle that validation placement should follow byte-access patterns rather than precedent.

On the AVX2/NEON sweep the design records 89-3-20 against simdjson (on-demand) across 112 tests on five platform/compiler combinations, with a perfect 23-0-0 sweep under MSVC and every corpus document won under Clang on the same x86 host. On three AVX-512 combinations, where the compress drain of §4 is measured on real silicon in the September 27 and October 4, 2026 sweeps, the record is 61-1-7 across 69 tests, with a perfect 23-0-0 sweep under Clang; all seven AVX-512 losses fall on prettified documents under the two 4-block builds. Every structural position both libraries emit matches exactly across the suite on eight builds, three of them AVX-512. The residual losses and ties localize to specific toolchains and content shapes rather than to the architecture. None of these divergences individually is a revolution; stage 1 has looked "solved" since 2019 precisely because the foundational algorithms are excellent. The companion paper [5] pursues that last seam through the rest of the pipeline.

---

## References

1. G. Langdale, D. Lemire. "Parsing Gigabytes of JSON per Second." *The VLDB Journal*, 28(6), 2019.
2. simdjson, `src/generic/stage1/json_structural_indexer.h`, master branch. https://github.com/simdjson/simdjson (retrieved July 2026).
3. simdjson issue #1812, "Improve the AVX-512 kernel with the compress instructions." https://github.com/simdjson/simdjson/issues/1812 (2022).
4. D. Lemire. "Fast bitset decoding using Intel AVX-512." lemire.me/blog, May 2022.
5. Nihilai Collective Corp. "Two Stages, On Demand." Nihilai Collective Corp, 2026.
6. T. Bray (ed.). "The JavaScript Object Notation (JSON) Data Interchange Format." RFC 8259, IETF, 2017.
7. Jsonifier repository and live benchmark results. https://github.com/nihilai-collective/Jsonifier ; https://nihilai-collective.net
8. Nihilai Collective Corp. "Schema-Free, On Demand: Generic JSON Parsing in Jsonifier." Nihilai Collective Corp, 2026.