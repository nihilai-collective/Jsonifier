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
		return target.get(out) == jsonifier::generic::error_codes::success ? out : value_type{};
	}

	template<typename value_type, bool minified> inline jsonifier::generic::error_codes extractionError(const jsonifier::generic::generic_value<minified>& target) {
		value_type out{};
		return target.get(out);
	}

	alignas(64) inline constexpr jsonifier::parse_options comma{};
	alignas(64) inline constexpr jsonifier::parse_options newLines{ .newLineDelimited = true };

	struct stream_summary {
		jsonifier::generic::error_codes lastError{ jsonifier::generic::error_codes::success };

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
			if (doc.error() != jsonifier::generic::error_codes::success) {
				summary.lastError = doc.error();
				continue;
			}
			summary.sources.emplace_back(iter.source());
			summary.indices.emplace_back(iter.currentIndex());
			if (doc.type() == jsonifier::json_types::object) {
				summary.idSum += extracted<int64_t>(doc.getObject().findField("id"));
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
		using error_codes = jsonifier::generic::error_codes;
		static_assert(std::input_iterator<typename jsonifier::generic::document_stream<parser_type, comma>::iterator>);
		static_assert(std::input_iterator<typename jsonifier::generic::document_stream<parser_type, newLines>::iterator>);

		static constexpr rt_ut::string_literal streamNdjsonName{ "generic_stream_ndjson" };
		rt_ut::unit_test<streamNdjsonName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n" });
			return summary.sources.size() == 3 && summary.idSum == 6 && summary.indices == std::vector<uint64_t>{ 0, 9, 18 } && summary.sources[1] == R"({"id":2})" &&
				summary.lastError == error_codes::success && summary.truncated == 0;
		});

		static constexpr rt_ut::string_literal streamScalarsName{ "generic_stream_whitespace_scalars" };
		rt_ut::unit_test<streamScalarsName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ " 1 \"two\"\t[3]\r\n{\"id\":4} true null -5.5" });
			return summary.sources == std::vector<std::string>{ "1", "\"two\"", "[3]", "{\"id\":4}", "true", "null", "-5.5" } && summary.idSum == 4;
		});

		static constexpr rt_ut::string_literal streamValuesName{ "generic_stream_values_per_document" };
		rt_ut::unit_test<streamValuesName, true>::assert_eq(true, [&] {
			std::string input{ "{\"a\":[1,2,3],\"b\":\"x\"} {\"b\":\"y\",\"a\":[4]}" };
			std::string out{};
			for (jsonifier::generic::generic_document<false> doc: parser.iterateMany(input)) {
				out += extracted<std::string_view>(doc.findField("b"));
				out += std::to_string(extracted<int64_t>(doc.findField("a").getArray().at(0)));
			}
			return out == "x1y4";
		});

		static constexpr rt_ut::string_literal streamCommaName{ "generic_stream_comma_separated" };
		rt_ut::unit_test<streamCommaName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "{\"id\":1} , {\"id\":2},[3],4 ,\"five\"" }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources == std::vector<std::string>{ "{\"id\":1}", "{\"id\":2}", "[3]", "4", "\"five\"" } && summary.idSum == 3 &&
				summary.lastError == error_codes::success;
		});

		static constexpr rt_ut::string_literal streamCommaDisallowedName{ "generic_stream_comma_disallowed" };
		rt_ut::unit_test<streamCommaDisallowedName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1},{\"id\":2}" });
			return summary.sources.size() == 1 && summary.lastError == error_codes::tape_error;
		});

		static constexpr rt_ut::string_literal streamCommaTrailingName{ "generic_stream_comma_trailing" };
		rt_ut::unit_test<streamCommaTrailingName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "1,2, " }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources.size() == 2 && summary.lastError == error_codes::trailing_content;
		});

		static constexpr rt_ut::string_literal streamCommaDoubleName{ "generic_stream_comma_double" };
		rt_ut::unit_test<streamCommaDoubleName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream<comma>(parser, std::string{ "1,,2" }, jsonifier::parse_options{}.defaultBatchSize);
			return summary.sources.size() == 1 && summary.lastError == error_codes::tape_error;
		});

		static constexpr rt_ut::string_literal streamBatchedName{ "generic_stream_small_batches" };
		rt_ut::unit_test<streamBatchedName, true>::assert_eq(true, [&] {
			const std::string input = makeNdjson(500, "\n");
			const auto whole		= summarizeStream(parser, input);
			bool allMatch			= whole.sources.size() == 500 && whole.idSum == 124750;
			for (uint64_t batch: { 128ull, 131ull, 200ull, 1000ull, 4096ull }) {
				allMatch = allMatch && summarizeStream(parser, input, batch) == whole;
			}
			return allMatch;
		});

		static constexpr rt_ut::string_literal streamBatchedCommaName{ "generic_stream_small_batches_comma" };
		rt_ut::unit_test<streamBatchedCommaName, true>::assert_eq(true, [&] {
			std::string input = makeNdjson(300, ",\n");
			input.resize(input.size() - 2);
			const auto whole = summarizeStream<comma>(parser, input, jsonifier::parse_options{}.defaultBatchSize);
			bool allMatch	 = whole.sources.size() == 300 && whole.idSum == 44850 && whole.lastError == error_codes::success;
			for (uint64_t batch: { 128ull, 151ull, 512ull }) {
				allMatch = allMatch && summarizeStream<comma>(parser, input, batch) == whole;
			}
			return allMatch;
		});

		static constexpr rt_ut::string_literal streamBatchedScalarsName{ "generic_stream_small_batches_scalars" };
		rt_ut::unit_test<streamBatchedScalarsName, true>::assert_eq(true, [&] {
			std::string input{};
			for (uint64_t x = 0; x < 400; ++x) {
				input += std::to_string(x * 1234567) + (x % 3 == 0 ? " \"s\\\"tr\" " : " true\n");
			}
			const auto whole = summarizeStream(parser, input);
			return whole.sources.size() == 800 && summarizeStream(parser, input, 64) == whole && summarizeStream(parser, input, 77) == whole;
		});

		static constexpr rt_ut::string_literal streamTruncatedName{ "generic_stream_truncated" };
		rt_ut::unit_test<streamTruncatedName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "{\"id\":1}\n{\"id\":" });
			return summary.sources.size() == 1 && summary.truncated == 6 && summary.lastError == error_codes::success;
		});

		static constexpr rt_ut::string_literal streamTruncatedStringName{ "generic_stream_truncated_string" };
		rt_ut::unit_test<streamTruncatedStringName, true>::assert_eq(true, [&] {
			const auto summary = summarizeStream(parser, std::string{ "[1] {\"id\":\"}}}}" });
			return summary.sources.size() == 1 && summary.truncated == 11;
		});

		static constexpr rt_ut::string_literal streamCapacityName{ "generic_stream_capacity" };
		rt_ut::unit_test<streamCapacityName, true>::assert_eq(true, [&] {
			std::string input{ "{\"id\":1}\n{\"big\":\"" + std::string(200, 'x') + "\"}\n{\"id\":2}" };
			const auto summary = summarizeStream(parser, input, 64);
			return summary.sources.size() == 1 && summary.lastError == error_codes::capacity;
		});

		static constexpr rt_ut::string_literal streamEmptyName{ "generic_stream_empty" };
		rt_ut::unit_test<streamEmptyName, true>::assert_eq(true, [&] {
			return summarizeStream(parser, std::string{}).sources.empty() && summarizeStream(parser, std::string{ " \n\t " }).sources.empty();
		});

		static constexpr rt_ut::string_literal streamStrayCloserName{ "generic_stream_stray_closer" };
		rt_ut::unit_test<streamStrayCloserName, true>::assert_eq(static_cast<uint8_t>(error_codes::tape_error), [&] {
			return static_cast<uint8_t>(summarizeStream(parser, std::string{ "[1] ] [2]" }).lastError);
		});

		static constexpr rt_ut::string_literal streamPartialReadsName{ "generic_stream_partial_reads" };
		rt_ut::unit_test<streamPartialReadsName, true>::assert_eq(true, [&] {
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
						idSum += static_cast<uint64_t>(extracted<int64_t>(object.findField("id")));
						auto nested = object.findField("nested");
						auto deep	= nested.findField("deep");
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

	template<bool minified> inline bool tailObjectParses(jsonifier::jsonifier_core& parser, uint64_t padLength) {
		const std::string pad(padLength, 'x');
		const std::string input{ minified ? R"({"pad":")" + pad + R"(","k":"a\"b\\","n":-12345,"t":true})"
										  : "{ \"pad\" : \"" + pad + "\",\n  \"k\" : \"a\\\"b\\\\\" ,\n  \"n\" : -12345 ,\n  \"t\" : true }\n" };
		auto document{ parser.iterate<jsonifier::parse_options{ .minified = minified }>(input) };
		return extracted<std::string_view>(document.findField("pad")) == pad && extracted<std::string_view>(document.findField("k")) == "a\"b\\" &&
			extracted<int64_t>(document.findField("n")) == -12345 && extracted<bool>(document.findField("t"));
	}

	template<bool minified> inline bool tailArrayParses(jsonifier::jsonifier_core& parser, uint64_t padLength) {
		const std::string pad(padLength, 'y');
		const std::string input{ minified ? R"([")" + pad + R"(",1.5e3,"q\\\""])" : "[ \"" + pad + "\" , 1.5e3 ,\n \"q\\\\\\\"\" ]" };
		auto array{ parser.iterate<jsonifier::parse_options{ .minified = minified }>(input).getArray() };
		return extracted<std::string_view>(array.at(0)) == pad && extracted<double>(array.at(1)) == 1500.0 && extracted<std::string_view>(array.at(2)) == "q\\\"";
	}

	template<bool minified> inline bool tailWidthsParse(jsonifier::jsonifier_core& parser) {
		for (uint64_t padLength{ 0 }; padLength < 300; ++padLength) {
			if (!tailObjectParses<minified>(parser, padLength) || !tailArrayParses<minified>(parser, padLength)) {
				std::cout << "tail width mismatch at pad length " << padLength << (minified ? " (minified)" : " (prettified)") << std::endl;
				return false;
			}
		}
		return true;
	}

	inline void runTests() {
		std::cout << "generic_parser Tests" << std::endl;

		using error_codes = jsonifier::generic::error_codes;

		jsonifier::jsonifier_core parser{};
		std::string sample{ R"({ "a" : 1, "b": [1, 2.5, -3, true, false, null, "x\ny", "plain"], "c": {"d": {"e": "deep"}}, "e~/": 7, "big": 18446744073709551615 })" };

		static constexpr rt_ut::string_literal iterateSuccessName{ "generic_iterate_success" };
		rt_ut::unit_test<iterateSuccessName, true>::assert_eq(static_cast<uint8_t>(error_codes::success), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).error());
		});

		static constexpr rt_ut::string_literal rootTypeObjectName{ "generic_root_type_object" };
		rt_ut::unit_test<rootTypeObjectName, true>::assert_eq(static_cast<uint8_t>(jsonifier::json_types::object), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).type());
		});

		static constexpr rt_ut::string_literal findInt64Name{ "generic_find_int64" };
		rt_ut::unit_test<findInt64Name, true>::assert_eq(static_cast<int64_t>(1), [&] {
			return extracted<int64_t>(parser.iterate(sample).findField("a"));
		});

		static constexpr rt_ut::string_literal nestedStringName{ "generic_nested_string" };
		rt_ut::unit_test<nestedStringName, true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample).findField("c").findField("d").findField("e")) == "deep";
		});

		static constexpr rt_ut::string_literal reAccessName{ "generic_re_access" };
		rt_ut::unit_test<reAccessName, true>::assert_eq(true, [&] {
			auto doc			= parser.iterate(sample);
			const int64_t first = extracted<int64_t>(doc.findField("a"));
			const auto nested	= extracted<std::string_view>(doc.findField("c").findField("d").findField("e"));
			return first == 1 && nested == "deep" && extracted<int64_t>(doc.findField("a")) == 1;
		});

		static constexpr rt_ut::string_literal arrayIterationTypesName{ "generic_array_iteration_types" };
		rt_ut::unit_test<arrayIterationTypesName, true>::assert_eq(true, [&] {
			std::string types{};
			for (jsonifier::generic::generic_value<false> element: parser.iterate(sample).findField("b").getArray()) {
				types += static_cast<char>('0' + static_cast<uint8_t>(element.type()));
			}
			return types == "33344522";
		});

		static constexpr rt_ut::string_literal escapedStringName{ "generic_escaped_string" };
		rt_ut::unit_test<escapedStringName, true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample).findField("b").getArray().at(6)) == "x\ny";
		});

		static constexpr rt_ut::string_literal escapedStringIntoStdStringName{ "generic_escaped_string_into_std_string" };
		rt_ut::unit_test<escapedStringIntoStdStringName, true>::assert_eq(true, [&] {
			std::string out{};
			const auto errorCode = parser.iterate(sample).findField("b").getArray().at(6).getString(out);
			return errorCode == error_codes::success && out == "x\ny";
		});

		static constexpr rt_ut::string_literal escapedStringIntoJsonifierStringName{ "generic_escaped_string_into_jsonifier_string" };
		rt_ut::unit_test<escapedStringIntoJsonifierStringName, true>::assert_eq(true, [&] {
			jsonifier::string out{};
			const auto errorCode = parser.iterate(sample).findField("b").getArray().at(6).getString(out);
			return errorCode == error_codes::success && std::string_view{ out.data(), static_cast<size_t>(out.size()) } == "x\ny";
		});

		static constexpr rt_ut::string_literal plainStringIntoJsonifierStringName{ "generic_plain_string_into_jsonifier_string" };
		rt_ut::unit_test<plainStringIntoJsonifierStringName, true>::assert_eq(true, [&] {
			jsonifier::string out{ "a much longer previous value that must be shrunk" };
			const auto errorCode = parser.iterate(sample).findField("b").getArray().at(7).getString(out);
			return errorCode == error_codes::success && std::string_view{ out.data(), static_cast<size_t>(out.size()) } == "plain";
		});

		static constexpr rt_ut::string_literal plainStringIntoStdStringName{ "generic_plain_string_into_std_string" };
		rt_ut::unit_test<plainStringIntoStdStringName, true>::assert_eq(true, [&] {
			std::string out{ "previous contents that are longer" };
			const auto errorCode = parser.iterate(sample).findField("b").getArray().at(7).getString(out);
			return errorCode == error_codes::success && out == "plain";
		});

		static constexpr rt_ut::string_literal getDoubleName{ "generic_get_double" };
		rt_ut::unit_test<getDoubleName, true>::assert_eq(true, [&] {
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(sample).findField("b").getArray().at(1))) == std::bit_cast<uint64_t>(2.5);
		});

		static constexpr rt_ut::string_literal getNegativeInt64Name{ "generic_get_negative_int64" };
		rt_ut::unit_test<getNegativeInt64Name, true>::assert_eq(static_cast<int64_t>(-3), [&] {
			return extracted<int64_t>(parser.iterate(sample).findField("b").getArray().at(2));
		});

		static constexpr rt_ut::string_literal getBoolName{ "generic_get_bool" };
		rt_ut::unit_test<getBoolName, true>::assert_eq(true, [&] {
			auto doc = parser.iterate(sample);
			return extracted<bool>(doc.findField("b").getArray().at(3)) && !extracted<bool>(doc.findField("b").getArray().at(4));
		});

		static constexpr rt_ut::string_literal isNullName{ "generic_is_null" };
		rt_ut::unit_test<isNullName, true>::assert_eq(true, [&] {
			return parser.iterate(sample).findField("b").getArray().at(5).isNull();
		});

		static constexpr rt_ut::string_literal uint64MaxName{ "generic_uint64_max" };
		rt_ut::unit_test<uint64MaxName, true>::assert_eq(static_cast<uint64_t>(18446744073709551615ULL), [&] {
			return extracted<uint64_t>(parser.iterate(sample).findField("big"));
		});

		static constexpr rt_ut::string_literal jsonPointerNestedName{ "generic_json_pointer_nested" };
		rt_ut::unit_test<jsonPointerNestedName, true>::assert_eq(true, [&] {
			return extracted<std::string_view>(parser.iterate(sample).atPointer("/c/d/e")) == "deep";
		});

		static constexpr rt_ut::string_literal jsonPointerEscapedKeyName{ "generic_json_pointer_escaped_key" };
		rt_ut::unit_test<jsonPointerEscapedKeyName, true>::assert_eq(static_cast<int64_t>(7), [&] {
			return extracted<int64_t>(parser.iterate(sample).atPointer("/e~0~1"));
		});

		static constexpr rt_ut::string_literal jsonPointerArrayIndexName{ "generic_json_pointer_array_index" };
		rt_ut::unit_test<jsonPointerArrayIndexName, true>::assert_eq(true, [&] {
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(sample).atPointer("/b/1"))) == std::bit_cast<uint64_t>(2.5);
		});

		static constexpr rt_ut::string_literal jsonPointerInvalidName{ "generic_json_pointer_invalid" };
		rt_ut::unit_test<jsonPointerInvalidName, true>::assert_eq(static_cast<uint8_t>(error_codes::invalid_json_pointer), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).atPointer("c").error());
		});

		static constexpr rt_ut::string_literal countElementsName{ "generic_count_elements" };
		rt_ut::unit_test<countElementsName, true>::assert_eq(static_cast<uint64_t>(8), [&] {
			uint64_t count{};
			return parser.iterate(sample).findField("b").getArray().countElements(count) == error_codes::success ? count : uint64_t{};
		});

		static constexpr rt_ut::string_literal countFieldsName{ "generic_count_fields" };
		rt_ut::unit_test<countFieldsName, true>::assert_eq(static_cast<uint64_t>(5), [&] {
			uint64_t count{};
			return parser.iterate(sample).getObject().countFields(count) == error_codes::success ? count : uint64_t{};
		});

		static constexpr rt_ut::string_literal fieldKeysInOrderName{ "generic_field_keys_in_order" };
		rt_ut::unit_test<fieldKeysInOrderName, true>::assert_eq(true, [&] {
			jsonifier::string keys{};
			auto document = parser.iterate(sample);
			auto object	  = document.getObject();
			for (auto field: object) {
				keys += field.key();
				keys += ',';
			}
			return keys == "a,b,c,e~/,big,";
		});

		static constexpr rt_ut::string_literal rawJsonContainerName{ "generic_raw_json_container" };
		rt_ut::unit_test<rawJsonContainerName, true>::assert_eq(true, [&] {
			jsonifier::string_view raw{};
			return parser.iterate(sample).findField("c").rawJson(raw) == error_codes::success && raw == R"({"d": {"e": "deep"}})";
		});

		static constexpr rt_ut::string_literal noSuchFieldName{ "generic_no_such_field" };
		rt_ut::unit_test<noSuchFieldName, true>::assert_eq(static_cast<uint8_t>(error_codes::no_such_field), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).findField("zzz").error());
		});

		static constexpr rt_ut::string_literal incorrectTypeName{ "generic_incorrect_type" };
		rt_ut::unit_test<incorrectTypeName, true>::assert_eq(static_cast<uint8_t>(error_codes::incorrect_type), [&] {
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(sample).findField("a")));
		});

		static constexpr rt_ut::string_literal indexOutOfBoundsName{ "generic_index_out_of_bounds" };
		rt_ut::unit_test<indexOutOfBoundsName, true>::assert_eq(static_cast<uint8_t>(error_codes::index_out_of_bounds), [&] {
			return static_cast<uint8_t>(parser.iterate(sample).findField("b").getArray().at(8).error());
		});

		static constexpr rt_ut::string_literal unclosedContainerName{ "generic_unclosed_container" };
		rt_ut::unit_test<unclosedContainerName, true>::assert_eq(static_cast<uint8_t>(error_codes::unclosed_container), [&] {
			std::string input{ "[1,2" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		static constexpr rt_ut::string_literal mismatchedCloserName{ "generic_mismatched_closer" };
		rt_ut::unit_test<mismatchedCloserName, true>::assert_eq(false, [&] {
			std::string input{ "[1,2]]" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getArray().countElements(count));
			return doc.atEnd();
		});

		static constexpr rt_ut::string_literal trailingContentName{ "generic_trailing_content" };
		rt_ut::unit_test<trailingContentName, true>::assert_eq(false, [&] {
			std::string input{ R"({"a":1}x)" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getObject().countFields(count));
			return doc.atEnd();
		});

		static constexpr rt_ut::string_literal cleanAtEndName{ "generic_clean_document_at_end" };
		rt_ut::unit_test<cleanAtEndName, true>::assert_eq(true, [&] {
			std::string input{ R"({"a":[1,2]})" };
			auto doc = parser.iterate(input);
			uint64_t count{};
			static_cast<void>(doc.getObject().countFields(count));
			return doc.atEnd();
		});

		static constexpr rt_ut::string_literal partialNestedName{ "generic_partial_nested_then_sibling" };
		rt_ut::unit_test<partialNestedName, true>::assert_eq(static_cast<int64_t>(5), [&] {
			std::string input{ R"({"a":{"x":[1,{"y":2}],"z":3},"b":[4,5]})" };
			auto object = parser.iterate(input).getObject();
			static_cast<void>(object.findField("a").getObject().findField("x"));
			return extracted<int64_t>(object.findField("b").getArray().at(1));
		});

		static constexpr rt_ut::string_literal missingThenPresentName{ "generic_missing_then_present_field" };
		rt_ut::unit_test<missingThenPresentName, true>::assert_eq(true, [&] {
			std::string input{ R"({"a":1,"b":2,"c":3})" };
			auto object			= parser.iterate(input).getObject();
			const bool missing	= object.findField("q").error() == error_codes::no_such_field;
			const int64_t first = extracted<int64_t>(object.findField("a"));
			return missing && first == 1 && extracted<int64_t>(object.findField("c")) == 3;
		});

		static constexpr rt_ut::string_literal emptyInputName{ "generic_empty_input" };
		rt_ut::unit_test<emptyInputName, true>::assert_eq(static_cast<uint8_t>(error_codes::empty), [&] {
			std::string input{};
			return static_cast<uint8_t>(parser.iterate(input).error());
		});

		static constexpr rt_ut::string_literal missingCommaName{ "generic_missing_comma" };
		rt_ut::unit_test<missingCommaName, true>::assert_eq(static_cast<uint8_t>(error_codes::tape_error), [&] {
			std::string input{ "[1 2]" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		static constexpr rt_ut::string_literal trailingCommaName{ "generic_trailing_comma" };
		rt_ut::unit_test<trailingCommaName, true>::assert_eq(static_cast<uint8_t>(error_codes::tape_error), [&] {
			std::string input{ "[1,]" };
			auto array = parser.iterate(input).getArray();
			auto iter  = array.begin();
			++iter;
			return static_cast<uint8_t>((*iter).error());
		});

		static constexpr rt_ut::string_literal badAtomName{ "generic_bad_atom" };
		rt_ut::unit_test<badAtomName, true>::assert_eq(static_cast<uint8_t>(error_codes::t_atom_error), [&] {
			std::string input{ "[tru]" };
			return static_cast<uint8_t>(extractionError<bool>(parser.iterate(input)[0]));
		});

		static constexpr rt_ut::string_literal missingColonName{ "generic_missing_colon" };
		rt_ut::unit_test<missingColonName, true>::assert_eq(static_cast<uint8_t>(error_codes::tape_error), [&] {
			std::string input{ R"({"a" 1})" };
			return static_cast<uint8_t>(parser.iterate(input).findField("a").error());
		});

		static constexpr rt_ut::string_literal rootNumberName{ "generic_root_number" };
		rt_ut::unit_test<rootNumberName, true>::assert_eq(static_cast<int64_t>(42), [&] {
			std::string input{ "42" };
			return extracted<int64_t>(parser.iterate(input));
		});

		static constexpr rt_ut::string_literal rootStringWithWhitespaceName{ "generic_root_string_with_whitespace" };
		rt_ut::unit_test<rootStringWithWhitespaceName, true>::assert_eq(true, [&] {
			std::string input{ R"(  "s"  )" };
			return extracted<std::string_view>(parser.iterate(input)) == "s";
		});

		static constexpr rt_ut::string_literal nonAsciiStringName{ "generic_non_ascii_string" };
		rt_ut::unit_test<nonAsciiStringName, true>::assert_eq(true, [&] {
			std::string input{ "\"caf\xC3\xA9\"" };
			return extracted<std::string_view>(parser.iterate(input)) == "caf\xC3\xA9";
		});

		static constexpr rt_ut::string_literal invalidUtf8StringName{ "generic_invalid_utf8_string" };
		rt_ut::unit_test<invalidUtf8StringName, true>::assert_eq(static_cast<uint8_t>(error_codes::string_error), [&] {
			std::string input{ "\"bad\xC3\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		static constexpr rt_ut::string_literal longPlainStringName{ "generic_long_plain_string" };
		rt_ut::unit_test<longPlainStringName, true>::assert_eq(true, [&] {
			std::string payload(300, 'x');
			std::string input{ "\"" + payload + "\"" };
			return extracted<std::string_view>(parser.iterate(input)) == payload;
		});

		static constexpr rt_ut::string_literal minifiedOptionName{ "generic_minified_option" };
		rt_ut::unit_test<minifiedOptionName, true>::assert_eq(static_cast<int64_t>(3), [&] {
			std::string input{ R"({"a":[1,2,3]})" };
			return extracted<int64_t>(parser.iterate<jsonifier::parse_options{ .minified = true }>(input).findField("a").getArray().at(2));
		});

		static constexpr rt_ut::string_literal rootScalarDoubleName{ "generic_root_scalar_double" };
		rt_ut::unit_test<rootScalarDoubleName, true>::assert_eq(true, [&] {
			std::string input{ "-12.5" };
			return std::bit_cast<uint64_t>(extracted<double>(parser.iterate(input))) == std::bit_cast<uint64_t>(-12.5);
		});

		static constexpr rt_ut::string_literal rootScalarRawJsonName{ "generic_root_scalar_raw_json" };
		rt_ut::unit_test<rootScalarRawJsonName, true>::assert_eq(true, [&] {
			std::string input{ "42  " };
			jsonifier::string_view raw{};
			return parser.iterate(input).rawJson(raw) == error_codes::success && raw == "42";
		});

		static constexpr rt_ut::string_literal nonAsciiZeroCopyName{ "generic_non_ascii_zero_copy" };
		rt_ut::unit_test<nonAsciiZeroCopyName, true>::assert_eq(true, [&] {
			std::string input{ "[\"caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80 and a longer tail past one simd register\"]" };
			std::string_view out{};
			const auto errorCode = parser.iterate(input)[0].getString(out);
			return errorCode == error_codes::success && out.data() == input.data() + 2 && out == std::string_view{ input }.substr(2, input.size() - 4);
		});

		static constexpr rt_ut::string_literal overlongUtf8Name{ "generic_overlong_utf8_rejected" };
		rt_ut::unit_test<overlongUtf8Name, true>::assert_eq(static_cast<uint8_t>(error_codes::string_error), [&] {
			std::string input{ "\"bad \xC0\xAF\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		static constexpr rt_ut::string_literal surrogateUtf8Name{ "generic_surrogate_utf8_rejected" };
		rt_ut::unit_test<surrogateUtf8Name, true>::assert_eq(static_cast<uint8_t>(error_codes::string_error), [&] {
			std::string input{ "\"bad \xED\xA0\x80\"" };
			return static_cast<uint8_t>(extractionError<std::string_view>(parser.iterate(input)));
		});

		static constexpr rt_ut::string_literal escapedViewsStableName{ "generic_escaped_views_stay_valid" };
		rt_ut::unit_test<escapedViewsStableName, true>::assert_eq(true, [&] {
			std::string input{ R"(["a\nb","c\td","e\\f"])" };
			auto doc = parser.iterate(input);
			std::string_view first{};
			std::string_view second{};
			static_cast<void>(doc[0].getString(first));
			static_cast<void>(doc[1].getString(second));
			bool stable{ true };
			for (uint64_t round = 0; round < 5000; ++round) {
				std::string_view again{};
				stable = stable && doc[round % 3].getString(again) == error_codes::success;
			}
			std::string_view third{};
			static_cast<void>(doc[2].getString(third));
			return stable && first == "a\nb" && second == "c\td" && third == "e\\f";
		});

		static constexpr rt_ut::string_literal escapedKeyName{ "generic_escaped_key_matches" };
		rt_ut::unit_test<escapedKeyName, true>::assert_eq(static_cast<int64_t>(7), [&] {
			std::string input{ R"({"x":1,"a\u0062":7})" };
			return extracted<int64_t>(parser.iterate(input).findField("ab"));
		});

		static constexpr rt_ut::string_literal inOrderMissName{ "generic_in_order_miss_keeps_position" };
		rt_ut::unit_test<inOrderMissName, true>::assert_eq(true, [&] {
			std::string input{ R"({"a":{"n":1},"b":[1,2],"c":3,"d":4})" };
			auto object = parser.iterate(input).getObject();
			const jsonifier::generic::generic_value<false> first{ object.findField("a") };
			const bool nested = extracted<int64_t>(first.findField("n")) == 1;
			const bool miss	  = object.findField("zz").error() == error_codes::no_such_field;
			const bool hit	  = extracted<int64_t>(object.findField("c")) == 3;
			const bool later  = extracted<int64_t>(object.findField("d")) == 4;
			return nested && miss && hit && later;
		});

		static constexpr rt_ut::string_literal largeObjectName{ "generic_in_order_large_object" };
		rt_ut::unit_test<largeObjectName, true>::assert_eq(true, [&] {
			std::string input{ "{" };
			for (uint64_t index = 0; index < 300; ++index) {
				input += (index ? ",\"field_" : "\"field_") + std::to_string(index) + "\":" + std::to_string(index * 3);
			}
			input += "}";
			auto object = parser.iterate(input).getObject();
			bool allMatch{ true };
			for (uint64_t index = 0; index < 300; ++index) {
				allMatch = allMatch && extracted<int64_t>(object.findField("field_" + std::to_string(index))) == static_cast<int64_t>(index * 3);
			}
			return allMatch;
		});

		static constexpr rt_ut::string_literal repeatedKeysAcrossObjectsName{ "generic_same_keys_in_different_objects" };
		rt_ut::unit_test<repeatedKeysAcrossObjectsName, true>::assert_eq(true, [&] {
			std::string input{ R"([{"a":1,"b":2,"c":3},{"a":10,"b":20,"c":30}])" };
			auto array	  = parser.iterate(input).getArray();
			auto first	  = array.at(0).getObject();
			const bool a  = extracted<int64_t>(first.findField("a")) == 1;
			const bool c  = extracted<int64_t>(first.findField("c")) == 3;
			auto second	  = array.at(1).getObject();
			const bool a2 = extracted<int64_t>(second.findField("a")) == 10;
			const bool b2 = extracted<int64_t>(second.findField("b")) == 20;
			const bool c2 = extracted<int64_t>(second.findField("c")) == 30;
			return a && c && a2 && b2 && c2;
		});

		static constexpr rt_ut::string_literal repeatedMissName{ "generic_repeated_misses_keep_position" };
		rt_ut::unit_test<repeatedMissName, true>::assert_eq(true, [&] {
			std::string input{ R"({"a":1,"b":{"n":[1,2,3]},"c":3})" };
			auto object			  = parser.iterate(input).getObject();
			const bool firstMiss  = object.findField("q").error() == error_codes::no_such_field;
			const bool secondMiss = object.findField("r").error() == error_codes::no_such_field;
			const bool a		  = extracted<int64_t>(object.findField("a")) == 1;
			const jsonifier::generic::generic_value<false> nested{ object.findField("b") };
			const bool n		  = extracted<int64_t>(nested.findField("n").getArray().at(2)) == 3;
			return firstMiss && secondMiss && a && n && extracted<int64_t>(object.findField("c")) == 3;
		});

		static constexpr rt_ut::string_literal longKeysReverseName{ "generic_in_order_long_keys" };
		rt_ut::unit_test<longKeysReverseName, true>::assert_eq(true, [&] {
			std::string input{ R"({"a_rather_long_key_name_number_one":1,"a_rather_long_key_name_number_two":2,"a_rather_long_key_name_number_three":3})" };
			auto object		 = parser.iterate(input).getObject();
			const bool one	 = extracted<int64_t>(object.findField("a_rather_long_key_name_number_one")) == 1;
			const bool two	 = extracted<int64_t>(object.findField("a_rather_long_key_name_number_two")) == 2;
			const bool three = extracted<int64_t>(object.findField("a_rather_long_key_name_number_three")) == 3;
			return three && two && one;
		});

		static constexpr rt_ut::string_literal prettifiedReverseName{ "generic_in_order_prettified" };
		rt_ut::unit_test<prettifiedReverseName, true>::assert_eq(true, [&] {
			std::string input{ "{\n  \"alpha\" : 1,\n  \"beta\"  : 2,\n  \"gamma\" : 3\n}" };
			auto object		 = parser.iterate(input).getObject();
			const bool alpha = extracted<int64_t>(object.findField("alpha")) == 1;
			const bool beta	 = extracted<int64_t>(object.findField("beta")) == 2;
			const bool gamma = extracted<int64_t>(object.findField("gamma")) == 3;
			return gamma && alpha && beta;
		});

		static constexpr rt_ut::string_literal pointerOverflowName{ "generic_json_pointer_index_overflow" };
		rt_ut::unit_test<pointerOverflowName, true>::assert_eq(static_cast<uint8_t>(error_codes::index_out_of_bounds), [&] {
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
