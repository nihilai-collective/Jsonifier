/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type = number_type;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<number_type>::impl(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = endIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						iter  = iterNew;
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
						iter = iterNew;
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			auto iterStart = iter;
			const auto res = string_scanner<parseOpts>::impl(iter, endIter);
			if (!res.valid) [[unlikely]] {
				iter = iterStart;
				return reject<parse_statuses::invalid_string_characters>();
			}
			if (res.rawLength == 0) [[unlikely]] {
				if constexpr (concepts::has_resize<string_type>) {
					if (value.size() != 0) {
						value.clear();
					}
				}
				++iter;
				if (iter > endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
				return true;
			}
			if constexpr (concepts::has_resize<string_type>) {
				if (value.size() != res.rawLength) [[unlikely]] {
					value.resize(res.rawLength);
				}
			}
			if (res.firstEscape == string_scanner<parseOpts>::npos) [[likely]] {
				std::memcpy(value.data(), iter, res.rawLength);
			} else [[unlikely]] {
				std::memcpy(value.data(), iter, res.firstEscape);
				const auto finalPtr = string_scanner<parseOpts>::unescapeImpl(iter + res.firstEscape, iter + res.rawLength, value.data() + res.firstEscape);
				if (!finalPtr) [[unlikely]] {
					iter = iterStart;
					return reject<parse_statuses::invalid_string_characters>();
				}
				if constexpr (concepts::has_resize<string_type>) {
					value.resize(static_cast<uint64_t>(finalPtr - value.data()));
				}
			}
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
		template<typename value_type> static constexpr value_type allSpacesMask{ repeatByte<0x20, value_type>() };
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

		JSONIFIER_INLINE void skipWhitespace() noexcept {
			skipWhitespaceShort();
		}

		template<typename integer_type> JSONIFIER_INLINE void swarSkipWS() noexcept {
			static constexpr uint64_t byteCount{ sizeof(integer_type) };
			if constexpr (parseOpts.nullTerminated) {
				if (!whitespaceTable[static_cast<uint8_t>(*iter)]) [[likely]] {
					return;
				}
				++iter;
				if (*iter != ' ') {
					return;
				}
				integer_type v;
				while (iter + byteCount <= endIter) {
					std::memcpy(&v, iter, byteCount);
					if (const integer_type diff = static_cast<integer_type>(v ^ allSpacesMask<integer_type>); diff) {
						iter += simd::tzcnt(diff) >> 3;
						break;
					}
					iter += byteCount;
				}
				while (*iter == ' ') {
					++iter;
				}
			} else {
				if (iter < endIter && !whitespaceTable[static_cast<uint8_t>(*iter)]) [[likely]] {
					return;
				}
				if (iter >= endIter) {
					return;
				}
				++iter;
				if (iter >= endIter || *iter != ' ') {
					return;
				}
				integer_type v;
				while (iter + byteCount <= endIter) {
					std::memcpy(&v, iter, byteCount);
					if (const integer_type diff = static_cast<integer_type>(v ^ allSpacesMask<integer_type>); diff) {
						iter += simd::tzcnt(diff) >> 3;
						break;
					}
					iter += byteCount;
				}
				while (iter < endIter && *iter == ' ') {
					++iter;
				}
			}
		}

		JSONIFIER_INLINE void skipWhitespaceAfterComma() noexcept {
			const uint64_t depth = currentObjectDepth + currentArrayDepth;
			if (depth > 2) {
				swarSkipWS<uint64_t>();
			} else if (depth > 1) {
				swarSkipWS<uint32_t>();
			} else {
				skipWhitespaceShort();
			}
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			skipWhitespace();
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespaceAfterComma();
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
						skipWhitespaceAfterComma();
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
					skipWhitespaceAfterComma();
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
						skipWhitespaceAfterComma();
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

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEquals() noexcept {
			skipWhitespace();
			if (checkChar<charToCheck>()) {
				++iter;
				if constexpr (charToCheck == ',') {
					skipWhitespaceAfterComma();
				}
				return true;
			}
			return false;
		}

		JSONIFIER_INLINE void skipWhitespaceShort() noexcept {
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
			skipWhitespace();
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
								skipWhitespace();
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
								skipWhitespace();
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
				skipWhitespace();
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type = number_type;
			skipWhitespace();
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>();
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			}
		}

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = endIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							skipWhitespace();
							return true;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespace();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							skipWhitespace();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespace();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						iter  = iterNew;
						skipWhitespace();
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
						iter = iterNew;
						skipWhitespace();
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			auto iterStart = iter;
			const auto res = string_scanner<parseOpts>::impl(iter, endIter);
			if (!res.valid) [[unlikely]] {
				iter = iterStart;
				return reject<parse_statuses::invalid_string_characters>();
			}
			if (res.rawLength == 0) [[unlikely]] {
				if constexpr (concepts::has_resize<string_type>) {
					if (value.size() != 0) {
						value.clear();
					}
				}
				++iter;
				if (iter > endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>();
				}
				return true;
			}
			if constexpr (concepts::has_resize<string_type>) {
				if (value.size() != res.rawLength) [[unlikely]] {
					value.resize(res.rawLength);
				}
			}
			if (res.firstEscape == string_scanner<parseOpts>::npos) [[likely]] {
				std::memcpy(value.data(), iter, res.rawLength);
			} else [[unlikely]] {
				std::memcpy(value.data(), iter, res.firstEscape);
				const auto finalPtr = string_scanner<parseOpts>::unescapeImpl(iter + res.firstEscape, iter + res.rawLength, value.data() + res.firstEscape);
				if (!finalPtr) [[unlikely]] {
					iter = iterStart;
					return reject<parse_statuses::invalid_string_characters>();
				}
				if constexpr (concepts::has_resize<string_type>) {
					value.resize(static_cast<uint64_t>(finalPtr - value.data()));
				}
			}
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
		string_view_ptr stringRootIter{};
		string_view_ptr stringEndIter{};
		structural_index_ptr rootIter{};
		structural_index_ptr endIter{};
		uint64_t currentObjectDepth{};
		uint64_t currentArrayDepth{};
		std::vector<error>* errors{};
		structural_index_ptr iter{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, structural_index_ptr rootIterNew, structural_index_ptr endIterNew,
			structural_index_ptr iterNew, string_view_ptr stringRootIterNew, string_view_ptr stringEndIterNew) noexcept
			: stringBuffer{ stringBufferNew }, stringRootIter{ stringRootIterNew }, stringEndIter{ stringEndIterNew }, rootIter{ rootIterNew }, endIter{ endIterNew },
			  errors{ errorsNew }, iter{ iterNew } {
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = (iter + 1) < endIter ? stringRootIter + *(iter + 1) : stringEndIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
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
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
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
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
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
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
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
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = stringEndIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						++iter;
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
						++iter;
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				}
			}
		}

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			string_view_ptr strPtr = currentPtr() + 1;
			if (strPtr >= stringEndIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const auto res = string_scanner<parseOpts>::impl(strPtr, stringEndIter);
			if (!res.valid) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (concepts::has_resize<string_type>) {
				if (value.size() != res.rawLength) [[unlikely]] {
					value.resize(res.rawLength);
				}
			}
			if (res.firstEscape == string_scanner<parseOpts>::npos) [[likely]] {
				std::memcpy(value.data(), strPtr, res.rawLength);
			} else [[unlikely]] {
				std::memcpy(value.data(), strPtr, res.firstEscape);
				const auto finalPtr = string_scanner<parseOpts>::unescapeImpl(strPtr + res.firstEscape, strPtr + res.rawLength, value.data() + res.firstEscape);
				if (!finalPtr) [[unlikely]] {
					return reject<parse_statuses::invalid_string_characters>();
				}
				if constexpr (concepts::has_resize<string_type>) {
					value.resize(static_cast<uint64_t>(finalPtr - value.data()));
				}
			}
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
		string_view_ptr stringRootIter{};
		structural_index_ptr rootIter{};
		string_view_ptr stringEndIter{};
		structural_index_ptr endIter{};
		uint64_t currentObjectDepth{};
		uint64_t currentArrayDepth{};
		std::vector<error>* errors{};
		structural_index_ptr iter{};

	  public:
		JSONIFIER_INLINE json_iterator() noexcept = default;

		JSONIFIER_INLINE json_iterator(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, structural_index_ptr rootIterNew, structural_index_ptr endIterNew,
			structural_index_ptr iterNew, string_view_ptr stringRootIterNew, string_view_ptr stringEndIterNew) noexcept
			: stringBuffer{ stringBufferNew }, stringRootIter{ stringRootIterNew }, rootIter{ rootIterNew }, stringEndIter{ stringEndIterNew }, endIter{ endIterNew },
			  errors{ errorsNew }, iter{ iterNew } {
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = (iter + 1) < endIter ? stringRootIter + *(iter + 1) : stringEndIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
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
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
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
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
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
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
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
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						if (iterNew < stringEndIter && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
							return reject<parse_statuses::missing_comma>();
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
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

		template<jsonifier::concepts::num_t number_type> JSONIFIER_INLINE bool iterateRootNumber(number_type& value) noexcept {
			using value_type		   = number_type;
			string_view_ptr valueStart = currentPtr();
			string_view_ptr valueEnd   = stringEndIter;
			if constexpr (concepts::integer_t<value_type>) {
				if constexpr (concepts::uint_types<value_type>) {
					if constexpr (concepts::uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (concepts::int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::impl(value, valueStart, valueEnd); iterNew) {
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::impl(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							++iter;
							iterNew = skipWhitespace(iterNew);
							return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				}
			} else {
				if constexpr (std::is_volatile_v<jsonifier::internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::impl(temp, valueStart, valueEnd); iterNew) {
						value = static_cast<value_type>(temp);
						++iter;
						iterNew = skipWhitespace(iterNew);
						return iterNew == valueEnd ? true : reject<parse_statuses::unfinished_input>();
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = float_parser<number_type>::impl(value, valueStart, valueEnd); iterNew) {
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

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateRootBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::bool_t bool_type> JSONIFIER_INLINE bool iterateBool(bool_type& value) noexcept {
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

		template<jsonifier::concepts::string_t string_type> JSONIFIER_INLINE bool iterateString(string_type& value) noexcept {
			string_view_ptr strPtr = currentPtr() + 1;
			if (strPtr >= stringEndIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>();
			}
			const auto res = string_scanner<parseOpts>::impl(strPtr, stringEndIter);
			if (!res.valid) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>();
			}
			if constexpr (concepts::has_resize<string_type>) {
				if (value.size() != res.rawLength) [[unlikely]] {
					value.resize(res.rawLength);
				}
			}
			if (res.firstEscape == string_scanner<parseOpts>::npos) [[likely]] {
				std::memcpy(value.data(), strPtr, res.rawLength);
			} else [[unlikely]] {
				std::memcpy(value.data(), strPtr, res.firstEscape);
				const auto finalPtr = string_scanner<parseOpts>::unescapeImpl(strPtr + res.firstEscape, strPtr + res.rawLength, value.data() + res.firstEscape);
				if (!finalPtr) [[unlikely]] {
					return reject<parse_statuses::invalid_string_characters>();
				}
				if constexpr (concepts::has_resize<string_type>) {
					value.resize(static_cast<uint64_t>(finalPtr - value.data()));
				}
			}
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
