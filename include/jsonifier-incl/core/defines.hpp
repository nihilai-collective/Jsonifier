/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier

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
#if !defined(JSONIFIER_PLATFORM_LINUX)
	#if defined(__linux__)
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
	#if defined(__GNUC__) && !defined(__clang__)
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
	#if defined(NDEBUG)
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
#if JSONIFIER_COMPILER_MSVC && !defined(NOMINMAX)
	#define NOMINMAX
#endif
#if JSONIFIER_COMPILER_MSVC && !defined(WIN32_LEAN_AND_MEAN)
	#define WIN32_LEAN_AND_MEAN
#endif
