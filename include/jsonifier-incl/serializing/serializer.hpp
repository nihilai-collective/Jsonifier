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

#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/error.hpp>

namespace jsonifier::internal {

	template<typename value_type, typename context_type, serialize_options optionsNew> struct serialize_impl;

	struct size_context {
		uint64_t requiredSize{};
		uint64_t indent{};
	};

	template<typename value_type, serialize_options optionsNew> struct size_getter_impl;

	template<serialize_options options> struct get_size {
		template<typename value_type_new> JSONIFIER_INLINE static void impl(value_type_new&& value, size_context& context) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			size_getter_impl<value_type, options>::impl(internal::forward<value_type_new>(value), context);
		}
	};

	template<serialize_options options> struct serialize {
		template<typename value_type_new, typename context_type> inline static void impl(value_type_new&& value, context_type&& context) noexcept {
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

		inline serialize_context(string_buffer_ptr ptrNew, buffer_type& bufferNew) noexcept : bufferPtr{ ptrNew }, buffer{ bufferNew } {
		}

		string_buffer_ptr bufferPtr{};
		buffer_type& buffer{};
		uint64_t indent{};
		uint64_t index{};
	};

	template<typename derived_type> class serializer {
	  public:
		serializer& operator=(const serializer& other) = delete;
		serializer(const serializer& other)			   = delete;

		template<serialize_options optionsNew = serialize_options{}, typename value_type, concepts::buffer_like buffer_type>
		inline bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			if (derivedRef.stringBuffer.size() < sizeContext.requiredSize + 64) {
				derivedRef.stringBuffer.resize(sizeContext.requiredSize + 64);
			}
			serialize_context<decltype(derivedRef.stringBuffer)> context{ derivedRef.stringBuffer.data(), derivedRef.stringBuffer };
			serialize<options>::impl(object, context);
			context.index = static_cast<uint64_t>(context.bufferPtr - context.buffer.data());
			buffer.resize(context.index);
			std::copy(derivedRef.stringBuffer.data(), derivedRef.stringBuffer.data() + context.index, buffer.data());
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, typename value_type> inline string_view serializeJson(const value_type& object) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			if (derivedRef.stringBuffer.size() < sizeContext.requiredSize + 64) {
				derivedRef.stringBuffer.resize(sizeContext.requiredSize + 64);
			}
			serialize_context<decltype(derivedRef.stringBuffer)> context{ derivedRef.stringBuffer.data(), derivedRef.stringBuffer };
			serialize<options>::impl(object, context);
			context.index = static_cast<uint64_t>(context.bufferPtr - context.buffer.data());
			return string_view{ derivedRef.stringBuffer.data(), context.index };
		}

	  protected:
		derived_type& derivedRef{ initializeSelfRef() };

		serializer() noexcept : derivedRef{ initializeSelfRef() } {
		}

		derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		~serializer() noexcept = default;
	};

}
