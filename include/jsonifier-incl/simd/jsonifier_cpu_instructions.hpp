// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/jsonifier_cpu_instructions.hpp
#pragma once

#undef JSONIFIER_CPU_INSTRUCTIONS
#define JSONIFIER_CPU_INSTRUCTIONS 111

#undef JSONIFIER_SVE2_VECTOR_BITS
#define JSONIFIER_SVE2_VECTOR_BITS 0

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
	#error "JSONIFIER_NEON and JSONIFIER_SVE2 are mutually exclusive backends - every SVE2 part also reports NEON, so exactly one must be selected."
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2) && JSONIFIER_SVE2_VECTOR_BITS == 0
	#error "JSONIFIER_SVE2 is selected but JSONIFIER_SVE2_VECTOR_BITS is 0 - the fixed-length SVE2 typedefs require a measured vector length."
#endif
