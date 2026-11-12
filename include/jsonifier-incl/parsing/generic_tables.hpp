/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * include/jsonifier-incl/parsing/generic_tables.hpp
 */
#pragma once

#include <jsonifier-incl/parsing/generic_common_pre.hpp>

namespace jsonifier::generic {

	// Sampled from Dr. Lemire's library, simdjson: https://github.com/simdjson/simdjson
	consteval jsonifier::internal::array<int8_t, 256> genDepthDeltaTable() noexcept {
		jsonifier::internal::array<int8_t, 256> returnValues{};
		for (auto& entry: returnValues) {
			entry = 0;
		}
		returnValues[jsonifier::internal::uint_c_v<'{'>] = 1;
		returnValues[jsonifier::internal::uint_c_v<'['>] = 1;
		returnValues[jsonifier::internal::uint_c_v<'}'>] = -1;
		returnValues[jsonifier::internal::uint_c_v<']'>] = -1;
		return returnValues;
	}

	alignas(64) inline constexpr auto depthDeltaTableRaw{ genDepthDeltaTable() };

	inline constexpr const int8_t* __restrict depthDeltaTable{ depthDeltaTableRaw.data() };

	using array_type_table = internal::array<json_type, 256>;

	consteval array_type_table genTypeTable() noexcept {
		array_type_table returnValues{};
		for (auto& entry: returnValues) {
			entry = json_type::unset;
		}
		returnValues[jsonifier::internal::uint_c_v<'{'>] = json_type::object;
		returnValues[jsonifier::internal::uint_c_v<'['>] = json_type::array;
		returnValues[jsonifier::internal::uint_c_v<'"'>] = json_type::string;
		returnValues[jsonifier::internal::uint_c_v<'t'>] = json_type::boolean;
		returnValues[jsonifier::internal::uint_c_v<'f'>] = json_type::boolean;
		returnValues[jsonifier::internal::uint_c_v<'n'>] = json_type::null;
		returnValues[jsonifier::internal::uint_c_v<'-'>] = json_type::number;
		for (uint64_t x = jsonifier::internal::uint_c_v<'0'>; x <= jsonifier::internal::uint_c_v<'9'>; ++x) {
			returnValues[x] = json_type::number;
		}
		return returnValues;
	}

	alignas(64) inline constexpr auto typeTableRaw{ genTypeTable() };

	inline constexpr const json_type* __restrict typeTable{ typeTableRaw.data() };

	consteval jsonifier::internal::array<bool, 256> genSeperatorByteTable() noexcept {
		jsonifier::internal::array<bool, 256> returnValues{};
		for (auto& entry: returnValues) {
			entry = false;
		}
		returnValues[jsonifier::internal::uint_c_v<' '>]  = true;
		returnValues[jsonifier::internal::uint_c_v<'\n'>] = true;
		returnValues[jsonifier::internal::uint_c_v<'\r'>] = true;
		returnValues[jsonifier::internal::uint_c_v<'\t'>] = true;
		returnValues[jsonifier::internal::uint_c_v<','>]  = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto seperatorByteTableRaw{ genSeperatorByteTable() };

	inline constexpr const bool* __restrict seperatorByteTable{ seperatorByteTableRaw.data() };

	consteval internal::array<bool, 256ULL> genUtf8StringStopTable() noexcept {
		internal::array<bool, 256ULL> returnValues{};
		for (uint64_t x = 0; x < 0x20; ++x) {
			returnValues[x] = true;
		}
		returnValues[jsonifier::internal::uint_c_v<'"'>]  = true;
		returnValues[jsonifier::internal::uint_c_v<'\\'>] = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto utf8StringStopTableRaw{ genUtf8StringStopTable() };

	inline constexpr const bool* __restrict utf8StringStopTable{ utf8StringStopTableRaw.data() };

	consteval internal::array<bool, 256ULL> genAsciiStringStopTable() noexcept {
		internal::array<bool, 256ULL> returnValues{};
		for (uint64_t x = 0; x < 0x20; ++x) {
			returnValues[x] = true;
		}
		for (uint64_t x = 0x80; x < 0x100; ++x) {
			returnValues[x] = true;
		}
		returnValues[jsonifier::internal::uint_c_v<'"'>]  = true;
		returnValues[jsonifier::internal::uint_c_v<'\\'>] = true;
		return returnValues;
	}

	alignas(64) inline constexpr auto asciiStringStopTableRaw{ genAsciiStringStopTable() };

	inline constexpr const bool* __restrict asciiStringStopTable{ asciiStringStopTableRaw.data() };

}
