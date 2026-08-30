// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/prettifier.hpp
#pragma once

#include "common.hpp"

namespace prettifier_tests {

	inline static void runTests() {
		std::cout << "JSON Prettifier Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		rt_ut::unit_test<"prettify_simple_object", true>::assert_eq(std::string{ "{\n   \"a\": 1,\n   \"b\": 2\n}" }, [&] {
			return parser.prettifyJson(std::string{ R"({"a":1,"b":2})" });
		});

		rt_ut::unit_test<"prettify_nested_structure", true>::assert_eq(
			std::string{ "{\n   \"a\": {\n      \"b\": [\n         1,\n         2,\n         3\n      ],\n      \"c\": \"hello world\"\n   }\n}" }, [&] {
				return parser.prettifyJson(std::string{ R"({"a":{"b":[1,2,3],"c":"hello world"}})" });
			});

		rt_ut::unit_test<"prettify_empty_object_has_no_inner_newline", true>::assert_eq(std::string{ "{}" }, [&] {
			return parser.prettifyJson(std::string{ "{}" });
		});

		rt_ut::unit_test<"prettify_empty_array_has_no_inner_newline", true>::assert_eq(std::string{ "[]" }, [&] {
			return parser.prettifyJson(std::string{ "[]" });
		});

		rt_ut::unit_test<"prettify_array_of_objects", true>::assert_eq(std::string{ "[\n   {\n      \"a\": 1\n   },\n   {\n      \"b\": 2\n   }\n]" }, [&] {
			return parser.prettifyJson(std::string{ R"([{"a":1},{"b":2}])" });
		});

		rt_ut::unit_test<"prettify_bools_and_null", true>::assert_eq(std::string{ "{\n   \"a\": true,\n   \"b\": false,\n   \"c\": null\n}" }, [&] {
			return parser.prettifyJson(std::string{ R"({"a":true,"b":false,"c":null})" });
		});

		rt_ut::unit_test<"prettify_array_of_scalars", true>::assert_eq(std::string{ "[\n   1,\n   2,\n   3\n]" }, [&] {
			return parser.prettifyJson(std::string{ R"([1,2,3])" });
		});

		rt_ut::unit_test<"prettify_deeply_nested_structure", true>::assert_eq(
			std::string{ "{\n   \"a\": {\n      \"b\": {\n         \"c\": [\n            1,\n            2\n         ]\n      }\n   }\n}" }, [&] {
				return parser.prettifyJson(std::string{ R"({"a":{"b":{"c":[1,2]}}})" });
			});

		rt_ut::unit_test<"prettify_object_in_array_in_object", true>::assert_eq(
			std::string{ "{\n   \"a\": [\n      {\n         \"b\": 1\n      },\n      {\n         \"c\": 2\n      }\n   ]\n}" }, [&] {
				return parser.prettifyJson(std::string{ R"({"a":[{"b":1},{"c":2}]})" });
			});

		rt_ut::unit_test<"prettify_bare_top_level_string", true>::assert_eq(std::string{ R"("just a string")" }, [&] {
			return parser.prettifyJson(std::string{ R"("just a string")" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_empty_string", true>::assert_eq(std::string{ R"("")" }, [&] {
			return parser.prettifyJson(std::string{ R"("")" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_number", true>::assert_eq(std::string{ "42" }, [&] {
			return parser.prettifyJson(std::string{ "42" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_negative_exponent_number", true>::assert_eq(std::string{ "-123.45e6" }, [&] {
			return parser.prettifyJson(std::string{ "-123.45e6" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_true", true>::assert_eq(std::string{ "true" }, [&] {
			return parser.prettifyJson(std::string{ "true" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_false", true>::assert_eq(std::string{ "false" }, [&] {
			return parser.prettifyJson(std::string{ "false" });
		});

		rt_ut::unit_test<"prettify_bare_top_level_null", true>::assert_eq(std::string{ "null" }, [&] {
			return parser.prettifyJson(std::string{ "null" });
		});

		rt_ut::unit_test<"prettify_empty_input_reports_error", true>::assert_eq(true, [&] {
			auto result = parser.prettifyJson(std::string{ "" });
			return result.empty() && !parser.getErrors().empty();
		});

		rt_ut::unit_test<"prettify_custom_indent_size_and_char", true>::assert_eq(
			std::string{ "{\n\t\t\"a\":\t1,\n\t\t\"b\":\t[\n\t\t\t\t1,\n\t\t\t\t2\n\t\t]\n}" }, [&] {
				return parser.prettifyJson<jsonifier::prettify_options{ .indentSize = 2, .indentChar = '\t' }>(std::string{ R"({"a":1,"b":[1,2]})" });
			});

		rt_ut::unit_test<"prettify_buffer_overload_writes_into_existing_buffer", true>::assert_eq(std::string{ "{\n   \"a\": 1,\n   \"b\": 2\n}" }, [&] {
			std::string buffer{ "stale contents that should be overwritten" };
			bool success = parser.prettifyJson(std::string{ R"({"a":1,"b":2})" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"prettify_buffer_overload_handles_bare_top_level_scalar", true>::assert_eq(std::string{ "42" }, [&] {
			std::string buffer{};
			bool success = parser.prettifyJson(std::string{ "42" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"prettify_buffer_overload_empty_input_returns_false", true>::assert_eq(false, [&] {
			std::string buffer{ "stale" };
			return parser.prettifyJson(std::string{ "" }, buffer);
		});

		std::cout << "JSON Prettifier validation tests complete." << std::endl;
	}

}
