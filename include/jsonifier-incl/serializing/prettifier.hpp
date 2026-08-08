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

#include <jsonifier-incl/serializing/serialize_impl.hpp>
#include <jsonifier-incl/serializing/minifier.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/utilities/compare.hpp>

namespace jsonifier {

	struct prettify_options {
		uint64_t indentSize{ 3 };
		char indentChar{ ' ' };
	};

}

namespace jsonifier::internal {

	template<typename derived_type> class prettifier {
	  public:
		inline prettifier& operator=(const prettifier& other) = delete;
		inline prettifier(const prettifier& other)			  = delete;

		template<prettify_options options = prettify_options{}, concepts::string_t string_type> inline auto prettifyJson(string_type&& in) noexcept {
			if (derivedRef.stringBuffer.size() < in.size() * 5) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size() * 5);
			}
			static constexpr prettify_options optionsFinal{ options };
			const auto* dataPtr = in.data();
			derivedRef.errors.clear();
			rootIter = dataPtr;
			endIter	 = dataPtr + in.size();
			derivedRef.section.template reset<true>(dataPtr, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			auto* endStructural = derivedRef.section.end();
			if (iter == endStructural) [[unlikely]] {
				getErrors().emplace_back(error::constructError<status_classes::prettifying, prettify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return jsonifier::internal::remove_cvref_t<string_type>{};
			}
			jsonifier::internal::remove_cvref_t<string_type> newString{};
			auto index = impl<optionsFinal>(iter, endStructural, dataPtr, derivedRef.stringBuffer);
			if (index != std::numeric_limits<uint64_t>::max()) [[likely]] {
				newString.resize(index);
				std::memcpy(newString.data(), derivedRef.stringBuffer.data(), index);
				return newString;
			} else {
				return jsonifier::internal::remove_cvref_t<string_type>{};
			}
		}

		template<prettify_options options = prettify_options{}, concepts::string_t input_string_type, concepts::string_t output_buffer_type>
		inline bool prettifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			if (derivedRef.stringBuffer.size() < in.size() * 5) [[unlikely]] {
				derivedRef.stringBuffer.resize(in.size() * 5);
			}
			static constexpr prettify_options optionsFinal{ options };
			derivedRef.errors.clear();
			const auto* dataPtr = in.data();
			rootIter			= dataPtr;
			endIter				= dataPtr + in.size();
			derivedRef.section.template reset<true>(dataPtr, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			auto* endStructural = derivedRef.section.end();
			if (iter == endStructural) [[unlikely]] {
				getErrors().emplace_back(error::constructError<status_classes::prettifying, prettify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return false;
			}
			auto index = impl<optionsFinal>(iter, endStructural, dataPtr, derivedRef.stringBuffer);
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
		std::vector<json_structural_type> state{};
		string_view_ptr rootIter{};
		string_view_ptr endIter{};

		inline prettifier() noexcept {
			state.resize(64);
		}

		inline derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		inline std::vector<error>& getErrors() noexcept {
			return derivedRef.errors;
		}

		template<prettify_options options, concepts::string_t string_type, typename iterator, typename iterator_end>
		inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural, string_view_ptr __restrict stringRootIter, string_type&& out) noexcept {
			using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
			using open_indent  = indent_table<"\n", options.indentChar, options.indentSize>;
			using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;

			string_view_ptr newPtr{};
			uint64_t newSize{};
			int64_t indent{};
			int64_t depth{};
			uint64_t index{};
			while (iter < endStructural) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(stringRootIter[*iter])])) {
					case static_cast<uint64_t>(json_structural_type::string): {
						newPtr = stringRootIter + *iter;
						++iter;
						newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						std::memcpy(&out[index], newPtr, newSize);
						index += newSize;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::comma): {
						string_buffer_ptr outPtr = out.data() + index;
						comma_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(indent));
						index = static_cast<uint64_t>(outPtr - out.data());
						++iter;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::number): {
						newPtr = stringRootIter + *iter;
						++iter;
						newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						std::memcpy(&out[index], newPtr, newSize);
						index += newSize;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::colon): {
						static constexpr char valuesNew[3]{ ':', options.indentChar };
						alignas(64) static constexpr uint16_t colonIndentChar{ pack_values<string_literal{ valuesNew }>::value };
						std::memcpy(&out[index], &colonIndentChar, 2);
						index += 2;
						++iter;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::array_start): {
						out[index] = '[';
						++index;
						++iter;
						indent += options.indentSize;
						if (static_cast<uint64_t>(depth) >= state.size()) [[unlikely]] {
							state.resize(state.size() * 2);
						}
						state[static_cast<uint64_t>(depth)] = json_structural_type::array_start;
						++depth;
						if (stringRootIter[*iter] != ']') [[likely]] {
							string_buffer_ptr outPtr = out.data() + index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(indent));
							index = static_cast<uint64_t>(outPtr - out.data());
						} else {
							indent -= options.indentSize;
							--depth;
							out[index] = ']';
							++index;
							if (indent < 0) {
								getErrors().emplace_back(
									error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter, &rootIter[*iter], endIter));
								return std::numeric_limits<uint64_t>::max();
							}
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::array_end): {
						indent -= options.indentSize;
						--depth;
						if (indent < 0) {
							getErrors().emplace_back(
								error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						string_buffer_ptr outPtr = out.data() + index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(indent));
						index	   = static_cast<uint64_t>(outPtr - out.data());
						out[index] = ']';
						++index;
						++iter;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::null): {
						alignas(64) static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						std::memcpy(&out[index], &nullV, 4);
						index += 4;
						++iter;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::boolean): {
						if (stringRootIter[*iter] == 'f') {
							alignas(64) static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							std::memcpy(&out[index], &falseV, 5);
							index += 5;
							++iter;
						} else {
							alignas(64) static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							std::memcpy(&out[index], &trueV, 4);
							index += 4;
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::object_start): {
						out[index] = '{';
						++index;
						++iter;
						indent += options.indentSize;
						if (static_cast<uint64_t>(depth) >= state.size()) [[unlikely]] {
							state.resize(state.size() * 2);
						}
						state[static_cast<uint64_t>(depth)] = json_structural_type::object_start;
						++depth;
						if (stringRootIter[*iter] != '}') {
							string_buffer_ptr outPtr = out.data() + index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(indent));
							index = static_cast<uint64_t>(outPtr - out.data());
						} else {
							--depth;
							out[index] = '}';
							++index;
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(json_structural_type::object_end): {
						indent -= options.indentSize;
						--depth;
						if (indent < 0) {
							getErrors().emplace_back(
								error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						string_buffer_ptr outPtr = out.data() + index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(indent));
						index	   = static_cast<uint64_t>(outPtr - out.data());
						out[index] = '}';
						++index;
						++iter;
						break;
					}
					case static_cast<uint64_t>(json_structural_type::unset):
						[[fallthrough]];
					case static_cast<uint64_t>(json_structural_type::error):
						[[fallthrough]];
					default: {
						getErrors().emplace_back(
							error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter, &rootIter[*iter], endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
			}
			return index;
		}

		inline ~prettifier() noexcept = default;
	};

}// namespace internal
