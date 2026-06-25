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

#include <jsonifier-incl/core/config.hpp>
#include <jsonifier-incl/simd/simd_types.hpp>

namespace jsonifier::internal {

	// from
	// https://stackoverflow.com/questions/16337610/how-to-know-if-a-type-is-a-specialization-of-stdvector
	template<typename, template<typename...> typename> constexpr bool is_specialization_v = false;

	template<template<typename...> typename value_type, typename... arg_types> constexpr bool is_specialization_v<value_type<arg_types...>, value_type> = true;

	template<typename value_type> struct remove_cv {
		using type = value_type;
	};

	template<typename value_type> struct remove_cv<const value_type> {
		using type = value_type;
	};

	template<typename value_type> struct remove_cv<volatile value_type> {
		using type = value_type;
	};

	template<typename value_type> struct remove_cv<const volatile value_type> {
		using type = value_type;
	};

	template<typename value_type> using remove_cv_t = typename remove_cv<value_type>::type;

	template<typename value_type> struct remove_pointer {
		using type = value_type;
	};

	template<typename value_type> struct remove_pointer<value_type*> {
		using type = value_type;
	};

	template<typename value_type> using remove_pointer_t = typename remove_pointer<value_type>::type;

	template<typename value_type> struct remove_reference {
		using type = value_type;
	};

	template<typename value_type> struct remove_reference<value_type&> {
		using type = value_type;
	};

	template<typename value_type> struct remove_reference<value_type&&> {
		using type = value_type;
	};

	template<typename value_type> using remove_reference_t = typename remove_reference<value_type>::type;

	template<typename value_type> using remove_cvref_t = remove_cv_t<remove_reference_t<value_type>>;

	template<bool condition, typename type01, typename type02> struct conditional;

	template<typename type01, typename type02> struct conditional<true, type01, type02> {
		using type = type01;
	};

	template<typename type01, typename type02> struct conditional<false, type01, type02> {
		using type = type02;
	};

	template<bool condition, typename type01, typename type02> using conditional_t = conditional<condition, type01, type02>::type;

	template<typename derived_type> class parser;

	enum class avx_type { m128 = 0, m256 = 1, m512 = 2 };

	template<avx_type type> struct avx_type_wrapper;

	template<> struct avx_type_wrapper<avx_type::m128> {
		using type = jsonifier_simd_int_128;
	};

	template<> struct avx_type_wrapper<avx_type::m256> {
		using type = jsonifier_simd_int_256;
	};

	template<> struct avx_type_wrapper<avx_type::m512> {
		using type = jsonifier_simd_int_512;
	};

}
