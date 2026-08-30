// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/bounds_test.hpp
#pragma once

#include "common.hpp"
#include "parsing_tests.hpp"

#include <random>
#include <cstdint>

namespace bounds_tests {

	class random_die {
	  public:
		inline random_die() : seed{ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) } {
			seedState(seed);
			std::cout << "random_die seed: " << seed << std::endl;
		}

		inline explicit random_die(uint64_t seedNew) : seed{ seedNew } {
			seedState(seed);
			std::cout << "random_die seed: " << seed << std::endl;
		}

		inline uint64_t operator()() {
			return (next() % 8) + 1;
		}

		inline uint64_t getSeed() const {
			return seed;
		}

	  private:
		uint64_t seed;
		uint64_t s[4];

		inline static uint64_t rotl(uint64_t x, int32_t k) {
			return (x << k) | (x >> (64 - k));
		}

		inline uint64_t next() {
			uint64_t result = rotl(s[1] * 5, 7) * 9;
			uint64_t t		= s[1] << 17;
			s[2] ^= s[0];
			s[3] ^= s[1];
			s[1] ^= s[2];
			s[0] ^= s[3];
			s[2] ^= t;
			s[3] = rotl(s[3], 45);
			return result;
		}

		inline void seedState(uint64_t seedVal) {
			uint64_t z = seedVal;
			for (int32_t i = 0; i < 4; ++i) {
				z += 0x9e3779b97f4a7c15ULL;
				uint64_t x = z;
				x		   = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
				x		   = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
				x		   = x ^ (x >> 31);
				s[i]	   = x;
			}
		}
	};

	inline void sliceStringBySize(std::string& input, random_die& randomDie) {
		if (input.empty()) {
			return;
		}

		const size_t size = input.size();
		if (size >= 2000) {
			int64_t newSize = static_cast<int64_t>((input.size() * 9 / 10) - randomDie());
			if (newSize > 0) {
				input.resize(static_cast<uint64_t>(newSize));
			} else {
				input.pop_back();
			}
		} else {
			input.pop_back();
		}
	}

	template<rt_ut::string_literal testNameNew, typename test_data_type, bool prettified, bool partial, bool knownOrder, bool nullTerminated> inline static void boundsTestsImpl() {
		static constexpr rt_ut::string_literal testName{ testNameNew };
		static constexpr rt_ut::string_literal testNameRtUt{ testNameNew + ", " + testTypePartial<partial> + testTypeKnownOrder<knownOrder> +
			testTypeNullTerminated<nullTerminated> };
		auto dataToParse = file_handler::get(basePath.operator std::string() + "/json/" + testName.operator std::string() + ".json");
		jsonifier::jsonifier_core<> parser{};
		rt_ut::unit_test<testNameRtUt, true>::run(
			[&](std::string s) {
				test_data_type jsonifierValue;
				parser.parseJson<
					jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder, .minified = !prettified, .validateUtf8 = true, .nullTerminated = nullTerminated }>(
					jsonifierValue, s);
				if (parser.getErrors().size()) {
					return false;
				}
				random_die randomDie{};
				s.pop_back();
				test_data_type jsonifierValueLocal;
				while (!s.empty()) {
					parser.parseJson<jsonifier::
							parse_options{ .partialRead = partial, .knownOrder = knownOrder, .minified = !prettified, .validateUtf8 = true, .nullTerminated = nullTerminated }>(
						jsonifierValueLocal, s);
					if (!parser.getErrors().size()) {
						std::cout << "BOUNDS TEST FAILURE, test: " << testNameRtUt.operator std::string() << ", repro seed: " << randomDie.getSeed() << std::endl;
						return false;
					}
					sliceStringBySize(s, randomDie);
				}
				return true;
			},
			dataToParse);
	}

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void boundsTestsImpl() {
		std::cout << "Starting Bounds-Truncation Test, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": "
				  << std::endl;
		boundsTestsImpl<"Abc (In Order) Partial Test (Minified)", abc_in_order_partial_test, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (In Order) Partial Test (Prettified)", abc_in_order_partial_test, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (In Order) Test (Minified)", abc_in_order_test, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (In Order) Test (Prettified)", abc_in_order_test, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (Out of Order) Partial Test (Minified)", abc_out_of_order_partial_test, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (Out of Order) Partial Test (Prettified)", abc_out_of_order_partial_test, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (Out of Order) Test (Minified)", abc_out_of_order_test, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Abc (Out of Order) Test (Prettified)", abc_out_of_order_test, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Apache Builds Test (Minified)", apache_builds_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Apache Builds Test (Prettified)", apache_builds_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Canada Test (Minified)", canada_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Canada Test (Prettified)", canada_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"CitmCatalog Test (Minified)", citm_catalog_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"CitmCatalog Test (Prettified)", citm_catalog_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Discord Test (Minified)", discord_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Discord Test (Prettified)", discord_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Github Events Test (Minified)", github_events_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Github Events Test (Prettified)", github_events_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Google Maps Response Test (Minified)", google_maps_response_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Google Maps Response Test (Prettified)", google_maps_response_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Instruments Test (Minified)", instruments_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Instruments Test (Prettified)", instruments_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Marine IK Test (Minified)", marine_ik, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Marine IK Test (Prettified)", marine_ik, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Mesh Test (Minified)", mesh_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Mesh Test (Prettified)", mesh_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Semanticscholar Corpus Test (Minified)", semantic_scholar_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Semanticscholar Corpus Test (Prettified)", semantic_scholar_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Random Test (Minified)", random_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Random Test (Prettified)", random_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Twitter Partial Test (Minified)", twitter_partial_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Twitter Partial Test (Prettified)", twitter_partial_message, true, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Twitter Test (Minified)", twitter_message, false, partial, knownOrder, nullTerminated>();
		boundsTestsImpl<"Twitter Test (Prettified)", twitter_message, true, partial, knownOrder, nullTerminated>();
	}

	inline static void runTests() {
		boundsTestsImpl<false, false, false>();
		boundsTestsImpl<false, true, false>();
		boundsTestsImpl<true, false, false>();
		boundsTestsImpl<true, true, false>();
		boundsTestsImpl<false, false, true>();
		boundsTestsImpl<false, true, true>();
		boundsTestsImpl<true, false, true>();
		boundsTestsImpl<true, true, true>();
		std::cout << "Bounds truncation validation tests complete." << std::endl;
	}

}
