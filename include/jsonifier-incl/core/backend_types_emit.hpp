/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/backend_types_emit.hpp
 */

namespace jsonifier {

	template<> struct backend_types<::JSONIFIER_NAMESPACE::default_backend> {
		static constexpr uint64_t bytesPerStep{ ::JSONIFIER_NAMESPACE::simdBytesPerStep };

		template<typename derived_type> using json_printer	   = ::JSONIFIER_INTERNAL_NAMESPACE::json_printer<derived_type>;
		template<typename derived_type> using prettifier	   = ::JSONIFIER_INTERNAL_NAMESPACE::prettifier<derived_type>;
		template<typename derived_type> using serializer	   = ::JSONIFIER_INTERNAL_NAMESPACE::serializer<derived_type>;
		template<typename derived_type> using validator		   = ::JSONIFIER_INTERNAL_NAMESPACE::validator<derived_type>;
		template<typename derived_type> using minifier		   = ::JSONIFIER_INTERNAL_NAMESPACE::minifier<derived_type>;
		template<typename derived_type> using parser		   = ::JSONIFIER_INTERNAL_NAMESPACE::parser<derived_type>;
		template<typename derived_type> using generic_iterator = ::JSONIFIER_INTERNAL_NAMESPACE::generic_iterator<derived_type>;

		template<uint64_t initialBufferSize> using pod_string_reader =
			::JSONIFIER_INTERNAL_NAMESPACE::pod_simd_string_reader<::JSONIFIER_NAMESPACE::default_backend, initialBufferSize>;
		template<uint64_t initialBufferSize> using string_reader = ::JSONIFIER_INTERNAL_NAMESPACE::simd_string_reader<initialBufferSize>;
	};

}
