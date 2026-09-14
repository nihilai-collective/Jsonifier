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

	template<typename derived_type> class validator {
	  public:
		template<json_structural_type, typename derived_type_new> friend struct validate_impl;
		validator& operator=(const validator& other) = delete;
		validator(const validator& other)			 = delete;

		template<string_t string_type> inline bool validateJson(string_type&& in) noexcept {
			static constexpr parse_options validateOpts{};
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			derivedRef.section.template reset<validateOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			json_iterator<validateOpts, structural_index_ptr, remove_reference_t<decltype(getStringBuffer())>> context{ &getStringBuffer(), &getErrors(),
				derivedRef.section.begin(), derivedRef.section.end(), derivedRef.section.begin(), rootIter, endIter };
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

	  protected:
		derived_type& derivedRef{ initializeSelfRef() };

		validator() noexcept {
		}

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
			return derivedRef.stringBuffer;
		}

		std::vector<error>& getErrors() noexcept {
			return derivedRef.getErrors();
		}

		derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		~validator() noexcept = default;
	};

}// namespace internal
