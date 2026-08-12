/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/utilities/string_utils.hpp
 */
#pragma once

#include <jsonifier-incl/simd/utf8_validation.hpp>
#include <jsonifier-incl/containers/allocator.hpp>
#include <jsonifier-incl/utilities/hash_map.hpp>
#include <jsonifier-incl/utilities/str_to_d.hpp>
#include <jsonifier-incl/utilities/error.hpp>
#include <jsonifier-incl/utilities/simd.hpp>

namespace jsonifier::internal {

	template<typename = void> struct digit_tables {
		alignas(64) static constexpr uint32_t digitToVal32[]{ 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0x0u, 0x1u, 0x2u, 0x3u, 0x4u, 0x5u, 0x6u, 0x7u, 0x8u, 0x9u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xAu, 0xBu, 0xCu,
			0xDu, 0xEu, 0xFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xAu, 0xBu, 0xCu, 0xDu, 0xEu, 0xFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0u, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u, 0x90u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA0u, 0xB0u, 0xC0u, 0xD0u, 0xE0u, 0xF0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA0u, 0xB0u, 0xC0u, 0xD0u, 0xE0u, 0xF0u, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0u, 0x100u, 0x200u,
			0x300u, 0x400u, 0x500u, 0x600u, 0x700u, 0x800u, 0x900u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA00u, 0xB00u,
			0xC00u, 0xD00u, 0xE00u, 0xF00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA00u, 0xB00u, 0xC00u, 0xD00u, 0xE00u, 0xF00u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0u, 0x1000u, 0x2000u, 0x3000u, 0x4000u, 0x5000u, 0x6000u, 0x7000u, 0x8000u, 0x9000u,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA000u, 0xB000u, 0xC000u, 0xD000u, 0xE000u, 0xF000u, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xA000u,
			0xB000u, 0xC000u, 0xD000u, 0xE000u, 0xF000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
			0xFFFFFFFFu };
	};

	// Sampled from Simdjson library: https://github.com/simdjson/simdjson
	JSONIFIER_INLINE static uint32_t hexToU32NoCheck(string_view_ptr string1) noexcept {
		return digit_tables<>::digitToVal32[630ull + static_cast<uint64_t>(string1[0])] | digit_tables<>::digitToVal32[420ull + static_cast<uint64_t>(string1[1])] |
			digit_tables<>::digitToVal32[210ull + static_cast<uint64_t>(string1[2])] | digit_tables<>::digitToVal32[0ull + static_cast<uint64_t>(string1[3])];
	}

	// Sampled from Simdjson library: https://github.com/simdjson/simdjson
	JSONIFIER_INLINE static uint32_t codePointToUtf8(uint32_t cp, string_buffer_ptr c) noexcept {
		if (cp <= 0x7F) {
			c[0] = static_cast<char>(cp);
			return 1;
		}
		if (cp <= 0x7FF) {
			c[0] = static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
			c[1] = static_cast<char>(0x80 | (cp & 0x3F));
			return 2;
		}
		if (cp <= 0xFFFF) {
			c[0] = static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
			c[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			c[2] = static_cast<char>(0x80 | (cp & 0x3F));
			return 3;
		}
		if (cp <= 0x10FFFF) {
			c[0] = static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
			c[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			c[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			c[3] = static_cast<char>(0x80 | (cp & 0x3F));
			return 4;
		}
		return 0;
	}

	// Sampled from Simdjson library: https://github.com/simdjson/simdjson
	template<typename basic_iterator01, typename basic_iterator02>
	JSONIFIER_INLINE static bool handleUnicodeCodePoint(basic_iterator01& srcPtr, basic_iterator02& dstPtr, basic_iterator01 srcEnd) noexcept {
		static constexpr uint8_t bs{ '\\' };
		static constexpr uint8_t u{ 'u' };

		if ((srcPtr + 6) > srcEnd) [[unlikely]] {
			return false;
		}

		uint32_t codePoint = hexToU32NoCheck(srcPtr + 2);
		srcPtr += 6;
		if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
			if (codePoint >= 0xDC00) {
				return false;
			}
			if ((srcPtr + 6) > srcEnd) [[unlikely]] {
				return false;
			}
			if (((srcPtr[0] << 8) | srcPtr[1]) != ((bs << 8) | u)) {
				return false;
			}
			uint32_t lowSurrogate = hexToU32NoCheck(srcPtr + 2);
			uint32_t lowBit		  = lowSurrogate - 0xDC00;
			if (lowBit >> 10) {
				return false;
			}
			codePoint = (((codePoint - 0xD800) << 10) | lowBit) + 0x10000;
			srcPtr += 6;
		}
		const uint64_t offset = codePointToUtf8(codePoint, dstPtr);
		dstPtr += offset;
		return offset > 0;
	}

	template<char threshold, typename simd_type> [[maybe_unused]] JSONIFIER_INLINE static bool hasByteLessThanValue(const simd_type values) noexcept {
		return simd::opCmpLt(values, simd::gatherValue<simd_type>(threshold)) != 0;
	}

	template<auto maskValue, typename simd_type, typename integer_type>
	[[maybe_unused]] JSONIFIER_INLINE static integer_type findParse(simd_type simdValue, simd_type simdValues01, simd_type simdValues02) noexcept {
		auto result01 = simd::opOr(simd::opCmpEqRaw(simdValue, simdValues02), simd::opCmpEqRaw(simdValue, simdValues01));
		return static_cast<integer_type>(simd::postCmpTzcnt(static_cast<integer_type>(simd::opBitMaskRaw(result01))));
	}

	template<uint_types simd_type, uint_types integer_type> [[maybe_unused]] JSONIFIER_INLINE static integer_type findParse(simd_type& simdValue) noexcept {
		static constexpr integer_type mask{ repeatByte<0b01111111, integer_type>() };
		static constexpr integer_type hiBits{ repeatByte<0b10000000, integer_type>() };
		static constexpr integer_type quoteBits{ repeatByte<'"', integer_type>() };
		static constexpr integer_type bsBits{ repeatByte<'\\', integer_type>() };
		const integer_type lo7	= simdValue & mask;
		const integer_type next = ~((((lo7 ^ quoteBits) + mask) & ((lo7 ^ bsBits) + mask)) | simdValue) & hiBits;
		return static_cast<integer_type>(simd::countrZero(next) >> 3u);
	}

	template<typename simd_type, typename integer_type>
	[[maybe_unused]] JSONIFIER_INLINE static integer_type findSerialize(simd_type simdValue, simd_type simdValues01, simd_type simdValues02, simd_type simdValues03) noexcept {
		auto result01 = simd::opOr(simd::opOr(simd::opCmpLtRaw(simdValue, simdValues03), simd::opCmpEqRaw(simdValue, simdValues02)), simd::opCmpEqRaw(simdValue, simdValues01));
		return static_cast<integer_type>(simd::postCmpTzcnt(static_cast<integer_type>(simd::opBitMaskRaw(result01))));
	}

	template<uint_types simd_type, uint_types integer_type> [[maybe_unused]] JSONIFIER_INLINE static integer_type findSerialize(simd_type& simdValue) noexcept {
		static constexpr integer_type mask{ repeatByte<0b01111111, integer_type>() };
		static constexpr integer_type less32Bits{ repeatByte<0b01100000, integer_type>() };
		static constexpr integer_type hiBits{ repeatByte<0b10000000, integer_type>() };
		static constexpr integer_type quoteBits{ repeatByte<'"', integer_type>() };
		static constexpr integer_type bsBits{ repeatByte<'\\', integer_type>() };
		const integer_type lo7	= simdValue & mask;
		const integer_type next = ~((((lo7 ^ quoteBits) + mask) & ((lo7 ^ bsBits) + mask) & ((simdValue & less32Bits) + mask)) | simdValue) & hiBits;
		return static_cast<integer_type>(simd::countrZero(next) >> 3u);
	}

	// Sampled from Stephen Berry and his library, Glaze library: https://github.com/StephenBerry/Glaze
	template<typename basic_iterator01> [[maybe_unused]] JSONIFIER_INLINE static void skipStringImpl(basic_iterator01& string1, uint64_t lengthNew) noexcept {
		if (static_cast<int64_t>(lengthNew) > 0) {
			const auto endIter = string1 + lengthNew;
			while (string1 < endIter) {
				auto* newIter = char_comparison<'"', base_t<decltype(*string1)>>::memchar(string1, lengthNew);
				if (newIter) {
					string1	  = newIter;
					lengthNew = static_cast<uint64_t>(endIter - string1);

					auto* prev = string1 - 1;
					while (*prev == '\\') {
						--prev;
					}
					if (static_cast<uint64_t>(string1 - prev) % 2) {
						break;
					}
					++string1;
				} else {
					break;
				}
			}
		}
	}

	alignas(64) static constexpr array<char, 256> escapeMap{ []() constexpr {
		array<char, 256> returnValues{};
		returnValues[static_cast<uint64_t>('"')]  = '\"';
		returnValues[static_cast<uint64_t>('\\')] = '\\';
		returnValues[static_cast<uint64_t>('/')]  = '/';
		returnValues[static_cast<uint64_t>('b')]  = '\b';
		returnValues[static_cast<uint64_t>('f')]  = '\f';
		returnValues[static_cast<uint64_t>('n')]  = '\n';
		returnValues[static_cast<uint64_t>('r')]  = '\r';
		returnValues[static_cast<uint64_t>('t')]  = '\t';
		return returnValues;
	}() };

	template<typename basic_iterator01, typename basic_iterator02>
	JSONIFIER_INLINE static basic_iterator02 unescapeImpl(basic_iterator01 string1Start, const basic_iterator01 string1End, basic_iterator02 string2) noexcept {
		char escapeChar;
		while (string1Start < string1End) {
			escapeChar = *string1Start;
			if (escapeChar == '\\') {
				escapeChar = string1Start[1];
				if (escapeChar == 'u') {
					if (!handleUnicodeCodePoint(string1Start, string2, string1End)) {
						return nullptr;
					}
					continue;
				}
				escapeChar = escapeMap[static_cast<uint8_t>(escapeChar)];
				if (escapeChar == 0) {
					return nullptr;
				}
				*string2 = escapeChar;
				++string2;
				string1Start += 2;
				continue;
			}
			*string2 = escapeChar;
			++string2;
			++string1Start;
		}
		return string2;
	}

	template<typename executor_type, typename integer_sequence> struct string_parse_executor;

	template<typename executor_type, uint64_t... indices> struct string_parse_executor<executor_type, integer_sequence<indices...>> {
		template<typename... arg_types> JSONIFIER_INLINE static bool impl(arg_types&&... args) noexcept {
			return ((executor_type::template impl<indices>(std::forward<arg_types>(args)...) != nullptr) && ...);
		}
	};

#if JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX512)
	static constexpr uint64_t start_index = 0;
#elif JSONIFIER_CHECK_FOR_INSTRUCTION(JSONIFIER_AVX2)
	static constexpr uint64_t start_index = 1;
#else
	static constexpr uint64_t start_index = 2;
#endif

	static constexpr uint64_t list_size = 3;

	template<uint64_t start, uint64_t end, uint64_t... indices> struct make_ascending_range_impl : make_ascending_range_impl<start + 1, end, indices..., start> {};
	template<uint64_t end, uint64_t... indices> struct make_ascending_range_impl<end, end, indices...> {
		using type = integer_sequence<indices...>;
	};
	template<uint64_t start, uint64_t end> using make_ascending_range = typename make_ascending_range_impl<start, end>::type;

	template<parse_options options> struct string_scanner;

	template<parse_options options> struct string_scanner {
		struct scan_result {
			uint64_t firstEscape{};
			uint64_t rawLength{};
			bool valid{};
		};

		static constexpr uint64_t npos{ std::numeric_limits<uint64_t>::max() };

		template<typename basic_iterator01> JSONIFIER_INLINE static scan_result impl(basic_iterator01 string1Start, const basic_iterator01 string1End) noexcept {
			using simd_list_local					 = type_list_element_t<list_size - 1, simd::avx_integer_list>;
			using integer_type						 = typename simd_list_local::integer_type;
			using simd_type							 = typename simd_list_local::type::type;
			static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
			static constexpr integer_type mask		 = simd_list_local::mask;

			const auto stringStart	= string1Start;
			const auto stringEndNew = string1End - bytesProcessed;
			uint64_t firstEscape{ npos };
			char escapeChar;
			simd_type simdValue;
			integer_type nextBackslashOrQuote;

			const simd_type simdValues00 = simd::gatherValue<simd_type>('\\');
			const simd_type simdValues01 = simd::gatherValue<simd_type>('"');

			while (string1Start < stringEndNew) {
				simdValue			 = simd::gatherValuesU<simd_type>(string1Start);
				nextBackslashOrQuote = findParse<mask, simd_type, integer_type>(simdValue, simdValues00, simdValues01);
				if (nextBackslashOrQuote != mask) [[likely]] {
					escapeChar = string1Start[nextBackslashOrQuote];
					if (escapeChar == '"') {
						return { static_cast<uint64_t>(string1Start - stringStart) + nextBackslashOrQuote, firstEscape, true };
					}
					if (firstEscape == npos) {
						firstEscape = static_cast<uint64_t>(string1Start - stringStart) + nextBackslashOrQuote;
					}
					string1Start += nextBackslashOrQuote + 2ull;
				} else if (hasByteLessThanValue<32>(simdValue)) [[unlikely]] {
					return {};
				} else {
					string1Start += bytesProcessed;
				}
			}
			return shortImpl(stringStart, string1Start, string1End, firstEscape);
		}

		template<typename basic_iterator01> JSONIFIER_INLINE static scan_result shortImpl(const basic_iterator01 stringStart, basic_iterator01 string1Start,
			const basic_iterator01 string1End, uint64_t firstEscape) noexcept {
			char escapeChar;
			while (string1Start < string1End) {
				escapeChar = *string1Start;
				if (escapeChar == '"') {
					return { static_cast<uint64_t>(string1Start - stringStart), firstEscape, true };
				}
				if (escapeChar == '\\') {
					if (firstEscape == npos) {
						firstEscape = static_cast<uint64_t>(string1Start - stringStart);
					}
					string1Start += 2;
					continue;
				}
				if (static_cast<uint8_t>(escapeChar) < 32) [[unlikely]] {
					return {};
				}
				++string1Start;
			}
			return {};
		}
	};

	template<parse_options options>
		requires(options.validateUtf8)
	struct string_scanner<options> {
		struct scan_result {
			uint64_t outLength{ std::numeric_limits<uint64_t>::max() };
			uint64_t rawLength{};
		};

		static constexpr uint64_t npos{ std::numeric_limits<uint64_t>::max() };

		struct scan_state {
			utf8_validation_state validationState{};
			uint64_t rawLength{};
			uint64_t outLength{};
			bool complete{};
		};

		template<typename basic_iterator01, typename basic_iterator02>
		JSONIFIER_INLINE static bool handleEscape(basic_iterator01& srcPtr, const basic_iterator01 srcEnd, basic_iterator02& dstPtr) noexcept {
			const uint8_t escapeChar = static_cast<uint8_t>(srcPtr[1]);
			if (escapeChar == 'u') {
				return handleUnicodeCodePoint(srcPtr, dstPtr, srcEnd);
			}
			const uint8_t decoded = static_cast<uint8_t>(escapeMap[escapeChar]);
			if (decoded == 0u) [[unlikely]] {
				return false;
			}
			*dstPtr = static_cast<char>(decoded);
			++dstPtr;
			srcPtr += 2;
			return true;
		}

		struct string_scan_step {
			template<uint64_t index, typename basic_iterator01, typename basic_iterator02> JSONIFIER_INLINE static basic_iterator01 impl(basic_iterator01& string1Start,
				const basic_iterator01 string1End, basic_iterator02& string2, const basic_iterator01 stringStart, const basic_iterator02 outStart, scan_state& scanState) noexcept {
				using simd_list_local					 = type_list_element_t<index, simd::avx_integer_list>;
				using integer_type						 = typename simd_list_local::integer_type;
				static constexpr auto simd_type			 = simd_list_local::type::simd_type;
				using simd_type_local					 = typename simd_type_wrapper<simd_type>::type;
				static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
				if (scanState.complete || scanState.outLength == std::numeric_limits<uint64_t>::max()) {
					return nullptr;
				}
				utf8_register_validator<simd_type_wrapper<simd_type>> validator{ scanState.validationState };
				const simd_type_local simdValues00 = simd::gatherValue<simd_type_local>('\\');
				const simd_type_local simdValues01 = simd::gatherValue<simd_type_local>('"');
				const simd_type_local simdValues02 = simd::gatherValue<simd_type_local>(static_cast<char>(32));
				const auto stringEndNew			   = string1End - bytesProcessed;
				while (string1Start < stringEndNew) {
					const auto registerStart		= string1Start;
					const simd_type_local simdValue = simd::gatherValuesU<simd_type_local>(string1Start);
					simd::storeU(simdValue, string2);
					const integer_type delimiters = static_cast<integer_type>(simd::opBitMask(
						simd::opOr(simd::opOr(simd::opCmpEqRaw(simdValue, simdValues00), simd::opCmpEqRaw(simdValue, simdValues01)), simd::opCmpLtRaw(simdValue, simdValues02))));
					if (delimiters == static_cast<integer_type>(0)) {
						validator.checkRegister(simdValue);
						string1Start += bytesProcessed;
						string2 += bytesProcessed;
						continue;
					}
					const uint64_t offset	= static_cast<uint64_t>(simd::countrZero(delimiters));
					const uint8_t foundChar = static_cast<uint8_t>(string1Start[offset]);
					if (foundChar < 32) [[unlikely]] {
						scanState.outLength = std::numeric_limits<uint64_t>::max();
						scanState.complete	= true;
						return nullptr;
					}
					validator.checkPartial(registerStart, offset);
					string1Start += offset;
					string2 += offset;
					if (validator.errors()) [[unlikely]] {
						scanState.outLength = std::numeric_limits<uint64_t>::max();
						scanState.complete	= true;
						return nullptr;
					}
					if (foundChar == '"') {
						scanState.rawLength = static_cast<uint64_t>(string1Start - stringStart);
						scanState.outLength = static_cast<uint64_t>(string2 - outStart);
						scanState.complete	= true;
						return nullptr;
					}
					if (string1Start + 1 >= string1End) [[unlikely]] {
						scanState.outLength = std::numeric_limits<uint64_t>::max();
						scanState.complete	= true;
						return nullptr;
					}
					if (!handleEscape(string1Start, string1End, string2)) [[unlikely]] {
						scanState.outLength = std::numeric_limits<uint64_t>::max();
						scanState.complete	= true;
						return nullptr;
					}
					validator.reset();
				}
				validator.flush();

				if constexpr (index == 2) {
					auto validateFrom = string1Start;

					while (string1Start < string1End) {
						const uint8_t currentChar = static_cast<uint8_t>(*string1Start);

						if (currentChar == '"') {
							validator.checkPartial(validateFrom, static_cast<uint64_t>(string1Start - validateFrom));
							if (validator.errors()) [[unlikely]] {
								break;
							}
							validator.flush();
							scanState.rawLength = static_cast<uint64_t>(string1Start - stringStart);
							scanState.outLength = static_cast<uint64_t>(string2 - outStart);
							scanState.complete	= true;
							return nullptr;
						}

						if (currentChar == '\\') {
							validator.checkPartial(validateFrom, static_cast<uint64_t>(string1Start - validateFrom));
							if (validator.errors()) [[unlikely]] {
								break;
							}
							if (string1Start + 1 >= string1End) [[unlikely]] {
								break;
							}
							if (!handleEscape(string1Start, string1End, string2)) [[unlikely]] {
								break;
							}
							validator.reset();
							validateFrom = string1Start;
							continue;
						}

						if (currentChar < 32) [[unlikely]] {
							break;
						}

						*string2 = static_cast<char>(currentChar);
						++string2;
						++string1Start;
					}

					validator.flush();
					scanState.outLength = std::numeric_limits<uint64_t>::max();
					scanState.complete	= true;
				}

				return string1Start;
			}
		};

		template<typename basic_iterator01, typename basic_iterator02>
		JSONIFIER_INLINE static scan_result impl(basic_iterator01 string1Start, const basic_iterator01 string1End, basic_iterator02 string2) noexcept {
			const auto stringStart = string1Start;
			const auto outStart	   = string2;
			scan_state scanState{};

			string_parse_executor<string_scan_step, make_ascending_range<start_index, list_size>>::impl(string1Start, string1End, string2, stringStart, outStart, scanState);

			if (scanState.outLength == std::numeric_limits<uint64_t>::max()) [[unlikely]] {
				return {};
			}
			return { scanState.outLength, scanState.rawLength };
		}
	};

	alignas(64) static constexpr array<string_view_ptr, 256> escapeTable{ { "", R"(\u0001)", R"(\u0002)", R"(\u0003)", R"(\u0004)", R"(\u0005)", R"(\u0006)", R"(\a)", R"(\b)",
		R"(\t)", R"(\n)", R"(\v)", R"(\f)", R"(\r)", R"(\u000E)", R"(\u000F)", R"(\u0010)", R"(\u0011)", R"(\u0012)", R"(\u0013)", R"(\u0014)", R"(\u0015)", R"(\u0016)",
		R"(\u0017)", R"(\u0018)", R"(\u0019)", R"(\u001A)", R"(\u001B)", R"(\u001C)", R"(\u001D)", R"(\u001E)", R"(\u001F)", "", "", R"(\")", "", "", "", "", "", "", "", "", "",
		"", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
		"", "", "", "", "", R"(\\)", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" } };

	alignas(64) static constexpr array<uint64_t, 256> escapeTableSizes{ []() constexpr {
		array<uint64_t, 256> returnValues{};
		for (uint64_t x = 0; x < 256; ++x) {
			returnValues[x] = strLen(escapeTable[x]);
		}
		return returnValues;
	}() };

	template<serialize_options options> struct string_serializer {
		struct string_serialize_step {
			template<uint64_t index, typename basic_iterator01, typename basic_iterator02>
			JSONIFIER_INLINE static auto* impl(basic_iterator01& string1Start, const basic_iterator01 string1End, basic_iterator02& string2) noexcept {
				using simd_list_local					 = type_list_element_t<index, simd::avx_integer_list>;
				using integer_type						 = typename simd_list_local::integer_type;
				using simd_type							 = typename simd_list_local::type::type;
				static constexpr uint64_t bytesProcessed = simd_list_local::bytesProcessed;
				static constexpr integer_type mask		 = simd_list_local::mask;

				simd_type simdValue;
				uint64_t nextSize;
				uint8_t nextChar;
				integer_type nextEscapeable;
				string_view_ptr escapeChar;
				const auto stringEndNew = string1End - bytesProcessed;

				const simd_type simdValues01 = simd::gatherValue<simd_type>('"');
				const simd_type simdValues02 = simd::gatherValue<simd_type>('\\');
				const simd_type simdValues03 = simd::gatherValue<simd_type>(static_cast<char>(32));
				while (string1Start < stringEndNew) {
					simdValue = simd::gatherValuesU<simd_type>(string1Start);
					simd::storeU(simdValue, string2);
					nextEscapeable = findSerialize<simd_type, integer_type>(simdValue, simdValues01, simdValues02, simdValues03);
					if (nextEscapeable != mask) [[likely]] {
						nextChar   = static_cast<uint8_t>(string1Start[nextEscapeable]);
						nextSize   = escapeTableSizes[nextChar];
						escapeChar = escapeTable[nextChar];
						string2 += nextEscapeable;
						string1Start += nextEscapeable;
						memcpy_wrapper(string2, escapeChar, nextSize);
						string2 += nextSize;
						++string1Start;
					} else {
						string2 += bytesProcessed;
						string1Start += bytesProcessed;
					}
				}
				return string1Start;
			}
		};

		template<typename basic_iterator01, typename basic_iterator02>
		JSONIFIER_INLINE static basic_iterator02 shortImpl(basic_iterator01& string1Start, const basic_iterator01 string1End, basic_iterator02 string2) noexcept {
			string_view_ptr escapeChar;
			uint8_t nextChar;
			uint64_t escapeSize;
			for (; string1Start < string1End; ++string1Start) {
				nextChar   = static_cast<uint8_t>(*string1Start);
				escapeSize = escapeTableSizes[nextChar];
				if (escapeSize > 0) {
					escapeChar = escapeTable[nextChar];
					memcpy_wrapper(string2, escapeChar, escapeSize);
					string2 += escapeSize;
				} else {
					*string2 = *string1Start;
					++string2;
				}
			}
			return string2;
		}

		template<typename basic_iterator01, typename basic_iterator02>
		JSONIFIER_INLINE static basic_iterator02 impl(basic_iterator01 string1Start, basic_iterator02 string2, uint64_t lengthNew) noexcept {
			const basic_iterator01 string1End = string1Start + lengthNew;
			string_parse_executor<string_serialize_step, make_ascending_range<start_index, list_size>>::impl(string1Start, string1End, string2);
			return shortImpl(string1Start, string1End, string2);
		}
	};

	template<string_literal string> static consteval convert_length_to_int_t<string.size()> getStringAsInt() noexcept {
		string_view_ptr stringNew = string.data();
		convert_length_to_int_t<string.size()> returnValue{};
		for (uint64_t x = 0; x < string.size(); ++x) {
			returnValue |= static_cast<convert_length_to_int_t<string.size()>>(stringNew[x]) << x * 8;
		}
		if constexpr (std::endian::native == std::endian::big) {
			returnValue = byteswap(returnValue);
		}
		return returnValue;
	}

	template<string_literal stringNew> JSONIFIER_INLINE static bool compareStringAsInt(string_view_ptr src) {
		using integer_type = convert_length_to_int_t<stringNew.size()>;
		static constexpr auto string{ stringNew };
		static_assert(stringNew.size() == 4, "Sorry, but please only use a string with a length of 4 in this function!");
		alignas(64) static constexpr auto stringInt{ getStringAsInt<string>() };
		alignas(64) integer_type sourceVal;
		pow2_memcpy_wrapper<string.size()>(&sourceVal, src);
		return !static_cast<bool>(sourceVal ^ stringInt);
	}

	JSONIFIER_INLINE static bool validateBool(string_view_ptr context) noexcept {
		if (compareStringAsInt<"true">(context)) {
			return true;
		} else if (compareStringAsInt<"fals">(context) && context[4] == 'e') {
			return true;
		}
		return false;
	}

	JSONIFIER_INLINE static bool validateNull(string_view_ptr context) noexcept {
		if (compareStringAsInt<"null">(context)) [[likely]] {
			return true;
		} else {
			return false;
		}
	}

}// namespace internal
