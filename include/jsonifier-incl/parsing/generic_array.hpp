/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_array.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/generic_value.hpp>

namespace jsonifier::generic {

	template<bool minified> class generic_array {
	  public:
		using document_state = internal::document_state;
		using value			 = generic_value<minified>;

		class iterator {
		  public:
			using iterator_category = std::forward_iterator_tag;
			using value_type		= value;
			using difference_type	= ptrdiff_t;

			JSONIFIER_INLINE iterator() noexcept = default;

			JSONIFIER_INLINE iterator(document_state* docNew, internal::read_structural_index_ptr iterNew, uint64_t depthNew) noexcept
				: iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
				validateElementStart();
			}

			JSONIFIER_INLINE value operator*() const noexcept {
				return error_code == error_codes::success ? value{ doc, iter, depth } : value{ error_code };
			}

			JSONIFIER_INLINE bool operator==(const iterator& other) const noexcept {
				return iter == other.iter;
			}

			JSONIFIER_INLINE iterator& operator++() noexcept {
				if (error_code != error_codes::success) [[unlikely]] {
					iter = nullptr;
					return *this;
				}
				error_code = doc->advanceChild(iter, depth, internal::uint_c_v<']'>, iter);
				validateElementStart();
				return *this;
			}

		  protected:
			JSONIFIER_INLINE void validateElementStart() noexcept {
				if (error_code == error_codes::success && iter && document_state::isCloser(doc->charAt(iter))) [[unlikely]] {
					error_code = error_codes::tape_error;
				}
			}

			internal::read_structural_index_ptr iter{};
			document_state* doc{};
			uint64_t depth{};
			error_codes error_code{ error_codes::success };
		};

		JSONIFIER_INLINE generic_array() noexcept = default;

		JSONIFIER_INLINE generic_array(document_state* docNew, internal::read_structural_index_ptr openNew, uint64_t depthNew) noexcept
			: open{ openNew }, doc{ docNew }, depth{ depthNew } {
		}

		JSONIFIER_INLINE generic_array(error_codes errorCodeNew) noexcept : error_code{ errorCodeNew } {
		}

		JSONIFIER_INLINE generic_array& operator=(const generic_array& other) noexcept {
			open	   = other.open;
			doc		   = other.doc;
			depth	   = other.depth;
			error_code = other.error_code;
			return *this;
		}

		JSONIFIER_INLINE generic_array(const generic_array& other) noexcept : open{ other.open }, doc{ other.doc }, depth{ other.depth }, error_code{ other.error_code } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_codes countElements(uint64_t& out) const noexcept {
			if (error_code != error_codes::success) [[unlikely]] {
				return error_code;
			}
			out = 0;
			for (auto iter = begin(); iter != end(); ++iter) {
				if (const error_codes element_error_code = (*iter).error(); element_error_code != error_codes::success) [[unlikely]] {
					return element_error_code;
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
			if (position < state->tapeEnd && state->template charAt<internal::uint_c_v<']'>>(position)) {
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
			return value{ doc, open, depth - 1 }.rawJson(out);
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept {
			return value{ doc, open, depth - 1 }.atPointer(pointer);
		}

		JSONIFIER_INLINE value operator[](uint64_t index) const noexcept {
			return at(index);
		}

		JSONIFIER_INLINE value at(uint64_t index) const noexcept {
			if (error_code != error_codes::success) [[unlikely]] {
				return value{ error_code };
			}
			for (auto iter = begin(); iter != end(); ++iter) {
				if (index-- == 0) {
					return *iter;
				}
			}
			return value{ error_codes::index_out_of_bounds };
		}

		JSONIFIER_INLINE error_codes error() const noexcept {
			return error_code;
		}

		JSONIFIER_INLINE iterator end() const noexcept {
			return iterator{};
		}

	  protected:
		internal::read_structural_index_ptr open{};
		document_state* doc{};
		uint64_t depth{};
		error_codes error_code{ error_codes::success };
	};

}
