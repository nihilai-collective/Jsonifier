// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/containers/tuple.hpp
#pragma once

#include <jsonifier-incl/utilities/forward.hpp>

#if JSONIFIER_COMPILER_MSVC
	#define JSONIFIER_TUPLET_HAS_NO_UNIQUE_ADDRESS 1
	#define JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
	#define JSONIFIER_TUPLET_HAS_NO_UNIQUE_ADDRESS 1
	#define JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace jsonifier::internal {

	template<typename value_type>
	concept derivable_types = std::is_class_v<value_type> && !std::is_final_v<value_type>;

	template<uint64_t indexNew, typename value_type_new> struct type_list_elem {
		using value_type = value_type_new;
		static constexpr uint64_t index{ indexNew };
		JSONIFIER_TUPLET_NO_UNIQUE_ADDRESS value_type value;

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) & noexcept {
			return static_cast<value_type&>(value);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) const& noexcept {
			return static_cast<const value_type&>(value);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) && noexcept {
			return static_cast<value_type&&>(value);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) const&& noexcept {
			return static_cast<const value_type&&>(value);
		}

	  protected:
		template<uint64_t index, typename type_list_type> friend struct type_list_element;
		JSONIFIER_INLINE static constexpr type_list_elem getForType(tag<index>) {
			return type_list_elem{};
		}
	};

	template<uint64_t indexNew, derivable_types value_type_new> struct type_list_elem<indexNew, value_type_new>
		: public std::remove_const_t<std::remove_volatile_t<value_type_new>> {
		using value_type = value_type_new;
		static constexpr uint64_t index{ indexNew };

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) & noexcept JSONIFIER_LIFETIME_BOUND {
			return static_cast<value_type&>(*this);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) const& noexcept JSONIFIER_LIFETIME_BOUND {
			return static_cast<const value_type&>(*this);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) && noexcept JSONIFIER_LIFETIME_BOUND {
			return static_cast<value_type&&>(*this);
		}

		JSONIFIER_INLINE constexpr decltype(auto) operator[](tag<index>) const&& noexcept JSONIFIER_LIFETIME_BOUND {
			return static_cast<const value_type&&>(*this);
		}

	  protected:
		template<uint64_t index, typename type_list_type> friend struct type_list_element;
		JSONIFIER_INLINE static constexpr type_list_elem getForType(tag<index>) {
			return type_list_elem{};
		}
	};

	template<typename... value_types> struct type_list_impl : public value_types... {
		static constexpr uint64_t size{ sizeof...(value_types) };
		using value_types::operator[]...;
		using value_types::getForType...;
	};

	template<typename integer_sequence, typename... value_types> struct tuple_type_list;

	template<uint64_t... indices, typename... value_types> struct tuple_type_list<integer_sequence<indices...>, value_types...> {
		using type = type_list_impl<type_list_elem<indices, value_types>...>;
	};

	template<typename... value_types> using type_list_t = typename tuple_type_list<make_integer_sequence<sizeof...(value_types)>, value_types...>::type;

	template<uint64_t index, typename type_list_type> struct type_list_element {
		using type = typename decltype(std::remove_pointer_t<std::remove_cvref_t<type_list_type>>::getForType(tag<index>{}))::value_type;
	};

	template<uint64_t index, typename type_list_type> using type_list_element_t = type_list_element<index, type_list_type>::type;

	template<uint64_t index, typename tuple_type> using tuple_element_t = type_list_element_t<index, tuple_type>;

	template<typename value_type_01, typename value_type_02>
	concept eq_comparable_types = requires { std::declval<value_type_01>() == std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept neq_comparable_types = requires { std::declval<value_type_01>() != std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept lt_comparable_types = requires { std::declval<value_type_01>() < std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept lte_comparable_types = requires { std::declval<value_type_01>() <= std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept gt_comparable_types = requires { std::declval<value_type_01>() > std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept gte_comparable_types = requires { std::declval<value_type_01>() >= std::declval<value_type_02>(); };

	template<typename value_type_01, typename value_type_02>
	concept ss_comparable_types = requires { std::declval<value_type_01>() <=> std::declval<value_type_02>(); };

	struct eq_op {
		template<typename value_type_01, eq_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) == std::forward<value_type_02>(val02);
		}
	};

	struct neq_op {
		template<typename value_type_01, neq_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) != std::forward<value_type_02>(val02);
		}
	};

	struct lt_op {
		template<typename value_type_01, lt_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) < std::forward<value_type_02>(val02);
		}
	};

	struct lte_op {
		template<typename value_type_01, lte_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) <= std::forward<value_type_02>(val02);
		}
	};

	struct gt_op {
		template<typename value_type_01, gt_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) > std::forward<value_type_02>(val02);
		}
	};

	struct gte_op {
		template<typename value_type_01, gte_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr bool impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) >= std::forward<value_type_02>(val02);
		}
	};

	struct ss_op {
		template<typename value_type_01, ss_comparable_types<value_type_01> value_type_02>
		JSONIFIER_INLINE static constexpr auto impl(value_type_01&& val01, value_type_02&& val02) noexcept {
			return std::forward<value_type_01>(val01) <=> std::forward<value_type_02>(val02);
		}
	};

	template<typename integer_sequence, typename operation_type> struct comparison_op;

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, eq_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			return (eq_op::impl(t1[tag<indices>{}], t2[tag<indices>{}]) && ...);
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, neq_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			return !(t1 == t2);
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, ss_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr auto impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			using result_type  = std::common_comparison_category_t<decltype(std::declval<value_types_01>() <=> std::declval<value_types_02>())...>;
			result_type result = result_type::equivalent;
			((result = ss_op::impl(t1[tag<indices>{}], t2[tag<indices>{}]), result != 0) || ...);
			return result;
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, lt_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			bool result = false;
			(((result = lt_op::impl(t1[tag<indices>{}], t2[tag<indices>{}])) || (!lt_op::impl(t2[tag<indices>{}], t1[tag<indices>{}]))) && ...);
			return result;
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, lte_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			return !(t2 < t1);
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, gt_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			bool result = false;
			(((result = gt_op::impl(t1[tag<indices>{}], t2[tag<indices>{}])) || (!gt_op::impl(t2[tag<indices>{}], t1[tag<indices>{}]))) && ...);
			return result;
		}
	};

	template<uint64_t... indices> struct comparison_op<integer_sequence<indices...>, gte_op> {
		template<template<typename...> typename tuple_type_01, typename... value_types_01, template<typename...> typename tuple_type_02, typename... value_types_02>
		JSONIFIER_INLINE static constexpr bool impl(const tuple_type_01<value_types_01...>& t1, const tuple_type_02<value_types_02...>& t2) noexcept {
			return !(t1 < t2);
		}
	};

	template<typename... value_types> struct tuple;

	template<typename... value_type> struct tuple_size;

	template<typename... value_types> struct tuple : type_list_t<value_types...> {
		static constexpr uint64_t size{ sizeof...(value_types) };

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator==(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, eq_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator!=(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, neq_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr uint64_t operator<=>(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, ss_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator>(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, gt_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator>=(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, gte_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator<(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, lt_op>::impl(*this, other);
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator<=(const tuple<other_types...>& other) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return comparison_op<make_integer_sequence<sizeof...(value_types)>, lte_op>::impl(*this, other);
		}
	};

	template<> struct tuple<> : type_list_t<> {
		static constexpr uint64_t size{ 0 };

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator==(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return true;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator!=(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return false;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr auto operator<=>(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return std::strong_ordering::equal;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator>(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return false;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator>=(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return true;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator<(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return false;
		}

		template<typename... other_types> JSONIFIER_INLINE constexpr bool operator<=(const tuple<other_types...>&) const noexcept {
			static_assert(sizeof...(other_types) == size, "Sorry, but these tuples must be equal in size to be compared!");
			return true;
		}
	};

	template<typename... value_type> struct tuple_size<tuple<value_type...>> : public integral_constant<sizeof...(value_type)> {};

	template<typename... value_type> struct tuple_size<std::tuple<value_type...>> : public integral_constant<sizeof...(value_type)> {};

	template<typename tuple_type> static constexpr uint64_t tuple_size_v = tuple_size<std::remove_cvref_t<tuple_type>>::value;

	template<typename... value_types> tuple(value_types&&...) -> tuple<value_types...>;

	template<typename value_type, template<typename...> typename template_type> struct is_specialization_of_impl : std::false_type {};

	template<template<typename...> typename template_type, typename... args> struct is_specialization_of_impl<template_type<args...>, template_type> : std::true_type {};

	template<typename value_type, template<typename...> typename template_type>
	concept is_specialization_of_v = is_specialization_of_impl<value_type, template_type>::value;

	template<uint64_t index, typename Ts>
		requires(is_specialization_of_v<remove_cvref_t<Ts>, jsonifier::internal::tuple>)
	JSONIFIER_INLINE constexpr decltype(auto) getBecauseOtherLibAuthorsResolve(Ts&& t JSONIFIER_LIFETIME_BOUND) noexcept {
		return std::forward<Ts>(t)[tag<index>{}];
	}

	struct completion_signal {
		uint64_t index{};
		bool notDone{ true };

		constexpr operator bool() const {
			return notDone;
		}
	};

	template<uint64_t indexNew, typename target_type, typename tuple_type> struct index_tag {
		using element_type				 = type_list_element_t<indexNew, tuple_type>;
		static constexpr bool isNotMatch = !std::is_same_v<std::remove_cvref_t<target_type>, std::remove_cvref_t<element_type>>;
		static constexpr uint64_t index{ isNotMatch ? std::numeric_limits<uint64_t>::max() : indexNew };
		static constexpr completion_signal value{ index, isNotMatch };
	};

	template<typename target_type, typename tuple_type, typename integer_sequence> struct index_finder;

	template<typename target_type, typename tuple_type, uint64_t... indices> struct index_finder<target_type, tuple_type, integer_sequence<indices...>> {
		static constexpr uint64_t getIndex() {
			completion_signal result{};
			((result = index_tag<indices, target_type, tuple_type>::value) && ...);
			return result.index;
		}
		static constexpr uint64_t index{ getIndex() };
		static_assert(index != std::numeric_limits<uint64_t>::max(), "Sorry, but that type does not appear to exist in this tuple!");
	};

	template<typename target_type, typename tuple_type> JSONIFIER_INLINE static constexpr decltype(auto) get(tuple_type&& tupleVal) noexcept {
		constexpr uint64_t index = index_finder<target_type, tuple_type, make_integer_sequence<std::remove_cvref_t<tuple_type>::size>>::index;
		return std::forward<tuple_type>(tupleVal)[tag<index>{}];
	}

#if JSONIFIER_COMPILER_GCC && JSONIFIER_PLATFORM_MAC

	template<typename... types> JSONIFIER_INLINE static constexpr auto makeTuple(types&&... args) noexcept {
		auto result = tuple<types...>{};
		if constexpr (sizeof...(types) > 0) {
			constructInPlace<0>(result, std::forward<types>(args)...);
		}
		return result;
	}

	template<uint64_t I, typename tuple_t, typename arg_t, typename... rest_t>
	JSONIFIER_INLINE static constexpr void constructInPlace(tuple_t& t, arg_t&& arg, rest_t&&... rest) noexcept {
		t[tag<I>{}] = std::forward<arg_t>(arg);
		if constexpr (sizeof...(rest) > 0) {
			constructInPlace<I + 1>(t, std::forward<rest_t>(rest)...);
		}
	}

#else
	template<typename... types> JSONIFIER_INLINE static constexpr auto makeTuple(types&&... args) noexcept {
		using base = type_list_t<types...>;
		return tuple<types...>{ base{ { std::forward<types>(args) }... } };
	}
#endif

	template<typename... tuple_types> struct join_tuples;

	template<typename... left_types, typename... right_types, typename... rest_types> struct join_tuples<tuple<left_types...>, tuple<right_types...>, rest_types...> {
		using type = typename join_tuples<tuple<left_types..., right_types...>, rest_types...>::type;
	};

	template<typename... left_types, typename... right_types> struct join_tuples<tuple<left_types...>, tuple<right_types...>> {
		using type = tuple<left_types..., right_types...>;
	};

	template<typename... tuple_types> using join_tuples_t = typename join_tuples<tuple_types...>::type;

	template<typename... list_types> struct tuple_cat_impl {
		static constexpr uint64_t total{ (list_types::size + ...) };
		using result_type	   = join_tuples_t<list_types...>;
		using lists_tuple_type = type_list_t<list_types*...>;

		static consteval auto getMapValues() {
			if constexpr (total > 0) {
				struct tuple_cat_index_map {
					uint64_t listIdx[total]{};
					uint64_t localIdx[total]{};
				};
				constexpr uint64_t sizes[]{ list_types::size... };
				tuple_cat_index_map m{};
				uint64_t g{};
				for (uint64_t i = 0; i < sizeof...(list_types); ++i)
					for (uint64_t j = 0; j < sizes[i]; ++j, ++g) {
						m.listIdx[g]  = i;
						m.localIdx[g] = j;
					}
				return m;
			} else {
				struct tuple_cat_index_map {
					uint64_t listIdx[2]{};
					uint64_t localIdx[2]{};
				};
				return tuple_cat_index_map{};
			}
		}

		static constexpr auto map{ getMapValues() };

		template<uint64_t index, typename list_type> JSONIFIER_INLINE static constexpr decltype(auto) getIndividualElement(list_type&& list) {
			auto* tuplePtr = list[tag<map.listIdx[index]>{}];
			return (*tuplePtr)[tag<map.localIdx[index]>{}];
		}

		template<typename integer_sequence> struct tuple_cat_impl_internal;

		template<uint64_t... indices> struct tuple_cat_impl_internal<integer_sequence<indices...>> {
			template<typename list_type> JSONIFIER_INLINE static constexpr decltype(auto) impl(list_type&& lists) noexcept {
				return result_type{ { { getIndividualElement<indices>(std::forward<list_type>(lists)) }... } };
			}
		};

		template<typename... tuple_list_types> JSONIFIER_INLINE static constexpr decltype(auto) impl(tuple_list_types&&... listVals) noexcept {
			return tuple_cat_impl_internal<make_integer_sequence<total>>::impl(lists_tuple_type{ { { &std::forward<tuple_list_types>(listVals) } }... });
		}
	};

	template<typename... tuple_list_types> JSONIFIER_INLINE static constexpr decltype(auto) tupleCat(tuple_list_types&&... listVals) noexcept {
		return tuple_cat_impl<remove_reference_t<tuple_list_types>...>::impl(std::forward<tuple_list_types>(listVals)...);
	}
}

namespace std {

	template<uint64_t I, typename... Ts> struct tuple_element<I, jsonifier::internal::tuple<Ts...>> {
		using type = jsonifier::internal::tuple_element_t<I, jsonifier::internal::tuple<Ts...>>;
	};

	template<typename... Ts> struct tuple_size<jsonifier::internal::tuple<Ts...>> : public jsonifier::internal::integral_constant<jsonifier::internal::tuple<Ts...>::size> {};

	template<uint64_t I, typename Ts>
		requires(jsonifier::internal::is_specialization_of_v<remove_cvref_t<Ts>, jsonifier::internal::tuple>)
	JSONIFIER_INLINE constexpr decltype(auto) get(Ts&& t) noexcept {
		return jsonifier::internal::get<I>(t);
	}
}
