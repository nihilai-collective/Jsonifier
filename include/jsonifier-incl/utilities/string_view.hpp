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
/// Feb 20, 2023
#pragma once

#include <jsonifier-incl/utilities/string.hpp>

namespace jsonifier {

class string_view_base {
	  public:
		using value_type			 = char;
		using const_pointer			 = const value_type*;
		using const_reference		 = const value_type&;
		using iterator				 = internal::basic_iterator<value_type>;
		using const_iterator		 = internal::basic_iterator<const value_type>;
		using difference_type		 = std::ptrdiff_t;
		using const_reverse_iterator = std::reverse_iterator<const_iterator>;
		using size_type				 = uint64_t;
		using traits_type			 = std::char_traits<value_type>;

		static constexpr size_type npos{ std::numeric_limits<size_type>::max() };

		JSONIFIER_INLINE constexpr string_view_base() noexcept : dataVal(), sizeVal(0) {
		}

		template<uint64_t size> JSONIFIER_INLINE constexpr string_view_base& operator=(const string_base<size>& stringNew) noexcept {
			dataVal = stringNew.data();
			sizeVal = stringNew.size();
			return *this;
		}

		template<uint64_t size> JSONIFIER_INLINE constexpr string_view_base(const string_base<size>& stringNew) noexcept {
			*this = stringNew;
		}

		template<concepts::string_t value_type_newer> JSONIFIER_INLINE constexpr string_view_base& operator=(value_type_newer&& stringNew) noexcept {
			dataVal = stringNew.data();
			sizeVal = stringNew.size();
			return *this;
		}

		template<concepts::string_t value_type_newer> JSONIFIER_INLINE constexpr string_view_base(value_type_newer&& stringNew) noexcept {
			*this = stringNew;
		}

		template<typename value_type_newer, concepts::same_character_size<value_type>>
		JSONIFIER_INLINE constexpr string_view_base& operator=(const value_type_newer& stringNew) noexcept {
			dataVal = stringNew.data();
			sizeVal = stringNew.size();
			return *this;
		}

		template<typename value_type_newer, concepts::same_character_size<value_type>> JSONIFIER_INLINE constexpr string_view_base(const value_type_newer& stringNew) noexcept {
			*this = stringNew;
		}

		JSONIFIER_INLINE constexpr string_view_base(const_pointer pointerNew, const size_type countNew) noexcept : dataVal(pointerNew), sizeVal(countNew) {
		}

		JSONIFIER_INLINE constexpr string_view_base(const_pointer pointerNew) noexcept : dataVal(pointerNew), sizeVal(std::char_traits<value_type>::length(pointerNew)) {
		}

		JSONIFIER_INLINE constexpr const_iterator begin() const noexcept {
			return const_iterator{ dataVal };
		}

		JSONIFIER_INLINE constexpr const_iterator end() const noexcept {
			return const_iterator{ dataVal + sizeVal };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rbegin() const noexcept {
			return const_reverse_iterator{ end() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rend() const noexcept {
			return const_reverse_iterator{ begin() };
		}

		JSONIFIER_INLINE constexpr size_type size() const noexcept {
			return sizeVal;
		}

		JSONIFIER_INLINE constexpr size_type length() const noexcept {
			return sizeVal;
		}

		JSONIFIER_INLINE constexpr bool empty() const noexcept {
			return sizeVal == 0;
		}

		JSONIFIER_INLINE constexpr const_pointer data() const noexcept {
			return dataVal;
		}

		JSONIFIER_INLINE constexpr size_type maxSize() const noexcept {
			return internal::min(static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()), static_cast<uint64_t>(-1) / sizeof(value_type));
		}

		JSONIFIER_INLINE constexpr const_reference at(const size_type offsetNew) const noexcept(false) {
			if (offsetNew >= sizeVal) [[unlikely]] {
				throw std::out_of_range{ "Sorry, but that index is beyond the end of this string_view instance." };
			}
			return dataVal[offsetNew];
		}

		JSONIFIER_INLINE constexpr const_reference operator[](const size_type offsetNew) const noexcept {
			return dataVal[offsetNew];
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type rfind(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().rfind(std::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type find(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().find(std::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type findFirstOf(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().find_first_of(std::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type findLastOf(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().find_last_of(std::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type findFirstNotOf(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().find_first_not_of(std::forward<arg_types>(args)...);
		}

		template<typename... arg_types> JSONIFIER_INLINE constexpr size_type findLastNotOf(arg_types&&... args) const noexcept {
			return this->operator std::basic_string_view<value_type>().find_last_not_of(std::forward<arg_types>(args)...);
		}

		JSONIFIER_INLINE constexpr void swap(string_view_base& other) noexcept {
			std::swap(dataVal, other.dataVal);
			std::swap(sizeVal, other.sizeVal);
		}

		JSONIFIER_INLINE constexpr string_view_base substr(const size_type offsetNew = 0, size_type countNew = npos) const {
			if (offsetNew > sizeVal) [[unlikely]] {
				throw std::out_of_range("Substring position is out of range.");
			}

			countNew = internal::min(countNew, sizeVal - offsetNew);
			return string_view_base(dataVal + offsetNew, countNew);
		}

		template<uint64_t size> JSONIFIER_INLINE constexpr explicit operator string_base<size>() const noexcept {
			string_base<size> returnValue{};
			returnValue.resize(sizeVal);
			if (sizeVal > 0 && dataVal) [[likely]] {
				std::memcpy(returnValue.data(), data(), returnValue.size());
			}
			return returnValue;
		}

		template<typename value_type_newer = value_type> JSONIFIER_INLINE constexpr explicit operator std::basic_string<value_type_newer>() const noexcept {
			std::basic_string<value_type_newer> returnValue{};
			returnValue.resize(sizeVal);
			if (sizeVal > 0 && dataVal) [[likely]] {
				std::memcpy(returnValue.data(), data(), returnValue.size());
			}
			return returnValue;
		}

		template<typename value_type_newer = value_type> JSONIFIER_INLINE constexpr explicit operator std::basic_string_view<value_type_newer>() const noexcept {
			return { data(), size() };
		}

		template<uint64_t size> JSONIFIER_INLINE friend bool operator==(const string_view_base& lhs, const char (&rhs)[size]) noexcept {
			auto rhsLength = traits_type::length(rhs);
			return rhsLength == lhs.size() && internal::comparison::compare(lhs.data(), rhs, rhsLength);
		}

		template<concepts::string_t value_type_newer> JSONIFIER_INLINE friend bool operator==(const string_view_base& lhs, const value_type_newer& rhs) noexcept {
			if (lhs.size() == rhs.size()) {
				if (lhs.size() > 0) {
					return internal::comparison::compare(lhs.data(), rhs.data(), rhs.size());
				}
				return true;
			} else {
				return false;
			}
		}

		const_pointer dataVal{};
		size_type sizeVal{};
	};

	using string_view = string_view_base;

	JSONIFIER_INLINE static std::ostream& operator<<(std::ostream& os, const string_view& input) noexcept {
		os.write(input.data(), static_cast<std::streamsize>(input.size()));
		return os;
	}

	JSONIFIER_INLINE constexpr string_view operator""_sv(string_view_ptr stringNew, size_t lengthNew) noexcept {
		return string_view(stringNew, lengthNew);
	}

}// namespace jsonifier
