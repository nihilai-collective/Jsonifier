/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/i_to_str.hpp
 */
#pragma once

#include <jsonifier-incl/containers/allocator.hpp>

namespace jsonifier::internal {

	template<uint64_t count> consteval uint64_t genTableSize() {
		uint64_t result = 1;
		for (uint64_t j = 0; j < count; ++j) {
			result *= 10;
		}
		return result;
	}

	template<uint64_t count> using gen_table_value_t = conditional_t<(count <= 2ULL), uint16_t, uint32_t>;

	template<uint64_t count> consteval array<gen_table_value_t<count>, genTableSize<count>()> genTable() {
		using value_type		= gen_table_value_t<count>;
		constexpr uint64_t size = genTableSize<count>();
		array<value_type, size> t{};
		for (uint64_t i = 0; i < size; ++i) {
			uint64_t divisor = size / 10;
			for (uint64_t j = 0; j < count; ++j) {
				const auto digit	 = static_cast<value_type>(static_cast<uint8_t>('0') + (i / divisor % 10));
				const uint64_t shift = (std::endian::native == std::endian::little ? j : sizeof(value_type) - 1 - j) * 8;
				t[i] |= static_cast<value_type>(digit << shift);
				divisor /= 10;
			}
		}
		return t;
	}

	inline constexpr auto table02{ genTable<2ULL>() };
	inline constexpr const auto* __restrict charTable2DigitData = table02.data();
	inline constexpr auto table03{ genTable<3ULL>() };
	inline constexpr const auto* __restrict charTable3DigitData = table03.data();
	inline constexpr auto table04{ genTable<4ULL>() };
	inline constexpr const auto* __restrict charTable4DigitData = table04.data();

#if !JSONIFIER_COMPILER_CLANG && !JSONIFIER_COMPILER_GCC && !JSONIFIER_COMPILER_MSVC

	template<uint_types v_type_new> JSONIFIER_INLINE v_type_new mulhiPortable(v_type_new a, v_type_new b) noexcept {
		using v_type						= next_higher_int_t<v_type_new>;
		static constexpr uint64_t totalBits = sizeof(v_type_new) * 8;
		static constexpr uint64_t halfBits	= totalBits / 2;
		static constexpr v_type mask		= (static_cast<v_type>(1) << halfBits) - 1;
		const v_type aLo					= static_cast<v_type>(a) & mask;
		const v_type aHi					= static_cast<v_type>(a) >> halfBits;
		const v_type bLo					= static_cast<v_type>(b) & mask;
		const v_type bHi					= static_cast<v_type>(b) >> halfBits;
		const v_type loLo					= aLo * bLo;
		const v_type hiLo					= aHi * bLo;
		const v_type loHi					= aLo * bHi;
		const v_type hiHi					= aHi * bHi;
		const v_type cross					= (loLo >> halfBits) + (hiLo & mask) + (loHi & mask);
		return static_cast<v_type_new>(hiHi + (hiLo >> halfBits) + (loHi >> halfBits) + (cross >> halfBits));
	}

#endif

	struct multiply_and_shift {
		JSONIFIER_INLINE static uint64_t impl(uint64_t value) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			return static_cast<uint64_t>(static_cast<__uint128_t>(value) * 12379400392853802749ULL >> 90);
#elif JSONIFIER_COMPILER_MSVC
			uint64_t highPart;
			_umul128(12379400392853802749ULL, value, &highPart);
			return static_cast<uint64_t>(highPart >> (90 - 64ULL));
#else
			return static_cast<uint64_t>(mulhiPortable(value, 12379400392853802749ULL) >> (90 - 64ULL));
#endif
		}
	};

	template<uint_types v_type> JSONIFIER_INLINE void copy3Digits(write_buffer_ptr __restrict buf, const v_type value) noexcept {
		uint32_t packed;
		pow2MemcpyWrapper<4ULL>(&packed, &charTable3DigitData[value]);
		pow2MemcpyWrapper<2ULL>(buf, &packed);
		buf[2] = static_cast<uint8_t>(packed >> 16);
	}

	template<typename v_type, uint64_t digitLength> struct to_chars_internal;

	template<typename v_type> struct to_chars;

	template<uint_types v_type> struct to_chars_internal<v_type, 5ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type a = value * 3518437209ULL >> 45;
			*buf		   = static_cast<uint8_t>(a + '0');
			pow2MemcpyWrapper<4ULL>(buf + 1, charTable4DigitData + value - a * 10000);
			return buf + 5;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 6ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type ab = value * 3518437209ULL >> 45;
			pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + ab);
			pow2MemcpyWrapper<4ULL>(buf + 2, charTable4DigitData + value - (ab * 10000ULL));
			return buf + 6;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 7ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abc = value * 3518437209ULL >> 45;
			copy3Digits(buf, abc);
			pow2MemcpyWrapper<4ULL>(buf + 3, charTable4DigitData + value - (abc * 10000ULL));
			return buf + 7;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 8ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcd = value * 3518437209ULL >> 45;
			pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + abcd);
			pow2MemcpyWrapper<4ULL>(buf + 4, charTable4DigitData + value - (abcd * 10000ULL));
			return buf + 8;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 9ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type a		  = static_cast<v_type>(multiply_and_shift::impl(value));
			const v_type bcdefghi = value - a * 100000000ULL;
			const v_type bcde	  = bcdefghi * 3518437209ULL >> 45;
			const v_type fghi	  = bcdefghi - (bcde * 10000ULL);
			*buf				  = static_cast<uint8_t>(a + '0');
			pow2MemcpyWrapper<4ULL>(buf + 1, charTable4DigitData + bcde);
			pow2MemcpyWrapper<4ULL>(buf + 5, charTable4DigitData + fghi);
			return buf + 9;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 10ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type ab		  = static_cast<v_type>(multiply_and_shift::impl(value));
			const v_type cdefghij = value - ab * 100000000ULL;
			const v_type cdef	  = cdefghij * 3518437209ULL >> 45;
			const v_type ghij	  = cdefghij - (cdef * 10000ULL);
			pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + ab);
			pow2MemcpyWrapper<4ULL>(buf + 2, charTable4DigitData + cdef);
			pow2MemcpyWrapper<4ULL>(buf + 6, charTable4DigitData + ghij);
			return buf + 10;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 11ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abc	  = multiply_and_shift::impl(value);
			const v_type defghijk = value - abc * 100000000ULL;
			const v_type defg	  = defghijk * 3518437209ULL >> 45;
			const v_type hijk	  = defghijk - (defg * 10000ULL);
			copy3Digits(buf, abc);
			pow2MemcpyWrapper<4ULL>(buf + 3, charTable4DigitData + defg);
			pow2MemcpyWrapper<4ULL>(buf + 7, charTable4DigitData + hijk);
			return buf + 11;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 12ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcd	  = multiply_and_shift::impl(value);
			const v_type efghijkl = value - abcd * 100000000ULL;
			const v_type efgh	  = efghijkl * 3518437209ULL >> 45;
			const v_type ijkl	  = efghijkl - (efgh * 10000ULL);
			pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + abcd);
			pow2MemcpyWrapper<4ULL>(buf + 4, charTable4DigitData + efgh);
			pow2MemcpyWrapper<4ULL>(buf + 8, charTable4DigitData + ijkl);
			return buf + 12;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 13ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcde	  = multiply_and_shift::impl(value);
			const v_type fghijklm = value - abcde * 100000000ULL;
			const v_type a		  = abcde * 3518437209ULL >> 45;
			const v_type bcde	  = abcde - (a * 10000ULL);
			const v_type fghi	  = fghijklm * 3518437209ULL >> 45;
			const v_type jklm	  = fghijklm - (fghi * 10000ULL);
			*buf				  = static_cast<uint8_t>(a + '0');
			pow2MemcpyWrapper<4ULL>(buf + 1, charTable4DigitData + bcde);
			pow2MemcpyWrapper<4ULL>(buf + 5, charTable4DigitData + fghi);
			pow2MemcpyWrapper<4ULL>(buf + 9, charTable4DigitData + jklm);
			return buf + 13;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 14ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdef	  = multiply_and_shift::impl(value);
			const v_type ghijklmn = value - abcdef * 100000000ULL;
			const v_type ab		  = abcdef * 3518437209ULL >> 45;
			const v_type cdef	  = abcdef - (ab * 10000ULL);
			const v_type ghij	  = ghijklmn * 3518437209ULL >> 45;
			const v_type klmn	  = ghijklmn - (ghij * 10000ULL);
			pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + ab);
			pow2MemcpyWrapper<4ULL>(buf + 2, charTable4DigitData + cdef);
			pow2MemcpyWrapper<4ULL>(buf + 6, charTable4DigitData + ghij);
			pow2MemcpyWrapper<4ULL>(buf + 10, charTable4DigitData + klmn);
			return buf + 14;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 15ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefg  = multiply_and_shift::impl(value);
			const v_type hijklmno = value - abcdefg * 100000000ULL;
			const v_type abc	  = abcdefg * 3518437209ULL >> 45;
			const v_type defg	  = abcdefg - (abc * 10000ULL);
			const v_type hijk	  = hijklmno * 3518437209ULL >> 45;
			const v_type lmno	  = hijklmno - (hijk * 10000ULL);
			copy3Digits(buf, abc);
			pow2MemcpyWrapper<4ULL>(buf + 3, charTable4DigitData + defg);
			pow2MemcpyWrapper<4ULL>(buf + 7, charTable4DigitData + hijk);
			pow2MemcpyWrapper<4ULL>(buf + 11, charTable4DigitData + lmno);
			return buf + 15;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 16ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefgh = multiply_and_shift::impl(value);
			const v_type ijklmnop = value - abcdefgh * 100000000ULL;
			const v_type abcd	  = abcdefgh * 3518437209ULL >> 45;
			const v_type efgh	  = abcdefgh - (abcd * 10000ULL);
			const v_type ijkl	  = ijklmnop * 3518437209ULL >> 45;
			const v_type mnop	  = ijklmnop - (ijkl * 10000ULL);
			pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + abcd);
			pow2MemcpyWrapper<4ULL>(buf + 4, charTable4DigitData + efgh);
			pow2MemcpyWrapper<4ULL>(buf + 8, charTable4DigitData + ijkl);
			pow2MemcpyWrapper<4ULL>(buf + 12, charTable4DigitData + mnop);
			return buf + 16;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 17ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghi = multiply_and_shift::impl(value);
			const v_type jklmnopq  = value - abcdefghi * 100000000ULL;
			const v_type a		   = multiply_and_shift::impl(abcdefghi);
			const v_type bcdefghi  = abcdefghi - a * 100000000ULL;
			const v_type bcde	   = bcdefghi * 3518437209ULL >> 45;
			const v_type fghi	   = bcdefghi - (bcde * 10000ULL);
			const v_type jklm	   = jklmnopq * 3518437209ULL >> 45;
			const v_type nopq	   = jklmnopq - (jklm * 10000ULL);
			*buf				   = static_cast<uint8_t>(a + '0');
			pow2MemcpyWrapper<4ULL>(buf + 1, charTable4DigitData + bcde);
			pow2MemcpyWrapper<4ULL>(buf + 5, charTable4DigitData + fghi);
			pow2MemcpyWrapper<4ULL>(buf + 9, charTable4DigitData + jklm);
			pow2MemcpyWrapper<4ULL>(buf + 13, charTable4DigitData + nopq);
			return buf + 17;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 18ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghij = multiply_and_shift::impl(value);
			const v_type klmnopqr	= value - abcdefghij * 100000000ULL;
			const v_type ab			= multiply_and_shift::impl(abcdefghij);
			const v_type cdefghij	= abcdefghij - ab * 100000000ULL;
			const v_type cdef		= cdefghij * 3518437209ULL >> 45;
			const v_type ghij		= cdefghij - (cdef * 10000ULL);
			const v_type klmn		= klmnopqr * 3518437209ULL >> 45;
			const v_type opqr		= klmnopqr - (klmn * 10000ULL);
			pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + ab);
			pow2MemcpyWrapper<4ULL>(buf + 2, charTable4DigitData + cdef);
			pow2MemcpyWrapper<4ULL>(buf + 6, charTable4DigitData + ghij);
			pow2MemcpyWrapper<4ULL>(buf + 10, charTable4DigitData + klmn);
			pow2MemcpyWrapper<4ULL>(buf + 14, charTable4DigitData + opqr);
			return buf + 18;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 19ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghijk = multiply_and_shift::impl(value);
			const v_type lmnopqrs	 = value - abcdefghijk * 100000000ULL;
			const v_type abc		 = multiply_and_shift::impl(abcdefghijk);
			const v_type defghijk	 = abcdefghijk - abc * 100000000ULL;
			const v_type defg		 = defghijk * 3518437209ULL >> 45;
			const v_type hijk		 = defghijk - (defg * 10000ULL);
			const v_type lmno		 = lmnopqrs * 3518437209ULL >> 45;
			const v_type pqrs		 = lmnopqrs - (lmno * 10000ULL);
			copy3Digits(buf, abc);
			pow2MemcpyWrapper<4ULL>(buf + 3, charTable4DigitData + defg);
			pow2MemcpyWrapper<4ULL>(buf + 7, charTable4DigitData + hijk);
			pow2MemcpyWrapper<4ULL>(buf + 11, charTable4DigitData + lmno);
			pow2MemcpyWrapper<4ULL>(buf + 15, charTable4DigitData + pqrs);
			return buf + 19;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 20ULL> {
		static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghijkl = multiply_and_shift::impl(value);
			const v_type mnopqrst	  = value - abcdefghijkl * 100000000ULL;
			const v_type abcd		  = multiply_and_shift::impl(abcdefghijkl);
			const v_type efghijkl	  = abcdefghijkl - abcd * 100000000ULL;
			const v_type efgh		  = efghijkl * 3518437209ULL >> 45;
			const v_type ijkl		  = efghijkl - (efgh * 10000ULL);
			const v_type mnop		  = mnopqrst * 3518437209ULL >> 45;
			const v_type qrst		  = mnopqrst - (mnop * 10000ULL);
			pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + abcd);
			pow2MemcpyWrapper<4ULL>(buf + 4, charTable4DigitData + efgh);
			pow2MemcpyWrapper<4ULL>(buf + 8, charTable4DigitData + ijkl);
			pow2MemcpyWrapper<4ULL>(buf + 12, charTable4DigitData + mnop);
			pow2MemcpyWrapper<4ULL>(buf + 16, charTable4DigitData + qrst);
			return buf + 20;
		}
	};

	template<uint64_types auto size, uint_types v_type>
	JSONIFIER_INLINE write_buffer_ptr implInternal(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
		return to_chars_internal<v_type, size>::impl(buf, value);
	}

	template<uint64_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 100000000ULL ? value < 10000ULL ? value < 100ULL ? value < 10U ? (static_cast<void>(buf[0] = static_cast<uint8_t>(value + '0')), buf + 1)
																						  : (static_cast<void>(pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + value)), buf + 2)
						: value < 1000U										? (static_cast<void>(copy3Digits(buf, value)), buf + 3)
																			: (static_cast<void>(pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + value)), buf + 4)
					: value < 1000000ULL				   ? value < 100000ULL ? implInternal<5ULL>(buf, value) : implInternal<6ULL>(buf, value)
					: value < 10000000ULL				   ? implInternal<7ULL>(buf, value)
														   : implInternal<8ULL>(buf, value)
				: value < 1000000000000ULL		  ? value < 10000000000ULL ? value < 1000000000ULL ? implInternal<9ULL>(buf, value) : implInternal<10ULL>(buf, value)
					: value < 100000000000ULL						? implInternal<11ULL>(buf, value)
																	: implInternal<12ULL>(buf, value)
				: value < 10000000000000000ULL	  ? value < 100000000000000ULL ? value < 10000000000000ULL ? implInternal<13ULL>(buf, value) : implInternal<14ULL>(buf, value)
					: value < 1000000000000000ULL							? implInternal<15ULL>(buf, value)
																			: implInternal<16ULL>(buf, value)
				: value < 1000000000000000000ULL  ? value < 100000000000000000ULL ? implInternal<17ULL>(buf, value) : implInternal<18ULL>(buf, value)
				: value < 10000000000000000000ULL ? implInternal<19ULL>(buf, value)
												  : implInternal<20ULL>(buf, value);
		}
	};

	template<uint32_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 100000U	  ? value < 1000U ? value < 100U ? value < 10U ? (static_cast<void>(buf[0] = static_cast<uint8_t>(value + '0')), buf + 1)
																				   : (static_cast<void>(pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + value)), buf + 2)
																	 : (static_cast<void>(copy3Digits(buf, value)), buf + 3)
					: value < 10000ULL			   ? (static_cast<void>(pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + value)), buf + 4)
												   : implInternal<5ULL>(buf, value)
				: value < 10000000U	  ? value < 1000000U ? implInternal<6ULL>(buf, value) : implInternal<7ULL>(buf, value)
				: value < 1000000000U ? value < 100000000U ? implInternal<8ULL>(buf, value) : implInternal<9ULL>(buf, value)
									  : implInternal<10ULL>(buf, value);
		}
	};

	template<uint16_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 1000U   ? value < 100U ? value < 10U ? (static_cast<void>(buf[0] = static_cast<uint8_t>(value + '0')), buf + 1)
																: (static_cast<void>(pow2MemcpyWrapper<2ULL>(buf, charTable2DigitData + value)), buf + 2)
												  : (static_cast<void>(copy3Digits(buf, value)), buf + 3)
				: value < 10000ULL ? (static_cast<void>(pow2MemcpyWrapper<4ULL>(buf, charTable4DigitData + value)), buf + 4)
								   : implInternal<5ULL>(buf, value);
		}
	};

	template<uint8_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr __restrict buf, const v_type value) noexcept {
			return value < 100 ? value < 10 ? (static_cast<void>(buf[0] = static_cast<uint8_t>(value + '0')), buf + 1)
											: (static_cast<void>(pow2MemcpyWrapper<2>(buf, &charTable2DigitData[value])), buf + 2)
							   : (static_cast<void>(copy3Digits(buf, value)), buf + 3);
		}
	};

	template<int_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static write_buffer_ptr impl(write_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			using unsigned_type					= std::make_unsigned_t<v_type>;
			constexpr unsigned_type shiftAmount = static_cast<unsigned_type>(sizeof(v_type) * 8ULL - 1ULL);
			return (value < 0) ? implNegative(buf, value)
							   : to_chars<unsigned_type>::impl(buf,
									 static_cast<unsigned_type>((static_cast<unsigned_type>(value) ^ static_cast<unsigned_type>(value >> shiftAmount)) -
										 static_cast<unsigned_type>(value >> shiftAmount)));
		}

		JSONIFIER_INLINE static write_buffer_ptr implNegative(write_buffer_ptr __restrict buf, const v_type value) noexcept {
			using unsigned_type					= std::make_unsigned_t<v_type>;
			constexpr unsigned_type shiftAmount = static_cast<unsigned_type>(sizeof(v_type) * 8ULL - 1ULL);
			*buf								= '-';
			return to_chars<unsigned_type>::impl(buf + 1,
				static_cast<unsigned_type>(
					(static_cast<unsigned_type>(value) ^ static_cast<unsigned_type>(value >> shiftAmount)) - static_cast<unsigned_type>(value >> shiftAmount)));
		}
	};

}
