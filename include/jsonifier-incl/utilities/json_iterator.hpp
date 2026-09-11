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

	struct ws_skip_step {
		template<typename basic_iterator01> JSONIFIER_INLINE static basic_iterator01 impl(basic_iterator01& iter, const basic_iterator01 endIter) noexcept {
			using simd_list_local					 = type_list_element_t<2, simd::avx_list>;
			using integer_type						 = typename simd_list_local::integer_type;
			using simd_type_local					 = typename simd_list_local::type::type;
			static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
			static constexpr integer_type allOnes	 = simd_list_local::mask;

			const simd_type_local whitespaceTableLocal = simd::gatherValue<simd_type_local>(' ');
			simd_type_local in;
			while (iter + bytesProcessed <= endIter) {
				in				= simd::gatherValuesU<simd_type_local>(iter);
				const auto mask = static_cast<integer_type>(simd::opCmpEq(in, whitespaceTableLocal));
				if (mask != allOnes) {
					iter += postCmpTzcnt(static_cast<integer_type>(~mask));
					return nullptr;
				}
				iter += bytesProcessed;
			}
			return iter;
		}
	};

	template<parse_options parseOpts, typename string_buffer_type>
		requires(!parseOpts.minified)
	struct json_iterator<parseOpts, string_view_ptr, string_buffer_type> {
	  protected:
		static constexpr uint64_t allSpacesMask{ 0x2020202020202020ull };
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

		JSONIFIER_INLINE bool atWhitespace() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return whitespaceTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < endIter && whitespaceTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE void skipWhitespace() noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (true) {
					if (!whitespaceTable[static_cast<uint8_t>(*iter)]) [[likely]] {
						return;
					}
					++iter;
					if (!whitespaceTable[static_cast<uint8_t>(*iter)]) {
						return;
					}
					++iter;
					ws_skip_step::impl(iter, endIter);
					while (whitespaceTable[static_cast<uint8_t>(*iter)]) {
						++iter;
					}
					return;
				}
			} else {
				while (true) {
					if (iter >= endIter || !whitespaceTable[static_cast<uint8_t>(*iter)]) [[likely]] {
						return;
					}
					++iter;
					if (iter >= endIter || !whitespaceTable[static_cast<uint8_t>(*iter)]) {
						return;
					}
					++iter;
					ws_skip_step::impl(iter, endIter);
					while (iter < endIter && whitespaceTable[static_cast<uint8_t>(*iter)]) {
						++iter;
					}
					return;
				}
			}
		}

		JSONIFIER_INLINE sep_result collectObjectSeparator() noexcept {
			if (atWhitespace()) [[unlikely]] {
				skipWhitespace();
			}
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespace();
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
						skipWhitespace();
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
					skipWhitespace();
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
						skipWhitespace();
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
			return checkChar<charToCheck>() ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck> JSONIFIER_INLINE bool incrementIfEqualsNoWs() noexcept {
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
							skipWhitespace();
							return true;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespace();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							iter = iterNew;
							skipWhitespace();
							return iterNew == valueEnd;
						}
						return reject<parse_statuses::invalid_number_value>();
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							value = static_cast<value_type>(i);
							iter  = iterNew;
							skipWhitespace();
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
						skipWhitespace();
						return iterNew == valueEnd;
					}
					return reject<parse_statuses::invalid_number_value>();
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						iter = iterNew;
						skipWhitespace();
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
			return incrementIfEqualsNoWsFirst<'{'>() ? true : reject<parse_statuses::missing_object_start>();
		}

		JSONIFIER_INLINE bool arrayStart() noexcept {
			return incrementIfEquals<'['>() ? true : reject<parse_statuses::missing_array_start>();
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
