/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/simd.hpp
 */
// Sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
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
			for (uint64_t i = 0; i < std::min<uint64_t>(len, 64); ++i) {
				char c = static_cast<char>(str[i]);
				if (c == '\n' || c == '\r' || c == '\t') {
					c = ' ';
				}
				cout << c;
			}
			cout << endl;
		}
		cout << "BITS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			cout << ((bits >> i) & 1ULL);
		}
		cout << endl;
		cout << "IDX:  ";
		for (uint64_t i = 0; i < 64; ++i) {
			cout << (i % 10);
		}
		cout << endl;
		cout << "TENS: ";
		for (uint64_t i = 0; i < 64; ++i) {
			cout << ((i / 10) % 10);
		}
		cout << endl << endl;
	}

	struct tail_block_builder {
		static constexpr uint64_t pageSize = 4096;

		alignas(64) static constexpr uint8_t laneIndices[64]{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
			32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 };

		JSONIFIER_INLINE static void buildInPlace(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, uint64_t paddedBytes) noexcept {
			const jsonifier_simd_int_t spaces = simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(0x20));
			const jsonifier_simd_int_t lanes  = simd::gatherValues<jsonifier_simd_int_t>(+laneIndices);
			for (uint64_t offset = 0; offset < paddedBytes; offset += simdBytesPerRegister) {
				const uint64_t lanesLeft		= remaining > offset ? std::min<uint64_t>(remaining - offset, simdBytesPerRegister) : 0;
				const jsonifier_simd_int_t keep = simd::opCmpLtRaw(lanes, simd::gatherValue<jsonifier_simd_int_t>(static_cast<char>(lanesLeft)));
				const jsonifier_simd_int_t data = simd::gatherValuesU<jsonifier_simd_int_t>(src + offset);
				simd::storeU(simd::opXor(spaces, simd::opAnd(simd::opXor(data, spaces), keep)), dst + offset);
			}
		}

		JSONIFIER_INLINE static void buildByCopy(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, uint64_t paddedBytes) noexcept {
			std::memset(dst + remaining, static_cast<uint8_t>(0x20), paddedBytes - remaining);
			jsonifierMemcpy(dst, src, remaining);
		}

#if JSONIFIER_AVX512
		JSONIFIER_INLINE static void buildMasked(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, uint64_t paddedBytes) noexcept {
			const __m512i spaces = _mm512_set1_epi8(0x20);
			for (uint64_t offset = 0; offset < paddedBytes; offset += 64) {
				const uint64_t lanesLeft = remaining > offset ? remaining - offset : 0;
				const __mmask64 keep	 = lanesLeft >= 64 ? ~__mmask64{} : static_cast<__mmask64>((uint64_t{ 1 } << lanesLeft) - 1);
				_mm512_store_si512(dst + offset, _mm512_mask_loadu_epi8(spaces, keep, src + offset));
			}
		}
#endif

		JSONIFIER_INLINE static void impl(write_buffer_ptr dst, read_buffer_ptr src, uint64_t remaining, uint64_t paddedBytes) noexcept {
#if JSONIFIER_AVX512
			if constexpr (!JSONIFIER_ASAN_ENABLED) {
				return buildMasked(dst, src, remaining, paddedBytes);
			}
#endif
			if (readStaysInPage(src, remaining, paddedBytes)) [[likely]] {
				buildInPlace(dst, src, remaining, paddedBytes);
			} else {
				buildByCopy(dst, src, remaining, paddedBytes);
			}
		}

		JSONIFIER_INLINE static bool readStaysInPage(read_buffer_ptr src, uint64_t remaining, uint64_t paddedBytes) noexcept {
			if constexpr (JSONIFIER_ASAN_ENABLED) {
				return false;
			} else {
				const uint64_t lastValid = static_cast<uint64_t>(std::bit_cast<uintptr_t>(src)) + remaining - 1;
				const uint64_t lastRead	 = static_cast<uint64_t>(std::bit_cast<uintptr_t>(src)) + paddedBytes - 1;
				return (lastValid ^ lastRead) < pageSize;
			}
		}
	};

	struct string_block_reader {
		JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> getRemainder(uint64_t paddedBytes) noexcept {
			tail_block_builder::impl(+block, inString + index, length - index, paddedBytes);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			inString		= std::bit_cast<read_buffer_ptr>(stringViewNew);
			length			= lengthNew;
			index			= 0;
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
		JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> getRemainder(uint64_t paddedBytes) noexcept {
			tail_block_builder::impl(+block, inString + index, length - index, paddedBytes);
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			inString		= std::bit_cast<read_buffer_ptr>(stringViewNew);
			length			= lengthNew;
			index			= 0;
		}

		alignas(simdBytesPerRegister) uint8_t block[simdBytesPerBlock]{};
		read_buffer_ptr inString{};
		uint64_t length{};
		uint64_t index{};
	};

	template<uint64_t stepBytes = simdBytesPerStep> struct pod_block_reader {
		JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> getRemainder() noexcept {
			const uint64_t remaining = lengthVal - indexVal;
			read_buffer_ptr srcPtr	 = inString + indexVal;
#if JSONIFIER_AVX512
			if constexpr (!JSONIFIER_ASAN_ENABLED) {
				tail_block_builder::buildMasked(+block, srcPtr, remaining, simdBytesPerBlock);
				return +block;
			}
#endif
			if (tail_block_builder::readStaysInPage(srcPtr, remaining, simdBytesPerBlock)) [[likely]] {
				tail_block_builder::buildInPlace(+block, srcPtr, remaining, simdBytesPerBlock);
			} else {
				jsonifierMemcpyUpTo<simdBytesPerBlock - 1>(+block, srcPtr, remaining);
				std::memset(+block + remaining, static_cast<uint8_t>(0x20), simdBytesPerBlock - remaining);
			}
			return +block;
		}

		JSONIFIER_INLINE void reset(read_buffer_ptr stringViewNew, uint64_t lengthNew) noexcept {
			fullStepEnd	 = lengthNew & ~(stepBytes - 1ull);
			fullBlockEnd = lengthNew & ~(simdBytesPerBlock - 1ull);
			inString	 = std::bit_cast<read_buffer_ptr>(stringViewNew);
			lengthVal	 = lengthNew;
			indexVal	 = 0;
		}

		JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> fullBlock() noexcept {
			read_buffer_ptr newPtr = inString + indexVal;
			indexVal += simdBytesPerBlock;
			return newPtr;
		}

		JSONIFIER_INLINE unrestrict_t<read_buffer_ptr> fullStep() noexcept {
			read_buffer_ptr newPtr = inString + indexVal;
			indexVal += stepBytes;
			return newPtr;
		}

		JSONIFIER_INLINE uint64_t getRemainderBytes() const noexcept {
			return lengthVal - indexVal;
		}

		JSONIFIER_INLINE bool hasFullBlock() const noexcept {
			return indexVal < fullBlockEnd;
		}

		JSONIFIER_INLINE bool hasFullStep() const noexcept {
			return indexVal < fullStepEnd;
		}

		JSONIFIER_INLINE uint64_t length() const noexcept {
			return lengthVal;
		}

		JSONIFIER_INLINE uint64_t index() const noexcept {
			return indexVal;
		}

		alignas(simdBytesPerRegister) uint8_t block[simdBytesPerBlock];
		read_buffer_ptr inString{};
		uint64_t fullBlockEnd{};
		uint64_t fullStepEnd{};
		uint64_t lengthVal{};
		uint64_t indexVal{};

		static_assert((stepBytes & (stepBytes - 1)) == 0 && stepBytes >= simdBytesPerBlock);
	};

	using stage1_bracket_masks = uint64_t;

	inline constexpr bool stage1BracketsEnabled{ false };

	JSONIFIER_INLINE stage1_bracket_masks collectBrackets(const simd_array_t&) noexcept {
		return {};
	}

	struct bracket_record {
		uint64_t structurals;
		uint64_t brackets;
		uint64_t tapeBase;
	};

	struct bracket_matcher : alloc_wrapper<bracket_record> {
		using allocator = alloc_wrapper<bracket_record>;
		static constexpr uint64_t stackCapacity{ 256 };

		JSONIFIER_INLINE bracket_matcher() noexcept = default;

		bracket_matcher& operator=(bracket_matcher&&)	   = delete;
		bracket_matcher(bracket_matcher&&)				   = delete;
		bracket_matcher& operator=(const bracket_matcher&) = delete;
		bracket_matcher(const bracket_matcher&)			   = delete;

		JSONIFIER_INLINE void reset(write_structural_index_ptr closesNew, [[maybe_unused]] uint64_t stringLength) noexcept {
			if constexpr (stage1BracketsEnabled) {
				const uint64_t neededCapacity = stringLength / 64 + simdBlocksPerStep + 2;
				if (neededCapacity > recordCapacity) {
					if (records) {
						allocator::deallocate(records, recordCapacity);
					}
					records		   = allocator::allocate(neededCapacity);
					recordCapacity = neededCapacity;
				}
			}
			closes		= closesNew;
			recordCount = 0;
		}

		template<uint64_t blockCount> JSONIFIER_INLINE void recordStep([[maybe_unused]] const array<uint64_t, blockCount>& bitsArr,
			[[maybe_unused]] const array<uint64_t, blockCount>& cntsArr, [[maybe_unused]] const array<stage1_bracket_masks, blockCount>& bracketsArr,
			[[maybe_unused]] uint64_t tapeBase) noexcept {
			if constexpr (stage1BracketsEnabled) {
				bracket_record* __restrict out = records;
				uint64_t count				   = recordCount;
				for (uint64_t x = 0; x < blockCount; ++x) {
					const uint64_t structurals = bitsArr[x];
					const uint64_t brackets	   = bracketsArr[x] & structurals;
					out[count]				   = bracket_record{ structurals, brackets, tapeBase };
					count += static_cast<uint64_t>(brackets != 0);
					tapeBase += cntsArr[x];
				}
				recordCount = count;
			}
		}

		JSONIFIER_INLINE void finish([[maybe_unused]] read_buffer_ptr root, [[maybe_unused]] const uint32_t* tape) noexcept {
			if constexpr (stage1BracketsEnabled) {
				uint64_t depth{};
				for (uint64_t x = 0; x < recordCount; ++x) {
					const bracket_record record = records[x];
					uint64_t brackets			= record.brackets;
					while (brackets) {
						const uint64_t bit	 = brackets & (0 - brackets);
						const uint32_t index = static_cast<uint32_t>(record.tapeBase + simd::tape_writer_op::correctedPopcount(record.structurals & (bit - 1)));
						const uint8_t folded = static_cast<uint8_t>(root[tape[index]] | 0x20);
						if (folded == uint_c_v<'{'>) {
							closes[index] = 0;
							if (depth < stackCapacity) {
								stack[depth] = index;
							}
							++depth;
						} else if (folded == uint_c_v<'}'> && depth) {
							--depth;
							if (depth < stackCapacity) {
								closes[stack[depth]] = index + 1;
							}
						}
						brackets = simd::tape_writer_op::advance(brackets);
					}
				}
			}
		}

		JSONIFIER_INLINE ~bracket_matcher() noexcept {
			if (records) {
				allocator::deallocate(records, recordCapacity);
			}
		}

		write_structural_index_ptr closes{};
		bracket_record* records{};
		uint64_t recordCapacity{};
		uint64_t recordCount{};
		uint32_t stack[stackCapacity];
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
			rope.template nextScalar<registerBytes, registerCount>(inVals, simd::gatherValue<simd_type>('\\'), simd::gatherValue<simd_type>('"'));
			const uint64_t op = simd::scalar_op_collector<registerBytes, registerCount>::impl(inVals, simd::gatherValues<simd_type>(simd::opArray<registerBytes>.data()),
				simd::gatherValue<simd_type>(static_cast<char>(0x20)));
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
			if constexpr (!stage1BracketsEnabled) {
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

	struct simd_string_reader_members : simd::rope_detector<rope_block>, string_block_reader, add_tape_values<make_integer_sequence<simdBlocksPerStep>>, alloc_wrapper<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		using allocator = alloc_wrapper<uint32_t>;

		JSONIFIER_INLINE simd_string_reader_members() noexcept = default;

		JSONIFIER_INLINE simd_string_reader_members& operator=(simd_string_reader_members&& other) noexcept {
			if (&other != this) {
				swap(tapeCount, other.tapeCount);
				swap(capacity, other.capacity);
				swap(string_block_reader::length, other.string_block_reader::length);
				swap(closesTape, other.closesTape);
				swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE simd_string_reader_members(simd_string_reader_members&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		simd_string_reader_members& operator=(const simd_string_reader_members& other) = delete;
		simd_string_reader_members(const simd_string_reader_members& other)			   = delete;

		template<bool minified> JSONIFIER_NOINLINE void reset(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape   = allocator::allocate(neededCapacity);
				auto newCloses = allocator::allocate(neededCapacity);
				if (tape) {
					allocator::deallocate(tape, capacity);
					allocator::deallocate(closesTape, capacity);
				}
				tape	   = newTape;
				closesTape = newCloses;
				capacity   = neededCapacity;
			}
			matcher.reset(closesTape, stringLength);

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
			matcher.finish(rootIter, tape);
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
				allocator::deallocate(closesTape, capacity);
				closesTape = nullptr;
				tape	   = nullptr;
			}
		}

		JSONIFIER_INLINE const uint32_t* closesBegin() const noexcept {
			return stage1BracketsEnabled ? closesTape : nullptr;
		}

	  protected:
		template<uint64_t blockCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processBlocks(read_buffer_ptr blockPtr,
			uint64_t stepBaseIndex, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			array<stage1_bracket_masks, blockCount> bracketsArr;
			array<uint64_t, blockCount> bitsArr;
			array<uint64_t, blockCount> cntsArr;
			processBlocksImpl<0>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (blockCount > 1) {
				processBlocksImpl<1>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (blockCount > 2) {
					processBlocksImpl<2>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (blockCount > 4) {
						processBlocksImpl<4>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}

			add_tape_values<make_integer_sequence<blockCount>>::impl(bitsArr, cntsArr, tape + tapeCount, stepBaseIndex);
			matcher.recordStep(bitsArr, cntsArr, bracketsArr, tapeCount);

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
				read_buffer_ptr tailPtr = string_block_reader::getRemainder(simdBytesPerBlock);
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
				const uint64_t stepBaseIndex = string_block_reader::index;
				processBlocks<blockCount>(string_block_reader::inString + stepBaseIndex, stepBaseIndex, bsRegister, quoteRegister, opTable, spaceMask, args...);
				string_block_reader::index += blockCount * 64;
			}
			if constexpr (blockCount > 1) {
				drainCascade<blockCount / 2>(bsRegister, quoteRegister, opTable, spaceMask, args...);
			}
		}

		template<uint64_t currentBlock, uint64_t laneCount, typename... jsonifier_simd_int_types>
		JSONIFIER_INLINE void processBlocksImpl(array<stage1_bracket_masks, laneCount>& bracketsArr, array<uint64_t, laneCount>& bitsArr, array<uint64_t, laneCount>& cntsArr,
			read_buffer_ptr blockPtr, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
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
			const uint64_t structurals = getStructurals(inVals, opTable, spaceMask, args...) & ~simd::rope_detector<rope_block>::stringTail();
			bracketsArr[currentBlock]  = collectBrackets(inVals);
			bitsArr[currentBlock]	   = structurals;
			cntsArr[currentBlock]	   = simd::tape_writer_op::correctedPopcount(structurals);
		}

		template<typename... jsonifier_simd_int_types> JSONIFIER_INLINE void processTailBlock(read_buffer_ptr blockPtr, uint64_t blockBaseIndex,
			const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_types... args) noexcept {
			array<stage1_bracket_masks, 1> bracketsArr;
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			processBlocksImpl<0>(bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tape + tapeCount, blockBaseIndex);
			matcher.recordStep(bitsArr, cntsArr, bracketsArr, tapeCount);
			tapeCount += cntsArr[0];
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask) noexcept {
			const uint64_t op		   = simd::op_collector::impl(in01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
#if JSONIFIER_NEON
			const uint64x2_t wsOp	  = simd::ws_op_collector::impl(in01, opTable, spaceMask, whitespaceTableLocal);
			const uint64_t whitespace = vgetq_lane_u64(wsOp, 0);
			const uint64_t op		  = vgetq_lane_u64(wsOp, 1);
#else
			const uint64_t whitespace = simd::ws_collector::impl(in01, whitespaceTableLocal);
			const uint64_t op		  = simd::op_collector::impl(in01, opTable, spaceMask);
#endif
			const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
			const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | simd::rope_detector<rope_block>::quotes | scalarStart;
		}

		write_structural_index_ptr closesTape{};
		write_structural_index_ptr tape{};
		bracket_matcher matcher{};
		uint64_t tapeCount{};
		uint64_t capacity{};
	};

	struct simd_string_reader_locals : string_block_reader_locals, add_tape_values<make_integer_sequence<simdBlocksPerStep>>, alloc_wrapper<uint32_t> {
		friend add_tape_values<make_integer_sequence<simdBlocksPerStep>>;
		using allocator = alloc_wrapper<uint32_t>;
		using rope_type = simd::rope_detector<rope_block>;

		JSONIFIER_INLINE simd_string_reader_locals() noexcept = default;

		JSONIFIER_INLINE simd_string_reader_locals& operator=(simd_string_reader_locals&& other) noexcept {
			if (&other != this) {
				swap(tapeCount, other.tapeCount);
				swap(capacity, other.capacity);
				swap(string_block_reader_locals::length, other.string_block_reader_locals::length);
				swap(closesTape, other.closesTape);
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
			const uint8x16_t counts01 = vcntq_u8(vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(bitsArr[offset]), vcreate_u64(bitsArr[offset + 1]))));
			const uint8x16_t counts23 = vcntq_u8(vreinterpretq_u8_u64(vcombine_u64(vcreate_u64(bitsArr[offset + 2]), vcreate_u64(bitsArr[offset + 3]))));
			uint8x16_t sums			  = vpaddq_u8(counts01, counts23);
			sums					  = vpaddq_u8(sums, sums);
			sums					  = vpaddq_u8(sums, sums);
			const uint32_t packed	  = vgetq_lane_u32(vreinterpretq_u32_u8(sums), 0);
			cntsArr[offset]			  = packed & 0xFFu;
			cntsArr[offset + 1]		  = (packed >> 8) & 0xFFu;
			cntsArr[offset + 2]		  = (packed >> 16) & 0xFFu;
			cntsArr[offset + 3]		  = packed >> 24;
		}

		JSONIFIER_INLINE static void collectCounts(const array<uint64_t, simdBlocksPerStep>& __restrict bitsArr, array<uint64_t, simdBlocksPerStep>& __restrict cntsArr) noexcept {
			collectCountsQuad<0>(bitsArr, cntsArr);
			if constexpr (simdBlocksPerStep > 4) {
				collectCountsQuad<4>(bitsArr, cntsArr);
			}
		}

#endif

		template<uint64_t currentBlock, uint64_t laneCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static void processBlocksImpl(rope_type& __restrict rope,
			array<stage1_bracket_masks, laneCount>& __restrict bracketsArr, array<uint64_t, laneCount>& __restrict bitsArr, array<uint64_t, laneCount>& __restrict cntsArr,
			read_buffer_ptr blockPtr, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
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
			const uint64_t structurals = getStructurals(rope, inVals, opTable, spaceMask, args...) & ~rope.stringTail();
			bracketsArr[currentBlock]  = collectBrackets(inVals);
			bitsArr[currentBlock]	   = structurals;
#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON
			if constexpr (laneCount != simdBlocksPerStep || laneCount < 4) {
				cntsArr[currentBlock] = simd::tape_writer_op::correctedPopcount(structurals);
			}
#else
			cntsArr[currentBlock] = simd::tape_writer_op::correctedPopcount(structurals);
#endif
		}

		template<bool minified, typename... jsonifier_simd_int_types> JSONIFIER_INLINE void resetImpl(const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			rope_type rope{};
			write_structural_index_ptr tapeLocal = tape;
			read_buffer_ptr srcPtr				 = string_block_reader_locals::inString;
			const uint64_t lengthLocal			 = string_block_reader_locals::length;
			uint64_t tapeCountLocal				 = 0;
			uint64_t indexLocal					 = 0;

			drainCascade<simdBlocksPerStep>(rope, matcher, srcPtr, tapeLocal, tapeCountLocal, indexLocal, lengthLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
			string_block_reader_locals::index = indexLocal;

			if (const uint64_t remaining = lengthLocal - indexLocal; remaining != 0) {
				read_buffer_ptr tailPtr = string_block_reader_locals::getRemainder(simdBytesPerBlock);
				uint64_t narrowCount{};
				if (narrow_tail_dispatch<minified>::impl(rope, tailPtr, tapeLocal + tapeCountLocal, indexLocal, remaining, narrowCount)) {
					tapeCountLocal += narrowCount;
				} else {
					tapeCountLocal += processTailBlock(rope, matcher, tailPtr, tapeLocal, tapeCountLocal, indexLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
				}
				while (tapeCountLocal > 0 && tapeLocal[tapeCountLocal - 1] >= lengthLocal) {
					--tapeCountLocal;
				}
			}
			tapeCount = tapeCountLocal;
		}

		template<uint64_t blockCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static void drainCascade(rope_type& __restrict rope,
			bracket_matcher& __restrict matcher, read_buffer_ptr srcPtr, write_structural_index_ptr tapePtr, uint64_t& tapeCountLocal, uint64_t& indexLocal, uint64_t lengthLocal,
			const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_types... args) noexcept {
			while (lengthLocal - indexLocal >= blockCount * 64) {
				tapeCountLocal += processBlocks<blockCount>(rope, matcher, srcPtr + indexLocal, tapePtr, tapeCountLocal, indexLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
				indexLocal += blockCount * 64;
			}
			if constexpr (blockCount > 1) {
				drainCascade<blockCount / 2>(rope, matcher, srcPtr, tapePtr, tapeCountLocal, indexLocal, lengthLocal, bsRegister, quoteRegister, opTable, spaceMask, args...);
			}
		}

		template<typename... jsonifier_simd_int_types> JSONIFIER_INLINE static uint64_t processTailBlock(rope_type& __restrict rope, bracket_matcher& __restrict matcher,
			read_buffer_ptr blockPtr, write_structural_index_ptr tapePtr, uint64_t tapeBase, uint64_t blockBaseIndex, const jsonifier_simd_int_t bsRegister,
			const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_types... args) noexcept {
			array<stage1_bracket_masks, 1> bracketsArr;
			array<uint64_t, 1> bitsArr;
			array<uint64_t, 1> cntsArr;
			processBlocksImpl<0>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			add_tape_values<make_integer_sequence<1>>::impl(bitsArr, cntsArr, tapePtr + tapeBase, blockBaseIndex);
			matcher.recordStep(bitsArr, cntsArr, bracketsArr, tapeBase);
			return cntsArr[0];
		}

		template<uint64_t blockCount, typename... jsonifier_simd_int_types> JSONIFIER_INLINE static uint64_t processBlocks(rope_type& __restrict rope,
			bracket_matcher& __restrict matcher, read_buffer_ptr blockPtr, write_structural_index_ptr tapePtr, uint64_t tapeBase, uint64_t stepBaseIndex,
			const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
			const jsonifier_simd_int_types... args) noexcept {
			array<stage1_bracket_masks, blockCount> bracketsArr;
			array<uint64_t, blockCount> bitsArr;
			array<uint64_t, blockCount> cntsArr;
			processBlocksImpl<0>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
			if constexpr (blockCount > 1) {
				processBlocksImpl<1>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
				if constexpr (blockCount > 2) {
					processBlocksImpl<2>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					processBlocksImpl<3>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					if constexpr (blockCount > 4) {
						processBlocksImpl<4>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<5>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<6>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
						processBlocksImpl<7>(rope, bracketsArr, bitsArr, cntsArr, blockPtr, bsRegister, quoteRegister, opTable, spaceMask, args...);
					}
				}
			}
#if JSONIFIER_COMPILER_CLANG && JSONIFIER_NEON
			if constexpr (blockCount == simdBlocksPerStep && blockCount >= 4) {
				collectCounts(bitsArr, cntsArr);
			}
#endif

			add_tape_values<make_integer_sequence<blockCount>>::impl(bitsArr, cntsArr, tapePtr + tapeBase, stepBaseIndex);
			matcher.recordStep(bitsArr, cntsArr, bracketsArr, tapeBase);

			uint64_t stepCount = cntsArr[0];
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

		template<bool minified> JSONIFIER_NOINLINE void reset(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape   = allocator::allocate(neededCapacity);
				auto newCloses = allocator::allocate(neededCapacity);
				if (tape) {
					allocator::deallocate(tape, capacity);
					allocator::deallocate(closesTape, capacity);
				}
				tape	   = newTape;
				closesTape = newCloses;
				capacity   = neededCapacity;
			}
			matcher.reset(closesTape, stringLength);

			string_block_reader_locals::reset(rootIter, stringLength);

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
			matcher.finish(rootIter, tape);
		}

		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const simd_array_t in01, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask, const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
#if JSONIFIER_NEON
			const uint64x2_t wsOp	  = simd::ws_op_collector::impl(in01, opTable, spaceMask, whitespaceTableLocal);
			const uint64_t whitespace = vgetq_lane_u64(wsOp, 0);
			const uint64_t op		  = vgetq_lane_u64(wsOp, 1);
#else
			const uint64_t whitespace = simd::ws_collector::impl(in01, whitespaceTableLocal);
			const uint64_t op		  = simd::op_collector::impl(in01, opTable, spaceMask);
#endif
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | whitespace | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
		}

		JSONIFIER_INLINE static uint64_t getStructurals(rope_type& __restrict rope, const simd_array_t in01, const jsonifier_simd_int_t opTable,
			const jsonifier_simd_int_t spaceMask) noexcept {
			const uint64_t op		   = simd::op_collector::impl(in01, opTable, spaceMask);
			const uint64_t quotes	   = rope.quotes;
			const uint64_t scalar	   = ~(op | quotes);
			const uint64_t follows	   = rope.followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | quotes | scalarStart;
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
				allocator::deallocate(closesTape, capacity);
				closesTape = nullptr;
				tape	   = nullptr;
			}
		}

		JSONIFIER_INLINE const uint32_t* closesBegin() const noexcept {
			return stage1BracketsEnabled ? closesTape : nullptr;
		}

		write_structural_index_ptr closesTape{};
		write_structural_index_ptr tape{};
		bracket_matcher matcher{};
		uint64_t tapeCount{};
		uint64_t capacity{};
	};

#if JSONIFIER_COMPILER_CLANG
	using simd_string_reader = simd_string_reader_members;
#else
	using simd_string_reader = simd_string_reader_locals;
#endif

	struct pod_simd_string_reader_members : simd::rope_detector<rope_block>, alloc_wrapper<uint32_t> {
		using allocator = alloc_wrapper<uint32_t>;
		using rope_type = simd::rope_detector<rope_block>;
		static constexpr uint64_t stepBytes{ podBytesPerStep };

		JSONIFIER_INLINE pod_simd_string_reader_members() noexcept = default;

		JSONIFIER_INLINE pod_simd_string_reader_members& operator=(pod_simd_string_reader_members&& other) noexcept {
			if (&other != this) {
				swap(tapeCount, other.tapeCount);
				swap(capacity, other.capacity);
				swap(length, other.length);
				swap(tape, other.tape);
			}
			return *this;
		}

		JSONIFIER_INLINE pod_simd_string_reader_members(pod_simd_string_reader_members&& other) noexcept : allocator{} {
			*this = internal::move(other);
		}

		pod_simd_string_reader_members& operator=(const pod_simd_string_reader_members& other) = delete;
		pod_simd_string_reader_members(const pod_simd_string_reader_members& other)			   = delete;

		template<uint64_t currentBlock, uint64_t registerBytes, uint64_t registerCount, uint64_t blocksPerStep, typename... simd_types>
		JSONIFIER_INLINE void processBlocksImpl(array<uint64_t, blocksPerStep>& __restrict bitsArr, array<uint64_t, blocksPerStep>& __restrict cntsArr, read_buffer_ptr blockPtr,
			const simd_register_t<registerBytes> bsRegister, const simd_register_t<registerBytes> quoteRegister, const simd_register_t<registerBytes> opTable,
			const simd_register_t<registerBytes> spaceMask, const simd_types... args) noexcept {
			using simd_type = simd_register_t<registerBytes>;
			pod_simd_array_t<registerCount, registerBytes> inVals;
			inVals.template set<0>(simd::gatherValuesU<simd_type>(blockPtr + currentBlock * simdBytesPerBlock));
			if constexpr (registerCount > 1) {
				inVals.template set<1>(simd::gatherValuesU<simd_type>(blockPtr + currentBlock * simdBytesPerBlock + registerBytes * 1));
				if constexpr (registerCount > 2) {
					inVals.template set<2>(simd::gatherValuesU<simd_type>(blockPtr + currentBlock * simdBytesPerBlock + registerBytes * 2));
					inVals.template set<3>(simd::gatherValuesU<simd_type>(blockPtr + currentBlock * simdBytesPerBlock + registerBytes * 3));
				}
			}
			rope_type::template nextScalar<registerBytes, registerCount>(inVals, bsRegister, quoteRegister);
			const uint64_t structurals = getStructurals<registerBytes, registerCount>(inVals, opTable, spaceMask, args...) & ~rope_type::stringTail();
			bitsArr[currentBlock]	   = structurals;
			cntsArr[currentBlock]	   = simd::tape_writer_op::correctedPopcount(structurals);
		}

		template<bool minified, uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE void resetDispatch(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
			using simd_type				  = simd_register_t<registerBytes>;
			const uint64_t neededCapacity = stringLength + 64;
			if (neededCapacity > capacity) {
				auto newTape = allocator::allocate(neededCapacity);
				if (tape) {
					allocator::deallocate(tape, capacity);
				}
				tape	 = newTape;
				capacity = neededCapacity;
			}

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

		template<uint64_t blocksPerStep, uint64_t registerBytes, uint64_t registerCount, typename... simd_types> JSONIFIER_INLINE void processBlocks(read_buffer_ptr blockPtr,
			uint64_t stepBaseIndex, const simd_register_t<registerBytes> bsRegister, const simd_register_t<registerBytes> quoteRegister,
			const simd_register_t<registerBytes> opTable, const simd_register_t<registerBytes> spaceMask, const simd_types... args) noexcept {
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

		template<uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE uint64_t getStructurals(const pod_simd_array_t<registerCount, registerBytes> in01,
			const simd_register_t<registerBytes> opTable, const simd_register_t<registerBytes> spaceMask, const simd_register_t<registerBytes> whitespaceTableLocal) noexcept {
			const uint64_t whitespace  = simd::pod_ws_collector<registerBytes, registerCount>::impl(in01, whitespaceTableLocal);
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | whitespace | rope_type::quotes);
			const uint64_t follows	   = rope_type::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | rope_type::quotes | scalarStart;
		}

		template<uint64_t registerBytes, uint64_t registerCount, typename... simd_types> JSONIFIER_INLINE void resetImpl(read_buffer_ptr rootIter, uint64_t stringLength,
			const simd_register_t<registerBytes> bsRegister, const simd_register_t<registerBytes> quoteRegister, const simd_register_t<registerBytes> opTable,
			const simd_register_t<registerBytes> spaceMask, const simd_types... args) noexcept {
			static constexpr uint64_t blocksPerStep{ stepBytes / simdBytesPerBlock };
			stringBlockReader.reset(rootIter, stringLength);
			rope_type::prevInString	 = 0;
			rope_type::prevScalar	 = 0;
			rope_type::nextIsEscaped = 0;
			tapeCount				 = 0;
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

		template<uint64_t registerBytes, uint64_t registerCount> JSONIFIER_INLINE uint64_t getStructurals(const pod_simd_array_t<registerCount, registerBytes> in01,
			const simd_register_t<registerBytes> opTable, const simd_register_t<registerBytes> spaceMask) noexcept {
			const uint64_t op		   = simd::scalar_op_collector<registerBytes, registerCount>::impl(in01, opTable, spaceMask);
			const uint64_t scalar	   = ~(op | rope_type::quotes);
			const uint64_t follows	   = rope_type::followsNonquoteScalar(scalar);
			const uint64_t scalarStart = scalar & ~follows;
			return op | rope_type::quotes | scalarStart;
		}

		template<bool minified> JSONIFIER_NOINLINE void reset(read_buffer_ptr rootIter, uint64_t stringLength) noexcept {
#if JSONIFIER_ANY_AVX
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
			if constexpr (simdBytesPerRegister >= 32) {
				if (stringLength <= 32) {
					return resetDispatch<minified, 32, 1>(rootIter, stringLength);
				}
			}
#elif JSONIFIER_NEON
			if (stringLength <= 16) {
				return resetDispatch<minified, 16, 1>(rootIter, stringLength);
			}
			if (stringLength <= 32) {
				return resetDispatch<minified, 16, 2>(rootIter, stringLength);
			}
#endif
			resetDispatch<minified, simdBytesPerRegister, simdRegistersPerBlock>(rootIter, stringLength);
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> begin() noexcept {
			tape[tapeCount] = static_cast<uint32_t>(length);
			return tape;
		}

		JSONIFIER_INLINE unrestrict_t<write_structural_index_ptr> end() noexcept {
			return tape + tapeCount;
		}

		JSONIFIER_INLINE uint64_t getTapeCount() noexcept {
			return tapeCount;
		}

		JSONIFIER_INLINE ~pod_simd_string_reader_members() noexcept {
			if (tape) {
				allocator::deallocate(tape, capacity);
				tape = nullptr;
			}
		}

		pod_block_reader<stepBytes> stringBlockReader;
		write_structural_index_ptr tape{};
		uint64_t tapeCount{};
		uint64_t capacity{};
		uint64_t length{};
	};

	using pod_simd_string_reader = pod_simd_string_reader_members;

}
