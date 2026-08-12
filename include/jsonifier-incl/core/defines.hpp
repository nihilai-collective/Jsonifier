// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/defines.hpp
#pragma once

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

#if !defined(JSONIFIER_DISPATCH_TABLE_COUNT)
	#if JSONIFIER_PLATFORM_MAC && JSONIFIER_COMPILER_GCC
		#define JSONIFIER_DISPATCH_TABLE_COUNT 4
	#elif JSONIFIER_PLATFORM_MAC
		#define JSONIFIER_DISPATCH_TABLE_COUNT 0
	#else
		#define JSONIFIER_DISPATCH_TABLE_COUNT 2
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

#if !defined(JSONIFIER_LIFETIME_BOUND)
	#if JSONIFIER_COMPILER_CLANG
		#define JSONIFIER_LIFETIME_BOUND [[clang::lifetimebound]]
	#elif JSONIFIER_COMPILER_MSVC
		#define JSONIFIER_LIFETIME_BOUND [[msvc::lifetimebound]]
	#else
		#define JSONIFIER_LIFETIME_BOUND
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
