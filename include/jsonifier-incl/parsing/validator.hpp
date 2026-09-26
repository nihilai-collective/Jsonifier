/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/validator.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>

namespace jsonifier::internal {

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type value) noexcept {
		return value + strLen(value);
	}

	template<pointer_t value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type value) noexcept {
		return std::bit_cast<read_buffer_ptr>(value);
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getEndIter(value_type& value) noexcept {
		return value.data() + value.size();
	}

	template<has_data value_type> JSONIFIER_INLINE static read_buffer_ptr getBeginIter(value_type& value) noexcept {
		return value.data();
	}

	template<typename derived_type_new> struct validator {
		using derived_type = derived_type_new;

		static constexpr parse_options optionsVal{};

		using cursor = json_cursor<optionsVal, structural_index_ptr>;

		template<string_t string_type> inline bool validateJson(string_type&& in) noexcept {
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			derivedRef.section.template reset<optionsVal.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			parse_context<optionsVal, structural_index_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors, rootIter,
				endIter };
			auto newSize = static_cast<uint64_t>(endIter - rootIter) / 2;
			if (derivedRef.stringBuffer.size() < newSize) {
				derivedRef.stringBuffer.resize(newSize);
			}
			derivedRef.errors.clear();
			const structural_index_ptr iter{ derivedRef.section.begin() };
			const structural_index_ptr end{ derivedRef.section.end() };
			if (!cursor::anyInput(iter, end, context)) {
				return false;
			}
			const structural_index_ptr iterNew = impl(iter, end, 0, context);
			if (!iterNew) {
				return false;
			}
			static_cast<void>(cursor::checkIfDone(iterNew, end, context));
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

		template<typename context_type>
		inline static structural_index_ptr impl(structural_index_ptr iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::notAtEnd(iter, end)) {
				return nullptr;
			}
			const auto c = *cursor::valuePtr(iter, context);
			if (c == '{') {
				return validateObject(iter, end, depth, context);
			} else if (c == '[') {
				return validateArray(iter, end, depth, context);
			} else if (c == '"') {
				return validateString(iter, end, context);
			} else if (numberTable[static_cast<uint8_t>(c)]) {
				return validateNumber(iter, end, context);
			} else if (boolTable[static_cast<uint8_t>(c)]) {
				return validateBool(iter, end, context);
			} else if (c == 'n') {
				return validateNull(iter, end, context);
			} else {
				return nullptr;
			}
		}

		template<typename context_type>
		inline static structural_index_ptr validateObject(structural_index_ptr iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'{'>(iter, end, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<'}'>(iter, end, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, end)) {
				iter = validateString(iter, end, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::template checkChar<':'>(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
				++iter;
				iter = validator<derived_type_new>::impl(iter, end, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, end, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<'}'>(iter, end, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		template<typename context_type>
		inline static structural_index_ptr validateArray(structural_index_ptr iter, structural_index_ptr end, uint64_t depth, context_type& context) noexcept {
			if (!(cursor::checkDepth(iter, depth, context) && cursor::template checkChar<'['>(iter, end, context))) [[unlikely]] {
				return nullptr;
			}
			++iter;
			if (cursor::template checkChar<']'>(iter, end, context)) [[unlikely]] {
				return ++iter;
			}
			while (cursor::notAtEnd(iter, end)) {
				iter = validator<derived_type_new>::impl(iter, end, depth + 1, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (cursor::template checkChar<','>(iter, end, context)) [[likely]] {
					++iter;
				} else if (cursor::template checkChar<']'>(iter, end, context)) {
					return ++iter;
				} else {
					return nullptr;
				}
			}
			return nullptr;
		}

		template<typename context_type>
		JSONIFIER_INLINE static structural_index_ptr validateString(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			if (!cursor::template checkChar<'"'>(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			auto newPtr = cursor::valuePtr(iter, context);
			++iter;
			auto endPtr		   = cursor::notAtEnd(iter, end) ? cursor::valuePtr(iter, context) : (newPtr + (end - iter));
			using scanner_type = string_scanner<optionsVal>;
			auto& scratch	   = context.getStringBuffer();
			const auto needed  = static_cast<uint64_t>(endPtr - newPtr) + simdBytesPerStep;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			return scanner_type::impl(newPtr, endPtr, scratch.data()).outLength != std::numeric_limits<uint64_t>::max() ? iter : nullptr;
		}

		template<typename context_type>
		JSONIFIER_INLINE static structural_index_ptr validateNumber(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			auto newPtr = cursor::valuePtr(iter, context);
			++iter;
			if (cursor::notAtEnd(iter, end) && (*newPtr != 0x30u || !numberTable[static_cast<uint64_t>(*(newPtr + 1))])) [[likely]] {
				consumeSign(newPtr);
				consumeDigits(newPtr);
				if (consumeChar(0x2Eu, newPtr)) {
					if (!cursor::notAtEnd(iter, end) || !consumeDigits(newPtr)) {
						return nullptr;
					}
				}
				if (consumeChar(0x65u, newPtr) || consumeChar(0x45u, newPtr)) {
					consumeSign(newPtr);
				}
				return iter;
			} else {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static bool consumeDigits(read_buffer_ptr& newerPtr, uint64_t minCount = 1) {
			uint64_t count = 0;
			while (is_digit(static_cast<uint8_t>(*newerPtr))) {
				++newerPtr;
				++count;
			}
			return count >= minCount;
		}

		JSONIFIER_INLINE static bool consumeChar(char expected, read_buffer_ptr& newerPtr) {
			if (*newerPtr == expected) {
				++newerPtr;
				return true;
			}
			return false;
		}

		JSONIFIER_INLINE static void consumeSign(read_buffer_ptr& newerPtr) {
			if (*newerPtr == '-' || *newerPtr == '+') {
				++newerPtr;
			}
			return;
		}

		template<typename context_type>
		JSONIFIER_INLINE static structural_index_ptr validateBool(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			if (cursor::notAtEnd(iter, end) && jsonifier::internal::validateBool(cursor::valuePtr(iter, context))) [[likely]] {
				return ++iter;
			} else {
				return nullptr;
			}
		}

		template<typename context_type>
		JSONIFIER_INLINE static structural_index_ptr validateNull(structural_index_ptr iter, structural_index_ptr end, context_type& context) noexcept {
			if (cursor::notAtEnd(iter, end) && jsonifier::internal::validateNull(cursor::valuePtr(iter, context))) [[likely]] {
				return ++iter;
			} else {
				return nullptr;
			}
		}
	};

}// namespace internal
