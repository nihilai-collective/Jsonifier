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
#include <string_view>
#include <utility>
#include <string>
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

template<typename... arg_types> inline void jsonifierFailMemcpyImpl(arg_types&&...) {
	static_assert(sizeof...(arg_types) == 0,
		"Sorry, but un-constrained memcpy is banned in this library! Only use our public-facing include <jsonifier> in your code! Or, if you're inside our own headers, remove "
		"the std library include you just added.");
}

namespace std {

	template<typename... arg_types> inline void jsonifierFailMemcpyImpl(arg_types&&... args) {
		::jsonifierFailMemcpyImpl(args...);
	}

}

namespace jsonifier {

	template<uint64_t size, typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void pow2MemcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src) noexcept {
		static_assert(std::has_single_bit(size), "Sorry, but you can only memcpy a power-of-2 size.");
		std::memcpy(dst, src, size);
	}

	template<typename value_type_01, typename value_type_02>
	JSONIFIER_INLINE void memcpyWrapper(value_type_01* __restrict dst, const value_type_02* __restrict src, uint64_t size) noexcept {
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
#define memcpy(...) jsonifierFailMemcpyImpl(__VA_ARGS__)

	struct serialize_options {
		uint64_t indentSize{ 3 };
		char indentChar{ ' ' };
		uint64_t indent{};
		bool prettify{};
	};

	struct parse_options {
		bool nullTerminated{ true };
		uint64_t maxDepth{ 1024 };
		bool newLineDelimited{};
		bool partialRead{};
		bool knownOrder{};
		bool minified{};
	};

	struct prettify_options {
		uint64_t indentSize{ 3 };
		char indentChar{ ' ' };
	};

}
