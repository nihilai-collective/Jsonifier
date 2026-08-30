// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/simd_types.hpp
#pragma once

#include <jsonifier-incl/core/config.hpp>
#include <jsonifier-incl/simd/simd_x.hpp>

namespace jsonifier {

	static constexpr uint64_t simdBytesPerRegister{ internal::cpu_properties::get_value(internal::cpu_property_types::alignment) };

}

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)

namespace jsonifier {

	using jsonifier_simd_int_128 = __m128i;
	using jsonifier_simd_int_256 = __m256i;
	using jsonifier_simd_int_512 = __m512i;

	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)

	static constexpr const char* cpu_arch_name{ "AVX512" };

	using jsonifier_simd_int_t = jsonifier_simd_int_512;
		#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdBlocksPerStep = 8;
		#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdBlocksPerStep = 4;
		#else
	static constexpr uint64_t simdBlocksPerStep = 4;
		#endif

	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)

	static constexpr const char* cpu_arch_name{ "AVX2" };

	using jsonifier_simd_int_t = jsonifier_simd_int_256;
		#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdTapeStep		= 4;
	static constexpr uint64_t simdBlocksPerStep = 4;
		#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdTapeStep		= 1;
	static constexpr uint64_t simdBlocksPerStep = 8;
		#else
	static constexpr uint64_t simdTapeStep		= 4;
	static constexpr uint64_t simdBlocksPerStep = 8;
		#endif

	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX)

	static constexpr const char* cpu_arch_name{ "AVX" };

	using jsonifier_simd_int_t = jsonifier_simd_int_128;
		#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdTapeStep		= 2;
	static constexpr uint64_t simdBlocksPerStep = 4;
		#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdTapeStep		= 2;
	static constexpr uint64_t simdBlocksPerStep = 8;
		#else
	static constexpr uint64_t simdTapeStep		= 1;
	static constexpr uint64_t simdBlocksPerStep = 4;
		#endif
	#endif

#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)

namespace jsonifier {

	static_assert(JSONIFIER_SVE2_VECTOR_BITS == 128, "Jsonifier's SVE2 path is only implemented for a 128-bit vector length.");

	static constexpr const char* cpu_arch_name{ "SVE2" };

	#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdTapeStep		= 4;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdTapeStep		= 8;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#else
	static constexpr uint64_t simdTapeStep		= 1;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#endif

	using jsonifier_simd_int_128 = svuint8_t __attribute__((arm_sve_vector_bits(JSONIFIER_SVE2_VECTOR_BITS)));
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;
	using jsonifier_simd_int_t	 = jsonifier_simd_int_128;

#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)

namespace jsonifier {

	static constexpr const char* cpu_arch_name{ "NEON" };

	#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdTapeStep		= 8;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdTapeStep		= 8;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#else
	static constexpr uint64_t simdTapeStep		= 8;
	static constexpr uint64_t simdBlocksPerStep = 4;
	#endif
	using jsonifier_simd_int_128 = uint8x16_t;
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;
	using jsonifier_simd_int_t	 = jsonifier_simd_int_128;

#else

namespace jsonifier {

	static constexpr const char* cpu_arch_name{ "FALLBACK" };

	using jsonifier_simd_int_128				= jsonifier::internal::simd::simd_x;
	using jsonifier_simd_int_256				= uint32_t;
	using jsonifier_simd_int_512				= uint64_t;
	using jsonifier_simd_int_t					= jsonifier_simd_int_128;
	static constexpr uint64_t simdTapeStep		= 4;
	static constexpr uint64_t simdBlocksPerStep = 8;

#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	template<typename value_type>
	concept simd_int_sve2_type = std::same_as<std::remove_cvref_t<value_type>, jsonifier_simd_int_t>;
#endif

	static constexpr uint64_t registersPerBlock{ 64 / simdBytesPerRegister };
	static constexpr uint64_t simdBytesPerBlock{ 64 };
	static constexpr uint64_t simdBytesPerStep = simdBlocksPerStep * simdBytesPerBlock;

	static_assert(simdBytesPerRegister == sizeof(jsonifier_simd_int_t),
		"simdBytesPerRegister disagrees with the actual register width; registersPerBlock and every bitmask collapse depend on these matching.");
	static_assert(simdBytesPerBlock % simdBytesPerRegister == 0, "Register width must evenly divide the 64-byte block.");

	template<uint64_t size> struct simd_array {
		using size_type = uint64_t;
		alignas(sizeof(jsonifier_simd_int_t)) jsonifier_simd_int_t values[size]{};

		template<uint64_t indexNew> JSONIFIER_INLINE void set(jsonifier_simd_int_t value) noexcept {
			static_assert(indexNew < size, "simd_array::set index out of range.");
			values[indexNew] = value;
		}

		template<uint64_t indexNew> JSONIFIER_INLINE jsonifier_simd_int_t get() const noexcept {
			static_assert(indexNew < size, "simd_array::get index out of range.");
			return values[indexNew];
		}
	};

	using simd_array_t = simd_array<registersPerBlock>;

	using string_view_ptr	   = const char*;
	using structural_index_ptr = uint32_t*;
	using string_buffer_ptr	   = char*;
}
