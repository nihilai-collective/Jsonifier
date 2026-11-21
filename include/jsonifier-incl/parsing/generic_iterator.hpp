/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_iterator.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/document_stream.hpp>

namespace jsonifier::internal {

	template<typename derived_type_new> struct generic_iterator {
		using derived_type = derived_type_new;

		template<parse_options options = parse_options{}, typename buffer_type> generic::generic_document<options.minified> iterate(const buffer_type& in) {
			using generic::parse_statuses;
			using document = generic::generic_document<options.minified>;
			read_buffer_ptr rootIter{ std::bit_cast<read_buffer_ptr>(in.data()) };
			const uint64_t length{ in.size() };
			if (!rootIter || length == 0) [[unlikely]] {
				return document{ parse_statuses::no_input };
			}
			const uint64_t tapeCount{ indexGenericDocument<options>(rootIter, length) };
			if (tapeCount == 0) [[unlikely]] {
				return document{ parse_statuses::no_input };
			}
			generic_storage& data{ derivedRef.genericStorage() };
			document_state& state{ data.stateFor() };
			state.root	 = rootIter;
			state.end	 = rootIter + length;
			state.cursor = state.tape;
			state.depth	 = 0;
			if (tapeCount > 1 && !document_state::isOpener(rootIter[state.tape[0]])) [[unlikely]] {
				return document{ parse_statuses::unfinished_input };
			}
			data.arena.reset(length);
			data.fieldIndex.reset();
			state.arena		 = &data.arena;
			state.fieldIndex = &data.fieldIndex;
			return document{ &state, state.tape, 0 };
		}

		template<parse_options options> JSONIFIER_INLINE uint64_t indexGenericDocument(read_buffer_ptr rootIter, uint64_t length) noexcept {
			if (!derivedRef.section.template tryResetLoneValue<options.minified>(rootIter, length)) {
				return indexGenericWindow<options>(rootIter, length);
			}
			document_state& state{ derivedRef.genericStorage().stateFor() };
			state.tape	  = derivedRef.section.begin();
			state.tapeEnd = derivedRef.section.end();
			return 1;
		}

		template<parse_options options> inline uint64_t indexGenericWindow(read_buffer_ptr rootIter, uint64_t length) noexcept {
			document_state& state{ derivedRef.genericStorage().stateFor() };
			derivedRef.section.template reset<options.minified>(rootIter, length);
			state.tape	  = derivedRef.section.begin();
			state.tapeEnd = derivedRef.section.end();
			return derivedRef.section.getTapeCount();
		}

	  protected:
		inline generic_iterator() noexcept						   = default;
		generic_iterator& operator=(generic_iterator&& other)	   = delete;
		generic_iterator(generic_iterator&& other)				   = delete;
		generic_iterator& operator=(const generic_iterator& other) = delete;
		generic_iterator(const generic_iterator& other)			   = delete;
		inline ~generic_iterator() noexcept						   = default;

		derived_type& derivedRef{ *static_cast<derived_type*>(this) };
	};

}
