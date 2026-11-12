/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/json_iterator.hpp
 */
#pragma once

#include <jsonifier-incl/core/defines.hpp>

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/simd.hpp>
#include <jsonifier-incl/utilities/string_utils.hpp>
#include <jsonifier-incl/utilities/error.hpp>

namespace jsonifier::internal {

	template<string_literal stringNew> JSONIFIER_INLINE bool compareStringAsInt(read_buffer_ptr src) noexcept;

	template<typename basic_iterator01> [[maybe_unused]] JSONIFIER_INLINE void skipStringImpl(basic_iterator01& string1, uint64_t lengthNew) noexcept;

	template<parse_options parseOpts, typename string_type> struct iterate_string_context {
		using scan_result = typename string_scanner<parseOpts>::scan_result;

		iterate_string_context() noexcept = delete;

		JSONIFIER_INLINE iterate_string_context(scan_result& resultNew, read_buffer_ptr strPtrNew, read_buffer_ptr endPtrNew) noexcept
			: strPtr{ strPtrNew }, endPtr{ endPtrNew }, result{ resultNew } {
		}

		iterate_string_context& operator=(iterate_string_context&&) noexcept	  = delete;
		iterate_string_context(iterate_string_context&&) noexcept				  = delete;
		iterate_string_context& operator=(const iterate_string_context&) noexcept = delete;
		iterate_string_context(const iterate_string_context&) noexcept			  = delete;

		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			result = string_scanner<parseOpts>::impl(strPtr, endPtr, ptrNew);
			return result.outLength == std::numeric_limits<uint64_t>::max() ? uint64_t{} : result.outLength;
		}

		read_buffer_ptr strPtr{};
		read_buffer_ptr endPtr{};
		scan_result& result;
	};

	template<typename string_type, typename source_type> struct copy_string_context {
		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			jsonifierMemcpy(ptrNew, srcPtr, length);
			return length;
		}

		const source_type* srcPtr{};
		uint64_t length{};
	};

	template<string_types string_type, typename source_type> JSONIFIER_INLINE void assignScannedString(string_type& value, const source_type* srcPtr, uint64_t length) {
		if constexpr (resize_and_overwriteable_types<string_type>) {
			value.resize_and_overwrite(length, copy_string_context<string_type, source_type>{ srcPtr, length });
		} else {
			if constexpr (resizeable_types<string_type>) {
				value.resize(length);
			}
			jsonifierMemcpy(value.data(), srcPtr, length);
		}
	}

	template<parse_options parseOpts, typename string_type> struct validating_copy_context {
		JSONIFIER_INLINE uint64_t operator()(remove_pointer_t<typename string_type::pointer>* __restrict ptrNew, uint64_t) const noexcept {
			success = string_validating_copier<parseOpts>::impl(srcPtr, length, ptrNew);
			return success ? length : uint64_t{};
		}

		read_buffer_ptr srcPtr{};
		uint64_t length{};
		bool& success;
	};

	struct swar_string_result {
		uint64_t length{};
		bool found{};
	};

	JSONIFIER_INLINE swar_string_result swarScanAsciiString(read_buffer_ptr strPtr, read_buffer_ptr endPtr) noexcept {
		if constexpr (std::endian::native != std::endian::little) {
			return {};
		} else {
			static constexpr uint64_t ones{ 0x0101010101010101ull };
			static constexpr uint64_t highs{ 0x8080808080808080ull };
			static constexpr uint64_t quoteBytes{ ones * uint_c_v<'"'> };
			static constexpr uint64_t slashBytes{ ones * uint_c_v<'\\'> };
			static constexpr uint64_t controlBytes{ ones * 32u };
			read_buffer_ptr it{ strPtr };
			uint64_t asciiAcc{};
			while (endPtr - it >= 8) {
				uint64_t chunk;
				pow2MemcpyWrapper<8>(&chunk, it);
				const uint64_t quotes  = chunk ^ quoteBytes;
				const uint64_t slashes = chunk ^ slashBytes;
				const uint64_t hits	   = (((quotes - ones) & ~quotes) | ((slashes - ones) & ~slashes) | ((chunk - controlBytes) & ~chunk)) & highs;
				if (hits == 0) {
					asciiAcc |= chunk;
					it += 8;
					continue;
				}
				const uint64_t offset	 = simd::countrZeroUnsafe(hits) >> 3;
				const uint64_t preceding = offset == 0 ? uint64_t{} : (~uint64_t{} >> (64 - 8 * offset));
				asciiAcc |= chunk & preceding;
				if (it[offset] != uint_c_v<'"'> || (asciiAcc & highs) != 0) {
					return {};
				}
				return { static_cast<uint64_t>(it + offset - strPtr), true };
			}
			if ((asciiAcc & highs) != 0) {
				return {};
			}
			for (; it < endPtr; ++it) {
				const uint8_t currentChar = static_cast<uint8_t>(*it);
				if (currentChar == uint_c_v<'"'>) {
					return { static_cast<uint64_t>(it - strPtr), true };
				}
				if (currentChar == uint_c_v<'\\'> || currentChar < 32 || currentChar >= 0x80) {
					return {};
				}
			}
			return {};
		}
	}

	template<parse_options parseOpts, bool swarFirst = true, string_types string_type, typename buffer_type>
	JSONIFIER_INLINE bool parseStringContents(string_type& value, read_buffer_ptr strPtr, read_buffer_ptr endPtr, buffer_type& scratch, uint64_t& rawLength) {
		if constexpr (swarFirst) {
			if (const auto swar = jsonifier::internal::swarScanAsciiString(strPtr, endPtr); swar.found) [[likely]] {
				rawLength = swar.length;
				if constexpr (requires { value.assign(strPtr, swar.length); }) {
					value.assign(strPtr, swar.length);
				} else {
					jsonifier::internal::assignScannedString(value, strPtr, swar.length);
				}
				return true;
			}
		}
		const auto prescan = string_prescanner<parseOpts>::impl(strPtr, endPtr);
		if (!prescan.found) [[unlikely]] {
			return false;
		}
		rawLength = prescan.length;
		if (!prescan.escaped && !prescan.nonAscii) [[likely]] {
			if (prescan.control) [[unlikely]] {
				return false;
			}
			jsonifier::internal::assignScannedString(value, strPtr, prescan.length);
			return true;
		}
		if constexpr (resize_and_overwriteable_types<string_type>) {
			if (!prescan.escaped) [[likely]] {
				bool success{};
				value.resize_and_overwrite(prescan.length, validating_copy_context<parseOpts, string_type>{ strPtr, prescan.length, success });
				return success;
			}
			typename string_scanner<parseOpts>::scan_result res{};
			value.resize_and_overwrite(prescan.length + simdBytesPerRegister, iterate_string_context<parseOpts, string_type>{ res, strPtr, endPtr });
			return res.outLength != std::numeric_limits<uint64_t>::max();
		} else {
			const auto needed = prescan.length + simdBytesPerRegister;
			if (scratch.size() < needed) [[unlikely]] {
				scratch.resize(needed);
			}
			uint64_t outLength{ prescan.length };
			if (!prescan.escaped) [[likely]] {
				if (!string_validating_copier<parseOpts>::impl(strPtr, prescan.length, scratch.data())) [[unlikely]] {
					return false;
				}
			} else {
				const auto res = string_scanner<parseOpts>::impl(strPtr, endPtr, scratch.data());
				if (res.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
					return false;
				}
				outLength = res.outLength;
			}
			jsonifier::internal::assignScannedString(value, scratch.data(), outLength);
			return true;
		}
	}

	template<parse_options parseOpts, string_types string_type, typename buffer_type> JSONIFIER_INLINE bool parseBoundedStringContents(string_type& value, read_buffer_ptr strPtr,
		read_buffer_ptr boundPtr, read_buffer_ptr endPtr, buffer_type& scratch, uint64_t& rawLength) {
		if constexpr (resize_and_overwriteable_types<string_type>) {
			if (boundPtr > strPtr && static_cast<uint64_t>(boundPtr - strPtr) >= simdBytesPerRegister) {
				const uint64_t bound		  = static_cast<uint64_t>(boundPtr - strPtr);
				const read_buffer_ptr scanEnd = static_cast<uint64_t>(endPtr - boundPtr) > simdBytesPerRegister ? boundPtr + simdBytesPerRegister : endPtr;
				typename string_scanner<parseOpts>::scan_result res{};
				value.resize_and_overwrite(bound + simdBytesPerRegister, iterate_string_context<parseOpts, string_type>{ res, strPtr, scanEnd });
				rawLength = res.rawLength;
				return res.outLength != std::numeric_limits<uint64_t>::max();
			}
		}
		return jsonifier::internal::parseStringContents<parseOpts, false>(value, strPtr, endPtr, scratch, rawLength);
	}

	enum class sep_result : uint8_t {
		cont,
		ended,
		error,
	};

	template<parse_options parseOpts, typename iterator_type, typename string_buffer_type> struct parse_context;

	template<parse_options parseOpts, typename string_buffer_type> struct parse_context<parseOpts, unrestrict_t<read_buffer_ptr>, string_buffer_type> {
		using iterator_type			 = read_buffer_ptr;
		using template_iterator_type = unrestrict_t<read_buffer_ptr>;

		JSONIFIER_INLINE parse_context(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, read_buffer_ptr rootIterNew, read_buffer_ptr endIterNew) noexcept
			: stringBuffer{ stringBufferNew }, errors{ errorsNew }, rootIter{ rootIterNew }, endIter{ endIterNew } {
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject(read_buffer_ptr errorPos) noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(rootIter, errorPos, endIter));
			return false;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		base_t<decltype(*std::declval<decltype([]() {
			return read_buffer_ptr{};
		}())>())>
			wsChar{};

		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		read_buffer_ptr rootIter{};
		read_buffer_ptr endIter{};
		uint64_t indentSize{};
	};

	template<parse_options parseOpts, typename string_buffer_type> struct parse_context<parseOpts, unrestrict_t<write_structural_index_ptr>, string_buffer_type> {
		using iterator_type			 = write_structural_index_ptr;
		using template_iterator_type = unrestrict_t<write_structural_index_ptr>;

		JSONIFIER_INLINE parse_context(string_buffer_type* stringBufferNew, std::vector<error>* errorsNew, read_buffer_ptr stringRootNew, read_buffer_ptr stringEndNew,
			write_structural_index_ptr endIterNew) noexcept
			: endIter{ endIterNew }, stringBuffer{ stringBufferNew }, errors{ errorsNew }, stringRoot{ stringRootNew }, stringEnd{ stringEndNew } {
		}

		template<parse_statuses errorType> [[nodiscard]] bool reject(read_buffer_ptr errorPos) noexcept {
			errors->emplace_back(error::constructError<status_classes::parsing, errorType>(stringRoot, errorPos, stringEnd));
			return false;
		}

		JSONIFIER_INLINE string_buffer_type& getStringBuffer() noexcept {
			return *stringBuffer;
		}

		JSONIFIER_INLINE std::vector<error>& getErrors() noexcept {
			return *errors;
		}

		write_structural_index_ptr endIter{};
		string_buffer_type* stringBuffer{};
		std::vector<error>* errors{};
		read_buffer_ptr stringRoot{};
		read_buffer_ptr stringEnd{};
	};

#if JSONIFIER_ANY_AVX || (JSONIFIER_NEON && !JSONIFIER_SVE2)
	struct container_skipper {
		JSONIFIER_INLINE static uint64_t closerOffset(simd::rope_detector<rope_block>& rope, const simd_array_t block, uint64_t& depth, const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_t openRegister,
			const jsonifier_simd_int_t closeRegister) noexcept {
			rope.next(block, bsRegister, quoteRegister);
			const simd::bracket_masks masks = simd::bracket_collector::impl(block, spaceMask, openRegister, closeRegister);
			const uint64_t opens			= masks.opens & ~rope.inString;
			const uint64_t closes			= masks.closes & ~rope.inString;
			const uint64_t closeCount		= static_cast<uint64_t>(std::popcount(closes));
			if (depth > closeCount) [[likely]] {
				depth = depth - closeCount + static_cast<uint64_t>(std::popcount(opens));
				return simdBytesPerBlock;
			}
			uint64_t events = opens | closes;
			while (events) {
				const uint64_t offset = static_cast<uint64_t>(std::countr_zero(events));
				if ((opens >> offset) & 1u) {
					++depth;
				} else if (--depth == 0) {
					return offset;
				}
				events &= events - 1;
			}
			return simdBytesPerBlock;
		}

		JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> impl(unrestrict_t<read_buffer_ptr> iter, read_buffer_ptr end) noexcept {
			const jsonifier_simd_int_t bsRegister	 = simd::gatherValue<jsonifier_simd_int_t>('\\');
			const jsonifier_simd_int_t quoteRegister = simd::gatherValue<jsonifier_simd_int_t>('"');
			const jsonifier_simd_int_t spaceMask	 = simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x20));
			const jsonifier_simd_int_t openRegister	 = simd::gatherValue<jsonifier_simd_int_t>('{');
			const jsonifier_simd_int_t closeRegister = simd::gatherValue<jsonifier_simd_int_t>('}');
			simd::rope_detector<rope_block> rope{};
			uint64_t depth{};
			while (static_cast<uint64_t>(end - iter) >= simdBytesPerBlock) {
				if (const uint64_t offset = closerOffset(rope, loadBlock(iter), depth, bsRegister, quoteRegister, spaceMask, openRegister, closeRegister);
					offset < simdBytesPerBlock) {
					return iter + offset + 1;
				}
				iter += simdBytesPerBlock;
			}
			if (const uint64_t remaining = static_cast<uint64_t>(end - iter); remaining != 0) {
				alignas(simdBytesPerRegister) uint8_t tail[simdBytesPerBlock];
				tail_block_builder::impl(tail, iter, remaining, simdBytesPerBlock);
				if (const uint64_t offset = closerOffset(rope, loadBlock(tail), depth, bsRegister, quoteRegister, spaceMask, openRegister, closeRegister); offset < remaining) {
					return iter + offset + 1;
				}
			}
			return nullptr;
		}

		JSONIFIER_INLINE static simd_array_t loadBlock(read_buffer_ptr blockPtr) noexcept {
			simd_array_t block;
			block.template set<0>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr));
			if constexpr (simdRegistersPerBlock > 1) {
				block.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + simdBytesPerRegister));
				if constexpr (simdRegistersPerBlock > 2) {
					block.template set<2>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + simdBytesPerRegister * 2));
					block.template set<3>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + simdBytesPerRegister * 3));
				}
			}
			return block;
		}
	};
#endif

	template<parse_options parseOpts, typename iterator_type> struct json_cursor;

	template<parse_options parseOpts> struct json_cursor<parseOpts, unrestrict_t<read_buffer_ptr>> {
		using iterator_type			 = read_buffer_ptr;
		using template_iterator_type = unrestrict_t<read_buffer_ptr>;

		template<bool_types bool_type, typename context_type> JSONIFIER_INLINE static bool iterateBool(bool_type& value, read_buffer_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			if (context.endIter - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			iter += 4;
			if (comparison == trueVal) {
				value = true;
			} else if (comparison == falseVal && ( *iter == uint_c_v<'e'> )) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		template<string_types string_type, typename context_type> JSONIFIER_INLINE static bool iterateString(string_type& value, read_buffer_ptr& iter, context_type& context) {
			++iter;
			if (iter >= context.endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!jsonifier::internal::parseStringContents<parseOpts>(value, iter, context.endIter, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			iter += rawLength + 1;
			return true;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(read_buffer_ptr iter, [[maybe_unused]] context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == uint_c_v<charToCheck>;
			} else {
				return iter < context.endIter && *iter == uint_c_v<charToCheck>;
			}
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectObjectSeparator(read_buffer_ptr& iter, uint64_t, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const uint8_t c = *iter;
				if (c == uint_c_v<','>) [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == uint_c_v<'}'>) {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < context.endIter) [[likely]] {
					const uint8_t c = *iter;
					if (c == uint_c_v<','>) [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == uint_c_v<'}'>) {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectArraySeparator(read_buffer_ptr& iter, uint64_t, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				const uint8_t c = *iter;
				if (c == uint_c_v<','>) [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == uint_c_v<']'>) {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < context.endIter) [[likely]] {
					const uint8_t c = *iter;
					if (c == uint_c_v<','>) [[likely]] {
						++iter;
						return sep_result::cont;
					}
					if (c == uint_c_v<']'>) {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(read_buffer_ptr& iter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			while (true) {
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != uint_c_v<'"'>) [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				} else {
					if (iter >= context.endIter || *iter != uint_c_v<'"'>) [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				}
				if (!skipString(iter, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, context)) [[unlikely]] {
					return false;
				}
				if (objectMaybeEnd(iter, depth, context)) {
					return true;
				}
				if (!collectObjectComma(iter, context)) [[unlikely]] {
					return false;
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'{'>(iter, context) ? true : reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(read_buffer_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'['>(iter, context) ? true : reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(read_buffer_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> valuePtr(read_buffer_ptr iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(read_buffer_ptr& iter, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<'}'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(read_buffer_ptr& iter, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<']'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(read_buffer_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(read_buffer_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(read_buffer_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> stringEnd(context_type& context) noexcept {
			return context.endIter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(read_buffer_ptr iter, context_type& context) noexcept {
			return iter < context.endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(read_buffer_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			if (context.endIter - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			iter += 4;
			if (comparison == nullVal) [[likely]] {
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(read_buffer_ptr iter, context_type& context) noexcept {
			return iter >= context.endIter ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(read_buffer_ptr& iter, context_type& context) noexcept {
			++iter;
			jsonifier::internal::skipStringImpl(iter, static_cast<uint64_t>(context.endIter - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == uint_c_v<'"'>) [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < context.endIter && *iter == uint_c_v<'"'>) [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(read_buffer_ptr& iter, context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == uint_c_v<'\0'>) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= context.endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case uint_c_v<'"'>: {
					return skipString(iter, context);
				}
				case uint_c_v<'{'>:
					[[fallthrough]];
				case uint_c_v<'['>: {
#if JSONIFIER_ANY_AVX || (JSONIFIER_NEON && !JSONIFIER_SVE2)
					if (const unrestrict_t<read_buffer_ptr> after = container_skipper::impl(iter, context.endIter); after) [[likely]] {
						iter = after;
						return true;
					}
					return reject<parse_statuses::unexpected_string_end>(context.endIter, context);
#else
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != uint_c_v<'\0'>) {
							const uint8_t c = *iter;
							if (c == uint_c_v<'"'>) {
								if (!skipString(iter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							++iter;
							if (c == uint_c_v<'{'> || c == uint_c_v<'['>) {
								++depth;
							} else if (c == uint_c_v<'}'> || c == uint_c_v<']'>) {
								if (--depth == 0) {
									return true;
								}
							}
						}
					} else {
						while (iter < context.endIter) {
							const uint8_t c = *iter;
							if (c == uint_c_v<'"'>) {
								if (!skipString(iter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							++iter;
							if (c == uint_c_v<'{'> || c == uint_c_v<'['>) {
								++depth;
							} else if (c == uint_c_v<'}'> || c == uint_c_v<']'>) {
								if (--depth == 0) {
									return true;
								}
							}
						}
					}
					return reject<parse_statuses::unexpected_string_end>(iter, context);
#endif
				}
				default: {
					if constexpr (parseOpts.nullTerminated) {
						while (true) {
							const uint8_t c = *iter;
							if (c == uint_c_v<','> || c == uint_c_v<']'> || c == uint_c_v<'}'> || c == uint_c_v<'\0'> || whitespaceTable[c]) {
								return true;
							}
							++iter;
						}
					} else {
						while (iter < context.endIter) {
							const uint8_t c = *iter;
							if (c == uint_c_v<','> || c == uint_c_v<']'> || c == uint_c_v<'}'> || whitespaceTable[c]) {
								return true;
							}
							++iter;
						}
						return true;
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(read_buffer_ptr iter, context_type& context) noexcept {
			return iter && iter != context.endIter ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<number_types number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, read_buffer_ptr& iter, context_type& context) noexcept {
			using value_type = number_type;
			if constexpr (integer_types<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, context.endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, context.endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, context.endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, context.endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, context.endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, context.endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		JSONIFIER_INLINE static bool notAtEnd(read_buffer_ptr iter, [[maybe_unused]] auto& context) noexcept {
			return iter < context.endIter;
		}
	};

	struct ws_tracker {
		inline ~ws_tracker() noexcept {
			cout << "Fail: " << fail << endl;
			cout << "Pass: " << pass << endl;
		}

		uint64_t pass{};
		uint64_t fail{};
	};

	template<parse_options parseOpts>
		requires(!parseOpts.minified)
	struct json_cursor<parseOpts, unrestrict_t<read_buffer_ptr>> {
		using iterator_type			 = read_buffer_ptr;
		using template_iterator_type = unrestrict_t<read_buffer_ptr>;

		enum class indent_result_types : uint8_t {
			success,
			fail,
		};

		enum class ws_sizes : uint8_t { eq_0, eq_1, eq_2, gt_2, eq_4, gt_4, eq_8 };

		static constexpr uint32_t tagShift	= 62u;
		static constexpr uintptr_t tagMask	= uintptr_t{ 0x3 } << tagShift;
		static constexpr uintptr_t addrMask = ~tagMask;

		alignas(64) static constexpr ws_sizes wsSizes[9]{ ws_sizes::eq_0, ws_sizes::eq_1, ws_sizes::eq_2, ws_sizes::gt_2, ws_sizes::eq_4, ws_sizes::gt_4, ws_sizes::gt_4,
			ws_sizes::gt_4, ws_sizes::eq_8 };

		template<bool_types bool_type, typename context_type> JSONIFIER_INLINE static bool iterateBool(bool_type& value, read_buffer_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			skipWhitespaceScalar(iter, context);
			if (context.endIter - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			iter += 4;
			if (comparison == trueVal) {
				value = true;
			} else if (comparison == falseVal && ( *iter == uint_c_v<'e'> )) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespacePredictedClose(read_buffer_ptr& iter, const uint64_t depth, context_type& context) noexcept {
			if (atWhitespace(iter, context)) {
				skipWhitespacePredicted(iter, depth - (depth != 0), context);
			}
		}

		template<string_types string_type, typename context_type> JSONIFIER_INLINE static bool iterateString(string_type& value, read_buffer_ptr& iter, context_type& context) {
			++iter;
			if (iter >= context.endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!jsonifier::internal::parseStringContents<parseOpts>(value, iter, context.endIter, context.getStringBuffer(), rawLength)) [[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			iter += rawLength + 1;
			return true;
		}

		JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> spanIsIndent(read_buffer_ptr iterLocal, uint64_t count, base_t<decltype(*read_buffer_ptr{})> wsChar) noexcept {
			const uint64_t fill{ static_cast<uint64_t>(static_cast<uint8_t>(wsChar)) * 0x0101010101010101ull };
			uint64_t remaining{ count };
			while (remaining > 16) {
				const jsonifier_simd_int_128 charValue{ gatherValue<jsonifier_simd_int_128>(wsChar) };
				const jsonifier_simd_int_128 iterValues{ gatherValuesU<jsonifier_simd_int_128>(iterLocal) };
				const uint16_t mask{ static_cast<uint16_t>(opCmpEq(charValue, iterValues)) };
				if (mask != std::numeric_limits<uint16_t>::max()) {
					return setTag(iterLocal + simd::countrZeroUnsafe(static_cast<uint16_t>(~mask)), indent_result_types::fail);
				}
				remaining -= 16;
				iterLocal += 16;
			}
			while (remaining > 8) {
				uint64_t chunk;
				pow2MemcpyWrapper<8>(&chunk, iterLocal);
				const uint64_t diff{ chunk ^ fill };
				if (diff) {
					return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
				}
				remaining -= 8;
				iterLocal += 8;
			}
			switch (static_cast<uint64_t>(wsSizes[remaining])) {
				case static_cast<uint64_t>(ws_sizes::eq_1): {
					const bool equals{ static_cast<uint8_t>(*iterLocal) == static_cast<uint8_t>(fill) };
					if (!equals) {
						return setTag(iterLocal, indent_result_types::fail);
					}
					iterLocal += 1;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_2): {
					uint16_t chunk;
					pow2MemcpyWrapper<2>(&chunk, iterLocal);
					const uint16_t diff{ static_cast<uint16_t>(chunk ^ static_cast<uint16_t>(fill)) };
					if (diff) {
						return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 2;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::gt_2): {
					const uint64_t difference{ remaining - 2 };
					uint32_t chunk;
					pow2MemcpyWrapper<2>(std::bit_cast<write_buffer_ptr>(&chunk), iterLocal);
					pow2MemcpyWrapper<2>(std::bit_cast<write_buffer_ptr>(&chunk) + 2, iterLocal + difference);
					const uint32_t diff{ chunk ^ static_cast<uint32_t>(fill) };
					if (diff) {
						return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += remaining;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_4): {
					uint32_t chunk;
					pow2MemcpyWrapper<4>(&chunk, iterLocal);
					const uint32_t diff{ chunk ^ static_cast<uint32_t>(fill) };
					if (diff) {
						return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 4;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::gt_4): {
					const uint64_t difference{ remaining - 4 };
					uint64_t chunk;
					pow2MemcpyWrapper<4>(std::bit_cast<write_buffer_ptr>(&chunk), iterLocal);
					pow2MemcpyWrapper<4>(std::bit_cast<write_buffer_ptr>(&chunk) + 4, iterLocal + difference);
					const uint64_t diff{ chunk ^ fill };
					if (diff) {
						return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += remaining;
					break;
				}
				case static_cast<uint64_t>(ws_sizes::eq_8): {
					uint64_t chunk;
					pow2MemcpyWrapper<8>(&chunk, iterLocal);
					const uint64_t diff{ chunk ^ fill };
					if (diff) {
						return setTag(iterLocal + (simd::countrZeroUnsafe(diff) >> 3), indent_result_types::fail);
					}
					iterLocal += 8;
					break;
				}
				default: {
					return setTag(iterLocal, indent_result_types::success);
				}
			}
			return setTag(iterLocal, indent_result_types::success);
		}

		template<typename context_type> JSONIFIER_INLINE static void skipWhitespacePredicted(read_buffer_ptr& iter, const uint64_t depth, context_type& context) noexcept {
			if (atNewline(iter, context)) [[likely]] {
				const read_buffer_ptr probe{ skipNewline(iter, context) };
				const uint64_t predicted{ context.indentSize * depth };
				if (probe + predicted < context.endIter) [[likely]] {
					uintptr_t res{ std::bit_cast<uintptr_t>(spanIsIndent(probe, predicted, context.wsChar)) };
					indent_result_types result{ getTag(res) };
					auto iterNew = stripTag(res);
					if (result == indent_result_types::success && !whitespaceTable[static_cast<uint64_t>(*iterNew)]) [[likely]] {
						iter = iterNew;
						return;
					}
					iter = probe;
					skipWhitespaceScalar(iter, context);
					return;
				}
				iter = probe;
			}
			skipWhitespaceScalar(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEqualsNoWsFirst(read_buffer_ptr& iter, context_type& context) noexcept {
			if (incrementIfEqualsNoWs<charToCheck>(iter, context)) [[likely]] {
				return true;
			}
			if (atWhitespace(iter, context)) [[unlikely]] {
				skipWhitespaceScalar(iter, context);
				return incrementIfEqualsNoWs<charToCheck>(iter, context);
			}
			return false;
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectObjectSeparator(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, depth, context);
			if constexpr (parseOpts.nullTerminated) {
				const uint8_t c = *iter;
				if (c == uint_c_v<','>) [[likely]] {
					++iter;
					skipWhitespacePredicted(iter, depth, context);
					return sep_result::cont;
				}
				if (c == uint_c_v<'}'>) {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < context.endIter) [[likely]] {
					const uint8_t c = *iter;
					if (c == uint_c_v<','>) [[likely]] {
						++iter;
						skipWhitespacePredicted(iter, depth, context);
						return sep_result::cont;
					}
					if (c == uint_c_v<'}'>) {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(read_buffer_ptr iter, [[maybe_unused]] context_type& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return *iter == uint_c_v<charToCheck>;
			} else {
				return iter < context.endIter && *iter == uint_c_v<charToCheck>;
			}
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectArraySeparator(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, depth, context);
			if constexpr (parseOpts.nullTerminated) {
				const uint8_t c = *iter;
				if (c == uint_c_v<','>) [[likely]] {
					++iter;
					skipWhitespacePredicted(iter, depth, context);
					return sep_result::cont;
				}
				if (c == uint_c_v<']'>) {
					++iter;
					return sep_result::ended;
				}
			} else {
				if (iter < context.endIter) [[likely]] {
					const uint8_t c = *iter;
					if (c == uint_c_v<','>) [[likely]] {
						++iter;
						skipWhitespacePredicted(iter, depth, context);
						return sep_result::cont;
					}
					if (c == uint_c_v<']'>) {
						++iter;
						return sep_result::ended;
					}
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEqualsNoWs(read_buffer_ptr& iter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(read_buffer_ptr& iter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, context);
			return checkChar<charToCheck>(iter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			while (true) {
				if (atWhitespace(iter, context)) [[unlikely]] {
					skipWhitespaceScalar(iter, context);
				}
				if constexpr (parseOpts.nullTerminated) {
					if (*iter != uint_c_v<'"'>) [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				} else {
					if (iter >= context.endIter || *iter != uint_c_v<'"'>) [[unlikely]] {
						return reject<parse_statuses::missing_key_start>(iter, context);
					}
				}
				if (!skipString(iter, context)) [[unlikely]] {
					return false;
				}
				if (!collectObjectColon(iter, context)) [[unlikely]] {
					return false;
				}
				if (!skipValue(iter, context)) [[unlikely]] {
					return false;
				}
				switch (static_cast<uint64_t>(collectObjectSeparator(iter, depth, context))) {
					case static_cast<uint64_t>(sep_result::cont): {
						continue;
					}
					case static_cast<uint64_t>(sep_result::ended): {
						return true;
					}
					default: {
						return false;
					}
				}
			}
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(read_buffer_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(iter);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, depth, context);
			return incrementIfEqualsNoWs<'}'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespacePredictedClose(iter, depth, context);
			return incrementIfEqualsNoWs<']'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespaceScalar(iter, context);
			if (!(checkDepth(iter, depth, context) && incrementIfEqualsNoWs<'{'>(iter, context))) {
				return reject<parse_statuses::missing_object_start>(iter, context);
			}
			skipWhitespacePredicted(iter, depth + 1, context);
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(read_buffer_ptr& iter, uint64_t depth, context_type& context) noexcept {
			skipWhitespaceScalar(iter, context);
			if (!(checkDepth(iter, depth, context) && incrementIfEqualsNoWs<'['>(iter, context))) {
				return reject<parse_statuses::missing_array_start>(iter, context);
			}
			skipWhitespacePredicted(iter, depth + 1, context);
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(read_buffer_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> valuePtr(read_buffer_ptr iter, context_type&) noexcept {
			return iter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(read_buffer_ptr& iter, context_type& context) noexcept {
			return incrementIfEqualsNoWs<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(read_buffer_ptr& iter, context_type& context) noexcept {
			static constexpr uint16_t colonSpace{ std::endian::native == std::endian::little
					? static_cast<uint16_t>(static_cast<uint16_t>(':') | (static_cast<uint16_t>(' ') << 8))
					: static_cast<uint16_t>((static_cast<uint16_t>(':') << 8) | static_cast<uint16_t>(' ')) };
			if (context.endIter - iter >= 2) [[likely]] {
				uint16_t chunk;
				pow2MemcpyWrapper<2>(&chunk, iter);
				if (chunk == colonSpace) [[likely]] {
					iter += 2;
					return true;
				}
			}
			return incrementIfEqualsNoWsFirst<':'>(iter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> skipNewline(read_buffer_ptr iterLocal, [[maybe_unused]] auto& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (newlineTable[static_cast<uint64_t>(*iterLocal)]) {
					++iterLocal;
				}
			} else {
				while (iterLocal < context.endIter && newlineTable[static_cast<uint64_t>(*iterLocal)]) {
					++iterLocal;
				}
			}
			return iterLocal;
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(read_buffer_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static void collectIndentSize(read_buffer_ptr iter, context_type& context) noexcept {
			read_buffer_ptr probe{ iter };
			while (probe < context.endIter && whitespaceTable[static_cast<uint64_t>(*probe)]) {
				++probe;
			}
			if (probe >= context.endIter || ( *probe != uint_c_v<'{'> && *probe != uint_c_v<'['> )) {
				return;
			}
			++probe;
			if (probe >= context.endIter || !newlineTable[static_cast<uint64_t>(*probe)]) {
				return;
			}
			probe = skipNewline(probe, context);
			if (probe >= context.endIter || !whitespaceTable[static_cast<uint64_t>(*probe)]) {
				return;
			}
			context.wsChar = *probe;
			uint64_t count{};
			while (probe < context.endIter && *probe == context.wsChar) {
				++probe;
				++count;
			}
			context.indentSize = count;
		}

		JSONIFIER_INLINE static constexpr unrestrict_t<read_buffer_ptr> setTag(read_buffer_ptr ptr, indent_result_types status) noexcept {
			const uintptr_t raw = std::bit_cast<uintptr_t>(ptr);
			return std::bit_cast<read_buffer_ptr>((raw & addrMask) | (static_cast<uintptr_t>(status) << tagShift));
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> stringEnd(context_type& context) noexcept {
			return context.endIter;
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(read_buffer_ptr iter, context_type& context) noexcept {
			return iter < context.endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(read_buffer_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			skipWhitespaceScalar(iter, context);
			if (context.endIter - iter < 4) [[unlikely]] {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, iter);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			iter += 4;
			if (comparison == nullVal) [[likely]] {
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(read_buffer_ptr iter, context_type& context) noexcept {
			if (atWhitespace(iter, context)) [[unlikely]] {
				skipWhitespaceScalar(iter, context);
			}
			return iter >= context.endIter ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(read_buffer_ptr& iter, context_type& context) noexcept {
			++iter;
			jsonifier::internal::skipStringImpl(iter, static_cast<uint64_t>(context.endIter - iter));
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == uint_c_v<'"'>) [[likely]] {
					++iter;
					return true;
				}
			} else {
				if (iter < context.endIter && *iter == uint_c_v<'"'>) [[likely]] {
					++iter;
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(read_buffer_ptr& iter, context_type& context) noexcept {
			skipWhitespaceScalar(iter, context);
			if constexpr (parseOpts.nullTerminated) {
				if (*iter == uint_c_v<'\0'>) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			} else {
				if (iter >= context.endIter) [[unlikely]] {
					return reject<parse_statuses::unexpected_end_of_input>(iter, context);
				}
			}
			switch (*iter) {
				case uint_c_v<'"'>: {
					return skipString(iter, context);
				}
				case uint_c_v<'{'>:
					[[fallthrough]];
				case uint_c_v<'['>: {
#if JSONIFIER_ANY_AVX || (JSONIFIER_NEON && !JSONIFIER_SVE2)
					if (const unrestrict_t<read_buffer_ptr> after = container_skipper::impl(iter, context.endIter); after) [[likely]] {
						iter = after;
						return true;
					}
					return reject<parse_statuses::unexpected_string_end>(context.endIter, context);
#else
					uint64_t depth{};
					if constexpr (parseOpts.nullTerminated) {
						while (*iter != uint_c_v<'\0'>) {
							const uint8_t c = *iter;
							if (c == uint_c_v<'"'>) {
								if (!skipString(iter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[c]) {
								skipWhitespaceScalar(iter, context);
								continue;
							}
							++iter;
							if (c == uint_c_v<'{'> || c == uint_c_v<'['>) {
								++depth;
							} else if (c == uint_c_v<'}'> || c == uint_c_v<']'>) {
								if (--depth == 0) {
									return true;
								}
							}
						}
					} else {
						while (iter < context.endIter) {
							const uint8_t c = *iter;
							if (c == uint_c_v<'"'>) {
								if (!skipString(iter, context)) [[unlikely]] {
									return false;
								}
								continue;
							}
							if (whitespaceTable[c]) {
								skipWhitespaceScalar(iter, context);
								continue;
							}
							++iter;
							if (c == uint_c_v<'{'> || c == uint_c_v<'['>) {
								++depth;
							} else if (c == uint_c_v<'}'> || c == uint_c_v<']'>) {
								if (--depth == 0) {
									return true;
								}
							}
						}
					}
					return reject<parse_statuses::unexpected_string_end>(iter, context);
#endif
				}
				default: {
					if constexpr (parseOpts.nullTerminated) {
						while (true) {
							const uint8_t c = *iter;
							if (c == uint_c_v<','> || c == uint_c_v<']'> || c == uint_c_v<'}'> || c == uint_c_v<'\0'> || whitespaceTable[c]) {
								return true;
							}
							++iter;
						}
					} else {
						while (iter < context.endIter) {
							const uint8_t c = *iter;
							if (c == uint_c_v<','> || c == uint_c_v<']'> || c == uint_c_v<'}'> || whitespaceTable[c]) {
								return true;
							}
							++iter;
						}
						return true;
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(read_buffer_ptr iter, context_type& context) noexcept {
			return iter && iter != context.endIter ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<number_types number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, read_buffer_ptr& iter, context_type& context) noexcept {
			using value_type = number_type;
			if (atWhitespace(iter, context)) [[unlikely]] {
				skipWhitespaceScalar(iter, context);
			}
			if constexpr (integer_types<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, context.endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, iter, context.endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, iter, context.endIter); iterNew) {
							iter = iterNew;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, iter, context.endIter); iterNew) {
							iter  = iterNew;
							value = static_cast<value_type>(i);
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = float_parser<double>::parseFloat(temp, iter, context.endIter); iterNew) {
						iter  = iterNew;
						value = static_cast<value_type>(temp);
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = float_parser<value_type>::parseFloat(value, iter, context.endIter); iterNew) {
						iter = iterNew;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		JSONIFIER_INLINE static void skipWhitespaceScalar(read_buffer_ptr& iter, [[maybe_unused]] auto& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				while (whitespaceTable[static_cast<uint64_t>(*iter)]) {
					++iter;
				}
			} else {
				while (iter < context.endIter && whitespaceTable[static_cast<uint64_t>(*iter)]) {
					++iter;
				}
			}
		}

		JSONIFIER_INLINE static bool atWhitespace(read_buffer_ptr iter, [[maybe_unused]] auto& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return whitespaceTable[static_cast<uint64_t>(*iter)];
			} else {
				return iter < context.endIter && whitespaceTable[static_cast<uint64_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static bool atNewline(read_buffer_ptr iter, [[maybe_unused]] auto& context) noexcept {
			if constexpr (parseOpts.nullTerminated) {
				return newlineTable[static_cast<uint64_t>(*iter)];
			} else {
				return iter < context.endIter && newlineTable[static_cast<uint64_t>(*iter)];
			}
		}

		JSONIFIER_INLINE static bool notAtEnd(read_buffer_ptr iter, [[maybe_unused]] auto& context) noexcept {
			return iter < context.endIter;
		}

		JSONIFIER_INLINE static constexpr unrestrict_t<read_buffer_ptr> stripTag(uintptr_t ptr) noexcept {
			return std::bit_cast<read_buffer_ptr>(ptr & addrMask);
		}

		JSONIFIER_INLINE static constexpr indent_result_types getTag(const uintptr_t ptr) noexcept {
			return static_cast<indent_result_types>((ptr & tagMask) >> tagShift);
		}

		static_assert(sizeof(uintptr_t) == 8, "pointer tagging requires 64-bit pointers");
	};

	alignas(64) inline constexpr auto validPostPrimitiveTable{ [] {
		array<bool, 256> table{};
		table[uint_c_v<','>]  = true;
		table[uint_c_v<'}'>]  = true;
		table[uint_c_v<']'>]  = true;
		table[uint_c_v<' '>]  = true;
		table[uint_c_v<'\t'>] = true;
		table[uint_c_v<'\n'>] = true;
		table[uint_c_v<'\r'>] = true;
		return table;
	}() };

	template<parse_options parseOpts> struct json_cursor<parseOpts, unrestrict_t<write_structural_index_ptr>> {
		using iterator_type			 = write_structural_index_ptr;
		using template_iterator_type = unrestrict_t<write_structural_index_ptr>;

		template<typename context_type> JSONIFIER_INLINE static bool checkPostPrimitive(read_buffer_ptr iterNew, write_structural_index_ptr iter, context_type& context) noexcept {
			if (iterNew < context.stringEnd && !validPostPrimitiveTable[static_cast<uint64_t>(*iterNew)]) [[unlikely]] {
				return reject<parse_statuses::missing_comma>(iter, context);
			}
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectObjectSeparator(write_structural_index_ptr& iter, uint64_t, context_type& context) noexcept {
			if (iter < context.endIter) [[likely]] {
				const char c = static_cast<char>(*valuePtr(iter, context));
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == '}') {
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<string_types string_type, typename context_type>
		JSONIFIER_INLINE static bool iterateString(string_type& value, write_structural_index_ptr& iter, context_type& context) {
			read_buffer_ptr strPtr = valuePtr(iter, context) + 1;
			if (strPtr >= context.stringEnd) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			uint64_t rawLength{};
			if (!jsonifier::internal::parseBoundedStringContents<parseOpts>(value, strPtr, context.stringRoot + iter[1], context.stringEnd, context.getStringBuffer(), rawLength))
				[[unlikely]] {
				return reject<parse_statuses::invalid_string_characters>(iter, context);
			}
			++iter;
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static sep_result collectArraySeparator(write_structural_index_ptr& iter, uint64_t, context_type& context) noexcept {
			if (iter < context.endIter) [[likely]] {
				const char c = static_cast<char>(*valuePtr(iter, context));
				if (c == ',') [[likely]] {
					++iter;
					return sep_result::cont;
				}
				if (c == ']') {
					++iter;
					return sep_result::ended;
				}
			}
			static_cast<void>(reject<parse_statuses::missing_comma>(iter, context));
			return sep_result::error;
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool incrementIfEquals(write_structural_index_ptr& iter, context_type& context) noexcept {
			return checkChar<charToCheck>(iter, context) ? (static_cast<void>(++iter), true) : false;
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> valuePtr(write_structural_index_ptr iter, context_type& context) noexcept {
			return context.stringRoot + *iter;
		}

		template<parse_statuses errorType, typename context_type> JSONIFIER_INLINE static bool reject(write_structural_index_ptr iter, context_type& context) noexcept {
			return context.template reject<errorType>(&context.stringRoot[*iter]);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipRemainingObject(write_structural_index_ptr& iter, uint64_t, context_type& context) noexcept {
			if (iter >= context.endIter || *valuePtr(iter, context) != uint_c_v<'"'>) [[unlikely]] {
				return reject<parse_statuses::missing_key_start>(iter, context);
			}
			int64_t depth{ 1 };
			while (iter < context.endIter) {
				depth += nestingDeltaTable[static_cast<uint64_t>(context.stringRoot[*iter])];
				++iter;
				if (depth == 0) {
					return true;
				}
			}
			return reject<parse_statuses::unexpected_string_end>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectStart(write_structural_index_ptr& iter, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'{'>(iter, context) ? true : reject<parse_statuses::missing_object_start>(iter, context);
		}

		template<char charToCheck, typename context_type> JSONIFIER_INLINE static bool checkChar(write_structural_index_ptr iter, context_type& context) noexcept {
			return iter < context.endIter && *valuePtr(iter, context) == uint_c_v<charToCheck>;
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayStart(write_structural_index_ptr& iter, uint64_t depth, context_type& context) noexcept {
			return checkDepth(iter, depth, context) && incrementIfEquals<'['>(iter, context) ? true : reject<parse_statuses::missing_array_start>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkDepth(write_structural_index_ptr iter, uint64_t depth, context_type& context) noexcept {
			return depth < static_cast<uint64_t>(parseOpts.maxDepth) ? true : reject<parse_statuses::exceeded_max_depth>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool objectMaybeEnd(write_structural_index_ptr& iter, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<'}'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool arrayMaybeEnd(write_structural_index_ptr& iter, uint64_t, context_type& context) noexcept {
			return incrementIfEquals<']'>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectComma(write_structural_index_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectObjectColon(write_structural_index_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<':'>(iter, context) ? true : reject<parse_statuses::missing_colon>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool collectArrayComma(write_structural_index_ptr& iter, context_type& context) noexcept {
			return incrementIfEquals<','>(iter, context) ? true : reject<parse_statuses::missing_comma>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool hasMoreInput(write_structural_index_ptr iter, context_type& context) noexcept {
			return iter < context.endIter ? true : reject<parse_statuses::unexpected_end_of_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool iterateNull(write_structural_index_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			read_buffer_ptr ptr = valuePtr(iter, context);
			if (context.stringEnd - ptr < 4) {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, ptr);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			if (comparison == nullVal) [[likely]] {
				++iter;
				return true;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_null_value>(iter, context);
			}
		}

		template<typename context_type> JSONIFIER_INLINE static bool checkIfDone(write_structural_index_ptr iter, context_type& context) noexcept {
			bool done{};
			if (context.stringRoot && iter) {
				done = iter >= context.endIter || &context.stringRoot[*iter] == context.stringEnd;
			}
			return done ? true : reject<parse_statuses::unfinished_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipValue(write_structural_index_ptr& iter, context_type& context) noexcept {
			if (iter >= context.endIter) [[unlikely]] {
				return reject<parse_statuses::unexpected_end_of_input>(iter, context);
			}
			const char first = static_cast<char>(*valuePtr(iter, context));
			if (first == '{' || first == '[') {
				int64_t depth{};
				while (iter < context.endIter) {
					depth += nestingDeltaTable[static_cast<uint64_t>(context.stringRoot[*iter])];
					++iter;
					if (depth == 0) {
						return true;
					}
				}
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}
			++iter;
			return true;
		}

		template<typename context_type> JSONIFIER_INLINE static bool anyInput(write_structural_index_ptr iter, context_type& context) noexcept {
			return context.stringRoot && context.stringRoot != context.stringEnd ? true : reject<parse_statuses::no_input>(iter, context);
		}

		template<typename context_type> JSONIFIER_INLINE static bool skipString(write_structural_index_ptr& iter, context_type&) noexcept {
			++iter;
			return true;
		}

		template<number_types number_type, typename context_type>
		JSONIFIER_INLINE static bool iterateNumber(number_type& value, write_structural_index_ptr& iter, context_type& context) noexcept {
			using value_type		   = number_type;
			read_buffer_ptr valueStart = valuePtr(iter, context);
			read_buffer_ptr valueEnd   = (iter + 1) < context.endIter ? context.stringRoot + *(iter + 1) : context.stringEnd;
			if constexpr (integer_types<value_type>) {
				if constexpr (uint_types<value_type>) {
					if constexpr (uint64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						uint64_t i;
						if (auto iterNew = integer_parser<uint64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				} else {
					if constexpr (int64_types<value_type>) {
						if (auto iterNew = integer_parser<value_type>::parseInt(value, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					} else {
						int64_t i;
						if (auto iterNew = integer_parser<int64_t>::parseInt(i, valueStart, valueEnd); iterNew) {
							if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
								return false;
							}
							value = static_cast<value_type>(i);
							++iter;
							return true;
						} else {
							return reject<parse_statuses::invalid_number_value>(iter, context);
						}
					}
				}
			} else {
				if constexpr (std::is_volatile_v<internal::remove_reference_t<decltype(value)>>) {
					double temp;
					if (auto iterNew = internal::float_parser<double>::parseFloat(temp, valueStart, valueEnd); iterNew) {
						if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
							return false;
						}
						value = static_cast<value_type>(temp);
						++iter;
						return true;
					}
					return reject<parse_statuses::invalid_number_value>(iter, context);
				} else {
					if (auto iterNew = internal::float_parser<value_type>::parseFloat(value, valueStart, valueEnd); iterNew) {
						if (!checkPostPrimitive(iterNew, iter, context)) [[unlikely]] {
							return false;
						}
						++iter;
						return true;
					} else {
						return reject<parse_statuses::invalid_number_value>(iter, context);
					}
				}
			}
		}

		template<typename context_type> JSONIFIER_INLINE static unrestrict_t<read_buffer_ptr> stringEnd(context_type& context) noexcept {
			return context.stringEnd;
		}

		template<bool_types bool_type, typename context_type>
		JSONIFIER_INLINE static bool iterateBool(bool_type& value, write_structural_index_ptr& iter, context_type& context) noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			read_buffer_ptr ptr = valuePtr(iter, context);
			if (context.stringEnd - ptr < 4) {
				return reject<parse_statuses::unexpected_string_end>(iter, context);
			}

			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, ptr);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			if (comparison == trueVal) {
				value = true;
				++iter;
			} else if (comparison == falseVal && ( *(ptr + 4) == uint_c_v<'e'> )) [[likely]] {
				value = false;
				++iter;
			} else [[unlikely]] {
				return reject<parse_statuses::invalid_bool_value>(iter, context);
			}
			return true;
		}

		JSONIFIER_INLINE static bool notAtEnd(write_structural_index_ptr iter, [[maybe_unused]] auto& context) noexcept {
			return iter < context.endIter;
		}
	};

}
