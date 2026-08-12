// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/mesh.hpp
#pragma once

#include "common.hpp"

struct batch {
	std::vector<int64_t> indexRange{};
	std::vector<int64_t> vertexRange{};
	std::vector<int64_t> usedBones{};
};

struct morph_targets {};

struct mesh_message {
	std::vector<batch> batches{};
	morph_targets morphTargets{};
	std::vector<double> positions{};
	std::vector<double> tex0{};
	std::vector<int64_t> colors{};
	std::vector<std::vector<int64_t>> influences{};
	std::vector<double> normals{};
	std::vector<int64_t> indices{};
};

template<> struct jsonifier::core<batch> {
	using value_type				 = batch;
	static constexpr auto parseValue = createValue<&value_type::indexRange, &value_type::vertexRange, &value_type::usedBones>();
};

template<> struct jsonifier::core<morph_targets> {
	using value_type				 = morph_targets;
	static constexpr auto parseValue = createValue();
};

template<> struct jsonifier::core<mesh_message> {
	using value_type				 = mesh_message;
	static constexpr auto parseValue = createValue<&value_type::batches, &value_type::morphTargets, &value_type::positions, &value_type::tex0, &value_type::colors,
		&value_type::influences, &value_type::normals, &value_type::indices>();
};
