// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/random.hpp
#pragma once

#include "common.hpp"

struct friend_element {
	int64_t id{};
	std::string name{};
	std::string phone{};
};

struct result_data {
	int64_t id{};
	std::string avatar{};
	int64_t age{};
	bool admin{};
	std::string name{};
	std::string company{};
	std::string phone{};
	std::string email{};
	std::string birthDate{};
	std::vector<friend_element> friends{};
	std::string field{};
};

struct random_message {
	int64_t id{};
	std::string jsonrpc{};
	int64_t total{};
	std::vector<result_data> result{};
};

template<> struct jsonifier::core<friend_element> {
	using value_type				 = friend_element;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::name, &value_type::phone>();
};

template<> struct jsonifier::core<result_data> {
	using value_type				 = result_data;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::avatar, &value_type::age, &value_type::admin, &value_type::name, &value_type::company,
		&value_type::phone, &value_type::email, &value_type::birthDate, &value_type::friends, &value_type::field>();
};

template<> struct jsonifier::core<random_message> {
	using value_type				 = random_message;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::jsonrpc, &value_type::total, &value_type::result>();
};
