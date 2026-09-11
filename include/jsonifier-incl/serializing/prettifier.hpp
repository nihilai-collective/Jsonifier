// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/serializing/prettifier.hpp
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

	struct prettify_status {
		uint64_t object_depth{};
		uint64_t array_depth{};
		uint64_t newSize{};
		uint64_t index{};
		int64_t indent{};
	};

	template<typename derived_type> class prettifier {
	  public:
		inline prettifier& operator=(const prettifier& other) = delete;
		inline prettifier(const prettifier& other)			  = delete;

		template<prettify_options options = prettify_options{}, string_t string_type> inline base_t<string_type> prettifyJson(string_type&& in) noexcept {
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
			base_t<string_type> newString{};
			if (iter == endStructural) [[unlikely]] {
				getErrors().emplace_back(error::constructError<status_classes::prettifying, prettify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
			} else {
				auto index = impl<optionsFinal>(iter, endStructural, dataPtr, derivedRef.stringBuffer);
				if (index != std::numeric_limits<uint64_t>::max()) [[likely]] {
					newString.resize(index);
					std::memcpy(newString.data(), derivedRef.stringBuffer.data(), index);
				}
			}
			return newString;
		}

		template<prettify_options options = prettify_options{}, string_t input_string_type, string_t output_buffer_type>
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
		string_view_ptr rootIter{};
		string_view_ptr endIter{};

		inline prettifier() noexcept {
		}

		inline derived_type& initializeSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		inline std::vector<error>& getErrors() noexcept {
			return derivedRef.errors;
		}

		template<prettify_options options, string_t string_type, typename iterator, typename iterator_end>
		inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural, string_view_ptr __restrict stringRootIter, string_type&& outBuffer) noexcept {
			using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
			using open_indent  = indent_table<"\n", options.indentChar, options.indentSize>;
			using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;
			using enum json_structural_type;
			string_view_ptr newPtr{};
			prettify_status status{};
			while (iter < endStructural) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(stringRootIter[*iter])])) {
					case static_cast<uint64_t>(string): {
						newPtr = stringRootIter + *iter;
						++iter;
						status.newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						std::memcpy(&outBuffer[status.index], newPtr, status.newSize);
						status.index += status.newSize;
						break;
					}
					case static_cast<uint64_t>(comma): {
						string_buffer_ptr outPtr = outBuffer.data() + status.index;
						comma_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index = static_cast<uint64_t>(outPtr - outBuffer.data());
						++iter;
						break;
					}
					case static_cast<uint64_t>(number): {
						newPtr = stringRootIter + *iter;
						++iter;
						status.newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						std::memcpy(&outBuffer[status.index], newPtr, status.newSize);
						status.index += status.newSize;
						break;
					}
					case static_cast<uint64_t>(colon): {
						static constexpr char valuesNew[3]{ ':', options.indentChar };
						alignas(64) static constexpr uint16_t colonIndentChar{ pack_values<string_literal{ valuesNew }>::value };
						std::memcpy(&outBuffer[status.index], &colonIndentChar, 2);
						status.index += 2;
						++iter;
						break;
					}
					case static_cast<uint64_t>(array_start): {
						outBuffer[status.index] = '[';
						++status.index;
						++iter;
						++status.array_depth;
						status.indent += options.indentSize;
						if (stringRootIter[*iter] != ']') [[likely]] {
							string_buffer_ptr outPtr = outBuffer.data() + status.index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
							status.index = static_cast<uint64_t>(outPtr - outBuffer.data());
						} else {
							--status.array_depth;
							status.indent -= options.indentSize;
							outBuffer[status.index] = ']';
							++status.index;
							if (status.indent < 0) {
								getErrors().emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
									rootIter, &rootIter[*iter], endIter));
								return std::numeric_limits<uint64_t>::max();
							}
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(array_end): {
						status.indent -= options.indentSize;
						--status.array_depth;
						if (status.indent < 0) {
							getErrors().emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
								rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						string_buffer_ptr outPtr = outBuffer.data() + status.index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index	 = static_cast<uint64_t>(outPtr - outBuffer.data());
						outBuffer[status.index] = ']';
						++status.index;
						++iter;
						break;
					}
					case static_cast<uint64_t>(null): {
						alignas(64) static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						std::memcpy(&outBuffer[status.index], &nullV, 4);
						status.index += 4;
						++iter;
						break;
					}
					case static_cast<uint64_t>(boolean): {
						if (stringRootIter[*iter] == 'f') {
							alignas(64) static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							std::memcpy(&outBuffer[status.index], &falseV, 8);
							status.index += 5;
							++iter;
						} else {
							alignas(64) static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							std::memcpy(&outBuffer[status.index], &trueV, 4);
							status.index += 4;
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(object_start): {
						outBuffer[status.index] = '{';
						++status.object_depth;
						++status.index;
						++iter;
						status.indent += options.indentSize;
						if (stringRootIter[*iter] != '}') {
							string_buffer_ptr outPtr = outBuffer.data() + status.index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
							status.index = static_cast<uint64_t>(outPtr - outBuffer.data());
						} else {
							--status.object_depth;
							status.indent -= options.indentSize;
							outBuffer[status.index] = '}';
							++status.index;
							++iter;
						}
						break;
					}
					case static_cast<uint64_t>(object_end): {
						status.indent -= options.indentSize;
						--status.object_depth;
						if (status.indent < 0) {
							getErrors().emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
								rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						string_buffer_ptr outPtr = outBuffer.data() + status.index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index	 = static_cast<uint64_t>(outPtr - outBuffer.data());
						outBuffer[status.index] = '}';
						++status.index;
						++iter;
						break;
					}
					case static_cast<uint64_t>(unset):
						[[fallthrough]];
					case static_cast<uint64_t>(error):
						[[fallthrough]];
					default: {
						getErrors().emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter,
							&rootIter[*iter], endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
			}
			if (status.array_depth > 0 || status.object_depth > 0) {
				if (status.array_depth > 0) {
					getErrors().emplace_back(
						jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::unclosed_array>(rootIter, &rootIter[*iter], endIter));
				} else if (status.object_depth > 0) {
					getErrors().emplace_back(
						jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::unclosed_object>(rootIter, &rootIter[*iter], endIter));
				}
				status.index = 0;
				return std::numeric_limits<uint64_t>::max();
			}
			return status.index;
		}

		inline ~prettifier() noexcept = default;
	};

}// namespace internal
