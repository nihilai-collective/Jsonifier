/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/hash.hpp
 */
#pragma once

#include "common.hpp"

namespace hash_validation_tests {

	template<uint64_t length> inline constexpr jsonifier::internal::array<char, length> makeBytes(uint64_t variant) {
		jsonifier::internal::array<char, length> buffer{};
		for (uint64_t i = 0; i < length; ++i) {
			buffer[i] = static_cast<char>((i * 41u + variant * 17u + 7u) & 0xFFu);
		}
		return buffer;
	}

	inline uint64_t referenceHash(uint64_t seed, const std::string& bytes) noexcept {
		uint64_t seed64{ seed };
		uint64_t i{ 0 };
		uint64_t remaining{ bytes.size() };

		while (remaining >= 8) {
			uint64_t chunk{};
			for (uint64_t b = 0; b < 8; ++b) {
				chunk |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i + b])) << (b * 8);
			}
			seed64 ^= chunk * 0x9E3779B185EBCA87ull;
			i += 8;
			remaining -= 8;
		}

		if (remaining >= 4) {
			uint32_t chunk32{};
			jsonifier::pow2MemcpyWrapper<4>(&chunk32, bytes.data() + i);
			seed64 ^= static_cast<uint64_t>(chunk32) * 0x9E3779B185EBCA87ull;
			i += 4;
			remaining -= 4;
		}

		if (remaining >= 2) {
			uint16_t chunk16{};
			jsonifier::pow2MemcpyWrapper<2>(&chunk16, bytes.data() + i);
			seed64 ^= static_cast<uint64_t>(chunk16) * 0x9E3779B185EBCA87ull;
			i += 2;
			remaining -= 2;
		}

		if (remaining > 0) {
			seed64 ^= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i])) * 0x9E3779B185EBCA87ull;
		}

		return seed64 ^ (seed64 >> 32);
	}

	template<uint64_t seed, uint64_t length> inline bool matchesReference() {
		static constexpr auto bytes		 = makeBytes<length + 1>(seed);
		static constexpr uint64_t ctHash = []() consteval {
			jsonifier::internal::ct_key_hasher ctHasher{};
			ctHasher.seed = seed;
			return ctHasher.hashKeyCt(bytes.data(), length);
		}();
		const std::string byteString{ bytes.data(), length };
		const uint64_t expected = referenceHash(seed, byteString);
		const uint64_t rtHash	= jsonifier::internal::rt_key_hasher<seed>::hashKeyRt(std::bit_cast<jsonifier::read_buffer_ptr>(byteString.data()), length);
		if (ctHash != expected) {
			std::cout << "HASH_CT MISMATCH for seed " << seed << ", length " << length << ": ct=" << ctHash << " expected=" << expected << std::endl;
			return false;
		}
		if (rtHash != expected) {
			std::cout << "HASH_RT MISMATCH for seed " << seed << ", length " << length << ": rt=" << rtHash << " expected=" << expected << std::endl;
			return false;
		}
		return true;
	}

	template<uint64_t seed, uint64_t... lengths> inline bool matchesReferenceAcrossLengths(std::integer_sequence<uint64_t, lengths...>) {
		return (true & ... & matchesReference<seed, lengths>());
	}

	template<uint64_t seed> inline bool matchesReferenceAcrossLengths() {
		return matchesReferenceAcrossLengths<seed>(std::make_integer_sequence<uint64_t, 41>{});
	}

	template<rt_ut::string_literal testName, uint64_t seed> inline void runSeedTest() {
		rt_ut::unit_test<testName, true>::assert_eq(true, [] {
			return matchesReferenceAcrossLengths<seed>();
		});
	}

	static_assert(jsonifier::internal::ct_key_hasher{}.hashKeyCt("compile-time-key", 16) == jsonifier::internal::ct_key_hasher{}.hashKeyCt("compile-time-key", 16),
		"ct_key_hasher::hashKeyCt must be evaluable in a constant expression");

	inline void runTests() {
		std::cout << "hash.hpp Tests" << std::endl;

		runSeedTest<"hash_matches_reference_seed_0", 0>();
		runSeedTest<"hash_matches_reference_seed_1", 1>();
		runSeedTest<"hash_matches_reference_seed_all_ones", 0xFFFFFFFFFFFFFFFFull>();
		runSeedTest<"hash_matches_reference_seed_prns_0", jsonifier::internal::prns[0]>();
		runSeedTest<"hash_matches_reference_seed_prns_67", jsonifier::internal::prns[67]>();
		runSeedTest<"hash_matches_reference_seed_prns_134", jsonifier::internal::prns[134]>();

		rt_ut::unit_test<"hash_zero_length_uses_seed_only", true>::assert_eq(true, [] {
			static constexpr jsonifier::internal::ct_key_hasher hasher{ 123456789ull };
			const uint64_t expected = hasher.seed ^ (hasher.seed >> 32);
			const uint64_t ctResult = hasher.hashKeyCt("", 0);
			const uint64_t rtResult = jsonifier::internal::rt_key_hasher<123456789ull>::hashKeyRt(std::bit_cast<jsonifier::read_buffer_ptr>(+""), 0);
			return ctResult == expected && rtResult == expected;
		});

		rt_ut::unit_test<"hash_different_seed_changes_output", true>::assert_eq(true, [] {
			static constexpr auto bytes		= makeBytes<20>(1);
			static constexpr uint64_t hashA = jsonifier::internal::ct_key_hasher{ 111ull }.hashKeyCt(bytes.data(), bytes.size());
			static constexpr uint64_t hashB = jsonifier::internal::ct_key_hasher{ 222ull }.hashKeyCt(bytes.data(), bytes.size());
			return hashA != hashB;
		});

		rt_ut::unit_test<"hash_updateSeed_cycles_through_prns_table", true>::assert_eq(true, [] {
			jsonifier::internal::ct_key_hasher hasher{};
			bool allPassed = hasher.seed == jsonifier::internal::prns[0] && hasher.index == 1;
			hasher.updateSeed();
			allPassed = allPassed && hasher.seed == jsonifier::internal::prns[1] && hasher.index == 2;
			return allPassed;
		});

		rt_ut::unit_test<"hash_updateSeed_wraps_around_prns_table", true>::assert_eq(true, [] {
			jsonifier::internal::ct_key_hasher hasher{};
			hasher.index = jsonifier::internal::prns.size();
			hasher.updateSeed();
			return hasher.seed == jsonifier::internal::prns[0];
		});

		std::cout << "hash.hpp validation tests complete." << std::endl;
	}

}
