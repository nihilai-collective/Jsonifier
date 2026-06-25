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

#include <jsonifier-incl/simd/simd_x.hpp>
#include <jsonifier-incl/simd/simd_types.hpp>
#include <jsonifier-incl/simd/bmi.hpp>

namespace jsonifier::simd {

#if !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX) && !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) && !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) && \
	!JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON) && !JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcnt(const value_type value) noexcept {
		return tzcnt(value);
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcntUnsafe(const value_type value) noexcept {
		return tzcntUnsafe(value);
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* str) noexcept {
		simd_int_type_new result{};
		std::memcpy(&result.values, str, 16);
		return result;
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* str) noexcept {
		simd_int_type_new result{};
		std::memcpy(&result.values, str, 16);
		return result;
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t str) noexcept {
		simd_int_type_new result{};
		result.values.xUint64[0] = static_cast<uint64_t>(str);
		result.values.xUint64[1] = static_cast<uint64_t>(str);
		return result;
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t str) noexcept {
		simd_int_type_new result{};
		result.values.xUint8[0]	 = static_cast<uint8_t>(str);
		result.values.xUint8[1]	 = static_cast<uint8_t>(str);
		result.values.xUint8[2]	 = static_cast<uint8_t>(str);
		result.values.xUint8[3]	 = static_cast<uint8_t>(str);
		result.values.xUint8[4]	 = static_cast<uint8_t>(str);
		result.values.xUint8[5]	 = static_cast<uint8_t>(str);
		result.values.xUint8[6]	 = static_cast<uint8_t>(str);
		result.values.xUint8[7]	 = static_cast<uint8_t>(str);
		result.values.xUint8[8]	 = static_cast<uint8_t>(str);
		result.values.xUint8[9]	 = static_cast<uint8_t>(str);
		result.values.xUint8[10] = static_cast<uint8_t>(str);
		result.values.xUint8[11] = static_cast<uint8_t>(str);
		result.values.xUint8[12] = static_cast<uint8_t>(str);
		result.values.xUint8[13] = static_cast<uint8_t>(str);
		result.values.xUint8[14] = static_cast<uint8_t>(str);
		result.values.xUint8[15] = static_cast<uint8_t>(str);
		return result;
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* storageLocation) noexcept {
		std::memcpy(storageLocation, &value.values, 16);
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* storageLocation) noexcept {
		std::memcpy(storageLocation, &value.values, 16);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static uint16_t opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		uint16_t result{};
		result |= static_cast<uint16_t>(value.values.xUint8[0] == other.values.xUint8[0]) << 0;
		result |= static_cast<uint16_t>(value.values.xUint8[1] == other.values.xUint8[1]) << 1;
		result |= static_cast<uint16_t>(value.values.xUint8[2] == other.values.xUint8[2]) << 2;
		result |= static_cast<uint16_t>(value.values.xUint8[3] == other.values.xUint8[3]) << 3;
		result |= static_cast<uint16_t>(value.values.xUint8[4] == other.values.xUint8[4]) << 4;
		result |= static_cast<uint16_t>(value.values.xUint8[5] == other.values.xUint8[5]) << 5;
		result |= static_cast<uint16_t>(value.values.xUint8[6] == other.values.xUint8[6]) << 6;
		result |= static_cast<uint16_t>(value.values.xUint8[7] == other.values.xUint8[7]) << 7;
		result |= static_cast<uint16_t>(value.values.xUint8[8] == other.values.xUint8[8]) << 8;
		result |= static_cast<uint16_t>(value.values.xUint8[9] == other.values.xUint8[9]) << 9;
		result |= static_cast<uint16_t>(value.values.xUint8[10] == other.values.xUint8[10]) << 10;
		result |= static_cast<uint16_t>(value.values.xUint8[11] == other.values.xUint8[11]) << 11;
		result |= static_cast<uint16_t>(value.values.xUint8[12] == other.values.xUint8[12]) << 12;
		result |= static_cast<uint16_t>(value.values.xUint8[13] == other.values.xUint8[13]) << 13;
		result |= static_cast<uint16_t>(value.values.xUint8[14] == other.values.xUint8[14]) << 14;
		result |= static_cast<uint16_t>(value.values.xUint8[15] == other.values.xUint8[15]) << 15;
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static uint16_t opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		uint16_t result{};
		result |= static_cast<uint16_t>(value.values.xUint8[0] < other.values.xUint8[0]) << 0;
		result |= static_cast<uint16_t>(value.values.xUint8[1] < other.values.xUint8[1]) << 1;
		result |= static_cast<uint16_t>(value.values.xUint8[2] < other.values.xUint8[2]) << 2;
		result |= static_cast<uint16_t>(value.values.xUint8[3] < other.values.xUint8[3]) << 3;
		result |= static_cast<uint16_t>(value.values.xUint8[4] < other.values.xUint8[4]) << 4;
		result |= static_cast<uint16_t>(value.values.xUint8[5] < other.values.xUint8[5]) << 5;
		result |= static_cast<uint16_t>(value.values.xUint8[6] < other.values.xUint8[6]) << 6;
		result |= static_cast<uint16_t>(value.values.xUint8[7] < other.values.xUint8[7]) << 7;
		result |= static_cast<uint16_t>(value.values.xUint8[8] < other.values.xUint8[8]) << 8;
		result |= static_cast<uint16_t>(value.values.xUint8[9] < other.values.xUint8[9]) << 9;
		result |= static_cast<uint16_t>(value.values.xUint8[10] < other.values.xUint8[10]) << 10;
		result |= static_cast<uint16_t>(value.values.xUint8[11] < other.values.xUint8[11]) << 11;
		result |= static_cast<uint16_t>(value.values.xUint8[12] < other.values.xUint8[12]) << 12;
		result |= static_cast<uint16_t>(value.values.xUint8[13] < other.values.xUint8[13]) << 13;
		result |= static_cast<uint16_t>(value.values.xUint8[14] < other.values.xUint8[14]) << 14;
		result |= static_cast<uint16_t>(value.values.xUint8[15] < other.values.xUint8[15]) << 15;
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint8[0]	 = (value.values.xUint8[0] == other.values.xUint8[0]) ? 0xFFu : 0x00u;
		result.values.xUint8[1]	 = (value.values.xUint8[1] == other.values.xUint8[1]) ? 0xFFu : 0x00u;
		result.values.xUint8[2]	 = (value.values.xUint8[2] == other.values.xUint8[2]) ? 0xFFu : 0x00u;
		result.values.xUint8[3]	 = (value.values.xUint8[3] == other.values.xUint8[3]) ? 0xFFu : 0x00u;
		result.values.xUint8[4]	 = (value.values.xUint8[4] == other.values.xUint8[4]) ? 0xFFu : 0x00u;
		result.values.xUint8[5]	 = (value.values.xUint8[5] == other.values.xUint8[5]) ? 0xFFu : 0x00u;
		result.values.xUint8[6]	 = (value.values.xUint8[6] == other.values.xUint8[6]) ? 0xFFu : 0x00u;
		result.values.xUint8[7]	 = (value.values.xUint8[7] == other.values.xUint8[7]) ? 0xFFu : 0x00u;
		result.values.xUint8[8]	 = (value.values.xUint8[8] == other.values.xUint8[8]) ? 0xFFu : 0x00u;
		result.values.xUint8[9]	 = (value.values.xUint8[9] == other.values.xUint8[9]) ? 0xFFu : 0x00u;
		result.values.xUint8[10] = (value.values.xUint8[10] == other.values.xUint8[10]) ? 0xFFu : 0x00u;
		result.values.xUint8[11] = (value.values.xUint8[11] == other.values.xUint8[11]) ? 0xFFu : 0x00u;
		result.values.xUint8[12] = (value.values.xUint8[12] == other.values.xUint8[12]) ? 0xFFu : 0x00u;
		result.values.xUint8[13] = (value.values.xUint8[13] == other.values.xUint8[13]) ? 0xFFu : 0x00u;
		result.values.xUint8[14] = (value.values.xUint8[14] == other.values.xUint8[14]) ? 0xFFu : 0x00u;
		result.values.xUint8[15] = (value.values.xUint8[15] == other.values.xUint8[15]) ? 0xFFu : 0x00u;
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint8[0]	 = (value.values.xUint8[0] < other.values.xUint8[0]) ? 0xFFu : 0x00u;
		result.values.xUint8[1]	 = (value.values.xUint8[1] < other.values.xUint8[1]) ? 0xFFu : 0x00u;
		result.values.xUint8[2]	 = (value.values.xUint8[2] < other.values.xUint8[2]) ? 0xFFu : 0x00u;
		result.values.xUint8[3]	 = (value.values.xUint8[3] < other.values.xUint8[3]) ? 0xFFu : 0x00u;
		result.values.xUint8[4]	 = (value.values.xUint8[4] < other.values.xUint8[4]) ? 0xFFu : 0x00u;
		result.values.xUint8[5]	 = (value.values.xUint8[5] < other.values.xUint8[5]) ? 0xFFu : 0x00u;
		result.values.xUint8[6]	 = (value.values.xUint8[6] < other.values.xUint8[6]) ? 0xFFu : 0x00u;
		result.values.xUint8[7]	 = (value.values.xUint8[7] < other.values.xUint8[7]) ? 0xFFu : 0x00u;
		result.values.xUint8[8]	 = (value.values.xUint8[8] < other.values.xUint8[8]) ? 0xFFu : 0x00u;
		result.values.xUint8[9]	 = (value.values.xUint8[9] < other.values.xUint8[9]) ? 0xFFu : 0x00u;
		result.values.xUint8[10] = (value.values.xUint8[10] < other.values.xUint8[10]) ? 0xFFu : 0x00u;
		result.values.xUint8[11] = (value.values.xUint8[11] < other.values.xUint8[11]) ? 0xFFu : 0x00u;
		result.values.xUint8[12] = (value.values.xUint8[12] < other.values.xUint8[12]) ? 0xFFu : 0x00u;
		result.values.xUint8[13] = (value.values.xUint8[13] < other.values.xUint8[13]) ? 0xFFu : 0x00u;
		result.values.xUint8[14] = (value.values.xUint8[14] < other.values.xUint8[14]) ? 0xFFu : 0x00u;
		result.values.xUint8[15] = (value.values.xUint8[15] < other.values.xUint8[15]) ? 0xFFu : 0x00u;
		return result;
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint16_t opBitMask(simd_int_t01 value) noexcept {
		uint64_t rawLo = value.values.xUint64[0];
		uint64_t rawHi = value.values.xUint64[1];
		if constexpr (std::endian::native == std::endian::big) {
			rawLo = internal::byteswap(rawLo);
			rawHi = internal::byteswap(rawHi);
		}
		uint64_t highBits0 = rawLo & 0x8080808080808080ull;
		uint64_t highBits1 = rawHi & 0x8080808080808080ull;
		uint16_t mask0	   = static_cast<uint16_t>((highBits0 * 0x0002040810204081ull) >> 56);
		uint16_t mask1	   = static_cast<uint16_t>((highBits1 * 0x0002040810204081ull) >> 56);
		return static_cast<uint16_t>(mask0 | (mask1 << 8));
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint16_t opBitMaskRaw(simd_int_t01 value) noexcept {
		return opBitMask(value);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		uint8_t idx				 = other.values.xUint8[0];
		result.values.xUint8[0]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[1];
		result.values.xUint8[1]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[2];
		result.values.xUint8[2]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[3];
		result.values.xUint8[3]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[4];
		result.values.xUint8[4]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[5];
		result.values.xUint8[5]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[6];
		result.values.xUint8[6]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[7];
		result.values.xUint8[7]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[8];
		result.values.xUint8[8]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[9];
		result.values.xUint8[9]	 = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[10];
		result.values.xUint8[10] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[11];
		result.values.xUint8[11] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[12];
		result.values.xUint8[12] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[13];
		result.values.xUint8[13] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[14];
		result.values.xUint8[14] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		idx						 = other.values.xUint8[15];
		result.values.xUint8[15] = (idx & 0x80u) ? 0x00u : value.values.xUint8[idx & 0x0Fu];
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint64[0] = value.values.xUint64[0] ^ other.values.xUint64[0];
		result.values.xUint64[1] = value.values.xUint64[1] ^ other.values.xUint64[1];
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		uint16_t a				 = value.values.xUint8[0];
		uint16_t b				 = other.values.xUint8[0];
		result.values.xUint8[0]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[1];
		b						 = other.values.xUint8[1];
		result.values.xUint8[1]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[2];
		b						 = other.values.xUint8[2];
		result.values.xUint8[2]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[3];
		b						 = other.values.xUint8[3];
		result.values.xUint8[3]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[4];
		b						 = other.values.xUint8[4];
		result.values.xUint8[4]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[5];
		b						 = other.values.xUint8[5];
		result.values.xUint8[5]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[6];
		b						 = other.values.xUint8[6];
		result.values.xUint8[6]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[7];
		b						 = other.values.xUint8[7];
		result.values.xUint8[7]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[8];
		b						 = other.values.xUint8[8];
		result.values.xUint8[8]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[9];
		b						 = other.values.xUint8[9];
		result.values.xUint8[9]	 = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[10];
		b						 = other.values.xUint8[10];
		result.values.xUint8[10] = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[11];
		b						 = other.values.xUint8[11];
		result.values.xUint8[11] = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[12];
		b						 = other.values.xUint8[12];
		result.values.xUint8[12] = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[13];
		b						 = other.values.xUint8[13];
		result.values.xUint8[13] = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[14];
		b						 = other.values.xUint8[14];
		result.values.xUint8[14] = static_cast<uint8_t>(a > b ? a - b : 0);
		a						 = value.values.xUint8[15];
		b						 = other.values.xUint8[15];
		result.values.xUint8[15] = static_cast<uint8_t>(a > b ? a - b : 0);
		return result;
	}

	template<uint16_t permuteMask, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opPermute(simd_int_t01 value, simd_int_t02) noexcept {
		return value;
	}

	template<uint16_t alignment, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		{
			constexpr int64_t src = static_cast<int64_t>(0) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[0] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[0] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[0] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(1) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[1] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[1] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[1] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(2) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[2] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[2] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[2] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(3) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[3] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[3] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[3] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(4) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[4] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[4] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[4] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(5) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[5] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[5] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[5] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(6) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[6] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[6] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[6] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(7) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[7] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[7] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[7] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(8) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[8] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[8] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[8] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(9) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[9] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[9] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[9] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(10) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[10] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[10] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[10] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(11) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[11] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[11] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[11] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(12) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[12] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[12] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[12] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(13) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[13] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[13] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[13] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(14) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[14] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[14] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[14] = 0;
			}
		}
		{
			constexpr int64_t src = static_cast<int64_t>(15) + static_cast<int64_t>(alignment);
			if constexpr (src >= 0 && src < static_cast<int64_t>(simd_int_t01::eightPer)) {
				result.values.xUint8[15] = other.values.xUint8[static_cast<uint64_t>(src)];
			} else if constexpr (src >= static_cast<int64_t>(simd_int_t01::eightPer) && src < static_cast<int64_t>(simd_int_t01::eightPer) * 2) {
				result.values.xUint8[15] = value.values.xUint8[static_cast<uint64_t>(src - static_cast<int64_t>(simd_int_t01::eightPer))];
			} else {
				result.values.xUint8[15] = 0;
			}
		}
		return result;
	}

	template<uint16_t alignment, simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opSrLi(simd_int_t01 value) noexcept {
		simd_int_t01 result{};
		result.values.xUint16[0] = static_cast<uint16_t>(value.values.xUint16[0] >> alignment);
		result.values.xUint16[1] = static_cast<uint16_t>(value.values.xUint16[1] >> alignment);
		result.values.xUint16[2] = static_cast<uint16_t>(value.values.xUint16[2] >> alignment);
		result.values.xUint16[3] = static_cast<uint16_t>(value.values.xUint16[3] >> alignment);
		result.values.xUint16[4] = static_cast<uint16_t>(value.values.xUint16[4] >> alignment);
		result.values.xUint16[5] = static_cast<uint16_t>(value.values.xUint16[5] >> alignment);
		result.values.xUint16[6] = static_cast<uint16_t>(value.values.xUint16[6] >> alignment);
		result.values.xUint16[7] = static_cast<uint16_t>(value.values.xUint16[7] >> alignment);
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint64[0] = value.values.xUint64[0] & other.values.xUint64[0];
		result.values.xUint64[1] = value.values.xUint64[1] & other.values.xUint64[1];
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint64[0] = value.values.xUint64[0] | other.values.xUint64[0];
		result.values.xUint64[1] = value.values.xUint64[1] | other.values.xUint64[1];
		return result;
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		simd_int_t01 result{};
		result.values.xUint64[0] = value.values.xUint64[0] & ~other.values.xUint64[0];
		result.values.xUint64[1] = value.values.xUint64[1] & ~other.values.xUint64[1];
		return result;
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool opTest(simd_int_t01 value) noexcept {
		if (value.values.xUint64[0] != 0) {
			return false;
		}
		if (value.values.xUint64[1] != 0) {
			return false;
		}
		return true;
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opNot(simd_int_t01 value) noexcept {
		simd_int_t01 result{};
		result.values.xUint64[0] = ~value.values.xUint64[0];
		result.values.xUint64[1] = ~value.values.xUint64[1];
		return result;
	}

	template<uint16_t N, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_t01 opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return opAlignR<N>(current, previous);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool anyBitsSetAnywhere(simd_int_t01 value) noexcept {
		return !opTest(value);
	}

	template<simd_int_128_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

	template<typename simd_type> JSONIFIER_INLINE bool isAscii(simd_type value) {
		return opBitMask(value) == 0;
	}

#endif

}
