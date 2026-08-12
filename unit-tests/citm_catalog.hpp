// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/citm_catalog.hpp
#pragma once

#include "common.hpp"

struct audience_sub_category_names {
	std::string the337100890{};
};

struct names {};

struct event {
	std::nullptr_t description{};
	int64_t id{};
	std::optional<std::string> logo{};
	std::string name{};
	std::vector<int64_t> subTopicIds{};
	std::nullptr_t subjectCode{};
	std::nullptr_t subtitle{};
	std::vector<int64_t> topicIds{};
};

struct price {
	int64_t amount{};
	int64_t audienceSubCategoryId{};
	int64_t seatCategoryId{};
};

struct area {
	int64_t areaId{};
	std::vector<std::nullptr_t> blockIds{};
};

struct seat_category {
	std::vector<area> areas{};
	int64_t seatCategoryId{};
};

struct venue_names {
	std::string PLEYEL_PLEYEL{};
};

struct performance {
	int64_t eventId{};
	int64_t id{};
	std::optional<std::string> logo{};
	std::nullptr_t name{};
	std::vector<price> prices{};
	std::vector<seat_category> seatCategories{};
	std::nullptr_t seatMapImage{};
	int64_t start{};
	std::string venueCode{};
};

struct citm_catalog_message {
	std::unordered_map<std::string, std::string> areaNames{};
	audience_sub_category_names audienceSubCategoryNames{};
	names blockNames{};
	std::unordered_map<std::string, event> events{};
	std::vector<performance> performances{};
	std::unordered_map<std::string, std::string> seatCategoryNames{};
	std::unordered_map<std::string, std::string> subTopicNames{};
	names subjectNames{};
	std::unordered_map<std::string, std::string> topicNames{};
	std::unordered_map<std::string, std::vector<int64_t>> topicSubTopics{};
	venue_names venueNames{};
};

template<> struct jsonifier::core<audience_sub_category_names> {
	using value_type				 = audience_sub_category_names;
	static constexpr auto parseValue = createValue<makeJsonEntity<&value_type::the337100890, "337100890">()>();
};

template<> struct jsonifier::core<names> {
	using value_type				 = names;
	static constexpr auto parseValue = createValue();
};

template<> struct jsonifier::core<event> {
	using value_type				 = event;
	static constexpr auto parseValue = createValue<&value_type::description, &value_type::id, &value_type::logo, &value_type::name, &value_type::subTopicIds,
		&value_type::subjectCode, &value_type::subtitle, &value_type::topicIds>();
};

template<> struct jsonifier::core<price> {
	using value_type				 = price;
	static constexpr auto parseValue = createValue<&value_type::amount, &value_type::audienceSubCategoryId, &value_type::seatCategoryId>();
};

template<> struct jsonifier::core<area> {
	using value_type				 = area;
	static constexpr auto parseValue = createValue<&value_type::areaId, &value_type::blockIds>();
};

template<> struct jsonifier::core<seat_category> {
	using value_type				 = seat_category;
	static constexpr auto parseValue = createValue<&value_type::areas, &value_type::seatCategoryId>();
};

template<> struct jsonifier::core<venue_names> {
	using value_type				 = venue_names;
	static constexpr auto parseValue = createValue<&value_type::PLEYEL_PLEYEL>();
};

template<> struct jsonifier::core<performance> {
	using value_type				 = performance;
	static constexpr auto parseValue = createValue<&value_type::eventId, &value_type::id, &value_type::logo, &value_type::name, &value_type::prices, &value_type::seatCategories,
		&value_type::seatMapImage, &value_type::start, &value_type::venueCode>();
};

template<> struct jsonifier::core<citm_catalog_message> {
	using value_type = citm_catalog_message;
	static constexpr auto parseValue =
		createValue<&value_type::areaNames, &value_type::audienceSubCategoryNames, &value_type::blockNames, &value_type::events, &value_type::performances,
			&value_type::seatCategoryNames, &value_type::subTopicNames, &value_type::subjectNames, &value_type::topicNames, &value_type::topicSubTopics, &value_type::venueNames>();
};
