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
#include <jsonifier-incl/utilities/concepts.hpp>

namespace jsonifier::internal {

	template<typename value_type_new, uint64_t size> struct sized_iterator {
		using iterator_concept	= std::contiguous_iterator_tag;
		using iterator_category = std::random_access_iterator_tag;
		using element_type		= value_type_new;
		using value_type		= value_type_new;
		using difference_type	= ptrdiff_t;
		using pointer			= value_type*;
		using reference			= value_type&;

		JSONIFIER_INLINE constexpr sized_iterator() noexcept {
		}

		JSONIFIER_INLINE constexpr sized_iterator(pointer ptrNew) noexcept : ptr(ptrNew) {
		}

		JSONIFIER_INLINE constexpr reference operator*() const noexcept {
			return *ptr;
		}

		JSONIFIER_INLINE constexpr pointer operator->() const noexcept {
			return ptr;
		}

		JSONIFIER_INLINE constexpr sized_iterator& operator++() noexcept {
			++ptr;
			return *this;
		}

		JSONIFIER_INLINE constexpr sized_iterator operator++(int32_t) noexcept {
			sized_iterator temp = *this;
			++*this;
			return temp;
		}

		JSONIFIER_INLINE constexpr sized_iterator& operator--() noexcept {
			--ptr;
			return *this;
		}

		JSONIFIER_INLINE constexpr sized_iterator operator--(int32_t) noexcept {
			sized_iterator temp = *this;
			--*this;
			return temp;
		}

		JSONIFIER_INLINE constexpr sized_iterator& operator+=(const difference_type offSet) noexcept {
			ptr += offSet;
			return *this;
		}

		JSONIFIER_INLINE constexpr sized_iterator operator+(const difference_type offSet) const noexcept {
			sized_iterator temp = *this;
			temp += offSet;
			return temp;
		}

		JSONIFIER_INLINE friend constexpr sized_iterator operator+(const difference_type offSet, sized_iterator next) noexcept {
			next += offSet;
			return next;
		}

		JSONIFIER_INLINE constexpr sized_iterator& operator-=(const difference_type offSet) noexcept {
			return *this += -offSet;
		}

		JSONIFIER_INLINE constexpr sized_iterator operator-(const difference_type offSet) const noexcept {
			sized_iterator temp = *this;
			temp -= offSet;
			return temp;
		}

		JSONIFIER_INLINE constexpr difference_type operator-(const sized_iterator& other) const noexcept {
			return ptr - other.ptr;
		}

		JSONIFIER_INLINE constexpr reference operator[](const difference_type offSet) const noexcept {
			return *(*this + offSet);
		}

		JSONIFIER_INLINE constexpr bool operator==(const sized_iterator& other) const noexcept {
			return ptr == other.ptr;
		}

		JSONIFIER_INLINE constexpr bool operator!=(const sized_iterator& other) const noexcept {
			return !(*this == other);
		}

		JSONIFIER_INLINE constexpr bool operator<(const sized_iterator& other) const noexcept {
			return ptr < other.ptr;
		}

		JSONIFIER_INLINE constexpr bool operator>(const sized_iterator& other) const noexcept {
			return other < *this;
		}

		JSONIFIER_INLINE constexpr bool operator<=(const sized_iterator& other) const noexcept {
			return !(other < *this);
		}

		JSONIFIER_INLINE constexpr bool operator>=(const sized_iterator& other) const noexcept {
			return !(*this < other);
		}

	  protected:
		pointer ptr;
	};

	template<typename value_type_new> struct sized_iterator<value_type_new, 0ULL> {
		using iterator_concept	= std::contiguous_iterator_tag;
		using iterator_category = std::random_access_iterator_tag;
		using element_type		= value_type_new;
		using value_type		= value_type_new;
		using difference_type	= ptrdiff_t;
		using pointer			= value_type*;
		using reference			= value_type&;

		JSONIFIER_INLINE constexpr sized_iterator() noexcept {
		}

		JSONIFIER_INLINE constexpr sized_iterator(std::nullptr_t) noexcept {
		}

		JSONIFIER_INLINE constexpr bool operator==(const sized_iterator&) const noexcept {
			return true;
		}

		JSONIFIER_INLINE constexpr bool operator!=(const sized_iterator&) const noexcept {
			return false;
		}

		JSONIFIER_INLINE constexpr bool operator<(const sized_iterator&) const noexcept {
			return false;
		}

		JSONIFIER_INLINE constexpr bool operator>(const sized_iterator&) const noexcept {
			return false;
		}

		JSONIFIER_INLINE constexpr bool operator<=(const sized_iterator& other) const noexcept {
			return !(other < *this);
		}

		JSONIFIER_INLINE constexpr bool operator>=(const sized_iterator& other) const noexcept {
			return !(*this < other);
		}
	};

	template<typename value_type> using basic_iterator = sized_iterator<value_type, 1ULL>;

}// namespace internal
