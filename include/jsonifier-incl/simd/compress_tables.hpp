/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/simd/compress_tables.hpp
 */
#pragma once

#include <jsonifier-incl/simd/bit_ops.hpp>

namespace jsonifier::internal::simd {

	consteval array<uint64_t, 256ULL> genCompactLanes(const uint64_t laneBase) noexcept {
		array<uint64_t, 256ULL> returnValues{};
		for (uint64_t mask{ 0 }; mask < 256; ++mask) {
			uint64_t lanes{};
			uint64_t count{};
			for (uint64_t bit{ 0 }; bit < 8; ++bit) {
				if ((mask >> bit) & 1) {
					lanes |= (bit + laneBase) << (8 * count++);
				}
			}
			returnValues[mask] = lanes;
		}
		return returnValues;
	}

	consteval array<uint8_t[16ULL], 256ULL> genMergeLanes() noexcept {
		array<uint8_t[16ULL], 256ULL> returnValues{};
		for (uint64_t mask{ 0 }; mask < 256; ++mask) {
			uint64_t lowCount{};
			for (uint64_t bit{ 0 }; bit < 8; ++bit) {
				lowCount += (mask >> bit) & 1;
			}
			for (uint64_t x{ 0 }; x < 16; ++x) {
				returnValues[mask][x] = static_cast<uint8_t>(x < lowCount ? x : x - lowCount + 8);
			}
		}
		return returnValues;
	}

	struct compress_tables {
		array<uint8_t[16], 256ULL> merge;
		array<uint64_t, 256ULL> high;
		array<uint64_t, 256ULL> low;
	};

	alignas(64) inline constexpr compress_tables compressTables{ genMergeLanes(), genCompactLanes(8), genCompactLanes(0) };

	alignas(64) inline constexpr const uint64_t* compressTablesHigh{ compressTables.high.data() };
	alignas(64) inline constexpr const uint64_t* compressTablesLow{ compressTables.low.data() };
	alignas(64) inline constexpr const uint8_t* compressTablesMerge{ compressTables.merge[0] };

}
