// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/concepts.hpp
#pragma once

#include <jsonifier-incl/utilities/forward.hpp>

namespace jsonifier {

	enum class json_type : uint8_t {
		object	= 0,
		array	= 1,
		string	= 2,
		number	= 3,
		boolean = 4,
		null	= 5,
		unset	= 8,
		count,
	};

	class string_view_base;

	template<uint64_t = 17> class string_base;

	class raw_json_data;

	// Idea for this interface sampled from Stephen Berry and his library, Glaze library: https://github.com/stephenberry/glaze
	template<typename value_type> struct core;

	struct skip {};

}

namespace jsonifier::concepts {

	template<typename, typename> static constexpr bool is_same_v						  = false;
	template<typename value_type> static constexpr bool is_same_v<value_type, value_type> = true;

	template<typename value_type> using base_t = internal::remove_cvref_t<value_type>;

	template<typename value_type>
	concept has_static_size = requires() { base_t<value_type>::staticSize; };

	template<typename value_type_01, typename value_type_02>
	concept same_as_types = is_same_v<internal::remove_reference_t<value_type_01>, internal::remove_reference_t<value_type_02>>;

	template<typename value_type>
	concept enum_types = std::is_enum_v<base_t<value_type>> && std::is_unsigned_v<std::underlying_type_t<base_t<value_type>>>;

	template<typename value_type>
	concept integral_types = std::is_integral_v<base_t<value_type>> && !same_as_types<base_t<value_type>, bool>;

	template<typename value_type>
	concept trivially_copyable_types = __is_trivial(base_t<value_type>) || std::is_standard_layout_v<base_t<value_type>>;

	template<typename value_type>
	concept int_types = std::is_signed_v<base_t<value_type>> && integral_types<value_type>;

	template<typename value_type>
	concept int8_types = int_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 1;

	template<typename value_type>
	concept int16_types = int_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 2;

	template<typename value_type>
	concept int32_types = int_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 4;

	template<typename value_type>
	concept int64_types = int_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 8;

	template<typename value_type>
	concept uint_types = std::is_unsigned_v<base_t<value_type>> && integral_types<value_type>;

	template<typename value_type>
	concept uint8_types = uint_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 1;

	template<typename value_type>
	concept uint16_types = uint_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 2;

	template<typename value_type>
	concept uint32_types = uint_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 4;

	template<typename value_type>
	concept uint64_types = uint_types<value_type> && sizeof(internal::remove_cvref_t<value_type>) == 8;

	template<typename value_type>
	concept uintegral_or_enum_types = uint_types<value_type> || enum_types<value_type>;

	template<typename value_type_01, typename value_type_02>
	concept at_least_one_enum_types = ( enum_types<value_type_01> && uint_types<value_type_02> ) || (uint_types<value_type_01> && enum_types<value_type_02>);

	template<typename value_type_01, typename value_type_02>
	concept indexable_types =
		at_least_one_enum_types<value_type_01, value_type_02> || same_as_types<value_type_01, value_type_02> || (integral_types<value_type_01> && integral_types<value_type_02>);

	template<typename value_type>
	concept skip_t = std::is_same_v<jsonifier::internal::remove_cvref_t<value_type>, skip>;

	template<typename value_type>
	concept has_range = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.begin() };
		{ value.end() };
	};

	template<typename value_type>
	concept map_subscriptable = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{
			value[typename jsonifier::internal::remove_cvref_t<value_type>::key_type{}]
		} -> std::same_as<const typename jsonifier::internal::remove_cvref_t<value_type>::mapped_type&>;
	} || requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value[typename jsonifier::internal::remove_cvref_t<value_type>::key_type{}] } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::mapped_type&>;
	};

	template<typename value_type>
	concept vector_subscriptable = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value[typename jsonifier::internal::remove_cvref_t<value_type>::size_type{}] } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::const_reference>;
	} || requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value[typename jsonifier::internal::remove_cvref_t<value_type>::size_type{}] } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::reference>;
	};

	template<typename value_type>
	concept has_size = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.size() } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::size_type>;
	};

	template<typename value_type>
	concept has_empty = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.empty() } -> std::same_as<bool>;
	};

	template<typename value_type>
	concept variant_t = requires(jsonifier::internal::remove_cvref_t<value_type> var) {
		{ var.index() } -> std::same_as<uint64_t>;
		{ var.valueless_by_exception() } -> std::same_as<bool>;
		{ std::holds_alternative<decltype(std::get<0>(var))>(var) } -> std::same_as<bool>;
		{ std::get<0>(var) } -> std::same_as<decltype(std::get<0>(var))&>;
		{ std::get_if<0>(&var) } -> std::same_as<jsonifier::internal::remove_cvref_t<decltype(std::get<0>(var))>*>;
	};

	template<typename value_type>
	concept has_resize = requires(jsonifier::internal::remove_cvref_t<value_type> value) { value.resize(typename jsonifier::internal::remove_cvref_t<value_type>::size_type{}); };

	template<typename value_type>
	concept has_reserve = requires(jsonifier::internal::remove_cvref_t<value_type> value) { value.reserve(typename jsonifier::internal::remove_cvref_t<value_type>::size_type{}); };

	template<typename value_type>
	concept has_data = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.data() };
	};

	template<typename value_type>
	concept stateless = std::is_empty_v<jsonifier::internal::remove_cvref_t<value_type>>;

	template<typename value_type>
	concept bool_t =
		std::same_as<jsonifier::internal::remove_cvref_t<value_type>, bool> || std::same_as<jsonifier::internal::remove_cvref_t<value_type>, std::vector<bool>::reference> ||
		std::same_as<jsonifier::internal::remove_cvref_t<value_type>, std::vector<bool>::const_reference>;

	template<typename value_type>
	concept always_null_t = std::same_as<jsonifier::internal::remove_cvref_t<value_type>, std::nullptr_t> ||
		std::same_as<jsonifier::internal::remove_cvref_t<value_type>, std::monostate> || std::same_as<jsonifier::internal::remove_cvref_t<value_type>, std::nullopt_t>;

	template<typename value_type>
	concept pointer_t = (std::is_pointer_v<jsonifier::internal::remove_cvref_t<value_type>> ||
							( std::is_null_pointer_v<jsonifier::internal::remove_cvref_t<value_type>> && !std::is_array_v<jsonifier::internal::remove_cvref_t<value_type>> )) &&
		!always_null_t<value_type>;

	template<typename value_type>
	concept float_t = std::floating_point<jsonifier::internal::remove_cvref_t<value_type>> && (std::numeric_limits<jsonifier::internal::remove_cvref_t<value_type>>::radix == 2) &&
		std::numeric_limits<jsonifier::internal::remove_cvref_t<value_type>>::is_iec559;

	template<typename value_type>
	concept void_t = std::is_void_v<jsonifier::internal::remove_cvref_t<value_type>>;

	template<typename value_type>
	concept char_t = std::same_as<jsonifier::internal::remove_cvref_t<value_type>, char>;

	template<typename value_type>
	concept num_t = (float_t<value_type> || uint_types<value_type> || int_types<value_type>) && !char_t<value_type>;

	template<typename value_type>
	concept has_substr = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{
			value.substr(typename jsonifier::internal::remove_cvref_t<value_type>::size_type{}, typename jsonifier::internal::remove_cvref_t<value_type>::size_type{})
		} -> std::same_as<jsonifier::internal::remove_cvref_t<value_type>>;
	};

	template<typename value_type>
	concept has_find = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.find(typename jsonifier::internal::remove_cvref_t<value_type>::value_type{}) } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::size_type>;
	} || requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.find(typename jsonifier::internal::remove_cvref_t<value_type>::key_type{}) } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::iterator>;
	} || requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{
			value.find(typename jsonifier::internal::remove_cvref_t<value_type>::key_type{})
		} -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::const_iterator>;
	};

	template<typename value_type>
	concept string_t = has_substr<value_type> && has_data<value_type> && has_size<value_type> && vector_subscriptable<value_type> && has_find<value_type>;

	template<typename value_type>
	concept string_view_t =
		has_substr<value_type> && has_data<value_type> && has_size<value_type> && vector_subscriptable<value_type> && has_find<value_type> && !has_resize<value_type>;

	template<typename value_type>
	concept map_t = map_subscriptable<value_type> && has_range<value_type> && has_size<value_type> && has_find<value_type> && has_empty<value_type>;

	template<typename value_type>
	concept pair_t = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		typename jsonifier::internal::remove_cvref_t<value_type>::first_type;
		typename jsonifier::internal::remove_cvref_t<value_type>::second_type;
	};

	template<typename value_type>
	concept has_emplace_back = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{
			value.emplace_back(typename jsonifier::internal::remove_cvref_t<value_type>::value_type{})
		} -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::reference>;
	};

	template<typename value_type>
	concept has_release = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.release() } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::pointer>;
	};

	template<typename value_type>
	concept has_reset = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.reset() } -> std::same_as<void>;
	};

	template<typename value_type>
	concept has_get = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.get() } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::element_type*>;
	};

	template<typename value_type>
	concept copyable = std::copyable<jsonifier::internal::remove_cvref_t<value_type>>;

	template<typename value_type>
	concept unique_ptr_t = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		typename jsonifier::internal::remove_cvref_t<value_type>::element_type;
		typename jsonifier::internal::remove_cvref_t<value_type>::deleter_type;
	} && has_release<value_type> && has_get<value_type>;	

	template<typename value_type>
	concept shared_ptr_t = has_reset<value_type> && has_get<value_type> && copyable<value_type>;

	template<typename value_type>
	concept any_pointer_t = unique_ptr_t<value_type> || shared_ptr_t<value_type> || pointer_t<value_type>;

	template<typename value_type>
	concept has_excluded_keys = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ value.jsonifierExcludedKeys };
	};

	template<typename value_type>
	concept nullable_t = !string_t<value_type> && requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		bool(value);
		{ *value };
	};

	template<typename value_type>
	concept null_t = nullable_t<value_type> || always_null_t<value_type>;

	template<typename value_type>
	concept raw_json_t = std::same_as<jsonifier::internal::remove_cvref_t<value_type>, raw_json_data>;

	template<typename value_type01, typename value_type02>
	concept same_character_size = requires {
		sizeof(typename jsonifier::internal::remove_cvref_t<value_type01>::value_type) == sizeof(typename jsonifier::internal::remove_cvref_t<value_type02>::value_type);
	} && string_t<value_type01> && string_t<value_type02>;

	template<typename value_type> constexpr bool hasSizeEqualToZero{ std::tuple_size_v<jsonifier::internal::remove_cvref_t<value_type>> == 0 };

	template<typename value_type>
	concept has_get_template = requires(jsonifier::internal::remove_cvref_t<value_type> value) {
		{ std::get<0>(value) } -> std::same_as<decltype(std::get<0>(value))>;
	};

	template<typename value_type>
	concept skip_or_always_null_t = skip_t<value_type> || always_null_t<value_type>;

	template<typename value_type>
	concept tuple_t = requires { std::tuple_size<jsonifier::internal::remove_cvref_t<value_type>>::value; } && !has_data<value_type>;

	template<typename value_type> using decay_keep_volatile_t = std::remove_const_t<jsonifier::internal::remove_reference_t<value_type>>;

	template<typename value_type>
	concept optional_t = requires(jsonifier::internal::remove_cvref_t<value_type> opt) {
		{ opt.has_value() } -> std::same_as<bool>;
		{ opt.value() } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::value_type&>;
		{ *opt } -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::value_type&>;
		{ opt.reset() } -> std::same_as<void>;
		{
			opt.emplace(typename jsonifier::internal::remove_cvref_t<value_type>::value_type{})
		} -> std::same_as<typename jsonifier::internal::remove_cvref_t<value_type>::value_type&>;
	};

	template<typename value_type>
	concept any_pointer_or_optional_t = any_pointer_t<value_type> || optional_t<value_type>;

	template<typename value_type>
	concept enum_t = std::is_enum_v<jsonifier::internal::remove_cvref_t<value_type>>;

	template<typename value_type>
	concept vector_t = vector_subscriptable<value_type> && has_resize<value_type> && has_emplace_back<value_type>;

	template<typename value_type>
	concept jsonifier_object_t = requires { core<jsonifier::internal::remove_cvref_t<value_type>>::parseValue; };

	template<typename value_type>
	concept raw_array_t = ( std::is_array_v<jsonifier::internal::remove_cvref_t<value_type>> && !std::is_pointer_v<jsonifier::internal::remove_cvref_t<value_type>> ) ||
		(vector_subscriptable<value_type> && !vector_t<value_type> && !has_substr<value_type> && !tuple_t<value_type>);

	template<typename value_type>
	concept buffer_like = vector_subscriptable<value_type> && has_data<value_type> && has_resize<value_type>;

	template<typename value_type>
	concept accessor_t = optional_t<value_type> || variant_t<value_type> || pointer_t<value_type> || shared_ptr_t<value_type> || unique_ptr_t<value_type>;

	template<typename value_type>
	concept time_t = internal::is_specialization_v<std::chrono::duration<jsonifier::internal::remove_cvref_t<value_type>>, std::chrono::duration>;

	template<typename value_type>
	concept integer_t =
		std::integral<jsonifier::internal::remove_cvref_t<value_type>> && !bool_t<value_type> && !std::floating_point<jsonifier::internal::remove_cvref_t<value_type>>;

	template<typename value_type>
	concept json_object_t = jsonifier_object_t<value_type> || map_t<value_type>;

	template<typename value_type>
	concept json_array_t = raw_array_t<value_type> || vector_t<value_type> || tuple_t<value_type>;

	template<typename value_type>
	concept json_bool_t = bool_t<value_type>;

	template<typename value_type>
	concept json_string_t = string_t<value_type> || string_view_t<value_type>;

	template<typename value_type>
	concept json_number_t = num_t<value_type>;

	template<typename value_type>
	concept json_null_t = always_null_t<value_type>;

	template<typename value_type>
	concept json_accessor_t = optional_t<value_type> || variant_t<value_type> || shared_ptr_t<value_type> || unique_ptr_t<value_type> || pointer_t<value_type>;

	template<typename value_type>
	concept force_inlineable_type = json_number_t<value_type> || json_bool_t<value_type> || json_string_t<value_type> || json_accessor_t<value_type> || json_null_t<value_type>;

	template<typename value_type>
	concept count_types = requires { base_t<value_type>::count; } && static_cast<uint64_t>(base_t<value_type>::count) < 128;

	template<typename value_type>
	concept printable_enum_types = enum_types<value_type> && count_types<value_type>;

	template<typename value_type>
	concept integral_constant_types = requires {
		{ base_t<value_type>::value } -> std::convertible_to<uint64_t>;
	};

}
