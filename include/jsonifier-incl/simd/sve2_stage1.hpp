/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
/// https://github.com/nihilai-collective/Jsonifier
#pragma once

#include <jsonifier-incl/simd/neon.hpp>

namespace jsonifier::simd {

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

	JSONIFIER_INLINE static jsonifier_simd_int_t sve2BitMask() noexcept {
		return svld1_u8(svptrue_b8(), sve2BitMaskPattern);
	}

	JSONIFIER_INLINE static jsonifier_simd_int_t sve2Vpaddq(const jsonifier_simd_int_t a, const jsonifier_simd_int_t b) noexcept {
		return svadd_u8_x(svptrue_b8(), svuzp1_u8(a, b), svuzp2_u8(a, b));
	}

	JSONIFIER_INLINE static uint64_t sve2CollapseToBitmask(const jsonifier_simd_int_t match_0, const jsonifier_simd_int_t match_1, const jsonifier_simd_int_t match_2,
		const jsonifier_simd_int_t match_3, const jsonifier_simd_int_t bitMask) noexcept {
		const auto pg	= svptrue_b8();
		auto sum0		= sve2Vpaddq(svand_u8_x(pg, match_0, bitMask), svand_u8_x(pg, match_1, bitMask));
		const auto sum1 = sve2Vpaddq(svand_u8_x(pg, match_2, bitMask), svand_u8_x(pg, match_3, bitMask));
		sum0			= sve2Vpaddq(sum0, sum1);
		sum0			= sve2Vpaddq(sum0, sum0);
		return svlastb_u64(svptrue_pat_b64(SV_VL1), svreinterpret_u64_u8(sum0));
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
			return sve2CollapseToBitmask(in_01.template get<0>(), in_01.template get<1>(), in_01.template get<2>(), in_01.template get<3>(), sve2BitMask());
		}
	};

	struct ws_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
			const auto pg						= svptrue_b8();
			const auto space					= svdup_n_u8(' ');
			const jsonifier_simd_int_t d00		= in_01.template get<0>();
			const jsonifier_simd_int_t d01		= in_01.template get<1>();
			const jsonifier_simd_int_t d02		= in_01.template get<2>();
			const jsonifier_simd_int_t d03		= in_01.template get<3>();
			const jsonifier_simd_int_t matchWs0 = svtbx_u8(svsel_u8(svcmpeq_u8(pg, d00, space), svdup_n_u8(0xFF), svdup_n_u8(0x00)), whitespaceTableLocal, d00);
			const jsonifier_simd_int_t matchWs1 = svtbx_u8(svsel_u8(svcmpeq_u8(pg, d01, space), svdup_n_u8(0xFF), svdup_n_u8(0x00)), whitespaceTableLocal, d01);
			const jsonifier_simd_int_t matchWs2 = svtbx_u8(svsel_u8(svcmpeq_u8(pg, d02, space), svdup_n_u8(0xFF), svdup_n_u8(0x00)), whitespaceTableLocal, d02);
			const jsonifier_simd_int_t matchWs3 = svtbx_u8(svsel_u8(svcmpeq_u8(pg, d03, space), svdup_n_u8(0xFF), svdup_n_u8(0x00)), whitespaceTableLocal, d03);
			return sve2CollapseToBitmask(matchWs0, matchWs1, matchWs2, matchWs3, sve2BitMask());
		}
	};

	struct op_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t) noexcept {
			const auto pg						= svptrue_b8();
			const auto three					= svdup_n_u8(3);
			const jsonifier_simd_int_t d00		= in_01.template get<0>();
			const jsonifier_simd_int_t d01		= in_01.template get<1>();
			const jsonifier_simd_int_t d02		= in_01.template get<2>();
			const jsonifier_simd_int_t d03		= in_01.template get<3>();
			const auto idx0						= svlsr_n_u8_x(pg, svadd_u8_x(pg, d00, three), 4);
			const auto idx1						= svlsr_n_u8_x(pg, svadd_u8_x(pg, d01, three), 4);
			const auto idx2						= svlsr_n_u8_x(pg, svadd_u8_x(pg, d02, three), 4);
			const auto idx3						= svlsr_n_u8_x(pg, svadd_u8_x(pg, d03, three), 4);
			const auto zeroes					= svdup_n_u8(0x00);
			const auto ones						= svdup_n_u8(0xFF);
			const jsonifier_simd_int_t matchOp0 = svsel_u8(svcmpeq_u8(pg, svtbl_u8(opTable, idx0), d00), ones, zeroes);
			const jsonifier_simd_int_t matchOp1 = svsel_u8(svcmpeq_u8(pg, svtbl_u8(opTable, idx1), d01), ones, zeroes);
			const jsonifier_simd_int_t matchOp2 = svsel_u8(svcmpeq_u8(pg, svtbl_u8(opTable, idx2), d02), ones, zeroes);
			const jsonifier_simd_int_t matchOp3 = svsel_u8(svcmpeq_u8(pg, svtbl_u8(opTable, idx3), d03), ones, zeroes);
			return sve2CollapseToBitmask(matchOp0, matchOp1, matchOp2, matchOp3, sve2BitMask());
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

		JSONIFIER_INLINE static uint64_t toBitmask(const jsonifier_simd_int_t match_0, const jsonifier_simd_int_t match_1, const jsonifier_simd_int_t match_2,
			const jsonifier_simd_int_t match_3, const jsonifier_simd_int_t bitMask) noexcept {
			return sve2CollapseToBitmask(match_0, match_1, match_2, match_3, bitMask);
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
			const auto zeroes			  = svdup_n_u8(0x00);
			const auto ones				  = svdup_n_u8(0xFF);
			const auto bitMask			  = sve2BitMask();
			const jsonifier_simd_int_t d0 = in_01.template get<0>();
			const jsonifier_simd_int_t d1 = in_01.template get<1>();
			const jsonifier_simd_int_t d2 = in_01.template get<2>();
			const jsonifier_simd_int_t d3 = in_01.template get<3>();
			const uint64_t backslashLocal = toBitmask(svsel_u8(svcmpeq_u8(pg, d0, bsRegister), ones, zeroes), svsel_u8(svcmpeq_u8(pg, d1, bsRegister), ones, zeroes),
				svsel_u8(svcmpeq_u8(pg, d2, bsRegister), ones, zeroes), svsel_u8(svcmpeq_u8(pg, d3, bsRegister), ones, zeroes), bitMask);
			const uint64_t quotesLocal	  = toBitmask(svsel_u8(svcmpeq_u8(pg, d0, quoteRegister), ones, zeroes), svsel_u8(svcmpeq_u8(pg, d1, quoteRegister), ones, zeroes),
				   svsel_u8(svcmpeq_u8(pg, d2, quoteRegister), ones, zeroes), svsel_u8(svcmpeq_u8(pg, d3, quoteRegister), ones, zeroes), bitMask);
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
