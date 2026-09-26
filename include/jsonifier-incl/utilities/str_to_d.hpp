/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/str_to_d.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/fast_float.hpp>
#include <jsonifier-incl/utilities/utility.hpp>

namespace jsonifier::internal {

	template<typename = void> struct exp_tables {
		static constexpr bool expTable[]{ false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false };

		static constexpr bool expFracTable[]{ false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
			false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false };
	};

	static constexpr char decimal{ '.' };
	static constexpr char minus{ '-' };
	static constexpr char plus{ '+' };
	static constexpr char nine{ '9' };

	struct parsed_number {
		read_buffer_ptr lastMatch{};
		span<char> fraction{};
		span<char> integer{};
		bool tooManyDigits{};
		uint64_t mantissa{};
		int64_t exponent{};
		bool negative{};
		bool valid{};
	};

	JSONIFIER_INLINE static parsed_number parse_number_string(read_buffer_ptr iter, read_buffer_ptr end, bool storeSpans) noexcept {
		parsed_number answer{};
		answer.negative = (*iter == minus);
		if (answer.negative) {
			++iter;
			if (iter == end || !is_integer(*iter)) [[unlikely]] {
				return answer;
			}
		}
		read_buffer_ptr const startDigits = iter;

		uint64_t mantissa{};
		if ((iter != end) && is_integer(*iter)) {
			mantissa = static_cast<uint64_t>(*iter - '0');
			++iter;
			if ((iter != end) && is_integer(*iter)) {
				mantissa = 10 * mantissa + static_cast<uint64_t>(*iter - '0');
				++iter;
				if ((iter != end) && is_integer(*iter)) {
					mantissa = 10 * mantissa + static_cast<uint64_t>(*iter - '0');
					++iter;
					if ((iter != end) && is_integer(*iter)) {
						mantissa = 10 * mantissa + static_cast<uint64_t>(*iter - '0');
						++iter;
						if ((iter != end) && is_integer(*iter)) {
							mantissa = 10 * mantissa + static_cast<uint64_t>(*iter - '0');
							++iter;
							while ((iter != end) && is_integer(*iter)) {
								mantissa = 10 * mantissa + static_cast<uint64_t>(*iter - '0');
								++iter;
							}
						}
					}
				}
			}
		}
		read_buffer_ptr const endOfIntegerPart = iter;
		int64_t digitCount					   = static_cast<int64_t>(endOfIntegerPart - startDigits);
		if (storeSpans) {
			answer.integer = span<char>{ startDigits, endOfIntegerPart };
		}
		if (digitCount == 0 || (startDigits[0] == '0' && digitCount > 1)) [[unlikely]] {
			return answer;
		}

		int64_t exponent{};
		bool const hasDecimalPoint = (iter != end) && (*iter == decimal);
		if (hasDecimalPoint) {
			++iter;
			read_buffer_ptr const before = iter;
			loop_parse_if_eight_digits(iter, end, mantissa);
			while ((iter != end) && is_integer(*iter)) {
				uint8_t digit = static_cast<uint8_t>(*iter - '0');
				++iter;
				mantissa = mantissa * 10 + digit;
			}
			exponent = before - iter;
			if (storeSpans) {
				answer.fraction = span<char>{ before, iter };
			}
			digitCount -= exponent;
			if (exponent == 0) [[unlikely]] {
				return answer;
			}
		}

		int64_t expNumber{};
		if ((iter != end) && ((*iter == 'e') || (*iter == 'E'))) {
			++iter;
			bool negExp = false;
			if ((iter != end) && (*iter == minus)) {
				negExp = true;
				++iter;
			} else if ((iter != end) && (*iter == plus)) {
				++iter;
			}
			if ((iter == end) || !is_integer(*iter)) [[unlikely]] {
				return answer;
			}
			while ((iter != end) && is_integer(*iter)) {
				uint8_t digit = static_cast<uint8_t>(*iter - '0');
				if (expNumber < 0x10000000) {
					expNumber = 10 * expNumber + digit;
				}
				++iter;
			}
			if (negExp) {
				expNumber = -expNumber;
			}
			exponent += expNumber;
		}
		answer.lastMatch = iter;
		answer.valid	 = true;

		if (digitCount > 19) [[unlikely]] {
			read_buffer_ptr start = startDigits;
			while ((start != end) && (*start == '0' || *start == decimal)) {
				if (*start == '0') {
					--digitCount;
				}
				++start;
			}
			if (digitCount > 19) {
				answer.tooManyDigits = true;
				if (storeSpans) {
					static constexpr uint64_t minNineteenDigitInteger{ 1000000000000000000 };
					mantissa = 0;
					iter	 = answer.integer.ptr;
					while ((mantissa < minNineteenDigitInteger) && (iter != answer.integer.end)) {
						mantissa = mantissa * 10 + static_cast<uint64_t>(*iter - '0');
						++iter;
					}
					if (mantissa >= minNineteenDigitInteger) {
						exponent = endOfIntegerPart - iter + expNumber;
					} else {
						iter = answer.fraction.ptr;
						while ((mantissa < minNineteenDigitInteger) && (iter != answer.fraction.end)) {
							mantissa = mantissa * 10 + static_cast<uint64_t>(*iter - '0');
							++iter;
						}
						exponent = answer.fraction.ptr - iter + expNumber;
					}
				}
			}
		}
		answer.exponent = exponent;
		answer.mantissa = mantissa;
		return answer;
	}

	template<float_t value_type> struct float_parser {
		JSONIFIER_INLINE static bool clinger_fast_path(uint64_t mantissa, int64_t exponent, bool negative, value_type& value) noexcept {
			if (binary_format<value_type>::min_exponent_fast_path <= exponent && exponent <= binary_format<value_type>::max_exponent_fast_path &&
				mantissa <= binary_format<value_type>::max_mantissa_fast_path_value) {
				if (rounds_to_nearest::roundsToNearest) {
					value = static_cast<value_type>(mantissa);
					if (exponent < 0) {
						value = value / binary_format<value_type>::exact_power_of_ten(-exponent);
					} else {
						value = value * binary_format<value_type>::exact_power_of_ten(exponent);
					}
					if (negative) {
						value = -value;
					}
					return true;
				} else if (exponent >= 0 && mantissa <= binary_format<value_type>::max_mantissa_fast_path(exponent)) {
#if JSONIFIER_COMPILER_CLANG
					if (mantissa == 0) {
						value = negative ? static_cast<value_type>(-0.) : static_cast<value_type>(0.);
						return true;
					}
#endif
					value = static_cast<value_type>(mantissa) * binary_format<value_type>::exact_power_of_ten(exponent);
					if (negative) {
						value = -value;
					}
					return true;
				}
			}
			return false;
		}

		JSONIFIER_INLINE static read_buffer_ptr parseFloatSlow(value_type& value, read_buffer_ptr iter, read_buffer_ptr end) noexcept {
			parsed_number pns = parse_number_string(iter, end, true);
			if (!pns.tooManyDigits && clinger_fast_path(pns.mantissa, pns.exponent, pns.negative, value)) {
				return pns.lastMatch;
			}
			adjusted_mantissa am = compute_float<binary_format<value_type>>(pns.exponent, pns.mantissa);
			if (pns.tooManyDigits && am.power2 >= 0) {
				if (am != compute_float<binary_format<value_type>>(pns.exponent, pns.mantissa + 1)) {
					am = compute_error<binary_format<value_type>>(pns.exponent, pns.mantissa);
				}
			}
			if (am.power2 < 0) {
				am = digit_comp<value_type>(pns.integer, pns.fraction, pns.mantissa, pns.exponent, am);
			}
			if (am.power2 == binary_format<value_type>::infinite_power) {
				return nullptr;
			}
			to_float(pns.negative, am, value);
			return pns.lastMatch;
		}

		JSONIFIER_INLINE static read_buffer_ptr parseFloat(value_type& value, read_buffer_ptr iter, read_buffer_ptr end = nullptr) noexcept {
			if (iter >= end) [[unlikely]] {
				return nullptr;
			}
			parsed_number pns = parse_number_string(iter, end, false);
			if (!pns.valid) [[unlikely]] {
				return nullptr;
			}
			if (pns.tooManyDigits) [[unlikely]] {
				return parseFloatSlow(value, iter, end);
			}
			if (clinger_fast_path(pns.mantissa, pns.exponent, pns.negative, value)) {
				return pns.lastMatch;
			}
			adjusted_mantissa am = compute_float<binary_format<value_type>>(pns.exponent, pns.mantissa);
			if (am.power2 < 0) [[unlikely]] {
				return parseFloatSlow(value, iter, end);
			}
			if (am.power2 == binary_format<value_type>::infinite_power) [[unlikely]] {
				return nullptr;
			}
			to_float(pns.negative, am, value);
			return pns.lastMatch;
		}
	};

}
