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

math(EXPR JSONIFIER_LZCNT "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 0) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_POPCNT "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 1) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_BMI "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 2) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_CLMUL "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 3) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_NEON "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 4) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_AVX "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 5) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_AVX2 "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 6) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_AVX512 "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 7) & 1" OUTPUT_FORMAT DECIMAL)
math(EXPR JSONIFIER_SVE2 "(${JSONIFIER_SELECTED_CPU_INSTRUCTIONS} >> 8) & 1" OUTPUT_FORMAT DECIMAL)

file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp" "/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
 */
#pragma once

#undef JSONIFIER_SVE2_VECTOR_BITS
#define JSONIFIER_SVE2_VECTOR_BITS ${JSONIFIER_SELECTED_SVE2_VECTOR_BITS}

#if !defined(JSONIFIER_LZCNT)
	#define JSONIFIER_LZCNT ${JSONIFIER_LZCNT}
#endif

#if !defined(JSONIFIER_POPCNT)
	#define JSONIFIER_POPCNT ${JSONIFIER_POPCNT}
#endif

#if !defined(JSONIFIER_BMI)
	#define JSONIFIER_BMI ${JSONIFIER_BMI}
#endif

#if !defined(JSONIFIER_CLMUL)
	#define JSONIFIER_CLMUL ${JSONIFIER_CLMUL}
#endif

#if !defined(JSONIFIER_NEON)
	#define JSONIFIER_NEON ${JSONIFIER_NEON}
#endif

#if !defined(JSONIFIER_AVX)
	#define JSONIFIER_AVX ${JSONIFIER_AVX}
#endif

#if !defined(JSONIFIER_AVX2)
	#define JSONIFIER_AVX2 ${JSONIFIER_AVX2}
#endif

#if !defined(JSONIFIER_AVX512)
	#define JSONIFIER_AVX512 ${JSONIFIER_AVX512}
#endif

#if !defined(JSONIFIER_SVE2)
	#define JSONIFIER_SVE2 ${JSONIFIER_SVE2}
#endif

#if !defined(JSONIFIER_ANY_AVX)
	#define JSONIFIER_ANY_AVX (JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512)
#endif

#if !defined(JSONIFIER_ANY_SIMD)
	#define JSONIFIER_ANY_SIMD (JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512 || JSONIFIER_NEON || JSONIFIER_SVE2)
#endif

#if JSONIFIER_NEON && JSONIFIER_SVE2
	#error \"JSONIFIER_NEON and JSONIFIER_SVE2 are mutually exclusive backends - every SVE2 part also reports NEON, so exactly one must be selected.\"
#endif

#if JSONIFIER_SVE2 && JSONIFIER_SVE2_VECTOR_BITS == 0
	#error \"JSONIFIER_SVE2 is selected but JSONIFIER_SVE2_VECTOR_BITS is 0 - the fixed-length SVE2 typedefs require a measured vector length.\"
#endif
")
