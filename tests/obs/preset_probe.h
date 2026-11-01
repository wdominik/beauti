// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise real plugin callbacks, persistence, external edits, and the reset of unusable saved settings.
#pragma once
#include "presets.h"
#include <chrono>
#include <stdexcept>
#include <thread>

static void record_preset(std::ostream &out, const char *name, obs_source_t *source, bool &first)
{
	auto *settings = obs_source_get_settings(source);
	auto *props = obs_source_properties(source);
	if (!first)
		out << ",";
	first = false;
	out << "{\"case\":" << json_string(name) << ",\"settings\":" << obs_data_get_json_with_defaults(settings)
	    << ",\"properties\":[";
	bool property_first = true;
	dump_properties(out, props, property_first);
	out << "]}";
	obs_properties_destroy(props);
	obs_data_release(settings);
}
static void select_preset(obs_source_t *source, int id)
{
	auto *settings = obs_source_get_settings(source);
	auto *props = obs_source_properties(source);
	obs_data_set_int(settings, "preset", id);
	// This is the same callback OBS invokes for the dropdown, including widget refresh.
	if (!obs_property_modified(obs_properties_get(props, "preset"), settings))
		throw std::runtime_error("Preset change must refresh the properties view");
	obs_source_update(source, settings);
	const bool manual = !beauti::look_preset(obs_data_get_int(settings, "preset"));
	for (const auto *section :
	     {"retouch_section", "tone_section", "color_section", "dodge_section", "smooth_section", "eyes_section"})
		if (obs_property_visible(obs_properties_get(props, section)) != manual)
			throw std::runtime_error("Preset callback left incorrect slider visibility");
	if (!obs_property_visible(obs_properties_get(props, "processing_section")))
		throw std::runtime_error("Preset callback hid processing controls");
	obs_properties_destroy(props);
	obs_data_release(settings);
}
static void probe_presets(obs_source_t *filter, const std::string &report)
{
	auto *initial = obs_source_get_settings(filter);
	auto *restore = obs_data_create_from_json(obs_data_get_json_with_defaults(initial));
	obs_data_release(initial);
	std::ofstream out(beauti::utf8_path(report + ".presets.json"));
	out << "[";
	bool first = true;
	auto *fresh = obs_source_create_private("beauti_filter", "Preset fresh", nullptr);
	if (!fresh)
		throw std::runtime_error("Could not create a fresh preset test filter");
	record_preset(out, "fresh", fresh, first);
	obs_source_release(fresh);
	auto *hardware = obs_source_get_settings(filter);
	obs_data_set_int(hardware, "native_backend", 2);
	obs_data_set_int(hardware, "native_quality", 1);
	obs_data_set_int(hardware, "native_size", 960);
	obs_data_set_int(hardware, "native_fps", 17);
	obs_source_update(filter, hardware);
	obs_data_release(hardware);
	for (const auto &preset : beauti::kLookPresets) {
		select_preset(filter, preset.id);
		record_preset(out, preset.label, filter, first);
		select_preset(filter, 0);
		record_preset(out, "manual-after-preset", filter, first);
	}
	auto *manual = obs_source_get_settings(filter);
	obs_data_set_int(manual, "retouch", 17);
	obs_data_set_int(manual, "smooth_fine", 13);
	obs_data_set_int(manual, "local_color_size", 177);
	obs_source_update(filter, manual);
	auto *serialized = obs_data_create_from_json(obs_data_get_json_with_defaults(manual));
	obs_data_release(manual);
	auto *reloaded = obs_source_create_private("beauti_filter", "Preset reload", serialized);
	obs_data_release(serialized);
	if (!reloaded)
		throw std::runtime_error("Could not reload manual settings");
	record_preset(out, "manual-reloaded", reloaded, first);
	obs_source_release(reloaded);
	select_preset(filter, 999);
	record_preset(out, "unknown-id", filter, first);
	select_preset(filter, 103);
	record_preset(out, "preset-after-manual", filter, first);
	// A preset that is active while a script sets one effect value switches to Manual.
	select_preset(filter, 102);
	// OBS defers updates of video sources to the next video tick; let it apply the preset.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	auto *external = obs_source_get_settings(filter);
	obs_data_set_int(external, "retouch", 10);
	obs_source_update(filter, external);
	obs_data_release(external);
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	record_preset(out, "external-edit", filter, first);
	// Before 1.0, saved settings without a usable preset are reset to the default
	// preset; processing settings are kept.
	for (const auto *scenario : {"hardware-only", "no-preset", "retired-id"}) {
		auto *saved = obs_data_create();
		obs_data_set_int(saved, "native_fps", 17);
		if (std::strcmp(scenario, "hardware-only") != 0)
			obs_data_set_int(saved, "retouch", 37);
		if (std::strcmp(scenario, "retired-id") == 0)
			obs_data_set_int(saved, "preset", 3);
		auto *created = obs_source_create_private("beauti_filter", scenario, saved);
		obs_data_release(saved);
		if (!created)
			throw std::runtime_error("Could not create a filter from saved settings");
		record_preset(out, scenario, created, first);
		obs_source_release(created);
	}
	out << "]\n";
	obs_source_update(filter, restore);
	obs_data_release(restore);
}
