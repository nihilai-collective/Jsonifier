// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/avx.hpp
#pragma once

#include <jsonifier-incl/simd/simd_types.hpp>
#include <jsonifier-incl/simd/bmi.hpp>

namespace jsonifier::simd {

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX)

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcnt(const value_type value) noexcept {
		return tzcnt(value);
	}

	template<typename value_type> [[maybe_unused]] JSONIFIER_INLINE static value_type postCmpTzcntUnsafe(const value_type value) noexcept {
		return tzcntUnsafe(value);
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm_load_si128(static_cast<const __m128i*>(str));
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm_loadu_si128(static_cast<const __m128i*>(str));
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t str) noexcept {
		return _mm_set1_epi64x(static_cast<const int64_t>(str));
	}

	template<simd_int_128_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t str) noexcept {
		return _mm_set1_epi8(static_cast<const char>(str));
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm_store_si128(static_cast<__m128i*>(storageLocation), value);
	}

	template<simd_int_128_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm_storeu_si128(static_cast<__m128i*>(storageLocation), value);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(value, other)));
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_128 offset = _mm_set1_epi8(static_cast<char>(0x80));
		return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(_mm_add_epi8(other, offset), _mm_add_epi8(value, offset))));
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_cmpeq_epi8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_128 offset = _mm_set1_epi8(static_cast<char>(0x80));
		return _mm_cmpgt_epi8(_mm_add_epi8(other, offset), _mm_add_epi8(value, offset));
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opBitMask(simd_int_t01 value) noexcept {
		return _mm_movemask_epi8(value);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm_movemask_epi8(value);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_shuffle_epi8(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_xor_si128(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_subs_epu8(value, other);
	}

	template<int32_t permuteMask, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPermute(simd_int_t01 value, simd_int_t02) noexcept {
		return value;
	}

	template<int32_t alignment, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_alignr_epi8(value, other, alignment);
	}

	template<int32_t alignment, simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opSrLi(simd_int_t01 value) noexcept {
		return _mm_srli_epi16(value, alignment);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_and_si128(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_or_si128(value, other);
	}

	template<simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm_andnot_si128(other, value);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opTest(simd_int_t01 value) noexcept {
		return _mm_testz_si128(value, value);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opNot(simd_int_t01 value) noexcept {
		return _mm_xor_si128(value, _mm_set1_epi64x(static_cast<int64_t>(0xFFFFFFFFFFFFFFFFll)));
	}

	template<int32_t N, simd_int_128_type simd_int_t01, simd_int_128_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return simd::opAlignR<N>(current, previous);
	}

	template<simd_int_128_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool anyBitsSetAnywhere(simd_int_t01 value) noexcept {
		return !simd::opTest(value);
	}

	template<simd_int_128_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), opOr(chunks.template get<1>(), opOr(chunks.template get<2>(), chunks.template get<3>())));
	}

	#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512) || JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)

	template<simd_int_256_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm256_load_si256(static_cast<const __m256i*>(str));
	}

	template<simd_int_256_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm256_loadu_si256(static_cast<const __m256i*>(str));
	}

	template<simd_int_256_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm256_set1_epi64x(static_cast<const int64_t>(value));
	}

	template<simd_int_256_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm256_set1_epi8(static_cast<const char>(value));
	}

	template<simd_int_256_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm256_store_si256(static_cast<__m256i*>(storageLocation), value);
	}

	template<simd_int_256_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm256_storeu_si256(static_cast<__m256i*>(storageLocation), value);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(value, other)));
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_256 offset = _mm256_set1_epi8(static_cast<char>(0x80));
		return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpgt_epi8(_mm256_add_epi8(other, offset), _mm256_add_epi8(value, offset))));
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_cmpeq_epi8(value, other);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		const jsonifier_simd_int_256 offset = _mm256_set1_epi8(static_cast<char>(0x80));
		return _mm256_cmpgt_epi8(_mm256_add_epi8(other, offset), _mm256_add_epi8(value, offset));
	}

	template<simd_int_256_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opBitMask(simd_int_t01 value) noexcept {
		return _mm256_movemask_epi8(value);
	}

	template<simd_int_256_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm256_movemask_epi8(value);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_shuffle_epi8(value, other);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_xor_si256(value, other);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_subs_epu8(value, other);
	}

	template<int32_t permuteMask, simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPermute(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_permute2x128_si256(value, other, permuteMask);
	}

	template<int32_t alignment, simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_alignr_epi8(value, other, alignment);
	}

	template<int32_t alignment, simd_int_256_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opSrLi(simd_int_t01 value) noexcept {
		return _mm256_srli_epi16(value, alignment);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_and_si256(value, other);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_or_si256(value, other);
	}

	template<simd_int_256_type simd_int_t01, simd_int_256_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm256_andnot_si256(other, value);
	}

	template<simd_int_256_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opTest(simd_int_t01 value) noexcept {
		return _mm256_testz_si256(value, value);
	}

	template<simd_int_256_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opNot(simd_int_t01 value) noexcept {
		return _mm256_xor_si256(value, _mm256_set1_epi64x(static_cast<int64_t>(0xFFFFFFFFFFFFFFFFll)));
	}

	template<int64_t N, simd_int_256_type simd_type> inline simd_type opPrev(simd_type cur, simd_type prev) {
		return _mm256_alignr_epi8(cur, _mm256_permute2x128_si256(prev, cur, 0x21), N);
	}

	template<simd_int_256_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return opOr(chunks.template get<0>(), chunks.template get<1>());
	}

		#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)

	template<simd_int_512_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValues(const void* __restrict str) noexcept {
		return _mm512_load_si512(static_cast<const __m512i*>(str));
	}

	template<simd_int_512_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValuesU(const void* __restrict str) noexcept {
		return _mm512_loadu_si512(static_cast<const __m512i*>(str));
	}

	template<simd_int_512_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 8)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm512_set1_epi64(static_cast<const int64_t>(value));
	}

	template<simd_int_512_type simd_int_type_new, typename char_t>
		requires(sizeof(char_t) == 1)
	[[maybe_unused]] JSONIFIER_INLINE static simd_int_type_new gatherValue(const char_t value) noexcept {
		return _mm512_set1_epi8(static_cast<const char>(value));
	}

	template<simd_int_512_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void store(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm512_store_si512(static_cast<__m512i*>(storageLocation), value);
	}

	template<simd_int_512_type simd_int_type_new> [[maybe_unused]] JSONIFIER_INLINE static void storeU(simd_int_type_new value, void* __restrict storageLocation) noexcept {
		_mm512_storeu_si512(static_cast<__m512i*>(storageLocation), value);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEq(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint64_t>(_mm512_cmpeq_epi8_mask(value, other));
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLt(simd_int_t01 value, simd_int_t02 other) noexcept {
		return static_cast<uint64_t>(_mm512_cmplt_epu8_mask(value, other));
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpEqRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_movm_epi8(_mm512_cmpeq_epi8_mask(value, other));
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opCmpLtRaw(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_movm_epi8(_mm512_cmplt_epu8_mask(value, other));
	}

	template<simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static uint64_t opBitMask(simd_int_t01 value) noexcept {
		return static_cast<uint64_t>(_mm512_movepi8_mask(value));
	}

	template<simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opBitMaskRaw(simd_int_t01 value) noexcept {
		return _mm512_movepi8_mask(value);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opShuffle(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_shuffle_epi8(value, other);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opXor(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_xor_si512(value, other);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opSubs(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_subs_epu8(value, other);
	}

	template<int32_t permuteMask, simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPermute(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_shuffle_i64x2(value, other, permuteMask);
	}

	template<int32_t alignment, simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAlignR(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_alignr_epi8(value, other, alignment);
	}

	template<int32_t alignment, simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opSrLi(simd_int_t01 value) noexcept {
		return _mm512_srli_epi16(value, alignment);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opAnd(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_and_si512(value, other);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02> [[maybe_unused]] JSONIFIER_INLINE static auto opOr(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_or_si512(value, other);
	}

	template<simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opAndNot(simd_int_t01 value, simd_int_t02 other) noexcept {
		return _mm512_andnot_si512(other, value);
	}

	template<simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opTest(simd_int_t01 value) noexcept {
		return _mm512_test_epi64_mask(value, value) == 0;
	}

	template<simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static auto opNot(simd_int_t01 value) noexcept {
		return _mm512_xor_si512(value, _mm512_set1_epi64(static_cast<int64_t>(0xFFFFFFFFFFFFFFFFll)));
	}

	template<int32_t N, simd_int_512_type simd_int_t01, simd_int_512_type simd_int_t02>
	[[maybe_unused]] JSONIFIER_INLINE static auto opPrev(simd_int_t01 current, simd_int_t02 previous) noexcept {
		return _mm512_alignr_epi8(current, _mm512_permutex2var_epi64(previous, _mm512_set_epi64(13, 12, 11, 10, 9, 8, 7, 6), current), N);
	}

	template<simd_int_512_type simd_int_t01> [[maybe_unused]] JSONIFIER_INLINE static bool anyBitsSetAnywhere(simd_int_t01 value) noexcept {
		return !simd::opTest(value);
	}

	template<simd_int_512_type simd_int_t01, uint64_t totalChunks> JSONIFIER_INLINE static simd_int_t01 orAll(const simd_array<totalChunks> chunks) noexcept {
		return chunks.template get<0>();
	}

		#endif

	#endif

	template<typename simd_type> JSONIFIER_INLINE bool isAscii(simd_type value) {
		return opBitMask(value) == 0;
	}

#endif

}
