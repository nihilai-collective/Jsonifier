# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# unit-tests/CMakeLists.txt

add_executable(jsonifier-unit-tests main.cpp)

include(FetchContent)

FetchContent_Declare(
  rt-ut
  GIT_REPOSITORY https://github.com/realtimechris/rt-ut.git
  GIT_TAG main
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rt-ut)

target_link_libraries(jsonifier-unit-tests
    PUBLIC
        Jsonifier::Jsonifier rt-ut::rt-ut 
)

option(JSONIFIER_ASAN  "Enable AddressSanitizer"            OFF)
option(JSONIFIER_UBSAN "Enable UndefinedBehaviorSanitizer"  OFF)

set(JSONIFIER_TEST_BASE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/" CACHE STRING "Root path the unit tests resolve their JSON data against")
set(JSONIFIER_ASAN_EFFECTIVE  ${JSONIFIER_ASAN})
set(JSONIFIER_UBSAN_EFFECTIVE ${JSONIFIER_UBSAN})

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(JSONIFIER_ASAN_EFFECTIVE OFF)
    set(JSONIFIER_UBSAN_EFFECTIVE OFF)
    message(STATUS "Disabling sanitizers: not supported with GCC on macOS.")
endif()

if(JSONIFIER_UBSAN_EFFECTIVE AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(WARNING
        "JSONIFIER_UBSAN has no effect with MSVC (no equivalent runtime). "
        "Use Clang-cl or a Clang/GCC build to get UBSan coverage.")
    set(JSONIFIER_UBSAN_EFFECTIVE OFF)
endif()

set(JSONIFIER_HOMEBREW_GCC_LIBDIR "")
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    find_program(JSONIFIER_BREW_EXE brew)
    if(JSONIFIER_BREW_EXE)
        execute_process(
            COMMAND ${JSONIFIER_BREW_EXE} --prefix gcc
            OUTPUT_VARIABLE JSONIFIER_BREW_GCC_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE JSONIFIER_BREW_RC
        )
        if(JSONIFIER_BREW_RC EQUAL 0 AND JSONIFIER_BREW_GCC_PREFIX)
            file(GLOB JSONIFIER_GCC_LIB_CANDIDATES
                "${JSONIFIER_BREW_GCC_PREFIX}/lib/gcc/*")
            if(JSONIFIER_GCC_LIB_CANDIDATES)
                list(SORT JSONIFIER_GCC_LIB_CANDIDATES)
                list(REVERSE JSONIFIER_GCC_LIB_CANDIDATES)
                list(GET JSONIFIER_GCC_LIB_CANDIDATES 0 JSONIFIER_HOMEBREW_GCC_LIBDIR)
            endif()
        endif()
    endif()
    if(NOT JSONIFIER_HOMEBREW_GCC_LIBDIR)
        message(STATUS
            "Could not auto-detect Homebrew GCC runtime path; link-time "
            "rpath for libstdc++ will not be added. Set "
            "JSONIFIER_HOMEBREW_GCC_LIBDIR manually if needed.")
    endif()
endif()

set(JSONIFIER_MSVC_COMPILER_OPTIONS
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>>>:/Od;/Zi;/RTC1>
    $<$<AND:$<CONFIG:Debug>,$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>>:/Od;/Zi>
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:/fsanitize=address>
    /wd4324
    /wd4820
    /wd5039
    /wd4710
    /wd5045
    /wd4711
    /wd4623
    /wd4626
    /wd4686
    /wd4865
    /wd5027
    /Wall
    /W4
    /bigobj
    /WX
)

set(JSONIFIER_CLANG_COMPILER_OPTIONS
    $<$<AND:$<CONFIG:Release>,$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>>:-O1>
    $<$<CONFIG:Debug>:-O0;-g;-fno-omit-frame-pointer>
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:-fsanitize=address;-fno-omit-frame-pointer;-fno-optimize-sibling-calls;-fsanitize-address-use-after-scope;-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=0>
    $<$<BOOL:${JSONIFIER_UBSAN_EFFECTIVE}>:-fsanitize=undefined;-fno-sanitize-recover=all>
    $<$<AND:$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>,$<NOT:$<VERSION_LESS:$<CXX_COMPILER_VERSION>,23>>>:-Wno-lifetime-safety-lifetimebound-violation;-Wno-lifetime-safety-intra-tu-suggestions;-Wno-lifetime-safety-invalidation>
    -Weverything
    -Wpedantic
    -Werror   
    -Wno-unsafe-buffer-usage    
    -Wno-c++20-extensions
    -Wno-c++20-compat
    -Wextra
    -Wall
    -Wno-c++98-compat-pedantic
    -Wno-c++98-compat
    -Wno-padded
)

set(JSONIFIER_GNU_COMPILER_OPTIONS
    $<$<CONFIG:Debug>:-O0;-g>
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:-fsanitize=address;-fsanitize-address-use-after-scope;-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=0>
    $<$<BOOL:${JSONIFIER_UBSAN_EFFECTIVE}>:-fsanitize=undefined;-fno-sanitize-recover=all>
    -Wpedantic
    -Wno-ignored-attributes
    -Werror
    -Wextra
    -Wall
)

set(JSONIFIER_COMPILER_OPTIONS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:${JSONIFIER_CLANG_COMPILER_OPTIONS}>
    $<$<CXX_COMPILER_ID:GNU>:${JSONIFIER_GNU_COMPILER_OPTIONS}>
    $<$<CXX_COMPILER_ID:MSVC>:${JSONIFIER_MSVC_COMPILER_OPTIONS}>
)

target_compile_options(
    jsonifier-unit-tests PUBLIC
    ${JSONIFIER_COMPILER_OPTIONS}
)

set(JSONIFIER_MSVC_LINK_OPTIONS
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:/DEBUG>
    $<$<BOOL:${JSONIFIER_UBSAN_EFFECTIVE}>:/DEBUG>
)

set(JSONIFIER_CLANG_LINK_OPTIONS
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:-fsanitize=address>
    $<$<BOOL:${JSONIFIER_UBSAN_EFFECTIVE}>:-fsanitize=undefined>
)

set(JSONIFIER_GNU_LINK_OPTIONS
    $<$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>:-fsanitize=address>
    $<$<BOOL:${JSONIFIER_UBSAN_EFFECTIVE}>:-fsanitize=undefined>
    $<$<AND:$<PLATFORM_ID:Linux>,$<BOOL:${JSONIFIER_ASAN_EFFECTIVE}>>:-static-libasan>
)

set(JSONIFIER_LINK_OPTIONS
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:${JSONIFIER_CLANG_LINK_OPTIONS}>
    $<$<CXX_COMPILER_ID:GNU>:${JSONIFIER_GNU_LINK_OPTIONS}>
    $<$<CXX_COMPILER_ID:MSVC>:${JSONIFIER_MSVC_LINK_OPTIONS}>
)

target_link_options(jsonifier-unit-tests
    PUBLIC ${JSONIFIER_LINK_OPTIONS}
)

target_compile_definitions(jsonifier-unit-tests PUBLIC 
"BASE_PATH=\"${JSONIFIER_TEST_BASE_PATH}\"")

install(
    FILES
        $<TARGET_FILE:jsonifier-unit-tests>
    DESTINATION bin
    OPTIONAL
)
