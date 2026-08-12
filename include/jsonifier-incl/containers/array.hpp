// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/containers/array.hpp
#pragma once

#include <jsonifier-incl/core/config.hpp>
#include <jsonifier-incl/containers/iterator.hpp>

namespace jsonifier::internal {

	template<typename value_type_new, uint64_t sizeNew> struct alignas(64) array {
	  public:
		static_assert(std::is_object_v<value_type_new>, "The C++ Standard forbids containers of non-object types because of [container.requirements].");

		using value_type			 = value_type_new;
		using size_type				 = uint64_t;
		using difference_type		 = ptrdiff_t;
		using pointer				 = value_type*;
		using const_pointer			 = const value_type*;
		using reference				 = value_type&;
		using r_reference			 = value_type&&;
		using const_reference		 = const value_type&;
		using const_r_reference		 = const value_type&&;
		using iterator				 = sized_iterator<value_type, sizeNew>;
		using const_iterator		 = sized_iterator<const value_type, sizeNew>;
		using reverse_iterator		 = std::reverse_iterator<iterator>;
		using const_reverse_iterator = std::reverse_iterator<const_iterator>;

		JSONIFIER_INLINE constexpr void fill(const value_type& value) noexcept {
			std::fill_n(dataVal, sizeNew, value);
		}

		JSONIFIER_INLINE constexpr void swap(array& other) noexcept(std::is_nothrow_swappable<value_type>::value) {
			std::swap_ranges(dataVal, dataVal + sizeNew, other.dataVal);
		}

		JSONIFIER_INLINE constexpr iterator begin() noexcept JSONIFIER_LIFETIME_BOUND {
			return iterator{ dataVal };
		}

		JSONIFIER_INLINE constexpr iterator end() noexcept JSONIFIER_LIFETIME_BOUND {
			return iterator{ dataVal + sizeNew };
		}

		JSONIFIER_INLINE constexpr const_iterator begin() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_iterator{ dataVal };
		}

		JSONIFIER_INLINE constexpr const_iterator end() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_iterator{ dataVal + sizeNew };
		}

		JSONIFIER_INLINE constexpr const_iterator cbegin() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_iterator{ dataVal };
		}

		JSONIFIER_INLINE constexpr const_iterator cend() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_iterator{ dataVal + sizeNew };
		}

		JSONIFIER_INLINE constexpr reverse_iterator rbegin() noexcept JSONIFIER_LIFETIME_BOUND {
			return reverse_iterator{ end() };
		}

		JSONIFIER_INLINE constexpr reverse_iterator rend() noexcept JSONIFIER_LIFETIME_BOUND {
			return reverse_iterator{ begin() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rbegin() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_reverse_iterator{ end() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rend() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_reverse_iterator{ begin() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator crbegin() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_reverse_iterator{ cend() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator crend() const noexcept JSONIFIER_LIFETIME_BOUND {
			return const_reverse_iterator{ cbegin() };
		}

		JSONIFIER_INLINE constexpr reference front() noexcept JSONIFIER_LIFETIME_BOUND {
			return dataVal[0];
		}

		JSONIFIER_INLINE constexpr const_reference front() const noexcept JSONIFIER_LIFETIME_BOUND {
			return dataVal[0];
		}

		JSONIFIER_INLINE constexpr reference back() noexcept JSONIFIER_LIFETIME_BOUND {
			return dataVal[sizeNew - 1];
		}

		JSONIFIER_INLINE constexpr const_reference back() const noexcept JSONIFIER_LIFETIME_BOUND {
			return dataVal[sizeNew - 1];
		}

		JSONIFIER_INLINE constexpr size_type size() const noexcept {
			return sizeNew;
		}

		JSONIFIER_INLINE constexpr size_type maxSize() const noexcept {
			return sizeNew;
		}

		JSONIFIER_INLINE constexpr bool empty() const noexcept {
			return false;
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr reference at(index_type position) noexcept(false) JSONIFIER_LIFETIME_BOUND {
			if (sizeNew <= position) {
				throw std::runtime_error{ "invalid array<T, N> subscript" };
			}

			return dataVal[position];
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr const_reference at(index_type position) const
			noexcept(false) JSONIFIER_LIFETIME_BOUND {
			if (sizeNew <= position) {
				throw std::runtime_error{ "invalid array<T, N> subscript" };
			}

			return dataVal[position];
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr const_r_reference operator[](index_type position) const&& noexcept {
			return static_cast<const_r_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr const_reference operator[](index_type position) const& noexcept {
			return static_cast<const_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr r_reference operator[](index_type position) && noexcept {
			return static_cast<r_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::indexable_types<size_type> index_type> JSONIFIER_INLINE constexpr reference operator[](index_type position) & noexcept {
			return static_cast<reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::integral_constant_types tag_type> JSONIFIER_INLINE constexpr const_r_reference operator[](tag_type position) const&& noexcept {
			static_assert(static_cast<uint64_t>(position) < sizeNew, "Sorry, but that index is out of bounds!");
			return static_cast<const_r_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::integral_constant_types tag_type> JSONIFIER_INLINE constexpr const_reference operator[](tag_type position) const& noexcept {
			static_assert(static_cast<uint64_t>(position) < sizeNew, "Sorry, but that index is out of bounds!");
			return static_cast<const_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::integral_constant_types tag_type> JSONIFIER_INLINE constexpr r_reference operator[](tag_type position) && noexcept {
			static_assert(static_cast<uint64_t>(position) < sizeNew, "Sorry, but that index is out of bounds!");
			return static_cast<r_reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		template<concepts::integral_constant_types tag_type> JSONIFIER_INLINE constexpr reference operator[](tag_type position) & noexcept {
			static_assert(static_cast<uint64_t>(position) < sizeNew, "Sorry, but that index is out of bounds!");
			return static_cast<reference>(dataVal[static_cast<uint64_t>(position)]);
		}

		JSONIFIER_INLINE constexpr pointer data() noexcept {
			return dataVal;
		}

		JSONIFIER_INLINE constexpr const_pointer data() const noexcept {
			return dataVal;
		}

		JSONIFIER_INLINE constexpr friend bool operator==(const array& lhs, const array& rhs) noexcept {
			for (uint64_t x = 0; x < sizeNew; ++x) {
				if (lhs[x] != rhs[x]) {
					return false;
				}
			}
			return true;
		}

		JSONIFIER_INLINE constexpr friend bool operator!=(const array& lhs, const array& rhs) noexcept {
			return !(lhs == rhs);
		}

		alignas(64) value_type dataVal[sizeNew];
	};

	template<typename first, typename... rest> array(first, rest...) -> array<concepts::base_t<first>, 1 + sizeof...(rest)>;

	template<typename first, uint64_t size> array(array<first, size>) -> array<concepts::base_t<first>, size>;

	template<typename value_type_new> struct alignas(64) array<value_type_new, 0ULL> {
	  public:
		using value_type			 = value_type_new;
		using size_type				 = uint64_t;
		using difference_type		 = ptrdiff_t;
		using pointer				 = value_type*;
		using const_pointer			 = const value_type*;
		using reference				 = value_type&;
		using const_reference		 = const value_type&;
		using iterator				 = sized_iterator<value_type, 0ULL>;
		using const_iterator		 = sized_iterator<const value_type, 0ULL>;
		using reverse_iterator		 = std::reverse_iterator<iterator>;
		using const_reverse_iterator = std::reverse_iterator<const_iterator>;

		JSONIFIER_INLINE constexpr void fill(const value_type&) noexcept {
		}

		JSONIFIER_INLINE constexpr void swap(array&) noexcept {
		}

		JSONIFIER_INLINE constexpr iterator begin() noexcept {
			return iterator{};
		}

		JSONIFIER_INLINE constexpr iterator end() noexcept {
			return iterator{};
		}

		JSONIFIER_INLINE constexpr const_iterator begin() const noexcept {
			return const_iterator{};
		}

		JSONIFIER_INLINE constexpr const_iterator end() const noexcept {
			return const_iterator{};
		}

		JSONIFIER_INLINE constexpr const_iterator cbegin() const noexcept {
			return const_iterator{};
		}

		JSONIFIER_INLINE constexpr const_iterator cend() const noexcept {
			return const_iterator{};
		}

		JSONIFIER_INLINE constexpr reverse_iterator rbegin() noexcept {
			return reverse_iterator{ end() };
		}

		JSONIFIER_INLINE constexpr reverse_iterator rend() noexcept {
			return reverse_iterator{ begin() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rbegin() const noexcept {
			return const_reverse_iterator{ end() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator rend() const noexcept {
			return const_reverse_iterator{ begin() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator crbegin() const noexcept {
			return const_reverse_iterator{ cend() };
		}

		JSONIFIER_INLINE constexpr const_reverse_iterator crend() const noexcept {
			return const_reverse_iterator{ cbegin() };
		}

		template<typename index_type> JSONIFIER_INLINE constexpr reference at(index_type) noexcept(false) {
			throw std::runtime_error{ "invalid array<T, 0> subscript" };
		}

		template<typename index_type> JSONIFIER_INLINE constexpr const_reference at(index_type) const noexcept(false) {
			throw std::runtime_error{ "invalid array<T, 0> subscript" };
		}

		JSONIFIER_INLINE constexpr size_type size() const noexcept {
			return 0;
		}

		JSONIFIER_INLINE constexpr size_type maxSize() const noexcept {
			return 0;
		}

		JSONIFIER_INLINE constexpr bool empty() const noexcept {
			return true;
		}

		JSONIFIER_INLINE constexpr pointer data() noexcept {
			return nullptr;
		}

		JSONIFIER_INLINE constexpr const_pointer data() const noexcept {
			return nullptr;
		}

		JSONIFIER_INLINE constexpr friend bool operator==(const array&, const array&) noexcept {
			return true;
		}

		JSONIFIER_INLINE constexpr friend bool operator!=(const array&, const array&) noexcept {
			return false;
		}

	};

}
