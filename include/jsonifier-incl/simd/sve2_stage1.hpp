// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/sve2_stage1.hpp
// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/simd/neon.hpp>

namespace jsonifier::internal::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

	static_assert(JSONIFIER_SVE2_VECTOR_BITS == 128,
		"This block collapses registersPerBlock registers into a single uint64_t bitmask, and SVE2 ADDP is segment-wise; both hold only at 128-bit VL.");

	static constexpr internal::array<uint64_t, registersPerBlock> shiftAmounts{ [] {
		internal::array<uint64_t, registersPerBlock> returnValue{};
		for (uint64_t x = 0; x < registersPerBlock; ++x) {
			returnValue[x] = simdBytesPerRegister * x;
		}
		return returnValue;
	}() };

	inline static consteval uint64_t getShiftAmount(const uint64_t index) {
		return shiftAmounts[index % shiftAmounts.size()];
	}

	alignas(16) static constexpr uint8_t sve2BitMaskPattern[16]{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };

	struct prefix_xor_op {
		JSONIFIER_INLINE static uint64_t impl(uint64_t bitmask) noexcept {
			const poly64_t allOnes = static_cast<poly64_t>(~0ull);
			const poly128_t result = vmull_p64(static_cast<poly64_t>(bitmask), allOnes);
			return static_cast<uint64_t>(vgetq_lane_u64(vreinterpretq_u64_p128(result), 0));
		}
	};

	JSONIFIER_INLINE static jsonifier_simd_int_t sve2BitMask() noexcept {
		return svreinterpret_u8_u64(svdup_n_u64(0x8040201008040201ULL));
	}

	JSONIFIER_INLINE static uint64_t sve2CollapseMasked(const jsonifier_simd_int_t masked_0, const jsonifier_simd_int_t masked_1, const jsonifier_simd_int_t masked_2,
		const jsonifier_simd_int_t masked_3) noexcept {
		uint8x16_t sum0		  = vpaddq_u8(svget_neonq_u8(masked_0), svget_neonq_u8(masked_1));
		const uint8x16_t sum1 = vpaddq_u8(svget_neonq_u8(masked_2), svget_neonq_u8(masked_3));
		sum0				  = vpaddq_u8(sum0, sum1);
		sum0				  = vpaddq_u8(sum0, sum0);
		return vgetq_lane_u64(vreinterpretq_u64_u8(sum0), 0);
	}

	JSONIFIER_INLINE static uint64_t sve2CollapseToBitmask(const jsonifier_simd_int_t match_0, const jsonifier_simd_int_t match_1, const jsonifier_simd_int_t match_2,
		const jsonifier_simd_int_t match_3, const jsonifier_simd_int_t bitMask) noexcept {
		const auto pg = svptrue_b8();
		return sve2CollapseMasked(svand_u8_x(pg, match_0, bitMask), svand_u8_x(pg, match_1, bitMask), svand_u8_x(pg, match_2, bitMask), svand_u8_x(pg, match_3, bitMask));
	}

	JSONIFIER_INLINE static jsonifier_simd_int_t sve2WsChars() noexcept {
		alignas(16) static constexpr uint8_t wsPattern[16]{ ' ', '\t', '\n', '\r', ' ', '\t', '\n', '\r', ' ', '\t', '\n', '\r', ' ', '\t', '\n', '\r' };
		return svld1_u8(svptrue_b8(), wsPattern);
	}

	JSONIFIER_INLINE static jsonifier_simd_int_t sve2OpChars() noexcept {
		alignas(16) static constexpr uint8_t opPattern[16]{ ',', ':', '[', ']', '{', '}', ',', ':', '[', ']', '{', '}', ',', ':', '[', ']' };
		return svld1_u8(svptrue_b8(), opPattern);
	}

	struct ws_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t) noexcept {
			const auto pg	   = svptrue_b8();
			const auto bitMask = sve2BitMask();
			const auto wsChars = sve2WsChars();
			return sve2CollapseMasked(svand_u8_z(svmatch_u8(pg, in_01.template get<0>(), wsChars), bitMask, bitMask),
				svand_u8_z(svmatch_u8(pg, in_01.template get<1>(), wsChars), bitMask, bitMask), svand_u8_z(svmatch_u8(pg, in_01.template get<2>(), wsChars), bitMask, bitMask),
				svand_u8_z(svmatch_u8(pg, in_01.template get<3>(), wsChars), bitMask, bitMask));
		}
	};

	struct op_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t, const jsonifier_simd_int_t) noexcept {
			const auto pg	   = svptrue_b8();
			const auto bitMask = sve2BitMask();
			const auto opChars = sve2OpChars();
			return sve2CollapseMasked(svand_u8_z(svmatch_u8(pg, in_01.template get<0>(), opChars), bitMask, bitMask),
				svand_u8_z(svmatch_u8(pg, in_01.template get<1>(), opChars), bitMask, bitMask), svand_u8_z(svmatch_u8(pg, in_01.template get<2>(), opChars), bitMask, bitMask),
				svand_u8_z(svmatch_u8(pg, in_01.template get<3>(), opChars), bitMask, bitMask));
		}
	};

	struct unescaped_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01) noexcept {
			const auto pg	   = svptrue_b8();
			const auto bitMask = sve2BitMask();
			return sve2CollapseMasked(svand_u8_x(pg, in_01.template get<0>(), bitMask), svand_u8_x(pg, in_01.template get<1>(), bitMask),
				svand_u8_x(pg, in_01.template get<2>(), bitMask), svand_u8_x(pg, in_01.template get<3>(), bitMask));
		}
	};

	template<typename rope_block> struct rope_detector : rope_block {
		uint64_t nextIsEscaped{};
		uint64_t prevInString{};
		uint64_t prevScalar{};

		JSONIFIER_INLINE void reset() {
			nextIsEscaped = 0;
			prevInString  = 0;
			prevScalar	  = 0;
		}

		JSONIFIER_INLINE static uint64_t toBitmask(const jsonifier_simd_int_t masked_0, const jsonifier_simd_int_t masked_1, const jsonifier_simd_int_t masked_2,
			const jsonifier_simd_int_t masked_3) noexcept {
			return sve2CollapseMasked(masked_0, masked_1, masked_2, masked_3);
		}		

		JSONIFIER_INLINE void finishNextNoInString() noexcept {
			rope_block::inString = prevInString;
		}

		JSONIFIER_INLINE void finishNextInString() noexcept {
			const uint64_t inString = simd::prefix_xor_op::impl(rope_block::quotes) ^ prevInString;
			prevInString			= static_cast<uint64_t>(static_cast<int64_t>(inString) >> 63);
			rope_block::inString	= inString;
		}

		JSONIFIER_INLINE void next(const simd_array_t in_01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
			const auto pg				  = svptrue_b8();
			const auto bitMask			  = sve2BitMask();
			const jsonifier_simd_int_t d0 = in_01.template get<0>();
			const jsonifier_simd_int_t d1 = in_01.template get<1>();
			const jsonifier_simd_int_t d2 = in_01.template get<2>();
			const jsonifier_simd_int_t d3 = in_01.template get<3>();
			const auto eqBs0			  = svcmpeq_u8(pg, d0, bsRegister);
			const auto eqQ0				  = svcmpeq_u8(pg, d0, quoteRegister);
			const auto eqBs1			  = svcmpeq_u8(pg, d1, bsRegister);
			const auto eqQ1				  = svcmpeq_u8(pg, d1, quoteRegister);
			const auto eqBs2			  = svcmpeq_u8(pg, d2, bsRegister);
			const auto eqQ2				  = svcmpeq_u8(pg, d2, quoteRegister);
			const auto eqBs3			  = svcmpeq_u8(pg, d3, bsRegister);
			const auto eqQ3				  = svcmpeq_u8(pg, d3, quoteRegister);
			const uint64_t backslashLocal = sve2CollapseMasked(svand_u8_z(eqBs0, bitMask, bitMask), svand_u8_z(eqBs1, bitMask, bitMask), svand_u8_z(eqBs2, bitMask, bitMask),
				svand_u8_z(eqBs3, bitMask, bitMask));
			const uint64_t quotesLocal =
				sve2CollapseMasked(svand_u8_z(eqQ0, bitMask, bitMask), svand_u8_z(eqQ1, bitMask, bitMask), svand_u8_z(eqQ2, bitMask, bitMask), svand_u8_z(eqQ3, bitMask, bitMask));
			const uint64_t escaped = nextEscapeAndTerminalCode(backslashLocal);
			const uint64_t quotes  = (quotesLocal & ~escaped);
			rope_block::escaped	   = escaped;
			rope_block::quotes	   = quotes;
			return quotes ? finishNextInString() : finishNextNoInString();
		}

		JSONIFIER_INLINE uint64_t nextEscapeAndTerminalCodeImpl(const uint64_t potentialEscape) noexcept {
			static constexpr uint64_t oddBits{ 0xAAAAAAAAAAAAAAAAULL };
			const uint64_t maybeEscaped				 = potentialEscape << 1;
			const uint64_t maybeEscapedAndOddBits	 = maybeEscaped | oddBits;
			const uint64_t evenSeriesCodesAndOddBits = maybeEscapedAndOddBits - potentialEscape;
			return evenSeriesCodesAndOddBits ^ oddBits;
		}

		JSONIFIER_INLINE uint64_t nextEscapeAndTerminalCode(const uint64_t backslashLocal) noexcept {
			if (!backslashLocal) {
				const uint64_t escaped = nextIsEscaped;
				nextIsEscaped		   = 0;
				return escaped;
			}
			const uint64_t escapeAndTerminalCode = nextEscapeAndTerminalCodeImpl(backslashLocal & ~nextIsEscaped);
			const uint64_t escaped				 = escapeAndTerminalCode ^ (backslashLocal | nextIsEscaped);
			nextIsEscaped						 = (escapeAndTerminalCode & backslashLocal) >> 63;
			return escaped;
		}

		JSONIFIER_INLINE uint64_t followsNonquoteScalar(const uint64_t nonquoteScalar) noexcept {
			const uint64_t shifted = (nonquoteScalar << 1) | prevScalar;
			prevScalar			   = nonquoteScalar >> 63;
			return shifted;
		}
	};

	struct tape_writer_op {
		JSONIFIER_INLINE static uint32_t extractIndex(const uint64_t base, const uint64_t bits) noexcept {
			return static_cast<uint32_t>(simd::countrZero(bits) + base);
		}

		JSONIFIER_INLINE static uint64_t advance(const uint64_t bits) noexcept {
			return blsr(bits);
		}

		JSONIFIER_INLINE static uint64_t correctedPopcount(const uint64_t bits) noexcept {
			return static_cast<uint64_t>(popCount(bits));
		}
	};

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateWhitespaceArraySve2() {
		constexpr const uint8_t values[]{ 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0x00u, 0x00u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> whitespaceArray{ generateWhitespaceArraySve2<size>() };

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateOpArraySve2() {
		constexpr const uint8_t values[]{ 0xFFu, 0x00u, ',', ':', 0x00u, '[', ']', '{', '}', 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> opArray{ generateOpArraySve2<size>() };

#endif

}
