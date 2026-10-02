/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/serialize_impl.hpp
 */
#pragma once

#include <jsonifier-incl/serializing/serializer.hpp>
#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/json_entity.hpp>

namespace jsonifier::internal {

	template<typename value_type> consteval uint64_t getValueSize(value_type value) {
		if constexpr (integral_t<value_type>) {
			return sizeof(value_type);
		} else {
			return value.size();
		}
	}

	template<string_literal string> struct char_blitter {
		static constexpr uint64_t lengthToAdvance{ string.size() };
		static constexpr uint64_t lengthToCopy{ getValueSize(pack_values<string>::value) };
		static constexpr auto value{ pack_values<string>::value };
	};

	template<serialize_options options, string_literal key> static consteval uint64_t objectEntrySize() noexcept {
		if constexpr (options.prettify) {
			return key.size() + 4;
		} else {
			return key.size() + 3;
		}
	}

	template<serialize_options options, typename json_entity_type> struct json_entity_size : public json_entity_type {
		template<typename value_type, typename context_type> inline static void processIndex(value_type& value, context_type& context) {
			if constexpr (has_excluded_keys<value_type>) {
				auto& keys = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename jsonifier::internal::remove_reference_t<decltype(keys)>::key_type>(json_entity_type::name)) != keys.end()) [[unlikely]] {
					return;
				}
			}
			context.requiredSize += objectEntrySize<options, json_entity_type::name>();
			using v_type = remove_cv_t<decltype(getMember<json_entity_type::memberPtr>(value))>;
			if constexpr (has_static_size<get_size_impl<v_type, options>>) {
				context.requiredSize += get_size_impl<v_type, options>::staticSize;
			} else {
				get_size<options>::impl(getMember<json_entity_type::memberPtr>(value), context);
			}
			if constexpr (!json_entity_type::isItLast) {
				if constexpr (options.prettify) {
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}
			}
		}

		inline constexpr json_entity_size() noexcept = default;
	};

	template<typename... bases> struct size_getter_map : public bases... {
		template<typename... arg_types> inline static constexpr void iterateValues([[maybe_unused]] arg_types&&... args) {
			((bases::processIndex(internal::forward<arg_types>(args)...)), ...);
		}
	};

	template<serialize_options options, typename value_type, typename integer_sequence> struct get_size_getter_base;

	template<serialize_options options, typename value_type, uint64_t... index> struct get_size_getter_base<options, value_type, integer_sequence<index...>> {
		using type = size_getter_map<json_entity_size<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<serialize_options options, typename value_type> using size_getter_base_t =
		typename get_size_getter_base<options, value_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			static constexpr auto memberCount{ coreTupleSize<value_type> };

			if constexpr (memberCount > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}

				size_getter_base_t<options, value_type>::iterateValues(value, context);

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					context.requiredSize += 1 + context.indent;
				}
				++context.requiredSize;
			} else {
				context.requiredSize += 2;
			}
		}
	};

	template<map_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			using key_type	   = remove_cvref_t<typename remove_cvref_t<value_type_new>::key_type>;
			using mapped_type  = remove_cvref_t<typename remove_cvref_t<value_type_new>::mapped_type>;
			const auto newSize = value.size();
			if (newSize > 0) [[likely]] {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}

				context.requiredSize += options.prettify ? (newSize - 1) * (2 + context.indent) : (newSize - 1);
				context.requiredSize += newSize * (options.prettify ? 2 : 1);

				if constexpr (has_static_size<get_size_impl<key_type, options>> && has_static_size<get_size_impl<mapped_type, options>>) {
					context.requiredSize += newSize * (get_size_impl<key_type, options>::staticSize + get_size_impl<mapped_type, options>::staticSize);
				} else if constexpr (has_static_size<get_size_impl<key_type, options>>) {
					auto iter = value.begin();
					if constexpr (!string_t<key_type>) {
						context.requiredSize += newSize * (get_size_impl<key_type, options>::staticSize + 2);
					} else {
						context.requiredSize += newSize * get_size_impl<key_type, options>::staticSize;
					}
					const auto end = value.end();
					for (; iter != end; ++iter) {
						get_size<options>::impl(iter->second, context);
					}
				} else if constexpr (has_static_size<get_size_impl<mapped_type, options>>) {
					context.requiredSize += newSize * get_size_impl<mapped_type, options>::staticSize;
					auto iter = value.begin();
					if constexpr (!string_t<key_type>) {
						context.requiredSize += newSize * 2;
					}
					const auto end = value.end();
					for (; iter != end; ++iter) {
						get_size<options>::impl(iter->first, context);
					}
				} else {
					auto iter = value.begin();
					if constexpr (!string_t<key_type>) {
						context.requiredSize += newSize * 2;
					}
					const auto end = value.end();
					for (; iter != end; ++iter) {
						get_size<options>::impl(iter->first, context);
						get_size<options>::impl(iter->second, context);
					}
				}

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					context.requiredSize += 1 + context.indent;
				}
				++context.requiredSize;
			} else {
				context.requiredSize += 2;
			}
		}
	};

	template<vector_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			using elem_type	   = remove_cvref_t<typename remove_cvref_t<value_type_new>::value_type>;
			const auto newSize = value.size();
			if (newSize > 0) [[likely]] {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}

				if constexpr (options.prettify) {
					context.requiredSize += (newSize - 1) * (2 + context.indent);
				} else {
					context.requiredSize += newSize - 1;
				}

				if constexpr (has_static_size<get_size_impl<elem_type, options>>) {
					context.requiredSize += newSize * get_size_impl<elem_type, options>::staticSize;
				} else {
					auto iter = getBeginIterVec(value);
					for (uint64_t index{}; index != newSize; ++index) {
						get_size<options>::impl(iter[static_cast<int64_t>(index)], context);
					}
				}

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					context.requiredSize += 1 + context.indent;
				}
				++context.requiredSize;
			} else {
				context.requiredSize += 2;
			}
		}
	};

	template<raw_array_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<template<typename, auto> typename value_type_new, typename value_type_internal, auto size>
		JSONIFIER_INLINE static void impl(const value_type_new<value_type_internal, size>& value, size_context& context) noexcept {
			using elem_type				  = remove_cvref_t<value_type_internal>;
			static constexpr auto newSize = size;
			if constexpr (newSize > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					context.requiredSize += 2 + context.indent;
					context.requiredSize += (newSize - 1) * (2 + context.indent);
				} else {
					context.requiredSize += newSize;
				}
				if constexpr (has_static_size<get_size_impl<elem_type, options>>) {
					context.requiredSize += newSize * get_size_impl<elem_type, options>::staticSize;
				} else {
					auto iter = getBeginIterVec(value);
					for (uint64_t index{}; index != newSize; ++index) {
						get_size<options>::impl(iter[static_cast<int64_t>(index)], context);
					}
				}
				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					context.requiredSize += 1 + context.indent;
				}
				++context.requiredSize;
			} else {
				context.requiredSize += 2;
			}
		}
	};

	template<tuple_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr auto memberCount = tuple_size_v<value_type>;

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			if constexpr (memberCount > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}
				get_size<options>::impl(get<0>(value), context);
				if constexpr (memberCount > 1) {
					functor_runner<tuple_member_sizer, offset_sequence<make_integer_sequence<memberCount - 1>, 1>>::impl(value, context);
				}
				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					context.requiredSize += 1 + context.indent;
				}
				++context.requiredSize;
			} else {
				context.requiredSize += 2;
			}
		}

		template<auto... values> struct tuple_member_sizer {
			template<uint64_t index, typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
				if constexpr (options.prettify) {
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}
				get_size<options>::impl(get<index>(value), context);
			}
		};
	};

	template<number_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr uint64_t staticSize{ 32 };

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&, size_context& context) noexcept {
			context.requiredSize += staticSize;
		}
	};

	template<enum_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr uint64_t staticSize{ 32 };

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&, size_context& context) noexcept {
			context.requiredSize += staticSize;
		}
	};

	template<string_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			const auto newSize = value.size();
			context.requiredSize += newSize * 6 + 2;
		}
	};

	template<char_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr uint64_t staticSize{ 8 };

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&, size_context& context) noexcept {
			context.requiredSize += 8;
		}
	};

	template<bool_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr uint64_t staticSize{ 5 };

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&, size_context& context) noexcept {
			context.requiredSize += 5;
		}
	};

	template<skip_or_always_null_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		static constexpr uint64_t staticSize{ 4 };

		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&, size_context& context) noexcept {
			alignas(64) static constexpr char_blitter<"null"> nullV{};
			context.requiredSize += nullV.lengthToAdvance;
		}
	};

	template<any_pointer_or_optional_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			if (value) {
				using v_type = remove_cv_t<decltype(*value)>;
				if constexpr (has_static_size<get_size_impl<v_type, options>>) {
					context.requiredSize += get_size_impl<v_type, options>::staticSize;
				} else {
					get_size<options>::impl(*value, context);
				}
			} else {
				alignas(64) static constexpr char_blitter<"null"> nullV{};
				context.requiredSize += nullV.lengthToAdvance;
			}
		}
	};

	template<raw_json_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			const auto rawJson = value.rawJson();
			const auto size	   = rawJson.size();
			context.requiredSize += size;
		}
	};

	template<serialize_options options> struct get_size_visit_functor {
		template<typename value_type, typename context_type> JSONIFIER_INLINE static void impl(value_type&& valueNewer, context_type&& contextNew) {
			get_size<options>::impl(valueNewer, contextNew);
		}
	};

	template<variant_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			internal::visit<get_size_visit_functor<options>>(value, context);
		}
	};

	template<auto data> struct indent_blitter {
		template<uint64_t index> JSONIFIER_INLINE static bool impl([[maybe_unused]] write_buffer_ptr __restrict& bufferPtr, uint64_t& remainingLength) {
			static constexpr const uint64_t* ptr = data.data() + index;
			static constexpr uint64_t sizeToCopy{ sizeof(uint64_t) };
			static constexpr uint64_t offset{ index * sizeof(uint64_t) };
			if (static_cast<int64_t>(remainingLength) > 0) {
				pow2MemcpyWrapper<sizeToCopy>(bufferPtr + offset, ptr);
				remainingLength -= sizeToCopy;
				return true;
			} else {
				return false;
			}
		}
	};

	template<string_literal prefix, char indentChar, uint64_t indentSize> struct indent_table {
		alignas(64) static constexpr uint64_t maxDepth{ 8 };
		alignas(64) static constexpr uint64_t maxIndentBytes{ maxDepth * indentSize };
		alignas(64) static constexpr uint64_t totalLen{ prefix.size() + maxIndentBytes };
		alignas(64) static constexpr uint64_t paddedLen{ (totalLen + 7) & ~uint64_t{ 7 } };

		JSONIFIER_INLINE static void blitWithOverflow(write_buffer_ptr __restrict& bufferPtr, uint64_t totalIndent) noexcept {
			const uint64_t capped  = totalIndent < maxIndentBytes ? totalIndent : maxIndentBytes;
			const uint64_t advance = prefix.size() + capped;
			uint64_t copyLen	   = (advance + 7) & ~uint64_t{ 7 };
			functor_runner<indent_blitter, make_integer_sequence<paddedLen / sizeof(uint64_t)>, data>::implAnd(bufferPtr, copyLen);
			bufferPtr += advance;
			const uint64_t remaining = totalIndent - capped;
			if (remaining) [[unlikely]] {
				std::memset(bufferPtr, indentChar, remaining);
				bufferPtr += remaining;
			}
		}

		alignas(64) static constexpr array<uint64_t, (paddedLen / sizeof(uint64_t))> data{ []() -> array<uint64_t, (paddedLen / sizeof(uint64_t))> {
			array<uint64_t, (paddedLen / sizeof(uint64_t))> arr{};
			for (uint64_t i = 0; i < prefix.size(); ++i) {
				arr[i / sizeof(uint64_t)] |= static_cast<uint64_t>(static_cast<uint8_t>(prefix.values[i])) << ((i % sizeof(uint64_t)) * 8);
			}
			for (uint64_t i = 0; i < maxIndentBytes; ++i) {
				const uint64_t idx = prefix.size() + i;
				arr[idx / sizeof(uint64_t)] |= static_cast<uint64_t>(static_cast<uint8_t>(indentChar)) << ((idx % sizeof(uint64_t)) * 8);
			}
			return arr;
		}() };
	};

	template<uint64_t size> consteval uint64_t roundToChunk() noexcept {
		if constexpr (size <= 2) {
			return 2;
		} else if constexpr (size <= 4) {
			return 4;
		} else if constexpr (size <= 8) {
			return 8;
		} else if constexpr (size <= 16) {
			return 16;
		} else {
			return (size + 31) & ~uint64_t{ 31 };
		}
	}

	template<string_literal literal> struct packed_blitter {
		static constexpr uint64_t lengthToAdvance{ literal.size() };
		static constexpr uint64_t lengthToCopy{ roundToChunk<literal.size()>() };
		static constexpr bool isScalar{ lengthToCopy <= 8 };
		using int_type = conditional_t<isScalar, convert_length_to_int_t<lengthToCopy>, uint64_t>;
		static constexpr uint64_t wordCount{ isScalar ? 1 : lengthToCopy / 8 };
		using return_type = conditional_t<isScalar, int_type, array<uint64_t, wordCount>>;

		alignas(64) static constexpr return_type value{ []() -> return_type {
			if constexpr (isScalar) {
				int_type val{};
				for (uint64_t x = 0; x < literal.size(); ++x) {
					if constexpr (std::endian::native == std::endian::little) {
						val |= static_cast<int_type>(static_cast<uint8_t>(literal.values[x])) << (x * 8);
					} else {
						val |= static_cast<int_type>(static_cast<uint8_t>(literal.values[x])) << ((sizeof(int_type) - 1 - x) * 8);
					}
				}
				return val;
			} else {
				array<uint64_t, wordCount> arr{};
				for (uint64_t x = 0; x < literal.size(); ++x) {
					if constexpr (std::endian::native == std::endian::little) {
						arr[x / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(literal.values[x])) << ((x % 8) * 8);
					} else {
						arr[x / 8] |= static_cast<uint64_t>(static_cast<uint8_t>(literal.values[x])) << ((7 - (x % 8)) * 8);
					}
				}
				return arr;
			}
		}() };

		JSONIFIER_INLINE static void blit(write_buffer_ptr __restrict& bufferPtr) noexcept {
			if constexpr (isScalar) {
				pow2MemcpyWrapper<lengthToCopy>(bufferPtr, &value);
			} else {
				memcpyWrapper(bufferPtr, value.data(), lengthToCopy);
			}
			bufferPtr += lengthToAdvance;
		}
	};

	template<serialize_options options, string_literal key> JSONIFIER_INLINE static write_buffer_ptr writeObjectEntry(write_buffer_ptr __restrict bufferPtr) noexcept {
		static constexpr auto unQuotedKey = string_literal{ "\"" } + key;
		if constexpr (options.prettify) {
			packed_blitter<unQuotedKey + string_literal{ "\": " }>::blit(bufferPtr);
		} else {
			packed_blitter<unQuotedKey + string_literal{ "\":" }>::blit(bufferPtr);
		}
		return bufferPtr;
	}

	template<serialize_options options, bool isItLast> JSONIFIER_INLINE static write_buffer_ptr writeObjectExit(write_buffer_ptr __restrict bufferPtr, [[maybe_unused]] uint64_t indent) noexcept {
		if constexpr (!isItLast) {
			if constexpr (options.prettify) {
				using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
				comma_indent::blitWithOverflow(bufferPtr, indent);
			} else {
				*bufferPtr = ',';
				++bufferPtr;
			}
		}
		return bufferPtr;
	}

	template<serialize_options options, typename json_entity_type> struct json_entity_serialize : public json_entity_type {
		template<typename value_type>
		inline static write_buffer_ptr processIndex(value_type& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			if constexpr (has_excluded_keys<value_type>) {
				auto& keys = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename jsonifier::internal::remove_reference_t<decltype(keys)>::key_type>(json_entity_type::name)) != keys.end()) [[unlikely]] {
					return bufferPtr;
				}
			}
			bufferPtr = writeObjectEntry<options, json_entity_type::name>(bufferPtr);
			bufferPtr = serialize<options>::impl(getMember<json_entity_type::memberPtr>(value), bufferPtr, indent);
			return writeObjectExit<options, json_entity_type::isItLast>(bufferPtr, indent);
		}

		inline constexpr json_entity_serialize() noexcept = default;
	};

	template<typename... bases> struct serialize_map : public bases... {
		template<typename value_type>
		inline static write_buffer_ptr iterateValues([[maybe_unused]] value_type& value, write_buffer_ptr __restrict bufferPtr,
			[[maybe_unused]] uint64_t indent) noexcept {
			((bufferPtr = bases::processIndex(value, bufferPtr, indent)), ...);
			return bufferPtr;
		}
	};

	template<serialize_options options, typename value_type, typename integer_sequence> struct get_serialize_base;

	template<serialize_options options, typename value_type, uint64_t... index> struct get_serialize_base<options, value_type, integer_sequence<index...>> {
		using type = serialize_map<json_entity_serialize<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<serialize_options options, typename value_type> using serialize_base_t =
		typename get_serialize_base<options, value_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		using open_indent  = indent_table<"{\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"{}"> emptyObject{};

		template<typename value_type_new> inline static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			static constexpr auto memberCount{ coreTupleSize<value_type> };

			if constexpr (memberCount > 0) {
				const uint64_t innerIndent = indent + options.indentSize;
				if constexpr (options.prettify) {
					open_indent::blitWithOverflow(bufferPtr, innerIndent);
				} else {
					*bufferPtr = '{';
					++bufferPtr;
				}

				bufferPtr = serialize_base_t<options, value_type>::iterateValues(value, bufferPtr, innerIndent);

				if constexpr (options.prettify) {
					close_indent::blitWithOverflow(bufferPtr, indent);
				}
				*bufferPtr = '}';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyObject.lengthToCopy>(bufferPtr, &emptyObject.value);
				bufferPtr += emptyObject.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<map_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		using open_indent  = indent_table<"{\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<": "> colonSpace{};
		alignas(64) static constexpr char_blitter<"{}"> emptyObject{};

		template<typename key_type_new, typename mapped_type_new>
		inline static write_buffer_ptr writePair(key_type_new& key, mapped_type_new& mapped, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			using key_type = base_t<key_type_new>;
			if constexpr (!string_t<key_type>) {
				*bufferPtr = '"';
				++bufferPtr;
			}
			bufferPtr = serialize<options>::impl(key, bufferPtr, indent);
			if constexpr (!string_t<key_type>) {
				*bufferPtr = '"';
				++bufferPtr;
			}
			if constexpr (options.prettify) {
				pow2MemcpyWrapper<colonSpace.lengthToCopy>(bufferPtr, &colonSpace.value);
				bufferPtr += colonSpace.lengthToAdvance;
			} else {
				*bufferPtr = ':';
				++bufferPtr;
			}
			return serialize<options>::impl(mapped, bufferPtr, indent);
		}

		template<typename value_type_new> inline static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			const auto newSize = value.size();

			if (newSize > 0) [[likely]] {
				const uint64_t innerIndent = indent + options.indentSize;
				if constexpr (options.prettify) {
					open_indent::blitWithOverflow(bufferPtr, innerIndent);
				} else {
					*bufferPtr = '{';
					++bufferPtr;
				}
				auto iter = value.begin();
				bufferPtr = writePair(iter->first, iter->second, bufferPtr, innerIndent);
				++iter;
				const auto end = value.end();
				for (; iter != end; ++iter) {
					if constexpr (options.prettify) {
						comma_indent::blitWithOverflow(bufferPtr, innerIndent);
					} else {
						*bufferPtr = ',';
						++bufferPtr;
					}
					bufferPtr = writePair(iter->first, iter->second, bufferPtr, innerIndent);
				}
				if constexpr (options.prettify) {
					close_indent::blitWithOverflow(bufferPtr, indent);
				}
				*bufferPtr = '}';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyObject.lengthToCopy>(bufferPtr, &emptyObject.value);
				bufferPtr += emptyObject.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<vector_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};

		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			const auto newSize = value.size();
			if (newSize > 0) [[likely]] {
				const uint64_t innerIndent = indent + options.indentSize;
				if constexpr (options.prettify) {
					open_indent::blitWithOverflow(bufferPtr, innerIndent);
				} else {
					*bufferPtr = '[';
					++bufferPtr;
				}

				auto iter = getBeginIterVec(value);
				bufferPtr = serialize<options>::impl(iter[0], bufferPtr, innerIndent);
				for (uint64_t index{ 1 }; index != newSize; ++index) {
					if constexpr (options.prettify) {
						comma_indent::blitWithOverflow(bufferPtr, innerIndent);
					} else {
						*bufferPtr = ',';
						++bufferPtr;
					}
					bufferPtr = serialize<options>::impl(iter[static_cast<int64_t>(index)], bufferPtr, innerIndent);
				}

				if constexpr (options.prettify) {
					close_indent::blitWithOverflow(bufferPtr, indent);
				}
				*bufferPtr = ']';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyArray.lengthToCopy>(bufferPtr, &emptyArray.value);
				bufferPtr += emptyArray.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<raw_array_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};

		template<template<typename, auto> typename value_type_new, typename value_type_internal, auto size>
		JSONIFIER_INLINE static write_buffer_ptr impl(const value_type_new<value_type_internal, size>& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			static constexpr auto newSize = size;
			if constexpr (newSize > 0) {
				const uint64_t innerIndent = indent + options.indentSize;
				if constexpr (options.prettify) {
					open_indent::blitWithOverflow(bufferPtr, innerIndent);
				} else {
					*bufferPtr = '[';
					++bufferPtr;
				}

				auto iter = getBeginIterVec(value);
				bufferPtr = serialize<options>::impl(iter[0], bufferPtr, innerIndent);
				if constexpr (newSize > 1) {
					for (uint64_t index{ 1 }; index != newSize; ++index) {
						if constexpr (options.prettify) {
							comma_indent::blitWithOverflow(bufferPtr, innerIndent);
						} else {
							*bufferPtr = ',';
							++bufferPtr;
						}
						bufferPtr = serialize<options>::impl(iter[index], bufferPtr, innerIndent);
					}
				}

				if constexpr (options.prettify) {
					close_indent::blitWithOverflow(bufferPtr, indent);
				}
				*bufferPtr = ']';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyArray.lengthToCopy>(bufferPtr, &emptyArray.value);
				bufferPtr += emptyArray.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<tuple_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;
		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};
		static constexpr auto memberCount = tuple_size_v<value_type>;

		template<uint64_t index, typename value_type_new>
		inline static write_buffer_ptr serializeMember(value_type_new& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			if constexpr (options.prettify) {
				comma_indent::blitWithOverflow(bufferPtr, indent);
			} else {
				*bufferPtr = ',';
				++bufferPtr;
			}
			return serialize<options>::impl(get<index>(value), bufferPtr, indent);
		}

		template<typename value_type_new, uint64_t... indices>
		inline static write_buffer_ptr serializeRest(value_type_new& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent, integer_sequence<indices...>) noexcept {
			((bufferPtr = serializeMember<indices + 1>(value, bufferPtr, indent)), ...);
			return bufferPtr;
		}

		template<typename value_type_new> inline static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			if constexpr (memberCount > 0) {
				const uint64_t innerIndent = indent + options.indentSize;
				if constexpr (options.prettify) {
					open_indent::blitWithOverflow(bufferPtr, innerIndent);
				} else {
					*bufferPtr = '[';
					++bufferPtr;
				}
				bufferPtr = serialize<options>::impl(get<0>(value), bufferPtr, innerIndent);
				if constexpr (memberCount > 1) {
					bufferPtr = serializeRest(value, bufferPtr, innerIndent, make_integer_sequence<memberCount - 1>{});
				}
				if constexpr (options.prettify) {
					close_indent::blitWithOverflow(bufferPtr, indent);
				}
				*bufferPtr = ']';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyArray.lengthToCopy>(bufferPtr, &emptyArray.value);
				bufferPtr += emptyArray.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<string_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		alignas(64) static constexpr char_blitter<"\"\""> emptyString{};
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			const auto newSize = value.size();
			if (newSize > 0) {
				*bufferPtr = '"';
				++bufferPtr;
				bufferPtr  = string_serializer<options>::impl(value.data(), bufferPtr, newSize);
				*bufferPtr = '"';
				++bufferPtr;
			} else {
				pow2MemcpyWrapper<emptyString.lengthToCopy>(bufferPtr, &emptyString.value);
				bufferPtr += emptyString.lengthToAdvance;
			}
			return bufferPtr;
		}
	};

	template<char_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			*bufferPtr = '"';
			++bufferPtr;
			const uint8_t nextChar = static_cast<uint8_t>(value);
			memcpyWrapper(bufferPtr, charEscapeTable[nextChar], charEscapeSizes[nextChar]);
			bufferPtr += charEscapeSizes[nextChar];
			*bufferPtr = '"';
			return bufferPtr + 1;
		}
	};

	template<enum_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			int64_t valueNew{ static_cast<int64_t>(value) };
			return serialize<options>::impl(valueNew, bufferPtr, indent);
		}
	};

	template<number_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			if constexpr (sizeof(value_type) == 8) {
				return to_chars<std::remove_cvref_t<value_type_new>>::impl(bufferPtr, value);
			} else {
				if constexpr (uint_types<std::remove_cvref_t<value_type_new>>) {
					return to_chars<uint64_t>::impl(bufferPtr, static_cast<uint64_t>(value));
				} else if constexpr (int_types<value_type>) {
					return to_chars<int64_t>::impl(bufferPtr, static_cast<int64_t>(value));
				} else {
					return to_chars<double>::impl(bufferPtr, static_cast<double>(value));
				}
			}
		}
	};

	template<bool_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			alignas(64) static constexpr uint64_t falseVInt{ [] {
				if constexpr (std::endian::native == std::endian::little) {
					return 435728179558ULL;
				} else {
					return 7377296907481120768ULL;
				}
			}() };
			alignas(64) static constexpr uint64_t trueVInt{ [] {
				if constexpr (std::endian::native == std::endian::little) {
					return 434025983730ULL;
				} else {
					return 17433142848793870336ULL;
				}
			}() };
			const uint64_t state = falseVInt - (value * trueVInt);
			pow2MemcpyWrapper<8>(bufferPtr, &state);
			return bufferPtr + (5 - value);
		}
	};

	template<any_pointer_or_optional_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			if (value) {
				return serialize<options>::impl(*value, bufferPtr, indent);
			} else {
				alignas(64) static constexpr char_blitter<"null"> nullV{};
				pow2MemcpyWrapper<nullV.lengthToCopy>(bufferPtr, &nullV.value);
				return bufferPtr + nullV.lengthToAdvance;
			}
		}
	};

	template<raw_json_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			const auto rawJson = value.rawJson();
			const auto size	   = rawJson.size();
			memcpyWrapper(bufferPtr, rawJson.data(), size);
			return bufferPtr + size;
		}
	};

	template<skip_or_always_null_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&&, write_buffer_ptr __restrict bufferPtr, uint64_t) noexcept {
			alignas(64) static constexpr char_blitter<"null"> nullV{};
			pow2MemcpyWrapper<nullV.lengthToCopy>(bufferPtr, &nullV.value);
			return bufferPtr + nullV.lengthToAdvance;
		}
	};

	template<serialize_options options> struct serialize_visit_functor {
		template<typename value_type> JSONIFIER_INLINE static void impl(value_type&& valueNewer, write_buffer_ptr& bufferPtr, uint64_t indent) noexcept {
			bufferPtr = serialize<options>::impl(valueNewer, bufferPtr, indent);
		}
	};

	template<variant_t value_type, serialize_options options> struct serialize_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr __restrict bufferPtr, uint64_t indent) noexcept {
			internal::visit<serialize_visit_functor<options>>(value, bufferPtr, indent);
			return bufferPtr;
		}
	};
}
