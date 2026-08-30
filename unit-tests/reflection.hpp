// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/reflection.hpp
#pragma once

#include "common.hpp"

namespace reflection_tests {

	struct simple_struct {
		int32_t id{};
		std::string name{};
	};

	struct many_members_struct {
		int32_t a{};
		int32_t b{};
		int32_t c{};
		int32_t d{};
		int32_t e{};
	};

	struct underscore_names_struct {
		int32_t leading_word_underscore{};
		int32_t trailing_underscore_{};
		int32_t multiple_word_underscores{};
	};

	struct varied_length_names_struct {
		int32_t x{};
		int32_t averyveryveryverylongmembernamethatgoesonandonandon{};
	};

	struct numeric_suffix_struct {
		int32_t value1{};
		int32_t value2{};
		int32_t value10{};
	};

	namespace nested {
		struct inner_struct {
			int32_t depth{};
			std::string label{};
		};
	}

	inline static std::string extractName(const auto& sv) {
		return std::string{ sv.data(), sv.size() };
	}

	inline static void runTests() {
		std::cout << "Reflection getName<> Tests" << std::endl;

		rt_ut::unit_test<"reflection_short_member_name", true>::assert_eq(std::string{ "id" }, [] {
			return extractName(jsonifier::internal::getName<&simple_struct::id>());
		});

		rt_ut::unit_test<"reflection_second_member_of_same_struct", true>::assert_eq(std::string{ "name" }, [] {
			return extractName(jsonifier::internal::getName<&simple_struct::name>());
		});

		rt_ut::unit_test<"reflection_five_distinct_members_no_cross_contamination", true>::assert_eq(std::make_tuple(std::string{ "a" }, std::string{ "b" }, std::string{ "c" },
																				   std::string{ "d" }, std::string{ "e" }),
			[] {
				return std::make_tuple(extractName(jsonifier::internal::getName<&many_members_struct::a>()), extractName(jsonifier::internal::getName<&many_members_struct::b>()),
					extractName(jsonifier::internal::getName<&many_members_struct::c>()), extractName(jsonifier::internal::getName<&many_members_struct::d>()),
					extractName(jsonifier::internal::getName<&many_members_struct::e>()));
			});

		rt_ut::unit_test<"reflection_leading_word_underscore_name", true>::assert_eq(std::string{ "leading_word_underscore" }, [] {
			return extractName(jsonifier::internal::getName<&underscore_names_struct::leading_word_underscore>());
		});

		rt_ut::unit_test<"reflection_trailing_underscore_name", true>::assert_eq(std::string{ "trailing_underscore_" }, [] {
			return extractName(jsonifier::internal::getName<&underscore_names_struct::trailing_underscore_>());
		});

		rt_ut::unit_test<"reflection_multiple_underscores_name", true>::assert_eq(std::string{ "multiple_word_underscores" }, [] {
			return extractName(jsonifier::internal::getName<&underscore_names_struct::multiple_word_underscores>());
		});

		rt_ut::unit_test<"reflection_single_char_member_name", true>::assert_eq(std::string{ "x" }, [] {
			return extractName(jsonifier::internal::getName<&varied_length_names_struct::x>());
		});

		rt_ut::unit_test<"reflection_very_long_member_name", true>::assert_eq(std::string{ "averyveryveryverylongmembernamethatgoesonandonandon" }, [] {
			return extractName(jsonifier::internal::getName<&varied_length_names_struct::averyveryveryverylongmembernamethatgoesonandonandon>());
		});

		rt_ut::unit_test<"reflection_numeric_suffix_one", true>::assert_eq(std::string{ "value1" }, [] {
			return extractName(jsonifier::internal::getName<&numeric_suffix_struct::value1>());
		});

		rt_ut::unit_test<"reflection_numeric_suffix_ten_not_truncated_to_one", true>::assert_eq(std::string{ "value10" }, [] {
			return extractName(jsonifier::internal::getName<&numeric_suffix_struct::value10>());
		});

		rt_ut::unit_test<"reflection_member_of_struct_in_nested_namespace", true>::assert_eq(std::string{ "depth" }, [] {
			return extractName(jsonifier::internal::getName<&nested::inner_struct::depth>());
		});

		rt_ut::unit_test<"reflection_second_member_of_nested_namespace_struct", true>::assert_eq(std::string{ "label" }, [] {
			return extractName(jsonifier::internal::getName<&nested::inner_struct::label>());
		});

		rt_ut::unit_test<"reflection_name_usable_at_compile_time", true>::assert_eq(true, [] {
			constexpr auto sv = jsonifier::internal::getName<&simple_struct::id>();
			static_assert(sv.size() == 2, "getName<> must be usable in a constexpr/static_assert context");
			return sv.size() == 2 && sv[0] == 'i' && sv[1] == 'd';
		});

		std::cout << "Reflection getName<> tests complete." << std::endl;
	}

}
