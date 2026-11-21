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
#include <stdexcept>
#include <algorithm>
#include <optional>
#include <utility>
#include <variant>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <cctype>
#include <memory>
#include <cfloat>
#include <vector>
#include <atomic>
#include <limits>
#include <bit>

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
	#if defined(__GNUC__) && !JSONIFIER_COMPILER_CLANG && !defined(__INTEL_LLVM_COMPILER) && !defined(__NVCOMPILER)
		#define JSONIFIER_COMPILER_GCC 1
	#else
		#define JSONIFIER_COMPILER_GCC 0
	#endif
#endif

#if !defined(JSONIFIER_COMPILER_MSVC)
	#if defined(_MSC_VER) && !JSONIFIER_COMPILER_CLANG
		#define JSONIFIER_COMPILER_MSVC 1
	#else
		#define JSONIFIER_COMPILER_MSVC 0
	#endif
#endif

#if defined(__LZCNT__) || (JSONIFIER_COMPILER_MSVC && defined(__AVX2__)) || JSONIFIER_LZCNT_PRE
	#define JSONIFIER_LZCNT 1
#else
	#define JSONIFIER_LZCNT 0
#endif

#if defined(__POPCNT__) || (JSONIFIER_COMPILER_MSVC && defined(__AVX__)) || JSONIFIER_POPCNT_PRE
	#define JSONIFIER_POPCNT 1
#else
	#define JSONIFIER_POPCNT 0
#endif

#if defined(__BMI__) || (JSONIFIER_COMPILER_MSVC && defined(__AVX2__)) || JSONIFIER_BMI_PRE
	#define JSONIFIER_BMI 1
#else
	#define JSONIFIER_BMI 0
#endif

#if defined(__PCLMUL__) || defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO) || (JSONIFIER_COMPILER_MSVC && (defined(__AVX2__) || JSONIFIER_ARCH_ARM64)) || \
	JSONIFIER_CLMUL_PRE
	#define JSONIFIER_CLMUL 1
#else
	#define JSONIFIER_CLMUL 0
#endif

#if defined(__AVX__) || JSONIFIER_AVX_PRE
	#define JSONIFIER_AVX 1
#else
	#define JSONIFIER_AVX 0
#endif

#if defined(__AVX2__) || JSONIFIER_AVX2_PRE
	#define JSONIFIER_AVX2 1
#else
	#define JSONIFIER_AVX2 0
#endif

#if (defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VBMI2__)) || JSONIFIER_AVX512_PRE
	#define JSONIFIER_AVX512 1
#else
	#define JSONIFIER_AVX512 0
#endif

#if defined(__ARM_FEATURE_SVE2) || JSONIFIER_SVE2_PRE
	#define JSONIFIER_SVE2 1
#else
	#define JSONIFIER_SVE2 0
#endif

#if ((defined(__ARM_NEON) || JSONIFIER_ARCH_ARM64 || JSONIFIER_NEON_PRE) && !JSONIFIER_SVE2)
	#define JSONIFIER_NEON 1
#else
	#define JSONIFIER_NEON 0
#endif

#define JSONIFIER_ANY_AVX (JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512)
#define JSONIFIER_ANY_SIMD (JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512 || JSONIFIER_NEON || JSONIFIER_SVE2)

#if JSONIFIER_NEON && JSONIFIER_SVE2
	#error "JSONIFIER_NEON and JSONIFIER_SVE2 are mutually exclusive backends - every SVE2 part also reports NEON, so exactly one must be selected."
#endif

#if JSONIFIER_SVE2 && JSONIFIER_SVE2_VECTOR_BITS == 0
	#error "JSONIFIER_SVE2 is selected but JSONIFIER_SVE2_VECTOR_BITS is 0 - the fixed-length SVE2 typedefs require a measured vector length."
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

#if !defined(JSONIFIER_INLINE_LINUX_GCC)
	#if JSONIFIER_OPTIMIZED && JSONIFIER_PLATFORM_LINUX && JSONIFIER_COMPILER_GCC
		#define JSONIFIER_INLINE_LINUX_GCC inline __attribute__((always_inline))
	#else
		#define JSONIFIER_INLINE_LINUX_GCC inline
	#endif
#endif

#if !defined(JSONIFIER_INLINE_EXCEPT_GCC)
	#if JSONIFIER_OPTIMIZED && !JSONIFIER_COMPILER_GCC
		#if JSONIFIER_COMPILER_MSVC
			#define JSONIFIER_INLINE_EXCEPT_GCC [[msvc::forceinline]] inline
		#else
			#define JSONIFIER_INLINE_EXCEPT_GCC inline __attribute__((always_inline))
		#endif
	#else
		#define JSONIFIER_INLINE_EXCEPT_GCC inline
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

#if JSONIFIER_ANY_AVX || (JSONIFIER_ARCH_X64 && (JSONIFIER_LZCNT || JSONIFIER_POPCNT || JSONIFIER_BMI))
	#include <immintrin.h>
#elif JSONIFIER_NEON
	#include <arm_neon.h>
#elif JSONIFIER_SVE2
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
	#include <sys/types.h>
	#include <sys/mman.h>
	#include <unistd.h>
	#include <cerrno>
	#include <poll.h>
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

#if JSONIFIER_COMPILER_GCC || JSONIFIER_COMPILER_CLANG
	#define JSONIFIER_ASM(x) asm x
#else
	#define JSONIFIER_ASM(x)
#endif

#if defined(JSONIFIER_USE_INT128)
#elif defined(__SIZEOF_INT128__)
	#define JSONIFIER_USE_INT128 1
#else
	#define JSONIFIER_USE_INT128 0
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

	template<typename... arg_types> inline void jsonifierFailMemmoveImpl(arg_types&&... args) noexcept {
		::jsonifierFailMemmoveImpl(args...);
	}

	template<typename... arg_types> inline void jsonifierFailMemcpyImpl(arg_types&&... args) noexcept {
		::jsonifierFailMemcpyImpl(args...);
	}

}

namespace jsonifier::internal {

	template<uint64_t size, typename value_type_01, typename value_type_02> JSONIFIER_INLINE void pow2MemmoveWrapper(value_type_01* dst, const value_type_02* src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memmove a power-of-2 size.");
		alignas(size) uint8_t scratch[size];
		std::memcpy(scratch, src, size);
		std::memcpy(dst, scratch, size);
	}

	template<typename value_type_01, typename value_type_02> JSONIFIER_INLINE void memmoveWrapper(value_type_01* dst, const value_type_02* src, uint64_t size) noexcept {
		std::memmove(dst, src, size);
	}

	template<typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void memcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src, uint64_t size) noexcept {
		std::memcpy(dst, src, size);
	}

	template<uint64_t size, typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void pow2MemcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memcpy a power-of-2 size.");
		std::memcpy(dst, src, size);
	}

}

#define reinterpret_cast jsonifier::internal::banned_reinterpret_cast
#define const_cast jsonifier::internal::banned_const_cast
#define dynamic_cast jsonifier::internal::banned_dynamic_cast
#define memcpy(...) jsonifierFailMemcpyImpl(__VA_ARGS__)
#define memmove(...) jsonifierFailMemmoveImpl(__VA_ARGS__)
