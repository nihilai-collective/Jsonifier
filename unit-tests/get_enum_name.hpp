// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/get_enum_name.hpp
#pragma once

#include "common.hpp"

namespace enum_name_tests {

	enum class shape_kind : uint8_t {
		circle,
		square,
		triangle,
		count
	};

	enum class sparse_kind : uint32_t {
		alpha = 0,
		beta  = 2,
		gamma = 4,
		count = 5
	};

	inline static void runTests() {
		std::cout << "Enum Name Reflection Tests" << std::endl;

		rt_ut::unit_test<"enum_name_get_name_first_value", true>::assert_eq("circle", [] {
			return jsonifier::internal::getName(shape_kind::circle);
		});

		rt_ut::unit_test<"enum_name_get_name_middle_value", true>::assert_eq("square", [] {
			return jsonifier::internal::getName(shape_kind::square);
		});

		rt_ut::unit_test<"enum_name_get_name_last_named_value", true>::assert_eq("triangle", [] {
			return jsonifier::internal::getName(shape_kind::triangle);
		});

		rt_ut::unit_test<"enum_name_stream_operator_writes_name", true>::assert_eq(true, [] {
			std::stringstream stream{};
			jsonifier::internal::operator<<(stream, shape_kind::square);
			return stream.str() == "square";
		});

		rt_ut::unit_test<"enum_name_valid_count_excludes_sentinel", true>::assert_eq(static_cast<uint64_t>(3), [] {
			return jsonifier::internal::enum_data<shape_kind>::validCount;
		});

		rt_ut::unit_test<"enum_name_sparse_named_values_resolve", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(sparse_kind::alpha) == "alpha" && jsonifier::internal::getName(sparse_kind::beta) == "beta" &&
				jsonifier::internal::getName(sparse_kind::gamma) == "gamma";
		});

		rt_ut::unit_test<"enum_name_sparse_gap_values_are_unknown", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(static_cast<sparse_kind>(1)) == "Unknown Type" &&
				jsonifier::internal::getName(static_cast<sparse_kind>(3)) == "Unknown Type";
		});

		rt_ut::unit_test<"enum_name_sparse_valid_count_skips_gaps", true>::assert_eq(static_cast<uint64_t>(3), [] {
			return jsonifier::internal::enum_data<sparse_kind>::validCount;
		});

		rt_ut::unit_test<"enum_name_out_of_probe_range_is_unknown", true>::assert_eq(true, [] {
			return jsonifier::internal::getName(static_cast<shape_kind>(200)) == "Unknown Type";
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_rejects_empty", true>::assert_eq(false, [] {
			return jsonifier::internal::isValidEnumName("");
		});

		rt_ut::unit_test<"enum_name_is_valid_enum_name_rejects_parens", true>::assert_eq(false, [] {
			return jsonifier::internal::isValidEnumName("(shape_kind)5");
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
