// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
namespace beauti {
// Render-thread policy, independent of the inference provider. Times are ns.
//
// The last-good hold is sized from the largest of three intervals: the engine's
// own processing time, the requested analysis cadence, and the observed delivery
// interval between the last two uninterrupted results. The third one covers
// scheduling delay between inference and delivery, which the engine time cannot
// see; it is forgotten after a cadence change or a fallback so a hung worker
// never widens its own window beyond the fixed cap.
//
// States: good (a processed result is displayed), degraded (the source shows
// after a stall, error, or confirmed no-face; two valid results restore good),
// and startup. `reason` names the observable cause for the diagnostics log.
// accept() returns true when the result becomes the displayed one; visible()
// expires the hold and reports whether a result may still be shown.
struct Continuity {
	static constexpr uint64_t ms = 1000000, max_age = 5000 * ms;
	static constexpr double hold_min_ms = 350, hold_max_ms = 2500, miss_min_ms = 150, miss_max_ms = 1500;
	uint64_t captured = 0, completed = 0, deadline = 0, miss_started = 0;
	uint64_t miss_budget = 150 * ms;
	double runtime = 0, cadence = 0, delivery = 0; // ms
	uint32_t sequence = 0;
	unsigned misses = 0, recovery = 0;
	bool good = false, degraded = false;
	const char *reason = "startup";
	void reset() { *this = Continuity{}; }
	void budgets()
	{
		const double interval = std::max({runtime, cadence, delivery});
		deadline =
			std::min(captured + max_age,
				 completed + uint64_t(std::clamp(2 * interval + 200., hold_min_ms, hold_max_ms)) * ms);
		miss_budget = uint64_t(std::clamp(1.25 * interval + 75., miss_min_ms, miss_max_ms)) * ms;
	}
	void set_cadence(double period_ms)
	{
		if (period_ms != cadence) {
			cadence = period_ms;
			delivery = 0; // measured under the old cadence; re-measure from the next result
			if (good)
				budgets();
		}
	}
	void fail(const char *why)
	{
		good = false;
		degraded = true;
		recovery = 0;
		reason = why;
	}
	bool visible(uint64_t now)
	{
		if (good && (now >= deadline || now < completed || (misses && now - miss_started >= miss_budget))) {
			good = false;
			degraded = true;
			recovery = 0;
			reason = misses ? "no_face_timeout" : "stall";
		}
		return good;
	}
	bool accept(uint32_t seq, uint64_t capture, uint64_t now, double runtime_ms, bool face)
	{
		visible(now);
		if (seq <= sequence || capture > now || now - capture >= max_age)
			return false;
		sequence = seq;
		if (!face) {
			recovery = 0;
			if (++misses == 1)
				miss_started = now;
			if (good && misses == 1)
				reason = "holding_no_face";
			if (misses >= 2) {
				good = false;
				degraded = true;
				reason = "confirmed_no_face";
			}
			return false;
		}
		misses = 0;
		miss_started = 0;
		if (degraded && ++recovery < 2) {
			reason = "recovering";
			return false;
		}
		// Only an uninterrupted run of good results measures the delivery interval.
		delivery = good && completed ? std::min(double(now - completed) / ms, hold_max_ms) : 0;
		captured = capture;
		completed = now;
		runtime = runtime_ms;
		budgets();
		good = true;
		degraded = false;
		recovery = 0;
		reason = "processed";
		return true;
	}
};
}
