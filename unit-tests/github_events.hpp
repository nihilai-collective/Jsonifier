/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/github_events.hpp
 */
#pragma once

#include "common.hpp"

struct github_actor_data {
	std::string gravatar_id{};
	std::string login{};
	std::string avatar_url{};
	std::string url{};
	int64_t id{};
};

struct github_user_data {
	std::string url{};
	std::string gists_url{};
	std::string gravatar_id{};
	std::string type{};
	std::string avatar_url{};
	std::string subscriptions_url{};
	std::string received_events_url{};
	std::string organizations_url{};
	std::string repos_url{};
	std::string login{};
	int64_t id{};
	std::string starred_url{};
	std::string events_url{};
	std::string followers_url{};
	std::string following_url{};
};

struct github_comment_data {
	github_user_data user{};
	std::string url{};
	std::string issue_url{};
	std::string created_at{};
	std::string body{};
	std::string updated_at{};
	int64_t id{};
};

struct github_author_data {
	std::string email{};
	std::string name{};
};

struct github_commit_data {
	std::string url{};
	std::string message{};
	bool distinct{};
	std::string sha{};
	github_author_data author{};
};

struct github_forkee_data {
	std::string description{};
	bool fork{};
	std::string url{};
	std::string language{};
	std::string stargazers_url{};
	std::string clone_url{};
	std::string tags_url{};
	std::string full_name{};
	std::string merges_url{};
	int64_t forks{};
	bool privateVal{};
	std::string git_refs_url{};
	std::string archive_url{};
	std::string collaborators_url{};
	github_user_data owner{};
	std::string languages_url{};
	std::string trees_url{};
	std::string labels_url{};
	std::string html_url{};
	std::string pushed_at{};
	std::string created_at{};
	bool has_issues{};
	std::string forks_url{};
	std::string branches_url{};
	std::string commits_url{};
	std::string notifications_url{};
	int64_t open_issues{};
	std::string contents_url{};
	std::string blobs_url{};
	std::string issues_url{};
	std::string compare_url{};
	std::string issue_events_url{};
	std::string name{};
	std::string updated_at{};
	std::string statuses_url{};
	int64_t forks_count{};
	std::string assignees_url{};
	std::string ssh_url{};
	bool publicVal{};
	bool has_wiki{};
	std::string subscribers_url{};
	std::optional<std::string> mirror_url{};
	int64_t watchers_count{};
	int64_t id{};
	bool has_downloads{};
	std::string git_commits_url{};
	std::string downloads_url{};
	std::string pulls_url{};
	std::optional<std::string> homepage{};
	std::string issue_comment_url{};
	std::string hooks_url{};
	std::string subscription_url{};
	std::string milestones_url{};
	std::string svn_url{};
	std::string events_url{};
	std::string git_tags_url{};
	std::string teams_url{};
	std::string comments_url{};
	int64_t open_issues_count{};
	std::string keys_url{};
	std::string git_url{};
	std::string contributors_url{};
	int64_t size{};
	int64_t watchers{};
};

struct github_pull_request_data {
	std::optional<std::string> html_url{};
	std::optional<std::string> patch_url{};
	std::optional<std::string> diff_url{};
};

struct github_issue_data {
	github_user_data user{};
	std::string url{};
	std::vector<std::string> labels{};
	std::string html_url{};
	std::string labels_url{};
	github_pull_request_data pull_request{};
	std::string created_at{};
	std::optional<std::string> closed_at{};
	std::optional<std::string> milestone{};
	std::string title{};
	std::string body{};
	std::string updated_at{};
	int64_t number{};
	std::string state{};
	std::optional<github_user_data> assignee{};
	int64_t id{};
	std::string events_url{};
	std::string comments_url{};
	int64_t comments{};
};

struct github_page_data {
	std::string page_name{};
	std::string html_url{};
	std::string title{};
	std::string sha{};
	std::optional<std::string> summary{};
	std::string action{};
};

struct github_payload_data {
	std::optional<std::vector<github_commit_data>> commits{};
	std::optional<int64_t> distinct_size{};
	std::optional<std::string> ref{};
	std::optional<int64_t> push_id{};
	std::optional<std::string> head{};
	std::optional<std::string> before{};
	std::optional<int64_t> size{};
	std::optional<std::string> description{};
	std::optional<std::string> master_branch{};
	std::optional<std::string> ref_type{};
	std::optional<github_forkee_data> forkee{};
	std::optional<std::string> action{};
	std::optional<github_issue_data> issue{};
	std::optional<github_comment_data> comment{};
	std::optional<std::vector<github_page_data>> pages{};
};

struct github_repo_data {
	std::string url{};
	int64_t id{};
	std::string name{};
};

struct github_event_data {
	std::string type{};
	std::string created_at{};
	github_actor_data actor{};
	github_repo_data repo{};
	bool publicVal{};
	github_payload_data payload{};
	std::string id{};
	std::optional<github_actor_data> org{};
};

using github_events_message = std::vector<github_event_data>;

template<> struct jsonifier::core<github_actor_data> {
	using value_type				 = github_actor_data;
	static constexpr auto parseValue = createValue<&value_type::gravatar_id, &value_type::login, &value_type::avatar_url, &value_type::url, &value_type::id>();
};

template<> struct jsonifier::core<github_user_data> {
	using value_type				 = github_user_data;
	static constexpr auto parseValue = createValue<&value_type::url, &value_type::gists_url, &value_type::gravatar_id, &value_type::type, &value_type::avatar_url,
		&value_type::subscriptions_url, &value_type::received_events_url, &value_type::organizations_url, &value_type::repos_url, &value_type::login, &value_type::id,
		&value_type::starred_url, &value_type::events_url, &value_type::followers_url, &value_type::following_url>();
};

template<> struct jsonifier::core<github_comment_data> {
	using value_type = github_comment_data;
	static constexpr auto parseValue =
		createValue<&value_type::user, &value_type::url, &value_type::issue_url, &value_type::created_at, &value_type::body, &value_type::updated_at, &value_type::id>();
};

template<> struct jsonifier::core<github_author_data> {
	using value_type				 = github_author_data;
	static constexpr auto parseValue = createValue<&value_type::email, &value_type::name>();
};

template<> struct jsonifier::core<github_commit_data> {
	using value_type				 = github_commit_data;
	static constexpr auto parseValue = createValue<&value_type::url, &value_type::message, &value_type::distinct, &value_type::sha, &value_type::author>();
};

template<> struct jsonifier::core<github_forkee_data> {
	using value_type				 = github_forkee_data;
	static constexpr auto parseValue = createValue<&value_type::description, &value_type::fork, &value_type::url, &value_type::language, &value_type::stargazers_url,
		&value_type::clone_url, &value_type::tags_url, &value_type::full_name, &value_type::merges_url, &value_type::forks, makeJsonEntity<&value_type::privateVal, "private">(),
		&value_type::git_refs_url, &value_type::archive_url, &value_type::collaborators_url, &value_type::owner, &value_type::languages_url, &value_type::trees_url,
		&value_type::labels_url, &value_type::html_url, &value_type::pushed_at, &value_type::created_at, &value_type::has_issues, &value_type::forks_url, &value_type::branches_url,
		&value_type::commits_url, &value_type::notifications_url, &value_type::open_issues, &value_type::contents_url, &value_type::blobs_url, &value_type::issues_url,
		&value_type::compare_url, &value_type::issue_events_url, &value_type::name, &value_type::updated_at, &value_type::statuses_url, &value_type::forks_count,
		&value_type::assignees_url, &value_type::ssh_url, makeJsonEntity<&value_type::publicVal, "public">(), &value_type::has_wiki, &value_type::subscribers_url,
		&value_type::mirror_url, &value_type::watchers_count, &value_type::id, &value_type::has_downloads, &value_type::git_commits_url, &value_type::downloads_url,
		&value_type::pulls_url, &value_type::homepage, &value_type::issue_comment_url, &value_type::hooks_url, &value_type::subscription_url, &value_type::milestones_url,
		&value_type::svn_url, &value_type::events_url, &value_type::git_tags_url, &value_type::teams_url, &value_type::comments_url, &value_type::open_issues_count,
		&value_type::keys_url, &value_type::git_url, &value_type::contributors_url, &value_type::size, &value_type::watchers>();
};

template<> struct jsonifier::core<github_pull_request_data> {
	using value_type				 = github_pull_request_data;
	static constexpr auto parseValue = createValue<&value_type::html_url, &value_type::patch_url, &value_type::diff_url>();
};

template<> struct jsonifier::core<github_issue_data> {
	using value_type				 = github_issue_data;
	static constexpr auto parseValue = createValue<&value_type::user, &value_type::url, &value_type::labels, &value_type::html_url, &value_type::labels_url,
		&value_type::pull_request, &value_type::created_at, &value_type::closed_at, &value_type::milestone, &value_type::title, &value_type::body, &value_type::updated_at,
		&value_type::number, &value_type::state, &value_type::assignee, &value_type::id, &value_type::events_url, &value_type::comments_url, &value_type::comments>();
};

template<> struct jsonifier::core<github_page_data> {
	using value_type = github_page_data;
	static constexpr auto parseValue =
		createValue<&value_type::page_name, &value_type::html_url, &value_type::title, &value_type::sha, &value_type::summary, &value_type::action>();
};

template<> struct jsonifier::core<github_payload_data> {
	using value_type				 = github_payload_data;
	static constexpr auto parseValue = createValue<&value_type::commits, &value_type::distinct_size, &value_type::ref, &value_type::push_id, &value_type::head, &value_type::before,
		&value_type::size, &value_type::description, &value_type::master_branch, &value_type::ref_type, &value_type::forkee, &value_type::action, &value_type::issue,
		&value_type::comment, &value_type::pages>();
};

template<> struct jsonifier::core<github_repo_data> {
	using value_type				 = github_repo_data;
	static constexpr auto parseValue = createValue<&value_type::url, &value_type::id, &value_type::name>();
};

template<> struct jsonifier::core<github_event_data> {
	using value_type				 = github_event_data;
	static constexpr auto parseValue = createValue<&value_type::type, &value_type::created_at, &value_type::actor, &value_type::repo,
		makeJsonEntity<&value_type::publicVal, "public">(), &value_type::payload, &value_type::id, &value_type::org>();
};
