/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/minifier.hpp
 */
#pragma once

#include "common.hpp"

namespace minifier_tests {

	inline static void runTests() {
		std::cout << "JSON Minifier Buffer Overload Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		auto minifyInto = [&](std::string input, std::string buffer = std::string{}) {
			bool success = parser.minifyJson(std::move(input), buffer);
			return success ? buffer : std::string{ "FAILED" };
		};

		rt_ut::unit_test<"buffer_minify_simple_object_strips_whitespace", true>::assert_eq(std::string{ R"({"a":1,"b":2})" }, [&] {
			return minifyInto(std::string{ R"({ "a" : 1 , "b" : 2 })" });
		});

		rt_ut::unit_test<"buffer_minify_nested_structure", true>::assert_eq(std::string{ R"({"a":{"b":[1,2,3],"c":"hello world"}})" }, [&] {
			return minifyInto(std::string{ "{\n\t\"a\": {\n\t\t\"b\": [1, 2, 3],\n\t\t\"c\": \"hello world\"\n\t}\n}" });
		});

		rt_ut::unit_test<"buffer_minify_already_minified_is_idempotent", true>::assert_eq(std::string{ R"({"a":1,"b":[1,2,3]})" }, [&] {
			return minifyInto(std::string{ R"({"a":1,"b":[1,2,3]})" });
		});

		rt_ut::unit_test<"buffer_minify_preserves_spaces_inside_strings", true>::assert_eq(std::string{ R"({"key":"value with spaces"})" }, [&] {
			return minifyInto(std::string{ R"({"key": "value with spaces"})" });
		});

		rt_ut::unit_test<"buffer_minify_preserves_escaped_characters", true>::assert_eq(std::string{ R"({"key":"line1\nline2\ttab\"quote\""})" }, [&] {
			return minifyInto(std::string{ R"({"key": "line1\nline2\ttab\"quote\""})" });
		});

		rt_ut::unit_test<"buffer_minify_preserves_brackets_inside_strings", true>::assert_eq(std::string{ R"({"a":"[{]}","b":[1]})" }, [&] {
			return minifyInto(std::string{ R"({ "a" : "[{]}" , "b" : [ 1 ] })" });
		});

		rt_ut::unit_test<"buffer_minify_strips_whitespace_between_string_and_comma", true>::assert_eq(std::string{ R"({"a":"x","b":"y"})" }, [&] {
			return minifyInto(std::string{ R"({"a": "x"   ,   "b": "y"})" });
		});

		rt_ut::unit_test<"buffer_minify_preserves_number_formats_verbatim", true>::assert_eq(std::string{ R"([1,-2,3.14,-3.14,1e10,1E-10,1.5e+20,0,-0])" }, [&] {
			return minifyInto(std::string{ R"([1, -2, 3.14, -3.14, 1e10, 1E-10, 1.5e+20, 0, -0])" });
		});

		rt_ut::unit_test<"buffer_minify_bools_and_null", true>::assert_eq(std::string{ R"({"a":true,"b":false,"c":null})" }, [&] {
			return minifyInto(std::string{ R"({"a": true, "b": false, "c": null})" });
		});

		rt_ut::unit_test<"buffer_minify_empty_object", true>::assert_eq(std::string{ "{}" }, [&] {
			return minifyInto(std::string{ "{}" });
		});

		rt_ut::unit_test<"buffer_minify_empty_array", true>::assert_eq(std::string{ "[]" }, [&] {
			return minifyInto(std::string{ "[]" });
		});

		rt_ut::unit_test<"buffer_minify_deeply_nested_structure", true>::assert_eq(std::string{ R"({"a":{"b":{"c":{"d":[1,[2,[3]]]}}}})" }, [&] {
			return minifyInto(std::string{ R"({"a":{"b":{"c":{"d":[1,[2,[3]]]}}}})" });
		});

		rt_ut::unit_test<"buffer_minify_array_of_strings", true>::assert_eq(std::string{ R"(["a","b","c"])" }, [&] {
			return minifyInto(std::string{ R"(["a", "b", "c"])" });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_string", true>::assert_eq(std::string{ R"("just a string")" }, [&] {
			return minifyInto(std::string{ R"("just a string")" });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_empty_string", true>::assert_eq(std::string{ R"("")" }, [&] {
			return minifyInto(std::string{ R"("")" });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_number", true>::assert_eq(std::string{ "42" }, [&] {
			return minifyInto(std::string{ "  42  " });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_negative_exponent_number", true>::assert_eq(std::string{ "-123.45e6" }, [&] {
			return minifyInto(std::string{ "-123.45e6" });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_true", true>::assert_eq(std::string{ "true" }, [&] {
			return minifyInto(std::string{ "  true  " });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_false", true>::assert_eq(std::string{ "false" }, [&] {
			return minifyInto(std::string{ "false" });
		});

		rt_ut::unit_test<"buffer_minify_bare_top_level_null", true>::assert_eq(std::string{ "null" }, [&] {
			return minifyInto(std::string{ "null" });
		});

		rt_ut::unit_test<"buffer_minify_overwrites_larger_stale_buffer", true>::assert_eq(std::string{ R"({"a":1,"b":2})" }, [&] {
			return minifyInto(std::string{ R"({ "a" : 1 , "b" : 2 })" }, std::string(4096, 'X'));
		});

		rt_ut::unit_test<"buffer_minify_grows_smaller_stale_buffer", true>::assert_eq(std::string{ R"({"a":1,"b":2})" }, [&] {
			return minifyInto(std::string{ R"({ "a" : 1 , "b" : 2 })" }, std::string{ "ab" });
		});

		rt_ut::unit_test<"buffer_minify_exact_size_buffer_no_overrun", true>::assert_eq(std::string{ R"({"a":1})" }, [&] {
			std::string buffer{};
			buffer.resize(7);
			bool success = parser.minifyJson(std::string{ R"({"a":1})" }, buffer);
			return success ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"buffer_minify_reused_buffer_shrinks_to_second_output", true>::assert_eq(std::string{ "42" }, [&] {
			std::string buffer{};
			bool firstSuccess  = parser.minifyJson(std::string{ "{\n\t\"a\": {\n\t\t\"b\": [1, 2, 3],\n\t\t\"c\": \"hello world\"\n\t}\n}" }, buffer);
			bool secondSuccess = parser.minifyJson(std::string{ "  42  " }, buffer);
			return (firstSuccess && secondSuccess) ? buffer : std::string{ "FAILED" };
		});

		rt_ut::unit_test<"buffer_minify_large_input_exceeds_initial_string_buffer", true>::assert_eq(true, [&] {
			std::string input{ "[" };
			std::string expected{ "[" };
			for (uint64_t x = 0; x < 200000; ++x) {
				if (x > 0) {
					input += " ,\n\t ";
					expected += ",";
				}
				input += "{ \"key\" : \"value with spaces\" }";
				expected += R"({"key":"value with spaces"})";
			}
			input += " ]";
			expected += "]";
			return minifyInto(std::move(input)) == expected;
		});

		rt_ut::unit_test<"buffer_minify_empty_input_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.minifyJson(std::string{ "" }, buffer);
			return !success && !parser.getErrors().empty();
		});

		rt_ut::unit_test<"buffer_minify_empty_input_leaves_buffer_untouched", true>::assert_eq(std::string{ "stale" }, [&] {
			std::string buffer{ "stale" };
			parser.minifyJson(std::string{ "" }, buffer);
			return buffer;
		});

		rt_ut::unit_test<"buffer_minify_whitespace_only_input_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.minifyJson(std::string{ "   \n\t  " }, buffer);
			return !success && !parser.getErrors().empty() && buffer == "stale";
		});
		rt_ut::unit_test<"buffer_minify_unclosed_array_diagnostic", true>::assert_eq(std::string{ "success=0 errors=1 buffer=" }, [&] {
			std::string buffer{ "stale" };
			bool success = parser.minifyJson(std::string{ "[" }, buffer);
			return "success=" + std::to_string(success) + " errors=" + std::to_string(!parser.getErrors().empty()) + " buffer=" + buffer;
		});

		rt_ut::unit_test<"buffer_minify_unclosed_array_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.minifyJson(std::string{ "[" }, buffer);
			return !success && !parser.getErrors().empty() && buffer.empty();
		});

		rt_ut::unit_test<"buffer_minify_unclosed_object_returns_false_and_reports_error", true>::assert_eq(true, [&] {
			std::string buffer{ "stale" };
			bool success = parser.minifyJson(std::string{ "{" }, buffer);
			return !success && !parser.getErrors().empty() && buffer.empty();
		});

		rt_ut::unit_test<"buffer_minify_errors_cleared_after_subsequent_success", true>::assert_eq(true, [&] {
			std::string buffer{};
			bool failed	   = !parser.minifyJson(std::string{ "" }, buffer);
			bool succeeded = parser.minifyJson(std::string{ R"({ "a" : 1 })" }, buffer);
			return failed && succeeded && parser.getErrors().empty() && buffer == R"({"a":1})";
		});

		std::cout << "JSON Minifier validation tests complete." << std::endl;
	}

}
