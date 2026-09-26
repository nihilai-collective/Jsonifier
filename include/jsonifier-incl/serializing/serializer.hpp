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

	struct size_context {
		uint64_t requiredSize{};
		uint64_t indent{};
	};

	template<typename value_type, serialize_options optionsNew> struct get_size_impl;

	template<serialize_options options> struct get_size {
		template<typename value_type_new> inline static void impl(value_type_new& value, size_context& context) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			get_size_impl<value_type, options>::impl(value, context);
		}
	};

	template<serialize_options options> struct serialize {
		template<typename value_type_new>
		JSONIFIER_INLINE static write_buffer_ptr implInline(value_type_new&& value, write_buffer_ptr bufferPtr, uint64_t indent) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			return serialize_impl<value_type, options>::impl(internal::forward<value_type_new>(value), bufferPtr, indent);
		}

		template<typename value_type_new> inline static write_buffer_ptr impl(value_type_new&& value, write_buffer_ptr bufferPtr, uint64_t indent) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			return serialize_impl<value_type, options>::impl(internal::forward<value_type_new>(value), bufferPtr, indent);
		}
	};

	template<serialize_options options, typename value_type> struct serialize_writer_ro {
		JSONIFIER_INLINE uint64_t operator()(write_buffer_ptr ptrNew, uint64_t) noexcept {
			write_buffer_ptr bufferPtr;
			if constexpr (bool_t<value_type> || number_t<value_type> || string_t<value_type>) {
				bufferPtr = serialize<options>::implInline(object, ptrNew, 0);
			} else {
				bufferPtr = serialize<options>::impl(object, ptrNew, 0);
			}
			return static_cast<uint64_t>(bufferPtr - ptrNew);
		}

		inline serialize_writer_ro(value_type& objectNew) noexcept : object{ objectNew } {
		}

		serialize_writer_ro& operator=(const serialize_writer_ro&) noexcept = delete;
		serialize_writer_ro& operator=(serialize_writer_ro&&) noexcept		= delete;
		serialize_writer_ro(const serialize_writer_ro&) noexcept			= delete;
		serialize_writer_ro(serialize_writer_ro&&) noexcept					= delete;

		value_type& object;
	};

	template<typename derived_type_new> struct serializer {
		using derived_type = derived_type_new;

		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like buffer_type>
		inline bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			const uint64_t newSize = sizeContext.requiredSize + 64ull;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
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

		template<serialize_options optionsNew = serialize_options{}, number_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			const uint64_t newSize{ digit_sizes<value_type>::value };
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				const auto bufferPtr = serialize<options>::implInline(object, buffer.data(), 0);
				buffer.resize(static_cast<uint64_t>(bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, string_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			const auto newSize = object.size() * 6ull + 2ull + simdBytesPerStep;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				const auto bufferPtr = serialize<options>::implInline(object, buffer.data(), 0);
				buffer.resize(static_cast<uint64_t>(bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, bool_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(8ull, serialize_writer_ro<options, remove_reference_t<value_type>>{ object });
			} else {
				if (buffer.size() < 8ull) {
					buffer.resize(8ull);
				}
				const uint64_t targetSize = object ? 4ull : 5ull;
				serialize<options>::implInline(object, buffer.data(), 0);
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
		inline ~serializer() noexcept				   = default;
		inline serializer() noexcept				   = default;
	};

}
