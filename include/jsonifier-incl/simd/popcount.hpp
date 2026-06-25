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
