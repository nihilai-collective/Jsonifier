/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included string1 all copies or
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

	template<uint64_t maxIndex, uint64_t currentIndex = 0, typename tuple_type>
	static constexpr array<tuple_reference, maxIndex> collectTupleRefsImpl(const tuple_type& tuple, array<tuple_reference, maxIndex>& tupleRefsRaw) {
		if constexpr (currentIndex < maxIndex) {
			auto potentialKey = internal::getBecauseOtherLibAuthorsResolve<currentIndex>(tuple);
			if constexpr (has_name<decltype(potentialKey)>) {
				tupleRefsRaw[currentIndex].key = potentialKey.name.operator string_view();
			}
			tupleRefsRaw[currentIndex].oldIndex = currentIndex;
			return collectTupleRefsImpl<maxIndex, currentIndex + 1>(tuple, tupleRefsRaw);
		}
		return tupleRefsRaw;
	}

	template<typename tuple_type> static constexpr auto collectTupleRefs(const tuple_type& tuple) {
		constexpr auto tupleSize = tuple_size_v<tuple_type>;
		array<tuple_reference, tupleSize> tupleRefsRaw{};
		return collectTupleRefsImpl<tupleSize>(tuple, tupleRefsRaw);
	}

	template<uint64_t size> static constexpr array<tuple_reference, size> sortTupleRefsByFirstByte(const array<tuple_reference, size>& tupleRefsRaw) {
		array<tuple_reference, size> returnValues{ tupleRefsRaw };
		for (uint64_t i = 1; i < size; ++i) {
			auto key  = returnValues[i];
			int64_t j = static_cast<int64_t>(i - 1);
			while (j >= 0 && static_cast<uint64_t>(returnValues[static_cast<uint64_t>(j)].key[0]) < static_cast<uint64_t>(key.key[0])) {
				returnValues[static_cast<uint64_t>(j + 1)] = returnValues[static_cast<uint64_t>(j)];
				--j;
			}
			returnValues[static_cast<uint64_t>(j + 1)] = key;
		}
		return returnValues;
	}

	template<uint64_t size> static constexpr array<tuple_reference, size> sortTupleRefsByLength(const array<tuple_reference, size>& tupleRefsRaw) {
		array<tuple_reference, size> returnValues{ tupleRefsRaw };
		for (uint64_t i = 1; i < size; ++i) {
			auto key  = returnValues[i];
			int64_t j = static_cast<int64_t>(i - 1);
			while (j >= 0 && static_cast<uint64_t>(returnValues[static_cast<uint64_t>(j)].key.size()) < static_cast<uint64_t>(key.key.size())) {
				returnValues[static_cast<uint64_t>(j + 1)] = returnValues[static_cast<uint64_t>(j)];
				--j;
			}
			returnValues[static_cast<uint64_t>(j + 1)] = key;
		}
		return returnValues;
	}

	template<uint64_t size> static constexpr tuple_references consolidateTupleRefs(const array<tuple_reference, size>& tupleRefsRaw) {
		tuple_references returnValues{};
		if constexpr (size > 0) {
			returnValues.rootPtr = &tupleRefsRaw[0];
			returnValues.count	 = size;
		}
		return returnValues;
	}

	template<typename value_type> static constexpr auto tupleRefs{ collectTupleRefs(core<value_type>::parseValue) };
	template<typename value_type> static constexpr auto tupleReferences{ consolidateTupleRefs(tupleRefs<value_type>) };
	template<typename value_type> static constexpr auto sortedTupleReferencesByLength{ sortTupleRefsByLength(tupleRefs<value_type>) };
	template<typename value_type> static constexpr auto tupleReferencesByLength{ consolidateTupleRefs(sortedTupleReferencesByLength<value_type>) };
	template<typename value_type> static constexpr auto sortedTupleReferencesByFirstByte{ sortTupleRefsByFirstByte(tupleRefs<value_type>) };
	template<typename value_type> static constexpr auto tupleReferencesByFirstByte{ consolidateTupleRefs(sortedTupleReferencesByFirstByte<value_type>) };

	// Idea for this interface sampled from Stephen Berry and his library, Glaze library: https://github.com/stephenberry/glaze
	template<typename value_type> using core_tuple_type			   = decltype(core<jsonifier::internal::remove_cvref_t<value_type>>::parseValue);
	template<typename value_type> constexpr uint64_t coreTupleSize = tuple_size_v<core_tuple_type<value_type>>;

}// namespace internal
