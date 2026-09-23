/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/simd_types.hpp
 */
#pragma once

#include <jsonifier-incl/core/config.hpp>
#include <jsonifier-incl/simd/simd_x.hpp>

namespace jsonifier {

	using read_buffer_ptr	   = const char*;
	using structural_index_ptr = uint32_t*;
	using write_buffer_ptr	   = char*;

	static constexpr uint64_t simdBytesPerRegister{ internal::cpu_properties::get_value(internal::cpu_property_types::alignment) };

}

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)

namespace jsonifier {

	using jsonifier_simd_int_128 = __m128i;
	using jsonifier_simd_int_256 = __m256i;
	using jsonifier_simd_int_512 = __m512i;

	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)

	static constexpr read_buffer_ptr cpu_arch_name{ "AVX512" };

	using jsonifier_simd_int_t = jsonifier_simd_int_512;
		#if JSONIFIER_COMPILER_CLANG
	static constexpr uint64_t simdBlocksPerStep = 8;
		#elif JSONIFIER_COMPILER_GCC
	static constexpr uint64_t simdBlocksPerStep = 4;
		#else
	static constexpr uint64_t simdBlocksPerStep = 4;
		#endif

	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)

	static constexpr read_buffer_ptr cpu_arch_name{ "AVX2" };

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

	static constexpr read_buffer_ptr cpu_arch_name{ "AVX" };

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

	static constexpr read_buffer_ptr cpu_arch_name{ "SVE2" };

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

	static constexpr read_buffer_ptr cpu_arch_name{ "NEON" };

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
	using jsonifier_simd_int_128 = uint8x16_t;
	using jsonifier_simd_int_256 = uint32_t;
	using jsonifier_simd_int_512 = uint64_t;
	using jsonifier_simd_int_t	 = jsonifier_simd_int_128;

#else

namespace jsonifier {

	static constexpr read_buffer_ptr cpu_arch_name{ "FALLBACK" };

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

	static constexpr uint64_t simdRegistersPerBlock{ 64 / simdBytesPerRegister };
	static constexpr uint64_t simdBytesPerBlock{ 64 };
	static constexpr uint64_t simdBytesPerStep = simdBlocksPerStep * simdBytesPerBlock;

	static_assert(simdBytesPerRegister == sizeof(jsonifier_simd_int_t),
		"simdBytesPerRegister disagrees with the actual register width; simdRegistersPerBlock and every bitmask collapse depend on these matching.");
	static_assert(simdBytesPerBlock % simdBytesPerRegister == 0, "Register width must evenly divide the 64-byte block.");

	template<uint64_t registerBytes> struct simd_register {
		static_assert(registerBytes == simdBytesPerRegister);
		using type = jsonifier_simd_int_t;
	};

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	template<> struct simd_register<16> {
		using type = jsonifier_simd_int_128;
	};
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
	template<> struct simd_register<32> {
		using type = jsonifier_simd_int_256;
	};
#endif

	template<uint64_t registerCount, uint64_t registerBytes = simdBytesPerRegister> struct simd_register_array {
		using simd_type = typename simd_register<registerBytes>::type;
		alignas(registerBytes) simd_type values[registerCount]{};

		template<uint64_t indexNew> JSONIFIER_INLINE void set(simd_type value) noexcept {
			static_assert(indexNew < registerCount, "simd_register_array::set index out of range.");
			values[indexNew] = value;
		}

		template<uint64_t indexNew> JSONIFIER_INLINE simd_type get() const noexcept {
			static_assert(indexNew < registerCount, "simd_register_array::get index out of range.");
			return values[indexNew];
		}
	};

	template<uint64_t registerCount> using simd_array = simd_register_array<registerCount>;

	using simd_array_t = simd_array<simdRegistersPerBlock>;

	template<uint64_t registerCount, uint64_t registerBytes> using scalar_simd_array_t = simd_register_array<registerCount, registerBytes>;
}
