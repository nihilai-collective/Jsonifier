// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/string_class.hpp
#pragma once

#include "common.hpp"

namespace string_class_tests {

	inline static void runTests() {
		std::cout << "String-Class Tests" << std::endl;

		rt_ut::unit_test<"string_default_construct_is_empty", true>::assert_eq(true, [] {
			jsonifier::string s{};
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_construct_from_literal", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello" };
			return s.size() == 5 && s == "hello";
		});

		rt_ut::unit_test<"string_construct_from_empty_literal", true>::assert_eq(true, [] {
			jsonifier::string s{ "" };
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_construct_from_pointer_and_size", true>::assert_eq(true, [] {
			const char* raw = "hello world";
			jsonifier::string s{ raw, 5 };
			return s.size() == 5 && s == "hello";
		});

		rt_ut::unit_test<"string_construct_from_nullptr_pointer", true>::assert_eq(true, [] {
			const char* raw = nullptr;
			jsonifier::string s{ raw };
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_construct_from_std_string", true>::assert_eq(true, [] {
			std::string src{ "from std string" };
			jsonifier::string s{ src };
			return s.size() == src.size() && s == src;
		});

		rt_ut::unit_test<"string_construct_from_single_char", true>::assert_eq(true, [] {
			jsonifier::string s{ 'x' };
			return s.size() == 1 && s[0] == 'x';
		});

		rt_ut::unit_test<"string_copy_constructor_deep_copies", true>::assert_eq(true, [] {
			jsonifier::string original{ "copy me" };
			jsonifier::string copy{ original };
			copy[0] = 'C';
			return original[0] == 'c' && copy[0] == 'C' && original.size() == copy.size();
		});

		rt_ut::unit_test<"string_copy_assignment_deep_copies", true>::assert_eq(true, [] {
			jsonifier::string original{ "assign me" };
			jsonifier::string target{ "placeholder" };
			target	  = original;
			target[0] = 'A';
			return original[0] == 'a' && target[0] == 'A' && original == "assign me";
		});

		rt_ut::unit_test<"string_move_constructor_transfers_data", true>::assert_eq(true, [] {
			jsonifier::string original{ "move me" };
			jsonifier::string moved{ std::move(original) };
			return moved == "move me" && moved.size() == 7;
		});

		rt_ut::unit_test<"string_move_assignment_transfers_data", true>::assert_eq(true, [] {
			jsonifier::string original{ "move assign" };
			jsonifier::string target{};
			target = std::move(original);
			return target == "move assign" && target.size() == 11;
		});

		rt_ut::unit_test<"string_self_copy_assignment_is_safe", true>::assert_eq(true, [] {
			jsonifier::string s{ "self" };
			jsonifier::string& selfRef = s;
			s						   = selfRef;
			return s == "self" && s.size() == 4;
		});

		rt_ut::unit_test<"string_self_move_assignment_is_safe", true>::assert_eq(true, [] {
			jsonifier::string s{ "selfmove" };
			jsonifier::string& selfRef2 = s;
			s							= std::move(selfRef2);
			return s.size() == 8;
		});

		rt_ut::unit_test<"string_equality_same_content", true>::assert_eq(true, [] {
			jsonifier::string a{ "same" };
			jsonifier::string b{ "same" };
			return a == b;
		});

		rt_ut::unit_test<"string_equality_different_content", true>::assert_eq(true, [] {
			jsonifier::string a{ "one" };
			jsonifier::string b{ "two" };
			return !(a == b);
		});

		rt_ut::unit_test<"string_equality_different_lengths", true>::assert_eq(true, [] {
			jsonifier::string a{ "short" };
			jsonifier::string b{ "longer string" };
			return !(a == b);
		});

		rt_ut::unit_test<"string_equality_against_literal", true>::assert_eq(true, [] {
			jsonifier::string s{ "literal" };
			return s == "literal" && !(s == "different");
		});

		rt_ut::unit_test<"string_equality_two_empty_strings", true>::assert_eq(true, [] {
			jsonifier::string a{};
			jsonifier::string b{};
			return a == b;
		});

		rt_ut::unit_test<"string_operator_plus_equals_string", true>::assert_eq(true, [] {
			jsonifier::string s{ "foo" };
			jsonifier::string other{ "bar" };
			s += other;
			return s == "foobar" && s.size() == 6;
		});

		rt_ut::unit_test<"string_operator_plus_equals_literal", true>::assert_eq(true, [] {
			jsonifier::string s{ "foo" };
			s += "baz";
			return s == "foobaz" && s.size() == 6;
		});

		rt_ut::unit_test<"string_operator_plus_equals_char", true>::assert_eq(true, [] {
			jsonifier::string s{ "ab" };
			s += 'c';
			return s == "abc" && s.size() == 3;
		});

		rt_ut::unit_test<"string_operator_plus_equals_onto_empty", true>::assert_eq(true, [] {
			jsonifier::string s{};
			s += "grown";
			return s == "grown" && s.size() == 5;
		});

		rt_ut::unit_test<"string_operator_plus_returns_new_string", true>::assert_eq(true, [] {
			jsonifier::string a{ "foo" };
			jsonifier::string b{ "bar" };
			jsonifier::string c = a + b;
			return c == "foobar" && a == "foo" && b == "bar";
		});

		rt_ut::unit_test<"string_operator_plus_char", true>::assert_eq(true, [] {
			jsonifier::string a{ "ab" };
			jsonifier::string c = a + 'c';
			return c == "abc" && a == "ab";
		});

		rt_ut::unit_test<"string_append_repeatedly_forces_growth", true>::assert_eq(true, [] {
			jsonifier::string s{};
			for (int i = 0; i < 1000; ++i) {
				s += 'a';
			}
			bool allCorrect = s.size() == 1000;
			for (uint64_t i = 0; i < s.size() && allCorrect; ++i) {
				allCorrect = s[i] == 'a';
			}
			return allCorrect;
		});

		rt_ut::unit_test<"string_append_large_block_at_once", true>::assert_eq(true, [] {
			jsonifier::string s{ "start-" };
			std::string bigBlock(500, 'z');
			jsonifier::string bigJStr{ bigBlock };
			s += bigJStr;
			return s.size() == 6 + 500 && s[5] == '-' && s[6] == 'z' && s[s.size() - 1] == 'z';
		});

		rt_ut::unit_test<"string_data_null_terminated_after_construct", true>::assert_eq(true, [] {
			jsonifier::string s{ "term" };
			return s.data()[s.size()] == '\0';
		});

		rt_ut::unit_test<"string_data_null_terminated_after_append", true>::assert_eq(true, [] {
			jsonifier::string s{ "term" };
			s += "inated";
			return s.data()[s.size()] == '\0';
		});

		rt_ut::unit_test<"string_data_null_terminated_after_emplace_back", true>::assert_eq(true, [] {
			jsonifier::string s{};
			for (int i = 0; i < 50; ++i) {
				s.emplace_back('q');
			}
			return s.data()[s.size()] == '\0' && s.size() == 50;
		});

		rt_ut::unit_test<"string_data_null_terminated_after_erase", true>::assert_eq(true, [] {
			jsonifier::string s{ "removeprefix" };
			s.erase(6);
			return s.data()[s.size()] == '\0';
		});

		rt_ut::unit_test<"string_indexing_read_and_write", true>::assert_eq(true, [] {
			jsonifier::string s{ "index" };
			s[0] = 'I';
			return s[0] == 'I' && s[1] == 'n' && s[4] == 'x';
		});

		rt_ut::unit_test<"string_at_valid_index", true>::assert_eq(true, [] {
			jsonifier::string s{ "atcheck" };
			return s.at(0) == 'a' && s.at(6) == 'k';
		});

		rt_ut::unit_test<"string_at_throws_on_out_of_range", true>::assert_eq(true, [] {
			jsonifier::string s{ "short" };
			bool threw = false;
			try {
				[[maybe_unused]] auto c = s.at(100);
			} catch (const std::runtime_error&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"string_at_throws_on_exact_size_index", true>::assert_eq(true, [] {
			jsonifier::string s{ "abc" };
			bool threw = false;
			try {
				[[maybe_unused]] auto c = s.at(3);
			} catch (const std::runtime_error&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"string_clear_resets_size_not_capacity", true>::assert_eq(true, [] {
			jsonifier::string s{ "clearme" };
			auto capBefore = s.capacity();
			s.clear();
			return s.empty() && s.size() == 0 && s.capacity() == capBefore;
		});

		rt_ut::unit_test<"string_clear_on_already_empty_string", true>::assert_eq(true, [] {
			jsonifier::string s{};
			s.clear();
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_resize_grow_pads_with_zero", true>::assert_eq(true, [] {
			jsonifier::string s{ "ab" };
			s.resize(5);
			return s.size() == 5 && s[0] == 'a' && s[1] == 'b' && s.data()[s.size()] == '\0';
		});

		rt_ut::unit_test<"string_resize_shrink_truncates", true>::assert_eq(true, [] {
			jsonifier::string s{ "truncate" };
			s.resize(3);
			return s.size() == 3 && s == "tru";
		});

		rt_ut::unit_test<"string_resize_to_zero_empties", true>::assert_eq(true, [] {
			jsonifier::string s{ "gone" };
			s.resize(0);
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_resize_to_same_size_is_noop", true>::assert_eq(true, [] {
			jsonifier::string s{ "same" };
			s.resize(4);
			return s == "same" && s.size() == 4;
		});

		rt_ut::unit_test<"string_reserve_increases_capacity_without_changing_size", true>::assert_eq(true, [] {
			jsonifier::string s{ "keep" };
			auto sizeBefore = s.size();
			s.reserve(1000);
			return s.capacity() >= 1000 && s.size() == sizeBefore && s == "keep";
		});

		rt_ut::unit_test<"string_reserve_smaller_than_capacity_is_noop", true>::assert_eq(true, [] {
			jsonifier::string s{};
			s.reserve(1000);
			auto capAfterFirst = s.capacity();
			s.reserve(10);
			return s.capacity() == capAfterFirst;
		});

		rt_ut::unit_test<"string_substr_middle", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello world" };
			auto sub = s.substr(6, 5);
			return sub == "world" && sub.size() == 5;
		});

		rt_ut::unit_test<"string_substr_from_start", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello world" };
			auto sub = s.substr(0, 5);
			return sub == "hello";
		});

		rt_ut::unit_test<"string_substr_to_end_default_count", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello world" };
			auto sub = s.substr(6);
			return sub == "world";
		});

		rt_ut::unit_test<"string_substr_count_exceeds_remaining_clamps", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello" };
			auto sub = s.substr(2, 9999);
			return sub == "llo";
		});

		rt_ut::unit_test<"string_substr_zero_count_gives_empty", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello" };
			auto sub = s.substr(1, 0);
			return sub.empty();
		});

		rt_ut::unit_test<"string_substr_position_out_of_range_throws", true>::assert_eq(true, [] {
			jsonifier::string s{ "hi" };
			bool threw = false;
			try {
				[[maybe_unused]] auto sub = s.substr(50);
			} catch (const std::out_of_range&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"string_substr_position_at_size_throws", true>::assert_eq(true, [] {
			jsonifier::string s{ "abc" };
			bool threw = false;
			try {
				[[maybe_unused]] auto sub = s.substr(3);
			} catch (const std::out_of_range&) {
				threw = true;
			}
			return threw;
		});

		rt_ut::unit_test<"string_erase_by_count_removes_prefix", true>::assert_eq(true, [] {
			jsonifier::string s{ "removeThis" };
			s.erase(6);
			return s == "This" && s.size() == 4;
		});

		rt_ut::unit_test<"string_erase_by_iterator_removes_prefix", true>::assert_eq(true, [] {
			jsonifier::string s{ "removeThis" };
			s.erase(s.begin() + 6);
			return s == "This" && s.size() == 4;
		});

		rt_ut::unit_test<"string_erase_zero_count_is_noop", true>::assert_eq(true, [] {
			jsonifier::string s{ "untouched" };
			s.erase(static_cast<jsonifier::string::size_type>(0));
			return s == "untouched" && s.size() == 9;
		});

		rt_ut::unit_test<"string_erase_more_than_size_clamps_to_empty", true>::assert_eq(true, [] {
			jsonifier::string s{ "short" };
			s.erase(9999);
			return s.empty() && s.size() == 0;
		});

		rt_ut::unit_test<"string_emplace_back_single_char", true>::assert_eq(true, [] {
			jsonifier::string s{ "grow" };
			s.emplace_back('!');
			return s == "grow!" && s.size() == 5;
		});

		rt_ut::unit_test<"string_emplace_back_onto_empty", true>::assert_eq(true, [] {
			jsonifier::string s{};
			s.emplace_back('a');
			return s == "a" && s.size() == 1;
		});

		rt_ut::unit_test<"string_insert_single_char_middle", true>::assert_eq(true, [] {
			jsonifier::string s{ "helo" };
			s.insert(s.begin() + 2, 'l');
			return s == "hello" && s.size() == 5;
		});

		rt_ut::unit_test<"string_insert_single_char_at_begin", true>::assert_eq(true, [] {
			jsonifier::string s{ "bc" };
			s.insert(s.begin(), 'a');
			return s == "abc" && s.size() == 3;
		});

		rt_ut::unit_test<"string_insert_single_char_at_end", true>::assert_eq(true, [] {
			jsonifier::string s{ "ab" };
			s.insert(s.end(), 'c');
			return s == "abc" && s.size() == 3;
		});

		rt_ut::unit_test<"string_insert_range_middle", true>::assert_eq(true, [] {
			jsonifier::string s{ "ac" };
			jsonifier::string toInsert{ "b" };
			s.insert(s.begin() + 1, toInsert.begin(), toInsert.end());
			return s == "abc" && s.size() == 3;
		});

		rt_ut::unit_test<"string_insert_range_multi_char", true>::assert_eq(true, [] {
			jsonifier::string s{ "af" };
			jsonifier::string toInsert{ "bcde" };
			s.insert(s.begin() + 1, toInsert.begin(), toInsert.end());
			return s == "abcdef" && s.size() == 6;
		});

		rt_ut::unit_test<"string_append_pointer_and_size", true>::assert_eq(true, [] {
			jsonifier::string s{ "start" };
			const char* extra = "-more";
			s.append(extra, 5);
			return s == "start-more" && s.size() == 10;
		});

		rt_ut::unit_test<"string_append_string_base_overload", true>::assert_eq(true, [] {
			jsonifier::string s{ "a" };
			jsonifier::string other{ "b" };
			s.append(other);
			return s == "ab" && s.size() == 2;
		});

		rt_ut::unit_test<"string_iteration_forward_via_range_for", true>::assert_eq(true, [] {
			jsonifier::string s{ "abc" };
			std::string collected{};
			for (auto c: s) {
				collected += c;
			}
			return collected == "abc";
		});

		rt_ut::unit_test<"string_iteration_reverse", true>::assert_eq(true, [] {
			jsonifier::string s{ "abc" };
			std::string collected{};
			for (auto it = s.rbegin(); it != s.rend(); ++it) {
				collected += *it;
			}
			return collected == "cba";
		});

		rt_ut::unit_test<"string_begin_equals_end_when_empty", true>::assert_eq(true, [] {
			jsonifier::string s{};
			return s.begin() == s.end();
		});

		rt_ut::unit_test<"string_find_locates_substring", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello world" };
			return s.find("world") == 6;
		});

		rt_ut::unit_test<"string_find_returns_npos_when_absent", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello world" };
			return s.find("xyz") == jsonifier::string::npos;
		});

		rt_ut::unit_test<"string_rfind_locates_last_occurrence", true>::assert_eq(true, [] {
			jsonifier::string s{ "abcabc" };
			return s.rfind("abc") == 3;
		});

		rt_ut::unit_test<"string_find_first_of_char_set", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello" };
			return s.findFirstOf("lo") == 2;
		});

		rt_ut::unit_test<"string_find_last_of_char_set", true>::assert_eq(true, [] {
			jsonifier::string s{ "hello" };
			return s.findLastOf("lo") == 4;
		});

		rt_ut::unit_test<"string_find_first_not_of_char_set", true>::assert_eq(true, [] {
			jsonifier::string s{ "aaabbb" };
			return s.findFirstNotOf("a") == 3;
		});

		rt_ut::unit_test<"string_find_last_not_of_char_set", true>::assert_eq(true, [] {
			jsonifier::string s{ "aaabbb" };
			return s.findLastNotOf("b") == 2;
		});

		rt_ut::unit_test<"string_conversion_to_string_view", true>::assert_eq(true, [] {
			jsonifier::string s{ "viewme" };
			std::string_view view = s;
			return view == "viewme" && view.size() == 6;
		});

		rt_ut::unit_test<"string_conversion_to_std_string", true>::assert_eq(true, [] {
			jsonifier::string s{ "convert" };
			auto std_str = static_cast<std::string>(s);
			return std_str == "convert" && std_str.size() == 7;
		});

		rt_ut::unit_test<"string_swap_exchanges_contents", true>::assert_eq(true, [] {
			jsonifier::string a{ "first" };
			jsonifier::string b{ "second-longer" };
			a.swap(b);
			return a == "second-longer" && b == "first";
		});

		rt_ut::unit_test<"string_construct_from_iterator_and_size", true>::assert_eq(true, [] {
			const jsonifier::string source{ "iterconstruct" };
			jsonifier::string s{ source.begin(), 4 };
			return s == "iter" && s.size() == 4;
		});

		rt_ut::unit_test<"string_char_array_plus_string_base", true>::assert_eq(true, [] {
			jsonifier::string s{ "world" };
			jsonifier::string result = "hello " + s;
			return result == "hello world";
		});

		rt_ut::unit_test<"string_repeated_grow_shrink_cycle_preserves_correctness", true>::assert_eq(true, [] {
			jsonifier::string s{ "x" };
			for (int i = 0; i < 20; ++i) {
				s += "yyyyyyyyyy";
				s.resize(s.size() > 5 ? s.size() - 3 : s.size());
			}
			bool nullTerminated = s.data()[s.size()] == '\0';
			return nullTerminated && s.size() > 0;
		});

		rt_ut::unit_test<"string_large_construction_from_std_string", true>::assert_eq(true, [] {
			std::string big(10000, 'm');
			jsonifier::string s{ big };
			bool allM = s.size() == 10000;
			for (uint64_t i = 0; i < s.size() && allM; ++i) {
				allM = s[i] == 'm';
			}
			return allM && s.data()[s.size()] == '\0';
		});

		rt_ut::unit_test<"string_maxSize_is_positive", true>::assert_eq(true, [] {
			return jsonifier::string::maxSize() > 0;
		});

		std::cout << "String-Class tests complete." << std::endl;
	}

}
