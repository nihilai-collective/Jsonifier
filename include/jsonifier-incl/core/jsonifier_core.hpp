/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/jsonifier_core.hpp
 */
#pragma once

#include <jsonifier-incl/core/prixon_core.hpp>

namespace jsonifier {

	template<uint64_t initialBufferSize = 1024 * 1024> class jsonifier_core : public prixon_core,
																			  public internal::json_printer<jsonifier_core<initialBufferSize>>,
																			  public internal::prettifier<jsonifier_core<initialBufferSize>>,
																			  public internal::serializer<jsonifier_core<initialBufferSize>>,
																			  public internal::validator<jsonifier_core<initialBufferSize>>,
																			  public internal::minifier<jsonifier_core<initialBufferSize>>,
																			  public internal::parser<jsonifier_core<initialBufferSize>> {
	  public:
		friend struct internal::json_printer<jsonifier_core<initialBufferSize>>;
		friend struct internal::prettifier<jsonifier_core<initialBufferSize>>;
		friend struct internal::serializer<jsonifier_core<initialBufferSize>>;
		friend struct internal::validator<jsonifier_core<initialBufferSize>>;
		friend struct internal::minifier<jsonifier_core<initialBufferSize>>;
		friend struct internal::parser<jsonifier_core<initialBufferSize>>;

		inline jsonifier_core() noexcept = default;

		inline jsonifier_core(jsonifier_core&& other) noexcept
			: podSection(internal::move(other.podSection)), section(internal::move(other.section)), stringBuffer(internal::move(other.stringBuffer)),
			  errors(internal::move(other.errors)) {
		}

		inline jsonifier_core& operator=(jsonifier_core&& other) noexcept {
			if (this != &other) [[likely]] {
				podSection	 = internal::move(other.podSection);
				stringBuffer = internal::move(other.stringBuffer);
				section		 = internal::move(other.section);
				errors		 = internal::move(other.errors);
			}
			return *this;
		}

		inline jsonifier_core(const jsonifier_core& other) noexcept : podSection(other.podSection), section(other.section), stringBuffer(other.stringBuffer), errors(other.errors) {
		}

		inline jsonifier_core& operator=(const jsonifier_core& other) noexcept {
			if (this != &other) [[likely]] {
				podSection	 = other.podSection;
				stringBuffer = other.stringBuffer;
				section		 = other.section;
				errors		 = other.errors;
			}
			return *this;
		}

		inline ~jsonifier_core() noexcept = default;

		inline std::vector<internal::error>& getErrors() noexcept {
			return errors;
		}

		inline const std::vector<internal::error>& getErrors() const noexcept {
			return errors;
		}

	  protected:
		internal::pod_simd_string_reader<simdBytesPerStep> podSection{};
		internal::simd_string_reader<initialBufferSize> section{};
		string_base<initialBufferSize> stringBuffer{};
		std::vector<internal::error> errors{};
	};

}
