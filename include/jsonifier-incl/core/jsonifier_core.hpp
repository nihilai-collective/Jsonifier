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
																			  public internal::json_printer,
																			  public internal::prettifier<jsonifier_core<initialBufferSize>>,
																			  public internal::serializer<jsonifier_core<initialBufferSize>>,
																			  public internal::validator<jsonifier_core<initialBufferSize>>,
																			  public internal::minifier<jsonifier_core<initialBufferSize>>,
																			  public internal::parser<jsonifier_core<initialBufferSize>> {
	  public:
		friend struct internal::json_printer;
		friend class internal::prettifier<jsonifier_core<initialBufferSize>>;
		friend class internal::serializer<jsonifier_core<initialBufferSize>>;
		friend class internal::validator<jsonifier_core<initialBufferSize>>;
		friend class internal::minifier<jsonifier_core<initialBufferSize>>;
		friend class internal::parser<jsonifier_core<initialBufferSize>>;

		jsonifier_core() noexcept = default;

		jsonifier_core(jsonifier_core&& other) noexcept
			: prixon_core(internal::move(other)), internal::json_printer(internal::move(other)), prettifier(internal::move(other)), serializer(internal::move(other)),
			  validator(internal::move(other)), minifier(internal::move(other)), parser(internal::move(other)), section(internal::move(other.section)),
			  stringBuffer(internal::move(other.stringBuffer)), errors(internal::move(other.errors)) {
		}

		jsonifier_core& operator=(jsonifier_core&& other) noexcept {
			if (this != &other) [[likely]] {
				prixon_core::operator=(internal::move(other));
				internal::json_printer::operator=(internal::move(other));
				prettifier::operator=(internal::move(other));
				serializer::operator=(internal::move(other));
				validator::operator=(internal::move(other));
				minifier::operator=(internal::move(other));
				parser::operator=(internal::move(other));

				section		 = internal::move(other.section);
				stringBuffer = internal::move(other.stringBuffer);
				errors		 = internal::move(other.errors);
			}
			return *this;
		}

		jsonifier_core(const jsonifier_core& other)
			: prixon_core(other), internal::json_printer(other), prettifier(other), serializer(other), validator(other), minifier(other), parser(other), section(other.section),
			  stringBuffer(other.stringBuffer), errors(other.errors) {
		}

		jsonifier_core& operator=(const jsonifier_core& other) {
			if (this != &other) [[likely]] {
				prixon_core::operator=(other);
				internal::json_printer::operator=(other);
				prettifier::operator=(other);
				serializer::operator=(other);
				validator::operator=(other);
				minifier::operator=(other);
				parser::operator=(other);

				section		 = other.section;
				stringBuffer = other.stringBuffer;
				errors		 = other.errors;
			}
			return *this;
		}

		~jsonifier_core() noexcept = default;

		std::vector<internal::error>& getErrors() noexcept {
			return errors;
		}

		const std::vector<internal::error>& getErrors() const noexcept {
			return errors;
		}

	  protected:
		using comparator = internal::json_printer;
		using prettifier = internal::prettifier<jsonifier_core<initialBufferSize>>;
		using serializer = internal::serializer<jsonifier_core<initialBufferSize>>;
		using validator	 = internal::validator<jsonifier_core<initialBufferSize>>;
		using minifier	 = internal::minifier<jsonifier_core<initialBufferSize>>;
		using parser	 = internal::parser<jsonifier_core<initialBufferSize>>;

		internal::simd_string_reader<initialBufferSize> section{};
		string_base<initialBufferSize> stringBuffer{};
		std::vector<internal::error> errors{};
	};

}
