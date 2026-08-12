// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/conformance.hpp
#pragma once

#include "common.hpp"

namespace conformance_tests {

	template<rt_ut::string_literal testNameNew, bool partial, bool knownOrder, bool nullTerminated, typename test_type,
		jsonifier::internal::parse_statuses parse_error = jsonifier::internal::parse_statuses{}>
	inline static void runConformanceTest(const std::string& dataToParse, jsonifier::jsonifier_core<>& parser) {
		static constexpr rt_ut::string_literal testName{ testNameNew + ", " + testTypePartial<partial> + testTypeKnownOrder<knownOrder> + testTypeNullTerminated<nullTerminated> };
		[[maybe_unused]] test_type valueNew{};
		parser.parseJson<jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder, .validateUtf8 = true, .nullTerminated = nullTerminated }>(valueNew,
			dataToParse);
		rt_ut::unit_test<testName, true>::template assert_eq<parse_error>([&]() {
			auto& errors = parser.getErrors();
			for (auto& value: errors) {
				std::cout << "Error: " << value << std::endl;
			}
			if (errors.size() > 0) {
				return errors[0].operator jsonifier::internal::parse_statuses();
			} else {
				return jsonifier::internal::parse_statuses::success;
			}
		});
	}

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void conformanceTestsImpl() {
		jsonifier::jsonifier_core<> parser{};
		{
			std::unordered_map<std::string, test_base> jsonTests{};
			processFilesInFolder(jsonTests, "jsonchecker/fail");
			std::cout << "Conformance Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
			runConformanceTest<"fail02.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail02.json"].fileContents, parser);
			runConformanceTest<"fail03.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail03.json"].fileContents, parser);
			runConformanceTest<"fail04.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail04.json"].fileContents, parser);
			runConformanceTest<"fail05.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail05.json"].fileContents, parser);
			runConformanceTest<"fail06.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail06.json"].fileContents, parser);
			runConformanceTest<"fail07.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail07.json"].fileContents, parser);
			runConformanceTest<"fail08.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail08.json"].fileContents, parser);
			runConformanceTest<"fail09.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, bool>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail09.json"].fileContents, parser);
			runConformanceTest<"fail10.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, bool>, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail10.json"].fileContents, parser);
			runConformanceTest<"fail11.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, int32_t>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail11.json"].fileContents, parser);
			runConformanceTest<"fail12.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail12.json"].fileContents, parser);
			runConformanceTest<"fail13.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, int32_t>,
				jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["fail13.json"].fileContents, parser);
			runConformanceTest<"fail14.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, int32_t>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail14.json"].fileContents, parser);
			runConformanceTest<"fail15.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail15.json"].fileContents, parser);
			runConformanceTest<"fail16.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail16.json"].fileContents, parser);
			runConformanceTest<"fail17.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail17.json"].fileContents, parser);
			runConformanceTest<"fail19.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::missing_colon>(jsonTests["fail19.json"].fileContents, parser);
			runConformanceTest<"fail20.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail20.json"].fileContents, parser);
			runConformanceTest<"fail21.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::missing_colon>(jsonTests["fail21.json"].fileContents, parser);
			runConformanceTest<"fail22.json", partial, knownOrder, nullTerminated, std::vector<bool>, jsonifier::internal::parse_statuses::invalid_bool_value>(
				jsonTests["fail22.json"].fileContents, parser);
			runConformanceTest<"fail23.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail23.json"].fileContents, parser);
			runConformanceTest<"fail24.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail24.json"].fileContents, parser);
			runConformanceTest<"fail25.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail25.json"].fileContents, parser);
			runConformanceTest<"fail26.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail26.json"].fileContents, parser);
			runConformanceTest<"fail27.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail27.json"].fileContents, parser);
			runConformanceTest<"fail28.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail28.json"].fileContents, parser);
			runConformanceTest<"fail29.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail29.json"].fileContents, parser);
			runConformanceTest<"fail30.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail30.json"].fileContents, parser);
			runConformanceTest<"fail31.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail31.json"].fileContents, parser);
			runConformanceTest<"fail32.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, bool>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail32.json"].fileContents, parser);
			runConformanceTest<"fail33.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail33.json"].fileContents, parser);
			runConformanceTest<"fail34.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail34.json"].fileContents, parser);
			runConformanceTest<"fail35.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail35.json"].fileContents, parser);
			runConformanceTest<"fail36.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail36.json"].fileContents, parser);
			runConformanceTest<"fail37.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail37.json"].fileContents, parser);
			runConformanceTest<"fail38.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail38.json"].fileContents, parser);
			runConformanceTest<"fail42.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail42.json"].fileContents, parser);
			runConformanceTest<"fail43.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail43.json"].fileContents, parser);
			runConformanceTest<"fail44.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail44.json"].fileContents, parser);
			runConformanceTest<"fail45.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail45.json"].fileContents, parser);
			runConformanceTest<"fail46.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail46.json"].fileContents, parser);
			runConformanceTest<"fail47.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail47.json"].fileContents, parser);
			runConformanceTest<"fail48.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail48.json"].fileContents, parser);
			runConformanceTest<"fail49.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail49.json"].fileContents, parser);
			runConformanceTest<"fail50.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail50.json"].fileContents, parser);
			runConformanceTest<"fail51.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail51.json"].fileContents, parser);
			runConformanceTest<"fail52.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail52.json"].fileContents, parser);
			runConformanceTest<"fail53.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["fail53.json"].fileContents, parser);
			runConformanceTest<"fail54.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail54.json"].fileContents, parser);
			runConformanceTest<"fail55.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail55.json"].fileContents, parser);
			runConformanceTest<"fail56.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail56.json"].fileContents, parser);
			runConformanceTest<"fail57.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail57.json"].fileContents, parser);
			runConformanceTest<"fail58.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail58.json"].fileContents, parser);
			runConformanceTest<"fail59.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail59.json"].fileContents, parser);
			runConformanceTest<"fail60.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail60.json"].fileContents, parser);
			runConformanceTest<"fail61.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail61.json"].fileContents, parser);
			runConformanceTest<"fail62.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail62.json"].fileContents, parser);
			runConformanceTest<"fail63.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail63.json"].fileContents, parser);
			runConformanceTest<"fail64.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail64.json"].fileContents, parser);
			runConformanceTest<"fail65.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::invalid_bool_value>(
				jsonTests["fail65.json"].fileContents, parser);
			runConformanceTest<"fail66.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail66.json"].fileContents, parser);
			runConformanceTest<"fail67.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail67.json"].fileContents, parser);
			runConformanceTest<"fail68.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail68.json"].fileContents, parser);
			runConformanceTest<"fail69.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::invalid_bool_value>(
				jsonTests["fail69.json"].fileContents, parser);
			runConformanceTest<"fail70.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::no_input>(
				jsonTests["fail70.json"].fileContents, parser);
			runConformanceTest<"fail71.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail71.json"].fileContents, parser);
			runConformanceTest<"fail72.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail72.json"].fileContents, parser);
			runConformanceTest<"fail73.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail73.json"].fileContents, parser);
			runConformanceTest<"fail74.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail74.json"].fileContents, parser);
			runConformanceTest<"fail75.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::invalid_bool_value>(
				jsonTests["fail75.json"].fileContents, parser);
			runConformanceTest<"fail76.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail76.json"].fileContents, parser);
			runConformanceTest<"fail77.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
				jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["fail77.json"].fileContents, parser);
			runConformanceTest<"fail78.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::invalid_number_value>(
				jsonTests["fail78.json"].fileContents, parser);
			runConformanceTest<"fail79.json", partial, knownOrder, nullTerminated, std::vector<int32_t>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail79.json"].fileContents, parser);
			runConformanceTest<"fail80.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, double>, jsonifier::internal::parse_statuses::missing_comma>(
				jsonTests["fail80.json"].fileContents, parser);
			runConformanceTest<"fail81.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::unfinished_input>(
				jsonTests["fail81.json"].fileContents, parser);
			runConformanceTest<"fail82.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::invalid_string_characters>(
				jsonTests["fail82.json"].fileContents, parser);
		}
		{
			std::unordered_map<std::string, test_base> jsonTests{};
			processFilesInFolder(jsonTests, "jsonchecker/pass");
			std::cout << "Conformance Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
			runConformanceTest<"pass01.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass01.json"].fileContents, parser);
			runConformanceTest<"pass02.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass02.json"].fileContents, parser);
			runConformanceTest<"pass03.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass03.json"].fileContents, parser);
			runConformanceTest<"pass04.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass04.json"].fileContents, parser);
			runConformanceTest<"pass05.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass05.json"].fileContents,
				parser);
			runConformanceTest<"pass06.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::success>(jsonTests["pass06.json"].fileContents,
				parser);
			runConformanceTest<"pass07.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass07.json"].fileContents, parser);
			runConformanceTest<"pass08.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass08.json"].fileContents,
				parser);
			runConformanceTest<"pass09.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::success>(jsonTests["pass09.json"].fileContents,
				parser);
			runConformanceTest<"pass10.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::success>(jsonTests["pass10.json"].fileContents,
				parser);
			runConformanceTest<"pass11.json", partial, knownOrder, nullTerminated, uint64_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass11.json"].fileContents,
				parser);
			runConformanceTest<"pass12.json", partial, knownOrder, nullTerminated, uint64_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass12.json"].fileContents,
				parser);
			runConformanceTest<"pass13.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass13.json"].fileContents,
				parser);
			runConformanceTest<"pass14.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, int32_t>, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass14.json"].fileContents, parser);
			runConformanceTest<"pass15.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass15.json"].fileContents, parser);
			runConformanceTest<"pass16.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass16.json"].fileContents,
				parser);
			runConformanceTest<"pass17.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass17.json"].fileContents, parser);
			runConformanceTest<"pass18.json", partial, knownOrder, nullTerminated, std::vector<jsonifier::raw_json_data>, jsonifier::internal::parse_statuses::success>(
				jsonTests["pass18.json"].fileContents, parser);
			runConformanceTest<"pass19.json", partial, knownOrder, nullTerminated, int32_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass19.json"].fileContents,
				parser);
			runConformanceTest<"pass20.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass20.json"].fileContents,
				parser);
			runConformanceTest<"pass21.json", partial, knownOrder, nullTerminated, uint64_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass21.json"].fileContents,
				parser);
			runConformanceTest<"pass22.json", partial, knownOrder, nullTerminated, uint64_t, jsonifier::internal::parse_statuses::success>(jsonTests["pass22.json"].fileContents,
				parser);
			runConformanceTest<"pass23.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass23.json"].fileContents,
				parser);
			runConformanceTest<"pass24.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass24.json"].fileContents,
				parser);
			runConformanceTest<"pass25.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass25.json"].fileContents,
				parser);
			runConformanceTest<"pass26.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass26.json"].fileContents,
				parser);
			runConformanceTest<"pass27.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(jsonTests["pass27.json"].fileContents,
				parser);
		}
	}

	inline static void runTests() {
		conformanceTestsImpl<false, false, false>();
		conformanceTestsImpl<false, true, false>();
		conformanceTestsImpl<true, false, false>();
		conformanceTestsImpl<true, true, false>();
		conformanceTestsImpl<false, false, true>();
		conformanceTestsImpl<false, true, true>();
		conformanceTestsImpl<true, false, true>();
		conformanceTestsImpl<true, true, true>();
		std::cout << "Conformance validation tests complete." << std::endl;
	}

}
