/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/integer.hpp
 */
#pragma once

#include "common.hpp"

namespace int_validation_tests {

	inline constexpr jsonifier::internal::array<std::string_view, 28> inputValues{ { "0", "1", "-1", "42", "-42", "123456789", "-123456789", "2147483647", "-2147483648",
		"9223372036854775807", "-9223372036854775808", "0.0", "1.5", "-1.5", "3.14159", "-2.71828", "123.456", "-789.012", "1e5", "-2e3", "3.14e10", "-4.2e-1", "5E2",
		"10000000e-7", "1.00000000000000000000e1", "1.99999999999999999999", "1.5e-18", "9.223372036854775807e18" } };

	inline constexpr jsonifier::internal::array<int64_t, 28> outputValues{ { 0, 1, -1, 42, -42, 123456789, -123456789, 2147483647, -2147483648, 9223372036854775807LL,
		std::numeric_limits<int64_t>::min(), 0, 1, -1, 3, -2, 123, -789, 100000, -2000, 31400000000LL, 0, 500, 1, 10, 1, 0, 9223372036854775807LL } };

	inline constexpr jsonifier::internal::array<std::string_view, 14> failValues{ { "9223372036854775808", "-9223372036854775809", "-", "1.2.3", "1e", "1e+", "1e-", "\"abc\"",
		"true", "null", "{}", "1e9223372036854775809", "1.5e9223372036854775809", "9.223372036854775808e18" } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline void intTestsImpl() {
		std::cout << "Int Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<int64_t, int64_t, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		std::cout << "Int Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		fail_test_runner<int64_t, failValues, partial, knownOrder, nullTerminated, fail_tests_runner, jsonifier::internal::make_integer_sequence<failValues.size()>>::impl();
		return;
	}

	inline void runTests() {
		intTestsImpl<false, false, false>();
		intTestsImpl<false, true, false>();
		intTestsImpl<true, false, false>();
		intTestsImpl<true, true, false>();
		intTestsImpl<false, false, true>();
		intTestsImpl<false, true, true>();
		intTestsImpl<true, false, true>();
		intTestsImpl<true, true, true>();
		std::cout << "Int validation tests complete." << std::endl;
	}
}

namespace uint_validation_tests {

	inline constexpr jsonifier::internal::array<std::string_view, 18> inputValues{ { "0", "1", "42", "123456789", "2147483647", "18446744073709551615", "0.0", "1.5", "3.14159",
		"123.456", "1e5", "2e3", "3.14e10", "4.2e-1", "5E2", "10000000e-7", "1.000000000000000000000e1", "1.8446744073709551615e19" } };

	inline constexpr jsonifier::internal::array<uint64_t, 18> outputValues{ { 0, 1, 42, 123456789, 2147483647, 18446744073709551615ULL, 0, 1, 3, 123, 100000, 2000, 31400000000ULL,
		0, 500, 1, 10, 18446744073709551615ULL } };

	inline constexpr jsonifier::internal::array<std::string_view, 13> failValues{ { "18446744073709551616", "-9223372036854775809", "-", "1.2.3", "1e", "1e+", "1e-", "\"abc\"",
		"true", "null", "{}", "3e18446744073709551617", "1.8446744073709551616e19" } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline void uintTestsImpl() {
		std::cout << "Uint Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<uint64_t, uint64_t, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		std::cout << "Uint Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		fail_test_runner<uint64_t, failValues, partial, knownOrder, nullTerminated, fail_tests_runner, jsonifier::internal::make_integer_sequence<failValues.size()>>::impl();
		return;
	}

	inline void runTests() {
		uintTestsImpl<false, false, false>();
		uintTestsImpl<false, true, false>();
		uintTestsImpl<true, false, false>();
		uintTestsImpl<true, true, false>();
		uintTestsImpl<false, false, true>();
		uintTestsImpl<false, true, true>();
		uintTestsImpl<true, false, true>();
		uintTestsImpl<true, true, true>();
		std::cout << "Uint validation tests complete." << std::endl;
	}

}
