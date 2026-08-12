# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# CMakeLists.txt

cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/get_version.cmake)

project(Jsonifier
    VERSION ${CLEAN_VERSION}
    DESCRIPTION "A few classes for parsing and serializing json - very rapidly."
    LANGUAGES CXX
)

include(cmake/library_setup.cmake)

include(cmake/installation_setup.cmake)

if(JSONIFIER_UNIT_TESTS)
    add_subdirectory(unit-tests)
endif()
