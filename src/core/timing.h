// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
namespace beauti {
// Opt-in host-wall diagnostics. These are not GPU kernel/event timings.
inline bool timing_enabled()
{
	static const bool enabled = [] {
		const char *v = std::getenv("BEAUTI_TIMING");
		return v && v[0] == '1' && v[1] == '\0';
	}();
	return enabled;
}
// Opt-in session summary in the OBS log (src/core/live_stats.h), without the per-stage output.
inline bool stats_enabled()
{
	static const bool enabled = [] {
		const char *v = std::getenv("BEAUTI_STATS");
		return v && v[0] == '1' && v[1] == '\0';
	}();
	return enabled;
}
using TimingClock = std::chrono::steady_clock;
inline void timing_log(const char *stage, uint32_t frame, double ms)
{
	std::fprintf(stderr, "Beauti timing stage=%s frame=%u ms=%.6f\n", stage, frame, ms);
}
struct TimingSpan {
	const char *stage;
	uint32_t frame;
	bool enabled;
	TimingClock::time_point start{};
	TimingSpan(const TimingSpan &) = delete;
	TimingSpan &operator=(const TimingSpan &) = delete;
	explicit TimingSpan(const char *name, uint32_t sequence = 0)
		: stage(name), frame(sequence), enabled(timing_enabled())
	{
		if (enabled)
			start = TimingClock::now();
	}
	~TimingSpan()
	{
		if (enabled)
			timing_log(stage, frame,
				   std::chrono::duration<double, std::milli>(TimingClock::now() - start).count());
	}
};
}
