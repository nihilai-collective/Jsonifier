// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/float.hpp
#pragma once

#include "common.hpp"

namespace float_validation_tests {

	constexpr jsonifier::internal::array<std::string_view, 64> inputValues{ { "0.0", "-0.0", "1.0", "-1.0", "1.5", "-1.5", "3.1416", "1E10", "1e10", "1E+10", "1E-10", "-1E10",
		"-1e10", "-1E+10", "-1E-10", "1.234E+10", "1.234E-10", "1.79769e+308", "2.22507e-308", "-1.79769e+308", "-2.22507e-308", "4.9406564584124654e-324",
		"2.2250738585072009e-308", "2.2250738585072014e-308", "1.7976931348623157e+308", "1e-10000", "18446744073709551616", "-9223372036854775809", "0.9868011474609375", "123e34",
		"45913141877270640000.0", "2.2250738585072011e-308", "1e-214748363", "1e-214748364", "0.017976931348623157e+310", "2.2250738585072012e-308",
		"2.22507385850720113605740979670913197593481954635164564e-308", "2.22507385850720113605740979670913197593481954635164565e-308",
		"0.999999999999999944488848768742172978818416595458984375", "0.999999999999999944488848768742172978818416595458984374",
		"0.999999999999999944488848768742172978818416595458984376", "1.00000000000000011102230246251565404236316680908203125",
		"1.00000000000000011102230246251565404236316680908203124", "1.00000000000000011102230246251565404236316680908203126", "72057594037927928.0", "72057594037927936.0",
		"72057594037927932.0", "7205759403792793199999e-5", "7205759403792793200001e-5", "9223372036854774784.0", "9223372036854775808.0", "9223372036854775296.0",
		"922337203685477529599999e-5", "922337203685477529600001e-5", "10141204801825834086073718800384", "10141204801825835211973625643008", "10141204801825834649023672221696",
		"1014120480182583464902367222169599999e-5", "1014120480182583464902367222169600001e-5", "5708990770823838890407843763683279797179383808",
		"5708990770823839524233143877797980545530986496", "5708990770823839207320493820740630171355185152", "5708990770823839207320493820740630171355185151999e-3",
		"5708990770823839207320493820740630171355185152001e-3" } };

	constexpr jsonifier::internal::array<double, 64> outputValues{ { 0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 3.1416, 1E10, 1e10, 1E+10, 1E-10, -1E10, -1e10, -1E+10, -1E-10, 1.234E+10,
		1.234E-10, 1.79769e+308, 2.22507e-308, -1.79769e+308, -2.22507e-308, 4.9406564584124654e-324, 2.2250738585072009e-308, 2.2250738585072014e-308, 1.7976931348623157e+308,
		0.0, 18446744073709551616.0, -9223372036854775809.0, 0.9868011474609375, 123e34, 45913141877270640000.0, 2.2250738585072011e-308, 0.0, 0.0, 1.7976931348623157e+308,
		2.2250738585072014e-308, 2.2250738585072009e-308, 2.2250738585072014e-308, 1.0, 0.99999999999999989, 1.0, 1.0, 1.0, 1.00000000000000022, 72057594037927928.0,
		72057594037927936.0, 72057594037927936.0, 72057594037927928.0, 72057594037927936.0, 9223372036854774784.0, 9223372036854775808.0, 9223372036854775808.0,
		9223372036854774784.0, 9223372036854775808.0, 10141204801825834086073718800384.0, 10141204801825835211973625643008.0, 10141204801825835211973625643008.0,
		10141204801825834086073718800384.0, 10141204801825835211973625643008.0, 5708990770823838890407843763683279797179383808.0, 5708990770823839524233143877797980545530986496.0,
		5708990770823839524233143877797980545530986496.0, 5708990770823838890407843763683279797179383808.0, 5708990770823839524233143877797980545530986496.0 } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void floatTestsImpl() {
		std::cout << "Float Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<double, double, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		return;
	}

	inline static void zmijSerializationTests() {
		std::cout << "Zmij Double Serialization Tests: " << std::endl;
		jsonifier::jsonifier_core<> parser{};

		auto serialize = [&](double value) {
			std::string buffer{};
			parser.serializeJson(value, buffer);
			for (auto& err: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << err << std::endl;
			}
			return buffer;
		};

		rt_ut::unit_test<"zmij_zero", true>::assert_eq(std::string{ "0" }, serialize, 0.0);
		rt_ut::unit_test<"zmij_negative_zero", true>::assert_eq(std::string{ "-0" }, serialize, -0.0);
		rt_ut::unit_test<"zmij_whole_number_no_trailing_point", true>::assert_eq(std::string{ "100" }, serialize, 100.0);
		rt_ut::unit_test<"zmij_simple_fraction", true>::assert_eq(std::string{ "1.5" }, serialize, 1.5);
		rt_ut::unit_test<"zmij_leading_zero_fraction", true>::assert_eq(std::string{ "0.5" }, serialize, 0.5);
		rt_ut::unit_test<"zmij_negative_fraction", true>::assert_eq(std::string{ "-10.5" }, serialize, -10.5);
		rt_ut::unit_test<"zmij_small_fixed_notation", true>::assert_eq(std::string{ "0.001" }, serialize, 0.001);
		rt_ut::unit_test<"zmij_fixed_notation_upper_bound", true>::assert_eq(std::string{ "0.0001" }, serialize, 1e-4);
		rt_ut::unit_test<"zmij_scientific_notation_lower_bound", true>::assert_eq(std::string{ "1e-05" }, serialize, 1e-5);
		rt_ut::unit_test<"zmij_scientific_small_significand", true>::assert_eq(std::string{ "9.999e-05" }, serialize, 9.999e-5);
		rt_ut::unit_test<"zmij_scientific_notation_positive_exp", true>::assert_eq(std::string{ "1e+20" }, serialize, 1e20);
		rt_ut::unit_test<"zmij_scientific_notation_three_digit_exp", true>::assert_eq(std::string{ "1e+21" }, serialize, 1e21);
		rt_ut::unit_test<"zmij_scientific_many_sig_figs", true>::assert_eq(std::string{ "1.23456789e+21" }, serialize, 1.23456789e21);
		rt_ut::unit_test<"zmij_negative_scientific_negative_exp", true>::assert_eq(std::string{ "-1.23456789e-21" }, serialize, -1.23456789e-21);
		rt_ut::unit_test<"zmij_max_double", true>::assert_eq(std::string{ "1.7976931348623157e+308" }, serialize, std::numeric_limits<double>::max());
		rt_ut::unit_test<"zmij_min_normal_double", true>::assert_eq(std::string{ "2.2250738585072014e-308" }, serialize, std::numeric_limits<double>::min());
		rt_ut::unit_test<"zmij_smallest_denormal", true>::assert_eq(std::string{ "5e-324" }, serialize, std::numeric_limits<double>::denorm_min());
		rt_ut::unit_test<"zmij_unrepresentable_value_rounds_down", true>::assert_eq(std::string{ "9007199254740992" }, serialize, 9007199254740993.0);
		rt_ut::unit_test<"zmij_large_integer_switches_to_scientific", true>::assert_eq(std::string{ "1.2345678901234568e+17" }, serialize, 123456789012345680.0);

		for (uint64_t i = 0; i < outputValues.size(); ++i) {
			std::string buffer{};
			parser.serializeJson(outputValues[i], buffer);
			for (auto& err: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << err << std::endl;
			}
			double reparsed{};
			parser.parseJson(reparsed, buffer);
			for (auto& err: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << err << std::endl;
			}
			rt_ut::unit_test<"zmij_roundtrip_fidelity_of_parsed_values", true>::assert_eq(outputValues[i], [&]() {
				return reparsed;
			});
		}

		std::cout << "Zmij Double Serialization tests complete." << std::endl;
	}

	inline static void runTests() {
		floatTestsImpl<false, false, false>();
		floatTestsImpl<false, true, false>();
		floatTestsImpl<true, false, false>();
		floatTestsImpl<true, true, false>();
		floatTestsImpl<false, false, true>();
		floatTestsImpl<false, true, true>();
		floatTestsImpl<true, false, true>();
		floatTestsImpl<true, true, true>();
		zmijSerializationTests();
		std::cout << "Float validation tests complete." << std::endl;
	}

}
