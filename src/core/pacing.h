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
// The capture interval of a fixed target. A target above the canvas rate captures on every tick,
// so its interval is the tick's: the readback timing then sees the time an analysis really has.
inline constexpr uint64_t fixed_capture_interval(uint32_t target_fps, uint64_t frame_interval)
{
	return std::max<uint64_t>(1000000000 / std::max(target_fps, 1u), frame_interval);
}
// The automatic analysis rate: the canvas rate divided by a whole number, so that the processed
// video keeps an even cadence instead of repeating frames at irregular moments. The divisor
// grows only on evidence: results that actually miss their ticks, or an OBS frame time near its
// budget. An analysis that nearly fills the interval is no reason, as long as its results
// arrive. The divisor shrinks as an attempt, which the same evidence then keeps or takes back.
// The analysis time cannot decide it: on Core AI an analysis takes about 30% longer at half the
// rate, so its time at the lower rate only rules out a rate that could not deliver even then.
// An increase that is taken back within a minute has failed, and each failure doubles the wait
// before the next attempt; one that holds longer clears that wait. Times are ns.
class AnalysisRate {
	static constexpr uint64_t second = 1000000000;
	static constexpr uint64_t evaluate_every = second, settle = 2 * second, first_retry = 10 * second,
				  max_retry = 600 * second, failed_within = 60 * second;
	static constexpr uint64_t max_rate = 60, min_budget_rate = 15;
	static constexpr double frame_limit = .85, frame_room = .7;
	static constexpr double delivery_limit = .9;
	std::array<float, 32> recent{};
	unsigned delivery_shortfalls = 0;
	size_t count = 0, next = 0;
	uint32_t current = 0;
	uint64_t last_evaluation = 0, measure_from = 0, last_change = 0, last_increase = 0, retry = first_retry,
		 delivered_results = 0;

	// The first results after a start, a pause, or a rate change are slower than the ones that
	// follow, so they are not judged.
	void settle_from(uint64_t now)
	{
		measure_from = now + settle;
		delivery_shortfalls = 0;
	}
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
	// A worker result reached the render thread, with or without a face.
	void delivered() { ++delivered_results; }
	// A new analysis context begins (device, quality, analysis size, or source): the old results
	// are dropped, and the new engine can take seconds to load. A short load leaves a few results
	// in the windows on both sides of the pause, which would read as two shortfalls in a row. The
	// old divisor and its analysis times also describe another engine: after the CPU they would
	// hold an accelerator at a few analyses per second, rising one step at a time. So the rate
	// starts over as on a new filter, and is judged again from its first analysis.
	void restart()
	{
		if (current)
			reason = "new context";
		current = 0;
		count = next = 0;
		delivered_results = 0;
		delivery_shortfalls = 0;
		retry = first_retry;
		last_increase = 0;
	}
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
		// Before the first analysis the engine is loading or no face is in the picture.
		if (!count) {
			last_evaluation = now;
			delivered_results = 0;
			settle_from(now);
			return;
		}
		if (now < last_evaluation + evaluate_every)
			return;
		const uint64_t window_start = last_evaluation, window = now - last_evaluation;
		const double expected = double(window) / double(frame_interval * current);
		const double results = double(delivered_results);
		delivered_results = 0;
		last_evaluation = now;
		// No results, or no ticks for a while, say nothing about the rate: the engine may be
		// loading or failing, or the source was hidden.
		if (results == 0 || window > 2 * std::max(evaluate_every, frame_interval)) {
			settle_from(now);
			return;
		}
		if (window_start < measure_from)
			return;
		// One short window can be a passing disturbance, so it takes two in a row. A window
		// counts whole results: below ten analyses per second less than one missing result is
		// its rounding, not a miss, and would keep a low rate from ever rising again.
		const bool short_window = results < expected * delivery_limit && results + 1 <= expected;
		delivery_shortfalls = short_window ? delivery_shortfalls + 1 : 0;
		const double tick_ms = double(frame_interval) / 1e6, analysis = analysis_p90();
		const bool budget_may_drop =
			double(second) / (double(frame_interval) * (current + 1)) >= double(min_budget_rate);
		const bool too_slow = delivery_shortfalls >= 2;
		const bool too_heavy = frame_ms > frame_limit * tick_ms && budget_may_drop;
		// An increase that has held this long succeeded: the next drop starts with the short wait.
		if (last_increase && now >= last_increase + failed_within && !too_slow && !too_heavy) {
			retry = first_retry;
			last_increase = 0;
		}
		last_results = results;
		last_expected = expected;
		last_frame_ms = frame_ms;
		last_analysis = analysis;
		if (too_slow || too_heavy) {
			reason = too_slow ? "results missed their ticks" : "frame budget";
			if (last_increase && now < last_increase + failed_within)
				retry = std::min(retry * 2, max_retry);
			++current;
			last_change = now;
			last_increase = 0;
			settle_from(now);
			return;
		}
		if (current <= lowest || now < last_change + retry)
			return;
		// Attempt the higher rate unless the analysis, as slow as it is now, would already miss
		// the share of ticks that lowers a rate.
		const uint32_t lower = current - 1;
		if (analysis * delivery_limit < tick_ms * lower &&
		    frame_ms * double(current) / double(lower) < frame_room * tick_ms) {
			current = lower;
			last_change = last_increase = now;
			settle_from(now);
			reason = "attempt";
		}
	}
	uint32_t divisor() const { return std::max(current, 1u); }
	// Why the divisor last changed, and the values behind that decision, for the log.
	const char *reason = "start";
	double last_results = 0, last_expected = 0, last_frame_ms = 0, last_analysis = 0;
	uint64_t interval(uint64_t frame_interval) const { return frame_interval * divisor(); }
};
// Whether work behind a control is still needed. It stays on for a moment after the control
// reaches zero, so that dragging a slider through zero does not restart anything. Times are ns.
class PauseTimer {
	static constexpr uint64_t delay = 2000000000;
	uint64_t last_wanted = 0;

public:
	bool active(bool wanted, uint64_t now)
	{
		if (wanted)
			last_wanted = now;
		return last_wanted && now >= last_wanted && now - last_wanted < delay;
	}
};
inline constexpr std::chrono::seconds kEngineIdleRelease{180};
inline constexpr std::chrono::seconds kFirstRetryDelay{2}, kMaxRetryDelay{60};
inline constexpr std::chrono::seconds next_retry_delay(std::chrono::seconds delay)
{
	return std::min(delay * 2, kMaxRetryDelay);
}
}
