// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/hash.hpp
#pragma once

#include "common.hpp"

namespace hash_validation_tests {

	inline static std::string makeBytes(uint64_t length, uint64_t variant) {
		std::string buffer{};
		buffer.resize(length);
		for (uint64_t i = 0; i < length; ++i) {
			buffer[i] = static_cast<char>((i * 41u + variant * 17u + 7u) & 0xFFu);
		}
		return buffer;
	}

	inline static uint64_t referenceHash(uint64_t seed, const std::string& bytes) {
		uint64_t seed64{ seed };
		uint64_t i{ 0 };
		while (bytes.size() - i >= 8) {
			uint64_t chunk{};
			for (uint64_t b = 0; b < 8; ++b) {
				chunk |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i + b])) << (b * 8);
			}
			seed64 ^= chunk * 0x9E3779B185EBCA87ull;
			i += 8;
		}
		if (i < bytes.size()) {
			uint64_t tail{};
			const uint64_t remaining = bytes.size() - i;
			for (uint64_t b = 0; b < remaining; ++b) {
				tail |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[i + b])) << (b * 8);
			}
			seed64 ^= tail * 0x9E3779B185EBCA87ull;
		}
		return seed64 ^ (seed64 >> 32);
	}

	template<uint64_t seed> inline static bool matchesReference(const std::string& bytes) {
		jsonifier::internal::ct_key_hasher ctHasher{};
		ctHasher.seed			  = seed;
		const uint64_t expected	  = referenceHash(seed, bytes);
		const uint64_t ctHash	  = ctHasher.hashKeyCt(bytes.data(), bytes.size());
		const uint64_t rtHash	  = jsonifier::internal::rt_key_hasher<seed>::hashKeyRt(bytes.data(), bytes.size());
		if (ctHash != expected) {
			std::cout << "HASH_CT MISMATCH for seed " << seed << ", length " << bytes.size() << ": ct=" << ctHash << " expected=" << expected << std::endl;
			return false;
		}
		if (rtHash != expected) {
			std::cout << "HASH_RT MISMATCH for seed " << seed << ", length " << bytes.size() << ": rt=" << rtHash << " expected=" << expected << std::endl;
			return false;
		}
		return true;
	}

	template<uint64_t seed> inline static bool matchesReferenceAcrossLengths() {
		bool allPassed{ true };
		for (uint64_t length = 0; length <= 40; ++length) {
			allPassed = matchesReference<seed>(makeBytes(length, seed)) && allPassed;
		}
		return allPassed;
	}

	template<rt_ut::string_literal testName, uint64_t seed> inline static void runSeedTest() {
		rt_ut::unit_test<testName, true>::assert_eq(true, [] {
			return matchesReferenceAcrossLengths<seed>();
		});
	}

	static_assert(jsonifier::internal::ct_key_hasher{}.hashKeyCt("compile-time-key", 16) == jsonifier::internal::ct_key_hasher{}.hashKeyCt("compile-time-key", 16),
		"ct_key_hasher::hashKeyCt must be evaluable in a constant expression");

	inline static void runTests() {
		std::cout << "hash.hpp Tests" << std::endl;

		runSeedTest<"hash_matches_reference_seed_0", 0>();
		runSeedTest<"hash_matches_reference_seed_1", 1>();
		runSeedTest<"hash_matches_reference_seed_all_ones", 0xFFFFFFFFFFFFFFFFull>();
		runSeedTest<"hash_matches_reference_seed_prns_0", jsonifier::internal::prns[0]>();
		runSeedTest<"hash_matches_reference_seed_prns_67", jsonifier::internal::prns[67]>();
		runSeedTest<"hash_matches_reference_seed_prns_134", jsonifier::internal::prns[134]>();

		rt_ut::unit_test<"hash_zero_length_uses_seed_only", true>::assert_eq(true, [] {
			jsonifier::internal::ct_key_hasher hasher{};
			hasher.seed				= 123456789ull;
			const uint64_t expected		= hasher.seed ^ (hasher.seed >> 32);
			const uint64_t ctResult		= hasher.hashKeyCt("", 0);
			const uint64_t rtResult		= jsonifier::internal::rt_key_hasher<123456789ull>::hashKeyRt("", 0);
			return ctResult == expected && rtResult == expected;
		});

		rt_ut::unit_test<"hash_different_seed_changes_output", true>::assert_eq(true, [] {
			const std::string bytes = makeBytes(20, 1);
			jsonifier::internal::ct_key_hasher hasherA{};
			hasherA.seed = 111ull;
			jsonifier::internal::ct_key_hasher hasherB{};
			hasherB.seed = 222ull;
			return hasherA.hashKeyCt(bytes.data(), bytes.size()) != hasherB.hashKeyCt(bytes.data(), bytes.size());
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
