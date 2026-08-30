// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/lzcount.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>

namespace jsonifier::internal::simd {

	template<uint16_types value_type> JSONIFIER_INLINE value_type lzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return value == 0 ? static_cast<value_type>(16) : static_cast<value_type>(__builtin_clz(value) - 16);
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return value == 0 ? static_cast<value_type>(16) : static_cast<value_type>(__lzcnt16(static_cast<uint16_t>(value)));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountLeadingZeros16(static_cast<uint16_t>(value)));
	#else
		return static_cast<value_type>(std::countl_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countl_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE value_type lzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_lzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(__lzcnt(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? static_cast<value_type>(32) : static_cast<value_type>(__builtin_clz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountLeadingZeros(value));
	#else
		return static_cast<value_type>(std::countl_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countl_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE value_type lzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_lzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(__lzcnt64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? static_cast<value_type>(64) : static_cast<value_type>(__builtin_clzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountLeadingZeros64(value));
	#else
		return static_cast<value_type>(std::countl_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countl_zero(value));
#endif
	}

}
