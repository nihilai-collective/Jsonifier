// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/compare.hpp
#pragma once

#include "common.hpp"

namespace compare_tests {

	template<uint64_t length> inline static bool memcharFindsNeedleAtEveryPosition() {
		for (uint64_t pos = 0; pos < length; ++pos) {
			std::string buffer(length, 'x');
			buffer[pos] = '"';
			const auto* result = jsonifier::internal::char_comparison<'"', char>::memchar(buffer.data(), length);
			if (result != buffer.data() + pos) {
				std::cout << "MEMCHAR failed to find needle at position " << pos << " for length " << length << std::endl;
				return false;
			}
		}
		return true;
	}

	template<uint64_t length> inline static bool memcharReturnsNullWhenAbsent() {
		std::string buffer(length, 'x');
		const auto* result = jsonifier::internal::char_comparison<'"', char>::memchar(buffer.data(), length);
		if (result != nullptr) {
			std::cout << "MEMCHAR unexpectedly found a needle for length " << length << std::endl;
			return false;
		}
		return true;
	}

	template<rt_ut::string_literal testName, uint64_t length> inline static void runMemcharLengthTest() {
		rt_ut::unit_test<testName, true>::assert_eq(true, [] {
			return memcharFindsNeedleAtEveryPosition<length>() && memcharReturnsNullWhenAbsent<length>();
		});
	}

	template<uint64_t length> inline static bool compareMatchesIdenticalBuffers() {
		std::string a(length, 'z');
		std::string b(length, 'z');
		return jsonifier::internal::comparison::compare(a.data(), b.data(), length);
	}

	template<uint64_t length> inline static bool compareDetectsMismatchAtEveryPosition() {
		for (uint64_t pos = 0; pos < length; ++pos) {
			std::string a(length, 'z');
			std::string b(length, 'z');
			b[pos] = static_cast<char>(~b[pos]);
			if (jsonifier::internal::comparison::compare(a.data(), b.data(), length)) {
				std::cout << "COMPARE failed to detect mismatch at position " << pos << " for length " << length << std::endl;
				return false;
			}
		}
		return true;
	}

	template<rt_ut::string_literal testName, uint64_t length> inline static void runCompareLengthTest() {
		rt_ut::unit_test<testName, true>::assert_eq(true, [] {
			return compareMatchesIdenticalBuffers<length>() && compareDetectsMismatchAtEveryPosition<length>();
		});
	}

	inline static void runTests() {
		std::cout << "compare.hpp Tests" << std::endl;

		rt_ut::unit_test<"memchar_zero_length_returns_null", true>::assert_eq(true, [] {
			std::string buffer(8, 'x');
			return jsonifier::internal::char_comparison<'"', char>::memchar(buffer.data(), 0) == nullptr;
		});

		runMemcharLengthTest<"memchar_length_1", 1>();
		runMemcharLengthTest<"memchar_length_2", 2>();
		runMemcharLengthTest<"memchar_length_3", 3>();
		runMemcharLengthTest<"memchar_length_4", 4>();
		runMemcharLengthTest<"memchar_length_5", 5>();
		runMemcharLengthTest<"memchar_length_7", 7>();
		runMemcharLengthTest<"memchar_length_8", 8>();
		runMemcharLengthTest<"memchar_length_9", 9>();
		runMemcharLengthTest<"memchar_length_15", 15>();
		runMemcharLengthTest<"memchar_length_16_sse_boundary", 16>();
		runMemcharLengthTest<"memchar_length_17", 17>();
		runMemcharLengthTest<"memchar_length_31", 31>();
		runMemcharLengthTest<"memchar_length_32_avx2_boundary", 32>();
		runMemcharLengthTest<"memchar_length_33", 33>();
		runMemcharLengthTest<"memchar_length_63", 63>();
		runMemcharLengthTest<"memchar_length_64_avx512_boundary", 64>();
		runMemcharLengthTest<"memchar_length_65", 65>();
		runMemcharLengthTest<"memchar_length_100_multi_vector", 100>();
		runMemcharLengthTest<"memchar_length_200_multi_vector", 200>();

		rt_ut::unit_test<"memchar_different_needle_character_still_matches", true>::assert_eq(true, [] {
			std::string buffer(40, 'x');
			buffer[23] = '\n';
			const auto* result = jsonifier::internal::char_comparison<'\n', char>::memchar(buffer.data(), buffer.size());
			return result == buffer.data() + 23;
		});

		rt_ut::unit_test<"memchar_needle_never_matches_when_absent_for_different_needle", true>::assert_eq(true, [] {
			std::string buffer(40, 'x');
			const auto* result = jsonifier::internal::char_comparison<'\n', char>::memchar(buffer.data(), buffer.size());
			return result == nullptr;
		});

		rt_ut::unit_test<"compare_zero_length_is_trivially_equal", true>::assert_eq(true, [] {
			std::string a{};
			std::string b{};
			return jsonifier::internal::comparison::compare(a.data(), b.data(), 0);
		});

		runCompareLengthTest<"compare_length_1", 1>();
		runCompareLengthTest<"compare_length_2", 2>();
		runCompareLengthTest<"compare_length_3", 3>();
		runCompareLengthTest<"compare_length_4", 4>();
		runCompareLengthTest<"compare_length_5", 5>();
		runCompareLengthTest<"compare_length_7", 7>();
		runCompareLengthTest<"compare_length_8", 8>();
		runCompareLengthTest<"compare_length_9", 9>();
		runCompareLengthTest<"compare_length_15", 15>();
		runCompareLengthTest<"compare_length_16_sse_boundary", 16>();
		runCompareLengthTest<"compare_length_17", 17>();
		runCompareLengthTest<"compare_length_31", 31>();
		runCompareLengthTest<"compare_length_32_avx2_boundary", 32>();
		runCompareLengthTest<"compare_length_33", 33>();
		runCompareLengthTest<"compare_length_63", 63>();
		runCompareLengthTest<"compare_length_64_avx512_boundary", 64>();
		runCompareLengthTest<"compare_length_65", 65>();
		runCompareLengthTest<"compare_length_100_multi_vector", 100>();
		runCompareLengthTest<"compare_length_200_multi_vector", 200>();

		std::cout << "compare.hpp validation tests complete." << std::endl;
	}

}
