// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/jsonifier_core.hpp
#pragma once

#include <jsonifier-incl/core/prixon_core.hpp>

namespace jsonifier {

	template<uint64_t initialBufferSize = 1024 * 1024> class jsonifier_core : public prixon_core<initialBufferSize> {
	  public:
		friend struct internal::json_printer;
		friend class internal::prettifier<jsonifier_core<initialBufferSize>>;
		friend class internal::serializer<jsonifier_core<initialBufferSize>>;
		friend class internal::validator<jsonifier_core<initialBufferSize>>;
		friend class internal::minifier<jsonifier_core<initialBufferSize>>;
		friend class internal::parser<jsonifier_core<initialBufferSize>>;

		jsonifier_core() noexcept = default;

		jsonifier_core& operator=(jsonifier_core&& other) noexcept {
			if (this != &other) [[likely]] {
				stringBuffer = internal::move(other.stringBuffer);
				section		 = internal::move(other.section);
				errors		 = internal::move(other.errors);
			}
			return *this;
		}

		jsonifier_core(jsonifier_core&& other) noexcept : prettifier{}, serializer{}, validator{}, minifier{}, parser{} {
			*this = internal::move(other);
		}

		jsonifier_core& operator=(const jsonifier_core& other) noexcept {
			if (this != &other) [[likely]] {
				stringBuffer = other.stringBuffer;
				section		 = other.section;
				errors		 = other.errors;
			}
			return *this;
		}

		jsonifier_core(const jsonifier_core& other) noexcept : prettifier{}, serializer{}, validator{}, minifier{}, parser{} {
			*this = other;
		}

		std::vector<internal::error>& getErrors() noexcept {
			return errors;
		}

		~jsonifier_core() noexcept = default;

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
