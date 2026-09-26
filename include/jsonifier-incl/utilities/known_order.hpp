/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/known_order.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/json_entity.hpp>
#include <jsonifier-incl/core/core.hpp>

namespace jsonifier::internal {

	template<typename... types> struct known_order_type_list {};

	template<uint64_t memberCount> using known_order_index_t = std::conditional_t<(memberCount < 256), uint8_t, uint64_t>;

	template<uint64_t memberCount> inline constexpr array<known_order_index_t<memberCount>, (memberCount != 0 ? memberCount : 1)> generateKnownOrderIndices() {
		array<known_order_index_t<memberCount>, (memberCount != 0 ? memberCount : 1)> returnValues{};
		for (uint64_t x = 0; x < memberCount; ++x) {
			returnValues[x] = static_cast<known_order_index_t<memberCount>>(x);
		}
		return returnValues;
	}

	template<typename value_type> struct known_order_slot {
		static constexpr uint64_t memberCount{ coreTupleSize<value_type> };
		array<known_order_index_t<memberCount>, (memberCount != 0 ? memberCount : 1)> indices{ generateKnownOrderIndices<memberCount>() };
	};

	template<typename value_type, uint64_t... indices> consteval auto knownOrderMemberTypes(integer_sequence<indices...>) noexcept {
		return known_order_type_list<base_t<typename remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<indices>(core<value_type>::parseValue))>::member_type>...>{};
	}

	template<typename value_type> consteval auto knownOrderChildTypes() noexcept {
		if constexpr (jsonifier_object_t<value_type>) {
			return knownOrderMemberTypes<value_type>(make_integer_sequence<coreTupleSize<value_type>>{});
		} else if constexpr (string_t<value_type>) {
			return known_order_type_list<>{};
		} else if constexpr (requires { typename value_type::mapped_type; }) {
			return known_order_type_list<base_t<typename value_type::mapped_type>>{};
		} else if constexpr (requires { typename value_type::element_type; }) {
			return known_order_type_list<base_t<typename value_type::element_type>>{};
		} else if constexpr (requires { typename value_type::value_type; }) {
			return known_order_type_list<base_t<typename value_type::value_type>>{};
		} else if constexpr (std::is_array_v<value_type>) {
			return known_order_type_list<base_t<std::remove_extent_t<value_type>>>{};
		} else if constexpr (std::is_pointer_v<value_type>) {
			return known_order_type_list<base_t<std::remove_pointer_t<value_type>>>{};
		} else {
			return known_order_type_list<>{};
		}
	}

	template<typename visited, typename... pending> struct known_order_collect;

	template<typename visited> struct known_order_collect<visited> {
		using type = visited;
	};

	template<bool alreadyVisited, typename visited, typename children, typename head, typename... tail> struct known_order_collect_step;

	template<typename visited, typename children, typename head, typename... tail> struct known_order_collect_step<true, visited, children, head, tail...> {
		using type = typename known_order_collect<visited, tail...>::type;
	};

	template<typename... visitedTypes, typename... childTypes, typename head, typename... tail>
	struct known_order_collect_step<false, known_order_type_list<visitedTypes...>, known_order_type_list<childTypes...>, head, tail...> {
		using next_visited = std::conditional_t<jsonifier_object_t<head>, known_order_type_list<visitedTypes..., head>, known_order_type_list<visitedTypes...>>;
		using type		   = typename known_order_collect<next_visited, childTypes..., tail...>::type;
	};

	template<typename... visitedTypes, typename head, typename... tail> struct known_order_collect<known_order_type_list<visitedTypes...>, head, tail...> {
		using type = typename known_order_collect_step<(std::is_same_v<visitedTypes, head> || ...), known_order_type_list<visitedTypes...>, decltype(knownOrderChildTypes<head>()),
			head, tail...>::type;
	};

	template<typename type_list> struct known_order_state;

	template<typename... types> struct known_order_state<known_order_type_list<types...>> : public known_order_slot<types>... {};

	template<typename root_type> using known_order_state_t = known_order_state<typename known_order_collect<known_order_type_list<>, base_t<root_type>>::type>;

	template<typename root_type> thread_local constinit known_order_state_t<root_type> knownOrderStates{};

	template<typename value_type> thread_local constinit known_order_slot<value_type> knownOrderFallbackSlot{};

	template<typename base_context_type, typename state_type> struct known_order_context : public base_context_type {
		using base_context_type::base_context_type;

		state_type* knownOrderState{};

		template<typename value_type> JSONIFIER_INLINE auto& knownOrderIndices() noexcept {
			if constexpr (std::is_base_of_v<known_order_slot<value_type>, state_type>) {
				return static_cast<known_order_slot<value_type>&>(*knownOrderState).indices;
			} else {
				return knownOrderFallbackSlot<value_type>.indices;
			}
		}
	};

	template<bool enabled, typename base_context_type, typename root_type> struct known_order_context_select {
		using type = base_context_type;
	};

	template<typename base_context_type, typename root_type> struct known_order_context_select<true, base_context_type, root_type> {
		using type = known_order_context<base_context_type, known_order_state_t<root_type>>;
	};

	template<bool enabled, typename base_context_type, typename root_type> using known_order_context_t =
		typename known_order_context_select<enabled, base_context_type, base_t<root_type>>::type;

	template<typename value_type, typename context_type> JSONIFIER_INLINE auto& knownOrderIndicesFor(context_type& context) noexcept {
		if constexpr (requires { context.template knownOrderIndices<base_t<value_type>>(); }) {
			return context.template knownOrderIndices<base_t<value_type>>();
		} else {
			return knownOrderFallbackSlot<base_t<value_type>>.indices;
		}
	}

}