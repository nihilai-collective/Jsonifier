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
#include "conformance.hpp"

namespace json_test_suite_tests {

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void jsonTestSuiteYNTestsImpl() {
		jsonifier::jsonifier_core<> parser{};
		std::unordered_map<std::string, test_base> jsonTests{};
		processFilesInFolder(jsonTests, "JSONTestSuite/test_parsing");
		std::cout << "JSONTestSuite Y/N Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;

		conformance_tests::runConformanceTest<"n_array_1_true_without_comma.json", partial, knownOrder, nullTerminated, std::vector<bool>,
			jsonifier::internal::parse_statuses::invalid_bool_value>(jsonTests["n_array_1_true_without_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_a_invalid_utf8.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_a_invalid_utf8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_colon_instead_of_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_array_colon_instead_of_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_comma_after_close.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_array_comma_after_close.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_comma_and_number.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_array_comma_and_number.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_double_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_double_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_double_extra_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_double_extra_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_extra_close.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_array_extra_close.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_extra_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_extra_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_incomplete_invalid_value.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_incomplete_invalid_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_incomplete.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_array_incomplete.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_inner_array_no_comma.json", partial, knownOrder, nullTerminated, std::vector<std::vector<std::string>>,
			jsonifier::internal::parse_statuses::missing_array_start>(jsonTests["n_array_inner_array_no_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_invalid_utf8.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_invalid_utf8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_items_separated_by_semicolon.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_items_separated_by_semicolon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_just_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_just_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_just_minus.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_array_just_minus.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_missing_value.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_missing_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_newlines_unclosed.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_newlines_unclosed.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_number_and_comma.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_array_number_and_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_number_and_several_commas.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_array_number_and_several_commas.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_spaces_vertical_tab_formfeed.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_spaces_vertical_tab_formfeed.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_star_inside.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_star_inside.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_unclosed_trailing_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_unclosed_trailing_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_unclosed_with_new_lines.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_array_unclosed_with_new_lines.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_unclosed_with_object_inside.json", partial, knownOrder, nullTerminated,
			std::vector<std::unordered_map<std::string, std::string>>, jsonifier::internal::parse_statuses::missing_comma>(
			jsonTests["n_array_unclosed_with_object_inside.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_array_unclosed.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_array_unclosed.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_incomplete_false.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::invalid_bool_value>(
			jsonTests["n_incomplete_false.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_incomplete_null.json", partial, knownOrder, nullTerminated, std::nullptr_t,
			jsonifier::internal::parse_statuses::invalid_null_value>(jsonTests["n_incomplete_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_incomplete_true.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::invalid_bool_value>(
			jsonTests["n_incomplete_true.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_multidigit_number_then_00.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_multidigit_number_then_00.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_-01.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_-01.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_-1.0..json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_-1.0..json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_-2..json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_-2..json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_-NaN.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_-NaN.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_.-1.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_.-1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_.2e-3.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_.2e-3.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_++.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_++.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_+1.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_+1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_+Inf.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_+Inf.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0_capital_E.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0_capital_E.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0_capital_E+.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0_capital_E+.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0.1.2.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0.1.2.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0.3e.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0.3e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0.3e+.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0.3e+.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0.e1.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0.e1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0e.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_0e+.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_0e+.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_1_000.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_1_000.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_1.0e-.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_1.0e-.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_1.0e.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_1.0e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_1.0e+.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_1.0e+.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_1eE2.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_1eE2.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_2.e-3.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_2.e-3.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_2.e+3.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_2.e+3.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_2.e3.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_2.e3.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_9.e+.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_9.e+.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_expression.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_expression.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_hex_1_digit.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_hex_1_digit.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_hex_2_digits.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_hex_2_digits.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_Inf.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_Inf.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_infinity.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_infinity.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_invalid-negative-real.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_invalid-negative-real.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_invalid-utf-8-in-bigger-int.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_invalid-utf-8-in-bigger-int.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_invalid-utf-8-in-exponent.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_invalid-utf-8-in-exponent.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_invalid-utf-8-in-int.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_invalid-utf-8-in-int.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_invalid+-.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_invalid+-.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_minus_infinity.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_minus_infinity.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_minus_sign_with_trailing_garbage.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_minus_sign_with_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_minus_space_1.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_minus_space_1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_NaN.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_NaN.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_neg_int_starting_with_zero.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_neg_int_starting_with_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_neg_real_without_int_part.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_neg_real_without_int_part.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_neg_with_garbage_at_end.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_neg_with_garbage_at_end.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_real_garbage_after_e.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_real_garbage_after_e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_real_with_invalid_utf8_after_e.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_real_with_invalid_utf8_after_e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_real_without_fractional_part.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_real_without_fractional_part.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_starting_with_dot.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_starting_with_dot.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_U+FF11_fullwidth_digit_one.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_U+FF11_fullwidth_digit_one.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_with_alpha_char.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_with_alpha_char.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_with_alpha.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::invalid_number_value>(
			jsonTests["n_number_with_alpha.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_number_with_leading_zero.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::invalid_number_value>(jsonTests["n_number_with_leading_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_bad_value.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_object_bad_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_bracket_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_bracket_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_comma_instead_of_colon.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_colon>(jsonTests["n_object_comma_instead_of_colon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_double_colon.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_double_colon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_emoji.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_emoji.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_garbage_at_end.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_object_garbage_at_end.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_key_with_single_quotes.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_key_with_single_quotes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_lone_continuation_byte_in_key_and_trailing_comma.json", partial, knownOrder, nullTerminated,
			std::unordered_map<std::string, std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
			jsonTests["n_object_lone_continuation_byte_in_key_and_trailing_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_missing_colon.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_colon>(jsonTests["n_object_missing_colon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_missing_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_missing_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_missing_semicolon.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_colon>(jsonTests["n_object_missing_semicolon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_missing_value.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_missing_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_no-colon.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_colon>(jsonTests["n_object_no-colon.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_non_string_key_but_huge_number_instead.json", partial, knownOrder, nullTerminated,
			std::unordered_map<std::string, std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
			jsonTests["n_object_non_string_key_but_huge_number_instead.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_non_string_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_non_string_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_repeated_null_null.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_repeated_null_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_several_trailing_commas.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_several_trailing_commas.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_single_quote.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_single_quote.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_trailing_comma.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_trailing_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_trailing_comment_open.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_object_trailing_comment_open.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_trailing_comment_slash_open_incomplete.json", partial, knownOrder, nullTerminated,
			std::unordered_map<std::string, std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
			jsonTests["n_object_trailing_comment_slash_open_incomplete.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_trailing_comment_slash_open.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_object_trailing_comment_slash_open.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_trailing_comment.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_object_trailing_comment.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_two_commas_in_a_row.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_two_commas_in_a_row.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_unquoted_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_unquoted_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_unterminated-value.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_object_unterminated-value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_with_single_string.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_colon>(jsonTests["n_object_with_single_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_object_with_trailing_garbage.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_object_with_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_single_space.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_single_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_1_surrogate_then_escape_u.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_1_surrogate_then_escape_u.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_1_surrogate_then_escape_u1.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_1_surrogate_then_escape_u1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_1_surrogate_then_escape_u1x.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_1_surrogate_then_escape_u1x.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_1_surrogate_then_escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_1_surrogate_then_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_accentuated_char_no_quotes.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_accentuated_char_no_quotes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_backslash_00.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_backslash_00.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_escape_x.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_escape_x.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_escaped_backslash_bad.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_escaped_backslash_bad.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_escaped_ctrl_char_tab.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_escaped_ctrl_char_tab.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_escaped_emoji.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_escaped_emoji.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_incomplete_escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_incomplete_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_incomplete_escaped_character.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_incomplete_escaped_character.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_incomplete_surrogate_escape_invalid.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_incomplete_surrogate_escape_invalid.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_incomplete_surrogate.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_incomplete_surrogate.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_invalid_backslash_esc.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_invalid_backslash_esc.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_invalid_unicode_escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_invalid_unicode_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_invalid_utf8_after_escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_invalid_utf8_after_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_invalid-utf-8-in-escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_invalid-utf-8-in-escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_leading_uescaped_thinspace.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_leading_uescaped_thinspace.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_no_quotes_with_bad_escape.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_no_quotes_with_bad_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_single_doublequote.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::unexpected_end_of_input>(jsonTests["n_string_single_doublequote.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_single_quote.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_single_quote.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_single_string_no_double_quotes.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_single_string_no_double_quotes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_start_escape_unclosed.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_start_escape_unclosed.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_unescaped_ctrl_char.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_unescaped_ctrl_char.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_unescaped_newline.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_unescaped_newline.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_unescaped_tab.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_unescaped_tab.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_unicode_CapitalU.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_string_unicode_CapitalU.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_string_with_trailing_garbage.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_string_with_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_100000_opening_arrays.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_100000_opening_arrays.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_angle_bracket_..json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_angle_bracket_..json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_angle_bracket_null.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_angle_bracket_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_array_trailing_garbage.json", partial, knownOrder, nullTerminated, std::vector<bool>,
			jsonifier::internal::parse_statuses::invalid_bool_value>(jsonTests["n_structure_array_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_array_with_extra_array_close.json", partial, knownOrder, nullTerminated, std::vector<bool>,
			jsonifier::internal::parse_statuses::invalid_bool_value>(jsonTests["n_structure_array_with_extra_array_close.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_array_with_unclosed_string.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_array_with_unclosed_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_ascii-unicode-identifier.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_ascii-unicode-identifier.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_capitalized_True.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_capitalized_True.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_close_unopened_array.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_close_unopened_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_comma_instead_of_closing_brace.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_comma_instead_of_closing_brace.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_double_array.json", partial, knownOrder, nullTerminated, std::vector<std::vector<std::string>>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_structure_double_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_end_array.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_end_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_incomplete_UTF8_BOM.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_incomplete_UTF8_BOM.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_lone-invalid-utf-8.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_lone-invalid-utf-8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_lone-open-bracket.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_lone-open-bracket.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_no_data.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::no_input>(jsonTests["n_structure_no_data.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_null-byte-outside-string.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_null-byte-outside-string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_number_with_trailing_garbage.json", partial, knownOrder, nullTerminated, double,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_structure_number_with_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_object_followed_by_closing_object.json", partial, knownOrder, nullTerminated,
			std::unordered_map<std::string, std::string>, jsonifier::internal::parse_statuses::unfinished_input>(
			jsonTests["n_structure_object_followed_by_closing_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_object_unclosed_no_value.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_object_unclosed_no_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_object_with_comment.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_object_with_comment.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_object_with_trailing_garbage.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, bool>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_structure_object_with_trailing_garbage.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_apostrophe.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_open_array_apostrophe.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_comma.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_array_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_object.json", partial, knownOrder, nullTerminated, std::vector<std::unordered_map<std::string, std::string>>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_array_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_open_object.json", partial, knownOrder, nullTerminated,
			std::vector<std::unordered_map<std::string, std::string>>, jsonifier::internal::parse_statuses::invalid_string_characters>(
			jsonTests["n_structure_open_array_open_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_open_string.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_array_open_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_array_string.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_structure_open_array_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object_close_array.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_object_close_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object_comma.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_object_comma.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object_open_array.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_object_open_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object_open_string.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_object_open_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object_string_with_apostrophes.json", partial, knownOrder, nullTerminated,
			std::unordered_map<std::string, std::string>, jsonifier::internal::parse_statuses::invalid_string_characters>(
			jsonTests["n_structure_open_object_string_with_apostrophes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_object.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_open_open.json", partial, knownOrder, nullTerminated, std::string,
			jsonifier::internal::parse_statuses::invalid_string_characters>(jsonTests["n_structure_open_open.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_single_eacute.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_single_eacute.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_single_star.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_single_star.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_trailing_#.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::unfinished_input>(jsonTests["n_structure_trailing_#.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_U+2060_word_joined.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_U+2060_word_joined.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_uescaped_LF_before_string.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_uescaped_LF_before_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unclosed_array_partial_null.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_unclosed_array_partial_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unclosed_array_unfinished_false.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_unclosed_array_unfinished_false.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unclosed_array_unfinished_true.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_unclosed_array_unfinished_true.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unclosed_array.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_structure_unclosed_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unclosed_object.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_comma>(jsonTests["n_structure_unclosed_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_unicode-identifier.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_unicode-identifier.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_UTF8_BOM_no_data.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_UTF8_BOM_no_data.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_whitespace_formfeed.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_whitespace_formfeed.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"n_structure_whitespace_U+2060_word_joiner.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::missing_object_start>(jsonTests["n_structure_whitespace_U+2060_word_joiner.json"].fileContents, parser);

		conformance_tests::runConformanceTest<"y_array_arraysWithSpaces.json", partial, knownOrder, nullTerminated, std::vector<std::vector<std::string>>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_arraysWithSpaces.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_empty-string.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_empty-string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_empty.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_array_empty.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_ending_with_newline.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_ending_with_newline.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_false.json", partial, knownOrder, nullTerminated, std::vector<bool>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_array_false.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_heterogeneous.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_heterogeneous.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_null.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_array_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_with_1_and_newline.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_with_1_and_newline.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_with_leading_space.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_with_leading_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_with_several_null.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_with_several_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_array_with_trailing_space.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_array_with_trailing_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_0e+1.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_0e+1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_0e1.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_0e1.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_after_space.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_after_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_double_close_to_zero.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_double_close_to_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_int_with_exp.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_int_with_exp.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_minus_zero.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_minus_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_negative_int.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_negative_int.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_negative_one.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_negative_one.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_negative_zero.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_negative_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_capital_e_neg_exp.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_capital_e_neg_exp.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_capital_e_pos_exp.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_capital_e_pos_exp.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_capital_e.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_capital_e.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_exponent.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_exponent.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_fraction_exponent.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_fraction_exponent.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_neg_exp.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_real_neg_exp.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_real_pos_exponent.json", partial, knownOrder, nullTerminated, std::vector<double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_number_real_pos_exponent.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_simple_int.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_simple_int.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number_simple_real.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number_simple_real.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_number.json", partial, knownOrder, nullTerminated, std::vector<double>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_number.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_basic.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_basic.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_duplicated_key_and_value.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_duplicated_key_and_value.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_duplicated_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_duplicated_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_empty_key.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_empty_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_empty.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_empty.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_escaped_null_in_key.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_escaped_null_in_key.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_extreme_numbers.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, double>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_extreme_numbers.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_long_strings.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_long_strings.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_simple.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::vector<double>>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_simple.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_string_unicode.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_string_unicode.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object_with_newlines.json", partial, knownOrder, nullTerminated, std::unordered_map<std::string, std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_object_with_newlines.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_object.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_object.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_1_2_3_bytes_UTF-8_sequences.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_1_2_3_bytes_UTF-8_sequences.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_accepted_surrogate_pair.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_accepted_surrogate_pair.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_accepted_surrogate_pairs.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_accepted_surrogate_pairs.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_allowed_escapes.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_allowed_escapes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_backslash_and_u_escaped_zero.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_backslash_and_u_escaped_zero.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_backslash_doublequotes.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_backslash_doublequotes.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_comments.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_comments.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_double_escape_a.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_double_escape_a.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_double_escape_n.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_double_escape_n.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_escaped_control_character.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_escaped_control_character.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_escaped_noncharacter.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_escaped_noncharacter.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_in_array_with_leading_space.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_in_array_with_leading_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_in_array.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_in_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_last_surrogates_1_and_2.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_last_surrogates_1_and_2.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_nbsp_uescaped.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_nbsp_uescaped.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_nonCharacterInUTF-8_U+10FFFF.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_nonCharacterInUTF-8_U+10FFFF.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_nonCharacterInUTF-8_U+FFFF.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_nonCharacterInUTF-8_U+FFFF.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_null_escape.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_null_escape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_one-byte-utf-8.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_one-byte-utf-8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_pi.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_string_pi.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_reservedCharacterInUTF-8_U+1BFFF.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_reservedCharacterInUTF-8_U+1BFFF.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_simple_ascii.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_simple_ascii.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_space.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_string_space.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_surrogates_U+1D11E_MUSICAL_SYMBOL_G_CLEF.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_surrogates_U+1D11E_MUSICAL_SYMBOL_G_CLEF.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_three-byte-utf-8.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_three-byte-utf-8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_two-byte-utf-8.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_two-byte-utf-8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_u+2028_line_sep.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_u+2028_line_sep.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_u+2029_par_sep.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_u+2029_par_sep.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_uEscape.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_string_uEscape.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_uescaped_newline.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_uescaped_newline.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unescaped_char_delete.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unescaped_char_delete.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_2.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_2.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_escaped_double_quote.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_escaped_double_quote.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+10FFFE_nonchar.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+10FFFE_nonchar.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+1FFFE_nonchar.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+1FFFE_nonchar.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+200B_ZERO_WIDTH_SPACE.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+200B_ZERO_WIDTH_SPACE.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+2064_invisible_plus.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+2064_invisible_plus.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+FDD0_nonchar.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+FDD0_nonchar.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode_U+FFFE_nonchar.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicode_U+FFFE_nonchar.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicode.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_string_unicode.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_unicodeEscapedBackslash.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_unicodeEscapedBackslash.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_utf8.json", partial, knownOrder, nullTerminated, std::vector<std::string>, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_string_utf8.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_string_with_del_character.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_string_with_del_character.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_false.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_lonely_false.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_int.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_lonely_int.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_negative_real.json", partial, knownOrder, nullTerminated, double, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_lonely_negative_real.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_null.json", partial, knownOrder, nullTerminated, jsonifier::raw_json_data,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_structure_lonely_null.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_string.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_lonely_string.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_lonely_true.json", partial, knownOrder, nullTerminated, bool, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_lonely_true.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_string_empty.json", partial, knownOrder, nullTerminated, std::string, jsonifier::internal::parse_statuses::success>(
			jsonTests["y_structure_string_empty.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_trailing_newline.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_structure_trailing_newline.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_true_in_array.json", partial, knownOrder, nullTerminated, std::vector<bool>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_structure_true_in_array.json"].fileContents, parser);
		conformance_tests::runConformanceTest<"y_structure_whitespace_array.json", partial, knownOrder, nullTerminated, std::vector<std::string>,
			jsonifier::internal::parse_statuses::success>(jsonTests["y_structure_whitespace_array.json"].fileContents, parser);
	}

	inline static void runTests() {
		jsonTestSuiteYNTestsImpl<false, false, false>();
		jsonTestSuiteYNTestsImpl<false, true, false>();
		jsonTestSuiteYNTestsImpl<true, false, false>();
		jsonTestSuiteYNTestsImpl<true, true, false>();
		jsonTestSuiteYNTestsImpl<false, false, true>();
		jsonTestSuiteYNTestsImpl<false, true, true>();
		jsonTestSuiteYNTestsImpl<true, false, true>();
		jsonTestSuiteYNTestsImpl<true, true, true>();
	}

}
