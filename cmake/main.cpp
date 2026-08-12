// The following code was based heavily on this code: https://github.com/simdjson/simdjson/blob/master/src/internal/isadetection.h
// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// cmake/main.cpp

#include <iostream>
#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
	#if defined(__linux__)
		#include <sys/auxv.h>
		#include <asm/hwcap.h>
	#elif defined(__APPLE__)
		#include <sys/sysctl.h>
	#endif
	#if defined(__ARM_FEATURE_SVE)
		#include <arm_sve.h>
	#endif
#elif defined(__x86_64__) || defined(_M_X64)
	#if defined(_MSC_VER)
		#include <intrin.h>
	#elif defined(__GNUC__) || defined(__clang__)
		#include <cpuid.h>
	#endif
#endif

enum class instruction_sets : uint32_t {
	fallback = 0x000,
	lzcnt	 = 0x001,
	popcnt	 = 0x002,
	bmi1	 = 0x004,
	clmul	 = 0x008,
	neon	 = 0x010,
	avx		 = 0x020,
	avx_2	 = 0x040,
	avx_512	 = 0x080,
	sve2	 = 0x100,
};

constexpr uint32_t to_bits(instruction_sets value) {
	return static_cast<uint32_t>(value);
}

#if defined(__aarch64__) || defined(_M_ARM64)

	#if defined(__linux__) && !defined(HWCAP2_SVE2)
		#define HWCAP2_SVE2 (1 << 1)
	#endif

	#if defined(__linux__) && !defined(HWCAP_PMULL)
		#define HWCAP_PMULL (1 << 4)
	#endif

static uint32_t detect_supported_architectures() {
	uint32_t host_isa = to_bits(instruction_sets::fallback);
	#if defined(__linux__)
	if (getauxval(AT_HWCAP) & HWCAP_ASIMD) {
		host_isa |= to_bits(instruction_sets::neon);
	}
	if (getauxval(AT_HWCAP) & HWCAP_PMULL) {
		host_isa |= to_bits(instruction_sets::clmul);
	}
	if (getauxval(AT_HWCAP2) & HWCAP2_SVE2) {
		host_isa |= to_bits(instruction_sets::sve2);
	}
	#elif defined(__APPLE__)
	int32_t value{};
	size_t size = sizeof(value);
	if (sysctlbyname("hw.optional.AdvSIMD", &value, &size, nullptr, 0) != 0 || value) {
		host_isa |= to_bits(instruction_sets::neon);
	}
	value = 0;
	size  = sizeof(value);
	if (sysctlbyname("hw.optional.arm.FEAT_PMULL", &value, &size, nullptr, 0) == 0 && value) {
		host_isa |= to_bits(instruction_sets::clmul);
	}
	#else
	host_isa |= to_bits(instruction_sets::neon);
	#endif
	return host_isa;
}

static uint32_t detect_sve2_vector_length_bits() {
	#if defined(__ARM_FEATURE_SVE)
	return static_cast<uint32_t>(svcntb()) * 8u;
	#else
	return 0u;
	#endif
}

#elif defined(__x86_64__) || defined(_M_X64)

static constexpr uint32_t cpuid_popcnt_bit		= 1u << 23;
static constexpr uint32_t cpuid_avx_bit			= 1u << 28;
static constexpr uint32_t cpuid_osx_save		= (1u << 26) | (1u << 27);
static constexpr uint32_t cpuid_lzcnt_bit		= 1u << 5;
static constexpr uint32_t cpuid_bmi1_bit		= 1u << 3;
static constexpr uint32_t cpuid_pclmulqdq_bit	= 1 << 1;
static constexpr uint32_t cpuid_avx2_bit		= 1u << 5;
static constexpr uint32_t cpuid_avx512f_bit		= 1u << 16;
static constexpr uint32_t cpuid_avx512bw_bit	= 1u << 30;
static constexpr uint32_t cpuid_avx512vbmi2_bit = 1u << 6;
static constexpr uint64_t cpuid_avx256_saved	= 1ull << 2;
static constexpr uint64_t cpuid_avx512_saved	= 7ull << 5;

struct cpuid_result {
	uint32_t eax{};
	uint32_t ebx{};
	uint32_t ecx{};
	uint32_t edx{};
	bool valid{};
};

static cpuid_result cpuid(uint32_t leaf, uint32_t subleaf) {
	cpuid_result result{};
	#if defined(_MSC_VER)
	int32_t max_leaf[4]{};
	__cpuid(max_leaf, static_cast<int32_t>(leaf & 0x80000000u));
	if (leaf > static_cast<uint32_t>(max_leaf[0])) {
		return result;
	}
	int32_t registers[4]{};
	__cpuidex(registers, static_cast<int32_t>(leaf), static_cast<int32_t>(subleaf));
	result.eax	 = static_cast<uint32_t>(registers[0]);
	result.ebx	 = static_cast<uint32_t>(registers[1]);
	result.ecx	 = static_cast<uint32_t>(registers[2]);
	result.edx	 = static_cast<uint32_t>(registers[3]);
	result.valid = true;
	#elif defined(__GNUC__) || defined(__clang__)
	result.valid = __get_cpuid_count(leaf, subleaf, &result.eax, &result.ebx, &result.ecx, &result.edx) != 0;
	#endif
	return result;
}

static uint64_t xgetbv() {
	#if defined(_MSC_VER)
	return _xgetbv(0);
	#else
	uint32_t eax{}, edx{};
	asm volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
	return (static_cast<uint64_t>(edx) << 32) | eax;
	#endif
}

static uint32_t detect_supported_architectures() {
	uint32_t host_isa = to_bits(instruction_sets::fallback);

	const cpuid_result leaf_one = cpuid(0x1, 0x0);
	if (!leaf_one.valid) {
		return host_isa;
	}

	if (leaf_one.ecx & cpuid_popcnt_bit) {
		host_isa |= to_bits(instruction_sets::popcnt);
	}

	if (leaf_one.ecx & cpuid_pclmulqdq_bit) {
		host_isa |= to_bits(instruction_sets::clmul);
	}

	const cpuid_result leaf_extended = cpuid(0x80000001, 0x0);
	if (leaf_extended.valid && (leaf_extended.ecx & cpuid_lzcnt_bit)) {
		host_isa |= to_bits(instruction_sets::lzcnt);
	}

	const cpuid_result leaf_seven = cpuid(0x7, 0x0);
	if (!leaf_seven.valid) {
		return host_isa;
	}

	if (leaf_seven.ebx & cpuid_bmi1_bit) {
		host_isa |= to_bits(instruction_sets::bmi1);
	}

	if ((leaf_one.ecx & cpuid_osx_save) != cpuid_osx_save) {
		return host_isa;
	}

	const uint64_t xcr0 = xgetbv();
	if ((xcr0 & cpuid_avx256_saved) == 0) {
		return host_isa;
	}

	if (leaf_one.ecx & cpuid_avx_bit) {
		host_isa |= to_bits(instruction_sets::avx);
	}

	if (leaf_seven.ebx & cpuid_avx2_bit) {
		host_isa |= to_bits(instruction_sets::avx_2);
	}

	if ((xcr0 & cpuid_avx512_saved) != cpuid_avx512_saved) {
		return host_isa;
	}

	const bool has_avx512 = (leaf_seven.ebx & cpuid_avx512f_bit) && (leaf_seven.ebx & cpuid_avx512bw_bit) && (leaf_seven.ecx & cpuid_avx512vbmi2_bit);
	if (has_avx512) {
		host_isa |= to_bits(instruction_sets::avx_512);
	}

	return host_isa;
}

static uint32_t detect_sve2_vector_length_bits() {
	return 0u;
}

#else

static uint32_t detect_supported_architectures() {
	return to_bits(instruction_sets::fallback);
}

static uint32_t detect_sve2_vector_length_bits() {
	return 0u;
}

#endif

static void report(const char* name, uint32_t supported_isa, instruction_sets value) {
	std::cout << name << "=" << ((supported_isa & to_bits(value)) ? 1 : 0) << std::endl;
}

int main() {
	const uint32_t supported_isa = detect_supported_architectures();
	report("HAS_LZCNT", supported_isa, instruction_sets::lzcnt);
	report("HAS_POPCNT", supported_isa, instruction_sets::popcnt);
	report("HAS_BMI1", supported_isa, instruction_sets::bmi1);
	report("HAS_CLMUL", supported_isa, instruction_sets::clmul);
	report("HAS_NEON", supported_isa, instruction_sets::neon);
	report("HAS_AVX512", supported_isa, instruction_sets::avx_512);
	report("HAS_AVX2", supported_isa, instruction_sets::avx_2);
	report("HAS_AVX", supported_isa, instruction_sets::avx);
	report("HAS_SVE2", supported_isa, instruction_sets::sve2);

	uint32_t sve2_vl_bits = 0u;
	if (supported_isa & to_bits(instruction_sets::sve2)) {
		sve2_vl_bits = detect_sve2_vector_length_bits();
	}
	std::cout << "SVE2_VL_BITS=" << sve2_vl_bits << std::endl;

	std::cout << "CPU_MASK=" << supported_isa << std::endl;
	std::cout << "CPU_SUCCESS=1" << std::endl;
	return 0;
}
