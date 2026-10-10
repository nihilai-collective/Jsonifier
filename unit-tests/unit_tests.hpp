/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/unit_tests.hpp
 */
#pragma once

#include "common.hpp"

enum class Color : uint8_t { Red, Green, Blue };

struct simple_struct {
	std::string name{};
	double value{};
	int32_t id{};
};

template<> struct jsonifier::core<simple_struct> {
	using value_type				 = simple_struct;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::name, &value_type::value>();
};

struct escaped_keys {
	int32_t quote{};
	int32_t backslash{};
	int32_t both{};
};

template<> struct jsonifier::core<escaped_keys> {
	using value_type = escaped_keys;
	static constexpr auto parseValue =
		createValue<makeJsonEntity<&value_type::quote, "say \"hi\"">(), makeJsonEntity<&value_type::backslash, "C:\\dir">(), makeJsonEntity<&value_type::both, "\\\"">()>();
};

struct char_roundtrip {
	uint8_t uchar_val{};
	int32_t int_val{};
	char char_val{};
};

template<> struct jsonifier::core<char_roundtrip> {
	using value_type				 = char_roundtrip;
	static constexpr auto parseValue = createValue<&value_type::char_val, &value_type::uchar_val, &value_type::int_val>();
};

struct sub_thing {
	std::string b{ "stuff" };
	double a{ 3.14 };
};

template<> struct jsonifier::core<sub_thing> {
	using value_type				 = sub_thing;
	static constexpr auto parseValue = createValue<&value_type::a, &value_type::b>();
};

struct sub_thing2 {
	double d{ 0.000000000001 };
	double e{ 203082348402.1 };
	double g{ 12380.00000013 };
	double h{ 1000000.000001 };
	double c{ 999.342494903 };
	std::string b{ "stuff" };
	float f{ 89.089f };
	double a{ 3.14 };
};

template<> struct jsonifier::core<sub_thing2> {
	using value_type = sub_thing2;
	static constexpr auto parseValue =
		createValue<&value_type::a, &value_type::b, &value_type::c, &value_type::d, &value_type::e, &value_type::f, &value_type::g, &value_type::h>();
};

struct V3 {
	bool operator==(const V3& rhs) const {
		return (std::equal_to<double>{}(x, rhs.x) && std::equal_to<double>{}(y, rhs.y) && std::equal_to<double>{}(z, rhs.z));
	}

	double x{ 3.14 };
	double y{ 2.7 };
	double z{ 6.5 };
};

template<> struct jsonifier::core<V3> {
	using value_type				 = V3;
	static constexpr auto parseValue = createValue<&value_type::x, &value_type::y, &value_type::z>();
};

struct nested_struct {
	std::vector<int32_t> numbers{};
	simple_struct inner{};
	bool flag{};
};

template<> struct jsonifier::core<nested_struct> {
	using value_type				 = nested_struct;
	static constexpr auto parseValue = createValue<&value_type::inner, &value_type::numbers, &value_type::flag>();
};

struct Thing {
	jsonifier::internal::array<std::string, 4> array{ { "as\"df\\ghjkl", "pie", "42", "foo" } };
	std::map<std::string, int32_t> map{ { "a", 4 }, { "f", 7 }, { "b", 12 } };
	std::vector<bool> vb{ true, false, false, true, true, true, true };
	std::shared_ptr<sub_thing> sptr = std::make_shared<sub_thing>();
	jsonifier::internal::array<sub_thing2, 1> thing2array{};
	std::vector<V3> vector{ { 9.0, 6.7, 3.1 }, {} };
	std::vector<double> doubles{ 9.0, 6.7, 3.1 };
	std::vector<int32_t> numbers{ 6, 7, 8, 2 };
	std::optional<V3> optional{};
	Color color{ Color::Green };
	sub_thing thing{};
	double d{ 2.0 };
	int32_t i{ 8 };
	char c{ 'W' };
	V3 vec3{};
	bool b{};
};

template<> struct jsonifier::core<Thing> {
	using value_type = Thing;
	static constexpr auto parseValue =
		createValue<&value_type::thing, &value_type::thing2array, &value_type::vec3, &value_type::numbers, &value_type::doubles, &value_type::vector, &value_type::i,
			&value_type::d, &value_type::b, &value_type::c, &value_type::color, &value_type::vb, &value_type::sptr, &value_type::optional, &value_type::array, &value_type::map>();
};

struct escaped_struct {
	std::string escaped_key2{ "hi" };
	std::string escape_chars{};
	int32_t escaped_key{};
};

template<> struct jsonifier::core<escaped_struct> {
	using value_type = escaped_struct;
	static constexpr auto parseValue =
		createValue<makeJsonEntity<&value_type::escaped_key, "escaped\"key">(), makeJsonEntity<&value_type::escaped_key2, "escaped\"\"key2">(), &value_type::escape_chars>();
};

enum class Vehicle : uint32_t { Car, Truck, Plane };

enum class TestData : uint8_t { None, A, B, C, D, ERROR_E = 0xFF };

struct dummy_data {
	TestData b{ TestData::None };
	TestData c{ TestData::None };
	TestData d{ TestData::None };
	TestData e{ TestData::None };
	uint32_t id{ 0 };
	int32_t a{ 0 };
	int64_t f{ 0 };
};

template<> struct jsonifier::core<dummy_data> {
	using value_type				 = dummy_data;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::a, &value_type::b, &value_type::c, &value_type::d, &value_type::e, &value_type::f>();
};

struct BasicStruct {
	std::array<uint32_t, 3> arr{};
	std::string str{};
	int32_t i{};
	double d{};
};

template<> struct jsonifier::core<BasicStruct> {
	using value_type				 = BasicStruct;
	static constexpr auto parseValue = createValue<&value_type::i, &value_type::d, &value_type::str, &value_type::arr>();
};

struct MetaStruct {
	std::string name{};
	int32_t count{};
};

template<> struct jsonifier::core<MetaStruct> {
	using value_type				 = MetaStruct;
	static constexpr auto parseValue = createValue<makeJsonEntity<&value_type::count, "cnt">(), makeJsonEntity<&value_type::name, "label">()>();
};

struct WithOptional {
	std::string required{ "default" };
	std::optional<double> maybe{};
};

template<> struct jsonifier::core<WithOptional> {
	using value_type				 = WithOptional;
	static constexpr auto parseValue = createValue<&value_type::required, &value_type::maybe>();
};

struct EnumHolder {
	Color c{ Color::Green };
};

template<> struct jsonifier::core<EnumHolder> {
	using value_type				 = EnumHolder;
	static constexpr auto parseValue = createValue<&value_type::c>();
};

struct ContainerStruct {
	jsonifier::internal::array<std::string, 2> arr{ { "Hello", "World" } };
	std::tuple<int32_t, double, std::string> tup{ 42, 2.718, "pi?" };
	std::vector<int32_t> vec{ 1, 2, 3 };
};

template<> struct jsonifier::core<ContainerStruct> {
	using value_type				 = ContainerStruct;
	static constexpr auto parseValue = createValue<&value_type::vec, &value_type::arr, &value_type::tup>();
};

struct MapStruct {
	std::map<std::string, int32_t> str_map{ { "one", 1 }, { "two", 2 } };
};

template<> struct jsonifier::core<MapStruct> {
	using value_type				 = MapStruct;
	static constexpr auto parseValue = createValue<&value_type::str_map>();
};

struct PrettifyStruct {
	std::string msg{};
	int32_t id{};
};

template<> struct jsonifier::core<PrettifyStruct> {
	using value_type				 = PrettifyStruct;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::msg>();
};

struct FloatPrecision {
	double val{ 3.141592653589793 };
};

template<> struct jsonifier::core<FloatPrecision> {
	using value_type				 = FloatPrecision;
	static constexpr auto parseValue = createValue<&value_type::val>();
};

struct NestedStruct {
	std::vector<int32_t> nums{};
	BasicStruct inner{};
};

template<> struct jsonifier::core<NestedStruct> {
	using value_type				 = NestedStruct;
	static constexpr auto parseValue = createValue<&value_type::inner, &value_type::nums>();
};

struct SharedPtrStruct {
	std::shared_ptr<BasicStruct> ptr{};
};

template<> struct jsonifier::core<SharedPtrStruct> {
	using value_type				 = SharedPtrStruct;
	static constexpr auto parseValue = createValue<&value_type::ptr>();
};

struct BasicStructVec {
	std::vector<BasicStruct> items{};
};

template<> struct jsonifier::core<BasicStructVec> {
	using value_type				 = BasicStructVec;
	static constexpr auto parseValue = createValue<&value_type::items>();
};

struct DummyDataVec {
	std::vector<dummy_data> items{};
};

template<> struct jsonifier::core<DummyDataVec> {
	using value_type				 = DummyDataVec;
	static constexpr auto parseValue = createValue<&value_type::items>();
};

template<typename parser_type> void printErrors(parser_type& parser) {
	for (auto& error: parser.getErrors()) {
		std::cout << error.reportError() << std::endl;
	}
}

namespace unit_tests {

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void unitTestsImpl() {
		static constexpr jsonifier::parse_options opts{ .nullTerminated = nullTerminated, .partialRead = partial, .knownOrder = knownOrder };

		static constexpr auto test_partial_basic = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,"str":"Hello","arr":[1,2,3]})";
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.str, parsed.arr[0]);
		};

		static constexpr auto test_partial_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct original{ .name = "roundtrip", .value = 2.71828, .id = 99 };
			std::string serialized{};
			parser.serializeJson(original, serialized);
			simple_struct parsed{};
			parser.parseJson<opts>(parsed, serialized);
			printErrors(parser);
			return std::make_tuple(parsed.id, parsed.name, parsed.value);
		};

		static constexpr auto test_partial_nested = []() {
			jsonifier::jsonifier_core<> parser{};
			nested_struct obj{};
			obj.inner	= { .name = "deep", .value = 9.5, .id = 7 };
			obj.numbers = { 1, 2, 3, 4, 5, 6, 7, 8 };
			obj.flag	= true;
			std::string json{};
			parser.serializeJson(obj, json);
			nested_struct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.id, parsed.inner.name, parsed.numbers.size(), parsed.flag);
		};

		static constexpr auto test_partial_meta_renamed = []() {
			jsonifier::jsonifier_core<> parser{};
			MetaStruct parsed{};
			std::string input = R"({"cnt":10,"label":"Widget"})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return std::make_tuple(parsed.count, parsed.name);
		};

		static constexpr auto test_partial_optional_present = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional parsed{};
			std::string input = R"({"required":"changed","maybe":3.1415})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return parsed.required == std::string{ "changed" } && parsed.maybe.has_value() && std::equal_to<double>{}(*parsed.maybe, 3.1415);
		};

		static constexpr auto test_partial_optional_absent = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional parsed{};
			std::string input = R"({"required":"only"})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return parsed.required == std::string{ "only" } && !parsed.maybe.has_value();
		};

		static constexpr auto test_partial_containers = []() {
			jsonifier::jsonifier_core<> parser{};
			ContainerStruct c{};
			std::string json{};
			parser.serializeJson(c, json);
			ContainerStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.vec == std::vector<int32_t>{ 1, 2, 3 }, parsed.arr[0], std::get<2>(parsed.tup));
		};

		static constexpr auto test_partial_map = []() {
			jsonifier::jsonifier_core<> parser{};
			MapStruct original{};
			std::string json{};
			parser.serializeJson(original, json);
			MapStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str_map["one"] == 1 && parsed.str_map["two"] == 2;
		};

		static constexpr auto test_partial_vector_of_structs = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStructVec original{};
			original.items = { { { .arr = { { 1, 2, 3 } }, .str = "a", .i = 1, .d = 1.1 }, { .arr = { { 4, 5, 6 } }, .str = "b", .i = 2, .d = 2.2 },
				{ .arr = { { 7, 8, 9 } }, .str = "c", .i = 3, .d = 3.3 } } };
			std::string json{};
			parser.serializeJson(original, json);
			BasicStructVec parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.items.size(), parsed.items[0].i, parsed.items[1].str, parsed.items[2].arr[2]);
		};

		static constexpr auto test_partial_complex_thing = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.c, parsed.numbers.size(), parsed.array[0]);
		};

		static constexpr auto test_partial_unicode = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { 1, 2, 3 }, .str = "Hello 世界 🌍 test", .i = 1, .d = 1.0 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("世界") != std::string::npos && parsed.str.find("🌍") != std::string::npos;
		};

		static constexpr auto test_partial_special_chars = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { 9, 9, 9 }, .str = "with \"quotes\" and \\ slash \n newline", .i = 5, .d = 6.7 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.i == 5 && parsed.str.find("\"quotes\"") != std::string::npos && parsed.str.find("\n") != std::string::npos;
		};

		static constexpr auto test_partial_large_payload = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStructVec original{};
			for (int32_t i = 0; i < 64; ++i) {
				original.items.push_back(
					{ .arr = { { uint32_t(i), uint32_t(i + 1), uint32_t(i + 2) } }, .str = "item_" + std::to_string(i), .i = i, .d = static_cast<double>(i) * 0.5 });
			}
			std::string json{};
			parser.serializeJson(original, json);
			BasicStructVec parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.items.size(), parsed.items[0].i, parsed.items[63].i, parsed.items[32].str);
		};

		static constexpr auto test_partial_boundary_lengths = []() {
			jsonifier::jsonifier_core<> parser{};
			bool all_passed = true;
			for (uint64_t pad = 0; pad < 80; ++pad) {
				BasicStruct obj{ .arr = { { 1, 2, 3 } }, .str = std::string(pad, 'x'), .i = 7, .d = 1.5 };
				std::string json{};
				parser.serializeJson(obj, json);
				BasicStruct parsed{};
				parser.parseJson<opts>(parsed, json);
				printErrors(parser);
				if (parsed.i != 7 || parsed.str.size() != pad || parsed.arr[2] != 3u) {
					all_passed = false;
					break;
				}
			}
			return all_passed;
		};

		static constexpr auto test_partial_nested_struct_vec = []() {
			jsonifier::jsonifier_core<> parser{};
			NestedStruct ns{};
			ns.inner = { .arr = { { 1, 2, 3 } }, .str = "nested", .i = 42, .d = 3.14 };
			ns.nums	 = { 10, 20, 30, 40, 50 };
			std::string json{};
			parser.serializeJson(ns, json);
			NestedStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.i, parsed.inner.str, parsed.nums.size(), parsed.nums[4]);
		};

		static constexpr auto test_partial_minified = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { { 1, 2, 3 } }, .str = "minified", .i = 42, .d = 3.14 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			static constexpr jsonifier::parse_options minifiedOpts{ .partialRead = partial, .knownOrder = knownOrder, .minified = true };
			parser.parseJson<minifiedOpts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.str);
		};

		static constexpr auto test_partial_with_validation = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { { 4, 5, 6 } }, .str = "validated", .i = 11, .d = 2.5 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.str, parsed.arr[1]);
		};

		static constexpr auto test_basic_reflection = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { 1, 2, 3 }, .str = "Hello", .i = 42, .d = 3.14 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct obj2{};
			parser.parseJson<opts>(obj2, json);
			printErrors(parser);
			return std::make_tuple(json, obj2.i, obj2.d, obj2.str, obj2.arr[0]);
		};

		static constexpr auto test_meta_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			MetaStruct obj{ .name = "Gadget", .count = 5 };
			std::string json{};
			parser.serializeJson(obj, json);
			MetaStruct obj2{};
			std::string input = R"({"cnt":10,"label":"Widget"})";
			parser.parseJson<opts>(obj2, input);
			printErrors(parser);
			return std::make_tuple(json, obj2.count, obj2.name);
		};

		static constexpr auto test_optional_fields = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			std::string input = R"({"required":"changed","maybe":3.1415})";
			parser.parseJson<opts>(obj, input);
			printErrors(parser);
			obj.maybe.reset();
			json.clear();
			parser.serializeJson(obj, json);
			return obj.required == std::string{ "changed" } && !obj.maybe.has_value() && (json.find("maybe") == std::string::npos || json.find("null") != std::string::npos);
		};

		static constexpr auto test_enum_as_integer = []() {
			jsonifier::jsonifier_core<> parser{};
			EnumHolder obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			EnumHolder parsed{};
			std::string input = R"({"c":2})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return std::make_tuple(json, parsed.c == Color::Blue);
		};

		static constexpr auto test_enum_map_key = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, int32_t> obj{ { "one", 1 } };
			std::string json{};
			parser.serializeJson(obj, json);
			std::map<std::string, int32_t> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed["one"] == 1;
		};

		static constexpr auto test_containers = []() {
			jsonifier::jsonifier_core<> parser{};
			ContainerStruct c{};
			std::string json{};
			parser.serializeJson(c, json);
			ContainerStruct c2{};
			parser.parseJson<opts>(c2, json);
			printErrors(parser);
			return std::make_tuple(c2.vec == std::vector<int32_t>{ 1, 2, 3 }, c2.arr[0], std::get<2>(c2.tup));
		};

		static constexpr auto test_map_unordered = []() {
			jsonifier::jsonifier_core<> parser{};
			MapStruct ms{};
			std::string json{};
			parser.serializeJson(ms, json);
			MapStruct ms2{};
			parser.parseJson<opts>(ms2, json);
			printErrors(parser);
			return (json.find("one") != std::string::npos) && ms2.str_map["one"] == 1;
		};

		static constexpr auto test_prettify = []() {
			jsonifier::jsonifier_core<> parser{};
			PrettifyStruct pd{ .msg = "Hello", .id = 123 };
			std::string json{};
			parser.serializeJson(pd, json);
			std::string pretty{};
			parser.prettifyJson(json, pretty);
			std::string minified{};
			parser.minifyJson(pretty, minified);
			return (pretty.find('\n') != std::string::npos) && minified == json;
		};

		static constexpr auto test_minify = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string prettified = "{\n    \"id\": 42,\n    \"msg\": \"test\"\n}";
			std::string minified{};
			parser.minifyJson(prettified, minified);
			return minified.find("\n") == std::string::npos && minified.size() < prettified.size();
		};

		static constexpr auto test_validate_valid = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,"str":"Hello","arr":[1,2,3]})";
			return parser.validateJson(json);
		};

		static constexpr auto test_validate_invalid = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,})";
			return !parser.validateJson(json);
		};


		static constexpr auto test_skip_string_escaped_quote = []() {
			jsonifier::jsonifier_core<> parser{};
			// Exactly the input, no terminator, so AddressSanitizer reports any read past it.
			auto parseExact = [&](std::string_view json, simple_struct& value) {
				std::vector<char> buffer(json.begin(), json.end());
				return parser.parseJson<jsonifier::parse_options{ .nullTerminated = false }>(value, std::string_view{ buffer.data(), buffer.size() });
			};
			simple_struct value{};
			// Unknown key whose string value is cut off after an escaped quote.
			for (std::string_view json: { R"({"note":"\"})", R"({"note":"a\"b)" }) {
				if (parseExact(json, value)) {
					return false;
				}
			}
			// Escaped quotes inside a skipped value still end at the right quote.
			if (!parseExact(R"({"note":"a\"b\"c","id":7})", value)) {
				return false;
			}
			return value.id == 7;
		};

		static constexpr auto test_validate_truncated_literals = []() {
			jsonifier::jsonifier_core<> parser{};
			// Each input sits in a heap buffer of exactly its size plus the null
			// terminator, so AddressSanitizer reports any read past the end.
			for (std::string_view json: { "[t", "[f", "[n", "[tru", "[fals", "[nul", "t", "f", "n" }) {
				std::vector<char> buffer(json.size() + 1);
				std::copy(json.begin(), json.end(), buffer.begin());
				if (parser.validateJson(std::string_view{ buffer.data(), json.size() })) {
					return false;
				}
			}
			// A '0' followed by a byte >= 0x80 used to index numberTable with a
			// negative offset; only the absence of a sanitizer report is checked.
			static_cast<void>(parser.validateJson(std::string_view{ "[0\xE5]" }));
			return true;
		};

		static constexpr auto test_validate_strict_scalars = []() {
			jsonifier::jsonifier_core<> parser{};
			// Each input sits in a heap buffer of exactly its size plus the null terminator.
			auto validate = [&](std::string_view json) {
				std::vector<char> buffer(json.size() + 1);
				std::copy(json.begin(), json.end(), buffer.begin());
				return parser.validateJson(std::string_view{ buffer.data(), json.size() });
			};
			for (std::string_view json:
				{ "42", "-0", " 1.5e+10 ", "\"a\"", "\"\"", "true", "null", "[0,-0.0,1E5,1e-5,123.456e78]", "[\"\\u00e9\",\"\xC3\xA9\"]", "{\"a\" : 1 , \"b\" : [ ] }" }) {
				if (!validate(json)) {
					return false;
				}
			}
			for (std::string_view json: { "[-]", "[+1]", "[1+2]", "[0x42]", "[01]", "[-01]", "[1.]", "[.5]", "[1e]", "[1e+]", "[--1]", "[1.5.5]", "[1ee5]", "[truex]", "[nullnull]",
					 "[-Infinity]", "[\"\\x\"]", "[\"a\x01\"]", "[\"\xFF\"]", "[\"\xC3\"]", "[\"\xED\xA0\x80\"]", "[\"\xC0\xAF\"]", "\"a\" \"b\"", "1 2", "01", "-" }) {
				if (validate(json)) {
					return false;
				}
			}
			return true;
		};

		static constexpr auto test_escaped_member_keys = []() {
			jsonifier::jsonifier_core<> parser{};
			escaped_keys value{ 1, 2, 3 };
			std::string json{};
			parser.serializeJson(value, json);
			if (json != R"({"say \"hi\"":1,"C:\\dir":2,"\\\"":3})" || !parser.validateJson(json)) {
				return false;
			}
			// Keys out of declaration order go through the hash lookup instead of the in-order fast path.
			escaped_keys parsed{};
			if (!parser.parseJson(parsed, std::string{ R"({"\\\"":3,"C:\\dir":2,"say \"hi\"":1})" })) {
				return false;
			}
			return parsed.quote == 1 && parsed.backslash == 2 && parsed.both == 3;
		};

		static constexpr auto test_integer_truncated_fraction = []() {
			jsonifier::jsonifier_core<> parser{};
			// Exactly the input, no terminator, so AddressSanitizer reports any read past it.
			auto parseExact = [&](std::string_view json, auto& value) {
				std::vector<char> buffer(json.begin(), json.end());
				return parser.parseJson<jsonifier::parse_options{ .nullTerminated = false }>(value, std::string_view{ buffer.data(), buffer.size() });
			};
			// An integer whose fraction or exponent runs into the end of the input.
			for (std::string_view json: { "[1.", "[1.5", "[1e", "[1e5", "[1.5e", "[1.5e1", "[1.5e+" }) {
				std::vector<int64_t> signedValues{};
				std::vector<uint64_t> unsignedValues{};
				if (parseExact(json, signedValues) || parseExact(json, unsignedValues)) {
					return false;
				}
			}
			std::vector<int64_t> signedValues{};
			std::vector<uint64_t> unsignedValues{};
			return parseExact("[1.5e1,2e1]", signedValues) && parseExact("[1.5e1,2e1]", unsignedValues) && signedValues == std::vector<int64_t>{ 15, 20 } &&
				unsignedValues == std::vector<uint64_t>{ 15, 20 };
		};

		static constexpr auto test_float_precision = []() {
			jsonifier::jsonifier_core<> parser{};
			FloatPrecision fp{};
			std::string json{};
			parser.serializeJson(fp, json);
			return json.find("3.14159") != std::string::npos;
		};

		static constexpr auto test_nested_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			NestedStruct ns{};
			ns.inner = { .arr = { { 1, 2, 3 } }, .str = "nested", .i = 42, .d = 3.14 };
			ns.nums	 = { 10, 20, 30 };
			std::string json{};
			parser.serializeJson(ns, json);
			NestedStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.i, parsed.nums.size());
		};

		static constexpr auto test_shared_ptr = []() {
			jsonifier::jsonifier_core<> parser{};
			SharedPtrStruct sps{};
			sps.ptr		 = std::make_shared<BasicStruct>();
			sps.ptr->i	 = 99;
			sps.ptr->str = "shared";
			std::string json{};
			parser.serializeJson(sps, json);
			SharedPtrStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.ptr && parsed.ptr->i == 99 && parsed.ptr->str == std::string{ "shared" };
		};

		static constexpr auto test_vector_of_structs = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<BasicStruct> vec{ { .arr = { { 1, 2, 3 } }, .str = "a", .i = 1, .d = 1.1 }, { .arr = { { 4, 5, 6 } }, .str = "b", .i = 2, .d = 2.2 } };
			std::string json{};
			parser.serializeJson(vec, json);
			std::vector<BasicStruct> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.size(), parsed[0].i, parsed[1].str);
		};

		static constexpr auto test_array_of_enums = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<Color, 3> arr{ Color::Red, Color::Green, Color::Blue };
			std::string json{};
			parser.serializeJson(arr, json);
			jsonifier::internal::array<Color, 3> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed[0] == Color::Red, parsed[2] == Color::Blue);
		};

		static constexpr auto test_optional_with_value = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional obj{ "test", 5.5 };
			std::string json{};
			parser.serializeJson(obj, json);
			WithOptional parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.maybe.has_value() && *parsed.maybe == 5.5;
		};

		static constexpr auto test_optional_without_value = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional obj{ "test", std::nullopt };
			std::string json{};
			parser.serializeJson(obj, json);
			WithOptional parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return !parsed.maybe.has_value();
		};

		static constexpr auto test_empty_containers = []() {
			jsonifier::jsonifier_core<> parser{};
			ContainerStruct c{};
			c.vec.clear();
			c.arr = { { "", "" } };
			std::string json{};
			parser.serializeJson(c, json);
			ContainerStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.vec.empty();
		};

		static constexpr auto test_large_numbers = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max() - 1, std::numeric_limits<uint32_t>::max() - 2 },
				.str			  = "max",
				.i				  = 2147483647,
				.d				  = 1.7976931348623157e+308 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.arr[0]);
		};

		static constexpr auto test_special_string_chars = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { { 1, 2, 3 } }, .str = "test\"quote\\slash\nnewline", .i = 1, .d = 1.0 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("\"") != std::string::npos && parsed.str.find("\n") != std::string::npos;
		};

		static constexpr auto test_unicode_string = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ .arr = { { 1, 2, 3 } }, .str = "Hello 世界 🌍", .i = 1, .d = 1.0 };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("世界") != std::string::npos;
		};

		static constexpr auto test_tuple_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			std::tuple<int32_t, double, std::string> tup{ 123, 4.56, "test" };
			std::string json{};
			parser.serializeJson(tup, json);
			std::tuple<int32_t, double, std::string> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(std::get<0>(parsed), std::get<2>(parsed));
		};

		static constexpr auto test_nested_maps = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, std::map<std::string, int32_t>> nested{ { "outer1", { { "inner1", 1 }, { "inner2", 2 } } }, { "outer2", { { "inner3", 3 } } } };
			std::string json{};
			parser.serializeJson(nested, json);
			std::map<std::string, std::map<std::string, int32_t>> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed["outer1"]["inner1"], parsed["outer2"]["inner3"]);
		};

		static constexpr auto test_vector_of_vectors = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<std::vector<int32_t>> vec{ { 1, 2 }, { 3, 4, 5 } };
			std::string json{};
			parser.serializeJson(vec, json);
			std::vector<std::vector<int32_t>> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.size(), parsed[1].size(), parsed[1][2]);
		};

		static constexpr auto test_char_empty = []() {
			jsonifier::jsonifier_core<> parser{};
			char_roundtrip deserialized{ .uchar_val = 'b', .int_val = 1, .char_val = 'a' };
			std::string buffer{};
			parser.serializeJson(deserialized, buffer);
			parser.parseJson<opts>(deserialized, buffer);
			printErrors(parser);
			return std::make_tuple(deserialized.char_val, deserialized.uchar_val, deserialized.int_val);
		};

		static constexpr auto test_basic_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct obj{ .name = "test", .value = 3.14, .id = 42 };
			std::string result{};
			parser.serializeJson(obj, result);
			return !result.empty() && result.find("42") != std::string::npos;
		};

		static constexpr auto test_basic_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"id":42,"name":"test","value":3.14})";
			simple_struct obj{};
			parser.parseJson<opts>(obj, json);
			printErrors(parser);
			return std::make_tuple(obj.id, obj.name);
		};

		static constexpr auto test_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct original{ .name = "roundtrip", .value = 2.71828, .id = 99 };
			std::string serialized{};
			parser.serializeJson(original, serialized);
			simple_struct parsed{};
			parser.parseJson<opts>(parsed, serialized);
			printErrors(parser);
			return std::make_tuple(parsed.id, parsed.name);
		};

		static constexpr auto test_nested = []() {
			jsonifier::jsonifier_core<> parser{};
			nested_struct obj{};
			obj.inner	= { .name = "nested", .value = 1.5, .id = 1 };
			obj.numbers = { 1, 2, 3, 4, 5 };
			obj.flag	= true;
			std::string json{};
			parser.serializeJson(obj, json);
			nested_struct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.id, parsed.numbers.size(), parsed.flag);
		};

		static constexpr auto test_double_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(3.14, buffer);
			std::string a = buffer;
			parser.serializeJson(0.0, buffer);
			std::string b = buffer;
			parser.serializeJson(-0.0, buffer);
			std::string c = buffer;
			return std::make_tuple(a, b, c);
		};

		static constexpr auto test_double_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			double num{};
			parser.parseJson<opts>(num, "3.14");
			printErrors(parser);
			double a = num;
			parser.parseJson<opts>(num, "9.81");
			printErrors(parser);
			double b = num;
			parser.parseJson<opts>(num, "0");
			printErrors(parser);
			double c = num;
			return std::make_tuple(a, b, c);
		};

		static constexpr auto test_int_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(0, buffer);
			std::string a = buffer;
			parser.serializeJson(999, buffer);
			std::string b = buffer;
			parser.serializeJson(-6, buffer);
			std::string c = buffer;
			return std::make_tuple(a, b, c);
		};

		static constexpr auto test_int_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			int32_t num{};
			parser.parseJson<opts>(num, "-1");
			printErrors(parser);
			int32_t a = num;
			parser.parseJson<opts>(num, "0");
			printErrors(parser);
			int32_t b = num;
			parser.parseJson<opts>(num, "999");
			printErrors(parser);
			int32_t c = num;
			return std::make_tuple(a, b, c);
		};

		static constexpr auto test_bool_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(true, buffer);
			std::string a = buffer;
			parser.serializeJson(false, buffer);
			std::string b = buffer;
			return std::make_tuple(a, b);
		};

		static constexpr auto test_bool_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			bool val{};
			parser.parseJson<opts>(val, "true");
			printErrors(parser);
			bool a = val;
			parser.parseJson<opts>(val, "false");
			printErrors(parser);
			bool b = val;
			return std::make_tuple(a, b);
		};

		static constexpr auto test_string_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(std::string{ "fish" }, buffer);
			std::string a = buffer;
			parser.serializeJson(std::string{ "as\"df\\ghjkl" }, buffer);
			std::string b = buffer;
			return std::make_tuple(a, b);
		};

		static constexpr auto test_string_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string val{};
			parser.parseJson<opts>(val, "\"fish\"");
			printErrors(parser);
			std::string a = val;
			parser.parseJson<opts>(val, "\"as\\\"df\\\\ghjkl\"");
			printErrors(parser);
			std::string b = val;
			return std::make_tuple(a, b);
		};

		static constexpr auto test_vector_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<int32_t> vec{ 1, 2, 3, 4, 5 };
			std::string json{};
			parser.serializeJson(vec, json);
			return json.find("[") != std::string::npos && json.find("]") != std::string::npos;
		};

		static constexpr auto test_vector_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "[10,20,30,40,50]";
			std::vector<int32_t> vec{};
			parser.parseJson<opts>(vec, json);
			printErrors(parser);
			return std::make_tuple(vec.size(), vec[0], vec[4]);
		};

		static constexpr auto test_array_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<int32_t, 3> arr{ 1, 2, 3 };
			std::string json{};
			parser.serializeJson(arr, json);
			return json.find("[") != std::string::npos && json.find("]") != std::string::npos;
		};

		static constexpr auto test_array_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "[10,20,30]";
			jsonifier::internal::array<int32_t, 3> arr{};
			parser.parseJson<opts>(arr, json);
			printErrors(parser);
			return std::make_tuple(arr.size(), arr[0], arr[2]);
		};

		static constexpr auto test_escaped_chars_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"escaped\"key":0,"escaped\"\"key2":"hi","escape_chars":"\b\f\n\r\t"})";
			escaped_struct obj{};
			parser.parseJson<opts>(obj, json);
			printErrors(parser);
			return obj.escape_chars;
		};

		static constexpr auto test_enum_serialize = []() -> std::string {
			jsonifier::jsonifier_core<> parser{};
			Color color = Color::Green;
			std::string json{};
			parser.serializeJson(color, json);
			return json;
		};

		static constexpr auto test_enum_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "0";
			Color color{};
			parser.parseJson<opts>(color, json);
			printErrors(parser);
			return color == Color::Red;
		};

		static constexpr auto test_enum_array = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<Color, 3> arr{};
			std::string json = "[1,0,2]";
			parser.parseJson<opts>(arr, json);
			printErrors(parser);
			return std::make_tuple(arr[0] == Color::Green, arr[1] == Color::Red, arr[2] == Color::Blue);
		};

		static constexpr auto test_vehicle_enum = []() {
			jsonifier::jsonifier_core<> parser{};
			Vehicle vehicle = Vehicle::Plane;
			std::string json{};
			parser.serializeJson(vehicle, json);
			Vehicle parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(json, parsed == Vehicle::Plane);
		};

		static constexpr auto test_complex_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.c);
		};

		static constexpr auto test_optional_empty = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			obj.optional = std::nullopt;
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return !parsed.optional.has_value();
		};

		static constexpr auto test_optional_value = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			obj.optional = V3{ 1.0, 2.0, 3.0 };
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.optional.has_value() && parsed.optional->x == 1.0;
		};

		static constexpr auto test_map = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, int32_t> map{ { "a", 4 }, { "f", 7 }, { "b", 12 } };
			std::string json{};
			parser.serializeJson(map, json);
			std::map<std::string, int32_t> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed["a"], parsed["f"], parsed["b"]);
		};

		static constexpr auto test_dummy_data = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<dummy_data> test_data = { { .b = TestData::None, .c = TestData::None, .d = TestData::None, .e = TestData::None, .id = 0, .a = 0, .f = 0 },
				{ .b = TestData::A, .c = TestData::B, .d = TestData::A, .e = TestData::B, .id = 1, .a = 1, .f = 0xDDDDDDDD },
				{ .b = TestData::A, .c = TestData::B, .d = TestData::C, .e = TestData::D, .id = 2, .a = 6, .f = 0xEEEEEEEE },
				{ .b = TestData::ERROR_E, .c = TestData::ERROR_E, .d = TestData::ERROR_E, .e = TestData::ERROR_E, .id = 3, .a = -1, .f = 0xFFFFFFFF } };
			std::string json{};
			parser.serializeJson(test_data, json);
			std::vector<dummy_data> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.size(), parsed[1].b == TestData::A);
		};

		std::cout << "Unit Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;

		rt_ut::unit_test<"Partial Basic", true>::assert_eq(std::make_tuple(42, 3.14, std::string{ "Hello" }, 1u), test_partial_basic);
		rt_ut::unit_test<"Partial Roundtrip", true>::assert_eq(std::make_tuple(99, std::string{ "roundtrip" }, 2.71828), test_partial_roundtrip);
		rt_ut::unit_test<"Partial Nested", true>::assert_eq(std::make_tuple(7, std::string{ "deep" }, uint64_t{ 8 }, true), test_partial_nested);
		rt_ut::unit_test<"Partial Meta Renamed", true>::assert_eq(std::make_tuple(10, std::string{ "Widget" }), test_partial_meta_renamed);
		rt_ut::unit_test<"Partial Optional Present", true>::assert_eq(true, test_partial_optional_present);
		rt_ut::unit_test<"Partial Optional Absent", true>::assert_eq(true, test_partial_optional_absent);
		rt_ut::unit_test<"Partial containers", true>::assert_eq(std::make_tuple(true, std::string{ "Hello" }, std::string{ "pi?" }), test_partial_containers);
		rt_ut::unit_test<"Partial Map", true>::assert_eq(true, test_partial_map);
		rt_ut::unit_test<"Partial Vector of Structs", true>::assert_eq(std::make_tuple(uint64_t{ 3 }, 1, std::string{ "b" }, 9u), test_partial_vector_of_structs);
		rt_ut::unit_test<"Partial Complex Thing", true>::assert_eq(std::make_tuple(8, 2.0, 'W', uint64_t{ 4 }, std::string{ "as\"df\\ghjkl" }), test_partial_complex_thing);
		rt_ut::unit_test<"Partial Unicode", true>::assert_eq(true, test_partial_unicode);
		rt_ut::unit_test<"Partial Special Chars", true>::assert_eq(true, test_partial_special_chars);
		rt_ut::unit_test<"Partial Large Payload", true>::assert_eq(std::make_tuple(uint64_t{ 64 }, 0, 63, std::string{ "item_32" }), test_partial_large_payload);
		rt_ut::unit_test<"Partial Boundary Lengths", true>::assert_eq(true, test_partial_boundary_lengths);
		rt_ut::unit_test<"Partial Nested Struct Vec", true>::assert_eq(std::make_tuple(42, std::string{ "nested" }, uint64_t{ 5 }, 50), test_partial_nested_struct_vec);
		rt_ut::unit_test<"Partial Minified", true>::assert_eq(std::make_tuple(42, std::string{ "minified" }), test_partial_minified);
		rt_ut::unit_test<"Partial With Validation", true>::assert_eq(std::make_tuple(11, std::string{ "validated" }, 5u), test_partial_with_validation);
		rt_ut::unit_test<"Basic Reflection", true>::assert_eq(
			std::make_tuple(std::string{ R"({"i":42,"d":3.14,"str":"Hello","arr":[1,2,3]})" }, 42, 3.14, std::string{ "Hello" }, 1u), test_basic_reflection);
		rt_ut::unit_test<"Meta Struct Renamed Fields", true>::assert_eq(std::make_tuple(std::string{ R"({"cnt":5,"label":"Gadget"})" }, 10, std::string{ "Widget" }),
			test_meta_struct);
		rt_ut::unit_test<"Optional Fields", true>::assert_eq(true, test_optional_fields);
		rt_ut::unit_test<"Enum as Integer", true>::assert_eq(std::make_tuple(std::string{ R"({"c":1})" }, true), test_enum_as_integer);
		rt_ut::unit_test<"Enum Map Key", true>::assert_eq(true, test_enum_map_key);
		rt_ut::unit_test<"containers", true>::assert_eq(std::make_tuple(true, std::string{ "Hello" }, std::string{ "pi?" }), test_containers);
		rt_ut::unit_test<"Map and Unordered Map", true>::assert_eq(true, test_map_unordered);
		rt_ut::unit_test<"Prettify", true>::assert_eq(true, test_prettify);
		rt_ut::unit_test<"Minify", true>::assert_eq(true, test_minify);
		rt_ut::unit_test<"Validate Valid", true>::assert_eq(true, test_validate_valid);
		rt_ut::unit_test<"Validate Invalid", true>::assert_eq(true, test_validate_invalid);
		rt_ut::unit_test<"Skip String Escaped Quote", true>::assert_eq(true, test_skip_string_escaped_quote);
		rt_ut::unit_test<"Validate Truncated Literals", true>::assert_eq(true, test_validate_truncated_literals);
		rt_ut::unit_test<"Validate Strict Scalars", true>::assert_eq(true, test_validate_strict_scalars);
		rt_ut::unit_test<"Escaped Member Keys", true>::assert_eq(true, test_escaped_member_keys);
		rt_ut::unit_test<"Integer Truncated Fraction", true>::assert_eq(true, test_integer_truncated_fraction);
		rt_ut::unit_test<"Float Precision", true>::assert_eq(true, test_float_precision);
		rt_ut::unit_test<"Nested Struct", true>::assert_eq(std::make_tuple(42, uint64_t{ 3 }), test_nested_struct);
		rt_ut::unit_test<"Shared Ptr", true>::assert_eq(true, test_shared_ptr);
		rt_ut::unit_test<"Vector of Structs", true>::assert_eq(std::make_tuple(uint64_t{ 2 }, 1, std::string{ "b" }), test_vector_of_structs);
		rt_ut::unit_test<"Array of Enums", true>::assert_eq(std::make_tuple(true, true), test_array_of_enums);
		rt_ut::unit_test<"Optional With Value", true>::assert_eq(true, test_optional_with_value);
		rt_ut::unit_test<"Optional Without Value", true>::assert_eq(true, test_optional_without_value);
		rt_ut::unit_test<"Empty containers", true>::assert_eq(true, test_empty_containers);
		rt_ut::unit_test<"Large Numbers", true>::assert_eq(std::make_tuple(2147483647, std::numeric_limits<uint32_t>::max()), test_large_numbers);
		rt_ut::unit_test<"Special String Chars", true>::assert_eq(true, test_special_string_chars);
		rt_ut::unit_test<"Unicode String", true>::assert_eq(true, test_unicode_string);
		rt_ut::unit_test<"Tuple Roundtrip", true>::assert_eq(std::make_tuple(123, std::string{ "test" }), test_tuple_roundtrip);
		rt_ut::unit_test<"Nested Maps", true>::assert_eq(std::make_tuple(1, 3), test_nested_maps);
		rt_ut::unit_test<"Vector of Vectors", true>::assert_eq(std::make_tuple(uint64_t{ 2 }, uint64_t{ 3 }, 5), test_vector_of_vectors);
		rt_ut::unit_test<"Char Empty String", true>::assert_eq(std::make_tuple(char{ 'a' }, static_cast<uint8_t>('b'), 1), test_char_empty);
		rt_ut::unit_test<"Basic Serialize", true>::assert_eq(true, test_basic_serialize);
		rt_ut::unit_test<"Basic Parse", true>::assert_eq(std::make_tuple(42, std::string{ "test" }), test_basic_parse);
		rt_ut::unit_test<"Roundtrip", true>::assert_eq(std::make_tuple(99, std::string{ "roundtrip" }), test_roundtrip);
		rt_ut::unit_test<"Nested Structures", true>::assert_eq(std::make_tuple(1, uint64_t{ 5 }, true), test_nested);
		rt_ut::unit_test<"Double Write", true>::assert_eq(std::make_tuple(std::string{ "3.14" }, std::string{ "0" }, std::string{ "-0" }), test_double_write);
		rt_ut::unit_test<"Double Parse", true>::assert_eq(std::make_tuple(3.14, 9.81, 0.0), test_double_parse);
		rt_ut::unit_test<"Int Write", true>::assert_eq(std::make_tuple(std::string{ "0" }, std::string{ "999" }, std::string{ "-6" }), test_int_write);
		rt_ut::unit_test<"Int Parse", true>::assert_eq(std::make_tuple(-1, 0, 999), test_int_parse);
		rt_ut::unit_test<"Bool Write", true>::assert_eq(std::make_tuple(std::string{ "true" }, std::string{ "false" }), test_bool_write);
		rt_ut::unit_test<"Bool Parse", true>::assert_eq(std::make_tuple(true, false), test_bool_parse);
		rt_ut::unit_test<"String Write", true>::assert_eq(std::make_tuple(std::string{ "\"fish\"" }, std::string{ "\"as\\\"df\\\\ghjkl\"" }), test_string_write);
		rt_ut::unit_test<"String Parse", true>::assert_eq(std::make_tuple(std::string{ "fish" }, std::string{ "as\"df\\ghjkl" }), test_string_parse);
		rt_ut::unit_test<"Vector Serialize", true>::assert_eq(true, test_vector_serialize);
		rt_ut::unit_test<"Vector Parse", true>::assert_eq(std::make_tuple(uint64_t{ 5 }, 10, 50), test_vector_parse);
		rt_ut::unit_test<"Array Serialize", true>::assert_eq(true, test_array_serialize);
		rt_ut::unit_test<"Array Parse", true>::assert_eq(std::make_tuple(uint64_t{ 3 }, 10, 30), test_array_parse);
		rt_ut::unit_test<"Escaped Chars Parse", true>::assert_eq(std::string{ "\b\f\n\r\t" }, test_escaped_chars_parse);
		rt_ut::unit_test<"Enum Serialize", true>::assert_eq(std::string{ "1" }, test_enum_serialize);
		rt_ut::unit_test<"Enum Parse", true>::assert_eq(true, test_enum_parse);
		rt_ut::unit_test<"Enum Array", true>::assert_eq(std::make_tuple(true, true, true), test_enum_array);
		rt_ut::unit_test<"Vehicle Enum", true>::assert_eq(std::make_tuple(std::string{ "2" }, true), test_vehicle_enum);
		rt_ut::unit_test<"Complex Struct", true>::assert_eq(std::make_tuple(8, 2.0, 'W'), test_complex_struct);
		rt_ut::unit_test<"Optional Empty", true>::assert_eq(true, test_optional_empty);
		rt_ut::unit_test<"Optional Value", true>::assert_eq(true, test_optional_value);
		rt_ut::unit_test<"Map", true>::assert_eq(std::make_tuple(4, 7, 12), test_map);
		rt_ut::unit_test<"Dummy Data Vector", true>::assert_eq(std::make_tuple(uint64_t{ 4 }, true), test_dummy_data);
	}

	inline static void runTests() {
		unitTestsImpl<false, false, false>();
		unitTestsImpl<false, true, false>();
		unitTestsImpl<true, false, false>();
		unitTestsImpl<true, true, false>();
		unitTestsImpl<false, false, true>();
		unitTestsImpl<false, true, true>();
		unitTestsImpl<true, false, true>();
		unitTestsImpl<true, true, true>();
		std::cout << "Unit test validation tests complete." << std::endl;
	}

}
