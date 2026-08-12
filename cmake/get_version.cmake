# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/get_version.cmake

find_package(Git QUIET)

if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result
    )
endif()

if(git_result EQUAL 0 AND git_tag)
    string(REGEX REPLACE "^v" "" CLEAN_VERSION "${git_tag}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CLEAN_VERSION)
    string(STRIP "${CLEAN_VERSION}" CLEAN_VERSION)
else()
    message(WARNING "Could not determine version from git tag or VERSION file. Falling back to default.")
    set(CLEAN_VERSION "0.0.0")
endif()
