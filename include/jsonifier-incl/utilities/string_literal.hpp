/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier
#pragma once

#include <jsonifier-incl/core/config.hpp>

namespace jsonifier::internal {

	template<uint64_t sizeVal> struct string_literal {
		using value_type	  = char;
		using const_reference = const value_type&;
		using reference		  = value_type&;
		using const_pointer	  = const value_type*;
		using pointer		  = value_type*;
		using size_type		  = uint64_t;

		static constexpr size_type length{ sizeVal - 1 };

		JSONIFIER_INLINE constexpr string_literal() noexcept = default;

		JSONIFIER_INLINE constexpr string_literal(const char (&str)[sizeVal]) noexcept {
			for (uint64_t x = 0; x < length; ++x) {
				values[x] = str[x];
			}
			values[length] = '\0';
		}

		JSONIFIER_INLINE constexpr const_pointer data() const noexcept JSONIFIER_LIFETIME_BOUND {
			return values;
		}

		JSONIFIER_INLINE constexpr pointer data() noexcept JSONIFIER_LIFETIME_BOUND {
			return values;
		}

		template<size_type sizeNew> JSONIFIER_INLINE constexpr string_literal<sizeNew + sizeVal - 1> operator+(const string_literal<sizeNew>& str) const noexcept {
			string_literal<sizeNew + sizeVal - 1> newLiteral{};
			std::copy(values, values + size(), newLiteral.data());
			std::copy(str.data(), str.data() + sizeNew, newLiteral.data() + size());
			return newLiteral;
		}

		template<size_type sizeNew> JSONIFIER_INLINE constexpr string_literal<sizeNew + sizeVal - 1> operator+(const value_type (&str)[sizeNew]) const noexcept {
			string_literal<sizeNew + sizeVal - 1> newLiteral{};
			std::copy(values, values + size(), newLiteral.data());
			std::copy(str, str + sizeNew, newLiteral.data() + size());
			return newLiteral;
		}

		template<size_type sizeNew> JSONIFIER_INLINE friend constexpr auto operator+(const value_type (&lhs)[sizeNew], const string_literal<sizeVal>& str) noexcept {
			return string_literal<sizeNew>{ lhs } + str;
		}

		JSONIFIER_INLINE constexpr reference operator[](size_type index) noexcept JSONIFIER_LIFETIME_BOUND {
			return values[index];
		}

		JSONIFIER_INLINE constexpr const_reference operator[](size_type index) const noexcept JSONIFIER_LIFETIME_BOUND {
			return values[index];
		}

		JSONIFIER_INLINE static constexpr size_type size() noexcept {
			return length;
		}

		template<typename string_type> JSONIFIER_INLINE constexpr operator string_type() const noexcept {
			alignas(64) string_type returnValues{ values, length };
			return returnValues;
		}

		alignas(64) value_type values[sizeVal]{};
	};

	template<uint64_t sizeVal> string_literal(const char (&)[sizeVal]) -> string_literal<sizeVal>;

	template<uint64_t size> std::ostream& operator<<(std::ostream& os, const string_literal<size>& input) noexcept {
		os.write(input.data(), static_cast<std::streamsize>(input.size()));
		return os;
	}

	template<string_literal literal> constexpr uint64_t escapedKeyLength() noexcept {
		uint64_t newLength{};
		for (uint64_t x = 0; x < literal.size(); ++x) {
			const char c = literal[x];
			newLength += (c == '"' || c == '\\') ? 2 : 1;
		}
		return newLength;
	}

	template<string_literal literal> constexpr string_literal<escapedKeyLength<literal>() + 1> escapeKeyLiteral() noexcept {
		constexpr uint64_t newLength{ escapedKeyLength<literal>() };
		string_literal<newLength + 1> returnValues{};
		uint64_t outIndex{};
		for (uint64_t x = 0; x < literal.size(); ++x) {
			const char c = literal[x];
			if (c == '"' || c == '\\') {
				returnValues[outIndex] = '\\';
				++outIndex;
			}
			returnValues[outIndex] = c;
			++outIndex;
		}
		returnValues[newLength] = '\0';
		return returnValues;
	}

	template<string_literal literal> static constexpr auto escapedKeyLiteral{ escapeKeyLiteral<literal>() };

}
