// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/utilities/hash_map.hpp
#pragma once

#include <jsonifier-incl/core/core.hpp>
#include <jsonifier-incl/utilities/hash.hpp>
#include <jsonifier-incl/utilities/string_view.hpp>
#include <jsonifier-incl/containers/array.hpp>
#include <jsonifier-incl/utilities/reflection.hpp>
#include <jsonifier-incl/utilities/get_enum_name.hpp>
#include <jsonifier-incl/containers/tuple.hpp>

namespace std {

	template<jsonifier::concepts::string_t string_type> struct hash<string_type> : public std::hash<std::string_view> {
		uint64_t operator()(const string_type& stringNew) const noexcept {
			return std::hash<std::string_view>::operator()(std::string_view{ stringNew });
		}
	};
}

namespace jsonifier::internal {

	template<typename value_type, uint64_t size> std::ostream& operator<<(std::ostream& os, const array<value_type, size>& values) {
		os << "[";
		for (uint64_t x = 0; x < size; ++x) {
			os << values[x];
			if (x < size - 1) {
				os << ",";
			}
		}
		os << "]";
		return os;
	}

	static constexpr uint64_t npos64{ ~0ull };
	static constexpr uint8_t npos8{ 0xFFu };

	static constexpr bool contains(const uint8_t* hashDataNew, uint8_t byteToCheckFor, uint64_t size) noexcept {
		for (uint64_t x = 0; x < size; ++x) {
			if (hashDataNew[x] == byteToCheckFor) {
				return true;
			}
		}
		return false;
	}

	struct byte_set {
		uint64_t bits[4]{};

		constexpr void clear() noexcept {
			bits[0] = 0ull;
			bits[1] = 0ull;
			bits[2] = 0ull;
			bits[3] = 0ull;
		}

		constexpr bool testAndSet(uint8_t value) noexcept {
			const uint64_t mask{ 1ull << (value & 63u) };
			uint64_t& word{ bits[value >> 6u] };
			const bool wasSet{ (word & mask) != 0ull };
			word |= mask;
			return wasSet;
		}
	};

	template<uint64_t length> using map_simd_t = typename jsonifier::internal::conditional_t<length >= 64 && simdBytesPerRegister >= 64, avx_type_wrapper<avx_type::m512>,
		jsonifier::internal::conditional_t<length >= 32 && simdBytesPerRegister >= 32, avx_type_wrapper<avx_type::m256>, avx_type_wrapper<avx_type::m128>>>::type;

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

	static constexpr uint64_t setSimdWidth(uint64_t length) noexcept {
		return length >= 64ull && simdBytesPerRegister >= 64ull ? 64ull : length >= 32ull && simdBytesPerRegister >= 32ull ? 32ull : 16ull;
	}

	struct key_stats_t {
		uint64_t minLength{ npos64 };
		uint64_t uniqueIndex{};
		uint64_t maxLength{};
	};

	static constexpr uint64_t findUniqueColumnIndex(const tuple_references& tupleRefsRaw, uint64_t maxIndex, uint64_t startingIndex = 0) noexcept {
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

	static constexpr uint64_t nextPowerOfTwo(uint64_t value) noexcept {
		uint64_t result = 16;
		while (result < value) {
			result <<= 1;
		}
		return result;
	}

	static constexpr uint64_t maxStorageSize{ 2048 };

	static constexpr uint64_t storageSizeFor(uint64_t keyCount) noexcept {
		const uint64_t target{ nextPowerOfTwo(keyCount * 16ull) };
		return target < 256ull ? 256ull : (target > maxStorageSize ? maxStorageSize : target);
	}

	template<uint64_t storageSizeNew> struct hash_map_construction_data {
		static constexpr uint64_t storageSize{ storageSizeNew };
		array<uint16_t, storageSizeNew / setSimdWidth(storageSizeNew)> bucketSizes{};
		alignas(64) array<uint8_t, storageSizeNew + 1ULL> controlBytes{};
		array<uint8_t, 256ULL> uniqueIndices{};
		array<uint16_t, storageSizeNew + 1ULL> indices{};
		uint64_t bucketSize{ setSimdWidth(storageSizeNew) };
		uint64_t numGroups{ storageSizeNew / bucketSize };
		ct_key_hasher hasher{};
		hash_map_types type{};
		uint64_t uniqueIndex{};
		char firstChar{};

		constexpr hash_map_construction_data() noexcept = default;
	};

	struct empty_data {
		template<uint64_t storageSizeNew> constexpr empty_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	struct single_element_data {
		static constexpr uint64_t storageSize{ 1 };
		template<uint64_t storageSizeNew> constexpr single_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	struct double_element_data {
		static constexpr uint64_t storageSize{ 2 };
		template<uint64_t storageSizeNew> constexpr double_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct triple_element_data {
		static constexpr uint64_t storageSize{ 3 };
		template<uint64_t storageSizeNew> constexpr triple_element_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndex{ newData.uniqueIndex }, type{ newData.type }, firstChar{ newData.firstChar }, seed{ newData.hasher.seed } {
		}
		uint64_t uniqueIndex{};
		hash_map_types type{};
		char firstChar{};
		uint64_t seed{};
	};

	struct single_byte_data {
		static constexpr uint64_t storageSize{ 256 };
		template<uint64_t storageSizeNew> constexpr single_byte_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndices{ newData.uniqueIndices }, uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		array<uint8_t, 256ULL> uniqueIndices{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct first_byte_and_unique_index_data {
		static constexpr uint64_t storageSize{ 256 };
		template<uint64_t storageSizeNew> constexpr first_byte_and_unique_index_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept : type{ newData.type } {
		}
		hash_map_types type{};
	};

	template<uint64_t storageSizeNew> struct unique_byte_and_length_data {
		static constexpr uint64_t storageSize{ storageSizeNew };
		constexpr unique_byte_and_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: indices{ newData.indices }, uniqueIndex{ newData.uniqueIndex }, type{ newData.type } {
		}
		array<uint16_t, storageSizeNew + 1ULL> indices{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
	};

	struct unique_per_length_data {
		static constexpr uint64_t storageSize{ 256 };
		template<uint64_t storageSizeNew> constexpr unique_per_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: uniqueIndices{ newData.uniqueIndices }, type{ newData.type } {
		}
		array<uint8_t, 256ULL> uniqueIndices{};
		hash_map_types type{};
	};

	template<uint64_t storageSizeNew> struct simd_full_length_data {
		static constexpr uint64_t storageSize{ storageSizeNew };
		constexpr simd_full_length_data(const hash_map_construction_data<storageSizeNew>& newData) noexcept
			: controlBytes{ newData.controlBytes }, bucketSize{ newData.bucketSize }, numGroups{ newData.numGroups }, indices{ newData.indices },
			  uniqueIndex{ newData.uniqueIndex }, type{ newData.type }, seed{ newData.hasher.seed } {
		}
		alignas(64) array<uint8_t, storageSizeNew + 1ULL> controlBytes{};
		char padding01[simdBytesPerRegister - ((storageSizeNew + 1) % 8)]{};
		uint64_t bucketSize{ setSimdWidth(storageSizeNew) };
		uint64_t numGroups{ storageSizeNew / bucketSize };
		array<uint16_t, storageSizeNew + 1ULL> indices{};
		uint64_t uniqueIndex{};
		hash_map_types type{};
		uint64_t seed{};
	};

	struct string_lengths : public tuple_references {
		uint64_t length{};
	};

	static constexpr uint64_t countUniqueLengths(const tuple_references& sortedByLength) noexcept {
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

	template<uint64_t stringLengthCount> static constexpr array<string_lengths, stringLengthCount> collectLengths(const tuple_references& sortedByLength) noexcept {
		array<string_lengths, stringLengthCount> valuesNew{};
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
		array<uint8_t, 256> uniqueIndexByFirstByte{};
		bool valid{};
	};

	static constexpr first_byte_columns collectFirstByteColumns(const tuple_references& sortedByFirstByte) noexcept {
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

	template<typename value_type> static constexpr auto firstByteColumns{ collectFirstByteColumns(tupleReferencesByFirstByte<value_type>) };

	struct first_byte_arrays {
		array<uint8_t, 256> uniqueIndexByFirstByte{};
		array<uint8_t, 256 * 256> indexByFirstByteAndChar{};
	};

	static constexpr first_byte_arrays buildFirstByteArrays(const tuple_references& sortedByFirstByte, const first_byte_columns& columns) noexcept {
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

	static constexpr key_stats_t keyStatsImpl(const tuple_references& tupleRefsRaw) noexcept {
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

	template<typename value_type> static constexpr auto keyStatsVal = keyStatsImpl(tupleReferences<value_type>);

	template<typename value_type, uint64_t storageSize>
	static constexpr void collectSimdFullLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
		constexpr uint16_t indexSentinel{ static_cast<uint16_t>(storageSize) };
		bool collided{ true };
		for (uint64_t w = keyStatsVal<value_type>.minLength; w <= keyStatsVal<value_type>.maxLength; ++w) {
			returnValues.uniqueIndex = w;
			for (uint64_t x = 0; x < 16; ++x) {
				returnValues.bucketSizes.fill(0);
				returnValues.controlBytes.fill(npos8);
				returnValues.indices.fill(indexSentinel);
				returnValues.hasher.updateSeed();
				collided = false;
				for (uint64_t y = 0; y < pairsNew.count; ++y) {
					const auto& key			 = pairsNew.rootPtr[y].key;
					const auto keyLength	 = returnValues.uniqueIndex > key.size() ? key.size() : returnValues.uniqueIndex;
					const auto hash			 = returnValues.hasher.hashKeyCt(key.data(), keyLength);
					const auto groupPos		 = (hash >> 8) % returnValues.numGroups;
					const auto ctrlByte		 = static_cast<uint8_t>(hash);
					const auto bucketStart	 = groupPos * returnValues.bucketSize;
					const auto bucketSizeNew = returnValues.bucketSizes[groupPos]++;
					const auto slot			 = bucketStart + bucketSizeNew;
					if (bucketSizeNew >= returnValues.bucketSize || returnValues.indices[slot] != indexSentinel ||
						contains(returnValues.controlBytes.data() + bucketStart, ctrlByte, returnValues.bucketSize)) {
						collided = true;
						break;
					}
					returnValues.controlBytes[slot] = ctrlByte;
					returnValues.indices[slot]		= pairsNew.rootPtr[y].oldIndex;
				}
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
	static constexpr void collectUniquePerLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
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
	static constexpr void collectUniqueByteAndLengthHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
		constexpr uint16_t indexSentinel{ static_cast<uint16_t>(storageSize) };
		bool collided{ true };
		while (returnValues.uniqueIndex < keyStatsVal<value_type>.minLength) {
			returnValues.indices.fill(indexSentinel);
			collided = false;
			for (uint64_t x = 0; x < pairsNew.count; ++x) {
				const auto& key = pairsNew.rootPtr[x].key;
				const auto hash = static_cast<uint64_t>(key[returnValues.uniqueIndex]) ^ static_cast<uint64_t>(key.size());
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
	static constexpr void collectFirstByteAndUniqueIndexHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
		if constexpr (keyStatsVal<value_type>.maxLength < 256 && firstByteColumns<value_type>.valid) {
			returnValues.type = hash_map_types::first_byte_and_unique_index;
		} else {
			collectUniqueByteAndLengthHashMapData<value_type>(returnValues, pairsNew);
		}
	}

	// Sampled from Stephen Berry and his library, Glaze library: https://github.com/StephenBerry/Glaze
	template<typename value_type, uint64_t storageSize>
	static constexpr void collectSingleByteHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
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
	static constexpr void collectTripleElementHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
		returnValues.uniqueIndex = keyStatsVal<value_type>.uniqueIndex;
		bool collided{ true };
		while (returnValues.uniqueIndex != npos64) {
			returnValues.firstChar = static_cast<char>(static_cast<uint8_t>(pairsNew.rootPtr[0].key[returnValues.uniqueIndex]));
			const auto mix1		   = static_cast<uint8_t>(pairsNew.rootPtr[1].key[returnValues.uniqueIndex]) ^ returnValues.firstChar;
			const auto mix2		   = static_cast<uint8_t>(pairsNew.rootPtr[2].key[returnValues.uniqueIndex]) ^ returnValues.firstChar;
			for (uint64_t x = 0; x < 4; ++x) {
				uint8_t hash1 = static_cast<uint8_t>((mix1 * static_cast<uint8_t>(returnValues.hasher.seed)) & 3);
				uint8_t hash2 = static_cast<uint8_t>((mix2 * static_cast<uint8_t>(returnValues.hasher.seed)) & 3);

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
	static constexpr void collectDoubleElementHashMapData(hash_map_construction_data<storageSize>& returnValues, const tuple_references& pairsNew) noexcept {
		returnValues.uniqueIndex = keyStatsVal<value_type>.uniqueIndex;
		bool collided{ true };
		while (returnValues.uniqueIndex != npos64) {
			if ((static_cast<uint64_t>(pairsNew.rootPtr[0].key[returnValues.uniqueIndex]) & 1ull) == 0ull &&
				(static_cast<uint64_t>(pairsNew.rootPtr[1].key[returnValues.uniqueIndex]) & 1ull) == 1ull) {
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

	template<typename value_type, uint64_t storageSize> static constexpr hash_map_construction_data<storageSize> collectMapConstructionDataImpl() noexcept {
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

	template<typename value_type, uint64_t storageSize> static constexpr auto mapConstructionDataAt = collectMapConstructionDataImpl<value_type, storageSize>();

	template<typename value_type> static constexpr const auto& selectMapConstructionData() noexcept {
		constexpr auto preferredSize = storageSizeFor(tupleReferences<value_type>.count);
		if constexpr (preferredSize < maxStorageSize && mapConstructionDataAt<value_type, preferredSize>.type == hash_map_types::unset) {
			return mapConstructionDataAt<value_type, maxStorageSize>;
		} else {
			return mapConstructionDataAt<value_type, preferredSize>;
		}
	}

	template<typename value_type> static constexpr const auto& mapConstructionData = selectMapConstructionData<value_type>();

	template<typename value_type> constexpr hash_map_types classifyMapType() noexcept {
		return mapConstructionData<value_type>.type;
	}

	template<typename value_type> constexpr decltype(auto) collectMapConstructionData() noexcept {
		constexpr auto& constructionData = mapConstructionData<value_type>;
		constexpr auto storageSize		 = jsonifier::internal::remove_cvref_t<decltype(constructionData)>::storageSize;
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
		} else if constexpr (constructionData.type == hash_map_types::simd_full_length) {
			return simd_full_length_data<storageSize>{ constructionData };
		}
	}

	template<uint64_t keyMaxLength>
	static constexpr array<uint8_t, (keyMaxLength + 1) * 256> generateMappingsForLengths(const tuple_references& keys, const array<uint8_t, 256>& uniqueIndices) noexcept {
		array<uint8_t, (keyMaxLength + 1) * 256> mappings{};
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

	struct hash_map_type_tracker {
		std::unordered_map<std::string, hash_map_types> types{};

		JSONIFIER_INLINE void addType(const std::string& typeName, hash_map_types hashMapType) {
			types[typeName] = hashMapType;
		}

		~hash_map_type_tracker() {
			for (auto& [key, value]: types) {
				std::cout << "Type: " << key << ", Hash Map Type: " << value << std::endl;
			}
		}
	};

#if !defined(NDEBUG)
	#if JSONIFIER_COMPILER_CLANG
		#pragma clang diagnostic push
		#pragma clang diagnostic ignored "-Wexit-time-destructors"
		#pragma clang diagnostic ignored "-Wglobal-constructors"
	#endif
	inline static hash_map_type_tracker hashMapTypeTracker{};
	#if JSONIFIER_COMPILER_CLANG
		#pragma clang diagnostic pop
	#endif
#endif

	template<typename value_type> static constexpr auto hashData = collectMapConstructionData<value_type>();

	template<typename value_type, typename iterator_newer> struct hash_map {
		static constexpr uint64_t lengthSpread{ keyStatsVal<value_type>.maxLength - keyStatsVal<value_type>.minLength };
		static constexpr uint64_t subAmount01{ lengthSpread >= simdBytesPerRegister ? keyStatsVal<value_type>.minLength : 0ull };
		static constexpr uint64_t subAmount02{ lengthSpread >= simdBytesPerRegister ? lengthSpread + 2ull : keyStatsVal<value_type>.maxLength + 2ull };

		template<typename char_type> JSONIFIER_INLINE static const char_type* boundedQuoteScan(char_type* iter, iterator_newer end) noexcept {
			const int64_t remaining = (end - iter) - static_cast<int64_t>(subAmount01);
			if (remaining <= 0) [[unlikely]] {
				return nullptr;
			}
			const uint64_t scanLen = (static_cast<uint64_t>(remaining) < subAmount02) ? static_cast<uint64_t>(remaining) : subAmount02;
			return char_comparison<'"', jsonifier::internal::remove_cvref_t<decltype(*iter)>>::memchar(iter + subAmount01, scanLen);
		}

		template<typename iterator_type01, typename iterator_type02>
		JSONIFIER_INLINE static bool checkForEnd(const iterator_type01& iterNew, const iterator_type02& endNew, const uint64_t distance) {
			return (iterNew + distance) < endNew;
		}

		JSONIFIER_INLINE static uint64_t findIndex(iterator_newer iter, iterator_newer end) noexcept {
#if !defined(NDEBUG)
			hashMapTypeTracker.addType(typeid(value_type).name(), hashData<value_type>.type);
#endif
			if constexpr (hashData<value_type>.type == hash_map_types::single_element) {
				return 0ull;
			} else if constexpr (hashData<value_type>.type == hash_map_types::double_element) {
				if (checkForEnd(iter, end, hashData<value_type>.uniqueIndex)) [[likely]] {
					return static_cast<uint64_t>(iter[static_cast<uint8_t>(hashData<value_type>.uniqueIndex)]) & 1u;
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::triple_element) {
				if (checkForEnd(iter, end, hashData<value_type>.uniqueIndex)) [[likely]] {
					return (static_cast<uint8_t>(iter[hashData<value_type>.uniqueIndex] ^ hashData<value_type>.firstChar) * hashData<value_type>.seed) & 3u;
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::single_byte) {
				if (checkForEnd(iter, end, hashData<value_type>.uniqueIndex)) [[likely]] {
					return hashData<value_type>.uniqueIndices[static_cast<uint8_t>(iter[static_cast<uint8_t>(hashData<value_type>.uniqueIndex)])];
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::first_byte_and_unique_index) {
				static constexpr auto arrays = buildFirstByteArrays(tupleReferencesByFirstByte<value_type>, firstByteColumns<value_type>);
				if (iter < end) [[likely]] {
					const uint8_t firstByte = static_cast<uint8_t>(iter[0]);
					const uint8_t uniqueIdx = arrays.uniqueIndexByFirstByte[firstByte];
					if (uniqueIdx != npos8 && checkForEnd(iter, end, uniqueIdx)) [[likely]] {
						const uint8_t keyChar = static_cast<uint8_t>(iter[uniqueIdx]);
						return arrays.indexByFirstByteAndChar[(static_cast<uint64_t>(firstByte) << 8) | static_cast<uint64_t>(keyChar)];
					}
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::unique_byte_and_length) {
				static_assert((hashData<value_type>.storageSize & (hashData<value_type>.storageSize - 1)) == 0, "storageSize must be power of two");
				static constexpr uint64_t storageMask = hashData<value_type>.storageSize - 1;
				const auto newPtr					  = boundedQuoteScan(iter, end);
				if (newPtr) [[likely]] {
					const uint64_t length = static_cast<uint64_t>(newPtr - iter);
					if (checkForEnd(iter, end, hashData<value_type>.uniqueIndex)) [[likely]] {
						const uint64_t combinedKey = static_cast<uint64_t>(iter[hashData<value_type>.uniqueIndex]) ^ length;
						return hashData<value_type>.indices[combinedKey & storageMask];
					}
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::unique_per_length) {
				static constexpr auto mappings =
					generateMappingsForLengths<keyStatsVal<value_type>.maxLength>(tupleReferencesByLength<value_type>, hashData<value_type>.uniqueIndices);
				const auto newPtr = boundedQuoteScan(iter, end);
				if (newPtr) [[likely]] {
					const uint64_t length		  = static_cast<uint64_t>(newPtr - iter);
					const uint64_t localUniqueIdx = hashData<value_type>.uniqueIndices[length];
					if (localUniqueIdx != npos8 && checkForEnd(iter, end, localUniqueIdx)) [[likely]] {
						return static_cast<uint64_t>(mappings[(length << 8) | static_cast<uint64_t>(iter[localUniqueIdx])]);
					}
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::simd_full_length) {
				static_assert((hashData<value_type>.storageSize & (hashData<value_type>.storageSize - 1)) == 0, "storageSize must be power of two");
				using simd_type = map_simd_t<hashData<value_type>.storageSize>;
				static constexpr rt_key_hasher<hashData<value_type>.seed> hasher{};
				static constexpr auto sizeMask{ hashData<value_type>.numGroups - 1u };
				static constexpr auto ctrlBytesPtr{ hashData<value_type>.controlBytes.data() };
				const auto newPtr = boundedQuoteScan(iter, end);
				if (newPtr) [[likely]] {
					uint64_t length = static_cast<uint64_t>(newPtr - iter);
					length			= (hashData<value_type>.uniqueIndex > length) ? length : hashData<value_type>.uniqueIndex;
					length			= length < static_cast<uint64_t>(end - iter) ? length : static_cast<uint64_t>(end - iter);
					if (checkForEnd(iter, end, length)) [[likely]] {
						const auto hash			   = hasher.hashKeyRt(iter, length);
						const uint64_t group	   = (hash >> 8) & (sizeMask);
						const uint64_t resultIndex = group * hashData<value_type>.bucketSize;
						const uint64_t matches{ simd::opCmpEq(simd::gatherValue<simd_type>(static_cast<uint8_t>(hash)),
							simd::gatherValues<simd_type>(ctrlBytesPtr + resultIndex)) };
						const uint64_t tz = simd::postCmpTzcnt(matches);
						if (matches != 0) [[likely]] {
							return hashData<value_type>.indices[resultIndex + tz];
						}
						return hashData<value_type>.storageSize;
					}
				}
				return hashData<value_type>.storageSize;
			} else if constexpr (hashData<value_type>.type == hash_map_types::empty) {
				return hashData<value_type>.storageSize;
			}
		}
	};
}
