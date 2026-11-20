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
		using document_state = internal::document_state;
		using field			 = generic_field<minified>;
		using value			 = generic_value<minified>;

		class iterator {
		  public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= field;
			using difference_type	= ptrdiff_t;

			JSONIFIER_INLINE iterator() noexcept = default;

			JSONIFIER_INLINE iterator(document_state* docNew, internal::read_structural_index_ptr keyNew, uint64_t depthNew) noexcept : doc{ docNew }, depth{ depthNew } {
				error_code = doc->startField(keyNew, depth, iter);
				if (error_code != error_codes::success) [[unlikely]] {
					iter = keyNew;
				}
			}

			JSONIFIER_INLINE iterator& operator++() noexcept {
				if (error_code != error_codes::success) [[unlikely]] {
					iter = nullptr;
					return *this;
				}
				error_code = doc->nextField(depth, iter);
				if (error_code != error_codes::success) [[unlikely]] {
					iter = doc->cursor;
				}
				return *this;
			}

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return iter == other.iter;
			}

			JSONIFIER_INLINE field operator*() const noexcept {
				return field{ doc, iter, depth, error_code };
			}

		  protected:
			internal::read_structural_index_ptr iter{};
			document_state* doc{};
			uint64_t depth{};
			error_codes error_code{ error_codes::success };
		};

		JSONIFIER_INLINE generic_object() noexcept = default;

		JSONIFIER_INLINE generic_object(document_state* docNew, internal::read_structural_index_ptr openNew, uint64_t depthNew) noexcept
			: open{ openNew }, doc{ docNew }, depth{ depthNew } {
		}

		JSONIFIER_INLINE generic_object(error_codes errorCodeNew) noexcept : error_code{ errorCodeNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_codes countFields(uint64_t& out) const noexcept {
			if (error_code != error_codes::success) [[unlikely]] {
				return error_code;
			}
			out = 0;
			for (auto iter = begin(); iter != end(); ++iter) {
				if (const error_codes field_error_code = (*iter).error(); field_error_code != error_codes::success) [[unlikely]] {
					return field_error_code;
				}
				++out;
			}
			return error_codes::success;
		}

		JSONIFIER_INLINE iterator begin() const noexcept {
			if (error_code != error_codes::success) [[unlikely]] {
				return iterator{};
			}
			document_state* const state					 = doc;
			internal::read_structural_index_ptr position = open + 1;
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

		[[nodiscard]] JSONIFIER_INLINE error_codes rawJson(string_view& out) const noexcept {
			return value{ doc, open, depth }.rawJson(out);
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept {
			return value{ doc, open, depth }.atPointer(pointer);
		}

		JSONIFIER_INLINE value findField(string_view key) noexcept {
			if (error_code != error_codes::success) [[unlikely]] {
				return value{ error_code };
			}
			document_state* const state = doc;
			if (state->depth > depth) {
				if (const error_codes skip_error_code = state->skipChild(depth); skip_error_code != error_codes::success) [[unlikely]] {
					return value{ skip_error_code };
				}
			}
			if (state->depth != depth) [[unlikely]] {
				state->cursor = open + 1;
				state->depth  = depth;
			}
			const internal::read_structural_index_ptr restorePoint = state->cursor;
			internal::read_structural_index_ptr resumePoint		   = restorePoint;
			if (resumePoint < state->tapeEnd && state->template charAt<internal::uint_c_v<','>>(resumePoint)) {
				++resumePoint;
			}
			value found{ scanFields<false>(key, resumePoint, nullptr) };
			if (found.error_code == error_codes::no_such_field && resumePoint != open + 1) {
				found = scanFields<false>(key, open + 1, resumePoint);
			}
			if (found.error_code == error_codes::no_such_field) [[unlikely]] {
				found = scanFields<true>(key, open + 1, nullptr);
			}
			if (found.error_code == error_codes::no_such_field) [[unlikely]] {
				state->cursor = restorePoint;
				state->depth  = depth;
			}
			return found;
		}

		JSONIFIER_INLINE error_codes error() const noexcept {
			return error_code;
		}

		JSONIFIER_INLINE iterator end() const noexcept {
			return iterator{};
		}

	  protected:
		template<bool escapeAware>
		JSONIFIER_INLINE value scanFields(string_view key, internal::read_structural_index_ptr position, internal::read_structural_index_ptr stop) noexcept {
			document_state* const state						  = doc;
			const internal::read_structural_index_ptr tapeEnd = state->tapeEnd;
			while (true) {
				if (position >= tapeEnd) [[unlikely]] {
					state->cursor = tapeEnd;
					return value{ error_codes::unclosed_container };
				}
				if (position == stop || state->template charAt<internal::uint_c_v<'}'>>(position)) {
					return value{ error_codes::no_such_field };
				}
				if (position + 1 >= tapeEnd || !state->template charAt<internal::uint_c_v<'"'>>(position) ||
					!state->template charAt<internal::uint_c_v<':'>>(position + 1)) [[unlikely]] {
					state->cursor = position;
					return value{ error_codes::tape_error };
				}
				bool matched{};
				if constexpr (escapeAware) {
					matched = state->keyHasEscape(position) && state->unescapedKeyMatches(position, key);
				} else {
					matched = state->template keyMatches<minified>(position, key);
				}
				state->cursor = position + 2;
				state->depth  = depth + 1;
				if (matched) {
					return value{ state, position + 2, depth + 1 };
				}
				if (const error_codes skip_error_code = state->skipChild(depth); skip_error_code != error_codes::success) [[unlikely]] {
					return value{ skip_error_code };
				}
				position = state->cursor;
				if (position >= tapeEnd) [[unlikely]] {
					return value{ error_codes::unclosed_container };
				}
				if (state->template charAt<internal::uint_c_v<','>>(position)) [[likely]] {
					++position;
				} else if (!state->template charAt<internal::uint_c_v<'}'>>(position)) [[unlikely]] {
					return value{ error_codes::tape_error };
				}
			}
		}

		internal::read_structural_index_ptr open{};
		document_state* doc{};
		uint64_t depth{};
		error_codes error_code{ error_codes::success };
	};

}
