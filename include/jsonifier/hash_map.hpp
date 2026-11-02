/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier/hash_map.hpp
 */
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
	#define JSONIFIER_INLINE [[msvc::forceinline]] inline
#else
	#define JSONIFIER_INLINE inline __attribute__((always_inline))
#endif

#if defined(__AVX512BW__)
	#define JSONIFIER_AVX512 1
#else
	#define JSONIFIER_AVX512 0
#endif

#if defined(__AVX2__)
	#define JSONIFIER_AVX2 1
#else
	#define JSONIFIER_AVX2 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
	#define JSONIFIER_SSE2 1
#else
	#define JSONIFIER_SSE2 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
	#define JSONIFIER_NEON 1
#else
	#define JSONIFIER_NEON 0
#endif

#if JSONIFIER_AVX512 || JSONIFIER_AVX2 || JSONIFIER_SSE2
	#include <immintrin.h>
#elif JSONIFIER_NEON
	#include <arm_neon.h>
#endif

#if JSONIFIER_SSE2 || JSONIFIER_NEON
	#define JSONIFIER_MASKED_COMPARE 1
#else
	#define JSONIFIER_MASKED_COMPARE 0
#endif

#if defined(_MSC_VER) && !defined(__clang__)
	#define JSONIFIER_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
	#define JSONIFIER_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))
#endif

#if defined(__GNUC__) || defined(__clang__)
	#define JSONIFIER_OBJECT_SIZE(pointer) __builtin_object_size(pointer, 0)
#else
	#define JSONIFIER_OBJECT_SIZE(pointer) (~static_cast<std::size_t>(0))
#endif

namespace jsonifier {

	template<std::size_t sizeNew> struct string_literal {
		constexpr string_literal(const char (&str)[sizeNew]) noexcept {
			std::copy_n(str, sizeNew, values);
		}

		constexpr std::string_view view() const noexcept {
			return std::string_view{ values, sizeNew - 1 };
		}

		char values[sizeNew]{};
	};

}

namespace jsonifier::internal {

#if JSONIFIER_AVX512
	inline constexpr uint64_t simdBytesPerRegister{ 64 };
#elif JSONIFIER_AVX2
	inline constexpr uint64_t simdBytesPerRegister{ 32 };
#else
	inline constexpr uint64_t simdBytesPerRegister{ 16 };
#endif

	inline constexpr uint64_t npos64{ ~0ull };
	inline constexpr uint8_t npos8{ 0xFFu };
	inline constexpr uint64_t maxStorageSize{ 2048 };

	struct bucket_search {
		JSONIFIER_INLINE static uint64_t matchMask(const uint8_t* bucket, uint8_t byte) noexcept {
#if JSONIFIER_AVX512
			return _mm512_cmpeq_epi8_mask(_mm512_set1_epi8(static_cast<char>(byte)), _mm512_load_si512(bucket));
#elif JSONIFIER_AVX2
			return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_set1_epi8(static_cast<char>(byte)), _mm256_load_si256(std::bit_cast<const __m256i*>(bucket)))));
#elif JSONIFIER_SSE2
			return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_set1_epi8(static_cast<char>(byte)), _mm_load_si128(std::bit_cast<const __m128i*>(bucket)))));
#elif JSONIFIER_NEON
			const uint8x16_t matches = vceqq_u8(vdupq_n_u8(byte), vld1q_u8(bucket));
			return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(matches), 4)), 0);
#else
			uint64_t mask{};
			for (uint64_t x = 0; x < simdBytesPerRegister; ++x) {
				mask |= static_cast<uint64_t>(bucket[x] == byte) << x;
			}
			return mask;
#endif
		}

		JSONIFIER_INLINE static uint64_t firstOffset(uint64_t mask) noexcept {
#if JSONIFIER_NEON
			return static_cast<uint64_t>(std::countr_zero(mask)) >> 2u;
#else
			return static_cast<uint64_t>(std::countr_zero(mask));
#endif
		}
	};

	struct overlapping_compare {
		template<typename value_type> JSONIFIER_INLINE static value_type load(const char* ptr) noexcept {
			value_type value;
			std::memcpy(&value, ptr, sizeof(value_type));
			return value;
		}

		JSONIFIER_INLINE static bool upTo3(const char* lhs, const char* rhs, uint64_t length) noexcept {
			const uint64_t middle{ length >> 1u };
			return ((lhs[0] ^ rhs[0]) | (lhs[middle] ^ rhs[middle]) | (lhs[length - 1] ^ rhs[length - 1])) == 0;
		}

		JSONIFIER_INLINE static bool upTo8(const char* lhs, const char* rhs, uint64_t length) noexcept {
			return ((load<uint32_t>(lhs) ^ load<uint32_t>(rhs)) | (load<uint32_t>(lhs + length - 4) ^ load<uint32_t>(rhs + length - 4))) == 0;
		}

		JSONIFIER_INLINE static bool upTo16(const char* lhs, const char* rhs, uint64_t length) noexcept {
			return ((load<uint64_t>(lhs) ^ load<uint64_t>(rhs)) | (load<uint64_t>(lhs + length - 8) ^ load<uint64_t>(rhs + length - 8))) == 0;
		}

		JSONIFIER_INLINE static bool upTo32(const char* lhs, const char* rhs, uint64_t length) noexcept {
#if JSONIFIER_SSE2
			const __m128i head = _mm_cmpeq_epi8(_mm_loadu_si128(std::bit_cast<const __m128i*>(lhs)), _mm_loadu_si128(std::bit_cast<const __m128i*>(rhs)));
			const __m128i tail = _mm_cmpeq_epi8(_mm_loadu_si128(std::bit_cast<const __m128i*>(lhs + length - 16)), _mm_loadu_si128(std::bit_cast<const __m128i*>(rhs + length - 16)));
			return _mm_movemask_epi8(_mm_and_si128(head, tail)) == 0xFFFF;
#elif JSONIFIER_NEON
			const uint8x16_t head = vceqq_u8(vld1q_u8(std::bit_cast<const uint8_t*>(lhs)), vld1q_u8(std::bit_cast<const uint8_t*>(rhs)));
			const uint8x16_t tail = vceqq_u8(vld1q_u8(std::bit_cast<const uint8_t*>(lhs + length - 16)), vld1q_u8(std::bit_cast<const uint8_t*>(rhs + length - 16)));
			return vminvq_u8(vandq_u8(head, tail)) == 0xFFu;
#else
			return ((load<uint64_t>(lhs) ^ load<uint64_t>(rhs)) | (load<uint64_t>(lhs + 8) ^ load<uint64_t>(rhs + 8)) |
					   (load<uint64_t>(lhs + length - 16) ^ load<uint64_t>(rhs + length - 16)) | (load<uint64_t>(lhs + length - 8) ^ load<uint64_t>(rhs + length - 8))) == 0;
#endif
		}

		JSONIFIER_INLINE static bool upTo64(const char* lhs, const char* rhs, uint64_t length) noexcept {
#if JSONIFIER_AVX2
			const __m256i head = _mm256_cmpeq_epi8(_mm256_loadu_si256(std::bit_cast<const __m256i*>(lhs)), _mm256_loadu_si256(std::bit_cast<const __m256i*>(rhs)));
			const __m256i tail = _mm256_cmpeq_epi8(_mm256_loadu_si256(std::bit_cast<const __m256i*>(lhs + length - 32)), _mm256_loadu_si256(std::bit_cast<const __m256i*>(rhs + length - 32)));
			return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_and_si256(head, tail))) == 0xFFFFFFFFu;
#else
			return upTo32(lhs, rhs, 32) && upTo32(lhs + length - 32, rhs + length - 32, 32);
#endif
		}

		JSONIFIER_INLINE static bool longer(const char* lhs, const char* rhs, uint64_t length) noexcept {
			const char* lhsEnd{ lhs + length };
			const char* rhsEnd{ rhs + length };
			while (length > 64) {
				if (!upTo64(lhs, rhs, 64)) {
					return false;
				}
				length -= 64;
				lhs += 64;
				rhs += 64;
			}
			return upTo64(lhsEnd - 64, rhsEnd - 64, 64);
		}
	};

	template<uint64_t minLength, uint64_t maxLength> struct key_comparison {
		JSONIFIER_INLINE static bool impl(const char* lhs, const char* rhs, uint64_t length) noexcept {
			if constexpr (minLength == maxLength) {
				length = minLength;
			}
			if constexpr (maxLength > 64) {
				if (length > 64) {
					return overlapping_compare::longer(lhs, rhs, length);
				}
			}
			if constexpr (maxLength > 32) {
				if (minLength >= 32 || length > 32) {
					return overlapping_compare::upTo64(lhs, rhs, length);
				}
			}
			if constexpr (maxLength > 16) {
				if (minLength >= 16 || length > 16) {
					return overlapping_compare::upTo32(lhs, rhs, length);
				}
			}
			if constexpr (maxLength > 8) {
				if (minLength >= 8 || length > 8) {
					return overlapping_compare::upTo16(lhs, rhs, length);
				}
			}
			if constexpr (maxLength > 3) {
				if (minLength >= 4 || length > 3) {
					return overlapping_compare::upTo8(lhs, rhs, length);
				}
			}
			if constexpr (minLength == 0) {
				if (length == 0) {
					return true;
				}
			}
			return overlapping_compare::upTo3(lhs, rhs, length);
		}
	};

	inline constexpr std::array<uint64_t, 135ULL> prns{ { 1033321092324544984ull, 2666561049963377653ull, 3901177690447069239ull, 4218182233242110882ull, 5911765535454950103ull,
		6788651254494793497ull, 7100864855074445223ull, 8121427956336305945ull, 9038010914689427860ull, 14840306302415334885ull, 2861875790078914964ull, 3162274379479658823ull,
		4716213344225307449ull, 540950270129450019ull, 6138393194460717092ull, 7344427311844191385ull, 8475133706542525636ull, 9707373313909664576ull, 13125261184447140558ull,
		2935828130652229499ull, 3352961464321085856ull, 4654333323360932970ull, 5071886467123008198ull, 6337413869067417456ull, 7068363609472928302ull, 8706829452892616150ull,
		9383326841165471636ull, 16102866716245881820ull, 2811014628691939071ull, 3268225168635854144ull, 4143407405368768949ull, 5597712091605167573ull, 6100647393685909969ull,
		7810560643861675820ull, 8193567265249468576ull, 9274898615585908930ull, 1186958974127274710ull, 246203706832441443ull, 3668316000003001120ull, 4918933721064431133ull,
		5627034507966762943ull, 6439181573813589114ull, 7007274452014357082ull, 8727797399712164036ull, 9543719554692367837ull, 17847026727775507706ull, 2455135551339952688ull,
		3249111793759010315ull, 4777639692643446085ull, 5509261895474102266ull, 6044529605818700585ull, 7171416927005376731ull, 8039758273674712696ull, 9590025961231307183ull,
		11664492738409977550ull, 2284380607188886295ull, 3813446608469001272ull, 4331825983983119949ull, 5837226587704917004ull, 6635783511790253542ull, 705765947185415012ull,
		8161069307156878177ull, 9569010482089894937ull, 10396430003774290631ull, 2470420036324932164ull, 3824517089721070566ull, 4514057289782578484ull, 5632633334704453746ull,
		6925174598726443993ull, 7516137935779736019ull, 824571755531910559ull, 9361703638678697870ull, 13796235445108347584ull, 2481146946454909733ull, 3823008181066037442ull,
		4754601782272553608ull, 588747081408207180ull, 6322894155156329605ull, 7737051281621502357ull, 8728044800884920985ull, 9923282424466541678ull, 18161647536849824815ull,
		2607456623799892498ull, 3651449820230355939ull, 4624058760756378704ull, 564341449426358799ull, 6732980169420780216ull, 7082954320121844082ull, 8326246156222992233ull,
		9417642353078551282ull, 17539099562248686315ull, 226802774388233589ull, 3258991441457498839ull, 4386515027804287469ull, 5492870100834679754ull, 6249105792560430415ull,
		7289628920991893817ull, 8241072433031030544ull, 9727644451441173921ull, 13586305903807621608ull, 200516020872926547ull, 3730616597292952024ull, 4256645544584917949ull,
		5613544969337462956ull, 6264647669092059269ull, 7960331042009250727ull, 8582958636583556090ull, 9171663339272942914ull, 1619903645133495717ull, 2840616349619109328ull,
		3166096472286566799ull, 4494229804275778550ull, 5497884137148100871ull, 6572487097017879223ull, 738706937289335047ull, 8122825727823277447ull, 9131968469543030694ull,
		14054393997887833558ull, 2874030832643593377ull, 3673904271267944876ull, 4542812785880908260ull, 5621946313585701459ull, 6176632143181793702ull, 7512972502278041818ull,
		8724494295438506783ull, 9277533619161797917ull, 13495127262014153477ull, 2883303557104387784ull, 3039599040070277986ull, 4196273005435491662ull, 5417879022829474871ull,
		6476778602757520149ull, 7959620869796075525ull, 8518936512742009562ull, 9635246566869230345ull } };

	inline constexpr char hashZeroes[4]{};

	struct key_hash_mix {
		static constexpr uint64_t primary{ 0x9E3779B185EBCA87ull };
		static constexpr uint64_t secondary{ 0xC2B2AE3D27D4EB4Full };

		template<typename value_type> JSONIFIER_INLINE static constexpr value_type load(const char* ptr) noexcept {
			if (std::is_constant_evaluated()) {
				std::array<char, sizeof(value_type)> values{};
				std::copy_n(ptr, sizeof(value_type), values.begin());
				return std::bit_cast<value_type>(values);
			}
			value_type value;
			std::memcpy(&value, ptr, sizeof(value_type));
			return value;
		}

		JSONIFIER_INLINE static constexpr uint64_t finish(uint64_t value) noexcept {
			const uint64_t folded{ (value ^ (value >> 32)) * primary };
			return folded ^ (folded >> 32);
		}

		JSONIFIER_INLINE static constexpr uint64_t sampled(uint64_t seed, const char* value, uint64_t length) noexcept {
			const uint64_t bytes{ static_cast<uint64_t>(static_cast<uint8_t>(value[0])) | (static_cast<uint64_t>(static_cast<uint8_t>(value[length >> 1])) << 8) |
				(static_cast<uint64_t>(static_cast<uint8_t>(value[length - 1])) << 16) | (length << 24) };
			const uint64_t mixed{ (seed ^ bytes) * primary };
			return mixed ^ (mixed >> 32);
		}

		JSONIFIER_INLINE static constexpr uint64_t upTo8(uint64_t seed, const char* value, uint64_t length) noexcept {
			const uint64_t hasWords{ length >= 4 };
			const char* const sources[2]{ hashZeroes, value };
			const uint64_t tailOffset{ (length - 4) & (0ull - hasWords) };
			const uint64_t words{ static_cast<uint64_t>(load<uint32_t>(sources[hasWords])) | (static_cast<uint64_t>(load<uint32_t>(sources[hasWords] + tailOffset)) << 32) };
			const uint64_t bytes{ static_cast<uint64_t>(static_cast<uint8_t>(value[0])) | (static_cast<uint64_t>(static_cast<uint8_t>(value[length >> 1])) << 8) |
				(static_cast<uint64_t>(static_cast<uint8_t>(value[length - 1])) << 16) | (length << 24) };
			return finish(seed ^ (words * primary) ^ (bytes * secondary));
		}

		JSONIFIER_INLINE static constexpr uint64_t upTo16(uint64_t seed, const char* value, uint64_t length) noexcept {
			return finish(seed ^ (load<uint64_t>(value) * primary) ^ ((load<uint64_t>(value + length - 8) ^ length) * secondary));
		}

		JSONIFIER_INLINE static constexpr uint64_t longer(uint64_t seed, const char* value, uint64_t length) noexcept {
			const char* tail{ value + length - 16 };
			uint64_t seed64{ seed ^ (length * primary) };
			while (length > 16) {
				seed64 = std::rotl(seed64 ^ (load<uint64_t>(value) * primary) ^ (load<uint64_t>(value + 8) * secondary), 23);
				value += 16;
				length -= 16;
			}
			return finish(seed64 ^ (load<uint64_t>(tail) * primary) ^ (load<uint64_t>(tail + 8) * secondary));
		}
	};

	template<uint64_t minLength, uint64_t maxLength> struct key_hash {
		JSONIFIER_INLINE static constexpr uint64_t impl(uint64_t seed, const char* value, uint64_t length) noexcept {
			if constexpr (minLength == maxLength) {
				length = minLength;
			}
			if constexpr (maxLength > 16) {
				if (minLength > 16 || length > 16) {
					return key_hash_mix::longer(seed, value, length);
				}
			}
			if constexpr (maxLength > 8) {
				if (minLength > 8 || length > 8) {
					return key_hash_mix::upTo16(seed, value, length);
				}
			}
			if constexpr (minLength == 0) {
				if (length == 0) {
					return key_hash_mix::finish(seed);
				}
			}
			return key_hash_mix::upTo8(seed, value, length);
		}
	};

	struct ct_key_hasher {
		consteval uint64_t hashKeyCt(const char* value, uint64_t length) const {
			return key_hash<0, npos64>::impl(seed, value, length);
		}

		constexpr void updateSeed() noexcept {
			seed = prns[index % prns.size()];
			++index;
		}

		constexpr ct_key_hasher() noexcept {
			updateSeed();
		}

		uint64_t index{};
		uint64_t seed{};
	};

	template<string_literal... keys> struct key_list {
		static constexpr std::array<std::string_view, sizeof...(keys)> values{ keys.view()... };
	};

	struct tuple_reference {
		uint8_t oldIndex{};
		std::string_view key{};
	};

	struct tuple_references {
		const tuple_reference* rootPtr{};
		uint64_t count{};
	};

	struct by_first_byte {
		static consteval bool impl(const tuple_reference& lhs, const tuple_reference& rhs) {
			const uint64_t lhsByte = lhs.key.size() ? static_cast<uint8_t>(lhs.key[0]) : 0ull;
			const uint64_t rhsByte = rhs.key.size() ? static_cast<uint8_t>(rhs.key[0]) : 0ull;
			return lhsByte < rhsByte;
		}
	};

	struct by_length {
		static consteval bool impl(const tuple_reference& lhs, const tuple_reference& rhs) {
			return lhs.key.size() < rhs.key.size();
		}
	};

	template<typename value_type> consteval auto collectTupleRefs() {
		std::array<tuple_reference, value_type::values.size()> tupleRefsRaw{};
		for (uint64_t x = 0; x < value_type::values.size(); ++x) {
			tupleRefsRaw[x].oldIndex = static_cast<uint8_t>(x);
			tupleRefsRaw[x].key		 = value_type::values[x];
		}
		return tupleRefsRaw;
	}

	template<typename comparator_type, uint64_t size> consteval std::array<tuple_reference, size> sortTupleRefs(const std::array<tuple_reference, size>& tupleRefsRaw) {
		std::array<tuple_reference, size> returnValues{ tupleRefsRaw };
		for (uint64_t i = 1; i < size; ++i) {
			auto key  = returnValues[i];
			int64_t j = static_cast<int64_t>(i) - 1;
			while (j >= 0 && comparator_type::impl(returnValues[static_cast<uint64_t>(j)], key)) {
				returnValues[static_cast<uint64_t>(j + 1)] = returnValues[static_cast<uint64_t>(j)];
				--j;
			}
			returnValues[static_cast<uint64_t>(j + 1)] = key;
		}
		return returnValues;
	}

	template<uint64_t size> consteval tuple_references consolidateTupleRefs(const std::array<tuple_reference, size>& tupleRefsRaw) {
		tuple_references returnValues{};
		if constexpr (size > 0) {
			returnValues.rootPtr = &tupleRefsRaw[0];
			returnValues.count	 = size;
		}
		return returnValues;
	}

	template<typename value_type> inline constexpr auto tupleRefs{ collectTupleRefs<value_type>() };
	template<typename value_type> inline constexpr auto tupleReferences{ consolidateTupleRefs(tupleRefs<value_type>) };
	template<typename value_type> inline constexpr auto sortedTupleReferencesByLength{ sortTupleRefs<by_length>(tupleRefs<value_type>) };
	template<typename value_type> inline constexpr auto tupleReferencesByLength{ consolidateTupleRefs(sortedTupleReferencesByLength<value_type>) };
	template<typename value_type> inline constexpr auto sortedTupleReferencesByFirstByte{ sortTupleRefs<by_first_byte>(tupleRefs<value_type>) };
	template<typename value_type> inline constexpr auto tupleReferencesByFirstByte{ consolidateTupleRefs(sortedTupleReferencesByFirstByte<value_type>) };

	constexpr bool contains(const uint8_t* hashDataNew, uint8_t byteToCheckFor, uint64_t size) noexcept {
		for (uint64_t x = 0; x < size; ++x) {
			if (hashDataNew[x] == byteToCheckFor) {
				return true;
			}
		}
		return false;
	}

	struct byte_set {
		consteval bool testAndSet(uint8_t value) {
			const uint64_t mask{ 1ull << (value & 63u) };
			uint64_t& word{ bits[value >> 6u] };
			const bool wasSet{ (word & mask) != 0ull };
			word |= mask;
			return wasSet;
		}

		constexpr void clear() noexcept {
			bits[0] = 0ull;
			bits[1] = 0ull;
			bits[2] = 0ull;
			bits[3] = 0ull;
		}

		uint64_t bits[4]{};
	};

	enum class hash_map_types : uint16_t {
		unset						= 0,
		empty						= 1,
		single_element				= 2,
		double_element				= 3,
		triple_element				= 4,
		single_byte					= 5,
		first_byte_and_unique_index = 6,
		unique_byte_and_length		= 7,
		unique_per_length			= 8,
		simd_full_length			= 9,
		count,
	};

	struct key_stats_t {
		uint64_t minLength{ npos64 };
		uint64_t uniqueIndex{};
		uint64_t maxLength{};
	};

	consteval uint64_t findUniqueColumnIndex(const tuple_references& tupleRefsRaw, uint64_t maxIndex, uint64_t startingIndex = 0) {
		byte_set seen{};
		for (uint64_t index = startingIndex; index < maxIndex; ++index) {
			seen.clear();
			bool allDifferent = true;
			for (uint64_t x = 0; x < tupleRefsRaw.count; ++x) {
				if (seen.testAndSet(static_cast<uint8_t>(tupleRefsRaw.rootPtr[x].key[index]))) {
					allDifferent = false;
					break;
				}
			}
			if (allDifferent) {
				return index;
			}
		}
		return npos64;
	}

	consteval uint64_t nextPowerOfTwo(uint64_t value) {
		uint64_t result = 16;
		while (result < value) {
			result <<= 1;
		}
		return result;
	}

	consteval uint64_t storageSizeFor(uint64_t keyCount) {
		const uint64_t target{ nextPowerOfTwo(keyCount * 16ull) };
		return target < 256ull ? 256ull : (target > maxStorageSize ? maxStorageSize : target);
	}

	template<uint64_t storageSizeNew> struct hash_map_construction_data {
		static constexpr uint64_t storageSize{ storageSizeNew };
		static constexpr uint64_t numGroups{ storageSizeNew / simdBytesPerRegister };

		std::array<uint16_t, numGroups> bucketSizes{};
		std::array<uint8_t, storageSizeNew + 1ULL> controlBytes{};
		std::array<uint16_t, storageSizeNew + 1ULL> indices{};
		std::array<uint8_t, 256ULL> uniqueIndices{};
		ct_key_hasher hasher{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
		char firstChar{};
		bool sampled{};
	};

	struct empty_data {
		template<uint64_t storageSizeNew> constexpr empty_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	struct single_element_data {
		template<uint64_t storageSizeNew> constexpr single_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	struct double_element_data {
		template<uint64_t storageSizeNew> constexpr double_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct triple_element_data {
		template<uint64_t storageSizeNew> constexpr triple_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndex{ newData.uniqueIndex }, seed{ newData.hasher.seed }, type{ newData.type }, firstChar{ newData.firstChar } {
		}
		uint64_t uniqueIndex{};
		uint64_t seed{};
		hash_map_types type{};
		char firstChar{};
	};

	struct single_byte_data {
		template<uint64_t storageSizeNew> constexpr single_byte_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndices{ newData.uniqueIndices }, uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		std::array<uint8_t, 256ULL> uniqueIndices{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct first_byte_and_unique_index_data {
		template<uint64_t storageSizeNew> constexpr first_byte_and_unique_index_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	template<uint64_t storageSizeNew> struct unique_byte_and_length_data {
		static constexpr uint64_t storageSize{ storageSizeNew };
		constexpr unique_byte_and_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: indices{ newData.indices }, uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		std::array<uint16_t, storageSizeNew + 1ULL> indices{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct unique_per_length_data {
		template<uint64_t storageSizeNew> constexpr unique_per_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndices{ newData.uniqueIndices }, type{ newData.type } {
		}
		std::array<uint8_t, 256ULL> uniqueIndices{};
		hash_map_types type{};
	};

	template<uint64_t storageSizeNew> struct simd_full_length_data {
		static constexpr uint64_t numGroups{ storageSizeNew / simdBytesPerRegister };

		constexpr simd_full_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: controlBytes{ newData.controlBytes }, indices{ newData.indices }, uniqueIndex{ newData.uniqueIndex }, seed{ newData.hasher.seed }, type{ newData.type }, sampled{ newData.sampled } {
		}

		alignas(simdBytesPerRegister) std::array<uint8_t, storageSizeNew + 1ULL> controlBytes{};
		std::array<uint16_t, storageSizeNew + 1ULL> indices{};
		uint64_t uniqueIndex{};
		uint64_t seed{};
		hash_map_types type{};
		bool sampled{};
	};

	struct string_lengths : public tuple_references {
		uint64_t length{};
	};

	consteval uint64_t countUniqueLengths(const tuple_references& sortedByLength) {
		uint64_t returnValue{};
		uint64_t x{};
		while (x < sortedByLength.count) {
			const uint64_t length{ sortedByLength.rootPtr[x].key.size() };
			++returnValue;
			while (x < sortedByLength.count && sortedByLength.rootPtr[x].key.size() == length) {
				++x;
			}
		}
		return returnValue;
	}

	template<uint64_t stringLengthCount> consteval std::array<string_lengths, stringLengthCount> collectLengths(const tuple_references& sortedByLength) {
		std::array<string_lengths, stringLengthCount> valuesNew{};
		uint64_t currentIndex{};
		uint64_t x{};
		while (x < sortedByLength.count && currentIndex < stringLengthCount) {
			const uint64_t length{ sortedByLength.rootPtr[x].key.size() };
			string_lengths tupleRefsRaw{};
			tupleRefsRaw.rootPtr = &sortedByLength.rootPtr[x];
			tupleRefsRaw.length	 = length;
			while (x < sortedByLength.count && sortedByLength.rootPtr[x].key.size() == length) {
				++tupleRefsRaw.count;
				++x;
			}
			valuesNew[currentIndex] = tupleRefsRaw;
			++currentIndex;
		}
		return valuesNew;
	}

	struct first_byte_columns {
		std::array<uint8_t, 256> uniqueIndexByFirstByte{};
		bool valid{};
	};

	consteval first_byte_columns collectFirstByteColumns(const tuple_references& sortedByFirstByte) {
		first_byte_columns result{};
		result.uniqueIndexByFirstByte.fill(npos8);
		byte_set seen{};
		uint64_t bucketStart{};
		while (bucketStart < sortedByFirstByte.count) {
			if (sortedByFirstByte.rootPtr[bucketStart].key.empty()) {
				return result;
			}
			const uint8_t firstByte{ static_cast<uint8_t>(sortedByFirstByte.rootPtr[bucketStart].key[0]) };
			uint64_t bucketEnd{ bucketStart };
			uint64_t minLenInBucket{ npos64 };
			while (bucketEnd < sortedByFirstByte.count && !sortedByFirstByte.rootPtr[bucketEnd].key.empty() &&
				static_cast<uint8_t>(sortedByFirstByte.rootPtr[bucketEnd].key[0]) == firstByte) {
				const uint64_t length{ sortedByFirstByte.rootPtr[bucketEnd].key.size() };
				if (length < minLenInBucket) {
					minLenInBucket = length;
				}
				++bucketEnd;
			}
			uint64_t chosenColumn{ npos64 };
			for (uint64_t col = 0; col < minLenInBucket; ++col) {
				seen.clear();
				bool allDifferent = true;
				for (uint64_t b = bucketStart; b < bucketEnd; ++b) {
					if (seen.testAndSet(static_cast<uint8_t>(sortedByFirstByte.rootPtr[b].key[col]))) {
						allDifferent = false;
						break;
					}
				}
				if (allDifferent) {
					chosenColumn = col;
					break;
				}
			}
			if (chosenColumn == npos64 || chosenColumn >= npos8) {
				return result;
			}
			result.uniqueIndexByFirstByte[firstByte] = static_cast<uint8_t>(chosenColumn);
			bucketStart								 = bucketEnd;
		}
		result.valid = true;
		return result;
	}

	template<typename value_type> inline constexpr auto firstByteColumns{ collectFirstByteColumns(tupleReferencesByFirstByte<value_type>) };

	struct first_byte_arrays {
		std::array<uint8_t, 256 * 256> indexByFirstByteAndChar{};
		std::array<uint8_t, 256> uniqueIndexByFirstByte{};
	};

	consteval first_byte_arrays buildFirstByteArrays(const tuple_references& sortedByFirstByte, const first_byte_columns& columns) {
		first_byte_arrays result{};
		result.uniqueIndexByFirstByte = columns.uniqueIndexByFirstByte;
		result.indexByFirstByteAndChar.fill(npos8);
		for (uint64_t x = 0; x < sortedByFirstByte.count; ++x) {
			const auto& ref{ sortedByFirstByte.rootPtr[x] };
			const uint64_t firstByte{ static_cast<uint8_t>(ref.key[0]) };
			const uint64_t chosenColumn{ columns.uniqueIndexByFirstByte[firstByte] };
			const uint64_t keyChar{ static_cast<uint8_t>(ref.key[chosenColumn]) };
			result.indexByFirstByteAndChar[(firstByte << 8) | keyChar] = ref.oldIndex;
		}
		return result;
	}

	template<typename value_type> inline constexpr auto firstByteArrays{ buildFirstByteArrays(tupleReferencesByFirstByte<value_type>, firstByteColumns<value_type>) };

	consteval key_stats_t keyStatsImpl(const tuple_references& tupleRefsRaw) {
		key_stats_t stats{};
		for (uint64_t x = 0; x < tupleRefsRaw.count; ++x) {
			const uint64_t num{ tupleRefsRaw.rootPtr[x].key.size() };
			if (num > stats.maxLength) {
				stats.maxLength = num;
			}
			if (num < stats.minLength) {
				stats.minLength = num;
			}
		}
		stats.uniqueIndex = findUniqueColumnIndex(tupleRefsRaw, stats.minLength);
		return stats;
	}

	template<typename value_type> inline constexpr auto keyStatsVal = keyStatsImpl(tupleReferences<value_type>);

	template<typename value_type, uint64_t storageSize>
	consteval bool placeSimdKeys(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		constexpr uint16_t indexSentinel{ static_cast<uint16_t>(storageSize) };
		constexpr uint64_t numGroups{ hash_map_construction_data<storageSize>::numGroups };
		returnValues.bucketSizes.fill(0);
		returnValues.controlBytes.fill(npos8);
		returnValues.indices.fill(indexSentinel);
		returnValues.hasher.updateSeed();
		for (uint64_t y = 0; y < pairsNew.count; ++y) {
			const auto& key			 = pairsNew.rootPtr[y].key;
			const auto keyLength	 = returnValues.uniqueIndex > key.size() ? key.size() : returnValues.uniqueIndex;
			const auto hash			 = returnValues.sampled ? key_hash_mix::sampled(returnValues.hasher.seed, key.data(), key.size()) : returnValues.hasher.hashKeyCt(key.data(), keyLength);
			const auto groupPos		 = (hash >> 8) % numGroups;
			const auto ctrlByte		 = static_cast<uint8_t>(hash);
			const auto bucketStart	 = groupPos * simdBytesPerRegister;
			const auto bucketSizeNew = returnValues.bucketSizes[groupPos]++;
			const auto slot			 = bucketStart + bucketSizeNew;
			if (bucketSizeNew >= simdBytesPerRegister || returnValues.indices[slot] != indexSentinel ||
				contains(returnValues.controlBytes.data() + bucketStart, ctrlByte, simdBytesPerRegister)) {
				return false;
			}
			returnValues.controlBytes[slot] = ctrlByte;
			returnValues.indices[slot]		= pairsNew.rootPtr[y].oldIndex;
		}
		return true;
	}

	template<typename value_type, uint64_t storageSize>
	consteval void collectSimdFullLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		if constexpr (keyStatsVal<value_type>.minLength > 0) {
			returnValues.sampled = true;
			for (uint64_t x = 0; x < 64; ++x) {
				if (placeSimdKeys<value_type>(returnValues, pairsNew)) {
					returnValues.type = hash_map_types::simd_full_length;
					return;
				}
			}
			returnValues.sampled = false;
		}
		bool collided{ true };
		for (uint64_t w = keyStatsVal<value_type>.minLength; w <= keyStatsVal<value_type>.maxLength; ++w) {
			returnValues.uniqueIndex = w;
			for (uint64_t x = 0; x < 16; ++x) {
				collided = !placeSimdKeys<value_type>(returnValues, pairsNew);
				if (!collided) {
					break;
				}
			}
			if (!collided) {
				break;
			}
		}
		if (collided) {
			returnValues.type		 = hash_map_types::unset;
			returnValues.uniqueIndex = npos64;
		} else {
			returnValues.type = hash_map_types::simd_full_length;
		}
	}

	template<typename value_type, uint64_t storageSize>
	consteval void collectUniquePerLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		bool fallback = false;
		if constexpr (keyStatsVal<value_type>.maxLength < 256) {
			constexpr auto uniqueLengthCount = countUniqueLengths(tupleReferencesByLength<value_type>);
			constexpr auto results			 = collectLengths<uniqueLengthCount>(tupleReferencesByLength<value_type>);
			returnValues.uniqueIndices.fill(npos8);
			for (uint64_t x = 0; x < uniqueLengthCount; ++x) {
				const auto uniqueIndex = findUniqueColumnIndex(results[x], results[x].length);
				if (uniqueIndex == npos64 || uniqueIndex >= npos8) {
					fallback = true;
					break;
				}
				returnValues.uniqueIndices[results[x].length] = static_cast<uint8_t>(uniqueIndex);
			}
		} else {
			fallback = true;
		}
		if (fallback) {
			collectSimdFullLengthHashMapData<value_type>(returnValues, pairsNew);
		} else {
			returnValues.type = hash_map_types::unique_per_length;
		}
	}

	template<typename value_type, uint64_t storageSize>
	consteval void collectUniqueByteAndLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		constexpr uint16_t indexSentinel{ static_cast<uint16_t>(storageSize) };
		bool collided{ true };
		returnValues.uniqueIndex = 0;
		while (returnValues.uniqueIndex < keyStatsVal<value_type>.minLength) {
			returnValues.indices.fill(indexSentinel);
			collided = false;
			for (uint64_t x = 0; x < pairsNew.count; ++x) {
				const auto& key = pairsNew.rootPtr[x].key;
				const auto hash = static_cast<uint64_t>(static_cast<uint8_t>(key[returnValues.uniqueIndex])) ^ static_cast<uint64_t>(key.size());
				const auto slot = hash % storageSize;
				if (returnValues.indices[slot] != indexSentinel) {
					collided = true;
					break;
				}
				returnValues.indices[slot] = pairsNew.rootPtr[x].oldIndex;
			}
			if (!collided) {
				break;
			}
			++returnValues.uniqueIndex;
		}
		if (collided) {
			collectUniquePerLengthHashMapData<value_type>(returnValues, pairsNew);
		} else {
			returnValues.type = hash_map_types::unique_byte_and_length;
		}
	}

	template<typename value_type, uint64_t storageSize>
	consteval void collectFirstByteAndUniqueIndexHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		if constexpr (keyStatsVal<value_type>.maxLength < 256 && firstByteColumns<value_type>.valid) {
			returnValues.type = hash_map_types::first_byte_and_unique_index;
		} else {
			collectUniqueByteAndLengthHashMapData<value_type>(returnValues, pairsNew);
		}
	}

	// Sampled from Stephen Berry and his library, Glaze library: https://github.com/StephenBerry/Glaze
	template<typename value_type, uint64_t storageSize>
	consteval void collectSingleByteHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		returnValues.uniqueIndex = keyStatsVal<value_type>.uniqueIndex;
		if (returnValues.uniqueIndex != npos64) {
			returnValues.uniqueIndices.fill(npos8);
			for (uint64_t x = 0; x < pairsNew.count; ++x) {
				const auto& newRef				 = pairsNew.rootPtr[x];
				const auto slot					 = static_cast<uint8_t>(newRef.key[returnValues.uniqueIndex]);
				returnValues.uniqueIndices[slot] = newRef.oldIndex;
			}
			returnValues.type = hash_map_types::single_byte;
		} else {
			collectFirstByteAndUniqueIndexHashMapData<value_type>(returnValues, pairsNew);
		}
	}

	// Sampled from Stephen Berry and his library, Glaze library: https://github.com/StephenBerry/Glaze
	template<typename value_type, uint64_t storageSize>
	consteval void collectTripleElementHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		returnValues.uniqueIndex = keyStatsVal<value_type>.uniqueIndex;
		bool collided{ true };
		while (returnValues.uniqueIndex != npos64) {
			returnValues.firstChar = pairsNew.rootPtr[0].key[returnValues.uniqueIndex];
			const auto firstByte   = static_cast<uint8_t>(returnValues.firstChar);
			const auto mix1		   = static_cast<uint8_t>(static_cast<uint8_t>(pairsNew.rootPtr[1].key[returnValues.uniqueIndex]) ^ firstByte);
			const auto mix2		   = static_cast<uint8_t>(static_cast<uint8_t>(pairsNew.rootPtr[2].key[returnValues.uniqueIndex]) ^ firstByte);
			for (uint64_t x = 0; x < 4; ++x) {
				const uint8_t hash1 = static_cast<uint8_t>((mix1 * static_cast<uint8_t>(returnValues.hasher.seed)) & 3);
				const uint8_t hash2 = static_cast<uint8_t>((mix2 * static_cast<uint8_t>(returnValues.hasher.seed)) & 3);

				if (hash1 == 1 && hash2 == 2) {
					collided = false;
					break;
				} else {
					returnValues.hasher.updateSeed();
				}
			}
			if (!collided) {
				break;
			}
			returnValues.uniqueIndex = findUniqueColumnIndex(pairsNew, keyStatsVal<value_type>.minLength, returnValues.uniqueIndex + 1);
		}
		if (collided) {
			collectSingleByteHashMapData<value_type>(returnValues, pairsNew);
		} else {
			returnValues.type = hash_map_types::triple_element;
		}
	}

	template<typename value_type, uint64_t storageSize>
	consteval void collectDoubleElementHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) {
		returnValues.uniqueIndex = keyStatsVal<value_type>.uniqueIndex;
		bool collided{ true };
		while (returnValues.uniqueIndex != npos64) {
			if ((static_cast<uint8_t>(pairsNew.rootPtr[0].key[returnValues.uniqueIndex]) & 1u) == 0u && (static_cast<uint8_t>(pairsNew.rootPtr[1].key[returnValues.uniqueIndex]) & 1u) == 1u) {
				collided = false;
				break;
			}
			returnValues.uniqueIndex = findUniqueColumnIndex(pairsNew, keyStatsVal<value_type>.minLength, returnValues.uniqueIndex + 1);
		}
		if (collided) {
			collectSingleByteHashMapData<value_type>(returnValues, pairsNew);
		} else {
			returnValues.type = hash_map_types::double_element;
		}
	}

	template<typename value_type, uint64_t storageSize> consteval hash_map_construction_data<storageSize> collectMapConstructionDataImpl() {
		hash_map_construction_data<storageSize> returnValues{};
		if constexpr (tupleReferences<value_type>.count == 0) {
			returnValues.type = hash_map_types::empty;
		} else if constexpr (tupleReferences<value_type>.count == 1) {
			returnValues.type = hash_map_types::single_element;
		} else if constexpr (keyStatsVal<value_type>.uniqueIndex != npos64) {
			if constexpr (tupleReferences<value_type>.count == 2) {
				collectDoubleElementHashMapData<value_type>(returnValues, tupleReferences<value_type>);
			} else if constexpr (tupleReferences<value_type>.count == 3) {
				collectTripleElementHashMapData<value_type>(returnValues, tupleReferences<value_type>);
			} else {
				collectSingleByteHashMapData<value_type>(returnValues, tupleReferences<value_type>);
			}
		} else {
			collectFirstByteAndUniqueIndexHashMapData<value_type>(returnValues, tupleReferences<value_type>);
		}
		return returnValues;
	}

	template<typename value_type, uint64_t storageSize> inline constexpr auto mapConstructionDataAt = collectMapConstructionDataImpl<value_type, storageSize>();

	template<typename value_type> consteval const auto& selectMapConstructionData() {
		constexpr auto preferredSize = storageSizeFor(tupleReferences<value_type>.count);
		if constexpr (preferredSize < maxStorageSize && mapConstructionDataAt<value_type, preferredSize>.type == hash_map_types::unset) {
			return mapConstructionDataAt<value_type, maxStorageSize>;
		} else {
			return mapConstructionDataAt<value_type, preferredSize>;
		}
	}

	template<typename value_type> inline constexpr const auto& mapConstructionData = selectMapConstructionData<value_type>();

	template<typename value_type> consteval auto collectMapConstructionData() {
		constexpr auto& constructionData = mapConstructionData<value_type>;
		constexpr auto storageSize		 = std::remove_cvref_t<decltype(constructionData)>::storageSize;
		static_assert(constructionData.type != hash_map_types::unset, "Failed to construct that hashmap!");
		if constexpr (constructionData.type == hash_map_types::empty) {
			return empty_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::single_element) {
			return single_element_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::double_element) {
			return double_element_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::triple_element) {
			return triple_element_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::single_byte) {
			return single_byte_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::first_byte_and_unique_index) {
			return first_byte_and_unique_index_data{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::unique_byte_and_length) {
			return unique_byte_and_length_data<storageSize>{ constructionData };
		} else if constexpr (constructionData.type == hash_map_types::unique_per_length) {
			return unique_per_length_data{ constructionData };
		} else {
			return simd_full_length_data<storageSize>{ constructionData };
		}
	}

	template<uint64_t keyMaxLength>
	consteval std::array<uint8_t, (keyMaxLength + 1) * 256> generateMappingsForLengths(const tuple_references& keys, const std::array<uint8_t, 256>& uniqueIndices) {
		std::array<uint8_t, (keyMaxLength + 1) * 256> mappings{};
		mappings.fill(npos8);
		for (uint64_t x = 0; x < keys.count; ++x) {
			const auto& key			  = keys.rootPtr[x].key;
			const uint8_t uniqueIndex = uniqueIndices[key.size()];
			if (uniqueIndex != npos8 && uniqueIndex < key.size()) {
				const uint64_t keyChar	 = static_cast<uint8_t>(key[uniqueIndex]);
				const uint64_t flatIndex = key.size() * 256 + keyChar;
				mappings[flatIndex]		 = keys.rootPtr[x].oldIndex;
			}
		}
		return mappings;
	}

	template<typename value_type> inline constexpr auto hashData = collectMapConstructionData<value_type>();

	template<typename value_type> inline constexpr auto lengthMappings{ generateMappingsForLengths<keyStatsVal<value_type>.maxLength>(tupleReferencesByLength<value_type>,
		hashData<value_type>.uniqueIndices) };

	template<typename value_type> consteval std::string_view strategyNameOf() {
		constexpr hash_map_types type{ hashData<value_type>.type };
		if constexpr (type == hash_map_types::simd_full_length) {
			return hashData<value_type>.sampled ? "simd_sampled_hash" : "simd_full_hash";
		} else if constexpr (type == hash_map_types::empty) {
			return "empty";
		} else if constexpr (type == hash_map_types::single_element) {
			return "single_element";
		} else if constexpr (type == hash_map_types::double_element) {
			return "double_element";
		} else if constexpr (type == hash_map_types::triple_element) {
			return "triple_element";
		} else if constexpr (type == hash_map_types::single_byte) {
			return "single_byte";
		} else if constexpr (type == hash_map_types::first_byte_and_unique_index) {
			return "first_byte_and_unique_index";
		} else if constexpr (type == hash_map_types::unique_byte_and_length) {
			return "unique_byte_and_length";
		} else {
			return "unique_per_length";
		}
	}

	consteval uint64_t maskedCompareWidth(uint64_t maxLength) {
		return maxLength <= 16 ? 16 : (maxLength <= 32 ? 32 : 64);
	}

	consteval std::array<uint8_t, 128> buildMaskWindow() {
		std::array<uint8_t, 128> window{};
		for (uint64_t x = 0; x < 64; ++x) {
			window[x] = 0xFFu;
		}
		return window;
	}

	alignas(64) inline constexpr std::array<uint8_t, 128> maskWindow{ buildMaskWindow() };

#if JSONIFIER_MASKED_COMPARE
	template<uint64_t bytes> struct masked_chunk;

	template<> struct masked_chunk<16> {
		static constexpr uint32_t fullMask{ 0xFFFFu };

	#if JSONIFIER_SSE2
		using vector_type = __m128i;

		JSONIFIER_INLINE JSONIFIER_NO_SANITIZE_ADDRESS static vector_type loadInput(const char* data, uint64_t length) noexcept {
			if (JSONIFIER_OBJECT_SIZE(data) >= 16 && (reinterpret_cast<uintptr_t>(data) & 4095u) <= 4096u - 16u) [[likely]] {
				return _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
			}
			alignas(16) char buffer[16]{};
			std::memcpy(buffer, data, length);
			return _mm_load_si128(reinterpret_cast<const __m128i*>(buffer));
		}

		JSONIFIER_INLINE static uint32_t impl(const char* data, uint64_t length, const char* stored) noexcept {
			const __m128i mask{ _mm_loadu_si128(reinterpret_cast<const __m128i*>(maskWindow.data() + 64 - length)) };
			const __m128i input{ _mm_and_si128(loadInput(data, length), mask) };
			return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(input, _mm_load_si128(reinterpret_cast<const __m128i*>(stored)))));
		}
	#else
		using vector_type = uint8x16_t;

		JSONIFIER_INLINE JSONIFIER_NO_SANITIZE_ADDRESS static vector_type loadInput(const char* data, uint64_t length) noexcept {
			if (JSONIFIER_OBJECT_SIZE(data) >= 16 && (reinterpret_cast<uintptr_t>(data) & 4095u) <= 4096u - 16u) [[likely]] {
				return vld1q_u8(reinterpret_cast<const uint8_t*>(data));
			}
			alignas(16) uint8_t buffer[16]{};
			std::memcpy(buffer, data, length);
			return vld1q_u8(buffer);
		}

		JSONIFIER_INLINE static uint32_t impl(const char* data, uint64_t length, const char* stored) noexcept {
			const uint8x16_t input{ vandq_u8(loadInput(data, length), vld1q_u8(maskWindow.data() + 64 - length)) };
			return vminvq_u8(vceqq_u8(input, vld1q_u8(reinterpret_cast<const uint8_t*>(stored)))) == 0xFFu ? fullMask : 0u;
		}
	#endif
	};

	#if JSONIFIER_AVX2
	template<> struct masked_chunk<32> {
		static constexpr uint32_t fullMask{ 0xFFFFFFFFu };

		JSONIFIER_INLINE JSONIFIER_NO_SANITIZE_ADDRESS static __m256i loadInput(const char* data, uint64_t length) noexcept {
			if (JSONIFIER_OBJECT_SIZE(data) >= 32 && (reinterpret_cast<uintptr_t>(data) & 4095u) <= 4096u - 32u) [[likely]] {
				return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
			}
			alignas(32) char buffer[32]{};
			std::memcpy(buffer, data, length);
			return _mm256_load_si256(reinterpret_cast<const __m256i*>(buffer));
		}

		JSONIFIER_INLINE static uint32_t impl(const char* data, uint64_t length, const char* stored) noexcept {
			const __m256i mask{ _mm256_loadu_si256(reinterpret_cast<const __m256i*>(maskWindow.data() + 64 - length)) };
			const __m256i input{ _mm256_and_si256(loadInput(data, length), mask) };
			return static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(input, _mm256_load_si256(reinterpret_cast<const __m256i*>(stored)))));
		}
	};
	#endif

	template<uint64_t width> struct masked_compare {
	#if JSONIFIER_AVX2
		static constexpr uint64_t chunkBytes{ width >= 32 ? 32 : 16 };
	#else
		static constexpr uint64_t chunkBytes{ 16 };
	#endif
		using chunk_type = masked_chunk<chunkBytes>;

		JSONIFIER_INLINE static bool impl(const char* data, uint64_t length, const char* stored) noexcept {
			const uint64_t clamped{ length < width ? length : width };
	#if JSONIFIER_AVX512
			if constexpr (width == 64) {
				const __mmask64 mask{ clamped >= 64 ? ~0ull : (1ull << clamped) - 1ull };
				const __m512i input{ _mm512_maskz_loadu_epi8(mask, data) };
				return _mm512_cmpneq_epi8_mask(input, _mm512_load_si512(stored)) == 0;
			}
	#endif
			uint32_t matches{ chunk_type::fullMask };
			for (uint64_t x = 0; x < width / chunkBytes; ++x) {
				const uint64_t offset{ x * chunkBytes };
				const uint64_t remaining{ clamped > offset ? clamped - offset : 0 };
				const uint64_t chunkLength{ remaining < chunkBytes ? remaining : chunkBytes };
				matches &= chunk_type::impl(chunkLength > 0 ? data + offset : stored + offset, chunkLength, stored + offset);
			}
			return matches == chunk_type::fullMask;
		}
	};
#endif

	template<typename value_type> struct flat_key_table {
		static constexpr uint64_t keyCount{ value_type::values.size() };
		static constexpr uint64_t maxLength{ keyStatsVal<value_type>.maxLength };
		static constexpr uint64_t paddedLength{ std::bit_ceil(maxLength > 0 ? maxLength : 1ull) };
		static constexpr uint64_t stride{ paddedLength > maskedCompareWidth(maxLength) ? paddedLength : maskedCompareWidth(maxLength) };

		consteval flat_key_table() {
			for (uint64_t x = 0; x < keyCount; ++x) {
				const std::string_view key{ value_type::values[x] };
				lengths[x] = key.size();
				for (uint64_t y = 0; y < key.size(); ++y) {
					keys[x * stride + y] = key[y];
				}
			}
			lengths[keyCount] = npos64;
		}

		alignas(64) std::array<char, (keyCount + 1) * stride> keys{};
		std::array<uint64_t, keyCount + 1> lengths{};
	};

	struct verify_entry {
		const char* data{};
		uint64_t size{ npos64 };
	};

	template<typename value_type> consteval std::array<verify_entry, value_type::values.size() + 1> buildVerifyEntries() {
		std::array<verify_entry, value_type::values.size() + 1> entries{};
		for (uint64_t x = 0; x < value_type::values.size(); ++x) {
			entries[x].data = value_type::values[x].data();
			entries[x].size = value_type::values[x].size();
		}
		return entries;
	}

	template<typename value_type> inline constexpr auto verifyEntries{ buildVerifyEntries<value_type>() };

	template<typename value_type> inline constexpr flat_key_table<value_type> flatKeys{};

	consteval bool hasSingleCompareTier(uint64_t minLength, uint64_t maxLength) {
		return (minLength >= 1 && maxLength <= 3) || (minLength >= 4 && maxLength <= 8) || (minLength >= 8 && maxLength <= 16) || (minLength >= 16 && maxLength <= 32) || (minLength >= 32 && maxLength <= 64);
	}

	enum class verify_modes : uint8_t {
		exact_flat,
		masked_flat,
		indirect,
	};

	consteval verify_modes selectVerifyMode(uint64_t minLength, uint64_t maxLength) {
		if (hasSingleCompareTier(minLength, maxLength)) {
			return verify_modes::exact_flat;
		}
		return (JSONIFIER_MASKED_COMPARE && maxLength <= 64) ? verify_modes::masked_flat : verify_modes::indirect;
	}

	template<typename value_type, verify_modes mode = selectVerifyMode(keyStatsVal<value_type>.minLength, keyStatsVal<value_type>.maxLength)> struct key_verifier {
		using comparison_type = key_comparison<keyStatsVal<value_type>.minLength, keyStatsVal<value_type>.maxLength>;

		JSONIFIER_INLINE static bool impl(uint64_t index, const char* data, uint64_t length) noexcept {
			return flatKeys<value_type>.lengths[index] == length && comparison_type::impl(data, flatKeys<value_type>.keys.data() + index * flat_key_table<value_type>::stride, length);
		}
	};

#if JSONIFIER_MASKED_COMPARE
	template<typename value_type> struct key_verifier<value_type, verify_modes::masked_flat> {
		using comparison_type = masked_compare<maskedCompareWidth(keyStatsVal<value_type>.maxLength)>;

		JSONIFIER_INLINE static bool impl(uint64_t index, const char* data, uint64_t length) noexcept {
			const bool lengthMatches{ flatKeys<value_type>.lengths[index] == length };
			const bool bytesMatch{ comparison_type::impl(data, length, flatKeys<value_type>.keys.data() + index * flat_key_table<value_type>::stride) };
			return lengthMatches & bytesMatch;
		}
	};
#endif

	template<typename value_type> struct key_verifier<value_type, verify_modes::indirect> {
		using comparison_type = key_comparison<keyStatsVal<value_type>.minLength, keyStatsVal<value_type>.maxLength>;

		JSONIFIER_INLINE static bool impl(uint64_t index, const char* data, uint64_t length) noexcept {
			const verify_entry& expected{ verifyEntries<value_type>[index] };
			return expected.size == length && comparison_type::impl(data, expected.data, length);
		}
	};

	template<typename value_type, hash_map_types type = hashData<value_type>.type> struct candidate_finder;

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::empty> {
		JSONIFIER_INLINE static uint64_t impl(const char*, uint64_t) noexcept {
			return npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::single_element> {
		JSONIFIER_INLINE static uint64_t impl(const char*, uint64_t) noexcept {
			return 0ull;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::double_element> {
		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			return length > hashData<value_type>.uniqueIndex ? static_cast<uint64_t>(static_cast<uint8_t>(data[hashData<value_type>.uniqueIndex]) & 1u) : npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::triple_element> {
		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			if (length > hashData<value_type>.uniqueIndex) {
				const uint8_t mixed{ static_cast<uint8_t>(static_cast<uint8_t>(data[hashData<value_type>.uniqueIndex]) ^ static_cast<uint8_t>(hashData<value_type>.firstChar)) };
				return (mixed * hashData<value_type>.seed) & 3u;
			}
			return npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::single_byte> {
		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			return length > hashData<value_type>.uniqueIndex ? hashData<value_type>.uniqueIndices[static_cast<uint8_t>(data[hashData<value_type>.uniqueIndex])] : npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::first_byte_and_unique_index> {
		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			if (length > 0) {
				const uint64_t firstByte{ static_cast<uint8_t>(data[0]) };
				const uint64_t uniqueIndex{ firstByteArrays<value_type>.uniqueIndexByFirstByte[firstByte] };
				if (uniqueIndex < length) {
					return firstByteArrays<value_type>.indexByFirstByteAndChar[(firstByte << 8) | static_cast<uint8_t>(data[uniqueIndex])];
				}
			}
			return npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::unique_byte_and_length> {
		static constexpr uint64_t storageMask{ std::remove_cvref_t<decltype(hashData<value_type>)>::storageSize - 1 };

		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			if (length > hashData<value_type>.uniqueIndex) {
				return hashData<value_type>.indices[(static_cast<uint64_t>(static_cast<uint8_t>(data[hashData<value_type>.uniqueIndex])) ^ length) & storageMask];
			}
			return npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::unique_per_length> {
		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			if (length <= keyStatsVal<value_type>.maxLength) {
				const uint64_t uniqueIndex{ hashData<value_type>.uniqueIndices[length] };
				if (uniqueIndex < length) {
					return lengthMappings<value_type>[(length << 8) | static_cast<uint8_t>(data[uniqueIndex])];
				}
			}
			return npos64;
		}
	};

	template<typename value_type> struct candidate_finder<value_type, hash_map_types::simd_full_length> {
		static constexpr uint64_t groupMask{ std::remove_cvref_t<decltype(hashData<value_type>)>::numGroups - 1 };
		static constexpr uint64_t minLength{ keyStatsVal<value_type>.minLength };
		static constexpr uint64_t lengthSpread{ keyStatsVal<value_type>.maxLength - minLength };

		JSONIFIER_INLINE static uint64_t computeHash(const char* data, uint64_t length) noexcept {
			if constexpr (hashData<value_type>.sampled) {
				return key_hash_mix::sampled(hashData<value_type>.seed, data, length);
			} else {
				const uint64_t hashLength{ hashData<value_type>.uniqueIndex > length ? length : hashData<value_type>.uniqueIndex };
				return key_hash<minLength, hashData<value_type>.uniqueIndex>::impl(hashData<value_type>.seed, data, hashLength);
			}
		}

		JSONIFIER_INLINE static uint64_t impl(const char* data, uint64_t length) noexcept {
			if (length - minLength > lengthSpread) {
				return npos64;
			}
			const uint64_t hash{ computeHash(data, length) };
			const uint64_t bucketStart{ ((hash >> 8) & groupMask) * simdBytesPerRegister };
			const uint64_t matches{ bucket_search::matchMask(hashData<value_type>.controlBytes.data() + bucketStart, static_cast<uint8_t>(hash)) };
			return matches != 0 ? hashData<value_type>.indices[bucketStart + bucket_search::firstOffset(matches)] : npos64;
		}
	};

}

namespace jsonifier {

	template<string_literal... keys> struct hash_map {
		static constexpr uint64_t keyCount{ sizeof...(keys) };
		static_assert(keyCount < internal::npos8, "hash_map supports at most 254 keys.");

		static consteval std::string_view strategyName() {
			return internal::strategyNameOf<value_type>();
		}

		JSONIFIER_INLINE static uint64_t findIndex(std::string_view key) noexcept {
			const uint64_t candidate{ internal::candidate_finder<value_type>::impl(key.data(), key.size()) };
			if constexpr (keyCount > 0) {
				const uint64_t index{ candidate < keyCount ? candidate : keyCount };
				if (internal::key_verifier<value_type>::impl(index, key.data(), key.size())) {
					return index;
				}
			}
			return keyCount;
		}

	  private:
		using value_type = internal::key_list<keys...>;
	};

}
