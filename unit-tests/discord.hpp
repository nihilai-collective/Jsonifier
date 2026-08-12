// MIT License @ /License.md
// Copyright (c) 2026 Nihilai Collective Corp
// https://github.com/nihilai-collective/jsonifier
// unit-tests/discord.hpp
#pragma once

#include "common.hpp"

struct emoji_data {
	std::string id{};
	std::string name{};
	std::vector<std::nullptr_t> roles{};
	bool require_colons{};
	bool managed{};
	bool animated{};
	bool available{};
};

struct incidents_data_data {
	std::string invites_disabled_until{};
	std::string dms_disabled_until{};
};

struct tags_data {
	std::optional<std::string> bot_id{};
	std::nullptr_t guild_connections{};
};

struct role_data {
	std::string id{};
	std::string name{};
	std::nullptr_t description{};
	std::string permissions{};
	int64_t position{};
	int64_t color{};
	bool hoist{};
	bool managed{};
	bool mentionable{};
	std::nullptr_t icon{};
	std::nullptr_t unicode_emoji{};
	int64_t flags{};
	std::optional<tags_data> tags{};
};

struct sticker_data {
	std::string id{};
	std::string name{};
	std::string tags{};
	int64_t type{};
	int64_t format_type{};
	std::string description{};
	std::string asset{};
	bool available{};
	std::string guild_id{};
};

struct discord_message {
	std::string id{};
	std::string name{};
	std::string icon{};
	std::string description{};
	std::nullptr_t home_header{};
	std::nullptr_t splash{};
	std::string discovery_splash{};
	std::vector<std::string> features{};
	std::nullptr_t banner{};
	std::string owner_id{};
	std::nullptr_t application_id{};
	std::string region{};
	std::nullptr_t afk_channel_id{};
	int64_t afk_timeout{};
	std::string system_channel_id{};
	int64_t system_channel_flags{};
	bool widget_enabled{};
	std::string widget_channel_id{};
	int64_t verification_level{};
	std::vector<role_data> roles{};
	int64_t default_message_notifications{};
	int64_t mfa_level{};
	int64_t explicit_content_filter{};
	std::nullptr_t max_presences{};
	int64_t max_members{};
	int64_t max_stage_video_channel_users{};
	int64_t max_video_channel_users{};
	std::nullptr_t vanity_url_code{};
	int64_t premium_tier{};
	int64_t premium_subscription_count{};
	std::string preferred_locale{};
	std::string rules_channel_id{};
	std::string safety_alerts_channel_id{};
	std::string public_updates_channel_id{};
	std::nullptr_t hub_type{};
	bool premium_progress_bar_enabled{};
	std::string latest_onboarding_question_id{};
	bool nsfw{};
	int64_t nsfw_level{};
	std::vector<emoji_data> emojis{};
	std::vector<sticker_data> stickers{};
	incidents_data_data incidents_data{};
	std::nullptr_t inventory_settings{};
	bool embed_enabled{};
	std::string embed_channel_id{};
	int64_t approximate_member_count{};
	int64_t approximate_presence_count{};
};

template<> struct jsonifier::core<emoji_data> {
	using value_type = emoji_data;
	static constexpr auto parseValue =
		createValue<&value_type::id, &value_type::name, &value_type::roles, &value_type::require_colons, &value_type::managed, &value_type::animated, &value_type::available>();
};

template<> struct jsonifier::core<incidents_data_data> {
	using value_type				 = incidents_data_data;
	static constexpr auto parseValue = createValue<&value_type::invites_disabled_until, &value_type::dms_disabled_until>();
};

template<> struct jsonifier::core<tags_data> {
	using value_type				 = tags_data;
	static constexpr auto parseValue = createValue<&value_type::bot_id, &value_type::guild_connections>();
};

template<> struct jsonifier::core<role_data> {
	using value_type = role_data;
	static constexpr auto parseValue =
		createValue<&value_type::id, &value_type::name, &value_type::description, &value_type::permissions, &value_type::position, &value_type::color, &value_type::hoist,
			&value_type::managed, &value_type::mentionable, &value_type::icon, &value_type::unicode_emoji, &value_type::flags, &value_type::tags>();
};

template<> struct jsonifier::core<sticker_data> {
	using value_type				 = sticker_data;
	static constexpr auto parseValue = createValue<&value_type::id, &value_type::name, &value_type::tags, &value_type::type, &value_type::format_type, &value_type::description,
		&value_type::asset, &value_type::available, &value_type::guild_id>();
};

template<> struct jsonifier::core<discord_message> {
	using value_type = discord_message;
	static constexpr auto parseValue =
		createValue<&value_type::id, &value_type::name, &value_type::icon, &value_type::description, &value_type::home_header, &value_type::splash, &value_type::discovery_splash,
			&value_type::features, &value_type::banner, &value_type::owner_id, &value_type::application_id, &value_type::region, &value_type::afk_channel_id,
			&value_type::afk_timeout, &value_type::system_channel_id, &value_type::system_channel_flags, &value_type::widget_enabled, &value_type::widget_channel_id,
			&value_type::verification_level, &value_type::roles, &value_type::default_message_notifications, &value_type::mfa_level, &value_type::explicit_content_filter,
			&value_type::max_presences, &value_type::max_members, &value_type::max_stage_video_channel_users, &value_type::max_video_channel_users, &value_type::vanity_url_code,
			&value_type::premium_tier, &value_type::premium_subscription_count, &value_type::preferred_locale, &value_type::rules_channel_id, &value_type::safety_alerts_channel_id,
			&value_type::public_updates_channel_id, &value_type::hub_type, &value_type::premium_progress_bar_enabled, &value_type::latest_onboarding_question_id, &value_type::nsfw,
			&value_type::nsfw_level, &value_type::emojis, &value_type::stickers, &value_type::incidents_data, &value_type::inventory_settings, &value_type::embed_enabled,
			&value_type::embed_channel_id, &value_type::approximate_member_count, &value_type::approximate_presence_count>();
};
