// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/i_to_str.hpp
#pragma once

#include <jsonifier-incl/containers/allocator.hpp>

namespace jsonifier::internal {

	template<std::endian, uint64_t size = 0> struct int_tables_impl {};

	template<std::endian endianness> static constexpr array<uint16_t, 100> gen_2() {
		array<uint16_t, 100> t{};
		for (uint32_t i = 0; i < 100; ++i) {
			if constexpr (endianness == std::endian::little) {
				t[i] |= static_cast<uint16_t>(static_cast<uint8_t>('0') + (i / 10));
				t[i] |= static_cast<uint16_t>(static_cast<uint8_t>('0') + (i % 10)) << 8;
			} else {
				t[i] |= static_cast<uint16_t>(static_cast<uint8_t>('0') + (i / 10)) << 8;
				t[i] |= static_cast<uint16_t>(static_cast<uint8_t>('0') + (i % 10));
			}
		}
		return t;
	}

	template<std::endian endianness> static constexpr array<array<char, 3>, 1000> gen_3() {
		array<array<char, 3>, 1000> t{};
		for (uint32_t i = 0; i < 1000; ++i) {
			if constexpr (endianness == std::endian::little) {
				t[i][0] = static_cast<char>(static_cast<uint8_t>('0') + (i / 100));
				t[i][1] = static_cast<char>(static_cast<uint8_t>('0') + (i / 10 % 10));
				t[i][2] = static_cast<char>(static_cast<uint8_t>('0') + (i % 10));
			} else {
				t[i][2] = static_cast<char>(static_cast<uint8_t>('0') + (i / 100));
				t[i][1] = static_cast<char>(static_cast<uint8_t>('0') + (i / 10 % 10));
				t[i][0] = static_cast<char>(static_cast<uint8_t>('0') + (i % 10));
			}
		}
		return t;
	}

	template<std::endian endianness> static constexpr array<uint32_t, 10000> gen_4() {
		array<uint32_t, 10000> t{};
		for (uint32_t i = 0; i < 10000; ++i) {
			if constexpr (endianness == std::endian::little) {
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 1000));
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 100 % 10)) << 8;
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 10 % 10)) << 16;
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i % 10)) << 24;
			} else {
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 1000)) << 24;
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 100 % 10)) << 16;
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i / 10 % 10)) << 8;
				t[i] |= static_cast<uint32_t>(static_cast<uint8_t>('0') + (i % 10));
			}
		}
		return t;
	}

	template<std::endian endianness> struct int_tables_impl<endianness, 2> {
		alignas(64) static constexpr array<uint16_t, 100> table{ gen_2<endianness>() };
		alignas(64) static constexpr const uint16_t* __restrict values{ table.data() };
	};

	template<std::endian endianness> struct int_tables_impl<endianness, 3> {
		alignas(64) static constexpr array<array<char, 3>, 1000> table{ gen_3<endianness>() };
		alignas(64) static constexpr const array<char, 3>* __restrict values{ table.data() };
	};

	template<std::endian endianness> struct int_tables_impl<endianness, 4> {
		alignas(64) static constexpr array<uint32_t, 10000> table{ gen_4<endianness>() };
		alignas(64) static constexpr const uint32_t* __restrict values{ table.data() };
	};

	template<uint64_t size> using int_tables = int_tables_impl<std::endian::native, size>;

#if !JSONIFIER_COMPILER_CLANG && !JSONIFIER_COMPILER_GCC && !JSONIFIER_COMPILER_MSVC

	template<uint_types v_type_new> JSONIFIER_INLINE static v_type_new mulhi_portable(v_type_new a, v_type_new b) noexcept {
		using v_type						 = next_higher_int_t<v_type_new>;
		static constexpr uint64_t total_bits = sizeof(v_type_new) * 8;
		static constexpr uint64_t half_bits	 = total_bits / 2;
		static constexpr v_type mask		 = (static_cast<v_type>(1) << half_bits) - 1;
		const v_type a_lo					 = static_cast<v_type>(a) & mask;
		const v_type a_hi					 = static_cast<v_type>(a) >> half_bits;
		const v_type b_lo					 = static_cast<v_type>(b) & mask;
		const v_type b_hi					 = static_cast<v_type>(b) >> half_bits;
		const v_type lo_lo					 = a_lo * b_lo;
		const v_type hi_lo					 = a_hi * b_lo;
		const v_type lo_hi					 = a_lo * b_hi;
		const v_type hi_hi					 = a_hi * b_hi;
		const v_type cross					 = (lo_lo >> half_bits) + (hi_lo & mask) + (lo_hi & mask);
		return static_cast<v_type_new>(hi_hi + (hi_lo >> half_bits) + (lo_hi >> half_bits) + (cross >> half_bits));
	}

#endif

	struct multiply_and_shift {
		JSONIFIER_INLINE static uint64_t impl(uint64_t value) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			return static_cast<uint64_t>(static_cast<__uint128_t>(value) * 12379400392853802749ULL >> 90);
#elif JSONIFIER_COMPILER_MSVC
			uint64_t high_part;
			_umul128(12379400392853802749ULL, value, &high_part);
			return static_cast<uint64_t>(high_part >> (90 - 64ULL));
#else
			return static_cast<uint64_t>(mulhi_portable(value, 12379400392853802749ULL) >> (90 - 64ULL));
#endif
		}
	};

	static constexpr const auto* __restrict char_table_2_digit_data = int_tables<2>::values;
	static constexpr const auto* __restrict char_table_3_digit_data = int_tables<3>::values;
	static constexpr const auto* __restrict char_table_4_digit_data = int_tables<4>::values;

	template<typename v_type, uint64_t digit_length> struct to_chars_internal;

	template<typename v_type> struct to_chars;

	template<uint_types v_type> struct to_chars_internal<v_type, 5ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type a = value * 3518437209ULL >> 45;
			*buf		   = static_cast<char>(a) + '0';
			std::memcpy(buf + 1, char_table_4_digit_data + value - a * 10000, 4ULL);
			return buf + 5;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 6ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type ab = value * 3518437209ULL >> 45;
			std::memcpy(buf, char_table_2_digit_data + ab, 2ULL);
			std::memcpy(buf + 2, char_table_4_digit_data + value - (ab * 10000ULL), 4ULL);
			return buf + 6;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 7ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abc = value * 3518437209ULL >> 45;
			std::memcpy(buf, char_table_3_digit_data + abc, 3ULL);
			std::memcpy(buf + 3, char_table_4_digit_data + value - (abc * 10000ULL), 4ULL);
			return buf + 7;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 8ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcd = value * 3518437209ULL >> 45;
			std::memcpy(buf, char_table_4_digit_data + abcd, 4ULL);
			std::memcpy(buf + 4, char_table_4_digit_data + value - (abcd * 10000ULL), 4ULL);
			return buf + 8;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 9ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type a		  = static_cast<v_type>(multiply_and_shift::impl(value));
			const v_type bcdefghi = value - a * 100000000ULL;
			const v_type bcde	  = bcdefghi * 3518437209ULL >> 45;
			const v_type fghi	  = bcdefghi - (bcde * 10000ULL);
			*buf				  = static_cast<char>(a) + '0';
			std::memcpy(buf + 1, char_table_4_digit_data + bcde, 4ULL);
			std::memcpy(buf + 5, char_table_4_digit_data + fghi, 4ULL);
			return buf + 9;
		}
	};

	template<uint_types v_type> struct to_chars_internal<v_type, 10ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type ab		  = static_cast<v_type>(multiply_and_shift::impl(value));
			const v_type cdefghij = value - ab * 100000000ULL;
			const v_type cdef	  = cdefghij * 3518437209ULL >> 45;
			const v_type ghij	  = cdefghij - (cdef * 10000ULL);
			std::memcpy(buf, char_table_2_digit_data + ab, 2ULL);
			std::memcpy(buf + 2, char_table_4_digit_data + cdef, 4ULL);
			std::memcpy(buf + 6, char_table_4_digit_data + ghij, 4ULL);
			return buf + 10;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 11ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abc	  = multiply_and_shift::impl(value);
			const v_type defghijk = value - abc * 100000000ULL;
			const v_type defg	  = defghijk * 3518437209ULL >> 45;
			const v_type hijk	  = defghijk - (defg * 10000ULL);
			std::memcpy(buf, char_table_3_digit_data + abc, 3ULL);
			std::memcpy(buf + 3, char_table_4_digit_data + defg, 4ULL);
			std::memcpy(buf + 7, char_table_4_digit_data + hijk, 4ULL);
			return buf + 11;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 12ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcd	  = multiply_and_shift::impl(value);
			const v_type efghijkl = value - abcd * 100000000ULL;
			const v_type efgh	  = efghijkl * 3518437209ULL >> 45;
			const v_type ijkl	  = efghijkl - (efgh * 10000ULL);
			std::memcpy(buf, char_table_4_digit_data + abcd, 4ULL);
			std::memcpy(buf + 4, char_table_4_digit_data + efgh, 4ULL);
			std::memcpy(buf + 8, char_table_4_digit_data + ijkl, 4ULL);
			return buf + 12;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 13ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcde	  = multiply_and_shift::impl(value);
			const v_type fghijklm = value - abcde * 100000000ULL;
			const v_type a		  = abcde * 3518437209ULL >> 45;
			const v_type bcde	  = abcde - (a * 10000ULL);
			const v_type fghi	  = fghijklm * 3518437209ULL >> 45;
			const v_type jklm	  = fghijklm - (fghi * 10000ULL);
			*buf				  = static_cast<char>(a) + '0';
			std::memcpy(buf + 1, char_table_4_digit_data + bcde, 4ULL);
			std::memcpy(buf + 5, char_table_4_digit_data + fghi, 4ULL);
			std::memcpy(buf + 9, char_table_4_digit_data + jklm, 4ULL);
			return buf + 13;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 14ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdef	  = multiply_and_shift::impl(value);
			const v_type ghijklmn = value - abcdef * 100000000ULL;
			const v_type ab		  = abcdef * 3518437209ULL >> 45;
			const v_type cdef	  = abcdef - (ab * 10000ULL);
			const v_type ghij	  = ghijklmn * 3518437209ULL >> 45;
			const v_type klmn	  = ghijklmn - (ghij * 10000ULL);
			std::memcpy(buf, char_table_2_digit_data + ab, 2ULL);
			std::memcpy(buf + 2, char_table_4_digit_data + cdef, 4ULL);
			std::memcpy(buf + 6, char_table_4_digit_data + ghij, 4ULL);
			std::memcpy(buf + 10, char_table_4_digit_data + klmn, 4ULL);
			return buf + 14;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 15ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefg  = multiply_and_shift::impl(value);
			const v_type hijklmno = value - abcdefg * 100000000ULL;
			const v_type abc	  = abcdefg * 3518437209ULL >> 45;
			const v_type defg	  = abcdefg - (abc * 10000ULL);
			const v_type hijk	  = hijklmno * 3518437209ULL >> 45;
			const v_type lmno	  = hijklmno - (hijk * 10000ULL);
			std::memcpy(buf, char_table_3_digit_data + abc, 3ULL);
			std::memcpy(buf + 3, char_table_4_digit_data + defg, 4ULL);
			std::memcpy(buf + 7, char_table_4_digit_data + hijk, 4ULL);
			std::memcpy(buf + 11, char_table_4_digit_data + lmno, 4ULL);
			return buf + 15;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 16ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefgh = multiply_and_shift::impl(value);
			const v_type ijklmnop = value - abcdefgh * 100000000ULL;
			const v_type abcd	  = abcdefgh * 3518437209ULL >> 45;
			const v_type efgh	  = abcdefgh - (abcd * 10000ULL);
			const v_type ijkl	  = ijklmnop * 3518437209ULL >> 45;
			const v_type mnop	  = ijklmnop - (ijkl * 10000ULL);
			std::memcpy(buf, char_table_4_digit_data + abcd, 4ULL);
			std::memcpy(buf + 4, char_table_4_digit_data + efgh, 4ULL);
			std::memcpy(buf + 8, char_table_4_digit_data + ijkl, 4ULL);
			std::memcpy(buf + 12, char_table_4_digit_data + mnop, 4ULL);
			return buf + 16;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 17ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghi = multiply_and_shift::impl(value);
			const v_type jklmnopq  = value - abcdefghi * 100000000ULL;
			const v_type a		   = multiply_and_shift::impl(abcdefghi);
			const v_type bcdefghi  = abcdefghi - a * 100000000ULL;
			const v_type bcde	   = bcdefghi * 3518437209ULL >> 45;
			const v_type fghi	   = bcdefghi - (bcde * 10000ULL);
			const v_type jklm	   = jklmnopq * 3518437209ULL >> 45;
			const v_type nopq	   = jklmnopq - (jklm * 10000ULL);
			*buf				   = static_cast<char>(a) + '0';
			std::memcpy(buf + 1, char_table_4_digit_data + bcde, 4ULL);
			std::memcpy(buf + 5, char_table_4_digit_data + fghi, 4ULL);
			std::memcpy(buf + 9, char_table_4_digit_data + jklm, 4ULL);
			std::memcpy(buf + 13, char_table_4_digit_data + nopq, 4ULL);
			return buf + 17;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 18ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghij = multiply_and_shift::impl(value);
			const v_type klmnopqr	= value - abcdefghij * 100000000ULL;
			const v_type ab			= multiply_and_shift::impl(abcdefghij);
			const v_type cdefghij	= abcdefghij - ab * 100000000ULL;
			const v_type cdef		= cdefghij * 3518437209ULL >> 45;
			const v_type ghij		= cdefghij - (cdef * 10000ULL);
			const v_type klmn		= klmnopqr * 3518437209ULL >> 45;
			const v_type opqr		= klmnopqr - (klmn * 10000ULL);
			std::memcpy(buf, char_table_2_digit_data + ab, 2ULL);
			std::memcpy(buf + 2, char_table_4_digit_data + cdef, 4ULL);
			std::memcpy(buf + 6, char_table_4_digit_data + ghij, 4ULL);
			std::memcpy(buf + 10, char_table_4_digit_data + klmn, 4ULL);
			std::memcpy(buf + 14, char_table_4_digit_data + opqr, 4ULL);
			return buf + 18;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 19ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghijk = multiply_and_shift::impl(value);
			const v_type lmnopqrs	 = value - abcdefghijk * 100000000ULL;
			const v_type abc		 = multiply_and_shift::impl(abcdefghijk);
			const v_type defghijk	 = abcdefghijk - abc * 100000000ULL;
			const v_type defg		 = defghijk * 3518437209ULL >> 45;
			const v_type hijk		 = defghijk - (defg * 10000ULL);
			const v_type lmno		 = lmnopqrs * 3518437209ULL >> 45;
			const v_type pqrs		 = lmnopqrs - (lmno * 10000ULL);
			std::memcpy(buf, char_table_3_digit_data + abc, 3ULL);
			std::memcpy(buf + 3, char_table_4_digit_data + defg, 4ULL);
			std::memcpy(buf + 7, char_table_4_digit_data + hijk, 4ULL);
			std::memcpy(buf + 11, char_table_4_digit_data + lmno, 4ULL);
			std::memcpy(buf + 15, char_table_4_digit_data + pqrs, 4ULL);
			return buf + 19;
		}
	};

	template<uint64_types v_type> struct to_chars_internal<v_type, 20ULL> {
		inline static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			const v_type abcdefghijkl = multiply_and_shift::impl(value);
			const v_type mnopqrst	  = value - abcdefghijkl * 100000000ULL;
			const v_type abcd		  = multiply_and_shift::impl(abcdefghijkl);
			const v_type efghijkl	  = abcdefghijkl - abcd * 100000000ULL;
			const v_type efgh		  = efghijkl * 3518437209ULL >> 45;
			const v_type ijkl		  = efghijkl - (efgh * 10000ULL);
			const v_type mnop		  = mnopqrst * 3518437209ULL >> 45;
			const v_type qrst		  = mnopqrst - (mnop * 10000ULL);
			std::memcpy(buf, char_table_4_digit_data + abcd, 4ULL);
			std::memcpy(buf + 4, char_table_4_digit_data + efgh, 4ULL);
			std::memcpy(buf + 8, char_table_4_digit_data + ijkl, 4ULL);
			std::memcpy(buf + 12, char_table_4_digit_data + mnop, 4ULL);
			std::memcpy(buf + 16, char_table_4_digit_data + qrst, 4ULL);
			return buf + 20;
		}
	};

	template<uint64_types auto size, uint_types v_type>
	JSONIFIER_INLINE static string_buffer_ptr impl_internal(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
		return to_chars_internal<v_type, size>::impl(buf, value);
	}

	template<uint64_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 100000000ULL			  ? value < 10000ULL ? value < 100ULL ? value < 10U ? (static_cast<void>(buf[0] = char(value) + '0'), buf + 1)
																									: (static_cast<void>(std::memcpy(buf, char_table_2_digit_data + value, 2ULL)), buf + 2)
								  : value < 1000U ? (static_cast<void>(std::memcpy(buf, char_table_3_digit_data + value, 3ULL)), buf + 3)
																					  : (static_cast<void>(std::memcpy(buf, char_table_4_digit_data + value, 4ULL)), buf + 4)
							  : value < 1000000ULL ? value < 100000ULL ? impl_internal<5ULL>(buf, value) : impl_internal<6ULL>(buf, value)
							  : value < 10000000ULL ? impl_internal<7ULL>(buf, value)
																	 : impl_internal<8ULL>(buf, value)
						  : value < 1000000000000ULL ? value < 10000000000ULL ? value < 1000000000ULL ? impl_internal<9ULL>(buf, value) : impl_internal<10ULL>(buf, value)
							  : value < 100000000000ULL ? impl_internal<11ULL>(buf, value)
																			  : impl_internal<12ULL>(buf, value)
						  : value < 10000000000000000ULL ? value < 100000000000000ULL ? value < 10000000000000ULL ? impl_internal<13ULL>(buf, value) : impl_internal<14ULL>(buf, value)
							  : value < 1000000000000000ULL ? impl_internal<15ULL>(buf, value)
																					  : impl_internal<16ULL>(buf, value)
						  : value < 1000000000000000000ULL ? value < 100000000000000000ULL ? impl_internal<17ULL>(buf, value) : impl_internal<18ULL>(buf, value)
						  : value < 10000000000000000000ULL ? impl_internal<19ULL>(buf, value)
												  : impl_internal<20ULL>(buf, value);
		}
	};

	template<uint32_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 100000U	  ? value < 1000U ? value < 100U ? value < 10U ? (static_cast<void>(buf[0] = char(value) + '0'), buf + 1)
																				   : (static_cast<void>(std::memcpy(buf, char_table_2_digit_data + value, 2ULL)), buf + 2)
																	 : (static_cast<void>(std::memcpy(buf, char_table_3_digit_data + value, 3ULL)), buf + 3)
					   : value < 10000ULL ? (static_cast<void>(std::memcpy(buf, char_table_4_digit_data + value, 4ULL)), buf + 4)
													  : impl_internal<5ULL>(buf, value)
				   : value < 10000000U ? value < 1000000U ? impl_internal<6ULL>(buf, value) : impl_internal<7ULL>(buf, value)
				   : value < 1000000000U ? value < 100000000U ? impl_internal<8ULL>(buf, value) : impl_internal<9ULL>(buf, value)
										 : impl_internal<10ULL>(buf, value);
		}
	};

	template<uint16_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			return value < 1000U ? value < 100U ? value < 10U ? (static_cast<void>(buf[0] = char(value) + '0'), buf + 1)
															  : (static_cast<void>(std::memcpy(buf, char_table_2_digit_data + value, 2ULL)), buf + 2)
												: (static_cast<void>(std::memcpy(buf, char_table_3_digit_data + value, 3ULL)), buf + 3)
				: value < 10000ULL ? (static_cast<void>(std::memcpy(buf, char_table_4_digit_data + value, 4ULL)), buf + 4)
								 : impl_internal<5ULL>(buf, value);
		}
	};

	template<uint8_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static string_buffer_ptr impl(string_buffer_ptr __restrict buf, const v_type value) noexcept {
			return value < 100
				? value < 10 ? (static_cast<void>(buf[0] = char(value) + '0'), buf + 1) : (static_cast<void>(std::memcpy(buf, &char_table_2_digit_data[value], 2)), buf + 2)
				: (static_cast<void>(std::memcpy(buf, &char_table_3_digit_data[value], 3)), buf + 3);
		}
	};

	template<int_types v_type> struct to_chars<v_type> {
		JSONIFIER_INLINE static string_buffer_ptr impl_negative(string_buffer_ptr __restrict buf, const v_type value) noexcept {
			using unsigned_type					 = std::make_unsigned_t<v_type>;
			constexpr unsigned_type shift_amount = static_cast<unsigned_type>(sizeof(v_type) * 8ULL - 1ULL);
			*buf								 = '-';
			return to_chars<unsigned_type>::impl(buf + 1,
				static_cast<unsigned_type>(
					(static_cast<unsigned_type>(value) ^ static_cast<unsigned_type>(value >> shift_amount)) - static_cast<unsigned_type>(value >> shift_amount)));
		}

		JSONIFIER_INLINE static string_buffer_ptr impl(string_buffer_ptr __restrict buf JSONIFIER_LIFETIME_BOUND, const v_type value) noexcept {
			using unsigned_type					 = std::make_unsigned_t<v_type>;
			constexpr unsigned_type shift_amount = static_cast<unsigned_type>(sizeof(v_type) * 8ULL - 1ULL);
			return (value < 0) ? impl_negative(buf, value)
							   : to_chars<unsigned_type>::impl(buf,
									 static_cast<unsigned_type>(
										 (static_cast<unsigned_type>(value) ^ static_cast<unsigned_type>(value >> shift_amount)) - static_cast<unsigned_type>(value >> shift_amount)));
		}
	};

};// namespace internal
