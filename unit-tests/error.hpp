// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/error.hpp
#pragma once

#include "common.hpp"

namespace error_tests {

	inline static void runTests() {
		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_comma>(
				testInput.data(), testInput.data() + 5, testInput.data() + testInput.size());

			rt_ut::unit_test<"error, constructError-basic-conversion", true>::template assert_eq<jsonifier::internal::parse_statuses::missing_comma>([&]() {
				return err.operator jsonifier::internal::parse_statuses();
			});
		}

		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto errA = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_comma>(
				testInput.data(), testInput.data() + 5, testInput.data() + testInput.size());
			auto errB = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_comma>(
				testInput.data(), testInput.data() + 5, testInput.data() + testInput.size());

			rt_ut::unit_test<"error, operator-equal-same-position", true>::template assert_eq<true>([&]() {
				return errA == errB;
			});
		}

		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto errA = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_comma>(
				testInput.data(), testInput.data() + 5, testInput.data() + testInput.size());
			auto errB = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_comma>(
				testInput.data(), testInput.data() + 8, testInput.data() + testInput.size());

			rt_ut::unit_test<"error, operator-not-equal-diff-position", true>::template assert_eq<false>([&]() {
				return errA == errB;
			});
		}

		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::validating, jsonifier::internal::parse_statuses::invalid_string_characters>(
				testInput.data(), testInput.data() + 3, testInput.data() + testInput.size());

			rt_ut::unit_test<"error, reportError-non-empty", true>::template assert_eq<false>([&]() {
				return err.reportError().empty();
			});
		}

		{
			static constexpr std::string_view multiLineInput{ "{\n\"a\": 1,\n\"b\": bad\n}" };
			auto badPos = multiLineInput.data() + multiLineInput.find("bad");
			auto err	= jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::invalid_bool_value>(
				   multiLineInput.data(), badPos, multiLineInput.data() + multiLineInput.size());

			rt_ut::unit_test<"error, reportError-contains-line-number", true>::template assert_eq<true>([&]() {
				return err.reportError().find("line: 3") != std::string::npos;
			});
		}

		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::missing_colon>(
				testInput.data(), testInput.data(), testInput.data() + testInput.size());

			rt_ut::unit_test<"error, constructError-zero-offset", true>::template assert_eq<jsonifier::internal::parse_statuses::missing_colon>([&]() {
				return err.operator jsonifier::internal::parse_statuses();
			});
		}

		{
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::unexpected_end_of_input>(
				testInput.data(), testInput.data() + testInput.size(), testInput.data() + testInput.size());

			rt_ut::unit_test<"error, constructError-end-offset", true>::template assert_eq<jsonifier::internal::parse_statuses::unexpected_end_of_input>([&]() {
				return err.operator jsonifier::internal::parse_statuses();
			});
		}

		{
			std::string controlCharInput{ "\"bad" };
			controlCharInput += '\x01';
			controlCharInput += "value\"";
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::illegal_control_character>(
				controlCharInput.data(), controlCharInput.data() + 4, controlCharInput.data() + controlCharInput.size());

			rt_ut::unit_test<"error, reportError-escapes-control-chars", true>::template assert_eq<true>([&]() {
				return err.reportError().find("\\x01") != std::string::npos;
			});
		}

		{
			std::ostringstream stream{};
			static constexpr std::string_view testInput{ R"({"key": "value"})" };
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::minifying, jsonifier::internal::parse_statuses::no_input>(testInput.data(),
				testInput.data() + 2, testInput.data() + testInput.size());
			stream << err;

			rt_ut::unit_test<"error, ostream-operator-non-empty", true>::template assert_eq<false>([&]() {
				return stream.str().empty();
			});
		}

		{
			std::string controlCharInput{ "\"bad" };
			controlCharInput += '\x01';
			controlCharInput += "value\"";
			auto err = jsonifier::internal::error::constructError<jsonifier::internal::status_classes::parsing, jsonifier::internal::parse_statuses::illegal_control_character>(
				controlCharInput.data(), controlCharInput.data() + 4, controlCharInput.data() + controlCharInput.size());

			rt_ut::unit_test<"error, diagnostic-control-char-report", true>::template assert_eq<false>([&]() {
				return err.reportError().empty();
			});
		}
		std::cout << "Error validation tests complete." << std::endl;
	}

}
