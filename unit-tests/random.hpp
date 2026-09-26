/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/random.hpp
 */
#pragma once

#include "common.hpp"

struct friend_element {
	std::string phone{};
	std::string name{};
	int64_t id{};
};

struct result_data {
	std::vector<friend_element> friends{};
	std::string birthDate{};
	std::string company{};
	std::string avatar{};
	std::string phone{};
	std::string email{};
	std::string field{};
	std::string name{};
	int64_t age{};
	int64_t id{};
	bool admin{};
};

struct random_message {
	std::vector<result_data> result{};
	std::string jsonrpc{};
	int64_t total{};
	int64_t id{};
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
