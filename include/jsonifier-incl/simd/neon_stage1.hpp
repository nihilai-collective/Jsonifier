// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/neon_stage1.hpp
// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/simd/neon.hpp>

namespace jsonifier::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)

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

	struct prefix_xor_op {
		JSONIFIER_INLINE static uint64_t impl(uint64_t bitmask) noexcept {
			bitmask ^= bitmask << 1;
			bitmask ^= bitmask << 2;
			bitmask ^= bitmask << 4;
			bitmask ^= bitmask << 8;
			bitmask ^= bitmask << 16;
			bitmask ^= bitmask << 32;
			return bitmask;
		}
	};

	struct unescaped_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01) noexcept {
			static constexpr uint8x16_t bitMask{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
			uint8x16_t sum0 = vpaddq_u8(vandq_u8(in_01.template get<0>(), bitMask), vandq_u8(in_01.template get<1>(), bitMask));
			uint8x16_t sum1 = vpaddq_u8(vandq_u8(in_01.template get<2>(), bitMask), vandq_u8(in_01.template get<3>(), bitMask));
			sum0			= vpaddq_u8(sum0, sum1);
			sum0			= vpaddq_u8(sum0, sum0);
			return vgetq_lane_u64(vreinterpretq_u64_u8(sum0), 0);
		}
	};

	struct ws_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
			const uint8x16_t d00	  = in_01.template get<0>();
			const uint8x16_t d01	  = in_01.template get<1>();
			const uint8x16_t d02	  = in_01.template get<2>();
			const uint8x16_t d03	  = in_01.template get<3>();
			const uint8x16_t matchWs0 = vqtbx1q_u8(vceqq_u8(d00, vdupq_n_u8(' ')), whitespaceTableLocal, d00);
			const uint8x16_t matchWs1 = vqtbx1q_u8(vceqq_u8(d01, vdupq_n_u8(' ')), whitespaceTableLocal, d01);
			const uint8x16_t matchWs2 = vqtbx1q_u8(vceqq_u8(d02, vdupq_n_u8(' ')), whitespaceTableLocal, d02);
			const uint8x16_t matchWs3 = vqtbx1q_u8(vceqq_u8(d03, vdupq_n_u8(' ')), whitespaceTableLocal, d03);
			static constexpr uint8x16_t bitMask{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
			uint8x16_t wsSum0 = vpaddq_u8(vandq_u8(matchWs0, bitMask), vandq_u8(matchWs1, bitMask));
			uint8x16_t wsSum1 = vpaddq_u8(vandq_u8(matchWs2, bitMask), vandq_u8(matchWs3, bitMask));
			wsSum0			  = vpaddq_u8(wsSum0, wsSum1);
			wsSum0			  = vpaddq_u8(wsSum0, wsSum0);
			return vgetq_lane_u64(vreinterpretq_u64_u8(wsSum0), 0);
		}
	};

	struct op_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t) noexcept {
			const uint8x16_t d00	  = in_01.template get<0>();
			const uint8x16_t d01	  = in_01.template get<1>();
			const uint8x16_t d02	  = in_01.template get<2>();
			const uint8x16_t d03	  = in_01.template get<3>();
			const uint8x16_t matchOp0 = vceqq_u8(vqtbl1q_u8(opTable, vshrq_n_u8(vaddq_u8(d00, vdupq_n_u8(3)), 4)), d00);
			const uint8x16_t matchOp1 = vceqq_u8(vqtbl1q_u8(opTable, vshrq_n_u8(vaddq_u8(d01, vdupq_n_u8(3)), 4)), d01);
			const uint8x16_t matchOp2 = vceqq_u8(vqtbl1q_u8(opTable, vshrq_n_u8(vaddq_u8(d02, vdupq_n_u8(3)), 4)), d02);
			const uint8x16_t matchOp3 = vceqq_u8(vqtbl1q_u8(opTable, vshrq_n_u8(vaddq_u8(d03, vdupq_n_u8(3)), 4)), d03);
			static constexpr uint8x16_t bitMask{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
			uint8x16_t opSum0 = vpaddq_u8(vandq_u8(matchOp0, bitMask), vandq_u8(matchOp1, bitMask));
			uint8x16_t opSum1 = vpaddq_u8(vandq_u8(matchOp2, bitMask), vandq_u8(matchOp3, bitMask));
			opSum0			  = vpaddq_u8(opSum0, opSum1);
			opSum0			  = vpaddq_u8(opSum0, opSum0);
			return vgetq_lane_u64(vreinterpretq_u64_u8(opSum0), 0);
		}
	};

	template<typename rope_block> struct rope_detector : rope_block {
		uint64_t nextIsEscaped{};
		uint64_t prevInString{};
		uint64_t prevScalar{};

		JSONIFIER_INLINE static uint64_t toBitmask(const uint8x16_t match_0, const uint8x16_t match_1, const uint8x16_t match_2, const uint8x16_t match_3,
			const uint8x16_t bitMask) noexcept {
			uint8x16_t sum0 = vpaddq_u8(vandq_u8(match_0, bitMask), vandq_u8(match_1, bitMask));
			uint8x16_t sum1 = vpaddq_u8(vandq_u8(match_2, bitMask), vandq_u8(match_3, bitMask));
			sum0			= vpaddq_u8(sum0, sum1);
			sum0			= vpaddq_u8(sum0, sum0);
			return vgetq_lane_u64(vreinterpretq_u64_u8(sum0), 0);
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
			static constexpr uint8x16_t bitMask{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
			const uint8x16_t d0			  = in_01.template get<0>();
			const uint8x16_t d1			  = in_01.template get<1>();
			const uint8x16_t d2			  = in_01.template get<2>();
			const uint8x16_t d3			  = in_01.template get<3>();
			const uint64_t backslashLocal = toBitmask(vceqq_u8(d0, bsRegister), vceqq_u8(d1, bsRegister), vceqq_u8(d2, bsRegister), vceqq_u8(d3, bsRegister), bitMask);
			const uint64_t quotesLocal	  = toBitmask(vceqq_u8(d0, quoteRegister), vceqq_u8(d1, quoteRegister), vceqq_u8(d2, quoteRegister), vceqq_u8(d3, quoteRegister), bitMask);
			const uint64_t escaped		  = nextEscapeAndTerminalCode(backslashLocal);
			const uint64_t quotes		  = (quotesLocal & ~escaped);
			rope_block::escaped			  = escaped;
			rope_block::quotes			  = quotes;
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
			return static_cast<uint32_t>(simd::tzcnt(bits) + base);
		}

		JSONIFIER_INLINE static uint64_t advance(const uint64_t bits) noexcept {
			return blsr(bits);
		}

		JSONIFIER_INLINE static uint64_t correctedPopcount(const uint64_t bits) noexcept {
			return static_cast<uint64_t>(popcnt(bits));
		}
	};

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateWhitespaceArrayNeon() {
		constexpr const uint8_t values[]{ 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0xFFu, 0x00u, 0x00u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> whitespaceArray{ generateWhitespaceArrayNeon<size>() };

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateOpArrayNeon() {
		constexpr const uint8_t values[]{ 0xFFu, 0x00u, ',', ':', 0x00u, '[', ']', '{', '}', 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> opArray{ generateOpArrayNeon<size>() };

#endif

}
