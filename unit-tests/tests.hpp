/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/tests.hpp
 */
#pragma once

#include "add_tape_values.hpp"
#include "allocator.hpp"
#include "array.hpp"
#include "compare.hpp"
#include "error.hpp"
#include "fastio.hpp"
#include "generic_parser.hpp"
#include "float.hpp"
#include "get_enum_name.hpp"
#include "integer.hpp"
#include "intrinsics.hpp"
#include "iterator.hpp"
#include "memcpy.hpp"
#include "string.hpp"
#include "string_class.hpp"
#include "string_literal_comparator.hpp"
#include "tuple.hpp"
#include "utf8_validation.hpp"

namespace tests {

	inline void testFunction() {
		add_tape_values_tests::runTests();
		allocator_tests::runTests();
		array_tests::runTests();
		compare_tests::runTests();
		error_tests::runTests();
		fastio_tests::runTests();
		float_validation_tests::runTests();
		generic_parser_tests::runTests();
		enum_name_tests::runTests();
		int_validation_tests::runTests();
		uint_validation_tests::runTests();
		intrinsics_tests::runTests();
		iterator_tests::runTests();
		memcpy_tests::runTests();
		string_validation_tests::runTests();
		string_class_tests::runTests();
		string_literal_comparator_impl_tests::runTests();
		tuple_tests::runTests();
		utf8_validation_tests::runTests();
	};

}
