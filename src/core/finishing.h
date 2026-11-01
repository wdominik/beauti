// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor-only finishing controls; the look presets supply their values.
#pragma once
#include "tuning.h"
#include <array>
#include <cstddef>
namespace beauti {
enum class FinishControl : size_t { Fine, Small, Medium, Broad, Size, Edges, Color, ColorSize, ColorEdges, Count };
struct FinishSetting {
	const char *key, *label;
	int minimum, maximum;
};
inline constexpr std::array<FinishSetting, size_t(FinishControl::Count)> kFinishSettings{{
	{"smooth_fine", "SmoothFine", 0, 100},
	{"smooth_small", "SmoothSmall", 0, 100},
	{"smooth_medium", "SmoothMedium", 0, 100},
	{"smooth_broad", "SmoothBroad", 0, 100},
	{"smooth_size", "StructureSize", 50, 200},
	{"smooth_edges", "EdgeProtection", 0, 200},
	{"local_color", "Strength", 0, 100},
	{"local_color_size", "ColorAreaSize", 25, 300},
	{"local_color_edges", "EdgeProtection", 0, 200},
}};
struct FinishingControls {
	std::array<float, 4> gains{tuning::kTextureGains[0], tuning::kTextureGains[1], tuning::kTextureGains[2],
				   tuning::kTextureGains[3]};
	float size = 1, edges = 1, color = 0, color_size = 1, color_edges = 1;
	bool operator==(const FinishingControls &) const = default;
};
inline FinishingControls finishing_controls(const std::array<float, kFinishSettings.size()> &values)
{
	return {{values[0] / 100.f, values[1] / 100.f, values[2] / 100.f, values[3] / 100.f},
		values[4] / 100.f,
		values[5] / 100.f,
		values[6] / 100.f,
		values[7] / 100.f,
		values[8] / 100.f};
}
}
