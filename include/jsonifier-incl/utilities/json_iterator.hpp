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

		JSONIFIER_INLINE iterate_string_context(scan_result& resultNew, read_buffer_ptr strPtrNew, read_buffer_ptr endPtrNew) noexcept
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
		uint64_t depth{};
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
		uint64_t depth{};
	};

	template<parse_options parseOpts, typename iterator_type> struct json_cursor;

	template<parse_options parseOpts> struct json_cursor<parseOpts, read_buffer_ptr> {
		using iterator_type = read_buffer_ptr;
		using read_ptr_type = remove_pointer_t<iterator_type>;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(auto&& __restrict iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(auto endIter, context_type&) noexcept {
			return endIter;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(auto&& __restrict iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		JSONIFIER_INLINE static bool notAtEnd(auto&& __restrict iter, auto endIter) noexcept {
			return iter < endIter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter && iter != endIter ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter >= endIter ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(auto&& __restrict iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(auto&& __restrict iter, auto endIter, context_type&) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == charToCheck;
			} else {
				return iter < endIter && *iter == charToCheck;
			}
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, endIter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespaceClose(auto&&, auto, context_type&) noexcept {
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
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
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case '"': {
					return skipString(iter, endIter, context);
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, endIter, context)) [[unlikely]] {
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
								if (!skipString(iter, endIter, context)) [[unlikely]] {
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

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			while (true) {
				if (!checkChar<'"'>(iter, endIter, context)) [[unlikely]] {
					return reject<parse_statuses::missing_key_start>(iter, context);
				}
				if (!skipString(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd(iter, endIter, context)) {
					return true;
				}
				if (!collectObjectComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			using value_type = number_type;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (read_ptr_type* const iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						uint64_t i;
						if (read_ptr_type* const iterNew = integer_parser<uint64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (read_ptr_type* const iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						int64_t i;
						if (read_ptr_type* const iterNew = integer_parser<int64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (read_ptr_type* const iterNew = float_parser<double>::parseFloat(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (read_ptr_type* const iterNew = float_parser<value_type>::parseFloat(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkChar<']'>(iter, endIter, context)) {
				--context.depth;
				++iter;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkChar<'}'>(iter, endIter, context)) {
				--context.depth;
				++iter;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, endIter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkDepth(iter, context.depth, context) && incrementIfEquals<'{'>(iter, endIter, context)) [[likely]] {
				++context.depth;
				return true;
			}
			return reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkDepth(iter, context.depth, context) && incrementIfEquals<'['>(iter, endIter, context)) [[likely]] {
				++context.depth;
				return true;
			}
			return reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<bool_t bool_type, typename context_type> JSONIFIER_INLINE static bool iterateBool(bool_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			if (endIter - iter < 4) [[unlikely]] {
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

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			if (endIter - iter < 4) [[unlikely]] {
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
		JSONIFIER_INLINE static bool iterateString(string_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!parseStringContents<parseOpts>(value, iter, endIter, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			iter += rawLength + 1;
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectSeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArraySeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context);
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
		using read_ptr_type = remove_pointer_t<iterator_type>;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(auto&& __restrict iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(auto endIter, context_type&) noexcept {
			return endIter;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(auto&& __restrict iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		JSONIFIER_INLINE static bool atWhitespace(auto&& __restrict iter, [[maybe_unused]] auto endIter) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return whitespaceTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < endIter && whitespaceTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static bool atNewline(auto&& __restrict iter, [[maybe_unused]] auto endIter) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return newlineTable[static_cast<uint8_t>(*iter)];
			} else {
				return iter < endIter && newlineTable[static_cast<uint8_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static read_buffer_ptr skipNewline(read_buffer_ptr iterLocal, read_buffer_ptr endIter) noexcept {
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

		JSONIFIER_INLINE static void skipWhitespaceScalar(auto&& __restrict iter, [[maybe_unused]] auto endIter) noexcept {
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

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespacePredicted(auto&& __restrict iter, auto endIter, const uint64_t depth, context_type& context) noexcept {
			if (atNewline(iter, endIter)) [[likely]] {
				const read_buffer_ptr probe{ skipNewline(iter, endIter) };
				const uint64_t predicted{ context.indentSize * depth };
				if (probe + predicted < endIter) [[likely]] {
					uintptr_t res{ std::bit_cast<uintptr_t>(spanIsIndent(probe, predicted, context.wsChar)) };
					indent_result_types result{ get_tag(res) };
					const read_buffer_ptr iterNew = strip_tag(res);
					if (result == indent_result_types::success && !whitespaceTable[static_cast<uint8_t>(*iterNew)]) [[likely]] {
						iter = iterNew;
						return;
					}
					iter = probe;
					skipWhitespaceScalar(iter, endIter);
					return;
				}
				iter = probe;
			}
			skipWhitespaceScalar(iter, endIter);
		}

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespaceClose(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (atWhitespace(iter, endIter)) {
				skipWhitespacePredicted(iter, endIter, context.depth - (context.depth != 0), context);
			}
		}

		template<typename context_type> JSONIFIER_INLINE static void collectIndentSize(auto iter, auto endIter, context_type& context) noexcept {
			read_buffer_ptr probe{ iter };
			while (probe < endIter && whitespaceTable[static_cast<uint8_t>(*probe)]) {
				++probe;
			}
			if (probe >= endIter || (*probe != '{' && *probe != '[')) {
				return;
			}
			++probe;
			if (probe >= endIter || !newlineTable[static_cast<uint8_t>(*probe)]) {
				return;
			}
			probe = skipNewline(probe, endIter);
			if (probe >= endIter || !whitespaceTable[static_cast<uint8_t>(*probe)]) {
				return;
			}
			context.wsChar = *probe;
			uint64_t count{};
			while (probe < endIter && *probe == context.wsChar) {
				++probe;
				++count;
			}
			context.indentSize = count;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(auto&& __restrict iter, auto endIter, context_type&) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == charToCheck;
			} else {
				return iter < endIter && *iter == charToCheck;
			}
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEqualsNoWs(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, endIter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, endIter);
			return checkChar<charToCheck>(iter, endIter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEqualsNoWsFirst(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<charToCheck>(iter, endIter, context)) [[likely]] {
				return true;
			}
			if (atWhitespace(iter, endIter)) [[unlikely]] {
				skipWhitespaceScalar(iter, endIter);
				return incrementIfEqualsNoWs<charToCheck>(iter, endIter, context);
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectSeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<','>(iter, endIter, context)) [[likely]] {
				skipWhitespacePredicted(iter, endIter, context.depth, context);
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArraySeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<','>(iter, endIter, context)) [[likely]] {
				skipWhitespacePredicted(iter, endIter, context.depth, context);
				return true;
			}
			return false;
		}

		JSONIFIER_INLINE static bool notAtEnd(auto&& __restrict iter, auto endIter) noexcept {
			return iter < endIter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter && iter != endIter ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (atWhitespace(iter, endIter)) [[unlikely]] {
				skipWhitespaceScalar(iter, endIter);
			}
			return iter >= endIter ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(auto&& __restrict iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
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
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, endIter);
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == '\0') [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case '"': {
					return skipString(iter, endIter, context);
				}
				case '{':
					[[fallthrough]];
				case '[': {
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != '\0') {
							const char c = *iter;
							if (c == '"') {
								if (!skipString(iter, endIter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar(iter, endIter);
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
								if (!skipString(iter, endIter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[static_cast<uint8_t>(c)]) {
								skipWhitespaceScalar(iter, endIter);
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

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			while (true) {
				if (atWhitespace(iter, endIter)) [[unlikely]] {
					skipWhitespaceScalar(iter, endIter);
				}
				if (!checkChar<'"'>(iter, endIter, context)) [[unlikely]] {
					return reject<parse_statuses::missing_key_start>(iter, context);
				}
				if (!skipString(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				skipWhitespaceClose(iter, endIter, context);
				if (collectObjectSeparator(iter, endIter, context)) [[likely]] {
					continue;
				}
				if (objectMaybeEnd(iter, endIter, context)) {
					return true;
				}
				return reject<parse_statuses::missing_comma>(iter, context);
			}
		}

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			using value_type = number_type;
			if (atWhitespace(iter, endIter)) [[unlikely]] {
				skipWhitespaceScalar(iter, endIter);
			}
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (read_ptr_type* const iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						uint64_t i;
						if (read_ptr_type* const iterNew = integer_parser<uint64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (read_ptr_type* const iterNew = integer_parser<value_type>::parseInt(value, iter, endIter); iterNew) {
							iter = iterNew;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						int64_t i;
						if (read_ptr_type* const iterNew = integer_parser<int64_t>::parseInt(i, iter, endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (read_ptr_type* const iterNew = float_parser<double>::parseFloat(temp, iter, endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (read_ptr_type* const iterNew = float_parser<value_type>::parseFloat(value, iter, endIter); iterNew) {
						iter = iterNew;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<']'>(iter, endIter, context)) {
				--context.depth;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<'}'>(iter, endIter, context)) {
				--context.depth;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEqualsNoWs<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			static constexpr uint16_t colonSpace{ std::endian::native == std::endian::little
					? static_cast<uint16_t>(static_cast<uint16_t>(':') | (static_cast<uint16_t>(' ') << 8))
					: static_cast<uint16_t>((static_cast<uint16_t>(':') << 8) | static_cast<uint16_t>(' ')) };
			if (endIter - iter >= 2) [[likely]] {
				uint16_t chunk;
				pow2MemcpyWrapper<2>(&chunk, iter);
				if (chunk == colonSpace) [[likely]] {
					iter += 2;
					return true;
				}
			}
			return incrementIfEqualsNoWsFirst<':'>(iter, endIter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, endIter);
			if (!(checkDepth(iter, context.depth, context) && incrementIfEqualsNoWs<'{'>(iter, endIter, context))) {
				return reject<parse_statuses::missing_object_start>(iter, context);
			}
			++context.depth;
			skipWhitespacePredicted(iter, endIter, context.depth, context);
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, endIter);
			if (!(checkDepth(iter, context.depth, context) && incrementIfEqualsNoWs<'['>(iter, endIter, context))) {
				return reject<parse_statuses::missing_array_start>(iter, context);
			}
			++context.depth;
			skipWhitespacePredicted(iter, endIter, context.depth, context);
			return true;
		}

		template<bool_t bool_type, typename context_type> JSONIFIER_INLINE static bool iterateBool(bool_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			skipWhitespaceScalar(iter, endIter);
			if (endIter - iter < 4) [[unlikely]] {
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

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			skipWhitespaceScalar(iter, endIter);
			if (endIter - iter < 4) [[unlikely]] {
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
		JSONIFIER_INLINE static bool iterateString(string_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			++iter;
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!parseStringContents<parseOpts>(value, iter, endIter, context.getStringBuffer(), rawLength)) [[unlikely]] {
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
		using read_ptr_type = remove_pointer_t<iterator_type>;

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr valuePtr(auto&& __restrict iter, context_type& context) noexcept {
			return context.stringRoot + *iter;
		}

		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr stringEnd(auto, context_type& context) noexcept {
			return context.stringEnd;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(auto&& __restrict iter, context_type& context) noexcept {
			return context.template reject<errorType>(&context.stringRoot[*iter]);
		}

		JSONIFIER_INLINE static bool notAtEnd(auto&& __restrict iter, auto endIter) noexcept {
			return iter < endIter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter < endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(auto&& __restrict iter, auto, context_type& context) noexcept {
			return context.stringRoot && context.stringRoot != context.stringEnd ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			bool done{};
			if (context.stringRoot && iter) {
				done = iter >= endIter || &context.stringRoot[*iter] == context.stringEnd;
			}
			return done ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(auto&& __restrict iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return iter < endIter && *valuePtr(iter, context) == charToCheck;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, endIter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespaceClose(auto&&, auto, context_type&) noexcept {
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(auto&& __restrict iter, auto, context_type&) noexcept {
			++iter;
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (iter >= endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			const char first = static_cast<char>(*valuePtr(iter, context));
			if (first == '{' || first == '[') {
				uint64_t depth{};
				while (iter < endIter) {
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

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			while (true) {
				if (iter >= endIter || *valuePtr(iter, context) != '"') [[unlikely]] {
					return reject<parse_statuses::missing_key_start>(iter, context);
				}
				++iter;
				if (!collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd(iter, endIter, context)) {
					return true;
				}
				if (!collectObjectComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkPostPrimitive(read_buffer_ptr iterNew, auto&& __restrict iter, context_type& context) noexcept {
			if (iterNew < context.stringEnd && !validPostPrimitiveTable[static_cast<uint8_t>(*iterNew)]) [[unlikely]] {
				return reject<parse_statuses::missing_comma>(iter, context);
			}
			return true;
		}

		template<number_t number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			using value_type		   = number_type;
			read_buffer_ptr valueStart = valuePtr(iter, context);
			read_buffer_ptr valueEnd   = (iter + 1) < endIter ? context.stringRoot + *(iter + 1) : context.stringEnd;
			if constexpr (integer_t<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						}
						return reject<parse_statuses::invalid_number_value>(iter, context);
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
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkDepth(iter, context.depth, context) && incrementIfEquals<'{'>(iter, endIter, context)) [[likely]] {
				++context.depth;
				return true;
			}
			return reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (checkDepth(iter, context.depth, context) && incrementIfEquals<'['>(iter, endIter, context)) [[likely]] {
				++context.depth;
				return true;
			}
			return reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEquals<']'>(iter, endIter, context)) {
				--context.depth;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			if (incrementIfEquals<'}'>(iter, endIter, context)) {
				--context.depth;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, endIter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<bool_t bool_type, typename context_type> JSONIFIER_INLINE static bool iterateBool(bool_type& value, auto&& __restrict iter, auto, context_type& context) noexcept {
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

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(auto&& __restrict iter, auto, context_type& context) noexcept {
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
		JSONIFIER_INLINE static bool iterateString(string_type& value, auto&& __restrict iter, auto, context_type& context) noexcept {
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

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectSeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArraySeparator(auto&& __restrict iter, auto endIter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, endIter, context);
		}
	};

}
