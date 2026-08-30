// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/str_to_d.hpp
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

	template<float_t value_type> struct float_parser;

	template<float_t value_type> struct float_parser {
		JSONIFIER_INLINE static string_view_ptr parseFloat(value_type& value, string_view_ptr iter, string_view_ptr end = nullptr) noexcept {
			using namespace jsonifier::internal;
			span<char> fraction;

			int64_t expNumber{};
			int64_t exponent{};
			uint64_t mantissa{};

			if (iter >= end) {
				return nullptr;
			}

			const bool negative{ *iter == minus };
			bool tooManyDigits{};

			if (negative) {
				++iter;

				if (!is_digit(static_cast<uint8_t>(*iter))) [[unlikely]] {
					return nullptr;
				}
			}

			span<char> integer{ iter };

			if (uint64_t val; (end - iter) >= 2 && (static_cast<void>(val = read2_to_u64(iter) - 0x3030), is_made_of_two_digits_no_sub(val))) {
				mantissa = mantissa * 100 + parse_two_digits_unrolled_no_sub(val);
				iter += 2;
			}

			while (is_digit(static_cast<uint8_t>(*iter))) {
				mantissa = 10 * mantissa + static_cast<uint8_t>(*iter - static_cast<uint8_t>('0'));
				++iter;
			}

			int64_t digitcount = static_cast<int64_t>(iter - integer.ptr);
			integer.end		   = integer.ptr + static_cast<uint64_t>(digitcount);

			if (digitcount == 0 || (integer.ptr[0] == static_cast<uint8_t>('0') && digitcount > 1)) [[unlikely]] {
				return nullptr;
			}

			char const* before;

			if (*iter == decimal) {
				++iter;
				before = iter;

				loop_parse_if_eight_digits(iter, end, mantissa);

				while ((iter != end) && is_digit(static_cast<uint8_t>(*iter))) {
					uint8_t digit = uint8_t(*iter - char('0'));
					++iter;
					mantissa = mantissa * 10 + digit;
				}

				exponent	 = before - iter;
				fraction.ptr = before;
				fraction.end = fraction.ptr + static_cast<uint64_t>(iter - before);
				digitcount -= exponent;

				if (exponent == 0) [[unlikely]] {
					return nullptr;
				}
			}

			if (exp_tables<>::expTable[static_cast<uint8_t>(*iter)]) {
				before = iter;
				++iter;
				bool negExp = false;
				if (minus == *iter) {
					negExp = true;
					++iter;
				} else if (plus == *iter) {
					++iter;
				}
				if (is_digit(static_cast<uint8_t>(*iter))) {
					while (is_digit(static_cast<uint8_t>(*iter))) {
						if (expNumber < 0x10000000) {
							expNumber = 10 * expNumber + static_cast<uint8_t>(*iter - static_cast<uint8_t>('0'));
						}
						++iter;
					}
					if (negExp) {
						expNumber = -expNumber;
					}
					exponent += expNumber;
				} else {
					return nullptr;
				}
			}

			if (digitcount > 19) {
				before = integer.ptr;
				while ((*before == static_cast<uint8_t>('0') || *before == decimal)) {
					if (*before == static_cast<uint8_t>('0')) {
						--digitcount;
					}
					++before;
				}

				if (digitcount > 19) {
					tooManyDigits = true;
					mantissa	  = 0;
					before		  = integer.ptr;
					static constexpr uint64_t minNineteenDigitInteger{ 1000000000000000000 };
					while ((mantissa < minNineteenDigitInteger) && (before != integer.end)) {
						mantissa = mantissa * 10 + static_cast<uint8_t>(*before - static_cast<uint8_t>('0'));
						++before;
					}
					if (mantissa >= minNineteenDigitInteger) {
						exponent = integer.end - before + expNumber;
					} else {
						before = fraction.ptr;
						while ((mantissa < minNineteenDigitInteger) && (before != fraction.end)) {
							mantissa = mantissa * 10 + static_cast<uint8_t>(*before - static_cast<uint8_t>('0'));
							++before;
						}
						exponent = fraction.ptr - before + expNumber;
					}
				}
			}

			if (binary_format<value_type>::min_exponent_fast_path <= exponent && exponent <= binary_format<value_type>::max_exponent_fast_path && !tooManyDigits) {
				if (rounds_to_nearest::roundsToNearest) {
					if (mantissa <= binary_format<value_type>::max_mantissa_fast_path_value) {
						value = static_cast<value_type>(mantissa);
						if (exponent < 0) {
							value = value / binary_format<value_type>::exact_power_of_ten(-exponent);
						} else {
							value = value * binary_format<value_type>::exact_power_of_ten(exponent);
						}
						if (negative) {
							value = -value;
						}
						return iter;
					}
				} else {
					if (exponent >= 0 && mantissa <= binary_format<value_type>::max_mantissa_fast_path(exponent)) {
#if defined(__clang__) || defined(JSONIFIER_FASTFLOAT_32BIT)
						if (mantissa == 0) {
							value = negative ? static_cast<value_type>(-0.) : static_cast<value_type>(0.);
							return iter;
						}
#endif
						value = static_cast<value_type>(mantissa) * binary_format<value_type>::exact_power_of_ten(exponent);
						if (negative) {
							value = -value;
						}
						return iter;
					}
				}
			}
			adjusted_mantissa am = compute_float<binary_format<value_type>>(exponent, mantissa);
			if (tooManyDigits && am.power2 >= 0) {
				if (am != compute_float<binary_format<value_type>>(exponent, mantissa + 1)) {
					am = compute_error<binary_format<value_type>>(exponent, mantissa);
				}
			}
			if (am.power2 < 0) [[unlikely]] {
				am = digit_comp<value_type>(integer, fraction, mantissa, exponent, am);
			}

			if (am.power2 == binary_format<value_type>::infinite_power) [[unlikely]] {
				return nullptr;
			}

			to_float(negative, am, value);
			return iter;
		}
	};
	
}
