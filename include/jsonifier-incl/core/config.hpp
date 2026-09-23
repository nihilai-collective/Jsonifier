/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/config.hpp
 */
#pragma once

#include <jsonifier-incl/simd/jsonifier_cpu_instructions.hpp>
#include <jsonifier-incl/simd/jsonifier_cpu_properties.hpp>
#include <source_location>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <variant>
#include <cstring>
#include <cstdint>
#include <memory>
#include <chrono>
#include <cfloat>
#include <atomic>
#include <vector>
#include <bit>

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	#include <immintrin.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	#include <arm_neon.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	#include <arm_neon_sve_bridge.h>
	#include <arm_neon.h>
	#include <arm_sve.h>
#endif

#if JSONIFIER_PLATFORM_WINDOWS
	#include <windows.h>
#elif JSONIFIER_PLATFORM_LINUX || JSONIFIER_PLATFORM_MAC || JSONIFIER_PLATFORM_ANDROID
	#include <sys/mman.h>
	#include <unistd.h>
#endif

template<typename... arg_types> void jsonifier_fail_memcpy_impl(arg_types&&...) {
	static_assert(sizeof...(arg_types) == 0,
		"Sorry, but un-constrained memcpy is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
		"the std library include you just added.");
}

namespace std {

	template<typename... arg_types> void jsonifier_fail_memcpy_impl(arg_types&&... args) {
		::jsonifier_fail_memcpy_impl(args...);
	}

}

namespace jsonifier {

	template<uint64_t size, typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void pow2_memcpy_wrapper(value_type_01* __restrict dst, const value_type_02* __restrict src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memcpy a power-of-2 size.");
		std::memcpy(dst, src, size);
	}

	template<typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void memcpy_wrapper(value_type_01* __restrict dst, const value_type_02* __restrict src, uint64_t size) noexcept {
		std::memcpy(dst, src, size);
	}

	template<typename... arg_types> struct banned_reinterpret_cast {
		static_assert(sizeof...(arg_types) == 0,
			"Sorry, but reinterpret_cast is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
			"the std library include you just added.");
	};

	template<typename... arg_types> struct banned_const_cast {
		static_assert(sizeof...(arg_types) == 0,
			"Sorry, but const_cast is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
			"the std library include you just added.");
	};

	template<typename... arg_types> struct banned_dynamic_cast {
		static_assert(sizeof...(arg_types) == 0,
			"Sorry, but dynamic_cast is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
			"the std library include you just added.");
	};

#define reinterpret_cast jsonifier::banned_reinterpret_cast
#define const_cast jsonifier::banned_const_cast
#define dynamic_cast jsonifier::banned_dynamic_cast
#define memcpy(...) jsonifier_fail_memcpy_impl(__VA_ARGS__)

	struct serialize_options {
		uint64_t indentSize{ 3 };
		char indentChar{ ' ' };
		uint64_t indent{};
		bool prettify{};
	};

	struct parse_options {
		bool partialRead{};
		bool knownOrder{};
		bool minified{};
		bool nullTerminated{ true };
		uint64_t maxDepth{ 1024 };
	};

}
