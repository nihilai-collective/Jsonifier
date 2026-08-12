/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/minifier.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/compare.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	alignas(64) static constexpr array<json_structural_type, 256> jsonTypes = []() constexpr {
		array<json_structural_type, 256> returnValues{};
		using enum json_structural_type;
		returnValues['"'] = string;
		returnValues[','] = comma;
		returnValues['0'] = number;
		returnValues['1'] = number;
		returnValues['2'] = number;
		returnValues['3'] = number;
		returnValues['4'] = number;
		returnValues['5'] = number;
		returnValues['6'] = number;
		returnValues['7'] = number;
		returnValues['8'] = number;
		returnValues['9'] = number;
		returnValues['-'] = number;
		returnValues[':'] = colon;
		returnValues['['] = array_start;
		returnValues[']'] = array_end;
		returnValues['n'] = null;
		returnValues['t'] = boolean;
		returnValues['f'] = boolean;
		returnValues['{'] = object_start;
		returnValues['}'] = object_end;
		return returnValues;
	}();

	template<typename derived_type> class minifier {
	  public:
		template<string_t string_type> inline base_t<string_type> minifyJson(string_type&& in) noexcept {
			if (derivedRef.stringBuffer.size() < in.size()) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size());
			}
			derivedRef.errors.clear();
			string_view_ptr rootIter = in.data();
			string_view_ptr endIter	 = rootIter + in.size();
			derivedRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			structural_index_ptr endStructural = derivedRef.section.end();
			base_t<string_type> newString{};
			if (iter == endStructural) {
				derivedRef.errors.emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
			} else {
				auto index = impl(iter, endStructural, derivedRef.stringBuffer, rootIter, endIter);
				if (index != std::numeric_limits<uint64_t>::max()) {
					newString.resize(index);
					memcpy_wrapper(newString.data(), derivedRef.stringBuffer.data(), index);
				}
			}
			return newString;
		}

		template<string_t input_string_type, string_t output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			if (derivedRef.stringBuffer.size() < in.size()) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size());
			}
			derivedRef.errors.clear();
			string_view_ptr rootIter = in.data();
			string_view_ptr endIter	 = rootIter + in.size();
			derivedRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			structural_index_ptr endStructural = derivedRef.section.end();
			if (iter == endStructural) {
				derivedRef.errors.emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return false;
			}
			auto index = impl(iter, endStructural, derivedRef.stringBuffer, rootIter, endIter);
			if (index != std::numeric_limits<uint64_t>::max()) [[likely]] {
				if (buffer.size() != index) [[likely]] {
					buffer.resize(index);
				}
				memcpy_wrapper(buffer.data(), derivedRef.stringBuffer.data(), index);
				return true;
			} else {
				return false;
			}
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		minifier() noexcept						   = default;
		minifier& operator=(const minifier& other) = delete;
		minifier(const minifier& other)			   = delete;
		minifier& operator=(minifier&& other)	   = delete;
		minifier(minifier&& other)				   = delete;
		~minifier() noexcept					   = default;

		JSONIFIER_INLINE void skipWs(int64_t& currentDistance, string_view_ptr previousPtr) noexcept {
			while (whitespaceTable[static_cast<uint8_t>(previousPtr[--currentDistance])]) {
			}
		}

		template<typename iterator_type>
		JSONIFIER_INLINE void backTrackWs(int64_t& currentDistance, string_view_ptr& previousPtr, iterator_type iter, string_view_ptr rootIter) noexcept {
			currentDistance = (rootIter + *iter) - previousPtr;
			skipWs(currentDistance, previousPtr);
			++currentDistance;
		}

		template<string_t string_type, typename iterator, typename iterator_end> inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural,
			string_type&& outBuffer, string_view_ptr rootIter, string_view_ptr endIter) noexcept {
			using enum json_structural_type;
			auto previousPtr = rootIter + *iter;
			int64_t currentDistance{};
			uint64_t index{};
			++iter;

			while (true) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(*previousPtr)])) {
					case static_cast<uint64_t>(string): {
						backTrackWs(currentDistance, previousPtr, iter, rootIter);
						if (currentDistance > 0) [[likely]] {
							memcpy_wrapper(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							derivedRef.errors.emplace_back(
								jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::invalid_string_length>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						break;
					}
					case static_cast<uint64_t>(comma): {
						outBuffer[index] = ',';
						++index;
						break;
					}
					case static_cast<uint64_t>(number): {
						currentDistance = 0;
						while (!whitespaceTable[static_cast<uint8_t>(previousPtr[++currentDistance])] && ((previousPtr + currentDistance) < (rootIter + *iter))) {
						}
						if (currentDistance > 0) [[likely]] {
							memcpy_wrapper(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							derivedRef.errors.emplace_back(
								jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::invalid_number_value>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						break;
					}
					case static_cast<uint64_t>(colon): {
						outBuffer[index] = ':';
						++index;
						break;
					}
					case static_cast<uint64_t>(array_start): {
						outBuffer[index] = '[';
						++index;
						break;
					}
					case static_cast<uint64_t>(array_end): {
						outBuffer[index] = ']';
						++index;
						break;
					}
					case static_cast<uint64_t>(null): {
						static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						pow2_memcpy_wrapper<4>(&outBuffer[index], &nullV);
						index += 4;
						break;
					}
					case static_cast<uint64_t>(boolean): {
						if (*previousPtr == 'f') {
							static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							pow2_memcpy_wrapper<8>(&outBuffer[index], &falseV);
							index += 5;
						} else {
							static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							pow2_memcpy_wrapper<4>(&outBuffer[index], &trueV);
							index += 4;
						}
						break;
					}
					case static_cast<uint64_t>(object_start): {
						outBuffer[index] = '{';
						++index;
						break;
					}
					case static_cast<uint64_t>(object_end): {
						outBuffer[index] = '}';
						++index;
						break;
					}
					case static_cast<uint64_t>(unset):
						[[fallthrough]];
					case static_cast<uint64_t>(error):
						[[fallthrough]];
					default: {
						derivedRef.errors.emplace_back(jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter,
							&rootIter[*iter], endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
				if (iter >= endStructural) {
					break;
				}
				previousPtr = rootIter + *iter;
				++iter;
			}
			return index;
		}
	};

}// namespace internal
