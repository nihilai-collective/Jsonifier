// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/bmi.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>

namespace jsonifier::internal::simd {

	template<uint16_types value_type> JSONIFIER_INLINE value_type blsr(value_type value) noexcept {
		return static_cast<value_type>(value & (value - 1));
	}

	template<uint32_types value_type> JSONIFIER_INLINE value_type blsr(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI)
		return static_cast<value_type>(_blsr_u32(value));
#else
		return static_cast<value_type>(value & (value - 1));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE value_type blsr(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI)
		return static_cast<value_type>(_blsr_u64(value));
#else
		return static_cast<value_type>(value & (value - 1));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE value_type tzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return value == 0 ? static_cast<value_type>(16) : static_cast<value_type>(__builtin_ctz(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return value == 0 ? static_cast<value_type>(16) : static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountTrailingZeros16(static_cast<uint16_t>(value)));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE value_type tzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? static_cast<value_type>(32) : static_cast<value_type>(__builtin_ctz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountTrailingZeros(value));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE value_type tzcnt(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? static_cast<value_type>(64) : static_cast<value_type>(__builtin_ctzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountTrailingZeros64(value));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE value_type tzcntUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<value_type>(__builtin_ctz(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<value_type>(_CountTrailingZeros(value));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE value_type tzcntUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(__builtin_ctz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountTrailingZeros(value));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE value_type tzcntUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<value_type>(__builtin_ctzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<value_type>(_CountTrailingZeros64(value));
	#else
		return static_cast<value_type>(std::countr_zero(value));
	#endif
#else
		return static_cast<value_type>(std::countr_zero(value));
#endif
	}

}
