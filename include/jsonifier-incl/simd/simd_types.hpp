/*
	MIT License

	Copyright (c) 2023 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier

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
	static constexpr uint64_t simdBlocksPerStep = 8;
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

	static constexpr const char* cpu_arch_name{ "FALLBACK" };

	using jsonifier_simd_int_128				= jsonifier::simd::simd_x;
	using jsonifier_simd_int_256				= uint32_t;
	using jsonifier_simd_int_512				= uint64_t;
	using jsonifier_simd_int_t					= jsonifier_simd_int_128;
	static constexpr uint64_t simdTapeStep		= 1;
	static constexpr uint64_t simdBlocksPerStep = 4;

#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	template<typename value_type>
	concept simd_int_sve2_type = std::same_as<std::remove_cvref_t<value_type>, jsonifier_simd_int_t>;
#endif

	static constexpr uint64_t registersPerBlock{ 64 / simdBytesPerRegister };
	static constexpr uint64_t simdBytesPerBlock{ 64 };
	static constexpr uint64_t simdBytesPerStep = simdBlocksPerStep * simdBytesPerBlock;

	template<typename value_type>
	concept simd_int_512_type = sizeof(value_type) == 64;
	template<typename value_type>
	concept simd_int_256_type = sizeof(value_type) == 32;
	template<typename value_type>
	concept simd_int_128_type = sizeof(value_type) == 16;

	template<uint64_t size> struct simd_array {
		using size_type = uint64_t;
		jsonifier_simd_int_t values[size]{};

		template<uint64_t indexNew> JSONIFIER_INLINE constexpr void set(jsonifier_simd_int_t value) noexcept {
			constexpr uint64_t index{ indexNew % size };
			values[index] = value;
		}

		template<uint64_t indexNew> JSONIFIER_INLINE constexpr jsonifier_simd_int_t get() const noexcept {
			constexpr uint64_t index{ indexNew % size };
			return values[index];
		}
	};

	using simd_array_t = simd_array<registersPerBlock>;

	using string_view_ptr	   = const char*;
	using structural_index_ptr = uint32_t*;
	using string_buffer_ptr	   = char*;
}
