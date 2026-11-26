/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/generic_parser.hpp
 */
#pragma once

#include "common.hpp"

namespace generic_parser_tests {

	template<typename value_type, bool minified> inline value_type extracted(const jsonifier::generic::generic_value<minified>& target) {
		value_type out{};
		return target.get(out) == jsonifier::generic::parse_statuses::success ? out : value_type{};
	}

	template<typename value_type, bool minified> inline jsonifier::generic::parse_statuses extractionError(const jsonifier::generic::generic_value<minified>& target) {
		value_type out{};
		return target.get(out);
	}

	template<bool minified> inline bool tailObjectParses(jsonifier::jsonifier_core<>& parser, uint64_t padLength) {
		const std::string pad(padLength, 'x');
		const std::string input{ minified ? R"({"pad":")" + pad + R"(","k":"a\"b\\","n":-12345,"t":true})"
										  : "{ \"pad\" : \"" + pad + "\",\n  \"k\" : \"a\\\"b\\\\\" ,\n  \"n\" : -12345 ,\n  \"t\" : true }\n" };
		auto document{ parser.iterate<jsonifier::parse_options{ .minified = minified }>(input) };
		return extracted<std::string_view>(document["pad"]) == pad && extracted<std::string_view>(document["k"]) == "a\"b\\" && extracted<int64_t>(document["n"]) == -12345 &&
			extracted<bool>(document["t"]);
	}

	template<bool minified> inline bool tailArrayParses(jsonifier::jsonifier_core<>& parser, uint64_t padLength) {
		const std::string pad(padLength, 'y');
		const std::string input{ minified ? R"([")" + pad + R"(",1.5e3,"q\\\""])" : "[ \"" + pad + "\" , 1.5e3 ,\n \"q\\\\\\\"\" ]" };
		auto array{ parser.iterate<jsonifier::parse_options{ .minified = minified }>(input).getArray() };
		return extracted<std::string_view>(array.at(0)) == pad && extracted<double>(array.at(1)) == 1500.0 && extracted<std::string_view>(array.at(2)) == "q\\\"";
	}

	template<bool minified> inline bool tailWidthsParse(jsonifier::jsonifier_core<>& parser) {
		for (uint64_t padLength{ 0 }; padLength < 300; ++padLength) {
			if (!tailObjectParses<minified>(parser, padLength) || !tailArrayParses<minified>(parser, padLength)) {
				std::cout << "tail width mismatch at pad length " << padLength << (minified ? " (minified)" : " (prettified)") << std::endl;
				return false;
			}
		}
		return true;
	}

	alignas(64) inline constexpr jsonifier::parse_options comma{};
	alignas(64) inline constexpr jsonifier::parse_options newLines{ .newLineDelimited = true };

	struct stream_summary {
		jsonifier::generic::parse_statuses lastError{ jsonifier::generic::parse_statuses::success };

		bool operator==(const stream_summary&) const = default;

		std::vector<std::string> sources{};
		std::vector<uint64_t> indices{};
		uint64_t truncated{};
		int64_t idSum{};
	};

	template<jsonifier::parse_options options = newLines, typename parser_type>
	inline stream_summary summarizeStream(parser_type& parser, const std::string& input, uint64_t batchSize = options.defaultBatchSize) {
		stream_summary summary{};
		auto stream = parser.template iterateMany<options>(input, batchSize);
		for (auto iter = stream.begin(); iter != stream.end(); ++iter) {
			jsonifier::generic::generic_document<false> doc{ *iter };
			if (doc.error() != jsonifier::generic::parse_statuses::success) {
				summary.lastError = doc.error();
				continue;
			}
			summary.sources.emplace_back(iter.source());
			summary.indices.emplace_back(iter.currentIndex());
			if (doc.type() == jsonifier::json_types::object) {
				summary.idSum += extracted<int64_t>(doc.getObject().findFieldUnordered("id"));
			}
		}
		summary.truncated = stream.truncatedBytes();
		return summary;
	}

	inline std::string makeNdjson(uint64_t count, std::string_view separator) {
		std::string out{};
		for (uint64_t x = 0; x < count; ++x) {
			out += R"({"name":"record \")" + std::to_string(x) + R"(\" {[,]}","tags":[1,2,{"k":"v"}],"id":)" + std::to_string(x) + "}";
			out += separator;
		}
		return out;
	}

	template<typename parser_type> inline void runStreamTests(parser_type& parser) {
		using parse_statuses = jsonifier::generic::parse_statuses;
		static_assert(std::input_iterator<typename jsonifier::generic::document_stream<parser_type, comma>::iterator>);
		static_assert(std::input_iterator<typename jsonifier::generic::document_stream<parser_type, newLines>::iterator>);

		rt_ut::unit_test<"generic_stream_ndjson", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n" });
			return summary.sources.size() == 3 && summary.idSum == 6 && summary.indices == std::vector<uint64_t>{ 0, 9, 18 } && summary.sources[1] == R"({"id":2})" &&
				summary.lastError == parse_statuses::success && summary.truncated == 0;
		});

		rt_ut::unit_test<"generic_stream_whitespace_scalars", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ " 1 \"two\"\t[3]\r\n{\"id\":4} true null -5.5" });
			return summary.sources == std::vector<std::string>{ "1", "\"two\"", "[3]", "{\"id\":4}", "true", "null", "-5.5" } && summary.idSum == 4;
		});

		rt_ut::unit_test<"generic_stream_values_per_document", true>::assert_eq(true, [&] {
			std::string input{ "{\"a\":[1,2,3],\"b\":\"x\"} {\"b\":\"y\",\"a\":[4]}" };
			std::string out{};
			for (jsonifier::generic::generic_document<false> doc: parser.iterateMany(input)) {
				out += extracted<std::string_view>(doc.getObject().findFieldUnordered("b"));
				out += std::to_string(extracted<int64_t>(doc["a"][0]));
			}
			return out == "x1y4";
		});

		rt_ut::unit_test<"generic_stream_comma_separated", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "{\"id\":1} , {\"id\":2},[3],4 ,\"five\"" }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources == std::vector<std::string>{ "{\"id\":1}", "{\"id\":2}", "[3]", "4", "\"five\"" } && summary.idSum == 3 &&
				summary.lastError == parse_statuses::success;
		});

		rt_ut::unit_test<"generic_stream_comma_disallowed", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1},{\"id\":2}" });
			return summary.sources.size() == 1 && summary.lastError == parse_statuses::unexpected_token;
		});

		rt_ut::unit_test<"generic_stream_comma_trailing", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "1,2, " }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources.size() == 2 && summary.lastError == parse_statuses::unfinished_input;
		});

		rt_ut::unit_test<"generic_stream_comma_double", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "1,,2" }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources.size() == 1 && summary.lastError == parse_statuses::unexpected_token;
		});

		rt_ut::unit_test<"generic_stream_small_batches", true>::assert_eq(true, [&] {
			const std::string input = makeNdjson(500, "\n");
			const auto whole		= summarizeStream(parser, input);
			bool allMatch			= whole.sources.size() == 500 && whole.idSum == 124750;
			for (uint64_t batch: { 128ull, 131ull, 200ull, 1000ull, 4096ull }) {
				allMatch = allMatch && summarizeStream(parser, input, batch) == whole;
			}
			return allMatch;
		});

		rt_ut::unit_test<"generic_stream_small_batches_comma", true>::assert_eq(true, [&] {
			std::string input = makeNdjson(300, ",\n");
			input.resize(input.size() - 2);
			const auto whole = summarizeStream<comma>(parser, input, jsonifier::parse_options{}.defaultBatchSize);
			bool allMatch	 = whole.sources.size() == 300 && whole.idSum == 44850 && whole.lastError == parse_statuses::success;
			for (uint64_t batch: { 128ull, 151ull, 512ull }) {
				allMatch = allMatch && summarizeStream<comma>(parser, input, batch) == whole;
			}
			return allMatch;
		});

		rt_ut::unit_test<"generic_stream_small_batches_scalars", true>::assert_eq(true, [&] {
			std::string input{};
			for (uint64_t x = 0; x < 400; ++x) {
				input += std::to_string(x * 1234567) + (x % 3 == 0 ? " \"s\\\"tr\" " : " true\n");
			}
			const auto whole = summarizeStream(parser, input);
			return whole.sources.size() == 800 && summarizeStream(parser, input, 64) == whole && summarizeStream(parser, input, 77) == whole;
		});

		rt_ut::unit_test<"generic_stream_truncated", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1}\n{\"id\":" });
			return summary.sources.size() == 1 && summary.truncated == 6 && summary.lastError == parse_statuses::success;
		});

		rt_ut::unit_test<"generic_stream_truncated_string", true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "[1] {\"id\":\"}}}}" });
			return summary.sources.size() == 1 && summary.truncated == 11;
		});

		rt_ut::unit_test<"generic_stream_capacity", true>::assert_eq(true, [&] {
			std::string input{ "{\"id\":1}\n{\"big\":\"" + std::string(200, 'x') + "\"}\n{\"id\":2}" };
			const auto summary = summarizeStream(parser, input, 64);
			return summary.sources.size() == 1 && summary.lastError == parse_statuses::capacity;
		});

		rt_ut::unit_test<"generic_stream_empty", true>::assert_eq(true, [&] {
			return summarizeStream(parser, std::string{}).sources.empty() && summarizeStream(parser, std::string{ " \n\t " }).sources.empty();
		});

		rt_ut::unit_test<"generic_stream_stray_closer", true>::assert_eq(static_cast<uint8_t>(parse_statuses::unexpected_token), [&] {
			return static_cast<uint8_t>(summarizeStream(parser, std::string{ "[1] ] [2]" }).lastError);
		});

		rt_ut::unit_test<"generic_stream_partial_reads", true>::assert_eq(true, [&] {
			std::string input{};
			for (uint64_t x = 0; x < 300; ++x) {
				input += "{\"id\":" + std::to_string(x) + ",\"nested\":{\"deep\":[1,{\"k\":\"}]\"}],\"s\":\"x\"},\"tail\":[[],{}]}\n";
				input += x % 2 == 0 ? "[" + std::to_string(x) + ",[2,3],{\"a\":1}]\n" : "\"str\" 7\n";
			}
			bool allMatch{ true };
			for (uint64_t batch: std::initializer_list<uint64_t>{ 128ull, 201ull, 4096ull, jsonifier::parse_options{}.defaultBatchSize }) {
				uint64_t idSum{};
				uint64_t deepSum{};
				uint64_t documents{};
				for (jsonifier::generic::generic_document<false> doc: parser.iterateMany(input, batch)) {
					++documents;
					if (doc.type() == jsonifier::json_types::object) {
						auto object = doc.getObject();
						idSum += static_cast<uint64_t>(extracted<int64_t>(object.findFieldUnordered("id")));
						auto nested = object.findFieldUnordered("nested");
						auto deep	= nested["deep"];
						deepSum += static_cast<uint64_t>(extracted<int64_t>(deep[0]));
					} else if (doc.type() == jsonifier::json_types::array) {
						idSum += static_cast<uint64_t>(extracted<int64_t>(doc[0]));
					}
				}
				allMatch = allMatch && documents == 300 + 150 + 300 && idSum == 44850 + 22350 && deepSum == 300;
			}
			return allMatch;
		});
	}

	inline void runTests() {
		std::cout << "generic_parser Tests" << std::endl;

		using parse_statuses = jsonifier::generic::parse_statuses;

		jsonifier::jsonifier_core<> parser{};
		std::string sample{ R"({ "a" : 1, "b": [1, 2.5, -3, true, false, null, "x\ny", "plain"], "c": {"d": {"e": "deep"}}, "e~/": 7, "big": 18446744073709551615 })" };

		rt_ut::unit_test<"generic_iterate_success", true>::assert_eq(static_cast<uint8_t>(parse_statuses::success), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).error());
		});

		rt_ut::unit_test<"generic_root_type_object", true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::object), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).type());
		});

		rt_ut::unit_test<"generic_find_int64", true>::assert_eq(static_cast<int64_t>(1), [&] {
			return extracted<int64_t>(parser.iterate(sample)["a"]);
		});

		rt_ut::unit_test<"generic_nested_string", true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample)["c"]["d"]["e"]) == "deep";
		});

		rt_ut::unit_test<"generic_re_access", true>::assert_eq(true, [&] {
			auto doc			= parser.iterate(sample);
			const int64_t first = extracted<int64_t>(doc["a"]);
			const auto nested	= extracted<std::string_view>(doc["c"]["d"]["e"]);
			return first == 1 && nested == "deep" && extracted<int64_t>(doc["a"]) == 1;
		});

		rt_ut::unit_test<"generic_array_iteration_types", true>::assert_eq(true, [&] {
			std::string types{};
			for (jsonifier::generic::generic_value<false> element: parser.iterate(sample)["b"].getArray()) {
				types += static_cast<char>('0' + static_cast<uint8_t>(element.type()));
			}
			return types == "33344522";
		});

		rt_ut::unit_test<"generic_escaped_string", true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample)["b"][6]) == "x\ny";
		});

		rt_ut::unit_test<"generic_escaped_string_into_std_string", true>::assert_eq(true, [&] {
			std::string out{};
			const auto errorCode = parser.iterate(sample)["b"][6].getString(out);
			return errorCode == parse_statuses::success && out == "x\ny";
		});

		static_assert(!jsonifier::internal::resize_and_overwriteable_types<jsonifier::string> && jsonifier::internal::resize_and_overwriteable_types<std::string>);

		rt_ut::unit_test<"generic_escaped_string_into_jsonifier_string", true>::assert_eq(true, [&] {
			jsonifier::string out{};
			const auto errorCode = parser.iterate(sample)["b"][6].getString(out);
			return errorCode == parse_statuses::success && std::string_view{ out.data(), out.size() } == "x\ny";
		});

		rt_ut::unit_test<"generic_plain_string_into_jsonifier_string", true>::assert_eq(true, [&] {
			jsonifier::string out{ "a much longer previous value that must be shrunk" };
			const auto errorCode = parser.iterate(sample)["b"][7].getString(out);
			return errorCode == parse_statuses::success && std::string_view{ out.data(), out.size() } == "plain";
		});

		rt_ut::unit_test<"generic_plain_string_into_std_string", true>::assert_eq(true, [&] {
			std::string out{ "previous contents that are longer" };
			const auto errorCode = parser.iterate(sample)["b"][7].getString(out);
			return errorCode == parse_statuses::success && out == "plain";
		});

		rt_ut::unit_test<"generic_get_double", true>::assert_eq(true, [&] {
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(sample)["b"][1])) == std::bit_cast<uint64_t>(2.5);
		});

		rt_ut::unit_test<"generic_get_negative_int64", true>::assert_eq(static_cast<int64_t>(-3), [&] {
			return extracted<int64_t>(parser.iterate(sample)["b"][2]);
		});

		rt_ut::unit_test<"generic_get_bool", true>::assert_eq(true, [&] {
			auto doc = parser.iterate(sample);
			return extracted<bool>(doc["b"][3]) && !extracted<bool>(doc["b"][4]);
		});

		rt_ut::unit_test<"generic_is_null", true>::assert_eq(true, [&] {
			return parser.iterate(sample)["b"][5].isNull();
		});

		rt_ut::unit_test<"generic_uint64_max", true>::assert_eq(static_cast<uint64_t>(18446744073709551615ULL), [&] {
			return extracted<uint64_t>(parser.iterate(sample)["big"]);
		});

		rt_ut::unit_test<"generic_json_pointer_nested", true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample).atPointer("/c/d/e")) == "deep";
		});

		rt_ut::unit_test<"generic_json_pointer_escaped_key", true>::assert_eq(static_cast<int64_t>(7), [&] {
			return extracted<int64_t>(parser.iterate(sample).atPointer("/e~0~1"));
		});

		rt_ut::unit_test<"generic_json_pointer_array_index", true>::assert_eq(true, [&] {
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(sample).atPointer("/b/1"))) == std::bit_cast<uint64_t>(2.5);
		});

		rt_ut::unit_test<"generic_json_pointer_invalid", true>::assert_eq(static_cast<uint8_t>(parse_statuses::invalid_json_pointer), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).atPointer("c").error());
		});

		rt_ut::unit_test<"generic_count_elements", true>::assert_eq(static_cast<uint64_t>(8), [&] {
			uint64_t count{};
			return parser.iterate(sample)["b"].getArray().countElements(count) == parse_statuses::success ? count : uint64_t{};
		});

		rt_ut::unit_test<"generic_count_fields", true>::assert_eq(static_cast<uint64_t>(5), [&] {
			uint64_t count{};
			return parser.iterate(sample).getObject().countFields(count) == parse_statuses::success ? count : uint64_t{};
		});

		rt_ut::unit_test<"generic_field_keys_in_order", true>::assert_eq(true, [&] {
			jsonifier::string keys{};
			for (auto field: parser.iterate(sample).getObject()) {
				keys += field.key();
				keys += ',';
			}
			return keys == "a,b,c,e~/,big,";
		});

		rt_ut::unit_test<"generic_raw_json_container", true>::assert_eq(true, [&] {
			jsonifier::string_view raw{};
			return parser.iterate(sample)["c"].rawJson(raw) == parse_statuses::success && raw == R"({"d": {"e": "deep"}})";
		});

		rt_ut::unit_test<"generic_no_such_field", true>::assert_eq(static_cast<uint8_t>(parse_statuses::no_such_field), [&] {
			return static_cast<uint8_t>(parser.iterate(sample)["zzz"].error());
		});

		rt_ut::unit_test<"generic_incorrect_type", true>::assert_eq(static_cast<uint8_t>(parse_statuses::incorrect_type), [&] {
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(sample)["a"]));
		});

		rt_ut::unit_test<"generic_index_out_of_bounds", true>::assert_eq(static_cast<uint8_t>(parse_statuses::index_out_of_bounds), [&] {
			return static_cast<uint8_t>(parser.iterate(sample)["b"][8].error());
		});

		rt_ut::unit_test<"generic_unclosed_container", true>::assert_eq(static_cast<uint8_t>(parse_statuses::unexpected_end_of_input), [&] {
			std::string input{ "[1,2" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		rt_ut::unit_test<"generic_mismatched_closer", true>::assert_eq(false, [&] {
			std::string input{ "[1,2]]" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getArray().countElements(count));
			return doc.atEnd();
		});

		rt_ut::unit_test<"generic_trailing_content", true>::assert_eq(false, [&] {
			std::string input{ R"({"a":1}x)" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getObject().countFields(count));
			return doc.atEnd();
		});

		rt_ut::unit_test<"generic_clean_document_at_end", true>::assert_eq(true, [&] {
			std::string input{ R"({"a":[1,2]})" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getObject().countFields(count));
			return doc.atEnd();
		});

		rt_ut::unit_test<"generic_partial_nested_then_sibling", true>::assert_eq(static_cast<int64_t>(5), [&] {
			std::string input{ R"({"a":{"x":[1,{"y":2}],"z":3},"b":[4,5]})" };
			auto object = parser.iterate(input).getObject();
			static_cast<void>(object.findFieldUnordered("a").getObject().findFieldUnordered("x"));
			return extracted<int64_t>(object.findFieldUnordered("b").getArray().at(1));
		});

		rt_ut::unit_test<"generic_missing_then_present_field", true>::assert_eq(true, [&] {
			std::string input{ R"({"a":1,"b":2,"c":3})" };
			auto object			= parser.iterate(input).getObject();
			const bool missing	= object.findFieldUnordered("q").error() == parse_statuses::no_such_field;
			const int64_t first = extracted<int64_t>(object.findFieldUnordered("c"));
			return missing && first == 3 && extracted<int64_t>(object.findFieldUnordered("a")) == 1;
		});

		rt_ut::unit_test<"generic_empty_input", true>::assert_eq(static_cast<uint8_t>(parse_statuses::no_input), [&] {
			std::string input{};
			return static_cast<uint8_t>(parser.iterate(input).error());
		});

		rt_ut::unit_test<"generic_missing_comma", true>::assert_eq(static_cast<uint8_t>(parse_statuses::unexpected_token), [&] {
			std::string input{ "[1 2]" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		rt_ut::unit_test<"generic_trailing_comma", true>::assert_eq(static_cast<uint8_t>(parse_statuses::unexpected_token), [&] {
			std::string input{ "[1,]" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		rt_ut::unit_test<"generic_bad_atom", true>::assert_eq(static_cast<uint8_t>(parse_statuses::invalid_bool_value), [&] {
			std::string input{ "[tru]" };
			return static_cast<uint8_t>(extractionError<bool>(parser.iterate(input)[0]));
		});

		rt_ut::unit_test<"generic_missing_colon", true>::assert_eq(static_cast<uint8_t>(parse_statuses::unexpected_token), [&] {
			std::string input{ R"({"a" 1})" };
			return static_cast<uint8_t>(parser.iterate(input)["a"].error());
		});

		rt_ut::unit_test<"generic_root_number", true>::assert_eq(static_cast<int64_t>(42), [&] {
			std::string input{ "42" };
			return extracted<int64_t>(parser.iterate(input));
		});

		rt_ut::unit_test<"generic_root_string_with_whitespace", true>::assert_eq(true, [&] {
			std::string input{ R"(  "s"  )" };
			return extracted<std::string_view>(parser.iterate(input)) == "s";
		});

		rt_ut::unit_test<"generic_non_ascii_string", true>::assert_eq(true, [&] {
			std::string input{ "\"caf\xC3\xA9\"" };
			return extracted<std::string_view>(parser.iterate(input)) == "caf\xC3\xA9";
		});

		rt_ut::unit_test<"generic_invalid_utf8_string", true>::assert_eq(static_cast<uint8_t>(parse_statuses::invalid_string_characters), [&] {
			std::string input{ "\"bad\xC3\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		rt_ut::unit_test<"generic_long_plain_string", true>::assert_eq(true, [&] {
			std::string payload(300, 'x');
			std::string input{ "\"" + payload + "\"" };
			return extracted<std::string_view>(parser.iterate(input)) == payload;
		});

		rt_ut::unit_test<"generic_minified_option", true>::assert_eq(static_cast<int64_t>(3), [&] {
			std::string input{ R"({"a":[1,2,3]})" };
			return extracted<int64_t>(parser.iterate<jsonifier::parse_options{ .minified = true }>(input)["a"][2]);
		});

		rt_ut::unit_test<"generic_root_scalar_double", true>::assert_eq(true, [&] {
			std::string input{ "-12.5" };
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(input))) == std::bit_cast<uint64_t>(-12.5);
		});

		rt_ut::unit_test<"generic_root_scalar_raw_json", true>::assert_eq(true, [&] {
			std::string input{ "42  " };
			jsonifier::string_view raw{};
			return parser.iterate(input).rawJson(raw) == parse_statuses::success && raw == "42";
		});

		rt_ut::unit_test<"generic_non_ascii_zero_copy", true>::assert_eq(true, [&] {
			std::string input{ "[\"caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80 and a longer tail past one simd register\"]" };
			std::string_view out{};
			const auto errorCode = parser.iterate(input)[0].getString(out);
			return errorCode == parse_statuses::success && out.data() == input.data() + 2 && out == std::string_view{ input }.substr(2, input.size() - 4);
		});

		rt_ut::unit_test<"generic_overlong_utf8_rejected", true>::assert_eq(static_cast<uint8_t>(parse_statuses::invalid_string_characters), [&] {
			std::string input{ "\"bad \xC0\xAF\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		rt_ut::unit_test<"generic_surrogate_utf8_rejected", true>::assert_eq(static_cast<uint8_t>(parse_statuses::invalid_string_characters), [&] {
			std::string input{ "\"bad \xED\xA0\x80\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		rt_ut::unit_test<"generic_escaped_views_stay_valid", true>::assert_eq(true, [&] {
			std::string input{ R"(["a\nb","c\td","e\\f"])" };
			auto doc = parser.iterate(input);
			std::string_view first{};
			std::string_view second{};
			static_cast<void>(doc[0].getString(first));
			static_cast<void>(doc[1].getString(second));
			bool stable{ true };
			for (uint64_t round = 0; round < 5000; ++round) {
				std::string_view again{};
				stable = stable && doc[round % 3].getString(again) == parse_statuses::success;
			}
			std::string_view third{};
			static_cast<void>(doc[2].getString(third));
			return stable && first == "a\nb" && second == "c\td" && third == "e\\f";
		});

		rt_ut::unit_test<"generic_escaped_key_matches", true>::assert_eq(static_cast<int64_t>(7), [&] {
			std::string input{ R"({"x":1,"a\u0062":7})" };
			return extracted<int64_t>(parser.iterate(input)["ab"]);
		});

		rt_ut::unit_test<"generic_unordered_wrap_miss_keeps_position", true>::assert_eq(true, [&] {
			std::string input{ R"({"a":{"n":1},"b":[1,2],"c":3,"d":4})" };
			auto object	   = parser.iterate(input).getObject();
			const bool hit = extracted<int64_t>(object.findFieldUnordered("c")) == 3;
			static_cast<void>(object.findFieldUnordered("b").error());
			const bool miss	 = object.findFieldUnordered("zz").error() == parse_statuses::no_such_field;
			const bool later = extracted<int64_t>(object.findFieldUnordered("d")) == 4;
			const jsonifier::generic::generic_value<false> earlier{ object.findFieldUnordered("a") };
			const bool nested = extracted<int64_t>(earlier["n"]) == 1;
			return hit && miss && later && nested;
		});

		rt_ut::unit_test<"generic_reverse_order_large_object", true>::assert_eq(true, [&] {
			std::string input{ "{" };
			for (uint64_t index = 0; index < 300; ++index) {
				input += (index ? ",\"field_" : "\"field_") + std::to_string(index) + "\":" + std::to_string(index * 3);
			}
			input += "}";
			auto object = parser.iterate(input).getObject();
			bool allMatch{ true };
			for (uint64_t index = 300; index-- > 0;) {
				allMatch = allMatch && extracted<int64_t>(object.findFieldUnordered("field_" + std::to_string(index))) == static_cast<int64_t>(index * 3);
			}
			return allMatch;
		});

		rt_ut::unit_test<"generic_same_keys_in_different_objects", true>::assert_eq(true, [&] {
			std::string input{ R"([{"a":1,"b":2,"c":3},{"a":10,"b":20,"c":30}])" };
			auto array	  = parser.iterate(input).getArray();
			auto first	  = array.at(0).getObject();
			const bool c  = extracted<int64_t>(first.findFieldUnordered("c")) == 3;
			const bool a  = extracted<int64_t>(first.findFieldUnordered("a")) == 1;
			auto second	  = array.at(1).getObject();
			const bool c2 = extracted<int64_t>(second.findFieldUnordered("c")) == 30;
			const bool a2 = extracted<int64_t>(second.findFieldUnordered("a")) == 10;
			const bool b2 = extracted<int64_t>(second.findFieldUnordered("b")) == 20;
			return c && a && c2 && a2 && b2;
		});

		rt_ut::unit_test<"generic_field_index_reset_between_documents", true>::assert_eq(true, [&] {
			std::string firstInput{ R"({"x":1,"y":2,"z":3})" };
			auto firstObject = parser.iterate(firstInput).getObject();
			const bool z	 = extracted<int64_t>(firstObject.findFieldUnordered("z")) == 3;
			const bool x	 = extracted<int64_t>(firstObject.findFieldUnordered("x")) == 1;
			std::string secondInput{ R"({"y":7,"x":8,"z":9})" };
			auto secondObject = parser.iterate(secondInput).getObject();
			const bool z2	  = extracted<int64_t>(secondObject.findFieldUnordered("z")) == 9;
			const bool x2	  = extracted<int64_t>(secondObject.findFieldUnordered("x")) == 8;
			const bool y2	  = extracted<int64_t>(secondObject.findFieldUnordered("y")) == 7;
			return z && x && z2 && x2 && y2;
		});

		rt_ut::unit_test<"generic_miss_after_full_index", true>::assert_eq(true, [&] {
			std::string input{ R"({"a":1,"b":{"n":[1,2,3]},"c":3})" };
			auto object			  = parser.iterate(input).getObject();
			const bool firstMiss  = object.findFieldUnordered("q").error() == parse_statuses::no_such_field;
			const bool secondMiss = object.findFieldUnordered("r").error() == parse_statuses::no_such_field;
			const bool c		  = extracted<int64_t>(object.findFieldUnordered("c")) == 3;
			const bool a		  = extracted<int64_t>(object.findFieldUnordered("a")) == 1;
			const jsonifier::generic::generic_value<false> nested{ object.findFieldUnordered("b") };
			return firstMiss && secondMiss && c && a && extracted<int64_t>(nested["n"][2]) == 3;
		});

		rt_ut::unit_test<"generic_reverse_order_long_keys", true>::assert_eq(true, [&] {
			std::string input{ R"({"a_rather_long_key_name_number_one":1,"a_rather_long_key_name_number_two":2,"a_rather_long_key_name_number_three":3})" };
			auto object		 = parser.iterate(input).getObject();
			const bool three = extracted<int64_t>(object.findFieldUnordered("a_rather_long_key_name_number_three")) == 3;
			const bool two	 = extracted<int64_t>(object.findFieldUnordered("a_rather_long_key_name_number_two")) == 2;
			const bool one	 = extracted<int64_t>(object.findFieldUnordered("a_rather_long_key_name_number_one")) == 1;
			return three && two && one;
		});

		rt_ut::unit_test<"generic_reverse_order_prettified", true>::assert_eq(true, [&] {
			std::string input{ "{\n  \"alpha\" : 1,\n  \"beta\"  : 2,\n  \"gamma\" : 3\n}" };
			auto object		 = parser.iterate(input).getObject();
			const bool gamma = extracted<int64_t>(object.findFieldUnordered("gamma")) == 3;
			const bool alpha = extracted<int64_t>(object.findFieldUnordered("alpha")) == 1;
			const bool beta	 = extracted<int64_t>(object.findFieldUnordered("beta")) == 2;
			return gamma && alpha && beta;
		});

		rt_ut::unit_test<"generic_json_pointer_index_overflow", true>::assert_eq(static_cast<uint8_t>(parse_statuses::index_out_of_bounds), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).atPointer("/b/184467440737095516160").error());
		});

		rt_ut::unit_test<"generic_tail_widths_minified", true>::assert_eq(true, [&] {
			return tailWidthsParse<true>(parser);
		});

		rt_ut::unit_test<"generic_tail_widths_prettified", true>::assert_eq(true, [&] {
			return tailWidthsParse<false>(parser);
		});

		runStreamTests(parser);
	}

}
