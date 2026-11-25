/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/package_json_reader.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/document_stream.hpp>

namespace jsonifier::generic {

	struct package_json_fields {
		string_view exports{};
		string_view imports{};
		string_view scripts{};
		string_view name{};
		string_view main{};
		string_view type{};
		bool hasExports{};
		bool hasImports{};
		bool hasScripts{};
		bool hasPrivate{};
		bool isPrivate{};
		bool hasName{};
		bool hasMain{};
	};

}

namespace jsonifier::internal {

#if JSONIFIER_COMPILER_GCC
	inline constexpr bool packageJsonSlotDispatch{ true };
#else
	inline constexpr bool packageJsonSlotDispatch{ false };
#endif

	struct package_json_slot_table {
		uint64_t tags[8]{};
	};

	inline constexpr uint64_t packageJsonFieldTags[7]{ document_state::keyTagOf("exports"), document_state::keyTagOf("imports"), document_state::keyTagOf("private"),
		document_state::keyTagOf("scripts"), document_state::keyTagOf("name"), document_state::keyTagOf("main"), document_state::keyTagOf("type") };

	consteval uint64_t packageJsonSlotOf(uint64_t tag, uint64_t multiplier) noexcept {
		return (tag * multiplier) >> 61;
	}

	consteval bool packageJsonSeparates(uint64_t multiplier) noexcept {
		uint64_t used{};
		for (const uint64_t tag: packageJsonFieldTags) {
			const uint64_t slot = packageJsonSlotOf(tag, multiplier);
			if ((used & (uint64_t{ 1 } << slot)) || slot == packageJsonSlotOf(0, multiplier)) {
				return false;
			}
			used |= uint64_t{ 1 } << slot;
		}
		return true;
	}

	consteval uint64_t packageJsonFindMultiplier() noexcept {
		uint64_t candidate{ 1 };
		while (!packageJsonSeparates((candidate * 0xD1B54A32D192ED03ull) | 1)) {
			++candidate;
		}
		return (candidate * 0xD1B54A32D192ED03ull) | 1;
	}

	inline constexpr uint64_t packageJsonTagMultiplier{ packageJsonFindMultiplier() };

	consteval package_json_slot_table packageJsonBuildSlotTable() noexcept {
		package_json_slot_table table{};
		for (uint64_t x = 0; x < 8; ++x) {
			table.tags[x] = ~uint64_t{};
		}
		for (const uint64_t tag: packageJsonFieldTags) {
			table.tags[packageJsonSlotOf(tag, packageJsonTagMultiplier)] = tag;
		}
		return table;
	}

	inline constexpr package_json_slot_table packageJsonSlotTable{ packageJsonBuildSlotTable() };

	template<bool minified> struct package_json_reader {
		using value		  = generic::generic_value<minified>;
		using error_codes = generic::error_codes;

		static constexpr uint64_t exportsTag{ document_state::keyTagOf("exports") };
		static constexpr uint64_t importsTag{ document_state::keyTagOf("imports") };
		static constexpr uint64_t privateTag{ document_state::keyTagOf("private") };
		static constexpr uint64_t scriptsTag{ document_state::keyTagOf("scripts") };
		static constexpr uint64_t nameTag{ document_state::keyTagOf("name") };
		static constexpr uint64_t mainTag{ document_state::keyTagOf("main") };
		static constexpr uint64_t typeTag{ document_state::keyTagOf("type") };

		static constexpr uint64_t nameKey{ packageJsonSlotDispatch ? packageJsonSlotOf(nameTag, packageJsonTagMultiplier) : nameTag };
		static constexpr uint64_t mainKey{ packageJsonSlotDispatch ? packageJsonSlotOf(mainTag, packageJsonTagMultiplier) : mainTag };
		static constexpr uint64_t typeKey{ packageJsonSlotDispatch ? packageJsonSlotOf(typeTag, packageJsonTagMultiplier) : typeTag };
		static constexpr uint64_t privateKey{ packageJsonSlotDispatch ? packageJsonSlotOf(privateTag, packageJsonTagMultiplier) : privateTag };
		static constexpr uint64_t exportsKey{ packageJsonSlotDispatch ? packageJsonSlotOf(exportsTag, packageJsonTagMultiplier) : exportsTag };
		static constexpr uint64_t importsKey{ packageJsonSlotDispatch ? packageJsonSlotOf(importsTag, packageJsonTagMultiplier) : importsTag };
		static constexpr uint64_t scriptsKey{ packageJsonSlotDispatch ? packageJsonSlotOf(scriptsTag, packageJsonTagMultiplier) : scriptsTag };

		JSONIFIER_INLINE static error_codes impl(document_state& state, generic::package_json_fields& out) noexcept {
			const read_structural_index_ptr tapeEnd = state.tapeEnd;
			read_structural_index_ptr position		= state.tape;
			if (!state.charAt<uint_c_v<'{'>>(position)) [[unlikely]] {
				return error_codes::incorrect_type;
			}
			++position;
			if (position >= tapeEnd) [[unlikely]] {
				return error_codes::unclosed_container;
			}
			if (state.charAt<uint_c_v<'}'>>(position)) {
				return error_codes::success;
			}
			while (true) {
				if (position + 2 >= tapeEnd) [[unlikely]] {
					return error_codes::unclosed_container;
				}
				if (!state.charAt<uint_c_v<'"'>>(position) || !state.charAt<uint_c_v<':'>>(position + 1)) [[unlikely]] {
					return error_codes::tape_error;
				}
				const read_structural_index_ptr valuePosition = position + 2;
				const read_structural_index_ptr after		  = state.skipValue(valuePosition);
				const uint64_t tag = state.keyTag<minified>(position);
				const uint64_t key = packageJsonSlotDispatch ? (tag * packageJsonTagMultiplier) >> 61 : tag;
				if (packageJsonSlotDispatch ? packageJsonSlotTable.tags[key] == tag : tag != 0) {
					if (const error_codes error_code = readField(state, key, valuePosition, after, out); error_code != error_codes::success) [[unlikely]] {
						return error_code;
					}
				}
				if (after >= tapeEnd) [[unlikely]] {
					return error_codes::unclosed_container;
				}
				const uint8_t separator = state.charAt(after);
				if (separator == uint_c_v<','>) [[likely]] {
					position = after + 1;
				} else if (separator == uint_c_v<'}'>) {
					return error_codes::success;
				} else [[unlikely]] {
					return error_codes::tape_error;
				}
			}
		}

	  protected:
#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON && !defined(_MSC_VER)
		JSONIFIER_NOINLINE
#else
		JSONIFIER_INLINE
#endif
			static error_codes readField(document_state& state, uint64_t key, read_structural_index_ptr valuePosition, read_structural_index_ptr after,
			generic::package_json_fields& out) noexcept {
			switch (key) {
				case nameKey: {
					if (const error_codes error_code = value{ &state, valuePosition, 2 }.getString(out.name); error_code != error_codes::success) [[unlikely]] {
						return error_code;
					}
					out.hasName = true;
					return error_codes::success;
				}
				case mainKey: {
					string_view text{};
					if (value{ &state, valuePosition, 2 }.getString(text) == error_codes::success) {
						out.main	= text;
						out.hasMain = true;
					}
					return error_codes::success;
				}
				case typeKey: {
					string_view text{};
					if (const error_codes error_code = value{ &state, valuePosition, 2 }.getString(text); error_code != error_codes::success) [[unlikely]] {
						return error_code;
					}
					if (text == "commonjs" || text == "module") {
						out.type = text;
					}
					return error_codes::success;
				}
				case privateKey: {
					bool isPrivate{};
					if (value{ &state, valuePosition, 2 }.getBool(isPrivate) == error_codes::success) {
						out.isPrivate  = isPrivate;
						out.hasPrivate = true;
					}
					return error_codes::success;
				}
				case exportsKey: {
					return readContainerOrString(state, valuePosition, after, out.exports, out.hasExports);
				}
				case importsKey: {
					return readContainerOrString(state, valuePosition, after, out.imports, out.hasImports);
				}
				case scriptsKey: {
					if (!state.charAt<uint_c_v<'{'>>(valuePosition)) {
						return error_codes::success;
					}
					return readRaw(state, valuePosition, after, out.scripts, out.hasScripts);
				}
				default: {
					return error_codes::success;
				}
			}
		}

		JSONIFIER_INLINE static error_codes readContainerOrString(document_state& state, read_structural_index_ptr valuePosition, read_structural_index_ptr after,
			string_view& target, bool& present) noexcept {
			const uint8_t first = state.charAt(valuePosition);
			if (document_state::isOpener(first)) {
				return readRaw(state, valuePosition, after, target, present);
			}
			if (first != uint_c_v<'"'>) {
				return error_codes::success;
			}
			if (const error_codes error_code = value{ &state, valuePosition, 2 }.getString(target); error_code != error_codes::success) [[unlikely]] {
				return error_code;
			}
			present = true;
			return error_codes::success;
		}

		JSONIFIER_INLINE static error_codes readRaw(document_state& state, read_structural_index_ptr valuePosition, read_structural_index_ptr after, string_view& target,
			bool& present) noexcept {
			if (!document_state::isCloser(state.charAt(after - 1))) [[unlikely]] {
				return error_codes::unclosed_container;
			}
			const read_buffer_ptr start = state.root + *valuePosition;
			target						= string_view{ std::bit_cast<const char*>(start), static_cast<uint64_t>(state.root + *(after - 1) + 1 - start) };
			present						= true;
			return error_codes::success;
		}
	};

}
