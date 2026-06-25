/*
	MIT License

	Copyright (c) 2024 RealTimeChris

	Permission is hereby granted, free of charge, to any person obtaining a copy of this
	software and associated documentation files (the "Software"), to deal in the Software
	without restriction, including without limitation the rights to use, copy, modify, merge,
	publish, distribute, sublicense, and/or sell copies of the Software, and to permit
	persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or
	substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.
*/
/// https://github.com/nihilai-collective/Jsonifier
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
