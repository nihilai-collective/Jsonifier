/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_object.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/generic_field.hpp>

namespace jsonifier::generic {

	template<bool minified> class generic_object {
	  public:
		using document_state = generic::document_state;
		using field			 = generic_field<minified>;
		using value			 = generic_value<minified>;

		class iterator {
		  public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= field;
			using difference_type	= ptrdiff_t;

			JSONIFIER_INLINE iterator() noexcept = default;

			JSONIFIER_INLINE iterator(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew) noexcept : iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
				validateFieldStart();
			}

			JSONIFIER_INLINE iterator& operator++() noexcept {
				if (err != error_code::success) [[unlikely]] {
					iter = nullptr;
					return *this;
				}
				err = doc->advanceChild(iter + 2, depth, internal::uint_c_v<'}'>, iter);
				validateFieldStart();
				return *this;
			}

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return iter == other.iter;
			}

			JSONIFIER_INLINE field operator*() const noexcept {
				return field{ doc, iter, depth, err };
			}

		  protected:
			JSONIFIER_INLINE void validateFieldStart() noexcept {
				if (err == error_code::success && iter &&
					(!doc->template charAt<internal::uint_c_v<'"'>>(iter) || iter + 1 >= doc->tapeEnd || !doc->template charAt<internal::uint_c_v<':'>>(iter + 1))) [[unlikely]] {
					err = error_code::tape_error;
				}
			}

			read_structural_index_ptr iter{};
			document_state* doc{};
			uint64_t depth{};
			error_code err{ error_code::success };
		};

#if JSONIFIER_PLATFORM_MAC && JSONIFIER_COMPILER_CLANG
		static constexpr uint32_t indexThreshold{ 16 };
#else
		static constexpr uint32_t indexThreshold{ 8 };
#endif
		static constexpr uint32_t wrapThreshold{ 2 };
		static constexpr uint32_t trailCapacity{ 16 };

		JSONIFIER_INLINE generic_object() noexcept = default;

		JSONIFIER_INLINE generic_object(document_state* docNew, read_structural_index_ptr openNew, uint64_t depthNew) noexcept : open{ openNew }, doc{ docNew }, depth{ depthNew } {
		}

		JSONIFIER_INLINE generic_object(error_code errNew) noexcept : err{ errNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_code countFields(uint64_t& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			out = 0;
			for (auto iter = begin(); iter != end(); ++iter) {
				if (const error_code childError = (*iter).error(); childError != error_code::success) [[unlikely]] {
					return childError;
				}
				++out;
			}
			return error_code::success;
		}

		JSONIFIER_INLINE iterator begin() const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return iterator{};
			}
			document_state* const state		   = doc;
			read_structural_index_ptr position = open + 1;
			if (position < state->tapeEnd && state->template charAt<internal::uint_c_v<'}'>>(position)) {
				state->cursor = position + 1;
				state->depth  = depth - 1;
				return iterator{};
			}
			state->cursor = position;
			state->depth  = depth;
			if (position >= state->tapeEnd) [[unlikely]] {
				return iterator{};
			}
			return iterator{ state, position, depth };
		}

		[[nodiscard]] JSONIFIER_INLINE error_code rawJson(string_view& out) const noexcept {
			return value{ doc, open, depth - 1 }.rawJson(out);
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept {
			return value{ doc, open, depth - 1 }.atPointer(pointer);
		}

		JSONIFIER_INLINE value findFieldUnordered(string_view key) noexcept {
			value found{ err };
			if (err == error_code::success) [[likely]] {
				settle();
				found = expectedField(key);
				if (found.err == error_code::no_such_field) [[unlikely]] {
					found = indexedField(key);
				}
			}
			return found;
		}

		JSONIFIER_INLINE value operator[](string_view key) noexcept {
			return findFieldUnordered(key);
		}

		JSONIFIER_INLINE value findField(string_view key) noexcept {
			value found{ err };
			if (err == error_code::success) [[likely]] {
				settle();
				found = scanFields<false>(key, doc->tapeEnd);
				if (found.err == error_code::no_such_field) [[unlikely]] {
					found = escapeAwareRetry(key, doc->cursor);
				}
			}
			return found;
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE iterator end() const noexcept {
			return iterator{};
		}

	  protected:
		JSONIFIER_INLINE void recordField(read_structural_index_ptr keyIter) const noexcept {
			document_state* const state = doc;
			read_buffer_ptr keyStart	= state->root + *keyIter + 1;
			read_buffer_ptr keyEnd		= state->root + *(keyIter + 1) - 1;
			if constexpr (!minified) {
				while (keyEnd > keyStart && *keyEnd != internal::uint_c_v<'"'>) {
					--keyEnd;
				}
			}
			const uint32_t object = objectIndex();
			state->fieldIndex->insert(field_index_map::hashKey(keyStart, static_cast<uint64_t>(keyEnd - keyStart), object), object, static_cast<uint32_t>(keyIter - state->tape));
		}

		JSONIFIER_INLINE value expectedField(string_view key) const noexcept {
			document_state* const state			= doc;
			read_structural_index_ptr candidate = state->cursor;
			if (candidate < state->tapeEnd && state->template charAt<internal::uint_c_v<','>>(candidate)) {
				++candidate;
			}
			if (candidate + 1 < state->tapeEnd && state->template charAt<internal::uint_c_v<'"'>>(candidate) && state->template charAt<internal::uint_c_v<':'>>(candidate + 1) &&
				state->template keyMatches<minified>(candidate, key)) [[likely]] {
				state->cursor = candidate + 2;
				return value{ state, candidate + 2, depth };
			}
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE value extendTrail(string_view key, read_structural_index_ptr resumePoint, bool resumed) noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			read_structural_index_ptr cursor  = trailCount == 0 ? open + 1 : state->skipValue(state->tape + trailEntry(trailCount - 1) + 2);
			while (cursor < tapeEnd) {
				const uint8_t current = state->charAt(cursor);
				if (current == internal::uint_c_v<'}'>) {
					trailComplete = true;
					return value{ error_code::no_such_field };
				}
				if (current == internal::uint_c_v<','>) {
					++cursor;
					continue;
				}
				if (current == internal::uint_c_v<'"'> && cursor + 1 < tapeEnd && state->template charAt<internal::uint_c_v<':'>>(cursor + 1)) [[likely]] {
					if (trailCount == trailCapacity) {
						trailOverflow = true;
						return value{ error_code::no_such_field };
					}
					const uint32_t keyOffset = static_cast<uint32_t>(cursor - state->tape);
					pow2MemcpyWrapper<sizeof(uint32_t)>(trailBytes + trailCount * sizeof(uint32_t), &keyOffset);
					++trailCount;
					if (state->template keyMatches<minified>(cursor, key)) {
						return trailHit(cursor, resumePoint, resumed);
					}
					cursor += 2;
				}
				cursor = state->skipValue(cursor);
			}
			trailOverflow = true;
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE void indexAllFields() const noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr cursor  = open + 1;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			while (cursor < tapeEnd) {
				const uint8_t current = state->charAt(cursor);
				if (current == internal::uint_c_v<'}'>) {
					return;
				}
				if (current == internal::uint_c_v<','>) {
					++cursor;
					continue;
				}
				if (current == internal::uint_c_v<'"'> && cursor + 1 < tapeEnd && state->template charAt<internal::uint_c_v<':'>>(cursor + 1)) [[likely]] {
					recordField(cursor);
					cursor += 2;
				}
				cursor = state->skipValue(cursor);
			}
		}

		template<bool escapeAware> JSONIFIER_INLINE value scanFields(string_view key, read_structural_index_ptr stop) noexcept {
			document_state* const state		  = doc;
			read_structural_index_ptr cursor  = state->cursor;
			read_structural_index_ptr tapeEnd = state->tapeEnd;
			uint32_t skipped{ skippedKeys };
			bool expectSeparator{};
			while (cursor < stop) {
				const uint8_t current = state->charAt(cursor);
				if (current == internal::uint_c_v<','>) {
					++cursor;
					expectSeparator = false;
					continue;
				}
				if (current == internal::uint_c_v<'}'>) {
					state->cursor = cursor;
					skippedKeys	  = skipped;
					return value{ error_code::no_such_field };
				}
				if (expectSeparator) [[unlikely]] {
					state->cursor = cursor;
					skippedKeys	  = skipped;
					return value{ error_code::tape_error };
				}
				if (current == internal::uint_c_v<'"'> && cursor + 1 < tapeEnd && state->template charAt<internal::uint_c_v<':'>>(cursor + 1)) [[likely]] {
					read_structural_index_ptr valueIter = cursor + 2;
					bool matched{};
					if constexpr (escapeAware) {
						const bool hasEscape = state->keyHasEscape(cursor);
						escapedKeySeen |= hasEscape;
						matched = hasEscape && state->unescapedKeyMatches(cursor, key);
					} else {
						matched = state->template keyMatches<minified>(cursor, key);
					}
					if (matched) {
						state->cursor = valueIter;
						skippedKeys	  = skipped;
						return value{ state, valueIter, depth };
					}
					++skipped;
					cursor = valueIter;
				}
				cursor			= state->skipValue(cursor);
				expectSeparator = true;
			}
			state->cursor = cursor;
			skippedKeys	  = skipped;
			return value{ cursor >= tapeEnd ? error_code::unclosed_container : error_code::no_such_field };
		}

		JSONIFIER_INLINE value trailHit(read_structural_index_ptr keyIter, read_structural_index_ptr resumePoint, bool resumed) const noexcept {
			if (resumed) {
				if (keyIter < resumePoint) {
					doc->noteBackwardHit();
				} else {
					doc->noteForwardHit();
				}
			}
			doc->cursor = keyIter + 2;
			return value{ doc, keyIter + 2, depth };
		}

		JSONIFIER_INLINE value trailLookup(string_view key, read_structural_index_ptr resumePoint, bool resumed) noexcept {
			document_state* const state = doc;
			for (uint32_t x = 0; x < trailCount; ++x) {
				read_structural_index_ptr keyIter = state->tape + trailEntry(x);
				if (state->template keyMatches<minified>(keyIter, key)) {
					return trailHit(keyIter, resumePoint, resumed);
				}
			}
			if (!trailComplete) {
				value found{ extendTrail(key, resumePoint, resumed) };
				if (found.err == error_code::success || trailOverflow) {
					return found;
				}
			}
			if (!escapeFree) {
				bool sawEscape{};
				for (uint32_t x = 0; x < trailCount; ++x) {
					read_structural_index_ptr keyIter = state->tape + trailEntry(x);
					if (state->keyHasEscape(keyIter)) {
						sawEscape = true;
						if (state->unescapedKeyMatches(keyIter, key)) {
							return trailHit(keyIter, resumePoint, resumed);
						}
					}
				}
				escapeFree = !sawEscape;
			}
			state->cursor = resumePoint;
			return value{ error_code::no_such_field };
		}

		JSONIFIER_INLINE value indexedField(string_view key) noexcept {
			document_state* const state			  = doc;
			read_structural_index_ptr resumePoint = state->cursor;
			value found{ error_code::no_such_field };
			if (!indexed) {
				if (!indexEarned) {
					const bool resumed = resumePoint != open + 1;
					if (!trailOverflow) {
						if (trailCount == 0 && !trailComplete && !(resumed && state->reverseMode())) {
							found = scanFields<false>(key, state->tapeEnd);
							if (found.err != error_code::no_such_field) {
								if (found.err == error_code::success && resumed) {
									state->noteForwardHit();
								}
								return found;
							}
						}
						found = trailLookup(key, resumePoint, resumed);
						if (!trailOverflow) {
							return found;
						}
						state->cursor = resumePoint;
					}
					if (resumed && state->reverseMode()) {
						found = scanBeforeResume(key, resumePoint);
						if (found.err == error_code::no_such_field) {
							state->cursor = resumePoint;
							found		  = scanFields<false>(key, state->tapeEnd);
							if (found.err == error_code::success) {
								state->noteForwardHit();
							}
						}
					} else {
						found = scanFields<false>(key, state->tapeEnd);
						if (found.err == error_code::success) {
							if (resumed) {
								state->noteForwardHit();
							}
						} else if (found.err == error_code::no_such_field && resumed) {
							found = scanBeforeResume(key, resumePoint);
						}
					}
					if (found.err == error_code::no_such_field) {
						found = escapeAwareRetry(key, resumePoint);
					}
					return found;
				}
				indexAllFields();
				indexed = true;
			}

			const uint32_t object = objectIndex();
			const uint64_t hash	  = field_index_map::hashKey(std::bit_cast<read_buffer_ptr>(key.data()), key.size(), object);
			const uint32_t hit	  = state->fieldIndex->template find<minified, key_matcher>(hash, object, state, key);
			if (hit != field_index_map::npos) {
				state->cursor = state->tape + hit + 2;
				found		  = value{ state, state->cursor, depth };
			}
			if (found.err == error_code::no_such_field) {
				found = escapeAwareRetry(key, resumePoint);
			}
			return found;
		}

		JSONIFIER_INLINE value escapeAwareRetry(string_view key, read_structural_index_ptr restorePoint) noexcept {
			if (escapeFree) {
				doc->cursor = restorePoint;
				return value{ error_code::no_such_field };
			}
			doc->cursor	   = open + 1;
			escapedKeySeen = false;
			value found{ scanFields<true>(key, doc->tapeEnd) };
			if (found.err == error_code::no_such_field) {
				escapeFree	= !escapedKeySeen;
				doc->cursor = restorePoint;
			}
			return found;
		}

		JSONIFIER_INLINE value scanBeforeResume(string_view key, read_structural_index_ptr resumePoint) noexcept {
			doc->cursor = open + 1;
			value found{ scanFields<false>(key, resumePoint) };
			indexEarned = ++wrapCount >= wrapThreshold && skippedKeys >= indexThreshold;
			if (found.err == error_code::success) {
				doc->noteBackwardHit();
			}
			return found;
		}

		JSONIFIER_INLINE uint32_t trailEntry(uint32_t index) const noexcept {
			uint32_t keyOffset;
			pow2MemcpyWrapper<sizeof(uint32_t)>(&keyOffset, trailBytes + index * sizeof(uint32_t));
			return keyOffset;
		}

		JSONIFIER_INLINE uint32_t objectIndex() const noexcept {
			return static_cast<uint32_t>(open - doc->tape);
		}

		JSONIFIER_INLINE void settle() const noexcept {
			if (doc->depth != depth) [[unlikely]] {
				if (doc->depth > depth) {
					doc->finishToDepth(depth);
				}
				if (doc->depth != depth) {
					rewind();
				}
			}
		}

		JSONIFIER_INLINE void rewind() const noexcept {
			doc->cursor = open + 1;
			doc->depth	= depth;
		}

		read_structural_index_ptr open{};
		document_state* doc{};
		uint64_t depth{};
		uint32_t skippedKeys{};
		uint32_t trailCount{};
		uint32_t wrapCount{};
		uint8_t trailBytes[trailCapacity * sizeof(uint32_t)];
		error_code err{ error_code::success };
		bool escapedKeySeen{};
		bool trailComplete{};
		bool trailOverflow{};
		bool indexEarned{};
		bool escapeFree{};
		bool indexed{};
	};

}
