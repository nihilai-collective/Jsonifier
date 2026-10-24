/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/jsonifier_core.hpp
 */
#pragma once

#include <jsonifier-incl/core/prixon_core.hpp>
#include <jsonifier-incl/simd/simd_config.hpp>
#include <jsonifier-incl/simd/avx.hpp>
#include <jsonifier-incl/simd/neon.hpp>
#include <jsonifier-incl/simd/sve2.hpp>
#include <jsonifier-incl/simd/fallback.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/simd/sve2_stage1.hpp>
#include <jsonifier-incl/simd/add_tape_values.hpp>
#include <jsonifier-incl/simd/utf8_validation.hpp>
#include <jsonifier-incl/utilities/compare.hpp>
#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/simd.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>
#include <jsonifier-incl/parsing/validator.hpp>
#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/serializing/serializer.hpp>
#include <jsonifier-incl/parsing/parse_impl.hpp>
#include <jsonifier-incl/serializing/serialize_impl.hpp>
#include <jsonifier-incl/serializing/minifier.hpp>
#include <jsonifier-incl/serializing/prettifier.hpp>
#include <jsonifier-incl/parsing/generic_iterator.hpp>

namespace jsonifier {

	template<uint64_t initialBufferSize = 1024 * 1024> class jsonifier_core : public prixon_core,
																			  public internal::json_printer<jsonifier_core<initialBufferSize>>,
																			  public internal::prettifier<jsonifier_core<initialBufferSize>>,
																			  public internal::serializer<jsonifier_core<initialBufferSize>>,
																			  public internal::validator<jsonifier_core<initialBufferSize>>,
																			  public internal::minifier<jsonifier_core<initialBufferSize>>,
																			  public internal::parser<jsonifier_core<initialBufferSize>>,
																			  public internal::generic_iterator<jsonifier_core<initialBufferSize>> {
	  public:
		friend struct internal::json_printer<jsonifier_core>;
		friend struct internal::prettifier<jsonifier_core>;
		friend struct internal::serializer<jsonifier_core>;
		friend struct internal::validator<jsonifier_core>;
		friend struct internal::minifier<jsonifier_core>;
		friend struct internal::parser<jsonifier_core>;
		friend struct internal::generic_iterator<jsonifier_core>;

		jsonifier_core() noexcept = default;

		jsonifier_core(jsonifier_core&& other) noexcept
			: podSection{ internal::move(other.podSection) }, section{ internal::move(other.section) }, stringBuffer{ internal::move(other.stringBuffer) },
			  errors{ internal::move(other.errors) }, genericData{ internal::move(other.genericData) } {
		}

		jsonifier_core& operator=(jsonifier_core&& other) noexcept {
			if (this != &other) [[likely]] {
				podSection	 = internal::move(other.podSection);
				section		 = internal::move(other.section);
				stringBuffer = internal::move(other.stringBuffer);
				errors		 = internal::move(other.errors);
				genericData	 = internal::move(other.genericData);
			}
			return *this;
		}

		jsonifier_core& operator=(const jsonifier_core& other) = delete;
		jsonifier_core(const jsonifier_core& other)			   = delete;

		template<parse_options options = parse_options{ .newLineDelimited = true }, typename buffer_type>
		generic::document_stream<jsonifier_core, options> iterateMany(const buffer_type& in, uint64_t batchSize = generic::defaultBatchSize) noexcept {
			return generic::document_stream<jsonifier_core, options>{ *this, std::bit_cast<read_buffer_ptr>(in.data()), in.size(), batchSize };
		}

		const std::vector<internal::error>& getErrors() const noexcept {
			return errors;
		}

		std::vector<internal::error>& getErrors() noexcept {
			return errors;
		}

		~jsonifier_core() noexcept = default;

	  protected:
		template<typename, parse_options> friend class generic::document_stream;

		internal::pod_simd_string_reader<simdBytesPerStep> podSection{};
		internal::simd_string_reader<initialBufferSize> section{};
		string_base<initialBufferSize> stringBuffer{};
		std::vector<internal::error> errors{};
		generic::generic_storage genericData{};

		JSONIFIER_INLINE generic::generic_storage& genericStorage() noexcept {
			return genericData;
		}
	};

	inline const char* activeBackendName() noexcept {
		return cpuArchName;
	}

}
