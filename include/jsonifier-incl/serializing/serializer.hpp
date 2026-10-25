/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/serializer.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/error.hpp>

namespace jsonifier::internal {

	template<typename value_type, serialize_options optionsNew> struct serialize_impl;

	template<typename value_type, serialize_options optionsNew> struct get_size_impl;

	template<serialize_options options> struct get_size {
		template<typename value_type_new> JSONIFIER_INLINE static uint64_t impl(value_type_new& __restrict value, uint64_t& __restrict indent) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			return get_size_impl<value_type, options>::impl(value, indent);
		}
	};

	template<serialize_options options> struct serialize {
		template<typename value_type_new>
		JSONIFIER_INLINE static unrestrict_t<write_buffer_ptr> impl(value_type_new&& __restrict value, write_buffer_ptr bufferPtr, uint64_t indent) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			return serialize_impl<value_type, options>::impl(internal::forward<value_type_new>(value), bufferPtr, indent);
		}
	};

	template<serialize_options options, typename value_type> struct serialize_writer_ro {
		JSONIFIER_INLINE uint64_t operator()(char* ptrNew, uint64_t) noexcept {
			const write_buffer_ptr start = std::bit_cast<write_buffer_ptr>(ptrNew);
			return static_cast<uint64_t>(serialize<options>::impl(object, start, 0) - start);
		}

		JSONIFIER_INLINE serialize_writer_ro(value_type& __restrict objectNew) noexcept : object{ objectNew } {
		}

		serialize_writer_ro& operator=(const serialize_writer_ro&) noexcept = delete;
		serialize_writer_ro& operator=(serialize_writer_ro&&) noexcept		= delete;
		serialize_writer_ro(const serialize_writer_ro&) noexcept			= delete;
		serialize_writer_ro(serialize_writer_ro&&) noexcept					= delete;

		value_type& __restrict object;
	};

	template<uint64_t maxBytes> struct serialize_copier_ro {
		JSONIFIER_INLINE uint64_t operator()(char* ptrNew, uint64_t) noexcept {
			jsonifierMemcpyUpTo<maxBytes>(ptrNew, source, length);
			return length;
		}

		JSONIFIER_INLINE serialize_copier_ro(const uint8_t* sourceNew, uint64_t lengthNew) noexcept : source{ sourceNew }, length{ lengthNew } {
		}

		serialize_copier_ro& operator=(const serialize_copier_ro&) noexcept = delete;
		serialize_copier_ro& operator=(serialize_copier_ro&&) noexcept		= delete;
		serialize_copier_ro(const serialize_copier_ro&) noexcept			= delete;
		serialize_copier_ro(serialize_copier_ro&&) noexcept					= delete;

		const uint8_t* source;
		uint64_t length;
	};

	template<typename derived_type_new> struct serializer {
		using derived_type = derived_type_new;

		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like_types buffer_type>
		bool serializeJson(value_type&& object, buffer_type&& buffer) {
			static constexpr serialize_options options{ optionsNew };
			uint64_t sizeIndent{};
			const uint64_t newSize = get_size<options>::impl(object, sizeIndent) + 64ull;
			if constexpr (resize_and_overwriteable_types<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				const auto bufferPtr = serialize<options>::impl(object, buffer.data(), 0);
				buffer.resize(static_cast<uint64_t>(bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, number_types value_type, buffer_like_types buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) {
			static constexpr serialize_options options{ optionsNew };
			static constexpr uint64_t newSize{ digit_sizes<value_type>::value };
#if JSONIFIER_COMPILER_MSVC
			if (buffer.capacity() < newSize) {
				alignas(64) uint8_t scratch[newSize];
				const uint64_t length = static_cast<uint64_t>(serialize<options>::impl(object, scratch, 0) - scratch);
				if constexpr (resize_and_overwriteable_types<remove_cvref_t<buffer_type>>) {
					const uint64_t requestSize = length > buffer.capacity() ? newSize : length;
					buffer.resize_and_overwrite(requestSize, serialize_copier_ro<newSize>{ scratch, length });
				} else {
					buffer.resize(length);
					jsonifierMemcpyUpTo<newSize>(buffer.data(), scratch, length);
				}
				return true;
			}
#endif
			if constexpr (resize_and_overwriteable_types<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				const auto bufferPtr = serialize<options>::impl(object, buffer.data(), 0);
				buffer.resize(static_cast<uint64_t>(bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, string_types value_type, buffer_like_types buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) {
			static constexpr serialize_options options{ optionsNew };
			const auto newSize = object.size() * 6ull + 2ull + simdBytesPerRegister;
			if constexpr (resize_and_overwriteable_types<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				const auto bufferPtr = serialize<options>::impl(object, buffer.data(), 0);
				buffer.resize(static_cast<uint64_t>(bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, bool_types value_type, buffer_like_types buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) {
			static constexpr serialize_options options{ optionsNew };
			if constexpr (resize_and_overwriteable_types<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(8ull, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < 8ull) {
					buffer.resize(8ull);
				}
				const uint64_t targetSize = object ? 4ull : 5ull;
				serialize<options>::impl(object, buffer.data(), 0);
				buffer.resize(targetSize);
			}
			return true;
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		serializer& operator=(const serializer& other) = delete;
		serializer& operator=(serializer&& other)	   = delete;
		serializer(const serializer& other)			   = delete;
		serializer(serializer&& other)				   = delete;
		~serializer() noexcept						   = default;
		serializer() noexcept						   = default;
	};

}
