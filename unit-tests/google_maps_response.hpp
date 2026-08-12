// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/google_maps_response.hpp
#pragma once

#include "common.hpp"

struct distance_data {
	std::string text{};
	int64_t value{};
};

struct element {
	distance_data distance{};
	distance_data duration{};
	std::string status{};
};

struct row {
	std::vector<element> elements{};
};

struct google_maps_response_message {
	std::vector<std::string> destination_addresses{};
	std::vector<std::string> origin_addresses{};
	std::vector<row> rows{};
	std::string status{};
};

template<> struct jsonifier::core<distance_data> {
	using value_type				 = distance_data;
	static constexpr auto parseValue = createValue<&value_type::text, &value_type::value>();
};

template<> struct jsonifier::core<element> {
	using value_type				 = element;
	static constexpr auto parseValue = createValue<&value_type::distance, &value_type::duration, &value_type::status>();
};

template<> struct jsonifier::core<row> {
	using value_type				 = row;
	static constexpr auto parseValue = createValue<&value_type::elements>();
};

template<> struct jsonifier::core<google_maps_response_message> {
	using value_type				 = google_maps_response_message;
	static constexpr auto parseValue = createValue<&value_type::destination_addresses, &value_type::origin_addresses, &value_type::rows, &value_type::status>();
};
