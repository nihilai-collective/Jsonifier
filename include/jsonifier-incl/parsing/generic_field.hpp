/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_field.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/generic_value.hpp>

namespace jsonifier::generic {

	template<bool minified> class generic_field {
	  public:
		using document_state = generic::document_state;

		JSONIFIER_INLINE string_view key() const noexcept {
			read_buffer_ptr keyStart = doc->root + *iter + 1;
			read_buffer_ptr keyEnd	 = doc->root + *(iter + 1) - 1;
			if constexpr (!minified) {
				while (keyEnd > keyStart && *keyEnd != internal::uint_c_v<'"'>) {
					--keyEnd;
				}
			}
			return string_view{ std::bit_cast<const char*>(keyStart), static_cast<uint64_t>(keyEnd - keyStart) };
		}

		JSONIFIER_INLINE generic_field(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew, error_code errNew = error_code::success) noexcept
			: err{ errNew }, iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
		}

		[[nodiscard]] JSONIFIER_INLINE error_code unescapedKey(std::string_view& out) const noexcept {
			return generic_value<minified>{ doc, iter, depth }.unescape(iter, out);
		}

		JSONIFIER_INLINE generic_value<minified> value() const noexcept {
			return err == error_code::success ? generic_value<minified>{ doc, iter + 2, depth } : generic_value<minified>{ err };
		}

		JSONIFIER_INLINE bool keyEquals(string_view keyNew) const noexcept {
			return doc->template keyMatches<minified>(iter, keyNew);
		}

		JSONIFIER_INLINE string_view rawKey() const noexcept {
			return doc->template rawKey<minified>(iter);
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE generic_field() noexcept = default;

	  protected:
		error_code err{ error_code::success };
		read_structural_index_ptr iter{};
		document_state* doc{};
		uint64_t depth{};
	};

}
