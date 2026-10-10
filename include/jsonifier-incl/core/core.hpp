/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/core/core.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/string_literal.hpp>
#include <jsonifier-incl/containers/tuple.hpp>

namespace jsonifier::internal {

	struct tuple_reference {
		uint8_t oldIndex{};
		string_view key{};
	};

	struct tuple_references {
		const tuple_reference* rootPtr{};
		uint64_t count{};
	};

	template<typename integer_sequence> struct tuple_ref_collector;

	template<uint64_t... indices> struct tuple_ref_collector<integer_sequence<indices...>> {
		template<uint64_t index, typename tuple_type, uint64_t maxIndex>
		inline static constexpr void impl(const tuple_type& tuple, array<tuple_reference, maxIndex>& tupleRefsRaw) {
			tupleRefsRaw[index].oldIndex = static_cast<uint8_t>(index);
			const auto& potentialKey	 = internal::getBecauseOtherLibAuthorsResolve<index>(tuple);
			if constexpr (has_name<decltype(potentialKey)>) {
				// The hash map is matched against key text as it appears in the JSON, so build it from the escaped name.
				tupleRefsRaw[index].key = escapedKeyLiteral<remove_cvref_t<decltype(potentialKey)>::name>.operator string_view();
			}
		}

		template<typename tuple_type, uint64_t maxIndex> inline static constexpr void impl(const tuple_type& tuple, array<tuple_reference, maxIndex>& tupleRefsRaw) {
			(impl<indices>(tuple, tupleRefsRaw), ...);
		}
	};

	template<typename tuple_type> inline constexpr auto collectTupleRefs(const tuple_type& tuple) -> array<tuple_reference, tuple_size_v<remove_cvref_t<tuple_type>>> {
		constexpr auto tupleSize = tuple_size_v<remove_cvref_t<tuple_type>>;
		array<tuple_reference, tupleSize> tupleRefsRaw{};
		tuple_ref_collector<make_integer_sequence<tupleSize>>::impl(tuple, tupleRefsRaw);
		return tupleRefsRaw;
	}

	template<uint64_t size, typename comparator_type>
	inline constexpr array<tuple_reference, size> sortTupleRefs(const array<tuple_reference, size>& tupleRefsRaw, comparator_type comparator) {
		array<tuple_reference, size> returnValues{ tupleRefsRaw };
		for (uint64_t i = 1; i < size; ++i) {
			auto key  = returnValues[i];
			int64_t j = static_cast<int64_t>(i) - 1;
			while (j >= 0 && comparator(returnValues[static_cast<uint64_t>(j)], key)) {
				returnValues[static_cast<uint64_t>(j + 1)] = returnValues[static_cast<uint64_t>(j)];
				--j;
			}
			returnValues[static_cast<uint64_t>(j + 1)] = key;
		}
		return returnValues;
	}

	static constexpr auto byFirstByte = [](const tuple_reference& lhs, const tuple_reference& rhs) {
		const uint64_t lhsByte = lhs.key.size() ? static_cast<uint8_t>(lhs.key[0]) : 0ull;
		const uint64_t rhsByte = rhs.key.size() ? static_cast<uint8_t>(rhs.key[0]) : 0ull;
		return lhsByte < rhsByte;
	};

	static constexpr auto byLength = [](const tuple_reference& lhs, const tuple_reference& rhs) {
		return lhs.key.size() < rhs.key.size();
	};

	template<uint64_t size> inline static constexpr tuple_references consolidateTupleRefs(const array<tuple_reference, size>& tupleRefsRaw) {
		tuple_references returnValues{};
		if constexpr (size > 0) {
			returnValues.rootPtr = &tupleRefsRaw[0];
			returnValues.count	 = size;
		}
		return returnValues;
	}

	template<typename value_type> static constexpr auto tupleRefs{ collectTupleRefs(core<remove_cvref_t<value_type>>::parseValue) };
	template<typename value_type> static constexpr auto tupleReferences{ consolidateTupleRefs(tupleRefs<value_type>) };
	template<typename value_type> static constexpr auto sortedTupleReferencesByLength{ sortTupleRefs(tupleRefs<value_type>, byLength) };
	template<typename value_type> static constexpr auto tupleReferencesByLength{ consolidateTupleRefs(sortedTupleReferencesByLength<value_type>) };
	template<typename value_type> static constexpr auto sortedTupleReferencesByFirstByte{ sortTupleRefs(tupleRefs<value_type>, byFirstByte) };
	template<typename value_type> static constexpr auto tupleReferencesByFirstByte{ consolidateTupleRefs(sortedTupleReferencesByFirstByte<value_type>) };

	// Idea for this interface sampled from Stephen Berry and his library, Glaze library: https://github.com/stephenberry/glaze
	template<typename value_type> using core_tuple_type					  = decltype(core<base_t<value_type>>::parseValue);
	template<typename value_type> static constexpr uint64_t coreTupleSize = tuple_size_v<core_tuple_type<value_type>>;

	template<typename value_type> static constexpr bool leaf_parse_type =
		string_t<value_type> || string_view_t<value_type> || bool_t<value_type> || number_t<value_type> || enum_t<value_type> || always_null_t<value_type>;

#if JSONIFIER_COMPILER_MSVC
	static constexpr uint64_t maxParseInlineMemberCount{ 8 };
#elif (JSONIFIER_COMPILER_GCC || (JSONIFIER_COMPILER_CLANG && JSONIFIER_PLATFORM_MAC))
	static constexpr uint64_t maxParseInlineMemberCount{ 64 };
#else
	static constexpr uint64_t maxParseInlineMemberCount{ 32 };
#endif

	template<typename value_type, uint64_t budget> consteval uint64_t parseMemberCount() noexcept;

	template<uint64_t budget, typename... child_types> consteval uint64_t summedChildCount() noexcept {
		uint64_t total{};
		static_cast<void>(((total += parseMemberCount<base_t<child_types>, budget>()), ...));
		return total > budget ? budget + 1 : total;
	}

	template<typename value_type, uint64_t budget, uint64_t... indices> consteval uint64_t summedMemberCount(integer_sequence<indices...>) noexcept {
		return summedChildCount<budget, typename remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<indices>(core<value_type>::parseValue))>::member_type...>();
	}

	template<typename value_type, uint64_t budget, uint64_t... indices> consteval uint64_t summedTupleElementCount(integer_sequence<indices...>) noexcept {
		return summedChildCount<budget, decltype(get<indices>(std::declval<value_type&>()))...>();
	}

	template<typename value_type, uint64_t budget, uint64_t... indices> consteval uint64_t summedVariantAlternativeCount(integer_sequence<indices...>) noexcept {
		return summedChildCount<budget, decltype(std::get<indices>(std::declval<value_type&>()))...>();
	}

	template<typename value_type, uint64_t budget> consteval uint64_t parseMemberCount() noexcept {
		if constexpr (leaf_parse_type<value_type>) {
			return 1;
		} else if constexpr (budget == 0) {
			return 1;
		} else if constexpr (jsonifier_object_t<value_type>) {
			return summedMemberCount<value_type, budget>(make_integer_sequence<coreTupleSize<value_type>>{});
		} else if constexpr (map_t<value_type>) {
			return 1 + summedChildCount<budget - 1, typename value_type::mapped_type>();
		} else if constexpr (vector_t<value_type>) {
			return 1 + summedChildCount<budget - 1, typename value_type::value_type>();
		} else if constexpr (raw_array_t<value_type>) {
			return 1 + summedChildCount<budget - 1, decltype(std::declval<value_type&>()[0])>();
		} else if constexpr (tuple_t<value_type>) {
			return summedTupleElementCount<value_type, budget>(make_integer_sequence<tuple_size_v<value_type>>{});
		} else if constexpr (variant_t<value_type>) {
			return summedVariantAlternativeCount<value_type, budget>(make_integer_sequence<std::variant_size_v<value_type>>{});
		} else if constexpr (optional_t<value_type>) {
			return 1 + summedChildCount<budget - 1, typename value_type::value_type>();
		} else if constexpr (unique_ptr_t<value_type> || shared_ptr_t<value_type> || pointer_t<value_type>) {
			return 1 + summedChildCount<budget - 1, decltype(*std::declval<value_type&>())>();
		} else {
			return 1;
		}
	}

	template<typename value_type, uint64_t maxMemberCount> consteval bool inlinableOpType() noexcept {
		return parseMemberCount<base_t<value_type>, maxMemberCount>() <= maxMemberCount;
	}

}// namespace internal
