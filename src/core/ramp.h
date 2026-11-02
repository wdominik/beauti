// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace beauti {
inline constexpr uint64_t kStrengthRampNs = 150000000; // 150 ms slider transitions
// Smoothstep over kStrengthRampNs. Retargets from the current value, never from
// zero, so a slider dragged mid-ramp continues without a jump.
struct StrengthRamp {
	float value = 0, from = 0, target = 0;
	uint64_t started = 0;
	float sample(float requested, uint64_t now)
	{
		if (started) {
			float t = now >= started ? std::min(1.f, float(now - started) / float(kStrengthRampNs)) : 1.f;
			value = from + (target - from) * (t * t * (3 - 2 * t));
		}
		if (requested != target) {
			from = value;
			target = requested;
			started = now;
		}
		return value;
	}
};
}
