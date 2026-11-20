/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/document_state.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/string_parser.hpp>

namespace jsonifier::internal {

	struct document_state {
		JSONIFIER_INLINE generic::error_codes skipChild(uint64_t parentDepth) noexcept {
			if (depth <= parentDepth) [[likely]] {
				return generic::error_codes::success;
			}
			const read_buffer_ptr base		   = root;
			const read_structural_index_ptr limit = tapeEnd;
			read_structural_index_ptr position	   = cursor;
			const int64_t target{ static_cast<int64_t>(parentDepth) };
			int64_t currentDepth{ static_cast<int64_t>(depth) };
			if (position >= limit) [[unlikely]] {
				return generic::error_codes::unclosed_container;
			}
			switch (base[*position++]) {
				case uint_c_v<'['>:
				case uint_c_v<'{'>:
				case uint_c_v<':'>:
				case uint_c_v<','>: {
					break;
				}
				case uint_c_v<'"'>: {
					if (position < limit && base[*position] == uint_c_v<':'>) {
						++position;
						break;
					}
					[[fallthrough]];
				}
				default: {
					--currentDepth;
					if (currentDepth <= target) {
						cursor = position;
						depth  = static_cast<uint64_t>(currentDepth);
						return generic::error_codes::success;
					}
					break;
				}
			}
			while (position < limit) {
				currentDepth += depthDeltaTable[base[*position++]];
				if (currentDepth <= target) {
					cursor = position;
					depth  = static_cast<uint64_t>(currentDepth);
					return generic::error_codes::success;
				}
			}
			cursor = position;
			depth  = static_cast<uint64_t>(currentDepth);
			return generic::error_codes::unclosed_container;
		}

		JSONIFIER_INLINE generic::error_codes startField(read_structural_index_ptr keyPosition, uint64_t objectDepth, read_structural_index_ptr& next) noexcept {
			if (keyPosition + 1 >= tapeEnd) [[unlikely]] {
				cursor = tapeEnd;
				return keyPosition < tapeEnd ? generic::error_codes::tape_error : generic::error_codes::unclosed_container;
			}
			if (root[*keyPosition] != uint_c_v<'"'> || root[*(keyPosition + 1)] != uint_c_v<':'>) [[unlikely]] {
				cursor = keyPosition;
				return generic::error_codes::tape_error;
			}
			next   = keyPosition;
			cursor = keyPosition + 2;
			depth  = objectDepth + 1;
			return generic::error_codes::success;
		}

		JSONIFIER_INLINE generic::error_codes nextField(uint64_t objectDepth, read_structural_index_ptr& next) noexcept {
			if (const generic::error_codes error_code = skipChild(objectDepth); error_code != generic::error_codes::success) [[unlikely]] {
				next = nullptr;
				return error_code;
			}
			read_structural_index_ptr position = cursor;
			if (position >= tapeEnd) [[unlikely]] {
				next = nullptr;
				return generic::error_codes::unclosed_container;
			}
			switch (root[*position++]) {
				case uint_c_v<'}'>: {
					cursor = position;
					depth  = objectDepth - 1;
					next   = nullptr;
					return generic::error_codes::success;
				}
				case uint_c_v<','>: {
					return startField(position, objectDepth, next);
				}
				default: {
					next = nullptr;
					return generic::error_codes::tape_error;
				}
			}
		}

		JSONIFIER_INLINE generic::error_codes nextElement(uint64_t arrayDepth, read_structural_index_ptr& next) noexcept {
			if (const generic::error_codes error_code = skipChild(arrayDepth); error_code != generic::error_codes::success) [[unlikely]] {
				next = nullptr;
				return error_code;
			}
			read_structural_index_ptr position = cursor;
			if (position >= tapeEnd) [[unlikely]] {
				next = nullptr;
				return generic::error_codes::unclosed_container;
			}
			switch (root[*position++]) {
				case uint_c_v<']'>: {
					cursor = position;
					depth  = arrayDepth - 1;
					next   = nullptr;
					return generic::error_codes::success;
				}
				case uint_c_v<','>: {
					cursor = position;
					depth  = arrayDepth + 1;
					next   = position;
					return position < tapeEnd ? generic::error_codes::success : generic::error_codes::unclosed_container;
				}
				default: {
					next = nullptr;
					return generic::error_codes::tape_error;
				}
			}
		}

		template<bool minified> JSONIFIER_INLINE bool keyMatches(read_structural_index_ptr iter, string_view key) const noexcept {
			const uint64_t keySize	 = key.size();
			const uint64_t span		 = static_cast<uint64_t>(*(iter + 1) - *iter);
			read_buffer_ptr keyStart = root + *iter + 1;
			if (span == keySize + 2) [[likely]] {
				return internal::comparison::compare(keyStart, key.data(), keySize);
			}
			if constexpr (minified) {
				return false;
			} else {
				return span > keySize + 2 && keyStart[keySize] == uint_c_v<'"'> && internal::comparison::compare(keyStart, key.data(), keySize);
			}
		}

		JSONIFIER_INLINE static unrestrict_t<read_structural_index_ptr> skipValue(read_buffer_ptr base, read_structural_index_ptr iter, read_structural_index_ptr limit) noexcept {
			if (!isOpener(base[*iter])) [[likely]] {
				return iter + 1;
			}
			int64_t openCount{ 1 };
			++iter;
			while (iter < limit) {
				openCount += depthDeltaTable[base[*iter]];
				++iter;
				if (openCount == 0) {
					return iter;
				}
			}
			return limit;
		}

		JSONIFIER_INLINE bool keyHasEscape(read_structural_index_ptr iter) const noexcept {
			read_buffer_ptr keyStart = root + *iter + 1;
			read_buffer_ptr keyLimit = root + *(iter + 1);
			return keyLimit > keyStart && char_comparison<'\\', uint8_t>::memchar(keyStart, static_cast<uint64_t>(keyLimit - keyStart));
		}

		JSONIFIER_INLINE unrestrict_t<read_structural_index_ptr> skipValue(read_structural_index_ptr iter) const noexcept {
			if (!isOpener(root[*iter])) [[likely]] {
				return iter + 1;
			}
			if (closes) [[likely]] {
				if (const uint32_t afterClose = closes[iter - closeTapeBase]; afterClose != 0) [[likely]] {
					if (read_structural_index_ptr position = closeTapeBase + afterClose; position <= tapeEnd) [[likely]] {
						return position;
					}
				}
			}
			return skipValue(root, iter, tapeEnd);
		}

		template<bool minified> JSONIFIER_INLINE string_view rawKey(read_structural_index_ptr iter) const noexcept {
			read_buffer_ptr keyStart = root + *iter + 1;
			read_buffer_ptr keyEnd	 = root + *(iter + 1) - 1;
			if constexpr (!minified) {
				while (keyEnd > keyStart && *keyEnd != uint_c_v<'"'>) {
					--keyEnd;
				}
			}
			return string_view{ std::bit_cast<const char*>(keyStart), static_cast<uint64_t>(keyEnd - keyStart) };
		}

		JSONIFIER_INLINE bool unescapedKeyMatches(read_structural_index_ptr iter, string_view key) const noexcept {
			read_buffer_ptr keyStart = root + *iter + 1;
			char* unescaped			 = arena->scratch();
			const auto scanned		 = string_parser::impl(keyStart, end, unescaped);
			return scanned.outLength == key.size() && internal::comparison::compare(unescaped, key.data(), key.size());
		}

		template<typename word_type> JSONIFIER_INLINE static word_type loadWord(read_buffer_ptr source) noexcept {
			word_type word;
			pow2MemcpyWrapper<sizeof(word_type)>(&word, source);
			return word;
		}

		JSONIFIER_INLINE static uint32_t loadLittle32(read_buffer_ptr source) noexcept {
			const uint32_t word = loadWord<uint32_t>(source);
			if constexpr (std::endian::native == std::endian::big) {
				return byteswap(word);
			} else {
				return word;
			}
		}

		template<bool minified> JSONIFIER_INLINE uint64_t keyTag(read_structural_index_ptr iter) const noexcept {
			const string_view key = rawKey<minified>(iter);
			const uint64_t length = key.size();
			if (length - 4 > 4) [[unlikely]] {
				return 0;
			}
			const read_buffer_ptr keyStart = std::bit_cast<read_buffer_ptr>(key.data());
			const uint64_t low			   = loadLittle32(keyStart);
			const uint64_t high			   = loadLittle32(keyStart + length - 4);
			return low | (high << ((length - 4) * 8));
		}

		static consteval uint64_t keyTagOf(std::string_view key) noexcept {
			if (key.size() < 4 || key.size() > 8) {
				return 0;
			}
			uint64_t tag{};
			for (uint64_t x = 0; x < key.size(); ++x) {
				tag |= static_cast<uint64_t>(static_cast<uint8_t>(key[x])) << (x * 8);
			}
			return tag;
		}

		template<uint8_t value> JSONIFIER_INLINE bool charAt(read_structural_index_ptr iter) const noexcept {
			return root[*iter] == value;
		}

		JSONIFIER_INLINE uint8_t charAt(read_structural_index_ptr iter) const noexcept {
			return root[*iter];
		}

		JSONIFIER_INLINE static bool isOpener(uint8_t c) noexcept {
			return (c | 0x20) == uint_c_v<'{'>;
		}

		JSONIFIER_INLINE static bool isCloser(uint8_t c) noexcept {
			return (c | 0x20) == uint_c_v<'}'>;
		}

		read_structural_index_ptr closeTapeBase{};
		read_structural_index_ptr tapeEnd{};
		read_structural_index_ptr cursor{};
		read_structural_index_ptr tape{};
		read_buffer_ptr root{};
		read_buffer_ptr end{};
		const uint32_t* closes{};
		string_arena* arena{};
		uint64_t depth{};
	};

}
