/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/memcpy.hpp
 */
#pragma once

#include "common.hpp"

namespace memcpy_tests {

	inline constexpr uint64_t guardBytes{ 128 };
	inline constexpr uint64_t maxOffset{ 64 };
	inline constexpr uint8_t dstFill{ 0xCD };

	struct copy_fixture {
		std::vector<uint8_t> src;
		std::vector<uint8_t> dst;

		explicit copy_fixture(uint64_t length) : src(length + maxOffset + 2 * guardBytes), dst(length + maxOffset + 2 * guardBytes) {
			for (uint64_t i = 0; i < src.size(); ++i) {
				src[i] = static_cast<uint8_t>((i * 131u + 7u) ^ (i >> 8));
			}
		}

		void resetDst() {
			std::memset(dst.data(), dstFill, dst.size());
		}

		uint8_t* dstAt(uint64_t offset) {
			return dst.data() + guardBytes + offset;
		}

		const uint8_t* srcAt(uint64_t offset) const {
			return src.data() + guardBytes + offset;
		}

		bool verify(uint64_t length, uint64_t srcOffset, uint64_t dstOffset) const {
			const uint64_t dstBegin = guardBytes + dstOffset;
			const uint64_t dstEnd	= dstBegin + length;
			for (uint64_t i = 0; i < dst.size(); ++i) {
				const bool inside	   = i >= dstBegin && i < dstEnd;
				const uint8_t expected = inside ? src[guardBytes + srcOffset + (i - dstBegin)] : dstFill;
				if (dst[i] != expected) {
					std::cout << "MISMATCH length=" << length << " srcOffset=" << srcOffset << " dstOffset=" << dstOffset << " index=" << i
							  << (inside ? " (inside copy)" : " (guard clobbered)") << " expected=" << static_cast<uint32_t>(expected)
							  << " actual=" << static_cast<uint32_t>(dst[i]) << std::endl;
					return false;
				}
			}
			return true;
		}
	};

	template<typename copier_type> inline bool sweepLength(uint64_t length, uint64_t offsetLimit, copier_type&& copier) {
		copy_fixture fixture{ length };
		for (uint64_t srcOffset = 0; srcOffset < offsetLimit; ++srcOffset) {
			for (uint64_t dstOffset = 0; dstOffset < offsetLimit; ++dstOffset) {
				fixture.resetDst();
				copier(fixture.dstAt(dstOffset), fixture.srcAt(srcOffset), length);
				if (!fixture.verify(length, srcOffset, dstOffset)) {
					return false;
				}
			}
		}
		return true;
	}

	template<typename copier_type> inline bool sweepLengths(uint64_t lengthBegin, uint64_t lengthEnd, uint64_t offsetLimit, copier_type&& copier) {
		for (uint64_t length = lengthBegin; length <= lengthEnd; ++length) {
			if (!sweepLength(length, offsetLimit, copier)) {
				return false;
			}
		}
		return true;
	}

	inline void bucketBoundaryLengths(uint64_t limit, std::vector<uint64_t>& lengths) {
		for (uint64_t power = 1; power <= limit; power <<= 1) {
			lengths.push_back(power - 1);
			lengths.push_back(power);
			if (power + 1 <= limit) {
				lengths.push_back(power + 1);
			}
			if (power == (1ULL << 62)) {
				break;
			}
		}
	}

	struct memcpy_copier {
		void operator()(void* dst, const void* src, uint64_t length) const noexcept {
			jsonifier::internal::jsonifierMemcpy(dst, src, length);
		}
	};

	template<uint64_t max_bytes> struct memcpy_up_to_copier {
		void operator()(void* dst, const void* src, uint64_t length) const noexcept {
			jsonifier::internal::jsonifierMemcpyUpTo<max_bytes>(dst, src, length);
		}
	};

	template<uint64_t max_bytes> inline bool upToExhaustive() {
		return sweepLengths(0, max_bytes, maxOffset, memcpy_up_to_copier<max_bytes>{});
	}

	template<uint64_t... max_bytes> inline bool upToExhaustiveAll() {
		return (upToExhaustive<max_bytes>() && ...);
	}

	template<uint64_t max_bytes> inline bool upToMatchesMemcpy() {
		for (uint64_t length = 0; length <= max_bytes; ++length) {
			copy_fixture upToFixture{ length };
			copy_fixture memcpyFixture{ length };
			upToFixture.resetDst();
			memcpyFixture.resetDst();
			jsonifier::internal::jsonifierMemcpyUpTo<max_bytes>(upToFixture.dstAt(3), upToFixture.srcAt(5), length);
			jsonifier::internal::jsonifierMemcpy(memcpyFixture.dstAt(3), memcpyFixture.srcAt(5), length);
			if (upToFixture.dst != memcpyFixture.dst) {
				std::cout << "UP_TO/MEMCPY DIVERGENCE max_bytes=" << max_bytes << " length=" << length << std::endl;
				return false;
			}
		}
		return true;
	}

	template<uint64_t... max_bytes> inline bool upToMatchesMemcpyAll() {
		return (upToMatchesMemcpy<max_bytes>() && ...);
	}

	inline void runTests() {
		std::cout << "Memcpy Tests" << std::endl;

		static constexpr rt_ut::string_literal memcpyZeroLengthName{ "jsonifier_memcpy_zero_length_writes_nothing" };
		rt_ut::unit_test<memcpyZeroLengthName, true>::assert_eq(true, [] {
			return sweepLength(0, maxOffset, memcpy_copier{});
		});

		static constexpr rt_ut::string_literal memcpyZeroLengthNullName{ "jsonifier_memcpy_zero_length_null_pointers" };
		rt_ut::unit_test<memcpyZeroLengthNullName, true>::assert_eq(true, [] {
			jsonifier::internal::jsonifierMemcpy(nullptr, nullptr, 0);
			return true;
		});

		static constexpr rt_ut::string_literal memcpyUpToZeroLengthNullName{ "jsonifier_memcpy_up_to_zero_length_null_pointers" };
		rt_ut::unit_test<memcpyUpToZeroLengthNullName, true>::assert_eq(true, [] {
			jsonifier::internal::jsonifierMemcpyUpTo<0>(nullptr, nullptr, 0);
			jsonifier::internal::jsonifierMemcpyUpTo<64>(nullptr, nullptr, 0);
			jsonifier::internal::jsonifierMemcpyUpTo<1024>(nullptr, nullptr, 0);
			return true;
		});

		static constexpr rt_ut::string_literal memcpySingleByteAllValuesName{ "jsonifier_memcpy_single_byte_all_values" };
		rt_ut::unit_test<memcpySingleByteAllValuesName, true>::assert_eq(true, [] {
			for (uint32_t value = 0; value < 256; ++value) {
				uint8_t src[3]{ 0x11, static_cast<uint8_t>(value), 0x22 };
				uint8_t dst[3]{ dstFill, dstFill, dstFill };
				jsonifier::internal::jsonifierMemcpy(dst + 1, src + 1, 1);
				if (dst[0] != dstFill || dst[1] != static_cast<uint8_t>(value) || dst[2] != dstFill) {
					std::cout << "SINGLE BYTE FAILURE value=" << value << std::endl;
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal memcpySmallExhaustiveName{ "jsonifier_memcpy_lengths_0_to_256_all_offsets" };
		rt_ut::unit_test<memcpySmallExhaustiveName, true>::assert_eq(true, [] {
			return sweepLengths(0, 256, maxOffset, memcpy_copier{});
		});

		static constexpr rt_ut::string_literal memcpyMediumName{ "jsonifier_memcpy_lengths_257_to_1100_sparse_offsets" };
		rt_ut::unit_test<memcpyMediumName, true>::assert_eq(true, [] {
			return sweepLengths(257, 1100, 9, memcpy_copier{});
		});

		static constexpr rt_ut::string_literal memcpyBucketBoundariesName{ "jsonifier_memcpy_power_of_two_boundaries_to_1mib" };
		rt_ut::unit_test<memcpyBucketBoundariesName, true>::assert_eq(true, [] {
			std::vector<uint64_t> lengths;
			bucketBoundaryLengths(1ULL << 20, lengths);
			for (uint64_t length: lengths) {
				if (!sweepLength(length, length > 4096 ? 3 : maxOffset, memcpy_copier{})) {
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal memcpyLargeOddName{ "jsonifier_memcpy_large_odd_lengths" };
		rt_ut::unit_test<memcpyLargeOddName, true>::assert_eq(true, [] {
			for (uint64_t length: { 1999ULL, 4097ULL, 65537ULL, 100003ULL, 1048583ULL }) {
				if (!sweepLength(length, 3, memcpy_copier{})) {
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal memcpyAllZeroAndOnesName{ "jsonifier_memcpy_uniform_patterns" };
		rt_ut::unit_test<memcpyAllZeroAndOnesName, true>::assert_eq(true, [] {
			for (uint8_t pattern: { uint8_t{ 0x00 }, uint8_t{ 0xFF }, uint8_t{ 0x80 }, uint8_t{ 0x7F } }) {
				for (uint64_t length = 0; length <= 160; ++length) {
					std::vector<uint8_t> src(length + 2, pattern);
					std::vector<uint8_t> dst(length + 2, static_cast<uint8_t>(~pattern));
					jsonifier::internal::jsonifierMemcpy(dst.data() + 1, src.data() + 1, length);
					if (dst.front() != static_cast<uint8_t>(~pattern) || dst.back() != static_cast<uint8_t>(~pattern)) {
						std::cout << "PATTERN GUARD FAILURE pattern=" << static_cast<uint32_t>(pattern) << " length=" << length << std::endl;
						return false;
					}
					for (uint64_t i = 1; i <= length; ++i) {
						if (dst[i] != pattern) {
							std::cout << "PATTERN FAILURE pattern=" << static_cast<uint32_t>(pattern) << " length=" << length << " index=" << i << std::endl;
							return false;
						}
					}
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal memcpyRepeatedIntoSameBufferName{ "jsonifier_memcpy_sequential_appends" };
		rt_ut::unit_test<memcpyRepeatedIntoSameBufferName, true>::assert_eq(true, [] {
			std::string expected;
			std::vector<char> buffer(1 << 16, '\0');
			uint64_t cursor = 0;
			for (uint64_t length = 0; length <= 200; ++length) {
				std::string chunk(length, '\0');
				for (uint64_t i = 0; i < length; ++i) {
					chunk[i] = static_cast<char>('a' + ((i + length) % 26));
				}
				jsonifier::internal::jsonifierMemcpy(buffer.data() + cursor, chunk.data(), chunk.size());
				cursor += chunk.size();
				expected += chunk;
			}
			return std::string_view{ buffer.data(), cursor } == expected;
		});

		static constexpr rt_ut::string_literal upToZeroName{ "jsonifier_memcpy_up_to_max_0" };
		rt_ut::unit_test<upToZeroName, true>::assert_eq(true, [] {
			return upToExhaustive<0>();
		});

		static constexpr rt_ut::string_literal upToSmallMaxesName{ "jsonifier_memcpy_up_to_max_1_through_17_exhaustive" };
		rt_ut::unit_test<upToSmallMaxesName, true>::assert_eq(true, [] {
			return upToExhaustiveAll<1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17>();
		});

		static constexpr rt_ut::string_literal upToBoundaryMaxesName{ "jsonifier_memcpy_up_to_power_of_two_boundary_maxes_exhaustive" };
		rt_ut::unit_test<upToBoundaryMaxesName, true>::assert_eq(true, [] {
			return upToExhaustiveAll<31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256>();
		});

		static constexpr rt_ut::string_literal upToLargeMaxName{ "jsonifier_memcpy_up_to_large_max" };
		rt_ut::unit_test<upToLargeMaxName, true>::assert_eq(true, [] {
			return sweepLengths(0, 1100, 9, memcpy_up_to_copier<4096>{}) && sweepLength(4096, maxOffset, memcpy_up_to_copier<4096>{}) &&
				sweepLength(65537, 3, memcpy_up_to_copier<1ULL << 20>{});
		});

		static constexpr rt_ut::string_literal upToSimdBlockMaxesName{ "jsonifier_memcpy_up_to_simd_block_minus_one_maxes" };
		rt_ut::unit_test<upToSimdBlockMaxesName, true>::assert_eq(true, [] {
			return upToExhaustiveAll<63, 255, 511>();
		});

		static constexpr rt_ut::string_literal upToMatchesMemcpyName{ "jsonifier_memcpy_up_to_matches_jsonifier_memcpy" };
		rt_ut::unit_test<upToMatchesMemcpyName, true>::assert_eq(true, [] {
			return upToMatchesMemcpyAll<0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 512>();
		});

		static constexpr rt_ut::string_literal upToMatchesStdMemcpyName{ "jsonifier_memcpy_and_up_to_match_std_memcpy" };
		rt_ut::unit_test<upToMatchesStdMemcpyName, true>::assert_eq(true, [] {
			for (uint64_t length = 0; length <= 300; ++length) {
				copy_fixture fixture{ length };
				std::vector<uint8_t> reference(fixture.dst.size(), dstFill);
				std::memcpy(reference.data() + guardBytes + 1, fixture.srcAt(2), length);
				fixture.resetDst();
				jsonifier::internal::jsonifierMemcpy(fixture.dstAt(1), fixture.srcAt(2), length);
				if (fixture.dst != reference) {
					std::cout << "STD MEMCPY DIVERGENCE length=" << length << std::endl;
					return false;
				}
				fixture.resetDst();
				jsonifier::internal::jsonifierMemcpyUpTo<300>(fixture.dstAt(1), fixture.srcAt(2), length);
				if (fixture.dst != reference) {
					std::cout << "STD MEMCPY UP_TO DIVERGENCE length=" << length << std::endl;
					return false;
				}
			}
			return true;
		});
	}

}
