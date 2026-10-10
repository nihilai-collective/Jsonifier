/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/validator.hpp
 */
#if !defined(JSONIFIER_PASS_GUARD_VALIDATOR)
	#define JSONIFIER_PASS_GUARD_VALIDATOR

	#include <jsonifier-incl/utilities/utility.hpp>
	#include <jsonifier-incl/utilities/string_utils.hpp>
	#include <jsonifier-incl/utilities/json_iterator.hpp>

namespace JSONIFIER_INTERNAL_NAMESPACE {

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value + strLen(value));
	}

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value);
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type& value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value.data() + value.size());
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type& value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value.data());
	}

	template<typename derived_type_new> struct validator {
		using derived_type = derived_type_new;

		static constexpr parse_options optionsVal{};

		using cursor = json_cursor<optionsVal, write_structural_index_ptr>;

		template<string_t string_type> inline bool validateJson(string_type&& in) noexcept {
			auto rootIter = ::JSONIFIER_INTERNAL_NAMESPACE::getBeginIter(in);
			auto endIter  = ::JSONIFIER_INTERNAL_NAMESPACE::getEndIter(in);
			derivedRef.section.template reset<optionsVal.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			parse_context<optionsVal, write_structural_index_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors,
				rootIter, endIter, derivedRef.section.end() };
			auto newSize = static_cast<uint64_t>(endIter - rootIter) / 2;
			if (derivedRef.stringBuffer.size() < newSize) {
				derivedRef.stringBuffer.resize(newSize);
			}
			derivedRef.errors.clear();
			const write_structural_index_ptr iter{ derivedRef.section.begin() };
			if (!cursor::anyInput(iter, context)) {
				return false;
			}
			// Bytes >= 0x80 may only appear inside strings, so one pass over the whole input covers every string.
			if (!::JSONIFIER_INTERNAL_NAMESPACE::validateUtf8(rootIter, static_cast<uint64_t>(endIter - rootIter))) [[unlikely]] {
				return false;
			}
			const write_structural_index_ptr iterNew = impl(iter, 0, context);
			if (!iterNew) {
				return false;
			}
			static_cast<void>(cursor::checkIfDone(iterNew, context));
			return derivedRef.errors.size() == 0;
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		validator& operator=(const validator& other) = delete;
		validator& operator=(validator&& other)		 = delete;
		validator(const validator& other)			 = delete;
		validator(validator&& other)				 = delete;
		inline ~validator() noexcept				 = default;
		inline validator() noexcept					 = default;

		template<typename context_type> inline static write_structural_index_ptr impl(write_structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::notAtEnd(iter, context)) {
				return nullptr;
			}
			const auto c = *cursor::valuePtr(iter, context);
			if (c == '{') {
				return validateObject(iter, depth, context);
			} else if (c == '[') {
				return validateArray(iter, depth, context);
			} else if (c == '"') {
				return validateString(iter, context);
			} else if (numberTable[static_cast<uint8_t>(c)]) {
				return validateNumber(iter, context);
			} else if (boolTable[static_cast<uint8_t>(c)]) {
				return validateBool(iter, context);
			} else if (c == 'n') {
				return validateNull(iter, context);
			} else {
				return nullptr;
			}
		}

		template<typename context_type> inline static write_structural_index_ptr validateObject(write_structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'{'>(iter, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<'}'>(iter, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, context)) {
				iter = validateString(iter, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::template checkChar<':'>(iter, context)) [[unlikely]] {
					return nullptr;
				}
				++iter;
				iter = validator<derived_type_new>::impl(iter, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<'}'>(iter, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		template<typename context_type> inline static write_structural_index_ptr validateArray(write_structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'['>(iter, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<']'>(iter, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, context)) {
				iter = validator<derived_type_new>::impl(iter, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<']'>(iter, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		// The structural index holds only the first byte of each scalar, so the end of a scalar is not indexed. A scalar ends
		// either at the next structural index or at the end of the input, and only whitespace may come between the two.
		template<typename context_type> JSONIFIER_INLINE static read_buffer_ptr scalarBound(write_structural_index_ptr nextIter, context_type& context) noexcept {
			return cursor::notAtEnd(nextIter, context) ? cursor::valuePtr(nextIter, context) : context.stringEnd;
		}

		JSONIFIER_INLINE static bool onlyWhitespace(read_buffer_ptr ptr, read_buffer_ptr bound) noexcept {
			if (ptr == bound) [[likely]] {
				return true;
			}
			for (; ptr < bound; ++ptr) {
				if (*ptr != ' ' && *ptr != '\t' && *ptr != '\n' && *ptr != '\r') {
					return false;
				}
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static write_structural_index_ptr validateString(write_structural_index_ptr iter, context_type& context) noexcept {
			if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
				return nullptr;
			}
			const auto contentPtr = cursor::valuePtr(iter, context) + 1;
			++iter;
			const auto bound = scalarBound(iter, context);
			// UTF-8 was checked for the whole input up front, so only find the closing quote and reject control characters.
			// Strings with escapes, a few percent in practice, go through the full scanner to check each escape.
			auto ptr   = contentPtr;
			bool found = false;
			if constexpr (std::endian::native == std::endian::little) {
				while (bound - ptr >= 8) {
					uint64_t chunk;
					pow2MemcpyWrapper<sizeof(chunk)>(&chunk, ptr);
					const uint64_t quotes	   = chunk ^ 0x2222222222222222ull;
					const uint64_t backslashes = chunk ^ 0x5C5C5C5C5C5C5C5Cull;
					// A byte is flagged if it is zero in quotes or backslashes, or below 0x20 in chunk. Borrows only move
					// upwards, so the lowest flagged byte is always a real match.
					const uint64_t flagged =
						(((quotes - 0x0101010101010101ull) & ~quotes) | ((backslashes - 0x0101010101010101ull) & ~backslashes) | ((chunk - 0x2020202020202020ull) & ~chunk)) &
						0x8080808080808080ull;
					if (flagged != 0) {
						ptr += static_cast<uint64_t>(std::countr_zero(flagged)) >> 3;
						found = true;
						break;
					}
					ptr += 8;
				}
			}
			if (!found) {
				while (ptr < bound && *ptr != '"' && *ptr != '\\' && *ptr >= 0x20) {
					++ptr;
				}
				if (ptr >= bound) [[unlikely]] {
					return nullptr;
				}
			}
			if (*ptr == '"') [[likely]] {
				return onlyWhitespace(ptr + 1, bound) ? iter : nullptr;
			}
			if (*ptr < 0x20) [[unlikely]] {
				return nullptr;
			}
			using scanner_type = string_scanner<optionsVal>;
			auto& scratch	   = context.getStringBuffer();
			const auto needed  = static_cast<uint64_t>(bound - contentPtr) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			const auto result = scanner_type::impl(contentPtr, bound, scratch.data());
			if (result.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return nullptr;
			}
			// rawLength is the offset of the closing quote.
			return onlyWhitespace(contentPtr + result.rawLength + 1, bound) ? iter : nullptr;
		}

		// RFC 8259: [ "-" ] ( "0" / digit1-9 *DIGIT ) [ "." 1*DIGIT ] [ ( "e" / "E" ) [ "-" / "+" ] 1*DIGIT ]
		template<typename context_type> JSONIFIER_INLINE static write_structural_index_ptr validateNumber(write_structural_index_ptr iter, context_type& context) noexcept {
			auto newPtr = cursor::valuePtr(iter, context);
			++iter;
			const auto bound   = scalarBound(iter, context);
			const auto readEnd = context.stringEnd;
			consumeChar('-', newPtr, bound);
			if (consumeChar('0', newPtr, bound)) {
				if (newPtr < bound && is_digit(static_cast<uint8_t>(*newPtr))) [[unlikely]] {
					return nullptr;
				}
			} else if (!consumeDigits(newPtr, bound, readEnd)) [[unlikely]] {
				return nullptr;
			}
			if (consumeChar('.', newPtr, bound) && !consumeDigits(newPtr, bound, readEnd)) [[unlikely]] {
				return nullptr;
			}
			if (consumeChar('e', newPtr, bound) || consumeChar('E', newPtr, bound)) {
				if (!consumeChar('-', newPtr, bound)) {
					consumeChar('+', newPtr, bound);
				}
				if (!consumeDigits(newPtr, bound, readEnd)) [[unlikely]] {
					return nullptr;
				}
			}
			return onlyWhitespace(newPtr, bound) ? iter : nullptr;
		}

		// Consumes the digits in [newerPtr, bound). Loads may read up to readEnd, the end of the input; bytes from bound on
		// are treated as non-digits.
		JSONIFIER_INLINE static bool consumeDigits(read_buffer_ptr& newerPtr, read_buffer_ptr bound, read_buffer_ptr readEnd) noexcept {
			const auto start = newerPtr;
			if constexpr (std::endian::native == std::endian::little) {
				// byte ^ '0' is below 10 only for a digit, and adding 0x76 sets the high bit of any byte at 10 or above.
				// Carries only move upwards, so the lowest flagged byte is the first non-digit.
				while (readEnd - newerPtr >= 8) {
					uint64_t chunk;
					pow2MemcpyWrapper<sizeof(chunk)>(&chunk, newerPtr);
					chunk ^= 0x3030303030303030ull;
					uint64_t nonDigits	 = ((chunk + 0x7676767676767676ull) | chunk) & 0x8080808080808080ull;
					const auto remaining = bound - newerPtr;
					if (remaining < 8) {
						nonDigits |= 0x80ull << (remaining * 8);
					}
					if (nonDigits != 0) {
						newerPtr += static_cast<uint64_t>(std::countr_zero(nonDigits)) >> 3;
						return newerPtr != start;
					}
					newerPtr += 8;
				}
			}
			while (newerPtr < bound && is_digit(static_cast<uint8_t>(*newerPtr))) {
				++newerPtr;
			}
			return newerPtr != start;
		}

		JSONIFIER_INLINE static bool consumeChar(char expected, read_buffer_ptr& newerPtr, read_buffer_ptr bound) noexcept {
			if (newerPtr < bound && *newerPtr == expected) {
				++newerPtr;
				return true;
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static write_structural_index_ptr validateBool(write_structural_index_ptr iter, context_type& context) noexcept {
			const auto newPtr = cursor::valuePtr(iter, context);
			if (!::JSONIFIER_INTERNAL_NAMESPACE::validateBool(newPtr, context.stringEnd)) [[unlikely]] {
				return nullptr;
			}
			++iter;
			return onlyWhitespace(newPtr + (*newPtr == 't' ? 4 : 5), scalarBound(iter, context)) ? iter : nullptr;
		}

		template<typename context_type> JSONIFIER_INLINE static write_structural_index_ptr validateNull(write_structural_index_ptr iter, context_type& context) noexcept {
			const auto newPtr = cursor::valuePtr(iter, context);
			if (!::JSONIFIER_INTERNAL_NAMESPACE::validateNull(newPtr, context.stringEnd)) [[unlikely]] {
				return nullptr;
			}
			++iter;
			return onlyWhitespace(newPtr + 4, scalarBound(iter, context)) ? iter : nullptr;
		}
	};

}// namespace internal

#endif
