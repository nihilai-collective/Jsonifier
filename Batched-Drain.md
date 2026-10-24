# Batched Drain, Fused Scan: The Architecture of Jsonifier's Stage 1

**Nihilai Collective Corp: Engineering Papers**  
*Nihilai Collective Corp*  
*Jsonifier, October 2026*  

---

## Abstract

Structural indexing, the transformation of raw JSON text into a compact tape of structural character positions, has been the foundational pass of every high-performance JSON parser since simdjson established the SIMD two-stage paradigm in 2019. This paper documents the architecture of Jsonifier's stage-1 implementation and contrasts it point-by-point against simdjson's generic stage 1 (json_structural_indexer.h, master branch, retrieved July 2026). We identify six architectural divergences: (1) a batched bit-drain that decouples mask production from tape emission; (2) per-compiler step geometry ranging from 256 bytes to 512 bytes per iteration; (3) precomputed popcount accumulation replacing incremental tail advancement; (4) UTF-8 validation fused register-by-register into string unescaping, riding loads the unescape loop already performs and carried across SIMD width transitions via a three-byte state, where simdjson validates the entire buffer in a dedicated stage-1 pass; (5) a compile-time minified specialization that deletes whitespace classification from the scan entirely; and (6) an AVX-512 compress drain that is the batched architecture's native drain rather than an override retrofitted into a word-interleaved one. We further argue that validation placement should follow access patterns: Jsonifier deliberately places unescaped-control-character validation in stage 2, where string bytes are already resident in registers, rather than in stage 1, and we present full conformance results across an 8-configuration test matrix as evidence that this placement loses no correctness. Benchmarks across eight platform/compiler combinations (five AVX2/NEON, three AVX-512) validate the design, including perfect sweeps against simdjson on four of the eight, and a structural-tape comparison across eight builds confirms that both libraries emit identical structural positions, 2,864,904 per build, with zero mismatches.

---

## 1. Introduction

In 2019, Langdale and Lemire demonstrated that JSON parsing need not be a byte-at-a-time state machine. simdjson's two-stage architecture (a SIMD-heavy first pass that locates every structural character and validates UTF-8, followed by a second pass that materializes values using the resulting index) established the design vocabulary that every subsequent high-performance parser has worked within. Jsonifier is no exception: its stage 1 is a direct descendant of that lineage. The odd-backslash escape-resolution arithmetic, the prefix-XOR in-string masking, the pseudo-structural character definition, and the lookup4 UTF-8 validation tables all trace their ancestry to simdjson, and Jsonifier's source acknowledges this debt explicitly.

This paper is not about the shared foundation. It is about the divergences: the places where Jsonifier's stage 1 departs from simdjson's design, why those departures were made, and what they are worth empirically. Some of the divergences are microarchitectural (how bit masks are drained to the tape), some are build-system-level (per-compiler tuning of loop geometry), and one is philosophical (where in the pipeline validation belongs). Together they produce a stage 1 that wins the large majority of head-to-head benchmark comparisons across eight platform/compiler combinations, emits a structural tape identical to simdjson's on every tested document, and passes the complete JSON conformance suite in all eight of Jsonifier's tested parse-option configurations.

The scope of this paper is stage 1 only: the pass that consumes raw bytes and produces the structural index tape. The overall fused parse architecture, on-demand key handling, out-of-order key resolution, and stage-2 value materialization are covered in the companion paper, "Two Stages, On Demand" [5]. Where a design decision in stage 1 only makes sense in light of a stage-2 property, we state the property and defer the details.

The remainder of this paper proceeds as follows. Section 2 briefly establishes the shared foundation to make the divergences legible. Sections 3 through 8 present the six divergences in order of impact: the batched drain (§3), the AVX-512 compress drain (§4), per-compiler step geometry (§5), UTF-8 validation fused into string unescaping and carried across width transitions (§6), the minified specialization (§7), and tape accounting (§8). Section 9 presents a case study in validation placement, namely the placement of control-character validation in stage 2 rather than stage 1, with conformance results. Section 10 presents AVX2 and NEON benchmark results and structural-tape equivalence against simdjson. Section 11 does the same for AVX-512, where the compress drain of §4 runs. Section 12 states limitations honestly. Section 13 concludes.

---

## 2. Shared Foundation

Both parsers define stage 1's job identically: given a buffer of bytes, emit the position of every *pseudo-structural* character. A character is pseudo-structural if it is a structural operator (`{`, `}`, `[`, `]`, `:`, `,`), a quote delimiting a string, or the first character of a scalar token (a number, `true`, `false`, or `null`); that is, a scalar character whose predecessor is whitespace, a structural character, or a closing quote. Everything inside strings is opaque to stage 1 except for the machinery needed to know *where the strings are*.

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

In-string masking uses prefix XOR over the mask of unescaped quotes: every byte position between an opening and closing quote receives a 1 bit. On x86, Jsonifier computes this with a carry-less multiply against all-ones (`_mm_clmulepi64_si128`), the same PCLMULQDQ trick simdjson uses. On NEON targets with the cryptographic extension, the same product comes from a polynomial multiply (`vmull_p64`). Without it, and in the scalar fallback, a six-step Hillis-Steele XOR scan (`bitmask ^= bitmask << 1; ... << 32;`) produces the identical result. A sign-extension of the final bit (`static_cast<int64_t>(inString) >> 63`) carries the in-string state to the next word.

One structural refinement worth noting even in this "shared" section: Jsonifier's `rope_detector::next` branches on whether the word contains any quotes at all:

```cpp
return quotes ? finishNext() : finishNextNoInString();
```

The no-quote path skips the prefix-XOR entirely, simply propagating `prevInString`. For documents with long quote-free stretches (numeric arrays such as canada.json and mesh.json), this deletes the carry-less multiply from the majority of iterations. The branch is well-predicted precisely because such documents are homogeneous.

The UTF-8 validation tables are likewise shared ancestry: the three-shuffle lookup4 classification (byte1High, byte1Low, byte2High) with saturating-subtraction continuation checks is simdjson's algorithm. Jsonifier replicates the 16-entry tables to full register width (16, 32, or 64 bytes) at compile time via constexpr generator lambdas, so the same table constants serve SSE, AVX2, AVX-512, NEON, and SVE2 without per-ISA table definitions. Where the *placement* of validation differs, §6 covers it.

Having established what is shared, everything that follows is divergence.

---

## 3. Divergence 1: The Batched Drain

The single largest architectural difference between the two stage-1 implementations is *when tape emission happens relative to mask production*.

### 3.1 simdjson: interleaved word-granular drain

simdjson's inner loop processes 128 bytes per step as two 64-byte blocks. For each block, `json_structural_indexer::next` computes the structural mask, then immediately calls `bit_indexer::write` on the *previous* iteration's mask: a one-iteration software pipeline. `write` is the drain: it early-exits on a zero mask, popcounts, then extracts indices via a stepped unroll (`write_indexes_stepped` with STEP=4 up to a threshold of 24 set bits, falling back to a scalar loop beyond). Each extraction iteration is a serial dependency chain: trailing-zero count, store, clear-lowest-bit, repeat. Mask production and tape emission are interleaved at 64-bit-word granularity throughout the entire parse.

The consequence is that the drain's serial chain sits directly on the critical path between consecutive scan iterations. simdjson's in-source performance notes describe the mitigation: pipe two inputs at once so the scan of block N+1 can proceed while the drain of block N executes. This works, but it caps the overlap window at exactly one block, and it hard-codes that pipelining decision into the structure of the code.

### 3.2 Jsonifier: batch, then drain

Jsonifier separates the two phases completely. `processBlocks` runs the scan for `simdBlocksPerStep` consecutive 64-byte blocks, 4 or 8 blocks (256 or 512 bytes) on every configured target (§5), since the unrolled `if constexpr` chain supports at most 8 blocks per step. It stores each block's structural mask and its popcount into two stack arrays:

```cpp
array<uint64_t, simdBlocksPerStep> bitsArr;
array<uint64_t, simdBlocksPerStep> cntsArr;
processBlocksImpl<0>(bitsArr, cntsArr, blockPtr, ...);
processBlocksImpl<1>(bitsArr, cntsArr, blockPtr, ...);
...
add_tape_values<make_integer_sequence<simdBlocksPerStep>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);
```

Only after the entire step's masks exist does `add_tape_values` drain them all, with a fold expression walking the lanes and accumulating tape offsets from the precomputed counts. The generic (non-AVX-512) drain, the one every target in §10's sweep executes, is:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& __restrict bitsArr, const array<uint64_t, blocksPerStep>& __restrict cnts,
	write_structural_index_ptr __restrict tape, size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

(The AVX-512 `add_tape_values` of §4, selected by `#if JSONIFIER_AVX512` in `simd/add_tape_values.hpp`, has an identical `impl`; it differs only in its per-lane extraction body, which adds a zero-mask early exit ahead of the compress sequence.)

The performance argument is about the out-of-order window. Each lane's drain is still a serial tzcnt/blsr chain internally, but *adjacent lanes' chains are independent of each other*: lane 3's extraction does not consume lane 2's extracted values, only its precomputed count for the offset, and all counts were computed during the scan phase. A modern out-of-order core can therefore overlap the drain chains of multiple lanes with each other, and overlap the entire drain phase with the loads and shuffles of the *next* step's scan, without the source code dictating a fixed pipeline depth. Where simdjson hand-schedules a two-block pipeline, Jsonifier hands the scheduler a wide field of independent work and lets the hardware find the overlap.

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

The mechanism: a 512-bit register holds the identity byte sequence 0..63. `_mm512_maskz_compress_epi8`, masked by the structural bitmask, compacts the byte indices of every set bit to the front of the register in one instruction. The compacted bytes are then widened to 32-bit indices sixteen at a time (`_mm512_cvtepu8_epi32`), offset by the block's base position, and stored directly to the tape. There is no tzcnt, no blsr, and no per-bit loop anywhere in the hot path; the entire 64-bit mask drains in a handful of vector instructions whose count depends only on the popcount quartile, not on the popcount itself.

The count-gating (`count > 16`, `> 32`, `> 48`) deserves a note: real-world JSON rarely exceeds 16 structural characters per 64 bytes. Densely structural content, such as minified arrays of small numbers, approaches one structural per 2 bytes only pathologically; typical documents (twitter.json, citm_catalog.json) sit well under the first gate. The branches are therefore heavily biased and cheap, and the common case executes exactly one compress, one widen, one add, one store per 64-byte block.

The extraction technique is common to both libraries on AVX-512: simdjson's icelake `bit_indexer::write` and Jsonifier's `drainLane` are nearly instruction-identical in the extraction body, down to the same 0..63 identity constant, the same compress → cvtepu8 → add → store sequence, and the same 16/32/48 count gating, as both descend from the same 2022 blog post. This convergence is not a weakness of the comparison but its controlled variable: with the instruction sequence held essentially constant on both sides, any measured difference in drain throughput is attributable entirely to the surrounding architecture: batched versus word-interleaved invocation, scan-phase counts versus drain-time `count_ones`, fold-expression lane offsets versus incremental `tail += count`. Jsonifier's contribution is that context: a drain phase already decoupled from the scan, already batched across the step, and already fed by scan-phase counts, into which compress extraction fits without a hook, an override macro, or any per-word re-entry into the scan loop. Adjacent lanes' compress/widen/store sequences are independent and free to overlap in the out-of-order window, and the count-gating branches consume values computed an entire phase earlier.

---

## 5. Divergence 3: Per-Compiler Step Geometry

simdjson compiles one generic `step<128>`, two 64-byte blocks per iteration, for every compiler, on every ISA. The 128-byte figure was chosen, per the in-source performance notes, to soak available execution capacity on the microarchitectures of the time, and it is a reasonable universal default.

Jsonifier rejects the premise that a universal default exists. The step geometry, meaning how many 64-byte blocks are scanned per batch and (in the scalar-extraction fallback) how deep the stepped index writer unrolls, is selected per (ISA × compiler) pair at compile time:

| ISA | Clang | GCC | MSVC/other |
|---|---|---|---|
| AVX-512 | 8 blocks (512 B) | 8 blocks (512 B) | 8 blocks (512 B) |
| AVX2 | 4 blocks, tape step 4 | 8 blocks, tape step 4 | 8 blocks, tape step 4 |
| AVX/SSE | 8 blocks, tape step 8 | 8 blocks, tape step 8 | 8 blocks, tape step 8 |
| SVE2 | 8 blocks, tape step 8 | 8 blocks, tape step 8 | 8 blocks, tape step 8 |
| NEON | 4 blocks, tape step 8 | 4 blocks, tape step 8 | 4 blocks, tape step 8 |

(Values from `simd/simd_config.hpp`, which defines `simdBlocksPerStep` and `simdTapeStep` directly in a per-ISA `#if` chain; only the AVX2 cell branches on the compiler. AVX-512 has no tape-step cell because its drain is the fully vectorized compress path of §4, which has no stepped scalar writer to tune.)

The rationale is empirical rather than theoretical: the fully-unrolled scan body of §3 places materially different register-allocation and scheduling demands on each compiler. on AVX2, GCC and MSVC sustain 8-block steps that keep their generated code dense, while Clang's register allocation under the same 8-block unroll spills, and 4 blocks is its sweet spot. AVX-512, AVX and SVE2 take 8 blocks under every compiler, NEON takes 4, and the stepped tape writer bursts 8 indices everywhere except AVX2, where it bursts 4. Each cell in the table is the winner of a measured sweep on the benchmark corpus, not a guess.

The contrast is sharpest precisely where the vectors are widest. simdjson's icelake kernel classifies each 64-byte block as a single 512-bit chunk and retains the universal `index<128>` step, so on their widest ISA exactly two vector registers of scan work separate consecutive drains. Jsonifier's AVX-512 steps batch 8 such blocks (512 B) between drain phases under all three compilers, a 4x wider scan window on the hardware with the most execution capacity to fill.

The geometry constants gate `if constexpr` chains resolved at instantiation, so the divergence costs nothing at run time; it encodes the position that the compiler is part of the microarchitecture.

The sweeps reported in §10 and §11 predate the current table. For those runs `simdBlocksPerStep` was set to 8 blocks (512 B) on every ISA and under every compiler, so every target scanned the same step width; on AVX-512 that matches the table, while the AVX2 build under Clang and both NEON builds now ship 4-block steps. The cross-compiler variance in those results (compare the GCC and Clang columns on identical hardware) therefore comes from the compilers themselves at a fixed step width, not from differing geometry.

### 5.1 Selecting the tier at build time

Jsonifier compiles exactly one instruction-set tier per build, and the tier is chosen when CMake configures the project. At configure time, voided-hw-detection builds and runs a small detector program on the build machine. On x86 it reads CPUID leaves 1, 7 and 0x80000001 and checks XCR0 with `xgetbv`, so AVX, AVX2 and AVX-512 count only when the operating system saves their registers; AVX-512 additionally needs F, BW and VBMI2 together. On ARM it reads `HWCAP` on Linux and `sysctl` on macOS. The detector's output is cached in the build directory, so it runs once per configure, not once per build. CMake keeps the highest detected tier (AVX-512, then AVX2, then AVX on x86; SVE2 when its vector length is known, otherwise NEON on ARM), writes the selected feature bits into `simd/jsonifier_cpu_instructions.hpp`, and attaches the matching compiler flags to every translation unit that links Jsonifier. Setting `JSONIFIER_CPU_INSTRUCTIONS` at configure time replaces the detected bits, which is how cross-compiled builds pick their target; without it, a cross-compile skips detection and gets the scalar fallback. To build a lower tier than the one detected, the same configure-time value names it. `simd/simd_config.hpp` then selects the backend from the feature macros with a plain `#if` chain, and a `static_assert` stops the build if the detected register width disagrees with the selected backend's. The whole library is then compiled once for that tier. `jsonifier_core` is a single class that inherits its parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call is a direct, inlinable call into code built for that ISA. There is no CPUID read, no cached backend value and no per-call check.

simdjson takes the opposite position for its core and DOM API. It compiles one kernel per ISA and dispatches through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its On Demand front end, the closer comparison for Jsonifier, is mostly fixed at compile time. Stage 1 and string unescaping are still dispatched at run time, but the On Demand code itself is compiled for the builtin implementation that the compiler flags select, and its documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target.

The reason for one tier per build is inlining. Compiling several tiers into one binary needs per-function target attributes (`#pragma GCC target`, `#pragma clang attribute`), and those apply only to functions defined inside the region. A force-inlined, tier-specific callable passed to a standard-library template, such as the contexts Jsonifier hands to `std::basic_string::resize_and_overwrite`, is then called from code compiled at the baseline, and GCC and Clang reject that. Keeping `resize_and_overwrite` and the force-inlining is worth more than shipping several tiers in one binary, and a single tier per build keeps both. The cost is portability: a binary needs its configured tier on every machine that runs it, so a binary for mixed hardware is configured for the lowest tier it must support.

---

## 6. Divergence 4: UTF-8 Validation Fused into String Unescaping

Both parsers descend from the same lookup4 validation algorithm (§2). The divergence is *whether whole-buffer validation belongs in stage 1 at all*, and *how tightly the validator is threaded into the one loop guaranteed to touch every string byte*.

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

A single delimiter mask (backslash, quote, or any byte below 0x20) is built from the register the loop has just loaded and stored to the output; `validator.checkRegister(simdValue)` consumes that same load, so a plain (delimiter-free) register of string content is validated with zero additional memory traffic: the bytes are already resident because unescaping needed them anyway, and validation piggybacks on the load rather than issuing its own. When a register is cut short by a quote or backslash mid-register, `validator.checkPartial` covers exactly the consumed prefix, and `validator.errors()`, which includes the pending incomplete-sequence carry, is checked before the escape is processed, so no byte of string content is skipped regardless of how densely escapes are distributed. Because an escape sequence is pure ASCII and the carry has just been checked, the validator is then reset before scanning resumes. The trailing sub-register remainder is handled by a scalar epilogue inside the narrowest-width step of the cascade (§6.1), which calls `checkPartial` over each run of raw bytes as it reaches a quote or backslash. Coverage is therefore total over the string, assembled from several call sites rather than one continuous scan, but every one of those call sites is already inside the unescaping loop, at a point where the relevant bytes are already in a register for an unrelated reason.

The validator instance itself, `utf8_register_validator<simd_type_wrapper<simd_type>>`, is deliberately register-granular rather than block-granular: its unit of work is `sizeof(simd_type)` bytes (16, 32, or 64 depending on target), matching the width the unescaping loop already operates at, not a fixed 64-byte block independent of ISA:

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

(`maskNibble01` and `incompleteMax` are register constants built once per validator: a 0x80 broadcast and the width-appropriate tail of the `isIncompleteMax` table.) The ASCII fast path, the lookup4 special-case classification (`checkSpecialCases`), the 2-3-byte continuation check, and the incomplete-sequence carry via `isIncompleteMax` are all structurally the same lookup4 machinery as §2's shared ancestry; the divergence is exclusively in what unit of work the validator is built to consume and where it is invoked from, not in the underlying byte-classification algorithm. `checkPartial` handles sub-register remainders the same way the standalone block-level validator handles its tail: the partial content is memcpy'd into a scratch buffer padded with neutral ASCII (0x20 here; the standalone validator uses 0x41) and fed through the same `checkRegister` path, so no separate scalar validation code exists.

### 6.1 Validation Across Width Transitions

The description above holds for a single register width, but Jsonifier's string-unescaping loop is not single-width: it is a compile-time cascade over the target's full SIMD width list (`make_ascending_range<startIndex, listSize>`), draining a string through progressively narrower registers (64-byte steps while the remaining length supports them, then 32, then 16) before the scalar epilogue. (The cascade is 64→32→16 on AVX-512 and 32→16 on AVX2; SSE, NEON, and SVE2 have a single 16-byte width.) A validator whose carry state (`prevInput`, `incompleteRegister`) is a register of one fixed width cannot survive this cascade: naively resetting it at each width transition would sever the incomplete-sequence carry exactly where a multi-byte UTF-8 sequence straddles the boundary between a wide-register chunk and a narrow-register chunk.

The resolution follows from an observation about the lookup4 algorithm itself: the continuation checks consume at most the trailing three bytes of the previous register (`opPrev<15/14/13>`, alignr operations pulling 1, 2, and 3 tail bytes respectively), and the incomplete-sequence carry is fully determined by whether those same trailing bytes begin an unterminated sequence (≥ 0xC0 in the last position, ≥ 0xE0 in the second-to-last, ≥ 0xF0 in the third-to-last). The width-agnostic validation state is therefore three bytes, one incomplete flag, and one sticky error bit:

```cpp
struct utf8_validation_state {
	uint8_t prevBytes[3];
	bool prevIncomplete;
	bool error;
};
```

Each width's loop constructs a local, register-resident `utf8_register_validator` seeded from this state (the three carried bytes placed in the tail positions of a zeroed register are semantically indistinguishable from carrying the previous full register, regardless of what width that register was), runs its hot loop with the validator entirely in registers exactly as described above, and flushes back to the state (one store, three scalar compares, one reduction) only when the loop exits. The flush executes at most once per width traversed; the per-register cost within each width is unchanged from the single-width design. The flush's incomplete-flag computation applies the position-dependent thresholds above (the last carried byte against 0xC0, the second-to-last against 0xE0, the third-to-last against 0xF0), so the carry re-seeded into the next width is exactly the carry the register-resident `incompleteRegister` would have held. Copy construction and assignment of the register validator are explicitly deleted, converting any accidental duplication of in-flight carry state into a compile error rather than a silently reseeded validator with stale carry bytes.

The consequence is that validation coverage is continuous across the entire cascade: a 4-byte UTF-8 sequence whose lead byte lands in the final wide register of a string and whose continuations land in the subsequent narrower register is validated correctly, as is a sequence truncated by the true end of the string, which is caught by the padded `checkPartial` in whichever width sees the truncation, including the scalar epilogue, which reuses the narrowest width's validator and therefore the same carried state. The width-transition test family exercises both directions of this edge: multi-byte sequences deliberately straddling a transition (which must pass), and dangling lead bytes in each of the three carry positions at a transition followed by pure-ASCII content (which must fail, because the ASCII fast path's only cross-boundary guard is the seeded incomplete carry, making the flush thresholds load-bearing). simdjson's fixed 64-byte-block checker has no analogous problem to solve, since its stage-1 pass is width-homogeneous by construction, and that is precisely the point: fusing validation into a multi-width unescaping loop requires the carry state to be width-portable, and three bytes suffice.

The performance consequence of the cascade is that mid-length strings, the dominant string population in service payloads such as twitter.json and discord.json, drain through progressively narrower validated widths instead of falling from the widest register directly to the scalar epilogue, keeping vector-granular validation in play for string tails that a single-width design would validate byte-at-a-time.

### 6.2 The Accounting

Both parsers validate on loads already happening for another reason; simdjson's performance notes describe its stage-1 UTF-8 pass as filler work for pipeline slack. The divergence is what the load was for. simdjson's stage-1 loop loads every 64-byte block of the buffer to build the structural index, so its validator also covers punctuation, whitespace, and numeric content. Jsonifier's unescaping loop loads a register only to copy or unescape string content, a load scoped to exactly the bytes validation cares about. For documents that are structurally dense and string-light (canada.json, mesh.json: coordinate arrays), that scoping difference means the validated fraction of the buffer approaches zero for Jsonifier while remaining total for simdjson; even for string-heavy documents (twitter.json, discord.json) the validation cost is not a separate pass but a handful of additional vector instructions per register already in flight for string-handling reasons. simdjson's placement is, again, forced by its access pattern rather than by any inattention to cost: an on-demand consumer may never read a string's bytes at all, so stage 1 is the only pass guaranteed to see them, and validating the whole buffer in a dedicated pass, and accepting that most of what it loads is not string content, is the price of that guarantee.

The UTF-8 unit suite has two halves. 147 cases exercise `validateUtf8`, the standalone whole-buffer API for arbitrary byte buffers, which is built on the block-granular `utf8_checker` rather than the register-granular string-scanner validator. They cover dangling leads at chunk, block, and step boundaries; surrogates; overlongs in all widths; and continuation-position errors crossing chunk seams. 51 more cases drive the string-fused path through a full parse, including multi-byte sequences deliberately straddling width-transition boundaries within the cascade and dangling leads in each carry position at a transition (per §6.1). Width-transition, unaligned-pointer, and page-boundary sweeps round it out. The conformance corpus separately confirms the string-fused path end-to-end: fail34.json (a raw 0x80 byte inside a string) is rejected with `invalid_string_characters` by `checkRegister`/`checkPartial` during unescaping. Both the standalone validator and the fused string-scanner path are exercised under the project's AddressSanitizer and UndefinedBehaviorSanitizer builds across all five platform/compiler targets, and pass cleanly. That is meaningful here specifically because register-granular validators carrying cross-call and now cross-width state (`utf8_validation_state`, the seeded `prevInput`/`incompleteRegister` members) are exactly the shape of code where an off-by-one in the padding, seeding, or carry logic would manifest as a sanitizer-visible read past a buffer's end rather than merely a wrong answer.

---

## 7. Divergence 5: The Minified Specialization

Stage 1's whitespace classification exists to separate scalars from padding. But if the caller knows the input is minified (machine-generated JSON with no interstitial whitespace, which describes virtually all service-to-service traffic), the classification is pure waste: the only whitespace-adjacent character that matters is the space character's role in the op-table trick, and the general whitespace shuffle table contributes nothing.

Jsonifier makes this a compile-time template parameter. `reset<minified>` instantiates two distinct scan loops. The prettified path computes, per block:

```cpp
const uint64_t whitespace = simd::ws_collector::impl(in01, whitespaceTableLocal);
const uint64_t op         = simd::op_collector::impl(in01, opTable, spaceMask);
const uint64_t scalar     = ~(op | whitespace | quotes);
```

The minified path deletes the first line and its inputs entirely (the whitespace table register is never even loaded), and the scalar definition tightens to `~(op | quotes)`:

```cpp
const uint64_t op     = simd::op_collector::impl(in01, opTable, spaceMask);
const uint64_t scalar = ~(op | quotes);
```

Per 64-byte block, this removes one shuffle and one full-width compare per register, plus the register pressure of holding the whitespace table live through the loop. On a 128-bit target that is four shuffles and four compares per block; over an 8-block GCC step, sixty-four eliminated vector operations per 512-byte step. On NEON the prettified path computes both masks in one call (`ws_op_collector`), which reduces the whitespace and operator matches through a shared chain of paired adds and returns both 64-bit masks in one vector; the minified path still calls `op_collector` alone. The remainder trim (§8) guarantees correctness at the padded tail in both variants.

simdjson has no equivalent. Its json_minifier is an output transformation (produce minified text), not a parse-time fast path; its structural indexer always classifies whitespace. The distinction reflects the two libraries' relationship to compile-time knowledge: simdjson selects among precompiled kernels at runtime by CPU features, while Jsonifier's interface propagates caller knowledge about the *input's shape* into the instantiation itself. This is the stage-1 expression of the compile-time-knowledge thesis developed at length in the companion paper.

Every corpus test in §9's matrix runs in both Minified and Prettified variants; both paths pass identically.

---

## 8. Divergence 6: Tape Accounting and Remainder Handling

Three smaller mechanical divergences complete the inventory.

**Count computation.** simdjson recomputes the popcount inside `write` at drain time and advances the tail pointer incrementally per word. Jsonifier computes every popcount once, during the scan phase (`correctedPopcount` in `processBlocksImpl`), stores it in `cntsArr`, and both the drain's lane offsets and the final `tapeCount` advancement are pure additions over precomputed values. The popcounts thus execute in the scan's instruction mix, where port pressure differs from the drain's, and the drain's offset arithmetic has no dependence on extraction results.

**NEON index extraction.** On NEON, the 64-bit structural mask is synthesized from four 16-byte comparison results via the paired-add (`vpaddq_u8`) bitmask reduction, yielding one bit per byte. The backslash and quote masks share the final paired add, so one reduction yields both. The stepped writer extracts indices with a plain trailing-zero count (`countrZero`) and `blsr`, exactly as on x86. (The nibble-granularity `vshrn_n_u16` masks and the matching `postCmpTzcnt`, a trailing-zero count right-shifted by 2, are used by the string-scanning and comparison code, not by stage 1.) Unlike simdjson's SIMDJSON_PREFER_REVERSE_BITS option, Jsonifier has no bit-reversed extraction path.

**Convergent stepped writers.** In the non-AVX-512 fallback, Jsonifier's `write_indices_stepped_functor`, which extracts a fixed step of indices unconditionally and branches only on whether the count demands another step, is structurally identical to simdjson's `write_indexes_stepped`. We note this as convergent design rather than divergence, with one delta: simdjson's step is a single compile-time constant (STEP=4, overridable by macro), while Jsonifier's `simdTapeStep` is a cell in the per-compiler table of §5, which is 4 or 8.

**Remainder handling.** The two libraries pad opposite problems. simdjson pads *forward*: after the final block, it writes sentinel indices (the document length, repeated) beyond the last real structural, so that on-demand parsing which runs past the end lands on positions that provably cannot extend a valid document (the "repeated `[`" argument documented in their finish routine). Jsonifier pads *backward*: the tail is copied into a 0x20-filled (space) step-sized buffer, scanned through the identical vector path, and then over-emitted indices are trimmed:

```cpp
const uint64_t excess = stepBytes - remaining;
while (excess > 0 && tapeCount > 0 && tape[tapeCount - 1] >= string_block_reader::length) {
	--tapeCount;
}
```

Space padding generates no structural bits by construction, so the trim loop's work is bounded by pathological cases (a document ending mid-token); `begin()` then writes a single terminating index equal to the document length. The bounds-truncation test family in §9, which truncates every corpus document at every step-boundary-adjacent length across all eight option configurations, exists specifically to police this edge, and passes in full.

---

## 9. Case Study: Validation Placement Follows Access Patterns

This section examines a single validation rule, the rejection of unescaped control characters inside strings, as a case study in where validation belongs, because the two libraries place the identical check in different stages, and each placement is correct for its own architecture.

### 9.1 The rule and simdjson's placement

simdjson's stage 1 does one piece of *content* validation beyond UTF-8: it accumulates a mask of unescaped control characters (bytes below 0x20) and ANDs it against the in-string mask, because a raw control character inside a string is invalid JSON (RFC 8259 §7). This check lives in stage 1 for a structural reason: simdjson's on-demand stage 2 may *never touch a string's bytes again* (a document consumer that skips a field skips its bytes), so if stage 1 does not catch the raw tab, nothing will.

### 9.2 Jsonifier's placement

Jsonifier places the same validation in stage 2, during string materialization, byte-adjacent to the unescaping work it must do anyway. The §6 listing shows it as the below-0x20 comparison folded into the delimiter mask of every register the unescaping loop processes. The conformance suite confirms the placement end-to-end: the canonical failure cases for this exact rule, fail25.json (raw tab characters inside a string) and fail27.json (a raw line break inside a string), are rejected with `invalid_string_characters`, with the rejection originating in the stage-2 json_iterator, not in any stage-1 mask.

The §6.2 argument for UTF-8 applies here unchanged. Jsonifier's stage 2 walks every string it materializes, so the control-character check rides a load unescaping was always going to issue, and stage-1 accumulation would be redundant work on every block of every document. simdjson validates in stage 1 because its stage 2 might not look; Jsonifier validates in stage 2 because its stage 2 always looks.

The placement is ratified by the full conformance matrix: 77 fail-case documents (correctly rejected), 27 pass-case documents (correctly accepted), 27 round-trip documents, the 35-case string-pass and 26-case string-fail suites, the float/uint/int32_t numeric suites, the UTF-8 boundary suite (§6.2), the full corpus parse tests in minified and prettified variants, and the bounds-truncation family, each executed under all eight parse-option configurations (Partial-Reading × Known-Order × Null-Terminated, all toggles). Every test passes.

### 9.3 The principle

We generalize the comparison into the design rule this section is named for: **validation belongs where the validated bytes are already resident.** A parser whose second stage is guaranteed to walk string content should validate string content there; a parser whose second stage may skip content must validate it in the pass that is guaranteed to see it. Neither placement is universally correct; the access pattern decides. Copying a competitor's validation placement without copying their access pattern imports their costs without their necessity.

The principle is not foreign to simdjson itself. Their AVX-512 classifier's `| 0x20` op-table trick knowingly misclassifies the control characters 0x0C and 0x1A as operators, and the in-source comment notes that "this gets caught in stage 2, which checks the actual character." That is a deliberate acceptance of stage-1 imprecision on exactly the grounds argued here: stage 2 will look at those bytes anyway, so stage 1 need not be exact about them. The two libraries differ only in how far they extend the same logic.

---

## 10. Empirical Results: AVX2 and NEON

### 10.1 Methodology

Jsonifier's benchmark harness runs at nihilai-collective.net with live CSV result fetching from the repository's CI, branch-switching for isolated stage comparisons, and per-test win/tie/loss adjudication against simdjson (on-demand). The sweep ran on October 24, 2026, with Jsonifier b975cd7 and benchmarksuite ab75024 on every target. simdjson is 70f83c9 on Linux/Clang, Linux/GCC and both macOS targets, and 957ddbc on Windows/MSVC. Every target scans 8 blocks (512 B) per step rather than the per-compiler widths of the §5 table. The sweep covers 23 head-to-head tests per platform/compiler combination: five POD-type tests (Bool, Double, Int64, String, Uint64), each running stage 1 separately on 200 single-value inputs of at most 64 bytes, plus nine corpus documents (Canada, CitmCatalog, Discord, Google Maps Response, Instruments, Marine IK, Mesh, Random, Twitter), each in minified and prettified form. Both libraries execute stage 1 only (structural indexing into a tape), and **neither performs UTF-8 validation** in these runs, so the comparison isolates the scan and drain machinery of §§3–5, 7, and 8; the fused validation of §6 is not exercised on either side.

Sampling is adaptive: iterations begin at 100 and double each epoch up to a 100,000-iteration cap, with caches cleared before every iteration. Each epoch evaluates a trailing window of max(iterations/10, 30) samples. Sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap is reached, every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is retained. A result counts as converged only if that epoch has RSE below 5% *and* mean shift below 2.5%, on every target including the virtualized Apple Silicon ones; non-converged results are kept in the tallies and marked n/c in the per-test tables. Variance is Bessel-corrected, and ties are declared by Welch's t-test rather than by a fixed throughput band.

The hardware is an Intel i9-14900KF (Raptor Lake, AVX-512 fused off) under Windows/MSVC 19.44, Linux/Clang 24.0, and Linux/GCC 16.2, all under Linux 7.0.0 or Windows on the same machine, plus virtualized Apple M1 instances under macOS/GCC 16.2 and macOS/Clang 23.1. On x86 all three builds share one host. Both NEON builds report the same chip, but they run as separate virtual machines, so a GCC-versus-Clang comparison on NEON still carries host-noise effects. AVX-512 hardware is covered separately in §11.

Throughout this section, results are reported per test as a percentage delta: Jsonifier's throughput over simdjson's, minus one. A positive delta favors Jsonifier. With 23 tests per target there is no need to collapse them into a single mean; every number is shown.

### 10.2 Headline results (AVX2 and NEON)

On this sweep, Jsonifier's record against simdjson (on-demand) across the five AVX2/NEON platform/compiler combinations is **101 wins, 2 ties, 12 losses across 115 head-to-head tests**, including perfect 23-0-0 sweeps on Windows/MSVC and Linux/Clang. All three x86 targets converged on every test; on NEON, macOS/GCC has six non-converged results and macOS/Clang one, marked n/c below:

| Platform / Compiler | Wins | Ties | Losses |
|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 23 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 23 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 19 | 0 | 4 |
| macOS / GCC 16.2 (Apple M1, NEON) | 20 | 1 | 2 |
| macOS / Clang 23.1 (Apple M1, NEON) | 16 | 1 | 6 |
| **Aggregate** | **101** | **2** | **12** |

Per-test throughput deltas (Jsonifier over simdjson, minus one; a negative delta is a loss unless marked as a tie):

| Test | Windows / MSVC | Linux / Clang | Linux / GCC | macOS / GCC | macOS / Clang |
|---|---|---|---|---|---|
| Bool (POD) | +97.6% | +44.1% | +55.0% | +36.5% | +25.8% |
| Double (POD) | +90.7% | +31.8% | +60.8% | +83.9% (n/c) | −0.5% (tie) |
| Int64 (POD) | +74.0% | +43.3% | +49.7% | +56.7% | +43.5% |
| String (POD) | +56.6% | +7.6% | +28.2% | +9.8% | −20.3% |
| Uint64 (POD) | +66.9% | +27.1% | +50.2% | +32.6% | +26.5% |
| Canada (minified) | +44.3% | +26.0% | −5.4% | +21.2% (n/c) | +73.8% |
| Canada (prettified) | +43.7% | +17.2% | −2.1% | +0.6% (tie) | +4.9% (n/c) |
| CitmCatalog (minified) | +15.1% | +12.4% | +6.2% | +11.2% (n/c) | +1.8% |
| CitmCatalog (prettified) | +12.9% | +7.5% | +12.8% | +8.9% | +6.2% |
| Discord (minified) | +14.6% | +10.4% | +9.5% | +1.6% | +7.8% |
| Discord (prettified) | +9.7% | +8.2% | +4.3% | +3.1% (n/c) | −9.8% |
| Google Maps Response (minified) | +13.4% | +6.6% | +9.2% | −3.1% | +5.7% |
| Google Maps Response (prettified) | +10.9% | +6.9% | +10.9% | +15.9% | +15.0% |
| Instruments (minified) | +14.7% | +10.5% | +8.2% | +13.5% | −4.0% |
| Instruments (prettified) | +7.4% | +8.9% | +4.6% | +7.4% (n/c) | −3.9% |
| Marine IK (minified) | +28.8% | +24.1% | −8.0% | −9.9% (n/c) | +38.2% |
| Marine IK (prettified) | +28.3% | +13.4% | −10.7% | +4.9% | +12.4% |
| Mesh (minified) | +24.6% | +25.3% | +29.3% | +20.6% | +16.9% |
| Mesh (prettified) | +12.9% | +12.8% | +14.2% | +5.9% | −4.1% |
| Random (minified) | +14.9% | +10.4% | +10.5% | +16.6% | +2.1% |
| Random (prettified) | +13.4% | +11.1% | +8.7% | +14.7% | +11.7% |
| Twitter (minified) | +18.5% | +10.0% | +18.5% | +17.1% | +5.8% |
| Twitter (prettified) | +11.5% | +2.1% | +11.4% | +5.9% | −2.9% |

On the i9-14900KF, the three x86 builds record 65 wins, 0 ties and 4 losses across 69 tests. MSVC wins all 23, by +7.4% (prettified Instruments) up to +97.6% (Bool array); on the corpus documents alone its range is +7.4% to +44.3% (minified Canada). Clang also wins all 23: the corpus documents by +2.1% (prettified Twitter) to +26.0% (minified Canada), and the five POD-type tests by +7.6% (String) to +44.1% (Bool). MSVC and GCC win the same five tests by +28.2% to +97.6%. Each POD-type test runs stage 1 on 200 separate inputs of at most 64 bytes, so fixed per-call cost dominates in both libraries; these tests measure small-input dispatch and setup overhead more than the steady-state scan loop.

The losses localize to specific toolchains rather than spreading evenly. Linux/GCC's four losses are the two float-dense documents in both forms: minified Canada (−5.4%), prettified Canada (−2.1%), minified Marine IK (−8.0%) and prettified Marine IK (−10.7%). All four are wins for both MSVC and Clang on the same hardware. On NEON, macOS/GCC loses two tests, minified Marine IK (−9.9%, not converged) and minified Google Maps Response (−3.1%), and ties prettified Canada. macOS/Clang loses six: the String POD-type test (−20.3%), prettified Discord (−9.8%), prettified Mesh (−4.1%), minified Instruments (−4.0%), prettified Instruments (−3.9%) and prettified Twitter (−2.9%), and ties the Double POD-type test. It also posts the largest corpus margins in the sweep on float-dense content: +73.8% on minified Canada and +38.2% on minified Marine IK. No test loses on both NEON builds: every NEON loss is a win for the other compiler's build, and every x86 loss is a win for both sibling compilers' builds. The same source code, at the same step width, has different weak spots under GCC than under Clang, and on x86, none under MSVC or Clang. This is the compiler-as-microarchitecture point of §5, with the caveat from §10.1 that the two NEON builds run on separate virtual machines.

Per-platform tables, per-document plots, and the raw CSVs are served at nihilai-collective.net directly from the repository's committed CSV artifacts (the rendered figures are client-side fetches of the same files a reproducing reader would generate) and should be consulted for current numbers; the figures above are a snapshot at time of writing. Readers are encouraged to reproduce: the harness (benchmarksuite commit ab75024), corpus, and competitor versions are pinned by commit hash.

### 10.3 Attribution of gains

Isolated-stage benchmarking (the harness's branch-switching mode) attributes the stage-1 margin primarily to three of the six divergences: the batched drain (§3) on all targets; the per-compiler geometry (§5) as the explanation of why the margin *holds across* toolchains rather than appearing on one and vanishing on another; and the minified specialization (§7) on the minified half of the corpus. The tape accounting details (§8) are individually small and collectively present.

With UTF-8 validation disabled on both sides, these margins exclude the contribution of §6 entirely. That makes them a clean test of the scan-and-drain architecture: simdjson's dedicated stage-1 validation pass, which Jsonifier does not run at all, does not count toward Jsonifier's margin. The width-transition carry of §6.1 belongs to string unescaping in stage 2 and likewise plays no part in these numbers.

None of the §10.2 targets exercises the AVX-512 compress drain of §4: the i9-14900KF is a Raptor Lake part with AVX-512 fused off, so all three x86 targets run the AVX2 kernel and its stepped scalar drain, and the NEON targets are 16-byte by construction. Every §10.2 margin on x86 is therefore earned by the batched architecture around the *scalar* stepped drain, which if anything strengthens the §3 argument, since the batching wins without its most favorable extraction instruction in play. The compress drain is measured separately in §11.

### 10.4 Structural-tape equivalence

Throughput is meaningful only if both libraries compute the same thing. To verify this directly, a branch of simdjson (nihilai-collective/simdjson, commit 7002cd3) exposes its stage-1 structural tape, and the harness compares it position-by-position against Jsonifier's (commit 769b6b0, an earlier commit than the b975cd7 swept in §10.2) on every test of the suite: the nine corpus documents in both variants plus the five POD-type tests. The comparison ran on October 21, 2026 on five AVX2 and NEON builds, and every structural position matches on all five:

| Build | Jsonifier kernel | simdjson kernel |
|---|---|---|
| Linux / Clang 24.0 (Azure) | AVX2 | haswell |
| Linux / GCC 16.0.1 (Azure) | AVX2 | haswell |
| Windows / MSVC 19.51 (Azure) | AVX2 | haswell |
| macOS / Clang 23.1 | NEON | arm64 |
| macOS / GCC 16.2 | NEON | arm64 |

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

The check ran at 769b6b0 rather than at the swept commit, and at the §5 table's step widths rather than the 8-block step of §10.2, so it covers the same stage-1 algorithms measured there, including the NEON polynomial-multiply prefix XOR of §2, the paired backslash/quote reduction of §8 and the fused whitespace/operator collector of §7. The three x86 builds ran on Azure rather than on the i9-14900KF of §10.2, and each reports AVX2 as Jsonifier's active kernel and haswell as simdjson's, so they executed the same kernels §10.2 measured. The two macOS builds report NEON and arm64.

---

## 11. AVX-512: Performance and Correctness

The AVX-512 kernel gets its own section because it is the only kernel that runs the compress drain of §4. None of §10's hardware can run it: the i9-14900KF has AVX-512 fused off, and NEON registers are 16 bytes wide. This section measures that drain for speed and checks it for correctness, on hardware where it actually executes.

### 11.1 Hardware and methodology

The AVX-512 throughput targets are three builds on one Azure instance with an AMD EPYC 9V45: Linux/Clang 24.0 under Linux 7.0, Linux/GCC 16.0.1 under Linux 7.0, and Windows/MSVC 19.51 under Windows. On every target Jsonifier runs its AVX-512 kernel and simdjson runs its icelake kernel. All three were swept on October 24, 2026 with Jsonifier 96673f9, simdjson 957ddbc and benchmarksuite ab75024. Each target runs the same 23 tests, POD-type tests included.

Because the CPU is shared, every build scans 8 blocks (512 B) per step, and all three builds share every commit, the three targets differ only in compiler. Jsonifier is at the same commit as in §10 but the hardware differs, so the §10 and §11 tallies are reported separately. Sampling and tie detection follow §10.1, with convergence thresholds of 10% RSE and 5% mean shift on all three builds. Every test converged on every target.

### 11.2 Throughput

Across the three AVX-512 combinations Jsonifier records **68 wins, 0 ties, 1 loss across 69 tests**. Windows/MSVC and Linux/Clang sweep all 23 tests, and Linux/GCC wins 22 with one loss of under 5%:

| Platform / Compiler | Step | Wins | Ties | Losses |
|---|---|---|---|---|
| Windows / MSVC 19.51 (AMD EPYC 9V45, AVX-512) | 8 blocks (512 B) | 23 | 0 | 0 |
| Linux / Clang 24.0 (AMD EPYC 9V45, AVX-512) | 8 blocks (512 B) | 23 | 0 | 0 |
| Linux / GCC 16.0.1 (AMD EPYC 9V45, AVX-512) | 8 blocks (512 B) | 22 | 0 | 1 |
| **Aggregate** | | **68** | **0** | **1** |

Per-test throughput deltas (Jsonifier over simdjson, minus one; a negative delta is a loss unless marked as a tie):

| Test | Linux / Clang | Linux / GCC | Windows / MSVC |
|---|---|---|---|
| Bool (POD) | +7.8% | +41.2% | +61.6% |
| Double (POD) | +10.4% | +43.3% | +55.0% |
| Int64 (POD) | +12.2% | +27.6% | +53.5% |
| String (POD) | +9.9% | −2.2% | +48.8% |
| Uint64 (POD) | +10.5% | +36.8% | +58.6% |
| Canada (minified) | +45.0% | +24.0% | +52.9% |
| Canada (prettified) | +20.7% | +22.0% | +44.6% |
| CitmCatalog (minified) | +22.6% | +3.5% | +21.8% |
| CitmCatalog (prettified) | +1.2% | +3.0% | +9.2% |
| Discord (minified) | +8.8% | +5.9% | +27.4% |
| Discord (prettified) | +8.3% | +6.4% | +12.4% |
| Google Maps Response (minified) | +15.4% | +1.1% | +20.2% |
| Google Maps Response (prettified) | +6.0% | +6.0% | +14.5% |
| Instruments (minified) | +22.9% | +6.0% | +19.7% |
| Instruments (prettified) | +8.2% | +11.2% | +11.4% |
| Marine IK (minified) | +27.5% | +5.3% | +32.7% |
| Marine IK (prettified) | +9.9% | +13.3% | +24.2% |
| Mesh (minified) | +38.3% | +17.3% | +46.2% |
| Mesh (prettified) | +23.8% | +18.9% | +33.8% |
| Random (minified) | +30.8% | +8.4% | +25.5% |
| Random (prettified) | +8.3% | +8.7% | +14.6% |
| Twitter (minified) | +15.4% | +12.6% | +31.5% |
| Twitter (prettified) | +16.2% | +8.6% | +18.1% |

Four observations follow.

First, the result bears directly on §4's controlled-variable argument. On AVX-512 both libraries extract indices with an essentially identical compress → widen → add → store sequence, so the extraction instructions cancel out of the comparison. What remains is the surrounding architecture: batched versus word-interleaved invocation, scan-phase versus drain-time popcounts, fold-expression versus incremental tape offsets. On the corpus documents that architecture is worth up to +52.9% (MSVC, minified Canada), and all 54 corpus-document results are wins.

Second, this sweep holds hardware and step geometry fixed: one CPU, and 8 blocks (512 B) under every compiler. All three columns share every commit, so the differences between them are pure compiler effects. The differences are large. On the 18 corpus documents MSVC posts the largest delta on 15 and Clang on the remaining three (minified CitmCatalog, Instruments and Random); GCC posts none. GCC trails on minified input in particular: its minified deltas run +1.1% to +24.0%, against +8.8% to +45.0% under Clang and +19.7% to +52.9% under MSVC. The same source and the same geometry leave different margins under different compilers, which is the compiler-as-microarchitecture point of §5.

Third, the minified specialization of §7 is visible on most AVX-512 pairs. For 20 of the 27 document/target pairs, the minified variant's delta exceeds the prettified variant's. Six of the seven exceptions are on GCC: Discord (+5.9% minified, +6.4% prettified), Google Maps Response (+1.1%, +6.0%), Instruments (+6.0%, +11.2%), Marine IK (+5.3%, +13.3%), Mesh (+17.3%, +18.9%) and Random (+8.4%, +8.7%); the seventh is Twitter under Clang (+15.4%, +16.2%). Every document in both forms is a win on every target, with prettified deltas from +1.2% to +44.6%.

Fourth, the POD-type tests split by compiler: +48.8% to +61.6% under MSVC, −2.2% to +43.3% under GCC, and +7.8% to +12.2% under Clang. GCC's String POD-type test is the sweep's only loss. As in §10.2, these tests run stage 1 on 200 separate inputs of at most 64 bytes, so they measure small-input dispatch and per-call cost more than the steady-state scan loop.

### 11.3 Correctness: structural-tape equivalence on AVX-512

Throughput on AVX-512 means something only if the compress drain emits the right tape. This check is separate from the AVX2 and NEON check of §10.4 because it covers different code. The AVX-512 kernel replaces the whole extraction loop, including the tzcnt/blsr chain, with compress, widen, add and store, and it computes each lane's tape offset from counts taken during the scan (§4). None of that runs on the §10.4 builds.

The check uses the same tape-exposing simdjson branch as §10.4, on three builds, all run on October 24, 2026:

| Build | Jsonifier | simdjson |
|---|---|---|
| Linux / Clang 24.0 (Azure) | 96673f9, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |
| Linux / GCC 16.0.1 (Azure) | 96673f9, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |
| Windows / MSVC 19.51 | 96673f9, AVX-512 kernel | 7002cd3 (nihilai-collective branch), icelake kernel |

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

The check ran at 96673f9, the commit swept in §11.2, with the same 8-block AVX-512 step, so it covers exactly the compress drain, widening and count-gated stores measured there.

With the extraction instructions essentially the same and the tapes identical, the AVX-512 throughput differences in §11.2 come from the surrounding architecture, not from doing less work. The §10.4 scope limit on control-character edge cases applies here too.

---

## 12. Limitations and Non-Goals

Honesty about scope: simdjson's stage 1 provides capabilities Jsonifier's does not, and the comparison is complete only with them stated. Multi-document streaming is no longer among them for Jsonifier's schema-free parser: `jsonifier_core::iterateMany` runs this paper's stage 1 over fixed-size windows of a multi-document buffer, recovers document boundaries at window edges, accepts whitespace-separated (NDJSON) and comma-separated streams, and reports truncated trailing documents, as described and benchmarked against simdjson's `iterate_many` in the companion paper on generic parsing [8], §8. The remaining gaps are narrower. simdjson additionally supports RFC 7464 JSON text sequences and single-array (`comma_delimited_array`) streams, partial-UTF-8 tail trimming for chunked network input, a threaded mode that indexes the next window on a second thread, and the sentinel-padding scheme that makes its on-demand API safe against truncated garbage; Jsonifier has none of these. Neither library resumes a single document split across separate input buffers: both require each document to be complete within one window. Jsonifier's typed parser remains aimed at whole-document parsing into caller-defined types, and its Partial-Reading option addresses selective field extraction, not streaming.

The remaining benchmark losses are likewise stated plainly. On AVX2, Linux/GCC retains four losses, all on the float-dense Canada and Marine IK documents. Each is a win for both sibling compilers' builds on the same hardware, and MSVC and Clang have no losses or ties. On NEON, macOS/GCC retains two losses and one tie, and macOS/Clang six losses and one tie, both on virtualized M1 instances; the largest are macOS/Clang's String POD-type test (−20.3%) and macOS/GCC's minified Marine IK (−9.9%, not converged). No test loses on both NEON builds. On AVX-512 (§11), Linux/GCC retains one loss, the String POD-type test (−4.3%), and Windows/MSVC and Linux/Clang have no losses or ties. Each is an open item.

Additionally, the conformance evidence in §9 covers RFC 8259 document-level validity as exercised by the standard fail/pass corpus plus Jsonifier's extended suites; it is not a claim of byte-identical error *positions* with any other parser, and error-reporting granularity (Jsonifier reports global index, line, and local index with source context) is a feature comparison outside this paper's scope.

---

## 13. Conclusion

Six divergences separate Jsonifier's stage 1 from its simdjson ancestry: a batched drain that hands the out-of-order scheduler independent work instead of a hand-built pipeline; a compress-based AVX-512 index extraction operating as the batched architecture's native drain rather than as a kernel override retrofitted into an interleaved one; step geometry tuned per compiler on the position that the toolchain is part of the microarchitecture; UTF-8 validation eliminated from stage 1 entirely, fused register-by-register into the string-unescaping loop itself, and carried across the loop's SIMD width cascade through a three-byte validation state, riding loads the loop already performs at every width it occupies; a compile-time minified specialization that deletes whitespace classification when the caller knows it is unnecessary; and tape accounting built on precomputed counts. A seventh finding, the placement of control-character validation in stage 2 rather than stage 1 as ratified by the full conformance matrix, yields the general principle that validation placement should follow byte-access patterns rather than precedent.

On the AVX2/NEON sweep the design records 101-2-12 against simdjson (on-demand) across 115 tests on five platform/compiler combinations, with perfect 23-0-0 sweeps under MSVC and Clang on the same x86 host. On three AVX-512 combinations sharing one AMD EPYC 9V45, where the compress drain of §4 is measured on real silicon in the October 24, 2026 sweep, the record is 68-0-1 across 69 tests, with perfect 23-0-0 sweeps under MSVC and Clang; the one loss is GCC's String POD-type test, by under 5%. Both sweeps ran an 8-block (512 B) step on every target. Every structural position both libraries emit matches exactly across the suite on eight builds, three of them AVX-512. The residual losses and ties localize to specific toolchains and content shapes rather than to the architecture. None of these divergences individually is a revolution; stage 1 has looked "solved" since 2019 precisely because the foundational algorithms are excellent. The companion paper [5] carries the same approach through the rest of the pipeline.

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