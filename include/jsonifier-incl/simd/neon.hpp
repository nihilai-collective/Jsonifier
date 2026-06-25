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

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcnt(value_type value) noexcept {
		return tzcnt(value) >> 2;
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcntUnsafe(value_type value) noexcept {
		return tzcntUnsafe(value) >> 2;
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* str) noexcept {
		return vld1q_u8(static_cast<const uint8_t*>(str));
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* str) noexcept {
		return vld1q_u8(static_cast<const uint8_t*>(str));
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(char_t str) noexcept {
		return vdupq_n_u64(str);
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(char_t str) noexcept {
		return vdupq_n_u8(static_cast<uint8_t>(str));
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* storageLocation) noexcept {
		vst1q_u8(static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* storageLocation) noexcept {
		vst1q_u8(static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(value, other)), 4)), 0);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(vcgtq_u8(other, value)), 4)), 0);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vceqq_u8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vcgtq_u8(other, value);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint64_t opBitMaskRaw(simd_int_t01 value) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(value), 4)), 0);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint16_t opBitMask(simd_int_t01 value) noexcept {
		constexpr uint8x16_t bit_mask{ 0x01, 0x02, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80 };
		const auto minput = vandq_u8(value, bit_mask);
		uint8x16_t tmp	  = vpaddq_u8(minput, minput);
		tmp				  = vpaddq_u8(tmp, tmp);
		tmp				  = vpaddq_u8(tmp, tmp);
		return vgetq_lane_u16(vreinterpretq_u16_u8(tmp), 0);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		const auto bitMask{ vdupq_n_u8(0x0F) };
		return vqtbl1q_u8(value, vandq_u8(other, bitMask));
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_128 opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return veorq_u8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_128 opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vandq_u8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_128 opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vorrq_u8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_128 opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vbicq_u8(value, other);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool opTest(simd_int_t01 value) noexcept {
		return vmaxvq_u8(value) == 0;
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opNot(simd_int_t01 value) noexcept {
		return vmvnq_u8(value);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static jsonifier_simd_int_128 opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return vqsubq_u8(value, other);
	}

	template<int32_t alignment, simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opSrLi(simd_int_t01 value) noexcept {
		return vshrq_n_u8(value, alignment);
	}

	template<int32_t N, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return vextq_u8(previous, current, N);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool anyBitsSetAnywhere(simd_int_t01 value) noexcept {
		return !opTest(value);
	}

	template<simd_int_128_type simd_int_t> JSONIFIER_INLINE bool isAscii(simd_int_t input) {
		return vmaxvq_u8(input) < 0x80u;
	}

	template<simd_int_128_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

#endif

}
