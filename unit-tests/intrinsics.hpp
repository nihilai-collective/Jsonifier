/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/intrinsics.hpp
 */
#pragma once

#include "common.hpp"

namespace intrinsics_tests {

#if JSONIFIER_AVX512
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_512;
#elif JSONIFIER_AVX2
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_256;
#elif JSONIFIER_AVX
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_128;
#elif JSONIFIER_SVE2
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_128;
#elif JSONIFIER_NEON
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_128;
#else
	using active_simd_t = jsonifier::internal::jsonifier_simd_int_128;
#endif

	inline constexpr uint64_t activeWidth{ sizeof(active_simd_t) };

	inline void runTests() {
		std::cout << "SIMD Intrinsics Layer Tests (active backend: " << jsonifier::cpuArchName << ", width: " << activeWidth << " bytes)" << std::endl;

		rt_ut::unit_test<"op_cmp_eq_all_match", true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto b		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto allMatch = jsonifier::internal::simd::opCmpEq(a, b);
			auto noMatch  = jsonifier::internal::simd::opCmpEq(a, jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A)));
			return allMatch != noMatch && allMatch == jsonifier::internal::simd::opCmpEq(a, b);
		});

		rt_ut::unit_test<"op_cmp_eq_no_match", true>::assert_eq(true, [] {
			auto a		   = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto b		   = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			auto noMatch   = jsonifier::internal::simd::opCmpEq(a, b);
			auto selfMatch = jsonifier::internal::simd::opCmpEq(a, a);
			return noMatch != selfMatch;
		});

		rt_ut::unit_test<"op_cmp_eq_single_byte_positions_distinct", true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			auto needle = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			std::vector<decltype(jsonifier::internal::simd::opCmpEq(jsonifier::internal::simd::gatherValues<active_simd_t>(buffer), needle))> results;
			for (uint64_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x41, activeWidth);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				results.push_back(jsonifier::internal::simd::opCmpEq(loaded, needle));
			}
			for (uint64_t i = 0; i < results.size(); ++i) {
				for (uint64_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "COLLISION between byte position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		rt_ut::unit_test<"op_bit_mask_all_zero", true>::assert_eq(true, [] {
			auto zero	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			auto ones	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto zeroMask = jsonifier::internal::simd::opBitMask(zero);
			auto onesMask = jsonifier::internal::simd::opBitMask(ones);
			return zeroMask != onesMask && zeroMask == jsonifier::internal::simd::opBitMask(zero);
		});

		rt_ut::unit_test<"op_bit_mask_all_ones", true>::assert_eq(true, [] {
			auto ones	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto zero	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			auto onesMask = jsonifier::internal::simd::opBitMask(ones);
			auto zeroMask = jsonifier::internal::simd::opBitMask(zero);
			return onesMask != zeroMask && onesMask == jsonifier::internal::simd::opBitMask(ones);
		});

		rt_ut::unit_test<"op_bit_mask_single_bit_sweep_distinct", true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			std::memset(buffer, 0x00, activeWidth);
			auto baseline	  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
			auto baselineMask = jsonifier::internal::simd::opBitMask(baseline);
			std::vector<decltype(baselineMask)> results;
			for (uint64_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x00, activeWidth);
				buffer[targetPos] = 0xFF;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				results.push_back(jsonifier::internal::simd::opBitMask(loaded));
			}
			for (uint64_t i = 0; i < results.size(); ++i) {
				if (results[i] == baselineMask) {
					std::cout << "SINGLE-BIT-SWEEP position " << i << " matched all-zero baseline unexpectedly" << std::endl;
					return false;
				}
				for (uint64_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "SINGLE-BIT-SWEEP collision between position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		rt_ut::unit_test<"op_bit_mask_raw_structural_sweep_distinct", true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			auto needle = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			std::memset(buffer, 0x41, activeWidth);
			auto baselineLoaded = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
			auto baselineRaw	= jsonifier::internal::simd::opCmpEqRaw(baselineLoaded, needle);
			auto baselinePacked = jsonifier::internal::simd::opBitMaskRaw(baselineRaw);
			std::vector<decltype(baselinePacked)> results;
			for (uint64_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x41, activeWidth);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				auto raw		  = jsonifier::internal::simd::opCmpEqRaw(loaded, needle);
				results.push_back(jsonifier::internal::simd::opBitMaskRaw(raw));
			}
			for (uint64_t i = 0; i < results.size(); ++i) {
				if (results[i] == baselinePacked) {
					std::cout << "OPBITMASKRAW position " << i << " matched no-match baseline unexpectedly" << std::endl;
					return false;
				}
				for (uint64_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "OPBITMASKRAW collision between position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		rt_ut::unit_test<"op_and_basic", true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xF0));
			auto b	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x0F));
			auto res = jsonifier::internal::simd::opAnd(a, b);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		rt_ut::unit_test<"op_or_basic", true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xF0));
			auto b		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x0F));
			auto res	  = jsonifier::internal::simd::opOr(a, b);
			auto expected = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto match	  = jsonifier::internal::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::internal::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		rt_ut::unit_test<"op_xor_self_is_zero", true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x5A));
			auto res = jsonifier::internal::simd::opXor(a, a);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		rt_ut::unit_test<"op_subs_saturates_at_zero", true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x05));
			auto b	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x10));
			auto res = jsonifier::internal::simd::opSubs(a, b);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		rt_ut::unit_test<"op_sr_li_shift", true>::assert_eq(true, [] {
			alignas(64) uint8_t src[activeWidth];
			for (uint64_t i = 0; i < activeWidth; ++i) {
				src[i] = 0x80;
			}
			auto a	 = jsonifier::internal::simd::gatherValues<active_simd_t>(src);
			auto res = jsonifier::internal::simd::opSrLi<4>(a);
			alignas(64) uint8_t out[activeWidth];
			jsonifier::internal::simd::store(res, out);
			for (uint64_t i = 0; i < activeWidth; ++i) {
				if (out[i] != 0x08) {
					std::cout << "OP_SR_LI byte " << i << " expected 0x08 got 0x" << std::hex << static_cast<uint32_t>(out[i]) << std::dec << std::endl;
					return false;
				}
			}
			return true;
		});

		rt_ut::unit_test<"op_align_r_boundary_shift", true>::assert_eq(true, [] {
			alignas(64) uint8_t currentBuf[activeWidth];
			alignas(64) uint8_t previousBuf[activeWidth];
			std::memset(currentBuf, 0x41, activeWidth);
			std::memset(previousBuf, 0x42, activeWidth);
			auto current  = jsonifier::internal::simd::gatherValues<active_simd_t>(currentBuf);
			auto previous = jsonifier::internal::simd::gatherValues<active_simd_t>(previousBuf);
			auto shifted  = jsonifier::internal::simd::opPrev<15>(current, previous);
			alignas(64) uint8_t shiftedOut[activeWidth];
			jsonifier::internal::simd::store(shifted, shiftedOut);
			return shiftedOut[0] == 0x42 && shiftedOut[activeWidth - 1] == 0x41;
		});

		rt_ut::unit_test<"is_ascii_true_for_ascii", true>::assert_eq(true, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			return jsonifier::internal::simd::isAscii(v);
		});

		rt_ut::unit_test<"is_ascii_false_for_high_bit", true>::assert_eq(false, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x80));
			return jsonifier::internal::simd::isAscii(v);
		});

		rt_ut::unit_test<"store_round_trip", true>::assert_eq(true, [] {
			alignas(64) uint8_t src[activeWidth];
			alignas(64) uint8_t dst[activeWidth];
			for (uint64_t i = 0; i < activeWidth; ++i) {
				src[i] = static_cast<uint8_t>(i * 7 + 1);
			}
			auto loaded = jsonifier::internal::simd::gatherValues<active_simd_t>(src);
			jsonifier::internal::simd::store(loaded, dst);
			return std::memcmp(src, dst, activeWidth) == 0;
		});

		rt_ut::unit_test<"store_u_round_trip_unaligned", true>::assert_eq(true, [] {
			alignas(64) uint8_t srcBuf[activeWidth * 2];
			alignas(64) uint8_t dstBuf[activeWidth * 2];
			for (uint64_t i = 0; i < activeWidth * 2; ++i) {
				srcBuf[i] = static_cast<uint8_t>(i * 3 + 2);
			}
			uint64_t failCount = 0;
			for (uint64_t offset = 0; offset < activeWidth; ++offset) {
				std::memset(dstBuf, 0x00, activeWidth * 2);
				auto loaded = jsonifier::internal::simd::gatherValuesU<active_simd_t>(srcBuf + offset);
				jsonifier::internal::simd::storeU(loaded, dstBuf + offset);
				if (std::memcmp(srcBuf + offset, dstBuf + offset, activeWidth) != 0) {
					++failCount;
				}
			}
			return failCount == 0;
		});

		std::cout << "SIMD Intrinsics validation tests complete." << std::endl;
	}

}
