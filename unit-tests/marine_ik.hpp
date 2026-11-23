/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/marine_ik.hpp
 */
#pragma once

#include "common.hpp"

struct marine_ik_animation {
	std::vector<std::optional<std::string>> tracks{};
	std::string name{};
	int64_t fps{};
};

struct key {
	std::vector<double> pos{};
	std::vector<double> scl{};
	std::vector<double> rot{};
	double time{};
};

struct hierarchy_data {
	std::vector<key> keys{};
	int64_t parent{};
};

struct data_animation {
	std::vector<hierarchy_data> hierarchy{};
	std::string name{};
	double length{};
	int64_t fps{};
};

struct bone {
	std::vector<double> rotq{};
	std::vector<int64_t> scl{};
	std::vector<double> pos{};
	std::string name{};
	int64_t parent{};
};

struct data_metadata {
	std::string generator{};
	int64_t vertices{};
	int64_t version{};
	int64_t normals{};
	int64_t faces{};
	int64_t bones{};
	int64_t uvs{};
};

struct data_data {
	std::vector<data_animation> animations{};
	std::vector<std::vector<double>> uvs{};
	std::vector<int64_t> skinIndices{};
	std::vector<double> skinWeights{};
	std::vector<double> vertices{};
	int64_t influencesPerVertex{};
	std::vector<double> normals{};
	std::vector<int64_t> faces{};
	std::vector<bone> bones{};
	data_metadata metadata{};
	std::string name{};
};

struct marine_ik_geometry_data {
	std::string type{};
	std::string uuid{};
	data_data data{};
};

struct image_data {
	std::string uuid{};
	std::string name{};
	std::string url{};
};

struct material_data {
	int64_t vertexColors{};
	std::string blending{};
	int64_t shininess{};
	std::string name{};
	std::string type{};
	std::string uuid{};
	bool transparent{};
	int64_t emissive{};
	int64_t specular{};
	std::string map{};
	bool depthWrite{};
	bool depthTest{};
	int64_t color{};
};

struct marine_ik_metadata {
	std::string sourceFile{};
	std::string generator{};
	std::string type{};
	double version{};
};

struct child {
	std::vector<int64_t> matrix{};
	std::string material{};
	std::string geometry{};
	bool receiveShadow{};
	std::string name{};
	std::string uuid{};
	std::string type{};
	bool castShadow{};
	bool visible{};
};

struct object_data {
	std::vector<child> children{};
	std::vector<int64_t> matrix{};
	std::string type{};
	std::string uuid{};
};

struct texture_data {
	std::vector<int64_t> repeat{};
	std::vector<int64_t> wrap{};
	int64_t anisotropy{};
	std::string image{};
	int64_t minFilter{};
	int64_t magFilter{};
	std::string name{};
	std::string uuid{};
	int64_t mapping{};
};

struct marine_ik {
	std::vector<marine_ik_geometry_data> geometries{};
	std::vector<marine_ik_animation> animations{};
	std::vector<material_data> materials{};
	std::vector<texture_data> textures{};
	std::vector<image_data> images{};
	marine_ik_metadata metadata{};
	object_data object{};
};

template<> struct jsonifier::core<marine_ik_animation> {
	using value_type				 = marine_ik_animation;
	static constexpr auto parseValue = createValue<&value_type::tracks, &value_type::fps, &value_type::name>();
};

template<> struct jsonifier::core<key> {
	using value_type				 = key;
	static constexpr auto parseValue = createValue<&value_type::pos, &value_type::time, &value_type::scl, &value_type::rot>();
};

template<> struct jsonifier::core<hierarchy_data> {
	using value_type				 = hierarchy_data;
	static constexpr auto parseValue = createValue<&value_type::parent, &value_type::keys>();
};

template<> struct jsonifier::core<data_animation> {
	using value_type				 = data_animation;
	static constexpr auto parseValue = createValue<&value_type::hierarchy, &value_type::length, &value_type::fps, &value_type::name>();
};

template<> struct jsonifier::core<bone> {
	using value_type				 = bone;
	static constexpr auto parseValue = createValue<&value_type::parent, &value_type::pos, &value_type::rotq, &value_type::scl, &value_type::name>();
};

template<> struct jsonifier::core<data_metadata> {
	using value_type = data_metadata;
	static constexpr auto parseValue =
		createValue<&value_type::uvs, &value_type::version, &value_type::faces, &value_type::generator, &value_type::normals, &value_type::bones, &value_type::vertices>();
};

template<> struct jsonifier::core<data_data> {
	using value_type				 = data_data;
	static constexpr auto parseValue = createValue<&value_type::uvs, &value_type::animations, &value_type::vertices, &value_type::metadata, &value_type::name,
		&value_type::skinWeights, &value_type::skinIndices, &value_type::influencesPerVertex, &value_type::normals, &value_type::bones, &value_type::faces>();
};

template<> struct jsonifier::core<marine_ik_geometry_data> {
	using value_type				 = marine_ik_geometry_data;
	static constexpr auto parseValue = createValue<&value_type::type, &value_type::uuid, &value_type::data>();
};

template<> struct jsonifier::core<image_data> {
	using value_type				 = image_data;
	static constexpr auto parseValue = createValue<&value_type::url, &value_type::uuid, &value_type::name>();
};

template<> struct jsonifier::core<material_data> {
	using value_type				 = material_data;
	static constexpr auto parseValue = createValue<&value_type::vertexColors, &value_type::name, &value_type::type, &value_type::uuid, &value_type::blending, &value_type::map,
		&value_type::transparent, &value_type::depthTest, &value_type::color, &value_type::shininess, &value_type::emissive, &value_type::depthWrite, &value_type::specular>();
};

template<> struct jsonifier::core<marine_ik_metadata> {
	using value_type				 = marine_ik_metadata;
	static constexpr auto parseValue = createValue<&value_type::sourceFile, &value_type::generator, &value_type::type, &value_type::version>();
};

template<> struct jsonifier::core<child> {
	using value_type				 = child;
	static constexpr auto parseValue = createValue<&value_type::name, &value_type::uuid, &value_type::matrix, &value_type::visible, &value_type::type, &value_type::material,
		&value_type::castShadow, &value_type::receiveShadow, &value_type::geometry>();
};

template<> struct jsonifier::core<object_data> {
	using value_type				 = object_data;
	static constexpr auto parseValue = createValue<&value_type::children, &value_type::type, &value_type::matrix, &value_type::uuid>();
};

template<> struct jsonifier::core<texture_data> {
	using value_type				 = texture_data;
	static constexpr auto parseValue = createValue<&value_type::repeat, &value_type::wrap, &value_type::anisotropy, &value_type::image, &value_type::name, &value_type::mapping,
		&value_type::minFilter, &value_type::uuid, &value_type::magFilter>();
};

template<> struct jsonifier::core<marine_ik> {
	using value_type				 = marine_ik;
	static constexpr auto parseValue = createValue<&value_type::images, &value_type::geometries, &value_type::textures, &value_type::metadata, &value_type::materials,
		&value_type::object, &value_type::animations>();
};
