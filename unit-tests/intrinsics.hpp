// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/intrinsics.hpp
#pragma once

#include "common.hpp"

namespace intrinsics_tests {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
	using active_simd_t = jsonifier::jsonifier_simd_int_512;
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
	using active_simd_t = jsonifier::jsonifier_simd_int_256;
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX)
	using active_simd_t = jsonifier::jsonifier_simd_int_128;
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	using active_simd_t = jsonifier::jsonifier_simd_int_128;
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	using active_simd_t = jsonifier::jsonifier_simd_int_128;
#else
	using active_simd_t = jsonifier::jsonifier_simd_int_128;
#endif

	static constexpr uint64_t activeWidth{ sizeof(active_simd_t) };

	inline static void runTests() {
		std::cout << "SIMD Intrinsics Layer Tests (active backend: " << jsonifier::cpu_arch_name << ", width: " << activeWidth << " bytes)" << std::endl;

		static constexpr rt_ut::string_literal opCmpEqAllMatchName{ "op_cmp_eq_all_match" };
		rt_ut::unit_test<opCmpEqAllMatchName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto b		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto allMatch = jsonifier::internal::simd::opCmpEq(a, b);
			auto noMatch  = jsonifier::internal::simd::opCmpEq(a, jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A)));
			return allMatch != noMatch && allMatch == jsonifier::internal::simd::opCmpEq(a, b);
		});

		static constexpr rt_ut::string_literal opCmpEqNoMatchName{ "op_cmp_eq_no_match" };
		rt_ut::unit_test<opCmpEqNoMatchName, true>::assert_eq(true, [] {
			auto a		   = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			auto b		   = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			auto noMatch   = jsonifier::internal::simd::opCmpEq(a, b);
			auto selfMatch = jsonifier::internal::simd::opCmpEq(a, a);
			return noMatch != selfMatch;
		});

		static constexpr rt_ut::string_literal opCmpEqSingleBytePositionsName{ "op_cmp_eq_single_byte_positions_distinct" };
		rt_ut::unit_test<opCmpEqSingleBytePositionsName, true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			auto needle = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			std::vector<decltype(jsonifier::internal::simd::opCmpEq(jsonifier::internal::simd::gatherValues<active_simd_t>(buffer), needle))> results;
			for (size_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x41, activeWidth);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				results.push_back(jsonifier::internal::simd::opCmpEq(loaded, needle));
			}
			for (size_t i = 0; i < results.size(); ++i) {
				for (size_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "COLLISION between byte position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal opCmpLtBasicName{ "op_cmp_lt_basic" };
		rt_ut::unit_test<opCmpLtBasicName, true>::assert_eq(true, [] {
			auto lo		 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x20));
			auto hi		 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x21));
			auto ltTrue	 = jsonifier::internal::simd::opCmpLt(lo, hi);
			auto ltFalse = jsonifier::internal::simd::opCmpLt(hi, lo);
			auto ltSelf	 = jsonifier::internal::simd::opCmpLt(lo, lo);
			return ltTrue != ltFalse && ltTrue == jsonifier::internal::simd::opCmpLt(lo, hi) && ltFalse == ltSelf;
		});

		static constexpr rt_ut::string_literal opBitMaskAllZeroName{ "op_bit_mask_all_zero" };
		rt_ut::unit_test<opBitMaskAllZeroName, true>::assert_eq(true, [] {
			auto zero	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			auto ones	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto zeroMask = jsonifier::internal::simd::opBitMask(zero);
			auto onesMask = jsonifier::internal::simd::opBitMask(ones);
			return zeroMask != onesMask && zeroMask == jsonifier::internal::simd::opBitMask(zero);
		});

		static constexpr rt_ut::string_literal opBitMaskAllOnesName{ "op_bit_mask_all_ones" };
		rt_ut::unit_test<opBitMaskAllOnesName, true>::assert_eq(true, [] {
			auto ones	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto zero	  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			auto onesMask = jsonifier::internal::simd::opBitMask(ones);
			auto zeroMask = jsonifier::internal::simd::opBitMask(zero);
			return onesMask != zeroMask && onesMask == jsonifier::internal::simd::opBitMask(ones);
		});

		static constexpr rt_ut::string_literal opBitMaskSingleBitSweepName{ "op_bit_mask_single_bit_sweep_distinct" };
		rt_ut::unit_test<opBitMaskSingleBitSweepName, true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			std::memset(buffer, 0x00, activeWidth);
			auto baseline	  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
			auto baselineMask = jsonifier::internal::simd::opBitMask(baseline);
			std::vector<decltype(baselineMask)> results;
			for (size_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x00, activeWidth);
				buffer[targetPos] = 0xFF;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				results.push_back(jsonifier::internal::simd::opBitMask(loaded));
			}
			for (size_t i = 0; i < results.size(); ++i) {
				if (results[i] == baselineMask) {
					std::cout << "SINGLE-BIT-SWEEP position " << i << " matched all-zero baseline unexpectedly" << std::endl;
					return false;
				}
				for (size_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "SINGLE-BIT-SWEEP collision between position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal opBitMaskRawStructuralSweepName{ "op_bit_mask_raw_structural_sweep_distinct" };
		rt_ut::unit_test<opBitMaskRawStructuralSweepName, true>::assert_eq(true, [] {
			alignas(64) uint8_t buffer[activeWidth];
			auto needle = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x3A));
			std::memset(buffer, 0x41, activeWidth);
			auto baselineLoaded = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
			auto baselineRaw	= jsonifier::internal::simd::opCmpEqRaw(baselineLoaded, needle);
			auto baselinePacked = jsonifier::internal::simd::opBitMaskRaw(baselineRaw);
			std::vector<decltype(baselinePacked)> results;
			for (size_t targetPos = 0; targetPos < activeWidth; ++targetPos) {
				std::memset(buffer, 0x41, activeWidth);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::internal::simd::gatherValues<active_simd_t>(buffer);
				auto raw		  = jsonifier::internal::simd::opCmpEqRaw(loaded, needle);
				results.push_back(jsonifier::internal::simd::opBitMaskRaw(raw));
			}
			for (size_t i = 0; i < results.size(); ++i) {
				if (results[i] == baselinePacked) {
					std::cout << "OPBITMASKRAW position " << i << " matched no-match baseline unexpectedly" << std::endl;
					return false;
				}
				for (size_t j = i + 1; j < results.size(); ++j) {
					if (results[i] == results[j]) {
						std::cout << "OPBITMASKRAW collision between position " << i << " and " << j << std::endl;
						return false;
					}
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal opAndBasicName{ "op_and_basic" };
		rt_ut::unit_test<opAndBasicName, true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xF0));
			auto b	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x0F));
			auto res = jsonifier::internal::simd::opAnd(a, b);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opOrBasicName{ "op_or_basic" };
		rt_ut::unit_test<opOrBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xF0));
			auto b		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x0F));
			auto res	  = jsonifier::internal::simd::opOr(a, b);
			auto expected = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto match	  = jsonifier::internal::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::internal::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opXorSelfName{ "op_xor_self_is_zero" };
		rt_ut::unit_test<opXorSelfName, true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x5A));
			auto res = jsonifier::internal::simd::opXor(a, a);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opAndNotBasicName{ "op_and_not_basic" };
		rt_ut::unit_test<opAndNotBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto b		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x0F));
			auto res	  = jsonifier::internal::simd::opAndNot(a, b);
			auto expected = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xF0));
			auto match	  = jsonifier::internal::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::internal::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opNotBasicName{ "op_not_basic" };
		rt_ut::unit_test<opNotBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			auto res	  = jsonifier::internal::simd::opNot(a);
			auto expected = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0xFF));
			auto match	  = jsonifier::internal::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::internal::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opSubsSaturateName{ "op_subs_saturates_at_zero" };
		rt_ut::unit_test<opSubsSaturateName, true>::assert_eq(true, [] {
			auto a	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x05));
			auto b	 = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x10));
			auto res = jsonifier::internal::simd::opSubs(a, b);
			return static_cast<bool>(jsonifier::internal::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opSrLiShiftName{ "op_sr_li_shift" };
		rt_ut::unit_test<opSrLiShiftName, true>::assert_eq(true, [] {
			alignas(64) uint8_t src[activeWidth];
			for (size_t i = 0; i < activeWidth; ++i)
				src[i] = 0x80;
			auto a	 = jsonifier::internal::simd::gatherValues<active_simd_t>(src);
			auto res = jsonifier::internal::simd::opSrLi<4>(a);
			alignas(64) uint8_t out[activeWidth];
			jsonifier::internal::simd::store(res, out);
			for (size_t i = 0; i < activeWidth; ++i) {
				if (out[i] != 0x08) {
					std::cout << "OP_SR_LI byte " << i << " expected 0x08 got 0x" << std::hex << static_cast<uint32_t>(out[i]) << std::dec << std::endl;
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal opAlignRBoundaryName{ "op_align_r_boundary_shift" };
		rt_ut::unit_test<opAlignRBoundaryName, true>::assert_eq(true, [] {
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

		static constexpr rt_ut::string_literal isAsciiTrueName{ "is_ascii_true_for_ascii" };
		rt_ut::unit_test<isAsciiTrueName, true>::assert_eq(true, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x41));
			return jsonifier::internal::simd::isAscii(v);
		});

		static constexpr rt_ut::string_literal isAsciiFalseName{ "is_ascii_false_for_high_bit" };
		rt_ut::unit_test<isAsciiFalseName, true>::assert_eq(false, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x80));
			return jsonifier::internal::simd::isAscii(v);
		});

		static constexpr rt_ut::string_literal anyBitsSetTrueName{ "any_bits_set_true" };
		rt_ut::unit_test<anyBitsSetTrueName, true>::assert_eq(true, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x01));
			return jsonifier::internal::simd::anyBitsSetAnywhere(v);
		});

		static constexpr rt_ut::string_literal anyBitsSetFalseName{ "any_bits_set_false" };
		rt_ut::unit_test<anyBitsSetFalseName, true>::assert_eq(false, [] {
			auto v = jsonifier::internal::simd::gatherValue<active_simd_t>(static_cast<uint8_t>(0x00));
			return jsonifier::internal::simd::anyBitsSetAnywhere(v);
		});

		static constexpr rt_ut::string_literal storeRoundTripName{ "store_round_trip" };
		rt_ut::unit_test<storeRoundTripName, true>::assert_eq(true, [] {
			alignas(64) uint8_t src[activeWidth];
			alignas(64) uint8_t dst[activeWidth];
			for (size_t i = 0; i < activeWidth; ++i)
				src[i] = static_cast<uint8_t>(i * 7 + 1);
			auto loaded = jsonifier::internal::simd::gatherValues<active_simd_t>(src);
			jsonifier::internal::simd::store(loaded, dst);
			return std::memcmp(src, dst, activeWidth) == 0;
		});

		static constexpr rt_ut::string_literal storeURoundTripUnalignedName{ "store_u_round_trip_unaligned" };
		rt_ut::unit_test<storeURoundTripUnalignedName, true>::assert_eq(true, [] {
			alignas(64) uint8_t srcBuf[activeWidth * 2];
			alignas(64) uint8_t dstBuf[activeWidth * 2];
			for (size_t i = 0; i < activeWidth * 2; ++i)
				srcBuf[i] = static_cast<uint8_t>(i * 3 + 2);
			uint64_t failCount = 0;
			for (size_t offset = 0; offset < activeWidth; ++offset) {
				std::memset(dstBuf, 0x00, activeWidth * 2);
				auto loaded = jsonifier::internal::simd::gatherValuesU<active_simd_t>(srcBuf + offset);
				jsonifier::internal::simd::storeU(loaded, dstBuf + offset);
				if (std::memcmp(srcBuf + offset, dstBuf + offset, activeWidth) != 0)
					++failCount;
			}
			return failCount == 0;
		});

		std::cout << "SIMD Intrinsics validation tests complete." << std::endl;
	}

}
