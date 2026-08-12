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
			static constexpr std::string_view badInput{ R"({"test_string":)" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, badInput);
			const auto errCountBefore = parserA.getErrors().size();

			jsonifier::jsonifier_core<> parserB{ parserA };

			rt_ut::unit_test<"core, copy-construct-copies-errors", true>::assert_eq(errCountBefore, [&]() {
				return parserB.getErrors().size();
			});

			static constexpr std::string_view goodInput{ R"({"test_string":"ok","test_int":1,"test_bool":true})" };
			abc_in_order_partial_test_struct dataB{};
			parserB.parseJson(dataB, goodInput);

			rt_ut::unit_test<"core, copy-construct-independent-state", true>::assert_eq(errCountBefore, [&]() {
				return parserA.getErrors().size();
			});
		}

		{
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view goodInput{ R"({"test_string":"copied","test_int":99,"test_bool":true})" };
			abc_in_order_partial_test_struct dataA{};
			parserA.parseJson(dataA, goodInput);

			jsonifier::jsonifier_core<> parserB{};
			static constexpr std::string_view badInput{ R"({"test_string":)" };
			abc_in_order_partial_test_struct dataB{};
			parserB.parseJson(dataB, badInput);

			rt_ut::unit_test<"core, copy-assign-precondition-has-errors", true>::assert_ne(0ull, [&]() {
				return parserB.getErrors().size();
			});

			parserB = parserA;

			rt_ut::unit_test<"core, copy-assign-overwrites-errors", true>::assert_eq(0ull, [&]() {
				return parserB.getErrors().size();
			});

			abc_in_order_partial_test_struct dataOut{};
			rt_ut::unit_test<"core, copy-assign-remains-functional", true>::assert_eq(true, [&]() {
				return parserB.parseJson(dataOut, goodInput) && dataOut.test_string == "copied" && dataOut.test_int == 99 && dataOut.test_bool == true;
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
			static constexpr std::string_view testInput{ R"({"test_string":"self","test_int":3,"test_bool":true})" };
			abc_in_order_partial_test_struct data{};
			parserA.parseJson(data, testInput);

			jsonifier::jsonifier_core<>* selfPtr = &parserA;
			parserA								 = *selfPtr;

			abc_in_order_partial_test_struct dataOut{};
			rt_ut::unit_test<"core, self-copy-assign-remains-functional", true>::assert_eq(true, [&]() {
				return parserA.parseJson(dataOut, testInput) && dataOut.test_string == "self" && dataOut.test_int == 3 && dataOut.test_bool == true;
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
			jsonifier::jsonifier_core<> parserA{};
			static constexpr std::string_view inputA{ R"({"test_string":"first","test_int":1,"test_bool":true})" };
			static constexpr std::string_view inputB{ R"({"test_string":"second","test_int":2,"test_bool":false})" };
			abc_in_order_partial_test_struct dataA{};
			parserA.parseJson(dataA, inputA);

			jsonifier::jsonifier_core<> parserB{ parserA };
			abc_in_order_partial_test_struct dataB{};
			parserB.parseJson(dataB, inputB);

			abc_in_order_partial_test_struct dataAAgain{};
			rt_ut::unit_test<"core, copy-construct-buffers-not-aliased", true>::assert_eq(true, [&]() {
				return parserA.parseJson(dataAAgain, inputA) && dataAAgain.test_string == "first" && dataAAgain.test_int == 1 && dataAAgain.test_bool == true &&
					dataB.test_string == "second" && dataB.test_int == 2 && dataB.test_bool == false;
			});
		}

		std::cout << "Core copy/move validation tests complete." << std::endl;
	}

}
