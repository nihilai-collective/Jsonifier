// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/str_to_i.hpp
#pragma once

#include <jsonifier-incl/containers/allocator.hpp>
#include <jsonifier-incl/utilities/fast_float.hpp>
#include <jsonifier-incl/utilities/str_to_d.hpp>

namespace jsonifier::internal {

	template<bool negative> static constexpr uint64_t compValAddition{ [] {
		if constexpr (negative) {
			return 1ULL;
		} else {
			return 0ULL;
		}
	}() };

	template<typename v_type, bool negative> static constexpr array<std::make_unsigned_t<v_type>, 256> genRawCompVals() {
		constexpr auto max_value{ static_cast<std::make_unsigned_t<v_type>>(std::numeric_limits<std::remove_cvref_t<v_type>>::max()) + compValAddition<negative> };
		array<std::make_unsigned_t<v_type>, 256> returnValuesInternal{};
		returnValuesInternal['0'] = (max_value - 0) / 10;
		returnValuesInternal['1'] = (max_value - 1) / 10;
		returnValuesInternal['2'] = (max_value - 2) / 10;
		returnValuesInternal['3'] = (max_value - 3) / 10;
		returnValuesInternal['4'] = (max_value - 4) / 10;
		returnValuesInternal['5'] = (max_value - 5) / 10;
		returnValuesInternal['6'] = (max_value - 6) / 10;
		returnValuesInternal['7'] = (max_value - 7) / 10;
		returnValuesInternal['8'] = (max_value - 8) / 10;
		returnValuesInternal['9'] = (max_value - 9) / 10;
		return returnValuesInternal;
	};

	template<typename v_type, bool negative> alignas(64) static constexpr array<std::make_unsigned_t<v_type>, 256> rawCompVals{ genRawCompVals<v_type, negative>() };

	template<typename v_type, bool negative> alignas(64) static constexpr const std::make_unsigned_t<v_type>* __restrict compVals{ rawCompVals<v_type, negative>.data() };

	template<typename = void> struct pow_tables {
		alignas(64) static constexpr uint64_t powerOfTenUint[]{ 1ull, 10ull, 100ull, 1000ull, 10000ull, 100000ull, 1000000ull, 10000000ull, 100000000ull, 1000000000ull,
			10000000000ull, 100000000000ull, 1000000000000ull, 10000000000000ull, 100000000000000ull, 1000000000000000ull, 10000000000000000ull, 100000000000000000ull,
			1000000000000000000ull, 10000000000000000000ull };

		alignas(64) static constexpr int64_t powerOfTenInt[]{ 1ll, 10ll, 100ll, 1000ll, 10000ll, 100000ll, 1000000ll, 10000000ll, 100000000ll, 1000000000ll, 10000000000ll,
			100000000000ll, 1000000000000ll, 10000000000000ll, 100000000000000ll, 1000000000000000ll, 10000000000000000ll, 100000000000000000ll, 1000000000000000000ll };
	};

	template<typename value_type> struct integer_parser;

	template<int_types value_type> struct integer_parser<value_type> : public pow_tables<>, public exp_tables<> {
		constexpr integer_parser() noexcept = default;

		JSONIFIER_INLINE static value_type mul128Generic(value_type ab, value_type cd, value_type& hi) noexcept {
			value_type aHigh = ab >> 32;
			value_type aLow	 = ab & 0xFFFFFFFF;
			value_type bHigh = cd >> 32;
			value_type bLow	 = cd & 0xFFFFFFFF;
			value_type loLo	 = aLow * bLow;
			value_type loHi	 = aLow * bHigh;
			value_type hiLo	 = aHigh * bLow;
			value_type hiHi	 = aHigh * bHigh;
			value_type cross = (loLo >> 32) + (loHi & 0xFFFFFFFF) + (hiLo & 0xFFFFFFFF);
			value_type lo	 = (cross << 32) | (loLo & 0xFFFFFFFF);
			hi				 = hiHi + (loHi >> 32) + (hiLo >> 32) + (cross >> 32);
			return lo;
		}

		JSONIFIER_INLINE static bool multiply(value_type& value, value_type expValue) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			const __int128_t res = static_cast<__int128_t>(value) * static_cast<__int128_t>(expValue);
			value				 = static_cast<value_type>(res);
			return res <= std::numeric_limits<value_type>::max();
#elif JSONIFIER_COMPILER_MSVC
			value_type values;
			value = _mul128(value, expValue, &values);
			return values == 0;
#else
			value_type values;
			value = mul128Generic(value, expValue, &values);
			return values == 0;
#endif
		}

		JSONIFIER_INLINE static bool divide(value_type& value, value_type expValue) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			const __int128_t dividend = static_cast<__int128_t>(value);
			value					  = static_cast<value_type>(dividend / static_cast<__int128_t>(expValue));
			return (dividend % static_cast<__int128_t>(expValue)) == 0;
#elif JSONIFIER_COMPILER_MSVC
			value_type values;
			value = _div128(0, value, expValue, &values);
			return values == 0;
#else
			value_type values;
			values = value % expValue;
			value  = value / expValue;
			return values == 0;
#endif
		}

		JSONIFIER_INLINE static const uint8_t* parseFraction(value_type& value, const uint8_t* iter) noexcept {
			if (is_digit(*iter)) [[likely]] {
				value_type fracValue{ static_cast<value_type>(*iter - static_cast<uint8_t>('0')) };
				typename get_int_type<value_type>::type fracDigits{ 1 };
				++iter;
				while (is_digit(*iter)) {
					fracValue = fracValue * 10 + static_cast<value_type>(*iter - static_cast<uint8_t>('0'));
					++iter;
					++fracDigits;
				}
				if (expTable[*iter]) {
					++iter;
					int8_t expSign = 1;
					if (*iter == minus) {
						expSign = -1;
						++iter;
					} else if (*iter == plus) {
						++iter;
					}
					return parseExponentPostFrac(value, iter, expSign, fracValue, fracDigits);
				}
			}
			if (!expFracTable[*iter]) [[likely]] {
				return iter;
			} else {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static const uint8_t* parseExponentPostFrac(value_type& value, const uint8_t* iter, int8_t expSign, value_type fracValue,
			typename get_int_type<value_type>::type fracDigits) noexcept {
			if (is_digit(*iter)) [[likely]] {
				value_type expValue{ static_cast<value_type>(*iter - static_cast<uint8_t>('0')) };
				++iter;
				while (is_digit(*iter)) {
					expValue = expValue * 10 + static_cast<value_type>(*iter - static_cast<uint8_t>('0'));
					++iter;
				}
				if (expValue < 19) [[likely]] {
					const value_type powerExp = powerOfTenInt[expValue];

					constexpr value_type doubleMax = std::numeric_limits<value_type>::max();
					constexpr value_type doubleMin = std::numeric_limits<value_type>::min();

					if (fracDigits + expValue >= 0) {
						expValue *= expSign;
						const auto fractionalCorrection =
							expValue > fracDigits ? fracValue * powerOfTenInt[expValue - fracDigits] : fracValue / powerOfTenInt[fracDigits - expValue];
						return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), value += fractionalCorrection, iter) : nullptr)
											 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), value += fractionalCorrection, iter) : nullptr);
					} else {
						return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), iter) : nullptr)
											 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), iter) : nullptr);
					}
				} else [[unlikely]] {
					return nullptr;
				}
			} else [[unlikely]] {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static const uint8_t* parseExponent(value_type& value, const uint8_t* iter, int8_t expSign) noexcept {
			if (is_digit(*iter)) [[likely]] {
				value_type expValue{ static_cast<value_type>(*iter - static_cast<uint8_t>('0')) };
				++iter;
				while (is_digit(*iter)) {
					expValue = expValue * 10 + static_cast<value_type>(*iter - static_cast<uint8_t>('0'));
					++iter;
				}
				if (expValue < 19) [[likely]] {
					const value_type powerExp	   = powerOfTenInt[expValue];
					constexpr value_type doubleMax = std::numeric_limits<value_type>::max();
					constexpr value_type doubleMin = std::numeric_limits<value_type>::min();
					expValue *= expSign;
					return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), iter) : nullptr)
										 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), iter) : nullptr);
				} else [[unlikely]] {
					return nullptr;
				}
			} else [[unlikely]] {
				return nullptr;
			}
		}

		static const uint8_t* finishParse(value_type& value, const uint8_t* iter) noexcept {
			if (*iter == decimal) [[unlikely]] {
				++iter;
				return parseFraction(value, iter);
			} else if (expTable[*iter]) {
				++iter;
				int8_t expSign = 1;
				if (*iter == minus) {
					expSign = -1;
					++iter;
				} else if (*iter == plus) {
					++iter;
				}
				return parseExponent(value, iter, expSign);
			}
			if (!expFracTable[*iter]) [[likely]] {
				return nullptr;
			} else {
				return nullptr;
			}
		}

		template<bool negative> JSONIFIER_INLINE static const uint8_t* parseInteger(value_type& value, const uint8_t* iter) noexcept {
			using v_type_local = std::make_unsigned_t<value_type>;
			uint8_t numTmp{ *iter };
			if (is_digit(numTmp)) [[likely]] {
				value = numTmp - static_cast<uint8_t>('0');
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				return nullptr;
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (iter[-2] == static_cast<uint8_t>('0')) [[unlikely]] {
				return nullptr;
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}

			if (is_digit(numTmp)) [[likely]] {
				if (static_cast<uint64_t>(value) > static_cast<uint64_t>(compVals<value_type, negative>[numTmp])) [[unlikely]] {
					return nullptr;
				}
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					if constexpr (negative) {
						value *= -1;
					}
					return iter;
				}
				if constexpr (negative) {
					return (iter = finishParse(value, iter), value *= -1, iter);
				} else {
					return finishParse(value, iter);
				}
			}
			alignas(64) static constexpr v_type_local zero_val{ 0 };
			if constexpr (negative) {
				value = static_cast<value_type>(zero_val - static_cast<v_type_local>(value));
			} else {
				value = static_cast<value_type>(value);
			}
			return iter;
		}

		JSONIFIER_INLINE static string_view_ptr parseInt(value_type& value, string_view_ptr iter, string_view_ptr end) noexcept {
			if (iter < end) [[likely]] {
				if (*iter == minus) {
					++iter;
					const uint8_t* resultPtr = parseInteger<true>(value, std::bit_cast<const uint8_t*>(iter));
					if (resultPtr) [[likely]] {
						iter += resultPtr - std::bit_cast<const uint8_t*>(iter);
						return iter;
					} else {
						value = 0;
						return nullptr;
					}
				} else {
					const uint8_t* resultPtr = parseInteger<false>(value, std::bit_cast<const uint8_t*>(iter));
					if (resultPtr) [[likely]] {
						iter += resultPtr - std::bit_cast<const uint8_t*>(iter);
						return iter;
					} else {
						value = 0;
						return nullptr;
					}
				}
			} else {
				value = 0;
				return nullptr;
			}
		}
	};

	template<uint_types value_type> struct integer_parser<value_type> : public pow_tables<>, public exp_tables<> {
		constexpr integer_parser() noexcept = default;

		JSONIFIER_INLINE static value_type umul128Generic(value_type ab, value_type cd, value_type& hi) noexcept {
			value_type aHigh = ab >> 32;
			value_type aLow	 = ab & 0xFFFFFFFF;
			value_type bHigh = cd >> 32;
			value_type bLow	 = cd & 0xFFFFFFFF;
			value_type loLo	 = aLow * bLow;
			value_type loHi	 = aLow * bHigh;
			value_type hiLo	 = aHigh * bLow;
			value_type hiHi	 = aHigh * bHigh;
			value_type cross = (loLo >> 32) + (loHi & 0xFFFFFFFF) + (hiLo & 0xFFFFFFFF);
			value_type lo	 = (cross << 32) | (loLo & 0xFFFFFFFF);
			hi				 = hiHi + (loHi >> 32) + (hiLo >> 32) + (cross >> 32);
			return lo;
		}

		JSONIFIER_INLINE static bool multiply(value_type& value, value_type expValue) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			const __uint128_t res = static_cast<__uint128_t>(value) * static_cast<__uint128_t>(expValue);
			value				  = static_cast<value_type>(res);
			return res <= std::numeric_limits<value_type>::max();
#elif JSONIFIER_COMPILER_MSVC
			value_type values;
			value = _umul128(value, expValue, &values);
			return values == 0;
#else
			value_type values;
			value = umul128Generic(value, expValue, &values);
			return values == 0;
#endif
		}

		JSONIFIER_INLINE static bool divide(value_type& value, value_type expValue) noexcept {
#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
			const __uint128_t dividend = static_cast<__uint128_t>(value);
			value					   = static_cast<value_type>(dividend / static_cast<__uint128_t>(expValue));
			return (dividend % static_cast<__uint128_t>(expValue)) == 0;
#elif JSONIFIER_COMPILER_MSVC
			value_type values;
			value = _udiv128(0, value, expValue, &values);
			return values == 0;
#else
			value_type values;
			values = value % expValue;
			value  = value / expValue;
			return values == 0;
#endif
		}

		JSONIFIER_INLINE static const uint8_t* parseFraction(value_type& value, const uint8_t* iter) noexcept {
			if (is_digit(*iter)) [[likely]] {
				value_type fracValue{ static_cast<value_type>(*iter - static_cast<uint8_t>('0')) };
				typename get_int_type<value_type>::type fracDigits{ 1 };
				++iter;
				while (is_digit(*iter)) {
					fracValue = fracValue * 10 + static_cast<value_type>(*iter - static_cast<uint8_t>('0'));
					++iter;
					++fracDigits;
				}
				if (expTable[*iter]) {
					++iter;
					int8_t expSign = 1;
					if (*iter == minus) {
						expSign = -1;
						++iter;
					} else if (*iter == plus) {
						++iter;
					}
					return parseExponentPostFrac(value, iter, expSign, fracValue, fracDigits);
				}
			}
			if (!expFracTable[*iter]) [[likely]] {
				return iter;
			} else {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static const uint8_t* parseExponentPostFrac(value_type& value, const uint8_t* iter, int8_t expSign, value_type fracValue,
			typename get_int_type<value_type>::type fracDigits) noexcept {
			if (is_digit(*iter)) [[likely]] {
				int64_t expValue{ *iter - static_cast<uint8_t>('0') };
				++iter;
				while (is_digit(*iter)) {
					expValue = expValue * 10 + *iter - static_cast<uint8_t>('0');
					++iter;
				}
				if (expValue <= 19) [[likely]] {
					const value_type powerExp = powerOfTenUint[expValue];

					constexpr value_type doubleMax = std::numeric_limits<value_type>::max();
					constexpr value_type doubleMin = std::numeric_limits<value_type>::min();

					if (fracDigits + expValue >= 0) {
						expValue *= expSign;
						const auto fractionalCorrection =
							expValue > fracDigits ? fracValue * powerOfTenUint[expValue - fracDigits] : fracValue / powerOfTenUint[fracDigits - expValue];
						return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), value += fractionalCorrection, iter) : nullptr)
											 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), value += fractionalCorrection, iter) : nullptr);
					} else {
						return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), iter) : nullptr)
											 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), iter) : nullptr);
					}
				} else [[unlikely]] {
					return nullptr;
				}
			} else [[unlikely]] {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static const uint8_t* parseExponent(value_type& value, const uint8_t* iter, int8_t expSign) noexcept {
			if (is_digit(*iter)) [[likely]] {
				value_type expValue{ static_cast<value_type>(*iter - static_cast<uint8_t>('0')) };
				++iter;
				while (is_digit(*iter)) {
					expValue = expValue * 10 + static_cast<value_type>(*iter - static_cast<uint8_t>('0'));
					++iter;
				}
				if (expValue <= 19) [[likely]] {
					const value_type powerExp	   = powerOfTenUint[expValue];
					constexpr value_type doubleMax = std::numeric_limits<value_type>::max();
					constexpr value_type doubleMin = std::numeric_limits<value_type>::min();
					expValue *= static_cast<value_type>(expSign);
					return (expSign > 0) ? ((value <= (doubleMax / powerExp)) ? (multiply(value, powerExp), iter) : nullptr)
										 : ((value / powerExp >= (doubleMin)) ? (divide(value, powerExp), iter) : nullptr);
				} else [[unlikely]] {
					return nullptr;
				}
			} else [[unlikely]] {
				return nullptr;
			}
		}

		static const uint8_t* finishParse(value_type& value, const uint8_t* iter) noexcept {
			if (*iter == decimal) [[unlikely]] {
				++iter;
				return parseFraction(value, iter);
			} else if (expTable[*iter]) {
				++iter;
				int8_t expSign = 1;
				if (*iter == minus) {
					expSign = -1;
					++iter;
				} else if (*iter == plus) {
					++iter;
				}
				return parseExponent(value, iter, expSign);
			}
			if (!expFracTable[*iter]) [[likely]] {
				return iter;
			} else {
				return nullptr;
			}
		}

		JSONIFIER_INLINE static const uint8_t* parseInteger(value_type& value, const uint8_t* iter) noexcept {
			using v_type_local = std::make_unsigned_t<value_type>;
			uint8_t numTmp{ *iter };
			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(numTmp - static_cast<uint8_t>('0'));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				return nullptr;
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (iter[-2] == static_cast<uint8_t>('0')) [[unlikely]] {
				return nullptr;
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				if (static_cast<uint64_t>(value) > static_cast<uint64_t>(compVals<value_type, false>[numTmp])) [[unlikely]] {
					return nullptr;
				}
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
				return finishParse(value, iter);
			}

			if (is_digit(numTmp)) [[likely]] {
				value = static_cast<value_type>(static_cast<v_type_local>(value) * 10 + (numTmp - static_cast<uint8_t>('0')));
				++iter;
				numTmp = *iter;
			} else [[unlikely]] {
				if (!expFracTable[numTmp]) [[likely]] {
					return iter;
				}
			}
			return nullptr;
		}

		JSONIFIER_INLINE static string_view_ptr parseInt(value_type& value, string_view_ptr iter, string_view_ptr end) noexcept {
			if (iter < end) [[likely]] {
				const uint8_t* resultPtr = parseInteger(value, std::bit_cast<const uint8_t*>(iter));
				if (resultPtr) [[likely]] {
					iter += resultPtr - std::bit_cast<const uint8_t*>(iter);
					return iter;
				} else {
					value = 0;
					return nullptr;
				}
			} else {
				value = 0;
				return nullptr;
			}
		}
	};
}
