// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/core/core.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>
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

	template<typename value_type>
	concept has_name = requires(jsonifier::internal::remove_cvref_t<value_type> value) { value.name; };

	template<typename integer_sequence> struct tuple_ref_collector;

	template<uint64_t... indices> struct tuple_ref_collector<integer_sequence<indices...>> {
		template<uint64_t index, typename tuple_type, uint64_t maxIndex> static constexpr void impl(const tuple_type& tuple, array<tuple_reference, maxIndex>& tupleRefsRaw) {
			tupleRefsRaw[index].oldIndex = static_cast<uint8_t>(index);
			const auto& potentialKey	 = internal::getBecauseOtherLibAuthorsResolve<index>(tuple);
			if constexpr (has_name<decltype(potentialKey)>) {
				tupleRefsRaw[index].key = potentialKey.name.operator string_view();
			}
		}

		template<typename tuple_type, uint64_t maxIndex> static constexpr void impl(const tuple_type& tuple, array<tuple_reference, maxIndex>& tupleRefsRaw) {
			(impl<indices>(tuple, tupleRefsRaw), ...);
		}
	};

	template<typename tuple_type> constexpr auto collectTupleRefs(const tuple_type& tuple) -> array<tuple_reference, tuple_size_v<remove_cvref_t<tuple_type>>> {
		constexpr auto tupleSize = tuple_size_v<remove_cvref_t<tuple_type>>;
		array<tuple_reference, tupleSize> tupleRefsRaw{};
		tuple_ref_collector<make_integer_sequence<tupleSize>>::impl(tuple, tupleRefsRaw);
		return tupleRefsRaw;
	}

	template<uint64_t size, typename comparator_type>
	constexpr array<tuple_reference, size> sortTupleRefs(const array<tuple_reference, size>& tupleRefsRaw, comparator_type comparator) {
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

	template<uint64_t size> static constexpr tuple_references consolidateTupleRefs(const array<tuple_reference, size>& tupleRefsRaw) {
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
	template<typename value_type> using core_tuple_type					  = decltype(core<jsonifier::internal::remove_cvref_t<value_type>>::parseValue);
	template<typename value_type> static constexpr uint64_t coreTupleSize = tuple_size_v<core_tuple_type<value_type>>;

}// namespace internal
