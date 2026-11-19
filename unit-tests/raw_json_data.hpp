/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/raw_json_data.hpp
 */
#pragma once

#include "common.hpp"

namespace raw_json_data_tests {

	inline void runTests() {
		std::cout << "raw_json_data Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		rt_ut::unit_test<"raw_json_data_get_type_object", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::object), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1})" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_array", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::array), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_string", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::string), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"("hello")" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_number_int", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::number), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "12345" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_number_negative", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::number), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-42" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_bool_true", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::boolean), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "true" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_bool_false", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::boolean), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "false" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_null", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::null), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "null" });
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_type_default_constructed", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::null), [] {
			jsonifier::raw_json_data data{};
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_get_uint_round_trip", true>::assert_eq(static_cast<uint64_t>(18446744073709551615ULL), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "18446744073709551615" });
			return data.getUint();
		});

		rt_ut::unit_test<"raw_json_data_get_int_round_trip_positive", true>::assert_eq(static_cast<int64_t>(9223372036854775807LL), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "9223372036854775807" });
			return data.getInt();
		});

		rt_ut::unit_test<"raw_json_data_get_int_round_trip_negative", true>::assert_eq(static_cast<int64_t>(-9223372036854775807LL - 1), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-9223372036854775808" });
			return data.getInt();
		});
		rt_ut::unit_test<"raw_json_data_get_double_round_trip", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "3.14159" });
			auto val = data.getDouble();
			return val > 3.14158 && val < 3.14160;
		});

		rt_ut::unit_test<"raw_json_data_get_double_negative", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-2.5" });
			auto val = data.getDouble();
			return val > -2.50001 && val < -2.49999;
		});

		rt_ut::unit_test<"raw_json_data_get_double_scientific_notation", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "1.5e3" });
			auto val = data.getDouble();
			return val > 1499.9 && val < 1500.1;
		});

		rt_ut::unit_test<"raw_json_data_get_double_leading_zero", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "0.5" });
			auto val = data.getDouble();
			return val > 0.4999 && val < 0.5001;
		});

		rt_ut::unit_test<"raw_json_data_get_double_negative_fraction", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-0.1" });
			return data.getNumber().getType() == jsonifier::json_number::number_types::double64 && std::bit_cast<uint64_t>(data.getDouble()) == std::bit_cast<uint64_t>(-0.1);
		});

		rt_ut::unit_test<"raw_json_data_get_double_negative_exponent", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-1e2" });
			return data.getNumber().getType() == jsonifier::json_number::number_types::double64 && std::bit_cast<uint64_t>(data.getDouble()) == std::bit_cast<uint64_t>(-100.0);
		});

		rt_ut::unit_test<"raw_json_data_get_double_negative_in_array", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "[-7.9431,-12]" });
			return std::bit_cast<uint64_t>(data[0ULL].getDouble()) == std::bit_cast<uint64_t>(-7.9431) && data[1ULL].getInt() == -12;
		});
		rt_ut::unit_test<"raw_json_data_array_index_access", true>::assert_eq(static_cast<uint64_t>(3), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return data[2ULL].getUint();
		});

		rt_ut::unit_test<"raw_json_data_array_index_access_const", true>::assert_eq(static_cast<uint64_t>(1), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			const jsonifier::raw_json_data& constData = data;
			return constData[0ULL].getUint();
		});

		rt_ut::unit_test<"raw_json_data_object_key_access", true>::assert_eq(jsonifier::string{ "world" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"hello":"world"})" });
			return data["hello"].getString();
		});

		rt_ut::unit_test<"raw_json_data_object_key_access_const", true>::assert_eq(jsonifier::string{ "world" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"hello":"world"})" });
			const jsonifier::raw_json_data& constData = data;
			return constData["hello"].getString();
		});

		rt_ut::unit_test<"raw_json_data_object_operator_bracket_autovivify_from_null", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::object), [] {
			jsonifier::raw_json_data data{};
			data["newKey"];
			return static_cast<uint8_t>(data.getType());
		});

		rt_ut::unit_test<"raw_json_data_contains_true", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"key":1})" });
			return data.contains("key");
		});

		rt_ut::unit_test<"raw_json_data_contains_false_missing_key", true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"key":1})" });
			return data.contains("missing");
		});

		rt_ut::unit_test<"raw_json_data_contains_false_when_not_object", true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return data.contains("key");
		});

		rt_ut::unit_test<"raw_json_data_contains_false_on_number", true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			return data.contains("key");
		});

		rt_ut::unit_test<"raw_json_data_size_object", true>::assert_eq(static_cast<uint64_t>(2), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1,"b":2})" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_array", true>::assert_eq(static_cast<uint64_t>(4), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3,4])" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_string", true>::assert_eq(static_cast<uint64_t>(5), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"("hello")" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_number_is_zero", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_bool_is_zero", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "true" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_null_is_zero", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "null" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_empty_object", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "{}" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_size_empty_array", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "[]" });
			return data.size();
		});

		rt_ut::unit_test<"raw_json_data_equality_true_for_identical", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ R"({"a":1})" });
			parser.parseJson(data2, std::string{ R"({"a":1})" });
			return data1 == data2;
		});

		rt_ut::unit_test<"raw_json_data_equality_false_different_value", true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ R"({"a":1})" });
			parser.parseJson(data2, std::string{ R"({"a":2})" });
			return data1 == data2;
		});

		rt_ut::unit_test<"raw_json_data_equality_false_different_type", true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ "1" });
			parser.parseJson(data2, std::string{ "true" });
			return data1 == data2;
		});

		rt_ut::unit_test<"json_number_equality_matches_raw_string", true>::assert_eq(true, [] {
			jsonifier::json_number a{ jsonifier::string{ "42" } };
			jsonifier::json_number b{ jsonifier::string{ "42" } };
			return a == b;
		});

		rt_ut::unit_test<"raw_json_data_no_errors_on_valid_input", true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1,"b":[1,2,3],"c":"str"})" });
			return parser.getErrors().size();
		});

		rt_ut::unit_test<"raw_json_data_nested_object_in_array", true>::assert_eq(static_cast<uint64_t>(7), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([{"x":7}])" });
			return data[0ULL]["x"].getUint();
		});

		rt_ut::unit_test<"raw_json_data_deeply_nested_access", true>::assert_eq(jsonifier::string{ "found" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":{"b":{"c":"found"}}})" });
			return data["a"]["b"]["c"].getString();
		});

		rt_ut::unit_test<"raw_json_data_serialize_mixed_array", true>::assert_eq(std::string{ R"([1,-2,1.5,"s",true,false,null,[],{}])" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1, -2, 1.5, "s", true, false, null, [], {}])" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		rt_ut::unit_test<"raw_json_data_serialize_nested_object", true>::assert_eq(std::string{ R"({"a":[1,2]})" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":[1,2]})" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		rt_ut::unit_test<"raw_json_data_serialize_root_scalar", true>::assert_eq(std::string{ "42" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		rt_ut::unit_test<"raw_json_data_serialize_default_constructed_is_null", true>::assert_eq(std::string{ "null" }, [&] {
			jsonifier::raw_json_data data{};
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		rt_ut::unit_test<"raw_json_data_serialize_prettified", true>::assert_eq(std::string{ "{\n   \"k\": [\n      3,\n      \"v\"\n   ]\n}" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"k":[3,"v"]})" });
			std::string out{};
			parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, out);
			return out;
		});

		rt_ut::unit_test<"raw_json_data_rejects_invalid_values", true>::assert_eq(true, [&] {
			for (const char* json: { "[1,]", "[tru]", "[nul]", "[abc]", "[-]", "[01]", "[1.]", "[1]x", "{\"a\":fals}" }) {
				jsonifier::raw_json_data data{};
				if (parser.parseJson(data, std::string{ json })) {
					return false;
				}
			}
			return true;
		});

		rt_ut::unit_test<"raw_json_data_small_arrays_after_large_array", true>::assert_eq(true, [&] {
			// A large array followed by small ones: the small arrays must not reserve the large array's size.
			std::string json = "[[";
			for (int32_t i = 0; i < 1000; ++i) {
				json += i ? ",0" : "0";
			}
			json += "],[1,2],[3,4]]";
			jsonifier::raw_json_data data{};
			if (!parser.parseJson(data, json)) {
				return false;
			}
			const auto& outer = data.getArray();
			return outer.size() == 3 && outer[0ULL].getArray().size() == 1000 && outer[1ULL].getArray().size() == 2 && outer[1ULL].getArray().capacity() < 16 &&
				outer[2ULL].getArray()[1ULL].getUint() == 4;
		});

		rt_ut::unit_test<"raw_json_data_nested_decode", true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			if (!parser.parseJson(data, std::string{ R"({"a":[1,{"b":"c\"d"}],"e":true,"f":null,"g":-2.5})" })) {
				return false;
			}
			auto& object = data.getObject();
			return object["a"].getArray()[1ULL].getObject()["b"].getString() == "c\"d" && object["e"].getBool() && object["f"].getType() == jsonifier::json_types::null &&
				std::bit_cast<uint64_t>(object["g"].getDouble()) == std::bit_cast<uint64_t>(-2.5);
		});

		std::cout << "raw_json_data validation tests complete." << std::endl;
	}

}
