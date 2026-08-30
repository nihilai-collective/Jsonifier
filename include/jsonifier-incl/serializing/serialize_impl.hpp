// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/serializing/serialize_impl.hpp
#pragma once

#include <jsonifier-incl/serializing/serializer.hpp>
#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/json_entity.hpp>

namespace jsonifier::internal {

	template<typename value_type> consteval uint64_t getValueSize(value_type value) {
		if constexpr (integral_types<value_type>) {
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

	template<serialize_options options, string_literal key> JSONIFIER_INLINE static constexpr uint64_t objectEntrySize() noexcept {
		if constexpr (options.prettify) {
			return key.size() + 4;
		} else {
			return key.size() + 3;
		}
	}

	template<serialize_options options, typename json_entity_type> struct json_entity_size : public json_entity_type {
		constexpr json_entity_size() noexcept = default;

		template<typename value_type, typename context_type> JSONIFIER_INLINE static void processIndex(value_type& value, context_type& context) {
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
	};

	template<typename... bases> struct size_getter_map : public bases... {
		template<typename json_entity_type, typename... arg_types> JSONIFIER_INLINE static void iterateValuesImpl(arg_types&&... args) {
			json_entity_type::processIndex(internal::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static constexpr void iterateValues([[maybe_unused]] arg_types&&... args) {
			((iterateValuesImpl<bases>(internal::forward<arg_types>(args)...)), ...);
		}
	};

	template<serialize_options options, typename value_type, typename integer_sequence> struct get_size_getter_base;

	template<serialize_options options, typename value_type, uint64_t... index> struct get_size_getter_base<options, value_type, integer_sequence<index...>> {
		using type = size_getter_map<json_entity_size<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<serialize_options options, typename value_type> using size_getter_base_t =
		typename get_size_getter_base<options, value_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
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
		template<typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
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
					context.requiredSize += newSize * get_size_impl<key_type, options>::staticSize;
					auto iter	   = value.begin();
					const auto end = value.end();
					for (; iter != end; ++iter) {
						get_size<options>::impl(iter->second, context);
					}
				} else if constexpr (has_static_size<get_size_impl<mapped_type, options>>) {
					context.requiredSize += newSize * get_size_impl<mapped_type, options>::staticSize;
					auto iter	   = value.begin();
					const auto end = value.end();
					for (; iter != end; ++iter) {
						get_size<options>::impl(iter->first, context);
					}
				} else {
					auto iter	   = value.begin();
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
		template<typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
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

		template<auto... values> struct tuple_member_sizer {
			template<uint64_t index, typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
				if constexpr (options.prettify) {
					context.requiredSize += 2 + context.indent;
				} else {
					++context.requiredSize;
				}
				get_size<options>::impl(get<index>(value), context);
			}
		};

		template<typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
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
	};

	template<num_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
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

	template<variant_t value_type, serialize_options options> struct get_size_impl<value_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new& value, size_context& context) noexcept {
			static constexpr auto lambda = [](auto& valueNewer, auto& contextNew) {
				get_size<options>::impl(valueNewer, contextNew);
			};
			visit<lambda>(value, context);
		}
	};

	template<string_literal prefix, char indentChar, uint64_t indentSize> struct indent_table {
		alignas(64) static constexpr uint64_t maxDepth{ 8 };
		alignas(64) static constexpr uint64_t maxIndentBytes{ maxDepth * indentSize };
		alignas(64) static constexpr uint64_t totalLen{ prefix.size() + maxIndentBytes };
		alignas(64) static constexpr uint64_t paddedLen{ (totalLen + 7) & ~uint64_t{ 7 } };
		alignas(64) static constexpr array<char, paddedLen> data{ []() -> array<char, paddedLen> {
			array<char, paddedLen> arr{};
			for (uint64_t i = 0; i < prefix.size(); ++i)
				arr[i] = prefix.values[i];
			for (uint64_t i = 0; i < maxIndentBytes; ++i)
				arr[prefix.size() + i] = indentChar;
			return arr;
		}() };

		JSONIFIER_INLINE static void blitWithOverflow(string_buffer_ptr __restrict& bufferPtr, uint64_t totalIndent) noexcept {
			const uint64_t capped  = totalIndent < maxIndentBytes ? totalIndent : maxIndentBytes;
			const uint64_t advance = prefix.size() + capped;
			const uint64_t copyLen = (advance + 7) & ~uint64_t{ 7 };
			std::memcpy(bufferPtr, data.data(), copyLen);
			bufferPtr += advance;
			const uint64_t remaining = totalIndent - capped;
			if (remaining) [[unlikely]] {
				std::memset(bufferPtr, indentChar, remaining);
				bufferPtr += remaining;
			}
		}
	};

	template<uint64_t size> constexpr uint64_t roundToChunk() noexcept {
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
		using return_type = conditional_t<isScalar, int_type, array<uint64_t, wordCount> >;

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

		JSONIFIER_INLINE static void blit(string_buffer_ptr __restrict& bufferPtr) noexcept {
			if constexpr (isScalar) {
				std::memcpy(bufferPtr, &value, lengthToCopy);
			} else {
				std::memcpy(bufferPtr, value.data(), lengthToCopy);
			}
			bufferPtr += lengthToAdvance;
		}
	};

	template<serialize_options options, string_literal key, typename context_type> JSONIFIER_INLINE static void writeObjectEntry(context_type& context) {
		static constexpr auto unQuotedKey = string_literal{ "\"" } + key;
		if constexpr (options.prettify) {
			packed_blitter<unQuotedKey + string_literal{ "\": " }>::blit(context.bufferPtr);
		} else {
			packed_blitter<unQuotedKey + string_literal{ "\":" }>::blit(context.bufferPtr);
		}
	}

	template<serialize_options options, bool isItLast, typename context_type> JSONIFIER_INLINE static void writeObjectExit(context_type& context) {
		if constexpr (!isItLast) {
			if constexpr (options.prettify) {
				using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
				comma_indent::blitWithOverflow(context.bufferPtr, context.indent);
			} else {
				*context.bufferPtr = ',';
				++context.bufferPtr;
			}
		}
	}

	template<serialize_options options, typename json_entity_type> struct json_entity_serialize : public json_entity_type {
		constexpr json_entity_serialize() noexcept = default;

		template<typename value_type, typename context_type> JSONIFIER_INLINE static void processIndex(value_type& value, context_type& context) {
			if constexpr (has_excluded_keys<value_type>) {
				auto& keys = value.jsonifierExcludedKeys;
				if (keys.find(static_cast<typename jsonifier::internal::remove_reference_t<decltype(keys)>::key_type>(json_entity_type::name)) != keys.end()) [[unlikely]] {
					return;
				}
			}
			writeObjectEntry<options, json_entity_type::name>(context);
			serialize<options>::impl(getMember<json_entity_type::memberPtr>(value), context);
			writeObjectExit<options, json_entity_type::isItLast>(context);
		}
	};

	template<typename... bases> struct serialize_map : public bases... {
		template<typename json_entity_type, typename... arg_types> JSONIFIER_INLINE static void iterateValuesImpl(arg_types&&... args) {
			json_entity_type::processIndex(internal::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE static constexpr void iterateValues([[maybe_unused]] arg_types&&... args) {
			((iterateValuesImpl<bases>(internal::forward<arg_types>(args)...)), ...);
		}
	};

	template<serialize_options options, typename value_type, typename context_type, typename integer_sequence, typename... value_types> struct get_serialize_base;

	template<serialize_options options, typename value_type, typename context_type, uint64_t... index>
	struct get_serialize_base<options, value_type, context_type, integer_sequence<index...>> {
		using type = serialize_map<json_entity_serialize<options, remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<serialize_options options, typename value_type, typename context_type> using serialize_base_t =
		typename get_serialize_base<options, value_type, context_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		using open_indent  = indent_table<"{\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"{}"> emptyObject{};

		template<typename value_type_new> inline static void impl(value_type_new&& value, context_type& context) noexcept {
			static constexpr auto memberCount{ coreTupleSize<value_type> };

			if constexpr (memberCount > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					open_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = '{';
					++context.bufferPtr;
				}

				serialize_base_t<options, value_type, context_type>::iterateValues(value, context);

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					close_indent::blitWithOverflow(context.bufferPtr, context.indent);
				}
				*context.bufferPtr = '}';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, &emptyObject.value, emptyObject.lengthToCopy);
				context.bufferPtr += emptyObject.lengthToAdvance;
			}
		}
	};

	template<map_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		using open_indent  = indent_table<"{\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<": "> colonSpace{};
		alignas(64) static constexpr char_blitter<"{}"> emptyObject{};

		template<typename value_type_new> inline static void impl(value_type_new&& value, context_type& context) noexcept {
			const auto newSize = value.size();

			if (newSize > 0) [[likely]] {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					open_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = '{';
					++context.bufferPtr;
				}

				auto iter = value.begin();
				serialize<options>::impl(iter->first, context);
				if constexpr (options.prettify) {
					std::memcpy(context.bufferPtr, &colonSpace.value, colonSpace.lengthToCopy);
					context.bufferPtr += colonSpace.lengthToAdvance;
				} else {
					*context.bufferPtr = ':';
					++context.bufferPtr;
				}
				serialize<options>::impl(iter->second, context);
				++iter;
				const auto end = value.end();
				for (; iter != end; ++iter) {
					if constexpr (options.prettify) {
						comma_indent::blitWithOverflow(context.bufferPtr, context.indent);
					} else {
						*context.bufferPtr = ',';
						++context.bufferPtr;
					}
					serialize<options>::impl(iter->first, context);
					if constexpr (options.prettify) {
						std::memcpy(context.bufferPtr, &colonSpace.value, colonSpace.lengthToCopy);
						context.bufferPtr += colonSpace.lengthToAdvance;
					} else {
						*context.bufferPtr = ':';
						++context.bufferPtr;
					}
					serialize<options>::impl(iter->second, context);
				}
				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					close_indent::blitWithOverflow(context.bufferPtr, context.indent);
				}
				*context.bufferPtr = '}';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, &emptyObject.value, emptyObject.lengthToCopy);
				context.bufferPtr += emptyObject.lengthToAdvance;
			}
		}
	};

	template<vector_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};

		template<typename value_type_new> inline static void impl(value_type_new&& value, context_type& context) noexcept {
			const auto newSize = value.size();
			if (newSize > 0) [[likely]] {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					open_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = '[';
					++context.bufferPtr;
				}

				auto iter = getBeginIterVec(value);
				serialize<options>::impl(iter[0], context);
				for (uint64_t index{ 1 }; index != newSize; ++index) {
					if constexpr (options.prettify) {
						comma_indent::blitWithOverflow(context.bufferPtr, context.indent);
					} else {
						*context.bufferPtr = ',';
						++context.bufferPtr;
					}
					serialize<options>::impl(iter[static_cast<int64_t>(index)], context);
				}

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					close_indent::blitWithOverflow(context.bufferPtr, context.indent);
				}
				*context.bufferPtr = ']';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, &emptyArray.value, emptyArray.lengthToCopy);
				context.bufferPtr += emptyArray.lengthToAdvance;
			}
		}
	};

	template<raw_array_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};

		template<template<typename, auto> typename value_type_new, typename value_type_internal, auto size>
		JSONIFIER_INLINE static void impl(const value_type_new<value_type_internal, size>& value, context_type& context) noexcept {
			static constexpr auto newSize = size;
			if constexpr (newSize > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					open_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = '[';
					++context.bufferPtr;
				}

				auto iter = getBeginIterVec(value);
				serialize<options>::impl(iter[0], context);
				if constexpr (newSize > 1) {
					for (uint64_t index{ 1 }; index != newSize; ++index) {
						if constexpr (options.prettify) {
							comma_indent::blitWithOverflow(context.bufferPtr, context.indent);
						} else {
							*context.bufferPtr = ',';
							++context.bufferPtr;
						}
						serialize<options>::impl(iter[index], context);
					}
				}

				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					close_indent::blitWithOverflow(context.bufferPtr, context.indent);
				}
				*context.bufferPtr = ']';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, &emptyArray.value, emptyArray.lengthToCopy);
				context.bufferPtr += emptyArray.lengthToAdvance;
			}
		}
	};

	template<tuple_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		using open_indent  = indent_table<"[\n", options.indentChar, options.indentSize>;
		using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
		using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;
		alignas(64) static constexpr char_blitter<"[]"> emptyArray{};
		static constexpr auto memberCount = tuple_size_v<value_type>;
		template<auto... values> struct tuple_member_serializer {
			template<uint64_t index, typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
				if constexpr (options.prettify) {
					comma_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = ',';
					++context.bufferPtr;
				}
				serialize<options>::impl(get<index>(value), context);
			}
		};
		template<typename value_type_new> inline static void impl(value_type_new&& value, context_type& context) noexcept {
			if constexpr (memberCount > 0) {
				if constexpr (options.prettify) {
					context.indent += options.indentSize;
					open_indent::blitWithOverflow(context.bufferPtr, context.indent);
				} else {
					*context.bufferPtr = '[';
					++context.bufferPtr;
				}
				serialize<options>::impl(get<0>(value), context);
				if constexpr (memberCount > 1) {
					functor_runner<tuple_member_serializer, offset_sequence<make_integer_sequence<memberCount - 1>, 1>>::impl(value, context);
				}
				if constexpr (options.prettify) {
					context.indent -= options.indentSize;
					close_indent::blitWithOverflow(context.bufferPtr, context.indent);
				}
				*context.bufferPtr = ']';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, &emptyArray.value, emptyArray.lengthToCopy);
				context.bufferPtr += emptyArray.lengthToAdvance;
			}
		}
	};

	template<string_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		alignas(64) static constexpr char packedValues01[]{ "\"\"" };
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			const auto newSize = value.size();
			if (newSize > 0) {
				*context.bufferPtr = '"';
				++context.bufferPtr;
				context.bufferPtr  = string_serializer<options, decltype(value.data()), decltype(context.bufferPtr)>::impl(value.data(), context.bufferPtr, value.size());
				*context.bufferPtr = '"';
				++context.bufferPtr;
			} else {
				std::memcpy(context.bufferPtr, packedValues01, 2);
				context.bufferPtr += 2;
			}
		}
	};

	template<char_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			*context.bufferPtr = '"';
			++context.bufferPtr;
			const uint8_t nextChar = static_cast<uint8_t>(value);
			std::memcpy(context.bufferPtr, charEscapeTable[nextChar], charEscapeSizes[nextChar]);
			context.bufferPtr += charEscapeSizes[nextChar];
			*context.bufferPtr = '"';
			++context.bufferPtr;
		}
	};

	template<enum_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			int64_t valueNew{ static_cast<int64_t>(value) };
			serialize<options>::impl(valueNew, context);
		}
	};

	template<num_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			if constexpr (sizeof(value_type) == 8) {
				context.bufferPtr = to_chars<std::remove_cvref_t<value_type_new>>::impl(context.bufferPtr, value);
			} else {
				if constexpr (uint_types<std::remove_cvref_t<value_type_new>>) {
					context.bufferPtr = to_chars<std::remove_cvref_t<uint64_t>>::impl(context.bufferPtr, static_cast<uint64_t>(value));
				} else if constexpr (int_types<value_type>) {
					context.bufferPtr = to_chars<std::remove_cvref_t<int64_t>>::impl(context.bufferPtr, static_cast<int64_t>(value));
				} else {
					context.bufferPtr = to_chars<std::remove_cvref_t<double>>::impl(context.bufferPtr, static_cast<double>(value));
				}
			}
		}
	};

	template<bool_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
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
			std::memcpy(context.bufferPtr, &state, 5);
			context.bufferPtr += 5 - value;
		}
	};

	template<any_pointer_or_optional_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			if (value) {
				serialize<options>::impl(*value, context);
			} else {
				alignas(64) static constexpr char_blitter<"null"> nullV{};
				std::memcpy(context.bufferPtr, &nullV.value, nullV.lengthToCopy);
				context.bufferPtr += nullV.lengthToAdvance;
			}
		}
	};

	template<raw_json_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			const auto rawJson = value.rawJson();
			const auto size	   = rawJson.size();
			std::memcpy(context.bufferPtr, rawJson.data(), size);
			context.bufferPtr += size;
		}
	};

	template<skip_or_always_null_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&&, context_type& context) noexcept {
			alignas(64) static constexpr char_blitter<"null"> nullV{};
			std::memcpy(context.bufferPtr, &nullV.value, nullV.lengthToCopy);
			context.bufferPtr += nullV.lengthToAdvance;
		}
	};

	template<variant_t value_type, typename context_type, serialize_options options> struct serialize_impl<value_type, context_type, options> {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, context_type& context) noexcept {
			static constexpr auto lambda = [](auto&& valueNewer, auto&& contextNew) {
				serialize<options>::impl(valueNewer, contextNew);
			};
			visit<lambda>(value, context);
		}
	};

}
