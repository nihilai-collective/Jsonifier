// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/add_tape_values.hpp
#pragma once

#include "common.hpp"

#include <bit>

namespace add_tape_values_tests {

	inline static std::vector<uint32_t> runAddTapeValues(const jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep>& bitsArr, uint64_t strIdx) {
		jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> cntsArr{};
		uint64_t total{};
		for (uint64_t i = 0; i < jsonifier::simdBlocksPerStep; ++i) {
			cntsArr[i] = static_cast<uint64_t>(std::popcount(bitsArr[i]));
			total += cntsArr[i];
		}
		std::vector<uint32_t> tape(jsonifier::simdBlocksPerStep * 64 + 64, 0xFFFFFFFFu);
		jsonifier::internal::add_tape_values<jsonifier::internal::make_integer_sequence<jsonifier::simdBlocksPerStep>>::impl(bitsArr, cntsArr, tape.data(), strIdx);
		tape.resize(total);
		return tape;
	}

	inline static void runTests() {
		std::cout << "add_tape_values Tests" << std::endl;

		rt_ut::unit_test<"add_tape_values_single_bit_in_first_block", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[0] = 1ull << 5;
			auto tape  = runAddTapeValues(bitsArr, 100);
			return tape.size() == 1 && tape[0] == 105;
		});

		rt_ut::unit_test<"add_tape_values_multiple_bits_ascending_within_one_block", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[0] = (1ull << 0) | (1ull << 3) | (1ull << 10) | (1ull << 63);
			auto tape  = runAddTapeValues(bitsArr, 1000);
			return tape.size() == 4 && tape[0] == 1000 && tape[1] == 1003 && tape[2] == 1010 && tape[3] == 1063;
		});

		rt_ut::unit_test<"add_tape_values_count_not_multiple_of_tape_step", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[0] = (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 5);
			auto tape  = runAddTapeValues(bitsArr, 0);
			return tape.size() == 5 && tape[0] == 1 && tape[1] == 2 && tape[2] == 3 && tape[3] == 4 && tape[4] == 5;
		});

		rt_ut::unit_test<"add_tape_values_all_bits_set_in_one_block", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[0]	  = ~0ull;
			auto tape	  = runAddTapeValues(bitsArr, 0);
			bool allMatch = tape.size() == 64;
			for (uint64_t i = 0; i < 64 && allMatch; ++i) {
				allMatch = tape[i] == i;
			}
			return allMatch;
		});

		rt_ut::unit_test<"add_tape_values_spans_multiple_blocks_in_order", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[0] = (1ull << 1) | (1ull << 2);
			bitsArr[1] = (1ull << 0) | (1ull << 5) | (1ull << 30);
			bitsArr[2] = (1ull << 63);
			auto tape  = runAddTapeValues(bitsArr, 500);
			return tape.size() == 6 && tape[0] == 501 && tape[1] == 502 && tape[2] == 500 + 64 && tape[3] == 500 + 64 + 5 && tape[4] == 500 + 64 + 30 &&
				tape[5] == 500 + 128 + 63;
		});

		rt_ut::unit_test<"add_tape_values_empty_bitmask_produces_no_entries", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			auto tape = runAddTapeValues(bitsArr, 42);
			return tape.empty();
		});

		rt_ut::unit_test<"add_tape_values_last_block_populated_only", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			bitsArr[jsonifier::simdBlocksPerStep - 1] = (1ull << 7) | (1ull << 40);
			auto tape								   = runAddTapeValues(bitsArr, 0);
			const uint64_t base						   = (jsonifier::simdBlocksPerStep - 1) * 64;
			return tape.size() == 2 && tape[0] == base + 7 && tape[1] == base + 40;
		});

		rt_ut::unit_test<"add_tape_values_crosses_tape_step_boundary_within_block", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			uint64_t bits{};
			for (uint64_t i = 0; i < 20; ++i) {
				bits |= (1ull << (i * 3));
			}
			bitsArr[0]	  = bits;
			auto tape	  = runAddTapeValues(bitsArr, 0);
			bool allMatch = tape.size() == 20;
			for (uint64_t i = 0; i < 20 && allMatch; ++i) {
				allMatch = tape[i] == i * 3;
			}
			return allMatch;
		});

		rt_ut::unit_test<"add_tape_values_every_block_populated", true>::assert_eq(true, [] {
			jsonifier::internal::array<uint64_t, jsonifier::simdBlocksPerStep> bitsArr{};
			for (uint64_t i = 0; i < jsonifier::simdBlocksPerStep; ++i) {
				bitsArr[i] = 1ull << (i % 64);
			}
			auto tape	  = runAddTapeValues(bitsArr, 0);
			bool allMatch = tape.size() == jsonifier::simdBlocksPerStep;
			for (uint64_t i = 0; i < jsonifier::simdBlocksPerStep && allMatch; ++i) {
				allMatch = tape[i] == (i * 64 + i);
			}
			return allMatch;
		});

		std::cout << "add_tape_values validation tests complete." << std::endl;
	}

}
