/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/hash_map_tests.cpp
 */
#include <jsonifier/hash_map.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

template<typename map_type> struct key_set_check {
	static bool impl(const char* name, std::initializer_list<std::string_view> hits, std::initializer_list<std::string_view> misses) {
		bool passed{ true };
		uint64_t expected{};
		for (const std::string_view hit: hits) {
			const std::string copy{ hit };
			if (map_type::findIndex(copy) != expected) {
				std::printf("FAIL [%s]: hit '%s' did not resolve to %llu\n", name, copy.c_str(), static_cast<unsigned long long>(expected));
				passed = false;
			}
			++expected;
		}
		for (const std::string_view miss: misses) {
			const std::string copy{ miss };
			if (map_type::findIndex(copy) != map_type::keyCount) {
				std::printf("FAIL [%s]: miss '%s' resolved to a key\n", name, copy.c_str());
				passed = false;
			}
		}
		std::printf("%s %s\n", passed ? "OK  " : "FAIL", name);
		return passed;
	}
};

template<uint64_t minLength, uint64_t maxLength> struct comparison_check {
	static bool compare(const std::string& lhs, const std::string& rhs) {
		return jsonifier::internal::key_comparison<minLength, maxLength>::impl(lhs.data(), rhs.data(), lhs.size());
	}

	static bool impl() {
		bool passed{ true };
		for (uint64_t length = minLength; length <= maxLength; ++length) {
			std::string lhs(length, 'a');
			for (uint64_t x = 0; x < length; ++x) {
				lhs[x] = static_cast<char>('a' + x % 26);
			}
			std::string rhs{ lhs };
			if (!compare(lhs, rhs)) {
				std::printf("FAIL [compare]: equal keys of length %llu did not match\n", static_cast<unsigned long long>(length));
				passed = false;
			}
			for (uint64_t x = 0; x < length; ++x) {
				const char original{ rhs[x] };
				rhs[x] = static_cast<char>(original ^ 0x01);
				const bool lowBitMatched{ compare(lhs, rhs) };
				rhs[x] = static_cast<char>(original ^ 0x80);
				const bool highBitMatched{ compare(lhs, rhs) };
				rhs[x] = original;
				if (lowBitMatched || highBitMatched) {
					std::printf("FAIL [compare]: length %llu missed a difference at byte %llu\n", static_cast<unsigned long long>(length), static_cast<unsigned long long>(x));
					passed = false;
				}
			}
		}
		std::printf("%s compare %llu to %llu\n", passed ? "OK  " : "FAIL", static_cast<unsigned long long>(minLength), static_cast<unsigned long long>(maxLength));
		return passed;
	}
};

inline constexpr char hashText[]{ "the quick brown fox jumps over the lazy dog while the slow green turtle naps under an old oak tree beside a very quiet little stream" };

template<uint64_t... lengths> struct hash_agreement_check {
	static constexpr uint64_t seed{ 0x0123456789ABCDEFull };

	template<uint64_t length> static bool check() {
		static constexpr uint64_t expected{ jsonifier::internal::key_hash<0, jsonifier::internal::npos64>::impl(seed, hashText, length) };
		const std::string copy(hashText, length);
		const uint64_t wide{ jsonifier::internal::key_hash<0, jsonifier::internal::npos64>::impl(seed, copy.data(), copy.size()) };
		const uint64_t narrow{ jsonifier::internal::key_hash<length, length>::impl(seed, copy.data(), copy.size()) };
		if (wide != expected || narrow != expected) {
			std::printf("FAIL [hash]: compile-time and runtime hashes differ at length %llu\n", static_cast<unsigned long long>(length));
			return false;
		}
		return true;
	}

	static bool impl() {
		const bool passed = (check<lengths>() && ...);
		std::printf("%s hash agreement\n", passed ? "OK  " : "FAIL");
		return passed;
	}
};

#if JSONIFIER_MASKED_COMPARE
alignas(4096) inline char pageBuffer[8192]{};

template<uint64_t width> struct masked_compare_check {
	static bool compare(const char* data, uint64_t length, const char* stored) {
		return jsonifier::internal::masked_compare<width>::impl(data, length, stored);
	}

	static bool impl() {
		bool passed{ true };
		for (uint64_t length = 1; length <= width; ++length) {
			alignas(64) char stored[64]{};
			for (uint64_t x = 0; x < length; ++x) {
				stored[x] = static_cast<char>('a' + x % 26);
			}
			const uint64_t offsets[2]{ 0, 4096 - length };
			for (const uint64_t offset: offsets) {
				char* data{ pageBuffer + offset };
				std::memcpy(data, stored, length);
				if (!compare(data, length, stored)) {
					std::printf("FAIL [masked]: width %llu, length %llu at offset %llu did not match\n", static_cast<unsigned long long>(width), static_cast<unsigned long long>(length),
						static_cast<unsigned long long>(offset));
					passed = false;
				}
				for (uint64_t x = 0; x < length; ++x) {
					const char original{ data[x] };
					data[x] = static_cast<char>(original ^ 0x01);
					const bool lowBitMatched{ compare(data, length, stored) };
					data[x] = static_cast<char>(original ^ 0x80);
					const bool highBitMatched{ compare(data, length, stored) };
					data[x] = original;
					if (lowBitMatched || highBitMatched) {
						std::printf("FAIL [masked]: width %llu, length %llu missed a difference at byte %llu\n", static_cast<unsigned long long>(width),
							static_cast<unsigned long long>(length), static_cast<unsigned long long>(x));
						passed = false;
					}
				}
				if (length < width && compare(data, length - 1, stored)) {
					std::printf("FAIL [masked]: width %llu, a prefix of length %llu matched\n", static_cast<unsigned long long>(width), static_cast<unsigned long long>(length - 1));
					passed = false;
				}
			}
		}
		std::printf("%s masked compare width %llu\n", passed ? "OK  " : "FAIL", static_cast<unsigned long long>(width));
		return passed;
	}
};
#endif

int main() {
	bool passed{ true };
#if JSONIFIER_MASKED_COMPARE
	passed &= masked_compare_check<16>::impl();
	passed &= masked_compare_check<32>::impl();
	passed &= masked_compare_check<64>::impl();
#endif
	passed &= hash_agreement_check<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 48, 49, 64, 65, 100>::impl();
	passed &= key_set_check<jsonifier::hash_map<"a_long_shared_prefix_for_key_00", "a_long_shared_prefix_for_key_01", "a_long_shared_prefix_for_key_02", "a_long_shared_prefix_for_key_03",
		"a_long_shared_prefix_for_key_10", "a_long_shared_prefix_for_key_11", "a_long_shared_prefix_for_key_12", "a_long_shared_prefix_for_key_13">>::impl("long shared prefix",
		{ "a_long_shared_prefix_for_key_00", "a_long_shared_prefix_for_key_01", "a_long_shared_prefix_for_key_02", "a_long_shared_prefix_for_key_03", "a_long_shared_prefix_for_key_10",
			"a_long_shared_prefix_for_key_11", "a_long_shared_prefix_for_key_12", "a_long_shared_prefix_for_key_13" },
		{ "", "a", "a_long_shared_prefix_for_key_0", "a_long_shared_prefix_for_key_04", "a_long_shared_prefix_for_key_000", "b_long_shared_prefix_for_key_00" });
	passed &= key_set_check<jsonifier::hash_map<"BA", "BAC", "BK", "BKNG", "AMD", "AMGN", "AMT", "AMZN", "C", "CAT", "CL", "COF", "COP", "COST">>::impl("mixed short lengths",
		{ "BA", "BAC", "BK", "BKNG", "AMD", "AMGN", "AMT", "AMZN", "C", "CAT", "CL", "COF", "COP", "COST" }, { "", "B", "A", "BAD", "AMGM", "AMGNX", "COPS", "CLX", "Z" });
	passed &= key_set_check<jsonifier::hash_map<"a_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_one", "b_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_two">>::impl(
		"beyond flat table", { "a_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_one", "b_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_two" },
		{ "", "a", "a_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_onf", "c_key_that_is_longer_than_the_sixty_four_byte_flat_table_limit_number_two" });
	passed &= comparison_check<1, 1>::impl();
	passed &= comparison_check<5, 5>::impl();
	passed &= comparison_check<0, 5>::impl();
	passed &= comparison_check<1, 3>::impl();
	passed &= comparison_check<2, 6>::impl();
	passed &= comparison_check<4, 8>::impl();
	passed &= comparison_check<8, 16>::impl();
	passed &= comparison_check<3, 15>::impl();
	passed &= comparison_check<16, 32>::impl();
	passed &= comparison_check<24, 54>::impl();
	passed &= comparison_check<32, 64>::impl();
	passed &= comparison_check<33, 64>::impl();
	passed &= comparison_check<65, 200>::impl();
	passed &= comparison_check<1, 200>::impl();
	passed &= key_set_check<jsonifier::hash_map<>>::impl("empty", {}, { "", "anything" });
	passed &= key_set_check<jsonifier::hash_map<"only">>::impl("single", { "only" }, { "", "onl", "only1", "literally_anything" });
	passed &= key_set_check<jsonifier::hash_map<"id", "label">>::impl("double", { "id", "label" }, { "", "i", "ids", "labels", "idx" });
	passed &= key_set_check<jsonifier::hash_map<"alpha", "bravo", "charlie">>::impl("triple", { "alpha", "bravo", "charlie" }, { "", "a", "alphas", "delta" });
	passed &= key_set_check<jsonifier::hash_map<"alpha", "bravo", "charlie", "delta", "echo">>::impl("single byte", { "alpha", "bravo", "charlie", "delta", "echo" },
		{ "", "a", "alphabet", "foxtrot", "echoes" });
	passed &= key_set_check<jsonifier::hash_map<"get_x", "get_y", "set_x", "set_y">>::impl("first byte", { "get_x", "get_y", "set_x", "set_y" }, { "", "g", "get_z", "set_", "put_x" });
	passed &= key_set_check<jsonifier::hash_map<"http", "https", "ftp", "ws", "wss", "file">>::impl("byte and length", { "http", "https", "ftp", "ws", "wss", "file" },
		{ "", "h", "ssh", "telnet", "httpss", "fil" });
	passed &= key_set_check<jsonifier::hash_map<"aa", "ab", "ba", "bb", "aaa", "aab", "aba", "abb">>::impl("per length", { "aa", "ab", "ba", "bb", "aaa", "aab", "aba", "abb" },
		{ "", "a", "ac", "aac", "bbb", "aaaa" });
	passed &= key_set_check<jsonifier::hash_map<"00000", "00001", "00010", "00011", "00100", "00101", "00110", "00111", "01000", "01001", "01010", "01011", "01100", "01101", "01110",
		"01111", "10000", "10001", "10010", "10011">>::impl("simd",
		{ "00000", "00001", "00010", "00011", "00100", "00101", "00110", "00111", "01000", "01001", "01010", "01011", "01100", "01101", "01110", "01111", "10000", "10001", "10010",
			"10011" },
		{ "", "0", "0000", "11111", "000000", "01110000" });
	passed &= key_set_check<jsonifier::hash_map<"org.springframework.web.bind.annotation.RequestMapping", "org.springframework.beans.factory.annotation.Autowired",
		"com.google.common.util.concurrent.ListenableFuture", "com.fasterxml.jackson.annotation.JsonProperty">>::impl("long keys",
		{ "org.springframework.web.bind.annotation.RequestMapping", "org.springframework.beans.factory.annotation.Autowired", "com.google.common.util.concurrent.ListenableFuture",
			"com.fasterxml.jackson.annotation.JsonProperty" },
		{ "", "org.springframework.web.bind.annotation.RequestMappings", "org.springframework.web.bind.annotation.RequestMappinG", "com.google.common.util.concurrent.ListenableFutur" });
	return passed ? 0 : 1;
}
