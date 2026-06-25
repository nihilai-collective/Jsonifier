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
	}
}
