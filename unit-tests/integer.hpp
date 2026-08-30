// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/integer.hpp
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

namespace uint_validation_tests {

	constexpr jsonifier::internal::array<std::string_view, 16> inputValues{ { "0", "1", "42", "123456789", "2147483647", "18446744073709551615", "0.0", "1.5", "3.14159", "123.456",
		"1e5", "2e3", "3.14e10", "4.2e-1", "5E2", "10000000e-7" } };

	constexpr jsonifier::internal::array<uint64_t, 16> outputValues{ { 0, 1, 42, 123456789, 2147483647, 18446744073709551615ULL, 0, 1, 3, 123, 100000, 2000, 31400000000ULL, 0, 500,
		1 } };

	constexpr jsonifier::internal::array<std::string_view, 11> failValues{ { "18446744073709551616", "-9223372036854775809", "-", "1.2.3", "1e", "1e+", "1e-", "\"abc\"", "true",
		"null", "{}" } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void uintTestsImpl() {
		std::cout << "Uint Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<uint64_t, uint64_t, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		std::cout << "Uint Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		fail_test_runner<uint64_t, failValues, partial, knownOrder, nullTerminated, fail_tests_runner, jsonifier::internal::make_integer_sequence<failValues.size()>>::impl();
		return;
	}

	inline static void runTests() {
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

namespace i_to_str_tests {

	constexpr jsonifier::internal::array<uint8_t, 8> uint8Boundaries{ { 0, 1, 9, 10, 99, 100, 254, 255 } };

	constexpr jsonifier::internal::array<uint16_t, 12> uint16Boundaries{ { 0, 1, 9, 10, 99, 100, 999, 1000, 9999, 10000, 65534, 65535 } };

	constexpr jsonifier::internal::array<uint32_t, 22> uint32Boundaries{ { 0u, 1u, 9u, 10u, 99u, 100u, 999u, 1000u, 9999u, 10000u, 99999u, 100000u, 999999u, 1000000u, 9999999u,
		10000000u, 99999999u, 100000000u, 999999999u, 1000000000u, 4294967294u, 4294967295u } };

	constexpr jsonifier::internal::array<uint64_t, 40> uint64Boundaries{ { 0ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, 9999ULL, 10000ULL, 99999ULL, 100000ULL, 999999ULL,
		1000000ULL, 9999999ULL, 10000000ULL, 99999999ULL, 100000000ULL, 999999999ULL, 1000000000ULL, 9999999999ULL, 10000000000ULL, 99999999999ULL, 100000000000ULL,
		999999999999ULL, 1000000000000ULL, 9999999999999ULL, 10000000000000ULL, 99999999999999ULL, 100000000000000ULL, 999999999999999ULL, 1000000000000000ULL,
		9999999999999999ULL, 10000000000000000ULL, 99999999999999999ULL, 100000000000000000ULL, 999999999999999999ULL, 1000000000000000000ULL, 9999999999999999999ULL,
		10000000000000000000ULL, 18446744073709551615ULL } };

	constexpr jsonifier::internal::array<int8_t, 13> int8Boundaries{ { 0, 1, -1, 9, -9, 10, -10, 99, -99, 100, -100, 127, -128 } };

	constexpr jsonifier::internal::array<int16_t, 21> int16Boundaries{ { 0, 1, -1, 9, -9, 10, -10, 99, -99, 100, -100, 999, -999, 1000, -1000, 9999, -9999, 10000, -10000, 32767,
		-32768 } };

	constexpr jsonifier::internal::array<int32_t, 41> int32Boundaries{ { 0, 1, -1, 9, -9, 10, -10, 99, -99, 100, -100, 999, -999, 1000, -1000, 9999, -9999, 10000, -10000, 99999,
		-99999, 100000, -100000, 999999, -999999, 1000000, -1000000, 9999999, -9999999, 10000000, -10000000, 99999999, -99999999, 100000000, -100000000, 999999999, -999999999,
		1000000000, -1000000000, 2147483647, -2147483648 } };

	constexpr jsonifier::internal::array<int64_t, 27> int64Boundaries{ { 0LL, 1LL, -1LL, 9LL, -9LL, 10LL, -10LL, 99999LL, -99999LL, 100000LL, -100000LL, 9999999999LL,
		-9999999999LL, 10000000000LL, -10000000000LL, 999999999999999LL, -999999999999999LL, 1000000000000000LL, -1000000000000000LL, 999999999999999999LL,
		-999999999999999999LL, 1000000000000000000LL, -1000000000000000000LL, 9223372036854775806LL, 9223372036854775807LL, -9223372036854775807LL,
		std::numeric_limits<int64_t>::min() } };

	template<typename value_type, const auto& values> inline static void checkToCharsMatchesStdToString() {
		for (auto value: values) {
			char buffer[32]{};
			auto* end = jsonifier::internal::to_chars<value_type>::impl(buffer, value);
			std::string result{ buffer, static_cast<std::size_t>(end - buffer) };
			rt_ut::unit_test<"i_to_str_digit_boundary_matches_std_to_string", true>::assert_eq(std::to_string(value), [&]() {
				return result;
			});
		}
	}

	template<typename value_type, const auto& values> inline static void checkToStringMatchesStdToString() {
		for (auto value: values) {
			auto result = jsonifier::toString(value);
			std::string resultStr{ result.data(), result.size() };
			rt_ut::unit_test<"jsonifier_to_string_matches_std_to_string", true>::assert_eq(std::to_string(value), [&]() {
				return resultStr;
			});
		}
	}

	inline static void runTests() {
		std::cout << "i_to_str Digit-Boundary Tests: " << std::endl;
		checkToCharsMatchesStdToString<uint8_t, uint8Boundaries>();
		checkToCharsMatchesStdToString<uint16_t, uint16Boundaries>();
		checkToCharsMatchesStdToString<uint32_t, uint32Boundaries>();
		checkToCharsMatchesStdToString<uint64_t, uint64Boundaries>();
		checkToCharsMatchesStdToString<int8_t, int8Boundaries>();
		checkToCharsMatchesStdToString<int16_t, int16Boundaries>();
		checkToCharsMatchesStdToString<int32_t, int32Boundaries>();
		checkToCharsMatchesStdToString<int64_t, int64Boundaries>();
		std::cout << "i_to_str Digit-Boundary tests complete." << std::endl;

		std::cout << "jsonifier::toString Narrow-Type Tests: " << std::endl;
		checkToStringMatchesStdToString<uint8_t, uint8Boundaries>();
		checkToStringMatchesStdToString<uint16_t, uint16Boundaries>();
		checkToStringMatchesStdToString<uint32_t, uint32Boundaries>();
		checkToStringMatchesStdToString<uint64_t, uint64Boundaries>();
		checkToStringMatchesStdToString<int8_t, int8Boundaries>();
		checkToStringMatchesStdToString<int16_t, int16Boundaries>();
		checkToStringMatchesStdToString<int32_t, int32Boundaries>();
		checkToStringMatchesStdToString<int64_t, int64Boundaries>();
		std::cout << "jsonifier::toString tests complete." << std::endl;
	}

}
