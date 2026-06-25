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
		inline minifier& operator=(const minifier& other) = delete;
		inline minifier(const minifier& other)			  = delete;

		template<concepts::string_t string_type> inline auto minifyJson(string_type&& in) noexcept {
			if (derivedRef.stringBuffer.size() < in.size()) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size());
			}
			derivedRef.errors.clear();
			rootIter = in.data();
			endIter	 = rootIter + in.size();
			derivedRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			structural_index_ptr endStructural = derivedRef.section.end();
			jsonifier::internal::remove_cvref_t<string_type> newString{};
			if (iter == endStructural) {
				getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return newString;
			}
			auto index = impl(iter, endStructural, derivedRef.stringBuffer);
			if (index != std::numeric_limits<uint64_t>::max()) {
				newString.resize(index);
				std::memcpy(newString.data(), derivedRef.stringBuffer.data(), index);
				return newString;
			} else {
				return jsonifier::internal::remove_cvref_t<string_type>{};
			}
		}

		template<concepts::string_t input_string_type, concepts::string_t output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			if (derivedRef.stringBuffer.size() < in.size()) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size());
			}
			derivedRef.errors.clear();
			rootIter = in.data();
			endIter	 = rootIter + in.size();
			derivedRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			structural_index_ptr endStructural = derivedRef.section.end();
			if (iter == endStructural) {
				getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return false;
			}
			auto index = impl(iter, endStructural, derivedRef.stringBuffer);
			if (index != std::numeric_limits<uint64_t>::max()) [[likely]] {
				if (buffer.size() != index) [[likely]] {
					buffer.resize(index);
				}
				std::memcpy(buffer.data(), derivedRef.stringBuffer.data(), index);
				return true;
			} else {
				return false;
			}
		}

	  protected:
		derived_type& derivedRef{ initializeSelfRef() };
		string_view_ptr rootIter{};
		string_view_ptr endIter{};

		inline uint64_t getSize() const {
			return endIter - rootIter;
		}

		inline void skipWs(int64_t& currentDistance, string_view_ptr previousPtr) noexcept {
			while (whitespaceTable[static_cast<uint8_t>(previousPtr[--currentDistance])]) {
			}
		}

		template<typename iterator_type> inline void backTrackWs(int64_t& currentDistance, string_view_ptr& previousPtr, iterator_type iter) noexcept {
			currentDistance = (rootIter + *iter) - previousPtr;
			skipWs(currentDistance, previousPtr);
			++currentDistance;
		}

		template<concepts::string_t string_type, typename iterator, typename iterator_end>
		inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural, string_type&& out) noexcept {
			auto previousPtr = rootIter + *iter;
			int64_t currentDistance{};
			uint64_t index{};
			++iter;

			while (iter < endStructural) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(*previousPtr)])) {
					case static_cast<uint64_t>(json_structural_type::string): {
						backTrackWs(currentDistance, previousPtr, iter);
						if (currentDistance > 0) [[likely]] {
							std::memcpy(&out[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::invalid_string_length>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::comma): {
						out[index] = ',';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::number): {
						currentDistance = 0;
						while (!whitespaceTable[static_cast<uint8_t>(previousPtr[++currentDistance])] && ((previousPtr + currentDistance) < (rootIter + *iter))) {
						}
						if (currentDistance > 0) [[likely]] {
							std::memcpy(&out[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::invalid_number_value>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::colon): {
						out[index] = ':';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::array_start): {
						out[index] = '[';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::array_end): {
						out[index] = ']';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::null): {
						alignas(64) static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						std::memcpy(&out[index], &nullV, 4);
						index += 4;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::boolean): {
						if (*previousPtr == 'f') {
							alignas(64) static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							std::memcpy(&out[index], &falseV, 5);
							index += 5;
						} else {
							alignas(64) static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							std::memcpy(&out[index], &trueV, 4);
							index += 4;
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::object_start): {
						out[index] = '{';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::object_end): {
						out[index] = '}';
						++index;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::unset):
						[[fallthrough]];
					case static_cast<uint64_t>(json_structural_type::error):
						[[fallthrough]];
					default: {
						getErrors().emplace_back(
							error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter, &rootIter[*iter], endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
				previousPtr = rootIter + *iter;
				++iter;
			}
			if (previousPtr) {
				out[index] = *previousPtr;
				++index;
			}
			return index;
		}

		inline minifier() noexcept : derivedRef{ initializeSelfRef() } {
		}

		inline derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		inline std::vector<error>& getErrors() noexcept {
			return derivedRef.errors;
		}

		inline ~minifier() noexcept = default;
	};

}// namespace internal
