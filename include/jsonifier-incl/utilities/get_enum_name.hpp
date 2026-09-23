/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/get_enum_name.hpp
 */
#pragma once

#include <jsonifier-incl/core/fastio.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>

namespace jsonifier::internal {

	template<printable_enum_types auto current_index> JSONIFIER_INLINE consteval string_view getEnumName() {
		string_view str = std::source_location::current().function_name();
#if JSONIFIER_COMPILER_GCC
		str			   = str.substr(str.find("=") + 2);
		uint64_t end   = str.find(';');
		str			   = str.substr(0, end);
		uint64_t start = str.findLastOf(':') + 1;
		return str.substr(start);
#else
	#if JSONIFIER_COMPILER_MSVC
		constexpr read_buffer_ptr prettyFunctionTailLocal{ ">(void)" };
	#elif JSONIFIER_COMPILER_CLANG
		constexpr read_buffer_ptr prettyFunctionTailLocal{ "]" };
	#endif
		str			   = str.substr(str.find("=") + 2);
		uint64_t start = str.findLastOf(':') + 1;
		uint64_t end   = str.find(prettyFunctionTailLocal);
		return str.substr(start, end - start);
#endif
	}

	JSONIFIER_INLINE consteval bool isValidEnumName(string_view name) noexcept {
		if (name.empty()) {
			return false;
		}
		if (name.findFirstOf("()") != string_view::npos) {
			return false;
		}
		if (name[0] >= '0' && name[0] <= '9') {
			return false;
		}
		return true;
	}

	template<typename integer_sequence, printable_enum_types current_type> struct enum_entry_getter;

	template<uint64_types auto... indices, printable_enum_types current_type> struct enum_entry_getter<integer_sequence<indices...>, current_type> {
		JSONIFIER_INLINE static consteval uint64_t countValid() noexcept {
			uint64_t total{};
			((isValidEnumName(getEnumName<static_cast<current_type>(indices)>()) ? ++total : total), ...);
			return total;
		}

		template<uint64_t n> struct entries {
			array<string_view, n> names{};
			array<current_type, n> values{};
		};

		template<uint64_t n> JSONIFIER_INLINE static consteval entries<n> buildEntries() noexcept {
			entries<n> result{};
			uint64_t pos{};
			(
				[&] {
					constexpr auto value	   = static_cast<current_type>(indices);
					constexpr string_view name = getEnumName<value>();
					if constexpr (isValidEnumName(name)) {
						result.names[pos]  = name;
						result.values[pos] = value;
						++pos;
					}
				}(),
				...);
			return result;
		}
	};

	template<printable_enum_types current_type, uint64_t probe_range = static_cast<uint64_t>(current_type::count)> struct enum_data {
		using entry_getter = enum_entry_getter<make_integer_sequence<probe_range>, current_type>;
		static constexpr uint64_t validCount{ entry_getter::countValid() };
		static constexpr auto value{ entry_getter::template buildEntries<validCount>() };
	};

	template<printable_enum_types enum_type> JSONIFIER_INLINE constexpr string_view getName(enum_type type) noexcept {
		constexpr auto& entries{ enum_data<enum_type>::value };
		for (uint64_t i = 0; i < entries.values.size(); ++i) {
			if (entries.values[i] == type) {
				return entries.names[i];
			}
		}
		return "Unknown Type";
	}

	template<printable_enum_types enum_type, size_t buffer_size>
	JSONIFIER_INLINE static basic_stream<buffer_size>& operator<<(basic_stream<buffer_size>& os, enum_type type) noexcept {
		os << getName(type).operator std::basic_string_view<char>();
		return os;
	}

	template<printable_enum_types enum_type> JSONIFIER_INLINE static std::ostream& operator<<(std::ostream& os, enum_type type) noexcept {
		os << getName(type).operator std::basic_string_view<char>();
		return os;
	}

}
