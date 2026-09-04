// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/compare.hpp
#pragma once

#include <jsonifier-incl/utilities/string_literal.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/avx.hpp>
#include <jsonifier-incl/simd/neon.hpp>
#include <jsonifier-incl/simd/sve2.hpp>
#include <jsonifier-incl/simd/fallback.hpp>

namespace jsonifier::internal {

	template<const uint8_t repeat, uint_types return_type> static constexpr return_type repeatByte() noexcept {
		return static_cast<return_type>(0x0101010101010101ull) * static_cast<return_type>(repeat);
	}

	template<char valueNewer, typename char_type> struct char_comparison {
		static constexpr char value{ valueNewer };
		JSONIFIER_INLINE static const char_type* memchar(const char_type* data, uint64_t lengthNew) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
			if (lengthNew >= 64) {
				using type_list_element				 = type_list_element_t<0, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				using integer_type					 = typename type_list_element::integer_type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				const simd_type search_value		 = simd::gatherValue<simd_type>(static_cast<uint8_t>(value));
				integer_type mask;
				simd_type chunk;
				while (lengthNew >= vectorSize) {
					chunk = simd::gatherValuesU<simd_type>(data);
					mask  = simd::opCmpEq(chunk, search_value);
					if (mask != 0) [[unlikely]] {
						data += simd::postCmpTzcntUnsafe(mask);
						return data;
					}
					lengthNew -= vectorSize;
					data += vectorSize;
				}
			}
#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
			if (lengthNew >= 32) {
				using type_list_element				 = type_list_element_t<1, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				using integer_type					 = typename type_list_element::integer_type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				const simd_type search_value		 = simd::gatherValue<simd_type>(static_cast<uint8_t>(value));
				integer_type mask;
				simd_type chunk;
				while (lengthNew >= vectorSize) {
					chunk = simd::gatherValuesU<simd_type>(data);
					mask  = simd::opCmpEq(chunk, search_value);
					if (mask != 0) [[unlikely]] {
						data += simd::postCmpTzcntUnsafe(mask);
						return data;
					}
					lengthNew -= vectorSize;
					data += vectorSize;
				}
			}
#endif
			if (lengthNew >= 16) {
				using type_list_element				 = type_list_element_t<2, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				using integer_type					 = typename type_list_element::integer_type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				const simd_type search_value		 = simd::gatherValue<simd_type>(static_cast<uint8_t>(value));
				integer_type mask;
				simd_type chunk;
				while (lengthNew >= vectorSize) {
					chunk = simd::gatherValuesU<simd_type>(data);
					mask  = static_cast<integer_type>(simd::opCmpEq(chunk, search_value));
					if (mask != 0) [[unlikely]] {
						data += simd::postCmpTzcntUnsafe(mask);
						return data;
					}
					lengthNew -= vectorSize;
					data += vectorSize;
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint64_t) };
				if (lengthNew >= nBytes) {
					static constexpr auto valueNew{ repeatByte<value, uint64_t>() };
					static constexpr auto highBits{ repeatByte<0x80, uint64_t>() };
					static constexpr auto lowBits{ repeatByte<0x01, uint64_t>() };
					uint64_t simdValue;
					std::memcpy(&simdValue, data, sizeof(uint64_t));
					const auto chunk = simdValue ^ valueNew;
					auto next		 = ((chunk - lowBits) & ~chunk) & highBits;
					if (next) {
						next = simd::countrZeroUnsafe(static_cast<uint64_t>(next)) >> 3u;
						data += next;
						return data;
					} else {
						lengthNew -= nBytes;
						data += nBytes;
					}
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint32_t) };
				if (lengthNew >= nBytes) {
					static constexpr auto valueNew{ repeatByte<value, uint32_t>() };
					static constexpr auto highBits{ repeatByte<0x80, uint32_t>() };
					static constexpr auto lowBits{ repeatByte<0x01, uint32_t>() };
					uint32_t simdValue;
					std::memcpy(&simdValue, data, sizeof(uint32_t));
					const auto chunk = simdValue ^ valueNew;
					auto next		 = ((chunk - lowBits) & ~chunk) & highBits;
					if (next) {
						next = static_cast<uint32_t>(simd::countrZeroUnsafe(static_cast<uint32_t>(next)) >> 3u);
						data += next;
						return data;
					} else {
						lengthNew -= nBytes;
						data += nBytes;
					}
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint16_t) };
				if (lengthNew >= nBytes) {
					static constexpr auto valueNew{ repeatByte<value, uint16_t>() };
					static constexpr auto highBits{ repeatByte<0x80, uint16_t>() };
					static constexpr auto lowBits{ repeatByte<0x01, uint16_t>() };
					uint16_t simdValue;
					std::memcpy(&simdValue, data, sizeof(uint16_t));
					const auto chunk = simdValue ^ valueNew;
					auto next		 = ((chunk - lowBits) & ~chunk) & highBits;
					if (next) {
						next = static_cast<int32_t>(simd::countrZeroUnsafe(static_cast<uint16_t>(next)) >> 3u);
						data += next;
						return data;
					} else {
						lengthNew -= nBytes;
						data += nBytes;
					}
				}
			}

			if (lengthNew > 0 && *data == value) {
				return data;
			}
			return static_cast<decltype(data)>(nullptr);
		}
	};

	struct comparison {
		template<typename char_type01, typename char_type02> JSONIFIER_INLINE static bool compare(const char_type01* lhs, char_type02* rhs, uint64_t lengthNew) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
			if (lengthNew >= 64) {
				using type_list_element				 = type_list_element_t<0, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				static constexpr uint64_t mask		 = type_list_element::mask;
				simd_type value01, value02;
				while (lengthNew >= vectorSize) {
					value01 = simd::gatherValuesU<simd_type>(lhs);
					value02 = simd::gatherValuesU<simd_type>(rhs);
					if (simd::opCmpEq(value01, value02) != mask) {
						return false;
					}
					lengthNew -= vectorSize;
					lhs += vectorSize;
					rhs += vectorSize;
				}
			}
#endif
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
			if (lengthNew >= 32) {
				using type_list_element				 = type_list_element_t<1, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				static constexpr uint64_t mask		 = type_list_element::mask;
				simd_type value01, value02;
				while (lengthNew >= vectorSize) {
					value01 = simd::gatherValuesU<simd_type>(lhs);
					value02 = simd::gatherValuesU<simd_type>(rhs);
					if (simd::opCmpEq(value01, value02) != mask) {
						return false;
					}
					lengthNew -= vectorSize;
					lhs += vectorSize;
					rhs += vectorSize;
				}
			}
#endif
			if (lengthNew >= 16) {
				using type_list_element				 = type_list_element_t<2, simd::avx_list>;
				using simd_type						 = typename type_list_element::type::type;
				static constexpr uint64_t vectorSize = type_list_element::bytesProcessed;
				static constexpr uint64_t mask		 = type_list_element::mask;
				simd_type value01, value02;
				while (lengthNew >= vectorSize) {
					value01 = simd::gatherValuesU<simd_type>(lhs);
					value02 = simd::gatherValuesU<simd_type>(rhs);
					if (simd::opCmpEq(value01, value02) != mask) {
						return false;
					}
					lengthNew -= vectorSize;
					lhs += vectorSize;
					rhs += vectorSize;
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint64_t) };
				if (lengthNew >= nBytes) {
					uint64_t v1, v2;
					std::memcpy(&v1, lhs, nBytes);
					std::memcpy(&v2, rhs, nBytes);
					if ((v1 ^ v2) != 0) {
						return false;
					}
					lengthNew -= nBytes;
					lhs += nBytes;
					rhs += nBytes;
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint32_t) };
				if (lengthNew >= nBytes) {
					uint32_t v1, v2;
					std::memcpy(&v1, lhs, nBytes);
					std::memcpy(&v2, rhs, nBytes);
					if ((v1 ^ v2) != 0) {
						return false;
					}
					lengthNew -= nBytes;
					lhs += nBytes;
					rhs += nBytes;
				}
			}
			{
				static constexpr uint64_t nBytes{ sizeof(uint16_t) };
				if (lengthNew >= nBytes) {
					uint16_t v1, v2;
					std::memcpy(&v1, lhs, nBytes);
					std::memcpy(&v2, rhs, nBytes);
					if ((v1 ^ v2) != 0) {
						return false;
					}
					lengthNew -= nBytes;
					lhs += nBytes;
					rhs += nBytes;
				}
			}
			if (lengthNew && *lhs != *rhs) {
				return false;
			}
			return true;
		}
	};

	template<uint64_t size> static constexpr uint64_t getPackingSize() {
		if constexpr (size >= 64) {
			return 64;
		} else if constexpr (size >= 32) {
			return 32;
		} else {
			return 16;
		}
	}

	template<uint64_t length> struct convert_length_to_int {
		using type = jsonifier::internal::conditional_t<length == 1, uint8_t,
			jsonifier::internal::conditional_t<length <= 2, uint16_t,
				jsonifier::internal::conditional_t<length <= 4, uint32_t, jsonifier::internal::conditional_t<length <= 8, uint64_t, void>>>>;
	};

	template<uint64_t length> using convert_length_to_int_t = typename convert_length_to_int<length>::type;

	template<string_literal string>
		requires(string.length == 0)
	static constexpr auto packValues() {
		return uint8_t{};
	}

	template<uint64_t length> static constexpr uint64_t get_int_length_v = []<uint64_t lengthNew> {
		if constexpr (lengthNew == 1ULL) {
			return 1ULL;
		} else if constexpr (lengthNew == 2ULL) {
			return 2ULL;
		} else if constexpr (lengthNew <= 4ULL && lengthNew > 2ULL) {
			return 4ULL;
		} else if constexpr (lengthNew <= 8ULL && lengthNew > 4ULL) {
			return 8ULL;
		} else if constexpr (lengthNew <= 16ULL && lengthNew > 8ULL) {
			return 16ULL;
		} else {
			return lengthNew;
		}
	}.template operator()<length>();

	template<auto string> struct pack_values;

	template<gt_0_lte_8 auto string> struct pack_values<string> {
		static constexpr decltype(auto) gen() {
			using int_type = convert_length_to_int_t<string.length>;
			int_type returnValues{};
			for (uint64_t x = 0; x < string.length; ++x) {
				if constexpr (std::endian::native == std::endian::little) {
					returnValues |= static_cast<int_type>(static_cast<uint64_t>(static_cast<uint8_t>(string[x])) << ((x % 8) * 8));
				} else {
					returnValues |= static_cast<int_type>(static_cast<uint64_t>(static_cast<uint8_t>(string[x])) << (((sizeof(int_type) - 1 - (x % 8))) * 8));
				}
			}
			return returnValues;
		}
		alignas(64) static constexpr auto value{ gen() };
	};

	template<gt_8 auto string> struct pack_values<string> {
		using array_type = array<uint8_t, get_int_length_v<string.size()>>;
		static constexpr array_type gen() {
			array_type out_local{};
			for (uint64_t i = 0; i < string.size(); ++i)
				out_local[i] = static_cast<uint8_t>(string[i]);
			return out_local;
		}
		alignas(64) static constexpr auto value{ gen() };
	};

	struct string_status {
		uint64_t remaining_length{};
		uint64_t currentIndex{};
	};

	template<auto string, uint64_t offset> static constexpr string_literal<((offset >= string.length) ? string.length : offset) + 1> offSetIntoLiteral() noexcept {
		constexpr uint64_t newSize = ((offset >= string.length) ? string.length : offset);
		string_literal<newSize + 1> sl{};
		if constexpr (newSize > 0) {
			std::copy_n(string.data(), newSize, sl.values);
			sl.values[newSize] = '\0';
		}
		return sl;
	}

	template<auto string, uint64_t offset> static constexpr string_literal<((offset >= string.length) ? 0 : string.length - offset) + 1> offSetNewLiteral() noexcept {
		constexpr uint64_t newSize = (offset >= string.length) ? 0 : string.length - offset;
		string_literal<newSize + 1> sl{};
		if constexpr (newSize > 0) {
			std::copy_n(string.data() + offset, newSize, sl.values);
			sl.values[newSize] = '\0';
		}
		return sl;
	}

	template<uint64_t split_mod, uint64_t size> static consteval uint64_t getSplitStringCount(const string_literal<size>&) noexcept {
		constexpr uint64_t split_count = (size + split_mod - 1ull) / split_mod;
		return split_count == 0ull ? 1ull : split_count;
	}

	template<string_status status, auto stringNew, uint64_t split_mod> struct string_literal_splitter_impl {
		static constexpr uint64_t current_length{ split_mod < status.remaining_length ? split_mod : status.remaining_length };
		static constexpr auto string{ offSetIntoLiteral<offSetNewLiteral<stringNew, status.currentIndex>(), current_length>() };
	};

	template<typename sequence, auto stringNew, uint64_t split_mod> struct string_literal_splitter;

	template<uint64_t index, typename string_type> static constexpr string_literal<index + 1> stringLiteralFromView(string_type str) noexcept {
		string_literal<index + 1> sl{};
		std::copy_n(str.data(), str.size(), sl.values);
		sl[index] = '\0';
		return sl;
	}

	static constexpr auto getOffsetIntoLiteralSize(uint64_t inputSize) noexcept {
		if (inputSize >= 64ULL && simdBytesPerRegister >= 64ULL) {
			return 64ULL;
		} else if (inputSize >= 32ULL && simdBytesPerRegister >= 32ULL) {
			return 32ULL;
		} else {
			return 16ULL;
		}
	}

	template<typename sl_type, base_t<sl_type> stringNew, typename = void> struct string_literal_comparator_impl;

	template<uint64_t... indices, auto stringNew, uint64_t split_mod> struct string_literal_splitter<integer_sequence<indices...>, stringNew, split_mod> {
		static consteval string_status get_split_string_status(uint64_t index, uint64_t total_length) noexcept {
			uint64_t currentIndex{ split_mod * index };
			string_status status{ total_length, currentIndex };
			status.remaining_length -= currentIndex;
			return status;
		}

		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			return (static_cast<void>(
						((str = string_literal_comparator_impl<decltype(string_literal_splitter_impl<get_split_string_status(indices, stringNew.size()), stringNew, split_mod>::string),
							  string_literal_splitter_impl<get_split_string_status(indices, stringNew.size()), stringNew, split_mod>::string>::impl(str),
							 str != nullptr) &&
							...)),
				str);
		}
	};

	template<equals_0 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			return str;
		}
	};

	template<gt_0_lt_16 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			static constexpr auto stringLiteral{ stringNew };
			static constexpr auto newCount{ stringLiteral.size() };
			if constexpr (newCount > 8) {
				alignas(64) static constexpr auto valuesNew{ pack_values<stringLiteral>::value };
				jsonifier_simd_int_128 data1{};
				std::memcpy(&data1, str, newCount);
				const jsonifier_simd_int_128 data2{ simd::gatherValues<jsonifier_simd_int_128>(valuesNew.data()) };
				return simd::opTest(simd::opXor(data1, data2)) ? str + newCount : nullptr;
			} else if constexpr (newCount == 8) {
				static constexpr uint64_t valuesNew{ pack_values<stringLiteral>::value };
				uint64_t l;
				std::memcpy(&l, str, 8);
				return !(l ^ valuesNew) ? str + newCount : nullptr;
			} else if constexpr (newCount == 7) {
				static constexpr auto loString = stringLiteral.template substr<0, 4>();
				static constexpr auto hiString = stringLiteral.template substr<3, 4>();
				static constexpr uint32_t loValuesNew{ pack_values<loString>::value };
				static constexpr uint32_t hiValuesNew{ pack_values<hiString>::value };
				uint32_t lo, hi;
				std::memcpy(&lo, str, 4);
				std::memcpy(&hi, str + 3, 4);
				return !((lo ^ loValuesNew) | (hi ^ hiValuesNew)) ? str + newCount : nullptr;
			} else if constexpr (newCount == 6) {
				static constexpr auto loString = stringLiteral.template substr<0, 4>();
				static constexpr auto hiString = stringLiteral.template substr<2, 4>();
				static constexpr uint32_t loValuesNew{ pack_values<loString>::value };
				static constexpr uint32_t hiValuesNew{ pack_values<hiString>::value };
				uint32_t lo, hi;
				std::memcpy(&lo, str, 4);
				std::memcpy(&hi, str + 2, 4);
				return !((lo ^ loValuesNew) | (hi ^ hiValuesNew)) ? str + newCount : nullptr;
			} else if constexpr (newCount == 5) {
				static constexpr auto loString = stringLiteral.template substr<0, 4>();
				static constexpr auto hiString = stringLiteral.template substr<1, 4>();
				static constexpr uint32_t loValuesNew{ pack_values<loString>::value };
				static constexpr uint32_t hiValuesNew{ pack_values<hiString>::value };
				uint32_t lo, hi;
				std::memcpy(&lo, str, 4);
				std::memcpy(&hi, str + 1, 4);
				return !((lo ^ loValuesNew) | (hi ^ hiValuesNew)) ? str + newCount : nullptr;
			} else if constexpr (newCount == 4) {
				static constexpr uint32_t valuesNew{ pack_values<stringLiteral>::value };
				uint32_t l;
				std::memcpy(&l, str, 4);
				return !(l ^ valuesNew) ? str + newCount : nullptr;
			} else if constexpr (newCount == 3) {
				static constexpr auto loString = stringLiteral.template substr<0, 2>();
				static constexpr auto hiString = stringLiteral.template substr<1, 2>();
				static constexpr uint16_t loValuesNew{ pack_values<loString>::value };
				static constexpr uint16_t hiValuesNew{ pack_values<hiString>::value };
				uint16_t lo, hi;
				std::memcpy(&lo, str, 2);
				std::memcpy(&hi, str + 1, 2);
				return !((lo ^ loValuesNew) | (hi ^ hiValuesNew)) ? str + newCount : nullptr;
			} else if constexpr (newCount == 2) {
				static constexpr uint16_t valuesNew{ pack_values<stringLiteral>::value };
				uint16_t l;
				std::memcpy(&l, str, 2);
				return !(l ^ valuesNew) ? str + newCount : nullptr;
			} else if constexpr (newCount == 1) {
				return (*str == stringLiteral[0]) ? str + newCount : nullptr;
			} else {
				return str;
			}
		}
	};

	template<eq_16 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			static constexpr auto newLiteral{ stringNew };
			alignas(64) static constexpr auto valuesNew{ pack_values<newLiteral>::value };
			const jsonifier_simd_int_128 data1{ simd::gatherValuesU<jsonifier_simd_int_128>(str) };
			const jsonifier_simd_int_128 data2{ simd::gatherValues<jsonifier_simd_int_128>(valuesNew.data()) };
			return simd::opTest(simd::opXor(data1, data2)) ? str + 16 : nullptr;
		}
	};

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)

	template<eq_32 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			static constexpr auto newLiteral{ stringNew };
			alignas(64) static constexpr auto valuesNew{ pack_values<newLiteral>::value };
			const jsonifier_simd_int_256 data1{ simd::gatherValuesU<jsonifier_simd_int_256>(str) };
			const jsonifier_simd_int_256 data2{ simd::gatherValues<jsonifier_simd_int_256>(valuesNew.data()) };
			return simd::opTest(simd::opXor(data1, data2)) ? str + 32 : nullptr;
		}
	};

#endif

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
	template<eq_64 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			static constexpr auto newLiteral{ stringNew };
			alignas(64) static constexpr auto valuesNew{ pack_values<newLiteral>::value };
			const jsonifier_simd_int_512 data1{ simd::gatherValuesU<jsonifier_simd_int_512>(str) };
			const jsonifier_simd_int_512 data2{ simd::gatherValues<jsonifier_simd_int_512>(valuesNew.data()) };
			return simd::opCmpEq(data1, data2) == 0xFFFFFFFFFFFFFFFFULL ? str + 64 : nullptr;
		}
	};
#endif

	template<gt_16 sl_type, base_t<sl_type> stringNew> struct string_literal_comparator_impl<sl_type, stringNew, void> {
		static constexpr uint64_t split_mod{ getOffsetIntoLiteralSize(stringNew.size()) };
		static constexpr auto string_count{ getSplitStringCount<split_mod>(stringNew) };

		JSONIFIER_INLINE static string_view_ptr impl(string_view_ptr str) noexcept {
			return string_literal_splitter<make_integer_sequence<string_count>, stringNew, split_mod>::impl(str);
		}
	};

	template<auto string> struct string_literal_comparator {
		JSONIFIER_INLINE static bool impl(string_view_ptr str, uint64_t string_length) noexcept {
			using sl_type = decltype(string);
			return string_length >= string.size() ? string_literal_comparator_impl<sl_type, string>::impl(str) != nullptr : false;
		}
	};

}
