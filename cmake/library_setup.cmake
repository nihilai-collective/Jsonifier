# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/library_setup.cmake

add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

set(JSONIFIER_COMPILE_DEFINITIONS
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
    "JSONIFIER_DISPATCH_TABLE_COUNT=$<IF:$<PLATFORM_ID:Darwin>,$<IF:$<CXX_COMPILER_ID:GNU>,4,0>,2>"
    "JSONIFIER_INLINE=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>,$<IF:$<CXX_COMPILER_ID:MSVC>,[[msvc::forceinline]] inline,inline __attribute__((always_inline))>,inline>"
    "JSONIFIER_LIFETIME_BOUND=$<IF:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,[[clang::lifetimebound]],$<IF:$<CXX_COMPILER_ID:MSVC>,[[msvc::lifetimebound]],>>"
    $<$<PLATFORM_ID:Windows>:NOMINMAX;WIN32_LEAN_AND_MEAN>
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/jsonifier_detect_architecture.cmake)

target_include_directories(${PROJECT_NAME}
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(${PROJECT_NAME}
    INTERFACE
        ${SIMD_FLAGS}
        $<$<CXX_COMPILER_ID:MSVC>:/constexpr:steps100000000>
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fconstexpr-steps=100000000>
        $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=1000000000>
)

target_compile_definitions(${PROJECT_NAME}
    INTERFACE ${JSONIFIER_COMPILE_DEFINITIONS}
)
