/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/RealTimeChris/Json-Performance
#pragma once

#include "apache-builds.hpp"
#include "canada.hpp"
#include "citm_catalog.hpp"
#include "discord.hpp"
#include "github_events.hpp"
#include "google_maps_response.hpp"
#include "instruments.hpp"
#include "marine_ik.hpp"
#include "mesh.hpp"
#include "random.hpp"
#include "semanticscholar.hpp"
#include "twitter.hpp"

namespace parsing_tests {

	enum class test_types {
		minify,
		prettify,
		validate,
	};

	template<test_types test_type, rt_ut::string_literal testNameNew, typename value_type> inline static void utilityTests(jsonifier::jsonifier_core<>& parser) {
		static constexpr rt_ut::string_literal testName{ testNameNew };
		std::string dataToParse = file_handle::get(basePath.operator std::string() + "/json/" + testName.operator std::string() + ".json");
		value_type jsonifier_value;
		if constexpr (test_type == test_types::minify) {
			parser.minifyJson(dataToParse, jsonifier_value);
		} else if constexpr (test_type == test_types::prettify) {
			parser.prettifyJson(dataToParse, jsonifier_value);
		} else if constexpr (test_type == test_types::validate) {
			parser.validateJson(dataToParse);
		}
		rt_ut::unit_test<testName, true>::run([&]() {
			if (auto& errors = parser.getErrors(); errors.size() > 0) {
				for (auto& error_value: errors) {
					std::cout << "Jsonifier Error: " << error_value << std::endl;
				}
				return false;
			} else {
				return true;
			}
		});
	};

	template<rt_ut::string_literal testNameNew, typename value_type, bool prettified, bool partial, bool knownOrder, bool nullTerminated>
	inline static void parsingTests(jsonifier::jsonifier_core<>& parser) {
		static constexpr rt_ut::string_literal testName{ testNameNew };
		static constexpr rt_ut::string_literal testNameRtUt{ testNameNew + ", " + testTypePartial<partial> + testTypeKnownOrder<knownOrder> +
			testTypeNullTerminated<nullTerminated> };
		std::string dataToParse = file_handle::get(basePath.operator std::string() + "/json/" + testName.operator std::string() + ".json");
		std::string serializedJson{};
		value_type jsonifier_value;
		parser.parseJson<
			jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder, .minified = !prettified, .validateUtf8 = true, .nullTerminated = nullTerminated }>(
			jsonifier_value, dataToParse);
		parser.template serializeJson<jsonifier::serialize_options{ .prettify = prettified }>(jsonifier_value, serializedJson);
		rt_ut::unit_test<testNameRtUt, true>::run([&]() {
			if (auto& errors = parser.getErrors(); errors.size() > 0) {
				for (auto& error_value: errors) {
					std::cout << "Jsonifier Error: " << error_value << std::endl;
				}
				return false;
			} else {
				return true;
			}
		});
	};

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void parsingTestsImpl([[maybe_unused]] jsonifier::jsonifier_core<>& parser) {
		std::cout << "Parsing Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		parsingTests<"Abc (In Order) Partial Test (Minified)", abc_in_order_partial_test, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (In Order) Partial Test (Prettified)", abc_in_order_partial_test, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (In Order) Test (Minified)", abc_in_order_test, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (In Order) Test (Prettified)", abc_in_order_test, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (Out of Order) Partial Test (Minified)", abc_out_of_order_partial_test, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (Out of Order) Partial Test (Prettified)", abc_out_of_order_partial_test, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (Out of Order) Test (Minified)", abc_out_of_order_test, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Abc (Out of Order) Test (Prettified)", abc_out_of_order_test, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Apache Builds Test (Minified)", apache_builds_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Apache Builds Test (Prettified)", apache_builds_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Canada Test (Minified)", canada_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Canada Test (Prettified)", canada_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"CitmCatalog Test (Minified)", citm_catalog_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"CitmCatalog Test (Prettified)", citm_catalog_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Discord Test (Minified)", discord_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Discord Test (Prettified)", discord_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Github Events Test (Minified)", github_events_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Github Events Test (Prettified)", github_events_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Google Maps Response Test (Minified)", google_maps_response_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Google Maps Response Test (Prettified)", google_maps_response_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Instruments Test (Minified)", instruments_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Instruments Test (Prettified)", instruments_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Marine IK Test (Minified)", marine_ik, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Marine IK Test (Prettified)", marine_ik, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Mesh Test (Minified)", mesh_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Mesh Test (Prettified)", mesh_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Random Test (Minified)", random_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Random Test (Prettified)", random_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Semanticscholar Corpus Test (Minified)", semantic_scholar_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Semanticscholar Corpus Test (Prettified)", semantic_scholar_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Twitter Partial Test (Minified)", twitter_partial_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Twitter Partial Test (Prettified)", twitter_partial_message, true, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Twitter Test (Minified)", twitter_message, false, partial, knownOrder, nullTerminated>(parser);
		parsingTests<"Twitter Test (Prettified)", twitter_message, true, partial, knownOrder, nullTerminated>(parser);
	}

	inline static void runTests() {
		jsonifier::jsonifier_core<> parser{};
		parsingTestsImpl<false, false, false>(parser);
		parsingTestsImpl<false, true, false>(parser);
		parsingTestsImpl<true, false, false>(parser);
		parsingTestsImpl<true, true, false>(parser);
		parsingTestsImpl<false, false, true>(parser);
		parsingTestsImpl<false, true, true>(parser);
		parsingTestsImpl<true, false, true>(parser);
		parsingTestsImpl<true, true, true>(parser);
		utilityTests<test_types::minify, "Minify Test", std::string>(parser);
		utilityTests<test_types::prettify, "Prettify Test", std::string>(parser);
		utilityTests<test_types::validate, "Validate Test", std::string>(parser);
	}

}
