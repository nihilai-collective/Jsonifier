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
