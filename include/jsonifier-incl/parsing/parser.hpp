/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/parser.hpp
 */
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
		template<typename value_type, typename context_type> inline static bool impl(value_type&& value, context_type& context) noexcept {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::impl(value, context);
		}

		template<typename value_type, typename context_type> inline static bool rootImpl(value_type&& value, context_type& context) noexcept {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::rootImpl(value, context);
		}
	};

	template<typename derived_type_new> class parser {
	  public:
		friend class jsonifier::raw_json_data;

		using derived_type = derived_type_new;

		template<parse_options options = parse_options{}, typename comparison_type, typename buffer_type>
		inline bool parseJsonForComparison(comparison_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			derivedRef.errors.clear();
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			object.indices.clear();
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			if (derivedRef.errors.size() == 0) {
				object.indices.resize(derivedRef.section.getTapeCount());
				std::copy_n(derivedRef.section.begin(), object.indices.size(), object.indices.data());
				return true;
			} else {
				return false;
			}
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline structural_index_ptr collectStructurals(buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			return derivedRef.section.begin();
		}

		template<parse_options options = parse_options{}, string_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			using str_type			  = remove_cvref_t<value_type>;
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			if (rootIter >= endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unexpected_end_of_input>(rootIter, rootIter, endIter));
				return false;
			}
			if (*rootIter != '"') [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_string_characters>(rootIter, rootIter, endIter));
				return false;
			}
			++rootIter;
			if (rootIter >= endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unexpected_end_of_input>(rootIter, rootIter, endIter));
				return false;
			}
			const auto needed = static_cast<uint64_t>(endIter - rootIter) + simdBytesPerStep;
			typename string_scanner<options>::scan_result res{};
			if constexpr (has_resize_and_overwrite<str_type>) {
				object.resize_and_overwrite(needed, [&](auto* __restrict ptr, uint64_t) noexcept {
					res = string_scanner<options>::impl(rootIter, endIter, ptr);
					return res.outLength == std::numeric_limits<uint64_t>::max() ? uint64_t{} : res.outLength;
				});
			} else {
				if (object.size() < needed) [[unlikely]] {
					object.resize(needed);
				}
				res = string_scanner<options>::impl(rootIter, endIter, object.data());
				if (res.outLength != std::numeric_limits<uint64_t>::max()) [[likely]] {
					object.resize(res.outLength);
				}
			}
			if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_string_characters>(rootIter, rootIter, endIter));
				return false;
			}
			rootIter += res.rawLength + 1;
			if (rootIter > endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unexpected_end_of_input>(rootIter, rootIter, endIter));
				return false;
			}
			if constexpr (!options.minified) {
				while (rootIter < endIter && whitespaceTable[static_cast<uint8_t>(*rootIter)]) {
					++rootIter;
				}
			}
			if (rootIter != endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unfinished_input>(rootIter, rootIter, endIter));
				return false;
			}
			return true;
		}

		template<parse_options options = parse_options{}, bool_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			if (endIter - rootIter < 4) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, rootIter, endIter));
				return false;
			}
			uint32_t comparison;
			std::memcpy(&comparison, rootIter, 4);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = byteswap(comparison);
			}
			rootIter += 4;
			if (comparison == trueVal) {
				object = true;
			} else if (rootIter == endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unexpected_end_of_input>(rootIter, rootIter, endIter));
				return false;
			} else if (comparison == falseVal && (*rootIter == 'e')) [[likely]] {
				object = false;
				++rootIter;
			} else {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, rootIter, endIter));
				return false;
			}
			if (rootIter != endIter) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, rootIter, endIter));
				return false;
			}
			return true;
		}

		template<parse_options options = parse_options{}, number_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			using num_type			  = remove_cvref_t<value_type>;
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			if constexpr (integer_t<num_type>) {
				if constexpr (uint_types<num_type>) {
					if constexpr (uint64_types<num_type>) {
						if (auto iterNew = integer_parser<num_type>::parseInt(object, rootIter, endIter); iterNew) {
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, rootIter, endIter));
						return false;
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, rootIter, endIter); iterNew) {
							object = static_cast<num_type>(i);
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, rootIter, endIter));
						return false;
					}
				} else {
					if constexpr (int64_types<num_type>) {
						if (auto iterNew = integer_parser<num_type>::parseInt(object, rootIter, endIter); iterNew) {
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, rootIter, endIter));
						return false;
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, rootIter, endIter); iterNew) {
							object = static_cast<num_type>(i);
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, rootIter, endIter));
						return false;
					}
				}
			} else {
				if (auto iterNew = float_parser<num_type>::parseFloat(object, rootIter, endIter); iterNew) {
					return finish<options.minified>(rootIter, iterNew, endIter);
				}
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, rootIter, endIter));
				return false;
			}
		}

		template<parse_options options = parse_options{}, typename value_type, typename buffer_type> inline bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			derivedRef.errors.clear();
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			auto newSize  = static_cast<uint64_t>(endIter - rootIter);
			if (derivedRef.stringBuffer.size() < newSize) {
				derivedRef.stringBuffer.resize(newSize);
			}
			if constexpr (parseOpts.partialRead) {
				derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
				json_iterator<parseOpts, structural_index_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors,
					derivedRef.section.begin(), derivedRef.section.end(), derivedRef.section.begin(), rootIter, endIter };
				if (context.anyInput()) {
					parse<parseOpts>::rootImpl(object, context);
					context.checkIfDone();
					return derivedRef.errors.size() == 0;
				} else {
					return false;
				}
			} else {
				json_iterator<parseOpts, string_view_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors, rootIter,
					endIter };
				if (context.anyInput()) {
					parse<parseOpts>::rootImpl(object, context);
					context.checkIfDone();
					return derivedRef.errors.size() == 0;
				} else {
					return false;
				}
			}
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		parser() noexcept					   = default;
		parser& operator=(const parser& other) = delete;
		parser(const parser& other)			   = delete;
		parser& operator=(parser&& other)	   = delete;
		parser(parser&& other)				   = delete;
		~parser() noexcept					   = default;

		template<bool minified> JSONIFIER_INLINE bool finish(auto* rootIter, auto* iterNew, const auto* endIter) noexcept {
			if constexpr (!minified) {
				while (iterNew < endIter && whitespaceTable[static_cast<uint8_t>(*iterNew)]) {
					++iterNew;
				}
			}
			if (iterNew == endIter) [[likely]] {
				return true;
			}
			derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unfinished_input>(rootIter, iterNew, endIter));
			return false;
		}
	};

}
