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

file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp" "/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
 */
#pragma once

${JSONIFIER_CPU_MACROS}
")
