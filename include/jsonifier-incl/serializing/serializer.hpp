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

	template<typename value_type, typename context_type, serialize_options optionsNew> struct serialize_impl;

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
		template<typename value_type_new, typename context_type> inline static void impl(value_type_new&& value, context_type&& context) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			serialize_impl<value_type, context_type, options>::impl(internal::forward<value_type_new>(value), internal::forward<context_type>(context));
		}

		template<typename value_type_new, typename context_type> JSONIFIER_INLINE static void implInline(value_type_new&& value, context_type&& context) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			serialize_impl<value_type, context_type, options>::impl(internal::forward<value_type_new>(value), internal::forward<context_type>(context));
		}
	};

	template<typename buffer_type> struct serialize_context {
		inline serialize_context() noexcept = default;

		inline serialize_context& operator=(const serialize_context&) noexcept = delete;
		inline serialize_context(const serialize_context&) noexcept			   = delete;
		inline serialize_context& operator=(serialize_context&&) noexcept	   = delete;
		inline serialize_context(serialize_context&&) noexcept				   = delete;

		inline serialize_context(string_buffer_ptr ptrNew) noexcept : bufferPtr{ ptrNew } {
		}

		string_buffer_ptr __restrict bufferPtr{};
		uint64_t indent{};
		uint64_t index{};
	};

	template<serialize_options options, typename value_type, typename buffer_type> struct serialize_context_ro {
		inline serialize_context_ro() noexcept = default;

		inline serialize_context_ro& operator=(const serialize_context_ro&) noexcept = delete;
		inline serialize_context_ro(const serialize_context_ro&) noexcept			 = delete;
		inline serialize_context_ro& operator=(serialize_context_ro&&) noexcept		 = delete;
		inline serialize_context_ro(serialize_context_ro&&) noexcept				 = delete;

		inline serialize_context_ro(value_type& objectNew) noexcept : object{ objectNew } {
		}

		JSONIFIER_INLINE uint64_t operator()(char* __restrict ptrNew, uint64_t) {
			bufferPtr = ptrNew;
			serialize<options>::impl(object, *this);
			return static_cast<uint64_t>(bufferPtr - ptrNew);
		}

		string_buffer_ptr __restrict bufferPtr{};
		value_type& object;
		uint64_t indent{};
		uint64_t index{};
	};

	template<typename derived_type> class serializer {
	  public:
		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like buffer_type>
		inline bool serializeJsonDirect(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			serialize_context<decltype(buffer)> context{ buffer.data() };
			serialize<options>::implInline(object, context);
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, string_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			const auto newSize = object.size() * 6ull + 2ull + simdBytesPerStep;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_context_ro<options, remove_reference_t<value_type>, remove_cvref_t<buffer_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				serialize_context<remove_cvref_t<buffer_type>> context{ buffer.data() };
				serialize<options>::implInline(object, context);
				buffer.resize(static_cast<uint64_t>(context.bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, bool_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			const uint64_t targetSize = object ? 4ull : 5ull;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(targetSize, serialize_context_ro<options, remove_reference_t<value_type>, remove_cvref_t<buffer_type>>{ object });
			} else {
				if (buffer.size() < targetSize) {
					buffer.resize(targetSize);
				}
				serialize_context<remove_cvref_t<buffer_type>> context{ buffer.data() };
				serialize<options>::implInline(object, context);
				buffer.resize(targetSize);
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, number_t value_type, buffer_like buffer_type>
		JSONIFIER_INLINE bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			const uint64_t newSize{ digit_sizes<value_type>::value };
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_context_ro<options, remove_reference_t<value_type>, remove_cvref_t<buffer_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				serialize_context<decltype(buffer)> context{ buffer.data() };
				serialize<options>::implInline(object, context);
				context.index = static_cast<uint64_t>(context.bufferPtr - buffer.data());
				buffer.resize(context.index);
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like buffer_type>
		inline bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			const uint64_t newSize = sizeContext.requiredSize + 64ull;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<buffer_type>>) {
				buffer.resize_and_overwrite(newSize, serialize_context_ro<options, remove_reference_t<value_type>, remove_cvref_t<buffer_type>>{ object });
			} else {
				if (buffer.size() < newSize) {
					buffer.resize(newSize);
				}
				serialize_context<remove_cvref_t<buffer_type>> context{ buffer.data() };
				serialize<options>::impl(object, context);
				buffer.resize(static_cast<uint64_t>(context.bufferPtr - buffer.data()));
			}
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, typename value_type> inline string_view serializeJson(const value_type& object) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			if (derivedRef.stringBuffer.size() < sizeContext.requiredSize + 64) {
				derivedRef.stringBuffer.resize(sizeContext.requiredSize + 64);
			}
			serialize_context<decltype(derivedRef.stringBuffer)> context{ derivedRef.stringBuffer.data() };
			serialize<options>::impl(object, context);
			context.index = static_cast<uint64_t>(context.bufferPtr - derivedRef.stringBuffer.data());
			return string_view{ derivedRef.stringBuffer.data(), context.index };
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		serializer() noexcept						   = default;
		serializer& operator=(const serializer& other) = delete;
		serializer(const serializer& other)			   = delete;
		serializer& operator=(serializer&& other)	   = delete;
		serializer(serializer&& other)				   = delete;
		~serializer() noexcept						   = default;
	};

}
