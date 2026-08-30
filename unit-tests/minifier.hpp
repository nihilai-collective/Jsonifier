// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/minifier.hpp
#pragma once

#include "common.hpp"

namespace minifier_tests {

	inline static void runTests() {
		std::cout << "JSON Minifier Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		rt_ut::unit_test<"minify_simple_object_strips_whitespace", true>::assert_eq(std::string{ R"({"a":1,"b":2})" }, [&] {
			return parser.minifyJson(std::string{ R"({ "a" : 1 , "b" : 2 })" });
		});

		rt_ut::unit_test<"minify_nested_structure", true>::assert_eq(std::string{ R"({"a":{"b":[1,2,3],"c":"hello world"}})" }, [&] {
			return parser.minifyJson(std::string{ "{\n\t\"a\": {\n\t\t\"b\": [1, 2, 3],\n\t\t\"c\": \"hello world\"\n\t}\n}" });
		});

		rt_ut::unit_test<"minify_already_minified_is_idempotent", true>::assert_eq(std::string{ R"({"a":1,"b":[1,2,3]})" }, [&] {
			return parser.minifyJson(std::string{ R"({"a":1,"b":[1,2,3]})" });
		});

		rt_ut::unit_test<"minify_preserves_spaces_inside_strings", true>::assert_eq(std::string{ R"({"key":"value with spaces"})" }, [&] {
			return parser.minifyJson(std::string{ R"({"key": "value with spaces"})" });
		});

		rt_ut::unit_test<"minify_preserves_escaped_characters", true>::assert_eq(std::string{ R"({"key":"line1\nline2\ttab\"quote\""})" }, [&] {
			return parser.minifyJson(std::string{ R"({"key": "line1\nline2\ttab\"quote\""})" });
		});

		rt_ut::unit_test<"minify_strips_whitespace_between_string_and_comma", true>::assert_eq(std::string{ R"({"a":"x","b":"y"})" }, [&] {
			return parser.minifyJson(std::string{ R"({"a": "x"   ,   "b": "y"})" });
		});

		rt_ut::unit_test<"minify_preserves_number_formats_verbatim", true>::assert_eq(std::string{ R"([1,-2,3.14,-3.14,1e10,1E-10,1.5e+20,0,-0])" }, [&] {
			return parser.minifyJson(std::string{ R"([1, -2, 3.14, -3.14, 1e10, 1E-10, 1.5e+20, 0, -0])" });
		});

		rt_ut::unit_test<"minify_bools_and_null", true>::assert_eq(std::string{ R"({"a":true,"b":false,"c":null})" }, [&] {
			return parser.minifyJson(std::string{ R"({"a": true, "b": false, "c": null})" });
		});

		rt_ut::unit_test<"minify_empty_object", true>::assert_eq(std::string{ "{}" }, [&] {
			return parser.minifyJson(std::string{ "{}" });
		});

		rt_ut::unit_test<"minify_empty_array", true>::assert_eq(std::string{ "[]" }, [&] {
			return parser.minifyJson(std::string{ "[]" });
		});

		rt_ut::unit_test<"minify_deeply_nested_structure", true>::assert_eq(std::string{ R"({"a":{"b":{"c":{"d":[1,[2,[3]]]}}}})" }, [&] {
			return parser.minifyJson(std::string{ R"({"a":{"b":{"c":{"d":[1,[2,[3]]]}}}})" });
		});

		rt_ut::unit_test<"minify_array_of_strings", true>::assert_eq(std::string{ R"(["a","b","c"])" }, [&] {
			return parser.minifyJson(std::string{ R"(["a", "b", "c"])" });
		});

		rt_ut::unit_test<"minify_bare_top_level_string", true>::assert_eq(std::string{ R"("just a string")" }, [&] {
			return parser.minifyJson(std::string{ R"("just a string")" });
		});

		rt_ut::unit_test<"minify_bare_top_level_empty_string", true>::assert_eq(std::string{ R"("")" }, [&] {
			return parser.minifyJson(std::string{ R"("")" });
		});

		rt_ut::unit_test<"minify_bare_top_level_number", true>::assert_eq(std::string{ "42" }, [&] {
			return parser.minifyJson(std::string{ "  42  " });
		});

		rt_ut::unit_test<"minify_bare_top_level_negative_exponent_number", true>::assert_eq(std::string{ "-123.45e6" }, [&] {
			return parser.minifyJson(std::string{ "-123.45e6" });
		});

		rt_ut::unit_test<"minify_bare_top_level_true", true>::assert_eq(std::string{ "true" }, [&] {
			return parser.minifyJson(std::string{ "  true  " });
		});

		rt_ut::unit_test<"minify_bare_top_level_false", true>::assert_eq(std::string{ "false" }, [&] {
			return parser.minifyJson(std::string{ "false" });
		});

		rt_ut::unit_test<"minify_bare_top_level_null", true>::assert_eq(std::string{ "null" }, [&] {
			return parser.minifyJson(std::string{ "null" });
		});

		rt_ut::unit_test<"minify_empty_input_reports_error", true>::assert_eq(true, [&] {
			auto result = parser.minifyJson(std::string{ "" });
			return result.empty() && !parser.getErrors().empty();
		});

		rt_ut::unit_test<"minify_whitespace_only_input_reports_error", true>::assert_eq(true, [&] {
			auto result = parser.minifyJson(std::string{ "   \n\t  " });
			return result.empty() && !parser.getErrors().empty();
		});

		rt_ut::unit_test<"minify_buffer_overload_writes_into_existing_buffer", true>::assert_eq(std::string{ R"({"a":1,"b":2})" }, [&] {
			std::string buffer{ "stale contents that should be overwritten" };
			bool success = parser.minifyJson(std::string{ R"({ "a" : 1 , "b" : 2 })" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"minify_buffer_overload_handles_bare_top_level_scalar", true>::assert_eq(std::string{ "42" }, [&] {
			std::string buffer{};
			bool success = parser.minifyJson(std::string{ "  42  " }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"minify_buffer_overload_empty_input_returns_false", true>::assert_eq(false, [&] {
			std::string buffer{ "stale" };
			return parser.minifyJson(std::string{ "" }, buffer);
		});

		std::cout << "JSON Minifier validation tests complete." << std::endl;
	}

}
