// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/intrinsics.hpp
#pragma once

#include "common.hpp"

namespace intrinsics_tests {

	inline static void runTests() {
		std::cout << "SIMD Intrinsics Layer Tests" << std::endl;

		static constexpr rt_ut::string_literal opCmpEqAllMatchName{ "op_cmp_eq_all_match" };
		rt_ut::unit_test<opCmpEqAllMatchName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x41));
			auto b		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x41));
			auto allMatch = jsonifier::simd::opCmpEq(a, b);
			auto noMatch  = jsonifier::simd::opCmpEq(a, jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x3A)));
			return allMatch != noMatch && allMatch == jsonifier::simd::opCmpEq(a, b);
		});

		static constexpr rt_ut::string_literal opCmpEqNoMatchName{ "op_cmp_eq_no_match" };
		rt_ut::unit_test<opCmpEqNoMatchName, true>::assert_eq(true, [] {
			auto a		   = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x41));
			auto b		   = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x3A));
			auto noMatch   = jsonifier::simd::opCmpEq(a, b);
			auto selfMatch = jsonifier::simd::opCmpEq(a, a);
			return noMatch != selfMatch;
		});

		static constexpr rt_ut::string_literal opCmpEqSingleBytePositionsName{ "op_cmp_eq_single_byte_positions_distinct" };
		rt_ut::unit_test<opCmpEqSingleBytePositionsName, true>::assert_eq(true, [] {
			alignas(16) uint8_t buffer[16];
			auto needle = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x3A));
			std::vector<decltype(jsonifier::simd::opCmpEq(jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer), needle))> results;
			for (size_t targetPos = 0; targetPos < 16; ++targetPos) {
				std::memset(buffer, 0x41, 16);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer);
				results.push_back(jsonifier::simd::opCmpEq(loaded, needle));
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
			auto lo		 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x20));
			auto hi		 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x21));
			auto ltTrue	 = jsonifier::simd::opCmpLt(lo, hi);
			auto ltFalse = jsonifier::simd::opCmpLt(hi, lo);
			auto ltSelf	 = jsonifier::simd::opCmpLt(lo, lo);
			return ltTrue != ltFalse && ltTrue == jsonifier::simd::opCmpLt(lo, hi) && ltFalse == ltSelf;
		});

		static constexpr rt_ut::string_literal opBitMaskAllZeroName{ "op_bit_mask_all_zero" };
		rt_ut::unit_test<opBitMaskAllZeroName, true>::assert_eq(true, [] {
			auto zero	  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x00));
			auto ones	  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xFF));
			auto zeroMask = jsonifier::simd::opBitMask(zero);
			auto onesMask = jsonifier::simd::opBitMask(ones);
			return zeroMask != onesMask && zeroMask == jsonifier::simd::opBitMask(zero);
		});

		static constexpr rt_ut::string_literal opBitMaskAllOnesName{ "op_bit_mask_all_ones" };
		rt_ut::unit_test<opBitMaskAllOnesName, true>::assert_eq(true, [] {
			auto ones	  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xFF));
			auto zero	  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x00));
			auto onesMask = jsonifier::simd::opBitMask(ones);
			auto zeroMask = jsonifier::simd::opBitMask(zero);
			return onesMask != zeroMask && onesMask == jsonifier::simd::opBitMask(ones);
		});

		static constexpr rt_ut::string_literal opBitMaskSingleBitSweepName{ "op_bit_mask_single_bit_sweep_distinct" };
		rt_ut::unit_test<opBitMaskSingleBitSweepName, true>::assert_eq(true, [] {
			alignas(16) uint8_t buffer[16];
			std::memset(buffer, 0x00, 16);
			auto baseline	  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer);
			auto baselineMask = jsonifier::simd::opBitMask(baseline);
			std::vector<decltype(baselineMask)> results;
			for (size_t targetPos = 0; targetPos < 16; ++targetPos) {
				std::memset(buffer, 0x00, 16);
				buffer[targetPos] = 0xFF;
				auto loaded		  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer);
				results.push_back(jsonifier::simd::opBitMask(loaded));
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
			alignas(16) uint8_t buffer[16];
			auto needle = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x3A));
			std::memset(buffer, 0x41, 16);
			auto baselineLoaded = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer);
			auto baselineRaw	= jsonifier::simd::opCmpEqRaw(baselineLoaded, needle);
			auto baselinePacked = jsonifier::simd::opBitMaskRaw(baselineRaw);
			std::vector<decltype(baselinePacked)> results;
			for (size_t targetPos = 0; targetPos < 16; ++targetPos) {
				std::memset(buffer, 0x41, 16);
				buffer[targetPos] = 0x3A;
				auto loaded		  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(buffer);
				auto raw		  = jsonifier::simd::opCmpEqRaw(loaded, needle);
				results.push_back(jsonifier::simd::opBitMaskRaw(raw));
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
			auto a	 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xF0));
			auto b	 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x0F));
			auto res = jsonifier::simd::opAnd(a, b);
			return static_cast<bool>(jsonifier::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opOrBasicName{ "op_or_basic" };
		rt_ut::unit_test<opOrBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xF0));
			auto b		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x0F));
			auto res	  = jsonifier::simd::opOr(a, b);
			auto expected = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xFF));
			auto match	  = jsonifier::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opXorSelfName{ "op_xor_self_is_zero" };
		rt_ut::unit_test<opXorSelfName, true>::assert_eq(true, [] {
			auto a	 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x5A));
			auto res = jsonifier::simd::opXor(a, a);
			return static_cast<bool>(jsonifier::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opAndNotBasicName{ "op_and_not_basic" };
		rt_ut::unit_test<opAndNotBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xFF));
			auto b		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x0F));
			auto res	  = jsonifier::simd::opAndNot(a, b);
			auto expected = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xF0));
			auto match	  = jsonifier::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opNotBasicName{ "op_not_basic" };
		rt_ut::unit_test<opNotBasicName, true>::assert_eq(true, [] {
			auto a		  = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x00));
			auto res	  = jsonifier::simd::opNot(a);
			auto expected = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0xFF));
			auto match	  = jsonifier::simd::opCmpEq(res, expected);
			auto mismatch = jsonifier::simd::opCmpEq(res, a);
			return match != mismatch;
		});

		static constexpr rt_ut::string_literal opSubsSaturateName{ "op_subs_saturates_at_zero" };
		rt_ut::unit_test<opSubsSaturateName, true>::assert_eq(true, [] {
			auto a	 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x05));
			auto b	 = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x10));
			auto res = jsonifier::simd::opSubs(a, b);
			return static_cast<bool>(jsonifier::simd::opTest(res));
		});

		static constexpr rt_ut::string_literal opSrLiShiftName{ "op_sr_li_shift" };
		rt_ut::unit_test<opSrLiShiftName, true>::assert_eq(true, [] {
			alignas(16) uint8_t src[16];
			for (size_t i = 0; i < 16; ++i)
				src[i] = 0x80;
			auto a	 = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(src);
			auto res = jsonifier::simd::opSrLi<4>(a);
			alignas(16) uint8_t out[16];
			jsonifier::simd::store(res, out);
			for (size_t i = 0; i < 16; ++i) {
				if (out[i] != 0x08) {
					std::cout << "OP_SR_LI byte " << i << " expected 0x08 got 0x" << std::hex << static_cast<uint32_t>(out[i]) << std::dec << std::endl;
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal opAlignRBoundaryName{ "op_align_r_boundary_shift" };
		rt_ut::unit_test<opAlignRBoundaryName, true>::assert_eq(true, [] {
			alignas(16) uint8_t currentBuf[16];
			alignas(16) uint8_t previousBuf[16];
			std::memset(currentBuf, 0x41, 16);
			std::memset(previousBuf, 0x42, 16);
			auto current  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(currentBuf);
			auto previous = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(previousBuf);
			auto shifted  = jsonifier::simd::opPrev<15>(current, previous);
			alignas(16) uint8_t shiftedOut[16];
			jsonifier::simd::store(shifted, shiftedOut);
			return shiftedOut[0] == 0x42 && shiftedOut[1] == 0x41;
		});

		static constexpr rt_ut::string_literal isAsciiTrueName{ "is_ascii_true_for_ascii" };
		rt_ut::unit_test<isAsciiTrueName, true>::assert_eq(true, [] {
			auto v = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x41));
			return jsonifier::simd::isAscii(v);
		});

		static constexpr rt_ut::string_literal isAsciiFalseName{ "is_ascii_false_for_high_bit" };
		rt_ut::unit_test<isAsciiFalseName, true>::assert_eq(false, [] {
			auto v = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x80));
			return jsonifier::simd::isAscii(v);
		});

		static constexpr rt_ut::string_literal anyBitsSetTrueName{ "any_bits_set_true" };
		rt_ut::unit_test<anyBitsSetTrueName, true>::assert_eq(true, [] {
			auto v = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x01));
			return jsonifier::simd::anyBitsSetAnywhere(v);
		});

		static constexpr rt_ut::string_literal anyBitsSetFalseName{ "any_bits_set_false" };
		rt_ut::unit_test<anyBitsSetFalseName, true>::assert_eq(false, [] {
			auto v = jsonifier::simd::gatherValue<jsonifier::jsonifier_simd_int_128>(static_cast<uint8_t>(0x00));
			return jsonifier::simd::anyBitsSetAnywhere(v);
		});

		static constexpr rt_ut::string_literal storeRoundTripName{ "store_round_trip" };
		rt_ut::unit_test<storeRoundTripName, true>::assert_eq(true, [] {
			alignas(16) uint8_t src[16];
			alignas(16) uint8_t dst[16];
			for (size_t i = 0; i < 16; ++i)
				src[i] = static_cast<uint8_t>(i * 7 + 1);
			auto loaded = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(src);
			jsonifier::simd::store(loaded, dst);
			return std::memcmp(src, dst, 16) == 0;
		});

		static constexpr rt_ut::string_literal storeURoundTripUnalignedName{ "store_u_round_trip_unaligned" };
		rt_ut::unit_test<storeURoundTripUnalignedName, true>::assert_eq(true, [] {
			alignas(16) uint8_t srcBuf[32];
			alignas(16) uint8_t dstBuf[32];
			for (size_t i = 0; i < 32; ++i)
				srcBuf[i] = static_cast<uint8_t>(i * 3 + 2);
			uint64_t failCount = 0;
			for (size_t offset = 0; offset < 16; ++offset) {
				std::memset(dstBuf, 0x00, 32);
				auto loaded = jsonifier::simd::gatherValuesU<jsonifier::jsonifier_simd_int_128>(srcBuf + offset);
				jsonifier::simd::storeU(loaded, dstBuf + offset);
				if (std::memcmp(srcBuf + offset, dstBuf + offset, 16) != 0)
					++failCount;
			}
			return failCount == 0;
		});

		std::cout << "SIMD Intrinsics validation tests complete." << std::endl;
	}

}
