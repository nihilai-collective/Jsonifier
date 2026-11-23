/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/string_search.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/string_search_fwd.hpp>
#include <jsonifier-incl/utilities/compare.hpp>
#include <jsonifier-incl/simd/bit_ops.hpp>

namespace jsonifier::internal {

	inline constexpr uint64_t searchNpos{ std::numeric_limits<uint64_t>::max() };

#if JSONIFIER_NEON || JSONIFIER_SVE2
	inline constexpr uint64_t searchMaskStride{ 4 };
	inline constexpr uint64_t searchMaskLanes{ 0x1111111111111111ull };
#else
	inline constexpr uint64_t searchMaskStride{ 1 };
	inline constexpr uint64_t searchMaskLanes{ std::numeric_limits<uint64_t>::max() };
#endif

#if JSONIFIER_AVX512
	inline constexpr uint64_t searchTopTier{ 0 };
#elif JSONIFIER_AVX2
	inline constexpr uint64_t searchTopTier{ 1 };
#else
	inline constexpr uint64_t searchTopTier{ 2 };
#endif

	template<uint64_t tierIndex> struct search_tier {
		using type_list_element = type_list_element_t<tierIndex, simd::avx_list>;
		using simd_type			= typename type_list_element::type::type;
		using integer_type		= typename type_list_element::integer_type;
		static constexpr uint64_t vectorSize{ type_list_element::bytesProcessed };
		static constexpr uint64_t maskBits{ sizeof(integer_type) * 8 };

		JSONIFIER_INLINE static simd_type broadcast(char value) noexcept {
			return simd::gatherValue<simd_type>(value);
		}

		JSONIFIER_INLINE static simd_type load(const char* text) noexcept {
			return simd::gatherValuesU<simd_type>(text);
		}

		JSONIFIER_INLINE static integer_type matches(const simd_type& values, const simd_type& needle) noexcept {
			return static_cast<integer_type>(static_cast<integer_type>(simd::opCmpEq(values, needle)) & static_cast<integer_type>(searchMaskLanes));
		}

		JSONIFIER_INLINE static uint64_t lowestByte(integer_type mask) noexcept {
			return simd::countrZeroUnsafe(mask) / searchMaskStride;
		}

		JSONIFIER_INLINE static uint64_t highestBit(integer_type mask) noexcept {
			return maskBits - 1 - simd::countlZeroUnsafe(mask);
		}

		JSONIFIER_INLINE static integer_type clearLowest(integer_type mask) noexcept {
			return static_cast<integer_type>(mask & static_cast<integer_type>(mask - 1));
		}
	};

	struct byte_matcher {
		template<uint64_t tierIndex>
		JSONIFIER_INLINE static typename search_tier<tierIndex>::integer_type impl(const typename search_tier<tierIndex>::simd_type& values, const char* set, uint64_t) noexcept {
			return search_tier<tierIndex>::matches(values, search_tier<tierIndex>::broadcast(set[0]));
		}

		JSONIFIER_INLINE static constexpr bool scalar(char value, const char* set, uint64_t) noexcept {
			return value == set[0];
		}
	};

	struct byte_set_matcher {
		template<uint64_t tierIndex> JSONIFIER_INLINE static typename search_tier<tierIndex>::integer_type impl(const typename search_tier<tierIndex>::simd_type& values,
			const char* set, uint64_t setLength) noexcept {
			using integer_type = typename search_tier<tierIndex>::integer_type;
			integer_type mask{};
			for (uint64_t x = 0; x < setLength; ++x) {
				mask = static_cast<integer_type>(mask | search_tier<tierIndex>::matches(values, search_tier<tierIndex>::broadcast(set[x])));
			}
			return mask;
		}

		JSONIFIER_INLINE static constexpr bool scalar(char value, const char* set, uint64_t setLength) noexcept {
			for (uint64_t x = 0; x < setLength; ++x) {
				if (value == set[x]) {
					return true;
				}
			}
			return false;
		}
	};

	template<typename matcher> struct inverted_matcher {
		template<uint64_t tierIndex> JSONIFIER_INLINE static typename search_tier<tierIndex>::integer_type impl(const typename search_tier<tierIndex>::simd_type& values,
			const char* set, uint64_t setLength) noexcept {
			using integer_type = typename search_tier<tierIndex>::integer_type;
			return static_cast<integer_type>(static_cast<integer_type>(~matcher::template impl<tierIndex>(values, set, setLength)) & static_cast<integer_type>(searchMaskLanes));
		}

		JSONIFIER_INLINE static constexpr bool scalar(char value, const char* set, uint64_t setLength) noexcept {
			return !matcher::scalar(value, set, setLength);
		}
	};

	template<typename matcher, uint64_t tierIndex> struct find_matching_tier {
		using tier = search_tier<tierIndex>;

		JSONIFIER_INLINE static bool impl(const char* text, uint64_t length, const char* set, uint64_t setLength, uint64_t& index) noexcept {
			static constexpr uint64_t vectorSize{ tier::vectorSize };
			if constexpr (tierIndex == searchTopTier) {
				while (index + vectorSize * 4 <= length) {
					const auto mask0{ matcher::template impl<tierIndex>(tier::load(text + index), set, setLength) };
					const auto mask1{ matcher::template impl<tierIndex>(tier::load(text + index + vectorSize), set, setLength) };
					const auto mask2{ matcher::template impl<tierIndex>(tier::load(text + index + vectorSize * 2), set, setLength) };
					const auto mask3{ matcher::template impl<tierIndex>(tier::load(text + index + vectorSize * 3), set, setLength) };
					if ((mask0 | mask1 | mask2 | mask3) != 0) {
						index += mask0 ? tier::lowestByte(mask0)
							: mask1	   ? vectorSize + tier::lowestByte(mask1)
							: mask2	   ? vectorSize * 2 + tier::lowestByte(mask2)
									   : vectorSize * 3 + tier::lowestByte(mask3);
						return false;
					}
					index += vectorSize * 4;
				}
			}
			while (index + vectorSize <= length) {
				const auto mask{ matcher::template impl<tierIndex>(tier::load(text + index), set, setLength) };
				if (mask != 0) {
					index += tier::lowestByte(mask);
					return false;
				}
				index += vectorSize;
			}
			return true;
		}
	};

	template<typename matcher, uint64_t tierIndex> struct find_matching_reverse_tier {
		using tier = search_tier<tierIndex>;

		JSONIFIER_INLINE static uint64_t hit(uint64_t blockEnd, typename tier::integer_type mask) noexcept {
			return blockEnd - tier::vectorSize + tier::highestBit(mask) / searchMaskStride;
		}

		JSONIFIER_INLINE static bool impl(const char* text, const char* set, uint64_t setLength, uint64_t& end) noexcept {
			static constexpr uint64_t vectorSize{ tier::vectorSize };
			if constexpr (tierIndex == searchTopTier) {
				while (end >= vectorSize * 4) {
					const auto mask3{ matcher::template impl<tierIndex>(tier::load(text + end - vectorSize), set, setLength) };
					const auto mask2{ matcher::template impl<tierIndex>(tier::load(text + end - vectorSize * 2), set, setLength) };
					const auto mask1{ matcher::template impl<tierIndex>(tier::load(text + end - vectorSize * 3), set, setLength) };
					const auto mask0{ matcher::template impl<tierIndex>(tier::load(text + end - vectorSize * 4), set, setLength) };
					if ((mask0 | mask1 | mask2 | mask3) != 0) {
						end = mask3 ? hit(end, mask3) : mask2 ? hit(end - vectorSize, mask2) : mask1 ? hit(end - vectorSize * 2, mask1) : hit(end - vectorSize * 3, mask0);
						return false;
					}
					end -= vectorSize * 4;
				}
			}
			while (end >= vectorSize) {
				const auto mask{ matcher::template impl<tierIndex>(tier::load(text + end - vectorSize), set, setLength) };
				if (mask != 0) {
					end = hit(end, mask);
					return false;
				}
				end -= vectorSize;
			}
			return true;
		}
	};

	template<uint64_t tierIndex> struct find_substring_tier {
		using tier = search_tier<tierIndex>;

		JSONIFIER_INLINE static bool impl(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t& index) noexcept {
			static constexpr uint64_t vectorSize{ tier::vectorSize };
			const uint64_t lastOffset{ needleLength - 1 };
			const auto firstValues{ tier::broadcast(needle[0]) };
			const auto lastValues{ tier::broadcast(needle[lastOffset]) };
			while (index + vectorSize + lastOffset <= length) {
				auto mask{ static_cast<typename tier::integer_type>(
					tier::matches(tier::load(text + index), firstValues) & tier::matches(tier::load(text + index + lastOffset), lastValues)) };
				while (mask != 0) {
					const uint64_t candidate{ index + tier::lowestByte(mask) };
					if (comparison::compare(text + candidate + 1, needle + 1, needleLength - 2)) {
						index = candidate;
						return false;
					}
					mask = tier::clearLowest(mask);
				}
				index += vectorSize;
			}
			return true;
		}
	};

	template<uint64_t tierIndex> struct rfind_substring_tier {
		using tier = search_tier<tierIndex>;

		JSONIFIER_INLINE static bool impl(const char* text, const char* needle, uint64_t needleLength, uint64_t& end) noexcept {
			static constexpr uint64_t vectorSize{ tier::vectorSize };
			const uint64_t lastOffset{ needleLength - 1 };
			const auto firstValues{ tier::broadcast(needle[0]) };
			const auto lastValues{ tier::broadcast(needle[lastOffset]) };
			while (end >= vectorSize) {
				const uint64_t base{ end - vectorSize };
				auto mask{ static_cast<typename tier::integer_type>(
					tier::matches(tier::load(text + base), firstValues) & tier::matches(tier::load(text + base + lastOffset), lastValues)) };
				while (mask != 0) {
					const uint64_t bit{ tier::highestBit(mask) };
					const uint64_t candidate{ base + bit / searchMaskStride };
					if (comparison::compare(text + candidate + 1, needle + 1, needleLength - 2)) {
						end = candidate;
						return false;
					}
					mask = static_cast<typename tier::integer_type>(mask ^ static_cast<typename tier::integer_type>(static_cast<typename tier::integer_type>(1) << bit));
				}
				end = base;
			}
			return true;
		}
	};

	struct substring_window {
		using tier = search_tier<2>;
		static constexpr uint64_t windowSize{ 16 };

		JSONIFIER_INLINE static constexpr uint64_t lowBytes(uint64_t byteCount) noexcept {
			return byteCount * searchMaskStride >= 64 ? std::numeric_limits<uint64_t>::max() : (1ull << (byteCount * searchMaskStride)) - 1;
		}

		JSONIFIER_INLINE static bool usable(uint64_t length, uint64_t needleLength) noexcept {
			return length >= windowSize && needleLength <= windowSize;
		}

		JSONIFIER_INLINE static uint64_t candidates(const char* text, uint64_t base, const char* needle, uint64_t needleLength) noexcept {
			const auto block{ tier::load(text + base) };
			const uint64_t firstMask{ static_cast<uint64_t>(tier::matches(block, tier::broadcast(needle[0]))) };
			const uint64_t lastMask{ static_cast<uint64_t>(tier::matches(block, tier::broadcast(needle[needleLength - 1]))) };
			return firstMask & (lastMask >> ((needleLength - 1) * searchMaskStride));
		}

		JSONIFIER_INLINE static bool find(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t& index) noexcept {
			if (!usable(length, needleLength)) {
				return true;
			}
			const uint64_t last{ length - needleLength };
			const uint64_t span{ windowSize + 1 - needleLength };
			while (index <= last) {
				const uint64_t base{ index + windowSize <= length ? index : length - windowSize };
				uint64_t mask{ candidates(text, base, needle, needleLength) & lowBytes(span) & ~lowBytes(index - base) };
				while (mask != 0) {
					const uint64_t candidate{ base + simd::countrZeroUnsafe(mask) / searchMaskStride };
					if (comparison::compare(text + candidate + 1, needle + 1, needleLength - 2)) {
						index = candidate;
						return false;
					}
					mask = simd::blsr(mask);
				}
				index = base + span;
			}
			return true;
		}

		JSONIFIER_INLINE static bool rfind(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t& end) noexcept {
			if (!usable(length, needleLength)) {
				return true;
			}
			const uint64_t span{ windowSize + 1 - needleLength };
			while (end > 0) {
				const uint64_t base{ end >= span ? end - span : 0 };
				uint64_t mask{ candidates(text, base, needle, needleLength) & lowBytes(end - base) };
				while (mask != 0) {
					const uint64_t bit{ 63 - simd::countlZeroUnsafe(mask) };
					const uint64_t candidate{ base + bit / searchMaskStride };
					if (comparison::compare(text + candidate + 1, needle + 1, needleLength - 2)) {
						end = candidate;
						return false;
					}
					mask ^= 1ull << bit;
				}
				end = base;
			}
			return true;
		}
	};

	struct scalar_search {
		JSONIFIER_INLINE static constexpr bool matchesAt(const char* text, const char* needle, uint64_t needleLength) noexcept {
			for (uint64_t x = 0; x < needleLength; ++x) {
				if (text[x] != needle[x]) {
					return false;
				}
			}
			return true;
		}

		JSONIFIER_INLINE static constexpr uint64_t find(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t index) noexcept {
			for (; index + needleLength <= length; ++index) {
				if (matchesAt(text + index, needle, needleLength)) {
					return index;
				}
			}
			return searchNpos;
		}

		JSONIFIER_INLINE static constexpr uint64_t rfind(const char* text, const char* needle, uint64_t needleLength, uint64_t end) noexcept {
			while (end > 0) {
				--end;
				if (matchesAt(text + end, needle, needleLength)) {
					return end;
				}
			}
			return searchNpos;
		}

		template<typename matcher>
		JSONIFIER_INLINE static constexpr uint64_t findMatching(const char* text, uint64_t length, const char* set, uint64_t setLength, uint64_t index) noexcept {
			for (; index < length; ++index) {
				if (matcher::scalar(text[index], set, setLength)) {
					return index;
				}
			}
			return searchNpos;
		}

		template<typename matcher> JSONIFIER_INLINE static constexpr uint64_t findMatchingReverse(const char* text, const char* set, uint64_t setLength, uint64_t end) noexcept {
			while (end > 0) {
				--end;
				if (matcher::scalar(text[end], set, setLength)) {
					return end;
				}
			}
			return searchNpos;
		}
	};

	struct simd_search {
		template<typename matcher> JSONIFIER_INLINE static uint64_t findMatching(const char* text, uint64_t length, const char* set, uint64_t setLength, uint64_t index) noexcept {
#if JSONIFIER_AVX512
			if (!find_matching_tier<matcher, 0>::impl(text, length, set, setLength, index)) {
				return index;
			}
#endif
#if JSONIFIER_AVX512 || JSONIFIER_AVX2
			if (!find_matching_tier<matcher, 1>::impl(text, length, set, setLength, index)) {
				return index;
			}
#endif
			if (!find_matching_tier<matcher, 2>::impl(text, length, set, setLength, index)) {
				return index;
			}
			return scalar_search::findMatching<matcher>(text, length, set, setLength, index);
		}

		template<typename matcher> JSONIFIER_INLINE static uint64_t findMatchingReverse(const char* text, const char* set, uint64_t setLength, uint64_t end) noexcept {
#if JSONIFIER_AVX512
			if (!find_matching_reverse_tier<matcher, 0>::impl(text, set, setLength, end)) {
				return end;
			}
#endif
#if JSONIFIER_AVX512 || JSONIFIER_AVX2
			if (!find_matching_reverse_tier<matcher, 1>::impl(text, set, setLength, end)) {
				return end;
			}
#endif
			if (!find_matching_reverse_tier<matcher, 2>::impl(text, set, setLength, end)) {
				return end;
			}
			return scalar_search::findMatchingReverse<matcher>(text, set, setLength, end);
		}

		JSONIFIER_INLINE static uint64_t find(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t index) noexcept {
#if JSONIFIER_AVX512
			if (!find_substring_tier<0>::impl(text, length, needle, needleLength, index)) {
				return index;
			}
#endif
#if JSONIFIER_AVX512 || JSONIFIER_AVX2
			if (!find_substring_tier<1>::impl(text, length, needle, needleLength, index)) {
				return index;
			}
#endif
			if (!find_substring_tier<2>::impl(text, length, needle, needleLength, index) || !substring_window::find(text, length, needle, needleLength, index)) {
				return index;
			}
			return scalar_search::find(text, length, needle, needleLength, index);
		}

		JSONIFIER_INLINE static uint64_t rfind(const char* text, uint64_t length, const char* needle, uint64_t needleLength, uint64_t end) noexcept {
#if JSONIFIER_AVX512
			if (!rfind_substring_tier<0>::impl(text, needle, needleLength, end)) {
				return end;
			}
#endif
#if JSONIFIER_AVX512 || JSONIFIER_AVX2
			if (!rfind_substring_tier<1>::impl(text, needle, needleLength, end)) {
				return end;
			}
#endif
			if (!rfind_substring_tier<2>::impl(text, needle, needleLength, end) || !substring_window::rfind(text, length, needle, needleLength, end)) {
				return end;
			}
			return scalar_search::rfind(text, needle, needleLength, end);
		}
	};

	struct search_needle {
		const char* dataVal{};
		uint64_t position{};
		uint64_t sizeVal{};
		char single{};

		JSONIFIER_INLINE constexpr const char* data() const noexcept {
			return dataVal ? dataVal : &single;
		}
	};

	template<uint64_t defaultPosition> struct search_needle_builder {
		template<std::integral position_type = uint64_t> JSONIFIER_INLINE static constexpr search_needle impl(char value, position_type position = defaultPosition) noexcept {
			return search_needle{ nullptr, static_cast<uint64_t>(position), 1, value };
		}

		template<std::integral position_type = uint64_t>
		JSONIFIER_INLINE static constexpr search_needle impl(const char* value, position_type position = defaultPosition) noexcept {
			return search_needle{ value, static_cast<uint64_t>(position), value ? std::char_traits<char>::length(value) : 0, {} };
		}

		template<std::integral position_type, std::integral count_type>
		JSONIFIER_INLINE static constexpr search_needle impl(const char* value, position_type position, count_type count) noexcept {
			return search_needle{ value, static_cast<uint64_t>(position), static_cast<uint64_t>(count), {} };
		}

		template<search_string_types string_type, std::integral position_type = uint64_t>
		JSONIFIER_INLINE static constexpr search_needle impl(const string_type& value, position_type position = defaultPosition) noexcept {
			return search_needle{ value.data(), static_cast<uint64_t>(position), static_cast<uint64_t>(value.size()), {} };
		}
	};

	struct string_search_dispatch {
		template<typename single_matcher, typename set_matcher>
		JSONIFIER_INLINE static constexpr uint64_t forward(const char* text, uint64_t length, const search_needle& needle) noexcept {
			if (needle.position >= length) {
				return searchNpos;
			}
			if consteval {
				return scalar_search::findMatching<set_matcher>(text, length, needle.data(), needle.sizeVal, needle.position);
			} else {
				return needle.sizeVal == 1 ? simd_search::findMatching<single_matcher>(text, length, needle.data(), 1, needle.position)
										   : simd_search::findMatching<set_matcher>(text, length, needle.data(), needle.sizeVal, needle.position);
			}
		}

		template<typename single_matcher, typename set_matcher>
		JSONIFIER_INLINE static constexpr uint64_t reverse(const char* text, uint64_t length, const search_needle& needle) noexcept {
			if (length == 0) {
				return searchNpos;
			}
			const uint64_t end{ (needle.position < length ? needle.position : length - 1) + 1 };
			if consteval {
				return scalar_search::findMatchingReverse<set_matcher>(text, needle.data(), needle.sizeVal, end);
			} else {
				return needle.sizeVal == 1 ? simd_search::findMatchingReverse<single_matcher>(text, needle.data(), 1, end)
										   : simd_search::findMatchingReverse<set_matcher>(text, needle.data(), needle.sizeVal, end);
			}
		}
	};

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::find(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<0>::impl(std::forward<arg_types>(args)...) };
		const uint64_t position{ needle.position };
		if (needle.sizeVal == 0) {
			return position <= length ? position : searchNpos;
		}
		if (position > length || needle.sizeVal > length - position) {
			return searchNpos;
		}
		if consteval {
			return scalar_search::find(text, length, needle.data(), needle.sizeVal, position);
		} else {
			return needle.sizeVal == 1 ? simd_search::findMatching<byte_matcher>(text, length, needle.data(), 1, position)
									   : simd_search::find(text, length, needle.data(), needle.sizeVal, position);
		}
	}

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::rfind(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<searchNpos>::impl(std::forward<arg_types>(args)...) };
		if (needle.sizeVal > length) {
			return searchNpos;
		}
		if (needle.sizeVal == 0) {
			return needle.position < length ? needle.position : length;
		}
		const uint64_t end{ (needle.position < length - needle.sizeVal ? needle.position : length - needle.sizeVal) + 1 };
		if consteval {
			return scalar_search::rfind(text, needle.data(), needle.sizeVal, end);
		} else {
			return needle.sizeVal == 1 ? simd_search::findMatchingReverse<byte_matcher>(text, needle.data(), 1, end)
									   : simd_search::rfind(text, length, needle.data(), needle.sizeVal, end);
		}
	}

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::findFirstOf(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<0>::impl(std::forward<arg_types>(args)...) };
		return string_search_dispatch::forward<byte_matcher, byte_set_matcher>(text, length, needle);
	}

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::findFirstNotOf(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<0>::impl(std::forward<arg_types>(args)...) };
		return string_search_dispatch::forward<inverted_matcher<byte_matcher>, inverted_matcher<byte_set_matcher>>(text, length, needle);
	}

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::findLastOf(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<searchNpos>::impl(std::forward<arg_types>(args)...) };
		return string_search_dispatch::reverse<byte_matcher, byte_set_matcher>(text, length, needle);
	}

	template<typename... arg_types> JSONIFIER_INLINE constexpr uint64_t string_search::findLastNotOf(const char* text, uint64_t length, arg_types&&... args) noexcept {
		const search_needle needle{ search_needle_builder<searchNpos>::impl(std::forward<arg_types>(args)...) };
		return string_search_dispatch::reverse<inverted_matcher<byte_matcher>, inverted_matcher<byte_set_matcher>>(text, length, needle);
	}

}
