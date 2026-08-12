// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/marine_ik.hpp
#pragma once

#include "common.hpp"

struct marine_ik_animation {
	std::vector<std::optional<std::string>> tracks{};
	int64_t fps{};
	std::string name{};
};

struct key {
	std::vector<double> pos{};
	double time{};
	std::vector<double> scl{};
	std::vector<double> rot{};
};

struct hierarchy_data {
	int64_t parent{};
	std::vector<key> keys{};
};

struct data_animation {
	std::vector<hierarchy_data> hierarchy{};
	double length{};
	int64_t fps{};
	std::string name{};
};

struct bone {
	int64_t parent{};
	std::vector<double> pos{};
	std::vector<double> rotq{};
	std::vector<int64_t> scl{};
	std::string name{};
};

struct data_metadata {
	int64_t uvs{};
	int64_t version{};
	int64_t faces{};
	std::string generator{};
	int64_t normals{};
	int64_t bones{};
	int64_t vertices{};
};

struct data_data {
	std::vector<std::vector<double>> uvs{};
	std::vector<data_animation> animations{};
	std::vector<double> vertices{};
	data_metadata metadata{};
	std::string name{};
	std::vector<double> skinWeights{};
	std::vector<int64_t> skinIndices{};
	int64_t influencesPerVertex{};
	std::vector<double> normals{};
	std::vector<bone> bones{};
	std::vector<int64_t> faces{};
};

struct marine_ik_geometry_data {
	std::string type{};
	std::string uuid{};
	data_data data{};
};

struct image_data {
	std::string url{};
	std::string uuid{};
	std::string name{};
};

struct material_data {
	int64_t vertexColors{};
	std::string name{};
	std::string type{};
	std::string uuid{};
	std::string blending{};
	std::string map{};
	bool transparent{};
	bool depthTest{};
	int64_t color{};
	int64_t shininess{};
	int64_t emissive{};
	bool depthWrite{};
	int64_t specular{};
};

struct marine_ik_metadata {
	std::string sourceFile{};
	std::string generator{};
	std::string type{};
	double version{};
};

struct child {
	std::string name{};
	std::string uuid{};
	std::vector<int64_t> matrix{};
	bool visible{};
	std::string type{};
	std::string material{};
	bool castShadow{};
	bool receiveShadow{};
	std::string geometry{};
};

struct object_data {
	std::vector<child> children{};
	std::string type{};
	std::vector<int64_t> matrix{};
	std::string uuid{};
};

struct texture_data {
	std::vector<int64_t> repeat{};
	std::vector<int64_t> wrap{};
	int64_t anisotropy{};
	std::string image{};
	std::string name{};
	int64_t mapping{};
	int64_t minFilter{};
	std::string uuid{};
	int64_t magFilter{};
};

struct marine_ik {
	std::vector<image_data> images{};
	std::vector<marine_ik_geometry_data> geometries{};
	std::vector<texture_data> textures{};
	marine_ik_metadata metadata{};
	std::vector<material_data> materials{};
	object_data object{};
	std::vector<marine_ik_animation> animations{};
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
