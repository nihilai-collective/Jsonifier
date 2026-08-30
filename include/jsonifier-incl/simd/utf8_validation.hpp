// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/utf8_validation.hpp
// The code below drew heavy inspiration from Dr.Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/containers/array.hpp>
#include <jsonifier-incl/simd/fallback.hpp>
#include <jsonifier-incl/simd/popcount.hpp>
#include <jsonifier-incl/simd/avx.hpp>
#include <jsonifier-incl/simd/neon.hpp>

namespace jsonifier::internal {

	static constexpr uint8_t tooShort	  = 1 << 0;
	static constexpr uint8_t tooLong	  = 1 << 1;
	static constexpr uint8_t overLong3	  = 1 << 2;
	static constexpr uint8_t tooLarge	  = 1 << 3;
	static constexpr uint8_t surrogate	  = 1 << 4;
	static constexpr uint8_t overLong2	  = 1 << 5;
	static constexpr uint8_t twoConts	  = 1 << 7;
	static constexpr uint8_t tooLarge1000 = 1 << 6;
	static constexpr uint8_t overLong4	  = 1 << 6;
	static constexpr uint8_t carry		  = tooShort | tooLong | twoConts;

	using namespace jsonifier::internal::simd;

	alignas(64) static constexpr array<uint8_t, simdBytesPerRegister> byte1HighTable{ [] {
		const uint8_t byte1HighTableRaw[]{ tooLong, tooLong, tooLong, tooLong, tooLong, tooLong, tooLong, tooLong, twoConts, twoConts, twoConts, twoConts, tooShort | overLong2,
			tooShort, tooShort | overLong3 | surrogate, tooShort | tooLarge | tooLarge1000 | overLong4 };
		array<uint8_t, simdBytesPerRegister> returnValue{};
		for (uint64_t x = 0; x < simdBytesPerRegister; ++x) {
			returnValue[x] = byte1HighTableRaw[x % std::size(byte1HighTableRaw)];
		}
		return returnValue;
	}() };

	alignas(64) static constexpr array<uint8_t, simdBytesPerRegister> byte1LowTable{ [] {
		const uint8_t byte2LowTableRaw[]{ carry | overLong3 | overLong2 | overLong4, carry | overLong2, carry, carry, carry | tooLarge, carry | tooLarge | tooLarge1000,
			carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000,
			carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000, carry | tooLarge | tooLarge1000 | surrogate, carry | tooLarge | tooLarge1000,
			carry | tooLarge | tooLarge1000 };
		array<uint8_t, simdBytesPerRegister> returnValue{};
		for (uint64_t x = 0; x < simdBytesPerRegister; ++x) {
			returnValue[x] = byte2LowTableRaw[x % std::size(byte2LowTableRaw)];
		}
		return returnValue;
	}() };

	alignas(64) static constexpr array<uint8_t, simdBytesPerRegister> byte2HighTable{ [] {
		const uint8_t byte2HighTableRaw[]{ tooShort, tooShort, tooShort, tooShort, tooShort, tooShort, tooShort, tooShort,
			tooLong | overLong2 | twoConts | overLong3 | tooLarge1000 | overLong4, tooLong | overLong2 | twoConts | overLong3 | tooLarge,
			tooLong | overLong2 | twoConts | surrogate | tooLarge, tooLong | overLong2 | twoConts | surrogate | tooLarge, tooShort, tooShort, tooShort, tooShort };
		array<uint8_t, simdBytesPerRegister> returnValue{};
		for (uint64_t x = 0; x < simdBytesPerRegister; ++x) {
			returnValue[x] = byte2HighTableRaw[x % std::size(byte2HighTableRaw)];
		}
		return returnValue;
	}() };

	alignas(64) static constexpr uint8_t isIncompleteMax[64]{ 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
		255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
		255, 255, 255, 255, static_cast<uint8_t>(0xF0u - 1), static_cast<uint8_t>(0xE0u - 1), static_cast<uint8_t>(0xC0u - 1) };

	template<typename integer_sequence> struct chunk_loader;

	template<uint64_t... indices> struct chunk_loader<integer_sequence<indices...>> {
		template<uint64_t index> JSONIFIER_INLINE static void impl(simd_array_t& __restrict result, const uint8_t* __restrict src) noexcept {
			result.template set<index>(gatherValuesU<jsonifier_simd_int_t>(std::bit_cast<const jsonifier_simd_int_t* __restrict>(src + index * simdBytesPerRegister)));
		}

		JSONIFIER_INLINE static simd_array_t impl(const uint8_t* __restrict src) noexcept {
			simd_array_t returnValues;
			(impl<indices>(returnValues, src), ...);
			return returnValues;
		}
	};

	template<typename derived_type, typename integer_sequence> struct step_checker;

	template<typename derived_type, uint64_t... indices> struct step_checker<derived_type, integer_sequence<indices...>> {
		template<uint64_t index> JSONIFIER_INLINE void impl(const uint8_t* __restrict src_new) noexcept {
			static_cast<derived_type*>(this)->checkStepImpl(src_new + index * simdBytesPerBlock);
		}

		JSONIFIER_INLINE void impl(const uint8_t* __restrict src_new) noexcept {
			(impl<indices>(src_new), ...);
		}
	};

	struct utf8_checker_new : public step_checker<utf8_checker_new, make_integer_sequence<simdBlocksPerStep>> {
		using step_checker_type = step_checker<utf8_checker_new, make_integer_sequence<simdBlocksPerStep>>;
		jsonifier_simd_int_t prevIncomplete;
		jsonifier_simd_int_t prevInput;
		jsonifier_simd_int_t lookupH;
		jsonifier_simd_int_t lookupL;
		jsonifier_simd_int_t lookup2;
		jsonifier_simd_int_t error;

		JSONIFIER_INLINE void reset() {
			lookupH		   = gatherValues<jsonifier_simd_int_t>(std::bit_cast<const jsonifier_simd_int_t* __restrict>(byte1HighTable.data()));
			lookup2		   = gatherValues<jsonifier_simd_int_t>(std::bit_cast<const jsonifier_simd_int_t* __restrict>(byte2HighTable.data()));
			lookupL		   = gatherValues<jsonifier_simd_int_t>(std::bit_cast<const jsonifier_simd_int_t* __restrict>(byte1LowTable.data()));
			prevIncomplete = jsonifier_simd_int_t{};
			prevInput	   = jsonifier_simd_int_t{};
			error		   = jsonifier_simd_int_t{};
		}

		JSONIFIER_INLINE jsonifier_simd_int_t checkSpecialCases(jsonifier_simd_int_t input, jsonifier_simd_int_t p1) {
			const jsonifier_simd_int_t lo_nibble_mask = gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x0Fu));
			return opAnd(opAnd(opShuffle(lookupH, opAnd(opSrLi<4>(p1), lo_nibble_mask)), opShuffle(lookupL, opAnd(p1, lo_nibble_mask))),
				opShuffle(lookup2, opAnd(opSrLi<4>(input), lo_nibble_mask)));
		}

		JSONIFIER_INLINE jsonifier_simd_int_t mustBe23Continuation(jsonifier_simd_int_t p2, jsonifier_simd_int_t p3) {
			return opOr(opSubs(p2, gatherValue<jsonifier_simd_int_t>(static_cast<char>(0xE0u - 0x80u))),
				opSubs(p3, gatherValue<jsonifier_simd_int_t>(static_cast<char>(0xF0u - 0x80u))));
		}

		JSONIFIER_INLINE jsonifier_simd_int_t checkMultibyteLengths(jsonifier_simd_int_t input, jsonifier_simd_int_t prev, jsonifier_simd_int_t sc) {
			return opXor(opAnd(mustBe23Continuation(opPrev<14>(input, prev), opPrev<13>(input, prev)), gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x80u))), sc);
		}

		JSONIFIER_INLINE jsonifier_simd_int_t checkIncomplete(jsonifier_simd_int_t input) {
			return opSubs(input, gatherValues<jsonifier_simd_int_t>(isIncompleteMax + (64 - simdBytesPerRegister)));
		}

		JSONIFIER_INLINE void checkChunk(jsonifier_simd_int_t input, jsonifier_simd_int_t prev) {
			error = opOr(error, checkMultibyteLengths(input, prev, checkSpecialCases(input, opPrev<15>(input, prev))));
		}

		template<typename integer_sequence> struct chunk_processor;

		template<uint64_t... indices> struct chunk_processor<integer_sequence<indices...>> {
			template<uint64_t index> JSONIFIER_INLINE static void impl(utf8_checker_new& __restrict c, simd_array_t chunks, jsonifier_simd_int_t& __restrict prev) noexcept {
				c.checkChunk(chunks.template get<index>(), prev), prev = chunks.template get<index>();
			}

			JSONIFIER_INLINE static void impl(utf8_checker_new& __restrict c, simd_array_t chunks, jsonifier_simd_int_t& __restrict prev) noexcept {
				(impl<indices>(c, chunks, prev), ...);
			}
		};

		JSONIFIER_INLINE void checkStepImpl(const uint8_t* __restrict src) {
			simd_array_t chunks = chunk_loader<make_integer_sequence<registersPerBlock>>::impl(src);

			if (isAscii(simd::orAll<jsonifier_simd_int_t>(chunks))) {
				error		   = opOr(error, prevIncomplete);
				prevInput	   = chunks.template get<registersPerBlock - 1>();
				prevIncomplete = jsonifier_simd_int_t{};
				return;
			}

			chunk_processor<make_integer_sequence<registersPerBlock>>::impl(*this, chunks, prevInput);

			prevInput	   = chunks.template get<registersPerBlock - 1>();
			prevIncomplete = checkIncomplete(chunks.template get<registersPerBlock - 1>());
		}

		JSONIFIER_INLINE void checkStep(const uint8_t* __restrict src_new) {
			step_checker_type::impl(src_new);
		}

		JSONIFIER_INLINE bool errors() {
			return !opTest(opOr(error, prevIncomplete));
		}
	};

	JSONIFIER_INLINE bool validateUtf8(const uint8_t* __restrict src, uint64_t len) {
		if (len == 0) {
			return true;
		}

		utf8_checker_new checker{};
		checker.reset();

		uint64_t i = 0;

		for (; i + simdBytesPerStep <= len; i += simdBytesPerStep) {
			checker.checkStep(src + i);
		}

		if (i < len) {
			alignas(64) uint8_t tmp[simdBytesPerStep];
			std::memset(tmp, 0x41, simdBytesPerStep);
			std::memcpy(tmp, src + i, len - i);
			checker.checkStep(tmp);
		}

		return !checker.errors();
	}

	struct utf8_validation_state {
		uint8_t prevBytes[3];
		bool prevIncomplete;
		bool error;

		JSONIFIER_INLINE void reset() noexcept {
			prevBytes[0]   = 0;
			prevBytes[1]   = 0;
			prevBytes[2]   = 0;
			prevIncomplete = false;
			error		   = false;
		}
	};

	template<typename simd_type, typename integer_type> struct utf8_register_validator {
		static constexpr uint64_t bytesProcessed = sizeof(simd_type);
		using simd_type_alias					 = simd_type;
		using integer_type_alias				 = integer_type;
		utf8_validation_state& state;
		simd_type incompleteRegister;
		simd_type prevInput;
		simd_type lookupH;
		simd_type lookupL;
		simd_type lookup2;
		simd_type error;

		JSONIFIER_INLINE utf8_register_validator& operator=(const utf8_register_validator&) = delete;
		JSONIFIER_INLINE utf8_register_validator(const utf8_register_validator&)			= delete;

		JSONIFIER_INLINE utf8_register_validator(utf8_validation_state& stateNew) noexcept : state{ stateNew } {
			alignas(64) uint8_t tmp[bytesProcessed]{};
			tmp[bytesProcessed - 3] = state.prevBytes[0];
			tmp[bytesProcessed - 2] = state.prevBytes[1];
			tmp[bytesProcessed - 1] = state.prevBytes[2];
			incompleteRegister		= state.prevIncomplete ? simd::gatherValue<simd_type>(static_cast<char>(0x80u)) : simd_type{};
			lookupH					= simd::gatherValues<simd_type>(std::bit_cast<const simd_type* __restrict>(byte1HighTable.data()));
			lookup2					= simd::gatherValues<simd_type>(std::bit_cast<const simd_type* __restrict>(byte2HighTable.data()));
			lookupL					= simd::gatherValues<simd_type>(std::bit_cast<const simd_type* __restrict>(byte1LowTable.data()));
			prevInput				= simd::gatherValues<simd_type>(std::bit_cast<const simd_type* __restrict>(+tmp));
			error					= simd_type{};
		}

		JSONIFIER_INLINE simd_type checkSpecialCases(simd_type input, simd_type p1) noexcept {
			const simd_type loNibbleMask = simd::gatherValue<simd_type>(static_cast<char>(0x0Fu));
			return simd::opAnd(simd::opAnd(simd::opShuffle(lookupH, simd::opAnd(simd::opSrLi<4>(p1), loNibbleMask)), simd::opShuffle(lookupL, simd::opAnd(p1, loNibbleMask))),
				simd::opShuffle(lookup2, simd::opAnd(simd::opSrLi<4>(input), loNibbleMask)));
		}

		JSONIFIER_INLINE simd_type mustBe23Continuation(simd_type p2, simd_type p3) noexcept {
			return simd::opOr(simd::opSubs(p2, simd::gatherValue<simd_type>(static_cast<char>(0xE0u - 0x80u))),
				simd::opSubs(p3, simd::gatherValue<simd_type>(static_cast<char>(0xF0u - 0x80u))));
		}

		JSONIFIER_INLINE void checkRegister(simd_type input) noexcept {
			if (simd::isAscii(input)) {
				error			   = simd::opOr(error, incompleteRegister);
				prevInput		   = input;
				incompleteRegister = simd_type{};
				return;
			}
			const simd_type sc = checkSpecialCases(input, simd::opPrev<15>(input, prevInput));
			error			   = simd::opOr(error,
							 simd::opXor(simd::opAnd(mustBe23Continuation(simd::opPrev<14>(input, prevInput), simd::opPrev<13>(input, prevInput)),
											 simd::gatherValue<simd_type>(static_cast<char>(0x80u))),
								 sc));
			prevInput		   = input;
			incompleteRegister = simd::opSubs(input, simd::gatherValues<simd_type>(isIncompleteMax + (64 - bytesProcessed)));
		}

		JSONIFIER_INLINE void checkPartial(const void* __restrict src, uint64_t count) noexcept {
			if (count == 0) {
				return;
			}
			alignas(64) uint8_t tmp[bytesProcessed];
			std::memset(tmp, 0x41, bytesProcessed);
			std::memcpy(tmp, src, count);
			checkRegister(simd::gatherValues<simd_type>(std::bit_cast<const simd_type* __restrict>(+tmp)));
		}

		JSONIFIER_INLINE void flush() noexcept {
			alignas(64) uint8_t tmp[bytesProcessed];
			simd::store(prevInput, tmp);
			state.prevBytes[0]	 = tmp[bytesProcessed - 3];
			state.prevBytes[1]	 = tmp[bytesProcessed - 2];
			state.prevBytes[2]	 = tmp[bytesProcessed - 1];
			state.prevIncomplete = tmp[bytesProcessed - 1] >= 0xC0u || tmp[bytesProcessed - 2] >= 0xE0u || tmp[bytesProcessed - 3] >= 0xF0u;
			if (!simd::opTest(error)) {
				state.error = true;
			}
		}

		JSONIFIER_INLINE bool errors() noexcept {
			return state.error || !simd::opTest(simd::opOr(error, incompleteRegister));
		}
	};

}

namespace jsonifier {

	JSONIFIER_INLINE bool validateUtf8(const uint8_t* __restrict src, uint64_t len) {
		return internal::validateUtf8(src, len);
	}

}
