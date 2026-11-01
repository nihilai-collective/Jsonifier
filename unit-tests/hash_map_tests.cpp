/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/hash_map_tests.cpp
 */
#include <jsonifier/hash_map.hpp>

#include <cstdio>
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

int main() {
	bool passed{ true };
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
