/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/utility.hpp
 */
#pragma once

#include <jsonifier-incl/containers/array.hpp>
#include <jsonifier-incl/utilities/concepts.hpp>

namespace jsonifier::internal {

	template<typename value_type> using base_t = remove_cvref_t<value_type>;

	template<auto valueNew> struct make_static {
		static constexpr auto value{ valueNew };
	};

	constexpr array<bool, 256ULL> genWhitespaceTable() {
		array<bool, 256ULL> returnValues{};
		returnValues[static_cast<uint64_t>('\t')] = true;
		returnValues[static_cast<uint64_t>(' ')]  = true;
		returnValues[static_cast<uint64_t>('\n')] = true;
		returnValues[static_cast<uint64_t>('\r')] = true;
		return returnValues;
	}

	alignas(64) inline constexpr const bool* __restrict whitespaceTable{ []() constexpr {
		constexpr auto local{ genWhitespaceTable() };
		return make_static<local>::value.data();
	}() };

	constexpr array<int64_t, 256ULL> genNestingDeltaTable() {
		array<int64_t, 256ULL> returnValues{};
		returnValues[static_cast<uint64_t>('{')] = 1;
		returnValues[static_cast<uint64_t>('[')] = 1;
		returnValues[static_cast<uint64_t>('}')] = -1;
		returnValues[static_cast<uint64_t>(']')] = -1;
		return returnValues;
	}

	alignas(64) inline constexpr const int64_t* __restrict nestingDeltaTable{ []() constexpr {
		constexpr auto local{ genNestingDeltaTable() };
		return make_static<local>::value.data();
	}() };

	constexpr array<bool, 256ULL> genNewlineTable() {
		array<bool, 256ULL> returnValues{};
		returnValues[static_cast<uint64_t>('\n')] = true;
		returnValues[static_cast<uint64_t>('\r')] = true;
		return returnValues;
	}

	alignas(64) inline constexpr const bool* __restrict newlineTable{ []() constexpr {
		constexpr auto local{ genNewlineTable() };
		return make_static<local>::value.data();
	}() };

	constexpr array<bool, 256ULL> genNumberTable() {
		array<bool, 256ULL> returnValues{};
		returnValues[static_cast<uint64_t>('-')] = true;
		returnValues[static_cast<uint64_t>('0')] = true;
		returnValues[static_cast<uint64_t>('1')] = true;
		returnValues[static_cast<uint64_t>('2')] = true;
		returnValues[static_cast<uint64_t>('3')] = true;
		returnValues[static_cast<uint64_t>('4')] = true;
		returnValues[static_cast<uint64_t>('5')] = true;
		returnValues[static_cast<uint64_t>('6')] = true;
		returnValues[static_cast<uint64_t>('7')] = true;
		returnValues[static_cast<uint64_t>('8')] = true;
		returnValues[static_cast<uint64_t>('9')] = true;
		return returnValues;
	}

	alignas(64) inline constexpr const bool* __restrict numberTable{ []() constexpr {
		constexpr auto local{ genNumberTable() };
		return make_static<local>::value.data();
	}() };

	constexpr array<array<char, 2>, 256ULL> genCharEscapeStorage() {
		array<array<char, 2>, 256ULL> returnValue{};
		for (uint64_t x = 0; x < 256ULL; ++x) {
			returnValue[x][0] = static_cast<char>(x);
			returnValue[x][1] = '\0';
		}
		returnValue[static_cast<uint64_t>('\b')][0] = '\\';
		returnValue[static_cast<uint64_t>('\b')][1] = 'b';
		returnValue[static_cast<uint64_t>('\t')][0] = '\\';
		returnValue[static_cast<uint64_t>('\t')][1] = 't';
		returnValue[static_cast<uint64_t>('\n')][0] = '\\';
		returnValue[static_cast<uint64_t>('\n')][1] = 'n';
		returnValue[static_cast<uint64_t>('\f')][0] = '\\';
		returnValue[static_cast<uint64_t>('\f')][1] = 'f';
		returnValue[static_cast<uint64_t>('\r')][0] = '\\';
		returnValue[static_cast<uint64_t>('\r')][1] = 'r';
		returnValue[static_cast<uint64_t>('\"')][0] = '\\';
		returnValue[static_cast<uint64_t>('\"')][1] = '\"';
		returnValue[static_cast<uint64_t>('\\')][0] = '\\';
		returnValue[static_cast<uint64_t>('\\')][1] = '\\';
		return returnValue;
	}

	alignas(64) inline constexpr const array<char, 2>* __restrict charEscapeStorage{ []() constexpr {
		constexpr auto local{ genCharEscapeStorage() };
		return make_static<local>::value.data();
	}() };

	constexpr array<const char*, 256ULL> genCharEscapeTable() {
		array<const char*, 256ULL> returnValue{};
		for (uint64_t x = 0; x < 256ULL; ++x) {
			returnValue[x] = charEscapeStorage[x].data();
		}
		return returnValue;
	}

	alignas(64) inline constexpr const char* const* __restrict charEscapeTable{ []() constexpr {
		constexpr auto local{ genCharEscapeTable() };
		return make_static<local>::value.data();
	}() };

	constexpr array<uint64_t, 256ULL> genCharEscapeSizes() {
		array<uint64_t, 256ULL> returnValue{};
		for (uint64_t x = 0; x < 256ULL; ++x) {
			returnValue[x] = 1;
		}
		returnValue[static_cast<uint64_t>('\b')] = 2;
		returnValue[static_cast<uint64_t>('\t')] = 2;
		returnValue[static_cast<uint64_t>('\n')] = 2;
		returnValue[static_cast<uint64_t>('\f')] = 2;
		returnValue[static_cast<uint64_t>('\r')] = 2;
		returnValue[static_cast<uint64_t>('\"')] = 2;
		returnValue[static_cast<uint64_t>('\\')] = 2;
		return returnValue;
	}

	alignas(64) inline constexpr const uint64_t* __restrict charEscapeSizes{ []() constexpr {
		constexpr auto local{ genCharEscapeSizes() };
		return make_static<local>::value.data();
	}() };

	template<typename value_type> JSONIFIER_INLINE constexpr value_type&& forward(remove_reference_t<value_type>& t JSONIFIER_LIFETIME_BOUND) noexcept {
		return static_cast<value_type&&>(t);
	}

	template<typename value_type>
		requires(std::is_rvalue_reference_v<value_type>)
	JSONIFIER_INLINE constexpr value_type&& forward(remove_reference_t<value_type>&& t) noexcept {
		static_assert(!std::is_lvalue_reference_v<value_type>, "value_type cannot be an lvalue reference (e.g., U&).");
		return static_cast<value_type&&>(t);
	}

	template<typename value_type> JSONIFIER_INLINE constexpr jsonifier::internal::remove_reference_t<value_type>&& move(value_type&& value) noexcept {
		return static_cast<jsonifier::internal::remove_reference_t<value_type>&&>(value);
	}

	template<uint_types value_type> inline constexpr value_type byteswap(value_type value) noexcept {
		if constexpr (sizeof(value_type) == 1) {
			return value;
		} else if constexpr (sizeof(value_type) == 2) {
			return static_cast<value_type>((value >> 8) | (value << 8));
		} else if constexpr (sizeof(value_type) == 4) {
			return static_cast<value_type>(((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8) | ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24));
		} else if constexpr (sizeof(value_type) == 8) {
			return static_cast<value_type>(((value & 0x00000000000000FFull) << 56) | ((value & 0x000000000000FF00ull) << 40) | ((value & 0x0000000000FF0000ull) << 24) |
				((value & 0x00000000FF000000ull) << 8) | ((value & 0x000000FF00000000ull) >> 8) | ((value & 0x0000FF0000000000ull) >> 24) |
				((value & 0x00FF000000000000ull) >> 40) | ((value & 0xFF00000000000000ull) >> 56));
		} else {
			static_assert(sizeof(value_type) == 0, "byte_swap: unsupported type size");
		}
	}

	template<uint64_t bytesProcessedNew, typename simd_type, typename integer_type_new, integer_type_new maskNew> struct type_holder {
		static constexpr uint64_t bytesProcessed{ bytesProcessedNew };
		static constexpr integer_type_new mask{ maskNew };
		using type		   = simd_type;
		using integer_type = integer_type_new;
	};

	template<typename value_type> struct get_int_type {
		using type = jsonifier::internal::conditional_t<std::is_unsigned_v<value_type>, uint8_t, int8_t>;
	};

	template<uint_types auto valueNew> struct integral_constant {
		using value_type				  = decltype(valueNew);
		static constexpr value_type value = valueNew;

		JSONIFIER_INLINE constexpr value_type operator()() const noexcept {
			return value;
		}

		JSONIFIER_INLINE constexpr operator value_type() const noexcept {
			return value;
		}
	};

	template<uint_types auto index> using tag = integral_constant<index>;

	template<uint64_t... indices> struct integer_sequence {};

	template<typename sequence_one, typename sequence_two> struct concat_sequence;

	template<uint64_t... first_values, uint64_t... second_values> struct concat_sequence<integer_sequence<first_values...>, integer_sequence<second_values...>> {
		using type = integer_sequence<first_values..., (second_values + sizeof...(first_values))...>;
	};

	template<uint64_t count> struct make_sequence_impl {
		using half_type		 = typename make_sequence_impl<count / 2>::type;
		using remainder_type = typename make_sequence_impl<count - count / 2>::type;
		using type			 = typename concat_sequence<half_type, remainder_type>::type;
	};

	template<> struct make_sequence_impl<0ULL> {
		using type = integer_sequence<>;
	};

	template<> struct make_sequence_impl<1ULL> {
		using type = integer_sequence<0>;
	};

	template<uint64_t count> using make_integer_sequence = typename make_sequence_impl<count>::type;

	template<typename integer_sequence, uint64_t offset> struct offset_sequence;

	template<uint64_t... indices, uint64_t offset> struct offset_sequence<integer_sequence<indices...>, offset> {
		using type = integer_sequence<static_cast<decltype(offset)>(indices + offset)...>;
	};

	template<typename integer_sequence, uint64_t step> struct step_sequence;

	template<uint64_t... indices, uint64_t step_new> struct step_sequence<integer_sequence<indices...>, step_new> {
		using type = integer_sequence<static_cast<decltype(step_new)>(indices* step_new)...>;
	};

	template<typename integer_sequence, uint64_t step> using step_sequence_t = typename step_sequence<integer_sequence, step>::type;

	template<uint64_t start, uint64_t end, uint64_t step>
		requires(end >= start && step > 0)
	using make_stepped_range_sequence =
		typename offset_sequence<step_sequence_t<make_integer_sequence<static_cast<decltype(end)>((end - start + step - 1) / step)>, step>, start>::type;

	template<template<auto...> typename functor_type, typename integer_sequence, auto...> struct functor_runner;

	template<template<auto...> typename functor_type, uint64_t... indices, auto... values> struct functor_runner<functor_type, integer_sequence<indices...>, values...> {
		template<typename... arg_types> JSONIFIER_INLINE static auto implAnd([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices>(internal::forward<arg_types>(args)...) && ...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static auto impl([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices>(internal::forward<arg_types>(args)...), ...);
		}
	};

	template<template<auto...> typename functor_type, uint64_t... indices, uint64_t offsetVal, auto... values>
	struct functor_runner<functor_type, offset_sequence<integer_sequence<indices...>, offsetVal>, values...> {
		template<typename... arg_types> JSONIFIER_INLINE static auto implAnd([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices + offsetVal>(internal::forward<arg_types>(args)...) && ...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static auto impl([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices + offsetVal>(internal::forward<arg_types>(args)...), ...);
		}
	};

	template<typename function_type, typename sequence_type> struct visit_impl;

	template<typename function_type, uint64_t... indices> struct visit_impl<function_type, integer_sequence<indices...>> {
		template<typename variant_type, typename... arg_types> JSONIFIER_INLINE static constexpr void impl(variant_type&& variant, arg_types&&... args) noexcept {
			const auto idx = variant.index();
			static_cast<void>((
				(idx == indices ? (function_type::impl(std::get<indices>(internal::forward<variant_type>(variant)), internal::forward<arg_types>(args)...), true) : false) || ...));
		}
	};

	template<typename function_type, typename variant_type, typename... arg_types>
	JSONIFIER_INLINE static constexpr void visit(variant_type&& variant, arg_types&&... args) noexcept {
		visit_impl<function_type, make_integer_sequence<std::variant_size_v<base_t<variant_type>>>>::impl(internal::forward<variant_type>(variant),
			internal::forward<arg_types>(args)...);
	}

	template<integral_t value_type01, integral_t value_type02> JSONIFIER_INLINE constexpr value_type01 max(value_type01 value1, value_type02 value2) noexcept {
		return value1 > static_cast<value_type01>(value2) ? value1 : static_cast<value_type01>(value2);
	}

	template<integral_t value_type01, integral_t value_type02> JSONIFIER_INLINE constexpr value_type01 min(value_type01 value1, value_type02 value2) noexcept {
		return value1 < static_cast<value_type01>(value2) ? value1 : static_cast<value_type01>(value2);
	}

	JSONIFIER_INLINE constexpr uint64_t strLen(const char* input) noexcept {
		uint64_t returnVal{};
		if (input) {
			while (input[returnVal] != '\0') {
				++returnVal;
			}
		}
		return returnVal;
	}

	template<typename value_type> struct digit_sizes;

	template<uint64_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 20 };
	};

	template<int64_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 20 };
	};

	template<uint32_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 10 };
	};

	template<int32_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 11 };
	};

	template<uint16_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 5 };
	};

	template<int16_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 5 };
	};

	template<uint8_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 3 };
	};

	template<int8_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 3 };
	};

	template<float32_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 32 };
	};

	template<float64_types value_type> struct digit_sizes<value_type> {
		static constexpr uint64_t value{ 32 };
	};

	template<uint64_t chunk_bytes> JSONIFIER_INLINE void copyOverlappingChunks(std::byte* __restrict dst, const std::byte* __restrict src, uint64_t byte_count) {
		pow2MemcpyWrapper<chunk_bytes>(dst, src);
		pow2MemcpyWrapper<chunk_bytes>(dst + byte_count - chunk_bytes, src + byte_count - chunk_bytes);
	}

	JSONIFIER_INLINE static void copyDecomposed(std::byte* __restrict dst, const std::byte* __restrict src, uint64_t byte_count) {
		uint64_t offset{};
		if ((byte_count >> 7) & 1ull) {
			pow2MemcpyWrapper<1ull << 7>(dst + offset, src + offset);
			offset += 1ull << 7;
		}
		if ((byte_count >> 6) & 1ull) {
			pow2MemcpyWrapper<1ull << 6>(dst + offset, src + offset);
			offset += 1ull << 6;
		}
		if ((byte_count >> 5) & 1ull) {
			pow2MemcpyWrapper<1ull << 5>(dst + offset, src + offset);
			offset += 1ull << 5;
		}
		if ((byte_count >> 4) & 1ull) {
			pow2MemcpyWrapper<1ull << 4>(dst + offset, src + offset);
			offset += 1ull << 4;
		}
		if ((byte_count >> 3) & 1ull) {
			pow2MemcpyWrapper<1ull << 3>(dst + offset, src + offset);
			offset += 1ull << 3;
		}
		if ((byte_count >> 2) & 1ull) {
			pow2MemcpyWrapper<1ull << 2>(dst + offset, src + offset);
			offset += 1ull << 2;
		}
		if ((byte_count >> 1) & 1ull) {
			pow2MemcpyWrapper<1ull << 1>(dst + offset, src + offset);
			offset += 1ull << 1;
		}
		if ((byte_count >> 0) & 1ull) {
			pow2MemcpyWrapper<1ull << 0>(dst + offset, src + offset);
			offset += 1ull << 0;
		}
	}

	template<uint64_t max_bytes> JSONIFIER_INLINE void jsonifierMemcpyUpTo(void* __restrict dst, const void* __restrict src, uint64_t byte_count) {
		switch (std::bit_width(byte_count)) {
			case 0: {
				return;
			}
			case 1: {
				*static_cast<std::byte* __restrict>(dst) = *static_cast<const std::byte* __restrict>(src);
				return;
			}
			case 2: {
				if constexpr (max_bytes >= 2) {
					copyOverlappingChunks<2>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 3: {
				if constexpr (max_bytes >= 4) {
					copyOverlappingChunks<4>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 4: {
				if constexpr (max_bytes >= 8) {
					copyOverlappingChunks<8>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 5: {
				if constexpr (max_bytes >= 16) {
					copyOverlappingChunks<16>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 6: {
				if constexpr (max_bytes >= 32) {
					copyOverlappingChunks<32>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 7: {
				if constexpr (max_bytes >= 64) {
					copyOverlappingChunks<64>(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			case 8: {
				if constexpr (max_bytes >= 128) {
					copyDecomposed(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
			default: {
				if constexpr (max_bytes >= 256) {
					memcpyWrapper(static_cast<std::byte* __restrict>(dst), static_cast<const std::byte* __restrict>(src), byte_count);
				}
				return;
			}
		}
	}

	JSONIFIER_INLINE void jsonifierMemcpy(void* __restrict destination, const void* __restrict source, uint64_t byte_count) {
		jsonifierMemcpyUpTo<std::numeric_limits<uint64_t>::max()>(destination, source, byte_count);
	}

}

#include <jsonifier-incl/containers/tuple.hpp>

namespace jsonifier::internal::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	using avx_integer_list = internal::type_list_t<internal::type_holder<64, internal::simd_type_wrapper<internal::avx_type::m512>, uint64_t, 64>,
		internal::type_holder<32, internal::simd_type_wrapper<internal::avx_type::m256>, uint32_t, 32>,
		internal::type_holder<16, internal::simd_type_wrapper<internal::avx_type::m128>, uint64_t, 16>>;
	using avx_list		   = internal::type_list_t<internal::type_holder<64, internal::simd_type_wrapper<internal::avx_type::m512>, uint64_t, std::numeric_limits<uint64_t>::max()>,
		internal::type_holder<32, internal::simd_type_wrapper<internal::avx_type::m256>, uint32_t, std::numeric_limits<uint32_t>::max()>,
		internal::type_holder<16, internal::simd_type_wrapper<internal::avx_type::m128>, uint64_t, std::numeric_limits<uint64_t>::max()>>;
#else
	using avx_integer_list = internal::type_list_t<internal::type_holder<64, internal::simd_type_wrapper<internal::avx_type::m512>, uint64_t, 64>,
		internal::type_holder<32, internal::simd_type_wrapper<internal::avx_type::m256>, uint32_t, 32>,
		internal::type_holder<16, internal::simd_type_wrapper<internal::avx_type::m128>, uint16_t, 16>>;
	using avx_list		   = internal::type_list_t<internal::type_holder<64, internal::simd_type_wrapper<internal::avx_type::m512>, uint64_t, std::numeric_limits<uint64_t>::max()>,
		internal::type_holder<32, internal::simd_type_wrapper<internal::avx_type::m256>, uint32_t, std::numeric_limits<uint32_t>::max()>,
		internal::type_holder<16, internal::simd_type_wrapper<internal::avx_type::m128>, uint16_t, std::numeric_limits<uint16_t>::max()>>;
#endif

}
