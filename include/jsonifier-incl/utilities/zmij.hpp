/*
 * SPDX-License-Identifier: MIT AND (MIT OR BSL-1.0)
 * Copyright (c) 2026 Nihilai Collective Corp
 * Portions Copyright (c) 2025 - present, Victor Zverovich (modified for jsonifier)
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/zmij.hpp
 */
#pragma once

// Modified from zmij, a double-to-string conversion algorithm based on Schubfach and xjb.
// Copyright (c) 2025 - present, Victor Zverovich
// Distributed under the MIT license (see LICENSE) or alternatively
// the Boost Software License, Version 1.0.
// https://github.com/vitaut/zmij/

#include <jsonifier-incl/core/config.hpp>

namespace jsonifier::zmij {
	struct dec_fp {
		int64_t sig;
		bool negative;
		int32_t exp;
	};
}

#include <jsonifier-incl/simd/simd_config.hpp>
#include <jsonifier-incl/simd/avx.hpp>

namespace jsonifier::zmij {
	namespace impl {

		constexpr auto isConstantEvaluated() noexcept -> bool {
#ifdef __cpp_lib_is_constant_evaluated
			return std::is_constant_evaluated();
#else
			return false;
#endif
		}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		inline constexpr bool isBigEndian = true;
#else
		inline constexpr bool isBigEndian = false;
#endif

		JSONIFIER_INLINE auto bswap64(uint64_t x) noexcept -> uint64_t {
#if ZMIJ_HAS_BUILTIN(__builtin_bswap64)
			return __builtin_bswap64(x);
#elif JSONIFIER_COMPILER_MSVC
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

		JSONIFIER_INLINE auto select(uint64_t condition, int64_t trueValue, int64_t falseValue) noexcept -> int64_t {
			if constexpr (!(JSONIFIER_ARCH_X64 && JSONIFIER_COMPILER_GCC)) {
				return (condition != 0) ? trueValue : falseValue;
			} else {
				ZMIJ_ASM(volatile("test %2, %2\n\t"
								  "cmovne %1, %0\n\t" : "+r"(falseValue) : "r"(trueValue),
					"r"(condition) : "cc"));
				return falseValue;
			}
		}

		struct uint128 {
			template<uint64_t shift> [[maybe_unused]] JSONIFIER_INLINE constexpr auto operator>>(jsonifier::internal::integral_constant<shift>) const noexcept -> uint128 {
				if constexpr (shift == 32) {
					return { hi >> 32, (hi << 32) | (lo >> 32) };
				} else {
					static_assert(shift >= 64 && shift < 128);
					return { 0, hi >> (shift - 64) };
				}
			}

			[[maybe_unused]] JSONIFIER_INLINE explicit constexpr operator uint64_t() const noexcept {
				return lo;
			}

			uint64_t hi;
			uint64_t lo;
		};

#if ZMIJ_USE_INT128
		using uint128_t = __uint128_t;
#else
		using uint128_t = uint128;
#endif

#if ZMIJ_USE_INT128 && JSONIFIER_PLATFORM_MAC
		inline constexpr bool useUmul128Hi64 = true;
#else
		inline constexpr bool useUmul128Hi64 = false;
#endif

		JSONIFIER_INLINE constexpr auto umul128(uint64_t x, uint64_t y) noexcept -> uint128_t {
#if ZMIJ_USE_INT128
			return uint128_t(x) * y;
#else
			if (!isConstantEvaluated()) {
	#if JSONIFIER_PLATFORM_WINDOWS && JSONIFIER_ARCH_X64 && defined(__cpp_lib_is_constant_evaluated)
				uint64_t hi = 0;
				uint64_t lo = _umul128(x, y, &hi);
				return { hi, lo };
	#elif JSONIFIER_PLATFORM_WINDOWS && JSONIFIER_ARCH_ARM64 && defined(__cpp_lib_is_constant_evaluated)
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

		JSONIFIER_INLINE constexpr auto umul128Hi64(uint64_t x, uint64_t y) noexcept -> uint64_t {
#if ZMIJ_USE_INT128
			return static_cast<uint64_t>(umul128(x, y) >> 64);
#else
			return umul128(x, y).hi;
#endif
		}

		JSONIFIER_INLINE auto umul128AddHi64(uint64_t x, uint64_t y, uint64_t c) noexcept -> uint64_t {
#if ZMIJ_USE_INT128
			return static_cast<uint64_t>((uint128_t(x) * y + c) >> 64);
#else
			auto p = umul128(x, y);
			return p.hi + (p.lo + c < p.lo ? 1ULL : 0ULL);
#endif
		}

		JSONIFIER_INLINE auto umul192Hi128(uint64_t xHi, uint64_t xLo, uint64_t y) noexcept -> uint128 {
#if ZMIJ_USE_INT128
			uint128_t p = umul128(xHi, y);
			uint64_t lo = static_cast<uint64_t>(p) + static_cast<uint64_t>(umul128(xLo, y) >> 64);
			return { static_cast<uint64_t>(p >> 64) + (lo < static_cast<uint64_t>(p) ? 1ULL : 0ULL), lo };
#else
			uint128 p	= umul128(xHi, y);
			uint64_t lo = p.lo + umul128(xLo, y).hi;
			return { p.hi + (lo < p.lo ? 1ULL : 0ULL), lo };
#endif
		}

		JSONIFIER_INLINE auto div10(uint64_t x) noexcept -> uint64_t {
			constexpr uint64_t div10Sig64 = (1ull << 63) / 5ULL + 1ULL;
			return ZMIJ_USE_INT128 ? umul128Hi64(x, div10Sig64) : x / 10ULL;
		}

		JSONIFIER_INLINE constexpr auto computeDecExp(int32_t binExp, bool regular = true) noexcept -> int32_t {
			constexpr int32_t log103Over4Sig = 131'072;
			constexpr int32_t log102Sig = 315'653, log102Exp = 20;
			return (binExp * log102Sig - (regular ? 0 : log103Over4Sig)) >> log102Exp;
		}

		template<typename float_type> struct float_traits : std::numeric_limits<float_type> {
			static constexpr int32_t numBits		= float_traits::digits == 53 ? 64 : 32;
			static constexpr int32_t numSigBits		= float_traits::digits - 1;
			static constexpr int32_t numExpBits		= numBits - numSigBits - 1;
			static constexpr int32_t expMask		= static_cast<int32_t>((1U << numExpBits) - 1U);
			static constexpr int32_t expBias		= static_cast<int32_t>((1U << (numExpBits - 1)) - 1U);
			static constexpr int32_t expOffset		= expBias + numSigBits;
			static constexpr int32_t minFixedDecExp = -4;
			static constexpr int32_t maxFixedDecExp = computeDecExp(float_traits::digits + 1) - 1;
			using sig_type							= std::conditional_t<numBits == 64, uint64_t, uint32_t>;
			static constexpr sig_type implicitBit	= static_cast<sig_type>(1) << numSigBits;

			JSONIFIER_INLINE static auto toBits(float_type value) noexcept -> sig_type {
				sig_type bits;
				jsonifier::pow2MemcpyWrapper<sizeof(value)>(&bits, &value);
				return bits;
			}

			JSONIFIER_INLINE static auto getExp(sig_type bits) noexcept -> int64_t {
				return static_cast<int64_t>((bits << 1U) >> (numSigBits + 1));
			}

			JSONIFIER_INLINE static auto isNegative(sig_type bits) noexcept -> bool {
				return (bits >> (numBits - 1)) != 0;
			}

			JSONIFIER_INLINE static auto getSig(sig_type bits) noexcept -> sig_type {
				return bits & (implicitBit - 1U);
			}

			static_assert(float_traits::is_iec559, "IEEE 754 required");
		};

		inline constexpr uint64_t pow10Minor[]{ 0x8000000000000000, 0xa000000000000000, 0xc800000000000000, 0xfa00000000000000, 0x9c40000000000000, 0xc350000000000000,
			0xf424000000000000, 0x9896800000000000, 0xbebc200000000000, 0xee6b280000000000, 0x9502f90000000000, 0xba43b74000000000, 0xe8d4a51000000000, 0x9184e72a00000000,
			0xb5e620f480000000, 0xe35fa931a0000000, 0x8e1bc9bf04000000, 0xb1a2bc2ec5000000, 0xde0b6b3a76400000, 0x8ac7230489e80000, 0xad78ebc5ac620000, 0xd8d726b7177a8000,
			0x878678326eac9000, 0xa968163f0a57b400, 0xd3c21bcecceda100, 0x84595161401484a0, 0xa56fa5b99019a5c8, 0xcecb8f27f4200f3a };

		inline constexpr uint128 pow10Major[]{ { 0xaf8e5410288e1b6f, 0x07ecf0ae5ee44dda }, { 0xb1442798f49ffb4a, 0x99cd11cfdf41779d }, { 0xb2fe3f0b8599ef07, 0x861fa7e6dcb4aa15 },
			{ 0xb4bca50b065abe63, 0x0fed077a756b53aa }, { 0xb67f6455292cbf08, 0x1a3bc84c17b1d543 }, { 0xb84687c269ef3bfb, 0x3d5d514f40eea742 },
			{ 0xba121a4650e4ddeb, 0x92f34d62616ce413 }, { 0xbbe226efb628afea, 0x890489f70a55368c }, { 0xbdb6b8e905cb600f, 0x5400e987bbc1c921 },
			{ 0xbf8fdb78849a5f96, 0xde98520472bdd034 }, { 0xc16d9a0095928a27, 0x75b7053c0f178294 }, { 0xc350000000000000, 0x0000000000000000 },
			{ 0xc5371912364ce305, 0x6c28000000000000 }, { 0xc722f0ef9d80aad6, 0x424d3ad2b7b97ef6 }, { 0xc913936dd571c84c, 0x03bc3a19cd1e38ea },
			{ 0xcb090c8001ab551c, 0x5cadf5bfd3072cc6 }, { 0xcd036837130890a1, 0x36dba887c37a8c10 }, { 0xcf02b2c21207ef2e, 0x94f967e45e03f4bc },
			{ 0xd106f86e69d785c7, 0xe13336d701beba52 }, { 0xd31045a8341ca07c, 0x1ede48111209a051 }, { 0xd51ea6fa85785631, 0x552a74227f3ea566 },
			{ 0xd732290fbacaf133, 0xa97c177947ad4096 }, { 0xd94ad8b1c7380874, 0x18375281ae7822bc } };

		inline constexpr uint32_t pow10Fixups[]{ 0x0a4e363f, 0x00001840, 0x00006400, 0x24200040, 0x00000000, 0x0c000000, 0x82c81380, 0x5e4ce01f, 0xd730f60f, 0x0000001b, 0x00000000,
			0xcdf7fffc, 0x6e8201d8, 0x40cd3fd1, 0xdb642501, 0x00000d0d, 0x14042400, 0x53713840, 0x11781db4, 0x00000000 };

		struct alignas(simdBytesPerRegister) pow10_significand_table {
			static constexpr bool compress	   = ZMIJ_OPTIMIZE_SIZE != 0;
			static constexpr bool splitTables  = !compress && JSONIFIER_ARCH_ARM64 != 0;
			static constexpr int32_t numPow10s = 618;
			uint64_t data[compress ? 1 : numPow10s * 2]{};

			JSONIFIER_INLINE static constexpr auto compute(uint32_t i) noexcept -> uint128 {
				constexpr uint32_t stride = sizeof(pow10Minor) / sizeof(*pow10Minor);
				auto m					  = pow10Minor[(i + 10U) % stride];
				auto h					  = pow10Major[(i + 10U) / stride];

				uint64_t h1 = umul128Hi64(h.lo, m);

				uint64_t c0 = h.lo * m;
				uint64_t c1 = h1 + h.hi * m;
				uint64_t c2 = (c1 < h1 ? 1ULL : 0ULL) + umul128Hi64(h.hi, m);

				uint128 result = (c2 >> 63) != 0ULL ? uint128{ c2, c1 } : uint128{ (c2 << 1) | (c1 >> 63), (c1 << 1) | (c0 >> 63) };
				result.lo -= (pow10Fixups[i >> 5] >> (i & 31U)) & 1ULL;
				return result;
			}

			consteval pow10_significand_table() {
				for (int32_t i = 0; i < numPow10s && !compress; ++i) {
					uint128 result = compute(static_cast<uint32_t>(i));
					if constexpr (splitTables) {
						data[numPow10s - i - 1]		= result.hi;
						data[numPow10s * 2 - i - 1] = result.lo;
					} else {
						data[i * 2]		= result.hi;
						data[i * 2 + 1] = result.lo;
					}
				}
			}

			JSONIFIER_INLINE constexpr auto operator[](int32_t decExp) const noexcept -> uint128 {
				constexpr int32_t decExpMin = -293;
				int32_t i					= decExp - decExpMin;
				if constexpr (compress) {
					return compute(static_cast<uint32_t>(i));
				} else {
					if constexpr (!splitTables) {
						const uint64_t* p = data + i * 2;
						return { p[0], p[1] };
					} else {
						const uint64_t* p = data + numPow10s + decExpMin;
						if (!isConstantEvaluated()) {
							ZMIJ_ASM(("" : "+r"(p)));
							return { p[~decExp], p[~decExp + numPow10s] };
						} else {
							return { p[~decExp], p[~decExp + numPow10s] };
						}
					}
				}
			}
		};

		JSONIFIER_INLINE constexpr auto computeExpShift(int32_t binExp, int32_t decExp) noexcept -> uint8_t {
			constexpr int32_t log2Pow10Sig = 217'707, log2Pow10Exp = 16;
			int32_t pow10BinExp = (-decExp * log2Pow10Sig) >> log2Pow10Exp;
			return static_cast<uint8_t>(binExp + pow10BinExp + 1);
		}

		struct exp_shift_table {
			static constexpr bool enable		= ZMIJ_OPTIMIZE_SIZE == 0;
			static constexpr int32_t extraShift = 6;

			consteval exp_shift_table() {
				for (int32_t rawExp = 0; rawExp < static_cast<int32_t>(sizeof(data)) && enable; ++rawExp) {
					int32_t binExp = rawExp - float_traits<double>::expOffset;
					if (rawExp == 0) {
						++binExp;
					}
					int32_t decExp = computeDecExp(binExp);
					data[rawExp]   = static_cast<uint8_t>(computeExpShift(binExp, decExp + 1) + extraShift);
				}
			}

			uint8_t data[enable ? float_traits<double>::expMask + 1 : 1]{};
		};

		struct alignas(simdBytesPerRegister) exp_string_table {
			static constexpr bool enable	   = ZMIJ_USE_EXP_STRING_TABLE;
			using traits					   = float_traits<double>;
			static constexpr int32_t minDecExp = traits::min_exponent10 - traits::max_digits10;
			static constexpr int32_t offset	   = -minDecExp;
			uint64_t data[enable ? traits::max_exponent10 - minDecExp + 1 : 1]{};

			consteval exp_string_table() {
				for (int32_t e = minDecExp; e <= traits::max_exponent10 && enable; ++e) {
					uint64_t absE = e >= 0 ? static_cast<uint64_t>(e) : static_cast<uint64_t>(-e);
					uint64_t bc	  = absE % 100ULL;
					uint64_t val  = ((bc % 10ULL + static_cast<uint64_t>('0')) << 8) | (bc / 10ULL + static_cast<uint64_t>('0'));
					if (uint64_t a = absE / 100ULL) {
						val = (val << 8) | (a + static_cast<uint64_t>('0'));
					}
					uint64_t len	 = 4ULL + (absE >= 100ULL ? 1ULL : 0ULL);
					data[e + offset] = (len << 48) | (val << 16) | (static_cast<uint64_t>(e >= 0 ? '+' : '-') << 8) | static_cast<uint64_t>('e');
				}
			}
		};

		struct exp_float_shuffle_table {
			static constexpr bool enable		  = (JSONIFIER_ANY_SIMD != 0) && exp_string_table::enable;
			static constexpr uint8_t expPos		  = 8;
			static constexpr uint8_t lastDigitPos = 12;
			static constexpr uint8_t pointPos	  = 13;

			struct entry {
				jsonifier::read_buffer_ptr shuffle;
				uint8_t length;
			};

			consteval exp_float_shuffle_table() {
				for (int32_t idx = 0; idx < 32 && enable; ++idx) {
					int32_t numDigits  = (idx >> 2) + 1;
					bool hasLastDigit  = ((idx >> 1) & 1) != 0;
					bool hasExtraDigit = (idx & 1) != 0;

					uint8_t* out = &data[idx * 16];
					for (int32_t i = 0; i < 16; ++i) {
						out[i] = 0x80;
					}
					uint8_t leadingDigitPos = hasExtraDigit ? 7U : 6U;
					uint8_t length			= 0;
					if (hasLastDigit) {
						out[length++] = leadingDigitPos;
						out[length++] = pointPos;
						for (int32_t i = static_cast<int32_t>(leadingDigitPos) - 1; i >= 0; --i) {
							out[length++] = static_cast<uint8_t>(i);
						}
						out[length++] = lastDigitPos;
					} else {
						length = static_cast<uint8_t>(numDigits + (hasExtraDigit ? 1 : 0));
						if (length == 2) {
							length = 1;
						}
						out[0] = leadingDigitPos;
						out[1] = pointPos;
						for (int32_t i = 2; i < static_cast<int32_t>(length); ++i) {
							out[i] = static_cast<uint8_t>(static_cast<int32_t>(leadingDigitPos) + 1 - i);
						}
					}
					for (uint8_t i = 0; i < 4U; ++i) {
						out[length++] = static_cast<uint8_t>(static_cast<int32_t>(expPos) + static_cast<int32_t>(i));
					}
					out[15] = length;
				}
			}

			JSONIFIER_INLINE constexpr auto getEntry(int32_t numDigits, bool hasLastDigit, bool hasExtraDigit) const noexcept {
				int32_t idx = (numDigits - 1) * 4 + (hasLastDigit ? 2 : 0) + (hasExtraDigit ? 1 : 0);
				return entry{ &data[idx * 16], data[idx * 16 + 15] };
			}

			alignas(simdBytesPerRegister) uint8_t data[enable ? 32 * 16 : 1]{};
		};

		struct fixed_layout_table {
			using traits						= float_traits<double>;
			static constexpr int32_t numEntries = traits::maxFixedDecExp - traits::minFixedDecExp + 1;

#if JSONIFIER_ANY_AVX
			static constexpr uint64_t entryAlignment = 64;
#else
			static constexpr uint64_t entryAlignment = JSONIFIER_ARCH_ARM64 && !ZMIJ_OPTIMIZE_SIZE ? 32 : 1;
#endif

			struct alignas(entryAlignment) entry {
#if JSONIFIER_ANY_AVX
				alignas(16) uint8_t shuffle[2][16];
#endif
				uint8_t startPos;
				uint8_t pointPos;

				uint8_t shiftPos;

#if JSONIFIER_ANY_AVX
				uint8_t lastDigitPos[2];
#endif

				uint8_t endPos[traits::max_digits10];
			};

			consteval fixed_layout_table() {
				for (int32_t decExp = traits::minFixedDecExp; decExp <= traits::maxFixedDecExp; ++decExp) {
					auto& e = data[decExp - traits::minFixedDecExp];

					e.startPos = decExp < 0 ? static_cast<uint8_t>(1 - decExp) : static_cast<uint8_t>(0);
					e.pointPos = decExp >= 0 ? static_cast<uint8_t>(1 + decExp) : static_cast<uint8_t>(1);
					e.shiftPos = static_cast<uint8_t>(static_cast<int32_t>(e.pointPos) + (decExp >= 0 ? 1 : 0));

#if JSONIFIER_ANY_AVX
					constexpr int32_t bcdSize = 16;
					for (int32_t extra = 0; extra < 2; ++extra) {
						int32_t len			  = bcdSize + extra - 1;
						e.lastDigitPos[extra] = static_cast<uint8_t>(len + ((decExp >= 0 && decExp < len) ? 1 : 0));
					}

					int32_t pointSlot = (decExp >= 0 && decExp <= 14) ? 1 + decExp : 128;
					for (int32_t extra = 0; extra < 2; ++extra) {
						uint8_t bcdIdx = extra == 0 ? 1 : 0;
						for (int32_t i = 0; i < bcdSize; ++i) {
							e.shuffle[extra][i] = i == pointSlot ? static_cast<uint8_t>(0xFF) : bcdIdx++;
						}
					}
#endif

					for (int32_t n = 1; n <= traits::max_digits10; ++n) {
						int32_t endPos = n;
						if (decExp >= 0) {
							endPos = n > decExp + 1 ? n + 1 : decExp + 1;
						}
						e.endPos[n - 1] = static_cast<uint8_t>(endPos);
					}
				}
			}

			[[maybe_unused]] JSONIFIER_INLINE constexpr auto get(int32_t decExp) const noexcept -> const entry& {
				constexpr auto min = traits::minFixedDecExp;
				return data[static_cast<uint64_t>(decExp - min)];
			}

			entry data[numEntries]{};
		};

		[[maybe_unused]] JSONIFIER_INLINE auto countTrailingNonzeros(uint64_t x) noexcept -> int32_t {
			if constexpr (isBigEndian) {
				x = bswap64(x);
			}
			return static_cast<int32_t>((70ULL - static_cast<uint64_t>(clz((x << 1U) | 1ULL))) / 8ULL);
		}

		[[maybe_unused]] JSONIFIER_INLINE auto digits2(uint64_t value) noexcept -> jsonifier::internal::unrestrict_t<jsonifier::read_buffer_ptr> {
			static const char data[] = "0001020304050607080910111213141516171819"
									   "2021222324252627282930313233343536373839"
									   "4041424344454647484950515253545556575859"
									   "6061626364656667686970717273747576777879"
									   "8081828384858687888990919293949596979899";
			return std::bit_cast<jsonifier::read_buffer_ptr>(&data[value * 2ULL]);
		}

		inline constexpr int32_t div10kExp	= 40;
		inline constexpr uint32_t div10kSig = static_cast<uint32_t>((1ULL << div10kExp) / 10000ULL + 1ULL);
		inline constexpr uint32_t neg10k	= static_cast<uint32_t>((1ULL << 32) - 10000ULL);
		inline constexpr int32_t div100Exp	= 19;
		inline constexpr uint32_t div100Sig = (1U << div100Exp) / 100U + 1U;
		inline constexpr uint32_t neg100	= (1U << 16) - 100U;
		inline constexpr int32_t div10Exp	= 10;
		inline constexpr uint32_t div10Sig	= (1U << div10Exp) / 10U + 1U;
		inline constexpr uint32_t neg10		= (1U << 8) - 10U;

		inline constexpr uint64_t zeros = 0x0101010101010101ULL * static_cast<uint64_t>('0');

		[[maybe_unused]] JSONIFIER_INLINE auto writeIf(jsonifier::write_buffer_ptr buffer, uint32_t digit, bool condition) noexcept
			-> jsonifier::internal::unrestrict_t<jsonifier::write_buffer_ptr> {
			*buffer = static_cast<uint8_t>(static_cast<uint32_t>('0') + digit);
			return buffer + (condition ? 1 : 0);
		}

		consteval auto splat64(uint64_t x) -> uint128 {
			return { x, x };
		}
		consteval auto splat32(uint32_t x) -> uint128 {
			return splat64((static_cast<uint64_t>(x) << 32) | static_cast<uint64_t>(x));
		}
		consteval auto splat16(uint16_t x) -> uint128 {
			return splat32((static_cast<uint32_t>(x) << 16) | static_cast<uint32_t>(x));
		}
		consteval auto pack8(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, uint8_t g, uint8_t h) -> uint64_t {
			using u64 = uint64_t;
			return (u64(h) << 56) | (u64(g) << 48) | (u64(f) << 40) | (u64(e) << 32) | (u64(d) << 24) | (u64(c) << 16) | (u64(b) << 8) | u64(a);
		}

		struct alignas(simdBytesPerRegister) data {
			static constexpr uint64_t threshold = 1000000000000000ULL;

			static constexpr uint64_t biasedHalf = (1ULL << 63) + 6ULL;

#if JSONIFIER_NEON || JSONIFIER_SVE2
			static constexpr int32_t neg10k = 0x10000 - 10000;

			using int32x4 = std::conditional_t<JSONIFIER_COMPILER_MSVC, int32_t[4], int32x4_t>;
			using int16x8 = std::conditional_t<JSONIFIER_COMPILER_MSVC, int16_t[8], int16x8_t>;

			static constexpr uint64_t mulConst		 = 0xabcc77118461cefdULL;
			static constexpr uint64_t hundredMillion = 100000000ULL;
			static constexpr int32x4 multipliers32{ static_cast<int32_t>(div10kSig), neg10k, static_cast<int32_t>(div100Sig << 12), static_cast<int32_t>(neg100) };
			static constexpr int16x8 multipliers16{ 0xce0, static_cast<int16_t>(neg10) };
#elif JSONIFIER_ANY_AVX
			static constexpr uint128 div100 = splat32(div100Sig);
			static constexpr uint128 div10	= splat16(static_cast<uint16_t>(((1U << 16) / 10U) + 1U));
			static constexpr uint128 neg100 = splat32(jsonifier::zmij::impl::neg100);
			static constexpr uint128 neg10	= splat16(static_cast<uint16_t>((1U << 8) - 10U));
			static constexpr uint128 bswap	= uint128{ pack8(15, 14, 13, 12, 11, 10, 9, 8), pack8(7, 6, 5, 4, 3, 2, 1, 0) };
			static constexpr uint128 div10k = splat64(div10kSig);
			static constexpr uint128 neg10k = splat64(jsonifier::zmij::impl::neg10k);
			static constexpr uint128 zeros	= splat64(jsonifier::zmij::impl::zeros);
#endif

			static constexpr exp_shift_table expShifts{};
			static constexpr exp_string_table expStrings{};
			static constexpr pow10_significand_table pow10Significands{};
			static constexpr fixed_layout_table fixedLayouts{};
			static constexpr exp_float_shuffle_table expFloatShuffles{};

			static constexpr uint8_t shiftShuffle[17]{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0 };
		};
		alignas(simdBytesPerRegister) constexpr data staticData{};

#if JSONIFIER_NEON || JSONIFIER_SVE2

		JSONIFIER_INLINE auto toBcd4x4(int32x4_t efghAbcdMnopIjkl) noexcept -> uint8x16_t {
			ZMIJ_ASM(("" : "+w"(efghAbcdMnopIjkl)));

			int32x4_t efAbMnIj		   = vqdmulhq_n_s32(efghAbcdMnopIjkl, staticData.multipliers32[2]);
			int16x8_t ghEfCdAbOpMnKlIj = vreinterpretq_s16_s32(vmlaq_n_s32(efghAbcdMnopIjkl, efAbMnIj, staticData.multipliers32[3]));
			int16x8_t high10s		   = vqdmulhq_n_s16(ghEfCdAbOpMnKlIj, staticData.multipliers16[0]);
			return vreinterpretq_u8_s16(vmlaq_n_s16(ghEfCdAbOpMnKlIj, high10s, staticData.multipliers16[1]));
		}

		JSONIFIER_INLINE auto toUnshuffledDigits(uint64_t value) noexcept -> uint8x16_t {
			uint64_t hundredMillion = staticData.hundredMillion;

			ZMIJ_ASM(("" : "+r"(hundredMillion)));

			uint64_t abcdefgh = static_cast<uint64_t>(umul128(value, staticData.mulConst) >> 90);
			uint64_t ijklmnop = value - abcdefgh * hundredMillion;

			uint64x1_t ijklmnopAbcdefgh64{ (ijklmnop << 32) | abcdefgh };
			int32x2_t abcdefghIjklmnop = vreinterpret_s32_u64(ijklmnopAbcdefgh64);

			int32x2_t abcdIjkl			 = vreinterpret_s32_u32(vshr_n_u32(vreinterpret_u32_s32(vqdmulh_n_s32(abcdefghIjklmnop, staticData.multipliers32[0])), 9U));
			int32x2_t efghAbcdMnopIjkl32 = vmla_n_s32(abcdefghIjklmnop, abcdIjkl, staticData.multipliers32[1]);

			int32x4_t efghAbcdMnopIjkl = vreinterpretq_s32_u32(vshll_n_u16(vreinterpret_u16_s32(efghAbcdMnopIjkl32), 0U));
			return toBcd4x4(efghAbcdMnopIjkl);
		}

#elif JSONIFIER_ANY_AVX

		JSONIFIER_INLINE auto toBcd4x4(__m128i y) noexcept -> __m128i {
			const __m128i div100Local = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.div100);
			const __m128i div10Local  = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.div10);
			const __m128i neg100Local = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.neg100);
			const __m128i neg10Local  = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.neg10);

			__m128i z = _mm_add_epi64(y, _mm_mullo_epi32(neg100Local, _mm_srli_epi32(_mm_mulhi_epu16(y, div100Local), 3)));
			return _mm_add_epi16(z, _mm_mullo_epi16(neg10Local, _mm_mulhi_epu16(z, div10Local)));
		}

#endif

		struct bcd_result {
			uint64_t bcd;
			int32_t len;
		};

		[[maybe_unused]] JSONIFIER_INLINE auto toBcd8(uint64_t abcdefgh) noexcept -> bcd_result {
			if constexpr (!JSONIFIER_ANY_SIMD) {
				uint64_t abcdEfgh = abcdefgh + neg10k * ((abcdefgh * div10kSig) >> div10kExp);
				uint64_t abCdEfGh = abcdEfgh + neg100 * (((abcdEfgh * div100Sig) >> div100Exp) & 0x7f0000007fULL);
				uint64_t aBCDEFGH = abCdEfGh + neg10 * (((abCdEfGh * div10Sig) >> div10Exp) & 0xf000f000f000fULL);
				uint64_t bcd	  = isBigEndian ? aBCDEFGH : bswap64(aBCDEFGH);
				return { bcd, countTrailingNonzeros(bcd) };
			} else {
				[[maybe_unused]] const auto* d = &staticData;
				ZMIJ_ASM(("" : "+r"(d)));

#if JSONIFIER_NEON || JSONIFIER_SVE2
				uint64_t abcdEfgh64	 = abcdefgh + neg10k * ((abcdefgh * div10kSig) >> div10kExp);
				int32x4_t abcdEfgh	 = vcombine_s32(vreinterpret_s32_u64(vcreate_u64(abcdEfgh64)), vdup_n_s32(0));
				uint8x16_t digits128 = toBcd4x4(abcdEfgh);
				uint8x8_t digits	 = vget_low_u8(digits128);
				uint64_t bcd		 = vget_lane_u64(vreinterpret_u64_u8(vrev64_u8(digits)), 0);
				return { bcd, countTrailingNonzeros(bcd) };
#elif JSONIFIER_ANY_AVX
				uint64_t abcdEfgh	   = abcdefgh + neg10k * ((abcdefgh * div10kSig) >> div10kExp);
				uint64_t unshuffledBcd = static_cast<uint64_t>(_mm_cvtsi128_si64(toBcd4x4(_mm_set_epi64x(0, static_cast<int64_t>(abcdEfgh)))));
				int32_t len			   = unshuffledBcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffledBcd)) / 8 : 0;
				return { bswap64(unshuffledBcd), len };
#endif
			}
		}

		template<int32_t numBits> struct dec_digits {
			uint64_t digits;
#if JSONIFIER_NEON || JSONIFIER_SVE2
			uint8x16_t unshuffled;
#elif JSONIFIER_ANY_AVX
			__m128i unshuffled;
#endif
			int32_t numDigits;
		};

		template<> struct dec_digits<64> {
#if JSONIFIER_NEON || JSONIFIER_SVE2
			using digits_type = uint16x8_t;
#elif JSONIFIER_ANY_AVX
			using digits_type = __m128i;
#else
			using digits_type = uint128;
#endif
			digits_type digits;
			int32_t numDigits;
		};

		template<int32_t numBits> JSONIFIER_INLINE auto toDigits(uint64_t value) noexcept -> dec_digits<numBits> {
#if !JSONIFIER_ANY_SIMD
			uint32_t hi = static_cast<uint32_t>(value / 100'000'000ULL);
			uint32_t lo = static_cast<uint32_t>(value % 100'000'000ULL);
			auto hiBcd	= toBcd8(hi);
			if (lo == 0U) {
				return { { hiBcd.bcd + zeros, zeros }, hiBcd.len };
			}
			auto loBcd = toBcd8(lo);
			return { { hiBcd.bcd + zeros, loBcd.bcd + zeros }, 8 + loBcd.len };
#elif JSONIFIER_NEON || JSONIFIER_SVE2
			auto unshuffledDigits = toUnshuffledDigits(value);
			uint8x16_t digits	  = vrev64q_u8(unshuffledDigits);
			uint16x8_t str		  = vaddq_u16(vreinterpretq_u16_u8(digits), vreinterpretq_u16_s8(vdupq_n_s8('0')));
			uint16x8_t isNotZero  = vreinterpretq_u16_u8(vcgtzq_s8(vreinterpretq_s8_u8(digits)));
			uint64_t nonzeroMask  = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(isNotZero, 4U)), 0);
			return { str, 16 - (clz(nonzeroMask) >> 2) };
#else
			uint32_t hi = static_cast<uint32_t>(value / 100'000'000ULL);
			uint32_t lo = static_cast<uint32_t>(value % 100'000'000ULL);

			const __m128i div10k	  = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.div10k);
			const __m128i neg10kLocal = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.neg10k);
			__m128i x				  = _mm_set_epi64x(static_cast<int64_t>(hi), static_cast<int64_t>(lo));
			__m128i y				  = _mm_add_epi64(x, _mm_mul_epu32(neg10kLocal, _mm_srli_epi64(_mm_mul_epu32(x, div10k), div10kExp)));

			__m128i bcd			   = toBcd4x4(y);
			const __m128i zerosVal = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.zeros);

			uint64_t mask = static_cast<uint64_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(bcd, _mm_setzero_si128())));
			int32_t len	  = 16 - static_cast<int32_t>(ctz(mask));
			bcd			  = internal::simd::opShuffle(bcd, internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.bswap));
			return { internal::simd::opOr(bcd, zerosVal), len };
#endif
		}

		template<> JSONIFIER_INLINE auto toDigits<32>(uint64_t value) noexcept -> dec_digits<32> {
#if JSONIFIER_ANY_AVX
			uint64_t abcdEfgh	   = value + neg10k * ((value * div10kSig) >> div10kExp);
			__m128i bcdXmm		   = toBcd4x4(_mm_set_epi64x(0, static_cast<int64_t>(abcdEfgh)));
			uint64_t unshuffledBcd = static_cast<uint64_t>(_mm_cvtsi128_si64(bcdXmm));
			int32_t len			   = unshuffledBcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffledBcd)) / 8 : 0;
			return { bswap64(unshuffledBcd) + zeros, bcdXmm, len };
#elif JSONIFIER_NEON || JSONIFIER_SVE2
			uint64_t abcdEfgh	   = value + neg10k * ((value * div10kSig) >> div10kExp);
			int32x4_t input		   = vcombine_s32(vreinterpret_s32_u64(vcreate_u64(abcdEfgh)), vdup_n_s32(0));
			uint8x16_t unshuffled  = toBcd4x4(input);
			uint64_t unshuffledBcd = vget_lane_u64(vreinterpret_u64_u8(vget_low_u8(unshuffled)), 0);
			int32_t len			   = unshuffledBcd != 0ULL ? 8 - static_cast<int32_t>(ctz(unshuffledBcd)) / 8 : 0;
			return { bswap64(unshuffledBcd) + zeros, unshuffled, len };
#else
			auto result = toBcd8(value);
			return { result.bcd + zeros, result.len };
#endif
		}

		JSONIFIER_INLINE void writeDigits(jsonifier::write_buffer_ptr buffer, dec_digits<64>::digits_type digits, bool dropLeadingZero) noexcept {
			if constexpr (!JSONIFIER_ANY_SIMD) {
				jsonifier::pow2MemcpyWrapper<sizeof(digits)>(buffer, &digits);
				jsonifier::pow2MemmoveWrapper<sizeof(digits)>(buffer, buffer + (dropLeadingZero ? 1 : 0));
				return;
			}
#if JSONIFIER_NEON || JSONIFIER_SVE2
			uint8x16_t shuffle = vld1q_u8(staticData.shiftShuffle + (dropLeadingZero ? 1 : 0));
			uint8x16_t shifted = vqtbl1q_u8(vreinterpretq_u8_u16(digits), shuffle);
			vst1q_u8(std::bit_cast<uint8_t*>(buffer), shifted);
#elif JSONIFIER_ANY_AVX
			__m128i shuffle = internal::simd::gatherValuesU<jsonifier::jsonifier_simd_int_128>(staticData.shiftShuffle + (dropLeadingZero ? 1 : 0));
			internal::simd::storeU(internal::simd::opShuffle(digits, shuffle), buffer);
#endif
		}

		JSONIFIER_INLINE void writeDigits(jsonifier::write_buffer_ptr buffer, uint64_t digits, bool dropLeadingZero) noexcept {
			jsonifier::pow2MemcpyWrapper<sizeof(digits)>(buffer, &digits);
			jsonifier::pow2MemmoveWrapper<sizeof(digits)>(buffer, buffer + (dropLeadingZero ? 1 : 0));
		}

		JSONIFIER_INLINE auto writeExpFloatSimd(jsonifier::write_buffer_ptr buffer, const dec_digits<32>& dig, int32_t lastDigit, bool hasLastDigit, bool hasExtraDigit,
			[[maybe_unused]] uint64_t expData) noexcept -> jsonifier::internal::unrestrict_t<jsonifier::write_buffer_ptr> {
			uint32_t prefix				   = (static_cast<uint32_t>('.') << 8) + static_cast<uint32_t>('0') + static_cast<uint32_t>(lastDigit);
			[[maybe_unused]] uint64_t tail = expData | (static_cast<uint64_t>(prefix) << 32);
			auto entry					   = staticData.expFloatShuffles.getEntry(dig.numDigits, hasLastDigit, hasExtraDigit);
#if JSONIFIER_ANY_AVX
			__m128i ascii	= internal::simd::opOr(dig.unshuffled, internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(&staticData.zeros));
			__m128i src		= _mm_insert_epi64(ascii, static_cast<int64_t>(tail), 1);
			__m128i shuffle = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(entry.shuffle);
			internal::simd::storeU(internal::simd::opShuffle(src, shuffle), buffer);
#elif JSONIFIER_NEON || JSONIFIER_SVE2
			uint8x16_t ascii   = vorrq_u8(dig.unshuffled, vdupq_n_u8(static_cast<uint8_t>('0')));
			uint8x16_t src	   = vreinterpretq_u8_u64(vsetq_lane_u64(tail, vreinterpretq_u64_u8(ascii), 1));
			uint8x16_t shuffle = vld1q_u8(entry.shuffle);
			uint8x16_t out	   = vqtbl1q_u8(src, shuffle);
			vst1q_u8(std::bit_cast<uint8_t*>(buffer), out);
#endif
			return buffer + entry.length;
		}

		[[maybe_unused]] JSONIFIER_INLINE auto writeExpFloatSimd(jsonifier::write_buffer_ptr, const dec_digits<64>&, int32_t, bool, bool, uint64_t) noexcept
			-> jsonifier::internal::unrestrict_t<jsonifier::write_buffer_ptr> {
			return nullptr;
		}

		struct to_decimal_result {
			bool hasLastDigit = false;
			int32_t lastDigit = 0;
			int64_t sig;
			int32_t exp;
		};

		template<typename float_type, typename uint_type> JSONIFIER_INLINE auto toDecimal(uint_type binSig, int64_t rawExp, bool regular) noexcept -> to_decimal_result {
			using traits				 = float_traits<float_type>;
			int64_t binExp				 = rawExp - traits::expOffset;
			constexpr int32_t numBits	 = std::numeric_limits<uint_type>::digits;
			constexpr int32_t extraShift = exp_shift_table::extraShift;

			if (!regular) [[ZMIJ_UNLIKELY]] {
				int32_t decExp = computeDecExp(static_cast<int32_t>(binExp), false);
				uint8_t shift  = static_cast<uint8_t>(computeExpShift(static_cast<int32_t>(binExp), decExp + 1) + extraShift);
				uint128 pow10  = staticData.pow10Significands[-decExp - 1];
				uint128 p	   = umul192Hi128(pow10.hi, pow10.lo, static_cast<uint64_t>(binSig) << shift);

				int64_t integral	= static_cast<int64_t>(p.hi >> extraShift);
				uint64_t fractional = (p.hi << (64 - extraShift)) | (p.lo >> extraShift);

				uint64_t halfUlp = pow10.hi >> (extraShift + 1 - static_cast<int32_t>(shift));
				bool roundUp	 = halfUlp > ~uint64_t(0ULL) - fractional;
				bool roundDown	 = (halfUlp >> 1) > fractional;
				integral += (roundUp ? 1LL : 0LL);

				int32_t digit = static_cast<int32_t>(umul128AddHi64(fractional, 10ULL, (1ULL << 63) - 1ULL));
				int32_t lo	  = static_cast<int32_t>(umul128AddHi64(fractional - (halfUlp >> 1), 10ULL, ~uint64_t(0ULL)));
				if (digit < lo) {
					digit = lo;
				}
				return { (roundUp || roundDown) == false, digit, integral, decExp };
			}

			constexpr uint64_t log102Sig = 78'913ULL;
			constexpr int32_t log102Exp	 = 18;
			int32_t decExp =
				useUmul128Hi64 ? static_cast<int32_t>(umul128Hi64(static_cast<uint64_t>(binExp), log102Sig << (64 - log102Exp))) : computeDecExp(static_cast<int32_t>(binExp));
			ZMIJ_ASM(("" : "+r"(decExp)));
			uint8_t shift = static_cast<uint8_t>(exp_shift_table::enable ? staticData.expShifts.data[static_cast<uint64_t>(binExp + float_traits<double>::expOffset)]
																		 : computeExpShift(static_cast<int32_t>(binExp), decExp + 1) + extraShift);
			uint64_t even = 1ULL - static_cast<uint64_t>(binSig & 1U);

			if constexpr (numBits == 32) {
				constexpr int32_t extraShift32 = 34;
				shift						   = static_cast<uint8_t>(static_cast<int32_t>(shift) + (extraShift32 - exp_shift_table::extraShift));
				uint64_t pow10Hi			   = staticData.pow10Significands[-decExp - 1].hi;
				uint64_t p					   = umul128Hi64(pow10Hi + 1ULL, static_cast<uint64_t>(binSig) << shift);

				int64_t integral	= static_cast<int64_t>(p >> extraShift32);
				uint64_t fractional = p & ((1ULL << extraShift32) - 1ULL);

				uint64_t halfUlp = (pow10Hi >> (65 - static_cast<int32_t>(shift))) + even;
				bool roundUp	 = ((fractional + halfUlp) >> extraShift32) != 0ULL;
				bool roundDown	 = halfUlp > fractional;
				integral += (roundUp ? 1LL : 0LL);

				int32_t digit = static_cast<int32_t>((fractional * 10ULL + (1ULL << (extraShift32 - 1))) >> extraShift32);
				if (fractional == (1ULL << (extraShift32 - 2))) [[ZMIJ_UNLIKELY]] {
					digit = 2;
				}
				return { (roundUp || roundDown) == false, digit, integral, decExp };
			} else {
				uint128 pow10 = staticData.pow10Significands[-decExp - 1];
				uint128 p	  = umul192Hi128(pow10.hi, pow10.lo, static_cast<uint64_t>(binSig) << shift);

				int64_t integral	= static_cast<int64_t>(p.hi >> extraShift);
				uint64_t fractional = (p.hi << (64 - extraShift)) | (p.lo >> extraShift);

				uint64_t halfUlp = (pow10.hi >> (extraShift + 1 - static_cast<int32_t>(shift))) + even;
				bool roundUp	 = fractional + halfUlp < fractional;
				bool roundDown	 = halfUlp > fractional;
				integral += (roundUp ? 1LL : 0LL);

				int32_t digit = static_cast<int32_t>(umul128AddHi64(fractional, 10ULL, staticData.biasedHalf));
				if (fractional == (1ULL << 62)) [[ZMIJ_UNLIKELY]] {
					digit = 2;
				}
				return { (roundUp || roundDown) == false, digit, integral, decExp };
			}
		}

	}

	using namespace impl;
}

namespace jsonifier::zmij {

	[[maybe_unused]] JSONIFIER_INLINE auto toDecimal(double value) noexcept -> dec_fp {
		using traits  = float_traits<double>;
		auto bits	  = traits::toBits(value);
		auto binExp	  = traits::getExp(bits);
		auto binSig	  = traits::getSig(bits);
		auto negative = traits::isNegative(bits);
		if (binExp == 0 || binExp == traits::expMask) [[ZMIJ_UNLIKELY]] {
			if (binExp != 0) {
				return { static_cast<int64_t>(binSig), negative, static_cast<int32_t>(~0U >> 1U) };
			}
			if (binSig == 0ULL) {
				return { 0LL, negative, 0 };
			}
			binExp = 1;
			binSig |= traits::implicitBit;
		}
		auto dec	   = jsonifier::zmij::impl::toDecimal<double>(binSig ^ traits::implicitBit, binExp, binSig != 0ULL);
		auto lastDigit = -static_cast<int32_t>(dec.hasLastDigit) & dec.lastDigit;
		return { dec.sig * 10LL + static_cast<int64_t>(lastDigit), negative, dec.exp };
	}

	namespace detail {

		template<typename float_type>
		JSONIFIER_INLINE jsonifier::internal::unrestrict_t<jsonifier::write_buffer_ptr> write(float_type value, jsonifier::write_buffer_ptr buffer) noexcept {
			using traits = float_traits<float_type>;
			auto bits	 = traits::toBits(value);
			auto binExp	 = traits::getExp(bits);
			auto binSig	 = traits::getSig(bits);

			*buffer = '-';
			buffer += (traits::isNegative(bits) ? 1 : 0);

			const auto* d = &staticData;
			ZMIJ_ASM(("" : "+r"(d)));
			uint64_t threshold = traits::numBits == 64 ? d->threshold : 10000000ULL;

			to_decimal_result dec;
			bool isNormal = static_cast<uint32_t>(binExp - 1LL) < static_cast<uint32_t>(traits::expMask - 1);
			if (!isNormal) [[ZMIJ_UNLIKELY]] {
				if (binExp != 0) {
					jsonifier::pow2MemcpyWrapper<4>(buffer, binSig == 0ULL ? "inf" : "nan");
					return buffer + 3;
				}
				if (binSig == 0ULL) {
					jsonifier::pow2MemcpyWrapper<2>(buffer, "0");
					return buffer + 1;
				}
				dec			   = jsonifier::zmij::impl::toDecimal<float_type>(binSig, 1LL, true);
				int64_t decSig = dec.sig * 10LL + static_cast<int64_t>(-static_cast<int32_t>(dec.hasLastDigit) & dec.lastDigit);
				int32_t decExp = dec.exp;
				while (decSig < static_cast<int64_t>(threshold)) {
					decSig *= 10LL;
					--decExp;
				}
				int64_t q		  = static_cast<int64_t>(jsonifier::zmij::impl::div10(static_cast<uint64_t>(decSig)));
				int32_t lastDigit = static_cast<int32_t>(decSig - q * 10LL);
				dec				  = { lastDigit != 0, lastDigit, q, decExp };
			} else {
				dec = jsonifier::zmij::impl::toDecimal<float_type>(binSig | traits::implicitBit, binExp, binSig != 0ULL);
			}
			bool hasLastDigit  = dec.hasLastDigit;
			bool hasExtraDigit = dec.sig >= static_cast<int64_t>(threshold);
			int32_t decExp	   = dec.exp + traits::max_digits10 - 2 + (hasExtraDigit ? 1 : 0);
			if constexpr (traits::numBits == 32) {
				if (dec.sig < static_cast<int64_t>(1e6)) [[ZMIJ_UNLIKELY]] {
					dec.sig		 = 10LL * dec.sig + static_cast<int64_t>(-static_cast<int32_t>(hasLastDigit) & dec.lastDigit);
					hasLastDigit = false;
					--decExp;
				}
			}

			jsonifier::write_buffer_ptr start = buffer;
			auto dig						  = toDigits<traits::numBits>(static_cast<uint64_t>(dec.sig));
			constexpr int32_t bcdSize		  = traits::numBits == 64 ? 16 : 8;
			if (decExp >= traits::minFixedDecExp && decExp <= traits::maxFixedDecExp) {
				jsonifier::pow2MemcpyWrapper<8>(start, &zeros);
				uint8_t lastDigitChar = static_cast<uint8_t>(static_cast<int32_t>('0') + (-static_cast<int32_t>(hasLastDigit) & dec.lastDigit));
				int32_t numDigits	  = hasLastDigit ? bcdSize : dig.numDigits - 1;

				const auto* fixedLayouts = &d->fixedLayouts;
				if constexpr (JSONIFIER_ARCH_ARM64) {
					ZMIJ_ASM(("" : "+r"(fixedLayouts)));
				}

				const auto& layout = fixedLayouts->get(decExp);
				buffer += layout.startPos;
#if JSONIFIER_ANY_AVX
				if constexpr (bcdSize == 16) {
					const uint64_t extraIndex = hasExtraDigit ? 1 : 0;
					__m128i shuffle			  = internal::simd::gatherValues<jsonifier::jsonifier_simd_int_128>(layout.shuffle[extraIndex]);
					internal::simd::storeU(internal::simd::opShuffle(dig.digits, shuffle), buffer);
					buffer[bcdSize]							= static_cast<uint8_t>(_mm_extract_epi8(dig.digits, 15));
					start[layout.pointPos]					= '.';
					buffer[layout.lastDigitPos[extraIndex]] = lastDigitChar;
					return buffer + layout.endPos[static_cast<uint64_t>(numDigits + (hasExtraDigit ? 1 : 0) - 1)];
				} else {
#endif
					writeDigits(buffer, dig.digits, !hasExtraDigit);
					buffer[bcdSize + (hasExtraDigit ? 1 : 0) - 1] = lastDigitChar;
					uint32_t pointPos							  = layout.pointPos;
					jsonifier::pow2MemmoveWrapper<static_cast<uint64_t>(bcdSize)>(start + layout.shiftPos, start + pointPos);
					start[pointPos] = '.';
					return buffer + layout.endPos[static_cast<uint64_t>(numDigits + (hasExtraDigit ? 1 : 0) - 1)];
#if JSONIFIER_ANY_AVX
				}
#endif
			}
			if constexpr (traits::numBits == 32 && exp_float_shuffle_table::enable) {
				uint64_t expData = d->expStrings.data[static_cast<uint64_t>(decExp + exp_string_table::offset)];
				return writeExpFloatSimd(buffer, dig, dec.lastDigit, hasLastDigit, hasExtraDigit, expData);
			} else {
				buffer += (hasExtraDigit ? 1 : 0);
				jsonifier::pow2MemcpyWrapper<bcdSize>(buffer, &dig.digits);
				buffer[bcdSize] = static_cast<uint8_t>(static_cast<int32_t>('0') + dec.lastDigit);
				buffer += select(hasLastDigit ? 1ULL : 0ULL, bcdSize + 1, dig.numDigits);
				start[0] = start[1];
				start[1] = '.';
				buffer -= (buffer - 1 == start + 1 ? 1 : 0);

				if constexpr (exp_string_table::enable) {
					uint64_t expData = d->expStrings.data[static_cast<uint64_t>(decExp + exp_string_table::offset)];
					int32_t len		 = static_cast<int32_t>(expData >> 48);
					if constexpr (isBigEndian) {
						expData = bswap64(expData);
					}
					jsonifier::pow2MemcpyWrapper<(traits::max_exponent10 >= 100 ? 8ULL : 4ULL)>(buffer, &expData);
					return buffer + len;
				} else {
					uint16_t eSign = static_cast<uint16_t>(
						decExp >= 0 ? (static_cast<int32_t>('+') << 8 | static_cast<int32_t>('e')) : (static_cast<int32_t>('-') << 8 | static_cast<int32_t>('e')));
					if constexpr (isBigEndian) {
						eSign = static_cast<uint16_t>((static_cast<int32_t>(eSign) << 8) | (static_cast<int32_t>(eSign) >> 8));
					} else {
						jsonifier::pow2MemcpyWrapper<2>(buffer, &eSign);
						buffer += 2;
						decExp = decExp >= 0 ? decExp : -decExp;
						if constexpr (traits::max_exponent10 >= 100) {
							uint32_t digit = useUmul128Hi64 ? static_cast<uint32_t>(umul128Hi64(static_cast<uint64_t>(decExp), 0x290000000000000ULL))
															: static_cast<uint32_t>((static_cast<uint32_t>(decExp) * div100Sig) >> div100Exp);
							*buffer		   = static_cast<uint8_t>(static_cast<uint32_t>('0') + digit);
							buffer += (decExp >= 100 ? 1 : 0);
							decExp -= static_cast<int32_t>(digit * 100U);
						} else {
							jsonifier::pow2MemcpyWrapper<2>(buffer, digits2(static_cast<uint64_t>(decExp)));
							return buffer + 2;
						}
					}
				}
			}
		}

	}
}
