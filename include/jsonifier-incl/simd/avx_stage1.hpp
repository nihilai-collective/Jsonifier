// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/avx_stage1.hpp
// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/containers/array.hpp>
#include <jsonifier-incl/simd/bit_ops.hpp>
#include <jsonifier-incl/simd/avx.hpp>

namespace jsonifier::internal::simd {

#if !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON) && !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

	static constexpr internal::array<uint64_t, registersPerBlock> shiftAmounts{ [] {
		internal::array<uint64_t, registersPerBlock> returnValue{};
		for (uint64_t x = 0; x < registersPerBlock; ++x) {
			returnValue[x] = simdBytesPerRegister * x;
		}
		return returnValue;
	}() };

	inline static consteval uint64_t getShiftAmount(const uint64_t index) noexcept {
		return shiftAmounts[index % shiftAmounts.size()];
	}

	struct prefix_xor_op {
		JSONIFIER_INLINE static uint64_t impl(uint64_t bitmask) noexcept {
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_CLMUL)
			const __m128i all_ones = _mm_set1_epi8(static_cast<char>(-1));
			const __m128i result   = _mm_clmulepi64_si128(_mm_set_epi64x(0, static_cast<int64_t>(bitmask)), all_ones, 0);
			return static_cast<uint64_t>(_mm_cvtsi128_si64(result));
	#else
			bitmask ^= bitmask << 1;
			bitmask ^= bitmask << 2;
			bitmask ^= bitmask << 4;
			bitmask ^= bitmask << 8;
			bitmask ^= bitmask << 16;
			bitmask ^= bitmask << 32;
			return bitmask;
	#endif
		}
	};

	struct cmp_eq_op {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t lhs, const jsonifier_simd_int_t rhsBroadcast) noexcept {
			uint64_t result = simd::opCmpEq(lhs.get<0>(), rhsBroadcast);
			if constexpr (registersPerBlock > 1) {
				result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<1>(), rhsBroadcast)) << getShiftAmount(1);
				if constexpr (registersPerBlock > 2) {
					result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<2>(), rhsBroadcast)) << getShiftAmount(2);
					result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<3>(), rhsBroadcast)) << getShiftAmount(3);
				}
			}
			return result;
		}

		JSONIFIER_INLINE static uint64_t impl(const simd_array_t lhs, const simd_array_t rhs) noexcept {
			uint64_t result = simd::opCmpEq(lhs.get<0>(), rhs.get<0>());
			if constexpr (registersPerBlock > 1) {
				result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<1>(), rhs.get<1>())) << getShiftAmount(1);
				if constexpr (registersPerBlock > 2) {
					result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<2>(), rhs.get<2>())) << getShiftAmount(2);
					result |= static_cast<uint64_t>(simd::opCmpEq(lhs.get<3>(), rhs.get<3>())) << getShiftAmount(3);
				}
			}
			return result;
		}
	};

	struct unescaped_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t ltRhs) noexcept {
			uint64_t result = static_cast<uint64_t>(opBitMask(ltRhs.get<0>()));
			if constexpr (registersPerBlock > 1) {
				result |= static_cast<uint64_t>(opBitMask(ltRhs.get<1>())) << getShiftAmount(1);
				if constexpr (registersPerBlock > 2) {
					result |= static_cast<uint64_t>(opBitMask(ltRhs.get<2>())) << getShiftAmount(2);
					result |= static_cast<uint64_t>(opBitMask(ltRhs.get<3>())) << getShiftAmount(3);
				}
			}
			return result;
		}
	};

	template<typename rope_block> struct rope_detector : rope_block {
		uint64_t nextIsEscaped{};
		uint64_t prevInString{};
		uint64_t prevScalar{};

		JSONIFIER_INLINE void finishNextNoInString() noexcept {
			rope_block::inString = prevInString;
		}

		JSONIFIER_INLINE void finishNextInString() noexcept {
			const uint64_t inString = simd::prefix_xor_op::impl(rope_block::quotes) ^ prevInString;
			prevInString			= static_cast<uint64_t>(static_cast<int64_t>(inString) >> 63);
			rope_block::inString	= inString;
		}

		JSONIFIER_INLINE void next(const simd_array_t in_01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
			const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
			const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
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

	struct ws_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
			simd_array_t wsShuffle;
			wsShuffle.set<0>(simd::opShuffle(whitespaceTableLocal, in_01.get<0>()));
			if constexpr (registersPerBlock > 1) {
				wsShuffle.set<1>(simd::opShuffle(whitespaceTableLocal, in_01.get<1>()));
				if constexpr (registersPerBlock > 2) {
					wsShuffle.set<2>(simd::opShuffle(whitespaceTableLocal, in_01.get<2>()));
					wsShuffle.set<3>(simd::opShuffle(whitespaceTableLocal, in_01.get<3>()));
				}
			}
			return cmp_eq_op::impl(in_01, wsShuffle);
		}
	};

	struct op_collector {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask) noexcept {
			simd_array_t orLhs;
			simd_array_t shuffleRhs;

			orLhs.set<0>(simd::opOr(in_01.get<0>(), spaceMask));
			shuffleRhs.set<0>(simd::opShuffle(opTable, in_01.get<0>()));
			if constexpr (registersPerBlock > 1) {
				orLhs.set<1>(simd::opOr(in_01.get<1>(), spaceMask));
				shuffleRhs.set<1>(simd::opShuffle(opTable, in_01.get<1>()));
				if constexpr (registersPerBlock > 2) {
					orLhs.set<2>(simd::opOr(in_01.get<2>(), spaceMask));
					shuffleRhs.set<2>(simd::opShuffle(opTable, in_01.get<2>()));
					orLhs.set<3>(simd::opOr(in_01.get<3>(), spaceMask));
					shuffleRhs.set<3>(simd::opShuffle(opTable, in_01.get<3>()));
				}
			}
			return cmp_eq_op::impl(orLhs, shuffleRhs);
		}
	};

	struct tape_writer_op {
		JSONIFIER_INLINE static uint32_t extractIndex(const uint64_t base, const uint64_t bits) noexcept {
			return static_cast<uint32_t>(simd::postCmpTzcnt(bits) + base);
		}

		JSONIFIER_INLINE static uint64_t advance(const uint64_t bits) noexcept {
			return blsr(bits);
		}

		JSONIFIER_INLINE static uint64_t correctedPopcount(const uint64_t bits) noexcept {
			return static_cast<uint64_t>(popCount(bits));
		}
	};

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateWhitespaceArray() noexcept {
		constexpr const uint8_t values[]{ 0x20u, 0x64u, 0x64u, 0x64u, 0x11u, 0x64u, 0x71u, 0x02u, 0x64u, '\t', '\n', 0x70u, 0x64u, '\r', 0x64u, 0x64u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> whitespaceArray{ generateWhitespaceArray<size>() };

	template<uint64_t size> static constexpr internal::array<uint8_t, size> generateOpArray() noexcept {
		constexpr const uint8_t values[]{ 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, ':', '{', ',', '}', 0x00u, 0x00u };
		internal::array<uint8_t, size> returnValues{};
		for (uint64_t x = 0; x < size; ++x) {
			returnValues[x] = values[x % 16];
		}
		return returnValues;
	};

	template<uint64_t size> alignas(64) static constexpr internal::array<uint8_t, size> opArray{ generateOpArray<size>() };

#endif

}
