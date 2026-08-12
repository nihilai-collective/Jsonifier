// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/int32_t.hpp
#pragma once

#include "common.hpp"

namespace int_validation_tests {

	constexpr jsonifier::internal::array<std::string_view, 24> inputValues{ { "0", "1", "-1", "42", "-42", "123456789", "-123456789", "2147483647", "-2147483648",
		"9223372036854775807", "-9223372036854775808", "0.0", "1.5", "-1.5", "3.14159", "-2.71828", "123.456", "-789.012", "1e5", "-2e3", "3.14e10", "-4.2e-1", "5E2",
		"10000000e-7" } };

	constexpr jsonifier::internal::array<int64_t, 24> outputValues{ { 0, 1, -1, 42, -42, 123456789, -123456789, 2147483647, -2147483648, 9223372036854775807LL,
		std::numeric_limits<int64_t>::min(), 0, 1, -1, 3, -2, 123, -789, 100000, -2000, 31400000000LL, 0, 500, 1 } };

	constexpr jsonifier::internal::array<std::string_view, 11> failValues{ { "9223372036854775808", "-9223372036854775809", "-", "1.2.3", "1e", "1e+", "1e-", "\"abc\"", "true",
		"null", "{}" } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void intTestsImpl() {
		std::cout << "Int Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<int64_t, int64_t, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		std::cout << "Int Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		fail_test_runner<int64_t, failValues, partial, knownOrder, nullTerminated, fail_tests_runner, jsonifier::internal::make_integer_sequence<failValues.size()>>::impl();
		return;
	}

	inline static void runTests() {
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
