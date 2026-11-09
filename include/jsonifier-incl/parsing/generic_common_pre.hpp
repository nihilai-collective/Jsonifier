/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_common_pre.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/parser.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::generic {

	enum class error_code : uint8_t {
		success,
		empty,
		tape_error,
		unclosed_container,
		trailing_content,
		incorrect_type,
		no_such_field,
		index_out_of_bounds,
		number_error,
		string_error,
		t_atom_error,
		f_atom_error,
		n_atom_error,
		invalid_json_pointer,
		capacity,
	};

	template<typename value_type> class alloc_buffer : internal::alloc_wrapper<value_type> {
	  public:
		using allocator = internal::alloc_wrapper<value_type>;

		JSONIFIER_INLINE alloc_buffer& operator=(alloc_buffer&& other) noexcept {
			if (this != &other) [[likely]] {
				release();
				ptr		 = std::exchange(other.ptr, nullptr);
				capacity = std::exchange(other.capacity, uint64_t{});
			}
			return *this;
		}

		void reserve(uint64_t capacityNew) noexcept {
			if (capacityNew > capacity) {
				release();
				ptr		 = allocator::allocate(capacityNew);
				capacity = capacityNew;
			}
		}

		JSONIFIER_INLINE alloc_buffer(alloc_buffer&& other) noexcept : capacity{ std::exchange(other.capacity, uint64_t{}) }, ptr{ std::exchange(other.ptr, nullptr) } {
		}

		JSONIFIER_INLINE explicit alloc_buffer(uint64_t capacityNew) noexcept {
			reserve(capacityNew);
		}

		JSONIFIER_INLINE uint64_t size() const noexcept {
			return capacity;
		}

		JSONIFIER_INLINE value_type* data() const noexcept {
			return ptr;
		}

		JSONIFIER_INLINE ~alloc_buffer() noexcept {
			release();
		}

		alloc_buffer& operator=(const alloc_buffer&) = delete;
		JSONIFIER_INLINE alloc_buffer() noexcept	 = default;
		alloc_buffer(const alloc_buffer&)			 = delete;

	  protected:
		JSONIFIER_INLINE void release() noexcept {
			if (ptr) {
				allocator::deallocate(ptr, capacity);
				ptr		 = nullptr;
				capacity = 0;
			}
		}

		uint64_t capacity{};
		value_type* ptr{};
	};

	struct string_arena {
		JSONIFIER_INLINE void reset(uint64_t documentLength) noexcept {
			buffer.reserve(documentLength * 2 + 128);
			slots	   = buffer.data();
			scratchPtr = buffer.data() + documentLength + 64;
		}

		JSONIFIER_INLINE char* slot(uint64_t offset) const noexcept {
			return slots + offset;
		}

		JSONIFIER_INLINE char* scratch() const noexcept {
			return scratchPtr;
		}

		JSONIFIER_INLINE string_arena& operator=(string_arena&& other) noexcept {
			if (this != &other) [[likely]] {
				buffer	   = internal::move(other.buffer);
				scratchPtr = std::exchange(other.scratchPtr, nullptr);
				slots	   = std::exchange(other.slots, nullptr);
			}
			return *this;
		}

		JSONIFIER_INLINE string_arena(string_arena&& other) noexcept
			: buffer{ internal::move(other.buffer) }, scratchPtr{ std::exchange(other.scratchPtr, nullptr) }, slots{ std::exchange(other.slots, nullptr) } {
		}

		string_arena& operator=(const string_arena&) = delete;
		string_arena(const string_arena&)			 = delete;
		string_arena() noexcept						 = default;

	  protected:
		alloc_buffer<char> buffer{};
		char* scratchPtr{};
		char* slots{};
	};

	struct utf8_span_step {
		template<uint64_t index> JSONIFIER_INLINE static internal::unrestrict_t<read_buffer_ptr> impl(internal::unrestrict_t<read_buffer_ptr>& current, read_buffer_ptr end,
			internal::utf8_validation_state& state) noexcept {
			using simd_list_local					 = internal::type_list_element_t<index, internal::simd::avx_integer_list>;
			static constexpr auto simdType			 = simd_list_local::type::simdType;
			using simd_type_local					 = typename internal::simd_type_wrapper<simdType>::type;
			static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
			internal::utf8_register_validator<internal::simd_type_wrapper<simdType>> validator{ state };
			while (static_cast<uint64_t>(end - current) >= bytesProcessed) {
				validator.checkRegister(internal::simd::gatherValuesU<simd_type_local>(current));
				current += bytesProcessed;
			}
			if constexpr (index == internal::listSize - 1) {
				validator.checkPartial(current, static_cast<uint64_t>(end - current));
				current = end;
				return validator.errors() ? nullptr : current;
			} else {
				validator.flush();
				return current;
			}
		}
	};

	struct key_matcher {
		template<bool minified, typename state_type, typename key_type> JSONIFIER_INLINE static bool impl(state_type&& state, key_type&& key, uint32_t keyIndex) noexcept {
			return state->template keyMatches<minified>(state->tape + keyIndex, key);
		}
	};

	template<bool minified> class generic_object;
	template<bool minified> class generic_array;
	template<bool minified> class generic_field;
	struct document_state;
	class field_index_map;

	struct plain_string_copy_context {
		JSONIFIER_INLINE uint64_t operator()(char* __restrict ptrNew, uint64_t length) const noexcept {
			jsonifier::internal::jsonifierMemcpy(ptrNew, source, length);
			return length;
		}

		JSONIFIER_INLINE plain_string_copy_context(read_buffer_ptr sourceNew) noexcept : source{ sourceNew } {
		}

		plain_string_copy_context& operator=(const plain_string_copy_context&) noexcept = delete;
		plain_string_copy_context& operator=(plain_string_copy_context&&) noexcept		= delete;
		plain_string_copy_context(const plain_string_copy_context&) noexcept			= delete;
		plain_string_copy_context(plain_string_copy_context&&) noexcept					= delete;
		plain_string_copy_context() noexcept											= delete;

		read_buffer_ptr source{};
	};

}
