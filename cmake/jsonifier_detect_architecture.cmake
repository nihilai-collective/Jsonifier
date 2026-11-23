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

#define JSONIFIER_SVE2_VECTOR_BITS ${JSONIFIER_SELECTED_SVE2_VECTOR_BITS}
#define JSONIFIER_LZCNT_PRE ${JSONIFIER_LZCNT}
#define JSONIFIER_POPCNT_PRE ${JSONIFIER_POPCNT}
#define JSONIFIER_BMI_PRE ${JSONIFIER_BMI}
#define JSONIFIER_CLMUL_PRE ${JSONIFIER_CLMUL}
#define JSONIFIER_NEON_PRE ${JSONIFIER_NEON}
#define JSONIFIER_AVX_PRE ${JSONIFIER_AVX}
#define JSONIFIER_AVX2_PRE ${JSONIFIER_AVX2}
#define JSONIFIER_AVX512_PRE ${JSONIFIER_AVX512}
#define JSONIFIER_SVE2_PRE ${JSONIFIER_SVE2}
")
