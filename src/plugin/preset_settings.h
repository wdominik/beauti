// SPDX-License-Identifier: GPL-3.0-or-later
// Look-preset handling on OBS settings, shared by create/update and the properties callback.
#pragma once
#include "presets.h"
#include <obs-data.h>
namespace beauti {
inline bool matches_preset(obs_data_t *settings, const LookPreset &preset)
{
	for (size_t i = 0; i < kStrengthKeys.size(); ++i)
		if (obs_data_get_int(settings, kStrengthKeys[i]) != preset.strengths[i])
			return false;
	for (size_t i = 0; i < kFinishSettings.size(); ++i)
		if (obs_data_get_int(settings, kFinishSettings[i].key) != preset.finishing[i])
			return false;
	return true;
}
inline void write_preset(obs_data_t *settings, const LookPreset &preset)
{
	obs_data_set_int(settings, "preset", preset.id);
	for (size_t i = 0; i < kStrengthKeys.size(); ++i)
		obs_data_set_int(settings, kStrengthKeys[i], preset.strengths[i]);
	for (size_t i = 0; i < kFinishSettings.size(); ++i)
		obs_data_set_int(settings, kFinishSettings[i].key, preset.finishing[i]);
}
// Writes the selected preset's effect values into the settings, so Manual starts from
// them and OBS saves them. selected is true when the preset was just chosen (or on the
// first update); otherwise values that differ from the preset were set from outside
// (scripting, WebSocket) and switch the filter to Manual instead of being overwritten.
// Before 1.0 there is no migration: an unknown ID resets the look to the default preset.
inline void apply_look_preset(obs_data_t *settings, bool selected)
{
	const auto id = obs_data_get_int(settings, "preset");
	if (id == kManualPreset)
		return;
	const auto *preset = look_preset(id);
	if (!preset) {
		write_preset(settings, kDefaultLook);
		return;
	}
	if (!selected && !matches_preset(settings, *preset)) {
		obs_data_set_int(settings, "preset", kManualPreset);
		return;
	}
	write_preset(settings, *preset);
}
}
