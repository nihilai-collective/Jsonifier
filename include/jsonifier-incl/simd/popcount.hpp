// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/popcount.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>

namespace jsonifier::simd {

	template<concepts::uint16_types value_type> JSONIFIER_INLINE value_type popcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<value_type>(__builtin_popcount(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<value_type>(__popcnt16(static_cast<uint16_t>(value)));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountOneBits(value));
	#else
		return static_cast<value_type>(std::popcount(value));
	#endif
#else
		return static_cast<value_type>(std::popcount(value));
#endif
	}

	template<concepts::uint32_types value_type> JSONIFIER_INLINE value_type popcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<value_type>(__builtin_popcount(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<value_type>(__popcnt(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountOneBits(value));
	#else
		return static_cast<value_type>(std::popcount(value));
	#endif
#else
		return static_cast<value_type>(std::popcount(value));
#endif
	}

	template<concepts::uint64_types value_type> JSONIFIER_INLINE value_type popcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<value_type>(__builtin_popcountll(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<value_type>(__popcnt64(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountOneBits64(value));
	#else
		return static_cast<value_type>(std::popcount(value));
	#endif
#else
		return static_cast<value_type>(std::popcount(value));
#endif
	}

}
