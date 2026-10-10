/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_iterator.hpp
 */
#if !defined(JSONIFIER_PASS_GUARD_GENERIC_ITERATOR)
	#define JSONIFIER_PASS_GUARD_GENERIC_ITERATOR

	#include <jsonifier-incl/parsing/generic_document.hpp>

namespace JSONIFIER_INTERNAL_NAMESPACE {

	template<typename derived_type_new> struct generic_iterator {
		using derived_type = derived_type_new;

		static constexpr uint64_t smallDocumentBytes{ ::JSONIFIER_NAMESPACE::simdBlocksPerStep * 64 };

		template<parse_options options = parse_options{}, typename buffer_type> inline ::jsonifier::generic::document iterate(const buffer_type& in) noexcept {
			using ::jsonifier::generic::document;
			using ::jsonifier::generic::error_code;
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
			data.state.root								= rootIter;
			data.state.end								= rootIter + length;
			data.state.cursor							= data.state.tape;
			data.state.depth							= 0;
			if (tapeCount > 1 && !::jsonifier::generic::document_state::isOpener(rootIter[data.state.tape[0]])) [[unlikely]] {
				return document{ error_code::trailing_content };
			}
			data.arena.reset(length);
			data.fieldIndex.reset();
			data.state.arena	  = &data.arena;
			data.state.fieldIndex = &data.fieldIndex;
			return document{ &data.state, data.state.tape, 0 };
		}

		template<parse_options options> inline uint64_t indexGenericWindow(read_buffer_ptr rootIter, uint64_t length) noexcept {
			::jsonifier::generic::document_state& state = derivedRef.genericStorage().state;
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

#endif
