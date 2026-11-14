// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in session statistics for live measurements (BEAUTI_STATS=1). The render thread
// feeds them; nothing here touches OBS. Counters and percentiles cover one segment: the
// filter starts a new one when the source, the canvas, or a setting it measures changes,
// so that one OBS session can measure several configurations.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <string>
namespace beauti {
// Fixed 1 ms buckets, so a long session costs no memory or allocations.
class MsHistogram {
	std::array<uint32_t, 8192> buckets{};
	uint64_t total = 0;
	double largest = 0;

public:
	void add(double ms)
	{
		const auto clamped = std::max(0.0, ms);
		++buckets[std::min(size_t(clamped), buckets.size() - 1)];
		++total;
		largest = std::max(largest, clamped);
	}
	// Cleared in place: the buckets are too large for a temporary on the graphics thread's stack.
	void clear()
	{
		buckets.fill(0);
		total = 0;
		largest = 0;
	}
	uint64_t count() const { return total; }
	double maximum() const { return largest; }
	// Upper edge of the bucket that holds the percentile, but never above the largest
	// sample, so a p95 cannot read higher than the maximum; 0 without samples.
	double percentile(double fraction) const
	{
		if (!total)
			return 0;
		const auto wanted = std::max<uint64_t>(1, uint64_t(double(total) * fraction + .999999));
		uint64_t seen = 0;
		for (size_t i = 0; i < buckets.size(); ++i) {
			seen += buckets[i];
			if (seen >= wanted)
				return std::min(double(i + 1), largest);
		}
		return largest;
	}
};
class LiveStats {
	uint64_t first_tick = 0, last_tick = 0, last_report = 0, lost_at = 0;
	// Time in which the analysis reported no face: in total, before the first processed frame,
	// and since the last loss. Nobody waits for the filter then, so it is no startup or recovery.
	uint64_t absent_total = 0, absent_startup = 0, absent_recovery = 0;
	uint64_t ticks = 0, processed = 0, passthrough = 0, repeated = 0, accepted = 0;
	uint64_t results = 0, no_face = 0, bridged = 0, scene_cuts = 0, losses = 0, rate_changes = 0;
	double startup_ms = -1;
	bool fresh = false, was_good = false, absent = false;
	MsHistogram age, analysis, compose, recovery, readback;
	std::map<std::string, uint64_t> loss_reasons;

public:
	static constexpr uint64_t report_interval = 10000000000; // ns
	// Starts a new segment.
	void clear()
	{
		first_tick = last_tick = last_report = lost_at = 0;
		absent_total = absent_startup = absent_recovery = 0;
		ticks = processed = passthrough = repeated = accepted = 0;
		results = no_face = bridged = scene_cuts = losses = rate_changes = 0;
		startup_ms = -1;
		fresh = was_good = absent = false;
		for (auto *histogram : {&age, &analysis, &compose, &recovery, &readback})
			histogram->clear();
		loss_reasons.clear();
	}
	// A worker result reached the render thread, accepted or not.
	void result(bool face, bool landmarks)
	{
		++results;
		no_face += !face;
		absent = !face;
		bridged += face && !landmarks;
	}
	// The result became the displayed analysis.
	void accept(double analysis_ms)
	{
		++accepted;
		fresh = true;
		analysis.add(analysis_ms);
	}
	void scene_cut() { ++scene_cuts; }
	// The automatic analysis rate moved to another fraction of the canvas rate.
	void rate_changed() { ++rate_changes; }
	void composed(double ms) { compose.add(ms); }
	// The graphics thread waited this long for a capture it read back at once.
	void read_back(double ms) { readback.add(ms); }
	// Once per video tick. drawn: the output shows processed pixels. good: the policy
	// holds a valid result; reason: why not, when it just stopped.
	void tick(uint64_t now, bool drawn, double age_ms, bool good, const char *reason)
	{
		if (!first_tick)
			first_tick = last_report = now;
		if (absent && last_tick) {
			const uint64_t step = now - last_tick;
			absent_total += step;
			if (startup_ms < 0)
				absent_startup += step;
			if (lost_at)
				absent_recovery += step;
		}
		last_tick = now;
		++ticks;
		if (drawn) {
			++processed;
			age.add(age_ms);
			repeated += !fresh;
			fresh = false;
			if (startup_ms < 0)
				startup_ms = double(now - first_tick - absent_startup) / 1e6;
		} else {
			++passthrough;
		}
		if (was_good && !good) {
			++losses;
			++loss_reasons[reason];
			lost_at = now;
			absent_recovery = 0;
		} else if (!was_good && good && lost_at) {
			recovery.add(double(now - lost_at - absent_recovery) / 1e6);
			lost_at = 0;
		}
		was_good = good;
	}
	bool report_due(uint64_t now) const { return first_tick && now - last_report >= report_interval; }
	void reported(uint64_t now) { last_report = now; }
	uint64_t tick_count() const { return ticks; }
	std::string summary() const
	{
		auto text = std::format(
			"seconds={:.1f} ticks={} processed={} passthrough={} accepted={} repeated={} startup_ms={:.0f} "
			"age_p50={:.0f} age_p95={:.0f} age_max={:.0f} analysis_p50={:.0f} analysis_p95={:.0f} "
			"compose_p50={:.0f} compose_p95={:.0f} results={} no_face={} bridged={} scene_cuts={} "
			"losses={} recovery_p50={:.0f} recovery_max={:.0f} readback_count={} readback_p50={:.0f} "
			"readback_p95={:.0f} readback_max={:.0f} rate_changes={} no_face_seconds={:.1f}",
			double(last_tick - first_tick) / 1e9, ticks, processed, passthrough, accepted, repeated,
			std::max(startup_ms, 0.0), age.percentile(.5), age.percentile(.95), age.maximum(),
			analysis.percentile(.5), analysis.percentile(.95), compose.percentile(.5),
			compose.percentile(.95), results, no_face, bridged, scene_cuts, losses, recovery.percentile(.5),
			recovery.maximum(), readback.count(), readback.percentile(.5), readback.percentile(.95),
			readback.maximum(), rate_changes, double(absent_total) / 1e9);
		for (const auto &[reason, count] : loss_reasons)
			text += std::format(" loss_{}={}", reason, count);
		return text;
	}
};
}
