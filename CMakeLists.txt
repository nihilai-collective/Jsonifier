# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# CMakeLists.txt

cmake_minimum_required(VERSION 3.21)

project(jsonifier_hash_map
    DESCRIPTION "A standalone compile-time hash map, extracted from Jsonifier."
    LANGUAGES CXX
)

add_library(jsonifier_hash_map INTERFACE)
add_library(jsonifier_hash_map::jsonifier_hash_map ALIAS jsonifier_hash_map)

target_include_directories(jsonifier_hash_map INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(jsonifier_hash_map INTERFACE cxx_std_20)

option(JSONIFIER_HASH_MAP_TESTS "Build the hash map tests" ${PROJECT_IS_TOP_LEVEL})

if(JSONIFIER_HASH_MAP_TESTS)
    add_executable(hash_map_tests unit-tests/hash_map_tests.cpp)
    target_link_libraries(hash_map_tests PRIVATE jsonifier_hash_map)
    target_compile_options(hash_map_tests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/constexpr:steps2147483647>
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-fconstexpr-steps=2147483647>
        $<$<CXX_COMPILER_ID:GNU>:-fconstexpr-ops-limit=2147483647>
    )
endif()
