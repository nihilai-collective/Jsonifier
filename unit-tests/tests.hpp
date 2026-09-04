// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/tests.hpp
#pragma once

#include "add_tape_values.hpp"
#include "allocator.hpp"
#include "array.hpp"
#include "bounds_test.hpp"
#include "compare.hpp"
#include "conformance.hpp"
#include "error.hpp"
#include "fastio.hpp"
#include "float.hpp"
#include "get_enum_name.hpp"
#include "hash.hpp"
#include "hash_map.hpp"
#include "integer.hpp"
#include "intrinsics.hpp"
#include "iterator.hpp"
#include "JSONTestSuite.hpp"
#include "minifier.hpp"
#include "parsing_tests.hpp"
#include "prettifier.hpp"
#include "printer.hpp"
#include "raw_json_data.hpp"
#include "reflection.hpp"
#include "round_trip.hpp"
#include "string.hpp"
#include "string_class.hpp"
#include "string_literal_comparator.hpp"
#include "tuple.hpp"
#include "unit_tests.hpp"
#include "utf8_validation.hpp"

namespace tests {

	inline static void testFunction() {
		add_tape_values_tests::runTests();
		allocator_tests::runTests();
		array_tests::runTests();
		bounds_tests::runTests();
		compare_tests::runTests();
		conformance_tests::runTests();
		error_tests::runTests();
		fastio_tests::runTests();
		float_validation_tests::runTests();
		enum_name_tests::runTests();
		hash_validation_tests::runTests();
		hash_map_tests::runTests();
		int_validation_tests::runTests();
		uint_validation_tests::runTests();
		i_to_str_tests::runTests();
		intrinsics_tests::runTests();
		iterator_tests::runTests();
		json_test_suite_tests::runTests();
		minifier_tests::runTests();
		parsing_tests::runTests();
		prettifier_tests::runTests();
		printer_tests::runTests();
		raw_json_data_tests::runTests();
		reflection_tests::runTests();
		round_trip_tests::runTests();
		string_validation_tests::runTests();
		string_class_tests::runTests();
		string_literal_comparator_impl_tests::runTests();
		tuple_tests::runTests();
		unit_tests::runTests();
		utf8_validation_tests::runTests();
	};

}
