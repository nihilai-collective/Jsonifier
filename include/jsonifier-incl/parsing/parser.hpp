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

	template<parse_options options, typename value_type, typename iterator_type> struct string_scan_context {
		using scan_result = typename string_scanner<options>::scan_result;

		inline string_scan_context() noexcept									   = default;
		inline string_scan_context& operator=(const string_scan_context&) noexcept = delete;
		inline string_scan_context(const string_scan_context&) noexcept			   = delete;
		inline string_scan_context& operator=(string_scan_context&&) noexcept	   = delete;
		inline string_scan_context(string_scan_context&&) noexcept				   = delete;

		inline string_scan_context(scan_result& resultNew, iterator_type strIterNew, iterator_type endIterNew) noexcept
			: result{ resultNew }, strIter{ strIterNew }, endIter{ endIterNew } {
		}

		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename value_type::pointer>* __restrict ptrNew, uint64_t) noexcept {
			result = string_scanner<options>::impl(strIter, endIter, ptrNew);
			return result.outLength == std::numeric_limits<uint64_t>::max() ? uint64_t{} : result.outLength;
		}

		scan_result& result;
		iterator_type strIter{};
		iterator_type endIter{};
	};

	template<typename derived_type_new> struct parser {
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

		template<parse_options options = parse_options{}, typename buffer_type> inline structural_index_ptr collectStructuralsSingle(buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			derivedRef.podSection.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			return derivedRef.podSection.begin();
		}

		template<parse_options options = parse_options{}, string_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			if constexpr (parseOpts.partialRead) {
				derivedRef.errors.clear();
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				if (!rootIter || rootIter == endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::no_input>(rootIter, rootIter, endIter));
					return false;
				}
				const structural_index_ptr tapeIter = indexStructurals<parseOpts.minified>(rootIter, endIter);
				auto* __restrict valueIter			= rootIter + *tapeIter;
				if (tapeIter == derivedRef.podSection.end() || *valueIter != '"') [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_string_characters>(rootIter, valueIter, endIter));
					return false;
				}
				if (valueIter + 1 >= endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unexpected_end_of_input>(rootIter, valueIter, endIter));
					return false;
				}
				if (scanString<options>(object, valueIter + 1, endIter).outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_string_characters>(rootIter, valueIter, endIter));
					return false;
				}
				const structural_index_ptr nextTapeIter = tapeIter + 1;
				if (nextTapeIter < derivedRef.podSection.end() && rootIter + *nextTapeIter != endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::unfinished_input>(rootIter, rootIter + *nextTapeIter, endIter));
					return false;
				}
				return true;
			} else {
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				if (rootIter >= endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::no_input>(rootIter, rootIter, endIter));
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
				const auto res = scanString<options>(object, rootIter, endIter);
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
		}

		template<parse_options options = parse_options{}, bool_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			if constexpr (parseOpts.partialRead) {
				derivedRef.errors.clear();
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				if (!rootIter || rootIter == endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::no_input>(rootIter, rootIter, endIter));
					return false;
				}
				const structural_index_ptr tapeIter = indexStructurals<parseOpts.minified>(rootIter, endIter);
				auto* __restrict valueIter			= rootIter + *tapeIter;
				static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
				static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
				const auto remaining = endIter - valueIter;
				if (remaining < 4) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, valueIter, endIter));
					return false;
				}
				uint32_t comparison;
				pow2_memcpy_wrapper<4>(&comparison, valueIter);
				if constexpr (std::endian::native == std::endian::big) {
					comparison = byteswap(comparison);
				}
				if (comparison == trueVal && remaining == 4) {
					object = true;
					return true;
				} else if (comparison == falseVal && remaining == 5 && valueIter[4] == 'e') [[likely]] {
					object = false;
					return true;
				}
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, valueIter, endIter));
				return false;
			} else {
				static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
				static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				if (endIter - rootIter < 4) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_bool_value>(rootIter, rootIter, endIter));
					return false;
				}
				uint32_t comparison;
				pow2_memcpy_wrapper<4>(&comparison, rootIter);
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
		}

		template<parse_options options = parse_options{}, number_t value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			if constexpr (parseOpts.partialRead) {
				derivedRef.errors.clear();
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				if (!rootIter || rootIter == endIter) [[unlikely]] {
					derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::no_input>(rootIter, rootIter, endIter));
					return false;
				}
				const structural_index_ptr tapeIter = indexStructurals<parseOpts.minified>(rootIter, endIter);
				auto* __restrict valueIter			= rootIter + *tapeIter;
				return parseRootNumber<options>(object, rootIter, valueIter, endIter);
			} else {
				auto* __restrict rootIter = getBeginIter(in);
				auto* __restrict endIter  = getEndIter(in);
				return parseRootNumber<options>(object, rootIter, rootIter, endIter);
			}
		}

		template<parse_options options = parse_options{}, typename value_type, typename buffer_type> inline bool parseJson(value_type&& object, const buffer_type& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			derivedRef.errors.clear();
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			auto newSize			  = static_cast<uint64_t>(endIter - rootIter);
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
				json_iterator<parseOpts, read_buffer_ptr, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer, &derivedRef.errors, rootIter,
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

		template<bool minified> JSONIFIER_INLINE structural_index_ptr indexStructurals(auto* rootIter, const auto* endIter) noexcept {
			derivedRef.podSection.template reset<minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			return derivedRef.podSection.begin();
		}

		template<parse_options options, typename value_type>
		JSONIFIER_INLINE static typename string_scanner<options>::scan_result scanString(value_type& object, auto* strIter, const auto* endIter) noexcept {
			using context_type = string_scan_context<options, value_type, decltype(strIter)>;
			const auto needed  = static_cast<uint64_t>(endIter - strIter) + simdBytesPerStep;
			typename string_scanner<options>::scan_result res{};
			if constexpr (has_resize_and_overwrite<value_type>) {
				object.resize_and_overwrite(needed, context_type{ res, strIter, endIter });
			} else {
				if (object.size() < needed) [[unlikely]] {
					object.resize(needed);
				}
				context_type context{ res, strIter, endIter };
				if (const uint64_t newLength = context(object.data(), needed); res.outLength != std::numeric_limits<uint64_t>::max()) [[likely]] {
					object.resize(newLength);
				}
			}
			return res;
		}

		template<parse_options options, typename value_type>
		JSONIFIER_INLINE bool parseRootNumber(value_type& object, auto* rootIter, auto* valueIter, const auto* endIter) noexcept {
			using num_type = remove_cvref_t<value_type>;
			if constexpr (integer_t<num_type>) {
				if constexpr (uint_types<num_type>) {
					if constexpr (uint64_types<num_type>) {
						if (auto iterNew = integer_parser<num_type>::parseInt(object, valueIter, endIter); iterNew) {
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, valueIter, endIter));
						return false;
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueIter, endIter); iterNew) {
							object = static_cast<num_type>(i);
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, valueIter, endIter));
						return false;
					}
				} else {
					if constexpr (int64_types<num_type>) {
						if (auto iterNew = integer_parser<num_type>::parseInt(object, valueIter, endIter); iterNew) {
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, valueIter, endIter));
						return false;
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueIter, endIter); iterNew) {
							object = static_cast<num_type>(i);
							return finish<options.minified>(rootIter, iterNew, endIter);
						}
						derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, valueIter, endIter));
						return false;
					}
				}
			} else {
				if (auto iterNew = float_parser<num_type>::parseFloat(object, valueIter, endIter); iterNew) {
					return finish<options.minified>(rootIter, iterNew, endIter);
				}
				derivedRef.errors.emplace_back(error::constructError<status_classes::parsing, parse_statuses::invalid_number_value>(rootIter, valueIter, endIter));
				return false;
			}
		}

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
