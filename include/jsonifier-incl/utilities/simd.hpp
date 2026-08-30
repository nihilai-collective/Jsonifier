// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/simd.hpp
// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/add_tape_values.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/core/fastio.hpp>

namespace jsonifier::internal {

	struct string_block_reader {
		static constexpr uint64_t stepBytes = simdBlocksPerStep * 64;
		JSONIFIER_INLINE void reset(string_view_ptr __restrict stringViewNew, uint64_t lengthNew) noexcept {
			lengthMinusStep = lengthNew < stepBytes ? 0 : lengthNew - stepBytes;
			inString		= std::bit_cast<const uint8_t*>(stringViewNew);
			length			= lengthNew;
			index			= 0;
		}

		JSONIFIER_INLINE const uint8_t* getRemainder() noexcept {
			if (length == index) [[unlikely]] {
				return nullptr;
			}
			uint8_t* __restrict blockPtr	 = +block;
			const uint8_t* __restrict srcPtr = inString + index;
			std::memset(blockPtr + (length - index), static_cast<uint8_t>(0x20), stepBytes - (length - index));
			std::memcpy(blockPtr, srcPtr, length - index);
			return +block;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return length - index;
		}

		JSONIFIER_INLINE const uint8_t* fullBlock() noexcept {
			const uint8_t* __restrict newPtr = inString + index;
			index += stepBytes;
			return newPtr;
		}

		JSONIFIER_INLINE bool hasFullBlock() const noexcept {
			return index < lengthMinusStep;
		}

	  protected:
		alignas(64) uint8_t block[stepBytes]{};
		const uint8_t* __restrict inString{};
		uint64_t lengthMinusStep{};
		uint64_t length{};
		uint64_t index{};
	};

	inline static void printBitsAligned(uint64_t bits, string_view_ptr label, string_view_ptr str = nullptr, uint64_t len = 0) noexcept {
		out << label << ":" << endl;
		if (str && len > 0) {
			out << "STR:  ";
			for (uint64_t i = 0; i < std::min<uint64_t>(len, 64); ++i) {
				char c = str[i];
				if (c == '\n' || c == '\r' || c == '\t')
					c = ' ';
				out << c;
			}
			out << endl;
		}
		out << "BITS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << ((bits >> i) & 1ULL);
		}
		out << endl;
		out << "IDX:  ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << (i % 10);
		}
		out << endl;
		out << "TENS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			out << ((i / 10) % 10);
		}
		out << endl << endl;
	}

	struct rope_block {
		uint64_t inString{};
		uint64_t escaped{};
		uint64_t quotes{};

		JSONIFIER_INLINE uint64_t stringTail() const noexcept {
			return inString ^ quotes;
		}

		JSONIFIER_INLINE uint64_t nonQuoteOutsideString(uint64_t mask) const noexcept {
			return mask & ~inString;
		}
	};

	template<uint64_t initialBufferSize>
	struct simd_string_reader : simd::rope_detector<rope_block>, string_block_reader, add_tape_values<make_integer_sequence<simdBlocksPerStep>>, alloc_wrapper<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		static constexpr uint64_t initialTapeSize{ initialBufferSize * 8 / 10 };
		using allocator = alloc_wrapper<uint32_t>;

		JSONIFIER_INLINE simd_string_reader() noexcept {
			tape	 = allocator::allocate(initialTapeSize);
			capacity = initialTapeSize;
		}

		template<bool minified> JSONIFIER_INLINE void reset(string_view_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity = (stringLength * 8 / 10) + 64;
			if (neededCapacity > capacity) {
				auto newTape = allocator::allocate(neededCapacity);
				allocator::deallocate(tape, capacity);
				tape	 = newTape;
				capacity = neededCapacity;
			}

			tapeCount = 0;
			string_block_reader::reset(rootIter, stringLength);
			simd::rope_detector<rope_block>::prevInString  = 0;
			simd::rope_detector<rope_block>::prevScalar	   = 0;
			simd::rope_detector<rope_block>::nextIsEscaped = 0;

			const jsonifier_simd_int_t bsRegister	 = simd::gatherValue<jsonifier_simd_int_t>('\\');
			const jsonifier_simd_int_t quoteRegister = simd::gatherValue<jsonifier_simd_int_t>('"');
			const jsonifier_simd_int_t opTable		 = simd::gatherValues<jsonifier_simd_int_t>(simd::opArray<simdBytesPerRegister>.data());
			const jsonifier_simd_int_t spaceMask	 = simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x20));

			if constexpr (minified) {
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask);
			} else {
				const jsonifier_simd_int_t whitespaceTableLocal = simd::gatherValues<jsonifier_simd_int_t>(simd::whitespaceArray<simdBytesPerRegister>.data());
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask, whitespaceTableLocal);
			}
		}

		JSONIFIER_INLINE structural_index_ptr end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE structural_index_ptr begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(string_block_reader::length);
			return tape;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		JSONIFIER_INLINE ~simd_string_reader() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

	  protected:
		structural_index_ptr __restrict tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};

		template<bool minified, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void resetImpl(const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			while (string_block_reader::hasFullBlock()) {
				const uint64_t stepBaseIndex = string_block_reader::index;
				processBlocks(string_block_reader::fullBlock(), stepBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
			}

			if (const uint64_t remaining = string_block_reader::getRemainderBytes(); remaining != 0) {
				processBlocks(string_block_reader::getRemainder(), string_block_reader::index, bsRegister, quoteRegister, opTable, spaceMask, args...);
				const uint64_t excess = stepBytes - remaining;
				while (excess > 0 && tapeCount > 0 && tape[tapeCount - 1] >= string_block_reader::length) {
					--tapeCount;
				}
			}
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask) noexcept {
			const uint64_t op		   = simd::op_collector::impl(in_01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
			const uint64_t whitespace  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
			const uint64_t op		   = simd::op_collector::impl(in_01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		template<uint64_t I, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processBlocksImpl(array<uint64_t, simdBlocksPerStep>& __restrict bitsArr,
			array<uint64_t, simdBlocksPerStep>& __restrict cntsArr, const uint8_t* __restrict blockPtr, const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			simd_array_t inVals;
			inVals.template set<0>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64));
			if constexpr (registersPerBlock > 1) {
				inVals.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 1));
				if constexpr (registersPerBlock > 2) {
					inVals.template set<2>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 2));
					inVals.template set<3>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 3));
				}
			}
			simd::rope_detector<rope_block>::next(inVals, bsRegister, quoteRegister);
			const uint64_t structurals = getStructurals(inVals, opTable, spaceMask, args...) & ~simd::rope_detector<rope_block>::stringTail();
			bitsArr[I]				   = structurals;
			cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
		}

		template<typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processBlocks(const uint8_t* __restrict blockPtr, uint64_t stepBaseIndex,
			const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_types... args) noexcept {
			array<uint64_t, simdBlocksPerStep> bitsArr;
			array<uint64_t, simdBlocksPerStep> cntsArr;
			processBlocksImpl<0>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (simdBlocksPerStep > 1) {
				processBlocksImpl<1>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (simdBlocksPerStep > 2) {
					processBlocksImpl<2>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (simdBlocksPerStep > 4) {
						processBlocksImpl<4>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}

			add_tape_values<make_integer_sequence<simdBlocksPerStep>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);

			tapeCount += cntsArr[0];
			if constexpr (simdBlocksPerStep > 1) {
				tapeCount += cntsArr[1];
				if constexpr (simdBlocksPerStep > 2) {
					tapeCount += cntsArr[2];
					tapeCount += cntsArr[3];
					if constexpr (simdBlocksPerStep > 4) {
						tapeCount += cntsArr[4];
						tapeCount += cntsArr[5];
						tapeCount += cntsArr[6];
						tapeCount += cntsArr[7];
					}
				}
			}
		}
	};

}
