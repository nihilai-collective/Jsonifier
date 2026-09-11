// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/json_iterator.hpp
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/simd.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/error.hpp>

namespace jsonifier::internal {

	template<parse_options options> struct string_parser;
	template<string_literal stringNew> JSONIFIER_INLINE static bool compareStringAsInt(string_view_ptr src);

	template<typename basic_iterator01> [[maybe_unused]] JSONIFIER_INLINE static void skipStringImpl(basic_iterator01& string1, uint64_t lengthNew) noexcept;

	template<parse_options parseOpts, typename iterator_type, typename string_buffer_type> struct json_iterator;

	enum class sep_result : uint8_t {
		cont,
		ended,
		error,
	};

	template<parse_options parseOpts, typename string_buffer_type> struct json_iterator<parseOpts, string_view_ptr, string_buffer_type> {
	  protected:
		string_buffer_type* stringBuffer{};
		uint64_t currentObjectDepth{};
		uint64_t currentArrayDepth{};
		std::vector<error>* errors{};
		string_view_ptr rootIter{};
		string_view_ptr endIter{};
		string_view_ptr iter{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew) noexcept : stringBuffer{ stringBufferNew }, rootIter{}, endIter{}, iter{} {
		}

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, string_view_ptr rootIterNew, string_view_ptr endIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew }, iter{ rootIterNew } {
		}

		JSONIFIER_INLINE string_view_ptr& currentPtr() noexcept {
			return iter;
		}

		JSONIFIER_INLINE string_view_ptr endPtr() const noexcept {
			return endIter;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		JSONIFIER_INLINE bool hasMoreInput() noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>();
		}

		JSONIFIER_INLINE bool anyInput() noexcept {
			return rootIter && rootIter != endIter ? true : reject<parse_statuses::no_input>();
		}

		JSONIFIER_INLINE bool checkIfDoneImpl() noexcept {
			return currentObjectDepth == 0 && currentArrayDepth == 0 && iter >= endIter;
		}

		JSONIFIER_INLINE bool checkIfDone() noexcept {
			return checkIfDoneImpl() ? true : reject<parse_statuses::unfinished_input>();
		}

		JSONIFIER_INLINE bool notAtEndPre() noexcept {
			return iter < endIter;
		}

		JSONIFIER_INLINE bool notAtEnd() noexcept {
			return checkCurrentDepth() && notAtEndPre();
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkCharUnsafe() noexcept {
			if constexpr (charToCheck == ']') {
				return *iter == charToCheck ? (static_cast<void>(--currentArrayDepth), true) : false;
			} else if constexpr (charToCheck == '}') {
				return *iter == charToCheck ? (static_cast<void>(--currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '{') {
				return *iter == charToCheck ? (static_cast<void>(++currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '[') {
				return *iter == charToCheck ? (static_cast<void>(++currentArrayDepth), true) : false;
			} else {
				return *iter == charToCheck;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkChar() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if constexpr (charToCheck == '{' || charToCheck == '[') {
					return checkCurrentDepth() ? checkCharUnsafe<charToCheck>() : false;
				} else {
					return checkCharUnsafe<charToCheck>();
				}
			} else {
				if constexpr (charToCheck == '{' || charToCheck == '[') {
					return notAtEnd() ? checkCharUnsafe<charToCheck>() : false;
				} else {
					return notAtEndPre() ? checkCharUnsafe<charToCheck>() : false;
				}
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEquals() noexcept {
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		JSONIFIER_INLINE bool checkCurrentDepth() noexcept {
			return currentObjectDepth + currentArrayDepth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>();
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject() noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(rootIter, iter, endIter));
			return false;
		}

		JSONIFIER_INLINE bool skipString() noexcept {
			++iter;
			skipStringImpl(iter, static_cast<uint64_t>(endIter - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '"') [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < endIter && *iter == '"') [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>();
		}

		JSONIFIER_INLINE bool skipValue() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
			} else {
				if (iter >= endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
			}
			switch (*iter) {
				case '"': {
					return skipString();
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString()) [[unlikely]] {
									return false;
								}
								continue;
							}
							++iter;
							if (c == '{' || c == '[') {
								++depth;
							} else if (c == '}' || c == ']') {
								if (--depth == 0) {
									return true;
								}
							}
						}
					} else {
						while (iter < endIter) {
							const char c = *iter;
							if (c == '"') {
								if (!skipString()) [[unlikely]] {
									return false;
								}
								continue;
							}
							++iter;
							if (c == '{' || c == '[') {
								++depth;
							} else if (c == '}' || c == ']') {
								if (--depth == 0) {
									return true;
								}
							}
						}
					}
					return reject<parse_statuses::unexpected_string_end>();
				}
				default: {
					if constexpr (parseOpts.nullTerminated) {
						while (true) {
							const char c = *iter;
							if (c == ',' || c == ']' || c == '}' || c == '\0' || whitespaceTable[static_cast<uint8_t>(c)]) {
								return true;
							}
							++iter;
						}
					} else {
						while (iter < endIter) {
							const char c = *iter;
							if (c == ',' || c == ']' || c == '}' || whitespaceTable[static_cast<uint8_t>(c)]) {
								return true;
							}
							++iter;
						}
						return true;
					}
				}
			}
		}

		JSONIFIER_INLINE bool skipRemainingObject() noexcept {
			while (true) {
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>();
					}
				} else {
					if (iter >= endIter || *iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>();
					}
				}
				if (!skipString()) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon()) [[unlikely]] {
					return false;
				}
				if (!skipValue()) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd()) {
					return true;
				}
				if (!collectObjectComma()) [[unlikely]] {
					return false;
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type = number_type;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = endIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						iter  = iterNew;
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						iter = iterNew;
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (endIter - ptr == 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				iter += 4;
				return true;
			} else if (endIter - ptr == 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				iter += 5;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool arrayMaybeEnd() noexcept {
			return incrementIfEquals<']'>();
		}

		JSONIFIER_INLINE bool collectArrayComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool objectMaybeEnd() noexcept {
			return incrementIfEquals<'}'>();
		}

		JSONIFIER_INLINE bool collectObjectComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool collectObjectColon() noexcept {
			return incrementIfEquals<':'>() ? true : reject<parse_statuses::missing_colon>();
		}

		JSONIFIER_INLINE bool objectStart() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStart() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		JSONIFIER_INLINE bool objectStartRoot() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStartRoot() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
			if (endIter - iter >= 4 && compareStringAsInt<"true">(iter)) {
				value = true;
				iter += 4;
				return true;
			} else if (endIter - iter >= 5 && compareStringAsInt<"fals">(iter) && iter[4] == 'e') {
				value = false;
				iter += 5;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool iterateNull() noexcept {
			if (endIter - iter >= 4 && compareStringAsInt<"null">(iter)) [[likely]] {
				iter += 4;
				return true;
			} else {
				return reject<parse_statuses::invalid_null_value>();
			}
		}

		template<string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const auto iterStart = iter;
			auto& scratch		 = getStringBuffer();
			const auto needed	 = static_cast<uint64_t>(endIter - iter) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto res = string_scanner<parseOpts>::impl(iter, endIter, scratch.data());
			if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				iter = iterStart;
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (has_resize<string_type>) {
				if (value.size() != res.outLength) [[unlikely]] {
					value.resize(res.outLength);
				}
			}
			std::memcpy(value.data(), scratch.data(), res.outLength);
			iter += res.rawLength + 1;
			if (iter > endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			return true;
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					--currentObjectDepth;
					++iter;
					return sep_result::ended;
				}
			} else {
				if (notAtEndPre()) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == '}') {
						--currentObjectDepth;
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}

		JSONIFIER_INLINE sep_result collectArraySeparator() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					--currentArrayDepth;
					++iter;
					return sep_result::ended;
				}
			} else {
				if (notAtEndPre()) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == ']') {
						--currentArrayDepth;
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}
	};

	template<parse_options parseOpts, typename string_buffer_type>
		requires(!parseOpts.minified)
	struct json_iterator<parseOpts, string_view_ptr, string_buffer_type> {
	  protected:
		base_t<decltype(*string_view_ptr{})> wsChar{};
		string_buffer_type* stringBuffer{};
		uint64_t currentObjectDepth{};
		uint64_t currentArrayDepth{};
		std::vector<error>* errors{};
		string_view_ptr rootIter{};
		string_view_ptr endIter{};
		string_view_ptr iter{};
		uint64_t indentSize{};
		bool indentKnown{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew) noexcept : stringBuffer{ stringBufferNew }, rootIter{}, endIter{}, iter{} {
		}

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, string_view_ptr rootIterNew, string_view_ptr endIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew }, iter{ rootIterNew } {
		}

		JSONIFIER_INLINE uint64_t currentDepth() const noexcept {
			return currentObjectDepth + currentArrayDepth;
		}

		JSONIFIER_INLINE bool atWhitespace() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return whitespaceTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < endIter && whitespaceTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE bool atNewline() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return newlineTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < endIter && newlineTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE string_view_ptr skipNewline(string_view_ptr iterLocal) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (newlineTable[static_cast<uint8_t>(*iterLocal)]) {
					++iterLocal;
				}
			} else {
				while (iterLocal < endIter && newlineTable[static_cast<uint8_t>(*iterLocal)]) {
					++iterLocal;
				}
			}
			return iterLocal;
		}

		JSONIFIER_INLINE uint64_t swarBroadcast() const noexcept {
			return static_cast<uint64_t>(static_cast<uint8_t>(wsChar)) * 0x0101010101010101ull;
		}

		template<uint_types value_type> JSONIFIER_INLINE bool swarCmp(string_view_ptr iterLocal, uint64_t fill) noexcept {
			value_type chunk;
			std::memcpy(&chunk, iterLocal, sizeof(value_type));
			return (chunk ^ fill) == 0;
		}

		struct indent_result {
			string_view_ptr pos;
			bool matched;
		};

		JSONIFIER_INLINE indent_result spanIsIndent(string_view_ptr iterLocal, uint64_t count) noexcept {
			const uint64_t fill{ swarBroadcast() };
			if (count < 16) [[likely]] {
				if (count >= 8) {
					uint64_t chunk;
					std::memcpy(&chunk, iterLocal, 8);
					const uint64_t diff{ chunk ^ fill };
					if (diff) {
						return { iterLocal + (simd::countrZero(diff) >> 3), false };
					}
					iterLocal += 8;
					count -= 8;
				}
				return swarCmpTail(iterLocal, count, fill);
			}
			uint64_t remaining{ count };
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
			if (remaining >= 32) {
				const jsonifier_simd_int_256 charValue{ gatherValue<jsonifier_simd_int_256>(wsChar) };
				while (remaining >= 32) {
					const jsonifier_simd_int_256 iterValues{ gatherValuesU<jsonifier_simd_int_256>(iterLocal) };
					const uint32_t mask{ static_cast<uint32_t>(opCmpEq(charValue, iterValues)) };
					if (mask != std::numeric_limits<uint32_t>::max()) {
						return { iterLocal + simd::countrZero(static_cast<uint32_t>(~mask)), false };
					}
					remaining -= 32;
					iterLocal += 32;
				}
			}
#endif
			if (remaining >= 16) {
				const jsonifier_simd_int_128 charValue{ gatherValue<jsonifier_simd_int_128>(wsChar) };
				while (remaining >= 16) {
					const jsonifier_simd_int_128 iterValues{ gatherValuesU<jsonifier_simd_int_128>(iterLocal) };
					const uint16_t mask{ static_cast<uint16_t>(opCmpEq(charValue, iterValues)) };
					if (mask != std::numeric_limits<uint16_t>::max()) {
						return { iterLocal + simd::countrZero(static_cast<uint32_t>(static_cast<uint16_t>(~mask))), false };
					}
					remaining -= 16;
					iterLocal += 16;
				}
			}
			if (remaining >= 8) {
				uint64_t chunk;
				std::memcpy(&chunk, iterLocal, 8);
				const uint64_t diff{ chunk ^ fill };
				if (diff) {
					return { iterLocal + (simd::countrZero(diff) >> 3), false };
				}
				iterLocal += 8;
				remaining -= 8;
			}
			return swarCmpTail(iterLocal, remaining, fill);
		}

		JSONIFIER_INLINE indent_result swarCmpTail(string_view_ptr iterLocal, uint64_t remaining, uint64_t fill) noexcept {
			if (remaining & 4) {
				uint32_t chunk;
				std::memcpy(&chunk, iterLocal, 4);
				const uint32_t diff{ chunk ^ static_cast<uint32_t>(fill) };
				if (diff) {
					return { iterLocal + (simd::countrZero(diff) >> 3), false };
				}
				iterLocal += 4;
			}
			if (remaining & 2) {
				uint16_t chunk;
				std::memcpy(&chunk, iterLocal, 2);
				const uint16_t diff{ static_cast<uint16_t>(chunk ^ static_cast<uint16_t>(fill)) };
				if (diff) {
					return { iterLocal + (simd::countrZero(static_cast<uint32_t>(diff)) >> 3), false };
				}
				iterLocal += 2;
			}
			if (remaining & 1) {
				return { iterLocal, *iterLocal == wsChar };
			}
			return { iterLocal, true };
		}

		JSONIFIER_INLINE void skipWhitespaceScalar() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (whitespaceTable[static_cast<uint8_t>(*iter)]) {
					++iter;
				}
			} else {
				while (iter < endIter && whitespaceTable[static_cast<uint8_t>(*iter)]) {
					++iter;
				}
			}
		}

		JSONIFIER_INLINE void skipWhitespace() noexcept {
			skipWhitespaceScalar();
		}

		JSONIFIER_INLINE void skipWhitespacePredicted() noexcept {
			if (indentKnown && atNewline()) [[likely]] {
				const string_view_ptr probe{ skipNewline(iter) };
				const uint64_t predicted{ indentSize * currentDepth() };
				if (probe + predicted < endIter) [[likely]] {
					const indent_result res{ spanIsIndent(probe, predicted) };
					iter = res.pos;
					if (res.matched && !whitespaceTable[static_cast<uint8_t>(*iter)]) [[likely]] {
						return;
					}
					skipWhitespaceScalar();
					return;
				}
				iter = probe;
			}
			skipWhitespaceScalar();
		}

		JSONIFIER_INLINE void collectIndentSizeRoot() noexcept {
			if (!atNewline()) {
				return;
			}
			string_view_ptr probe{ skipNewline(iter) };
			if (probe >= endIter || !whitespaceTable[static_cast<uint8_t>(*probe)]) {
				return;
			}
			wsChar = *probe;
			uint64_t count{};
			while (probe < endIter && *probe == wsChar) {
				++probe;
				++count;
			}
			indentSize	= count;
			indentKnown = count > 0;
		}

		JSONIFIER_INLINE void skipWhitespaceRoot() noexcept {
			collectIndentSizeRoot();
			skipWhitespaceScalar();
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkCharUnsafe() noexcept {
			if constexpr (charToCheck == ']') {
				return *iter == charToCheck ? (static_cast<void>(--currentArrayDepth), true) : false;
			} else if constexpr (charToCheck == '}') {
				return *iter == charToCheck ? (static_cast<void>(--currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '{') {
				return *iter == charToCheck ? (static_cast<void>(++currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '[') {
				return *iter == charToCheck ? (static_cast<void>(++currentArrayDepth), true) : false;
			} else {
				return *iter == charToCheck;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkChar() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if constexpr (charToCheck == '{' || charToCheck == '[') {
					return checkCurrentDepth() ? checkCharUnsafe<charToCheck>() : false;
				} else {
					return checkCharUnsafe<charToCheck>();
				}
			} else {
				if constexpr (charToCheck == '{' || charToCheck == '[') {
					return notAtEnd() ? checkCharUnsafe<charToCheck>() : false;
				} else {
					return notAtEndPre() ? checkCharUnsafe<charToCheck>() : false;
				}
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEqualsNoWs() noexcept {
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEquals() noexcept {
			skipWhitespace();
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEqualsNoWsFirst() noexcept {
			if (incrementIfEqualsNoWs<charToCheck>()) [[likely]] {
				return true;
			}
			if (atWhitespace()) [[unlikely]] {
				skipWhitespace();
				return incrementIfEqualsNoWs<charToCheck>();
			}
			return false;
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEqualsRoot() noexcept {
			if (!incrementIfEqualsNoWs<charToCheck>()) [[unlikely]] {
				if (!atWhitespace()) {
					return false;
				}
				skipWhitespaceScalar();
				if (!incrementIfEqualsNoWs<charToCheck>()) {
					return false;
				}
			}
			skipWhitespaceRoot();
			return true;
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			if (atWhitespace()) [[unlikely]] {
				skipWhitespace();
			}
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespacePredicted();
					return sep_result::cont;
				}
				if (c == '}') {
					--currentObjectDepth;
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < endIter) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						skipWhitespacePredicted();
						return sep_result::cont;
					}
					if (c == '}') {
						--currentObjectDepth;
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}

		JSONIFIER_INLINE sep_result collectArraySeparator() noexcept {
			skipWhitespace();
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespacePredicted();
					return sep_result::cont;
				}
				if (c == ']') {
					--currentArrayDepth;
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < endIter) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						skipWhitespacePredicted();
						return sep_result::cont;
					}
					if (c == ']') {
						--currentArrayDepth;
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}

		JSONIFIER_INLINE string_view_ptr& currentPtr() noexcept {
			return iter;
		}

		JSONIFIER_INLINE string_view_ptr endPtr() const noexcept {
			return endIter;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		JSONIFIER_INLINE bool hasMoreInput() noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>();
		}

		JSONIFIER_INLINE bool anyInput() noexcept {
			return rootIter && rootIter != endIter ? true : reject<parse_statuses::no_input>();
		}

		JSONIFIER_INLINE bool checkIfDoneImpl() noexcept {
			if (atWhitespace()) [[unlikely]] {
				skipWhitespace();
			}
			return currentObjectDepth == 0 && currentArrayDepth == 0 && iter >= endIter;
		}

		JSONIFIER_INLINE bool checkIfDone() noexcept {
			return checkIfDoneImpl() ? true : reject<parse_statuses::unfinished_input>();
		}

		JSONIFIER_INLINE bool notAtEndPre() noexcept {
			return iter < endIter;
		}

		JSONIFIER_INLINE bool notAtEnd() noexcept {
			return checkCurrentDepth() && notAtEndPre();
		}

		JSONIFIER_INLINE bool checkCurrentDepth() noexcept {
			return currentObjectDepth + currentArrayDepth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>();
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject() noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(rootIter, iter, endIter));
			return false;
		}

		JSONIFIER_INLINE bool skipString() noexcept {
			++iter;
			skipStringImpl(iter, static_cast<uint64_t>(endIter - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '"') [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < endIter && *iter == '"') [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>();
		}

		JSONIFIER_INLINE bool skipValue() noexcept {
			skipWhitespace();
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
			} else {
				if (iter >= endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
			}
			switch (*iter) {
				case '"': {
					return skipString();
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString()) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar();
								continue;
							}
							++iter;
							if (c == '{' || c == '[') {
								++depth;
							} else if (c == '}' || c == ']') {
								if (--depth == 0) {
									return true;
								}
							}
						}
					} else {
						while (iter < endIter) {
							const char c = *iter;
							if (c == '"') {
								if (!skipString()) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar();
								continue;
							}
							++iter;
							if (c == '{' || c == '[') {
								++depth;
							} else if (c == '}' || c == ']') {
								if (--depth == 0) {
									return true;
								}
							}
						}
					}
					return reject<parse_statuses::unexpected_string_end>();
				}
				default: {
					if constexpr (parseOpts.nullTerminated) {
						while (true) {
							const char c = *iter;
							if (c == ',' || c == ']' || c == '}' || c == '\0' || whitespaceTable[static_cast<uint8_t>(c)]) {
								return true;
							}
							++iter;
						}
					} else {
						while (iter < endIter) {
							const char c = *iter;
							if (c == ',' || c == ']' || c == '}' || whitespaceTable[static_cast<uint8_t>(c)]) {
								return true;
							}
							++iter;
						}
						return true;
					}
				}
			}
		}

		JSONIFIER_INLINE bool skipRemainingObject() noexcept {
			while (true) {
				if (atWhitespace()) [[unlikely]] {
					skipWhitespace();
				}
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>();
					}
				} else {
					if (iter >= endIter || *iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>();
					}
				}
				if (!skipString()) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon()) [[unlikely]] {
					return false;
				}
				if (!skipValue()) [[unlikely]] {
					return false;
				}
				switch (static_cast<uint64_t>(collectObjectSeparator())) {
					case static_cast<uint64_t>(sep_result::cont): {
						continue;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return true;
					}
					default: {
						return false;
					}
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type = number_type;
			if (atWhitespace()) [[unlikely]] {
				skipWhitespace();
			}
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = endIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							skipWhitespaceScalar();
							return true;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespaceScalar();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							skipWhitespaceScalar();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespaceScalar();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						iter  = iterNew;
						skipWhitespaceScalar();
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						iter = iterNew;
						skipWhitespaceScalar();
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (endIter - ptr == 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				iter += 4;
				return true;
			} else if (endIter - ptr == 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				iter += 5;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool arrayMaybeEnd() noexcept {
			return incrementIfEquals<']'>();
		}

		JSONIFIER_INLINE bool collectArrayComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool objectMaybeEnd() noexcept {
			return incrementIfEquals<'}'>();
		}

		JSONIFIER_INLINE bool collectObjectComma() noexcept {
			return incrementIfEqualsNoWs<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool collectObjectColon() noexcept {
			return incrementIfEqualsNoWsFirst<':'>() ? true : reject<parse_statuses::missing_colon>();
		}

		JSONIFIER_INLINE bool objectStart() noexcept {
			if (!incrementIfEquals<'{'>()) {
				return reject<parse_statuses::missing_object_start>();
			}
			skipWhitespacePredicted();
			return true;
		}

		JSONIFIER_INLINE bool arrayStart() noexcept {
			if (!incrementIfEquals<'['>()) {
				return reject<parse_statuses::missing_array_start>();
			}
			skipWhitespacePredicted();
			return true;
		}

		JSONIFIER_INLINE bool objectStartRoot() noexcept {
			return incrementIfEqualsRoot<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStartRoot() noexcept {
			return incrementIfEqualsRoot<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
			skipWhitespace();
			if (endIter - iter >= 4 && compareStringAsInt<"true">(iter)) {
				value = true;
				iter += 4;
				return true;
			} else if (endIter - iter >= 5 && compareStringAsInt<"fals">(iter) && iter[4] == 'e') {
				value = false;
				iter += 5;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool iterateNull() noexcept {
			skipWhitespace();
			if (endIter - iter >= 4 && compareStringAsInt<"null">(iter)) [[likely]] {
				iter += 4;
				return true;
			} else {
				return reject<parse_statuses::invalid_null_value>();
			}
		}

		template<string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const auto iterStart = iter;
			auto& scratch		 = getStringBuffer();
			const auto needed	 = static_cast<uint64_t>(endIter - iter) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto res = string_scanner<parseOpts>::impl(iter, endIter, scratch.data());
			if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				iter = iterStart;
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (has_resize<string_type>) {
				if (value.size() != res.outLength) [[unlikely]] {
					value.resize(res.outLength);
				}
			}
			std::memcpy(value.data(), scratch.data(), res.outLength);
			iter += res.rawLength + 1;
			if (iter > endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			return true;
		}
	};

	static constexpr auto validPostPrimitiveTable{ [] {
		array<bool, 256> table{};
		table[static_cast<uint8_t>(',')]  = true;
		table[static_cast<uint8_t>('}')]  = true;
		table[static_cast<uint8_t>(']')]  = true;
		table[static_cast<uint8_t>(' ')]  = true;
		table[static_cast<uint8_t>('\t')] = true;
		table[static_cast<uint8_t>('\n')] = true;
		table[static_cast<uint8_t>('\r')] = true;
		return table;
	}() };

	template<parse_options parseOpts, typename string_buffer_type> struct json_iterator<parseOpts, structural_index_ptr, string_buffer_type> {
	  protected:
		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		structural_index_ptr rootIter{};
		structural_index_ptr endIter{};
		structural_index_ptr iter{};
		string_view_ptr stringRootIter{};
		string_view_ptr stringEndIter{};
		uint64_t currentArrayDepth{};
		uint64_t currentObjectDepth{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, structural_index_ptr rootIterNew, structural_index_ptr endIterNew,
			structural_index_ptr iterNew, string_view_ptr stringRootIterNew, string_view_ptr stringEndIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew }, iter{ iterNew }, stringRootIter{ stringRootIterNew },
			  stringEndIter{ stringEndIterNew } {
		}

		JSONIFIER_INLINE bool anyInput() noexcept {
			return stringRootIter && stringRootIter != stringEndIter ? true : reject<parse_statuses::no_input>();
		}

		JSONIFIER_INLINE structural_index_ptr& currentIterPtr() noexcept {
			return iter;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		JSONIFIER_INLINE string_view_ptr currentPtr() noexcept {
			return stringRootIter + *iter;
		}

		JSONIFIER_INLINE string_view_ptr endPtr() const noexcept {
			return stringEndIter;
		}

		JSONIFIER_INLINE structural_index_ptr endIterPtr() const noexcept {
			return endIter;
		}

		JSONIFIER_INLINE bool hasMoreInput() noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>();
		}

		JSONIFIER_INLINE bool checkIfDoneImpl() noexcept {
			if (stringRootIter && iter) {
				if (iter >= endIter) {
					return currentArrayDepth == 0 && currentObjectDepth == 0;
				}
				return &stringRootIter[*iter] == stringEndIter && currentArrayDepth == 0 && currentObjectDepth == 0;
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE bool checkIfDone() noexcept {
			return checkIfDoneImpl() ? true : reject<parse_statuses::unfinished_input>();
		}

		JSONIFIER_INLINE bool notAtEndPre() noexcept {
			return iter < endIter;
		}

		JSONIFIER_INLINE bool notAtEnd() noexcept {
			return checkCurrentDepth() && notAtEndPre();
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkCharUnsafe() noexcept {
			if constexpr (charToCheck == ']') {
				return *currentPtr() == charToCheck ? (static_cast<void>(--currentArrayDepth), true) : false;
			} else if constexpr (charToCheck == '}') {
				return *currentPtr() == charToCheck ? (static_cast<void>(--currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '{') {
				return *currentPtr() == charToCheck ? (static_cast<void>(++currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '[') {
				return *currentPtr() == charToCheck ? (static_cast<void>(++currentArrayDepth), true) : false;
			} else {
				return *currentPtr() == charToCheck;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkChar() noexcept {
			if constexpr (charToCheck == '{' || charToCheck == '[') {
				return notAtEnd() ? checkCharUnsafe<charToCheck>() : false;
			} else {
				return notAtEndPre() ? checkCharUnsafe<charToCheck>() : false;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEquals() noexcept {
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		JSONIFIER_INLINE bool checkCurrentDepth() noexcept {
			return currentObjectDepth + currentArrayDepth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>();
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject() noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(stringRootIter, &stringRootIter[*iter], stringEndIter));
			return false;
		}

		JSONIFIER_INLINE bool skipString() noexcept {
			++iter;
			return true;
		}

		JSONIFIER_INLINE bool skipValue() noexcept {
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const char first = static_cast<char>(*currentPtr());
			if (first == '{' || first == '[') {
				uint64_t depth{};
				while (iter < endIter) {
					const char c = static_cast<char>(stringRootIter[*iter]);
					++iter;
					if (c == '{' || c == '[') {
						++depth;
					} else if (c == '}' || c == ']') {
						if (--depth == 0) {
							return true;
						}
					}
				}
				return reject<parse_statuses::unexpected_string_end>();
			}
			++iter;
			return true;
		}

		JSONIFIER_INLINE bool skipRemainingObject() noexcept {
			while (true) {
				if (iter >= endIter || *currentPtr() != '"') [[unlikely]] {
					return reject<parse_statuses::missing_key_start>();
				}
				++iter;
				if (!collectObjectColon()) [[unlikely]] {
					return false;
				}
				if (!skipValue()) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd()) {
					return true;
				}
				if (!collectObjectComma()) [[unlikely]] {
					return false;
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = (iter + 1) < endIter ? stringRootIter + *(iter + 1) : stringEndIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						++iter;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = stringEndIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						++iter;
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						++iter;
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr == 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				++iter;
				return true;
			} else if (stringEndIter - ptr == 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				++iter;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool objectStart() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStart() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		JSONIFIER_INLINE bool objectStartRoot() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStartRoot() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		JSONIFIER_INLINE bool arrayMaybeEnd() noexcept {
			return incrementIfEquals<']'>();
		}

		JSONIFIER_INLINE bool collectArrayComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool objectMaybeEnd() noexcept {
			return incrementIfEquals<'}'>();
		}

		JSONIFIER_INLINE bool collectObjectComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool collectObjectColon() noexcept {
			return incrementIfEquals<':'>() ? true : reject<parse_statuses::missing_colon>();
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr >= 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				++iter;
				return true;
			} else if (stringEndIter - ptr >= 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				++iter;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool iterateNull() noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr >= 4 && compareStringAsInt<"null">(ptr)) [[likely]] {
				++iter;
				return true;
			} else {
				return reject<parse_statuses::invalid_null_value>();
			}
		}

		template<string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			string_view_ptr strPtr = currentPtr() + 1;
			if (strPtr >= stringEndIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			auto& scratch	  = getStringBuffer();
			const auto needed = static_cast<uint64_t>(stringEndIter - strPtr) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto res = string_scanner<parseOpts>::impl(strPtr, stringEndIter, scratch.data());
			if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (has_resize<string_type>) {
				if (value.size() != res.outLength) [[unlikely]] {
					value.resize(res.outLength);
				}
			}
			std::memcpy(value.data(), scratch.data(), res.outLength);
			++iter;
			return true;
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			if (notAtEndPre()) [[likely]] {
				const char c = static_cast<char>(*currentPtr());
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					--currentObjectDepth;
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}

		JSONIFIER_INLINE sep_result collectArraySeparator() noexcept {
			if (notAtEndPre()) [[likely]] {
				const char c = static_cast<char>(*currentPtr());
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					--currentArrayDepth;
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}
	};

	template<parse_options parseOpts, typename string_buffer_type>
		requires(!parseOpts.minified)
	struct json_iterator<parseOpts, structural_index_ptr, string_buffer_type> {
	  protected:
		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		structural_index_ptr rootIter{};
		structural_index_ptr endIter{};
		structural_index_ptr iter{};
		string_view_ptr stringRootIter{};
		string_view_ptr stringEndIter{};
		uint64_t currentArrayDepth{};
		uint64_t currentObjectDepth{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, structural_index_ptr rootIterNew, structural_index_ptr endIterNew,
			structural_index_ptr iterNew, string_view_ptr stringRootIterNew, string_view_ptr stringEndIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew }, iter{ iterNew }, stringRootIter{ stringRootIterNew },
			  stringEndIter{ stringEndIterNew } {
		}

		JSONIFIER_INLINE bool anyInput() noexcept {
			return stringRootIter && stringRootIter != stringEndIter ? true : reject<parse_statuses::no_input>();
		}

		JSONIFIER_INLINE structural_index_ptr& currentIterPtr() noexcept {
			return iter;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		JSONIFIER_INLINE string_view_ptr currentPtr() noexcept {
			return stringRootIter + *iter;
		}

		JSONIFIER_INLINE string_view_ptr endPtr() const noexcept {
			return stringEndIter;
		}

		JSONIFIER_INLINE structural_index_ptr endIterPtr() const noexcept {
			return endIter;
		}

		JSONIFIER_INLINE bool hasMoreInput() noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>();
		}

		JSONIFIER_INLINE bool checkIfDoneImpl() noexcept {
			if (stringRootIter && iter) {
				if (iter >= endIter) {
					return currentArrayDepth == 0 && currentObjectDepth == 0;
				}
				return &stringRootIter[*iter] == stringEndIter && currentArrayDepth == 0 && currentObjectDepth == 0;
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE bool checkIfDone() noexcept {
			return checkIfDoneImpl() ? true : reject<parse_statuses::unfinished_input>();
		}

		JSONIFIER_INLINE bool notAtEndPre() noexcept {
			return iter < endIter;
		}

		JSONIFIER_INLINE bool notAtEnd() noexcept {
			return checkCurrentDepth() && notAtEndPre();
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkCharUnsafe() noexcept {
			if constexpr (charToCheck == ']') {
				return *currentPtr() == charToCheck ? (static_cast<void>(--currentArrayDepth), true) : false;
			} else if constexpr (charToCheck == '}') {
				return *currentPtr() == charToCheck ? (static_cast<void>(--currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '{') {
				return *currentPtr() == charToCheck ? (static_cast<void>(++currentObjectDepth), true) : false;
			} else if constexpr (charToCheck == '[') {
				return *currentPtr() == charToCheck ? (static_cast<void>(++currentArrayDepth), true) : false;
			} else {
				return *currentPtr() == charToCheck;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool checkChar() noexcept {
			if constexpr (charToCheck == '{' || charToCheck == '[') {
				return notAtEnd() ? checkCharUnsafe<charToCheck>() : false;
			} else {
				return notAtEndPre() ? checkCharUnsafe<charToCheck>() : false;
			}
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEquals() noexcept {
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		JSONIFIER_INLINE bool checkCurrentDepth() noexcept {
			return currentObjectDepth + currentArrayDepth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>();
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject() noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(stringRootIter, &stringRootIter[*iter], stringEndIter));
			return false;
		}

		JSONIFIER_INLINE bool skipString() noexcept {
			++iter;
			return true;
		}

		JSONIFIER_INLINE bool skipValue() noexcept {
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const char first = static_cast<char>(*currentPtr());
			if (first == '{' || first == '[') {
				uint64_t depth{};
				while (iter < endIter) {
					const char c = static_cast<char>(stringRootIter[*iter]);
					++iter;
					if (c == '{' || c == '[') {
						++depth;
					} else if (c == '}' || c == ']') {
						if (--depth == 0) {
							return true;
						}
					}
				}
				return reject<parse_statuses::unexpected_string_end>();
			}
			++iter;
			return true;
		}

		JSONIFIER_INLINE bool skipRemainingObject() noexcept {
			while (true) {
				if (iter >= endIter || *currentPtr() != '"') [[unlikely]] {
					return reject<parse_statuses::missing_key_start>();
				}
				++iter;
				if (!collectObjectColon()) [[unlikely]] {
					return false;
				}
				if (!skipValue()) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd()) {
					return true;
				}
				if (!collectObjectComma()) [[unlikely]] {
					return false;
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = (iter + 1) < endIter ? stringRootIter + *(iter + 1) : stringEndIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
								return reject<parse_statuses::missing_comma>();
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						++iter;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = stringEndIter;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						++iter;
						iterNew = skipWhitespace(iterNew);
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						++iter;
						iterNew = skipWhitespace(iterNew);
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		JSONIFIER_INLINE string_view_ptr skipWhitespace(string_view_ptr stringViewPtr) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (whitespaceTable[static_cast<uint8_t>(*stringViewPtr)]) {
					++stringViewPtr;
				}
			} else {
				while (stringViewPtr < stringEndIter && whitespaceTable[static_cast<uint8_t>(*stringViewPtr)]) {
					++stringViewPtr;
				}
			}
			return stringViewPtr;
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr == 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				++iter;
				return true;
			} else if (stringEndIter - ptr == 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				++iter;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool objectStart() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStart() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		JSONIFIER_INLINE bool objectStartRoot() noexcept {
			return incrementIfEquals<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStartRoot() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
		}

		JSONIFIER_INLINE bool arrayMaybeEnd() noexcept {
			return incrementIfEquals<']'>();
		}

		JSONIFIER_INLINE bool collectArrayComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool objectMaybeEnd() noexcept {
			return incrementIfEquals<'}'>();
		}

		JSONIFIER_INLINE bool collectObjectComma() noexcept {
			return incrementIfEquals<','>() ? true : reject<parse_statuses::missing_comma>();
		}

		JSONIFIER_INLINE bool collectObjectColon() noexcept {
			return incrementIfEquals<':'>() ? true : reject<parse_statuses::missing_colon>();
		}

		template<bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr >= 4 && compareStringAsInt<"true">(ptr)) {
				value = true;
				++iter;
				return true;
			} else if (stringEndIter - ptr >= 5 && compareStringAsInt<"fals">(ptr) && ptr[4] == 'e') {
				value = false;
				++iter;
				return true;
			}
			return reject<parse_statuses::invalid_bool_value>();
		}

		JSONIFIER_INLINE bool iterateNull() noexcept {
			string_view_ptr ptr = currentPtr();
			if (stringEndIter - ptr >= 4 && compareStringAsInt<"null">(ptr)) [[likely]] {
				++iter;
				return true;
			} else {
				return reject<parse_statuses::invalid_null_value>();
			}
		}

		template<string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			string_view_ptr strPtr = currentPtr() + 1;
			if (strPtr >= stringEndIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			auto& scratch	  = getStringBuffer();
			const auto needed = static_cast<uint64_t>(stringEndIter - strPtr) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto res = string_scanner<parseOpts>::impl(strPtr, stringEndIter, scratch.data());
			if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (has_resize<string_type>) {
				if (value.size() != res.outLength) [[unlikely]] {
					value.resize(res.outLength);
				}
			}
			std::memcpy(value.data(), scratch.data(), res.outLength);
			++iter;
			return true;
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			if (notAtEndPre()) [[likely]] {
				const char c = static_cast<char>(*currentPtr());
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					--currentObjectDepth;
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}

		JSONIFIER_INLINE sep_result collectArraySeparator() noexcept {
			if (notAtEndPre()) [[likely]] {
				const char c = static_cast<char>(*currentPtr());
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					--currentArrayDepth;
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>());
			return sep_result::error;
		}
	};

}
