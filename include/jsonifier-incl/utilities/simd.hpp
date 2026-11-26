/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/simd.hpp
 */
// Basis of this algorithm sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
#pragma once

#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/utilities/utility.hpp>
#include <jsonifier-incl/simd/add_tape_values.hpp>
#include <jsonifier-incl/simd/avx_stage1.hpp>
#include <jsonifier-incl/simd/neon_stage1.hpp>
#include <jsonifier-incl/core/fastio.hpp>

namespace jsonifier::internal {

	inline void printBitsAligned(uint64_t bits, const char* label, read_buffer_ptr str = nullptr, uint64_t len = 0) noexcept {
		cout << label << ":" << endl;
		if (str && len > 0) {
			cout << "STR:  ";
			for (uint64_t i{ 0 }; i < std::min<uint64_t>(len, 64); ++i) {
				char c{ static_cast<char>(str[i]) };
				if (c == '\n' || c == '\r' || c == '\t') {
					c = ' ';
				}
				cout << c;
			}
			cout << endl;
		}
		cout << "BITS: ";
		for (uint64_t i{ 0 }; i < 64; ++i) {
			cout << ((bits >> i) & 1ULL);
		}
		cout << endl;
		cout << "IDX:  ";
		for (uint64_t i{ 0 }; i < 64; ++i) {
			cout << (i % 10);
		}
		cout << endl;
		cout << "TENS: ";
		for (uint64_t i{ 0 }; i < 64; ++i) {
			cout << ((i / 10) % 10);
		}
		cout << endl << endl;
	}

	template<typename integer_sequence> struct tail_block_steps;

	template<uint64_t... indices> struct tail_block_steps<integer_sequence<indices...>> {
		template<uint64_t offset>
		JSONIFIER_INLINE static void step(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, const jsonifier_simd_int_t lanes, const jsonifier_simd_int_t spaces) noexcept {
			const uint64_t lanesLeft{ remaining > offset ? std::min<uint64_t>(remaining - offset, simdBytesPerRegister) : 0 };
			const jsonifier_simd_int_t keep{ simd::opCmpLtRaw(lanes, simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(lanesLeft))) };
			const jsonifier_simd_int_t data{ simd::gatherValuesU<jsonifier_simd_int_t>(src + offset) };
			simd::storeU(simd::opXor(spaces, simd::opAnd(simd::opXor(data, spaces), keep)), dst + offset);
		}

		JSONIFIER_INLINE static void impl(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, const jsonifier_simd_int_t lanes, const jsonifier_simd_int_t spaces) noexcept {
			(step<indices * simdBytesPerRegister>(dst, src, remaining, lanes, spaces), ...);
		}
	};

	struct tail_block_builder {
		static constexpr uint64_t pageSize{ 4096 };

		alignas(64) static constexpr uint8_t laneIndices[64]{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
			32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 };

		template<uint64_t paddedBytes> JSONIFIER_INLINE static void buildInPlace(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining) noexcept {
			const jsonifier_simd_int_t spaces{ simd::gatherValue<jsonifier_simd_int_t, 0x20>() };
			const jsonifier_simd_int_t lanes{ simd::gatherValues<jsonifier_simd_int_t>(+laneIndices) };
			tail_block_steps<make_integer_sequence<(paddedBytes + simdBytesPerRegister - 1) / simdBytesPerRegister>>::impl(dst, src, remaining, lanes, spaces);
		}

		template<uint64_t paddedBytes> JSONIFIER_INLINE static void buildByCopy(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining) noexcept {
			std::memset(dst + remaining, static_cast<uint8_t>(0x20), paddedBytes - remaining);
			jsonifierMemcpyUpTo<paddedBytes - 1>(dst, src, remaining);
		}

		template<uint64_t paddedBytes> JSONIFIER_INLINE static void impl(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining) noexcept {
			if (readStaysInPage<paddedBytes>(src, remaining)) [[likely]] {
				buildInPlace<paddedBytes>(dst, src, remaining);
			} else {
				buildByCopy<paddedBytes>(dst, src, remaining);
			}
		}

		template<uint64_t paddedBytes> JSONIFIER_INLINE static bool readStaysInPage(read_buffer_ptr src, uint64_t remaining) noexcept {
			if constexpr (JSONIFIER_ASAN_ENABLED) {
				return false;
			} else {
				const uint64_t lastValid{ std::bit_cast<uint64_t>(src) + remaining - 1 };
				const uint64_t lastRead{ std::bit_cast<uint64_t>(src) + paddedBytes - 1 };
				return (lastValid ^ lastRead) < pageSize;
			}
		}
	};

	enum class lone_value_classes : uint8_t {
		scalar,
		whitespace,
		structural,
	};

	consteval array<lone_value_classes, 256> generateLoneValueClasses() noexcept {
		array<lone_value_classes, 256> returnValues{};
		for (uint64_t x{ 0 }; x < 256; ++x) {
			returnValues[x] = lone_value_classes::scalar;
		}
		returnValues[uint_c_v<' '>]	 = lone_value_classes::whitespace;
		returnValues[uint_c_v<'\t'>] = lone_value_classes::whitespace;
		returnValues[uint_c_v<'\n'>] = lone_value_classes::whitespace;
		returnValues[uint_c_v<'\r'>] = lone_value_classes::whitespace;
		returnValues[uint_c_v<'"'>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<','>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<':'>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<'['>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<']'>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<'{'>]	 = lone_value_classes::structural;
		returnValues[uint_c_v<'}'>]	 = lone_value_classes::structural;
		return returnValues;
	}

	inline constexpr array<lone_value_classes, 256> loneValueClasses{ generateLoneValueClasses() };

	template<bool minified, bool acceptStrings> struct lone_value_scanner {
		static constexpr uint64_t noLoneValue{ std::numeric_limits<uint64_t>::max() };

		JSONIFIER_INLINE static uint64_t impl(read_buffer_ptr rootIter, uint64_t length) noexcept {
			uint64_t index{};
			if constexpr (!minified) {
				while (index < length && loneValueClasses[rootIter[index]] == lone_value_classes::whitespace) {
					++index;
				}
			}
			if (index == length) [[unlikely]] {
				return noLoneValue;
			}
			const uint64_t valueStart{ index };
			if (rootIter[index] == uint_c_v<'"'>) {
				if constexpr (!acceptStrings) {
					return noLoneValue;
				}
				uint64_t valueEnd{ length };
				if constexpr (!minified) {
					while (loneValueClasses[rootIter[valueEnd - 1]] == lone_value_classes::whitespace) {
						--valueEnd;
					}
				}
				if (valueEnd - valueStart < 2 || rootIter[valueEnd - 1] != uint_c_v<'"'>) [[unlikely]] {
					return noLoneValue;
				}
				uint64_t backslashCount{};
				while (rootIter[valueEnd - 2 - backslashCount] == uint_c_v<'\\'>) {
					++backslashCount;
				}
				return (backslashCount & 1) == 0 ? valueStart : noLoneValue;
			} else {
				while (index < length && loneValueClasses[rootIter[index]] == lone_value_classes::scalar) {
					++index;
				}
			}
			if constexpr (!minified) {
				while (index < length && loneValueClasses[rootIter[index]] == lone_value_classes::whitespace) {
					++index;
				}
			}
			return index == length ? valueStart : noLoneValue;
		}
	};

	struct string_block_reader {
		template<uint64_t paddedBytes> JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> getRemainder() noexcept {
			tail_block_builder::template impl<paddedBytes>(+block, inString + index, length - index);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			inString = std::bit_cast<read_buffer_ptr>(stringViewNew);
			length	 = lengthNew;
			index	 = 0;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return length - index;
		}

		alignas(simdBytesPerRegister) uint8_t block[simdBytesPerBlock]{};
		read_buffer_ptr inString{};
		uint64_t length{};
		uint64_t index{};
	};

	struct string_block_reader_locals {
		template<uint64_t paddedBytes> JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> getRemainder() noexcept {
			tail_block_builder::template impl<paddedBytes>(+block, inString + index, length - index);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			inString = std::bit_cast<read_buffer_ptr>(stringViewNew);
			length	 = lengthNew;
			index	 = 0;
		}

		alignas(simdBytesPerRegister) uint8_t block[simdBytesPerBlock]{};
		read_buffer_ptr inString{};
		uint64_t length{};
		uint64_t index{};
	};

	struct rope_block {
		JSONIFIER_INLINE uint64_t nonQuoteOutsideString(uint64_t mask) const noexcept {
			return mask & ~inString;
		}

		JSONIFIER_INLINE uint64_t stringTail() const noexcept {
			return inString ^ quotes;
		}

		uint64_t inString{};
		uint64_t escaped{};
		uint64_t quotes{};
	};

	template<bool minified, uint64_t registerBytes, uint64_t registerCount> struct narrow_tail_block {
		using simd_type = simd_register_t<registerBytes>;

		JSONIFIER_INLINE static uint64_t impl(simd::rope_detector<rope_block>& __restrict rope, read_buffer_ptr blockPtr, write_structural_index_ptr tapePtr,
			uint64_t blockBaseIndex) noexcept {
			pod_simd_array_t<registerCount, registerBytes> inVals;
			inVals.template set<0>(simd::gatherValuesU<simd_type>(blockPtr));
			if constexpr (registerCount > 1) {
				inVals.template set<1>(simd::gatherValuesU<simd_type>(blockPtr + registerBytes));
			}
			rope.template nextScalar<registerBytes, registerCount>(inVals, simd::gatherValue<simd_type, '\\'>(), simd::gatherValue<simd_type, '"'>());
			const uint64_t op = simd::scalar_op_collector<registerBytes, registerCount>::impl(inVals, simd::gatherValues<simd_type>(simd::opArray<registerBytes>.data()),
				simd::gatherValue<simd_type, 0x20>());
			uint64_t whitespace{};
			if constexpr (!minified) {
				whitespace = simd::pod_ws_collector<registerBytes, registerCount>::impl(inVals, simd::gatherValues<simd_type>(simd::whitespaceArray<registerBytes>.data()));
			}
			const uint64_t scalar  = ~(op | whitespace | rope.quotes);
			const uint64_t follows = rope.followsNonquoteScalar(scalar);
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			bitsArr[0] = (op | rope.quotes | (scalar & ~follows)) & ~rope.stringTail();
			cntsArr[0] = simd::tape_writer_op::correctedPopcount(bitsArr[0]);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tapePtr, blockBaseIndex);
			return cntsArr[0];
		}
	};

	template<bool minified> struct narrow_tail_dispatch {
		JSONIFIER_INLINE static bool impl([[maybe_unused]] simd::rope_detector<rope_block>& __restrict rope, [[maybe_unused]] read_buffer_ptr blockPtr,
			[[maybe_unused]] write_structural_index_ptr tapePtr, [[maybe_unused]] uint64_t blockBaseIndex, [[maybe_unused]] uint64_t remaining,
			[[maybe_unused]] uint64_t& count) noexcept {
			{
#if JSONIFIER_ANY_AVX
				if (remaining <= 16) {
					count = narrow_tail_block<minified, 16, 1>::impl(rope, blockPtr, tapePtr, blockBaseIndex);
					return true;
				}
				if constexpr (simdBytesPerRegister >= 32) {
					if (remaining <= 32) {
						count = narrow_tail_block<minified, 32, 1>::impl(rope, blockPtr, tapePtr, blockBaseIndex);
						return true;
					}
				}
#elif JSONIFIER_NEON
				if (remaining <= 16) {
					count = narrow_tail_block<minified, 16, 1>::impl(rope, blockPtr, tapePtr, blockBaseIndex);
					return true;
				}
				if (remaining <= 32) {
					count = narrow_tail_block<minified, 16, 2>::impl(rope, blockPtr, tapePtr, blockBaseIndex);
					return true;
				}
#endif
			}
			return false;
		}
	};

	template<uint64_t initialBufferSize> struct simd_string_reader_members : simd::rope_detector<rope_block>,
																			 string_block_reader,
																			 add_tape_values<make_integer_sequence<simdBlocksPerStep>>,
																			 aligned_allocator<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		using allocator = aligned_allocator<uint32_t>;

		JSONIFIER_INLINE simd_string_reader_members() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		JSONIFIER_INLINE simd_string_reader_members& operator=(simd_string_reader_members&& other) noexcept {
			if (&other != this) {
				swap(tapeCount, other.tapeCount);
				swap(capacity, other.capacity);
				swap(string_block_reader::length, other.string_block_reader::length);
				swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader_members(simd_string_reader_members&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		simd_string_reader_members& operator=(const simd_string_reader_members& other) = delete;
		simd_string_reader_members(const simd_string_reader_members& other)			   = delete;

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity{ stringLength + 64 };
			if (neededCapacity > capacity) {
				auto newTape{ allocator::allocate(neededCapacity) };
				allocator::deallocate(tape, capacity);
				tape	 = newTape;
				capacity = neededCapacity;
			}

			tapeCount = 0;
			string_block_reader::reset(rootIter, stringLength);
			simd::rope_detector<rope_block>::prevInString  = 0;
			simd::rope_detector<rope_block>::prevScalar	   = 0;
			simd::rope_detector<rope_block>::nextIsEscaped = 0;

			const jsonifier_simd_int_t bsRegister{ simd::gatherValue<jsonifier_simd_int_t, '\\'>() };
			const jsonifier_simd_int_t quoteRegister{ simd::gatherValue<jsonifier_simd_int_t, '"'>() };
			const jsonifier_simd_int_t opTable{ simd::gatherValues<jsonifier_simd_int_t>(simd::opArray<simdBytesPerRegister>.data()) };
			const jsonifier_simd_int_t spaceMask{ simd::gatherValue<jsonifier_simd_int_t, 0x20>() };

			if constexpr (minified) {
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask);
			} else {
				const jsonifier_simd_int_t whitespaceTableLocal{ simd::gatherValues<jsonifier_simd_int_t>(simd::whitespaceArray<simdBytesPerRegister>.data()) };
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask, whitespaceTableLocal);
			}
		}

		template<bool minified, bool acceptStrings = true> JSONIFIER_INLINE bool tryResetLoneValue(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t valueStart{ lone_value_scanner<minified, acceptStrings>::impl(rootIter, stringLength) };
			if (valueStart == lone_value_scanner<minified, acceptStrings>::noLoneValue) {
				return false;
			}
			string_block_reader::reset(rootIter, stringLength);
			tape[0]	  = static_cast<uint32_t>(valueStart);
			tapeCount = 1;
			return true;
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(string_block_reader::length);
			return tape;
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		JSONIFIER_INLINE ~simd_string_reader_members() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

	  protected:
		template<uint64_t blockCount = simdBlocksPerStep, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processBlocks(read_buffer_ptr blockPtr,
			uint64_t stepBaseIndex, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			array<uint64_t, blockCount> bitsArr;
			array<uint64_t, blockCount> cntsArr;
			processBlocksImpl<0>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (blockCount > 1) {
				processBlocksImpl<1>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (blockCount > 2) {
					processBlocksImpl<2>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (blockCount > 4) {
						processBlocksImpl<4>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}

			add_tape_values<make_integer_sequence<blockCount>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);

			tapeCount += cntsArr[0];
			if constexpr (blockCount > 1) {
				tapeCount += cntsArr[1];
				if constexpr (blockCount > 2) {
					tapeCount += cntsArr[2];
					tapeCount += cntsArr[3];
					if constexpr (blockCount > 4) {
						tapeCount += cntsArr[4];
						tapeCount += cntsArr[5];
						tapeCount += cntsArr[6];
						tapeCount += cntsArr[7];
					}
				}
			}
		}

		template<bool minified, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void resetImpl(const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			drainCascade<simdBlocksPerStep>(bsRegister, quoteRegister, opTable, spaceMask, args...);

			if (const uint64_t remaining = string_block_reader::getRemainderBytes(); remaining != 0) {
				read_buffer_ptr tailPtr = string_block_reader::template getRemainder<simdBytesPerBlock>();
				uint64_t narrowCount{};
				if (narrow_tail_dispatch<minified>::impl(*this, tailPtr, tape + tapeCount, string_block_reader::index, remaining, narrowCount)) {
					tapeCount += narrowCount;
				} else {
					processTailBlock(tailPtr, string_block_reader::index, bsRegister, quoteRegister, opTable, spaceMask, args...);
				}
				while (tapeCount > 0 && tape[tapeCount - 1] >= string_block_reader::length) {
					--tapeCount;
				}
			}
		}

		template<uint64_t blockCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void drainCascade(const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			while (string_block_reader::getRemainderBytes() >= blockCount * 64) {
				const uint64_t stepBaseIndex{ string_block_reader::index };
				processBlocks<blockCount>(string_block_reader::inString + stepBaseIndex, stepBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
				string_block_reader::index += blockCount * 64;
			}
			if constexpr (blockCount > 1) {
				drainCascade<blockCount / 2>(bsRegister, quoteRegister, opTable, spaceMask, args...);
			}
		}

		template<uint64_t currentBlock, uint64_t laneCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processBlocksImpl(array<uint64_t, laneCount>& bitsArr,
			array<uint64_t, laneCount>& cntsArr, read_buffer_ptr blockPtr, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister,
			const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			simd_array_t inVals;
			inVals.template set<0>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64));
			if constexpr (simdRegistersPerBlock > 1) {
				inVals.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 1));
				if constexpr (simdRegistersPerBlock > 2) {
					inVals.template set<2>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 2));
					inVals.template set<3>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 3));
				}
			}
			simd::rope_detector<rope_block>::next(inVals, bsRegister, quoteRegister);
			const uint64_t structurals{ getStructurals(inVals, opTable, spaceMask, args...) & ~simd::rope_detector<rope_block>::stringTail() };
			bitsArr[currentBlock] = structurals;
			cntsArr[currentBlock] = simd::tape_writer_op::correctedPopcount(structurals);
		}

		template<typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processTailBlock(read_buffer_ptr blockPtr, uint64_t blockBaseIndex,
			const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_types... args) noexcept {
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			processBlocksImpl<0>(bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tape + tapeCount, blockBaseIndex);
			tapeCount += cntsArr[0];
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask) noexcept {
			const uint64_t op{ simd::op_collector::impl(in01, opTable, spaceMask) };
			const uint64_t scalar{ ~(op | simd::rope_detector<rope_block>::quotes) };
			const uint64_t follows{ simd::rope_detector<rope_block>::followsNonquoteScalar(scalar) };
			const uint64_t scalarStart{ scalar & ~follows };
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
#if JSONIFIER_NEON
			const uint64x2_t wsOp{ simd::ws_op_collector::impl(in01, opTable, spaceMask, whitespaceTableLocal) };
			const uint64_t whitespace{ vgetq_lane_u64(wsOp, 0) };
			const uint64_t op{ vgetq_lane_u64(wsOp, 1) };
#else
			const uint64_t whitespace{ simd::ws_collector::impl(in01, whitespaceTableLocal) };
			const uint64_t op{ simd::op_collector::impl(in01, opTable, spaceMask) };
#endif
			const uint64_t scalar{ ~(op | whitespace | simd::rope_detector<rope_block>::quotes) };
			const uint64_t follows{ simd::rope_detector<rope_block>::followsNonquoteScalar(scalar) };
			const uint64_t scalarStart{ scalar & ~follows };
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		write_structural_index_ptr tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
	};

	template<uint64_t initialBufferSize>
	struct simd_string_reader_locals : string_block_reader_locals, add_tape_values<make_integer_sequence<simdBlocksPerStep>>, aligned_allocator<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		using allocator = aligned_allocator<uint32_t>;
		using rope_type = simd::rope_detector<rope_block>;

		JSONIFIER_INLINE simd_string_reader_locals() noexcept {
			tape	 = allocator::allocate(initialBufferSize);
			capacity = initialBufferSize;
		}

		JSONIFIER_INLINE simd_string_reader_locals& operator=(simd_string_reader_locals&& other) noexcept {
			if (&other != this) {
				swap(tapeCount, other.tapeCount);
				swap(capacity, other.capacity);
				swap(string_block_reader_locals::length, other.string_block_reader_locals::length);
				swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader_locals(simd_string_reader_locals&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		simd_string_reader_locals& operator=(const simd_string_reader_locals& other) = delete;
		simd_string_reader_locals(const simd_string_reader_locals& other)			 = delete;

#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON
		template<uint64_t offset> JSONIFIER_INLINE static void collectCountsQuad(const array<uint64_t, simdBlocksPerStep>& __restrict bitsArr,
			array<uint64_t, simdBlocksPerStep>& __restrict cntsArr) noexcept {
			const uint8x16_t counts01{ vcntq_u8(vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(bitsArr[offset]), vcreate_u64(bitsArr[offset + 1])))) };
			const uint8x16_t counts23{ vcntq_u8(vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(bitsArr[offset + 2]), vcreate_u64(bitsArr[offset + 3])))) };
			uint8x16_t sums{ vpaddq_u8(counts01, counts23) };
			sums = vpaddq_u8(sums, sums);
			sums = vpaddq_u8(sums, sums);
			const uint32_t packed{ vgetq_lane_u32(vreinterpretq_u32_u8(sums), 0) };
			cntsArr[offset]		= packed & 0xFFu;
			cntsArr[offset + 1] = (packed >> 8) & 0xFFu;
			cntsArr[offset + 2] = (packed >> 16) & 0xFFu;
			cntsArr[offset + 3] = packed >> 24;
		}

		JSONIFIER_INLINE static void collectCounts(const array<uint64_t, simdBlocksPerStep>& __restrict bitsArr, array<uint64_t, simdBlocksPerStep>& __restrict cntsArr) noexcept {
			collectCountsQuad<0>(bitsArr, cntsArr);
			if constexpr (simdBlocksPerStep > 4) {
				collectCountsQuad<4>(bitsArr, cntsArr);
			}
		}

		static_assert(simdBlocksPerStep == 4 || simdBlocksPerStep == 8);
#endif

		template<uint64_t currentBlock, uint64_t laneCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static void processBlocksImpl(rope_type& __restrict rope,
			array<uint64_t, laneCount>& __restrict bitsArr, array<uint64_t, laneCount>& __restrict cntsArr, read_buffer_ptr blockPtr, const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			simd_array_t inVals;
			inVals.template set<0>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64));
			if constexpr (simdRegistersPerBlock > 1) {
				inVals.template set<1>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 1));
				if constexpr (simdRegistersPerBlock > 2) {
					inVals.template set<2>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 2));
					inVals.template set<3>(simd::gatherValuesU<jsonifier_simd_int_t>(blockPtr + currentBlock * 64 + simdBytesPerRegister * 3));
				}
			}
			rope.next(inVals, bsRegister, quoteRegister);
			const uint64_t structurals{ getStructurals(rope, inVals, opTable, spaceMask, args...) & ~rope.stringTail() };
			bitsArr[currentBlock] = structurals;
#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON
			if constexpr (laneCount != simdBlocksPerStep) {
				cntsArr[currentBlock] = simd::tape_writer_op::correctedPopcount(structurals);
			}
#else
			cntsArr[currentBlock] = simd::tape_writer_op::correctedPopcount(structurals);
#endif
		}

		template<bool minified, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void resetImpl(const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			rope_type rope{};
			write_structural_index_ptr tapeLocal{ tape };
			read_buffer_ptr srcPtr{ string_block_reader_locals::inString };
			const uint64_t lengthLocal{ string_block_reader_locals::length };
			uint64_t tapeCountLocal{ 0 };
			uint64_t indexLocal{ 0 };

			drainCascade<simdBlocksPerStep>(rope, srcPtr, tapeLocal, tapeCountLocal, indexLocal, lengthLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
			string_block_reader_locals::index = indexLocal;

			if (const uint64_t remaining = lengthLocal - indexLocal; remaining != 0) {
				read_buffer_ptr tailPtr{ string_block_reader_locals::template getRemainder<simdBytesPerBlock>() };
				uint64_t narrowCount{};
				if (narrow_tail_dispatch<minified>::impl(rope, tailPtr, tapeLocal + tapeCountLocal, indexLocal, remaining, narrowCount)) {
					tapeCountLocal += narrowCount;
				} else {
					tapeCountLocal += processTailBlock(rope, tailPtr, tapeLocal + tapeCountLocal, indexLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
				}
				while (tapeCountLocal > 0 && tapeLocal[tapeCountLocal - 1] >= lengthLocal) {
					--tapeCountLocal;
				}
			}
			tapeCount = tapeCountLocal;
		}

		template<uint64_t blockCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static void drainCascade(rope_type& __restrict rope, read_buffer_ptr srcPtr,
			write_structural_index_ptr tapePtr, uint64_t& tapeCountLocal, uint64_t& indexLocal, uint64_t lengthLocal, const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			while (lengthLocal - indexLocal >= blockCount * 64) {
				tapeCountLocal +=
					processBlocks<blockCount>(rope, srcPtr + indexLocal, tapePtr + tapeCountLocal, indexLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
				indexLocal += blockCount * 64;
			}
			if constexpr (blockCount > 1) {
				drainCascade<blockCount / 2>(rope, srcPtr, tapePtr, tapeCountLocal, indexLocal, lengthLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
			}
		}

		template<typename... jsonifier_simd_int_types> JSONIFIER_INLINE static uint64_t processTailBlock(rope_type& __restrict rope, read_buffer_ptr blockPtr,
			write_structural_index_ptr tapePtr, uint64_t blockBaseIndex, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister,
			const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			processBlocksImpl<0>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tapePtr, blockBaseIndex);
			return cntsArr[0];
		}

		template<uint64_t blockCount = simdBlocksPerStep, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static uint64_t processBlocks(rope_type& __restrict rope,
			read_buffer_ptr blockPtr, write_structural_index_ptr tapePtr, uint64_t stepBaseIndex, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister,
			const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			array<uint64_t, blockCount> bitsArr;
			array<uint64_t, blockCount> cntsArr;
			processBlocksImpl<0>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (blockCount > 1) {
				processBlocksImpl<1>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (blockCount > 2) {
					processBlocksImpl<2>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (blockCount > 4) {
						processBlocksImpl<4>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7>(rope, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}
#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON
			if constexpr (blockCount == simdBlocksPerStep) {
				collectCounts(bitsArr, cntsArr);
			}
#endif

			add_tape_values<make_integer_sequence<blockCount>>::impl(bitsArr, cntsArr, tapePtr, stepBaseIndex);

			uint64_t stepCount{ cntsArr[0] };
			if constexpr (blockCount > 1) {
				stepCount += cntsArr[1];
				if constexpr (blockCount > 2) {
					stepCount += cntsArr[2];
					stepCount += cntsArr[3];
					if constexpr (blockCount > 4) {
						stepCount += cntsArr[4];
						stepCount += cntsArr[5];
						stepCount += cntsArr[6];
						stepCount += cntsArr[7];
					}
				}
			}
			return stepCount;
		}

		template<bool minified> JSONIFIER_INLINE void reset(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity{ stringLength + 64 };
			if (neededCapacity > capacity) {
				auto newTape{ allocator::allocate(neededCapacity) };
				allocator::deallocate(tape, capacity);
				tape	 = newTape;
				capacity = neededCapacity;
			}

			string_block_reader_locals::reset(rootIter, stringLength);

			const jsonifier_simd_int_t bsRegister{ simd::gatherValue<jsonifier_simd_int_t, '\\'>() };
			const jsonifier_simd_int_t quoteRegister{ simd::gatherValue<jsonifier_simd_int_t, '"'>() };
			const jsonifier_simd_int_t opTable{ simd::gatherValues<jsonifier_simd_int_t>(simd::opArray<simdBytesPerRegister>.data()) };
			const jsonifier_simd_int_t spaceMask{ simd::gatherValue<jsonifier_simd_int_t, 0x20>() };

			if constexpr (minified) {
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask);
			} else {
				const jsonifier_simd_int_t whitespaceTableLocal{ simd::gatherValues<jsonifier_simd_int_t>(simd::whitespaceArray<simdBytesPerRegister>.data()) };
				resetImpl<minified>(bsRegister, quoteRegister, opTable, spaceMask, whitespaceTableLocal);
			}
		}

		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const simd_array_t in01, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
#if JSONIFIER_NEON
			const uint64x2_t wsOp{ simd::ws_op_collector::impl(in01, opTable, spaceMask, whitespaceTableLocal) };
			const uint64_t whitespace{ vgetq_lane_u64(wsOp, 0) };
			const uint64_t op{ vgetq_lane_u64(wsOp, 1) };
#else
			const uint64_t whitespace{ simd::ws_collector::impl(in01, whitespaceTableLocal) };
			const uint64_t op{ simd::op_collector::impl(in01, opTable, spaceMask) };
#endif
			const uint64_t quotes{ rope.quotes };
			const uint64_t scalar{ ~(op | whitespace | quotes) };
			const uint64_t follows{ rope.followsNonquoteScalar(scalar) };
			const uint64_t scalarStart{ scalar & ~follows };
			return op | quotes | scalarStart;
		}

		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const simd_array_t in01, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask) noexcept {
			const uint64_t op{ simd::op_collector::impl(in01, opTable, spaceMask) };
			const uint64_t quotes{ rope.quotes };
			const uint64_t scalar{ ~(op | quotes) };
			const uint64_t follows{ rope.followsNonquoteScalar(scalar) };
			const uint64_t scalarStart{ scalar & ~follows };
			return op | quotes | scalarStart;
		}

		template<bool minified, bool acceptStrings = true> JSONIFIER_INLINE bool tryResetLoneValue(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t valueStart{ lone_value_scanner<minified, acceptStrings>::impl(rootIter, stringLength) };
			if (valueStart == lone_value_scanner<minified, acceptStrings>::noLoneValue) {
				return false;
			}
			string_block_reader_locals::reset(rootIter, stringLength);
			tape[0]	  = static_cast<uint32_t>(valueStart);
			tapeCount = 1;
			return true;
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(string_block_reader_locals::length);
			return tape;
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		JSONIFIER_INLINE ~simd_string_reader_locals() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

		write_structural_index_ptr tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
	};

#if JSONIFIER_COMPILER_CLANG
	template<uint64_t initialBufferSize> using simd_string_reader = simd_string_reader_members<initialBufferSize>;
#else
	template<uint64_t initialBufferSize> using simd_string_reader = simd_string_reader_locals<initialBufferSize>;
#endif

}
