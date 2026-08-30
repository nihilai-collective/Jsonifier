// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/printer.hpp
#pragma once

#include "common.hpp"

#include <sstream>

namespace printer_tests {

	struct leaf_struct {
		int32_t id{};
		std::string label{};
	};

	struct nested_printer_struct {
		leaf_struct inner{};
		std::vector<int32_t> numbers{};
		bool flag{};
	};

	enum class printer_enum : uint8_t { first, second };

}

template<> struct jsonifier::core<printer_tests::leaf_struct> {
	using value_type				 = printer_tests::leaf_struct;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::label>();
};

template<> struct jsonifier::core<printer_tests::nested_printer_struct> {
	using value_type				 = printer_tests::nested_printer_struct;
	static constexpr auto parseValue = createValue<&value_type::inner, &value_type::numbers, &value_type::flag>();
};

namespace printer_tests {

	inline static void runTests() {
		std::cout << "JSON Printer Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		rt_ut::unit_test<"printer_print_json_writes_header_line", true>::assert_eq(true, [&] {
			leaf_struct obj{ 7, "hello" };
			std::ostringstream os{};
			parser.printJson(obj, os);
			return os.str().find("Printing Json") == 0;
		});

		rt_ut::unit_test<"printer_print_json_impl_omits_header", true>::assert_eq(false, [&] {
			leaf_struct obj{ 7, "hello" };
			std::ostringstream os{};
			parser.printJsonImpl(obj, os);
			return os.str().find("Printing Json") != std::string::npos;
		});

		rt_ut::unit_test<"printer_object_reports_member_count_and_field_names", true>::assert_eq(true, [&] {
			leaf_struct obj{ 7, "hello" };
			std::ostringstream os{};
			parser.printJsonImpl(obj, os);
			auto text = os.str();
			return text.find("object (members: 2") != std::string::npos && text.find("id (size:") != std::string::npos && text.find("label (size:") != std::string::npos;
		});

		rt_ut::unit_test<"printer_int_field_prints_value", true>::assert_eq(true, [&] {
			leaf_struct obj{ 42, "hi" };
			std::ostringstream os{};
			parser.printJsonImpl(obj, os);
			return os.str().find("42 (size:") != std::string::npos;
		});

		rt_ut::unit_test<"printer_string_field_is_quoted_with_length", true>::assert_eq(true, [&] {
			leaf_struct obj{ 1, "hi" };
			std::ostringstream os{};
			parser.printJsonImpl(obj, os);
			return os.str().find("\"hi\" (length: 2") != std::string::npos;
		});

		rt_ut::unit_test<"printer_bool_prints_true", true>::assert_eq(true, [&] {
			std::ostringstream os{};
			parser.printJsonImpl(true, os);
			return os.str().find("true") != std::string::npos;
		});

		rt_ut::unit_test<"printer_bool_prints_false", true>::assert_eq(true, [&] {
			std::ostringstream os{};
			parser.printJsonImpl(false, os);
			return os.str().find("false") != std::string::npos;
		});

		rt_ut::unit_test<"printer_char_is_single_quoted", true>::assert_eq(true, [&] {
			std::ostringstream os{};
			parser.printJsonImpl('Q', os);
			return os.str().find("'Q'") != std::string::npos;
		});

		rt_ut::unit_test<"printer_enum_prints_underlying_integer", true>::assert_eq(true, [&] {
			std::ostringstream os{};
			parser.printJsonImpl(printer_enum::second, os);
			return os.str().find("1 (size:") != std::string::npos;
		});

		rt_ut::unit_test<"printer_vector_reports_element_count", true>::assert_eq(true, [&] {
			std::vector<int32_t> vec{ 1, 2, 3, 4 };
			std::ostringstream os{};
			parser.printJsonImpl(vec, os);
			return os.str().find("vector (elements: 4") != std::string::npos;
		});

		rt_ut::unit_test<"printer_array_reports_element_count", true>::assert_eq(true, [&] {
			jsonifier::internal::array<int32_t, 3> arr{ 5, 6, 7 };
			std::ostringstream os{};
			parser.printJsonImpl(arr, os);
			return os.str().find("array (elements: 3") != std::string::npos;
		});

		rt_ut::unit_test<"printer_map_reports_entry_count_and_key", true>::assert_eq(true, [&] {
			std::map<std::string, int32_t> map{ { "a", 1 }, { "b", 2 } };
			std::ostringstream os{};
			parser.printJsonImpl(map, os);
			auto text = os.str();
			return text.find("map (entries: 2") != std::string::npos && text.find("\"a\"") != std::string::npos;
		});

		rt_ut::unit_test<"printer_tuple_reports_element_indices", true>::assert_eq(true, [&] {
			std::tuple<int32_t, std::string> tup{ 5, "five" };
			std::ostringstream os{};
			parser.printJsonImpl(tup, os);
			auto text = os.str();
			return text.find("tuple (elements: 2") != std::string::npos && text.find("[0]:") != std::string::npos && text.find("[1]:") != std::string::npos;
		});

		rt_ut::unit_test<"printer_optional_with_value_unwraps_contents", true>::assert_eq(true, [&] {
			std::optional<double> opt{ 3.5 };
			std::ostringstream os{};
			parser.printJsonImpl(opt, os);
			return os.str().find("3.5") != std::string::npos;
		});

		rt_ut::unit_test<"printer_optional_without_value_prints_nullopt", true>::assert_eq(true, [&] {
			std::optional<double> opt{};
			std::ostringstream os{};
			parser.printJsonImpl(opt, os);
			return os.str().find("nullopt") != std::string::npos;
		});

		rt_ut::unit_test<"printer_null_shared_ptr_prints_null", true>::assert_eq(true, [&] {
			std::shared_ptr<leaf_struct> ptr{};
			std::ostringstream os{};
			parser.printJsonImpl(ptr, os);
			return os.str().find("null") != std::string::npos;
		});

		rt_ut::unit_test<"printer_populated_shared_ptr_unwraps_contents", true>::assert_eq(true, [&] {
			auto ptr = std::make_shared<leaf_struct>(leaf_struct{ 9, "nine" });
			std::ostringstream os{};
			parser.printJsonImpl(ptr, os);
			auto text = os.str();
			return text.find("shared_ptr ->") != std::string::npos && text.find("nine") != std::string::npos;
		});

		rt_ut::unit_test<"printer_null_raw_pointer_prints_null", true>::assert_eq(true, [&] {
			int32_t* ptr{ nullptr };
			std::ostringstream os{};
			parser.printJsonImpl(ptr, os);
			return os.str().find("null") != std::string::npos;
		});

		rt_ut::unit_test<"printer_populated_raw_pointer_unwraps_contents", true>::assert_eq(true, [&] {
			int32_t value{ 17 };
			int32_t* ptr{ &value };
			std::ostringstream os{};
			parser.printJsonImpl(ptr, os);
			auto text = os.str();
			return text.find("* ->") != std::string::npos && text.find("17 (size:") != std::string::npos;
		});

		rt_ut::unit_test<"printer_nullptr_t_prints_null", true>::assert_eq(true, [&] {
			std::ostringstream os{};
			parser.printJsonImpl(nullptr, os);
			return os.str().find("null") != std::string::npos;
		});

		rt_ut::unit_test<"printer_variant_reports_active_index_and_value", true>::assert_eq(true, [&] {
			std::variant<int32_t, std::string> var{ std::string{ "picked" } };
			std::ostringstream os{};
			parser.printJsonImpl(var, os);
			auto text = os.str();
			return text.find("variant (active index: 1") != std::string::npos && text.find("picked") != std::string::npos;
		});

		rt_ut::unit_test<"printer_skip_type_prints_skipped", true>::assert_eq(true, [&] {
			jsonifier::skip skipped{};
			std::ostringstream os{};
			parser.printJsonImpl(skipped, os);
			return os.str().find("skipped") != std::string::npos;
		});

		rt_ut::unit_test<"printer_raw_json_data_reports_raw_length", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1})" });
			std::ostringstream os{};
			parser.printJsonImpl(data, os);
			return os.str().find("raw_json (length: 7") != std::string::npos;
		});

		rt_ut::unit_test<"printer_nested_object_recurses_into_inner_fields", true>::assert_eq(true, [&] {
			nested_printer_struct obj{};
			obj.inner = { 3, "nested" };
			obj.numbers = { 1, 2 };
			std::ostringstream os{};
			parser.printJsonImpl(obj, os);
			auto text = os.str();
			return text.find("inner (size:") != std::string::npos && text.find("\"nested\"") != std::string::npos && text.find("vector (elements: 2") != std::string::npos;
		});

		std::cout << "JSON Printer validation tests complete." << std::endl;
	}

}
