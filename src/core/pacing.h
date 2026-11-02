// SPDX-License-Identifier: GPL-3.0-or-later
// Pure timing policy of the OBS filter: the analysis capture schedule, the automatic
// analysis rate, the readback timing, and the retry delay after a failed analysis.
// Independent of OBS and of the inference provider.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
namespace beauti {
// Decides on which video ticks a new capture starts. The schedule advances by the
// requested interval instead of restarting at the last tick, so a target that is no
// divisor of the canvas rate keeps its average rate (25 FPS on a 30 FPS canvas skips
// every sixth tick, rather than falling to 15 FPS). Times are ns.
struct CapturePacer {
	// Ticks and intervals come from integer ns; this absorbs their rounding.
	static constexpr uint64_t tolerance = 1000;
	uint64_t last = 0, next = 0;
	// A shortened interval takes effect at once; a clock running backward restarts.
	bool due(uint64_t now, uint64_t interval) const
	{
		return !last || now < last || now + tolerance >= std::min(next, last + interval);
	}
	void taken(uint64_t now, uint64_t interval)
	{
		const uint64_t scheduled = last ? std::min(next, last + interval) : now;
		const uint64_t behind = now > scheduled ? now - scheduled : 0;
		// After a pause or on a canvas slower than the target there is nothing to catch up.
		next = now >= last && behind < interval ? scheduled + interval : now + interval;
		last = now;
	}
};
// Whether a capture is read back in the tick that issued it, so that the analysis can finish
// before the next tick, or one tick later, which never waits for the GPU. Reading back at once
// makes the graphics thread wait for the GPU, which costs little on a free GPU and whole frames
// on a saturated one. A wait above a share of the frame interval therefore returns to the
// later readback for a while, for longer each time. Times are ns.
class ReadbackPolicy {
	static constexpr uint64_t first_cooldown = 5000000000, max_cooldown = 60000000000;
	static constexpr double analysis_margin = 1.2, max_wait_share = .3;
	uint64_t blocked_until = 0, cooldown = first_cooldown;

public:
	// It only helps when the analysis finishes before the next capture.
	bool immediate(uint64_t now, uint64_t capture_interval, double analysis_ms) const
	{
		return now >= blocked_until && analysis_ms * analysis_margin * 1e6 <= double(capture_interval);
	}
	void observe(uint64_t now, uint64_t wait, uint64_t frame_interval)
	{
		if (double(wait) > double(frame_interval) * max_wait_share) {
			blocked_until = now + cooldown;
			cooldown = std::min(cooldown * 2, max_cooldown);
		} else if (now >= blocked_until + max_cooldown) {
			cooldown = first_cooldown; // Long without a slow wait: the load has gone.
		}
	}
};
// The automatic analysis rate: the canvas rate divided by a whole number, so that the processed
// video keeps an even cadence instead of repeating frames at irregular moments. The divisor
// grows when the analysis cannot keep the cadence or the OBS frame time nears its budget, and
// shrinks after a stable period when the lower divisor would fit with room to spare. An
// increase that had to be taken back waits longer before the next attempt. Times are ns.
class AnalysisRate {
	static constexpr uint64_t second = 1000000000;
	static constexpr uint64_t evaluate_every = second, hold_after_change = 2 * second, first_retry = 10 * second,
				  max_retry = 120 * second, failed_within = 5 * second;
	static constexpr uint64_t max_rate = 60, min_budget_rate = 15;
	static constexpr double analysis_limit = .9, analysis_room = .7, frame_limit = .85, frame_room = .6;
	std::array<float, 32> recent{};
	size_t count = 0, next = 0;
	uint32_t current = 0;
	uint64_t last_evaluation = 0, last_change = 0, last_increase = 0, retry = first_retry;

	double analysis_p90()
	{
		auto values = recent;
		const auto end = values.begin() + std::ptrdiff_t(count);
		const auto middle = values.begin() + std::ptrdiff_t((count * 9) / 10);
		std::nth_element(values.begin(), middle, end);
		return double(*middle);
	}
	static uint32_t lowest_divisor(uint64_t frame_interval)
	{
		return uint32_t(std::max<uint64_t>(1, (second / max_rate + frame_interval - 1) / frame_interval));
	}

public:
	void analyzed(double ms)
	{
		recent[next] = float(ms);
		next = (next + 1) % recent.size();
		count = std::min(count + 1, recent.size());
	}
	// Call once per video tick. frame_ms is OBS's average time for one frame.
	void tick(uint64_t now, uint64_t frame_interval, double frame_ms)
	{
		const uint32_t lowest = lowest_divisor(frame_interval);
		if (!current) {
			// A fast canvas starts at half its rate and rises once the machine has shown it can.
			current = std::max(lowest, frame_interval < second / 40 ? 2u : 1u);
			last_change = last_evaluation = now;
			return;
		}
		current = std::max(current, lowest);
		if (now < last_evaluation + evaluate_every || !count)
			return;
		last_evaluation = now;
		const double tick_ms = double(frame_interval) / 1e6, analysis = analysis_p90();
		const bool budget_may_drop =
			double(second) / (double(frame_interval) * (current + 1)) >= double(min_budget_rate);
		const bool too_slow = analysis > analysis_limit * tick_ms * current;
		const bool too_heavy = frame_ms > frame_limit * tick_ms && budget_may_drop;
		if (too_slow || too_heavy) {
			if (now < last_change + hold_after_change)
				return;
			if (last_increase && now < last_increase + failed_within)
				retry = std::min(retry * 2, max_retry);
			++current;
			last_change = now;
			last_increase = 0;
			return;
		}
		if (current <= lowest || now < last_change + retry)
			return;
		const uint32_t lower = current - 1;
		if (analysis < analysis_room * tick_ms * lower &&
		    frame_ms * double(current) / double(lower) < frame_room * tick_ms) {
			current = lower;
			last_change = last_increase = now;
		}
	}
	uint32_t divisor() const { return std::max(current, 1u); }
	uint64_t interval(uint64_t frame_interval) const { return frame_interval * divisor(); }
};
inline constexpr std::chrono::seconds kFirstRetryDelay{2}, kMaxRetryDelay{60};
inline constexpr std::chrono::seconds next_retry_delay(std::chrono::seconds delay)
{
	return std::min(delay * 2, kMaxRetryDelay);
}
}
