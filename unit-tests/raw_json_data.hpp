/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/raw_json_data.hpp
 */
#pragma once

#include "common.hpp"

namespace raw_json_data_tests {

	inline static void runTests() {
		std::cout << "raw_json_data Tests" << std::endl;

		jsonifier::jsonifier_core<> parser{};

		static constexpr rt_ut::string_literal typeObjectName{ "raw_json_data_get_type_object" };
		rt_ut::unit_test<typeObjectName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::object), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1})" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeArrayName{ "raw_json_data_get_type_array" };
		rt_ut::unit_test<typeArrayName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::array), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeStringName{ "raw_json_data_get_type_string" };
		rt_ut::unit_test<typeStringName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::string), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"("hello")" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeNumberIntName{ "raw_json_data_get_type_number_int" };
		rt_ut::unit_test<typeNumberIntName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::number), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "12345" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeNumberNegativeName{ "raw_json_data_get_type_number_negative" };
		rt_ut::unit_test<typeNumberNegativeName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::number), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-42" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeBoolTrueName{ "raw_json_data_get_type_bool_true" };
		rt_ut::unit_test<typeBoolTrueName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::boolean), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "true" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeBoolFalseName{ "raw_json_data_get_type_bool_false" };
		rt_ut::unit_test<typeBoolFalseName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::boolean), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "false" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeNullName{ "raw_json_data_get_type_null" };
		rt_ut::unit_test<typeNullName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::null), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "null" });
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal typeDefaultConstructedName{ "raw_json_data_get_type_default_constructed" };
		rt_ut::unit_test<typeDefaultConstructedName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::null), [] {
			jsonifier::raw_json_data data{};
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal getUintRoundTripName{ "raw_json_data_get_uint_round_trip" };
		rt_ut::unit_test<getUintRoundTripName, true>::assert_eq(static_cast<uint64_t>(18446744073709551615ULL), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "18446744073709551615" });
			return data.getUint();
		});

		static constexpr rt_ut::string_literal getIntRoundTripPositiveName{ "raw_json_data_get_int_round_trip_positive" };
		rt_ut::unit_test<getIntRoundTripPositiveName, true>::assert_eq(static_cast<int64_t>(9223372036854775807LL), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "9223372036854775807" });
			return data.getInt();
		});

		static constexpr rt_ut::string_literal getIntRoundTripNegativeName{ "raw_json_data_get_int_round_trip_negative" };
		rt_ut::unit_test<getIntRoundTripNegativeName, true>::assert_eq(static_cast<int64_t>(-9223372036854775807LL - 1), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-9223372036854775808" });
			return data.getInt();
		});
		static constexpr rt_ut::string_literal getDoubleRoundTripName{ "raw_json_data_get_double_round_trip" };
		rt_ut::unit_test<getDoubleRoundTripName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "3.14159" });
			auto val = data.getDouble();
			return val > 3.14158 && val < 3.14160;
		});

		static constexpr rt_ut::string_literal getDoubleNegativeName{ "raw_json_data_get_double_negative" };
		rt_ut::unit_test<getDoubleNegativeName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-2.5" });
			auto val = data.getDouble();
			return val > -2.50001 && val < -2.49999;
		});

		static constexpr rt_ut::string_literal getDoubleScientificName{ "raw_json_data_get_double_scientific_notation" };
		rt_ut::unit_test<getDoubleScientificName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "1.5e3" });
			auto val = data.getDouble();
			return val > 1499.9 && val < 1500.1;
		});

		static constexpr rt_ut::string_literal getDoubleLeadingZeroName{ "raw_json_data_get_double_leading_zero" };
		rt_ut::unit_test<getDoubleLeadingZeroName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "0.5" });
			auto val = data.getDouble();
			return val > 0.4999 && val < 0.5001;
		});

		static constexpr rt_ut::string_literal getDoubleNegativeFractionName{ "raw_json_data_get_double_negative_fraction" };
		rt_ut::unit_test<getDoubleNegativeFractionName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-0.1" });
			return data.getNumber().getType() == jsonifier::json_number::number_types::double64 && std::bit_cast<uint64_t>(data.getDouble()) == std::bit_cast<uint64_t>(-0.1);
		});

		static constexpr rt_ut::string_literal getDoubleNegativeExponentName{ "raw_json_data_get_double_negative_exponent" };
		rt_ut::unit_test<getDoubleNegativeExponentName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "-1e2" });
			return data.getNumber().getType() == jsonifier::json_number::number_types::double64 && std::bit_cast<uint64_t>(data.getDouble()) == std::bit_cast<uint64_t>(-100.0);
		});

		static constexpr rt_ut::string_literal getDoubleNegativeInArrayName{ "raw_json_data_get_double_negative_in_array" };
		rt_ut::unit_test<getDoubleNegativeInArrayName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "[-7.9431,-12]" });
			return std::bit_cast<uint64_t>(data[0ULL].getDouble()) == std::bit_cast<uint64_t>(-7.9431) && data[1ULL].getInt() == -12;
		});
		static constexpr rt_ut::string_literal arrayIndexAccessName{ "raw_json_data_array_index_access" };
		rt_ut::unit_test<arrayIndexAccessName, true>::assert_eq(static_cast<uint64_t>(3), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return data[2ULL].getUint();
		});

		static constexpr rt_ut::string_literal arrayIndexAccessConstName{ "raw_json_data_array_index_access_const" };
		rt_ut::unit_test<arrayIndexAccessConstName, true>::assert_eq(static_cast<uint64_t>(1), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			const jsonifier::raw_json_data& constData = data;
			return constData[0ULL].getUint();
		});

		static constexpr rt_ut::string_literal objectKeyAccessName{ "raw_json_data_object_key_access" };
		rt_ut::unit_test<objectKeyAccessName, true>::assert_eq(jsonifier::string{ "world" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"hello":"world"})" });
			return data["hello"].getString();
		});

		static constexpr rt_ut::string_literal objectKeyAccessConstName{ "raw_json_data_object_key_access_const" };
		rt_ut::unit_test<objectKeyAccessConstName, true>::assert_eq(jsonifier::string{ "world" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"hello":"world"})" });
			const jsonifier::raw_json_data& constData = data;
			return constData["hello"].getString();
		});

		static constexpr rt_ut::string_literal objectAutoVivifyName{ "raw_json_data_object_operator_bracket_autovivify_from_null" };
		rt_ut::unit_test<objectAutoVivifyName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_type::object), [] {
			jsonifier::raw_json_data data{};
			data["newKey"];
			return static_cast<uint8_t>(data.getType());
		});

		static constexpr rt_ut::string_literal containsTrueName{ "raw_json_data_contains_true" };
		rt_ut::unit_test<containsTrueName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"key":1})" });
			return data.contains("key");
		});

		static constexpr rt_ut::string_literal containsFalseMissingKeyName{ "raw_json_data_contains_false_missing_key" };
		rt_ut::unit_test<containsFalseMissingKeyName, true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"key":1})" });
			return data.contains("missing");
		});

		static constexpr rt_ut::string_literal containsFalseNotObjectName{ "raw_json_data_contains_false_when_not_object" };
		rt_ut::unit_test<containsFalseNotObjectName, true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3])" });
			return data.contains("key");
		});

		static constexpr rt_ut::string_literal containsFalseOnNumberName{ "raw_json_data_contains_false_on_number" };
		rt_ut::unit_test<containsFalseOnNumberName, true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			return data.contains("key");
		});

		static constexpr rt_ut::string_literal sizeObjectName{ "raw_json_data_size_object" };
		rt_ut::unit_test<sizeObjectName, true>::assert_eq(static_cast<uint64_t>(2), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1,"b":2})" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeArrayName{ "raw_json_data_size_array" };
		rt_ut::unit_test<sizeArrayName, true>::assert_eq(static_cast<uint64_t>(4), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1,2,3,4])" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeStringName{ "raw_json_data_size_string" };
		rt_ut::unit_test<sizeStringName, true>::assert_eq(static_cast<uint64_t>(5), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"("hello")" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeNumberIsZeroName{ "raw_json_data_size_number_is_zero" };
		rt_ut::unit_test<sizeNumberIsZeroName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeBoolIsZeroName{ "raw_json_data_size_bool_is_zero" };
		rt_ut::unit_test<sizeBoolIsZeroName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "true" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeNullIsZeroName{ "raw_json_data_size_null_is_zero" };
		rt_ut::unit_test<sizeNullIsZeroName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "null" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeEmptyObjectName{ "raw_json_data_size_empty_object" };
		rt_ut::unit_test<sizeEmptyObjectName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "{}" });
			return data.size();
		});

		static constexpr rt_ut::string_literal sizeEmptyArrayName{ "raw_json_data_size_empty_array" };
		rt_ut::unit_test<sizeEmptyArrayName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "[]" });
			return data.size();
		});

		static constexpr rt_ut::string_literal equalityTrueName{ "raw_json_data_equality_true_for_identical" };
		rt_ut::unit_test<equalityTrueName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ R"({"a":1})" });
			parser.parseJson(data2, std::string{ R"({"a":1})" });
			return data1 == data2;
		});

		static constexpr rt_ut::string_literal equalityFalseDifferentValueName{ "raw_json_data_equality_false_different_value" };
		rt_ut::unit_test<equalityFalseDifferentValueName, true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ R"({"a":1})" });
			parser.parseJson(data2, std::string{ R"({"a":2})" });
			return data1 == data2;
		});

		static constexpr rt_ut::string_literal equalityFalseDifferentTypeName{ "raw_json_data_equality_false_different_type" };
		rt_ut::unit_test<equalityFalseDifferentTypeName, true>::assert_eq(false, [&] {
			jsonifier::raw_json_data data1{};
			jsonifier::raw_json_data data2{};
			parser.parseJson(data1, std::string{ "1" });
			parser.parseJson(data2, std::string{ "true" });
			return data1 == data2;
		});

		static constexpr rt_ut::string_literal jsonNumberEqualityName{ "json_number_equality_matches_raw_string" };
		rt_ut::unit_test<jsonNumberEqualityName, true>::assert_eq(true, [] {
			jsonifier::json_number a{ jsonifier::string{ "42" } };
			jsonifier::json_number b{ jsonifier::string{ "42" } };
			return a == b;
		});

		static constexpr rt_ut::string_literal noErrorsOnValidInputName{ "raw_json_data_no_errors_on_valid_input" };
		rt_ut::unit_test<noErrorsOnValidInputName, true>::assert_eq(static_cast<uint64_t>(0), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":1,"b":[1,2,3],"c":"str"})" });
			return parser.getErrors().size();
		});

		static constexpr rt_ut::string_literal nestedObjectInArrayName{ "raw_json_data_nested_object_in_array" };
		rt_ut::unit_test<nestedObjectInArrayName, true>::assert_eq(static_cast<uint64_t>(7), [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([{"x":7}])" });
			return data[0ULL]["x"].getUint();
		});

		static constexpr rt_ut::string_literal deeplyNestedAccessName{ "raw_json_data_deeply_nested_access" };
		rt_ut::unit_test<deeplyNestedAccessName, true>::assert_eq(jsonifier::string{ "found" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":{"b":{"c":"found"}}})" });
			return data["a"]["b"]["c"].getString();
		});

		static constexpr rt_ut::string_literal serializeMixedArrayName{ "raw_json_data_serialize_mixed_array" };
		rt_ut::unit_test<serializeMixedArrayName, true>::assert_eq(std::string{ R"([1,-2,1.5,"s",true,false,null,[],{}])" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"([1, -2, 1.5, "s", true, false, null, [], {}])" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		static constexpr rt_ut::string_literal serializeNestedObjectName{ "raw_json_data_serialize_nested_object" };
		rt_ut::unit_test<serializeNestedObjectName, true>::assert_eq(std::string{ R"({"a":[1,2]})" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"a":[1,2]})" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		static constexpr rt_ut::string_literal serializeRootScalarName{ "raw_json_data_serialize_root_scalar" };
		rt_ut::unit_test<serializeRootScalarName, true>::assert_eq(std::string{ "42" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ "42" });
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		static constexpr rt_ut::string_literal serializeDefaultName{ "raw_json_data_serialize_default_constructed_is_null" };
		rt_ut::unit_test<serializeDefaultName, true>::assert_eq(std::string{ "null" }, [&] {
			jsonifier::raw_json_data data{};
			std::string out{};
			parser.serializeJson(data, out);
			return out;
		});

		static constexpr rt_ut::string_literal serializePrettifiedName{ "raw_json_data_serialize_prettified" };
		rt_ut::unit_test<serializePrettifiedName, true>::assert_eq(std::string{ "{\n   \"k\": [\n      3,\n      \"v\"\n   ]\n}" }, [&] {
			jsonifier::raw_json_data data{};
			parser.parseJson(data, std::string{ R"({"k":[3,"v"]})" });
			std::string out{};
			parser.serializeJson<jsonifier::serialize_options{ .prettify = true }>(data, out);
			return out;
		});

		static constexpr rt_ut::string_literal rejectInvalidName{ "raw_json_data_rejects_invalid_values" };
		rt_ut::unit_test<rejectInvalidName, true>::assert_eq(true, [&] {
			for (const char* json: { "[1,]", "[tru]", "[nul]", "[abc]", "[-]", "[01]", "[1.]", "[1]x", "{\"a\":fals}" }) {
				jsonifier::raw_json_data data{};
				if (parser.parseJson(data, std::string{ json })) {
					return false;
				}
			}
			return true;
		});

		static constexpr rt_ut::string_literal smallArrayCapacityName{ "raw_json_data_small_arrays_after_large_array" };
		rt_ut::unit_test<smallArrayCapacityName, true>::assert_eq(true, [&] {
			// A large array followed by small ones: the small arrays must not reserve the large array's size.
			std::string json = "[[";
			for (int i = 0; i < 1000; ++i) {
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

		static constexpr rt_ut::string_literal nestedDecodeName{ "raw_json_data_nested_decode" };
		rt_ut::unit_test<nestedDecodeName, true>::assert_eq(true, [&] {
			jsonifier::raw_json_data data{};
			if (!parser.parseJson(data, std::string{ R"({"a":[1,{"b":"c\"d"}],"e":true,"f":null,"g":-2.5})" })) {
				return false;
			}
			auto& object = data.getObject();
			return object["a"].getArray()[1ULL].getObject()["b"].getString() == "c\"d" && object["e"].getBool() && object["f"].getType() == jsonifier::json_type::null &&
				std::bit_cast<uint64_t>(object["g"].getDouble()) == std::bit_cast<uint64_t>(-2.5);
		});

		std::cout << "raw_json_data validation tests complete." << std::endl;
	}

}
