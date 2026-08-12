// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/canada.hpp
#pragma once

#include "common.hpp"

struct geometry_data {
	std::string type{};
	std::vector<std::vector<std::vector<double>>> coordinates{};
};

struct properties_data {
	std::string name{};
};

struct feature {
	std::string type{};
	properties_data properties{};
	geometry_data geometry{};
};

struct canada_message {
	std::string type{};
	std::vector<feature> features{};
};

template<> struct jsonifier::core<geometry_data> {
	using value_type				 = geometry_data;
	static constexpr auto parseValue = createValue<&value_type::type, &value_type::coordinates>();
};

template<> struct jsonifier::core<properties_data> {
	using value_type				 = properties_data;
	static constexpr auto parseValue = createValue<&value_type::name>();
};

template<> struct jsonifier::core<feature> {
	using value_type				 = feature;
	static constexpr auto parseValue = createValue<&value_type::type, &value_type::properties, &value_type::geometry>();
};

template<> struct jsonifier::core<canada_message> {
	using value_type				 = canada_message;
	static constexpr auto parseValue = createValue<&value_type::type, &value_type::features>();
};
