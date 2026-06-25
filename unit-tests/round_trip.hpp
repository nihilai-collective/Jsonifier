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
/// https://github.com/nihilai-collective/Jsonifier
#pragma once

#include "common.hpp"

#include <unordered_map>
#include <vector>
#include <string>

namespace round_trip_tests {

	struct Obj2 {
		std::string foo;
	};

	struct Obj3 {
		Obj3()						 = default;
		Obj3& operator=(const Obj3&) = delete;
		Obj3(const Obj3&)			 = delete;
		std::unique_ptr<int32_t> a;
		std::unique_ptr<std::string> foo;
	};
}

template<> struct jsonifier::core<round_trip_tests::Obj2> {
	using value_type				 = round_trip_tests::Obj2;
	static constexpr auto parseValue = createValue<&value_type::foo>();
};

template<> struct jsonifier::core<round_trip_tests::Obj3> {
	using value_type				 = round_trip_tests::Obj3;
	static constexpr auto parseValue = createValue<&value_type::a, &value_type::foo>();
};

namespace round_trip_tests {

	template<rt_ut::string_literal testNameNew, typename test_type, bool partial, bool knownOrder, bool nullTerminated>
	inline static void runRoundTripTest(const std::string& dataToParse, jsonifier::jsonifier_core<>& parser) {
		test_type valueNew{};
		std::string newString{};
		static constexpr rt_ut::string_literal testName{ testNameNew + ", " + testTypePartial<partial> + testTypeKnownOrder<knownOrder> + testTypeNullTerminated<nullTerminated> };
		parser.parseJson<jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder, .validateUtf8 = true, .nullTerminated = nullTerminated }>(valueNew,
			dataToParse);
		for (auto& value: parser.getErrors()) {
			std::cout << "Jsonifier Error: " << value << std::endl;
		}
		parser.serializeJson(valueNew, newString);
		for (auto& value: parser.getErrors()) {
			std::cout << "Jsonifier Error: " << value << std::endl;
		}
		rt_ut::unit_test<testName, true>::run([&]() {
			return parser.getErrors().size() == 0;
		});
		return;
	}

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void roundTripTestsImpl() {
		jsonifier::jsonifier_core<> parser{};
		std::unordered_map<std::string, test_base> jsonTests{};
		processFilesInFolder(jsonTests, "/round_trip");
		std::cout << "Round Trip Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << ": " << std::endl;
		runRoundTripTest<"roundtrip01.json", std::vector<int32_t*>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip01.json"].fileContents, parser);
		runRoundTripTest<"roundtrip02.json", std::vector<bool>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip02.json"].fileContents, parser);
		runRoundTripTest<"roundtrip03.json", std::vector<bool>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip03.json"].fileContents, parser);
		runRoundTripTest<"roundtrip04.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip04.json"].fileContents, parser);
		runRoundTripTest<"roundtrip05.json", std::vector<std::string>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip05.json"].fileContents, parser);
		runRoundTripTest<"roundtrip06.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip06.json"].fileContents, parser);
		runRoundTripTest<"roundtrip07.json", std::unordered_map<std::string, std::string>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip07.json"].fileContents, parser);
		runRoundTripTest<"roundtrip08.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip08.json"].fileContents, parser);
		runRoundTripTest<"roundtrip09.json", Obj2, partial, knownOrder, nullTerminated>(jsonTests["roundtrip09.json"].fileContents, parser);
		runRoundTripTest<"roundtrip10.json", Obj3, partial, knownOrder, nullTerminated>(jsonTests["roundtrip10.json"].fileContents, parser);
		runRoundTripTest<"roundtrip11.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip11.json"].fileContents, parser);
		runRoundTripTest<"roundtrip12.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip12.json"].fileContents, parser);
		runRoundTripTest<"roundtrip13.json", std::vector<int64_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip13.json"].fileContents, parser);
		runRoundTripTest<"roundtrip14.json", std::vector<int64_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip14.json"].fileContents, parser);
		runRoundTripTest<"roundtrip15.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip15.json"].fileContents, parser);
		runRoundTripTest<"roundtrip16.json", std::vector<int32_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip16.json"].fileContents, parser);
		runRoundTripTest<"roundtrip17.json", std::vector<int64_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip17.json"].fileContents, parser);
		runRoundTripTest<"roundtrip18.json", std::vector<int64_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip18.json"].fileContents, parser);
		runRoundTripTest<"roundtrip19.json", std::vector<int64_t>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip19.json"].fileContents, parser);
		runRoundTripTest<"roundtrip20.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip20.json"].fileContents, parser);
		runRoundTripTest<"roundtrip21.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip21.json"].fileContents, parser);
		runRoundTripTest<"roundtrip22.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip22.json"].fileContents, parser);
		runRoundTripTest<"roundtrip23.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip23.json"].fileContents, parser);
		runRoundTripTest<"roundtrip24.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip24.json"].fileContents, parser);
		runRoundTripTest<"roundtrip25.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip25.json"].fileContents, parser);
		runRoundTripTest<"roundtrip26.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip26.json"].fileContents, parser);
		runRoundTripTest<"roundtrip27.json", std::vector<double>, partial, knownOrder, nullTerminated>(jsonTests["roundtrip27.json"].fileContents, parser);
		return;
	}

	inline static void runTests() {
		roundTripTestsImpl<false, false, false>();
		roundTripTestsImpl<false, true, false>();
		roundTripTestsImpl<true, false, false>();
		roundTripTestsImpl<true, true, false>();
		roundTripTestsImpl<false, false, true>();
		roundTripTestsImpl<false, true, true>();
		roundTripTestsImpl<true, false, true>();
		roundTripTestsImpl<true, true, true>();
	}
}
