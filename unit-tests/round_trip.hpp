/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/round_trip.hpp
 */
#pragma once

#include "common.hpp"
#include "conformance.hpp"

#include <unordered_map>
#include <vector>
#include <string>

namespace round_trip_tests {

	struct Obj2 {
		std::string foo;
	};

	struct Obj3 {
		Obj3& operator=(const Obj3&) = delete;

		std::unique_ptr<std::string> foo;

		Obj3(const Obj3&) = delete;

		std::unique_ptr<int32_t> a;

		Obj3() = default;
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
	inline void runRoundTripTest(const std::string& dataToParse, jsonifier::jsonifier_core<>& parser) {
		test_type valueNew{};
		std::string newString{};
		static constexpr rt_ut::string_literal testName{ testNameNew + ", " + testTypePartial<partial> + testTypeKnownOrder<knownOrder> + testTypeNullTerminated<nullTerminated> };
		parser.parseJson<jsonifier::parse_options{ .nullTerminated = nullTerminated, .partialRead = partial, .knownOrder = knownOrder }>(valueNew, dataToParse);
		for (auto& value: parser.getErrors()) {
			std::cout << "Jsonifier Error: " << value.reportError() << std::endl;
		}
		parser.serializeJson(valueNew, newString);
		for (auto& value: parser.getErrors()) {
			std::cout << "Jsonifier Error: " << value.reportError() << std::endl;
		}
		rt_ut::unit_test<testName, true>::run([&]() {
			return parser.getErrors().size() == 0;
		});
		return;
	}

	template<bool partial, bool knownOrder, bool nullTerminated> inline void roundTripTestsImpl() {
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

	template<rt_ut::string_literal testNameNew, bool nullTerminated> inline void runGenericRoundTripTest(const std::string& dataToParse, jsonifier::jsonifier_core<>& parser) {
		static constexpr rt_ut::string_literal testName{ testNameNew + ", Generic, " + testTypeNullTerminated<nullTerminated> };
		rt_ut::unit_test<testName, true>::template assert_eq<jsonifier::internal::parse_statuses::success>([&]() {
			return conformance_tests::genericStatus<nullTerminated>(dataToParse, parser);
		});
	}

	template<bool nullTerminated> inline void genericRoundTripTestsImpl() {
		jsonifier::jsonifier_core<> parser{};
		std::unordered_map<std::string, test_base> jsonTests{};
		processFilesInFolder(jsonTests, "/round_trip");
		std::cout << "Generic Round Trip Tests, " << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		runGenericRoundTripTest<"roundtrip01.json", nullTerminated>(jsonTests["roundtrip01.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip02.json", nullTerminated>(jsonTests["roundtrip02.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip03.json", nullTerminated>(jsonTests["roundtrip03.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip04.json", nullTerminated>(jsonTests["roundtrip04.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip05.json", nullTerminated>(jsonTests["roundtrip05.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip06.json", nullTerminated>(jsonTests["roundtrip06.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip07.json", nullTerminated>(jsonTests["roundtrip07.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip08.json", nullTerminated>(jsonTests["roundtrip08.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip09.json", nullTerminated>(jsonTests["roundtrip09.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip10.json", nullTerminated>(jsonTests["roundtrip10.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip11.json", nullTerminated>(jsonTests["roundtrip11.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip12.json", nullTerminated>(jsonTests["roundtrip12.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip13.json", nullTerminated>(jsonTests["roundtrip13.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip14.json", nullTerminated>(jsonTests["roundtrip14.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip15.json", nullTerminated>(jsonTests["roundtrip15.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip16.json", nullTerminated>(jsonTests["roundtrip16.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip17.json", nullTerminated>(jsonTests["roundtrip17.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip18.json", nullTerminated>(jsonTests["roundtrip18.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip19.json", nullTerminated>(jsonTests["roundtrip19.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip20.json", nullTerminated>(jsonTests["roundtrip20.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip21.json", nullTerminated>(jsonTests["roundtrip21.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip22.json", nullTerminated>(jsonTests["roundtrip22.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip23.json", nullTerminated>(jsonTests["roundtrip23.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip24.json", nullTerminated>(jsonTests["roundtrip24.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip25.json", nullTerminated>(jsonTests["roundtrip25.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip26.json", nullTerminated>(jsonTests["roundtrip26.json"].fileContents, parser);
		runGenericRoundTripTest<"roundtrip27.json", nullTerminated>(jsonTests["roundtrip27.json"].fileContents, parser);
	}

	inline void runTests() {
		roundTripTestsImpl<false, false, false>();
		roundTripTestsImpl<false, true, false>();
		roundTripTestsImpl<true, false, false>();
		roundTripTestsImpl<true, true, false>();
		roundTripTestsImpl<false, false, true>();
		roundTripTestsImpl<false, true, true>();
		roundTripTestsImpl<true, false, true>();
		roundTripTestsImpl<true, true, true>();
		genericRoundTripTestsImpl<false>();
		genericRoundTripTestsImpl<true>();
		std::cout << "Round trip validation tests complete." << std::endl;
	}
}
