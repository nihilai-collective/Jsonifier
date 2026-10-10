# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/library_setup.cmake

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/jsonifier_detect_architecture.cmake)

if(NOT DEFINED JSONIFIER_SVE2_VECTOR_BITS)
    set(JSONIFIER_SVE2_VECTOR_BITS 128)
endif()

set(JSONIFIER_COMPILE_DEFINITIONS
    JSONIFIER_SVE2_VECTOR_BITS=${JSONIFIER_SVE2_VECTOR_BITS}
    JSONIFIER_ARCH_X64=$<IF:$<OR:$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},AMD64>>,1,0>
    JSONIFIER_ARCH_ARM64=$<IF:$<OR:$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},aarch64>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},ARM64>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},arm64>>,1,0>
    JSONIFIER_PLATFORM_WINDOWS=$<IF:$<PLATFORM_ID:Windows>,1,0>
    JSONIFIER_PLATFORM_ANDROID=$<IF:$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>,1,0>
    JSONIFIER_PLATFORM_LINUX=$<IF:$<AND:$<PLATFORM_ID:Linux>,$<NOT:$<STREQUAL:${CMAKE_SYSTEM_NAME},Android>>>,1,0>
    JSONIFIER_PLATFORM_MAC=$<IF:$<PLATFORM_ID:Darwin>,1,0>
    JSONIFIER_COMPILER_CLANG=$<IF:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,1,0>
    JSONIFIER_COMPILER_MSVC=$<IF:$<CXX_COMPILER_ID:MSVC>,1,0>
    JSONIFIER_COMPILER_GCC=$<IF:$<CXX_COMPILER_ID:GNU>,1,0>
    JSONIFIER_OPTIMIZED=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>,1,0>
    "JSONIFIER_INLINE=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>,$<IF:$<CXX_COMPILER_ID:MSVC>,[[msvc::forceinline]] inline,inline __attribute__((always_inline))>,inline>"
    "JSONIFIER_INLINE_EXCEPT_MAC_GCC=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>,$<IF:$<CXX_COMPILER_ID:MSVC>,inline,$<IF:$<AND:$<PLATFORM_ID:Darwin>,$<CXX_COMPILER_ID:GNU>>,inline,inline __attribute__((always_inline))>>,inline>"
    "JSONIFIER_NOINLINE=$<IF:$<CXX_COMPILER_ID:MSVC>,__declspec(noinline) inline,inline __attribute__((noinline))>"
    "JSONIFIER_NO_SANITIZE_ADDRESS=$<IF:$<CXX_COMPILER_ID:MSVC>,__declspec(no_sanitize_address),__attribute__((no_sanitize_address))>"
    "JSONIFIER_LIFETIME_BOUND=$<IF:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,[[clang::lifetimebound]],$<IF:$<CXX_COMPILER_ID:MSVC>,[[msvc::lifetimebound]],>>"
    "JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS=$<IF:$<CXX_COMPILER_ID:MSVC>,[[msvc::no_unique_address]],[[no_unique_address]]>"
    $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>
)

target_include_directories(${PROJECT_NAME}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(${PROJECT_NAME}
    INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/constexpr:steps100000000>
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fconstexpr-steps=100000000>
        $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=1000000000>
)

target_link_libraries(${PROJECT_NAME}
    INTERFACE voided_hw::jsonifier_cpu
)

target_compile_definitions(${PROJECT_NAME}
    INTERFACE ${JSONIFIER_COMPILE_DEFINITIONS}
)

if(DEFINED JSONIFIER_MAX_AVX_TIER)
    target_compile_definitions(${PROJECT_NAME}
        INTERFACE JSONIFIER_MAX_AVX_TIER=${JSONIFIER_MAX_AVX_TIER}
    )
endif()
