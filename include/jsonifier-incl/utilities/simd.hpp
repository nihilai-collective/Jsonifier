/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/simd.hpp
 */
// The code below drew heavy inspiration from Dr. Lemire's library, simdjson (https://github.com/simdjson/simdjson)
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/add_tape_values.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/core/fastio.hpp>

namespace jsonifier::internal {

	inline static void printBitsAligned(uint64_t bits, read_buffer_ptr label, read_buffer_ptr __restrict str = nullptr, uint64_t len = 0) noexcept {
		out << label << ":" << endl;
		if (str && len > 0) {
			out << "STR:  ";
			for (uint64_t i = 0; i < std::min<uint64_t>(len, 64); ++i) {
				char c = str[i];
				if (c == '\n' || c == '\r' || c == '\t') {
					c = ' ';
				}
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

	struct string_block_reader {
		static constexpr uint64_t stepBytes = simdBlocksPerStep * 64;
		JSONIFIER_INLINE void reset(read_buffer_ptr __restrict stringViewNew, uint64_t lengthNew) noexcept {
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
			memcpy_wrapper(blockPtr, srcPtr, length - index);
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

	template<uint64_t stepBytes = simdBytesPerStep> struct pod_block_reader {
		static_assert((stepBytes & (stepBytes - 1)) == 0 && stepBytes >= simdBytesPerBlock);

		JSONIFIER_INLINE void reset(read_buffer_ptr __restrict stringViewNew, uint64_t lengthNew) noexcept {
			fullStepEnd	 = lengthNew & ~(stepBytes - 1ull);
			fullBlockEnd = lengthNew & ~(simdBytesPerBlock - 1ull);
			inString	 = std::bit_cast<const uint8_t*>(stringViewNew);
			lengthVal	 = lengthNew;
			indexVal	 = 0;
		}

		JSONIFIER_INLINE const uint8_t* getRemainder() noexcept {
			const uint64_t remaining		 = lengthVal - indexVal;
			uint8_t* __restrict blockPtr	 = +block;
			const uint8_t* __restrict srcPtr = inString + indexVal;
			memcpy_wrapper(blockPtr, srcPtr, remaining);
			std::memset(blockPtr + remaining, static_cast<uint8_t>(0x20), simdBytesPerBlock - remaining);
			return +block;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return lengthVal - indexVal;
		}

		JSONIFIER_INLINE uint64_t index() const noexcept {
			return indexVal;
		}

		JSONIFIER_INLINE uint64_t length() const noexcept {
			return lengthVal;
		}

		JSONIFIER_INLINE const uint8_t* fullStep() noexcept {
			const uint8_t* __restrict newPtr = inString + indexVal;
			indexVal += stepBytes;
			return newPtr;
		}

		JSONIFIER_INLINE bool hasFullStep() const noexcept {
			return indexVal < fullStepEnd;
		}

		JSONIFIER_INLINE const uint8_t* fullBlock() noexcept {
			const uint8_t* __restrict newPtr = inString + indexVal;
			indexVal += simdBytesPerBlock;
			return newPtr;
		}

		JSONIFIER_INLINE bool hasFullBlock() const noexcept {
			return indexVal < fullBlockEnd;
		}

	  protected:
		alignas(64) uint8_t block[simdBytesPerBlock];
		const uint8_t* __restrict inString{};
		uint64_t fullBlockEnd{};
		uint64_t fullStepEnd{};
		uint64_t lengthVal{};
		uint64_t indexVal{};
	};

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
		using allocator = alloc_wrapper<uint32_t>;

		JSONIFIER_INLINE simd_string_reader& operator=(simd_string_reader&& other) noexcept {
			if (&other != this) {
				std::swap(tapeCount, other.tapeCount);
				std::swap(capacity, other.capacity);
				std::swap(string_block_reader::length, other.string_block_reader::length);
				std::swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader(simd_string_reader&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		JSONIFIER_INLINE simd_string_reader& operator=(const simd_string_reader& other) noexcept {
			if (&other != this) {
				if (capacity < other.capacity) {
					auto newTape = allocator::allocate(other.capacity);
					if (tape) {
						allocator::deallocate(tape, capacity);
					}
					tape	 = newTape;
					capacity = other.capacity;
				}
				tapeCount					= other.tapeCount;
				string_block_reader::length = other.string_block_reader::length;
				if (other.tape) {
					memcpy_wrapper(tape, other.tape, sizeof(*tape) * (other.tapeCount + 1));
				}
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader(const simd_string_reader& other) noexcept : allocator{} {
			*this = other;
		}

		JSONIFIER_INLINE simd_string_reader() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity = stringLength + 64;
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
			if constexpr (simdRegistersPerBlock > 1) {
				inVals.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + I * 64 + simdBytesPerRegister * 1));
				if constexpr (simdRegistersPerBlock > 2) {
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

	template<uint64_t initialBufferSize> struct pod_simd_string_reader : simd::rope_detector<rope_block>, alloc_wrapper<uint32_t> {
		using allocator = alloc_wrapper<uint32_t>;

		JSONIFIER_INLINE pod_simd_string_reader& operator=(pod_simd_string_reader&& other) noexcept {
			if (&other != this) {
				std::swap(tapeCount, other.tapeCount);
				std::swap(capacity, other.capacity);
				std::swap(length, other.length);
				std::swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE pod_simd_string_reader(pod_simd_string_reader&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		JSONIFIER_INLINE pod_simd_string_reader& operator=(const pod_simd_string_reader& other) noexcept {
			if (&other != this) {
				if (capacity < other.capacity) {
					auto newTape = allocator::allocate(other.capacity);
					if (tape) {
						allocator::deallocate(tape, capacity);
					}
					tape	 = newTape;
					capacity = other.capacity;
				}
				tapeCount = other.tapeCount;
				length	  = other.length;
				if (other.tape) {
					memcpy_wrapper(tape, other.tape, sizeof(*tape) * (other.tapeCount + 1));
				}
			}
			return *this;
		}

		JSONIFIER_INLINE pod_simd_string_reader(const pod_simd_string_reader& other) noexcept : allocator{} {
			*this = other;
		}

		JSONIFIER_INLINE pod_simd_string_reader() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
			if (stringLength <= 32) {
				return resetDispatch<minified, 32, 1>(rootIter, stringLength);
			}
	#endif
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
			if (stringLength <= 32) {
				return resetDispatch<minified, 16, 2>(rootIter, stringLength);
			}
#endif
			resetDispatch<minified, simdBytesPerRegister, simdRegistersPerBlock>(rootIter, stringLength);
		}

		JSONIFIER_INLINE structural_index_ptr end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE structural_index_ptr begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(length);
			return tape;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		JSONIFIER_INLINE ~pod_simd_string_reader() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

	  protected:
		structural_index_ptr __restrict tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
		uint64_t length{};

		template<bool minified, uint64_t registerBytes, uint64_t registerCount>
		JSONIFIER_INLINE void resetDispatch(read_buffer_ptr __restrict rootIter, uint64_t stringLength) noexcept {
			using simd_type				  = typename simd_register<registerBytes>::type;
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape = allocator::allocate(neededCapacity);
				if (tape) {
					allocator::deallocate(tape, capacity);
				}
				tape	 = newTape;
				capacity = neededCapacity;
			}
			tapeCount									   = 0;
			simd::rope_detector<rope_block>::prevInString  = 0;
			simd::rope_detector<rope_block>::prevScalar	   = 0;
			simd::rope_detector<rope_block>::nextIsEscaped = 0;

			const simd_type bsRegister	  = simd::gatherValue<simd_type>('\\');
			const simd_type quoteRegister = simd::gatherValue<simd_type>('"');
			const simd_type opTable		  = simd::gatherValues<simd_type>(simd::opArray<registerBytes>.data());
			const simd_type spaceMask	  = simd::gatherValue<simd_type>(static_cast<char>(0x20));

			if constexpr (minified) {
				resetImpl<registerBytes, registerCount>(rootIter, stringLength, bsRegister, quoteRegister, opTable, spaceMask);
			} else {
				const simd_type whitespaceTableLocal = simd::gatherValues<simd_type>(simd::whitespaceArray<registerBytes>.data());
				resetImpl<registerBytes, registerCount>(rootIter, stringLength, bsRegister, quoteRegister, opTable, spaceMask, whitespaceTableLocal);
			}
		}

		template<uint64_t registerBytes, uint64_t registerCount, typename... simd_types> JSONIFIER_INLINE void resetImpl(read_buffer_ptr __restrict rootIter, uint64_t stringLength,
			const typename simd_register<registerBytes>::type bsRegister, const typename simd_register<registerBytes>::type quoteRegister,
			const typename simd_register<registerBytes>::type opTable, const typename simd_register<registerBytes>::type spaceMask, const simd_types... args) noexcept {
			static constexpr uint64_t blocksPerStep{ simdBytesPerStep / simdBytesPerBlock };
			pod_block_reader<simdBytesPerStep> stringBlockReader;
			stringBlockReader.reset(rootIter, stringLength);
			if constexpr (registerCount * registerBytes == simdBytesPerBlock) {
				while (stringBlockReader.hasFullStep()) {
					const uint64_t stepBaseIndex = stringBlockReader.index();
					processBlocks<blocksPerStep, registerBytes, registerCount>(stringBlockReader.fullStep(), stepBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
				}

				while (stringBlockReader.hasFullBlock()) {
					const uint64_t blockBaseIndex = stringBlockReader.index();
					processBlocks<1, registerBytes, registerCount>(stringBlockReader.fullBlock(), blockBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
				}
			}

			if (stringBlockReader.getRemainderBytes() != 0) {
				const uint64_t blockBaseIndex = stringBlockReader.index();
				processBlocks<1, registerBytes, registerCount>(stringBlockReader.getRemainder(), blockBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
				while (tapeCount > 0 && tape[tapeCount - 1] >= stringBlockReader.length()) {
					--tapeCount;
				}
			}
			length = stringBlockReader.length();
		}

		template<uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE uint64_t getStructurals(const scalar_simd_array_t<registerCount, registerBytes> in_01,
			const typename simd_register<registerBytes>::type opTable, const typename simd_register<registerBytes>::type spaceMask) noexcept {
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in_01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		template<uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE uint64_t getStructurals(const scalar_simd_array_t<registerCount, registerBytes> in_01,
			const typename simd_register<registerBytes>::type opTable, const typename simd_register<registerBytes>::type spaceMask,
			const typename simd_register<registerBytes>::type whitespaceTableLocal) noexcept {
			const uint64_t whitespace  = simd::pod_ws_collector<registerBytes, registerCount>::impl(in_01, whitespaceTableLocal);
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in_01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		template<uint64_t I, uint64_t registerBytes, uint64_t registerCount, uint64_t blocksPerStep, typename... simd_types>
		JSONIFIER_INLINE void processBlocksImpl(array<uint64_t, blocksPerStep>& __restrict bitsArr, array<uint64_t, blocksPerStep>& __restrict cntsArr,
			const uint8_t* __restrict blockPtr, const typename simd_register<registerBytes>::type bsRegister, const typename simd_register<registerBytes>::type quoteRegister,
			const typename simd_register<registerBytes>::type opTable, const typename simd_register<registerBytes>::type spaceMask, const simd_types... args) noexcept {
			using simd_type = typename simd_register<registerBytes>::type;
			scalar_simd_array_t<registerCount, registerBytes> inVals;
			inVals.template set<0>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock));
			if constexpr (registerCount > 1) {
				inVals.template set<1>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 1));
				if constexpr (registerCount > 2) {
					inVals.template set<2>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 2));
					inVals.template set<3>(simd::gatherValuesU<simd_type>(blockPtr + I * simdBytesPerBlock + registerBytes * 3));
				}
			}
			simd::rope_detector<rope_block>::template nextScalar<registerBytes, registerCount>(inVals, bsRegister, quoteRegister);
			const uint64_t structurals = getStructurals<registerBytes, registerCount>(inVals, opTable, spaceMask, args...) & ~simd::rope_detector<rope_block>::stringTail();
			bitsArr[I]				   = structurals;
			cntsArr[I]				   = simd::tape_writer_op::correctedPopcount(structurals);
		}

		template<uint64_t blocksPerStep, uint64_t registerBytes, uint64_t registerCount, typename... simd_types>
		JSONIFIER_INLINE void processBlocks(const uint8_t* __restrict blockPtr, uint64_t stepBaseIndex, const typename simd_register<registerBytes>::type bsRegister,
			const typename simd_register<registerBytes>::type quoteRegister, const typename simd_register<registerBytes>::type opTable,
			const typename simd_register<registerBytes>::type spaceMask, const simd_types... args) noexcept {
			array<uint64_t, blocksPerStep> bitsArr;
			array<uint64_t, blocksPerStep> cntsArr;
			processBlocksImpl<0, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (blocksPerStep > 1) {
				processBlocksImpl<1, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (blocksPerStep > 2) {
					processBlocksImpl<2, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (blocksPerStep > 4) {
						processBlocksImpl<4, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7, registerBytes, registerCount>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}

			add_tape_values<make_integer_sequence<blocksPerStep>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);

			tapeCount += cntsArr[0];
			if constexpr (blocksPerStep > 1) {
				tapeCount += cntsArr[1];
				if constexpr (blocksPerStep > 2) {
					tapeCount += cntsArr[2];
					tapeCount += cntsArr[3];
					if constexpr (blocksPerStep > 4) {
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
