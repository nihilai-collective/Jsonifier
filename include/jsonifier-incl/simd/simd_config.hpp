/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/simd_config.hpp
 */
#if !defined(JSONIFIER_PASS_GUARD_SIMD_CONFIG)
	#define JSONIFIER_PASS_GUARD_SIMD_CONFIG

	#include <jsonifier-incl/simd/backend_traits.hpp>

namespace JSONIFIER_NAMESPACE {

	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::avx512 };
	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::avx2 };
	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX)
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::avx };
	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::sve2 };
	#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::neon };
	#else
	static constexpr jsonifier_backend default_backend{ jsonifier_backend::fallback };
	#endif

	using default_backend_traits = backend_traits<default_backend>;

	static constexpr const char* cpu_arch_name{ default_backend_traits::name };

	using jsonifier_simd_int_t = typename default_backend_traits::simd_int_t;

	static constexpr uint64_t simdTapeStep{ default_backend_traits::tapeStep };
	static constexpr uint64_t simdBlocksPerStep{ default_backend_traits::blocksPerStep };
	static constexpr uint64_t simdBytesPerRegister{ backendBytesPerRegister<default_backend> };

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

	template<> struct simd_register<32> {
		using type = jsonifier_simd_int_256;
	};

	template<> struct simd_register<64> {
		using type = jsonifier_simd_int_512;
	};
	#endif

	template<uint64_t registerCount, uint64_t registerBytes = simdBytesPerRegister> struct simd_register_array {
		using simd_type = typename simd_register<registerBytes>::type;
		alignas(registerBytes) simd_type values[registerCount];

		template<uint64_t indexNew> JSONIFIER_INLINE void set(const simd_type value) noexcept {
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

	template<uint64_t registerCount, uint64_t registerBytes> using pod_simd_array_t = simd_register_array<registerCount, registerBytes>;
}

#endif
