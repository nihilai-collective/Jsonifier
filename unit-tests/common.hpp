/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/common.hpp
 */
#pragma once

#include <jsonifier>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <numeric>
#include <random>
#include <array>
#include <rt-ut>
#include <map>

class file_handler {
  public:
	static std::string get(const std::string& path) {
		std::error_code errorCode{};
		std::string contents{};
		const std::filesystem::path abs_path = std::filesystem::absolute(path, errorCode);
		if (errorCode) {
			std::cout << "Failed to resolve path for reading: " << path << " (" << errorCode.message() << ")" << std::endl;
			return contents;
		}
		if (!std::filesystem::exists(abs_path, errorCode)) {
			std::cout << "File not found: " << abs_path.string() << std::endl;
			return contents;
		}
		if (!std::filesystem::is_regular_file(abs_path, errorCode)) {
			std::cout << "Not a regular file: " << abs_path.string() << std::endl;
			return contents;
		}
		std::fstream stream{ abs_path, std::ios::in | std::ios::binary };
		if (!stream.is_open()) {
			std::cout << "Failed to open for reading: " << abs_path.string() << std::endl;
			return contents;
		}
		contents = std::string{ std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
		if (stream.bad()) {
			std::cout << "Read error: " << abs_path.string() << std::endl;
			return contents;
		}
		if (contents.empty()) {
			std::cout << "File is empty: " << abs_path.string() << std::endl;
		}
		return contents;
	}

	static void save_file(const std::string& data, const std::string& path) {
		std::error_code errorCode{};
		const std::filesystem::path abs_path = std::filesystem::absolute(path, errorCode);
		if (errorCode) {
			std::cout << "Failed to resolve path for writing: " << path << " (" << errorCode.message() << ")" << std::endl;
			return;
		}
		std::filesystem::create_directories(abs_path.parent_path(), errorCode);
		if (errorCode) {
			std::cout << "Failed to create directories: " << abs_path.parent_path().string() << " (" << errorCode.message() << ")" << std::endl;
			return;
		}
		std::fstream stream{ abs_path, std::ios::out | std::ios::trunc };
		if (!stream.is_open()) {
			std::cout << "Failed to open for writing: " << abs_path.string() << std::endl;
			return;
		}
		stream << data;
		stream.flush();
		const bool ok = stream.good();
		stream.close();
		std::cout << (ok ? "Saved: " : "Write error: ") << abs_path.string() << std::endl;
	}
};

template<bool partialRead> inline constexpr rt_ut::string_literal testTypePartial{ [] {
	if constexpr (partialRead) {
		return rt_ut::string_literal{ "Partial-Reading: Enabled, " };
	} else {
		return rt_ut::string_literal{ "Partial-Reading: Disabled, " };
	}
}() };

template<bool knownOrder> inline constexpr rt_ut::string_literal testTypeKnownOrder{ [] {
	if constexpr (knownOrder) {
		return rt_ut::string_literal{ "Known-Order: Enabled, " };
	} else {
		return rt_ut::string_literal{ "Known-Order: Disabled, " };
	}
}() };

template<bool nullTerminated> inline constexpr rt_ut::string_literal testTypeNullTerminated{ [] {
	if constexpr (nullTerminated) {
		return rt_ut::string_literal{ "Null-Terminated: Enabled" };
	} else {
		return rt_ut::string_literal{ "Null-Terminated: Disabled" };
	}
}() };

template<typename value_type_out, typename value_type_in, const auto& pass_tests, const auto& pass_values, uint64_t index> struct generic_pass_parse {
	static bool impl(jsonifier::jsonifier_core& parser) {
		value_type_in data{};
		auto document = parser.iterate(pass_tests[index]);
		if (document.error() != jsonifier::generic::error_code::success || document.get(data) != jsonifier::generic::error_code::success) {
			std::cout << "Jsonifier generic parse failed for: " << pass_tests[index] << std::endl;
			return false;
		}
		const bool matches = std::equal_to<>{}(static_cast<value_type_out>(data), pass_values[index]);
		if (!matches) {
			std::cout << "Jsonifier generic parse mismatch for: " << pass_tests[index] << std::endl;
		}
		return matches;
	}
};

template<typename value_type_in, const auto& fail_tests, uint64_t index> struct generic_fail_parse {
	static bool impl(jsonifier::jsonifier_core& parser) {
		value_type_in data{};
		auto document = parser.iterate(fail_tests[index]);
		return document.error() == jsonifier::generic::error_code::success && document.get(data) == jsonifier::generic::error_code::success;
	}
};

template<typename value_type_out, typename value_type_in, const auto& pass_tests, const auto& pass_values, bool partial, bool knownOrder, bool nullTerminated>
struct pass_tests_runner {
	template<uint64_t index> static void impl(jsonifier::jsonifier_core& parser) {
		rt_ut::unit_test<rt_ut::string_literal{ rt_ut::create_string_literal<pass_tests[index].size()>(pass_tests[index].data()) }, true>::assert_eq(true,
			&generic_pass_parse<value_type_out, value_type_in, pass_tests, pass_values, index>::impl, parser);
	}
};

template<typename value_type_in, const auto& fail_tests, bool partial, bool knownOrder, bool nullTerminated> struct fail_tests_runner {
	template<uint64_t index> static void impl(jsonifier::jsonifier_core& parser) {
		rt_ut::unit_test<rt_ut::string_literal{ rt_ut::create_string_literal<fail_tests[index].size()>(fail_tests[index].data()) }, true>::assert_ne(true,
			&generic_fail_parse<value_type_in, fail_tests, index>::impl, parser);
	}
};

template<typename value_type_out, typename value_type_in, const auto& pass_tests, const auto& pass_values, bool partial, bool knownOrder, bool nullTerminated,
	template<typename, typename, const auto&, const auto&, bool, bool, bool> typename functor, typename integer_sequence>
struct pass_test_runner {
	static void impl() = delete;
};

template<typename value_type_out, typename value_type_in, const auto& pass_tests, const auto& pass_values, bool partial, bool knownOrder, bool nullTerminated,
	template<typename, typename, const auto&, const auto&, bool, bool, bool> typename functor, uint64_t... indices>
struct pass_test_runner<value_type_out, value_type_in, pass_tests, pass_values, partial, knownOrder, nullTerminated, functor, jsonifier::internal::integer_sequence<indices...>> {
	static void impl() {
		jsonifier::jsonifier_core parser{};
		(functor<value_type_out, value_type_in, pass_tests, pass_values, partial, knownOrder, nullTerminated>::template impl<indices>(parser), ...);
	}
};

template<typename value_type_in, const auto& fail_tests, bool partial, bool knownOrder, bool nullTerminated, template<typename, const auto&, bool, bool, bool> typename functor,
	typename integer_sequence>
struct fail_test_runner {
	static void impl() = delete;
};

template<typename value_type_in, const auto& fail_tests, bool partial, bool knownOrder, bool nullTerminated, template<typename, const auto&, bool, bool, bool> typename functor,
	uint64_t... indices>
struct fail_test_runner<value_type_in, fail_tests, partial, knownOrder, nullTerminated, functor, jsonifier::internal::integer_sequence<indices...>> {
	static void impl() {
		jsonifier::jsonifier_core parser{};
		(functor<value_type_in, fail_tests, partial, knownOrder, nullTerminated>::template impl<indices>(parser), ...);
	}
};
