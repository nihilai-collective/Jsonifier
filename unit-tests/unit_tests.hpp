// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/unit_tests.hpp
#pragma once

#include "common.hpp"

enum class Color : uint8_t { Red, Green, Blue };

struct simple_struct {
	int32_t id{};
	std::string name{};
	double value{};
};

template<> struct jsonifier::core<simple_struct> {
	using value_type				 = simple_struct;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::name, &value_type::value>();
};

struct char_roundtrip {
	char char_val{};
	uint8_t uchar_val{};
	int32_t int_val{};
};

template<> struct jsonifier::core<char_roundtrip> {
	using value_type				 = char_roundtrip;
	static constexpr auto parseValue = createValue<&value_type::char_val, &value_type::uchar_val, &value_type::int_val>();
};

struct sub_thing {
	double a{ 3.14 };
	std::string b{ "stuff" };
};

template<> struct jsonifier::core<sub_thing> {
	using value_type				 = sub_thing;
	static constexpr auto parseValue = createValue<&value_type::a, &value_type::b>();
};

struct sub_thing2 {
	double a{ 3.14 };
	std::string b{ "stuff" };
	double c{ 999.342494903 };
	double d{ 0.000000000001 };
	double e{ 203082348402.1 };
	float f{ 89.089f };
	double g{ 12380.00000013 };
	double h{ 1000000.000001 };
};

template<> struct jsonifier::core<sub_thing2> {
	using value_type = sub_thing2;
	static constexpr auto parseValue =
		createValue<&value_type::a, &value_type::b, &value_type::c, &value_type::d, &value_type::e, &value_type::f, &value_type::g, &value_type::h>();
};

struct V3 {
	double x{ 3.14 };
	double y{ 2.7 };
	double z{ 6.5 };

	bool operator==(const V3& rhs) const {
		return (std::equal_to<double>{}(x, rhs.x) && std::equal_to<double>{}(y, rhs.y) && std::equal_to<double>{}(z, rhs.z));
	}
};

template<> struct jsonifier::core<V3> {
	using value_type				 = V3;
	static constexpr auto parseValue = createValue<&value_type::x, &value_type::y, &value_type::z>();
};

struct nested_struct {
	simple_struct inner{};
	std::vector<int32_t> numbers{};
	bool flag{};
};

template<> struct jsonifier::core<nested_struct> {
	using value_type				 = nested_struct;
	static constexpr auto parseValue = createValue<&value_type::inner, &value_type::numbers, &value_type::flag>();
};

struct Thing {
	sub_thing thing{};
	jsonifier::internal::array<sub_thing2, 1> thing2array{};
	V3 vec3{};
	std::vector<int32_t> numbers{ 6, 7, 8, 2 };
	jsonifier::internal::array<std::string, 4> array{ { "as\"df\\ghjkl", "pie", "42", "foo" } };
	std::vector<V3> vector{ { 9.0, 6.7, 3.1 }, {} };
	int32_t i{ 8 };
	double d{ 2.0 };
	bool b{};
	char c{ 'W' };
	Color color{ Color::Green };
	std::vector<bool> vb{ true, false, false, true, true, true, true };
	std::shared_ptr<sub_thing> sptr = std::make_shared<sub_thing>();
	std::optional<V3> optional{};
	std::vector<double> doubles{ 9.0, 6.7, 3.1 };
	std::map<std::string, int32_t> map{ { "a", 4 }, { "f", 7 }, { "b", 12 } };
};

template<> struct jsonifier::core<Thing> {
	using value_type = Thing;
	static constexpr auto parseValue =
		createValue<&value_type::thing, &value_type::thing2array, &value_type::vec3, &value_type::numbers, &value_type::doubles, &value_type::vector, &value_type::i,
			&value_type::d, &value_type::b, &value_type::c, &value_type::color, &value_type::vb, &value_type::sptr, &value_type::optional, &value_type::array, &value_type::map>();
};

struct escaped_struct {
	int32_t escaped_key{};
	std::string escaped_key2{ "hi" };
	std::string escape_chars{};
};

template<> struct jsonifier::core<escaped_struct> {
	using value_type = escaped_struct;
	static constexpr auto parseValue =
		createValue<makeJsonEntity<&value_type::escaped_key, "escaped\"key">(), makeJsonEntity<&value_type::escaped_key2, "escaped\"\"key2">(), &value_type::escape_chars>();
};

enum class Vehicle : uint32_t { Car, Truck, Plane };

enum class TestData : uint8_t { None, A, B, C, D, ERROR_E = 0xFF };

struct dummy_data {
	uint32_t id{ 0 };
	int32_t a{ 0 };
	TestData b{ TestData::None };
	TestData c{ TestData::None };
	TestData d{ TestData::None };
	TestData e{ TestData::None };
	int64_t f{ 0 };
};

template<> struct jsonifier::core<dummy_data> {
	using value_type				 = dummy_data;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::a, &value_type::b, &value_type::c, &value_type::d, &value_type::e, &value_type::f>();
};

struct BasicStruct {
	int32_t i{};
	double d{};
	std::string str{};
	std::array<uint32_t, 3> arr{};
};

template<> struct jsonifier::core<BasicStruct> {
	using value_type				 = BasicStruct;
	static constexpr auto parseValue = createValue<&value_type::i, &value_type::d, &value_type::str, &value_type::arr>();
};

struct MetaStruct {
	int32_t count{};
	std::string name{};
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
	std::vector<int32_t> vec{ 1, 2, 3 };
	jsonifier::internal::array<std::string, 2> arr{ { "Hello", "World" } };
	std::tuple<int32_t, double, std::string> tup{ 42, 2.718, "pi?" };
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
	int32_t id{};
	std::string msg{};
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
	BasicStruct inner{};
	std::vector<int32_t> nums{};
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
		std::cout << error << std::endl;
	}
}

namespace unit_tests {

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void unitTestsImpl() {
		static constexpr jsonifier::parse_options opts{ .partialRead = partial, .knownOrder = knownOrder, .validateUtf8 = true, .nullTerminated = nullTerminated };

		auto test_partial_basic = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,"str":"Hello","arr":[1,2,3]})";
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.str, parsed.arr[0]);
		};

		auto test_partial_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct original{ 99, "roundtrip", 2.71828 };
			std::string serialized{};
			parser.serializeJson(original, serialized);
			simple_struct parsed{};
			parser.parseJson<opts>(parsed, serialized);
			printErrors(parser);
			return std::make_tuple(parsed.id, parsed.name, parsed.value);
		};

		auto test_partial_nested = []() {
			jsonifier::jsonifier_core<> parser{};
			nested_struct obj{};
			obj.inner	= { 7, "deep", 9.5 };
			obj.numbers = { 1, 2, 3, 4, 5, 6, 7, 8 };
			obj.flag	= true;
			std::string json{};
			parser.serializeJson(obj, json);
			nested_struct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.id, parsed.inner.name, parsed.numbers.size(), parsed.flag);
		};

		auto test_partial_meta_renamed = []() {
			jsonifier::jsonifier_core<> parser{};
			MetaStruct parsed{};
			std::string input = R"({"cnt":10,"label":"Widget"})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return std::make_tuple(parsed.count, parsed.name);
		};

		auto test_partial_optional_present = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional parsed{};
			std::string input = R"({"required":"changed","maybe":3.1415})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return parsed.required == std::string{ "changed" } && parsed.maybe.has_value() && std::equal_to<double>{}(*parsed.maybe, 3.1415);
		};

		auto test_partial_optional_absent = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional parsed{};
			std::string input = R"({"required":"only"})";
			parser.parseJson<opts>(parsed, input);
			printErrors(parser);
			return parsed.required == std::string{ "only" } && !parsed.maybe.has_value();
		};

		auto test_partial_containers = []() {
			jsonifier::jsonifier_core<> parser{};
			ContainerStruct c{};
			std::string json{};
			parser.serializeJson(c, json);
			ContainerStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.vec == std::vector<int32_t>{ 1, 2, 3 }, parsed.arr[0], std::get<2>(parsed.tup));
		};

		auto test_partial_map = []() {
			jsonifier::jsonifier_core<> parser{};
			MapStruct original{};
			std::string json{};
			parser.serializeJson(original, json);
			MapStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str_map["one"] == 1 && parsed.str_map["two"] == 2;
		};

		auto test_partial_vector_of_structs = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStructVec original{};
			original.items = { { { 1, 1.1, "a", { { 1, 2, 3 } } }, { 2, 2.2, "b", { { 4, 5, 6 } } }, { 3, 3.3, "c", { { 7, 8, 9 } } } } };
			std::string json{};
			parser.serializeJson(original, json);
			BasicStructVec parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.items.size(), parsed.items[0].i, parsed.items[1].str, parsed.items[2].arr[2]);
		};

		auto test_partial_complex_thing = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.c, parsed.numbers.size(), parsed.array[0]);
		};

		auto test_partial_unicode = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 1, 1.0, "Hello 世界 🌍 test", { 1, 2, 3 } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("世界") != std::string::npos && parsed.str.find("🌍") != std::string::npos;
		};

		auto test_partial_special_chars = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 5, 6.7, "with \"quotes\" and \\ slash \n newline", { 9, 9, 9 } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.i == 5 && parsed.str.find("\"quotes\"") != std::string::npos && parsed.str.find("\n") != std::string::npos;
		};

		auto test_partial_large_payload = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStructVec original{};
			for (int32_t i = 0; i < 64; ++i) {
				original.items.push_back({ i, static_cast<double>(i) * 0.5, "item_" + std::to_string(i), { { uint32_t(i), uint32_t(i + 1), uint32_t(i + 2) } } });
			}
			std::string json{};
			parser.serializeJson(original, json);
			BasicStructVec parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.items.size(), parsed.items[0].i, parsed.items[63].i, parsed.items[32].str);
		};

		auto test_partial_boundary_lengths = []() {
			jsonifier::jsonifier_core<> parser{};
			bool all_passed = true;
			for (uint64_t pad = 0; pad < 80; ++pad) {
				BasicStruct obj{ 7, 1.5, std::string(pad, 'x'), { { 1, 2, 3 } } };
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

		auto test_partial_nested_struct_vec = []() {
			jsonifier::jsonifier_core<> parser{};
			NestedStruct ns{};
			ns.inner = { 42, 3.14, "nested", { { 1, 2, 3 } } };
			ns.nums	 = { 10, 20, 30, 40, 50 };
			std::string json{};
			parser.serializeJson(ns, json);
			NestedStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.i, parsed.inner.str, parsed.nums.size(), parsed.nums[4]);
		};

		auto test_partial_minified = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 42, 3.14, "minified", { { 1, 2, 3 } } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			static constexpr jsonifier::parse_options minifiedOpts{ .partialRead = partial, .knownOrder = knownOrder, .minified = true };
			parser.parseJson<minifiedOpts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.str);
		};

		auto test_partial_with_validation = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 11, 2.5, "validated", { { 4, 5, 6 } } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.str, parsed.arr[1]);
		};

		auto test_basic_reflection = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 42, 3.14, "Hello", { 1, 2, 3 } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct obj2{};
			parser.parseJson<opts>(obj2, json);
			printErrors(parser);
			return std::make_tuple(json, obj2.i, obj2.d, obj2.str, obj2.arr[0]);
		};

		auto test_meta_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			MetaStruct obj{ 5, "Gadget" };
			std::string json{};
			parser.serializeJson(obj, json);
			MetaStruct obj2{};
			std::string input = R"({"cnt":10,"label":"Widget"})";
			parser.parseJson<opts>(obj2, input);
			printErrors(parser);
			return std::make_tuple(json, obj2.count, obj2.name);
		};

		auto test_optional_fields = []() {
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

		auto test_enum_as_integer = []() {
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

		auto test_enum_map_key = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, int32_t> obj{ { "one", 1 } };
			std::string json{};
			parser.serializeJson(obj, json);
			std::map<std::string, int32_t> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed["one"] == 1;
		};

		auto test_containers = []() {
			jsonifier::jsonifier_core<> parser{};
			ContainerStruct c{};
			std::string json{};
			parser.serializeJson(c, json);
			ContainerStruct c2{};
			parser.parseJson<opts>(c2, json);
			printErrors(parser);
			return std::make_tuple(c2.vec == std::vector<int32_t>{ 1, 2, 3 }, c2.arr[0], std::get<2>(c2.tup));
		};

		auto test_map_unordered = []() {
			jsonifier::jsonifier_core<> parser{};
			MapStruct ms{};
			std::string json{};
			parser.serializeJson(ms, json);
			MapStruct ms2{};
			parser.parseJson<opts>(ms2, json);
			printErrors(parser);
			return (json.find("one") != std::string::npos) && ms2.str_map["one"] == 1;
		};

		auto test_prettify = []() {
			jsonifier::jsonifier_core<> parser{};
			PrettifyStruct pd{ 123, "Hello" };
			std::string json{};
			parser.serializeJson(pd, json);
			std::string pretty{};
			parser.prettifyJson(json, pretty);
			std::string minified{};
			parser.minifyJson(pretty, minified);
			return (pretty.find('\n') != std::string::npos) && minified == json;
		};

		auto test_minify = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string prettified = "{\n    \"id\": 42,\n    \"msg\": \"test\"\n}";
			std::string minified{};
			parser.minifyJson(prettified, minified);
			return minified.find("\n") == std::string::npos && minified.size() < prettified.size();
		};

		auto test_validate_valid = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,"str":"Hello","arr":[1,2,3]})";
			return parser.validateJson(json);
		};

		auto test_validate_invalid = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"i":42,"d":3.14,})";
			return !parser.validateJson(json);
		};

		auto test_float_precision = []() {
			jsonifier::jsonifier_core<> parser{};
			FloatPrecision fp{};
			std::string json{};
			parser.serializeJson(fp, json);
			return json.find("3.14159") != std::string::npos;
		};

		auto test_nested_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			NestedStruct ns{};
			ns.inner = { 42, 3.14, "nested", { { 1, 2, 3 } } };
			ns.nums	 = { 10, 20, 30 };
			std::string json{};
			parser.serializeJson(ns, json);
			NestedStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.i, parsed.nums.size());
		};

		auto test_shared_ptr = []() {
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

		auto test_vector_of_structs = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<BasicStruct> vec{ { 1, 1.1, "a", { { 1, 2, 3 } } }, { 2, 2.2, "b", { { 4, 5, 6 } } } };
			std::string json{};
			parser.serializeJson(vec, json);
			std::vector<BasicStruct> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.size(), parsed[0].i, parsed[1].str);
		};

		auto test_array_of_enums = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<Color, 3> arr{ Color::Red, Color::Green, Color::Blue };
			std::string json{};
			parser.serializeJson(arr, json);
			jsonifier::internal::array<Color, 3> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed[0] == Color::Red, parsed[2] == Color::Blue);
		};

		auto test_optional_with_value = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional obj{ "test", 5.5 };
			std::string json{};
			parser.serializeJson(obj, json);
			WithOptional parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.maybe.has_value() && *parsed.maybe == 5.5;
		};

		auto test_optional_without_value = []() {
			jsonifier::jsonifier_core<> parser{};
			WithOptional obj{ "test", std::nullopt };
			std::string json{};
			parser.serializeJson(obj, json);
			WithOptional parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return !parsed.maybe.has_value();
		};

		auto test_empty_containers = []() {
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

		auto test_large_numbers = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 2147483647, 1.7976931348623157e+308, "max",
				{ std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max() - 1, std::numeric_limits<uint32_t>::max() - 2 } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.arr[0]);
		};

		auto test_special_string_chars = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 1, 1.0, "test\"quote\\slash\nnewline", { { 1, 2, 3 } } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("\"") != std::string::npos && parsed.str.find("\n") != std::string::npos;
		};

		auto test_unicode_string = []() {
			jsonifier::jsonifier_core<> parser{};
			BasicStruct obj{ 1, 1.0, "Hello 世界 🌍", { { 1, 2, 3 } } };
			std::string json{};
			parser.serializeJson(obj, json);
			BasicStruct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return parsed.str.find("世界") != std::string::npos;
		};

		auto test_tuple_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			std::tuple<int32_t, double, std::string> tup{ 123, 4.56, "test" };
			std::string json{};
			parser.serializeJson(tup, json);
			std::tuple<int32_t, double, std::string> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(std::get<0>(parsed), std::get<2>(parsed));
		};

		auto test_nested_maps = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, std::map<std::string, int32_t>> nested{ { "outer1", { { "inner1", 1 }, { "inner2", 2 } } }, { "outer2", { { "inner3", 3 } } } };
			std::string json{};
			parser.serializeJson(nested, json);
			std::map<std::string, std::map<std::string, int32_t>> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed["outer1"]["inner1"], parsed["outer2"]["inner3"]);
		};

		auto test_vector_of_vectors = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<std::vector<int32_t>> vec{ { 1, 2 }, { 3, 4, 5 } };
			std::string json{};
			parser.serializeJson(vec, json);
			std::vector<std::vector<int32_t>> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.size(), parsed[1].size(), parsed[1][2]);
		};

		auto test_char_empty = []() {
			jsonifier::jsonifier_core<> parser{};
			char_roundtrip deserialized{ 'a', 'b', 1 };
			std::string buffer{};
			parser.serializeJson(deserialized, buffer);
			parser.parseJson<opts>(deserialized, buffer);
			printErrors(parser);
			return std::make_tuple(deserialized.char_val, deserialized.uchar_val, deserialized.int_val);
		};

		auto test_basic_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct obj{ 42, "test", 3.14 };
			std::string result{};
			parser.serializeJson(obj, result);
			return !result.empty() && result.find("42") != std::string::npos;
		};

		auto test_basic_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"id":42,"name":"test","value":3.14})";
			simple_struct obj{};
			parser.parseJson<opts>(obj, json);
			printErrors(parser);
			return std::make_tuple(obj.id, obj.name);
		};

		auto test_roundtrip = []() {
			jsonifier::jsonifier_core<> parser{};
			simple_struct original{ 99, "roundtrip", 2.71828 };
			std::string serialized{};
			parser.serializeJson(original, serialized);
			simple_struct parsed{};
			parser.parseJson<opts>(parsed, serialized);
			printErrors(parser);
			return std::make_tuple(parsed.id, parsed.name);
		};

		auto test_nested = []() {
			jsonifier::jsonifier_core<> parser{};
			nested_struct obj{};
			obj.inner	= { 1, "nested", 1.5 };
			obj.numbers = { 1, 2, 3, 4, 5 };
			obj.flag	= true;
			std::string json{};
			parser.serializeJson(obj, json);
			nested_struct parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.inner.id, parsed.numbers.size(), parsed.flag);
		};

		auto test_double_write = []() {
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

		auto test_double_parse = []() {
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

		auto test_int_write = []() {
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

		auto test_int_parse = []() {
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

		auto test_bool_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(true, buffer);
			std::string a = buffer;
			parser.serializeJson(false, buffer);
			std::string b = buffer;
			return std::make_tuple(a, b);
		};

		auto test_bool_parse = []() {
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

		auto test_string_write = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string buffer{};
			parser.serializeJson(std::string{ "fish" }, buffer);
			std::string a = buffer;
			parser.serializeJson(std::string{ "as\"df\\ghjkl" }, buffer);
			std::string b = buffer;
			return std::make_tuple(a, b);
		};

		auto test_string_parse = []() {
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

		auto test_vector_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<int32_t> vec{ 1, 2, 3, 4, 5 };
			std::string json{};
			parser.serializeJson(vec, json);
			return json.find("[") != std::string::npos && json.find("]") != std::string::npos;
		};

		auto test_vector_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "[10,20,30,40,50]";
			std::vector<int32_t> vec{};
			parser.parseJson<opts>(vec, json);
			printErrors(parser);
			return std::make_tuple(vec.size(), vec[0], vec[4]);
		};

		auto test_array_serialize = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<int32_t, 3> arr{ 1, 2, 3 };
			std::string json{};
			parser.serializeJson(arr, json);
			return json.find("[") != std::string::npos && json.find("]") != std::string::npos;
		};

		auto test_array_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "[10,20,30]";
			jsonifier::internal::array<int32_t, 3> arr{};
			parser.parseJson<opts>(arr, json);
			printErrors(parser);
			return std::make_tuple(arr.size(), arr[0], arr[2]);
		};

		auto test_escaped_chars_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = R"({"escaped\"key":0,"escaped\"\"key2":"hi","escape_chars":"\b\f\n\r\t"})";
			escaped_struct obj{};
			parser.parseJson<opts>(obj, json);
			printErrors(parser);
			return obj.escape_chars;
		};

		auto test_enum_serialize = []() -> std::string {
			jsonifier::jsonifier_core<> parser{};
			Color color = Color::Green;
			std::string json{};
			parser.serializeJson(color, json);
			return json;
		};

		auto test_enum_parse = []() {
			jsonifier::jsonifier_core<> parser{};
			std::string json = "0";
			Color color{};
			parser.parseJson<opts>(color, json);
			printErrors(parser);
			return color == Color::Red;
		};

		auto test_enum_array = []() {
			jsonifier::jsonifier_core<> parser{};
			jsonifier::internal::array<Color, 3> arr{};
			std::string json = "[1,0,2]";
			parser.parseJson<opts>(arr, json);
			printErrors(parser);
			return std::make_tuple(arr[0] == Color::Green, arr[1] == Color::Red, arr[2] == Color::Blue);
		};

		auto test_vehicle_enum = []() {
			jsonifier::jsonifier_core<> parser{};
			Vehicle vehicle = Vehicle::Plane;
			std::string json{};
			parser.serializeJson(vehicle, json);
			Vehicle parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(json, parsed == Vehicle::Plane);
		};

		auto test_complex_struct = []() {
			jsonifier::jsonifier_core<> parser{};
			Thing obj{};
			std::string json{};
			parser.serializeJson(obj, json);
			Thing parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed.i, parsed.d, parsed.c);
		};

		auto test_optional_empty = []() {
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

		auto test_optional_value = []() {
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

		auto test_map = []() {
			jsonifier::jsonifier_core<> parser{};
			std::map<std::string, int32_t> map{ { "a", 4 }, { "f", 7 }, { "b", 12 } };
			std::string json{};
			parser.serializeJson(map, json);
			std::map<std::string, int32_t> parsed{};
			parser.parseJson<opts>(parsed, json);
			printErrors(parser);
			return std::make_tuple(parsed["a"], parsed["f"], parsed["b"]);
		};

		auto test_dummy_data = []() {
			jsonifier::jsonifier_core<> parser{};
			std::vector<dummy_data> test_data = { { 0, 0, TestData::None, TestData::None, TestData::None, TestData::None, 0 },
				{ 1, 1, TestData::A, TestData::B, TestData::A, TestData::B, 0xDDDDDDDD }, { 2, 6, TestData::A, TestData::B, TestData::C, TestData::D, 0xEEEEEEEE },
				{ 3, -1, TestData::ERROR_E, TestData::ERROR_E, TestData::ERROR_E, TestData::ERROR_E, 0xFFFFFFFF } };
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
		rt_ut::unit_test<"Partial Nested", true>::assert_eq(std::make_tuple(7, std::string{ "deep" }, std::uint64_t{ 8 }, true), test_partial_nested);
		rt_ut::unit_test<"Partial Meta Renamed", true>::assert_eq(std::make_tuple(10, std::string{ "Widget" }), test_partial_meta_renamed);
		rt_ut::unit_test<"Partial Optional Present", true>::assert_eq(true, test_partial_optional_present);
		rt_ut::unit_test<"Partial Optional Absent", true>::assert_eq(true, test_partial_optional_absent);
		rt_ut::unit_test<"Partial containers", true>::assert_eq(std::make_tuple(true, std::string{ "Hello" }, std::string{ "pi?" }), test_partial_containers);
		rt_ut::unit_test<"Partial Map", true>::assert_eq(true, test_partial_map);
		rt_ut::unit_test<"Partial Vector of Structs", true>::assert_eq(std::make_tuple(std::uint64_t{ 3 }, 1, std::string{ "b" }, 9u), test_partial_vector_of_structs);
		rt_ut::unit_test<"Partial Complex Thing", true>::assert_eq(std::make_tuple(8, 2.0, 'W', std::uint64_t{ 4 }, std::string{ "as\"df\\ghjkl" }), test_partial_complex_thing);
		rt_ut::unit_test<"Partial Unicode", true>::assert_eq(true, test_partial_unicode);
		rt_ut::unit_test<"Partial Special Chars", true>::assert_eq(true, test_partial_special_chars);
		rt_ut::unit_test<"Partial Large Payload", true>::assert_eq(std::make_tuple(std::uint64_t{ 64 }, 0, 63, std::string{ "item_32" }), test_partial_large_payload);
		rt_ut::unit_test<"Partial Boundary Lengths", true>::assert_eq(true, test_partial_boundary_lengths);
		rt_ut::unit_test<"Partial Nested Struct Vec", true>::assert_eq(std::make_tuple(42, std::string{ "nested" }, std::uint64_t{ 5 }, 50), test_partial_nested_struct_vec);
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
		rt_ut::unit_test<"Float Precision", true>::assert_eq(true, test_float_precision);
		rt_ut::unit_test<"Nested Struct", true>::assert_eq(std::make_tuple(42, std::uint64_t{ 3 }), test_nested_struct);
		rt_ut::unit_test<"Shared Ptr", true>::assert_eq(true, test_shared_ptr);
		rt_ut::unit_test<"Vector of Structs", true>::assert_eq(std::make_tuple(std::uint64_t{ 2 }, 1, std::string{ "b" }), test_vector_of_structs);
		rt_ut::unit_test<"Array of Enums", true>::assert_eq(std::make_tuple(true, true), test_array_of_enums);
		rt_ut::unit_test<"Optional With Value", true>::assert_eq(true, test_optional_with_value);
		rt_ut::unit_test<"Optional Without Value", true>::assert_eq(true, test_optional_without_value);
		rt_ut::unit_test<"Empty containers", true>::assert_eq(true, test_empty_containers);
		rt_ut::unit_test<"Large Numbers", true>::assert_eq(std::make_tuple(2147483647, std::numeric_limits<uint32_t>::max()), test_large_numbers);
		rt_ut::unit_test<"Special String Chars", true>::assert_eq(true, test_special_string_chars);
		rt_ut::unit_test<"Unicode String", true>::assert_eq(true, test_unicode_string);
		rt_ut::unit_test<"Tuple Roundtrip", true>::assert_eq(std::make_tuple(123, std::string{ "test" }), test_tuple_roundtrip);
		rt_ut::unit_test<"Nested Maps", true>::assert_eq(std::make_tuple(1, 3), test_nested_maps);
		rt_ut::unit_test<"Vector of Vectors", true>::assert_eq(std::make_tuple(std::uint64_t{ 2 }, std::uint64_t{ 3 }, 5), test_vector_of_vectors);
		rt_ut::unit_test<"Char Empty String", true>::assert_eq(std::make_tuple(char{ 'a' }, static_cast<uint8_t>('b'), 1), test_char_empty);
		rt_ut::unit_test<"Basic Serialize", true>::assert_eq(true, test_basic_serialize);
		rt_ut::unit_test<"Basic Parse", true>::assert_eq(std::make_tuple(42, std::string{ "test" }), test_basic_parse);
		rt_ut::unit_test<"Roundtrip", true>::assert_eq(std::make_tuple(99, std::string{ "roundtrip" }), test_roundtrip);
		rt_ut::unit_test<"Nested Structures", true>::assert_eq(std::make_tuple(1, std::uint64_t{ 5 }, true), test_nested);
		rt_ut::unit_test<"Double Write", true>::assert_eq(std::make_tuple(std::string{ "3.14" }, std::string{ "0" }, std::string{ "-0" }), test_double_write);
		rt_ut::unit_test<"Double Parse", true>::assert_eq(std::make_tuple(3.14, 9.81, 0.0), test_double_parse);
		rt_ut::unit_test<"Int Write", true>::assert_eq(std::make_tuple(std::string{ "0" }, std::string{ "999" }, std::string{ "-6" }), test_int_write);
		rt_ut::unit_test<"Int Parse", true>::assert_eq(std::make_tuple(-1, 0, 999), test_int_parse);
		rt_ut::unit_test<"Bool Write", true>::assert_eq(std::make_tuple(std::string{ "true" }, std::string{ "false" }), test_bool_write);
		rt_ut::unit_test<"Bool Parse", true>::assert_eq(std::make_tuple(true, false), test_bool_parse);
		rt_ut::unit_test<"String Write", true>::assert_eq(std::make_tuple(std::string{ "\"fish\"" }, std::string{ "\"as\\\"df\\\\ghjkl\"" }), test_string_write);
		rt_ut::unit_test<"String Parse", true>::assert_eq(std::make_tuple(std::string{ "fish" }, std::string{ "as\"df\\ghjkl" }), test_string_parse);
		rt_ut::unit_test<"Vector Serialize", true>::assert_eq(true, test_vector_serialize);
		rt_ut::unit_test<"Vector Parse", true>::assert_eq(std::make_tuple(std::uint64_t{ 5 }, 10, 50), test_vector_parse);
		rt_ut::unit_test<"Array Serialize", true>::assert_eq(true, test_array_serialize);
		rt_ut::unit_test<"Array Parse", true>::assert_eq(std::make_tuple(std::uint64_t{ 3 }, 10, 30), test_array_parse);
		rt_ut::unit_test<"Escaped Chars Parse", true>::assert_eq(std::string{ "\b\f\n\r\t" }, test_escaped_chars_parse);
		rt_ut::unit_test<"Enum Serialize", true>::assert_eq(std::string{ "1" }, test_enum_serialize);
		rt_ut::unit_test<"Enum Parse", true>::assert_eq(true, test_enum_parse);
		rt_ut::unit_test<"Enum Array", true>::assert_eq(std::make_tuple(true, true, true), test_enum_array);
		rt_ut::unit_test<"Vehicle Enum", true>::assert_eq(std::make_tuple(std::string{ "2" }, true), test_vehicle_enum);
		rt_ut::unit_test<"Complex Struct", true>::assert_eq(std::make_tuple(8, 2.0, 'W'), test_complex_struct);
		rt_ut::unit_test<"Optional Empty", true>::assert_eq(true, test_optional_empty);
		rt_ut::unit_test<"Optional Value", true>::assert_eq(true, test_optional_value);
		rt_ut::unit_test<"Map", true>::assert_eq(std::make_tuple(4, 7, 12), test_map);
		rt_ut::unit_test<"Dummy Data Vector", true>::assert_eq(std::make_tuple(std::uint64_t{ 4 }, true), test_dummy_data);
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
