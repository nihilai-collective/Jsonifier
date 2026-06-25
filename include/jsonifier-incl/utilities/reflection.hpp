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

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_literal.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/core/core.hpp>

namespace jsonifier::internal {

	template<typename value_type> extern const value_type external;

	template<typename member_type> struct remove_member_pointer {
		using type = member_type;
	};

	template<typename class_type, typename member_type> struct remove_member_pointer<member_type class_type::*> {
		using type = class_type;
	};

	template<typename value_type> using remove_member_pointer_t = typename remove_member_pointer<value_type>::type;

	template<typename member_type> struct remove_class_pointer {
		using type = member_type;
	};

	template<typename class_type, typename member_type> struct remove_class_pointer<member_type class_type::*> {
		using type = member_type;
	};

	template<typename value_type> using remove_class_pointer_t = typename remove_class_pointer<value_type>::type;

#if JSONIFIER_COMPILER_CLANG
	constexpr auto pretty_function_tail = "]";
#elif JSONIFIER_COMPILER_GCC
	constexpr auto pretty_function_tail = ";";
#endif

#if JSONIFIER_COMPILER_MSVC
	template<typename value_type, auto p> static consteval string_view getNameImpl() noexcept {
		string_view str = std::source_location::current().function_name();
		str				= str.substr(str.find("->") + 2);
		return str.substr(0, str.find(">"));
	}
#else
	template<auto p> consteval string_view getNameImpl() noexcept {
		string_view str = std::source_location::current().function_name();
		str				= str.substr(str.find("&") + 1);
		str				= str.substr(0, str.find(pretty_function_tail));
		return str.substr(str.rfind("::") + 2);
	}
#endif

	template<auto p>
		requires(std::is_member_pointer_v<decltype(p)>)
	static constexpr string_view getName() noexcept {
#if JSONIFIER_COMPILER_MSVC
		using value_type		 = remove_member_pointer_t<decltype(p)>;
		constexpr auto pNew		 = p;
		constexpr auto newString = getNameImpl<value_type, &(external<value_type>.*pNew)>();
#else
		constexpr auto newString = getNameImpl<p>();
#endif
		return newString;
	}

}
