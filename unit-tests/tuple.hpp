// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/tuple.hpp
#pragma once

#include "common.hpp"

namespace tuple_tests {

	inline static void runTests() {
		std::cout << "Tuple Tests" << std::endl;

		rt_ut::unit_test<"tuple_make_tuple_and_get_by_type", true>::assert_eq(true, [] {
			auto t = jsonifier::internal::makeTuple(42, 3.5, std::string{ "hi" });
			return jsonifier::internal::get<int>(t) == 42 && std::equal_to<double>{}(jsonifier::internal::get<double>(t), 3.5) &&
				jsonifier::internal::get<std::string>(t) == std::string{ "hi" };
		});

		rt_ut::unit_test<"tuple_tag_subscript_access_and_mutation", true>::assert_eq(true, [] {
			auto t = jsonifier::internal::makeTuple(1, 2, 3);
			jsonifier::internal::getBecauseOtherLibAuthorsResolve<0>(t) = 100;
			return jsonifier::internal::getBecauseOtherLibAuthorsResolve<0>(t) == 100 && jsonifier::internal::getBecauseOtherLibAuthorsResolve<1>(t) == 2 &&
				jsonifier::internal::getBecauseOtherLibAuthorsResolve<2>(t) == 3;
		});

		rt_ut::unit_test<"tuple_size_matches_element_count", true>::assert_eq(true, [] {
			auto t = jsonifier::internal::makeTuple(1, 2.0, std::string{ "a" }, true);
			return decltype(t)::size == 4 && std::tuple_size_v<decltype(t)> == 4;
		});

		rt_ut::unit_test<"tuple_get_because_other_lib_authors_resolve_by_index", true>::assert_eq(true, [] {
			auto t = jsonifier::internal::makeTuple(9, std::string{ "nine" });
			return jsonifier::internal::getBecauseOtherLibAuthorsResolve<0>(t) == 9 && jsonifier::internal::getBecauseOtherLibAuthorsResolve<1>(t) == std::string{ "nine" };
		});

		rt_ut::unit_test<"tuple_equality_and_inequality", true>::assert_eq(true, [] {
			auto a = jsonifier::internal::makeTuple(1, 2, 3);
			auto b = jsonifier::internal::makeTuple(1, 2, 3);
			auto c = jsonifier::internal::makeTuple(1, 2, 4);
			return a == b && !(a != b) && a != c && !(a == c);
		});

		rt_ut::unit_test<"tuple_relational_ordering", true>::assert_eq(true, [] {
			auto a = jsonifier::internal::makeTuple(1, 2, 3);
			auto b = jsonifier::internal::makeTuple(1, 2, 4);
			return a < b && !(b < a) && b > a && !(a > b) && a <= b && b >= a;
		});

		rt_ut::unit_test<"tuple_three_way_comparison", true>::assert_eq(true, [] {
			auto a = jsonifier::internal::makeTuple(1, 2, 3);
			auto b = jsonifier::internal::makeTuple(1, 2, 4);
			auto c = jsonifier::internal::makeTuple(1, 2, 3);
			return std::is_lt(a <=> b) && std::is_gt(b <=> a) && std::is_eq(a <=> c);
		});

		rt_ut::unit_test<"tuple_cat_concatenates_in_order", true>::assert_eq(true, [] {
			auto a = jsonifier::internal::makeTuple(1, 2);
			auto b = jsonifier::internal::makeTuple(std::string{ "x" }, 4.5);
			auto joined = jsonifier::internal::tupleCat(a, b);
			return decltype(joined)::size == 4 && jsonifier::internal::getBecauseOtherLibAuthorsResolve<0>(joined) == 1 &&
				jsonifier::internal::getBecauseOtherLibAuthorsResolve<1>(joined) == 2 &&
				jsonifier::internal::getBecauseOtherLibAuthorsResolve<2>(joined) == std::string{ "x" } &&
				std::equal_to<double>{}(jsonifier::internal::getBecauseOtherLibAuthorsResolve<3>(joined), 4.5);
		});

		rt_ut::unit_test<"tuple_empty_tuple_comparisons", true>::assert_eq(true, [] {
			jsonifier::internal::tuple<> a{};
			jsonifier::internal::tuple<> b{};
			return a == b && !(a != b) && !(a < b) && a <= b && a >= b && !(a > b) && decltype(a)::size == 0;
		});

		rt_ut::unit_test<"tuple_structured_bindings_via_std_get", true>::assert_eq(true, [] {
			auto t = jsonifier::internal::makeTuple(7, std::string{ "seven" });
			auto& first  = std::get<0>(t);
			auto& second = std::get<1>(t);
			return first == 7 && second == std::string{ "seven" };
		});

		std::cout << "Tuple validation tests complete." << std::endl;
	}

}
