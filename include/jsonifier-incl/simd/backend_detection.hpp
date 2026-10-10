/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/backend_detection.hpp
 */
#pragma once

#include <jsonifier-incl/simd/backend_traits.hpp>

#if JSONIFIER_ARCH_X64 && !JSONIFIER_COMPILER_MSVC
	#include <cpuid.h>
#endif

namespace jsonifier::internal {

#if JSONIFIER_ARCH_X64

	struct cpuid_registers {
		uint32_t eax{};
		uint32_t ebx{};
		uint32_t ecx{};
		uint32_t edx{};
	};

	inline cpuid_registers readCpuid(uint32_t leaf, uint32_t subleaf) noexcept {
		cpuid_registers registers{};
	#if JSONIFIER_COMPILER_MSVC
		int32_t values[4]{};
		__cpuidex(values, static_cast<int32_t>(leaf), static_cast<int32_t>(subleaf));
		registers.eax = static_cast<uint32_t>(values[0]);
		registers.ebx = static_cast<uint32_t>(values[1]);
		registers.ecx = static_cast<uint32_t>(values[2]);
		registers.edx = static_cast<uint32_t>(values[3]);
	#else
		__cpuid_count(leaf, subleaf, registers.eax, registers.ebx, registers.ecx, registers.edx);
	#endif
		return registers;
	}

	inline uint64_t readXcr0() noexcept {
	#if JSONIFIER_COMPILER_MSVC
		return _xgetbv(0);
	#else
		uint32_t low{};
		uint32_t high{};
		__asm__ __volatile__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
		return (static_cast<uint64_t>(high) << 32) | low;
	#endif
	}

	struct x86_features {
		bool avx{};
		bool avx2{};
		bool avx512f{};
		bool avx512bw{};
		bool avx512vbmi2{};
		bool bmi1{};
		bool bmi2{};
		bool lzcnt{};
		bool popcnt{};
		bool pclmul{};
	};

	inline x86_features detectX86Features() noexcept {
		x86_features features{};
		const uint32_t maxLeaf = readCpuid(0, 0).eax;
		if (maxLeaf < 1) {
			return features;
		}
		const cpuid_registers leaf1 = readCpuid(1, 0);
		features.pclmul				= (leaf1.ecx >> 1) & 1u;
		features.popcnt				= (leaf1.ecx >> 23) & 1u;
		const bool osxsave			= (leaf1.ecx >> 27) & 1u;
		const uint64_t xcr0			= osxsave ? readXcr0() : 0;
		const bool osAvx			= (xcr0 & 0x6u) == 0x6u;
		const bool osAvx512			= (xcr0 & 0xE6u) == 0xE6u;
		features.avx				= osAvx && ((leaf1.ecx >> 28) & 1u);
		if (maxLeaf >= 7) {
			const cpuid_registers leaf7 = readCpuid(7, 0);
			features.bmi1				= (leaf7.ebx >> 3) & 1u;
			features.bmi2				= (leaf7.ebx >> 8) & 1u;
			features.avx2				= osAvx && ((leaf7.ebx >> 5) & 1u);
			features.avx512f			= osAvx512 && ((leaf7.ebx >> 16) & 1u);
			features.avx512bw			= osAvx512 && ((leaf7.ebx >> 30) & 1u);
			features.avx512vbmi2		= osAvx512 && ((leaf7.ecx >> 6) & 1u);
		}
		if (readCpuid(0x80000000u, 0).eax >= 0x80000001u) {
			features.lzcnt = (readCpuid(0x80000001u, 0).ecx >> 5) & 1u;
		}
		return features;
	}

	inline bool backendSupported(jsonifier_backend backend) noexcept {
		static const x86_features features{ detectX86Features() };
		const bool scalarExtensions = features.bmi1 && features.bmi2 && features.lzcnt && features.popcnt && features.pclmul;
		switch (static_cast<uint64_t>(backend)) {
			case static_cast<uint64_t>(jsonifier_backend::avx512): {
				return scalarExtensions && features.avx2 && features.avx512f && features.avx512bw && features.avx512vbmi2;
			}
			case static_cast<uint64_t>(jsonifier_backend::avx2): {
				return scalarExtensions && features.avx2;
			}
			case static_cast<uint64_t>(jsonifier_backend::avx): {
				return features.avx;
			}
			default: {
				return false;
			}
		}
	}

#else

	inline bool backendSupported(jsonifier_backend backend) noexcept {
		return backend == default_backend;
	}

#endif

	template<jsonifier_backend... backends> inline jsonifier_backend selectSupportedBackend() noexcept {
		static constexpr jsonifier_backend candidates[]{ backends... };
		for (const jsonifier_backend backend: candidates) {
			if (backendSupported(backend)) {
				return backend;
			}
		}
		return candidates[sizeof...(backends) - 1];
	}

}
