// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/string_literal_comparator.hpp
#pragma once

#include "common.hpp"

namespace string_literal_comparator_impl_tests {

	static constexpr char alphabetChars[]{ "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" };

	template<uint64_t count> static constexpr jsonifier::internal::string_literal<count + 1> alphabetSourceGen() {
		jsonifier::internal::string_literal<count + 1> lit{};
		for (uint64_t i = 0; i < count; ++i) {
			lit.values[i] = alphabetChars[i % (sizeof(alphabetChars) - 1)];
		}
		lit.values[count] = '\0';
		return lit;
	}

	template<uint64_t count> static constexpr auto alphabetSource{ alphabetSourceGen<count>() };

	template<uint64_t count> static constexpr jsonifier::internal::string_literal<count + 1> binarySourceGen() {
		jsonifier::internal::string_literal<count + 1> lit{};
		for (uint64_t i = 0; i < count; ++i) {
			lit.values[i] = static_cast<char>((i * 37u + 11u) & 0xFFu);
		}
		lit.values[count] = '\0';
		return lit;
	}

	template<uint64_t count> static constexpr auto binarySource{ binarySourceGen<count>() };

	template<jsonifier::internal::string_literal literal> inline static bool matchesExactly() {
		static constexpr auto localLiteral = literal;
		using sl_type						= decltype(localLiteral);
		std::string buffer{ localLiteral.data(), localLiteral.size() };
		const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buffer.data());
		return result == buffer.data() + localLiteral.size();
	}

	template<jsonifier::internal::string_literal literal> inline static bool detectsCorruptionAtEveryPosition() {
		static constexpr auto localLiteral = literal;
		using sl_type						= decltype(localLiteral);
		static constexpr uint64_t len		= localLiteral.size();
		for (uint64_t pos = 0; pos < len; ++pos) {
			std::string buffer{ localLiteral.data(), localLiteral.size() };
			buffer[pos]			= static_cast<char>(~buffer[pos]);
			const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buffer.data());
			if (result != nullptr) {
				std::cout << "STRING_LITERAL_COMPARATOR_IMPL failed to detect corruption at position " << pos << " for length " << len << std::endl;
				return false;
			}
		}
		return true;
	}
	
	template<jsonifier::internal::string_literal literal> inline static bool ignoresTrailingGarbage() {
		static constexpr auto localLiteral = literal;
		using sl_type					   = decltype(localLiteral);
		static constexpr uint64_t len	   = localLiteral.size();
		for (uint64_t pattern = 0; pattern < 4; ++pattern) {
			std::string buffer{ localLiteral.data(), len };
			buffer.append(64, static_cast<char>(pattern == 0 ? 0x00 : pattern == 1 ? 0xFF : pattern == 2 ? 0x41 : 0x7F));
			const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buffer.data());
			if (result != buffer.data() + len) {
				std::cout << "STRING_LITERAL_COMPARATOR_IMPL trailing-garbage failure, pattern " << pattern << " for length " << len << std::endl;
				return false;
			}
		}
		return true;
	}

	template<jsonifier::internal::string_literal literal> inline static bool detectsTranspositionCorruption() {
		static constexpr auto localLiteral = literal;
		using sl_type					   = decltype(localLiteral);
		static constexpr uint64_t len	   = localLiteral.size();
		if constexpr (len < 2) {
			return true;
		} else {
			for (uint64_t pos = 0; pos + 1 < len; ++pos) {
				std::string buffer{ localLiteral.data(), len };
				if (buffer[pos] == buffer[pos + 1]) {
					continue;
				}
				std::swap(buffer[pos], buffer[pos + 1]);
				const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buffer.data());
				if (result != nullptr) {
					std::cout << "STRING_LITERAL_COMPARATOR_IMPL failed to detect transposition at position " << pos << " for length " << len << std::endl;
					return false;
				}
			}
			return true;
		}
	}

	template<jsonifier::internal::string_literal literal> inline static bool detectsDoubleCorruption() {
		static constexpr auto localLiteral = literal;
		using sl_type					   = decltype(localLiteral);
		static constexpr uint64_t len	   = localLiteral.size();
		if constexpr (len < 2) {
			return true;
		} else {
			for (uint64_t first = 0; first < len; ++first) {
				for (uint64_t second = first + 1; second < len; ++second) {
					std::string buffer{ localLiteral.data(), len };
					buffer[first]	   = static_cast<char>(~buffer[first]);
					buffer[second]	   = static_cast<char>(~buffer[second]);
					const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buffer.data());
					if (result != nullptr) {
						std::cout << "STRING_LITERAL_COMPARATOR_IMPL failed to detect double corruption at " << first << "," << second << " for length " << len << std::endl;
						return false;
					}
				}
			}
			return true;
		}
	}

	struct guarded_buffer {
		char* ptr{};
		uint64_t len{};
		void* region{};
		uint64_t regionSize{};

		guarded_buffer(const char* src, uint64_t lengthNew) : len{ lengthNew } {
			const uint64_t pageSize{ 4096 };
			regionSize = pageSize * 2;
#if defined(_WIN32)
			region = VirtualAlloc(nullptr, regionSize, MEM_RESERVE, PAGE_NOACCESS);
			VirtualAlloc(region, pageSize, MEM_COMMIT, PAGE_READWRITE);
#else
			region = mmap(nullptr, regionSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			mprotect(region, pageSize, PROT_READ | PROT_WRITE);
#endif
			ptr = static_cast<char*>(region) + pageSize - len;
			std::memcpy(ptr, src, len);
		}

		~guarded_buffer() {
#if defined(_WIN32)
			VirtualFree(region, 0, MEM_RELEASE);
#else
			munmap(region, regionSize);
#endif
			std::cout << "guarded_buffer released" << std::endl;
		}
	};

	template<jsonifier::internal::string_literal literal> inline static bool matchesAtPageBoundary() {
		static constexpr auto localLiteral = literal;
		using sl_type					   = decltype(localLiteral);
		static constexpr uint64_t len	   = localLiteral.size();
		if constexpr (len == 0) {
			return true;
		} else {
			guarded_buffer buf{ localLiteral.data(), len };
			const char* result = jsonifier::internal::string_literal_comparator_impl<sl_type, localLiteral>::impl(buf.ptr);
			return result == buf.ptr + len;
		}
	}

	template<rt_ut::string_literal testName, jsonifier::internal::string_literal literal> inline static void runLengthTest() {
		rt_ut::unit_test<testName, true>::assert_eq(true, [] {
			return matchesExactly<literal>() && detectsCorruptionAtEveryPosition<literal>() && ignoresTrailingGarbage<literal>() && detectsTranspositionCorruption<literal>() &&
				detectsDoubleCorruption<literal>();
		});
	}

	template<size_t... indices> inline static void runExhaustiveLengths(std::index_sequence<indices...>) {
		(( void )(rt_ut::unit_test<"slc_exhaustive", true>::assert_eq(true,
			 [] {
				 return matchesExactly<alphabetSource<indices + 1>>() && detectsCorruptionAtEveryPosition<alphabetSource<indices + 1>>() &&
					 ignoresTrailingGarbage<alphabetSource<indices + 1>>();
			 })),
			...);
	}

	inline static void runTests() {
		std::cout << "string_literal_comparator_impl Tests" << std::endl;
		runLengthTest<"slc_length_0_empty", alphabetSource<0>>();
		runLengthTest<"slc_length_1", alphabetSource<1>>();
		runLengthTest<"slc_length_2", alphabetSource<2>>();
		runLengthTest<"slc_length_3", alphabetSource<3>>();
		runLengthTest<"slc_length_4", alphabetSource<4>>();
		runLengthTest<"slc_length_5", alphabetSource<5>>();
		runLengthTest<"slc_length_6", alphabetSource<6>>();
		runLengthTest<"slc_length_7", alphabetSource<7>>();
		runLengthTest<"slc_length_8", alphabetSource<8>>();
		runLengthTest<"slc_length_9", alphabetSource<9>>();
		runLengthTest<"slc_length_10", alphabetSource<10>>();
		runLengthTest<"slc_length_15", alphabetSource<15>>();
		runLengthTest<"slc_length_16", alphabetSource<16>>();
		runLengthTest<"slc_length_17_first_split", alphabetSource<17>>();
		runLengthTest<"slc_length_20", alphabetSource<20>>();
		runLengthTest<"slc_length_31", alphabetSource<31>>();
		runLengthTest<"slc_length_32", alphabetSource<32>>();
		runLengthTest<"slc_length_33", alphabetSource<33>>();
		runLengthTest<"slc_length_45", alphabetSource<45>>();
		runLengthTest<"slc_length_63", alphabetSource<63>>();
		runLengthTest<"slc_length_64", alphabetSource<64>>();
		runLengthTest<"slc_length_65", alphabetSource<65>>();
		runLengthTest<"slc_length_100_multi_split", alphabetSource<100>>();
		runLengthTest<"slc_length_130_multi_split", alphabetSource<130>>();
		runLengthTest<"slc_length_200_multi_split", alphabetSource<200>>();
		runLengthTest<"slc_binary_content_length_16", binarySource<16>>();
		runLengthTest<"slc_binary_content_length_40", binarySource<40>>();
		rt_ut::unit_test<"slc_page_boundary_15", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<15>>();
		});
		rt_ut::unit_test<"slc_page_boundary_16", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<16>>();
		});
		rt_ut::unit_test<"slc_page_boundary_17", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<17>>();
		});
		rt_ut::unit_test<"slc_page_boundary_31", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<31>>();
		});
		rt_ut::unit_test<"slc_page_boundary_32", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<32>>();
		});
		rt_ut::unit_test<"slc_page_boundary_33", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<33>>();
		});
		rt_ut::unit_test<"slc_page_boundary_63", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<63>>();
		});
		rt_ut::unit_test<"slc_page_boundary_64", true>::assert_eq(true, [] {
			return matchesAtPageBoundary<alphabetSource<64>>();
		});
		runExhaustiveLengths(std::make_index_sequence<80>{});
		rt_ut::unit_test<"slc_driver_rejects_short_buffer", true>::assert_eq(true, [] {
			static constexpr jsonifier::internal::string_literal needle{ "HelloWorld" };
			std::string buffer{ "Hello" };
			return !jsonifier::internal::string_literal_comparator<needle>::impl(buffer.data(), buffer.size());
		});
		rt_ut::unit_test<"slc_driver_accepts_exact_match", true>::assert_eq(true, [] {
			static constexpr jsonifier::internal::string_literal needle{ "HelloWorld" };
			std::string buffer{ "HelloWorld" };
			return jsonifier::internal::string_literal_comparator<needle>::impl(buffer.data(), buffer.size());
		});
		std::cout << "string_literal_comparator_impl validation tests complete." << std::endl;
	}

}
