/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier
#pragma once

#include "common.hpp"

namespace string_validation_tests {

	struct unit_test_string {
		constexpr unit_test_string() noexcept = default;

		template<uint64_t N> constexpr unit_test_string(const char (&string)[N]) : size_val(N - 1) {
			static_assert(N <= 256, "unit_test_string: input exceeds buffer capacity");
			std::copy_n(string, N - 1, values);
		}

		constexpr bool operator==(const unit_test_string& other) const {
			if (size_val != other.size_val) {
				return false;
			}
			for (uint64_t x = 0; x < size_val; ++x) {
				if (values[x] != other.values[x]) {
					return false;
				}
			}
			return true;
		}

		template<typename string_type> constexpr bool operator==(const string_type& other) const {
			if (size_val != other.size()) {
				return false;
			}
			for (uint64_t x = 0; x < size_val; ++x) {
				if (values[x] != other[x]) {
					return false;
				}
			}
			return true;
		}

		template<typename string_type> constexpr friend bool operator==(const string_type& lhs, const unit_test_string& rhs) {
			if (rhs.size_val != lhs.size()) {
				return false;
			}
			for (uint64_t x = 0; x < rhs.size_val; ++x) {
				if (rhs.values[x] != lhs[x]) {
					return false;
				}
			}
			return true;
		}

		constexpr size_t size() const {
			return size_val;
		}

		constexpr jsonifier::string_view_ptr data() const {
			return values;
		}

		constexpr operator std::string_view() const {
			return { values, size_val };
		}

		inline friend std::ostream& operator<<(std::ostream& os, const unit_test_string& string) {
			os.write(string.data(), static_cast<std::streamsize>(string.size()));
			return os;
		}

		size_t size_val{};
		char values[256]{};
	};

	constexpr jsonifier::internal::array<unit_test_string, 35> inputValues{ { "\"\"", "\"Hello\"", "\"Hello\\nWorld\"", "\"Hello\\u0000World\"", "\"\\\"\\\\/\\b\\f\\n\\r\\t\"",
		"\"\\u0024\"", "\"\\u00A2\"", "\"\\u20AC\"", "\"\\uD834\\uDD1E\"", "\" \"", "\"    \"", "\"\\u0001\"", "\"\\u001F\"", "\"\\u007F\"", "\"\\u0080\"", "\"\\u00FF\"",
		"\"\\u0100\"", "\"\\uFFFF\"", "\"😀\"", "\"🚀\"", "\"🍺\"", "\"日本語\"", "\"Русский\"", "\"العربية\"", "\"👨‍👩‍👧‍👦\"", "\"🏳️‍🌈\"",
		"\"a😀b😀c😀d\"", "\"\\\\\"", "\"\\\\\\\\\\\\\\\\\\\\\\\\\"", "\"\\/\"", "\"\\uD800\\uDC00\"", "\"\\uDBFF\\uDFFF\"", "\"\\u0000\\u0000\\u0000\\u0000\"",
		"\"\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\"",
		"\"Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua\"" } };

	constexpr jsonifier::internal::array<unit_test_string, 35> outputValues{ { "", "Hello", "Hello\nWorld", "Hello\0World", "\"\\/\b\f\n\r\t", "\x24", "\xC2\xA2", "\xE2\x82\xAC",
		"\xF0\x9D\x84\x9E", " ", "    ", "\x01", "\x1F", "\x7F", "\xC2\x80", "\xC3\xBF", "\xC4\x80", "\xEF\xBF\xBF", "😀", "🚀", "🍺", "日本語", "Русский", "العربية",
		"👨‍👩‍👧‍👦", "🏳️‍🌈", "a😀b😀c😀d", "\\", "\\\\\\\\\\\\", "/", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF", "\0\0\0\0", "\\\\\\\\\\\\\\\\\\\\",
		"Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua" } };

	constexpr jsonifier::internal::array<unit_test_string, 26> failValues{ { "\"\\u\"", "\"\\u0\"", "\"\\u00\"", "\"\\u000\"", "\"\\uZZZZ\"", "\"\\x41\"", "\"\\x00\"", "\"\\a\"",
		"\"\\v\"", "\"\\q\"", "\"\\8\"", "\"\\9\"", "\"\\017\"", "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\uD800\"", "\"\\uDC00\\uDC00\"", "\"\\uD800A\"", "\"\\uD800\\uFFFF\"",
		"\"\\uFFFF\\uD800\"", "\"\\uDBFF\"", "\"\\uD800\\uE000\"", "\"\\uDBFF\\uE000\"", "\"\\uD7FF\\uDC00\"", "\"abc", "\"abc\\" } };

	template<bool partial, bool knownOrder, bool nullTerminated> inline static void stringTestsImpl() {
		std::cout << "String Pass Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		pass_test_runner<std::string_view, std::string, inputValues, outputValues, partial, knownOrder, nullTerminated, pass_tests_runner,
			jsonifier::internal::make_integer_sequence<inputValues.size()>>::impl();
		std::cout << "String Fail Tests, " << testTypePartial<partial> << testTypeKnownOrder<knownOrder> << testTypeNullTerminated<nullTerminated> << ": " << std::endl;
		fail_test_runner<std::string, failValues, partial, knownOrder, nullTerminated, fail_tests_runner, jsonifier::internal::make_integer_sequence<failValues.size()>>::impl();
	}

	inline static void runTests() {
		stringTestsImpl<false, false, false>();
		stringTestsImpl<false, true, false>();
		stringTestsImpl<true, false, false>();
		stringTestsImpl<true, true, false>();
		stringTestsImpl<false, false, true>();
		stringTestsImpl<false, true, true>();
		stringTestsImpl<true, false, true>();
		stringTestsImpl<true, true, true>();
	}

}
