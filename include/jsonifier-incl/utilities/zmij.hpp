// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/zmij.hpp
#pragma once

// A double-to-string conversion algorithm based on Schubfach and xjb.
// Copyright (c) 2025 - present, Victor Zverovich
// Distributed under the MIT license (see LICENSE) or alternatively
// the Boost Software License, Version 1.0.
// https://github.com/vitaut/zmij/

namespace zmij {
	struct dec_fp {
		long long sig;
		int32_t exp;
		bool negative;
	};
}// namespace zmij

#include <jsonifier-incl/core/config.hpp>

#include <jsonifier-incl/simd/avx.hpp>
#include <jsonifier-incl/simd/neon.hpp>

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_SIMD)
	#define ZMIJ_USE_SIMD 1
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	#define ZMIJ_USE_NEON ZMIJ_USE_SIMD
	#include <arm_neon.h>
#else
	#define ZMIJ_USE_NEON 0
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	#define ZMIJ_USE_SSE ZMIJ_USE_SIMD
	#define ZMIJ_USE_SSE4_1 ZMIJ_USE_SSE
	#include <immintrin.h>
#else
	#define ZMIJ_USE_SSE 0
	#define ZMIJ_USE_SSE4_1 0
#endif

#define ZMIJ_USE_SIMD_SHUFFLE ((ZMIJ_USE_NEON || ZMIJ_USE_SSE4_1) && !ZMIJ_OPTIMIZE_SIZE)

#ifdef __aarch64__
	#define ZMIJ_AARCH64 1
#else
	#define ZMIJ_AARCH64 0
#endif

#ifdef __x86_64__
	#define ZMIJ_X86_64 1
#else
	#define ZMIJ_X86_64 0
#endif

#ifdef __clang__
	#define ZMIJ_CLANG 1
#else
	#define ZMIJ_CLANG 0
#endif

#ifdef _MSC_VER
	#define ZMIJ_MSC_VER _MSC_VER
	#include <intrin.h>// __lzcnt64/_umul128/__umulh
#else
	#define ZMIJ_MSC_VER 0
#endif

#if defined(__has_builtin) && !defined(ZMIJ_NO_BUILTINS)
	#define ZMIJ_HAS_BUILTIN(x) __has_builtin(x)
#else
	#define ZMIJ_HAS_BUILTIN(x) 0
#endif
#ifdef __has_attribute
	#define ZMIJ_HAS_ATTRIBUTE(x) __has_attribute(x)
#else
	#define ZMIJ_HAS_ATTRIBUTE(x) 0
#endif
#ifdef __has_cpp_attribute
	#define ZMIJ_HAS_CPP_ATTRIBUTE(x) __has_cpp_attribute(x)
#else
	#define ZMIJ_HAS_CPP_ATTRIBUTE(x) 0
#endif

#if ZMIJ_HAS_CPP_ATTRIBUTE(likely) && ZMIJ_HAS_CPP_ATTRIBUTE(unlikely)
	#define ZMIJ_LIKELY likely
	#define ZMIJ_UNLIKELY unlikely
#else
	#define ZMIJ_LIKELY
	#define ZMIJ_UNLIKELY
#endif

#if ZMIJ_HAS_CPP_ATTRIBUTE(maybe_unused)
	#define ZMIJ_MAYBE_UNUSED maybe_unused
#else
	#define ZMIJ_MAYBE_UNUSED
#endif

#ifdef ZMIJ_OPTIMIZE_SIZE
// Use the provided definition.
#elif defined(__OPTIMIZE_SIZE__)
	#define ZMIJ_OPTIMIZE_SIZE 1
#else
	#define ZMIJ_OPTIMIZE_SIZE 0
#endif
#ifndef ZMIJ_USE_EXP_STRING_TABLE
	#define ZMIJ_USE_EXP_STRING_TABLE ZMIJ_OPTIMIZE_SIZE == 0
#endif

#ifdef __GNUC__
	#define ZMIJ_ASM(x) asm x
#else
	#define ZMIJ_ASM(x)
#endif

// Declares struct members that must live in memory on ARM64 but are encoded as
// immediates in the x64 assembly.
#ifdef ZMIJ_CONST_DECL
// Use the provided definition.
#elif ZMIJ_AARCH64
	#define ZMIJ_CONST_DECL
#else
	#define ZMIJ_CONST_DECL static constexpr
#endif

namespace {

#ifdef __cpp_lib_is_constant_evaluated
	using std::is_constant_evaluated;
	#define ZMIJ_CONSTEXPR constexpr
#else
	constexpr auto is_constant_evaluated() -> bool {
		return false;
	}
	#define ZMIJ_CONSTEXPR
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	constexpr bool is_big_endian = true;
#else
	constexpr bool is_big_endian = false;
#endif

	JSONIFIER_INLINE auto bswap64(uint64_t x) noexcept -> uint64_t {
#if ZMIJ_HAS_BUILTIN(__builtin_bswap64)
		return __builtin_bswap64(x);
#elif ZMIJ_MSC_VER
		return _byteswap_uint64(x);
#else
		return ((x & 0xff00000000000000ULL) >> 56) | ((x & 0x00ff000000000000ULL) >> 40) | ((x & 0x0000ff0000000000ULL) >> 24) | ((x & 0x000000ff00000000ULL) >> 8) |
			((x & 0x00000000ff000000ULL) << 8) | ((x & 0x0000000000ff0000ULL) << 24) | ((x & 0x000000000000ff00ULL) << 40) | ((x & 0x00000000000000ffULL) << 56);
#endif
	}

	JSONIFIER_INLINE auto clz(uint64_t x) noexcept -> int32_t {
		return static_cast<int32_t>(std::countl_zero(x));
	}

	JSONIFIER_INLINE auto ctz(uint64_t x) noexcept -> int32_t {
		return static_cast<int32_t>(std::countr_zero(x));
	}

	JSONIFIER_INLINE auto select(uint64_t condition, int64_t true_value, int64_t false_value) -> int64_t {
		if constexpr (!ZMIJ_X86_64 || ZMIJ_CLANG) {
			return (condition != 0) ? true_value : false_value;
		} else {
			ZMIJ_ASM(volatile("test %2, %2\n\t"
							  "cmovne %1, %0\n\t" ://
				"+r"(false_value) : "r"(true_value),
				"r"(condition) : "cc"));
			return false_value;
		}
	}

	struct uint128 {
		uint64_t hi;
		uint64_t lo;

		[[maybe_unused]] JSONIFIER_INLINE explicit constexpr operator uint64_t() const noexcept {
			return lo;
		}

		template<uint64_t shift> [[maybe_unused]] JSONIFIER_INLINE constexpr auto operator>>(jsonifier::internal::integral_constant<shift>) const noexcept -> uint128 {
			if constexpr (shift == 32) {
				return { hi >> 32, (hi << 32) | (lo >> 32) };
			} else {
				static_assert(shift >= 64 && shift < 128);
				return { 0, hi >> (shift - 64) };
			}
		}
	};

#ifdef ZMIJ_USE_INT128
#elif defined(__SIZEOF_INT128__)
	#define ZMIJ_USE_INT128 1
#else
	#define ZMIJ_USE_INT128 0
#endif

#if ZMIJ_USE_INT128
	using uint128_t = __uint128_t;
#else
	using uint128_t = uint128;
#endif

#if ZMIJ_USE_INT128 && defined(__APPLE__)
	constexpr bool use_umul128_hi64 = true;
#else
	constexpr bool use_umul128_hi64 = false;
#endif

	JSONIFIER_INLINE constexpr auto umul128(uint64_t x, uint64_t y) noexcept -> uint128_t {
#if ZMIJ_USE_INT128
		return uint128_t(x) * y;
#else
		if (!is_constant_evaluated()) {
	#if defined(_M_AMD64) && defined(__cpp_lib_is_constant_evaluated)
			uint64_t hi = 0;
			uint64_t lo = _umul128(x, y, &hi);
			return { hi, lo };
	#elif defined(_M_ARM64) && defined(__cpp_lib_is_constant_evaluated)
			return { __umulh(x, y), x * y };
	#endif
		}
		uint64_t a = x >> 32;
		uint64_t b = static_cast<uint32_t>(x);
		uint64_t c = y >> 32;
		uint64_t d = static_cast<uint32_t>(y);

		uint64_t ac = a * c;
		uint64_t bc = b * c;
		uint64_t ad = a * d;
		uint64_t bd = b * d;

		uint64_t cs = (bd >> 32) + static_cast<uint32_t>(ad) + static_cast<uint32_t>(bc);
		return { ac + (ad >> 32) + (bc >> 32) + (cs >> 32), (cs << 32) + static_cast<uint32_t>(bd) };
#endif
	}

	JSONIFIER_INLINE constexpr auto umul128_hi64(uint64_t x, uint64_t y) noexcept -> uint64_t {
#if ZMIJ_USE_INT128
		return static_cast<uint64_t>(umul128(x, y) >> 64);
#else
		return umul128(x, y).hi;
#endif
	}

	JSONIFIER_INLINE auto umul128_add_hi64(uint64_t x, uint64_t y, uint64_t c) noexcept -> uint64_t {
#if ZMIJ_USE_INT128
		return static_cast<uint64_t>((uint128_t(x) * y + c) >> 64);
#else
		auto p = umul128(x, y);
		return p.hi + (p.lo + c < p.lo ? 1ULL : 0ULL);
#endif
	}

	JSONIFIER_INLINE auto umul192_hi128(uint64_t x_hi, uint64_t x_lo, uint64_t y) noexcept -> uint128 {
#if ZMIJ_USE_INT128
		uint128_t p = umul128(x_hi, y);
		uint64_t lo = static_cast<uint64_t>(p) + static_cast<uint64_t>(umul128(x_lo, y) >> 64);
		return { static_cast<uint64_t>(p >> 64) + (lo < static_cast<uint64_t>(p) ? 1ULL : 0ULL), lo };
#else
		uint128 p	= umul128(x_hi, y);
		uint64_t lo = p.lo + umul128(x_lo, y).hi;
		return { p.hi + (lo < p.lo ? 1ULL : 0ULL), lo };
#endif
	}

	JSONIFIER_INLINE auto div10(uint64_t x) noexcept -> uint64_t {
		constexpr uint64_t div10_sig64 = (1ull << 63) / 5ULL + 1ULL;
		return ZMIJ_USE_INT128 ? umul128_hi64(x, div10_sig64) : x / 10ULL;
	}

	JSONIFIER_INLINE constexpr auto compute_dec_exp(int32_t bin_exp, bool regular = true) noexcept -> int32_t {
		constexpr int32_t log10_3_over_4_sig = 131'072;
		constexpr int32_t log10_2_sig = 315'653, log10_2_exp = 20;
		return (bin_exp * log10_2_sig - (regular ? 0 : log10_3_over_4_sig)) >> log10_2_exp;
	}

	template<typename Float> struct float_traits : std::numeric_limits<Float> {
		static_assert(float_traits::is_iec559, "IEEE 754 required");

		static constexpr int32_t num_bits		   = float_traits::digits == 53 ? 64 : 32;
		static constexpr int32_t num_sig_bits	   = float_traits::digits - 1;
		static constexpr int32_t num_exp_bits	   = num_bits - num_sig_bits - 1;
		static constexpr int32_t exp_mask		   = static_cast<int32_t>((1U << num_exp_bits) - 1U);
		static constexpr int32_t exp_bias		   = static_cast<int32_t>((1U << (num_exp_bits - 1)) - 1U);
		static constexpr int32_t exp_offset		   = exp_bias + num_sig_bits;
		static constexpr int32_t min_fixed_dec_exp = -4;
		static constexpr int32_t max_fixed_dec_exp = compute_dec_exp(float_traits::digits + 1) - 1;

		using sig_type						   = std::conditional_t<num_bits == 64, uint64_t, uint32_t>;
		static constexpr sig_type implicit_bit = static_cast<sig_type>(1) << num_sig_bits;

		JSONIFIER_INLINE static auto to_bits(Float value) noexcept -> sig_type {
			sig_type bits;
			memcpy(&bits, &value, sizeof(value));
			return bits;
		}

		JSONIFIER_INLINE static auto is_negative(sig_type bits) noexcept -> bool {
			return (bits >> (num_bits - 1)) != 0;
		}
		JSONIFIER_INLINE static auto get_sig(sig_type bits) noexcept -> sig_type {
			return bits & (implicit_bit - 1U);
		}
		JSONIFIER_INLINE static auto get_exp(sig_type bits) noexcept -> int64_t {
			return static_cast<int64_t>((bits << 1U) >> (num_sig_bits + 1));
		}
	};

	constexpr uint64_t pow10_minor[] = {
		0x8000000000000000,
		0xa000000000000000,
		0xc800000000000000,
		0xfa00000000000000,
		0x9c40000000000000,
		0xc350000000000000,
		0xf424000000000000,
		0x9896800000000000,
		0xbebc200000000000,
		0xee6b280000000000,
		0x9502f90000000000,
		0xba43b74000000000,
		0xe8d4a51000000000,
		0x9184e72a00000000,
		0xb5e620f480000000,
		0xe35fa931a0000000,
		0x8e1bc9bf04000000,
		0xb1a2bc2ec5000000,
		0xde0b6b3a76400000,
		0x8ac7230489e80000,
		0xad78ebc5ac620000,
		0xd8d726b7177a8000,
		0x878678326eac9000,
		0xa968163f0a57b400,
		0xd3c21bcecceda100,
		0x84595161401484a0,
		0xa56fa5b99019a5c8,
		0xcecb8f27f4200f3a,
	};
	constexpr uint128 pow10_major[] = {
		{ 0xaf8e5410288e1b6f, 0x07ecf0ae5ee44dda },// -303
		{ 0xb1442798f49ffb4a, 0x99cd11cfdf41779d },// -275
		{ 0xb2fe3f0b8599ef07, 0x861fa7e6dcb4aa15 },// -247
		{ 0xb4bca50b065abe63, 0x0fed077a756b53aa },// -219
		{ 0xb67f6455292cbf08, 0x1a3bc84c17b1d543 },// -191
		{ 0xb84687c269ef3bfb, 0x3d5d514f40eea742 },// -163
		{ 0xba121a4650e4ddeb, 0x92f34d62616ce413 },// -135
		{ 0xbbe226efb628afea, 0x890489f70a55368c },// -107
		{ 0xbdb6b8e905cb600f, 0x5400e987bbc1c921 },//  -79
		{ 0xbf8fdb78849a5f96, 0xde98520472bdd034 },//  -51
		{ 0xc16d9a0095928a27, 0x75b7053c0f178294 },//  -23
		{ 0xc350000000000000, 0x0000000000000000 },//    5
		{ 0xc5371912364ce305, 0x6c28000000000000 },//   33
		{ 0xc722f0ef9d80aad6, 0x424d3ad2b7b97ef6 },//   61
		{ 0xc913936dd571c84c, 0x03bc3a19cd1e38ea },//   89
		{ 0xcb090c8001ab551c, 0x5cadf5bfd3072cc6 },//  117
		{ 0xcd036837130890a1, 0x36dba887c37a8c10 },//  145
		{ 0xcf02b2c21207ef2e, 0x94f967e45e03f4bc },//  173
		{ 0xd106f86e69d785c7, 0xe13336d701beba52 },//  201
		{ 0xd31045a8341ca07c, 0x1ede48111209a051 },//  229
		{ 0xd51ea6fa85785631, 0x552a74227f3ea566 },//  257
		{ 0xd732290fbacaf133, 0xa97c177947ad4096 },//  285
		{ 0xd94ad8b1c7380874, 0x18375281ae7822bc },//  313
	};
	constexpr uint32_t pow10_fixups[] = { 0x0a4e363f, 0x00001840, 0x00006400, 0x24200040, 0x00000000, 0x0c000000, 0x82c81380, 0x5e4ce01f, 0xd730f60f, 0x0000001b, 0x00000000,
		0xcdf7fffc, 0x6e8201d8, 0x40cd3fd1, 0xdb642501, 0x00000d0d, 0x14042400, 0x53713840, 0x11781db4, 0x00000000 };

	struct alignas(64) pow10_significand_table {
		static constexpr bool compress				 = ZMIJ_OPTIMIZE_SIZE != 0;
		static constexpr bool split_tables			 = !compress && ZMIJ_AARCH64 != 0;
		static constexpr int32_t num_pow10s				 = 618;
		uint64_t data[compress ? 1 : num_pow10s * 2] = {};

		JSONIFIER_INLINE static constexpr auto compute(uint32_t i) noexcept -> uint128 {
			constexpr uint32_t stride = sizeof(pow10_minor) / sizeof(*pow10_minor);
			auto m					  = pow10_minor[(i + 10U) % stride];
			auto h					  = pow10_major[(i + 10U) / stride];

			uint64_t h1 = umul128_hi64(h.lo, m);

			uint64_t c0 = h.lo * m;
			uint64_t c1 = h1 + h.hi * m;
			uint64_t c2 = (c1 < h1 ? 1ULL : 0ULL) + umul128_hi64(h.hi, m);

			uint128 result = (c2 >> 63) != 0ULL ? uint128{ c2, c1 } : uint128{ (c2 << 1) | (c1 >> 63), (c1 << 1) | (c0 >> 63) };
			result.lo -= (pow10_fixups[i >> 5] >> (i & 31U)) & 1ULL;
			return result;
		}

		consteval pow10_significand_table() {
			for (int32_t i = 0; i < num_pow10s && !compress; ++i) {
				uint128 result = compute(static_cast<uint32_t>(i));
				if constexpr (split_tables) {
					data[num_pow10s - i - 1]	 = result.hi;
					data[num_pow10s * 2 - i - 1] = result.lo;
				} else {
					data[i * 2]		= result.hi;
					data[i * 2 + 1] = result.lo;
				}
			}
		}

		JSONIFIER_INLINE constexpr auto operator[](int32_t dec_exp) const noexcept -> uint128 {
			constexpr int32_t dec_exp_min = -293;
			int32_t i					  = dec_exp - dec_exp_min;
			if constexpr (compress) {
				return compute(static_cast<uint32_t>(i));
			} else {
				if constexpr (!split_tables) {
					const uint64_t* p = data + i * 2;
					return { p[0], p[1] };
				} else {
					const uint64_t* p = data + num_pow10s + dec_exp_min;
					if (!is_constant_evaluated()) {
						ZMIJ_ASM(("" : "+r"(p)));
						return { p[~dec_exp], p[~dec_exp + num_pow10s] };
					} else {
						return { p[~dec_exp], p[~dec_exp + num_pow10s] };
					}
				}
			}
		}
	};

	JSONIFIER_INLINE constexpr auto compute_exp_shift(int32_t bin_exp, int32_t dec_exp) noexcept -> uint8_t {
		constexpr int32_t log2_pow10_sig = 217'707, log2_pow10_exp = 16;
		int32_t pow10_bin_exp = (-dec_exp * log2_pow10_sig) >> log2_pow10_exp;
		return static_cast<uint8_t>(bin_exp + pow10_bin_exp + 1);
	}

	struct exp_shift_table {
		static constexpr bool enable										= ZMIJ_OPTIMIZE_SIZE == 0;
		static constexpr int32_t extra_shift									= 6;
		uint8_t data[enable ? float_traits<double>::exp_mask + 1 : 1] = {};

		consteval exp_shift_table() {
			for (int32_t raw_exp = 0; raw_exp < static_cast<int32_t>(sizeof(data)) && enable; ++raw_exp) {
				int32_t bin_exp = raw_exp - float_traits<double>::exp_offset;
				if (raw_exp == 0)
					++bin_exp;
				int32_t dec_exp	  = compute_dec_exp(bin_exp);
				data[raw_exp] = static_cast<uint8_t>(compute_exp_shift(bin_exp, dec_exp + 1) + extra_shift);
			}
		}
	};

	struct alignas(64) exp_string_table {
		static constexpr bool enable										 = ZMIJ_USE_EXP_STRING_TABLE;
		using traits														 = float_traits<double>;
		static constexpr int32_t min_dec_exp									 = traits::min_exponent10 - traits::max_digits10;
		static constexpr int32_t offset											 = -min_dec_exp;
		uint64_t data[enable ? traits::max_exponent10 - min_dec_exp + 1 : 1] = {};

		consteval exp_string_table() {
			for (int32_t e = min_dec_exp; e <= traits::max_exponent10 && enable; ++e) {
				uint64_t abs_e = e >= 0 ? static_cast<uint64_t>(e) : static_cast<uint64_t>(-e);
				uint64_t bc	   = abs_e % 100ULL;
				uint64_t val   = ((bc % 10ULL + static_cast<uint64_t>('0')) << 8) | (bc / 10ULL + static_cast<uint64_t>('0'));
				if (uint64_t a = abs_e / 100ULL)
					val = (val << 8) | (a + static_cast<uint64_t>('0'));
				uint64_t len	 = 4ULL + (abs_e >= 100ULL ? 1ULL : 0ULL);
				data[e + offset] = (len << 48) | (val << 16) | (static_cast<uint64_t>(e >= 0 ? '+' : '-') << 8) | static_cast<uint64_t>('e');
			}
		}
	};

	struct exp_float_shuffle_table {
		static constexpr bool enable						 = (ZMIJ_USE_SSE4_1 || ZMIJ_USE_NEON) && exp_string_table::enable;
		static constexpr uint8_t exp_pos				 = 8;
		static constexpr uint8_t last_digit_pos		 = 12;
		static constexpr uint8_t point_pos			 = 13;
		alignas(64) uint8_t data[enable ? 32 * 16 : 1] = {};

		struct entry {
			const uint8_t* shuffle;
			uint8_t length;
		};

		JSONIFIER_INLINE constexpr auto get_entry(int32_t num_digits, bool has_last_digit, bool has_extra_digit) const noexcept {
			int32_t idx = (num_digits - 1) * 4 + (has_last_digit ? 2 : 0) + (has_extra_digit ? 1 : 0);
			return entry{ &data[idx * 16], data[idx * 16 + 15] };
		}

		consteval exp_float_shuffle_table() {
			for (int32_t idx = 0; idx < 32 && enable; ++idx) {
				int32_t num_digits		 = (idx >> 2) + 1;
				bool has_last_digit	 = ((idx >> 1) & 1) != 0;
				bool has_extra_digit = (idx & 1) != 0;

				uint8_t* out = &data[idx * 16];
				for (int32_t i = 0; i < 16; ++i)
					out[i] = 0x80;
				uint8_t leading_digit_pos = has_extra_digit ? 7U : 6U;
				uint8_t length			= 0;
				if (has_last_digit) {
					out[length++] = leading_digit_pos;
					out[length++] = point_pos;
					for (int32_t i = static_cast<int32_t>(leading_digit_pos) - 1; i >= 0; --i)
						out[length++] = static_cast<uint8_t>(i);
					out[length++] = last_digit_pos;
				} else {
					length = static_cast<uint8_t>(num_digits + (has_extra_digit ? 1 : 0));
					if (length == 2)
						length = 1;
					out[0] = leading_digit_pos;
					out[1] = point_pos;
					for (int32_t i = 2; i < static_cast<int32_t>(length); ++i)
						out[i] = static_cast<uint8_t>(static_cast<int32_t>(leading_digit_pos) + 1 - i);
				}
				for (uint8_t i = 0; i < 4U; ++i)
					out[length++] = static_cast<uint8_t>(static_cast<int32_t>(exp_pos) + static_cast<int32_t>(i));
				out[15] = length;
			}
		}
	};

	// Per-decimal-exponent buffer layout for branchless fixed-notation output.
	// Each entry holds the byte positions of the leading zeros, decimal point,
	// and end of output, indexed by the decimal exponent (dec_exp).
	struct fixed_layout_table {
		using traits					 = float_traits<double>;
		static constexpr int32_t num_entries = traits::max_fixed_dec_exp - traits::min_fixed_dec_exp + 1;

		// On AArch64, align entry to 32 bytes so indexing uses `lsl #5` not `umaddl`.
		struct alignas(ZMIJ_AARCH64 && !ZMIJ_OPTIMIZE_SIZE ? 32 : 1) entry {
			// Byte offset past leading "0.00..." before first significant digit.
			uint8_t start_pos;
			uint8_t point_pos;
			// Start position for shifting digits right by one to insert the point.
			uint8_t shift_pos;
			// Offset past the end of fixed-notation output, indexed by sig length - 1.
			uint8_t end_pos[traits::max_digits10];
		};
		entry data[num_entries] = {};

		consteval fixed_layout_table() {
			for (int32_t dec_exp = traits::min_fixed_dec_exp; dec_exp <= traits::max_fixed_dec_exp; ++dec_exp) {
				auto& e = data[dec_exp - traits::min_fixed_dec_exp];

				e.start_pos = dec_exp < 0 ? static_cast<uint8_t>(1 - dec_exp) : static_cast<uint8_t>(0);
				e.point_pos = dec_exp >= 0 ? static_cast<uint8_t>(1 + dec_exp) : static_cast<uint8_t>(1);
				e.shift_pos = static_cast<uint8_t>(static_cast<int32_t>(e.point_pos) + (dec_exp >= 0 ? 1 : 0));

				for (int32_t n = 1; n <= traits::max_digits10; ++n) {
					int32_t end_pos = n;
					if (dec_exp >= 0)
						end_pos = n > dec_exp + 1 ? n + 1 : dec_exp + 1;
					e.end_pos[n - 1] = static_cast<uint8_t>(end_pos);
				}
			}
		}

		[[maybe_unused]] inline constexpr auto get(int32_t dec_exp) const noexcept -> const entry& {
			constexpr auto min = traits::min_fixed_dec_exp;
			return data[static_cast<uint32_t>(dec_exp - min)];
		}
	};

	[[maybe_unused]] JSONIFIER_INLINE auto count_trailing_nonzeros(uint64_t x) noexcept -> int32_t {
		if constexpr (is_big_endian)
			x = bswap64(x);
		return static_cast<int32_t>((70ULL - static_cast<uint64_t>(clz((x << 1U) | 1ULL))) / 8ULL);
	}

	// Converts value in the range [0, 100) to a string. GCC generates a bit better
	// code when value is pointer-size (https://www.godbolt.org/z/5fEPMT1cc).
	[[maybe_unused]] JSONIFIER_INLINE auto digits2(uint64_t value) noexcept -> jsonifier::string_view_ptr {
		// Align data since unaligned access may be slower when crossing a
		// hardware-specific boundary.
		alignas(64) static const char data[] = "0001020304050607080910111213141516171819"
											   "2021222324252627282930313233343536373839"
											   "4041424344454647484950515253545556575859"
											   "6061626364656667686970717273747576777879"
											   "8081828384858687888990919293949596979899";
		return &data[value * 2ULL];
	}

	constexpr int32_t div10k_exp	  = 40;
	constexpr uint32_t div10k_sig = static_cast<uint32_t>((1ULL << div10k_exp) / 10000ULL + 1ULL);
	constexpr uint32_t neg10k	  = static_cast<uint32_t>((1ULL << 32) - 10000ULL);

	constexpr int32_t div100_exp	  = 19;
	constexpr uint32_t div100_sig = (1U << div100_exp) / 100U + 1U;
	constexpr uint32_t neg100	  = (1U << 16) - 100U;

	constexpr int32_t div10_exp		 = 10;
	constexpr uint32_t div10_sig = (1U << div10_exp) / 10U + 1U;
	constexpr uint32_t neg10	 = (1U << 8) - 10U;

	constexpr uint64_t zeros = 0x0101010101010101ULL * static_cast<uint64_t>('0');

	[[maybe_unused]] JSONIFIER_INLINE auto write_if(jsonifier::string_buffer_ptr buffer, uint32_t digit, bool condition) noexcept -> jsonifier::string_buffer_ptr {
		*buffer = static_cast<char>(static_cast<uint32_t>('0') + digit);
		return buffer + (condition ? 1 : 0);
	}

	static consteval auto splat64(uint64_t x) -> uint128 {
		return { x, x };
	}
	static consteval auto splat32(uint32_t x) -> uint128 {
		return splat64((static_cast<uint64_t>(x) << 32) | static_cast<uint64_t>(x));
	}
	static consteval auto splat16(uint16_t x) -> uint128 {
		return splat32((static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(x));
	}
	static consteval auto pack8(uint8_t a, uint8_t b, uint8_t c, uint8_t d,//
		uint8_t e, uint8_t f, uint8_t g, uint8_t h) -> uint64_t {
		using u64 = uint64_t;
		return (u64(h) << 56) | (u64(g) << 48) | (u64(f) << 40) | (u64(e) << 32) | (u64(d) << 24) | (u64(c) << 16) | (u64(b) << 8) | u64(a);
	}

	struct alignas(64) data {
		static constexpr uint64_t threshold = 1000000000000000ULL;
		// +6 is needed for boundary cases found by verify.py.
		static constexpr uint64_t biased_half = (1ULL << 63) + 6ULL;

#if ZMIJ_USE_NEON
		static constexpr int32_t neg10k = 0x10000 - 10000;

		using int32x4 = std::conditional_t<ZMIJ_MSC_VER != 0, int32_t[4], int32x4_t>;
		using int16x8 = std::conditional_t<ZMIJ_MSC_VER != 0, int16_t[8], int16x8_t>;

		static constexpr uint64_t mul_const		  = 0xabcc77118461cefdULL;
		static constexpr uint64_t hundred_million = 100000000ULL;
		static constexpr int32x4 multipliers32	  = { static_cast<int32_t>(div10k_sig), neg10k, static_cast<int32_t>(div100_sig << 12), static_cast<int32_t>(neg100) };
		static constexpr int16x8 multipliers16	  = { 0xce0, static_cast<int16_t>(neg10) };
#elif ZMIJ_USE_SSE
		// Ordered so that the values used to format floats fit in a single cache
		// line.
		static constexpr uint128 div100 = splat32(div100_sig);
		static constexpr uint128 div10	= splat16(static_cast<uint16_t>(((1U << 16) / 10U) + 1U));
	#if ZMIJ_USE_SSE4_1
		static constexpr uint128 neg100 = splat32(::neg100);
		static constexpr uint128 neg10	= splat16(static_cast<uint16_t>((1U << 8) - 10U));
		static constexpr uint128 bswap	= uint128{ pack8(15, 14, 13, 12, 11, 10, 9, 8), pack8(7, 6, 5, 4, 3, 2, 1, 0) };
	#else
		uint128 hundred	 = splat32(100U);
		uint128 moddiv10 = splat16(static_cast<uint16_t>(10U * (1U << 8) - 1U));
	#endif// ZMIJ_USE_SSE4_1
		static constexpr uint128 div10k = splat64(div10k_sig);
		static constexpr uint128 neg10k = splat64(::neg10k);
		static constexpr uint128 zeros	= splat64(::zeros);
#endif// ZMIJ_USE_SSE

		static constexpr exp_shift_table exp_shifts{};
		static constexpr exp_string_table exp_strings{};
		static constexpr pow10_significand_table pow10_significands{};
		static constexpr fixed_layout_table fixed_layouts{};
		static constexpr exp_float_shuffle_table exp_float_shuffles{};

		// Shuffle indices for SIMD digit shift. Offset 0 = identity, offset 1 =
		// shift left by 1 (drops the leading '0' of a 16-digit significand).
		static constexpr uint8_t shift_shuffle[17] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0 };
	};
	alignas(64) constexpr data static_data{};

#if ZMIJ_USE_NEON// An optimized version for NEON by Dougall Johnson.

	// Converts four numbers < 10000, one in each 32-bit lane, to BCD digits.
	JSONIFIER_INLINE auto to_bcd_4x4(int32x4_t efgh_abcd_mnop_ijkl) noexcept -> uint8x16_t {
		// Compiler barrier, or clang breaks the subsequent MLA into UADDW + MUL.
		ZMIJ_ASM(("" : "+w"(efgh_abcd_mnop_ijkl)));

		int32x4_t ef_ab_mn_ij			  = vqdmulhq_n_s32(efgh_abcd_mnop_ijkl, static_data.multipliers32[2]);
		int16x8_t gh_ef_cd_ab_op_mn_kl_ij = vreinterpretq_s16_s32(vmlaq_n_s32(efgh_abcd_mnop_ijkl, ef_ab_mn_ij, static_data.multipliers32[3]));
		int16x8_t high_10s				  = vqdmulhq_n_s16(gh_ef_cd_ab_op_mn_kl_ij, static_data.multipliers16[0]);
		return vreinterpretq_u8_s16(vmlaq_n_s16(gh_ef_cd_ab_op_mn_kl_ij, high_10s, static_data.multipliers16[1]));
	}

	JSONIFIER_INLINE auto to_unshuffled_digits(uint64_t value) -> uint8x16_t {
		uint64_t hundred_million = static_data.hundred_million;

		// Compiler barrier, or clang narrows the load to 32-bit and unpairs it.
		ZMIJ_ASM(("" : "+r"(hundred_million)));

		// abcdefgh = value / 100000000, ijklmnop = value % 100000000.
		uint64_t abcdefgh = static_cast<uint64_t>(umul128(value, static_data.mul_const) >> 90);
		uint64_t ijklmnop = value - abcdefgh * hundred_million;

		uint64x1_t ijklmnop_abcdefgh_64 = { (ijklmnop << 32) | abcdefgh };
		int32x2_t abcdefgh_ijklmnop		= vreinterpret_s32_u64(ijklmnop_abcdefgh_64);

		int32x2_t abcd_ijkl				 = vreinterpret_s32_u32(vshr_n_u32(vreinterpret_u32_s32(vqdmulh_n_s32(abcdefgh_ijklmnop, static_data.multipliers32[0])), 9U));
		int32x2_t efgh_abcd_mnop_ijkl_32 = vmla_n_s32(abcdefgh_ijklmnop, abcd_ijkl, static_data.multipliers32[1]);

		int32x4_t efgh_abcd_mnop_ijkl = vreinterpretq_s32_u32(vshll_n_u16(vreinterpret_u16_s32(efgh_abcd_mnop_ijkl_32), 0U));
		return to_bcd_4x4(efgh_abcd_mnop_ijkl);
	}

#elif ZMIJ_USE_SSE

	using m128ptr = const __m128i*;

	JSONIFIER_INLINE auto to_bcd_4x4(__m128i y) noexcept -> __m128i {
		const __m128i div100_local = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.div100);
		const __m128i div10_local  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.div10);
	#if ZMIJ_USE_SSE4_1
		const __m128i neg100_local = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.neg100);
		const __m128i neg10_local  = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.neg10);

		__m128i z = _mm_add_epi64(y, _mm_mullo_epi32(neg100_local, _mm_srli_epi32(_mm_mulhi_epu16(y, div100_local), 3)));
		return _mm_add_epi16(z, _mm_mullo_epi16(neg10_local, _mm_mulhi_epu16(z, div10_local)));
	#else
		const __m128i hundred  = _mm_load_si128(m128ptr(&static_data.hundred));
		const __m128i moddiv10 = _mm_load_si128(m128ptr(&static_data.moddiv10));

		__m128i y_div_100 = _mm_srli_epi16(_mm_mulhi_epu16(y, div100_local), 3);
		__m128i y_mod_100 = _mm_sub_epi16(y, _mm_mullo_epi16(y_div_100, hundred));
		__m128i z		  = _mm_or_si128(_mm_slli_epi32(y_mod_100, 16), y_div_100);
		return _mm_sub_epi16(_mm_slli_epi16(z, 8), _mm_mullo_epi16(moddiv10, _mm_mulhi_epu16(z, div10)));
	#endif
	}

#endif

	struct bcd_result {
		uint64_t bcd;
		int32_t len;
	};

	[[maybe_unused]] JSONIFIER_INLINE auto to_bcd8(uint64_t abcdefgh) noexcept -> bcd_result {
		if constexpr (!ZMIJ_USE_SSE && !ZMIJ_USE_NEON) {
			uint64_t abcd_efgh		 = abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
			uint64_t ab_cd_ef_gh	 = abcd_efgh + neg100 * (((abcd_efgh * div100_sig) >> div100_exp) & 0x7f0000007fULL);
			uint64_t a_b_c_d_e_f_g_h = ab_cd_ef_gh + neg10 * (((ab_cd_ef_gh * div10_sig) >> div10_exp) & 0xf000f000f000fULL);
			uint64_t bcd			 = is_big_endian ? a_b_c_d_e_f_g_h : bswap64(a_b_c_d_e_f_g_h);
			return { bcd, count_trailing_nonzeros(bcd) };
		} else {
			[[maybe_unused]] const auto* d = &static_data;
			ZMIJ_ASM(("" : "+r"(d)));

#if ZMIJ_USE_NEON
			uint64_t abcd_efgh_64 = abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
			int32x4_t abcd_efgh	  = vcombine_s32(vreinterpret_s32_u64(vcreate_u64(abcd_efgh_64)), vdup_n_s32(0));
			uint8x16_t digits_128 = to_bcd_4x4(abcd_efgh);
			uint8x8_t digits	  = vget_low_u8(digits_128);
			uint64_t bcd		  = vget_lane_u64(vreinterpret_u64_u8(vrev64_u8(digits)), 0);
			return { bcd, count_trailing_nonzeros(bcd) };
#elif ZMIJ_USE_SSE4_1
			uint64_t abcd_efgh		= abcdefgh + neg10k * ((abcdefgh * div10k_sig) >> div10k_exp);
			uint64_t unshuffled_bcd = static_cast<uint64_t>(_mm_cvtsi128_si64(to_bcd_4x4(_mm_set_epi64x(0, static_cast<int64_t>(abcd_efgh)))));
			int32_t len					= unshuffled_bcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffled_bcd)) / 8 : 0;
			return { bswap64(unshuffled_bcd), len };
#elif ZMIJ_USE_SSE
			uint64_t abcd_efgh = (abcdefgh << 32) - 10000ULL * ((abcdefgh * div10k_sig) >> div10k_exp);
			__m128i v		   = to_bcd_4x4(_mm_set_epi64x(0, static_cast<int64_t>(abcd_efgh)));
	#if defined(__x86_64__) || defined(_M_X64)
			uint64_t bcd = static_cast<uint64_t>(_mm_cvtsi128_si64(v));
	#else
			uint64_t bcd =
				(static_cast<uint64_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(v, 4)))) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(_mm_cvtsi128_si32(v)));
	#endif
			return { bcd, count_trailing_nonzeros(bcd) };
#endif
		}
	}

	template<int32_t num_bits> struct dec_digits {
		uint64_t digits;
#if ZMIJ_USE_NEON
		uint8x16_t unshuffled;
#elif ZMIJ_USE_SSE4_1
		__m128i unshuffled;
#endif
		int32_t num_digits;
	};

	template<> struct dec_digits<64> {
#if ZMIJ_USE_NEON
		using digits_type = uint16x8_t;
#elif ZMIJ_USE_SSE
		using digits_type = __m128i;
#else
		using digits_type = uint128;
#endif
		digits_type digits;
		int32_t num_digits;
	};

	template<int32_t num_bits> JSONIFIER_INLINE auto to_digits(uint64_t value) noexcept -> dec_digits<num_bits> {
#if !ZMIJ_USE_NEON && !ZMIJ_USE_SSE
		uint32_t hi = static_cast<uint32_t>(value / 100'000'000ULL);
		uint32_t lo = static_cast<uint32_t>(value % 100'000'000ULL);
		auto hi_bcd = to_bcd8(hi);
		if (lo == 0U)
			return { { hi_bcd.bcd + zeros, zeros }, hi_bcd.len };
		auto lo_bcd = to_bcd8(lo);
		return { { hi_bcd.bcd + zeros, lo_bcd.bcd + zeros }, 8 + lo_bcd.len };
#elif ZMIJ_USE_NEON
		auto unshuffled_digits = to_unshuffled_digits(value);
		uint8x16_t digits	   = vrev64q_u8(unshuffled_digits);
		uint16x8_t str		   = vaddq_u16(vreinterpretq_u16_u8(digits), vreinterpretq_u16_s8(vdupq_n_s8('0')));
		uint16x8_t is_not_zero = vreinterpretq_u16_u8(vcgtzq_s8(vreinterpretq_s8_u8(digits)));
		uint64_t nonzero_mask  = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(is_not_zero, 4U)), 0);
		return { str, 16 - (clz(nonzero_mask) >> 2) };
#else
		uint32_t hi = static_cast<uint32_t>(value / 100'000'000ULL);
		uint32_t lo = static_cast<uint32_t>(value % 100'000'000ULL);

		const __m128i div10k	   = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.div10k);
		const __m128i neg10k_local = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.neg10k);
		__m128i x				   = _mm_set_epi64x(static_cast<int64_t>(hi), static_cast<int64_t>(lo));
		__m128i y				   = _mm_add_epi64(x, _mm_mul_epu32(neg10k_local, _mm_srli_epi64(_mm_mul_epu32(x, div10k), div10k_exp)));

		if constexpr (!ZMIJ_USE_SSE4_1)
			y = _mm_shuffle_epi32(y, _MM_SHUFFLE(0, 1, 2, 3));

		__m128i bcd				= to_bcd_4x4(y);
		const __m128i zeros_val = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.zeros);

		uint64_t mask = static_cast<uint64_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(bcd, _mm_setzero_si128())));
		int32_t len		  = ZMIJ_USE_SSE4_1 ? 16 - static_cast<int32_t>(ctz(mask)) : 64 - clz(mask);
	#if ZMIJ_USE_SSE4_1
		bcd = _mm_shuffle_epi8(bcd, jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.bswap));
	#endif
		return { _mm_or_si128(bcd, zeros_val), len };
#endif
	}

	template<> JSONIFIER_INLINE auto to_digits<32>(uint64_t value) noexcept -> dec_digits<32> {
#if ZMIJ_USE_SSE4_1
		uint64_t abcd_efgh		= value + neg10k * ((value * div10k_sig) >> div10k_exp);
		__m128i bcd_xmm			= to_bcd_4x4(_mm_set_epi64x(0, static_cast<int64_t>(abcd_efgh)));
		uint64_t unshuffled_bcd = static_cast<uint64_t>(_mm_cvtsi128_si64(bcd_xmm));
		int32_t len					= unshuffled_bcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffled_bcd)) / 8 : 0;
		return { bswap64(unshuffled_bcd) + zeros, bcd_xmm, len };
#elif ZMIJ_USE_NEON
		uint64_t abcd_efgh		= value + neg10k * ((value * div10k_sig) >> div10k_exp);
		int32x4_t input			= vcombine_s32(vreinterpret_s32_u64(vcreate_u64(abcd_efgh)), vdup_n_s32(0));
		uint8x16_t unshuffled	= to_bcd_4x4(input);
		uint64_t unshuffled_bcd = vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(unshuffled)), 0);
		int32_t len					= unshuffled_bcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffled_bcd)) / 8 : 0;
		return { bswap64(unshuffled_bcd) + zeros, unshuffled, len };
#else
		auto result = to_bcd8(value);
		return { result.bcd + zeros, result.len };
#endif
	}

	JSONIFIER_INLINE void write_digits(jsonifier::string_buffer_ptr buffer, dec_digits<64>::digits_type digits, bool drop_leading_zero) noexcept {
		if constexpr (!ZMIJ_USE_NEON && !ZMIJ_USE_SSE4_1) {
			memcpy(buffer, &digits, sizeof(digits));
			memmove(buffer, buffer + (drop_leading_zero ? 1 : 0), sizeof(digits));
			return;
		}
#if ZMIJ_USE_NEON
		uint8x16_t shuffle = vld1q_u8(static_data.shift_shuffle + (drop_leading_zero ? 1 : 0));
		uint8x16_t shifted = vqtbl1q_u8(vreinterpretq_u8_u16(digits), shuffle);
		vst1q_u8(std::bit_cast<uint8_t*>(buffer), shifted);
#elif ZMIJ_USE_SSE4_1
		__m128i shuffle = _mm_loadu_si128(std::bit_cast<const __m128i*>(static_data.shift_shuffle + (drop_leading_zero ? 1 : 0)));
		_mm_storeu_si128(std::bit_cast<__m128i*>(buffer), _mm_shuffle_epi8(digits, shuffle));
#endif
	}

	JSONIFIER_INLINE void write_digits(jsonifier::string_buffer_ptr buffer, uint64_t digits, bool drop_leading_zero) noexcept {
		memcpy(buffer, &digits, sizeof(digits));
		memmove(buffer, buffer + (drop_leading_zero ? 1 : 0), sizeof(digits));
	}

	JSONIFIER_INLINE auto write_exp_float_simd(jsonifier::string_buffer_ptr buffer, const dec_digits<32>& dig, int32_t last_digit, bool has_last_digit, bool has_extra_digit,
		[[maybe_unused]] uint64_t exp_data) noexcept -> jsonifier::string_buffer_ptr {
		uint32_t prefix				   = (static_cast<uint32_t>('.') << 8) + static_cast<uint32_t>('0') + static_cast<uint32_t>(last_digit);
		[[maybe_unused]] uint64_t tail = exp_data | (static_cast<uint64_t>(prefix) << 32);
		auto entry					   = static_data.exp_float_shuffles.get_entry(dig.num_digits, has_last_digit, has_extra_digit);
#if ZMIJ_USE_SSE4_1
		__m128i ascii	= _mm_or_si128(dig.unshuffled, jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&static_data.zeros));
		__m128i src		= _mm_insert_epi64(ascii, static_cast<int64_t>(tail), 1);
		__m128i shuffle = jsonifier::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(entry.shuffle);
		__m128i out		= _mm_shuffle_epi8(src, shuffle);
		_mm_storeu_si128(std::bit_cast<__m128i*>(buffer), out);
#elif ZMIJ_USE_NEON
		uint8x16_t ascii   = vorrq_u8(dig.unshuffled, vdupq_n_u8(static_cast<uint8_t>('0')));
		uint8x16_t src	   = vreinterpretq_u8_u64(vsetq_lane_u64(tail, vreinterpretq_u64_u8(ascii), 1));
		uint8x16_t shuffle = vld1q_u8(entry.shuffle);
		uint8x16_t out	   = vqtbl1q_u8(src, shuffle);
		vst1q_u8(std::bit_cast<uint8_t*>(buffer), out);
#endif
		return buffer + entry.length;
	}

	[[maybe_unused]] JSONIFIER_INLINE auto write_exp_float_simd(jsonifier::string_buffer_ptr, const dec_digits<64>&, int32_t, bool, bool, uint64_t) noexcept
		-> jsonifier::string_buffer_ptr {
		return nullptr;
	}

	struct to_decimal_result {
		long long sig;
		int32_t exp;
		int32_t last_digit		= 0;
		bool has_last_digit = false;
	};

	// Here be 🐉s.
	// Converts a binary FP number bin_sig * 2**bin_exp to the shortest decimal
	// representation, where bin_exp = raw_exp - exp_offset.
	template<typename Float, typename UInt> JSONIFIER_INLINE static auto to_decimal(UInt bin_sig, int64_t raw_exp, bool regular) noexcept -> to_decimal_result {
		using traits			  = float_traits<Float>;
		int64_t bin_exp			  = raw_exp - traits::exp_offset;
		constexpr int32_t num_bits	  = std::numeric_limits<UInt>::digits;
		constexpr int32_t extra_shift = exp_shift_table::extra_shift;

		if (!regular) [[ZMIJ_UNLIKELY]] {
			int32_t dec_exp			= compute_dec_exp(static_cast<int32_t>(bin_exp), false);
			uint8_t shift = static_cast<uint8_t>(compute_exp_shift(static_cast<int32_t>(bin_exp), dec_exp + 1) + extra_shift);
			uint128 pow10		= static_data.pow10_significands[-dec_exp - 1];
			uint128 p			= umul192_hi128(pow10.hi, pow10.lo, static_cast<uint64_t>(bin_sig) << shift);

			long long integral	= static_cast<long long>(p.hi >> extra_shift);
			uint64_t fractional = (p.hi << (64 - extra_shift)) | (p.lo >> extra_shift);

			uint64_t half_ulp = pow10.hi >> (extra_shift + 1 - static_cast<int32_t>(shift));
			bool round_up	  = half_ulp > ~uint64_t(0ULL) - fractional;
			bool round_down	  = (half_ulp >> 1) > fractional;
			integral += (round_up ? 1LL : 0LL);

			int32_t digit = static_cast<int32_t>(umul128_add_hi64(fractional, 10ULL, (1ULL << 63) - 1ULL));
			int32_t lo	  = static_cast<int32_t>(umul128_add_hi64(fractional - (half_ulp >> 1), 10ULL, ~uint64_t(0ULL)));
			if (digit < lo)
				digit = lo;
			return { integral, dec_exp, digit, (round_up || round_down) == false };
		}

		constexpr uint64_t log10_2_sig = 78'913ULL;
		constexpr int32_t log10_2_exp	   = 18;
		int32_t dec_exp =
			use_umul128_hi64 ? static_cast<int32_t>(umul128_hi64(static_cast<uint64_t>(bin_exp), log10_2_sig << (64 - log10_2_exp))) : compute_dec_exp(static_cast<int32_t>(bin_exp));
		ZMIJ_ASM(("" : "+r"(dec_exp)));
		uint8_t shift = static_cast<uint8_t>(exp_shift_table::enable ? static_data.exp_shifts.data[static_cast<std::size_t>(bin_exp + float_traits<double>::exp_offset)]
																		   : compute_exp_shift(static_cast<int32_t>(bin_exp), dec_exp + 1) + extra_shift);
		uint64_t even		= 1ULL - static_cast<uint64_t>(bin_sig & 1U);

		if constexpr (num_bits == 32) {
			constexpr int32_t extra_shift_32 = 34;
			shift						 = static_cast<uint8_t>(static_cast<int32_t>(shift) + (extra_shift_32 - exp_shift_table::extra_shift));
			uint64_t pow10_hi			 = static_data.pow10_significands[-dec_exp - 1].hi;
			uint64_t p					 = umul128_hi64(pow10_hi + 1ULL, static_cast<uint64_t>(bin_sig) << shift);

			long long integral	= static_cast<long long>(p >> extra_shift_32);
			uint64_t fractional = p & ((1ULL << extra_shift_32) - 1ULL);

			uint64_t half_ulp = (pow10_hi >> (65 - static_cast<int32_t>(shift))) + even;
			bool round_up	  = ((fractional + half_ulp) >> extra_shift_32) != 0ULL;
			bool round_down	  = half_ulp > fractional;
			integral += (round_up ? 1LL : 0LL);

			int32_t digit = static_cast<int32_t>((fractional * 10ULL + (1ULL << (extra_shift_32 - 1))) >> extra_shift_32);
			if (fractional == (1ULL << (extra_shift_32 - 2))) [[ZMIJ_UNLIKELY]]
				digit = 2;
			return { integral, dec_exp, digit, (round_up || round_down) == false };
		} else {
			uint128 pow10 = static_data.pow10_significands[-dec_exp - 1];
			uint128 p	  = umul192_hi128(pow10.hi, pow10.lo, static_cast<uint64_t>(bin_sig) << shift);

			long long integral	= static_cast<long long>(p.hi >> extra_shift);
			uint64_t fractional = (p.hi << (64 - extra_shift)) | (p.lo >> extra_shift);

			uint64_t half_ulp = (pow10.hi >> (extra_shift + 1 - static_cast<int32_t>(shift))) + even;
			bool round_up	  = fractional + half_ulp < fractional;
			bool round_down	  = half_ulp > fractional;
			integral += (round_up ? 1LL : 0LL);

			int32_t digit = static_cast<int32_t>(umul128_add_hi64(fractional, 10ULL, static_data.biased_half));
			if (fractional == (1ULL << 62)) [[ZMIJ_UNLIKELY]]
				digit = 2;
			return { integral, dec_exp, digit, (round_up || round_down) == false };
		}
	}

}// namespace

namespace zmij {

	[[maybe_unused]] JSONIFIER_INLINE static auto to_decimal(double value) noexcept -> dec_fp {
		using traits  = float_traits<double>;
		auto bits	  = traits::to_bits(value);
		auto bin_exp  = traits::get_exp(bits);
		auto bin_sig  = traits::get_sig(bits);
		auto negative = traits::is_negative(bits);
		if (bin_exp == 0 || bin_exp == traits::exp_mask) [[ZMIJ_UNLIKELY]] {
			if (bin_exp != 0)
				return { static_cast<int64_t>(bin_sig), static_cast<int32_t>(~0U >> 1U), negative };
			if (bin_sig == 0ULL)
				return { 0LL, 0, negative };
			bin_exp = 1;
			bin_sig |= traits::implicit_bit;
		}
		auto dec		= ::to_decimal<double>(bin_sig ^ traits::implicit_bit, bin_exp, bin_sig != 0ULL);
		auto last_digit = -static_cast<int32_t>(dec.has_last_digit) & dec.last_digit;
		return { dec.sig * 10LL + static_cast<long long>(last_digit), dec.exp, negative };
	}

	namespace detail {

		template<typename Float> JSONIFIER_INLINE jsonifier::string_buffer_ptr write(Float value, jsonifier::string_buffer_ptr buffer) noexcept {
			using traits = float_traits<Float>;
			auto bits	 = traits::to_bits(value);
			auto bin_exp = traits::get_exp(bits);
			auto bin_sig = traits::get_sig(bits);

			*buffer = '-';
			buffer += (traits::is_negative(bits) ? 1 : 0);

			const auto* d = &static_data;
			ZMIJ_ASM(("" : "+r"(d)));
			uint64_t threshold = traits::num_bits == 64 ? d->threshold : 10000000ULL;

			to_decimal_result dec;
			bool is_normal = static_cast<uint32_t>(bin_exp - 1LL) < static_cast<uint32_t>(traits::exp_mask - 1);
			if (!is_normal) [[ZMIJ_UNLIKELY]] {
				if (bin_exp != 0) {
					memcpy(buffer, bin_sig == 0ULL ? "inf" : "nan", 4);
					return buffer + 3;
				}
				if (bin_sig == 0ULL) {
					memcpy(buffer, "0", 2);
					return buffer + 1;
				}
				dec				  = ::to_decimal<Float>(bin_sig, 1LL, true);
				long long dec_sig = dec.sig * 10LL + static_cast<long long>(-static_cast<int32_t>(dec.has_last_digit) & dec.last_digit);
				int32_t dec_exp		  = dec.exp;
				while (dec_sig < static_cast<long long>(threshold)) {
					dec_sig *= 10LL;
					--dec_exp;
				}
				long long q	   = static_cast<long long>(::div10(static_cast<uint64_t>(dec_sig)));
				int32_t last_digit = static_cast<int32_t>(dec_sig - q * 10LL);
				dec			   = { q, dec_exp, last_digit, last_digit != 0 };
			} else {
				dec = ::to_decimal<Float>(bin_sig | traits::implicit_bit, bin_exp, bin_sig != 0ULL);
			}
			bool has_last_digit	 = dec.has_last_digit;
			bool has_extra_digit = dec.sig >= static_cast<long long>(threshold);
			int32_t dec_exp			 = dec.exp + traits::max_digits10 - 2 + (has_extra_digit ? 1 : 0);
			if constexpr (traits::num_bits == 32) {
				if (dec.sig < static_cast<long long>(1e6)) [[ZMIJ_UNLIKELY]] {
					dec.sig		   = 10LL * dec.sig + static_cast<long long>(-static_cast<int32_t>(has_last_digit) & dec.last_digit);
					has_last_digit = false;
					--dec_exp;
				}
			}

			jsonifier::string_buffer_ptr start = buffer;
			auto dig						   = to_digits<traits::num_bits>(static_cast<uint64_t>(dec.sig));
			constexpr int32_t bcd_size			   = traits::num_bits == 64 ? 16 : 8;
			if (dec_exp >= traits::min_fixed_dec_exp && dec_exp <= traits::max_fixed_dec_exp) {
				memcpy(start, &zeros, 8);
				char last_digit_char = static_cast<char>(static_cast<int32_t>('0') + (-static_cast<int32_t>(has_last_digit) & dec.last_digit));
				int32_t num_digits		 = has_last_digit ? bcd_size : dig.num_digits - 1;

				const auto* fixed_layouts = &d->fixed_layouts;
				if constexpr (ZMIJ_AARCH64)
					ZMIJ_ASM(("" : "+r"(fixed_layouts)));

				const auto& layout = fixed_layouts->get(dec_exp);
				buffer += layout.start_pos;
				write_digits(buffer, dig.digits, !has_extra_digit);
				buffer[bcd_size + (has_extra_digit ? 1 : 0) - 1] = last_digit_char;
				uint32_t point_pos								 = layout.point_pos;
				memmove(start + layout.shift_pos, start + point_pos, bcd_size);
				start[point_pos] = '.';
				return buffer + layout.end_pos[static_cast<std::size_t>(num_digits + (has_extra_digit ? 1 : 0) - 1)];
			}
			if constexpr (traits::num_bits == 32 && exp_float_shuffle_table::enable) {
				uint64_t exp_data = d->exp_strings.data[static_cast<std::size_t>(dec_exp + exp_string_table::offset)];
				return write_exp_float_simd(buffer, dig, dec.last_digit, has_last_digit, has_extra_digit, exp_data);
			} else {
				buffer += (has_extra_digit ? 1 : 0);
				memcpy(buffer, &dig.digits, bcd_size);
				buffer[bcd_size] = static_cast<char>(static_cast<int32_t>('0') + dec.last_digit);
				buffer += select(has_last_digit ? 1ULL : 0ULL, bcd_size + 1, dig.num_digits);
				start[0] = start[1];
				start[1] = '.';
				buffer -= (buffer - 1 == start + 1 ? 1 : 0);

				if constexpr (exp_string_table::enable) {
					uint64_t exp_data = d->exp_strings.data[static_cast<std::size_t>(dec_exp + exp_string_table::offset)];
					int32_t len			  = static_cast<int32_t>(exp_data >> 48);
					if constexpr (is_big_endian)
						exp_data = bswap64(exp_data);
					memcpy(buffer, &exp_data, traits::max_exponent10 >= 100 ? 8ULL : 4ULL);
					return buffer + len;
				} else {
					uint16_t e_sign =
						static_cast<uint16_t>(dec_exp >= 0 ? (static_cast<int32_t>('+') << 8 | static_cast<int32_t>('e')) : (static_cast<int32_t>('-') << 8 | static_cast<int32_t>('e')));
					if constexpr (is_big_endian) {
						e_sign = static_cast<uint16_t>((static_cast<int32_t>(e_sign) << 8) | (static_cast<int32_t>(e_sign) >> 8));
					} else {
						memcpy(buffer, &e_sign, 2);
						buffer += 2;
						dec_exp = dec_exp >= 0 ? dec_exp : -dec_exp;
						if constexpr (traits::max_exponent10 >= 100) {
							uint32_t digit = use_umul128_hi64 ? static_cast<uint32_t>(umul128_hi64(static_cast<uint64_t>(dec_exp), 0x290000000000000ULL))
															  : static_cast<uint32_t>((static_cast<uint32_t>(dec_exp) * div100_sig) >> div100_exp);
							*buffer		   = static_cast<char>(static_cast<uint32_t>('0') + digit);
							buffer += (dec_exp >= 100 ? 1 : 0);
							dec_exp -= static_cast<int32_t>(digit * 100U);
						} else {
							memcpy(buffer, digits2(static_cast<uint64_t>(dec_exp)), 2);
							return buffer + 2;
						}
					}
				}
			}
		}

	}// namespace detail
}// namespace zmij
