/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/backend_passes.hpp
 */
#pragma once

#include <jsonifier-incl/core/prixon_core.hpp>
#include <jsonifier-incl/core/backend_pass_body.hpp>

namespace jsonifier {

	template<jsonifier_backend backend> struct backend_types;

}

#include <jsonifier-incl/core/backend_types_emit.hpp>

#if JSONIFIER_CONFIGURED_AVX_TIER >= 2

	#undef JSONIFIER_NAMESPACE
	#undef JSONIFIER_INTERNAL_NAMESPACE
	#undef JSONIFIER_BACKEND_PASS
	#undef JSONIFIER_CPU_INSTRUCTIONS
	#define JSONIFIER_NAMESPACE jsonifier::avx2
	#define JSONIFIER_INTERNAL_NAMESPACE jsonifier::avx2::internal
	#define JSONIFIER_BACKEND_PASS 1
	#define JSONIFIER_CPU_INSTRUCTIONS JSONIFIER_AVX2_TIER_INSTRUCTIONS

namespace jsonifier::avx2 {
	using namespace ::jsonifier;
}

namespace jsonifier::avx2::internal {
	using namespace ::jsonifier::internal;
}

namespace jsonifier::avx2::internal::simd {
	using namespace ::jsonifier::internal::simd;
}

	#include <jsonifier-incl/core/backend_pass_reset.hpp>

	#if JSONIFIER_COMPILER_CLANG
		#pragma clang attribute push(__attribute__((target("avx2,avx,sse4.2,bmi,bmi2,lzcnt,popcnt,pclmul"))), apply_to = function)
	#elif JSONIFIER_COMPILER_GCC
		#pragma GCC push_options
		#pragma GCC target("avx2,avx,sse4.2,bmi,bmi2,lzcnt,popcnt,pclmul")
	#endif

	#include <jsonifier-incl/core/backend_pass_body.hpp>

	#if JSONIFIER_COMPILER_CLANG
		#pragma clang attribute pop
	#elif JSONIFIER_COMPILER_GCC
		#pragma GCC pop_options
	#endif

	#include <jsonifier-incl/core/backend_types_emit.hpp>

#endif

#if JSONIFIER_CONFIGURED_AVX_TIER >= 3

	#undef JSONIFIER_NAMESPACE
	#undef JSONIFIER_INTERNAL_NAMESPACE
	#undef JSONIFIER_BACKEND_PASS
	#undef JSONIFIER_CPU_INSTRUCTIONS
	#define JSONIFIER_NAMESPACE jsonifier::avx512
	#define JSONIFIER_INTERNAL_NAMESPACE jsonifier::avx512::internal
	#define JSONIFIER_BACKEND_PASS 2
	#define JSONIFIER_CPU_INSTRUCTIONS JSONIFIER_AVX512_TIER_INSTRUCTIONS

namespace jsonifier::avx512 {
	using namespace ::jsonifier;
}

namespace jsonifier::avx512::internal {
	using namespace ::jsonifier::internal;
}

namespace jsonifier::avx512::internal::simd {
	using namespace ::jsonifier::internal::simd;
}

	#include <jsonifier-incl/core/backend_pass_reset.hpp>

	#if JSONIFIER_COMPILER_CLANG
		#pragma clang attribute push(__attribute__((target("avx512f,avx512bw,avx512vbmi2,avx2,avx,sse4.2,bmi,bmi2,lzcnt,popcnt,pclmul"))), apply_to = function)
	#elif JSONIFIER_COMPILER_GCC
		#pragma GCC push_options
		#pragma GCC target("avx512f,avx512bw,avx512vbmi2,avx2,avx,sse4.2,bmi,bmi2,lzcnt,popcnt,pclmul")
	#endif

	#include <jsonifier-incl/core/backend_pass_body.hpp>

	#if JSONIFIER_COMPILER_CLANG
		#pragma clang attribute pop
	#elif JSONIFIER_COMPILER_GCC
		#pragma GCC pop_options
	#endif

	#include <jsonifier-incl/core/backend_types_emit.hpp>

#endif

#if JSONIFIER_CONFIGURED_AVX_TIER >= 2
	#undef JSONIFIER_NAMESPACE
	#undef JSONIFIER_INTERNAL_NAMESPACE
	#undef JSONIFIER_BACKEND_PASS
	#undef JSONIFIER_CPU_INSTRUCTIONS
	#define JSONIFIER_NAMESPACE jsonifier
	#define JSONIFIER_INTERNAL_NAMESPACE jsonifier::internal
	#define JSONIFIER_BACKEND_PASS 0
	#define JSONIFIER_CPU_INSTRUCTIONS JSONIFIER_AVX_TIER_INSTRUCTIONS
#endif
