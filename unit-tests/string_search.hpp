/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/string_search.hpp
 */
#pragma once

#include "common.hpp"
#include <string_view>

namespace string_search_tests {

	struct search_fuzz_state {
		std::mt19937_64 engine{ 0x9e3779b97f4a7c15ull };
		uint64_t failures{};

		uint64_t next(uint64_t bound) {
			return std::uniform_int_distribution<uint64_t>{ 0, bound }(engine);
		}

		std::string make(uint64_t length, uint64_t alphabet) {
			std::string out(length, '\0');
			for (auto& value: out) {
				value = static_cast<char>('a' + next(alphabet - 1));
			}
			return out;
		}

		void check(const char* name, uint64_t expected, uint64_t actual, std::string_view text, std::string_view needle, uint64_t position) {
			if (expected != actual && ++failures <= 10) {
				std::cout << name << " mismatch: expected " << static_cast<int64_t>(expected) << " got " << static_cast<int64_t>(actual) << " position "
						  << static_cast<int64_t>(position) << " needle \"" << needle << "\" text \"" << text << "\"" << std::endl;
			}
		}
	};

	struct search_fuzz_case {
		static void impl(search_fuzz_state& state, uint64_t offset) {
			const uint64_t length{ state.next(300) };
			const uint64_t alphabet{ 1 + state.next(5) };
			std::vector<char> storage(offset + length + 1);
			const std::string source{ state.make(length, alphabet) };
			std::copy(source.begin(), source.end(), storage.begin() + static_cast<int64_t>(offset));
			const std::string_view text{ storage.data() + offset, length };
			const jsonifier::string_view view{ storage.data() + offset, length };

			const uint64_t needleLength{ state.next(state.next(1) ? 3 : 40) };
			std::string needle{ state.make(needleLength, alphabet) };
			if (length > 0 && needleLength > 0 && needleLength <= length && state.next(1)) {
				needle = source.substr(state.next(length - needleLength), needleLength);
			}
			const uint64_t position{ state.next(3) == 0 ? jsonifier::string_view::npos : state.next(length + 2) };
			const char single{ static_cast<char>('a' + state.next(alphabet)) };
			const std::string_view singleView{ &single, 1 };

			state.check("find", text.find(needle, position), view.find(needle.data(), position, needle.size()), text, needle, position);
			state.check("rfind", text.rfind(needle, position), view.rfind(needle.data(), position, needle.size()), text, needle, position);
			state.check("findFirstOf", text.find_first_of(needle, position), view.findFirstOf(needle.data(), position, needle.size()), text, needle, position);
			state.check("findFirstNotOf", text.find_first_not_of(needle, position), view.findFirstNotOf(needle.data(), position, needle.size()), text, needle, position);
			state.check("findLastOf", text.find_last_of(needle, position), view.findLastOf(needle.data(), position, needle.size()), text, needle, position);
			state.check("findLastNotOf", text.find_last_not_of(needle, position), view.findLastNotOf(needle.data(), position, needle.size()), text, needle, position);
			state.check("find(char)", text.find(single, position), view.find(single, position), text, singleView, position);
			state.check("rfind(char)", text.rfind(single, position), view.rfind(single, position), text, singleView, position);
			state.check("findFirstNotOf(char)", text.find_first_not_of(single, position), view.findFirstNotOf(single, position), text, singleView, position);
			state.check("findLastNotOf(char)", text.find_last_not_of(single, position), view.findLastNotOf(single, position), text, singleView, position);
		}
	};

	struct constexpr_search_cases {
		static constexpr bool impl() {
			constexpr jsonifier::string_view view{ "hello world, hello jsonifier" };
			return view.find("hello", 1) == 13 && view.rfind("hello") == 13 && view.findFirstOf("wj") == 6 && view.findLastNotOf("re") == 25 &&
				view.find('z') == jsonifier::string_view::npos;
		}
	};

	inline void runTests() {
		std::cout << "String-Search Tests" << std::endl;

		static_assert(constexpr_search_cases::impl());

		rt_ut::unit_test<"string_search_fuzz_matches_std_string_view", true>::assert_eq(0ull, [] {
			search_fuzz_state state{};
			for (uint64_t iteration = 0; iteration < 200000; ++iteration) {
				search_fuzz_case::impl(state, iteration % 64);
			}
			return state.failures;
		});

		rt_ut::unit_test<"string_search_string_forwards_to_view", true>::assert_eq(true, [] {
			jsonifier::string value{ "alpha::beta::gamma" };
			return value.find("::") == 5 && value.rfind("::") == 11 && value.findFirstOf(":") == 5 && value.findLastNotOf("a") == 16;
		});
	}

}
