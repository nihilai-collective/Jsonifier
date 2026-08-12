// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/config.hpp
#pragma once

#include <jsonifier-incl/simd/jsonifier_cpu_instructions.hpp>
#include <jsonifier-incl/simd/jsonifier_cpu_properties.hpp>
#include <source_location>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <optional>
#include <iomanip>
#include <variant>
#include <cstring>
#include <sstream>
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
#endif

namespace jsonifier {

	JSONIFIER_INLINE static consteval bool is_power_of_2(uint64_t value) noexcept {
		return value != 0 && (value & (value - 1)) == 0;
	}

	template<uint64_t size, typename value_type_01, typename value_type_02> JSONIFIER_INLINE void memcpy_wrapper(value_type_01* dst, const value_type_02* src) noexcept {
		static_assert(is_power_of_2(size), "Sorry, but you can only memcpy a power-of-2 size.");
		std::memcpy(dst, src, size);
	}

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
		bool validateUtf8{ true };
		bool nullTerminated{ true };
		uint64_t maxDepth{ 1024 };
	};

}
