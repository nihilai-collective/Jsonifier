/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/parser.hpp
 */
#pragma once

#include <jsonifier-incl/core/defines.hpp>

#include <jsonifier-incl/utilities/json_entity.hpp>
#include <jsonifier-incl/parsing/validator.hpp>
#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/number_utils.hpp>
#include <jsonifier-incl/utilities/string.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE auto getBeginIterVec(value_type& value JSONIFIER_LIFETIME_BOUND) {
		if constexpr (std::is_same_v<typename base_t<value_type>::value_type, bool>) {
			return value.begin();
		} else {
			return value.data();
		}
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE auto getEndIterVec(value_type& value JSONIFIER_LIFETIME_BOUND) {
		if constexpr (std::is_same_v<typename base_t<value_type>::value_type, bool>) {
			return value.end();
		} else {
			return value.data() + value.size();
		}
	}

	template<typename value_type, typename context_type, parse_options options> struct parse_impl;

	template<parse_options options> struct parse {
		template<typename value_type, typename iterator_type, typename context_type>
		JSONIFIER_NOINLINE static iterator_type implOutline(value_type& value, iterator_type iter, uint64_t depth, context_type& context) {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::implOutline(value, iter, depth, context);
		}

		template<typename value_type, typename iterator_type, typename context_type>
			requires(bool_types<remove_cvref_t<value_type>> || enum_types<remove_cvref_t<value_type>> || char_types<remove_cvref_t<value_type>>)
		JSONIFIER_INLINE static iterator_type impl(value_type&& value, iterator_type iter, uint64_t depth, context_type& context) {
			return parse_impl<remove_cvref_t<value_type>, context_type, options>::impl(value, iter, depth, context);
		}

		template<typename value_type, typename iterator_type, typename context_type>
		inline static iterator_type impl(value_type&& value, iterator_type iter, uint64_t depth, context_type& context) {
			if constexpr (inlinableOpType<remove_cvref_t<value_type>, maxParseInlineMemberCount>()) {
				return parse_impl<remove_cvref_t<value_type>, context_type, options>::impl(value, iter, depth, context);
			} else {
				return implOutline(value, iter, depth, context);
			}
		}
	};

	template<typename derived_type_new> struct parser {
	  public:
		friend class jsonifier::raw_json_data;

		using derived_type = derived_type_new;

		template<parse_options options = parse_options{}, bool_types value_type, typename buffer_type> JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01100101'01110011'01101100'01100001 };
			auto* __restrict beginIter = getBeginIter(in);
			auto* __restrict endIter   = getEndIter(in);
			auto* __restrict rootIter  = skipLeadingWhitespace<options.minified>(beginIter, endIter);
			if (endIter - rootIter > 0) [[likely]] {
				if (*rootIter == uint_c_v<'t'>) {
					if (endIter - rootIter > 3) [[likely]] {
						uint32_t comparison;
						pow2MemcpyWrapper<4>(&comparison, rootIter);
						if constexpr (std::endian::native == std::endian::big) {
							comparison = std::byteswap(comparison);
						}
						if (comparison == trueVal) [[likely]] {
							object = true;
							rootIter += 4;
							rootIter = skipLeadingWhitespace<options.minified>(rootIter, endIter);
							if (rootIter != endIter) [[unlikely]] {
								return reportError<parse_statuses::invalid_bool_value>(beginIter, rootIter, endIter);
							}
							return true;
						} else {
							return reportError<parse_statuses::invalid_bool_value>(beginIter, rootIter, endIter);
						}
					} else {
						return reportError<parse_statuses::unexpected_end_of_input>(beginIter, rootIter, endIter);
					}
				} else if (*rootIter == uint_c_v<'f'>) {
					++rootIter;
					if (endIter - rootIter > 3) [[likely]] {
						uint32_t comparison;
						pow2MemcpyWrapper<4>(&comparison, rootIter);
						if constexpr (std::endian::native == std::endian::big) {
							comparison = std::byteswap(comparison);
						}
						if (comparison == falseVal) [[likely]] {
							object = false;
							rootIter += 4;
							rootIter = skipLeadingWhitespace<options.minified>(rootIter, endIter);
							if (rootIter != endIter) [[unlikely]] {
								return reportError<parse_statuses::invalid_bool_value>(beginIter, rootIter, endIter);
							}
							return true;
						} else {
							return reportError<parse_statuses::invalid_bool_value>(beginIter, rootIter, endIter);
						}
					} else {
						return reportError<parse_statuses::unexpected_end_of_input>(beginIter, rootIter, endIter);
					}
				} else {
					return reportError<parse_statuses::invalid_bool_value>(beginIter, rootIter, endIter);
				}
			} else {
				return reportError<parse_statuses::unexpected_end_of_input>(beginIter, rootIter, endIter);
			}
		}

		template<parse_options options = parse_options{}, typename value_type, typename buffer_type> inline bool parseJson(value_type&& object, const buffer_type& in) {
			static constexpr parse_options parseOpts{ options };			
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			auto newSize{ static_cast<uint64_t>(endIter - rootIter) };
			if (derivedRef.stringBuffer.size() < newSize) {
				derivedRef.stringBuffer.resize(newSize);
			}
			if constexpr (parseOpts.partialRead) {
				derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
				parse_context<parseOpts, unrestrict_t<write_structural_index_ptr>, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer,
					&derivedRef.lastError, rootIter, endIter, derivedRef.section.end() };
				return runParse<parseOpts>(object, derivedRef.section.begin(), context);
			} else {
				parse_context<parseOpts, unrestrict_t<read_buffer_ptr>, remove_reference_t<decltype(derivedRef.stringBuffer)>> context{ &derivedRef.stringBuffer,
					&derivedRef.lastError, rootIter, endIter };
				return runParse<parseOpts>(object, static_cast<read_buffer_ptr>(rootIter), context);
			}
		}

		template<parse_options options = parse_options{}, string_types value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) {
			auto* __restrict beginIter = getBeginIter(in);
			auto* __restrict endIter   = getEndIter(in);
			auto* rootIter			   = skipLeadingWhitespace<options.minified>(beginIter, endIter);
			if (rootIter >= endIter) [[unlikely]] {
				return reportError<parse_statuses::no_input>(beginIter, rootIter, endIter);
			}
			auto* close = endIter - 1;
			if constexpr (!options.minified) {
				while (close > rootIter && whitespaceTable[static_cast<uint64_t>(*close)]) {
					--close;
				}
			}
			if (*rootIter != uint_c_v<'"'>) [[unlikely]] {
				return reportError<parse_statuses::invalid_string_characters>(beginIter, rootIter, endIter);
			}
			++rootIter;
			if (close < rootIter || *close != uint_c_v<'"'>) [[unlikely]] {
				for (auto* scan = rootIter; scan < endIter; ++scan) {
					if (*scan == uint_c_v<'"'>) {
						return reportError<parse_statuses::unfinished_input>(beginIter, scan, endIter);
					}
					if (static_cast<uint8_t>(*scan) < 0x20u) {
						return reportError<parse_statuses::invalid_string_characters>(beginIter, scan, endIter);
					}
					if (*scan == uint_c_v<'\\'>) {
						++scan;
					}
				}
				return reportError<parse_statuses::unexpected_end_of_input>(beginIter, rootIter, endIter);
			}
			if (!assignExactString<cascade_page_safe_loader>(object, rootIter, close, endIter)) [[unlikely]] {
				return reportError<parse_statuses::invalid_string_characters>(beginIter, rootIter, endIter);
			}
			return true;
		}

		template<parse_options options = parse_options{}, typename buffer_type> inline unrestrict_t<write_structural_index_ptr> collectStructurals(buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			return derivedRef.section.begin();
		}

		template<parse_options options = parse_options{}, typename buffer_type>
		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> collectStructuralsSingle(buffer_type&& in) noexcept {
			static constexpr parse_options parseOpts{ options };
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			const uint64_t length{ static_cast<uint64_t>(endIter - rootIter) };
			if (!derivedRef.section.template tryResetLoneValue<parseOpts.minified, false>(rootIter, length)) {
				derivedRef.section.template reset<parseOpts.minified>(rootIter, length);
			}
			return derivedRef.section.begin();
		}

		template<parse_options options = parse_options{}, typename comparison_type, typename buffer_type>
		inline bool parseJsonForComparison(comparison_type&& object, const buffer_type& in) {
			static constexpr parse_options parseOpts{ options };			
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			object.indices.clear();
			derivedRef.section.template reset<parseOpts.minified>(rootIter, static_cast<uint64_t>(endIter - rootIter));
			if (!derivedRef.lastError.hasError()) {
				object.indices.resize(derivedRef.section.getTapeCount());
				std::copy_n(derivedRef.section.begin(), object.indices.size(), object.indices.data());
				return true;
			} else {
				return false;
			}
		}

		template<parse_options options = parse_options{}, number_types value_type, typename buffer_type>
		JSONIFIER_INLINE bool parseJson(value_type&& object, const buffer_type& in) {
			auto* __restrict rootIter = getBeginIter(in);
			auto* __restrict endIter  = getEndIter(in);
			return parseRootNumber<options>(object, rootIter, skipLeadingWhitespace<options.minified>(rootIter, endIter), endIter);
		}

	  protected:
		inline parser() noexcept			   = default;
		parser& operator=(parser&& other)	   = delete;
		parser(parser&& other)				   = delete;
		parser& operator=(const parser& other) = delete;
		parser(const parser& other)			   = delete;

		template<parse_statuses status> JSONIFIER_NOINLINE bool reportError(const auto* rootIter, const auto* errorIter, const auto* endIter) {
			derivedRef.lastError = error::constructError<status_classes::parsing, status>(rootIter, errorIter, endIter);
			return false;
		}

		template<parse_options options, typename value_type> JSONIFIER_INLINE bool parseRootNumber(value_type& object, auto* rootIter, auto* valueIter, const auto* endIter) {
			using num_type = remove_cvref_t<value_type>;
			if constexpr (integer_types<num_type>) {
				if (auto iterNew = integer_parser<num_type>::parseInt(object, valueIter, endIter); iterNew) {
					return finish<options.minified>(rootIter, iterNew, endIter);
				}
			} else {
				if (auto iterNew = float_parser<num_type>::parseFloat(object, valueIter, endIter); iterNew) {
					return finish<options.minified>(rootIter, iterNew, endIter);
				}
			}
			return reportError<parse_statuses::invalid_number_value>(rootIter, valueIter, endIter);
		}

		template<bool minified> JSONIFIER_INLINE static auto* skipLeadingWhitespace(auto* rootIter, const auto* endIter) noexcept {
			if constexpr (!minified) {
				while (rootIter < endIter && whitespaceTable[static_cast<uint64_t>(*rootIter)]) {
					++rootIter;
				}
			}
			return rootIter;
		}

		template<bool minified> JSONIFIER_INLINE bool finish(auto* rootIter, auto* iterNew, const auto* endIter) {
			iterNew = skipLeadingWhitespace<minified>(iterNew, endIter);
			if (iterNew == endIter) [[likely]] {
				return true;
			}
			return reportError<parse_statuses::unfinished_input>(rootIter, iterNew, endIter);
		}

		template<parse_options parseOpts, typename value_type, typename iterator_type, typename context_type>
		JSONIFIER_INLINE bool runParse(value_type& object, iterator_type iter, context_type& context) {
			using cursor = json_cursor<parseOpts, unrestrict_t<iterator_type>>;
			derivedRef.lastError.clear();
			if (!cursor::anyInput(iter, context)) [[unlikely]] {
				return false;
			}
			if constexpr (!parseOpts.minified && !structural_context_types<context_type>) {
				cursor::collectIndentSize(iter, context);
			}
			if (const iterator_type iterNew = parse<parseOpts>::impl(object, iter, 0, context); iterNew) [[likely]] {
				static_cast<void>(cursor::checkIfDone(iterNew, context));
			} else if (!derivedRef.lastError.hasError()) [[unlikely]] {
				static_cast<void>(cursor::template reject<parse_statuses::unfinished_input>(iter, context));
			}
			return !derivedRef.lastError.hasError();
		}

		inline ~parser() noexcept = default;

		derived_type& derivedRef{ *static_cast<derived_type*>(this) };
	};

}
