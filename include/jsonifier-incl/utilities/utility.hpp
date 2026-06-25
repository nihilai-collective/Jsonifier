/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier
#pragma once

#include <jsonifier-incl/containers/array.hpp>
#include <jsonifier-incl/utilities/concepts.hpp>

namespace jsonifier::internal {

	alignas(64) static constexpr array<bool, 256ULL> whitespaceTable{ []() constexpr {
		array<bool, 256ULL> returnValues{};
		returnValues[static_cast<uint64_t>('\t')] = true;
		returnValues[static_cast<uint64_t>(' ')]  = true;
		returnValues[static_cast<uint64_t>('\n')] = true;
		returnValues[static_cast<uint64_t>('\r')] = true;
		return returnValues;
	}() };

	alignas(64) static constexpr array<bool, 256> numberTable{ []() constexpr {
		array<bool, 256> returnValues{};
		returnValues['-'] = true;
		returnValues['0'] = true;
		returnValues['1'] = true;
		returnValues['2'] = true;
		returnValues['3'] = true;
		returnValues['4'] = true;
		returnValues['5'] = true;
		returnValues['6'] = true;
		returnValues['7'] = true;
		returnValues['8'] = true;
		returnValues['9'] = true;
		return returnValues;
	}() };

	alignas(64) static constexpr array<char[2], 256> charEscapeStorage{ [] {
		array<char[2], 256> returnValue{};
		for (uint64_t x = 0; x < 256; ++x) {
			returnValue[x][0] = static_cast<char>(x);
			returnValue[x][1] = '\0';
		}
		returnValue['\b'][0] = '\\';
		returnValue['\b'][1] = 'b';
		returnValue['\t'][0] = '\\';
		returnValue['\t'][1] = 't';
		returnValue['\n'][0] = '\\';
		returnValue['\n'][1] = 'n';
		returnValue['\f'][0] = '\\';
		returnValue['\f'][1] = 'f';
		returnValue['\r'][0] = '\\';
		returnValue['\r'][1] = 'r';
		returnValue['\"'][0] = '\\';
		returnValue['\"'][1] = '\"';
		returnValue['\\'][0] = '\\';
		returnValue['\\'][1] = '\\';
		return returnValue;
	}() };

	alignas(64) static constexpr array<string_view_ptr, 256> charEscapeTable{ [] {
		array<string_view_ptr, 256> returnValue{};
		for (uint64_t x = 0; x < 256; ++x) {
			returnValue[x] = +charEscapeStorage[x];
		}
		return returnValue;
	}() };

	alignas(64) static constexpr array<uint64_t, 256> charEscapeSizes{ [] {
		array<uint64_t, 256> returnValue{};
		for (uint64_t x = 0; x < 256; ++x) {
			returnValue[x] = 1;
		}
		returnValue['\b'] = 2;
		returnValue['\t'] = 2;
		returnValue['\n'] = 2;
		returnValue['\f'] = 2;
		returnValue['\r'] = 2;
		returnValue['\"'] = 2;
		returnValue['\\'] = 2;
		return returnValue;
	}() };

	template<concepts::uint_types value_type> constexpr value_type byteswap(value_type value) noexcept {
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

	template<concepts::uint_types auto valueNew> struct integral_constant {
		using value_type				  = decltype(valueNew);
		static constexpr value_type value = valueNew;

		JSONIFIER_INLINE constexpr operator value_type() const noexcept {
			return value;
		}

		JSONIFIER_INLINE constexpr value_type operator()() const noexcept {
			return value;
		}
	};

	template<concepts::uint_types auto index> using tag = integral_constant<index>;

	template<typename value_type> JSONIFIER_INLINE constexpr jsonifier::internal::remove_reference_t<value_type>&& move(value_type&& value) noexcept {
		return static_cast<jsonifier::internal::remove_reference_t<value_type>&&>(value);
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

	template<typename value_type> JSONIFIER_INLINE constexpr value_type&& forward(remove_reference_t<value_type>& t JSONIFIER_LIFETIME_BOUND) noexcept {
		return static_cast<value_type&&>(t);
	}

	template<typename value_type>
		requires(std::is_rvalue_reference_v<value_type>)
	JSONIFIER_INLINE constexpr value_type&& forward(remove_reference_t<value_type>&& t) noexcept {
		static_assert(!std::is_lvalue_reference_v<value_type>, "value_type cannot be an lvalue reference (e.g., U&).");
		return static_cast<value_type&&>(t);
	}

	template<auto function, typename variant_type, typename... arg_types, uint64_t... indices>
	JSONIFIER_INLINE static constexpr void visitImpl(integer_sequence<indices...>, variant_type&& variant, arg_types&&... args) noexcept {
		const auto idx = variant.index();
		static_cast<void>(((idx == indices ? (function(std::get<indices>(internal::forward<variant_type>(variant)), internal::forward<arg_types>(args)...), true) : false) || ...));
	}

	template<auto function, typename variant_type, typename... arg_types> JSONIFIER_INLINE static constexpr void visit(variant_type&& variant, arg_types&&... args) noexcept {
		using seq_t = make_integer_sequence<std::variant_size_v<jsonifier::internal::remove_cvref_t<variant_type>>>;
		visitImpl<function>(seq_t{}, internal::forward<variant_type>(variant), internal::forward<arg_types>(args)...);
	}

	template<template<auto...> typename functor_type, typename integer_sequence, auto...> struct functor_runner;

	template<template<auto...> typename functor_type, uint64_t... indices, auto... values> struct functor_runner<functor_type, integer_sequence<indices...>, values...> {
		template<typename... arg_types> JSONIFIER_INLINE static auto impl([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices>(internal::forward<arg_types>(args)...), ...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static auto implAnd([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices>(internal::forward<arg_types>(args)...) && ...);
		}
	};

	template<template<auto...> typename functor_type, uint64_t... indices, uint64_t offsetVal, auto... values>
	struct functor_runner<functor_type, offset_sequence<integer_sequence<indices...>, offsetVal>, values...> {
		template<typename... arg_types> JSONIFIER_INLINE static auto impl([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices + offsetVal>(internal::forward<arg_types>(args)...), ...);
		}
		template<typename... arg_types> JSONIFIER_INLINE static auto implAnd([[maybe_unused]] arg_types&&... args) noexcept {
			return (functor_type<values...>::template impl<indices + offsetVal>(internal::forward<arg_types>(args)...) && ...);
		}
	};

	template<concepts::integral_types value_type01, concepts::integral_types value_type02> JSONIFIER_INLINE constexpr value_type01 max(value_type01 value1, value_type02 value2) {
		return value1 > static_cast<value_type01>(value2) ? value1 : static_cast<value_type01>(value2);
	}

	template<concepts::integral_types value_type01, concepts::integral_types value_type02> JSONIFIER_INLINE constexpr value_type01 min(value_type01 value1, value_type02 value2) {
		return value1 < static_cast<value_type01>(value2) ? value1 : static_cast<value_type01>(value2);
	}

	JSONIFIER_INLINE constexpr uint64_t strLen(string_view_ptr input) noexcept {
		uint64_t returnVal{};
		if (input) {
			while (input[returnVal] != '\0') {
				++returnVal;
			}
		}
		return returnVal;
	}

}

#include <jsonifier-incl/containers/tuple.hpp>

namespace jsonifier::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	using avx_integer_list = internal::type_list_t<internal::type_holder<64, internal::avx_type_wrapper<internal::avx_type::m512>, uint64_t, 64>,
		internal::type_holder<32, internal::avx_type_wrapper<internal::avx_type::m256>, uint32_t, 32>,
		internal::type_holder<16, internal::avx_type_wrapper<internal::avx_type::m128>, uint64_t, 16>>;
	using avx_list		   = internal::type_list_t<internal::type_holder<64, internal::avx_type_wrapper<internal::avx_type::m512>, uint64_t, std::numeric_limits<uint64_t>::max()>,
				internal::type_holder<32, internal::avx_type_wrapper<internal::avx_type::m256>, uint32_t, std::numeric_limits<uint32_t>::max()>,
				internal::type_holder<16, internal::avx_type_wrapper<internal::avx_type::m128>, uint64_t, std::numeric_limits<uint64_t>::max()>>;
#else
	using avx_integer_list = internal::type_list_t<internal::type_holder<64, internal::avx_type_wrapper<internal::avx_type::m512>, uint64_t, 64>,
		internal::type_holder<32, internal::avx_type_wrapper<internal::avx_type::m256>, uint32_t, 32>,
		internal::type_holder<16, internal::avx_type_wrapper<internal::avx_type::m128>, uint16_t, 16>>;
	using avx_list		   = internal::type_list_t<internal::type_holder<64, internal::avx_type_wrapper<internal::avx_type::m512>, uint64_t, std::numeric_limits<uint64_t>::max()>,
				internal::type_holder<32, internal::avx_type_wrapper<internal::avx_type::m256>, uint32_t, std::numeric_limits<uint32_t>::max()>,
				internal::type_holder<16, internal::avx_type_wrapper<internal::avx_type::m128>, uint16_t, std::numeric_limits<uint16_t>::max()>>;
#endif

}
