// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/printer.hpp
#pragma once

#include <jsonifier-incl/utilities/raw_json_data.hpp>
#include <jsonifier-incl/utilities/json_entity.hpp>
#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/parsing/validator.hpp>
#include <jsonifier-incl/utilities/string.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	template<typename value_type, typename = void> struct json_printer_impl;

	struct json_printer {
		template<typename value_type_new> inline static void printJson(value_type_new&& value, std::ostream& os, uint64_t depth = 0) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			os << "Printing Json" << std::endl;
			json_printer_impl<value_type>::impl(internal::forward<value_type_new>(value), os, depth);
		}

		template<typename value_type_new> inline static void printJsonImpl(value_type_new&& value, std::ostream& os, uint64_t depth = 0) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			json_printer_impl<value_type>::impl(internal::forward<value_type_new>(value), os, depth);
		}
	};

	inline void printIndent(std::ostream& os, uint64_t depth) noexcept {
		for (uint64_t x = 0; x < depth; ++x) {
			os << "  ";
		}
	}

	template<typename entity_type> struct json_entity_print : public entity_type {
		constexpr json_entity_print() = default;

		template<typename value_type> inline static void processIndex(const value_type& value, std::ostream& os, uint64_t depth) {
			printIndent(os, depth);
			os << entity_type::name.operator std::string_view() << " (size: " << sizeof(decltype(value.*entity_type::memberPtr)) << "): ";
			json_printer::printJsonImpl(value.*entity_type::memberPtr, os, depth + 1);
		}
	};

	template<typename... bases> struct print_map : public bases... {
		template<typename entity_type, typename... arg_types> inline static void iterateValuesImpl(arg_types&&... args) {
			entity_type::processIndex(internal::forward<arg_types>(args)...);
		}

		template<typename... arg_types> inline static void iterateValues(arg_types&&... args) {
			(iterateValuesImpl<bases>(internal::forward<arg_types>(args)...), ...);
		}
	};

	template<typename value_type, typename integer_sequence, typename... value_types> struct get_print_base;

	template<typename value_type, uint64_t... index> struct get_print_base<value_type, integer_sequence<index...>> {
		using type = print_map<json_entity_print<remove_cvref_t<decltype(getBecauseOtherLibAuthorsResolve<index>(core<value_type>::parseValue))>>...>;
	};

	template<typename value_type> using print_base_t = typename get_print_base<value_type, make_integer_sequence<coreTupleSize<value_type>>>::type;

	template<jsonifier_object_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			static constexpr auto memberCount{ coreTupleSize<value_type> };
			printIndent(os, depth);
			os << "object (members: " << memberCount << ", size: " << sizeof(remove_cvref_t<value_type_new>) << ") {" << std::endl;
			if constexpr (memberCount > 0) {
				print_base_t<remove_cvref_t<value_type>>::iterateValues(value, os, depth + 1);
			}
			printIndent(os, depth);
			os << "}" << std::endl;
		}
	};

	template<map_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			printIndent(os, depth);
			os << "map (entries: " << value.size() << ", size: " << sizeof(remove_cvref_t<value_type_new>) << ") {" << std::endl;
			for (auto iter = value.begin(); iter != value.end(); ++iter) {
				printIndent(os, depth + 1);
				os << "key: ";
				json_printer::printJsonImpl(iter->first, os, depth + 1);
				printIndent(os, depth + 1);
				os << "value: ";
				json_printer::printJsonImpl(iter->second, os, depth + 1);
			}
			printIndent(os, depth);
			os << "}" << std::endl;
		}
	};

	template<tuple_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			static constexpr auto size = std::tuple_size_v<remove_cvref_t<value_type>>;
			printIndent(os, depth);
			os << "tuple (elements: " << size << ", size: " << sizeof(remove_cvref_t<value_type_new>) << ") {" << std::endl;
			printElements<0, size>(value, os, depth + 1);
			printIndent(os, depth);
			os << "}" << std::endl;
		}

		template<uint64_t currentIndex, uint64_t newSize, typename value_type_new> inline static void printElements(value_type_new&& value, std::ostream& os, uint64_t depth) {
			if constexpr (currentIndex < newSize) {
				auto& sub = get<currentIndex>(value);
				printIndent(os, depth);
				os << "[" << currentIndex << "]: ";
				json_printer::printJsonImpl(sub, os, depth + 1);
				printElements<currentIndex + 1, newSize>(value, os, depth);
			}
		}
	};

	template<vector_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			const auto newSize = value.size();
			printIndent(os, depth);
			os << "vector (elements: " << newSize << ", size: " << sizeof(remove_cvref_t<value_type_new>) << ") [" << std::endl;
			auto iter = getBeginIterVec(value);
			for (int64_t index{ 0 }; index != static_cast<int64_t>(newSize); ++index) {
				printIndent(os, depth + 1);
				os << "[" << index << "]: ";
				json_printer::printJsonImpl(iter[index], os, depth + 1);
			}
			printIndent(os, depth);
			os << "]" << std::endl;
		}
	};

	template<raw_array_t value_type> struct json_printer_impl<value_type> {
		template<template<typename, uint64_t> typename value_type_new, typename value_type_internal, uint64_t size>
		inline static void impl(const value_type_new<value_type_internal, size>& value, std::ostream& os, uint64_t depth) {
			printIndent(os, depth);
			os << "array (elements: " << size << ", size: " << sizeof(value_type_new<value_type_internal, size>) << ") [" << std::endl;
			auto iter = getBeginIterVec(value);
			for (int64_t index{ 0 }; index != static_cast<int64_t>(size); ++index) {
				printIndent(os, depth + 1);
				os << "[" << index << "]: ";
				json_printer::printJsonImpl(iter[index], os, depth + 1);
			}
			printIndent(os, depth);
			os << "]" << std::endl;
		}
	};

	template<string_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			os << "\"" << value << "\" (length: " << value.size() << ", size: " << sizeof(remove_cvref_t<value_type_new>) << ")" << std::endl;
		}
	};

	template<char_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			os << "'" << value << "' (size: " << sizeof(remove_cvref_t<value_type_new>) << ")" << std::endl;
		}
	};

	template<enum_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			os << static_cast<int64_t>(value) << " (size: " << sizeof(remove_cvref_t<value_type_new>) << ")" << std::endl;
		}
	};

	template<num_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			os << value << " (size: " << sizeof(remove_cvref_t<value_type_new>) << ")" << std::endl;
		}
	};

	template<bool_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			os << (value ? "true" : "false") << " (size: " << sizeof(remove_cvref_t<value_type_new>) << ")" << std::endl;
		}
	};

	template<always_null_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&&, std::ostream& os, uint64_t) {
			os << "null" << std::endl;
		}
	};

	template<pointer_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, [[maybe_unused]] uint64_t depth) {
			if (!value) {
				os << "null" << std::endl;
				return;
			}
			os << "* -> ";
			json_printer::printJsonImpl(*value, os, depth);
		}
	};

	template<unique_ptr_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			if (!value) {
				os << "null" << std::endl;
				return;
			}
			os << "unique_ptr -> ";
			json_printer::printJsonImpl(*value, os, depth);
		}
	};

	template<shared_ptr_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			if (!value) {
				os << "null" << std::endl;
				return;
			}
			os << "shared_ptr -> ";
			json_printer::printJsonImpl(*value, os, depth);
		}
	};

	template<optional_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			if (!value) {
				os << "nullopt" << std::endl;
				return;
			}
			json_printer::printJsonImpl(value.value(), os, depth);
		}
	};

	template<variant_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			os << "variant (active index: " << value.index() << ", size: " << sizeof(remove_cvref_t<value_type_new>) << "): ";
			static constexpr auto lambda = [](auto&& valueNewer, std::ostream& osNew, uint64_t depthNew) {
				json_printer::printJsonImpl(valueNewer, osNew, depthNew);
			};
			visit<lambda>(value, os, depth);
		}
	};

	template<raw_json_t value_type> struct json_printer_impl<value_type> {
		inline static void printIndent(std::ostream& os, uint64_t depth) {
			for (uint64_t x = 0; x < depth; ++x) {
				os << "  ";
			}
		}

		inline static void printNumber(const jsonifier::json_number& number, std::ostream& os) {
			switch (static_cast<uint64_t>(number.getType())) {
				case static_cast<uint64_t>(jsonifier::json_number::number_types::uint64): {
					os << number.getUint() << " (uint64)";
					return;
				}
				case static_cast<uint64_t>(jsonifier::json_number::number_types::int64): {
					os << number.getInt() << " (int64)";
					return;
				}
				case static_cast<uint64_t>(jsonifier::json_number::number_types::double64): {
					os << number.getDouble() << " (double64)";
					return;
				}
				default: {
					os << "<invalid number>";
					return;
				}
			}
		}

		template<typename value_type_new> inline static void impl(value_type_new&& value, std::ostream& os, uint64_t depth) {
			switch (static_cast<uint64_t>(value.getType())) {
				case static_cast<uint64_t>(json_type::object): {
					const auto& object = value.getObject();
					os << "object (size: " << object.size() << ")" << std::endl;
					for (const auto& [key, entry]: object) {
						printIndent(os, depth + 1);
						os << key << ": ";
						impl(entry, os, depth + 1);
					}
					return;
				}
				case static_cast<uint64_t>(json_type::array): {
					const auto& array = value.getArray();
					os << "array (size: " << array.size() << ")" << std::endl;
					for (uint64_t x = 0; x < array.size(); ++x) {
						printIndent(os, depth + 1);
						os << "[" << x << "]: ";
						impl(array[x], os, depth + 1);
					}
					return;
				}
				case static_cast<uint64_t>(json_type::string): {
					const auto& string = value.getString();
					os << "\"" << string << "\" (length: " << string.size() << ")" << std::endl;
					return;
				}
				case static_cast<uint64_t>(json_type::number): {
					printNumber(value.getNumber(), os);
					os << std::endl;
					return;
				}
				case static_cast<uint64_t>(json_type::boolean): {
					os << (value.getBool() ? "true" : "false") << std::endl;
					return;
				}
				case static_cast<uint64_t>(json_type::null): {
					os << "null" << std::endl;
					return;
				}
				default: {
					os << "unset" << std::endl;
					return;
				}
			}
		}
	};

	template<skip_t value_type> struct json_printer_impl<value_type> {
		template<typename value_type_new> inline static void impl(value_type_new&&, std::ostream& os, uint64_t) {
			os << "skipped" << std::endl;
		}
	};

};
