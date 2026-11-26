/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/add_tape_values.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/simd/sve2_stage1.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>

namespace jsonifier::internal {

	template<typename integer_sequence_type> struct add_tape_values;

	template<uint64_t arraySize> static constexpr bool supportedLaneCount{ arraySize == 1 || arraySize == 2 || arraySize == 4 || arraySize == 8 };

	template<uint64_t arraySize> JSONIFIER_INLINE array<uint64_t, arraySize> laneOffsets(const array<uint64_t, arraySize>& __restrict cnts) {
		static_assert(supportedLaneCount<arraySize>, "laneOffsets supports 1, 2, 4, or 8 lanes.");
		array<uint64_t, arraySize> offsets{};
		if constexpr (arraySize == 2) {
			offsets[1] = cnts[0];
		} else if constexpr (arraySize == 4) {
			const uint64_t c01{ cnts[0] + cnts[1] };
			offsets[1] = cnts[0];
			offsets[2] = c01;
			offsets[3] = c01 + cnts[2];
		} else if constexpr (arraySize == 8) {
			const uint64_t c01{ cnts[0] + cnts[1] };
			const uint64_t c23{ cnts[2] + cnts[3] };
			const uint64_t c45{ cnts[4] + cnts[5] };
			const uint64_t c03{ c01 + c23 };
			offsets[1] = cnts[0];
			offsets[2] = c01;
			offsets[3] = c01 + cnts[2];
			offsets[4] = c03;
			offsets[5] = c03 + cnts[4];
			offsets[6] = c03 + c45;
			offsets[7] = c03 + c45 + cnts[6];
		}
		return offsets;
	}

#if JSONIFIER_AVX512

	// Sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
	template<uint64_t... indices> struct add_tape_values<integer_sequence<indices...>> {
		using size_type = uint64_t;
		static constexpr uint64_t blocksPerStep{ sizeof...(indices) };

		JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& __restrict bitsArr, const array<uint64_t, blocksPerStep>& __restrict cnts,
			write_structural_index_ptr tape, size_type strIdx) noexcept {
			const array<uint64_t, blocksPerStep> offsets{ laneOffsets(cnts) };
			const __m512i compressionMask{ _mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, 0x37363534, 0x33323130, 0x2f2e2d2c, 0x2b2a2928, 0x27262524, 0x23222120, 0x1f1e1d1c, 0x1b1a1918,
				0x17161514, 0x13121110, 0x0f0e0d0c, 0x0b0a0908, 0x07060504, 0x03020100) };
			const __m512i strIdxVec{ _mm512_set1_epi32(static_cast<int32_t>(strIdx)) };
			(drainLane<indices>(compressionMask, strIdxVec, bitsArr, cnts, tape + offsets[tag<indices>{}]), ...);
		}

	#if JSONIFIER_HAS_AVX512VBMI
		template<int32_t tier> JSONIFIER_INLINE static __m512i widenTier(const __m512i indexes) noexcept {
			static constexpr int32_t base{ tier * 16 };
			static constexpr __mmask64 byteToDwordMask{ 0x1111111111111111ull };
			const __m512i permutation{ _mm512_setr_epi32(base, base + 1, base + 2, base + 3, base + 4, base + 5, base + 6, base + 7, base + 8, base + 9, base + 10, base + 11,
				base + 12, base + 13, base + 14, base + 15) };
			return _mm512_maskz_permutexvar_epi8(byteToDwordMask, permutation, indexes);
		}
	#else
		template<int32_t tier> JSONIFIER_INLINE static __m512i widenTier(const __m512i indexes) noexcept {
			return _mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(indexes, tier));
		}
	#endif

		template<uint64_t index> JSONIFIER_INLINE static void drainLane(const __m512i compressionMask, const __m512i strIdxVec,
			const array<uint64_t, blocksPerStep>& __restrict bitsArr, const array<uint64_t, blocksPerStep>& __restrict cnts, write_structural_index_ptr tape) noexcept {
			uint64_t bits{ bitsArr[tag<index>{}] };
			const uint64_t count{ cnts[tag<index>{}] };
			static constexpr int32_t bitTotal{ static_cast<int32_t>(index * 64ull) };
			const __m512i indexes{ _mm512_maskz_compress_epi8(bits, compressionMask) };
			const __m512i startIndexLocal{ _mm512_add_epi32(strIdxVec, _mm512_set1_epi32(bitTotal)) };
			__m512i t0{ _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes)) };
			_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
			if (count > 16) [[unlikely]] {
				const __m512i t1{ widenTier<1>(indexes) };
				_mm512_storeu_si512(tape + 16, _mm512_add_epi32(t1, startIndexLocal));
				if (count > 32) {
					const __m512i t2{ widenTier<2>(indexes) };
					_mm512_storeu_si512(tape + 32, _mm512_add_epi32(t2, startIndexLocal));
					if (count > 48) {
						const __m512i t3{ widenTier<3>(indexes) };
						_mm512_storeu_si512(tape + 48, _mm512_add_epi32(t3, startIndexLocal));
					}
				}
			}
		}
	};

#else

	template<auto...> struct write_indices_functor {
		using size_type = uint64_t;

		template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& __restrict bits, write_structural_index_ptr tape) noexcept {
			{
				tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
				bits									  = simd::tape_writer_op::advance(bits);
			}
		}
	};

	template<uint64_t step> struct write_indices_stepped_functor {
		using size_type = uint64_t;

		template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& __restrict bits, write_structural_index_ptr tape, uint64_t cnt) noexcept {
			if constexpr (index > 0) {
				if ((index < cnt)) [[unlikely]] {
					{
						functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
					}
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

	template<uint64_t... indices> struct add_tape_values<integer_sequence<indices...>> {
		using size_type = uint64_t;
		static constexpr uint64_t blocksPerStep{ sizeof...(indices) };

		JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& __restrict bitsArr, const array<uint64_t, blocksPerStep>& __restrict cnts,
			write_structural_index_ptr tape, size_type strIdx) noexcept {
			const array<uint64_t, blocksPerStep> offsets{ laneOffsets(cnts) };
			(drainLane<indices>(bitsArr, cnts, tape + offsets[tag<indices>{}], strIdx), ...);
		}

		template<uint64_t index> JSONIFIER_INLINE static void drainLane(const array<uint64_t, blocksPerStep>& __restrict bitsArr,
			const array<uint64_t, blocksPerStep>& __restrict cnts, write_structural_index_ptr tape, size_type strIdx) noexcept {
			uint64_t bits{ bitsArr[tag<index>{}] };
			const uint64_t cnt{ cnts[tag<index>{}] };
			static constexpr size_type bitTotal{ tag<index>{} * 64ull };
			const size_type base{ bitTotal + strIdx };
			functor_runner<write_indices_stepped_functor, make_stepped_range_sequence<0, 64, simdTapeStep>, simdTapeStep>::implAnd(base, bits, tape, cnt);
		}
	};

#endif

}
