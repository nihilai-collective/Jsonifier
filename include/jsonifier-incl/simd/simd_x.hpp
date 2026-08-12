// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/simd_x.hpp
#pragma once

#include <jsonifier-incl/core/config.hpp>

namespace jsonifier::simd {

	struct simd_x {
	  public:
		static constexpr uint64_t sixtyFourPer{ static_cast<uint64_t>(16) / sizeof(uint64_t) };
		static constexpr uint64_t thirtyTwoPer{ static_cast<uint64_t>(16) / sizeof(uint32_t) };
		static constexpr uint64_t sixteenPer{ static_cast<uint64_t>(16) / sizeof(uint16_t) };
		static constexpr uint64_t eightPer{ static_cast<uint64_t>(16) / sizeof(uint8_t) };

		union alignas(16) storage_type {
			uint64_t xUint64[sixtyFourPer];
			uint32_t xUint32[thirtyTwoPer];
			uint16_t xUint16[sixteenPer];
			uint8_t xUint8[eightPer];
		};

		storage_type values;

		simd_x() noexcept						  = default;
		simd_x(const simd_x&) noexcept			  = default;
		simd_x& operator=(const simd_x&) noexcept = default;
	};

}
