/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic.hpp
 */
#pragma once

#include <jsonifier-incl/core/jsonifier_core.hpp>
#include <jsonifier-incl/parsing/generic_document.hpp>

namespace jsonifier::generic {

	template<uint64_t initialBufferSize> class parser {
	  public:
		static constexpr uint64_t defaultBatchSize{ generic::defaultBatchSize };

		template<parse_options options = parse_options{}, typename buffer_type> inline document iterate(const buffer_type& in) noexcept {
			return core.template iterate<options>(in);
		}

		template<parse_options options = parse_options{ .newLineDelimited = true }, typename buffer_type>
		inline document_stream<parser, options> iterateMany(const buffer_type& in, uint64_t batchSize = defaultBatchSize) noexcept {
			return document_stream<parser, options>{ *this, std::bit_cast<read_buffer_ptr>(in.data()), in.size(), batchSize };
		}

		inline parser() noexcept = default;

		parser& operator=(const parser&) = delete;
		parser(const parser&)			 = delete;

	  protected:
		template<typename, parse_options> friend class document_stream;

		template<parse_options options> inline uint64_t indexGenericWindow(read_buffer_ptr rootIter, uint64_t length) noexcept {
			return core.template indexGenericWindow<options>(rootIter, length);
		}

		inline generic_storage& genericStorage() noexcept {
			return core.genericStorage();
		}

		jsonifier_core<initialBufferSize> core{};
	};

}
