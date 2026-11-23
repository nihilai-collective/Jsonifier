/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nihilai Collective Corp
 * https://github.com/nihilai-collective/jsonifier
 * unit-tests/instruments.hpp
 */
#pragma once

#include "common.hpp"

struct node {
	int64_t value{};
	int64_t tick{};
};

struct envelope {
	std::vector<node> nodes{};
	int64_t sustain_start{};
	int64_t release_node{};
	int64_t sustain_end{};
	int64_t loop_start{};
	int64_t loop_end{};
};

struct instrument_data {
	bool default_filter_resonance_enabled{};
	std::optional<std::string> sample_map{};
	std::optional<std::string> note_map{};
	bool default_filter_cutoff_enabled{};
	std::optional<std::string> tuning{};
	int64_t default_filter_resonance{};
	int64_t random_resonance_weight{};
	int64_t default_filter_cutoff{};
	int64_t duplicate_note_action{};
	int64_t duplicate_check_type{};
	int64_t pitch_pan_separation{};
	int64_t random_cutoff_weight{};
	int64_t random_volume_weight{};
	int64_t default_filter_mode{};
	std::string legacy_filename{};
	int64_t pitch_to_tempo_lock{};
	envelope panning_envelope{};
	int64_t random_pan_weight{};
	int64_t pitch_pan_center{};
	envelope volume_envelope{};
	int64_t volume_ramp_down{};
	int64_t new_note_action{};
	envelope pitch_envelope{};
	int64_t volume_ramp_up{};
	int64_t global_volume{};
	int64_t midi_drum_set{};
	int64_t graph_insert{};
	int64_t midi_channel{};
	int64_t midi_program{};
	int64_t default_pan{};
	int64_t midi_bank{};
	std::string name{};
	int64_t fadeout{};
};

struct datum {
	int64_t channel{};
	int64_t fxparam{};
	int64_t volcmd{};
	int64_t volval{};
	int64_t fxcmd{};
	int64_t instr{};
	int64_t note{};
	int64_t row{};
};

struct pattern {
	std::optional<std::vector<datum>> data{};
	int64_t rows_per_measure{};
	int64_t rows_per_beat{};
	std::string name{};
	int64_t rows{};
};

struct sample_data {
	std::string legacy_filename{};
	int64_t c5_samplerate{};
	int64_t global_volume{};
	int64_t sustain_start{};
	int64_t vibrato_depth{};
	int64_t vibrato_sweep{};
	int64_t vibrato_rate{};
	int64_t vibrato_type{};
	int64_t sustain_end{};
	int64_t loop_start{};
	int64_t loop_end{};
	std::string name{};
	int64_t length{};
	int64_t volume{};
	int64_t pan{};
};

struct instruments_message {
	std::vector<instrument_data> instruments{};
	std::optional<std::string> pluginstate{};
	std::optional<std::string> orderlist{};
	std::optional<std::string> message{};
	std::vector<sample_data> samples{};
	std::vector<pattern> patterns{};
	std::nullptr_t graphstate{};
	std::string name{};
	int64_t version{};
};

template<> struct jsonifier::core<node> {
	using value_type				 = node;
	static constexpr auto parseValue = createValue<&value_type::tick, &value_type::value>();
};

template<> struct jsonifier::core<envelope> {
	using value_type = envelope;
	static constexpr auto parseValue =
		createValue<&value_type::loop_end, &value_type::loop_start, &value_type::nodes, &value_type::release_node, &value_type::sustain_end, &value_type::sustain_start>();
};

template<> struct jsonifier::core<instrument_data> {
	using value_type				 = instrument_data;
	static constexpr auto parseValue = createValue<&value_type::default_filter_cutoff, &value_type::default_filter_cutoff_enabled, &value_type::default_filter_mode,
		&value_type::default_filter_resonance, &value_type::default_filter_resonance_enabled, &value_type::default_pan, &value_type::duplicate_check_type,
		&value_type::duplicate_note_action, &value_type::fadeout, &value_type::global_volume, &value_type::graph_insert, &value_type::legacy_filename, &value_type::midi_bank,
		&value_type::midi_channel, &value_type::midi_drum_set, &value_type::midi_program, &value_type::name, &value_type::new_note_action, &value_type::note_map,
		&value_type::panning_envelope, &value_type::pitch_envelope, &value_type::pitch_pan_center, &value_type::pitch_pan_separation, &value_type::pitch_to_tempo_lock,
		&value_type::random_cutoff_weight, &value_type::random_pan_weight, &value_type::random_resonance_weight, &value_type::random_volume_weight, &value_type::sample_map,
		&value_type::tuning, &value_type::volume_envelope, &value_type::volume_ramp_down, &value_type::volume_ramp_up>();
};

template<> struct jsonifier::core<datum> {
	using value_type				 = datum;
	static constexpr auto parseValue = createValue<&value_type::channel, &value_type::fxcmd, &value_type::fxparam, &value_type::instr, &value_type::note, &value_type::row,
		&value_type::volcmd, &value_type::volval>();
};

template<> struct jsonifier::core<pattern> {
	using value_type				 = pattern;
	static constexpr auto parseValue = createValue<&value_type::data, &value_type::name, &value_type::rows, &value_type::rows_per_beat, &value_type::rows_per_measure>();
};

template<> struct jsonifier::core<sample_data> {
	using value_type				 = sample_data;
	static constexpr auto parseValue = createValue<&value_type::c5_samplerate, &value_type::global_volume, &value_type::legacy_filename, &value_type::length, &value_type::loop_end,
		&value_type::loop_start, &value_type::name, &value_type::pan, &value_type::sustain_end, &value_type::sustain_start, &value_type::vibrato_depth, &value_type::vibrato_rate,
		&value_type::vibrato_sweep, &value_type::vibrato_type, &value_type::volume>();
};

template<> struct jsonifier::core<instruments_message> {
	using value_type				 = instruments_message;
	static constexpr auto parseValue = createValue<&value_type::graphstate, &value_type::instruments, &value_type::message, &value_type::name, &value_type::orderlist,
		&value_type::patterns, &value_type::pluginstate, &value_type::samples, &value_type::version>();
};
