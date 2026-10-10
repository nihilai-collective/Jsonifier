/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/jsonifier_core.hpp
 */
#pragma once

#include "common.hpp"

namespace core_tests {

	inline static void runTests() {
		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view testInput{ R"({"test_string":"hello","test_int":42,"test_bool":true})" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, testInput);

			jsonifier::jsonifier_core<> parserB{ jsonifier::internal::move(parserA) };
			abc_in_order_partial_test_struct dataB{};

			rt_ut::unit_test<"core, move-construct-remains-functional", true>::assert_eq(true, [&]() {
				return parserB.parseJson(dataB, testInput) && dataB.test_string == "hello" && dataB.test_int == 42 && dataB.test_bool == true;
			});
		}

		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view testInput{ R"({"test_string":"world","test_int":7,"test_bool":false})" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, testInput);

			jsonifier::jsonifier_core<> parserB{};
			parserB = jsonifier::internal::move(parserA);
			abc_in_order_partial_test_struct dataB{};

			rt_ut::unit_test<"core, move-assign-remains-functional", true>::assert_eq(true, [&]() {
				return parserB.parseJson(dataB, testInput) && dataB.test_string == "world" && dataB.test_int == 7 && dataB.test_bool == false;
			});
		}

		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view goodInput{ R"({"test_string":"orig","test_int":5,"test_bool":false})" };
			abc_in_order_partial_test_struct dataA{};
			parserA.parseJson(dataA, goodInput);

			jsonifier::jsonifier_core<> parserB{};
			static constexpr std::string_view badInput{ R"({"test_string":)" };
			abc_in_order_partial_test_struct dataB{};
			parserB.parseJson(dataB, badInput);

			parserB = jsonifier::internal::move(parserA);

			rt_ut::unit_test<"core, move-assign-overwrites-errors", true>::assert_eq(0ull, [&]() {
				return parserB.getErrors().size();
			});
		}

		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view testInput{ R"({"test_string":"selfmove","test_int":8,"test_bool":false})" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, testInput);

			jsonifier::jsonifier_core<>* selfPtr = &parserA;
			parserA								 = jsonifier::internal::move(*selfPtr);

			abc_in_order_partial_test_struct dataOut{};
			rt_ut::unit_test<"core, self-move-assign-remains-functional", true>::assert_eq(true, [&]() {
				return parserA.parseJson(dataOut, testInput) && dataOut.test_string == "selfmove" && dataOut.test_int == 8 && dataOut.test_bool == false;
			});
		}

		{
			const jsonifier::jsonifier_backend selected{ jsonifier::jsonifier_core<>::selectBackend() };
			rt_ut::unit_test<"core, collection-selects-supported-backend", true>::assert_eq(true, [&]() {
				return jsonifier::internal::backendSupported(selected) || selected == jsonifier::default_backend;
			});
		}

		{
			jsonifier::jsonifier_core<> parser{};
			static constexpr std::string_view badInput{ R"({"test_string":)" };
			abc_in_order_partial_test_struct data{};
			parser.parseJson(data, badInput);

			rt_ut::unit_test<"core, collection-shares-error-storage", true>::assert_eq(true, [&]() {
				return !parser.getErrors().empty();
			});
		}

		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view badInput{ R"({"test_string":)" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, badInput);

			jsonifier::jsonifier_core<> parserB{ jsonifier::internal::move(parserA) };
			static constexpr std::string_view testInput{ R"({"test_string":"rebound","test_int":3,"test_bool":true})" };
			abc_in_order_partial_test_struct dataB{};

			rt_ut::unit_test<"core, collection-move-rebinds-shared-storage", true>::assert_eq(true, [&]() {
				return !parserB.getErrors().empty() && parserB.parseJson(dataB, testInput) && dataB.test_string == "rebound" && dataB.test_int == 3;
			});
		}

		std::cout << "Core copy/move validation tests complete." << std::endl;
	}

}
