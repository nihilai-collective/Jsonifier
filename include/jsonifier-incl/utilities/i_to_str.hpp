/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/i_to_str.hpp
 */
#pragma once

#include <jsonifier-incl/containers/aligned_allocator.hpp>

namespace jsonifier::internal {

	consteval array<uint16_t, 100> genOneOrTwoDigitTable() noexcept {
		array<uint16_t, 100> returnValue{};
		for (uint64_t i{ 0 }; i < 100; ++i) {
			const uint16_t first{ static_cast<uint16_t>(static_cast<uint8_t>('0') + (i < 10 ? i : i / 10)) };
			const uint16_t second{ static_cast<uint16_t>(i < 10 ? 0 : static_cast<uint8_t>('0') + i % 10) };
			returnValue[i] = std::endian::native == std::endian::little ? static_cast<uint16_t>(first | (second << 8)) : static_cast<uint16_t>((first << 8) | second);
		}
		return returnValue;
	}

	consteval array<uint32_t, 10000> genFourDigitTable() noexcept {
		array<uint32_t, 10000> returnValue{};
		for (uint64_t i{ 0 }; i < 10000; ++i) {
			uint64_t divisor{ 1000 };
			for (uint64_t j{ 0 }; j < sizeof(uint32_t); ++j) {
				const auto digit{ static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / divisor % 10)) };
				const uint64_t shift{ (std::endian::native == std::endian::little ? j : sizeof(uint32_t) - 1 - j) * 8 };
				returnValue[i] |= static_cast<uint32_t>(digit << shift);
				divisor /= 10;
			}
		}
		return returnValue;
	}

	alignas(64) inline constexpr auto oneOrTwoDigitTable{ genOneOrTwoDigitTable() };
	inline constexpr const auto* __restrict oneOrTwoDigitData{ oneOrTwoDigitTable.data() };
	alignas(64) inline constexpr auto fourDigitTable{ genFourDigitTable() };
	inline constexpr const auto* __restrict fourDigitData{ fourDigitTable.data() };

	template<typename v_type> struct to_chars;

	// Idea for this specific evolved implementation of our original 4-digit algorithm (https://github.com/nihilai-collective/Jsonifier/blob/9790d83ecdd997b70c31bd8f604d6f3b431997dc/include/jsonifier-incl/utilities/IToStr.hpp#L93-L124), sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
	struct digit_writer {
		JSONIFIER_INLINE static void writeFour(write_buffer_ptr buf, const uint32_t value) noexcept {
			pow2MemcpyWrapper<4ULL>(buf, fourDigitData + value);
		}

		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> writeOneOrTwo(write_buffer_ptr buf, const uint32_t value) noexcept {
			pow2MemcpyWrapper<2ULL>(buf, oneOrTwoDigitData + value);
			return buf + 1 + static_cast<uint64_t>(value >= 10U);
		}

		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> writeOneToFour(write_buffer_ptr buf, const uint32_t value) noexcept {
			const uint32_t zeros{ static_cast<uint32_t>(value < 10U) + static_cast<uint32_t>(value < 100U) + static_cast<uint32_t>(value < 1000U) };
			pow2MemcpyWrapper<4ULL>(buf, std::bit_cast<const uint8_t*>(fourDigitData + value) + zeros);
			return buf + 4 - zeros;
		}

		JSONIFIER_INLINE static void writeEight(write_buffer_ptr buf, const uint32_t value) noexcept {
			const uint32_t high{ static_cast<uint32_t>((static_cast<uint64_t>(value) * 109951163ULL) >> 40) };
			writeFour(buf, high);
			writeFour(buf + 4, value - high * 10000U);
		}

		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> writeBelow1e8(write_buffer_ptr buf, const uint32_t value) noexcept {
			if (value < 100U) {
				return writeOneOrTwo(buf, value);
			}
			if (value < 10000U) {
				return writeOneToFour(buf, value);
			}
			const uint32_t high{ static_cast<uint32_t>((static_cast<uint64_t>(value) * 109951163ULL) >> 40) };
			buf = writeOneToFour(buf, high);
			writeFour(buf, value - high * 10000U);
			return buf + 4;
		}
	};

	template<uint64_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(write_buffer_ptr buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			if (value < 100000000ULL) {
				return digit_writer::writeBelow1e8(buf, static_cast<uint32_t>(value));
			}
			const uint64_t high{ value / 100000000ULL };
			const uint32_t low{ static_cast<uint32_t>(value - high * 100000000ULL) };
			if (high < 100ULL) {
				buf = digit_writer::writeOneOrTwo(buf, static_cast<uint32_t>(high));
			} else if (high < 100000000ULL) {
				buf = digit_writer::writeBelow1e8(buf, static_cast<uint32_t>(high));
			} else {
				const uint64_t top{ high / 100000000ULL };
				buf = digit_writer::writeOneToFour(buf, static_cast<uint32_t>(top));
				digit_writer::writeEight(buf, static_cast<uint32_t>(high - top * 100000000ULL));
				buf += 8;
			}
			digit_writer::writeEight(buf, low);
			return buf + 8;
		}
	};

	template<uint32_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(write_buffer_ptr buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			if (value < 100000000U) {
				return digit_writer::writeBelow1e8(buf, static_cast<uint32_t>(value));
			}
			const uint32_t high{ static_cast<uint32_t>(value / 100000000ULL) };
			buf = digit_writer::writeOneOrTwo(buf, high);
			digit_writer::writeEight(buf, static_cast<uint32_t>(value) - high * 100000000U);
			return buf + 8;
		}
	};

	template<uint16_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(write_buffer_ptr buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return digit_writer::writeBelow1e8(buf, static_cast<uint32_t>(value));
		}
	};

	template<uint8_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(write_buffer_ptr buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 100U ? digit_writer::writeOneOrTwo(buf, static_cast<uint32_t>(value)) : digit_writer::writeOneToFour(buf, static_cast<uint32_t>(value));
		}
	};

	template<int_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(write_buffer_ptr buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			using unsigned_type = std::make_unsigned_t<v_type>;
			const bool negative{ value < 0 };
			const unsigned_type magnitude{ negative ? static_cast<unsigned_type>(unsigned_type{ 0 } - static_cast<unsigned_type>(value)) : static_cast<unsigned_type>(value) };
			*buf = '-';
			buf += static_cast<uint64_t>(negative);
			return to_chars<unsigned_type>::impl(buf, magnitude);
		}
	};
}
