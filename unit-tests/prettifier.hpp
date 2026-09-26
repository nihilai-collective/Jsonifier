/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/prettifier.hpp
 */
#pragma once

#include "common.hpp"

namespace prettifier_tests {

	inline static void runTests() {
		std::cout << "JSON Prettifier Buffer Overload Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		auto prettifyInto = [&](std::string input, std::string buffer = std::string{}) {
			bool success = parser.prettifyJson(std::move(input), buffer);
			return success ? buffer : std::string{ "FAILED" };
		};

		auto buildNestedArrays = [](size_t depth) {
			std::string openBrackets(depth, '[');
			std::string closeBrackets(depth, ']');
			return openBrackets + "1" + closeBrackets;
		};

		auto buildExpectedNestedArrays = [](size_t depth) {
			std::string expected{};
			for (size_t x = 0; x < depth; ++x) {
				expected += std::string(x * 3, ' ') + "[\n";
			}
			expected += std::string(depth * 3, ' ') + "1\n";
			for (size_t x = depth; x > 0; --x) {
				expected += std::string((x - 1) * 3, ' ') + "]";
				if (x > 1) {
					expected += "\n";
				}
			}
			return expected;
		};

		rt_ut::unit_test<"buffer_prettify_simple_object", true>::assert_eq(std::string{ "{\n   \"a\": 1,\n   \"b\": 2\n}" }, [&] {
			return prettifyInto(std::string{ R"({"a":1,"b":2})" });
		});

		rt_ut::unit_test<"buffer_prettify_nested_structure", true>::assert_eq(
			std::string{ "{\n   \"a\": {\n      \"b\": [\n         1,\n         2,\n         3\n      ],\n      \"c\": \"hello world\"\n   }\n}" }, [&] {
				return prettifyInto(std::string{ R"({"a":{"b":[1,2,3],"c":"hello world"}})" });
			});

		rt_ut::unit_test<"buffer_prettify_empty_object", true>::assert_eq(std::string{ "{}" }, [&] {
			return prettifyInto(std::string{ "{}" });
		});

		rt_ut::unit_test<"buffer_prettify_empty_array", true>::assert_eq(std::string{ "[]" }, [&] {
			return prettifyInto(std::string{ "[]" });
		});

		rt_ut::unit_test<"buffer_prettify_array_of_objects", true>::assert_eq(std::string{ "[\n   {\n      \"a\": 1\n   },\n   {\n      \"b\": 2\n   }\n]" }, [&] {
			return prettifyInto(std::string{ R"([{"a":1},{"b":2}])" });
		});

		rt_ut::unit_test<"buffer_prettify_bools_and_null", true>::assert_eq(std::string{ "{\n   \"a\": true,\n   \"b\": false,\n   \"c\": null\n}" }, [&] {
			return prettifyInto(std::string{ R"({"a":true,"b":false,"c":null})" });
		});

		rt_ut::unit_test<"buffer_prettify_array_of_scalars", true>::assert_eq(std::string{ "[\n   1,\n   2,\n   3\n]" }, [&] {
			return prettifyInto(std::string{ R"([1,2,3])" });
		});

		rt_ut::unit_test<"buffer_prettify_deeply_nested_structure", true>::assert_eq(
			std::string{ "{\n   \"a\": {\n      \"b\": {\n         \"c\": [\n            1,\n            2\n         ]\n      }\n   }\n}" }, [&] {
				return prettifyInto(std::string{ R"({"a":{"b":{"c":[1,2]}}})" });
			});

		rt_ut::unit_test<"buffer_prettify_object_in_array_in_object", true>::assert_eq(
			std::string{ "{\n   \"a\": [\n      {\n         \"b\": 1\n      },\n      {\n         \"c\": 2\n      }\n   ]\n}" }, [&] {
				return prettifyInto(std::string{ R"({"a":[{"b":1},{"c":2}]})" });
			});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_string", true>::assert_eq(std::string{ R"("just a string")" }, [&] {
			return prettifyInto(std::string{ R"("just a string")" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_empty_string", true>::assert_eq(std::string{ R"("")" }, [&] {
			return prettifyInto(std::string{ R"("")" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_number", true>::assert_eq(std::string{ "42" }, [&] {
			return prettifyInto(std::string{ "42" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_negative_exponent_number", true>::assert_eq(std::string{ "-123.45e6" }, [&] {
			return prettifyInto(std::string{ "-123.45e6" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_true", true>::assert_eq(std::string{ "true" }, [&] {
			return prettifyInto(std::string{ "true" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_false", true>::assert_eq(std::string{ "false" }, [&] {
			return prettifyInto(std::string{ "false" });
		});

		rt_ut::unit_test<"buffer_prettify_bare_top_level_null", true>::assert_eq(std::string{ "null" }, [&] {
			return prettifyInto(std::string{ "null" });
		});

		rt_ut::unit_test<"buffer_prettify_custom_indent_size_and_char", true>::assert_eq(std::string{ "{\n\t\t\"a\":\t1,\n\t\t\"b\":\t[\n\t\t\t\t1,\n\t\t\t\t2\n\t\t]\n}" }, [&] {
			std::string buffer{};
			bool success = parser.prettifyJson<jsonifier::prettify_options{ .indentSize = 2, .indentChar = '\t' }>(std::string{ R"({"a":1,"b":[1,2]})" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"buffer_prettify_overwrites_larger_stale_buffer", true>::assert_eq(std::string{ "{\n   \"a\": 1,\n   \"b\": 2\n}" }, [&] {
			return prettifyInto(std::string{ R"({"a":1,"b":2})" }, std::string(4096, 'X'));
		});

		rt_ut::unit_test<"buffer_prettify_grows_smaller_stale_buffer", true>::assert_eq(std::string{ "{\n   \"a\": 1,\n   \"b\": 2\n}" }, [&] {
			return prettifyInto(std::string{ R"({"a":1,"b":2})" }, std::string{ "ab" });
		});

		rt_ut::unit_test<"buffer_prettify_exact_size_buffer_no_overrun", true>::assert_eq(std::string{ "false" }, [&] {
			std::string buffer{};
			buffer.resize(5);
			bool success = parser.prettifyJson(std::string{ "false" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"buffer_prettify_reused_buffer_shrinks_to_second_output", true>::assert_eq(std::string{ "42" }, [&] {
			std::string buffer{};
			bool firstSuccess  = parser.prettifyJson(std::string{ R"({"a":{"b":[1,2,3],"c":"hello world"}})" }, buffer);
			bool secondSuccess = parser.prettifyJson(std::string{ "42" }, buffer);
			return (firstSuccess && secondSuccess) ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"buffer_prettify_nested_depth_10_exact", true>::assert_eq(buildExpectedNestedArrays(10), [&] {
			return prettifyInto(buildNestedArrays(10));
		});

		rt_ut::unit_test<"buffer_prettify_nested_depth_50_exact", true>::assert_eq(buildExpectedNestedArrays(50), [&] {
			return prettifyInto(buildNestedArrays(50));
		});

		rt_ut::unit_test<"buffer_prettify_nested_depth_650_exceeds_initial_string_buffer", true>::assert_eq(buildExpectedNestedArrays(650), [&] {
			return prettifyInto(buildNestedArrays(650));
		});

		rt_ut::unit_test<"buffer_prettify_empty_input_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.prettifyJson(std::string{ "" }, buffer);
			return !success && !parser.getErrors().empty();
		});

		rt_ut::unit_test<"buffer_prettify_empty_input_leaves_buffer_untouched", true>::assert_eq(std::string{ "stale" }, [&] {
			std::string buffer{ "stale" };
			parser.prettifyJson(std::string{ "" }, buffer);
			return buffer;
		});

		rt_ut::unit_test<"buffer_prettify_unclosed_array_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.prettifyJson(std::string{ "[" }, buffer);
			return !success && !parser.getErrors().empty() && buffer.empty();
		});

		rt_ut::unit_test<"buffer_prettify_unclosed_object_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.prettifyJson(std::string{ "{" }, buffer);
			return !success && !parser.getErrors().empty() && buffer.empty();
		});

		rt_ut::unit_test<"buffer_prettify_errors_cleared_after_subsequent_success", true>::assert_eq(true, [&] {
			std::string buffer{};
			bool failed	   = !parser.prettifyJson(std::string{ "[" }, buffer);
			bool succeeded = parser.prettifyJson(std::string{ R"({"a":1})" }, buffer);
			return failed && succeeded && parser.getErrors().empty() && buffer == "{\n   \"a\": 1\n}";
		});

		std::cout << "JSON Prettifier validation tests complete." << std::endl;
	}

}
