// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/bit_ops.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>

namespace jsonifier::internal::simd {

	template<uint16_types value_type> JSONIFIER_INLINE uint64_t countlZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return value == 0 ? 16ULL : static_cast<uint64_t>(__builtin_clz(value) - 16);
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return value == 0 ? 16ULL : static_cast<uint64_t>(__lzcnt16(static_cast<uint16_t>(value)));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountLeadingZeros16(value));
	#else
		return static_cast<uint64_t>(std::countl_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countl_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE uint64_t countlZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_lzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(__lzcnt(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? 32ULL : static_cast<uint64_t>(__builtin_clz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountLeadingZeros(value));
	#else
		return static_cast<uint64_t>(std::countl_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countl_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE uint64_t countlZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_lzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(__lzcnt64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? 64ULL : static_cast<uint64_t>(__builtin_clzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountLeadingZeros64(value));
	#else
		return static_cast<uint64_t>(std::countl_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countl_zero(value));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE uint64_t blsr(value_type value) noexcept {
		return static_cast<uint64_t>(value & (value - 1));
	}

	template<uint32_types value_type> JSONIFIER_INLINE uint64_t blsr(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI)
		return static_cast<uint64_t>(_blsr_u32(value));
#else
		return static_cast<uint64_t>(value & (value - 1));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE uint64_t blsr(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI)
		return static_cast<uint64_t>(_blsr_u64(value));
#else
		return static_cast<uint64_t>(value & (value - 1));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE uint64_t countrZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return value == 0 ? 16ULL : static_cast<uint64_t>(__builtin_ctz(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return value == 0 ? 16ULL : static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountTrailingZeros16(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE uint64_t countrZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? 32ULL : static_cast<uint64_t>(__builtin_ctz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountTrailingZeros(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE uint64_t countrZero(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return value == 0 ? 64ULL : static_cast<uint64_t>(__builtin_ctzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountTrailingZeros64(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE uint64_t countrZeroUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<uint64_t>(__builtin_ctz(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountTrailingZeros(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE uint64_t countrZeroUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_tzcnt_u32(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(__builtin_ctz(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountTrailingZeros(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE uint64_t countrZeroUnsafe(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_ARCH_X64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_tzcnt_u64(value));
	#elif JSONIFIER_ARCH_ARM64 && (JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG)
		return static_cast<uint64_t>(__builtin_ctzll(value));
	#elif JSONIFIER_ARCH_ARM64 && JSONIFIER_COMPILER_MSVC
		return static_cast<uint64_t>(_CountTrailingZeros64(value));
	#else
		return static_cast<uint64_t>(std::countr_zero(value));
	#endif
#else
		return static_cast<uint64_t>(std::countr_zero(value));
#endif
	}

	template<uint16_types value_type> JSONIFIER_INLINE uint64_t popCount(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<uint64_t>(__builtin_popcount(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<uint64_t>(__popcnt16(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountOneBits(value));
	#else
		return static_cast<uint64_t>(std::popcount(value));
	#endif
#else
		return static_cast<uint64_t>(std::popcount(value));
#endif
	}

	template<uint32_types value_type> JSONIFIER_INLINE uint64_t popCount(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<uint64_t>(__builtin_popcount(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<uint64_t>(__popcnt(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountOneBits(value));
	#else
		return static_cast<uint64_t>(std::popcount(value));
	#endif
#else
		return static_cast<uint64_t>(std::popcount(value));
#endif
	}

	template<uint64_types value_type> JSONIFIER_INLINE uint64_t popCount(value_type value) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT) || JSONIFIER_ARCH_ARM64
	#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
		return static_cast<uint64_t>(__builtin_popcountll(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_X64
		return static_cast<uint64_t>(__popcnt64(value));
	#elif JSONIFIER_COMPILER_MSVC && JSONIFIER_ARCH_ARM64
		return static_cast<uint64_t>(_CountOneBits64(value));
	#else
		return static_cast<uint64_t>(std::popcount(value));
	#endif
#else
		return static_cast<uint64_t>(std::popcount(value));
#endif
	}

}
