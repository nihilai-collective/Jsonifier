/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/neon.hpp
 */
#pragma once

#include <jsonifier-incl/simd/simd_config.hpp>
#include <jsonifier-incl/simd/bit_ops.hpp>
#include <jsonifier-incl/simd/compress_tables.hpp>

namespace jsonifier::internal::simd {

#if JSONIFIER_NEON

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpEqRaw(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vceqq_u8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpLtRaw(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vcgtq_u8(other, value);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opShuffle(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		const auto bitMask{ vdupq_n_u8(0x0F) };
		return vqtbl1q_u8(value, vandq_u8(other, bitMask));
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpEq(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(vceqq_u8(value, other)), 4)), 0);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE void storeU(const simd_int_t01 value, void* __restrict storageLocation) noexcept {
		vst1q_u8(static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE void storeUHalves(const simd_int_t01 value, void* __restrict storageLocation) noexcept {
		storeU(value, storageLocation);
	}

	template<simd_int_128_types simd_int_t01>
	[[maybe_unused]] JSONIFIER_INLINE uint64_t opCompressStore(const simd_int_t01 value, const uint64_t keep, void* __restrict storageLocation) noexcept {
		const uint8x16_t shuffle{ vcombine_u8(vcreate_u8(compressTablesLow[keep & 0xFF]), vcreate_u8(compressTablesHigh[(keep >> 8) & 0xFF])) };
		const uint8x16_t packed{ vqtbl1q_u8(vqtbl1q_u8(value, shuffle), vld1q_u8(compressTablesMerge + ((keep & 0xFF) << 4))) };
		vst1q_u8(static_cast<uint8_t*>(storageLocation), packed);
		return popcount(keep & 0xFFFFULL);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE void store(const simd_int_t01 value, void* __restrict storageLocation) noexcept {
		vst1q_u8(static_cast<uint8_t*>(storageLocation), value);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE simd_int_t01 gatherValuesU(const void* __restrict str) noexcept {
		return vld1q_u8(static_cast<read_buffer_ptr>(str));
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE simd_int_t01 gatherValues(const void* __restrict str) noexcept {
		return vld1q_u8(static_cast<read_buffer_ptr>(str));
	}

	template<int32_t alignment, simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opSrLi(const simd_int_t01 value) noexcept {
		return vshrq_n_u8(value, alignment);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE uint16_t opBitMask(const simd_int_t01 value) noexcept {
		static constexpr uint8x16_t bitMask{ 0x01, 0x02, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80 };
		const auto minput{ vandq_u8(value, bitMask) };
		uint8x16_t tmp{ vpaddq_u8(minput, minput) };
		tmp = vpaddq_u8(tmp, tmp);
		tmp = vpaddq_u8(tmp, tmp);
		return vgetq_lane_u16(vreinterpretq_u16_u8(tmp), 0);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE uint64_t opBitMaskRaw(const simd_int_t01 value) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(value), 4)), 0);
	}

	template<uint_types value_type> [[maybe_unused]] JSONIFIER_INLINE uint64_t postCmpTzcntUnsafe(value_type value) noexcept {
		return countrZeroUnsafe(value) >> 2;
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE bool opTest(const simd_int_t01 value) noexcept {
		return vget_lane_u64(vreinterpret_u64_u8(vqmovn_u16(vreinterpretq_u16_u8(value))), 0) == 0;
	}

	template<uint_types value_type> [[maybe_unused]] JSONIFIER_INLINE uint64_t postCmpTzcnt(value_type value) noexcept {
		return countrZero(value) >> 2;
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE jsonifier_simd_int_128 opSubs(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vqsubq_u8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE jsonifier_simd_int_128 opXor(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return veorq_u8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE jsonifier_simd_int_128 opAnd(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vandq_u8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE jsonifier_simd_int_128 opOr(const simd_int_t01 value, const simd_int_t02 other) noexcept {
		return vorrq_u8(value, other);
	}

	template<simd_int_128_types simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

	template<int32_t shift, simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opPrev(const simd_int_t01 current, const simd_int_t02 previous) noexcept {
		return vextq_u8(previous, current, shift);
	}

	template<simd_int_128_types simd_int_01> JSONIFIER_INLINE bool isAscii(const simd_int_01 input) noexcept {
		return opTest(vandq_u8(input, vdupq_n_u8(0x80)));
	}

	template<simd_int_128_types simd_int_t01, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_t01 gatherValue(char_t str) noexcept {
		return vdupq_n_u64(str);
	}

	template<simd_int_128_types simd_int_t01, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_t01 gatherValue(char_t str) noexcept {
		return vdupq_n_u8(static_cast<uint8_t>(str));
	}

	template<simd_int_128_types simd_int_t01, uint8_t value> [[maybe_unused]] JSONIFIER_INLINE simd_int_t01 gatherValue() noexcept {
		return vdupq_n_u8(value);
	}

#endif

}
