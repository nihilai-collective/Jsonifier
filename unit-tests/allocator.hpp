// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/allocator.hpp
#pragma once

#include "common.hpp"

namespace allocator_tests {

	struct alloc_pod {
		int32_t a;
		double b;
	};

	inline static void runTests() {
		std::cout << "Allocator Tests" << std::endl;

		rt_ut::unit_test<"allocator_round_up_pow2_zero_stays_zero", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 16 }>(uint64_t{ 0 });
		});

		rt_ut::unit_test<"allocator_round_up_pow2_exact_multiple_unchanged", true>::assert_eq(16ULL, [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 16 }>(uint64_t{ 16 });
		});

		rt_ut::unit_test<"allocator_round_up_pow2_rounds_to_next", true>::assert_eq(static_cast<uint64_t>(32), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 16 }>(uint64_t{ 17 });
		});

		rt_ut::unit_test<"allocator_round_up_pow2_boundary_below_next", true>::assert_eq(static_cast<uint64_t>(32), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 16 }>(uint64_t{ 31 });
		});

		rt_ut::unit_test<"allocator_round_down_pow2_zero_stays_zero", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 16 }>(uint64_t{ 0 });
		});

		rt_ut::unit_test<"allocator_round_down_pow2_exact_multiple_unchanged", true>::assert_eq(16ULL, [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 16 }>(uint64_t{ 16 });
		});

		rt_ut::unit_test<"allocator_round_down_pow2_truncates", true>::assert_eq(16ULL, [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 16 }>(uint64_t{ 31 });
		});

		rt_ut::unit_test<"allocator_round_down_pow2_boundary_above_prev", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 16 }>(uint64_t{ 15 });
		});

		rt_ut::unit_test<"allocator_round_up_non_pow2_zero_stays_zero", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 10 }>(uint64_t{ 0 });
		});

		rt_ut::unit_test<"allocator_round_up_non_pow2_exact_multiple_unchanged", true>::assert_eq(static_cast<uint64_t>(10), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 10 }>(uint64_t{ 10 });
		});

		rt_ut::unit_test<"allocator_round_up_non_pow2_rounds_to_next", true>::assert_eq(static_cast<uint64_t>(20), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 10 }>(uint64_t{ 11 });
		});

		rt_ut::unit_test<"allocator_round_up_non_pow2_boundary_below_next", true>::assert_eq(static_cast<uint64_t>(20), [] {
			return jsonifier::internal::roundUpToMultiple<uint64_t{ 10 }>(uint64_t{ 19 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_zero_stays_zero", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 10 }>(uint64_t{ 0 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_exact_multiple_unchanged", true>::assert_eq(static_cast<uint64_t>(10), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 10 }>(uint64_t{ 10 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_truncates", true>::assert_eq(static_cast<uint64_t>(10), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 10 }>(uint64_t{ 19 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_boundary_above_prev", true>::assert_eq(static_cast<uint64_t>(0), [] {
			return jsonifier::internal::roundDownToMultiple<uint64_t{ 10 }>(uint64_t{ 9 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_negative_floors_toward_neg_infinity", true>::assert_eq(static_cast<int64_t>(-3), [] {
			return jsonifier::internal::roundDownToMultiple<3, int64_t>(int64_t{ -1 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_negative_exact_multiple_unchanged", true>::assert_eq(static_cast<int64_t>(-3), [] {
			return jsonifier::internal::roundDownToMultiple<3, int64_t>(int64_t{ -3 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_negative_crosses_boundary", true>::assert_eq(static_cast<int64_t>(-6), [] {
			return jsonifier::internal::roundDownToMultiple<3, int64_t>(int64_t{ -4 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_negative_zero_stays_zero", true>::assert_eq(static_cast<int64_t>(0), [] {
			return jsonifier::internal::roundDownToMultiple<3, int64_t>(int64_t{ 0 });
		});

		rt_ut::unit_test<"allocator_round_down_non_pow2_negative_positive_side_still_truncates", true>::assert_eq(static_cast<int64_t>(3), [] {
			return jsonifier::internal::roundDownToMultiple<3, int64_t>(int64_t{ 4 });
		});

		rt_ut::unit_test<"allocator_allocate_zero_returns_null", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			auto* p = alloc.allocate(0);
			return p == nullptr;
		});

		rt_ut::unit_test<"allocator_deallocate_null_is_safe", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			alloc.deallocate(nullptr, 0);
			return true;
		});

		rt_ut::unit_test<"allocator_allocate_returns_simd_aligned_pointer", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			auto* p = alloc.allocate(64);
			const bool aligned = (reinterpret_cast<uintptr_t>(p) % jsonifier::internal::alloc_wrapper<int32_t>::alignment) == 0;
			alloc.deallocate(p, 64);
			return p != nullptr && aligned;
		});

		rt_ut::unit_test<"allocator_construct_destroy_round_trips_value", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<alloc_pod> alloc{};
			auto* p = alloc.allocate(1);
			alloc.construct(p, alloc_pod{ 42, 3.5 });
			const bool matches = p->a == 42 && std::equal_to<double>{}(p->b, 3.5);
			alloc.destroy(p);
			alloc.deallocate(p, 1);
			return matches;
		});

		rt_ut::unit_test<"allocator_equality_always_true", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> allocA{};
			jsonifier::internal::alloc_wrapper<int32_t> allocB{};
			return allocA == allocB;
		});

		rt_ut::unit_test<"allocator_inequality_always_false", true>::assert_eq(false, [] {
			jsonifier::internal::alloc_wrapper<int32_t> allocA{};
			jsonifier::internal::alloc_wrapper<int32_t> allocB{};
			return allocA != allocB;
		});

		rt_ut::unit_test<"allocator_max_size_matches_size_bound", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			return alloc.maxSize() == static_cast<uint64_t>(-1) / sizeof(int32_t);
		});

		rt_ut::unit_test<"allocator_rebind_produces_matching_type", true>::assert_eq(true, [] {
			return std::is_same_v<jsonifier::internal::alloc_wrapper<int32_t>::rebind<double>::other, jsonifier::internal::alloc_wrapper<double>>;
		});

		rt_ut::unit_test<"allocator_converting_constructor_from_other_specialization", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<double> allocSrc{};
			jsonifier::internal::alloc_wrapper<int32_t> allocDst{ allocSrc };
			return allocDst == jsonifier::internal::alloc_wrapper<int32_t>{};
		});

		rt_ut::unit_test<"allocator_bulk_allocate_construct_destroy_round_trip", true>::assert_eq(true, [] {
			static constexpr uint64_t elementCount{ 4096 };
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			auto* p = alloc.allocate(elementCount);
			for (uint64_t i = 0; i < elementCount; ++i) {
				alloc.construct(p + i, static_cast<int32_t>(i));
			}
			bool allMatch = p != nullptr;
			for (uint64_t i = 0; i < elementCount && allMatch; ++i) {
				allMatch = p[i] == static_cast<int32_t>(i);
			}
			for (uint64_t i = 0; i < elementCount; ++i) {
				alloc.destroy(p + i);
			}
			alloc.deallocate(p, elementCount);
			return allMatch;
		});

		rt_ut::unit_test<"allocator_reallocate_to_larger_count_preserves_new_contents", true>::assert_eq(true, [] {
			jsonifier::internal::alloc_wrapper<int32_t> alloc{};
			auto* small = alloc.allocate(16);
			for (uint64_t i = 0; i < 16; ++i) {
				alloc.construct(small + i, static_cast<int32_t>(7));
			}
			alloc.deallocate(small, 16);
			auto* large = alloc.allocate(512);
			for (uint64_t i = 0; i < 512; ++i) {
				alloc.construct(large + i, static_cast<int32_t>(3));
			}
			bool allMatch = large != nullptr;
			for (uint64_t i = 0; i < 512 && allMatch; ++i) {
				allMatch = large[i] == 3;
			}
			alloc.deallocate(large, 512);
			return allMatch;
		});

		rt_ut::unit_test<"allocator_large_allocation_crosses_huge_page_threshold", true>::assert_eq(true, [] {
			static constexpr uint64_t elementCount{ 600000 };
			jsonifier::internal::alloc_wrapper<double> alloc{};
			auto* p = alloc.allocate(elementCount);
			for (uint64_t i = 0; i < elementCount; ++i) {
				alloc.construct(p + i, static_cast<double>(i));
			}
			bool allMatch = p != nullptr;
			for (uint64_t i = 0; i < elementCount && allMatch; i += 9973) {
				allMatch = static_cast<uint64_t>(p[i]) == i;
			}
			for (uint64_t i = 0; i < elementCount; ++i) {
				alloc.destroy(p + i);
			}
			alloc.deallocate(p, elementCount);
			return allMatch && (elementCount * sizeof(double)) > (2ull * 1024ull * 1024ull * 2ull);
		});

		std::cout << "Allocator validation tests complete." << std::endl;
	}

}
