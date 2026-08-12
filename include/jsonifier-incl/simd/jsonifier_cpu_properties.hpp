// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// include/jsonifier-incl/simd/jsonifier_cpu_properties.hpp
#pragma once

#include <jsonifier-incl/core/defines.hpp>
#include <cstdint>

namespace jsonifier::internal {

	struct alignas(64) uint64_aligner {
		alignas(64) uint64_t value{};
		JSONIFIER_INLINE consteval operator const uint64_t&() const {
			return value;
		}
	};

	enum class cpu_property_types : uint64_t {
		alignment,
		arg_alignment,
	};

	struct cpu_properties {
	  public:
		static constexpr uint64_aligner values[]{ { 32ULL }, { 64ULL } };

		JSONIFIER_INLINE static consteval const uint64_t& get_value(cpu_property_types index) {
			return values[static_cast<uint64_t>(index)].value;
		}
	};

}
