// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/reflection.hpp
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
