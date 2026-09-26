/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/prettifier.hpp
 */
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

	template<prettify_options options, typename prettifier_type> struct prettify_context_ro {
		inline prettify_context_ro(prettifier_type& prettifierNew, structural_index_ptr iterNew, structural_index_ptr endStructuralNew, read_buffer_ptr dataPtrNew,
			read_buffer_ptr rootIterNew, read_buffer_ptr endIterNew) noexcept
			: endStructural{ endStructuralNew }, prettifier{ prettifierNew }, iter{ iterNew }, rootIter{ rootIterNew }, dataPtr{ dataPtrNew }, endIter{ endIterNew } {
		}

		JSONIFIER_INLINE uint64_t operator()(write_buffer_ptr __restrict ptrNew, uint64_t) noexcept {
			const auto index = prettifier.template impl<options>(iter, endStructural, dataPtr, ptrNew, rootIter, endIter);
			return index != std::numeric_limits<uint64_t>::max() ? index : 0;
		}

		prettify_context_ro& operator=(const prettify_context_ro&) noexcept = delete;
		prettify_context_ro& operator=(prettify_context_ro&&) noexcept		= delete;
		prettify_context_ro(const prettify_context_ro&) noexcept			= delete;
		prettify_context_ro(prettify_context_ro&&) noexcept					= delete;
		prettify_context_ro() noexcept										= delete;

		structural_index_ptr endStructural{};
		prettifier_type& prettifier;
		structural_index_ptr iter{};
		read_buffer_ptr rootIter{};
		read_buffer_ptr dataPtr{};
		read_buffer_ptr endIter{};
	};

	template<typename derived_type_new> struct prettifier {
		using derived_type = derived_type_new;
		template<prettify_options options, typename prettifier_type> friend struct prettify_context_ro;

		template<prettify_options options = prettify_options{}, string_t input_string_type, string_t output_buffer_type>
		inline bool prettifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			static constexpr prettify_options optionsFinal{ options };
			derivedRef.errors.clear();
			const auto* dataPtr		 = in.data();
			read_buffer_ptr rootIter = dataPtr;
			read_buffer_ptr endIter	 = dataPtr + in.size();
			derivedRef.section.template reset<true>(dataPtr, in.size());
			structural_index_ptr iter{ derivedRef.section.begin() };
			auto* endStructural = derivedRef.section.end();
			if (iter == endStructural) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::prettifying, prettify_statuses::no_input>(rootIter, rootIter, endIter));
				return false;
			}
			uint64_t depth{};
			uint64_t maxDepth{};
			for (auto* structural = iter; structural != endStructural; ++structural) {
				switch (dataPtr[*structural]) {
					case '[':
					case '{': {
						++depth;
						maxDepth = depth > maxDepth ? depth : maxDepth;
						break;
					}
					case ']':
					case '}': {
						depth -= depth > 0;
						break;
					}
					default: {
						break;
					}
				}
			}
			const uint64_t structuralCount = static_cast<uint64_t>(endStructural - iter);
			const uint64_t requiredSize	   = in.size() + structuralCount * (maxDepth * optionsFinal.indentSize + 2);
			using context_type			   = prettify_context_ro<optionsFinal, remove_reference_t<decltype(*this)>>;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<output_buffer_type>>) {
				buffer.resize_and_overwrite(requiredSize, context_type{ *this, iter, endStructural, dataPtr, rootIter, endIter });
			} else {
				if (buffer.size() < requiredSize) {
					buffer.resize(requiredSize);
				}
				context_type context{ *this, iter, endStructural, dataPtr, rootIter, endIter };
				buffer.resize(context(buffer.data(), requiredSize));
			}
			return !buffer.empty();
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		prettifier& operator=(const prettifier& other) = delete;
		prettifier& operator=(prettifier&& other)	   = delete;
		prettifier(const prettifier& other)			   = delete;
		prettifier(prettifier&& other)				   = delete;
		inline ~prettifier() noexcept				   = default;
		inline prettifier() noexcept				   = default;

		template<prettify_options options, prettify_buffer_t string_type, typename iterator, typename iterator_end> inline uint64_t impl(iterator* __restrict& iter,
			iterator_end* __restrict endStructural, read_buffer_ptr __restrict stringRootIter, string_type&& outBuffer, read_buffer_ptr rootIter,
			read_buffer_ptr endIter) noexcept {
			using comma_indent = indent_table<",\n", options.indentChar, options.indentSize>;
			using open_indent  = indent_table<"\n", options.indentChar, options.indentSize>;
			using close_indent = indent_table<"\n", options.indentChar, options.indentSize>;
			using enum json_structural_type;
			read_buffer_ptr newPtr{};
			prettify_status status{};
			while (iter < endStructural) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(stringRootIter[*iter])])) {
					case static_cast<uint64_t>(string): {
						newPtr = stringRootIter + *iter;
						++iter;
						status.newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						memcpyWrapper(&outBuffer[status.index], newPtr, status.newSize);
						status.index += status.newSize;
						break;
					}
					case static_cast<uint64_t>(comma): {
						write_buffer_ptr outPtr = outBuffer + status.index;
						comma_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index = static_cast<uint64_t>(outPtr - outBuffer);
						++iter;
						break;
					}
					case static_cast<uint64_t>(number): {
						newPtr = stringRootIter + *iter;
						++iter;
						status.newSize = static_cast<uint64_t>((stringRootIter + *iter) - newPtr);
						memcpyWrapper(&outBuffer[status.index], newPtr, status.newSize);
						status.index += status.newSize;
						break;
					}
					case static_cast<uint64_t>(colon): {
						static constexpr char valuesNew[3]{ ':', options.indentChar };
						alignas(64) static constexpr uint16_t colonIndentChar{ pack_values<string_literal{ valuesNew }>::value };
						pow2MemcpyWrapper<2>(&outBuffer[status.index], &colonIndentChar);
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
							write_buffer_ptr outPtr = outBuffer + status.index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
							status.index = static_cast<uint64_t>(outPtr - outBuffer);
						} else {
							--status.array_depth;
							status.indent -= options.indentSize;
							outBuffer[status.index] = ']';
							++status.index;
							if (status.indent < 0) {
								derivedRef.errors.emplace_back(
									jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(rootIter,
										&rootIter[*iter], endIter));
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
							derivedRef.errors.emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
								rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						write_buffer_ptr outPtr = outBuffer + status.index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index			= static_cast<uint64_t>(outPtr - outBuffer);
						outBuffer[status.index] = ']';
						++status.index;
						++iter;
						break;
					}
					case static_cast<uint64_t>(null): {
						static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						pow2MemcpyWrapper<4>(&outBuffer[status.index], &nullV);
						status.index += 4;
						++iter;
						break;
					}
					case static_cast<uint64_t>(boolean): {
						if (stringRootIter[*iter] == 'f') {
							static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							pow2MemcpyWrapper<8>(&outBuffer[status.index], &falseV);
							status.index += 5;
							++iter;
						} else {
							static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							pow2MemcpyWrapper<4>(&outBuffer[status.index], &trueV);
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
							write_buffer_ptr outPtr = outBuffer + status.index;
							open_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
							status.index = static_cast<uint64_t>(outPtr - outBuffer);
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
							derivedRef.errors.emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
								rootIter, &rootIter[*iter], endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						write_buffer_ptr outPtr = outBuffer + status.index;
						close_indent::blitWithOverflow(outPtr, static_cast<uint64_t>(status.indent));
						status.index			= static_cast<uint64_t>(outPtr - outBuffer);
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
						derivedRef.errors.emplace_back(jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::incorrect_structural_index>(
							rootIter, &rootIter[*iter], endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
			}
			if (status.array_depth > 0 || status.object_depth > 0) {
				if (status.array_depth > 0) {
					derivedRef.errors.emplace_back(
						jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::unclosed_array>(rootIter, &rootIter[*iter], endIter));
				} else if (status.object_depth > 0) {
					derivedRef.errors.emplace_back(
						jsonifier::internal::error::constructError<status_classes::prettifying, prettify_statuses::unclosed_object>(rootIter, &rootIter[*iter], endIter));
				}
				status.index = 0;
				return std::numeric_limits<uint64_t>::max();
			}
			return status.index;
		}
	};

}// namespace internal
