// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/parsing/validator.hpp
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/json_iterator.hpp>

namespace jsonifier::internal {

	template<pointer_t value_type> JSONIFIER_INLINE static string_view_ptr getEndIter(value_type value) noexcept {
		return value + strLen(value);
	}

	template<pointer_t value_type> JSONIFIER_INLINE static string_view_ptr getBeginIter(value_type value) noexcept {
		return std::bit_cast<string_view_ptr>(value);
	}

	template<has_data value_type> JSONIFIER_INLINE static string_view_ptr getEndIter(value_type& value) noexcept {
		return std::bit_cast<string_view_ptr>(value.data() + value.size());
	}

	template<has_data value_type> JSONIFIER_INLINE static string_view_ptr getBeginIter(value_type& value) noexcept {
		return std::bit_cast<string_view_ptr>(value.data());
	}

	template<json_structural_type typeNew, typename derived_type> struct validate_impl;

	template<typename derived_type_new> class validator {
	  public:
		template<json_structural_type, typename derived_type_newer> friend struct validate_impl;
		using derived_type = derived_type_new;
		friend derived_type;

		template<string_t string_type> inline bool validateJson(string_type&& in) noexcept {
			derived_type& selfRef{ getSelfRef() };
			static constexpr parse_options validateOpts{};
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			selfRef.section.template reset<validateOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			json_iterator<validateOpts, structural_index_ptr, remove_reference_t<decltype(getStringBuffer())>> context{ &getStringBuffer(), &getErrors(), selfRef.section.begin(),
				selfRef.section.end(), selfRef.section.begin(), rootIter, endIter };
			auto newSize = static_cast<uint64_t>(endIter - rootIter) / 2;
			if (getStringBuffer().size() < newSize) {
				getStringBuffer().resize(newSize);
			}
			getErrors().clear();
			if (context.anyInput()) {
				if (!impl(context)) {
					return false;
				}
				context.checkIfDone();
				return getErrors().size() == 0;
			} else {
				return false;
			}
		}

	  private:
		validator()								   = default;
		validator(const validator&)				   = default;
		validator& operator=(const validator&)	   = default;
		validator(validator&&) noexcept			   = default;
		validator& operator=(validator&&) noexcept = default;
		~validator()							   = default;

		template<typename context_type> inline static bool impl(context_type& context) noexcept {
			if (!context.notAtEndPre()) {
				return false;
			}
			const auto c = *context.currentPtr();
			if (c == '{') {
				return validate_impl<json_structural_type::object_start, derived_type>::impl(context);
			} else if (c == '[') {
				return validate_impl<json_structural_type::array_start, derived_type>::impl(context);
			} else if (c == '"') {
				return validate_impl<json_structural_type::string, derived_type>::impl(context);
			} else if (numberTable[static_cast<uint8_t>(c)]) {
				return validate_impl<json_structural_type::number, derived_type>::impl(context);
			} else if (boolTable[static_cast<uint8_t>(c)]) {
				return validate_impl<json_structural_type::boolean, derived_type>::impl(context);
			} else if (c == 'n') {
				return validate_impl<json_structural_type::null, derived_type>::impl(context);
			} else {
				return false;
			}
		}

		JSONIFIER_INLINE auto& getStringBuffer() noexcept {
			derived_type& selfRef{ getSelfRef() };
			return selfRef.stringBuffer;
		}

		std::vector<error>& getErrors() noexcept {
			derived_type& selfRef{ getSelfRef() };
			return selfRef.getErrors();
		}

		JSONIFIER_INLINE derived_type& getSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}
	};

}// namespace internal
