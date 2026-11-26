/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/serializing/minifier.hpp
 */
#pragma once

#include <jsonifier-incl/utilities/compare.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	consteval array<json_structural_types, 256ULL> genJsonTypes() noexcept {
		array<json_structural_types, 256ULL> returnValues{};
		using enum json_structural_types;
		returnValues[uint_c_v<'"'>] = string;
		returnValues[uint_c_v<','>] = comma;
		returnValues[uint_c_v<'0'>] = number;
		returnValues[uint_c_v<'1'>] = number;
		returnValues[uint_c_v<'2'>] = number;
		returnValues[uint_c_v<'3'>] = number;
		returnValues[uint_c_v<'4'>] = number;
		returnValues[uint_c_v<'5'>] = number;
		returnValues[uint_c_v<'6'>] = number;
		returnValues[uint_c_v<'7'>] = number;
		returnValues[uint_c_v<'8'>] = number;
		returnValues[uint_c_v<'9'>] = number;
		returnValues[uint_c_v<'-'>] = number;
		returnValues[uint_c_v<':'>] = colon;
		returnValues[uint_c_v<'['>] = array_start;
		returnValues[uint_c_v<']'>] = array_end;
		returnValues[uint_c_v<'n'>] = null;
		returnValues[uint_c_v<'t'>] = boolean;
		returnValues[uint_c_v<'f'>] = boolean;
		returnValues[uint_c_v<'{'>] = object_start;
		returnValues[uint_c_v<'}'>] = object_end;
		return returnValues;
	}

	alignas(64) inline constexpr auto jsonTypesRaw{ genJsonTypes() };

	inline constexpr const json_structural_types* __restrict jsonTypes{ jsonTypesRaw.data() };

	template<uint64_t index> struct register_compressor {
		JSONIFIER_INLINE static uint64_t impl(const simd_array_t block, const uint64_t keep, write_buffer_ptr out) noexcept {
			const uint64_t written{ simd::opCompressStore(block.template get<index>(), keep >> (simdBytesPerRegister * index), out) };
			if constexpr (index + 1 < simdRegistersPerBlock) {
				return written + register_compressor<index + 1>::impl(block, keep, out + written);
			} else {
				return written;
			}
		}
	};

	struct minify_registers {
		const jsonifier_simd_int_t whitespaceTable{ simd::gatherValues<jsonifier_simd_int_t>(simd::whitespaceArray<simdBytesPerRegister>.data()) };
		const jsonifier_simd_int_t quoteRegister{ simd::gatherValue<jsonifier_simd_int_t, '"'>() };
		const jsonifier_simd_int_t bsRegister{ simd::gatherValue<jsonifier_simd_int_t, '\\'>() };
#if JSONIFIER_CLMUL || JSONIFIER_SVE2
		const simd::prefix_xor_op::ones_type prefixXorOnes{ simd::prefix_xor_op::ones() };
#endif
	};

	struct block_minifier : public simd::rope_detector<rope_block> {
		using rope_type = simd::rope_detector<rope_block>;
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

		JSONIFIER_INLINE static void storeBlock(const simd_array_t block, write_buffer_ptr out) noexcept {
			simd::storeUHalves(block.template get<0>(), out);
			if constexpr (simdRegistersPerBlock > 1) {
				simd::storeUHalves(block.template get<1>(), out + simdBytesPerRegister);
				if constexpr (simdRegistersPerBlock > 2) {
					simd::storeUHalves(block.template get<2>(), out + simdBytesPerRegister * 2);
					simd::storeUHalves(block.template get<3>(), out + simdBytesPerRegister * 3);
				}
			}
		}

		JSONIFIER_INLINE static uint64_t store(const simd_array_t block, const uint64_t whitespace, write_buffer_ptr out) noexcept {
			if (!whitespace) {
				storeBlock(block, out);
				return simdBytesPerBlock;
			}
			return register_compressor<0>::impl(block, ~whitespace, out);
		}

		JSONIFIER_INLINE uint64_t whitespaceOf(const simd_array_t block) noexcept {
#if JSONIFIER_CLMUL || JSONIFIER_SVE2
			rope_type::next(block, registers.bsRegister, registers.quoteRegister, registers.prefixXorOnes);
#else
			rope_type::next(block, registers.bsRegister, registers.quoteRegister);
#endif
			uint64_t whitespace{ simd::ws_collector::impl(block, registers.whitespaceTable) };
#if JSONIFIER_COMPILER_GCC
			__asm__("" : "+r"(whitespace));
#endif
			return whitespace & ~rope_type::inString;
		}

		JSONIFIER_INLINE uint64_t impl(read_buffer_ptr blockPtr, write_buffer_ptr out) noexcept {
			const simd_array_t block{ loadBlock(blockPtr) };
			return store(block, whitespaceOf(block), out);
		}

		const minify_registers registers{};
	};

	template<typename derived_type_new> struct minifier {
		using derived_type = derived_type_new;

		template<string_types input_string_type, string_types output_buffer_type> inline bool minifyJson(input_string_type&& in, output_buffer_type&& buffer) {
			derivedRef.lastError.clear();
			read_buffer_ptr rootIter{ std::bit_cast<read_buffer_ptr>(in.data()) };
			const uint64_t length{ in.size() };
			read_buffer_ptr endIter{ rootIter + length };
			uint64_t firstIndex{};
			while (firstIndex < length && whitespaceTable[static_cast<uint64_t>(rootIter[firstIndex])]) {
				++firstIndex;
			}
			if (firstIndex == length) [[unlikely]] {
				derivedRef.lastError = error::constructError<status_classes::minifying, minify_statuses::no_input>(rootIter, rootIter, endIter);
				return false;
			}
			const uint64_t requiredSize{ length + simdBytesPerBlock };
			const minify_writer writer{ rootIter, endIter, *this };
			if constexpr (resize_and_overwriteable_types<remove_cvref_t<output_buffer_type>>) {
				buffer.resize_and_overwrite(requiredSize, writer);
			} else {
				buffer.resize(requiredSize);
				buffer.resize(writer(buffer.data(), requiredSize));
			}
			return !derivedRef.lastError.hasError();
		}

	  protected:
		struct minify_writer {
			JSONIFIER_INLINE uint64_t operator()(char* __restrict outNew, uint64_t) const noexcept {
				return owner.impl(std::bit_cast<write_buffer_ptr>(outNew), rootIter, endIter);
			}

			read_buffer_ptr rootIter;
			read_buffer_ptr endIter;
			minifier& owner;
		};

		inline minifier() noexcept				   = default;
		minifier& operator=(minifier&& other)	   = delete;
		minifier(minifier&& other)				   = delete;
		minifier& operator=(const minifier& other) = delete;
		minifier(const minifier& other)			   = delete;

		JSONIFIER_INLINE uint64_t impl(write_buffer_ptr out, read_buffer_ptr rootIter, read_buffer_ptr endIter) noexcept {
			write_buffer_ptr dst{ out };
			read_buffer_ptr iter{ rootIter };
			block_minifier blockMinifier{};
			const read_buffer_ptr lastPair{ endIter - 2 * simdBytesPerBlock };
			while (iter <= lastPair) {
				dst += blockMinifier.impl(iter, dst);
				dst += blockMinifier.impl(iter + simdBytesPerBlock, dst);
				iter += 2 * simdBytesPerBlock;
			}
			if (static_cast<uint64_t>(endIter - iter) >= simdBytesPerBlock) {
				dst += blockMinifier.impl(iter, dst);
				iter += simdBytesPerBlock;
			}
			if (const uint64_t remaining = static_cast<uint64_t>(endIter - iter); remaining != 0) {
				alignas(simdBytesPerRegister) uint8_t tail[simdBytesPerBlock];
				tail_block_builder::template impl<simdBytesPerBlock>(tail, iter, remaining);
				dst += blockMinifier.impl(tail, dst);
			}
			if (blockMinifier.prevInString) [[unlikely]] {
				derivedRef.lastError = error::constructError<status_classes::minifying, minify_statuses::invalid_string_length>(rootIter, endIter, endIter);
				return 0;
			}
			return static_cast<uint64_t>(dst - out);
		}

		inline ~minifier() noexcept = default;

		derived_type& derivedRef{ *static_cast<derived_type*>(this) };
	};

}
