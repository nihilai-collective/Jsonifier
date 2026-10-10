/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/minifier.hpp
 */
#if !defined(JSONIFIER_PASS_GUARD_MINIFIER)
	#define JSONIFIER_PASS_GUARD_MINIFIER

	#include <jsonifier-incl/utilities/compare.hpp>
	#include <jsonifier-incl/utilities/simd.hpp>

namespace JSONIFIER_INTERNAL_NAMESPACE {

	constexpr array<json_structural_type, 256ULL> genJsonTypes() {
		array<json_structural_type, 256ULL> returnValues{};
		using enum json_structural_type;
		returnValues[static_cast<uint64_t>('"')] = string;
		returnValues[static_cast<uint64_t>(',')] = comma;
		returnValues[static_cast<uint64_t>('0')] = number;
		returnValues[static_cast<uint64_t>('1')] = number;
		returnValues[static_cast<uint64_t>('2')] = number;
		returnValues[static_cast<uint64_t>('3')] = number;
		returnValues[static_cast<uint64_t>('4')] = number;
		returnValues[static_cast<uint64_t>('5')] = number;
		returnValues[static_cast<uint64_t>('6')] = number;
		returnValues[static_cast<uint64_t>('7')] = number;
		returnValues[static_cast<uint64_t>('8')] = number;
		returnValues[static_cast<uint64_t>('9')] = number;
		returnValues[static_cast<uint64_t>('-')] = number;
		returnValues[static_cast<uint64_t>(':')] = colon;
		returnValues[static_cast<uint64_t>('[')] = array_start;
		returnValues[static_cast<uint64_t>(']')] = array_end;
		returnValues[static_cast<uint64_t>('n')] = null;
		returnValues[static_cast<uint64_t>('t')] = boolean;
		returnValues[static_cast<uint64_t>('f')] = boolean;
		returnValues[static_cast<uint64_t>('{')] = object_start;
		returnValues[static_cast<uint64_t>('}')] = object_end;
		return returnValues;
	}

	alignas(64) inline constexpr const json_structural_type* __restrict jsonTypes{ []() constexpr {
		constexpr auto local{ genJsonTypes() };
		return make_static<local>::value.data();
	}() };

	template<typename minifier_type> struct minify_context_ro {
		inline minify_context_ro(minifier_type& minifierNew, write_structural_index_ptr iterNew, write_structural_index_ptr endStructuralNew, read_buffer_ptr rootIterNew,
			read_buffer_ptr endIterNew) noexcept
			: endStructural{ endStructuralNew }, iter{ iterNew }, rootIter{ rootIterNew }, endIter{ endIterNew }, minifier{ minifierNew } {
		}

		JSONIFIER_INLINE uint64_t operator()(char* __restrict ptrNew, uint64_t) noexcept {
			const auto index = minifier.impl(iter, endStructural, std::bit_cast<write_buffer_ptr>(ptrNew), rootIter, endIter);
			return index != std::numeric_limits<uint64_t>::max() ? index : 0;
		}

		minify_context_ro& operator=(const minify_context_ro&) noexcept = delete;
		minify_context_ro& operator=(minify_context_ro&&) noexcept		= delete;
		minify_context_ro(const minify_context_ro&) noexcept			= delete;
		minify_context_ro(minify_context_ro&&) noexcept					= delete;
		minify_context_ro() noexcept									= delete;

		write_structural_index_ptr endStructural{};
		write_structural_index_ptr iter{};
		read_buffer_ptr rootIter{};
		read_buffer_ptr endIter{};
		minifier_type& minifier;
	};

	template<typename derived_type_new> struct minifier {
		using derived_type = derived_type_new;
		template<typename prettifier_type> friend struct minify_context_ro;

		template<string_t input_string_type, string_t output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			derivedRef.errors.clear();
			read_buffer_ptr rootIter = std::bit_cast<read_buffer_ptr>(in.data());
			read_buffer_ptr endIter	 = rootIter + in.size();
			derivedRef.section.template reset<false>(rootIter, in.size());
			write_structural_index_ptr iter{ derivedRef.section.begin() };
			write_structural_index_ptr endStructural = derivedRef.section.end();
			if (iter == endStructural) [[unlikely]] {
				derivedRef.errors.emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, rootIter, endIter));
				return false;
			}
			const uint64_t requiredSize = in.size();
			using context_type			= minify_context_ro<remove_reference_t<decltype(*this)>>;
			if constexpr (has_resize_and_overwrite<remove_cvref_t<output_buffer_type>>) {
				buffer.resize_and_overwrite(requiredSize, context_type{ *this, iter, endStructural, rootIter, endIter });
			} else {
				if (buffer.size() < requiredSize) {
					buffer.resize(requiredSize);
				}
				context_type context{ *this, iter, endStructural, rootIter, endIter };
				buffer.resize(context(buffer.data(), requiredSize));
			}
			return !buffer.empty();
		}

	  protected:
		derived_type& derivedRef{ *static_cast<derived_type*>(this) };

		minifier& operator=(const minifier& other) = delete;
		minifier& operator=(minifier&& other)	   = delete;
		minifier(const minifier& other)			   = delete;
		minifier(minifier&& other)				   = delete;
		inline ~minifier() noexcept				   = default;
		inline minifier() noexcept				   = default;

		template<prettify_buffer_t string_type, typename iterator, typename iterator_end> inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural,
			string_type&& outBuffer, read_buffer_ptr rootIter, read_buffer_ptr endIter) noexcept {
			using enum json_structural_type;
			read_buffer_ptr previousPtr{};
			int64_t currentDistance{};
			int64_t arrayDepth{};
			int64_t objectDepth{};
			uint64_t index{};
			while (iter < endStructural) {
				previousPtr = rootIter + *iter;
				++iter;
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(*previousPtr)])) {
					case static_cast<uint64_t>(string): {
						backTrackWs(currentDistance, previousPtr, iter, rootIter);
						if (currentDistance > 0) [[likely]] {
							jsonifierMemcpy(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							derivedRef.errors.emplace_back(
								jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::invalid_string_length>(rootIter, previousPtr, endIter));
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
						jsonifierMemcpy(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
						index += static_cast<uint64_t>(currentDistance);
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
						++arrayDepth;
						break;
					}
					case static_cast<uint64_t>(array_end): {
						if (--arrayDepth < 0) [[unlikely]] {
							derivedRef.errors.emplace_back(
								jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter, previousPtr, endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						outBuffer[index] = ']';
						++index;
						break;
					}
					case static_cast<uint64_t>(null): {
						static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						pow2MemcpyWrapper<4>(&outBuffer[index], &nullV);
						index += 4;
						break;
					}
					case static_cast<uint64_t>(boolean): {
						if (*previousPtr == 'f') {
							static constexpr uint32_t falsV{ pack_values<string_literal{ "fals" }>::value };
							pow2MemcpyWrapper<4>(&outBuffer[index], &falsV);
							outBuffer[index + 4] = 'e';
							index += 5;
						} else {
							static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							pow2MemcpyWrapper<4>(&outBuffer[index], &trueV);
							index += 4;
						}
						break;
					}
					case static_cast<uint64_t>(object_start): {
						outBuffer[index] = '{';
						++index;
						++objectDepth;
						break;
					}
					case static_cast<uint64_t>(object_end): {
						if (--objectDepth < 0) [[unlikely]] {
							derivedRef.errors.emplace_back(
								jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter, previousPtr, endIter));
							return std::numeric_limits<uint64_t>::max();
						}
						outBuffer[index] = '}';
						++index;
						break;
					}
					case static_cast<uint64_t>(unset):
						[[fallthrough]];
					case static_cast<uint64_t>(error):
						[[fallthrough]];
					default: {
						derivedRef.errors.emplace_back(
							jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter, previousPtr, endIter));
						return std::numeric_limits<uint64_t>::max();
					}
				}
			}
			if ((arrayDepth | objectDepth) != 0) [[unlikely]] {
				if (arrayDepth != 0) {
					derivedRef.errors.emplace_back(
						jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::unclosed_array>(rootIter, previousPtr, endIter));
				} else {
					derivedRef.errors.emplace_back(
						jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::unclosed_object>(rootIter, previousPtr, endIter));
				}
				return std::numeric_limits<uint64_t>::max();
			}
			return index;
		}

		template<typename iterator_type>
		JSONIFIER_INLINE void backTrackWs(int64_t& currentDistance, read_buffer_ptr& previousPtr, iterator_type iter, read_buffer_ptr rootIter) noexcept {
			currentDistance = (rootIter + *iter) - previousPtr;
			skipWs(currentDistance, previousPtr);
			++currentDistance;
		}

		JSONIFIER_INLINE void skipWs(int64_t& currentDistance, read_buffer_ptr previousPtr) noexcept {
			while (whitespaceTable[static_cast<uint8_t>(previousPtr[--currentDistance])]) {
			}
		}
	};

}// namespace internal

#endif
