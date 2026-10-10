/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/backend_traits.hpp
 */
#pragma once

#include <jsonifier-incl/core/config.hpp>
#include <jsonifier-incl/simd/simd_x.hpp>

namespace jsonifier {

	using restricted_read_buffer_ptr  = const uint8_t* __restrict;
	using restricted_write_buffer_ptr = uint8_t* __restrict;
	using read_structural_index_ptr	  = const uint32_t*;
	using read_buffer_ptr			  = const uint8_t*;
	using write_structural_index_ptr  = uint32_t*;
	using write_buffer_ptr			  = uint8_t*;

	static constexpr uint64_t maxSimdBytesPerRegister{ internal::cpu_properties::get_value(internal::cpu_property_types::alignment) };

	enum class jsonifier_backend : uint8_t {
		fallback,
		avx,
		avx2,
		avx512,
		neon,
		sve2,
	};

	template<jsonifier_backend backend> struct backend_traits;

	template<uint64_t tapeStepNew, uint64_t blocksPerStepNew> struct backend_step_tuning {
		static constexpr uint64_t tapeStep{ tapeStepNew };
		static constexpr uint64_t blocksPerStep{ blocksPerStepNew };
	};

#if JSONIFIER_COMPILER_CLANG
	template<uint64_t clangTapeStep, uint64_t clangBlocksPerStep, uint64_t gccTapeStep, uint64_t gccBlocksPerStep, uint64_t msvcTapeStep, uint64_t msvcBlocksPerStep>
	using compiler_step_tuning = backend_step_tuning<clangTapeStep, clangBlocksPerStep>;
#elif JSONIFIER_COMPILER_GCC
	template<uint64_t clangTapeStep, uint64_t clangBlocksPerStep, uint64_t gccTapeStep, uint64_t gccBlocksPerStep, uint64_t msvcTapeStep, uint64_t msvcBlocksPerStep>
	using compiler_step_tuning = backend_step_tuning<gccTapeStep, gccBlocksPerStep>;
#else
	template<uint64_t clangTapeStep, uint64_t clangBlocksPerStep, uint64_t gccTapeStep, uint64_t gccBlocksPerStep, uint64_t msvcTapeStep, uint64_t msvcBlocksPerStep>
	using compiler_step_tuning = backend_step_tuning<msvcTapeStep, msvcBlocksPerStep>;
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)

	using jsonifier_simd_int_128 = __m128i;
	using jsonifier_simd_int_256 = __m256i;
	using jsonifier_simd_int_512 = __m512i;

	template<> struct backend_traits<jsonifier_backend::avx512> : compiler_step_tuning<4, 8, 4, 4, 4, 4> {
		static constexpr const char* name{ "AVX512" };
		using simd_int_t = jsonifier_simd_int_512;
	};

	template<> struct backend_traits<jsonifier_backend::avx2> : compiler_step_tuning<4, 4, 4, 8, 4, 8> {
		static constexpr const char* name{ "AVX2" };
		using simd_int_t = jsonifier_simd_int_256;
	};

	template<> struct backend_traits<jsonifier_backend::avx> : compiler_step_tuning<2, 4, 2, 8, 1, 4> {
		static constexpr const char* name{ "AVX" };
		using simd_int_t = jsonifier_simd_int_128;
	};

#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

	static_assert(JSONIFIER_SVE2_VECTOR_BITS == 128, "Jsonifier's SVE2 path is only implemented for a 128-bit vector length.");

	using jsonifier_simd_int_128 = svuint8_t __attribute__((arm_sve_vector_bits(JSONIFIER_SVE2_VECTOR_BITS)));
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;

	template<> struct backend_traits<jsonifier_backend::sve2> : compiler_step_tuning<4, 4, 8, 4, 1, 4> {
		static constexpr const char* name{ "SVE2" };
		using simd_int_t = jsonifier_simd_int_128;
	};

#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)

	using jsonifier_simd_int_128 = uint8x16_t;
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;

	template<> struct backend_traits<jsonifier_backend::neon> : compiler_step_tuning<8, 4, 8, 4, 1, 4> {
		static constexpr const char* name{ "NEON" };
		using simd_int_t = jsonifier_simd_int_128;
	};

#else

	using jsonifier_simd_int_128 = jsonifier::internal::simd::simd_x;
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;

	template<> struct backend_traits<jsonifier_backend::fallback> : backend_step_tuning<4, 8> {
		static constexpr const char* name{ "FALLBACK" };
		using simd_int_t = jsonifier_simd_int_128;
	};

#endif

	template<jsonifier_backend backend> static constexpr uint64_t backendBytesPerRegister{ sizeof(typename backend_traits<backend>::simd_int_t) };
	template<jsonifier_backend backend> static constexpr uint64_t backendRegistersPerBlock{ 64 / backendBytesPerRegister<backend> };
	template<jsonifier_backend backend> static constexpr uint64_t backendBytesPerStep{ backend_traits<backend>::blocksPerStep * 64 };

}
