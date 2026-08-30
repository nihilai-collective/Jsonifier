// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/array.hpp
#pragma once

#include "common.hpp"

namespace array_tests {

	inline static void runTests() {
		std::cout << "Array Tests" << std::endl;

		rt_ut::unit_test<"array_size_max_size_empty", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 4> arr{};
			return arr.size() == 4 && arr.maxSize() == 4 && !arr.empty();
		});

		rt_ut::unit_test<"array_front_and_back", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 4> arr{ { 10, 20, 30, 40 } };
			return arr.front() == 10 && arr.back() == 40;
		});

		rt_ut::unit_test<"array_operator_bracket_read_and_write", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> arr{ { 1, 2, 3 } };
			arr[1] = 99;
			const auto& constArr = arr;
			return arr[0] == 1 && arr[1] == 99 && constArr[2] == 3;
		});

		rt_ut::unit_test<"array_tag_based_compile_time_subscript", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> arr{ { 5, 6, 7 } };
			arr[jsonifier::internal::tag<uint64_t{ 0 }>{}] = 50;
			return arr[jsonifier::internal::tag<uint64_t{ 0 }>{}] == 50 && arr[jsonifier::internal::tag<uint64_t{ 1 }>{}] == 6 &&
				arr[jsonifier::internal::tag<uint64_t{ 2 }>{}] == 7;
		});

		rt_ut::unit_test<"array_at_returns_value_for_valid_index", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> arr{ { 1, 2, 3 } };
			const auto& constArr = arr;
			return arr.at(2) == 3 && constArr.at(0) == 1;
		});

		rt_ut::unit_test<"array_at_throws_on_out_of_range_index", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> arr{ { 1, 2, 3 } };
			bool threw = false;
			try {
				arr.at(3);
			} catch (const std::runtime_error&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"array_const_at_throws_on_out_of_range_index", true>::assert_eq(true, [] {
			const jsonifier::internal::array<int32_t, 3> arr{ { 1, 2, 3 } };
			bool threw = false;
			try {
				arr.at(3);
			} catch (const std::runtime_error&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"array_fill_sets_every_element", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 5> arr{};
			arr.fill(7);
			bool allMatch = true;
			for (uint64_t i = 0; i < arr.size(); ++i) {
				allMatch = allMatch && arr[i] == 7;
			}
			return allMatch;
		});

		rt_ut::unit_test<"array_swap_exchanges_contents", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> a{ { 1, 2, 3 } };
			jsonifier::internal::array<int32_t, 3> b{ { 4, 5, 6 } };
			a.swap(b);
			return a[0] == 4 && a[1] == 5 && a[2] == 6 && b[0] == 1 && b[1] == 2 && b[2] == 3;
		});

		rt_ut::unit_test<"array_data_pointer_is_contiguous_and_matches_elements", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> arr{ { 1, 2, 3 } };
			auto* p = arr.data();
			return p[0] == 1 && p[1] == 2 && p[2] == 3 && &arr[0] == p;
		});

		rt_ut::unit_test<"array_begin_end_range_for_visits_all_elements_in_order", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 4> arr{ { 1, 2, 3, 4 } };
			int64_t sum{};
			uint64_t count{};
			for (auto value: arr) {
				sum += value;
				++count;
			}
			return sum == 10 && count == 4;
		});

		rt_ut::unit_test<"array_equality_and_inequality", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 3> a{ { 1, 2, 3 } };
			jsonifier::internal::array<int32_t, 3> b{ { 1, 2, 3 } };
			jsonifier::internal::array<int32_t, 3> c{ { 1, 2, 4 } };
			return a == b && !(a != b) && a != c && !(a == c);
		});

		rt_ut::unit_test<"array_zero_size_reports_empty_and_no_data", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 0> arr{};
			return arr.size() == 0 && arr.maxSize() == 0 && arr.empty();
		});

		rt_ut::unit_test<"array_zero_size_equality_always_true", true>::assert_eq(true, [] {
			jsonifier::internal::array<int32_t, 0> a{};
			jsonifier::internal::array<int32_t, 0> b{};
			return a == b && !(a != b);
		});

		std::cout << "Array validation tests complete." << std::endl;
	}

}
