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

#include <jsonifier-incl/simd/jsonifier_cpu_instructions.hpp>
#include <jsonifier-incl/simd/jsonifier_cpu_properties.hpp>
#include <source_location>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <optional>
#include <iomanip>
#include <variant>
#include <cstring>
#include <sstream>
#include <chrono>
#include <cfloat>
#include <atomic>
#include <vector>
#include <bit>

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_ANY_AVX)
	#include <immintrin.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_NEON)
	#include <arm_neon.h>
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_SVE2)
	#include <arm_sve.h>
#endif

#if JSONIFIER_PLATFORM_WINDOWS
	#include <windows.h>
#elif JSONIFIER_PLATFORM_LINUX || JSONIFIER_PLATFORM_MAC
	#include <sys/mman.h>
#endif

namespace jsonifier {

	struct serialize_options {
		uint64_t indentSize{ 3 };
		char indentChar{ ' ' };
		uint64_t indent{};
		bool prettify{};
	};

	struct parse_options {
		bool partialRead{};
		bool knownOrder{};
		bool minified{};
		bool validateUtf8{ true };
		bool nullTerminated{ true };
		uint64_t maxDepth{ 1024 };
	};

}
