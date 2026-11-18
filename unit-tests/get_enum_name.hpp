/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/get_enum_name.hpp
 */
#pragma once

#include "common.hpp"

namespace enum_name_tests {

	enum class shape_kinds : uint8_t { circle, square, triangle, count };

	enum class sparse_kinds : uint32_t { alpha = 0, beta = 2, gamma = 4, count = 5 };

	inline void runTests() {
		std::cout << "Enum Name Reflection Tests" << std::endl;

		rt_ut::unit_test<"enum_name_get_name_first_value", true>::assert_eq("circle", [] {
			return jsonifier::internal::getName(shape_kinds::circle);
		});

		rt_ut::unit_test<"enum_name_get_name_middle_value", true>::assert_eq("square", [] {
			return jsonifier::internal::getName(shape_kinds::square);
		});

		rt_ut::unit_test<"enum_name_get_name_last_named_value", true>::assert_eq("triangle", [] {
			return jsonifier::internal::getName(shape_kinds::triangle);
		});

		rt_ut::unit_test<"enum_name_stream_operator_writes_name", true>::assert_eq(true, [] {
			jsonifier::internal::basic_stream<> stream{};
			jsonifier::internal::operator<<(stream, shape_kinds::square);
			auto streamText = stream.view();
			stream.discard();
			return streamText == "square";
		});

		rt_ut::unit_test<"enum_name_valid_count_excludes_sentinel", true>::assert_eq(static_cast<uint64_t>(3), [] {
			return jsonifier::internal::enum_data<shape_kinds>::validCount;
		});

		rt_ut::unit_test<"enum_name_sparse_named_values_resolve", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(sparse_kinds::alpha) == "alpha" && jsonifier::internal::getName(sparse_kinds::beta) == "beta" &&
				jsonifier::internal::getName(sparse_kinds::gamma) == "gamma";
		});

		rt_ut::unit_test<"enum_name_sparse_gap_values_are_unknown", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(static_cast<sparse_kinds>(1)) == "Unknown Type" && jsonifier::internal::getName(static_cast<sparse_kinds>(3)) == "Unknown Type";
		});

		rt_ut::unit_test<"enum_name_sparse_valid_count_skips_gaps", true>::assert_eq(static_cast<uint64_t>(3), [] {
			return jsonifier::internal::enum_data<sparse_kinds>::validCount;
		});

		rt_ut::unit_test<"enum_name_out_of_probe_range_is_unknown", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(static_cast<shape_kinds>(200)) == "Unknown Type";
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_rejects_empty", true>::assert_eq(false, [] {
			return jsonifier::internal::isValidEnumName("");
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_rejects_parens", true>::assert_eq(false, [] {
			return jsonifier::internal::isValidEnumName("(shape_kinds)5");
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_rejects_leading_digit", true>::assert_eq(false, [] {
			return jsonifier::internal::isValidEnumName("5shape");
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_accepts_identifier", true>::assert_eq(true, [] {
			return jsonifier::internal::isValidEnumName("circle");
		});

		std::cout << "Enum Name Reflection validation tests complete." << std::endl;
	}

}
