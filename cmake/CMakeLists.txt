# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/CMakeLists.txt

cmake_minimum_required(VERSION 3.10)
project(FeatureDetection)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(feature_detector main.cpp)
target_compile_options(feature_detector PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/arch:AVX512>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<NOT:$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},aarch64>>>:-march=native>
    $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},aarch64>>:-march=armv9-a+sve2>
)
