// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/json_entity.hpp
#pragma once

#include <jsonifier-incl/utilities/reflection.hpp>

namespace jsonifier::internal {

	template<auto memberPtrNew, string_literal nameNew> struct json_entity_temp {
		using member_type = remove_class_pointer_t<decltype(memberPtrNew)>;
		using class_type  = remove_member_pointer_t<decltype(memberPtrNew)>;
		static constexpr member_type class_type::* memberPtr{ memberPtrNew };
		static constexpr string_literal name{ nameNew };
	};

	template<auto memberPtrNew, string_literal nameNew, uint64_t indexNew, uint64_t maxIndex> struct json_entity {
		using member_type = remove_class_pointer_t<decltype(memberPtrNew)>;
		using class_type  = remove_member_pointer_t<decltype(memberPtrNew)>;
		static constexpr member_type class_type::* memberPtr{ memberPtrNew };
		static constexpr bool isItLast{ indexNew == maxIndex - 1 };
		static constexpr string_literal name{ nameNew };
		static constexpr uint64_t index{ indexNew };
	};

	template<typename value_type>
	concept is_json_entity_temp = requires {
		typename value_type::class_type;
		value_type::memberPtr;
	};

	template<uint64_t maxIndex, uint64_t index, auto value> static constexpr decltype(auto) makeJsonEntityAuto() noexcept {
		if constexpr (is_json_entity_temp<decltype(value)>) {
			return json_entity<value.memberPtr, value.name, index, maxIndex>{};
		} else {
			constexpr auto nameNew = getName<value>();
			return json_entity<value, stringLiteralFromView<nameNew.size()>(nameNew), index, maxIndex>{};
		}
	}

	template<typename value_type>
	concept convertible_to_json_entity = is_json_entity_temp<value_type> || std::is_member_pointer_v<value_type>;

	template<auto... values, uint64_t... indices> static constexpr auto createValueImpl(jsonifier::internal::integer_sequence<indices...>) {
		static_assert((convertible_to_json_entity<decltype(values)> && ...), "All arguments passed to createValue must be convertible to a json_entity.");
		return makeTuple(makeJsonEntityAuto<sizeof...(values), indices, values>()...);
	}

	template<auto element, typename value_type_new> JSONIFIER_INLINE decltype(auto) getMember(value_type_new& value) noexcept {
		using value_type = remove_cvref_t<decltype(element)>;
		if constexpr (std::is_member_object_pointer_v<value_type>) {
			return value.*element;
		} else if constexpr (std::is_pointer_v<value_type>) {
			return *element;
		} else {
			return element;
		}
	}

}

namespace jsonifier {

	template<auto memberPtr, internal::string_literal nameNew> static constexpr auto makeJsonEntity() {
		return internal::json_entity_temp<memberPtr, nameNew>{};
	}

	template<auto... values> static constexpr auto createValue() noexcept {
		return internal::createValueImpl<values...>(jsonifier::internal::make_integer_sequence<sizeof...(values)>{});
	}

}
