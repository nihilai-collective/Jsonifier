/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/float.hpp
 */
#pragma once

#include "common.hpp"

namespace float_validation_tests {

	inline constexpr jsonifier::internal::array<std::string_view, 64> inputValues{ { "0.0", "-0.0", "1.0", "-1.0", "1.5", "-1.5", "3.1416", "1E10", "1e10", "1E+10", "1E-10",
		"-1E10", "-1e10", "-1E+10", "-1E-10", "1.234E+10", "1.234E-10", "1.79769e+308", "2.22507e-308", "-1.79769e+308", "-2.22507e-308", "4.9406564584124654e-324",
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

	inline constexpr jsonifier::internal::array<double, 64> outputValues{ { 0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 3.1416, 1E10, 1e10, 1E+10, 1E-10, -1E10, -1e10, -1E+10, -1E-10,
		1.234E+10, 1.234E-10, 1.79769e+308, 2.22507e-308, -1.79769e+308, -2.22507e-308, 4.9406564584124654e-324, 2.2250738585072009e-308, 2.2250738585072014e-308,
		1.7976931348623157e+308, 0.0, 18446744073709551616.0, -9223372036854775809.0, 0.9868011474609375, 123e34, 45913141877270640000.0, 2.2250738585072011e-308, 0.0, 0.0,
		1.7976931348623157e+308, 2.2250738585072014e-308, 2.2250738585072009e-308, 2.2250738585072014e-308, 1.0, 0.99999999999999989, 1.0, 1.0, 1.0, 1.00000000000000022,
		72057594037927928.0, 72057594037927936.0, 72057594037927936.0, 72057594037927928.0, 72057594037927936.0, 9223372036854774784.0, 9223372036854775808.0,
		9223372036854775808.0, 9223372036854774784.0, 9223372036854775808.0, 10141204801825834086073718800384.0, 10141204801825835211973625643008.0,
		10141204801825835211973625643008.0, 10141204801825834086073718800384.0, 10141204801825835211973625643008.0, 5708990770823838890407843763683279797179383808.0,
		5708990770823839524233143877797980545530986496.0, 5708990770823839524233143877797980545530986496.0, 5708990770823838890407843763683279797179383808.0,
		5708990770823839524233143877797980545530986496.0 } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline void floatTestsImpl() {
		std::cout << "Float Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<double, double, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		return;
	}

	template<bool nullTerminated> inline void genericFloatTestsImpl() {
		std::cout << "Generic Float Tests, " << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<double, double, inputValues, outputValues, false, false, nullTerminated, generic_number_pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
	}

	inline void zmijSerializationTests() {
		std::cout << "Zmij Double Serialization Tests: " << std::endl;
		jsonifier::jsonifier_core<> parser{};

		auto serialize = [&](double value) {
			std::string buffer{};
			parser.serializeJson(value, buffer);
			for (auto& err: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << err.reportError() << std::endl;
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
				std::cout << "Jsonifier Error: " << err.reportError() << std::endl;
			}
			double reparsed{};
			parser.parseJson(reparsed, buffer);
			for (auto& err: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << err.reportError() << std::endl;
			}
			rt_ut::unit_test<"zmij_roundtrip_fidelity_of_parsed_values", true>::assert_eq(outputValues[i], [&]() {
				return reparsed;
			});
		}

		std::cout << "Zmij Double Serialization tests complete." << std::endl;
	}

	struct reference_double {
		static double impl(std::string_view text) {
#if defined(__cpp_lib_to_chars)
			double expected{};
			std::from_chars(text.data(), text.data() + text.size(), expected);
			return expected;
#else
			return std::strtod(std::string{ text }.c_str(), nullptr);
#endif
		}
	};

	template<jsonifier::internal::number_shapes shape> struct shape_checker {
		static bool impl(const std::string& line, uint64_t expectedBits, std::string_view shapeName) {
			double parsed{};
			const jsonifier::internal::read_buffer_ptr begin = std::bit_cast<jsonifier::internal::read_buffer_ptr>(line.data());
			const jsonifier::internal::read_buffer_ptr end	 = begin + line.size();
			const auto parsedEnd							 = jsonifier::internal::float_parser<double>::template parseFloat<shape>(parsed, begin, end);
			if (parsedEnd != end || std::bit_cast<uint64_t>(parsed) != expectedBits) {
				std::cout << "Shape " << shapeName << " mismatch on \"" << line << "\": got " << std::setprecision(17) << parsed << ", expected "
						  << std::bit_cast<double>(expectedBits) << std::endl;
				return false;
			}
			return true;
		}
	};

	struct corpus_line_checker {
		static bool impl(jsonifier::jsonifier_core<>& parser, const std::string& line) {
			double parsed{};
			parser.parseJson(parsed, line);
			if (!parser.getErrors().empty()) {
				for (auto& err: parser.getErrors()) {
					std::cout << "Jsonifier Error: " << err.reportError() << std::endl;
				}
				return false;
			}
			const double expected		= reference_double::impl(line);
			const uint64_t expectedBits = std::bit_cast<uint64_t>(expected);
			bool result					= true;
			if (std::bit_cast<uint64_t>(parsed) != expectedBits) {
				std::cout << "Corpus mismatch on \"" << line << "\": got " << std::setprecision(17) << parsed << ", expected " << expected << std::endl;
				result = false;
			}
			result &= shape_checker<jsonifier::internal::number_shapes::general>::impl(line, expectedBits, "general");
			result &= shape_checker<jsonifier::internal::number_shapes::short_fraction>::impl(line, expectedBits, "short_fraction");
			result &= shape_checker<jsonifier::internal::number_shapes::long_integer>::impl(line, expectedBits, "long_integer");
			return result;
		}
	};

	struct array_layout {
		std::string_view separator;
		std::string_view prefix;
		std::string_view suffix;
	};

	inline constexpr jsonifier::internal::array<array_layout, 3> arrayLayouts{ { { ",", "", "" }, { ", ", " ", " " }, { ",\n    ", "\n    ", "\n" } } };

	struct array_path_checker {
		static bool impl(const std::vector<std::string>& values, const array_layout& layout) {
			std::string text{ "[" };
			text += layout.prefix;
			for (uint64_t i = 0; i < values.size(); ++i) {
				if (i > 0) {
					text += layout.separator;
				}
				text += values[i];
			}
			text += layout.suffix;
			text += "]";
			jsonifier::jsonifier_core<> parser{};
			std::vector<double> typed{};
			parser.parseJson(typed, text);
			bool result = true;
			if (!parser.getErrors().empty()) {
				for (auto& err: parser.getErrors()) {
					std::cout << "Jsonifier Error: " << err.reportError() << std::endl;
				}
				result = false;
			}
			std::vector<double> genericValues{};
			if (const auto errorCode = parser.iterate(text).getArray(genericValues); errorCode != jsonifier::generic::parse_statuses::success) {
				std::cout << "Generic getArray<double> failed with code " << static_cast<uint64_t>(errorCode) << " on " << values.size() << " values." << std::endl;
				result = false;
			}
			result &= array_contents_checker::impl(values, typed, "typed");
			result &= array_contents_checker::impl(values, genericValues, "generic");
			return result;
		}

		struct array_contents_checker {
			static bool impl(const std::vector<std::string>& values, const std::vector<double>& parsed, std::string_view pathName) {
				if (parsed.size() != values.size()) {
					std::cout << pathName << " array size mismatch: got " << parsed.size() << ", expected " << values.size() << std::endl;
					return false;
				}
				uint64_t failureCount{};
				for (uint64_t i = 0; i < values.size(); ++i) {
					const double expected = reference_double::impl(values[i]);
					if (std::bit_cast<uint64_t>(parsed[i]) != std::bit_cast<uint64_t>(expected)) {
						if (failureCount < 8) {
							std::cout << pathName << " array mismatch at index " << i << " on \"" << values[i] << "\": got " << std::setprecision(17) << parsed[i] << ", expected "
									  << expected << std::endl;
						}
						++failureCount;
					}
				}
				return failureCount == 0;
			}
		};
	};

	struct array_layouts_checker {
		static bool impl(const std::vector<std::string>& values) {
			bool result = true;
			for (const auto& layout: arrayLayouts) {
				result &= array_path_checker::impl(values, layout);
			}
			return result;
		}
	};

	struct corpus_file_checker {
		static bool impl(const std::string& fileName) {
			const std::string contents = file_handler::get(basePath.operator std::string() + "/fast_float_corpus/" + fileName);
			if (contents.empty()) {
				return false;
			}
			jsonifier::jsonifier_core<> parser{};
			std::istringstream stream{ contents };
			std::vector<std::string> values{};
			std::string line{};
			uint64_t failureCount{};
			while (std::getline(stream, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
					line.pop_back();
				}
				if (line.empty()) {
					continue;
				}
				if (!corpus_line_checker::impl(parser, line)) {
					++failureCount;
				}
				values.emplace_back(line);
			}
			std::cout << fileName << ": " << values.size() - failureCount << "/" << values.size() << " values matched across root and all shapes." << std::endl;
			const bool arraysMatched = array_layouts_checker::impl(values);
			std::cout << fileName << ": typed and generic array paths " << (arraysMatched ? "matched." : "MISMATCHED.") << std::endl;
			return !values.empty() && failureCount == 0 && arraysMatched;
		}
	};

	struct adversarial_arrays_builder {
		static std::vector<std::vector<std::string>> impl() {
			return {
				{},
				{ "1.5" },
				{ "-0.5" },
				{ "0" },
				{ "1.5", "0.12345678901234567", "3.1415926535897932", "-2.7182818284590452", "1.7976931348623157e308", "2.2250738585072014e-308" },
				{ "0.12345678", "0.87654321", "-0.1111111", "9.99999999" },
				{ "0.1234567", "0.12345678", "0.123456789", "0.1234567890123456789" },
				{ "-1.5e-300", "1.5e300", "2.5E+10", "-9.5e-5", "4.9e-324", "1e-400", "-1e0" },
				{ "1.5", "123456789012345678901234567890", "0.000000000000000000000000000001", "18446744073709551616" },
				{ "12", "1.5", "-7", "0.25" },
				{ "1234567", "1.5", "0.001", "-98765432109876543" },
				{ "1.5", "2", "3.0", "4e1", "5E-1", "-6.25" },
			};
		}
	};

	struct adversarial_arrays_checker {
		static bool impl() {
			const std::vector<std::vector<std::string>> adversarialArrays = adversarial_arrays_builder::impl();
			bool result													  = true;
			for (uint64_t i = 0; i < adversarialArrays.size(); ++i) {
				if (!array_layouts_checker::impl(adversarialArrays[i])) {
					std::cout << "Adversarial array case " << i << " failed." << std::endl;
					result = false;
				}
				for (const auto& value: adversarialArrays[i]) {
					jsonifier::jsonifier_core<> parser{};
					result &= corpus_line_checker::impl(parser, value);
				}
			}
			return result;
		}
	};

	inline void fastFloatCorpusTests() {
		std::cout << "Fast Float Corpus Double Parsing Tests: " << std::endl;
		rt_ut::unit_test<"fast_float_corpus_canada", true>::assert_eq(true, corpus_file_checker::impl, std::string{ "canada.txt" });
		rt_ut::unit_test<"fast_float_corpus_canada_short", true>::assert_eq(true, corpus_file_checker::impl, std::string{ "canada_short.txt" });
		rt_ut::unit_test<"fast_float_corpus_contrived", true>::assert_eq(true, corpus_file_checker::impl, std::string{ "contrived.txt" });
		rt_ut::unit_test<"fast_float_corpus_mesh", true>::assert_eq(true, corpus_file_checker::impl, std::string{ "mesh.txt" });
		rt_ut::unit_test<"float_array_adversarial_shapes", true>::assert_eq(true, adversarial_arrays_checker::impl);
		std::cout << "Fast Float Corpus Double Parsing tests complete." << std::endl;
	}

	inline void runTests() {
		floatTestsImpl<false, false, false>();
		floatTestsImpl<false, true, false>();
		floatTestsImpl<true, false, false>();
		floatTestsImpl<true, true, false>();
		floatTestsImpl<false, false, true>();
		floatTestsImpl<false, true, true>();
		floatTestsImpl<true, false, true>();
		floatTestsImpl<true, true, true>();
		genericFloatTestsImpl<false>();
		genericFloatTestsImpl<true>();
		zmijSerializationTests();
		fastFloatCorpusTests();
		std::cout << "Float validation tests complete." << std::endl;
	}

}
