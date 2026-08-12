# MIT License @ /License.md
# Copyright (c) 2026 Nihilai Collective Corp
# https://github.com/nihilai-collective/jsonifier
# cmake/jsonifier_detect_architecture.cmake

if(NOT DEFINED JSONIFIER_CPU_PROPERTIES_CONSTRUCTED)

    set(JSONIFIER_FEATURE_DETECTOR_DIR "${CMAKE_CURRENT_BINARY_DIR}/jsonifier_feature_detector")
    set(JSONIFIER_SVE2_VL_BITS 0)

    if(DEFINED JSONIFIER_CPU_INSTRUCTIONS)
        string(REPLACE "|" ";" JSONIFIER_CPU_INSTRUCTIONS_PARTS "${JSONIFIER_CPU_INSTRUCTIONS}")
        math(EXPR JSONIFIER_CPU_INSTRUCTIONS_NUMERIC 0)
        foreach(PART ${JSONIFIER_CPU_INSTRUCTIONS_PARTS})
            string(STRIP "${PART}" PART)
            math(EXPR JSONIFIER_CPU_INSTRUCTIONS_NUMERIC "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} | ${PART} )")
        endforeach()
        if(DEFINED JSONIFIER_SVE2_VECTOR_BITS)
            set(JSONIFIER_SVE2_VL_BITS "${JSONIFIER_SVE2_VECTOR_BITS}")
        endif()
    elseif(CMAKE_CROSSCOMPILING)
        message(WARNING "Cross-compiling without JSONIFIER_CPU_INSTRUCTIONS defined; falling back to scalar.")
        math(EXPR JSONIFIER_CPU_INSTRUCTIONS_NUMERIC 0)
        if(DEFINED JSONIFIER_SVE2_VECTOR_BITS)
            set(JSONIFIER_SVE2_VL_BITS "${JSONIFIER_SVE2_VECTOR_BITS}")
        endif()
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake" -B "${JSONIFIER_FEATURE_DETECTOR_DIR}"
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            RESULT_VARIABLE JSONIFIER_DETECTOR_CONFIGURE_RESULT
            OUTPUT_QUIET
        )
        if(NOT JSONIFIER_DETECTOR_CONFIGURE_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure the Jsonifier feature detector.")
        endif()

        execute_process(
            COMMAND "${CMAKE_COMMAND}" --build "${JSONIFIER_FEATURE_DETECTOR_DIR}" --config Release
            RESULT_VARIABLE JSONIFIER_DETECTOR_BUILD_RESULT
            OUTPUT_QUIET
        )
        if(NOT JSONIFIER_DETECTOR_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build the Jsonifier feature detector.")
        endif()

        find_program(JSONIFIER_FEATURE_DETECTOR_EXE
            NAMES feature_detector
            PATHS "${JSONIFIER_FEATURE_DETECTOR_DIR}" "${JSONIFIER_FEATURE_DETECTOR_DIR}/Release"
            NO_DEFAULT_PATH
        )
        if(NOT JSONIFIER_FEATURE_DETECTOR_EXE)
            message(FATAL_ERROR "Could not locate the built Jsonifier feature detector.")
        endif()

        execute_process(
            COMMAND "${JSONIFIER_FEATURE_DETECTOR_EXE}"
            OUTPUT_VARIABLE JSONIFIER_DETECTOR_OUTPUT
            RESULT_VARIABLE JSONIFIER_DETECTOR_RUN_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT JSONIFIER_DETECTOR_RUN_RESULT EQUAL 0)
            message(FATAL_ERROR "The Jsonifier feature detector failed to run.")
        endif()

        if(NOT JSONIFIER_DETECTOR_OUTPUT MATCHES "CPU_SUCCESS=1")
            message(FATAL_ERROR "The Jsonifier feature detector produced incomplete output.")
        endif()

        if(NOT JSONIFIER_DETECTOR_OUTPUT MATCHES "CPU_MASK=([0-9]+)")
            message(FATAL_ERROR "The Jsonifier feature detector did not report a CPU mask.")
        endif()
        set(JSONIFIER_CPU_INSTRUCTIONS_NUMERIC "${CMAKE_MATCH_1}")

        if(JSONIFIER_DETECTOR_OUTPUT MATCHES "SVE2_VL_BITS=([0-9]+)")
            set(JSONIFIER_SVE2_VL_BITS "${CMAKE_MATCH_1}")
        endif()

        string(REPLACE "\n" ";" JSONIFIER_DETECTOR_LINES "${JSONIFIER_DETECTOR_OUTPUT}")
        foreach(LINE ${JSONIFIER_DETECTOR_LINES})
            string(STRIP "${LINE}" LINE)
            if(LINE MATCHES "^HAS_([A-Z0-9_]+)=1$")
                message(STATUS "Detected: ${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()

    set(SIMD_FLAGS "")
    math(EXPR JSONIFIER_CPU_INSTRUCTIONS 0)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
        set(JSONIFIER_TARGET_IS_AARCH64 TRUE)
    else()
        set(JSONIFIER_TARGET_IS_AARCH64 FALSE)
    endif()

    math(EXPR JSONIFIER_HAS_CLMUL_BIT "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & 0x8 )")

    function(jsonifier_add_instruction INSTRUCTION_SET_NAME INSTRUCTION_SET_FLAG DETECT_BIT OUTPUT_BIT)
        math(EXPR INSTRUCTION_PRESENT "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & ${DETECT_BIT} )")
        if(INSTRUCTION_PRESENT)
            math(EXPR JSONIFIER_CPU_INSTRUCTIONS "( ${JSONIFIER_CPU_INSTRUCTIONS} | ${OUTPUT_BIT} )" OUTPUT_FORMAT DECIMAL)
            set(JSONIFIER_CPU_INSTRUCTIONS "${JSONIFIER_CPU_INSTRUCTIONS}" PARENT_SCOPE)
            if(NOT "${INSTRUCTION_SET_FLAG}" STREQUAL "")
                if("${SIMD_FLAGS}" STREQUAL "")
                    set(SIMD_FLAGS "${INSTRUCTION_SET_FLAG}" PARENT_SCOPE)
                else()
                    set(SIMD_FLAGS "${SIMD_FLAGS};${INSTRUCTION_SET_FLAG}" PARENT_SCOPE)
                endif()
            endif()
        endif()
    endfunction()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        set(JSONIFIER_FLAG_LZCNT "")
        set(JSONIFIER_FLAG_POPCNT "")
        set(JSONIFIER_FLAG_BMI "")
        set(JSONIFIER_FLAG_CLMUL "")
        set(JSONIFIER_FLAG_AVX "/arch:AVX")
        set(JSONIFIER_FLAG_AVX2 "/arch:AVX2")
        set(JSONIFIER_FLAG_AVX512 "/arch:AVX512")
        set(JSONIFIER_FLAG_SVE2 "")
    elseif(JSONIFIER_TARGET_IS_AARCH64)
        set(JSONIFIER_FLAG_LZCNT "")
        set(JSONIFIER_FLAG_POPCNT "")
        set(JSONIFIER_FLAG_BMI "")
        set(JSONIFIER_FLAG_CLMUL "")
        set(JSONIFIER_FLAG_AVX "")
        set(JSONIFIER_FLAG_AVX2 "")
        set(JSONIFIER_FLAG_AVX512 "")
        if(JSONIFIER_HAS_CLMUL_BIT)
            set(JSONIFIER_SVE2_ARCH "armv9-a+sve2+aes")
        else()
            set(JSONIFIER_SVE2_ARCH "armv9-a+sve2")
        endif()
        if(JSONIFIER_SVE2_VL_BITS GREATER 0)
            set(JSONIFIER_FLAG_SVE2 "-march=${JSONIFIER_SVE2_ARCH};-msve-vector-bits=${JSONIFIER_SVE2_VL_BITS}")
        else()
            set(JSONIFIER_FLAG_SVE2 "-march=${JSONIFIER_SVE2_ARCH}")
        endif()
    else()
        set(JSONIFIER_FLAG_LZCNT "-mlzcnt")
        set(JSONIFIER_FLAG_POPCNT "-mpopcnt")
        set(JSONIFIER_FLAG_BMI "-mbmi")
        set(JSONIFIER_FLAG_CLMUL "-mpclmul")
        set(JSONIFIER_FLAG_AVX "-mavx")
        set(JSONIFIER_FLAG_AVX2 "-mavx2")
        set(JSONIFIER_FLAG_AVX512 "-mavx512vbmi2;-mavx512bw;-mavx512f")
        set(JSONIFIER_FLAG_SVE2 "")
    endif()

    jsonifier_add_instruction("LzCnt" "${JSONIFIER_FLAG_LZCNT}" 0x1 1)
    jsonifier_add_instruction("PopCnt" "${JSONIFIER_FLAG_POPCNT}" 0x2 2)
    jsonifier_add_instruction("Bmi" "${JSONIFIER_FLAG_BMI}" 0x4 4)
    jsonifier_add_instruction("ClMul" "${JSONIFIER_FLAG_CLMUL}" 0x8 8)

    math(EXPR INSTRUCTION_PRESENT128 "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & 0x20 )")
    math(EXPR INSTRUCTION_PRESENT256 "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & 0x40 )")
    math(EXPR INSTRUCTION_PRESENT512 "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & 0x80 )")
    math(EXPR INSTRUCTION_PRESENT_SVE2 "( ${JSONIFIER_CPU_INSTRUCTIONS_NUMERIC} & 0x100 )")

    if(INSTRUCTION_PRESENT_SVE2 AND JSONIFIER_SVE2_VL_BITS GREATER 0)
        jsonifier_add_instruction("Sve2" "${JSONIFIER_FLAG_SVE2}" 0x100 256)
    else()
        jsonifier_add_instruction("Neon" "" 0x10 16)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        if(INSTRUCTION_PRESENT512)
            jsonifier_add_instruction("Avx512" "${JSONIFIER_FLAG_AVX512}" 0x80 128)
            jsonifier_add_instruction("Avx2" "" 0x40 64)
            jsonifier_add_instruction("Avx" "" 0x20 32)
        elseif(INSTRUCTION_PRESENT256)
            jsonifier_add_instruction("Avx2" "${JSONIFIER_FLAG_AVX2}" 0x40 64)
            jsonifier_add_instruction("Avx" "" 0x20 32)
        elseif(INSTRUCTION_PRESENT128)
            jsonifier_add_instruction("Avx" "${JSONIFIER_FLAG_AVX}" 0x20 32)
        endif()
    else()
        if(INSTRUCTION_PRESENT512)
            jsonifier_add_instruction("Avx512" "${JSONIFIER_FLAG_AVX512}" 0x80 128)
        endif()
        if(INSTRUCTION_PRESENT256)
            jsonifier_add_instruction("Avx2" "${JSONIFIER_FLAG_AVX2}" 0x40 64)
        endif()
        if(INSTRUCTION_PRESENT128)
            jsonifier_add_instruction("Avx" "${JSONIFIER_FLAG_AVX}" 0x20 32)
        endif()
    endif()

    set(SIMD_FLAGS "${SIMD_FLAGS}" CACHE STRING "SIMD flags" FORCE)
    set(JSONIFIER_CPU_INSTRUCTIONS "${JSONIFIER_CPU_INSTRUCTIONS}" CACHE STRING "CPU Instruction Sets" FORCE)
    set(JSONIFIER_CPU_PROPERTIES_CONSTRUCTED TRUE CACHE BOOL "CPU Instruction Sets" FORCE)

    if(INSTRUCTION_PRESENT_SVE2 AND JSONIFIER_SVE2_VL_BITS GREATER 0)
        math(EXPR JSONIFIER_SVE2_ALIGNMENT "${JSONIFIER_SVE2_VL_BITS} / 8")
        if(JSONIFIER_SVE2_ALIGNMENT LESS 16)
            set(JSONIFIER_SVE2_ALIGNMENT 16)
        endif()
        set(JSONIFIER_CPU_ALIGNMENT "${JSONIFIER_SVE2_ALIGNMENT}" CACHE STRING "CPU Alignment" FORCE)
        set(JSONIFIER_SVE2_VECTOR_BITS "${JSONIFIER_SVE2_VL_BITS}" CACHE STRING "SVE2 Vector Length (bits)" FORCE)
    elseif(INSTRUCTION_PRESENT512)
        set(JSONIFIER_CPU_ALIGNMENT 64 CACHE STRING "CPU Alignment" FORCE)
        set(JSONIFIER_SVE2_VECTOR_BITS 0 CACHE STRING "SVE2 Vector Length (bits)" FORCE)
    elseif(INSTRUCTION_PRESENT256)
        set(JSONIFIER_CPU_ALIGNMENT 32 CACHE STRING "CPU Alignment" FORCE)
        set(JSONIFIER_SVE2_VECTOR_BITS 0 CACHE STRING "SVE2 Vector Length (bits)" FORCE)
    else()
        set(JSONIFIER_CPU_ALIGNMENT 16 CACHE STRING "CPU Alignment" FORCE)
        set(JSONIFIER_SVE2_VECTOR_BITS 0 CACHE STRING "SVE2 Vector Length (bits)" FORCE)
    endif()

else()

    if(NOT DEFINED JSONIFIER_CPU_INSTRUCTIONS)
        set(JSONIFIER_CPU_INSTRUCTIONS 0 CACHE STRING "CPU Instruction Sets" FORCE)
    endif()

    if(NOT DEFINED JSONIFIER_CPU_ALIGNMENT)
        set(JSONIFIER_CPU_ALIGNMENT 16 CACHE STRING "CPU Alignment" FORCE)
    endif()

    if(NOT DEFINED JSONIFIER_SVE2_VECTOR_BITS)
        set(JSONIFIER_SVE2_VECTOR_BITS 0 CACHE STRING "SVE2 Vector Length (bits)" FORCE)
    endif()

endif()

file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp" "// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
#pragma once

#undef JSONIFIER_CPU_INSTRUCTIONS
#define JSONIFIER_CPU_INSTRUCTIONS ${JSONIFIER_CPU_INSTRUCTIONS}

#undef JSONIFIER_SVE2_VECTOR_BITS
#define JSONIFIER_SVE2_VECTOR_BITS ${JSONIFIER_SVE2_VECTOR_BITS}

#if !defined(JSONIFIER_CHECK_FOR_INSTRUCTION)
	#define JSONIFIER_CHECK_FOR_INSTRUCTION(x) (JSONIFIER_CPU_INSTRUCTIONS & x)
#endif

#if !defined(JSONIFIER_LZCNT)
	#define JSONIFIER_LZCNT (1 << 0)
#endif
#if !defined(JSONIFIER_POPCNT)
	#define JSONIFIER_POPCNT (1 << 1)
#endif
#if !defined(JSONIFIER_BMI)
	#define JSONIFIER_BMI (1 << 2)
#endif
#if !defined(JSONIFIER_CLMUL)
	#define JSONIFIER_CLMUL (1 << 3)
#endif
#if !defined(JSONIFIER_NEON)
	#define JSONIFIER_NEON (1 << 4)
#endif
#if !defined(JSONIFIER_AVX)
	#define JSONIFIER_AVX (1 << 5)
#endif
#if !defined(JSONIFIER_AVX2)
	#define JSONIFIER_AVX2 (1 << 6)
#endif
#if !defined(JSONIFIER_AVX512)
	#define JSONIFIER_AVX512 (1 << 7)
#endif
#if !defined(JSONIFIER_SVE2)
	#define JSONIFIER_SVE2 (1 << 8)
#endif

#if !defined(JSONIFIER_ANY_AVX)
	#define JSONIFIER_ANY_AVX (JSONIFIER_AVX | JSONIFIER_AVX2 | JSONIFIER_AVX512)
#endif

#if !defined(JSONIFIER_ANY_SIMD)
	#define JSONIFIER_ANY_SIMD (JSONIFIER_AVX | JSONIFIER_AVX2 | JSONIFIER_AVX512 | JSONIFIER_NEON | JSONIFIER_SVE2)
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON) && JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	#error \"JSONIFIER_NEON and JSONIFIER_SVE2 are mutually exclusive backends - every SVE2 part also reports NEON, so exactly one must be selected.\"
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2) && JSONIFIER_SVE2_VECTOR_BITS == 0
	#error \"JSONIFIER_SVE2 is selected but JSONIFIER_SVE2_VECTOR_BITS is 0 - the fixed-length SVE2 typedefs require a measured vector length.\"
#endif
")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jsonifier_cpu_properties.hpp.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/jsonifier-incl/simd/jsonifier_cpu_properties.hpp"
    @ONLY
)
