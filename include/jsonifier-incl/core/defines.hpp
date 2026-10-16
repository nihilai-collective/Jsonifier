/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/defines.hpp
 */
#pragma once

#include <jsonifier-incl/simd/jsonifier_cpu_instructions.hpp>
#include <source_location>
#include <unordered_map>
#include <string_view>
#include <algorithm>
#include <optional>
#include <variant>
#include <cstring>
#include <utility>
#include <memory>
#include <cfloat>
#include <chrono>
#include <vector>
#include <limits>
#include <bit>

#ifndef JSONIFIER_SVE2_VECTOR_BITS
	#define JSONIFIER_SVE2_VECTOR_BITS 128
#endif

#if !defined(JSONIFIER_ARCH_X64)
	#if defined(__x86_64__) || defined(_M_X64)
		#define JSONIFIER_ARCH_X64 1
	#else
		#define JSONIFIER_ARCH_X64 0
	#endif
#endif

#if !defined(JSONIFIER_ARCH_ARM64)
	#if defined(__aarch64__) || defined(_M_ARM64)
		#define JSONIFIER_ARCH_ARM64 1
	#else
		#define JSONIFIER_ARCH_ARM64 0
	#endif
#endif

#if !defined(JSONIFIER_PLATFORM_WINDOWS)
	#if defined(_WIN32)
		#define JSONIFIER_PLATFORM_WINDOWS 1
	#else
		#define JSONIFIER_PLATFORM_WINDOWS 0
	#endif
#endif

#if !defined(JSONIFIER_PLATFORM_ANDROID)
	#if defined(__ANDROID__)
		#define JSONIFIER_PLATFORM_ANDROID 1
	#else
		#define JSONIFIER_PLATFORM_ANDROID 0
	#endif
#endif

#if !defined(JSONIFIER_PLATFORM_LINUX)
	#if defined(__linux__) && !defined(__ANDROID__)
		#define JSONIFIER_PLATFORM_LINUX 1
	#else
		#define JSONIFIER_PLATFORM_LINUX 0
	#endif
#endif

#if !defined(JSONIFIER_PLATFORM_MAC)
	#if defined(__APPLE__)
		#define JSONIFIER_PLATFORM_MAC 1
	#else
		#define JSONIFIER_PLATFORM_MAC 0
	#endif
#endif

#if !defined(JSONIFIER_COMPILER_CLANG)
	#if defined(__clang__)
		#define JSONIFIER_COMPILER_CLANG 1
	#else
		#define JSONIFIER_COMPILER_CLANG 0
	#endif
#endif

#if !defined(JSONIFIER_COMPILER_GCC)
	#if defined(__GNUC__) && !defined(__clang__) && !defined(__INTEL_LLVM_COMPILER) && !defined(__NVCOMPILER)
		#define JSONIFIER_COMPILER_GCC 1
	#else
		#define JSONIFIER_COMPILER_GCC 0
	#endif
#endif

#if !defined(JSONIFIER_COMPILER_MSVC)
	#if defined(_MSC_VER) && !defined(__clang__)
		#define JSONIFIER_COMPILER_MSVC 1
	#else
		#define JSONIFIER_COMPILER_MSVC 0
	#endif
#endif

#if !defined(JSONIFIER_OPTIMIZED)
	#if defined(__OPTIMIZE__) || (JSONIFIER_COMPILER_MSVC && !defined(_DEBUG))
		#define JSONIFIER_OPTIMIZED 1
	#else
		#define JSONIFIER_OPTIMIZED 0
	#endif
#endif

#if !defined(JSONIFIER_INLINE)
	#if JSONIFIER_OPTIMIZED
		#if JSONIFIER_COMPILER_MSVC
			#define JSONIFIER_INLINE [[msvc::forceinline]] inline
		#else
			#define JSONIFIER_INLINE inline __attribute__((always_inline))
		#endif
	#else
		#define JSONIFIER_INLINE inline
	#endif
#endif

#if !defined(JSONIFIER_NOINLINE)
	#if JSONIFIER_COMPILER_MSVC
		#define JSONIFIER_NOINLINE __declspec(noinline) inline
	#else
		#define JSONIFIER_NOINLINE inline __attribute__((noinline))
	#endif
#endif

#if !defined(JSONIFIER_NO_SANITIZE_ADDRESS)
	#if JSONIFIER_COMPILER_MSVC
		#define JSONIFIER_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
	#else
		#define JSONIFIER_NO_SANITIZE_ADDRESS __attribute__((no_sanitize_address))
	#endif
#endif

#if !defined(JSONIFIER_LIFETIME_BOUND)
	#if JSONIFIER_COMPILER_CLANG
		#define JSONIFIER_LIFETIME_BOUND [[clang::lifetimebound]]
	#elif JSONIFIER_COMPILER_MSVC
		#define JSONIFIER_LIFETIME_BOUND [[msvc::lifetimebound]]
	#else
		#define JSONIFIER_LIFETIME_BOUND
	#endif
#endif

#if !defined(JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS)
	#if JSONIFIER_COMPILER_MSVC
		#define JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
	#else
		#define JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS [[no_unique_address]]
	#endif
#endif

#if !defined(JSONIFIER_INLINE_EXCEPT_MAC_GCC)
	#if JSONIFIER_OPTIMIZED
		#if !(JSONIFIER_COMPILER_MSVC || (JSONIFIER_PLATFORM_MAC && JSONIFIER_COMPILER_GCC))
			#define JSONIFIER_INLINE_EXCEPT_MAC_GCC inline __attribute__((always_inline))
		#else
			#define JSONIFIER_INLINE_EXCEPT_MAC_GCC inline
		#endif
	#else
		#define JSONIFIER_INLINE_EXCEPT_MAC_GCC inline
	#endif
#endif

#if JSONIFIER_PLATFORM_WINDOWS && !defined(NOMINMAX)
	#define NOMINMAX
#endif

#if JSONIFIER_PLATFORM_WINDOWS && !defined(WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif

#if !JSONIFIER_COMPILER_MSVC && !JSONIFIER_COMPILER_GCC && !JSONIFIER_COMPILER_CLANG
	#error "Sorry, this compiler is not supported."
#endif

#if !JSONIFIER_PLATFORM_WINDOWS && !JSONIFIER_PLATFORM_MAC && !JSONIFIER_PLATFORM_LINUX && !JSONIFIER_PLATFORM_ANDROID
	#error "Sorry, this platform is not supported."
#endif

#if !JSONIFIER_ARCH_ARM64 && !JSONIFIER_ARCH_X64
	#error "Sorry, this arch is not supported."
#endif

#if JSONIFIER_ARCH_X64 && JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
		#define JSONIFIER_CONFIGURED_AVX_TIER 3
	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
		#define JSONIFIER_CONFIGURED_AVX_TIER 2
	#else
		#define JSONIFIER_CONFIGURED_AVX_TIER 1
	#endif
	#if defined(JSONIFIER_MAX_AVX_TIER) && JSONIFIER_CONFIGURED_AVX_TIER > JSONIFIER_MAX_AVX_TIER
		#undef JSONIFIER_CONFIGURED_AVX_TIER
		#define JSONIFIER_CONFIGURED_AVX_TIER JSONIFIER_MAX_AVX_TIER
	#endif
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_LZCNT)
		#define JSONIFIER_CONFIGURED_LZCNT JSONIFIER_LZCNT
	#else
		#define JSONIFIER_CONFIGURED_LZCNT 0
	#endif
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_POPCNT)
		#define JSONIFIER_CONFIGURED_POPCNT JSONIFIER_POPCNT
	#else
		#define JSONIFIER_CONFIGURED_POPCNT 0
	#endif
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_BMI)
		#define JSONIFIER_CONFIGURED_BMI JSONIFIER_BMI
	#else
		#define JSONIFIER_CONFIGURED_BMI 0
	#endif
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_CLMUL)
		#define JSONIFIER_CONFIGURED_CLMUL JSONIFIER_CLMUL
	#else
		#define JSONIFIER_CONFIGURED_CLMUL 0
	#endif
	#define JSONIFIER_SCALAR_INSTRUCTIONS (JSONIFIER_CONFIGURED_LZCNT | JSONIFIER_CONFIGURED_POPCNT | JSONIFIER_CONFIGURED_BMI | JSONIFIER_CONFIGURED_CLMUL)
	#define JSONIFIER_AVX_TIER_INSTRUCTIONS (JSONIFIER_SCALAR_INSTRUCTIONS | JSONIFIER_AVX)
	#define JSONIFIER_AVX2_TIER_INSTRUCTIONS (JSONIFIER_AVX_TIER_INSTRUCTIONS | JSONIFIER_AVX2)
	#define JSONIFIER_AVX512_TIER_INSTRUCTIONS (JSONIFIER_AVX2_TIER_INSTRUCTIONS | JSONIFIER_AVX512)
	#undef JSONIFIER_CPU_INSTRUCTIONS
	#define JSONIFIER_CPU_INSTRUCTIONS JSONIFIER_AVX_TIER_INSTRUCTIONS
#else
	#define JSONIFIER_CONFIGURED_AVX_TIER 0
#endif

#define JSONIFIER_NAMESPACE jsonifier
#define JSONIFIER_INTERNAL_NAMESPACE jsonifier::internal
#define JSONIFIER_BACKEND_PASS 0

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	#include <immintrin.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	#include <arm_neon.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	#include <arm_neon_sve_bridge.h>
	#include <arm_neon.h>
	#include <arm_sve.h>
#endif

#if JSONIFIER_COMPILER_MSVC
	#include <intrin.h>
#endif

#if JSONIFIER_PLATFORM_WINDOWS
	#include <windows.h>
#elif JSONIFIER_PLATFORM_LINUX || JSONIFIER_PLATFORM_MAC || JSONIFIER_PLATFORM_ANDROID
	#include <sys/mman.h>
	#include <unistd.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
	#define JSONIFIER_ASAN_ENABLED 1
#elif defined(__has_feature)
	#if __has_feature(address_sanitizer)
		#define JSONIFIER_ASAN_ENABLED 1
	#endif
#endif
#if !defined(JSONIFIER_ASAN_ENABLED)
	#define JSONIFIER_ASAN_ENABLED 0
#endif

#if defined(__has_builtin) && !defined(ZMIJ_NO_BUILTINS)
	#define ZMIJ_HAS_BUILTIN(x) __has_builtin(x)
#else
	#define ZMIJ_HAS_BUILTIN(x) 0
#endif
#ifdef __has_cpp_attribute
	#define ZMIJ_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
	#define ZMIJ_HAS_CPP_ATTRIBUTE(x) 0
#endif

#if ZMIJ_HAS_CPP_ATTRIBUTE(unlikely)
	#define ZMIJ_UNLIKELY unlikely
#else
	#define ZMIJ_UNLIKELY
#endif

#ifdef ZMIJ_OPTIMIZE_SIZE
#elif defined(__OPTIMIZE_SIZE__)
	#define ZMIJ_OPTIMIZE_SIZE 1
#else
	#define ZMIJ_OPTIMIZE_SIZE 0
#endif
#ifndef ZMIJ_USE_EXP_STRING_TABLE
	#define ZMIJ_USE_EXP_STRING_TABLE ZMIJ_OPTIMIZE_SIZE == 0
#endif

#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
	#define ZMIJ_ASM(x) asm x
#else
	#define ZMIJ_ASM(x)
#endif

#ifdef ZMIJ_CONST_DECL

#elif JSONIFIER_ARCH_ARM64
	#define ZMIJ_CONST_DECL
#else
	#define ZMIJ_CONST_DECL static constexpr
#endif

#ifdef ZMIJ_USE_INT128
#elif defined(__SIZEOF_INT128__)
	#define ZMIJ_USE_INT128 1
#else
	#define ZMIJ_USE_INT128 0
#endif

#define JSONIFIER_FASTFLOAT_TRY(x) \
	{ \
		if (!(x)) { \
			return false; \
		} \
	}

#if !defined(JSONIFIER_IS_DIGIT)
	#define JSONIFIER_IS_DIGIT(x) ((static_cast<uint8_t>(x - '0')) < 10)
#endif

template<typename... arg_types> inline void jsonifierFailMemcpyImpl(arg_types&&...) noexcept {
	static_assert(sizeof...(arg_types) == 0,
		"Sorry, but un-constrained memcpy is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
		"the std library include you just added.");
}

template<typename... arg_types> inline void jsonifierFailMemmoveImpl(arg_types&&...) noexcept {
	static_assert(sizeof...(arg_types) == 0,
		"Sorry, but un-constrained memmove is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
		"the std library include you just added.");
}

namespace std {

	template<typename... arg_types> inline void jsonifierFailMemcpyImpl(arg_types&&... args) noexcept {
		::jsonifierFailMemcpyImpl(args...);
	}

	template<typename... arg_types> inline void jsonifierFailMemmoveImpl(arg_types&&... args) noexcept {
		::jsonifierFailMemmoveImpl(args...);
	}

}

namespace jsonifier {

	template<uint64_t size, typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void pow2MemcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memcpy a power-of-2 size.");
		std::memcpy(dst, src, size);
	}

	template<uint64_t size, typename value_type_01, typename value_type_02> JSONIFIER_INLINE void pow2MemmoveWrapper(value_type_01* dst, const value_type_02* src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memmove a power-of-2 size.");
		alignas(size) uint8_t scratch[size];
		std::memcpy(scratch, src, size);
		std::memcpy(dst, scratch, size);
	}

	template<typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void memcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src, uint64_t size) noexcept {
		std::memcpy(dst, src, size);
	}

	template<typename value_type_01, typename value_type_02> JSONIFIER_INLINE void memmoveWrapper(value_type_01* dst, const value_type_02* src, uint64_t size) noexcept {
		std::memmove(dst, src, size);
	}

}

#define reinterpret_cast jsonifier::banned_reinterpret_cast
#define const_cast jsonifier::banned_const_cast
#define dynamic_cast jsonifier::banned_dynamic_cast
#define memcpy(...) jsonifierFailMemcpyImpl(__VA_ARGS__)
#define memmove(...) jsonifierFailMemmoveImpl(__VA_ARGS__)
