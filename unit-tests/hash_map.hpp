// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/hash_map.hpp
#pragma once

#include "common.hpp"

namespace hash_map_tests {

	struct hm_empty {};

	struct hm_single {
		int32_t only{};
	};

	struct hm_pair {
		int32_t id{};
		std::string label{};
	};

	struct hm_five {
		int32_t alpha{};
		int32_t bravo{};
		int32_t charlie{};
		int32_t delta{};
		int32_t echo{};
	};

	struct hm_first_byte_bucketed {
		int32_t get_x{};
		int32_t get_y{};
		int32_t set_x{};
		int32_t set_y{};
	};

	struct hm_wide_binary_keys {
		int32_t field00{};
		int32_t field01{};
		int32_t field02{};
		int32_t field03{};
		int32_t field04{};
		int32_t field05{};
		int32_t field06{};
		int32_t field07{};
		int32_t field08{};
		int32_t field09{};
		int32_t field10{};
		int32_t field11{};
		int32_t field12{};
		int32_t field13{};
		int32_t field14{};
		int32_t field15{};
		int32_t field16{};
		int32_t field17{};
		int32_t field18{};
		int32_t field19{};
	};

}

template<> struct jsonifier::core<hash_map_tests::hm_empty> {
	using value_type				 = hash_map_tests::hm_empty;
	static constexpr auto parseValue = createValue<>();
};

template<> struct jsonifier::core<hash_map_tests::hm_single> {
	using value_type				 = hash_map_tests::hm_single;
	static constexpr auto parseValue = createValue<&value_type::only>();
};

template<> struct jsonifier::core<hash_map_tests::hm_pair> {
	using value_type				 = hash_map_tests::hm_pair;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::label>();
};

template<> struct jsonifier::core<hash_map_tests::hm_five> {
	using value_type				 = hash_map_tests::hm_five;
	static constexpr auto parseValue = createValue<&value_type::alpha, &value_type::bravo, &value_type::charlie, &value_type::delta, &value_type::echo>();
};

template<> struct jsonifier::core<hash_map_tests::hm_first_byte_bucketed> {
	using value_type				 = hash_map_tests::hm_first_byte_bucketed;
	static constexpr auto parseValue = createValue<&value_type::get_x, &value_type::get_y, &value_type::set_x, &value_type::set_y>();
};

template<> struct jsonifier::core<hash_map_tests::hm_wide_binary_keys> {
	using value_type				 = hash_map_tests::hm_wide_binary_keys;
	static constexpr auto parseValue = createValue<makeJsonEntity<&value_type::field00, "00000">(), makeJsonEntity<&value_type::field01, "00001">(),
		makeJsonEntity<&value_type::field02, "00010">(), makeJsonEntity<&value_type::field03, "00011">(), makeJsonEntity<&value_type::field04, "00100">(),
		makeJsonEntity<&value_type::field05, "00101">(), makeJsonEntity<&value_type::field06, "00110">(), makeJsonEntity<&value_type::field07, "00111">(),
		makeJsonEntity<&value_type::field08, "01000">(), makeJsonEntity<&value_type::field09, "01001">(), makeJsonEntity<&value_type::field10, "01010">(),
		makeJsonEntity<&value_type::field11, "01011">(), makeJsonEntity<&value_type::field12, "01100">(), makeJsonEntity<&value_type::field13, "01101">(),
		makeJsonEntity<&value_type::field14, "01110">(), makeJsonEntity<&value_type::field15, "01111">(), makeJsonEntity<&value_type::field16, "10000">(),
		makeJsonEntity<&value_type::field17, "10001">(), makeJsonEntity<&value_type::field18, "10010">(), makeJsonEntity<&value_type::field19, "10011">()>();
};

namespace hash_map_tests {

	template<typename value_type> inline static uint64_t lookupKey(const std::string& key) {
		std::string buffer = key + "\":0}";
		return jsonifier::internal::hash_map<value_type, jsonifier::string_view_ptr>::findIndex(buffer.data(), buffer.data() + buffer.size());
	}

	inline static void runTests() {
		std::cout << "hash_map Tests" << std::endl;

		rt_ut::unit_test<"hash_map_empty_struct_never_matches", true>::assert_eq(true, [] {
			return lookupKey<hm_empty>("anything") == jsonifier::internal::hashData<hm_empty>.storageSize;
		});

		rt_ut::unit_test<"hash_map_empty_struct_round_trips_as_empty_object", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_empty obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			hm_empty parsed{};
			parser.parseJson(parsed, json);
			return json == std::string{ "{}" };
		});

		rt_ut::unit_test<"hash_map_find_index_single_element_always_returns_zero", true>::assert_eq(true, [] {
			return lookupKey<hm_single>("only") == 0 && lookupKey<hm_single>("literally_anything") == 0 && lookupKey<hm_single>("x") == 0;
		});

		rt_ut::unit_test<"hash_map_pair_resolves_correct_indices", true>::assert_eq(true, [] {
			return lookupKey<hm_pair>("id") == 0 && lookupKey<hm_pair>("label") == 1;
		});

		rt_ut::unit_test<"hash_map_five_fields_resolve_correct_indices", true>::assert_eq(true, [] {
			return lookupKey<hm_five>("alpha") == 0 && lookupKey<hm_five>("bravo") == 1 && lookupKey<hm_five>("charlie") == 2 && lookupKey<hm_five>("delta") == 3 &&
				lookupKey<hm_five>("echo") == 4;
		});

		rt_ut::unit_test<"hash_map_first_byte_bucket_resolves_correct_indices", true>::assert_eq(true, [] {
			return lookupKey<hm_first_byte_bucketed>("get_x") == 0 && lookupKey<hm_first_byte_bucketed>("get_y") == 1 && lookupKey<hm_first_byte_bucketed>("set_x") == 2 &&
				lookupKey<hm_first_byte_bucketed>("set_y") == 3;
		});

		rt_ut::unit_test<"hash_map_wide_binary_keys_resolve_correct_indices", true>::assert_eq(true, [] {
			static constexpr std::array<const char*, 20> keys{ { "00000", "00001", "00010", "00011", "00100", "00101", "00110", "00111", "01000", "01001", "01010", "01011",
				"01100", "01101", "01110", "01111", "10000", "10001", "10010", "10011" } };
			bool allMatch = true;
			for (uint64_t i = 0; i < 20 && allMatch; ++i) {
				allMatch = lookupKey<hm_wide_binary_keys>(keys[i]) == i;
			}
			return allMatch;
		});

		rt_ut::unit_test<"hash_map_wide_binary_keys_round_trip", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_wide_binary_keys obj{};
			obj.field00 = 100;
			obj.field07 = 42;
			obj.field19 = 999;
			std::string json{};
			parser.serializeJson(obj, json);
			hm_wide_binary_keys parsed{};
			parser.parseJson(parsed, json);
			return parsed.field00 == 100 && parsed.field07 == 42 && parsed.field19 == 999;
		});

		rt_ut::unit_test<"hash_map_unknown_key_in_input_is_skipped_gracefully", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_five obj{};
			std::string json =
				R"({"alpha":1,"mystery_field":{"nested":[1,2,3]},"bravo":2,"charlie":3,"unexpected":"value","delta":4,"echo":5})";
			parser.parseJson(obj, json);
			return obj.alpha == 1 && obj.bravo == 2 && obj.charlie == 3 && obj.delta == 4 && obj.echo == 5;
		});

		rt_ut::unit_test<"hash_map_near_miss_key_in_input_does_not_corrupt_real_field", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_pair obj{};
			std::string json = R"({"ids":999,"id":7,"labels":"wrong","label":"right"})";
			parser.parseJson(obj, json);
			return obj.id == 7 && obj.label == std::string{ "right" };
		});

		rt_ut::unit_test<"hash_map_wide_binary_keys_unknown_pattern_does_not_corrupt_real_fields", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_wide_binary_keys obj{};
			std::string json = R"({"11111":404,"00000":1,"01110000":404,"10011":19,"0000":404})";
			parser.parseJson(obj, json);
			return obj.field00 == 1 && obj.field19 == 19;
		});

		rt_ut::unit_test<"hash_map_single_element_parser_still_validates_key_text", true>::assert_eq(true, [] {
			jsonifier::jsonifier_core<> parser{};
			hm_single correct{};
			parser.parseJson(correct, std::string{ R"({"only":42})" });
			hm_single wrong{};
			parser.parseJson(wrong, std::string{ R"({"totally_wrong_name":42})" });
			return correct.only == 42 && wrong.only == 0;
		});

		std::cout << "hash_map validation tests complete." << std::endl;
	}

}
