// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/iterator.hpp
#pragma once

#include "common.hpp"

#include <algorithm>
#include <numeric>

namespace iterator_tests {

	inline static void runTests() {
		std::cout << "Iterator Tests" << std::endl;

		rt_ut::unit_test<"iterator_dereference_and_arrow", true>::assert_eq(true, [] {
			int32_t values[3]{ 10, 20, 30 };
			jsonifier::internal::basic_iterator<int32_t> it{ values };
			return *it == 10 && *it.operator->() == 10;
		});

		rt_ut::unit_test<"iterator_pre_and_post_increment", true>::assert_eq(true, [] {
			int32_t values[3]{ 10, 20, 30 };
			jsonifier::internal::basic_iterator<int32_t> it{ values };
			auto post = it++;
			bool postOk = *post == 10 && *it == 20;
			auto& preRef = ++it;
			bool preOk = *it == 30 && &preRef == &it;
			return postOk && preOk;
		});

		rt_ut::unit_test<"iterator_pre_and_post_decrement", true>::assert_eq(true, [] {
			int32_t values[3]{ 10, 20, 30 };
			jsonifier::internal::basic_iterator<int32_t> it{ values + 2 };
			auto post = it--;
			bool postOk = *post == 30 && *it == 20;
			auto& preRef = --it;
			bool preOk = *it == 10 && &preRef == &it;
			return postOk && preOk;
		});

		rt_ut::unit_test<"iterator_plus_equals_and_minus_equals", true>::assert_eq(true, [] {
			int32_t values[5]{ 0, 1, 2, 3, 4 };
			jsonifier::internal::basic_iterator<int32_t> it{ values };
			it += 3;
			bool plusOk = *it == 3;
			it -= 2;
			bool minusOk = *it == 1;
			return plusOk && minusOk;
		});

		rt_ut::unit_test<"iterator_binary_plus_and_minus_offset", true>::assert_eq(true, [] {
			int32_t values[5]{ 0, 1, 2, 3, 4 };
			jsonifier::internal::basic_iterator<int32_t> it{ values + 2 };
			auto forward = it + 2;
			auto backward = it - 1;
			auto friendForward = 2 + it;
			return *forward == 4 && *backward == 1 && *friendForward == 4;
		});

		rt_ut::unit_test<"iterator_difference_between_iterators", true>::assert_eq(true, [] {
			int32_t values[5]{ 0, 1, 2, 3, 4 };
			jsonifier::internal::basic_iterator<int32_t> begin{ values };
			jsonifier::internal::basic_iterator<int32_t> end{ values + 5 };
			return (end - begin) == 5 && (begin - end) == -5;
		});

		rt_ut::unit_test<"iterator_subscript_offset", true>::assert_eq(true, [] {
			int32_t values[5]{ 0, 1, 2, 3, 4 };
			jsonifier::internal::basic_iterator<int32_t> it{ values + 1 };
			return it[0] == 1 && it[2] == 3 && it[-1] == 0;
		});

		rt_ut::unit_test<"iterator_equality_and_inequality", true>::assert_eq(true, [] {
			int32_t values[3]{ 0, 1, 2 };
			jsonifier::internal::basic_iterator<int32_t> a{ values };
			jsonifier::internal::basic_iterator<int32_t> b{ values };
			jsonifier::internal::basic_iterator<int32_t> c{ values + 1 };
			return a == b && !(a != b) && a != c && !(a == c);
		});

		rt_ut::unit_test<"iterator_relational_ordering", true>::assert_eq(true, [] {
			int32_t values[3]{ 0, 1, 2 };
			jsonifier::internal::basic_iterator<int32_t> a{ values };
			jsonifier::internal::basic_iterator<int32_t> b{ values + 2 };
			return a < b && !(b < a) && b > a && !(a > b) && a <= a && a <= b && b >= b && b >= a;
		});

		rt_ut::unit_test<"iterator_zero_sized_variant_always_equal", true>::assert_eq(true, [] {
			jsonifier::internal::sized_iterator<int32_t, 0ULL> a{};
			jsonifier::internal::sized_iterator<int32_t, 0ULL> b{ nullptr };
			return a == b && !(a != b) && !(a < b) && !(a > b) && a <= b && a >= b;
		});

		rt_ut::unit_test<"iterator_range_for_loop_visits_all_elements", true>::assert_eq(true, [] {
			int32_t values[4]{ 3, 1, 4, 1 };
			jsonifier::internal::basic_iterator<int32_t> begin{ values };
			jsonifier::internal::basic_iterator<int32_t> end{ values + 4 };
			int64_t sum{};
			uint64_t count{};
			for (auto it = begin; it != end; ++it) {
				sum += *it;
				++count;
			}
			return count == 4 && sum == 9;
		});

		rt_ut::unit_test<"iterator_works_with_std_distance_and_sort", true>::assert_eq(true, [] {
			int32_t values[5]{ 5, 3, 4, 1, 2 };
			jsonifier::internal::basic_iterator<int32_t> begin{ values };
			jsonifier::internal::basic_iterator<int32_t> end{ values + 5 };
			bool distanceOk = std::distance(begin, end) == 5;
			std::sort(begin, end);
			bool sortedOk = values[0] == 1 && values[1] == 2 && values[2] == 3 && values[3] == 4 && values[4] == 5;
			bool accumulateOk = std::accumulate(begin, end, 0) == 15;
			return distanceOk && sortedOk && accumulateOk;
		});

		rt_ut::unit_test<"iterator_satisfies_contiguous_iterator_concept", true>::assert_eq(true, [] {
			return std::contiguous_iterator<jsonifier::internal::basic_iterator<int32_t>>;
		});

		rt_ut::unit_test<"string_view_begin_end_iterate_in_order", true>::assert_eq(true, [] {
			jsonifier::string_view sv{ "hello" };
			std::string collected{};
			for (auto it = sv.begin(); it != sv.end(); ++it) {
				collected.push_back(*it);
			}
			return collected == std::string{ "hello" };
		});

		rt_ut::unit_test<"string_view_distance_matches_size", true>::assert_eq(true, [] {
			jsonifier::string_view sv{ "distance test" };
			return static_cast<uint64_t>(std::distance(sv.begin(), sv.end())) == sv.size();
		});

		rt_ut::unit_test<"string_view_reverse_iteration_matches_expected", true>::assert_eq(true, [] {
			jsonifier::string_view sv{ "abcd" };
			std::string collected{};
			for (auto it = sv.rbegin(); it != sv.rend(); ++it) {
				collected.push_back(*it);
			}
			return collected == std::string{ "dcba" };
		});

		std::cout << "Iterator validation tests complete." << std::endl;
	}

}
