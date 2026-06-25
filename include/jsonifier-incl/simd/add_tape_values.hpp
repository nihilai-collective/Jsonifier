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
/// https://github.com/nihilai-collective/Jsonifier

#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/simd/sve2_stage1.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>

namespace jsonifier::internal {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)

	/// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
	template<typename integer_sequence_type> struct add_tape_values;

	template<uint64_t... indices> struct add_tape_values<integer_sequence<indices...>> {
		using size_type = uint64_t;
		static constexpr uint64_t blocksPerStep{ sizeof...(indices) };

		template<uint64_t index> JSONIFIER_INLINE static void drainLane(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts,
			structural_index_ptr tape, size_type strIdx) noexcept {
			uint64_t bits = bitsArr[tag<index>{}];
			if (!bits) [[unlikely]] {
				return;
			}
			const uint64_t count = cnts[tag<index>{}];
			static constexpr size_type bitTotal{ index * 64ull };
			const int32_t base			  = static_cast<int32_t>(bitTotal + strIdx);
			const __m512i indexes		  = _mm512_maskz_compress_epi8(bits,
						_mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, 0x37363534, 0x33323130, 0x2f2e2d2c, 0x2b2a2928, 0x27262524, 0x23222120, 0x1f1e1d1c, 0x1b1a1918, 0x17161514, 0x13121110,
							0x0f0e0d0c, 0x0b0a0908, 0x07060504, 0x03020100));
			const __m512i startIndexLocal = _mm512_set1_epi32(base);
			__m512i t0					  = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes));
			_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
			if (count > 16) {
				const __m512i t1 = _mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(indexes, 1));
				_mm512_storeu_si512(tape + 16, _mm512_add_epi32(t1, startIndexLocal));
				if (count > 32) {
					const __m512i t2 = _mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(indexes, 2));
					_mm512_storeu_si512(tape + 32, _mm512_add_epi32(t2, startIndexLocal));
					if (count > 48) {
						const __m512i t3 = _mm512_cvtepu8_epi32(_mm512_extracti32x4_epi32(indexes, 3));
						_mm512_storeu_si512(tape + 48, _mm512_add_epi32(t3, startIndexLocal));
					}
				}
			}
		}

		JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, structural_index_ptr tape,
			size_type strIdx) noexcept {
			uint64_t offset = 0;
			(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[indices]), ...);
		}
	};

#else

	template<auto...> struct write_indices_functor {
		using size_type = uint64_t;

		template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& bits, structural_index_ptr tape) noexcept {
			{
				tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
				bits									  = simd::tape_writer_op::advance(bits);
			}
		}
	};

	template<uint64_t step> struct write_indices_stepped_functor {
		using size_type = uint64_t;
		template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& bits, structural_index_ptr tape, uint64_t cnt) noexcept {
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

	template<typename integer_sequence_type> struct add_tape_values;

	template<uint64_t... indices> struct add_tape_values<integer_sequence<indices...>> {
		using size_type = uint64_t;

		template<uint64_t index> JSONIFIER_INLINE static void drainLane(const array<uint64_t, simdBlocksPerStep>& bitsArr, const array<uint64_t, simdBlocksPerStep>& cnts,
			structural_index_ptr tape, size_type strIdx) noexcept {
			uint64_t bits	   = bitsArr[tag<index>{}];
			const uint64_t cnt = cnts[tag<index>{}];
			static constexpr size_type bitTotal{ tag<index>{} * 64ull };
			const size_type base = bitTotal + strIdx;
			functor_runner<write_indices_stepped_functor, make_stepped_range_sequence<0, 64, simdTapeStep>, simdTapeStep>::implAnd(base, bits, tape, cnt);
		}

		JSONIFIER_INLINE static void impl(const array<uint64_t, simdBlocksPerStep>& bitsArr, const array<uint64_t, simdBlocksPerStep>& cnts, structural_index_ptr tape,
			size_type strIdx) noexcept {
			uint64_t offset = 0;
			(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
		}
	};


#endif

}
