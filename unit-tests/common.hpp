// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/common.hpp
#pragma once

#include <filesystem>
#include <jsonifier>
#include <iostream>
#include <fstream>
#include <random>
#include <array>
#include <rt-ut>
#include <map>

struct abc_in_order_test_struct {
	std::string test_string;
	uint64_t test_uint;
	int64_t test_int;
	double test_double;
	bool test_bool;
};

struct abc_in_order_test {
	std::vector<abc_in_order_test_struct> a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z;
};

struct abc_out_of_order_test_struct {
	bool test_bool;
	double test_double;
	int64_t test_int;
	uint64_t test_uint;
	std::string test_string;
};

struct abc_out_of_order_test {
	std::vector<abc_out_of_order_test_struct> z, y, x, w, v, u, t, s, r, q, p, o, n, m, l, k, j, i, h, g, f, e, d, c, b, a;
};

struct abc_in_order_partial_test_struct {
	std::string test_string;
	int64_t test_int;
	bool test_bool;
};

struct abc_in_order_partial_test {
	std::vector<abc_in_order_partial_test_struct> a, m, z;
};

struct abc_out_of_order_partial_test_struct {
	bool test_bool;
	int64_t test_int;
	std::string test_string;
};

struct abc_out_of_order_partial_test {
	std::vector<abc_out_of_order_partial_test_struct> z, m, a;
};

template<> struct jsonifier::core<abc_in_order_partial_test_struct> {
	using value_type				 = abc_in_order_partial_test_struct;
	static constexpr auto parseValue = createValue<&value_type::test_string, &value_type::test_int, &value_type::test_bool>();
};

template<> struct jsonifier::core<abc_in_order_partial_test> {
	using value_type				 = abc_in_order_partial_test;
	static constexpr auto parseValue = createValue<&value_type::a, &value_type::m, &value_type::z>();
};

template<> struct jsonifier::core<abc_in_order_test_struct> {
	using value_type				 = abc_in_order_test_struct;
	static constexpr auto parseValue = createValue<&value_type::test_string, &value_type::test_uint, &value_type::test_int, &value_type::test_double, &value_type::test_bool>();
};

template<> struct jsonifier::core<abc_in_order_test> {
	using value_type				 = abc_in_order_test;
	static constexpr auto parseValue = createValue<&value_type::a, &value_type::b, &value_type::c, &value_type::d, &value_type::e, &value_type::f, &value_type::g, &value_type::h,
		&value_type::i, &value_type::j, &value_type::k, &value_type::l, &value_type::m, &value_type::n, &value_type::o, &value_type::p, &value_type::q, &value_type::r,
		&value_type::s, &value_type::t, &value_type::u, &value_type::v, &value_type::w, &value_type::x, &value_type::y, &value_type::z>();
};

template<> struct jsonifier::core<abc_out_of_order_partial_test_struct> {
	using value_type				 = abc_out_of_order_partial_test_struct;
	static constexpr auto parseValue = createValue<&value_type::test_bool, &value_type::test_int, &value_type::test_string>();
};

template<> struct jsonifier::core<abc_out_of_order_partial_test> {
	using value_type				 = abc_out_of_order_partial_test;
	static constexpr auto parseValue = createValue<&value_type::z, &value_type::m, &value_type::a>();
};

template<> struct jsonifier::core<abc_out_of_order_test_struct> {
	using value_type				 = abc_out_of_order_test_struct;
	static constexpr auto parseValue = createValue<&value_type::test_bool, &value_type::test_double, &value_type::test_int, &value_type::test_uint, &value_type::test_string>();
};

template<> struct jsonifier::core<abc_out_of_order_test> {
	using value_type				 = abc_out_of_order_test;
	static constexpr auto parseValue = createValue<&value_type::z, &value_type::y, &value_type::x, &value_type::w, &value_type::v, &value_type::u, &value_type::t, &value_type::s,
		&value_type::r, &value_type::q, &value_type::p, &value_type::o, &value_type::n, &value_type::m, &value_type::l, &value_type::k, &value_type::j, &value_type::i,
		&value_type::h, &value_type::g, &value_type::f, &value_type::e, &value_type::d, &value_type::c, &value_type::b, &value_type::a>();
};

constexpr jsonifier::internal::string_literal basePath{ BASE_PATH };

class test_base {
  public:
	test_base() noexcept = default;

	test_base(const std::string& fileContentsNew) : fileContents{ fileContentsNew } {
	}
	std::string fileContents{};
};

inline static bool processFilesInFolder(std::unordered_map<std::string, test_base>& resultFileContents, const std::string& testType) noexcept {
	try {
		for (const auto& entry: std::filesystem::directory_iterator(std::string{ basePath.operator std::string() } + testType)) {
			if (entry.is_regular_file()) {
				const std::string fileName = entry.path().filename().string();

				if (fileName.size() >= 5 && fileName.substr(fileName.size() - 5) == std::string{ ".json" }) {
					std::ifstream file(entry.path());
					if (file.is_open()) {
						std::string fileContents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
						resultFileContents[fileName.data()] = { fileContents };
						file.close();
					} else {
						std::cerr << "Error opening file: " << fileName << std::endl;
						return false;
					}
				}
			}
		}
		std::cout << "Files found: " << resultFileContents.size() << std::endl;
	} catch (const std::exception& e) {
		std::cerr << "Error while processing files: " << e.what() << std::endl;
		return false;
	}

	return true;
}

class file_handler {
  public:
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
};

template<bool partialRead> static constexpr rt_ut::string_literal testTypePartial{ [] {
	if constexpr (partialRead) {
		return rt_ut::string_literal{ "Partial-Reading: Enabled, " };
	} else {
		return rt_ut::string_literal{ "Partial-Reading: Disabled, " };
	}
}() };

template<bool knownOrder> static constexpr rt_ut::string_literal testTypeKnownOrder{ [] {
	if constexpr (knownOrder) {
		return rt_ut::string_literal{ "Known-Order: Enabled, " };
	} else {
		return rt_ut::string_literal{ "Known-Order: Disabled, " };
	}
}() };

template<bool nullTerminated> static constexpr rt_ut::string_literal testTypeNullTerminated{ [] {
	if constexpr (nullTerminated) {
		return rt_ut::string_literal{ "Null-Terminated: Enabled" };
	} else {
		return rt_ut::string_literal{ "Null-Terminated: Disabled" };
	}
}() };

template<typename value_type_out, typename value_type_in, const auto& pass_tests, const auto& pass_values, bool partial, bool knownOrder, bool nullTerminated>
struct pass_tests_runner {
	template<uint64_t index> static void impl(jsonifier::jsonifier_core<>& parser) {
		value_type_in data{};
		rt_ut::unit_test<rt_ut::string_literal{ rt_ut::create_string_literal<pass_tests[index].size()>(pass_tests[index].data()) }, true>::assert_eq(pass_values[index], [&]() {
			parser.parseJson<jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder }>(data, pass_tests[index]);
			for (auto& value: parser.getErrors()) {
				std::cout << "Jsonifier Error: " << value << std::endl;
			}
			return static_cast<value_type_out>(data);
		});
	}
};

template<typename value_type_in, const auto& fail_tests, bool partial, bool knownOrder, bool nullTerminated> struct fail_tests_runner {
	template<uint64_t index> static void impl(jsonifier::jsonifier_core<>& parser) {
		value_type_in data{};
		rt_ut::unit_test<rt_ut::string_literal{ rt_ut::create_string_literal<fail_tests[index].size()>(fail_tests[index].data()) }, true>::assert_ne(true, [&]() {
			return parser.parseJson<jsonifier::parse_options{ .partialRead = partial, .knownOrder = knownOrder }>(data, fail_tests[index]);
		});
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
		jsonifier::jsonifier_core<> parser{};
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
		jsonifier::jsonifier_core<> parser{};
		(functor<value_type_in, fail_tests, partial, knownOrder, nullTerminated>::template impl<indices>(parser), ...);
	}
};
