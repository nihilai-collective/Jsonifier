/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/avx.hpp
 */
#pragma once

#include <jsonifier-incl/simd/simd_config.hpp>
#include <jsonifier-incl/simd/bit_ops.hpp>
#include <jsonifier-incl/simd/compress_tables.hpp>

namespace jsonifier::internal::simd {

#if JSONIFIER_AVX512 || JSONIFIER_AVX2 || JSONIFIER_AVX

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_cmpeq_epi8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_128 offset{ _mm_set1_epi8(static_cast<char>(0x80)) };
		return _mm_cmpgt_epi8(_mm_add_epi8(other, offset), _mm_add_epi8(value, offset));
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_shuffle_epi8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(value, other)));
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_subs_epu8(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_xor_si128(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_and_si128(value, other);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_or_si128(value, other);
	}

	template<simd_int_128_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation), value);
	}

	template<simd_int_128_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeUHalves(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		storeU(value, storageLocation);
	}

	template<simd_int_128_types simd_int_type_new>
	[[maybe_unused]] JSONIFIER_INLINE uint64_t opCompressStore(simd_int_type_new value, const uint64_t keep, void* __restrict storageLocation) noexcept {
		const __m128i shuffle{ _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(_mm_loadl_epi64(std::bit_cast<const __m128i*>(compressTablesLow + (keep & 0xFF)))),
			std::bit_cast<const double*>(compressTablesHigh + ((keep >> 8) & 0xFF)))) };
		const __m128i merge{ _mm_load_si128(std::bit_cast<const __m128i*>(compressTablesMerge + ((keep & 0xFF) << 4))) };
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation), _mm_shuffle_epi8(_mm_shuffle_epi8(value, shuffle), merge));
		return popcount(keep & 0xFFFFULL);
	}

	template<simd_int_128_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm_store_si128(static_cast<__m128i*>(storageLocation), value);
	}

	template<simd_int_128_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm_loadu_si128(static_cast<const __m128i*>(str));
	}

	template<simd_int_128_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm_load_si128(static_cast<const __m128i*>(str));
	}

	template<simd_int_128_types simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

	template<int32_t alignment, simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opSrLi(simd_int_t01 value) noexcept {
		return _mm_srli_epi16(value, alignment);
	}

	template<uint_types value_type> [[maybe_unused]] JSONIFIER_INLINE uint64_t postCmpTzcntUnsafe(const value_type value) noexcept {
		return countrZeroUnsafe(value);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm_movemask_epi8(value);
	}

	template<uint_types value_type> [[maybe_unused]] JSONIFIER_INLINE uint64_t postCmpTzcnt(const value_type value) noexcept {
		return countrZero(value);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opBitMask(simd_int_t01 value) noexcept {
		return _mm_movemask_epi8(value);
	}

	template<simd_int_128_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opTest(simd_int_t01 value) noexcept {
		return _mm_testz_si128(value, value);
	}

	template<simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpLtSignedRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_cmpgt_epi8(other, value);
	}

	template<int32_t alignment, simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_alignr_epi8(value, other, alignment);
	}

	template<int32_t shift, simd_int_128_types simd_int_t01, simd_int_128_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return opAlignR<shift>(current, previous);
	}

	template<simd_int_128_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t str) noexcept {
		return _mm_set1_epi64x(static_cast<const int64_t>(str));
	}

	template<simd_int_128_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t str) noexcept {
		return _mm_set1_epi8(static_cast<const char>(str));
	}

	#if JSONIFIER_AVX512 || JSONIFIER_AVX2

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_cmpeq_epi8(value, other);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_256 offset{ _mm256_set1_epi8(static_cast<char>(0x80)) };
		return _mm256_cmpgt_epi8(_mm256_add_epi8(other, offset), _mm256_add_epi8(value, offset));
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_shuffle_epi8(value, other);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(value, other)));
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_subs_epu8(value, other);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_xor_si256(value, other);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_and_si256(value, other);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_or_si256(value, other);
	}

	template<simd_int_256_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm256_storeu_si256(static_cast<__m256i*>(storageLocation), value);
	}

	template<simd_int_256_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeUHalves(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		__m128i quarter0{ _mm256_castsi256_si128(value) };
		__m128i quarter1{ _mm256_extracti128_si256(value, 1) };
		#if JSONIFIER_COMPILER_CLANG || JSONIFIER_COMPILER_GCC
		__asm__("" : "+x"(quarter0), "+x"(quarter1));
		#endif
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation), quarter0);
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation) + 1, quarter1);
	}

	template<simd_int_256_types simd_int_type_new>
	[[maybe_unused]] JSONIFIER_INLINE uint64_t opCompressStore(simd_int_type_new value, const uint64_t keep, void* __restrict storageLocation) noexcept {
		const __m128i lowShuffle{ _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(_mm_loadl_epi64(std::bit_cast<const __m128i*>(compressTablesLow + (keep & 0xFF)))),
			std::bit_cast<const double*>(compressTablesHigh + ((keep >> 8) & 0xFF)))) };
		const __m128i highShuffle{ _mm_castpd_si128(_mm_loadh_pd(_mm_castsi128_pd(_mm_loadl_epi64(std::bit_cast<const __m128i*>(compressTablesLow + ((keep >> 16) & 0xFF)))),
			std::bit_cast<const double*>(compressTablesHigh + ((keep >> 24) & 0xFF)))) };
		const __m128i lowMerge{ _mm_load_si128(std::bit_cast<const __m128i*>(compressTablesMerge + ((keep & 0xFF) << 4))) };
		const __m128i highMerge{ _mm_load_si128(std::bit_cast<const __m128i*>(compressTablesMerge + (((keep >> 16) & 0xFF) << 4))) };
		const __m256i packed{ _mm256_shuffle_epi8(_mm256_shuffle_epi8(value, _mm256_set_m128i(highShuffle, lowShuffle)), _mm256_set_m128i(highMerge, lowMerge)) };
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation), _mm256_castsi256_si128(packed));
		_mm_storeu_si128(std::bit_cast<__m128i*>(static_cast<uint8_t*>(storageLocation) + popcount(keep & 0xFFFFULL)), _mm256_extracti128_si256(packed, 1));
		return popcount(keep & 0xFFFFFFFFULL);
	}

	template<simd_int_256_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm256_store_si256(static_cast<__m256i*>(storageLocation), value);
	}

	template<simd_int_256_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm256_loadu_si256(static_cast<const __m256i*>(str));
	}

	template<simd_int_256_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm256_load_si256(static_cast<const __m256i*>(str));
	}

	template<simd_int_256_types simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), chunks.template get<1>());
	}

	template<int32_t alignment, simd_int_256_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opSrLi(simd_int_t01 value) noexcept {
		return _mm256_srli_epi16(value, alignment);
	}

	template<int64_t shift, simd_int_256_types simd_type> JSONIFIER_INLINE simd_type opPrev(simd_type cur, simd_type prev) noexcept {
		return _mm256_alignr_epi8(cur, _mm256_permute2x128_si256(prev, cur, 0x21), shift);
	}

	template<simd_int_256_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm256_movemask_epi8(value);
	}

	template<simd_int_256_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opBitMask(simd_int_t01 value) noexcept {
		return _mm256_movemask_epi8(value);
	}

	template<simd_int_256_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opTest(simd_int_t01 value) noexcept {
		return _mm256_testz_si256(value, value);
	}

	template<simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpLtSignedRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_cmpgt_epi8(other, value);
	}

	template<int32_t alignment, simd_int_256_types simd_int_t01, simd_int_256_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_alignr_epi8(value, other, alignment);
	}

	template<simd_int_256_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm256_set1_epi64x(static_cast<const int64_t>(value));
	}

	template<simd_int_256_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm256_set1_epi8(static_cast<const char>(value));
	}

		#if JSONIFIER_AVX512

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_movm_epi8(_mm512_cmpeq_epi8_mask(value, other));
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_movm_epi8(_mm512_cmplt_epu8_mask(value, other));
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_shuffle_epi8(value, other);
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpNeq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint64_t>(_mm512_cmpneq_epi8_mask(value, other));
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint64_t>(_mm512_cmpeq_epi8_mask(value, other));
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_subs_epu8(value, other);
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_xor_si512(value, other);
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_and_si512(value, other);
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_or_si512(value, other);
	}

	template<simd_int_512_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm512_storeu_si512(static_cast<__m512i*>(storageLocation), value);
	}

	template<simd_int_512_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void storeUHalves(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		storeU(value, storageLocation);
	}

	template<simd_int_512_types simd_int_type_new>
	[[maybe_unused]] JSONIFIER_INLINE uint64_t opCompressStore(simd_int_type_new value, const uint64_t keep, void* __restrict storageLocation) noexcept {
		_mm512_storeu_si512(storageLocation, _mm512_maskz_compress_epi8(keep, value));
		return popcount(keep);
	}

	template<simd_int_512_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm512_store_si512(static_cast<__m512i*>(storageLocation), value);
	}

	template<simd_int_512_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm512_loadu_si512(static_cast<const __m512i*>(str));
	}

	template<simd_int_512_types simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm512_load_si512(static_cast<const __m512i*>(str));
	}

	template<simd_int_512_types simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return chunks.template get<0>();
	}

	template<int32_t alignment, simd_int_512_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opSrLi(simd_int_t01 value) noexcept {
		return _mm512_srli_epi16(value, alignment);
	}

	template<int32_t shift, simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return _mm512_alignr_epi8(current, _mm512_permutex2var_epi64(previous, _mm512_set_epi64(13, 12, 11, 10, 9, 8, 7, 6), current), shift);
	}

	template<simd_int_512_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE uint64_t opBitMask(simd_int_t01 value) noexcept {
		return static_cast<uint64_t>(_mm512_movepi8_mask(value));
	}

	template<simd_int_512_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm512_movepi8_mask(value);
	}

	template<simd_int_512_types simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE auto opTest(simd_int_t01 value) noexcept {
		return _mm512_test_epi64_mask(value, value) == 0;
	}

	template<simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opCmpLtSignedRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_movm_epi8(_mm512_cmplt_epi8_mask(value, other));
	}

	template<int32_t alignment, simd_int_512_types simd_int_t01, simd_int_512_types simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_alignr_epi8(value, other, alignment);
	}

	template<simd_int_512_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm512_set1_epi64(static_cast<const int64_t>(value));
	}

	template<simd_int_512_types simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm512_set1_epi8(static_cast<const char>(value));
	}

		#endif

	#endif

	template<typename simd_type> JSONIFIER_INLINE bool isAscii(simd_type value) noexcept {
		return opBitMask(value) == 0;
	}

#endif

}
