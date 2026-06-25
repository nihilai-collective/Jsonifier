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

#include <jsonifier-incl/simd/simd_types.hpp>
#include <jsonifier-incl/simd/bmi.hpp>

namespace jsonifier::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcnt(value_type value) noexcept {
		return tzcnt(value) >> 2;
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcntUnsafe(value_type value) noexcept {
		return tzcntUnsafe(value) >> 2;
	}

	template<simd_int_sve2_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* str) noexcept {
		return svld1_u8(svptrue_b8(), static_cast<const uint8_t*>(str));
	}

	template<simd_int_sve2_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* str) noexcept {
		return svld1_u8(svptrue_b8(), static_cast<const uint8_t*>(str));
	}

	template<simd_int_sve2_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(char_t str) noexcept {
		return svreinterpret_u8_u64(svdup_n_u64(static_cast<uint64_t>(str)));
	}

	template<simd_int_sve2_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(char_t str) noexcept {
		return svdup_n_u8(static_cast<uint8_t>(str));
	}

	template<simd_int_sve2_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* storageLocation) noexcept {
		svst1_u8(svptrue_b8(), static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_sve2_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* storageLocation) noexcept {
		svst1_u8(svptrue_b8(), static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svsel_u8(svcmpeq_u8(svptrue_b8(), value, other), svdup_n_u8(0xFF), svdup_n_u8(0x00));
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svsel_u8(svcmplt_u8(svptrue_b8(), value, other), svdup_n_u8(0xFF), svdup_n_u8(0x00));
	}

	template<simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint64_t opBitMaskRaw(simd_int_t01 value) noexcept {
		static_assert(JSONIFIER_SVE2_VECTOR_BITS == 128, "opBitMaskRaw's nibble-per-byte packing only fits a uint64_t at 128-bit VL.");
		const jsonifier_simd_int_t narrowed = svshrnb_n_u16(svreinterpret_u16_u8(value), 4);
		const jsonifier_simd_int_t packed	= svuzp1_u8(narrowed, narrowed);
		return svlastb_u64(svptrue_pat_b64(SV_VL1), svreinterpret_u64_u8(packed));
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static uint64_t opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return opBitMaskRaw(opCmpEqRaw(value, other));
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static uint64_t opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		return opBitMaskRaw(opCmpLtRaw(value, other));
	}

	template<simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint16_t opBitMask(simd_int_t01 value) noexcept {
		static_assert(JSONIFIER_SVE2_VECTOR_BITS == 128, "opBitMask returns uint16_t and therefore only covers a 128-bit vector.");
		constexpr uint8_t bit_mask_pattern[16]{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
		const jsonifier_simd_int_t bit_mask = svld1_u8(svptrue_b8(), bit_mask_pattern);
		const jsonifier_simd_int_t masked	= svand_u8_x(svptrue_b8(), value, bit_mask);
		const uint64_t lo					= svaddv_u8(svptrue_pat_b8(SV_VL8), masked);
		const uint64_t hi					= svaddv_u8(svnot_b_z(svptrue_b8(), svptrue_pat_b8(SV_VL8)), masked);
		return static_cast<uint16_t>(lo | (hi << 8));
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_t bitMask = svreinterpret_u8_u8(svdup_n_u8(0x0F));
		return svtbl_u8(value, svand_u8_x(svptrue_b8(), other, bitMask));
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return sveor_u8_x(svptrue_b8(), value, other);
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svand_u8_x(svptrue_b8(), value, other);
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svorr_u8_x(svptrue_b8(), value, other);
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svbic_u8_x(svptrue_b8(), value, other);
	}

	template<simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool opTest(simd_int_t01 value) noexcept {
		return svmaxv_u8(svptrue_b8(), value) == 0;
	}

	template<simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opNot(simd_int_t01 value) noexcept {
		return svnot_u8_x(svptrue_b8(), value);
	}

	template<simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return svqsub_u8_x(svptrue_b8(), value, other);
	}

	template<int32_t alignment, simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opSrLi(simd_int_t01 value) noexcept {
		return svlsr_n_u8_x(svptrue_b8(), value, alignment);
	}

	template<int32_t N, simd_int_sve2_type simd_int_t01, simd_int_sve2_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_t opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return svext_u8(previous, current, N);
	}

	template<simd_int_sve2_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool anyBitsSetAnywhere(simd_int_t01 value) noexcept {
		return !opTest(value);
	}

	template<simd_int_sve2_type simd_int_t> JSONIFIER_INLINE bool isAscii(simd_int_t input) {
		return svmaxv_u8(svptrue_b8(), input) < 0x80u;
	}

	template<simd_int_sve2_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

#endif

}
