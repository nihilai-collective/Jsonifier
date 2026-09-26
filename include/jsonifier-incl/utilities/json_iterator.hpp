/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/json_iterator.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/simd.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/error.hpp>

namespace jsonifier::internal {

	template<string_literal stringNew> JSONIFIER_INLINE static bool compareStringAsInt(read_buffer_ptr src);

	template<typename basic_iterator01> [[maybe_unused]] JSONIFIER_INLINE static void skipStringImpl(basic_iterator01& string1, uint64_t lengthNew) noexcept;

	template<parse_options parseOpts, typename string_type> struct iterate_string_context {
		using scan_result = typename string_scanner<parseOpts>::scan_result;

		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			result = string_scanner<parseOpts>::impl(strPtr, endPtr, ptrNew);
			return result.outLength == std::numeric_limits<uint64_t>::max() ? uint64_t{} : result.outLength;
		}

		inline iterate_string_context(scan_result& resultNew, read_buffer_ptr strPtrNew, read_buffer_ptr endPtrNew) noexcept
			: strPtr{ strPtrNew }, endPtr{ endPtrNew }, result{ resultNew } {
		}

		inline iterate_string_context& operator=(const iterate_string_context&) noexcept = delete;
		inline iterate_string_context& operator=(iterate_string_context&&) noexcept		 = delete;
		inline iterate_string_context(const iterate_string_context&) noexcept			 = delete;
		inline iterate_string_context(iterate_string_context&&) noexcept				 = delete;
		inline iterate_string_context() noexcept										 = delete;

		read_buffer_ptr strPtr{};
		read_buffer_ptr endPtr{};
		scan_result& result;
	};

	template<typename string_type, typename source_type> struct copy_string_context {
		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			memcpyWrapper(ptrNew, srcPtr, length);
			return length;
		}

		const source_type* srcPtr{};
		uint64_t length{};
	};

	template<string_t string_type, typename source_type> JSONIFIER_INLINE static void assignScannedString(string_type& value, const source_type* srcPtr, uint64_t length) noexcept {
		if constexpr (has_resize_and_overwrite<string_type>) {
			value.resize_and_overwrite(length, copy_string_context<string_type, source_type>{ srcPtr, length });
		} else {
			if constexpr (has_resize<string_type>) {
				value.resize(length);
			}
			memcpyWrapper(value.data(), srcPtr, length);
		}
	}

	template<parse_options parseOpts, typename string_type> struct validating_copy_context {
		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			success = string_validating_copier<parseOpts>::impl(srcPtr, length, ptrNew);
			return success ? length : uint64_t{};
		}

		read_buffer_ptr srcPtr{};
		uint64_t length{};
		bool& success;
	};

	struct swar_string_result {
		uint64_t length{};
		bool found{};
	};

	JSONIFIER_INLINE static swar_string_result swarScanAsciiString(read_buffer_ptr strPtr, read_buffer_ptr endPtr) noexcept {
		if constexpr (std::endian::native != std::endian::little) {
			return {};
		} else {
			static constexpr uint64_t ones{ 0x0101010101010101ull };
			static constexpr uint64_t highs{ 0x8080808080808080ull };
			static constexpr uint64_t quoteBytes{ ones * static_cast<uint8_t>('"') };
			static constexpr uint64_t slashBytes{ ones * static_cast<uint8_t>('\\') };
			static constexpr uint64_t controlBytes{ ones * 32u };
			read_buffer_ptr it{ strPtr };
			uint64_t asciiAcc{};
			while (endPtr - it >= 8) {
				uint64_t chunk;
				pow2MemcpyWrapper<8>(&chunk, it);
				const uint64_t quotes  = chunk ^ quoteBytes;
				const uint64_t slashes = chunk ^ slashBytes;
				const uint64_t hits	   = (((quotes - ones) & ~quotes) | ((slashes - ones) & ~slashes) | ((chunk - controlBytes) & ~chunk)) & highs;
				if (hits == 0) {
					asciiAcc |= chunk;
					it += 8;
					continue;
				}
				const uint64_t offset	 = simd::countrZero(hits) >> 3;
				const uint64_t preceding = offset == 0 ? uint64_t{} : (~uint64_t{} >> (64 - 8 * offset));
				asciiAcc |= chunk & preceding;
				if (it[offset] != '"' || (asciiAcc & highs) != 0) {
					return {};
				}
				return { static_cast<uint64_t>(it + offset - strPtr), true };
			}
			if ((asciiAcc & highs) != 0) {
				return {};
			}
			for (; it < endPtr; ++it) {
				const uint8_t currentChar = static_cast<uint8_t>(*it);
				if (currentChar == '"') {
					return { static_cast<uint64_t>(it - strPtr), true };
				}
				if (currentChar == '\\' || currentChar < 32 || currentChar >= 0x80) {
					return {};
				}
			}
			return {};
		}
	}

	template<parse_options parseOpts, string_t string_type, typename buffer_type>
	JSONIFIER_INLINE static bool parseStringContents(string_type& value, read_buffer_ptr strPtr, read_buffer_ptr endPtr, buffer_type& scratch, uint64_t& rawLength) noexcept {
		if (const auto swar = swarScanAsciiString(strPtr, endPtr); swar.found) [[likely]] {
			rawLength = swar.length;
			if constexpr (requires { value.assign(strPtr, swar.length); }) {
				value.assign(strPtr, swar.length);
			} else {
				assignScannedString(value, strPtr, swar.length);
			}
			return true;
		}
		const auto prescan = string_prescanner<parseOpts>::impl(strPtr, endPtr);
		if (!prescan.found) [[unlikely]] {
			return false;
		}
		rawLength = prescan.length;
		if (!prescan.escaped && !prescan.nonAscii) [[likely]] {
			if (prescan.control) [[unlikely]] {
				return false;
			}
			assignScannedString(value, strPtr, prescan.length);
			return true;
		}
		if constexpr (has_resize_and_overwrite<string_type>) {
			if (!prescan.escaped) [[likely]] {
				bool success{};
				value.resize_and_overwrite(prescan.length, validating_copy_context<parseOpts, string_type>{ strPtr, prescan.length, success });
				return success;
			}
			typename string_scanner<parseOpts>::scan_result res{};
			value.resize_and_overwrite(prescan.length + simdBytesPerStep, iterate_string_context<parseOpts, string_type>{ res, strPtr, endPtr });
			return res.outLength != std::numeric_limits<uint64_t>::max();
		} else {
			const auto needed = prescan.length + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			uint64_t outLength{ prescan.length };
			if (!prescan.escaped) [[likely]] {
				if (!string_validating_copier<parseOpts>::impl(strPtr, prescan.length, scratch.data())) [[unlikely]] {
					return false;
				}
			} else {
				const auto res = string_scanner<parseOpts>::impl(strPtr, endPtr, scratch.data());
				if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
					return false;
				}
				outLength = res.outLength;
			}
			assignScannedString(value, scratch.data(), outLength);
			return true;
		}
	}

	enum class sep_result : uint8_t {
		cont,
		ended,
		error,
	};

	template<parse_options parseOpts, typename iterator_type, typename string_buffer_type> struct parse_context;

	template<parse_options parseOpts, typename string_buffer_type> struct parse_context<parseOpts, read_buffer_ptr, string_buffer_type> {
		using iterator_type = read_buffer_ptr;

		JSONIFIER_INLINE parse_context(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, read_buffer_ptr rootIterNew, read_buffer_ptr endIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew } {
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject(read_buffer_ptr errorPos) noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(rootIter, errorPos, endIter));
			return false;
		}

		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		read_buffer_ptr rootIter{};
		read_buffer_ptr endIter{};
		uint64_t indentSize{};
		base_t<decltype(*read_buffer_ptr{})> wsChar{};
	};

	template<parse_options parseOpts, typename string_buffer_type> struct parse_context<parseOpts, structural_index_ptr, string_buffer_type> {
		using iterator_type = structural_index_ptr;

		JSONIFIER_INLINE parse_context(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, read_buffer_ptr stringRootNew, read_buffer_ptr stringEndNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, stringRoot{ stringRootNew }, stringEnd{ stringEndNew } {
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject(read_buffer_ptr errorPos) noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(stringRoot, errorPos, stringEnd));
			return false;
		}

		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		read_buffer_ptr stringRoot{};
		read_buffer_ptr stringEnd{};
	};

	template<parse_options parseOpts, typename iterator_type> struct json_cursor;

	template<parse_options parseOpts> struct json_cursor<parseOpts, read_buffer_ptr> {
		using iterator_type = read_buffer_ptr;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(read_buffer_ptr iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(read_buffer_ptr end, context_type&) noexcept {
			return end;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(read_buffer_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		JSONIFIER_INLINE static bool notAtEnd(read_buffer_ptr iter, read_buffer_ptr end) noexcept {
			return iter < end;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			return iter < end ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			return iter && iter != end ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			return iter >= end ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(read_buffer_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(read_buffer_ptr iter, read_buffer_ptr end, context_type&) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == charToCheck;
			} else {
				return iter < end && *iter == charToCheck;
			}
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, end, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			++iter;
			skipStringImpl(iter, static_cast<uint64_t>(end - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '"') [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < end && *iter == '"') [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= end) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case '"': {
					return skipString(iter, end, context);
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, end, context)) [[unlikely]] {
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
						while (iter < end) {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, end, context)) [[unlikely]] {
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
					return reject<parse_statuses::unexpected_string_end>(iter, context);
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
						while (iter < end) {
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

		template<typename context_type>
		JSONIFIER_INLINE static bool skipRemainingObject(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			while (true) {
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				} else {
					if (iter >= end || *iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				}
				if (!skipString(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd(iter, end, depth, context)) {
					return true;
				}
				if (!collectObjectComma(iter, end, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			using value_type = number_type;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, end); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, end); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, end); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, end); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, end); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, end); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<']'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<'}'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, end, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'{'>(iter, end, context) ? true : reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'['>(iter, end, context) ? true : reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<bool_t bool_type, typename context_type>
		JSONIFIER_INLINE static bool iterateBool(bool_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			if (end - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			iter += 4;
			if (comparison == trueVal) {
				value = true;
			} else if (comparison == falseVal && (*iter == 'e')) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			if (end - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			iter += 4;
			if (comparison == nullVal) [[likely]] {
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<string_t string_type, typename context_type>
		JSONIFIER_INLINE static bool iterateString(string_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			++iter;
			if (iter >= end) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!parseStringContents<parseOpts>(value, iter, end, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			iter += rawLength + 1;
			return true;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectObjectSeparator(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < end) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == '}') {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectArraySeparator(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < end) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == ']') {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}
	};

	struct ws_tracker {
		uint64_t pass{};
		uint64_t fail{};
		~ws_tracker() {
			out << "Fail: " << fail << endl;
			out << "Pass: " << pass << endl;
		}
	};

	template<parse_options parseOpts>
		requires(!parseOpts.minified)
	struct json_cursor<parseOpts, read_buffer_ptr> {
		using iterator_type = read_buffer_ptr;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(read_buffer_ptr iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(read_buffer_ptr end, context_type&) noexcept {
			return end;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(read_buffer_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		JSONIFIER_INLINE static bool atWhitespace(read_buffer_ptr iter, read_buffer_ptr end) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return whitespaceTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < end && whitespaceTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static bool atNewline(read_buffer_ptr iter, read_buffer_ptr end) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return newlineTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < end && newlineTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static read_buffer_ptr skipNewline(read_buffer_ptr iterLocal, read_buffer_ptr end) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (newlineTable[static_cast<uint8_t>(*iterLocal)]) {
					++iterLocal;
				}
			} else {
				while (iterLocal < end && newlineTable[static_cast<uint8_t>(*iterLocal)]) {
					++iterLocal;
				}
			}
			return iterLocal;
		}

		enum class indent_result_types : uint8_t {
			success,
			fail,
		};

		static constexpr uint32_t tag_shift	 = 62u;
		static constexpr uintptr_t tag_mask	 = uintptr_t{ 0x3 } << tag_shift;
		static constexpr uintptr_t addr_mask = ~tag_mask;

		static_assert(sizeof(uintptr_t) == 8, "pointer tagging requires 64-bit pointers");

		JSONIFIER_INLINE static constexpr read_buffer_ptr set_tag(read_buffer_ptr ptr, indent_result_types status) noexcept {
			const uintptr_t raw = std::bit_cast<uintptr_t>(ptr);
			return std::bit_cast<read_buffer_ptr>((raw & addr_mask) | (static_cast<uintptr_t>(status) << tag_shift));
		}

		JSONIFIER_INLINE static constexpr read_buffer_ptr strip_tag(uintptr_t ptr) noexcept {
			return std::bit_cast<read_buffer_ptr>(ptr & addr_mask);
		}

		JSONIFIER_INLINE static constexpr indent_result_types get_tag(const uintptr_t ptr) noexcept {
			return static_cast<indent_result_types>((ptr & tag_mask) >> tag_shift);
		}

		enum class ws_sizes { eq_0, eq_1, eq_2, gt_2, eq_4, gt_4, eq_8 };

		alignas(64) static constexpr ws_sizes wsSizes[9]{ ws_sizes::eq_0, ws_sizes::eq_1, ws_sizes::eq_2, ws_sizes::gt_2, ws_sizes::eq_4, ws_sizes::gt_4, ws_sizes::gt_4,
			ws_sizes::gt_4, ws_sizes::eq_8 };

		JSONIFIER_INLINE static read_buffer_ptr spanIsIndent(read_buffer_ptr iterLocal, uint64_t count, base_t<decltype(*read_buffer_ptr{})> wsChar) noexcept {
			const uint64_t fill{ static_cast<uint64_t>(static_cast<uint8_t>(wsChar)) * 0x0101010101010101ull };
			uint64_t remaining{ count };
			while (remaining > 16) {
				const jsonifier_simd_int_128 charValue{ gatherValue<jsonifier_simd_int_128>(wsChar) };
				const jsonifier_simd_int_128 iterValues{ gatherValuesU<jsonifier_simd_int_128>(iterLocal) };
				const uint16_t mask{ static_cast<uint16_t>(opCmpEq(charValue, iterValues)) };
				if (mask != std::numeric_limits<uint16_t>::max()) {
					return set_tag(iterLocal + simd::countrZeroUnsafe(static_cast<uint16_t>(~mask)), indent_result_types::fail);
				}
				remaining -= 16;
				iterLocal += 16;
			}
			while (remaining > 8) {
				uint64_t chunk;
				pow2MemcpyWrapper<8>(&chunk, iterLocal);
				const uint64_t diff{ chunk ^ fill };
				if (diff) {
					return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
				}
				remaining -= 8;
				iterLocal += 8;
			}
			switch (static_cast<uint64_t>(wsSizes[remaining])) {
				case static_cast<uint64_t>(ws_sizes::eq_1): {
					const bool equals{ static_cast<uint8_t>(*iterLocal) == static_cast<uint8_t>(fill) };
					if (!equals) {
						return set_tag(iterLocal, indent_result_types::fail);
					}
					iterLocal += 1;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_2): {
					uint16_t chunk;
					pow2MemcpyWrapper<2>(&chunk, iterLocal);
					const uint16_t diff{ static_cast<uint16_t>(chunk ^ static_cast<uint16_t>(fill)) };
					if (diff) {
						return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 2;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::gt_2): {
					const uint64_t difference{ remaining - 2 };
					uint32_t chunk;
					pow2MemcpyWrapper<2>(std::bit_cast<write_buffer_ptr>(&chunk), iterLocal);
					pow2MemcpyWrapper<2>(std::bit_cast<write_buffer_ptr>(&chunk) + 2, iterLocal + difference);
					const uint32_t diff{ chunk ^ static_cast<uint32_t>(fill) };
					if (diff) {
						return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += remaining;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_4): {
					uint32_t chunk;
					pow2MemcpyWrapper<4>(&chunk, iterLocal);
					const uint32_t diff{ chunk ^ static_cast<uint32_t>(fill) };
					if (diff) {
						return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 4;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::gt_4): {
					const uint64_t difference{ remaining - 4 };
					uint64_t chunk;
					pow2MemcpyWrapper<4>(std::bit_cast<write_buffer_ptr>(&chunk), iterLocal);
					pow2MemcpyWrapper<4>(std::bit_cast<write_buffer_ptr>(&chunk) + 4, iterLocal + difference);
					const uint64_t diff{ chunk ^ fill };
					if (diff) {
						return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += remaining;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_8): {
					uint64_t chunk;
					pow2MemcpyWrapper<8>(&chunk, iterLocal);
					const uint64_t diff{ chunk ^ fill };
					if (diff) {
						return set_tag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 8;
					break;
				}
				default: {
					return set_tag(iterLocal, indent_result_types::success);
				}
			}
			return set_tag(iterLocal, indent_result_types::success);
		}

		JSONIFIER_INLINE static void skipWhitespaceScalar(read_buffer_ptr& iter, read_buffer_ptr end) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (whitespaceTable[static_cast<uint8_t>(*iter)]) {
					++iter;
				}
			} else {
				while (iter < end && whitespaceTable[static_cast<uint8_t>(*iter)]) {
					++iter;
				}
			}
		}

		template<typename context_type>
		JSONIFIER_INLINE static void skipWhitespacePredicted(read_buffer_ptr& iter, read_buffer_ptr end, const uint64_t depth, context_type& context) noexcept {
			if (atNewline(iter, end)) [[likely]] {
				const read_buffer_ptr probe{ skipNewline(iter, end) };
				const uint64_t predicted{ context.indentSize * depth };
				if (probe + predicted < end) [[likely]] {
					uintptr_t res{ std::bit_cast<uintptr_t>(spanIsIndent(probe, predicted, context.wsChar)) };
					indent_result_types result{ get_tag(res) };
					auto iterNew = strip_tag(res);
					if (result == indent_result_types::success && !whitespaceTable[static_cast<uint8_t>(*iterNew)]) [[likely]] {
						iter = iterNew;
						return;
					}
					iter = probe;
					skipWhitespaceScalar(iter, end);
					return;
				}
				iter = probe;
			}
			skipWhitespaceScalar(iter, end);
		}

		template<typename context_type>
		JSONIFIER_INLINE static void skipWhitespacePredictedClose(read_buffer_ptr& iter, read_buffer_ptr end, const uint64_t depth, context_type& context) noexcept {
			if (atWhitespace(iter, end)) {
				skipWhitespacePredicted(iter, end, depth - (depth != 0), context);
			}
		}

		template<typename context_type> JSONIFIER_INLINE static void collectIndentSize(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			read_buffer_ptr probe{ iter };
			while (probe < end && whitespaceTable[static_cast<uint8_t>(*probe)]) {
				++probe;
			}
			if (probe >= end || (*probe != '{' && *probe != '[')) {
				return;
			}
			++probe;
			if (probe >= end || !newlineTable[static_cast<uint8_t>(*probe)]) {
				return;
			}
			probe = skipNewline(probe, end);
			if (probe >= end || !whitespaceTable[static_cast<uint8_t>(*probe)]) {
				return;
			}
			context.wsChar = *probe;
			uint64_t count{};
			while (probe < end && *probe == context.wsChar) {
				++probe;
				++count;
			}
			context.indentSize = count;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(read_buffer_ptr iter, read_buffer_ptr end, context_type&) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == charToCheck;
			} else {
				return iter < end && *iter == charToCheck;
			}
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEqualsNoWs(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, end, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			skipWhitespaceScalar(iter, end);
			return checkChar<charToCheck>(iter, end, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck, typename context_type>
		JSONIFIER_INLINE static bool incrementIfEqualsNoWsFirst(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<charToCheck>(iter, end, context)) [[likely]] {
				return true;
			}
			if (atWhitespace(iter, end)) [[unlikely]] {
				skipWhitespaceScalar(iter, end);
				return incrementIfEqualsNoWs<charToCheck>(iter, end, context);
			}
			return false;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectObjectSeparator(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, end, depth, context);
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespacePredicted(iter, end, depth, context);
					return sep_result::cont;
				}
				if (c == '}') {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < end) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						skipWhitespacePredicted(iter, end, depth, context);
						return sep_result::cont;
					}
					if (c == '}') {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectArraySeparator(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, end, depth, context);
			if constexpr (parseOpts.nullTerminated) {
				const char c = *iter;
				if (c == ',') [[likely]] {
					++iter;
					skipWhitespacePredicted(iter, end, depth, context);
					return sep_result::cont;
				}
				if (c == ']') {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < end) [[likely]] {
					const char c = *iter;
					if (c == ',') [[likely]] {
						++iter;
						skipWhitespacePredicted(iter, end, depth, context);
						return sep_result::cont;
					}
					if (c == ']') {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		JSONIFIER_INLINE static bool notAtEnd(read_buffer_ptr iter, read_buffer_ptr end) noexcept {
			return iter < end;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			return iter < end ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			return iter && iter != end ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(read_buffer_ptr iter, read_buffer_ptr end, context_type& context) noexcept {
			if (atWhitespace(iter, end)) [[unlikely]] {
				skipWhitespaceScalar(iter, end);
			}
			return iter >= end ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(read_buffer_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			++iter;
			skipStringImpl(iter, static_cast<uint64_t>(end - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '"') [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < end && *iter == '"') [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			skipWhitespaceScalar(iter, end);
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= end) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case '"': {
					return skipString(iter, end, context);
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, end, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar(iter, end);
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
						while (iter < end) {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, end, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar(iter, end);
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
					return reject<parse_statuses::unexpected_string_end>(iter, context);
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
						while (iter < end) {
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

		template<typename context_type>
		JSONIFIER_INLINE static bool skipRemainingObject(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			while (true) {
				if (atWhitespace(iter, end)) [[unlikely]] {
					skipWhitespaceScalar(iter, end);
				}
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				} else {
					if (iter >= end || *iter != '"') [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				}
				if (!skipString(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, end, context)) [[unlikely]] {
					return false;
				}
				switch (static_cast<uint64_t>(collectObjectSeparator(iter, end, depth, context))) {
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

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			using value_type = number_type;
			if (atWhitespace(iter, end)) [[unlikely]] {
				skipWhitespaceScalar(iter, end);
			}
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, end); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, end); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, end); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, end); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, end); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, end); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, end, depth, context);
			return incrementIfEqualsNoWs<']'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, end, depth, context);
			return incrementIfEqualsNoWs<'}'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			return incrementIfEqualsNoWs<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			static constexpr uint16_t colonSpace{ std::endian::native == std::endian::little
					? static_cast<uint16_t>(static_cast<uint16_t>(':') | (static_cast<uint16_t>(' ') << 8))
					: static_cast<uint16_t>((static_cast<uint16_t>(':') << 8) | static_cast<uint16_t>(' ')) };
			if (end - iter >= 2) [[likely]] {
				uint16_t chunk;
				pow2MemcpyWrapper<2>(&chunk, iter);
				if (chunk == colonSpace) [[likely]] {
					iter += 2;
					return true;
				}
			}
			return incrementIfEqualsNoWsFirst<':'>(iter, end, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespaceScalar(iter, end);
			if (!(checkDepth(iter, depth, context) && incrementIfEqualsNoWs<'{'>(iter, end, context))) {
				return reject<parse_statuses::missing_object_start>(iter, context);
			}
			skipWhitespacePredicted(iter, end, depth + 1, context);
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(read_buffer_ptr& iter, read_buffer_ptr end, uint64_t depth, context_type& context) noexcept {
			skipWhitespaceScalar(iter, end);
			if (!(checkDepth(iter, depth, context) && incrementIfEqualsNoWs<'['>(iter, end, context))) {
				return reject<parse_statuses::missing_array_start>(iter, context);
			}
			skipWhitespacePredicted(iter, end, depth + 1, context);
			return true;
		}

		template<bool_t bool_type, typename context_type>
		JSONIFIER_INLINE static bool iterateBool(bool_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			skipWhitespaceScalar(iter, end);
			if (end - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			iter += 4;
			if (comparison == trueVal) {
				value = true;
			} else if (comparison == falseVal && (*iter == 'e')) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			skipWhitespaceScalar(iter, end);
			if (end - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			iter += 4;
			if (comparison == nullVal) [[likely]] {
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<string_t string_type, typename context_type>
		JSONIFIER_INLINE static bool iterateString(string_type& value, read_buffer_ptr& iter, read_buffer_ptr end, context_type& context) noexcept {
			++iter;
			if (iter >= end) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!parseStringContents<parseOpts>(value, iter, end, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			iter += rawLength + 1;
			return true;
		}
	};

	alignas(64) static constexpr auto validPostPrimitiveTable{ [] {
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

	template<parse_options parseOpts> struct json_cursor<parseOpts, structural_index_ptr> {
		using iterator_type = structural_index_ptr;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(structural_index_ptr iter, context_type& context) noexcept {
			return context.stringRoot + *iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(structural_index_ptr, context_type& context) noexcept {
			return context.stringEnd;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(structural_index_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(&context.stringRoot[*iter]);
		}

		JSONIFIER_INLINE static bool notAtEnd(structural_index_ptr iter, structural_index_ptr end) noexcept {
			return iter < end;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			return iter < end ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(structural_index_ptr iter, structural_index_ptr, context_type& context) noexcept {
			return context.stringRoot && context.stringRoot != context.stringEnd ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			bool done{};
			if (context.stringRoot && iter) {
				done = iter >= end || &context.stringRoot[*iter] == context.stringEnd;
			}
			return done ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			return iter < end && *valuePtr(iter, context) == charToCheck;
		}

		template<char charToCheck, typename context_type>
		JSONIFIER_INLINE static bool incrementIfEquals(structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, end, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(structural_index_ptr& iter, structural_index_ptr, context_type&) noexcept {
			++iter;
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			if (iter >= end) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			const char first = static_cast<char>(*valuePtr(iter, context));
			if (first == '{' || first == '[') {
				uint64_t depth{};
				while (iter < end) {
					const char c = static_cast<char>(context.stringRoot[*iter]);
					++iter;
					if (c == '{' || c == '[') {
						++depth;
					} else if (c == '}' || c == ']') {
						if (--depth == 0) {
							return true;
						}
					}
				}
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			++iter;
			return true;
		}

		template<typename context_type>
		JSONIFIER_INLINE static bool skipRemainingObject(structural_index_ptr& iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			while (true) {
				if (iter >= end || *valuePtr(iter, context) != '"') [[unlikely]] {
					return reject<parse_statuses::missing_key_start>(iter, context);
				}
				++iter;
				if (!collectObjectColon(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, end, context)) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd(iter, end, depth, context)) {
					return true;
				}
				if (!collectObjectComma(iter, end, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkPostPrimitive(read_buffer_ptr iterNew, structural_index_ptr iter, context_type& context) noexcept {
			if (iterNew < context.stringEnd && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
				return reject<parse_statuses::missing_comma>(iter, context);
			}
			return true;
		}

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			using value_type		   = number_type;
			read_buffer_ptr valueStart = valuePtr(iter, context);
			read_buffer_ptr valueEnd   = (iter + 1) < end ? context.stringRoot + *(iter + 1) : context.stringEnd;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
							return false;
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
							return false;
						}
						++iter;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(structural_index_ptr& iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'{'>(iter, end, context) ? true : reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(structural_index_ptr& iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'['>(iter, end, context) ? true : reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(structural_index_ptr& iter, structural_index_ptr end, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<']'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(structural_index_ptr& iter, structural_index_ptr end, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<'}'>(iter, end, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, end, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(structural_index_ptr& iter, structural_index_ptr end, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, end, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<bool_t bool_type, typename context_type>
		JSONIFIER_INLINE static bool iterateBool(bool_type& value, structural_index_ptr& iter, structural_index_ptr, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			read_buffer_ptr ptr = valuePtr(iter, context);
			if (context.stringEnd - ptr < 4) {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, ptr);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			if (comparison == trueVal) {
				value = true;
				++iter;
			} else if (comparison == falseVal && (*(ptr + 4) == 'e')) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(structural_index_ptr& iter, structural_index_ptr, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			read_buffer_ptr ptr = valuePtr(iter, context);
			if (context.stringEnd - ptr < 4) {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, ptr);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			if (comparison == nullVal) [[likely]] {
				++iter;
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<string_t string_type, typename context_type>
		JSONIFIER_INLINE static bool iterateString(string_type& value, structural_index_ptr& iter, structural_index_ptr, context_type& context) noexcept {
			read_buffer_ptr strPtr = valuePtr(iter, context) + 1;
			if (strPtr >= context.stringEnd) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!parseStringContents<parseOpts>(value, strPtr, context.stringEnd, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			++iter;
			return true;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectObjectSeparator(structural_index_ptr& iter, structural_index_ptr end, uint64_t, context_type& context) noexcept {
			if (iter < end) [[likely]] {
				const char c = static_cast<char>(*valuePtr(iter, context));
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<typename context_type>
		JSONIFIER_INLINE static sep_result collectArraySeparator(structural_index_ptr& iter, structural_index_ptr end, uint64_t, context_type& context) noexcept {
			if (iter < end) [[likely]] {
				const char c = static_cast<char>(*valuePtr(iter, context));
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}
	};

}
