// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/parsing/parser.hpp
#pragma once

#include <jsonifier-incl/utilities/json_entity.hpp>
#include <jsonifier-incl/parsing/validator.hpp>
#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static auto getBeginIterVec(value_type& value JSONIFIER_LIFETIME_BOUND) {
		if constexpr (std::is_same_v<typename value_type::value_type, bool>) {
			return value.begin();
		} else {
			return value.data();
		}
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static auto getEndIterVec(value_type& value JSONIFIER_LIFETIME_BOUND) {
		if constexpr (std::is_same_v<typename value_type::value_type, bool>) {
			return value.end();
		} else {
			return value.data() + value.size();
		}
	}

	template<typename value_type, typename context_type, parse_options options> struct parse_impl;

	template<parse_options options> struct parse {
		template<typename value_type, typename context_type> JSONIFIER_INLINE static bool impl(value_type&& value, context_type& context) noexcept {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::impl(value, context);
		}

		template<typename value_type, typename context_type> JSONIFIER_INLINE static bool rootImpl(value_type&& value, context_type& context) noexcept {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::rootImpl(value, context);
		}
	};

	template<typename context_type>
	concept structural_context = requires(context_type ctx) { ctx.currentIterPtr(); };

	template<typename derived_type_new> class parser {
	  public:
		friend class jsonifier::raw_json_data;

		using derived_type = derived_type_new;

		parser& operator=(const parser& other) = delete;
		parser(const parser& other)			   = delete;

		template<parse_options options = parse_options{}, typename comparison_type, typename buffer_type>
		inline bool parseJsonForComparison(comparison_type&& object, buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			object.indices.resize(derivedRef.section.getTapeCount());
			std::copy_n(derivedRef.section.begin(), object.indices.size(), object.indices.data());
			json_iterator<parseOpts, structural_index_ptr, remove_reference_t<decltype(getStringBuffer())>> context{ &getStringBuffer(), &getErrors(), derivedRef.section.begin(),
				derivedRef.section.end(), derivedRef.section.begin(), rootIter, endIter };
			auto newSize = static_cast<uint64_t>(*endIter) / 2;
			if (getStringBuffer().size() < newSize) {
				getStringBuffer().resize(newSize);
			}
			return getErrors().size() == 0;
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline structural_index_ptr collectStructurals(buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto rootIter = getBeginIter(in);
			auto endIter  = getEndIter(in);
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			return derivedRef.section.begin();
		}

		template<parse_options options = parse_options{}, typename value_type, typename buffer_type> inline bool parseJson(value_type&& object, buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			if constexpr (parseOpts.partialRead) {
				auto rootIter = getBeginIter(in);
				auto endIter  = getEndIter(in);
				derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
				json_iterator<parseOpts, structural_index_ptr, remove_reference_t<decltype(getStringBuffer())>> context{ &getStringBuffer(), &getErrors(),
					derivedRef.section.begin(), derivedRef.section.end(), derivedRef.section.begin(), rootIter, endIter };
				auto newSize = static_cast<uint64_t>(*endIter);
				if (getStringBuffer().size() < newSize) {
					getStringBuffer().resize(newSize);
				}
				getErrors().clear();
				if (context.anyInput()) {
					if (parse<parseOpts>::rootImpl(object, context)) {
					}
					context.checkIfDone();
					return getErrors().size() == 0;
				} else {
					return false;
				}
			} else {
				auto iter	 = getBeginIter(in);
				auto endIter = getEndIter(in);
				json_iterator<parseOpts, string_view_ptr, remove_reference_t<decltype(getStringBuffer())>> context{ &getStringBuffer(), &getErrors(), iter, endIter };
				auto newSize = static_cast<uint64_t>(endIter - iter);
				if (getStringBuffer().size() < newSize) {
					getStringBuffer().resize(newSize);
				}
				getErrors().clear();
				if (context.anyInput()) {
					parse<parseOpts>::rootImpl(object, context);
					context.checkIfDone();
				}
				return getErrors().size() == 0;
			}
		}

	  protected:
		std::vector<error>& getErrors() noexcept {
			return derivedRef.getErrors();
		}

		JSONIFIER_INLINE auto& getStringBuffer() noexcept {
			return derivedRef.stringBuffer;
		}

		derived_type& derivedRef{ initializeSelfRef() };

		parser() noexcept {
		}

		derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		~parser() noexcept = default;
	};

}
