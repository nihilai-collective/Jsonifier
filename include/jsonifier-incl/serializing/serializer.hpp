// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/serializing/serializer.hpp
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
		template<typename value_type_new> static void impl(value_type_new& value, size_context& context) noexcept {
			using value_type = remove_cvref_t<value_type_new>;
			get_size_impl<value_type, options>::impl(value, context);
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

		inline serialize_context(string_buffer_ptr ptrNew) noexcept : bufferPtr{ ptrNew } {
		}

		string_buffer_ptr __restrict bufferPtr{};
		uint64_t indent{};
		uint64_t index{};
	};

	template<typename derived_type> class serializer {
	  public:
		serializer& operator=(const serializer& other) = delete;
		serializer(const serializer& other)			   = delete;

		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like buffer_type>
		inline bool serializeJsonDirect(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			serialize_context<decltype(buffer)> context{ buffer.data() };
			serialize<options>::impl(object, context);
			return true;
		}

		template<serialize_options optionsNew = serialize_options{}, typename value_type, buffer_like buffer_type>
		inline bool serializeJson(value_type&& object, buffer_type&& buffer) noexcept {
			static constexpr serialize_options options{ optionsNew };
			size_context sizeContext{};
			get_size<options>::impl(object, sizeContext);
			if (derivedRef.stringBuffer.size() < sizeContext.requiredSize + 64) {
				derivedRef.stringBuffer.resize(sizeContext.requiredSize + 64);
			}
			serialize_context<decltype(derivedRef.stringBuffer)> context{ derivedRef.stringBuffer.data() };
			serialize<options>::impl(object, context);
			context.index = static_cast<uint64_t>(context.bufferPtr - derivedRef.stringBuffer.data());
			buffer.resize(context.index);
			static constexpr uint64_t charSize = sizeof(remove_cvref_t<decltype(buffer[0])>);
			std::memcpy(buffer.data(), derivedRef.stringBuffer.data(), context.index * charSize);
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
		derived_type& derivedRef{ initializeSelfRef() };

		serializer() noexcept {
		}

		derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		~serializer() noexcept = default;
	};

}
