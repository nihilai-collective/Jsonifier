/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/parse_impl.hpp
 */
#if !defined(JSONIFIER_PASS_GUARD_PARSE_IMPL)
	#define JSONIFIER_PASS_GUARD_PARSE_IMPL

	#include <jsonifier-incl/core/defines.hpp>

	#include <jsonifier-incl/utilities/number_utils.hpp>
	#include <jsonifier-incl/utilities/string_utils.hpp>
	#include <jsonifier-incl/parsing/parser.hpp>

namespace JSONIFIER_INTERNAL_NAMESPACE {

	#if JSONIFIER_COMPILER_MSVC
	enum class parse_result : uint8_t {
		active_member,
		inactive_member,
		ended,
		failed,
	};
	#else
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
	#endif

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

	#if JSONIFIER_COMPILER_MSVC
	template<parse_options options, typename value_type, typename context_type>
	JSONIFIER_INLINE static parse_result parseMemberValue(value_type&& value, auto&& __restrict iter, uint64_t depth, context_type& __restrict context) noexcept {
		const auto iterNew = parse<options>::impl(value, iter, depth, context);
		if (!iterNew) [[unlikely]] {
			return parse_result::failed;
		}
		iter = iterNew;
		return parse_result::active_member;
	}

	template<typename value_type, typename context_type, parse_options options> struct parse_types_impl {
		using cursor					  = cursor_t<options, context_type>;
		static constexpr auto memberCount = coreTupleSize<value_type>;

		template<uint64_t index> JSONIFIER_INLINE_EXCEPT_MAC_GCC static parse_result processIndex(value_type& __restrict value, auto&& __restrict iter, uint64_t depth,
			context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
						return parse_result::failed;
					}
					return parseMatchedMember<index>(value, iter, depth, context);
				}
			} else {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
							return parse_result::failed;
						}
						return parseMatchedMember<index>(value, iter, depth, context);
					}
				} else {
					static constexpr auto fusedKey	   = ::JSONIFIER_INTERNAL_NAMESPACE::makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMember<index>(value, iter, depth, context);
					}
				}
			}
			return parse_result::inactive_member;
		}

		template<uint64_t index>
		JSONIFIER_INLINE static parse_result parseMatchedMember(value_type& __restrict value, auto&& __restrict iter, uint64_t depth, context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					return cursor::skipValue(iter, context) ? parse_result::active_member : parse_result::failed;
				}
			}
			return ::JSONIFIER_INTERNAL_NAMESPACE::parseMemberValue<options>(getMember<ptrNew>(value), iter, depth, context);
		}

		template<uint64_t index> JSONIFIER_NOINLINE static parse_result processIndexOutline(value_type& __restrict value, auto&& __restrict iter, uint64_t depth,
			context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
						return parse_result::failed;
					}
					return parseMatchedMemberOutline<index>(value, iter, depth, context);
				}
			} else {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
							return parse_result::failed;
						}
						return parseMatchedMemberOutline<index>(value, iter, depth, context);
					}
				} else {
					static constexpr auto fusedKey	   = ::JSONIFIER_INTERNAL_NAMESPACE::makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMemberOutline<index>(value, iter, depth, context);
					}
				}
			}
			return parse_result::inactive_member;
		}

		template<uint64_t index> JSONIFIER_NOINLINE static parse_result parseMatchedMemberOutline(value_type& __restrict value, auto&& __restrict iter, uint64_t depth,
			context_type& __restrict context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					return cursor::skipValue(iter, context) ? parse_result::active_member : parse_result::failed;
				}
			}
			return ::JSONIFIER_INTERNAL_NAMESPACE::parseMemberValue<options>(getMember<ptrNew>(value), iter, depth, context);
		}
	};
	#else
	template<typename value_type, typename context_type, parse_options options> struct parse_types_impl {
		using iterator_type				  = typename context_type::iterator_type;
		using cursor					  = cursor_t<options, context_type>;
		using step_type					  = parse_step<iterator_type>;
		static constexpr auto memberCount = coreTupleSize<value_type>;

		template<uint64_t index>
		JSONIFIER_INLINE_EXCEPT_MAC_GCC static step_type processIndex(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
					return parseMatchedMember<index>(value, iter, depth, context);
				}
			} else {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
							return { iter, parse_result::failed };
						}
						return parseMatchedMember<index>(value, iter, depth, context);
					}
				} else {
					static constexpr auto fusedKey	   = ::JSONIFIER_INTERNAL_NAMESPACE::makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMember<index>(value, iter, depth, context);
					}
				}
			}
			return { iter, parse_result::inactive_member };
		}

		template<uint64_t index>
		JSONIFIER_INLINE_EXCEPT_MAC_GCC static step_type parseMatchedMember(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					const bool skipped = cursor::skipValue(iter, context);
					return { iter, skipped ? parse_result::active_member : parse_result::failed };
				}
			}
			const iterator_type iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, depth, context);
			return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
		}

		template<uint64_t index> JSONIFIER_NOINLINE static step_type processIndexOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			if constexpr (structural_context<context_type>) {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				const read_buffer_ptr keyStart		= cursor::valuePtr(iter, context);
				if ((keyStart + quotedKeySize) < cursor::stringEnd(context) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(keyStart)) [[likely]] {
					++iter;
					if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
					return parseMatchedMemberOutline<index>(value, iter, depth, context);
				}
			} else {
				static constexpr auto quotedKey		= ::JSONIFIER_INTERNAL_NAMESPACE::makeQuotedKeyLiteral(keyLiteral);
				static constexpr auto quotedKeySize = quotedKey.size();
				if constexpr (!options.minified) {
					if (((iter + quotedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(quotedKey), quotedKey>::impl(iter)) [[unlikely]] {
						iter += quotedKeySize;
						if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
							return { iter, parse_result::failed };
						}
						return parseMatchedMemberOutline<index>(value, iter, depth, context);
					}
				} else {
					static constexpr auto fusedKey	   = ::JSONIFIER_INTERNAL_NAMESPACE::makeFusedKeyLiteral(keyLiteral);
					static constexpr auto fusedKeySize = fusedKey.size();
					if (((iter + fusedKeySize) < context.endIter) && string_literal_comparator_impl<decltype(fusedKey), fusedKey>::impl(iter)) [[likely]] {
						iter += fusedKeySize;
						return parseMatchedMemberOutline<index>(value, iter, depth, context);
					}
				}
			}
			return { iter, parse_result::inactive_member };
		}

		template<uint64_t index>
		JSONIFIER_NOINLINE static step_type parseMatchedMemberOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			static constexpr auto tupleElem	 = getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue);
			static constexpr auto keyLiteral = escapedKeyLiteral<tupleElem.name>;
			static constexpr auto ptrNew	 = tupleElem.memberPtr;
			if constexpr (has_excluded_keys<value_type>) {
				static constexpr auto key = keyLiteral.operator jsonifier::string_view();
				auto& keys				  = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
					const bool skipped = cursor::skipValue(iter, context);
					return { iter, skipped ? parse_result::active_member : parse_result::failed };
				}
			}
			const iterator_type iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, depth, context);
			return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
		}
	};
	#endif

	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, typename integer_sequence>
	struct generateDispatchTableNew;

	#if JSONIFIER_COMPILER_MSVC
	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	struct generateDispatchTableNew<parsing_type, value_type, context_type, options, integer_sequence<indices...>> {
		JSONIFIER_INLINE static parse_result impl(value_type& __restrict value, auto&& __restrict iter, uint64_t depth, context_type& __restrict context) noexcept {
			parse_result result{ parse_result::inactive_member };
			static_cast<void>(
				((context.currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndex<indices>(value, iter, depth, context), true)
												  : false) ||
					...));
			return result;
		}

		JSONIFIER_NOINLINE static parse_result implOutline(value_type& __restrict value, auto&& __restrict iter, uint64_t depth, context_type& __restrict context) noexcept {
			parse_result result{ parse_result::inactive_member };
			static_cast<void>(((context.currentIndex == indices
									   ? (result = parsing_type<value_type, context_type, options>::template processIndexOutline<indices>(value, iter, depth, context), true)
									   : false) ||
				...));
			return result;
		}
	};
	#else
	template<template<typename, typename, parse_options> typename parsing_type, typename value_type, typename context_type, parse_options options, uint64_t... indices>
	struct generateDispatchTableNew<parsing_type, value_type, context_type, options, integer_sequence<indices...>> {
		using iterator_type = typename context_type::iterator_type;
		using step_type		= parse_step<iterator_type>;

		JSONIFIER_INLINE_EXCEPT_MAC_GCC static step_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context, uint64_t currentIndex) noexcept {
			step_type result{ iter, parse_result::inactive_member };
			static_cast<void>((
				(currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndex<indices>(value, iter, depth, context), true) : false) ||
				...));
			return result;
		}

		JSONIFIER_NOINLINE static step_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context, uint64_t currentIndex) noexcept {
			step_type result{ iter, parse_result::inactive_member };
			static_cast<void>(
				((currentIndex == indices ? (result = parsing_type<value_type, context_type, options>::template processIndexOutline<indices>(value, iter, depth, context), true)
										  : false) ||
					...));
			return result;
		}
	};
	#endif

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

	#if JSONIFIER_COMPILER_MSVC
		template<typename value_type, typename context_type> JSONIFIER_INLINE_EXCEPT_MAC_GCC static parse_result processIndex(value_type& __restrict value, auto&& __restrict iter,
			uint64_t depth, context_type& __restrict context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, depth, context)) {
					return parse_result::ended;
				}
				if (const auto result = tryKnownOrder(value, iter, depth, context); result != parse_result::inactive_member) {
					return result;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
						return parse_result::failed;
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							break;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							return parse_result::ended;
						}
						default: {
							return parse_result::failed;
						}
					}
				} else {
					if (cursor::objectMaybeEnd(iter, depth, context)) {
						return parse_result::ended;
					}
					if constexpr (!options.minified && !structural_context<context_type>) {
						cursor::skipWhitespaceScalar(iter, context);
					}
				}
				if constexpr (options.knownOrder) {
					if (const auto result = tryKnownOrder(value, iter, depth, context); result != parse_result::inactive_member) {
						return result;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (const auto result = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, depth, context);
						result != parse_result::inactive_member) [[likely]] {
						return result;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (const auto result = (context.currentIndex = indexNew, dispatch::impl(value, iter, depth, context)); result != parse_result::inactive_member) {
								return result;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
								indexNew2 < memberCount) [[likely]] {
								if (const auto result2 = (context.currentIndex = indexNew2, dispatch::impl(value, iter, depth, context));
									result2 != parse_result::inactive_member) {
									if (result2 == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return result2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
							indexNew2 < memberCount) [[likely]] {
							if (const auto result2 = (context.currentIndex = indexNew2, dispatch::impl(value, iter, depth, context)); result2 != parse_result::inactive_member) {
								return result2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return parse_result::failed;
				}
				if (!cursor::skipString(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						break;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return parse_result::ended;
					}
					default: {
						return parse_result::failed;
					}
				}
			}
		}

		template<typename value_type, typename context_type>
		JSONIFIER_INLINE static parse_result tryKnownOrder(value_type& __restrict value, auto&& __restrict iter, uint64_t depth, context_type& __restrict context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= ::JSONIFIER_INTERNAL_NAMESPACE::makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < context.endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							return cursor::skipValue(iter, context) ? parse_result::active_member : parse_result::failed;
						}
					}
					return ::JSONIFIER_INTERNAL_NAMESPACE::parseMemberValue<options>(getMember<ptrNew>(value), iter, depth, context);
				}
				return parse_result::inactive_member;
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, depth, context);
			}
		}

		template<typename value_type, typename context_type> JSONIFIER_NOINLINE static parse_result processIndexOutline(value_type& __restrict value, auto&& __restrict iter,
			uint64_t depth, context_type& __restrict context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, depth, context)) {
					return parse_result::ended;
				}
				if (const auto result = tryKnownOrderOutline(value, iter, depth, context); result != parse_result::inactive_member) {
					return result;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
						return parse_result::failed;
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							break;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							return parse_result::ended;
						}
						default: {
							return parse_result::failed;
						}
					}
				} else {
					if (cursor::objectMaybeEnd(iter, depth, context)) {
						return parse_result::ended;
					}
					if constexpr (!options.minified && !structural_context<context_type>) {
						cursor::skipWhitespaceScalar(iter, context);
					}
				}
				if constexpr (options.knownOrder) {
					if (const auto result = tryKnownOrder(value, iter, depth, context); result != parse_result::inactive_member) {
						return result;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (const auto result = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, depth, context);
						result != parse_result::inactive_member) [[likely]] {
						return result;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (const auto result = (context.currentIndex = indexNew, dispatch::impl(value, iter, depth, context)); result != parse_result::inactive_member) {
								return result;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
								indexNew2 < memberCount) [[likely]] {
								if (const auto result2 = (context.currentIndex = indexNew2, dispatch::impl(value, iter, depth, context));
									result2 != parse_result::inactive_member) {
									if (result2 == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return result2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
							indexNew2 < memberCount) [[likely]] {
							if (const auto result2 = (context.currentIndex = indexNew2, dispatch::impl(value, iter, depth, context)); result2 != parse_result::inactive_member) {
								return result2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return parse_result::failed;
				}
				if (!cursor::skipString(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return parse_result::failed;
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						break;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return parse_result::ended;
					}
					default: {
						return parse_result::failed;
					}
				}
			}
		}

		template<typename value_type, typename context_type> JSONIFIER_NOINLINE static parse_result tryKnownOrderOutline(value_type& __restrict value, auto&& __restrict iter,
			uint64_t depth, context_type& __restrict context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= ::JSONIFIER_INTERNAL_NAMESPACE::makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < context.endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							return cursor::skipValue(iter, context) ? parse_result::active_member : parse_result::failed;
						}
					}
					return ::JSONIFIER_INTERNAL_NAMESPACE::parseMemberValue<options>(getMember<ptrNew>(value), iter, depth, context);
				}
				return parse_result::inactive_member;
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, depth, context);
			}
		}
	};
	#else
		template<typename value_type, typename context_type> JSONIFIER_INLINE_EXCEPT_MAC_GCC static parse_step<typename context_type::iterator_type> processIndex(value_type& value,
			typename context_type::iterator_type iter, uint64_t depth, context_type& context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, depth, context)) {
					return { iter, parse_result::ended };
				}
				if (auto step = tryKnownOrder(value, iter, depth, context); step.result != parse_result::inactive_member) {
					return step;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
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
					if (cursor::objectMaybeEnd(iter, depth, context)) {
						return { iter, parse_result::ended };
					}
					if constexpr (!options.minified && !structural_context<context_type>) {
						cursor::skipWhitespaceScalar(iter, context);
					}
				}
				if constexpr (options.knownOrder) {
					if (auto step = tryKnownOrder(value, iter, depth, context); step.result != parse_result::inactive_member) {
						return step;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (auto step = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, depth, context);
						step.result != parse_result::inactive_member) [[likely]] {
						return step;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (auto step = dispatch::impl(value, iter, depth, context, indexNew); step.result != parse_result::inactive_member) {
								return step;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
								indexNew2 < memberCount) [[likely]] {
								if (auto step2 = dispatch::impl(value, iter, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
									if (step2.result == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return step2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
							indexNew2 < memberCount) [[likely]] {
							if (auto step2 = dispatch::impl(value, iter, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
								return step2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return { iter, parse_result::failed };
				}
				if (!cursor::skipString(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
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

		template<typename value_type, typename context_type> JSONIFIER_INLINE_EXCEPT_MAC_GCC static parse_step<typename context_type::iterator_type> tryKnownOrder(
			value_type& value, typename context_type::iterator_type iter, uint64_t depth, context_type& context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= ::JSONIFIER_INTERNAL_NAMESPACE::makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < context.endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							const bool skipped = cursor::skipValue(iter, context);
							return { iter, skipped ? parse_result::active_member : parse_result::failed };
						}
					}
					const auto iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, depth, context);
					return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
				}
				return { iter, parse_result::inactive_member };
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, depth, context);
			}
		}

		template<typename value_type, typename context_type> JSONIFIER_NOINLINE static parse_step<typename context_type::iterator_type> processIndexOutline(value_type& value,
			typename context_type::iterator_type iter, uint64_t depth, context_type& context) noexcept {
			using cursor   = cursor_t<options, context_type>;
			using dispatch = generateDispatchTableNew<parse_types_impl, value_type, context_type, options, make_integer_sequence<memberCount>>;
			if constexpr (options.minified && options.knownOrder && !structural_context<context_type>) {
				if (cursor::objectMaybeEnd(iter, depth, context)) {
					return { iter, parse_result::ended };
				}
				if (auto step = tryKnownOrderOutline(value, iter, depth, context); step.result != parse_result::inactive_member) {
					return step;
				}
				if constexpr (json_entity_type::index > 0) {
					if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
						return { iter, parse_result::failed };
					}
				}
			} else {
				if constexpr (json_entity_type::index > 0) {
					switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
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
					if (cursor::objectMaybeEnd(iter, depth, context)) {
						return { iter, parse_result::ended };
					}
					if constexpr (!options.minified && !structural_context<context_type>) {
						cursor::skipWhitespaceScalar(iter, context);
					}
				}
				if constexpr (options.knownOrder) {
					if (auto step = tryKnownOrder(value, iter, depth, context); step.result != parse_result::inactive_member) {
						return step;
					}
				}
			}
			while (true) {
				if constexpr (memberCount == 1) {
					if (auto step = parse_types_impl<value_type, context_type, options>::template processIndex<0>(value, iter, depth, context);
						step.result != parse_result::inactive_member) [[likely]] {
						return step;
					}
				} else {
					if constexpr (options.knownOrder) {
						if (const uint64_t indexNew = antiHashStatesNew<memberCount, value_type>[json_entity_type::index]; indexNew < memberCount) [[likely]] {
							if (auto step = dispatch::impl(value, iter, depth, context, indexNew); step.result != parse_result::inactive_member) {
								return step;
							}
							if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
								indexNew2 < memberCount) [[likely]] {
								if (auto step2 = dispatch::impl(value, iter, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
									if (step2.result == parse_result::active_member) {
										antiHashStatesNew<memberCount, value_type>[json_entity_type::index] = indexNew2;
									}
									return step2;
								}
							}
						}
					} else {
						if (auto indexNew2 = hash_map<value_type, read_buffer_ptr>::findIndex(cursor::valuePtr(iter, context) + 1, cursor::stringEnd(context));
							indexNew2 < memberCount) [[likely]] {
							if (auto step2 = dispatch::impl(value, iter, depth, context, indexNew2); step2.result != parse_result::inactive_member) {
								return step2;
							}
						}
					}
				}
				if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::missing_key_start>(iter, context));
					return { iter, parse_result::failed };
				}
				if (!cursor::skipString(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return { iter, parse_result::failed };
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, depth, context))) {
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

		template<typename value_type, typename context_type> JSONIFIER_NOINLINE static parse_step<typename context_type::iterator_type> tryKnownOrderOutline(value_type& value,
			typename context_type::iterator_type iter, uint64_t depth, context_type& context) noexcept {
			using cursor					 = cursor_t<options, context_type>;
			static constexpr auto keyLiteral = escapedKeyLiteral<json_entity_type::name>;
			static constexpr auto ptrNew	 = json_entity_type::memberPtr;
			if constexpr (options.minified && !structural_context<context_type>) {
				static constexpr auto memberLiteral		= ::JSONIFIER_INTERNAL_NAMESPACE::makeMemberLiteralNew<json_entity_type::index>(keyLiteral);
				static constexpr auto memberLiteralSize = memberLiteral.size();
				if (((iter + memberLiteralSize) < context.endIter) && string_literal_comparator_impl<decltype(memberLiteral), memberLiteral>::impl(iter)) [[likely]] {
					iter += memberLiteralSize;
					if constexpr (has_excluded_keys<value_type>) {
						static constexpr auto key = keyLiteral.operator jsonifier::string_view();
						const auto& keys		  = value.jsonifierExcludedKeys;
						if (keys.find(static_cast<typename remove_cvref_t<decltype(keys)>::key_type>(key)) != keys.end()) [[unlikely]] {
							const bool skipped = cursor::skipValue(iter, context);
							return { iter, skipped ? parse_result::active_member : parse_result::failed };
						}
					}
					const auto iterNew = parse<options>::impl(getMember<ptrNew>(value), iter, depth, context);
					return { iterNew, iterNew ? parse_result::active_member : parse_result::failed };
				}
				return { iter, parse_result::inactive_member };
			} else {
				return parse_types_impl<value_type, context_type, options>::template processIndex<json_entity_type::index>(value, iter, depth, context);
			}
		}
	};
	#endif

	#if JSONIFIER_COMPILER_MSVC
	template<typename... bases> struct parse_map : public bases... {
		template<typename value_type, typename context_type> JSONIFIER_INLINE static parse_result iterateValues([[maybe_unused]] value_type& __restrict value,
			[[maybe_unused]] auto&& __restrict iter, [[maybe_unused]] uint64_t depth, [[maybe_unused]] context_type& __restrict context) noexcept {
			parse_result result{ parse_result::active_member };
			static_cast<void>(((result = bases::processIndex(value, iter, depth, context), result == parse_result::active_member) && ...));
			return result;
		}

		template<typename value_type, typename context_type> JSONIFIER_NOINLINE static parse_result iterateValuesOutline([[maybe_unused]] value_type& __restrict value,
			[[maybe_unused]] auto&& __restrict iter, [[maybe_unused]] uint64_t depth, [[maybe_unused]] context_type& __restrict context) noexcept {
			parse_result result{ parse_result::active_member };
			static_cast<void>(((result = bases::processIndexOutline(value, iter, depth, context), result == parse_result::active_member) && ...));
			return result;
		}
	};
	#else
	template<typename... bases> struct parse_map : public bases... {
		template<typename value_type, typename iterator_type, typename context_type> JSONIFIER_INLINE_EXCEPT_MAC_GCC static parse_step<iterator_type> iterateValues(
			[[maybe_unused]] value_type& value, iterator_type iter, [[maybe_unused]] uint64_t depth, [[maybe_unused]] context_type& context) noexcept {
			parse_step<iterator_type> step{ iter, parse_result::active_member };
			static_cast<void>(((step = bases::processIndex(value, step.iter, depth, context), step.result == parse_result::active_member) && ...));
			return step;
		}

		template<typename value_type, typename iterator_type, typename context_type> JSONIFIER_NOINLINE static parse_step<iterator_type> iterateValuesOutline(
			[[maybe_unused]] value_type& value, iterator_type iter, [[maybe_unused]] uint64_t depth, [[maybe_unused]] context_type& context) noexcept {
			parse_step<iterator_type> step{ iter, parse_result::active_member };
			static_cast<void>(((step = bases::processIndexOutline(value, step.iter, depth, context), step.result == parse_result::active_member) && ...));
			return step;
		}
	};
	#endif

	template<parse_options options, typename value_type, typename context_type, typename integer_sequence> struct get_parse_base;

	template<parse_options options, typename value_type, typename context_type, uint64_t... index>
	struct get_parse_base<options, value_type, context_type, integer_sequence<index...>> {
		using type = parse_map<json_entity_parse<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<parse_options options, typename value_type, typename context_type> using parse_base_t =
		typename get_parse_base<options, value_type, context_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	#if JSONIFIER_COMPILER_MSVC
	template<jsonifier_object_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE_EXCEPT_MAC_GCC static iterator_type impl(value_type& __restrict value, iterator_type iter, uint64_t depth, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			const auto result = parse_base_t<options, value_type, context_type>::iterateValues(value, iter, innerDepth, context);
			if (result == parse_result::ended) {
				return iter;
			}
			if (result != parse_result::active_member) [[unlikely]] {
				return nullptr;
			}
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) {
				return iter;
			}
			if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			return cursor::skipRemainingObject(iter, innerDepth, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& __restrict value, iterator_type iter, uint64_t depth, context_type& __restrict context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			const auto result = parse_base_t<options, value_type, context_type>::iterateValuesOutline(value, iter, innerDepth, context);
			if (result == parse_result::ended) {
				return iter;
			}
			if (result != parse_result::active_member) [[unlikely]] {
				return nullptr;
			}
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) {
				return iter;
			}
			if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			return cursor::skipRemainingObject(iter, innerDepth, context) ? iter : nullptr;
		}
	};
	#else
	template<jsonifier_object_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE_EXCEPT_MAC_GCC static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			const auto step = parse_base_t<options, value_type, context_type>::iterateValues(value, iter, innerDepth, context);
			if (step.result == parse_result::ended) {
				return step.iter;
			}
			if (step.result != parse_result::active_member) [[unlikely]] {
				return nullptr;
			}
			iter = step.iter;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) {
				return iter;
			}
			if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			return cursor::skipRemainingObject(iter, innerDepth, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			const auto step = parse_base_t<options, value_type, context_type>::iterateValuesOutline(value, iter, innerDepth, context);
			if (step.result == parse_result::ended) {
				return step.iter;
			}
			if (step.result != parse_result::active_member) [[unlikely]] {
				return nullptr;
			}
			iter = step.iter;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) {
				return iter;
			}
			if (!cursor::collectObjectComma(iter, context)) [[unlikely]] {
				return nullptr;
			}
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			return cursor::skipRemainingObject(iter, innerDepth, context) ? iter : nullptr;
		}
	};
	#endif

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

		JSONIFIER_INLINE_EXCEPT_MAC_GCC static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			while (true) {
				iter = parse<options>::impl(::JSONIFIER_INTERNAL_NAMESPACE::getKeyNew<typename value_type::key_type>(), iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return nullptr;
				}
				iter = parse<options>::impl(value[::JSONIFIER_INTERNAL_NAMESPACE::getKeyNew<typename value_type::key_type>()], iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, innerDepth, context))) {
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

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::objectStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::objectMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			while (true) {
				iter = parse<options>::impl(::JSONIFIER_INTERNAL_NAMESPACE::getKeyNew<typename value_type::key_type>(), iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::collectObjectColon(iter, context)) [[unlikely]] {
					return nullptr;
				}
				iter = parse<options>::impl(value[::JSONIFIER_INTERNAL_NAMESPACE::getKeyNew<typename value_type::key_type>()], iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				switch (static_cast<uint64_t>(cursor::collectObjectSeparator(iter, innerDepth, context))) {
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

	template<typename value_type> static thread_local uint64_t elementCountHint{};

	template<typename value_type>
	static constexpr uint64_t maxHintedElements{ 4096 / sizeof(typename value_type::value_type) > 0 ? 4096 / sizeof(typename value_type::value_type) : 1 };

	template<typename value_type> JSONIFIER_INLINE static void reserveFromHint(value_type& value) noexcept {
		// The hint is shared by every array of this type and only grows, so for
		// raw_json_data it would make each small nested array reserve the
		// largest size seen anywhere in the document.
		if constexpr (has_reserve<value_type> && !raw_json_t<typename value_type::value_type>) {
			const uint64_t target = elementCountHint<value_type> < maxHintedElements<value_type> ? elementCountHint<value_type> : maxHintedElements<value_type>;
			if constexpr (requires { value.capacity(); }) {
				if (value.capacity() >= target) {
					return;
				}
			}
			value.reserve(target);
		}
	}

	template<typename value_type> JSONIFIER_INLINE static void finishInPlace(value_type& value, uint64_t oldSize, uint64_t newSize) noexcept {
		if (newSize < oldSize) {
			value.resize(newSize);
		}
		if (newSize > elementCountHint<value_type>) {
			elementCountHint<value_type> = newSize;
		}
	}

	#if JSONIFIER_COMPILER_CLANG
		#pragma clang diagnostic push
		#pragma clang diagnostic ignored "-Wexit-time-destructors"
		#pragma clang diagnostic ignored "-Wglobal-constructors"
	#endif
	template<typename value_type> static thread_local value_type valueTemp;
	#if JSONIFIER_COMPILER_CLANG
		#pragma clang diagnostic pop
	#endif

	template<typename element_type> inline constexpr bool fullyOverwritten{ std::is_arithmetic_v<element_type> };

	template<vector_t element_type> inline constexpr bool fullyOverwritten<element_type>{ fullyOverwritten<typename element_type::value_type> };

	#if JSONIFIER_BACKEND_PASS == 0
	template<typename value_type, typename iterator_type> JSONIFIER_INLINE static void moveAssignVec(value_type& value, iterator_type first, iterator_type last) {
		if constexpr (std::is_trivially_copyable_v<typename value_type::value_type>) {
			value.assign(first, last);
		} else {
			value.assign(std::make_move_iterator(first), std::make_move_iterator(last));
		}
	}
	#endif

	template<vector_t value_type, typename context_type, parse_options optionsNew> struct parse_impl<value_type, context_type, optionsNew> {
		static constexpr parse_options options{ optionsNew };
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE_EXCEPT_MAC_GCC static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				value.clear();
				return iter;
			}
			if constexpr (fullyOverwritten<typename value_type::value_type>) {
				uint64_t oldSize{ valueTemp<value_type>.size() };
				uint64_t newSize{};
				if (oldSize > 0) {
					auto beginIter = ::JSONIFIER_INTERNAL_NAMESPACE::getBeginIterVec(valueTemp<value_type>);
					for (uint64_t x = 0; x < oldSize; ++x) {
						iter = parse<options>::impl(beginIter[static_cast<int64_t>(x)], iter, innerDepth, context);
						if (!iter) [[unlikely]] {
							return nullptr;
						}
						++newSize;
						switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
							case static_cast<uint64_t>(sep_result::cont): {
								continue;
							}
							case static_cast<uint64_t>(sep_result::ended): {
								::JSONIFIER_INTERNAL_NAMESPACE::moveAssignVec(value, beginIter, beginIter + static_cast<int64_t>(newSize));
								return iter;
							}
							default: {
								return nullptr;
							}
						}
					}
				}
				while (cursor::notAtEnd(iter, context)) {
					iter = parse<options>::impl(valueTemp<value_type>.emplace_back(), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					++newSize;
					switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							::JSONIFIER_INTERNAL_NAMESPACE::moveAssignVec(value, ::JSONIFIER_INTERNAL_NAMESPACE::getBeginIterVec(valueTemp<value_type>),
								::JSONIFIER_INTERNAL_NAMESPACE::getEndIterVec(valueTemp<value_type>));
							return iter;
						}
						default: {
							return nullptr;
						}
					}
				}
			} else {
				const uint64_t oldSize{ value.size() };
				::JSONIFIER_INTERNAL_NAMESPACE::reserveFromHint(value);
				uint64_t newSize{};
				while (cursor::notAtEnd(iter, context)) {
					iter = parse<options>::impl(newSize < oldSize ? value[newSize] : value.emplace_back(), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					++newSize;
					switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							finishInPlace(value, oldSize, newSize);
							return iter;
						}
						default: {
							return nullptr;
						}
					}
				}
			}
			static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
			return nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				value.clear();
				return iter;
			}
			if constexpr (fullyOverwritten<typename value_type::value_type>) {
				uint64_t oldSize{ valueTemp<value_type>.size() };
				uint64_t newSize{};
				if (oldSize > 0) {
					auto beginIter = ::JSONIFIER_INTERNAL_NAMESPACE::getBeginIterVec(valueTemp<value_type>);
					for (uint64_t x = 0; x < oldSize; ++x) {
						iter = parse<options>::impl(beginIter[static_cast<int64_t>(x)], iter, innerDepth, context);
						if (!iter) [[unlikely]] {
							return nullptr;
						}
						++newSize;
						switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
							case static_cast<uint64_t>(sep_result::cont): {
								continue;
							}
							case static_cast<uint64_t>(sep_result::ended): {
								::JSONIFIER_INTERNAL_NAMESPACE::moveAssignVec(value, beginIter, beginIter + static_cast<int64_t>(newSize));
								return iter;
							}
							default: {
								return nullptr;
							}
						}
					}
				}
				while (cursor::notAtEnd(iter, context)) {
					iter = parse<options>::impl(valueTemp<value_type>.emplace_back(), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					++newSize;
					switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							::JSONIFIER_INTERNAL_NAMESPACE::moveAssignVec(value, ::JSONIFIER_INTERNAL_NAMESPACE::getBeginIterVec(valueTemp<value_type>),
								::JSONIFIER_INTERNAL_NAMESPACE::getEndIterVec(valueTemp<value_type>));
							return iter;
						}
						default: {
							return nullptr;
						}
					}
				}
			} else {
				const uint64_t oldSize{ value.size() };
				::JSONIFIER_INTERNAL_NAMESPACE::reserveFromHint(value);
				uint64_t newSize{};
				while (cursor::notAtEnd(iter, context)) {
					iter = parse<options>::impl(newSize < oldSize ? value[newSize] : value.emplace_back(), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					++newSize;
					switch (static_cast<uint64_t>(cursor::collectArraySeparator(iter, innerDepth, context))) {
						case static_cast<uint64_t>(sep_result::cont): {
							continue;
						}
						case static_cast<uint64_t>(sep_result::ended): {
							finishInPlace(value, oldSize, newSize);
							return iter;
						}
						default: {
							return nullptr;
						}
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

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
				auto iterNew = std::begin(value);
				for (uint64_t i = 0; i < nLocal; ++i) {
					iter = parse<options>::impl(*(iterNew++), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
						return iter;
					}
					if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (cursor::notAtEnd(iter, context)) {
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return nullptr;
				}
				if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
					return iter;
				}
				if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
					return nullptr;
				}
			}
			static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
			return nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if (const uint64_t nLocal = std::size(value); nLocal > 0) [[likely]] {
				auto iterNew = std::begin(value);
				for (uint64_t i = 0; i < nLocal; ++i) {
					iter = parse<options>::impl(*(iterNew++), iter, innerDepth, context);
					if (!iter) [[unlikely]] {
						return nullptr;
					}
					if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
						return iter;
					}
					if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (cursor::notAtEnd(iter, context)) {
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return nullptr;
				}
				if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
					return iter;
				}
				if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
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

		template<uint64_t index> JSONIFIER_INLINE static bool parseMember(value_type& value, iterator_type& iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::template incrementIfEquals<','>(iter, context)) [[unlikely]] {
				return false;
			}
			iter = parse<options>::impl(get<index>(value), iter, depth, context);
			return iter != nullptr;
		}

		template<uint64_t... indices>
		JSONIFIER_INLINE static iterator_type parseRest(value_type& value, iterator_type iter, uint64_t depth, context_type& context, integer_sequence<indices...>) noexcept {
			static_cast<void>((parseMember<indices + 1>(value, iter, depth, context) && ...));
			return iter;
		}

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if constexpr (memberCount > 0) {
				iter = parse<options>::impl(get<0>(value), iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if constexpr (memberCount > 1) {
					iter = parseRest(value, iter, innerDepth, context, make_integer_sequence<memberCount - 1>{});
					if (!iter) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (!cursor::arrayMaybeEnd(iter, innerDepth, context)) {
				if (!cursor::notAtEnd(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
					return nullptr;
				}
				if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return nullptr;
				}
			}
			return iter;
		}

		template<uint64_t index> JSONIFIER_NOINLINE static bool parseMemberOutline(value_type& value, iterator_type& iter, uint64_t depth, context_type& context) noexcept {
			return parseMember<index>(value, iter, depth, context);
		}

		template<uint64_t... indices> JSONIFIER_NOINLINE static iterator_type parseRestOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context,
			integer_sequence<indices...>) noexcept {
			static_cast<void>((parseMemberOutline<indices + 1>(value, iter, depth, context) && ...));
			return iter;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!cursor::arrayStart(iter, depth, context)) [[unlikely]] {
				return nullptr;
			}
			const uint64_t innerDepth = depth + 1;
			if (cursor::arrayMaybeEnd(iter, innerDepth, context)) [[unlikely]] {
				return iter;
			}
			if constexpr (memberCount > 0) {
				iter = parse<options>::impl(get<0>(value), iter, innerDepth, context);
				if (!iter) [[unlikely]] {
					return nullptr;
				}
				if constexpr (memberCount > 1) {
					iter = parseRestOutline(value, iter, innerDepth, context, make_integer_sequence<memberCount - 1>{});
					if (!iter) [[unlikely]] {
						return nullptr;
					}
				}
			}
			while (!cursor::arrayMaybeEnd(iter, innerDepth, context)) {
				if (!cursor::notAtEnd(iter, context)) [[unlikely]] {
					static_cast<void>(cursor::template reject<parse_statuses::unexpected_string_end>(iter, context));
					return nullptr;
				}
				if (!cursor::collectArrayComma(iter, context)) [[unlikely]] {
					return nullptr;
				}
				if (!cursor::skipValue(iter, context)) [[unlikely]] {
					return nullptr;
				}
			}
			return iter;
		}
	};

	template<string_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			if (!cursor::template checkChar<'"'>(iter, context)) [[unlikely]] {
				static_cast<void>(cursor::template reject<parse_statuses::invalid_string_characters>(iter, context));
				return nullptr;
			}
			return cursor::iterateString(value, iter, context) ? iter : nullptr;
		}
	};

	template<char_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t, context_type& context) noexcept {
			if (!cursor::hasMoreInput(iter, context)) [[unlikely]] {
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

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t, context_type& context) noexcept {
			uint64_t newValue{};
			if (!cursor::iterateNumber(newValue, iter, context)) [[unlikely]] {
				return nullptr;
			}
			value = static_cast<value_type>(newValue);
			return iter;
		}
	};

	template<number_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t, context_type& context) noexcept {
			return cursor::iterateNumber(value, iter, context) ? iter : nullptr;
		}
	};

	template<bool_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t, context_type& context) noexcept {
			return cursor::iterateBool(value, iter, context) ? iter : nullptr;
		}
	};

	template<always_null_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type&, iterator_type iter, uint64_t, context_type& context) noexcept {
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}
	};

	template<variant_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		template<json_type type, typename variant_type, uint64_t currentIndex = 0>
		JSONIFIER_INLINE static iterator_type iterateVariantTypes(variant_type&& variant, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((vector_t<element_type> || raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((string_t<element_type> || string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr (bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((number_t<element_type> || enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr (always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else {
					return iterateVariantTypes<type, variant_type, currentIndex + 1>(variant, iter, depth, context);
				}
			} else {
				static_cast<void>(cursor::template reject<parse_statuses::unexpected_token>(iter, context));
				return nullptr;
			}
		}

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			if (!cursor::hasMoreInput(iter, context)) [[unlikely]] {
				return nullptr;
			}
			switch (static_cast<uint8_t>(*cursor::valuePtr(iter, context))) {
				case '{': {
					return iterateVariantTypes<json_type::object>(value, iter, depth, context);
				}
				case '[': {
					return iterateVariantTypes<json_type::array>(value, iter, depth, context);
				}
				case '"': {
					return iterateVariantTypes<json_type::string>(value, iter, depth, context);
				}
				case 't':
					[[fallthrough]];
				case 'f': {
					return iterateVariantTypes<json_type::boolean>(value, iter, depth, context);
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
					return iterateVariantTypes<json_type::number>(value, iter, depth, context);
				}
				case 'n': {
					return iterateVariantTypes<json_type::null>(value, iter, depth, context);
				}
				default: {
					return iter;
				}
			}
		}

		template<json_type type, typename variant_type, uint64_t currentIndex = 0>
		JSONIFIER_NOINLINE static iterator_type iterateVariantTypesOutline(variant_type&& variant, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if constexpr (currentIndex < std::variant_size_v<remove_cvref_t<variant_type>>) {
				using element_type = remove_cvref_t<decltype(std::get<currentIndex>(std::declval<remove_cvref_t<variant_type>>()))>;
				if constexpr (jsonifier_object_t<element_type> && type == json_type::object) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((vector_t<element_type> || raw_array_t<element_type>) && type == json_type::array) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((string_t<element_type> || string_view_t<element_type>) && type == json_type::string) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr (bool_t<element_type> && type == json_type::boolean) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr ((number_t<element_type> || enum_t<element_type>) && type == json_type::number) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else if constexpr (always_null_t<element_type> && type == json_type::null) {
					return parse<options>::impl(variant.template emplace<element_type>(element_type{}), iter, depth, context);
				} else {
					return iterateVariantTypesOutline<type, variant_type, currentIndex + 1>(variant, iter, depth, context);
				}
			} else {
				static_cast<void>(cursor::template reject<parse_statuses::unexpected_token>(iter, context));
				return nullptr;
			}
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			if (!cursor::hasMoreInput(iter, context)) [[unlikely]] {
				return nullptr;
			}
			switch (static_cast<uint8_t>(*cursor::valuePtr(iter, context))) {
				case '{': {
					return iterateVariantTypesOutline<json_type::object>(value, iter, depth, context);
				}
				case '[': {
					return iterateVariantTypesOutline<json_type::array>(value, iter, depth, context);
				}
				case '"': {
					return iterateVariantTypesOutline<json_type::string>(value, iter, depth, context);
				}
				case 't':
					[[fallthrough]];
				case 'f': {
					return iterateVariantTypesOutline<json_type::boolean>(value, iter, depth, context);
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
					return iterateVariantTypesOutline<json_type::number>(value, iter, depth, context);
				}
				case 'n': {
					return iterateVariantTypesOutline<json_type::null>(value, iter, depth, context);
				}
				default: {
					return iter;
				}
			}
		}
	};

	template<parse_options options, typename iterator_type, typename context_type> JSONIFIER_INLINE static bool isNullValue(iterator_type& iter, context_type& context) noexcept {
		using cursor = cursor_t<options, context_type>;
		if constexpr (!options.minified && !structural_context<context_type>) {
			cursor::skipWhitespaceScalar(iter, context);
		}
		return !cursor::notAtEnd(iter, context) || *cursor::valuePtr(iter, context) == 'n';
	}

	template<optional_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				return parse<options>::impl(value.emplace(), iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				return parse<options>::impl(value.emplace(), iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}
	};

	template<shared_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_shared<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}
	};

	template<unique_ptr_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				using member_type = decltype(*value);
				if (!value) {
					value = std::make_unique<jsonifier::internal::remove_pointer_t<remove_cvref_t<member_type>>>();
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			value.reset();
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}
	};

	template<pointer_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}

		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if (!::JSONIFIER_INTERNAL_NAMESPACE::isNullValue<options>(iter, context)) [[likely]] {
				if (!value) [[unlikely]] {
					value = new jsonifier::internal::remove_pointer_t<value_type>{};
				}
				return parse<options>::impl(*value, iter, depth, context);
			}
			return cursor::iterateNull(iter, context) ? iter : nullptr;
		}
	};

	template<raw_json_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		// RFC 8259 number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
		JSONIFIER_INLINE static bool isJsonNumber(string_view token) noexcept {
			auto iter	   = token.data();
			const auto end = iter + token.size();
			auto digits	   = [&]() {
				const auto start = iter;
				while (iter < end && is_digit(static_cast<uint8_t>(*iter))) {
					++iter;
				}
				return iter != start;
			};
			if (iter < end && *iter == '-') {
				++iter;
			}
			if (iter < end && *iter == '0') {
				++iter;
			} else if (!digits()) {
				return false;
			}
			if (iter < end && *iter == '.') {
				++iter;
				if (!digits()) {
					return false;
				}
			}
			if (iter < end && (*iter == 'e' || *iter == 'E')) {
				++iter;
				if (iter < end && (*iter == '+' || *iter == '-')) {
					++iter;
				}
				if (!digits()) {
					return false;
				}
			}
			return iter == end;
		}

		// Decodes the value in place, in a single pass: containers and strings go
		// straight to the regular parsers, scalars are read from their token.
		JSONIFIER_INLINE static iterator_type impl(value_type& value, iterator_type iter, uint64_t depth, context_type& context) noexcept {
			if constexpr (!options.minified && !structural_context<context_type>) {
				cursor::skipWhitespaceScalar(iter, context);
			}
			if (!cursor::hasMoreInput(iter, context)) [[unlikely]] {
				return nullptr;
			}
			const read_buffer_ptr newPtr = cursor::valuePtr(iter, context);
			switch (*newPtr) {
				case '{':
					return parse<options>::impl(value.value.template emplace<typename value_type::object_type>(), iter, depth, context);
				case '[':
					return parse<options>::impl(value.value.template emplace<typename value_type::array_type>(), iter, depth, context);
				case '"':
					return parse<options>::impl(value.value.template emplace<typename value_type::string_type>(), iter, depth, context);
				default:
					break;
			}
			const iterator_type tokenIter = iter;
			if (!cursor::skipValue(iter, context)) [[unlikely]] {
				return nullptr;
			}
			const read_buffer_ptr endPtr = cursor::notAtEnd(iter, context) ? cursor::valuePtr(iter, context) : cursor::stringEnd(context);
			uint64_t newSize			 = static_cast<uint64_t>(endPtr - newPtr);
			if constexpr (!options.minified) {
				while (newSize > 0 && whitespaceTable[static_cast<uint8_t>(newPtr[newSize - 1])]) {
					--newSize;
				}
			}
			const string_view token{ std::bit_cast<const char*>(newPtr), newSize };
			if (token == "true") {
				value.value.template emplace<typename value_type::bool_type>(true);
			} else if (token == "false") {
				value.value.template emplace<typename value_type::bool_type>(false);
			} else if (token == "null") {
				value.value.template emplace<typename value_type::null_type>();
			} else if (isJsonNumber(token)) {
				value.value.template emplace<typename value_type::number_type>(token);
			} else {
				return cursor::template reject<parse_statuses::unexpected_token>(tokenIter, context) ? iter : nullptr;
			}
			return iter;
		}
	};

	template<skip_t value_type, typename context_type, parse_options options> struct parse_impl<value_type, context_type, options> {
		using iterator_type = typename context_type::iterator_type;
		using cursor		= cursor_t<options, context_type>;

		JSONIFIER_INLINE static iterator_type impl(value_type&, iterator_type iter, uint64_t, context_type& context) noexcept {
			return cursor::skipValue(iter, context) ? iter : nullptr;
		}
	};

}

#endif
