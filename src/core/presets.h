// SPDX-License-Identifier: GPL-3.0-or-later
// Stable look identities and effect values. Hardware settings are never part of a look.
#pragma once
#include "finishing.h"
#include <array>
#include <cstdint>
namespace beauti {
inline constexpr int kManualPreset = 0, kDefaultPreset = 102;
inline constexpr std::array<const char *, 5> kStrengthKeys{"retouch", "skin_tone", "dodge_burn", "smooth", "eyes"};
struct LookPreset {
	int id;
	const char *label;
	std::array<int, kStrengthKeys.size()> strengths;
	std::array<int, kFinishSettings.size()> finishing;
};
// IDs are saved in scenes. Before 1.0 a changed catalog may reuse or drop IDs;
// saved settings that no longer fit are reset to the default preset, not migrated.
// Low uses the fixed reference Smooth profile; High is the reference look; Very High
// removes fine texture more strongly without stronger eye enhancement.
inline constexpr std::array<LookPreset, 4> kLookPresets{{
	{101, "PresetLow", {40, 40, 30, 20, 25}, {45, 85, 70, 20, 100, 100, 0, 100, 100}},
	{102, "PresetMedium", {70, 70, 55, 40, 50}, {35, 90, 80, 25, 115, 110, 30, 110, 100}},
	{103, "PresetHigh", {100, 100, 80, 60, 80}, {25, 95, 90, 35, 130, 115, 60, 120, 100}},
	{104, "PresetVeryHigh", {100, 100, 100, 100, 80}, {100, 100, 100, 70, 130, 100, 90, 160, 100}},
}};
inline constexpr const LookPreset &kDefaultLook = kLookPresets[1];
static_assert(kDefaultLook.id == kDefaultPreset);
inline constexpr const LookPreset *look_preset(int64_t id)
{
	for (const auto &preset : kLookPresets)
		if (preset.id == id)
			return &preset;
	return nullptr;
}
}
