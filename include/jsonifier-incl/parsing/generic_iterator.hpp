/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_iterator.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/generic_document.hpp>

namespace jsonifier::internal {

	template<typename derived_type_new> struct generic_iterator {
		using derived_type = derived_type_new;

		static constexpr uint64_t smallDocumentBytes{ jsonifier::simdBlocksPerStep * 64 };

		template<parse_options options = parse_options{}, typename buffer_type> jsonifier::generic::basic_document<options.minified> iterate(const buffer_type& in) {
			using document		 = jsonifier::generic::basic_document<options.minified>;
			using document_state = jsonifier::generic::document_state<options.minified>;
			using jsonifier::generic::error_code;
			read_buffer_ptr rootIter = std::bit_cast<read_buffer_ptr>(in.data());
			const uint64_t length	 = in.size();
			if (!rootIter || length == 0) [[unlikely]] {
				return document{ error_code::empty };
			}
			const uint64_t tapeCount = indexGenericWindow<options>(rootIter, length);
			if (tapeCount == 0) [[unlikely]] {
				return document{ error_code::empty };
			}
			::jsonifier::generic::generic_storage& data = derivedRef.genericStorage();
			document_state& state						= data.template stateFor<options.minified>();
			state.root									= rootIter;
			state.end									= rootIter + length;
			state.cursor								= state.tape;
			state.depth									= 0;
			if (tapeCount > 1 && !document_state::isOpener(rootIter[state.tape[0]])) [[unlikely]] {
				return document{ error_code::trailing_content };
			}
			data.arena.reset(length);
			data.fieldIndex.reset();
			state.arena		 = &data.arena;
			state.fieldIndex = &data.fieldIndex;
			return document{ &state, state.tape, 0 };
		}

		template<parse_options options> uint64_t indexGenericWindow(read_buffer_ptr rootIter, uint64_t length) {
			::jsonifier::generic::document_state<options.minified>& state = derivedRef.genericStorage().template stateFor<options.minified>();
			if (length < smallDocumentBytes) {
				derivedRef.podSection.template reset<options.minified>(rootIter, length);
				state.tape	  = derivedRef.podSection.begin();
				state.tapeEnd = derivedRef.podSection.end();
				return derivedRef.podSection.getTapeCount();
			}
			derivedRef.section.template reset<options.minified>(rootIter, length);
			state.tape	  = derivedRef.section.begin();
			state.tapeEnd = derivedRef.section.end();
			return derivedRef.section.getTapeCount();
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };
	};

}
