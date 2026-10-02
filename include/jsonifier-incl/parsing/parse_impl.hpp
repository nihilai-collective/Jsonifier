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
		active_member,
		inactive_member,
		ended,
		failed,
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

	template<parse_options options, typename context_type> JSONIFIER_INLINE static void skipLeadingWhitespace([[maybe_unused]] auto&& __restrict iter, [[maybe_unused]] auto endIter) noexcept {
		if constexpr (!options.minified && !structural_context<context_type>) {
			cursor_t<options, context_type>::skipWhitespaceScalar(iter, endIter);
		}
	}

	template<parse_options options, typename context_type> JSONIFIER_INLINE static parse_result nextObjectMember(auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
		using cursor = cursor_t<options, context_type>;
		cursor::skipWhitespaceClose(iter, endIter, context);
		if (cursor::collectObjectSeparator(iter, endIter, context)) [[likely]] {
			return parse_result::active_member;
		}
		if (cursor::objectMaybeEnd(iter, endIter, context)) {
			return parse_result::ended;
		}
		static_cast<void>(cursor::template reject<parse_statuses::missing_comma>(iter, context));
		return parse_result::failed;
	}

	template<parse_options options, typename context_type> JSONIFIER_INLINE static parse_result nextArrayElement(auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
		using cursor = cursor_t<options, context_type>;
		cursor::skipWhitespaceClose(iter, endIter, context);
		if (cursor::collectArraySeparator(iter, endIter, context)) [[likely]] {
			return parse_result::active_member;
		}
		if (cursor::arrayMaybeEnd(iter, endIter, context)) {
			return parse_result::ended;
		}
		static_cast<void>(cursor::template reject<parse_statuses::missing_comma>(iter, context));
		return parse_result::failed;
	}

	template<typename value_type, typename context_type, parse_options options> struct parse_types_impl {
		using cursor					  = cursor_t<options, context_type>;
		static constexpr auto memberCount = coreTupleSize<value_type>;

		template<uint64_t index>
		JSONIFIER_NOT_ALWAYS_INLINE static parse_result processIndex(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(endIter, context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
						return parse_result::failed;
					}
					return parseMatchedMember<index>(value, iter, endIter, context);
				}
			} else {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
							return parse_result::failed;
						}
						return parseMatchedMember<index>(value, iter, endIter, context);
					}
				} else {
					static constexpr auto fusedKey	   = makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMember<index>(value, iter, endIter, context);
					}
				}
			}
			return parse_result::inactive_member;
		}

		template<uint64_t index>
		JSONIFIER_INLINE static parse_result parseMatchedMember(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					return cursor::skipValue(iter, endIter, context) ? parse_result::active_member : parse_result::failed;
				}
			}
			return parse<options>::impl(getMember<ptrNew>(value), iter, endIter, context) ? parse_result::active_member : parse_result::failed;
		}

		template<uint64_t index>
		JSONIFIER_NOINLINE static parse_result processIndexOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(endIter, context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
						return parse_result::failed;
					}
					return parseMatchedMemberOutline<index>(value, iter, endIter, context);
				}
			} else {
				static constexpr auto quotedKey		= makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
							return parse_result::failed;
						}
						return parseMatchedMemberOutline<index>(value, iter, endIter, context);
					}
				} else {
					static constexpr auto fusedKey	   = makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMemberOutline<index>(value, iter, endIter, context);
					}
				}
			}
			return parse_result::inactive_member;
		}

		template<uint64_t index>
		JSONIFIER_NOINLINE static parse_result parseMatchedMemberOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					return cursor::skipValue(iter, endIter, context) ? parse_result::active_member : parse_result::failed;
				}
			}
			return parse<options>::impl(getMember<ptrNew>(value), iter, endIter, context) ? parse_result::active_member : parse_result::failed;
		}
	};

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, typename integer_sequence>
	struct generateDispatchTableNew;

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	struct generateDispatchTableNew<parsing_type, value_type, context_type, options, integer_sequence<indices...>> {
		JSONIFIER_INLINE static parse_result impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context, uint64_t currentIndex) noexcept {
			parse_result result{ parse_result::inactive_member };
			static_cast<void>(
				((currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndex<indices>(value, iter, endIter, context), true) : false) ||
					...));
			return result;
		}

		JSONIFIER_NOINLINE static parse_result implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context, uint64_t currentIndex) noexcept {
			parse_result result{ parse_result::inactive_member };
			static_cast<void>((
				(currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndexOutline<indices>(value, iter, endIter, context), true)
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

		template<typename value_type, typename context_type>
		JSONIFIER_NOT_ALWAYS_INLINE static parse_result processIndex(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, endIter, context)) {
					return parse_result::ended;
				}
				if (const auto result = tryKnownOrder(value, iter, endIter, context); result != parse_result::inactive_member) {
					return result;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, endIter, context)) [[unlikely]] {
						return parse_result::failed;
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					if (const auto sep = nextObjectMember<options>(iter, endIter, context); sep != parse_result::active_member) {
						return sep;
					}
				} else {
					cursor::skipWhitespaceClose(iter, endIter, context);
					if (cursor::objectMaybeEnd(iter, endIter, context)) {
						return parse_result::ended;
					}
					skipLeadingWhitespace<options, context_type>(iter, endIter);
				}
				if constexpr (options.knownOrder) {
					if (const auto result = tryKnownOrder(value, iter, endIter, context); result != parse_result::inactive_member) {
						return result;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (const auto result = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, endIter, context);
						result != parse_result::inactive_member) [[likely]] {
						return result;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (const auto result = dispatch::impl(value, iter, endIter, context, indexNew); result != parse_result::inactive_member) {
								return result;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(endIter, context));
								indexNew2 < memberCount) [[likely]] {
								if (const auto result2 = dispatch::impl(value, iter, endIter, context, indexNew2); result2 != parse_result::inactive_member) {
									if (result2 == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return result2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(endIter, context));
							indexNew2 < memberCount) [[likely]] {
							if (const auto result2 = dispatch::impl(value, iter, endIter, context, indexNew2); result2 != parse_result::inactive_member) {
								return result2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, endIter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return parse_result::failed;
				}
				if (!cursor::skipString(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (const auto sep = nextObjectMember<options>(iter, endIter, context); sep != parse_result::active_member) {
					return sep;
				}
			}
		}

		template<typename value_type, typename context_type>
		JSONIFIER_INLINE static parse_result tryKnownOrder(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							return cursor::skipValue(iter, endIter, context) ? parse_result::active_member : parse_result::failed;
						}
					}
					return parse<options>::impl(getMember<ptrNew>(value), iter, endIter, context) ? parse_result::active_member : parse_result::failed;
				}
				return parse_result::inactive_member;
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, endIter, context);
			}
		}

		template<typename value_type, typename context_type>
		JSONIFIER_NOINLINE static parse_result processIndexOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, endIter, context)) {
					return parse_result::ended;
				}
				if (const auto result = tryKnownOrderOutline(value, iter, endIter, context); result != parse_result::inactive_member) {
					return result;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, endIter, context)) [[unlikely]] {
						return parse_result::failed;
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					if (const auto sep = nextObjectMember<options>(iter, endIter, context); sep != parse_result::active_member) {
						return sep;
					}
				} else {
					cursor::skipWhitespaceClose(iter, endIter, context);
					if (cursor::objectMaybeEnd(iter, endIter, context)) {
						return parse_result::ended;
					}
					skipLeadingWhitespace<options, context_type>(iter, endIter);
				}
				if constexpr (options.knownOrder) {
					if (const auto result = tryKnownOrder(value, iter, endIter, context); result != parse_result::inactive_member) {
						return result;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (const auto result = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, endIter, context);
						result != parse_result::inactive_member) [[likely]] {
						return result;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (const auto result = dispatch::impl(value, iter, endIter, context, indexNew); result != parse_result::inactive_member) {
								return result;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(endIter, context));
								indexNew2 < memberCount) [[likely]] {
								if (const auto result2 = dispatch::impl(value, iter, endIter, context, indexNew2); result2 != parse_result::inactive_member) {
									if (result2 == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return result2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(endIter, context));
							indexNew2 < memberCount) [[likely]] {
							if (const auto result2 = dispatch::impl(value, iter, endIter, context, indexNew2); result2 != parse_result::inactive_member) {
								return result2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, endIter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return parse_result::failed;
				}
				if (!cursor::skipString(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (const auto sep = nextObjectMember<options>(iter, endIter, context); sep != parse_result::active_member) {
					return sep;
				}
			}
		}

		template<typename value_type, typename context_type>
		JSONIFIER_NOINLINE static parse_result tryKnownOrderOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							return cursor::skipValue(iter, endIter, context) ? parse_result::active_member : parse_result::failed;
						}
					}
					return parse<options>::impl(getMember<ptrNew>(value), iter, endIter, context) ? parse_result::active_member : parse_result::failed;
				}
				return parse_result::inactive_member;
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, endIter, context);
			}
		}
	};

	template<typename... bases> struct parse_map : public bases... {
		template<typename value_type, typename context_type>
		JSONIFIER_INLINE static parse_result iterateValues([[maybe_unused]] value_type& __restrict value, [[maybe_unused]] auto&& __restrict iter, [[maybe_unused]] auto endIter,
			[[maybe_unused]] context_type& __restrict context) noexcept {
			parse_result result{ parse_result::active_member };
			static_cast<void>(((result = bases::processIndex(value, iter, endIter, context), result == parse_result::active_member) && ...));
			return result;
		}

		template<typename value_type, typename context_type>
		JSONIFIER_NOINLINE static parse_result iterateValuesOutline([[maybe_unused]] value_type& __restrict value, [[maybe_unused]] auto&& __restrict iter, [[maybe_unused]] auto endIter,
			[[maybe_unused]] context_type& __restrict context) noexcept {
			parse_result result{ parse_result::active_member };
			static_cast<void>(((result = bases::processIndexOutline(value, iter, endIter, context), result == parse_result::active_member) && ...));
			return result;
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
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			const auto result = parse_base_t<options, value_type, context_type>::iterateValues(value, iter, endIter, context);
			if (result == parse_result::ended) {
				return true;
			}
			if (result != parse_result::active_member) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) {
				return true;
			}
			if (!cursor::collectObjectComma(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			return cursor::skipRemainingObject(iter, endIter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			const auto result = parse_base_t<options, value_type, context_type>::iterateValuesOutline(value, iter, endIter, context);
			if (result == parse_result::ended) {
				return true;
			}
			if (result != parse_result::active_member) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) {
				return true;
			}
			if (!cursor::collectObjectComma(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			return cursor::skipRemainingObject(iter, endIter, context);
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
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			while (true) {
				if (!parse<options>::impl(getKeyNew<typename value_type::key_type>(), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!parse<options>::impl(value[getKeyNew<typename value_type::key_type>()], iter, endIter, context)) [[unlikely]] {
					return false;
				}
				const auto sep = nextObjectMember<options>(iter, endIter, context);
				if (sep == parse_result::active_member) [[likely]] {
					continue;
				}
				return sep == parse_result::ended;
			}
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::objectMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			while (true) {
				if (!parse<options>::impl(getKeyNew<typename value_type::key_type>(), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!cursor::collectObjectColon(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!parse<options>::impl(value[getKeyNew<typename value_type::key_type>()], iter, endIter, context)) [[unlikely]] {
					return false;
				}
				const auto sep = nextObjectMember<options>(iter, endIter, context);
				if (sep == parse_result::active_member) [[likely]] {
					continue;
				}
				return sep == parse_result::ended;
			}
		}
	};

#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wexit-time-destructors"
#endif
	template<typename value_type> static thread_local value_type valueTemp;
#if JSONIFIER_COMPILER_CLANG
	#pragma clang diagnostic pop
#endif

	template<vector_t value_type, typename context_type, parse_options optionsNew> struct parse_impl<value_type, context_type, optionsNew> {
		static constexpr parse_options options{ optionsNew };
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				value.clear();
				return true;
			}
			uint64_t oldSize{ valueTemp<value_type>.size() };
			uint64_t newSize{};
			if (oldSize > 0) {
				auto beginIter = getBeginIterVec(valueTemp<value_type>);
				for (uint64_t x = 0; x < oldSize; ++x) {
					if (!parse<options>::impl(beginIter[static_cast<int64_t>(x)], iter, endIter, context)) [[unlikely]] {
						return false;
					}
					++newSize;
					const auto sep = nextArrayElement<options>(iter, endIter, context);
					if (sep == parse_result::active_member) [[likely]] {
						continue;
					}
					if (sep == parse_result::ended) {
						moveAssignVec(value, beginIter, beginIter + static_cast<int64_t>(newSize));
						return true;
					}
					return false;
				}
			}
			while (cursor::notAtEnd(iter, endIter)) {
				if (!parse<options>::impl(valueTemp<value_type>.emplace_back(), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				++newSize;
				const auto sep = nextArrayElement<options>(iter, endIter, context);
				if (sep == parse_result::active_member) [[likely]] {
					continue;
				}
				if (sep == parse_result::ended) {
					moveAssignVec(value, getBeginIterVec(valueTemp<value_type>), getEndIterVec(valueTemp<value_type>));
					return true;
				}
				return false;
			}
			return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				value.clear();
				return true;
			}
			uint64_t oldSize{ valueTemp<value_type>.size() };
			uint64_t newSize{};
			if (oldSize > 0) {
				auto beginIter = getBeginIterVec(valueTemp<value_type>);
				for (uint64_t x = 0; x < oldSize; ++x) {
					if (!parse<options>::impl(beginIter[static_cast<int64_t>(x)], iter, endIter, context)) [[unlikely]] {
						return false;
					}
					++newSize;
					const auto sep = nextArrayElement<options>(iter, endIter, context);
					if (sep == parse_result::active_member) [[likely]] {
						continue;
					}
					if (sep == parse_result::ended) {
						moveAssignVec(value, beginIter, beginIter + static_cast<int64_t>(newSize));
						return true;
					}
					return false;
				}
			}
			while (cursor::notAtEnd(iter, endIter)) {
				if (!parse<options>::impl(valueTemp<value_type>.emplace_back(), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				++newSize;
				const auto sep = nextArrayElement<options>(iter, endIter, context);
				if (sep == parse_result::active_member) [[likely]] {
					continue;
				}
				if (sep == parse_result::ended) {
					moveAssignVec(value, getBeginIterVec(valueTemp<value_type>), getEndIterVec(valueTemp<value_type>));
					return true;
				}
				return false;
			}
			return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
		}
	};

	template<raw_array_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
				auto iterNew = std::begin(value);
				for (uint64_t i = 0; i < nLocal; ++i) {
					if (!parse<options>::impl(*(iterNew++), iter, endIter, context)) [[unlikely]] {
						return false;
					}
					cursor::skipWhitespaceClose(iter, endIter, context);
					if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
						return true;
					}
					if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
						return false;
					}
				}
			}
			while (cursor::notAtEnd(iter, endIter)) {
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				cursor::skipWhitespaceClose(iter, endIter, context);
				if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
					return true;
				}
				if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
			return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
				auto iterNew = std::begin(value);
				for (uint64_t i = 0; i < nLocal; ++i) {
					if (!parse<options>::impl(*(iterNew++), iter, endIter, context)) [[unlikely]] {
						return false;
					}
					cursor::skipWhitespaceClose(iter, endIter, context);
					if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
						return true;
					}
					if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
						return false;
					}
				}
			}
			while (cursor::notAtEnd(iter, endIter)) {
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				cursor::skipWhitespaceClose(iter, endIter, context);
				if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
					return true;
				}
				if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
			return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
		}
	};

	template<tuple_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor					  = cursor_t<options, context_type>;
		static constexpr auto memberCount = tuple_size_v<value_type>;

		template<uint64_t index> JSONIFIER_INLINE static bool parseMember(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return parse<options>::impl(get<index>(value), iter, endIter, context);
		}

		template<uint64_t... indices>
		JSONIFIER_INLINE static bool parseRest(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context, integer_sequence<indices...>) noexcept {
			bool succeeded{ true };
			static_cast<void>(
				((cursor::template incrementIfEquals<','>(iter, endIter, context) && (succeeded = parseMember<indices + 1>(value, iter, endIter, context))) && ...));
			return succeeded;
		}

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			if constexpr (memberCount > 0) {
				if (!parse<options>::impl(get<0>(value), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if constexpr (memberCount > 1) {
					if (!parseRest(value, iter, endIter, context, make_integer_sequence<memberCount - 1>{})) [[unlikely]] {
						return false;
					}
				}
			}
			while (true) {
				cursor::skipWhitespaceClose(iter, endIter, context);
				if (cursor::arrayMaybeEnd(iter, endIter, context)) {
					return true;
				}
				if (!cursor::notAtEnd(iter, endIter)) [[unlikely]] {
					return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
				}
				if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<uint64_t index> JSONIFIER_NOINLINE static bool parseMemberOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return parseMember<index>(value, iter, endIter, context);
		}

		template<uint64_t... indices>
		JSONIFIER_NOINLINE static bool parseRestOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context, integer_sequence<indices...>) noexcept {
			bool succeeded{ true };
			static_cast<void>(
				((cursor::template incrementIfEquals<','>(iter, endIter, context) && (succeeded = parseMemberOutline<indices + 1>(value, iter, endIter, context))) && ...));
			return succeeded;
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::arrayStart(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			cursor::skipWhitespaceClose(iter, endIter, context);
			if (cursor::arrayMaybeEnd(iter, endIter, context)) [[unlikely]] {
				return true;
			}
			if constexpr (memberCount > 0) {
				if (!parse<options>::impl(get<0>(value), iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if constexpr (memberCount > 1) {
					if (!parseRestOutline(value, iter, endIter, context, make_integer_sequence<memberCount - 1>{})) [[unlikely]] {
						return false;
					}
				}
			}
			while (true) {
				cursor::skipWhitespaceClose(iter, endIter, context);
				if (cursor::arrayMaybeEnd(iter, endIter, context)) {
					return true;
				}
				if (!cursor::notAtEnd(iter, endIter)) [[unlikely]] {
					return cursor::template reject<parse_statuses::unexpected_string_end>(iter, context);
				}
				if (!cursor::collectArrayComma(iter, endIter, context)) [[unlikely]] {
					return false;
				}
				if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
					return false;
				}
			}
		}
	};

	template<string_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			if (!cursor::template checkChar<'"'>(iter, endIter, context)) [[unlikely]] {
				return cursor::template reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			return cursor::iterateString(value, iter, endIter, context);
		}
	};

	template<char_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!cursor::hasMoreInput(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			value = static_cast<value_type>(cursor::valuePtr(iter, context)[1]);
			if constexpr (structural_context<context_type>) {
				++iter;
			} else {
				iter += sizeof(value_type) + 2;
			}
			return true;
		}
	};

	template<enum_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			uint64_t newValue{};
			if (!cursor::iterateNumber(newValue, iter, endIter, context)) [[unlikely]] {
				return false;
			}
			value = static_cast<value_type>(newValue);
			return true;
		}
	};

	template<number_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return cursor::iterateNumber(value, iter, endIter, context);
		}
	};

	template<bool_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return cursor::iterateBool(value, iter, endIter, context);
		}
	};

	template<always_null_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type&, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return cursor::iterateNull(iter, endIter, context);
		}
	};

	template<variant_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		template<json_type type, typename variant_type, uint64_t currentIndex = 0>
		JSONIFIER_INLINE static bool iterateVariantTypes(variant_type&& variant, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((vector_t<element_type> || raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((string_t<element_type> || string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr (bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((number_t<element_type> || enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr (always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else {
					return iterateVariantTypes<type, variant_type, currentIndex + 1>(variant, iter, endIter, context);
				}
			} else {
				return cursor::template reject<parse_statuses::unexpected_token>(iter, context);
			}
		}

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			if (!cursor::hasMoreInput(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			switch (static_cast<uint8_t>(*cursor::valuePtr(iter, context))) {
				case '{': {
					return iterateVariantTypes<json_type::object>(value, iter, endIter, context);
				}
				case '[': {
					return iterateVariantTypes<json_type::array>(value, iter, endIter, context);
				}
				case '"': {
					return iterateVariantTypes<json_type::string>(value, iter, endIter, context);
				}
				case 't':
					[[fallthrough]];
				case 'f': {
					return iterateVariantTypes<json_type::boolean>(value, iter, endIter, context);
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
					return iterateVariantTypes<json_type::number>(value, iter, endIter, context);
				}
				case 'n': {
					return iterateVariantTypes<json_type::null>(value, iter, endIter, context);
				}
				default: {
					return true;
				}
			}
		}

		template<json_type type, typename variant_type, uint64_t currentIndex = 0>
		JSONIFIER_NOINLINE static bool iterateVariantTypesOutline(variant_type&& variant, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((vector_t<element_type> || raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((string_t<element_type> || string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr (bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr ((number_t<element_type> || enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else if constexpr (always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, endIter, context);
				} else {
					return iterateVariantTypesOutline<type, variant_type, currentIndex + 1>(variant, iter, endIter, context);
				}
			} else {
				return cursor::template reject<parse_statuses::unexpected_token>(iter, context);
			}
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			if (!cursor::hasMoreInput(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			switch (static_cast<uint8_t>(*cursor::valuePtr(iter, context))) {
				case '{': {
					return iterateVariantTypesOutline<json_type::object>(value, iter, endIter, context);
				}
				case '[': {
					return iterateVariantTypesOutline<json_type::array>(value, iter, endIter, context);
				}
				case '"': {
					return iterateVariantTypesOutline<json_type::string>(value, iter, endIter, context);
				}
				case 't':
					[[fallthrough]];
				case 'f': {
					return iterateVariantTypesOutline<json_type::boolean>(value, iter, endIter, context);
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
					return iterateVariantTypesOutline<json_type::number>(value, iter, endIter, context);
				}
				case 'n': {
					return iterateVariantTypesOutline<json_type::null>(value, iter, endIter, context);
				}
				default: {
					return true;
				}
			}
		}
	};

	template<parse_options options, typename context_type> JSONIFIER_INLINE static bool isNullValue(auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
		using cursor = cursor_t<options, context_type>;
		skipLeadingWhitespace<options, context_type>(iter, endIter);
		return !cursor::notAtEnd(iter, endIter) || *cursor::valuePtr(iter, context) == 'n';
	}

	template<optional_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				return parse<options>::impl(value.emplace(), iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				return parse<options>::impl(value.emplace(), iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}
	};

	template<shared_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}
	};

	template<unique_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			value.reset();
			return cursor::iterateNull(iter, endIter, context);
		}
	};

	template<pointer_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			return cursor::iterateNull(iter, endIter, context);
		}

		JSONIFIER_NOINLINE static bool implOutline(value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			if (!isNullValue<options>(iter, endIter, context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, iter, endIter, context);
			}
			return cursor::iterateNull(iter, endIter, context);
		}
	};

	template<raw_json_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl([[maybe_unused]] value_type& __restrict value, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			skipLeadingWhitespace<options, context_type>(iter, endIter);
			if (!cursor::hasMoreInput(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			const read_buffer_ptr newPtr = cursor::valuePtr(iter, context);
			if (!cursor::skipValue(iter, endIter, context)) [[unlikely]] {
				return false;
			}
			const read_buffer_ptr endPtr = cursor::notAtEnd(iter, endIter) ? cursor::valuePtr(iter, context) : cursor::stringEnd(endIter, context);
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
			return true;
		}
	};

	template<skip_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using cursor = cursor_t<options, context_type>;

		JSONIFIER_INLINE static bool impl(value_type&, auto&& __restrict iter, auto endIter, context_type& __restrict context) noexcept {
			return cursor::skipValue(iter, endIter, context);
		}
	};

}
