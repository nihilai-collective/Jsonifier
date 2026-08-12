// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/tests.hpp
#pragma once

#include "bounds_test.hpp"
#include "conformance.hpp"
#include "error.hpp"
#include "float.hpp"
#include "int.hpp"
#include "intrinsics.hpp"
#include "JSONTestSuite.hpp"
#include "parsing_tests.hpp"
#include "raw_json_data.hpp"
#include "round_trip.hpp"
#include "string.hpp"
#include "uint.hpp"
#include "unit_tests.hpp"
#include "utf8_validation.hpp"

namespace tests {

	inline static void testFunction() {
		bounds_tests::runTests();
		conformance_tests::runTests();
		error_tests::runTests();
		float_validation_tests::runTests();
		int_validation_tests::runTests();
		intrinsics_tests::runTests();
		json_test_suite_tests::runTests();
		parsing_tests::runTests();
		raw_json_data_tests::runTests();
		round_trip_tests::runTests();
		string_validation_tests::runTests();
		uint_validation_tests::runTests();
		unit_tests::runTests();
		utf8_validation_tests::runTests();
	};

}
