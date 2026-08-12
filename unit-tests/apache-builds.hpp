// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/apache-builds.hpp
#pragma once

#include "common.hpp"

struct overall_load_data {};

struct job {
	std::string name;
	std::string url;
	std::string color;
};

struct view {
	std::string name;
	std::string url;
};

struct apache_builds_message {
	std::vector<overall_load_data> assignedLabels;
	std::string mode;
	std::string nodeDescription;
	std::string nodeName;
	int64_t numExecutors;
	std::string description;
	std::vector<job> jobs;
	overall_load_data overallLoad;
	view primaryView;
	bool quietingDown;
	int64_t slaveAgentPort;
	overall_load_data unlabeledLoad;
	bool useCrumbs;
	bool useSecurity;
	std::vector<view> views;
};

template<> struct jsonifier::core<overall_load_data> {
	using value_type				 = overall_load_data;
	static constexpr auto parseValue = createValue();
};

template<> struct jsonifier::core<job> {
	using value_type				 = job;
	static constexpr auto parseValue = createValue<&value_type::name, &value_type::url, &value_type::color>();
};

template<> struct jsonifier::core<view> {
	using value_type				 = view;
	static constexpr auto parseValue = createValue<&value_type::name, &value_type::url>();
};

template<> struct jsonifier::core<apache_builds_message> {
	using value_type				 = apache_builds_message;
	static constexpr auto parseValue = createValue<&value_type::assignedLabels, &value_type::mode, &value_type::nodeDescription, &value_type::nodeName, &value_type::numExecutors,
		&value_type::description, &value_type::jobs, &value_type::overallLoad, &value_type::primaryView, &value_type::quietingDown, &value_type::slaveAgentPort,
		&value_type::unlabeledLoad, &value_type::useCrumbs, &value_type::useSecurity, &value_type::views>();
};
