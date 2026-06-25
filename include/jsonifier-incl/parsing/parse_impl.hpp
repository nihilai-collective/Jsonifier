/*
	MIT License

	Copyright (c) 2023 RealTimeChris

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

#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/parsing/parser.hpp>

namespace jsonifier::internal {

	enum class parse_result : uint8_t {
		inactive_member,
		active_member,
		failed,
	};

	template<typename literal_type> JSONIFIER_INLINE static constexpr auto makeQuotedKeyLiteral(const literal_type& keyLiteral) noexcept {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" };
	}

	template<typename literal_type> JSONIFIER_INLINE static constexpr auto makeFusedKeyLiteral(const literal_type& keyLiteral) noexcept {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}

	template<uint64_t index, typename literal_type> JSONIFIER_INLINE static constexpr auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
		if constexpr (index > 0) {
			return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
		} else {
			return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
		}
	}

	template<typename value_type, typename context_type, parse_options options> struct parse_types_impl {
		static constexpr auto memberCount = coreTupleSize<value_type>;

		template<uint64_t index, typename... arg_types> JSONIFIER_INLINE static parse_result parseMatchedMember(value_type& value, context_type& context) {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (concepts::has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					return context.skipValue() ? parse_result::active_member : parse_result::failed;
				}
			}
			return parse<options>::impl(getMember<ptrNew>(value), context) ? parse_result::active_member : parse_result::failed;
		}

		template<uint64_t index> inline static parse_result processIndex(value_type& value, context_type& context) {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if ((context.currentPtr() + quotedKeySize) < context.endPtr() && string_literal_comparitor<decltype(quotedKey), quotedKey>::impl(context.currentPtr())) [[likely]] {
					++context.currentIterPtr();
					if (!context.collectObjectColon()) [[unlikely]] {
						return parse_result::failed;
					}
					return parseMatchedMember<index>(value, context);
				}
			} else {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const auto keyStart					= context.currentPtr();
				if constexpr (!options.minified) {
					if (((keyStart + quotedKeySize) < context.endPtr()) && string_literal_comparitor<decltype(quotedKey), quotedKey>::impl(keyStart)) [[unlikely]] {
						context.currentPtr() += quotedKeySize;
						if (!context.collectObjectColon()) [[unlikely]] {
							return parse_result::failed;
						}
						return parseMatchedMember<index>(value, context);
					}
				} else {
					static constexpr auto fusedKey	   = makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((keyStart + fusedKeySize) < context.endPtr()) && string_literal_comparitor<decltype(fusedKey), fusedKey>::impl(keyStart)) [[likely]] {
						context.currentPtr() += fusedKeySize;
						return parseMatchedMember<index>(value, context);
					}
				}
			}
			return parse_result::inactive_member;
		}
	};

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	static constexpr auto generateFunctionPtrsNew(integer_sequence<indices...>) noexcept {
		using function_type = decltype(&parsing_type<value_type, context_type, options>::template processIndex<0>);
		return array<function_type, sizeof...(indices)>{ { &parsing_type<value_type, context_type, options>::template processIndex<indices>... } };
	}

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, typename integer_sequence>
	struct generateDispatchTableNew;

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	struct generateDispatchTableNew<parsing_type, value_type, context_type, options, integer_sequence<indices...>> {
		using fn_type = parse_result (*)(value_type&, context_type&);

		alignas(64) static constexpr array<fn_type, sizeof...(indices)> table{ { &parsing_type<value_type, context_type, options>::template processIndex<indices>... } };

		JSONIFIER_INLINE static parse_result impl(value_type& value, context_type& context, uint64_t currentIndex) {
			if constexpr (sizeof...(indices) <= JSONIFIER_DISPATCH_TABLE_COUNT) {
				parse_result result{ parse_result::inactive_member };
				static_cast<void>(
					((currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndex<indices>(value, context), true) : false) || ...));
				return result;
			} else {
				return table[currentIndex](value, context);
			}
		}
	};

	template<uint64_t memberCount> constexpr array<uint64_t, (memberCount > 0 ? memberCount : 1)> generateAntiHashStatesTableNew() {
		array<uint64_t, (memberCount > 0 ? memberCount : 1)> returnValues{};
		for (uint64_t x = 0; x < memberCount; ++x) {
			returnValues[x] = x;
		}
		return returnValues;
	}

	template<uint64_t memberCount, typename value_type>
	thread_local constinit static array<uint64_t, (memberCount > 0 ? memberCount : 1)> antiHashStatesNew{ generateAntiHashStatesTableNew<memberCount>() };

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options>
	static constexpr auto functionPtrsNew{ generateFunctionPtrsNew<parsing_type, value_type, context_type, options>(make_integer_sequence<coreTupleSize<value_type>>{}) };

	template<typename value_type, typename context_type> JSONIFIER_INLINE static string_view_ptr getStringRoot(context_type& context) noexcept {
		if constexpr (structural_context<context_type>) {
			return context.currentPtr() - *context.currentIterPtr();
		} else {
			return context.currentPtr();
		}
	}

	template<parse_options options, typename json_entity_type> struct json_entity_parse : public json_entity_type {
		static constexpr auto memberCount{ coreTupleSize<typename json_entity_type::class_type> };

		template<typename value_type, typename context_type> JSONIFIER_INLINE static parse_result tryKnownOrder(value_type& value, context_type& context) {
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((context.currentPtr() + memberLiteralSize) < context.endPtr()) &&
					string_literal_comparitor<decltype(memberLiteral), memberLiteral>::impl(context.currentPtr())) [[likely]] {
					context.currentPtr() += memberLiteralSize;
					if constexpr (concepts::has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							return context.skipValue() ? parse_result::active_member : parse_result::failed;
						}
					}
					return parse<options>::impl(getMember<ptrNew>(value), context) ? parse_result::active_member : parse_result::failed;
				}
				return parse_result::inactive_member;
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, context);
			}
		}

		template<typename value_type, typename context_type> inline static bool processIndex(value_type& value, context_type& context) {
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (context.objectMaybeEnd()) {
					return false;
				}
				if (auto result = tryKnownOrder(value, context); result != parse_result::inactive_member) {
					return result == parse_result::active_member;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!context.collectObjectComma()) [[unlikely]] {
						return false;
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(context.collectObjectSeparator())) {
						case static_cast<uint64_t>(sep_result::cont): {
							break;
						}
						default: {
							return false;
						}
					}
				} else {
					if (context.objectMaybeEnd()) {
						return false;
					}
				}
				if constexpr (options.knownOrder) {
					if (auto result = tryKnownOrder(value, context); result != parse_result::inactive_member) {
						return result == parse_result::active_member;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (auto result = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, context); result != parse_result::inactive_member)
						[[likely]] {
						return result == parse_result::active_member;
					}
				} else {
					if (auto indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
						if (auto result =
								generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>::impl(value, context, indexNew);
							result != parse_result::inactive_member) {
							return result == parse_result::active_member;
						}
						const auto stringEnd = context.endPtr();
						if (auto indexNew2 = hash_map<value_type, string_view_ptr>::findIndex(context.currentPtr() + 1, stringEnd); indexNew2 < memberCount) [[likely]] {
							if (auto result2 = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>::impl(value,
									context, indexNew2);
								result2 != parse_result::inactive_member) {
								if constexpr (options.knownOrder) {
									if (result2 == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
								}
								return result2 == parse_result::active_member;
							}
						}
					}
				}
				if (!context.template checkChar<'"'>()) [[unlikely]] {
					return context.template reject<parse_statuses::missing_key_start>();
				}
				if (!context.skipString()) [[unlikely]] {
					return false;
				}
				if (!context.collectObjectColon()) [[unlikely]] {
					return false;
				}
				if (!context.skipValue()) [[unlikely]] {
					return false;
				}
				switch (static_cast<uint64_t>(context.collectObjectSeparator())) {
					case static_cast<uint64_t>(sep_result::cont): {
						break;
					}
					default: {
						return false;
					}
				}
			}
		}
	};

	template<typename... bases> struct parse_map : public bases... {
		template<typename json_entity_type, typename... arg_types> JSONIFIER_INLINE static bool iterateValuesImpl(arg_types&&... args) {
			return json_entity_type::processIndex(internal::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static constexpr bool iterateValues([[maybe_unused]] arg_types&&... args) {
			return ((iterateValuesImpl<bases>(internal::forward<arg_types>(args)...)) && ...);
		}
	};

	template<parse_options options, typename value_type, typename context_type, typename integer_sequence> struct get_parse_base;

	template<parse_options options, typename value_type, typename context_type, uint64_t... index>
	struct get_parse_base<options, value_type, context_type, integer_sequence<index...>> {
		using type = parse_map<json_entity_parse<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<parse_options options, typename value_type, typename context_type> using parse_base_t =
		typename get_parse_base<options, value_type, context_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<concepts::jsonifier_object_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		inline static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.objectStart()) [[likely]] {
				if (context.objectMaybeEnd()) [[unlikely]] {
					return true;
				}
				if (parse_base_t<options, value_type, context_type>::iterateValues(value, context)) {
					if (context.objectMaybeEnd()) {
						return true;
					}
					if (!context.collectObjectComma()) [[unlikely]] {
						return false;
					}
					return context.skipRemainingObject();
				}
				return context.getErrors().size() == 0;
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
	template<typename key_type> JSONIFIER_INLINE static key_type& getKeyNew() {
		thread_local static key_type key{};
		return key;
	}
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif

	template<concepts::map_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		inline static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.objectStart()) [[likely]] {
				if (context.objectMaybeEnd()) [[unlikely]] {
					return true;
				}
				while (true) {
					if (!parse<options>::impl(getKeyNew<typename value_type::key_type>(), context)) [[unlikely]] {
						return false;
					}
					if (!context.collectObjectColon()) [[unlikely]] {
						return false;
					}
					if (!parse<options>::impl(value[getKeyNew<typename value_type::key_type>()], context)) [[unlikely]] {
						return false;
					}
					switch (static_cast<uint64_t>(context.collectObjectSeparator())) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							return true;
						}
						default: {
							return false;
						}
					}
				}
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::vector_t value_type, typename context_type, parse_options optionsNew> struct parse_impl<value_type, context_type, optionsNew> {
		static constexpr parse_options options{ optionsNew };
		inline static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.arrayStart()) [[likely]] {
				if (context.arrayMaybeEnd()) [[unlikely]] {
					value.clear();
					return true;
				}
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
				static thread_local value_type valueTemp;
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif
				uint64_t oldSize{ valueTemp.size() };
				uint64_t newSize{};
				if (oldSize > 0) {
					auto beginIter = getBeginIterVec(valueTemp);
					for (uint64_t x = 0; x < oldSize; ++x) {
						if (parse<options>::impl(beginIter[static_cast<int64_t>(x)], context)) [[likely]] {
							++newSize;
							switch (static_cast<uint64_t>(context.collectArraySeparator())) {
								case static_cast<uint64_t>(sep_result::cont): {
									continue;
								}
								case static_cast<uint64_t>(sep_result::ended): {
									value.resize(newSize);
									std::move(beginIter, beginIter + static_cast<int64_t>(newSize), getBeginIterVec(value));
									return true;
								}
								default: {
									return false;
								}
							}
						} else {
							return false;
						}
					}
				}
				while (context.notAtEndPre()) {
					if (parse<options>::impl(valueTemp.emplace_back(), context)) [[likely]] {
						++newSize;
						switch (static_cast<uint64_t>(context.collectArraySeparator())) {
							case static_cast<uint64_t>(sep_result::cont): {
								continue;
							}
							case static_cast<uint64_t>(sep_result::ended): {
								value.resize(newSize);
								auto beginIter = getBeginIterVec(valueTemp);
								auto endIter   = getEndIterVec(valueTemp);
								std::move(beginIter, endIter, getBeginIterVec(value));
								return true;
							}
							default: {
								return false;
							}
						}
					} else {
						return false;
					}
				}
				return context.template reject<parse_statuses::unexpected_string_end>();
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::raw_array_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		inline static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.arrayStart()) [[likely]] {
				if (context.arrayMaybeEnd()) [[unlikely]] {
					return true;
				}
				if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
					auto iterNew = std::begin(value);
					for (uint64_t i = 0; i < nLocal; ++i) {
						if (parse<options>::impl(*(iterNew++), context)) [[likely]] {
							if (context.arrayMaybeEnd()) [[unlikely]] {
								return true;
							}
							if (!context.collectArrayComma()) [[unlikely]] {
								return false;
							}
						} else {
							return false;
						}
					}
				}
				while (context.notAtEndPre()) {
					if (context.skipValue()) [[likely]] {
						if (context.arrayMaybeEnd()) [[unlikely]] {
							return true;
						}
						if (!context.collectArrayComma()) [[unlikely]] {
							return false;
						}
					} else {
						return false;
					}
				}
				return context.template reject<parse_statuses::unexpected_string_end>();
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::tuple_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		static constexpr auto memberCount = tuple_size_v<value_type>;

		template<auto... values> struct tuple_member_parser {
			template<uint64_t index> JSONIFIER_INLINE static bool impl(value_type& value, context_type& context, bool& success) noexcept {
				if (!context.template incrementIfEquals<','>()) [[unlikely]] {
					success = true;
					return false;
				}
				if (parse<options>::impl(get<index>(value), context)) [[likely]] {
					return true;
				}
				success = false;
				return false;
			}
		};

		inline static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.arrayStart()) [[likely]] {
				if (context.arrayMaybeEnd()) [[unlikely]] {
					return true;
				}
				if constexpr (memberCount > 0) {
					if (!parse<options>::impl(get<0>(value), context)) [[unlikely]] {
						return false;
					}
					if constexpr (memberCount > 1) {
						bool success{ true };
						functor_runner<tuple_member_parser, offset_sequence<make_integer_sequence<memberCount - 1>, 1>>::implAnd(value, context, success);
						if (!success) [[unlikely]] {
							return false;
						}
					}
				}
				while (!context.arrayMaybeEnd()) {
					if (!context.notAtEndPre()) [[unlikely]] {
						return context.template reject<parse_statuses::unexpected_string_end>();
					}
					if (!context.collectArrayComma()) [[unlikely]] {
						return false;
					}
					if (!context.skipValue()) [[unlikely]] {
						return false;
					}
				}
				return true;
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::string_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				context.skipWhitespace();
			}
			if (!context.template checkChar<'"'>()) [[unlikely]] {
				return context.template reject<parse_statuses::invalid_string_characters>();
			}
			return context.iterateString(value);
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::char_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.hasMoreInput()) [[likely]] {
				value = static_cast<value_type>(context.currentPtr()[1]);
				if constexpr (structural_context<context_type>) {
					++context.currentIterPtr();
				} else {
					context.currentPtr() += sizeof(value_type) + 2;
				}
				return true;
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::enum_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			uint64_t newValue{};
			if (context.iterateNumber(newValue)) [[likely]] {
				value = static_cast<value_type>(newValue);
				return true;
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			uint64_t newValue{};
			if (context.iterateNumber(newValue)) [[likely]] {
				value = static_cast<value_type>(newValue);
				return true;
			} else {
				return false;
			}
		}
	};

	template<concepts::num_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			return context.iterateRootNumber(value);
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return context.iterateNumber(value);
		}
	};

	template<concepts::bool_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			return context.iterateRootBool(value);
		}

		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return context.iterateBool(value);
		}
	};

	template<concepts::always_null_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type&, context_type& context) noexcept {
			return context.iterateNull();
		}

		JSONIFIER_INLINE static bool impl(value_type&, context_type& context) noexcept {
			return context.iterateNull();
		}
	};

	template<concepts::variant_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		template<json_type type, typename variant_type, uint64_t currentIndex = 0> inline static bool iterateVariantTypes(variant_type&& variant, context_type& context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (concepts::jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else if constexpr ((concepts::vector_t<element_type> || concepts::raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else if constexpr ((concepts::string_t<element_type> || concepts::string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else if constexpr (concepts::bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else if constexpr ((concepts::num_t<element_type> || concepts::enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else if constexpr (concepts::always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), context);
				} else {
					return iterateVariantTypes<type, variant_type, currentIndex + 1>(variant, context);
				}
			} else {
				return context.template reject<parse_statuses::unexpected_token>();
			}
		}

		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (context.hasMoreInput()) [[likely]] {
				switch (static_cast<uint8_t>(*context.currentPtr())) {
					case '{': {
						return iterateVariantTypes<json_type::object>(value, context);
					}
					case '[': {
						return iterateVariantTypes<json_type::array>(value, context);
					}
					case '"': {
						return iterateVariantTypes<json_type::string>(value, context);
					}
					case 't':
						[[fallthrough]];
					case 'f': {
						return iterateVariantTypes<json_type::boolean>(value, context);
					}
					case '-':
						[[fallthrough]];
					case '0':
						[[fallthrough]];
					case '1':
						[[fallthrough]];
					case '2':
						[[fallthrough]];
					case '3':
						[[fallthrough]];
					case '4':
						[[fallthrough]];
					case '5':
						[[fallthrough]];
					case '6':
						[[fallthrough]];
					case '7':
						[[fallthrough]];
					case '8':
						[[fallthrough]];
					case '9': {
						return iterateVariantTypes<json_type::number>(value, context);
					}
					case 'n': {
						return iterateVariantTypes<json_type::null>(value, context);
					}
					default: {
						return true;
					}
				}
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<typename context_type, parse_options options> JSONIFIER_INLINE static bool isNullValue(context_type& context) noexcept {
		if constexpr (!options.minified && !structural_context<context_type>) {
			context.skipWhitespace();
		}
		return !context.notAtEndPre() || *context.currentPtr() == 'n';
	}

	template<concepts::optional_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (!isNullValue<context_type, options>(context)) [[likely]] {
				return parse<options>::impl(value.emplace(), context);
			} else {
				value.reset();
				return context.iterateNull();
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::shared_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (!isNullValue<context_type, options>(context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) [[unlikely]] {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, context);
			} else {
				return context.iterateNull();
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::unique_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (!isNullValue<context_type, options>(context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) [[unlikely]] {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, context);
			} else {
				return context.iterateNull();
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::pointer_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type& value, context_type& context) noexcept {
			if (!isNullValue<context_type, options>(context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, context);
			} else {
				return context.iterateNull();
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::raw_json_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl([[maybe_unused]] value_type& value, context_type& context) noexcept {
			if (context.hasMoreInput()) [[likely]] {
				string_view_ptr newPtr = context.currentPtr();
				[[maybe_unused]] string_view_ptr stringRoot{};
				if constexpr (structural_context<context_type>) {
					stringRoot = context.currentPtr() - *context.currentIterPtr();
				}
				if (!context.skipValue()) [[unlikely]] {
					return false;
				}
				string_view_ptr endPtr;
				if constexpr (structural_context<context_type>) {
					endPtr = context.notAtEndPre() ? context.currentPtr() : stringRoot + *context.endPtr();
				} else {
					endPtr = context.notAtEndPre() ? context.currentPtr() : context.endPtr();
				}
				uint64_t newSize = static_cast<uint64_t>(endPtr - newPtr);
				if constexpr (!options.minified) {
					while (newSize > 0 && whitespaceTable[static_cast<uint8_t>(newPtr[newSize - 1])]) {
						--newSize;
					}
				}
				if (newSize > 0) [[likely]] {
					string newString{};
					newString.resize(newSize);
					std::memcpy(newString.data(), newPtr, newSize);
					value = value_type{ context, newString };
				}
				return true;
			} else {
				return false;
			}
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

	template<concepts::skip_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		JSONIFIER_INLINE static bool rootImpl(value_type&, context_type& context) noexcept {
			return context.skipValue();
		}
		JSONIFIER_INLINE static bool impl(value_type& value, context_type& context) noexcept {
			return rootImpl(value, context);
		}
	};

}
