/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_value.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/document_state.hpp>

namespace jsonifier::generic {

	template<bool minified> class generic_value {
	  public:
		using document_state = generic::document_state;
		using object		 = generic_object<minified>;
		using array			 = generic_array<minified>;
		using value			 = generic_value;

		template<internal::buffer_like_types string_type> [[nodiscard]] JSONIFIER_INLINE error_code getString(string_type& out) const {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			if (!doc->template charAt<internal::uint_c_v<'"'>>(iter)) [[unlikely]] {
				return error_code::incorrect_type;
			}
			consume();
			read_buffer_ptr stringStart = doc->root + *iter + 1;
			if (const uint64_t directLength = zeroCopyLength<false>(stringStart); directLength != noZeroCopy) [[likely]] {
				if constexpr (internal::resize_and_overwriteable_types<string_type>) {
					out.resize_and_overwrite(directLength, plain_string_copy_context{ stringStart });
				} else {
					out.resize(directLength);
					plain_string_copy_context{ stringStart }(out.data(), directLength);
				}
				return error_code::success;
			}
			const uint64_t upperBound = static_cast<uint64_t>(doc->root + *(iter + 1) - stringStart) + 64;
			uint64_t outLength{};
			if constexpr (internal::resize_and_overwriteable_types<string_type>) {
				out.resize_and_overwrite(upperBound, string_parse_context{ stringStart, doc->end, outLength });
			} else {
				if (out.size() < upperBound) [[unlikely]] {
					out.resize(upperBound);
				}
				out.resize(string_parse_context{ stringStart, doc->end, outLength }(out.data(), upperBound));
			}
			return outLength == std::numeric_limits<uint64_t>::max() ? error_code::string_error : error_code::success;
		}

		[[nodiscard]] JSONIFIER_INLINE error_code rawJson(string_view& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			if (document_state::isOpener(*valueStart)) {
				read_structural_index_ptr afterClose = doc->skipValue(iter);
				if (afterClose == doc->tapeEnd && !document_state::isCloser(doc->charAt(afterClose - 1))) [[unlikely]] {
					return error_code::unclosed_container;
				}
				if (doc->cursor == iter) [[likely]] {
					doc->cursor = afterClose;
				}
				out = string_view{ std::bit_cast<const char*>(valueStart), static_cast<uint64_t>(doc->root + *(afterClose - 1) + 1 - valueStart) };
				return error_code::success;
			}
			read_buffer_ptr valueEnd = doc->root + *(iter + 1);
			while (valueEnd > valueStart && isWhitespace(valueEnd[-1])) {
				--valueEnd;
			}
			out = string_view{ std::bit_cast<const char*>(valueStart), static_cast<uint64_t>(valueEnd - valueStart) };
			return error_code::success;
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getBool(bool& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			switch (doc->charAt(iter)) {
				case internal::uint_c_v<'t'>: {
					consume();
					out = true;
					return trueMatches() ? error_code::success : error_code::t_atom_error;
				}
				case internal::uint_c_v<'f'>: {
					consume();
					out = false;
					return falseMatches() ? error_code::success : error_code::f_atom_error;
				}
				default: {
					return error_code::incorrect_type;
				}
			}
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getUint64(uint64_t& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::integer_parser<uint64_t>::parseInt(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getInt64(int64_t& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::integer_parser<int64_t>::parseInt(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getDouble(double& out) const noexcept {
			if (!isNumberStart()) [[unlikely]] {
				return typeError();
			}
			read_buffer_ptr valueStart = doc->root + *iter;
			read_buffer_ptr valueEnd   = doc->root + *(iter + 1);
			read_buffer_ptr parsedEnd  = internal::float_parser<double>::parseFloat(out, valueStart, valueEnd);
			consume();
			return finishNumber(parsedEnd, valueEnd);
		}

		[[nodiscard]] JSONIFIER_INLINE error_code getString(std::string_view& out) const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return err;
			}
			if (!doc->template charAt<internal::uint_c_v<'"'>>(iter)) [[unlikely]] {
				return error_code::incorrect_type;
			}
			consume();
			return unescape(iter, out);
		}

		[[nodiscard]] JSONIFIER_INLINE bool isNull() const noexcept {
			if (err == error_code::success && doc->template charAt<internal::uint_c_v<'n'>>(iter) && nullMatches()) {
				consume();
				return true;
			}
			return false;
		}

		JSONIFIER_INLINE json_type type() const noexcept {
			if (err != error_code::success) [[unlikely]] {
				return json_type::unset;
			}
			return typeTable[doc->charAt(iter)];
		}

		JSONIFIER_INLINE generic_value(document_state* docNew, read_structural_index_ptr iterNew, uint64_t depthNew) noexcept : iter{ iterNew }, doc{ docNew }, depth{ depthNew } {
		}

		template<jsonifier::internal::string_view_convertible_types key_type> JSONIFIER_INLINE value operator[](key_type&& key) const noexcept;
		template<typename value_type> [[nodiscard]] JSONIFIER_INLINE error_code get(value_type& out) const;
		JSONIFIER_INLINE value findFieldUnordered(string_view key) const noexcept;

		JSONIFIER_INLINE generic_value(error_code errNew) noexcept : err{ errNew } {
		}

		JSONIFIER_INLINE error_code error() const noexcept {
			return err;
		}

		JSONIFIER_INLINE value atPointer(string_view pointer) const noexcept;
		JSONIFIER_INLINE value findField(string_view key) const noexcept;
		JSONIFIER_INLINE value operator[](uint64_t index) const noexcept;
		JSONIFIER_INLINE object getObject() const noexcept;
		JSONIFIER_INLINE array getArray() const noexcept;
		JSONIFIER_INLINE generic_value() noexcept = default;

	  protected:
		static constexpr uint64_t noZeroCopy{ std::numeric_limits<uint64_t>::max() };
#if JSONIFIER_NEON
		static constexpr uint64_t stopMaskLaneShift{ 2 };
#else
		static constexpr uint64_t stopMaskLaneShift{ 0 };
#endif

		struct string_stop_masks {
			uint64_t nonAscii{};
			uint64_t stops{};
		};

		friend class generic_object<minified>;
		friend class generic_array<minified>;
		friend class generic_field<minified>;

		template<bool allowNonAscii> JSONIFIER_INLINE uint64_t zeroCopyLength(read_buffer_ptr stringStart) const noexcept {
			read_buffer_ptr current = stringStart;
			bool sawNonAscii{};
			if (static_cast<uint64_t>(doc->end - current) >= simdBytesPerRegister) [[likely]] {
				if (const string_stop_masks masks = selectStops<allowNonAscii>(stringStopMasks(current)); masks.stops != 0) [[likely]] {
					return resolveMaskedStop(stringStart, current, masks, false);
				} else {
					sawNonAscii = masks.nonAscii != 0;
				}
				current += simdBytesPerRegister;
				while (static_cast<uint64_t>(doc->end - current) >= simdBytesPerRegister) {
					if (const string_stop_masks masks = selectStops<allowNonAscii>(stringStopMasks(current)); masks.stops != 0) {
						return resolveMaskedStop(stringStart, current, masks, sawNonAscii);
					} else {
						sawNonAscii = sawNonAscii || masks.nonAscii != 0;
					}
					current += simdBytesPerRegister;
				}
			}
			uint8_t accumulated{ static_cast<uint8_t>(sawNonAscii ? 0x80 : 0x00) };
			for (; current < doc->end; ++current) {
				const uint8_t currentChar = static_cast<uint8_t>(*current);
				if ((allowNonAscii ? utf8StringStopTable : asciiStringStopTable)[currentChar]) {
					return resolveStringStop(stringStart, current, (accumulated & 0x80) != 0);
				}
				accumulated |= currentChar;
			}
			return noZeroCopy;
		}

		JSONIFIER_INLINE static bool validUtf8(read_buffer_ptr source, uint64_t length) noexcept {
			internal::utf8_validation_state state{};
			internal::unrestrict_t<read_buffer_ptr> current = source;
			return internal::string_parse_executor<utf8_span_step, internal::make_ascending_range<internal::startIndex, internal::listSize>>::impl(current, source + length, state);
		}

		JSONIFIER_INLINE static string_stop_masks stringStopMasks(read_buffer_ptr current) noexcept {
			const jsonifier_simd_int_t values		  = internal::simd::gatherValuesU<jsonifier_simd_int_t>(current);
			const jsonifier_simd_int_t backslashes	  = internal::simd::gatherValue<jsonifier_simd_int_t>('\\');
			const jsonifier_simd_int_t quotes		  = internal::simd::gatherValue<jsonifier_simd_int_t>('"');
			const jsonifier_simd_int_t controlCeiling = internal::simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x20));
#if JSONIFIER_AVX || JSONIFIER_AVX2 || JSONIFIER_AVX512
			using mask_type			= std::make_unsigned_t<decltype(internal::simd::opBitMask(jsonifier_simd_int_t{}))>;
			const uint64_t nonAscii = static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(values)));
			const auto stopsAndHigh = internal::simd::opOr(internal::simd::opOr(internal::simd::opCmpEqRaw(values, backslashes), internal::simd::opCmpEqRaw(values, quotes)),
				internal::simd::opCmpLtSignedRaw(values, controlCeiling));
			return { nonAscii, static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(stopsAndHigh))) & ~nonAscii };
#else
			const jsonifier_simd_int_t asciiCeiling = internal::simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x7F));
			const auto stops = internal::simd::opOr(internal::simd::opOr(internal::simd::opCmpEqRaw(values, backslashes), internal::simd::opCmpEqRaw(values, quotes)),
				internal::simd::opCmpLtRaw(values, controlCeiling));
	#if JSONIFIER_NEON
			return { internal::simd::opBitMaskRaw(internal::simd::opCmpLtRaw(asciiCeiling, values)), internal::simd::opBitMaskRaw(stops) };
	#else
			using mask_type = std::make_unsigned_t<decltype(internal::simd::opBitMask(jsonifier_simd_int_t{}))>;
			return { static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(internal::simd::opCmpLtRaw(asciiCeiling, values)))),
				static_cast<uint64_t>(static_cast<mask_type>(internal::simd::opBitMask(stops))) };
	#endif
#endif
		}

		template<jsonifier::internal::string_types value_type> JSONIFIER_INLINE error_code unescape(read_structural_index_ptr stringIter, value_type& result) const noexcept {
			read_buffer_ptr stringStart = doc->root + *stringIter + 1;
			if (const uint64_t directLength = zeroCopyLength<true>(stringStart); directLength != noZeroCopy) [[likely]] {
				result = value_type{ std::bit_cast<const char*>(stringStart), directLength };
				return error_code::success;
			}
			char* scratch	  = doc->arena->scratch();
			const auto parsed = string_parser::impl(stringStart, doc->end, scratch);
			if (parsed.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return error_code::string_error;
			}
			if (parsed.outLength == parsed.rawLength) {
				result = value_type{ std::bit_cast<const char*>(stringStart), parsed.rawLength };
				return error_code::success;
			}
			char* slot = doc->arena->slot(static_cast<uint64_t>(stringStart - doc->root));
			jsonifier::internal::jsonifierMemcpy(slot, scratch, parsed.outLength);
			result = value_type{ slot, parsed.outLength };
			return error_code::success;
		}

		template<uint32_t leadingWord, uint64_t atomSize> JSONIFIER_INLINE bool atomMatches() const noexcept {
			read_buffer_ptr valueStart = doc->root + *iter;
			if (static_cast<uint64_t>(doc->end - valueStart) < atomSize) [[unlikely]] {
				return false;
			}
			uint32_t comparison;
			pow2MemcpyWrapper<4>(&comparison, valueStart);
			if constexpr (std::endian::native == std::endian::big) {
				comparison = std::byteswap(comparison);
			}
			if constexpr (atomSize == 5) {
				if (comparison != leadingWord || valueStart[4] != internal::uint_c_v<'e'>) [[unlikely]] {
					return false;
				}
			} else {
				if (comparison != leadingWord) [[unlikely]] {
					return false;
				}
			}
			return valueStart + atomSize == doc->end || internal::validPostPrimitiveTable[static_cast<uint64_t>(valueStart[atomSize])];
		}

		JSONIFIER_INLINE bool trueMatches() const noexcept {
			static constexpr uint32_t trueVal{ 0b01100101'01110101'01110010'01110100 };
			return atomMatches<trueVal, 4>();
		}

		JSONIFIER_INLINE bool falseMatches() const noexcept {
			static constexpr uint32_t falseVal{ 0b01110011'01101100'01100001'01100110 };
			return atomMatches<falseVal, 5>();
		}

		JSONIFIER_INLINE bool nullMatches() const noexcept {
			static constexpr uint32_t nullVal{ 0b01101100'01101100'01110101'01101110 };
			return atomMatches<nullVal, 4>();
		}

		JSONIFIER_INLINE static uint64_t resolveMaskedStop(read_buffer_ptr stringStart, read_buffer_ptr current, string_stop_masks masks, bool sawNonAscii) noexcept {
			const uint64_t beforeStop = (masks.stops & (0 - masks.stops)) - 1;
			return resolveStringStop(stringStart, current + (internal::simd::countrZeroUnsafe(masks.stops) >> stopMaskLaneShift),
				sawNonAscii || (masks.nonAscii & beforeStop) != 0);
		}

		JSONIFIER_INLINE static uint64_t resolveStringStop(read_buffer_ptr stringStart, read_buffer_ptr stop, bool sawNonAscii) noexcept {
			if (*stop != internal::uint_c_v<'"'>) [[unlikely]] {
				return noZeroCopy;
			}
			const uint64_t length = static_cast<uint64_t>(stop - stringStart);
			return !sawNonAscii || validUtf8(stringStart, length) ? length : noZeroCopy;
		}

		JSONIFIER_INLINE static error_code finishNumber(read_buffer_ptr parsedEnd, read_buffer_ptr valueEnd) noexcept {
			if (parsedEnd && (parsedEnd == valueEnd || internal::validPostPrimitiveTable[static_cast<uint64_t>(*parsedEnd)])) [[likely]] {
				return error_code::success;
			}
			return error_code::number_error;
		}

		template<bool allowNonAscii> JSONIFIER_INLINE static string_stop_masks selectStops(string_stop_masks masks) noexcept {
			if constexpr (!allowNonAscii) {
				masks.stops |= masks.nonAscii;
				masks.nonAscii = 0;
			}
			return masks;
		}

		JSONIFIER_INLINE bool isNumberStart() const noexcept {
			return err == error_code::success && typeTable[doc->charAt(iter)] == json_type::number;
		}

		JSONIFIER_INLINE static bool isWhitespace(uint8_t c) noexcept {
			return jsonifier::internal::whitespaceTable[c];
		}

		JSONIFIER_INLINE error_code typeError() const noexcept {
			return err != error_code::success ? err : error_code::incorrect_type;
		}

		JSONIFIER_INLINE void consume() const noexcept {
			if (doc->cursor == iter) [[likely]] {
				doc->cursor = iter + 1;
			}
		}

		JSONIFIER_INLINE static bool unescapePointerToken(string_view& token, string& unescapedToken) noexcept;
		JSONIFIER_INLINE static error_code parsePointerIndex(string_view token, uint64_t& index) noexcept;
		JSONIFIER_INLINE value pointerStep(string_view token, string& unescapedToken) const noexcept;

		error_code err{ error_code::success };
		read_structural_index_ptr iter{};
		document_state* doc{};
		uint64_t depth{};
	};

}
