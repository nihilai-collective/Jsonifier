# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/jsonifier_detect_architecture.cmake

include(FetchContent)

FetchContent_Declare(
    voided_hw_detection
    GIT_REPOSITORY https://github.com/nihilai-collective/voided-hw-detection.git
    GIT_TAG main
)
FetchContent_MakeAvailable(voided_hw_detection)

voided_hw_detect_cpu(
    PREFIX JSONIFIER
    NO_DEFINITIONS
    TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jsonifier_cpu_properties.hpp.in"
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_properties.hpp"
)

if(TARGET voided_hw_jsonifier_cpu AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    get_target_property(jsonifier_cpu_flags voided_hw_jsonifier_cpu INTERFACE_COMPILE_OPTIONS)
    if(jsonifier_cpu_flags)
        list(FILTER jsonifier_cpu_flags EXCLUDE REGEX ":/arch:AVX[0-9]*>$")
        list(FILTER jsonifier_cpu_flags EXCLUDE REGEX ":-m(avx2|avx512[a-z0-9]*|fma|f16c)>$")
        set_property(TARGET voided_hw_jsonifier_cpu PROPERTY INTERFACE_COMPILE_OPTIONS "${jsonifier_cpu_flags}")
        message(STATUS "Jsonifier runtime dispatch: compiling at the AVX baseline; wider backends are selected at runtime.")
    endif()
endif()

file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp" "/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
 */
#pragma once

#undef JSONIFIER_CPU_INSTRUCTIONS
#define JSONIFIER_CPU_INSTRUCTIONS ${JSONIFIER_SELECTED_CPU_INSTRUCTIONS}

#undef JSONIFIER_SVE2_VECTOR_BITS
#define JSONIFIER_SVE2_VECTOR_BITS ${JSONIFIER_SELECTED_SVE2_VECTOR_BITS}

#if !defined(JSONIFIER_CHECK_FOR_INSTRUCTION)
	#define JSONIFIER_CHECK_FOR_INSTRUCTION(x) (JSONIFIER_CPU_INSTRUCTIONS & x)
#endif

#if !defined(JSONIFIER_LZCNT)
	#define JSONIFIER_LZCNT (1 << 0)
#endif
#if !defined(JSONIFIER_POPCNT)
	#define JSONIFIER_POPCNT (1 << 1)
#endif
#if !defined(JSONIFIER_BMI)
	#define JSONIFIER_BMI (1 << 2)
#endif
#if !defined(JSONIFIER_CLMUL)
	#define JSONIFIER_CLMUL (1 << 3)
#endif
#if !defined(JSONIFIER_NEON)
	#define JSONIFIER_NEON (1 << 4)
#endif
#if !defined(JSONIFIER_AVX)
	#define JSONIFIER_AVX (1 << 5)
#endif
#if !defined(JSONIFIER_AVX2)
	#define JSONIFIER_AVX2 (1 << 6)
#endif
#if !defined(JSONIFIER_AVX512)
	#define JSONIFIER_AVX512 (1 << 7)
#endif
#if !defined(JSONIFIER_SVE2)
	#define JSONIFIER_SVE2 (1 << 8)
#endif

#if !defined(JSONIFIER_ANY_AVX)
	#define JSONIFIER_ANY_AVX (JSONIFIER_AVX | JSONIFIER_AVX2 | JSONIFIER_AVX512)
#endif

#if !defined(JSONIFIER_ANY_SIMD)
	#define JSONIFIER_ANY_SIMD (JSONIFIER_AVX | JSONIFIER_AVX2 | JSONIFIER_AVX512 | JSONIFIER_NEON | JSONIFIER_SVE2)
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON) && JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	#error \"JSONIFIER_NEON and JSONIFIER_SVE2 are mutually exclusive backends - every SVE2 part also reports NEON, so exactly one must be selected.\"
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2) && JSONIFIER_SVE2_VECTOR_BITS == 0
	#error \"JSONIFIER_SVE2 is selected but JSONIFIER_SVE2_VECTOR_BITS is 0 - the fixed-length SVE2 typedefs require a measured vector length.\"
#endif
")
