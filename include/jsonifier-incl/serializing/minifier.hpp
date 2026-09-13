// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/serializing/minifier.hpp
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

	template<typename derived_type_new> class minifier {
	  public:
		using derived_type = derived_type_new;
		friend derived_type;

		template<string_t string_type> inline base_t<string_type> minifyJson(string_type&& in) noexcept {
			derived_type& selfRef{ getSelfRef() };
			if (selfRef.stringBuffer.size() < in.size()) [[unlikely]] {
				selfRef.stringBuffer.resize(in.size());
			}
			selfRef.errors.clear();
			rootIter = in.data();
			endIter	 = rootIter + in.size();
			selfRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ selfRef.section.begin() };
			structural_index_ptr endStructural = selfRef.section.end();
			base_t<string_type> newString{};
			if (iter == endStructural) {
				getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
			} else {
				auto index = impl(iter, endStructural, selfRef.stringBuffer);
				if (index != std::numeric_limits<uint64_t>::max()) {
					newString.resize(index);
					std::memcpy(newString.data(), selfRef.stringBuffer.data(), index);
				}
			}
			return newString;
		}

		template<string_t input_string_type, string_t output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) noexcept {
			derived_type& selfRef{ getSelfRef() };
			if (selfRef.stringBuffer.size() < in.size()) [[unlikely]] {
				selfRef.stringBuffer.resize(in.size());
			}
			selfRef.errors.clear();
			rootIter = in.data();
			endIter	 = rootIter + in.size();
			selfRef.section.template reset<false>(rootIter, in.size());
			structural_index_ptr iter{ selfRef.section.begin() };
			structural_index_ptr endStructural = selfRef.section.end();
			if (iter == endStructural) {
				getErrors().emplace_back(error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, &rootIter[*iter], endIter));
				return false;
			}
			auto index = impl(iter, endStructural, selfRef.stringBuffer);
			if (index != std::numeric_limits<uint64_t>::max()) [[likely]] {
				if (buffer.size() != index) [[likely]] {
					buffer.resize(index);
				}
				std::memcpy(buffer.data(), selfRef.stringBuffer.data(), index);
				return true;
			} else {
				return false;
			}
		}

	  private:
		string_view_ptr rootIter{};
		string_view_ptr endIter{};

		minifier()								 = default;
		minifier(const minifier&)				 = default;
		minifier& operator=(const minifier&)	 = default;
		minifier(minifier&&) noexcept			 = default;
		minifier& operator=(minifier&&) noexcept = default;
		~minifier()								 = default;

		JSONIFIER_INLINE uint64_t getSize() const {
			return endIter - rootIter;
		}

		JSONIFIER_INLINE void skipWs(int64_t& currentDistance, string_view_ptr previousPtr) noexcept {
			while (whitespaceTable[static_cast<uint8_t>(previousPtr[--currentDistance])]) {
			}
		}

		JSONIFIER_INLINE bool classifyWsMask128(string_view_ptr loadPtr, uint16_t& wsMaskOut) noexcept {
			alignas(64) static constexpr auto wsLutPtr{ whitespaceArray<sizeof(jsonifier_simd_int_128)>.data() };
			const jsonifier_simd_int_128 lutValues{ simd::gatherValues<jsonifier_simd_int_128>(wsLutPtr) };
			const jsonifier_simd_int_128 chunk{ simd::gatherValuesU<jsonifier_simd_int_128>(loadPtr) };
			const jsonifier_simd_int_128 classified{ simd::opShuffle(lutValues, chunk) };
			wsMaskOut = static_cast<uint16_t>(simd::opCmpEq(classified, chunk));
			return true;
		}

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
		JSONIFIER_INLINE bool classifyWsMask256(string_view_ptr loadPtr, uint32_t& wsMaskOut) noexcept {
			alignas(64) static constexpr auto wsLutPtr{ whitespaceArray<sizeof(jsonifier_simd_int_256)>.data() };
			const jsonifier_simd_int_256 lutValues{ simd::gatherValues<jsonifier_simd_int_256>(wsLutPtr) };
			const jsonifier_simd_int_256 chunk{ simd::gatherValuesU<jsonifier_simd_int_256>(loadPtr) };
			const jsonifier_simd_int_256 classified{ simd::opShuffle(lutValues, chunk) };
			wsMaskOut = static_cast<uint32_t>(simd::opCmpEq(classified, chunk));
			return true;
		}
#endif

		JSONIFIER_INLINE string_view_ptr trimTrailingWsBackward(string_view_ptr previousPtr, string_view_ptr boundary) noexcept {
			string_view_ptr cursor{ boundary };

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
			while (static_cast<uint64_t>(cursor - previousPtr) >= 32) {
				string_view_ptr loadPtr{ cursor - 32 };
				uint32_t wsMask{};
				classifyWsMask256(loadPtr, wsMask);
				const uint32_t nonWsMask{ static_cast<uint32_t>(~wsMask) };
				if (nonWsMask != 0) {
					const uint64_t lastNonWs{ 31ull - simd::countlZeroUnsafe(nonWsMask) };
					return loadPtr + lastNonWs + 1;
				}
				cursor = loadPtr;
			}
#endif
			while (static_cast<uint64_t>(cursor - previousPtr) >= 16) {
				string_view_ptr loadPtr{ cursor - 16 };
				uint16_t wsMask{};
				classifyWsMask128(loadPtr, wsMask);
				const uint16_t nonWsMask{ static_cast<uint16_t>(~wsMask) };
				if (nonWsMask != 0) {
					const uint64_t lastNonWs{ 15ull - simd::countlZeroUnsafe(static_cast<uint16_t>(nonWsMask)) };
					return loadPtr + lastNonWs + 1;
				}
				cursor = loadPtr;
			}

			int64_t currentDistance{ cursor - previousPtr };
			if (currentDistance > 0) {
				skipWs(currentDistance, previousPtr);
				++currentDistance;
				return previousPtr + currentDistance;
			}
			return previousPtr;
		}

		JSONIFIER_INLINE string_view_ptr scanNumberForwardToWs(string_view_ptr previousPtr, string_view_ptr boundary) noexcept {
			string_view_ptr cursor{ previousPtr };

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
			while (static_cast<uint64_t>(boundary - cursor) >= 32) {
				uint32_t wsMask{};
				classifyWsMask256(cursor, wsMask);
				if (wsMask != 0) {
					const uint64_t firstWs{ simd::countrZeroUnsafe(wsMask) };
					return cursor + firstWs;
				}
				cursor += 32;
			}
#endif
			while (static_cast<uint64_t>(boundary - cursor) >= 16) {
				uint16_t wsMask{};
				classifyWsMask128(cursor, wsMask);
				const uint16_t wsMask16{ static_cast<uint16_t>(wsMask) };
				if (wsMask16 != 0) {
					const uint64_t firstWs{ simd::countrZeroUnsafe(wsMask16) };
					return cursor + firstWs;
				}
				cursor += 16;
			}

			while (cursor < boundary && !whitespaceTable[static_cast<uint8_t>(*cursor)]) {
				++cursor;
			}
			return cursor;
		}

		template<typename iterator_type> JSONIFIER_INLINE void backTrackWs(int64_t& currentDistance, string_view_ptr& previousPtr, iterator_type iter) noexcept {
			string_view_ptr boundary{ rootIter + *iter };
			string_view_ptr contentEnd{ trimTrailingWsBackward(previousPtr, boundary) };
			currentDistance = contentEnd - previousPtr;
		}

		template<string_t string_type, typename iterator, typename iterator_end>
		inline uint64_t impl(iterator* __restrict& iter, iterator_end* __restrict endStructural, string_type&& outBuffer) noexcept {
			using enum json_structural_type;
			auto previousPtr = rootIter + *iter;
			int64_t currentDistance{};
			uint64_t index{};
			++iter;

			while (true) {
				switch (static_cast<uint64_t>(jsonTypes[static_cast<uint8_t>(*previousPtr)])) {
					case static_cast<uint64_t>(string): {
						backTrackWs(currentDistance, previousPtr, iter);
						if (currentDistance > 0) [[likely]] {
							std::memcpy(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							getErrors().emplace_back(
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
						string_view_ptr boundary{ rootIter + *iter };
						string_view_ptr numEnd{ scanNumberForwardToWs(previousPtr, boundary) };
						currentDistance = numEnd - previousPtr;
						if (currentDistance > 0) [[likely]] {
							std::memcpy(&outBuffer[index], previousPtr, static_cast<uint64_t>(currentDistance));
							index += static_cast<uint64_t>(currentDistance);
						} else {
							getErrors().emplace_back(
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
						alignas(64) static constexpr uint32_t nullV{ pack_values<string_literal{ "null" }>::value };
						std::memcpy(&outBuffer[index], &nullV, 4);
						index += 4;
						break;
					}
					case static_cast<uint64_t>(boolean): {
						if (*previousPtr == 'f') {
							alignas(64) static constexpr uint64_t falseV{ pack_values<string_literal{ "false" }>::value };
							std::memcpy(&outBuffer[index], &falseV, 8);
							index += 5;
						} else {
							alignas(64) static constexpr uint32_t trueV{ pack_values<string_literal{ "true" }>::value };
							std::memcpy(&outBuffer[index], &trueV, 4);
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
						getErrors().emplace_back(jsonifier::internal::error::constructError<status_classes::minifying, minify_statuses::incorrect_structural_index>(rootIter,
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

		JSONIFIER_INLINE derived_type& getSelfRef() noexcept {
			return *static_cast<derived_type*>(this);
		}

		inline std::vector<error>& getErrors() noexcept {
			derived_type& selfRef{ getSelfRef() };
			return selfRef.errors;
		}
	};

}// namespace internal
