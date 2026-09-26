/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/parse_impl.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/parsing/parser.hpp>

namespace jsonifier::internal {

	enum class parse_result : uint8_t {
		inactive_member,
		active_member,
		ended,
		failed,
	};

	template<typename iterator_type> struct parse_step {
		iterator_type iter;
		parse_result result;
	};

	template<typename literal_type> static consteval auto makeQuotedKeyLiteral(const literal_type& keyLiteral) noexcept {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" };
	}

	template<typename literal_type> static consteval auto makeFusedKeyLiteral(const literal_type& keyLiteral) noexcept {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}

	template<uint64_t index, typename literal_type> static consteval auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
		if constexpr (index > 0) {
			return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
		} else {
			return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
		}
	}

	template<parse_options options, typename context_type> using cursor_t = json_cursor<options, typename context_type::iterator_type>;

	template<typename value_type, typename context_type, parse_options options> struct parse_types_impl {
		using iterator_type				  = typename context_type::iterator_type;
		using cursor					  = cursor_t<options, context_type>;
		using step_type					  = parse_step<iterator_type>;
		static constexpr auto memberCount = coreTupleSize<value_type>;

		template<uint64_t index>
		JSONIFIER_NON_HEAVY_INLINE static step_type processIndex(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(end, context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, end, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
					return parseMatchedMember<index>(value, iter, end, depth, context);
				}
			} else {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < end) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, end, context)) [[unlikely]] {
							return { iter, parse_result::failed };
						}
						return parseMatchedMember<index>(value, iter, end, depth, context);
					}
				} else {
					static constexpr auto fusedKey	   = makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < end) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMember<index>(value, iter, end, depth, context);
					}
				}
			}
			return { iter, parse_result::inactive_member };
		}

		template<uint64_t index>
		JSONIFIER_NON_HEAVY_INLINE static step_type parseMatchedMember(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					const bool skipped = cursor::skipValue(iter, end, context);
					return { iter, skipped ? parse_result::active_member : parse_result::failed };
				}
			}
			const iterator_type iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, end, depth, context);
			return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
		}
	};

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, typename integer_sequence>
	struct generateDispatchTableNew;

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	struct generateDispatchTableNew<parsing_type, value_type, context_type, options, integer_sequence<indices...>> {
		using iterator_type = typename context_type::iterator_type;
		using step_type		= parse_step<iterator_type>;

		inline static step_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context, uint64_t currentIndex) noexcept {
			step_type result{ iter, parse_result::inactive_member };
			static_cast<void>(((currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndex<indices>(value, iter, end, depth, context), true)
														: false) ||
				...));
			return result;
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

	template<parse_options options, typename json_entity_type> struct json_entity_parse : public json_entity_type {
		static constexpr auto memberCount{ coreTupleSize<typename json_entity_type::class_type> };

		template<typename value_type, typename context_type> JSONIFIER_NON_HEAVY_INLINE static parse_step<typename context_type::iterator_type> processIndex(value_type& value,
			typename context_type::iterator_type iter,
			typename context_type::iterator_type end, uint64_t depth, context_type& context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, end, depth, context)) {
					return { iter, parse_result::ended };
				}
				if (auto step = tryKnownOrder(value, iter, end, depth, context); step.result != parse_result::inactive_member) {
					return step;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, end, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, end, depth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							break;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							return { iter, parse_result::ended };
						}
						default: {
							return { iter, parse_result::failed };
						}
					}
				} else {
					if (cursor::objectMaybeEnd(iter, end, depth, context)) {
						return { iter, parse_result::ended };
					}
					if constexpr (!options.minified && !structural_context<context_type>) {
						cursor::skipWhitespaceScalar(iter, end);
					}
				}
				if constexpr (options.knownOrder) {
					if (auto step = tryKnownOrder(value, iter, end, depth, context); step.result != parse_result::inactive_member) {
						return step;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (auto step = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, end, depth, context);
						step.result != parse_result::inactive_member) [[likely]] {
						return step;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (auto step = dispatch::impl(value, iter, end, depth, context, indexNew); step.result != parse_result::inactive_member) {
								return step;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(end, context));
								indexNew2 < memberCount) [[likely]] {
								if (auto step2 = dispatch::impl(value, iter, end, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
									if (step2.result == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return step2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(end, context));
							indexNew2 < memberCount) [[likely]] {
							if (auto step2 = dispatch::impl(value, iter, end, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
								return step2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, end, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return { iter, parse_result::failed };
				}
				if (!cursor::skipString(iter, end, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::collectObjectColon(iter, end, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::skipValue(iter, end, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, end, depth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						break;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return { iter, parse_result::ended };
					}
					default: {
						return { iter, parse_result::failed };
					}
				}
			}
		}

		template<typename value_type, typename context_type> JSONIFIER_NON_HEAVY_INLINE static parse_step<typename context_type::iterator_type> tryKnownOrder(value_type& value,
			typename context_type::iterator_type iter,
			typename context_type::iterator_type end, uint64_t depth, context_type& context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < end) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							const bool skipped = cursor::skipValue(iter, end, context);
							return { iter, skipped ? parse_result::active_member : parse_result::failed };
						}
					}
					const auto iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, end, depth, context);
					return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
				}
				return { iter, parse_result::inactive_member };
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, end, depth, context);
			}
		}
	};

	template<typename... bases> struct parse_map : public bases... {
		template<typename value_type, typename iterator_type, typename context_type>
		JSONIFIER_NON_HEAVY_INLINE static parse_step<iterator_type> iterateValues([[maybe_unused]] value_type& value, iterator_type iter, [[maybe_unused]] iterator_type end,
			[[maybe_unused]] uint64_t depth, [[maybe_unused]] context_type& context) noexcept {
			parse_step<iterator_type> step{ iter, parse_result::active_member };
			static_cast<void>(((step = bases::processIndex(value, step.iter, end, depth, context), step.result == parse_result::active_member) && ...));
			return step;
		}
	};

	template<parse_options options, typename value_type, typename context_type, typename integer_sequence> struct get_parse_base;

	template<parse_options options, typename value_type, typename context_type, uint64_t... index>
	struct get_parse_base<options, value_type, context_type, integer_sequence<index...>> {
		using type = parse_map<json_entity_parse<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<parse_options options, typename value_type, typename context_type> using parse_base_t =
		typename get_parse_base<options, value_type, context_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		inline static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, end, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			const auto step = parse_base_t<options, value_type, context_type>::iterateValues(value, iter, end, innerDepth, context);
			if (step.result == parse_result::ended) {
				return step.iter;
			}
			if (step.result != parse_result::active_member) [[unlikely]] {
				return nullptr;
			}
			iter = step.iter;
			if (cursor::objectMaybeEnd(iter, end, innerDepth, context)) {
				return iter;
			}
			if (!cursor::collectObjectComma(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, end);
			}
			return cursor::skipRemainingObject(iter, end, innerDepth, context) ? iter : nullptr;
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

	template<map_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		inline static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, end, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			while (true) {
				iter = parse<options>::impl(getKeyNew<typename value_type::key_type>(), iter, end, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::collectObjectColon(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
				iter = parse<options>::impl(value[getKeyNew<typename value_type::key_type>()], iter, end, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, end, innerDepth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						continue;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return iter;
					}
					default: {
						return nullptr;
					}
				}
			}
		}
	};

	template<vector_t value_type, typename context_type, parse_options optionsNew> struct parse_impl<value_type, context_type, optionsNew> {
		static constexpr parse_options options{ optionsNew };
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, end, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
				value.clear();
				return iter;
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
					iter = parse<options>::impl(beginIter[static_cast<int64_t>(x)], iter, end, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					++newSize;
					switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, end, innerDepth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							moveAssignVec(value, beginIter, beginIter + static_cast<int64_t>(newSize));
							return iter;
						}
						default: {
							return nullptr;
						}
					}
				}
			}
			while (cursor::notAtEnd(iter, end)) {
				iter = parse<options>::impl(valueTemp.emplace_back(), iter, end, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				++newSize;
				switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, end, innerDepth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						continue;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						moveAssignVec(value, getBeginIterVec(valueTemp), getEndIterVec(valueTemp));
						return iter;
					}
					default: {
						return nullptr;
					}
				}
			}
			static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
			return nullptr;
		}
	};

	template<raw_array_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		inline static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, end, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
				auto iterNew = std::begin(value);
				for (uint64_t i = 0; i < nLocal; ++i) {
					iter = parse<options>::impl(*(iterNew++), iter, end, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					if (cursor::arrayMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
						return iter;
					}
					if (!cursor::collectArrayComma(iter, end, context)) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (cursor::notAtEnd(iter, end)) {
				if (!cursor::skipValue(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
				if (cursor::arrayMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
					return iter;
				}
				if (!cursor::collectArrayComma(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
			}
			static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
			return nullptr;
		}
	};

	template<tuple_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type				  = typename context_type::iterator_type;
		using cursor					  = cursor_t<options, context_type>;
		static constexpr auto memberCount = tuple_size_v<value_type>;

		template<uint64_t index>
		inline static bool parseMember(value_type& value, iterator_type& iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::template incrementIfEquals<','>(iter, end, context)) [[unlikely]] {
				return false;
			}
			iter = parse<options>::impl(get<index>(value), iter, end, depth, context);
			return iter != nullptr;
		}

		template<uint64_t... indices> inline static iterator_type parseRest(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context,
			integer_sequence<indices...>) noexcept {
			static_cast<void>((parseMember<indices + 1>(value, iter, end, depth, context) && ...));
			return iter;
		}

		inline static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, end, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, end, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if constexpr (memberCount > 0) {
				iter = parse<options>::impl(get<0>(value), iter, end, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if constexpr (memberCount > 1) {
					iter = parseRest(value, iter, end, innerDepth, context, make_integer_sequence<memberCount - 1>{});
					if (!iter) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (!cursor::arrayMaybeEnd(iter, end, innerDepth, context)) {
				if (!cursor::notAtEnd(iter, end)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
					return nullptr;
				}
				if (!cursor::collectArrayComma(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::skipValue(iter, end, context)) [[unlikely]] {
					return nullptr;
				}
			}
			return iter;
		}
	};

	template<string_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, end);
			}
			if (!cursor::template checkChar<'"'>(iter, end, context)) [[unlikely]] {
				static_cast<void>(cursor::template reject<parse_statuses::invalid_string_characters>(iter, context));
				return nullptr;
			}
			return cursor::iterateString(value, iter, end, context) ? iter : nullptr;
		}
	};

	template<char_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			if (!cursor::hasMoreInput(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			value = static_cast<value_type>(cursor::valuePtr(iter, context)[1]);
			if constexpr (structural_context<context_type>) {
				++iter;
			} else {
				iter += sizeof(value_type) + 2;
			}
			return iter;
		}
	};

	template<enum_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			uint64_t newValue{};
			if (!cursor::iterateNumber(newValue, iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			value = static_cast<value_type>(newValue);
			return iter;
		}
	};

	template<number_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			return cursor::iterateNumber(value, iter, end, context) ? iter : nullptr;
		}
	};

	template<bool_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			return cursor::iterateBool(value, iter, end, context) ? iter : nullptr;
		}
	};

	template<always_null_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type&, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			return cursor::iterateNull(iter, end, context) ? iter : nullptr;
		}
	};

	template<variant_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		template<json_type type, typename variant_type, uint64_t currentIndex = 0>
		inline static iterator_type iterateVariantTypes(variant_type&& variant, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else if constexpr ((vector_t<element_type> || raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else if constexpr ((string_t<element_type> || string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else if constexpr (bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else if constexpr ((number_t<element_type> || enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else if constexpr (always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, end, depth, context);
				} else {
					return iterateVariantTypes<type, variant_type, currentIndex + 1>(variant, iter, end, depth, context);
				}
			} else {
				static_cast<void>(cursor::template reject<parse_statuses::unexpected_token>(iter, context));
				return nullptr;
			}
		}

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, end);
			}
			if (!cursor::hasMoreInput(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			switch (static_cast<uint8_t>(*cursor::valuePtr(iter, context))) {
				case '{': {
					return iterateVariantTypes<json_type::object>(value, iter, end, depth, context);
				}
				case '[': {
					return iterateVariantTypes<json_type::array>(value, iter, end, depth, context);
				}
				case '"': {
					return iterateVariantTypes<json_type::string>(value, iter, end, depth, context);
				}
				case 't':
					[[fallthrough]];
				case 'f': {
					return iterateVariantTypes<json_type::boolean>(value, iter, end, depth, context);
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
					return iterateVariantTypes<json_type::number>(value, iter, end, depth, context);
				}
				case 'n': {
					return iterateVariantTypes<json_type::null>(value, iter, end, depth, context);
				}
				default: {
					return iter;
				}
			}
		}
	};

	template<parse_options options, typename iterator_type, typename context_type>
	JSONIFIER_INLINE static bool isNullValue(iterator_type& iter, iterator_type end, context_type& context) noexcept {
		using cursor = cursor_t<options, context_type>;
		if constexpr (!options.minified && !structural_context<context_type>) {
			cursor::skipWhitespaceScalar(iter, end);
		}
		return !cursor::notAtEnd(iter, end) || *cursor::valuePtr(iter, context) == 'n';
	}

	template<optional_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!isNullValue<options>(iter, end, context)) [[likely]] {
				return parse<options>::impl(value.emplace(), iter, end, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, end, context) ? iter : nullptr;
		}
	};

	template<shared_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!isNullValue<options>(iter, end, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, end, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, end, context) ? iter : nullptr;
		}
	};

	template<unique_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!isNullValue<options>(iter, end, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, end, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, end, context) ? iter : nullptr;
		}
	};

	template<pointer_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, iterator_type end, uint64_t depth, context_type& context) noexcept {
			if (!isNullValue<options>(iter, end, context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, iter, end, depth, context);
			}
			return cursor::iterateNull(iter, end, context) ? iter : nullptr;
		}
	};

	template<raw_json_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl([[maybe_unused]] value_type& value, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, end);
			}
			if (!cursor::hasMoreInput(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			const read_buffer_ptr newPtr = cursor::valuePtr(iter, context);
			if (!cursor::skipValue(iter, end, context)) [[unlikely]] {
				return nullptr;
			}
			const read_buffer_ptr endPtr = cursor::notAtEnd(iter, end) ? cursor::valuePtr(iter, context) : cursor::stringEnd(end, context);
			uint64_t newSize			 = static_cast<uint64_t>(endPtr - newPtr);
			if constexpr (!options.minified) {
				while (newSize > 0 && whitespaceTable[static_cast<uint8_t>(newPtr[newSize - 1])]) {
					--newSize;
				}
			}
			if (newSize > 0) [[likely]] {
				string newString{};
				newString.resize(newSize);
				memcpyWrapper(newString.data(), newPtr, newSize);
				value = value_type{ context, newString };
			}
			return iter;
		}
	};

	template<skip_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type&, iterator_type iter, iterator_type end, uint64_t, context_type& context) noexcept {
			return cursor::skipValue(iter, end, context) ? iter : nullptr;
		}
	};

}
