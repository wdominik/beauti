// SPDX-License-Identifier: GPL-3.0-or-later
// Contract of the capture schedule and the retry delay (src/core/pacing.h).
#include "pacing.h"
#include <catch_amalgamated.hpp>
#include <vector>

namespace {
using beauti::CapturePacer;
constexpr uint64_t kSecond = 1000000000;

// Simulates 20 seconds of video ticks and returns the captures per second.
double captured_rate(double canvas, unsigned target)
{
	CapturePacer pacer;
	const uint64_t interval = kSecond / target;
	const auto ticks = uint64_t(canvas * 20);
	unsigned count = 0;
	for (uint64_t i = 0; i < ticks; ++i) {
		const auto now = kSecond + uint64_t(double(i) * double(kSecond) / canvas);
		if (pacer.due(now, interval)) {
			pacer.taken(now, interval);
			++count;
		}
	}
	return double(count) / 20;
}
}

TEST_CASE("The capture rate follows the target on every canvas", "[pacing]")
{
	const auto [canvas, target, expected] = GENERATE(table<double, unsigned, double>({
		{30, 30, 30},
		{30000. / 1001, 30, 30000. / 1001},
		{60, 60, 60},
		{60000. / 1001, 60, 60000. / 1001},
		{60, 30, 30},
		{60000. / 1001, 30, 30000. / 1001},
		{30, 25, 25},
		{60, 25, 25},
		{60, 24, 24},
		{60, 40, 40},
		{60, 59, 59},
		{30, 1, 1},
		{30, 60, 30},
	}));
	CAPTURE(canvas, target);
	CHECK_THAT(captured_rate(canvas, target), Catch::Matchers::WithinAbs(expected, .1));
}

TEST_CASE("The first tick captures, and an untaken capture stays due", "[pacing]")
{
	CapturePacer pacer;
	const uint64_t interval = kSecond / 30;
	CHECK(pacer.due(5, interval));
	pacer.taken(5, interval);
	CHECK_FALSE(pacer.due(5 + interval / 2, interval));
	const uint64_t tick = 5 + interval;
	CHECK(pacer.due(tick, interval));
	// All snapshot slots were busy, so the caller did not take it.
	CHECK(pacer.due(tick + interval / 2, interval));
}

TEST_CASE("A pause is not caught up afterward", "[pacing]")
{
	CapturePacer pacer;
	const uint64_t interval = kSecond / 30;
	pacer.taken(kSecond, interval);
	const uint64_t resumed = 10 * kSecond;
	REQUIRE(pacer.due(resumed, interval));
	pacer.taken(resumed, interval);
	CHECK_FALSE(pacer.due(resumed + interval / 2, interval));
	CHECK(pacer.due(resumed + interval, interval));
}

TEST_CASE("A shorter interval applies at once, a longer one after the next capture", "[pacing]")
{
	CapturePacer pacer;
	pacer.taken(kSecond, kSecond);
	CHECK_FALSE(pacer.due(kSecond + kSecond / 10, kSecond));
	CHECK(pacer.due(kSecond + kSecond / 10, kSecond / 30));
	pacer.taken(2 * kSecond, kSecond / 30);
	const uint64_t longer = kSecond / 10, scheduled = 2 * kSecond + kSecond / 30;
	CHECK_FALSE(pacer.due(scheduled - kSecond / 100, longer));
	REQUIRE(pacer.due(scheduled, longer));
	pacer.taken(scheduled, longer);
	CHECK_FALSE(pacer.due(scheduled + longer / 2, longer));
	CHECK(pacer.due(scheduled + longer, longer));
}

TEST_CASE("A clock running backward restarts the schedule", "[pacing]")
{
	CapturePacer pacer;
	const uint64_t interval = kSecond / 30;
	pacer.taken(10 * kSecond, interval);
	REQUIRE(pacer.due(kSecond, interval));
	pacer.taken(kSecond, interval);
	CHECK_FALSE(pacer.due(kSecond + interval / 2, interval));
	CHECK(pacer.due(kSecond + interval, interval));
}

TEST_CASE("The retry delay doubles up to its limit", "[pacing]")
{
	using std::chrono::seconds;
	std::vector<seconds> delays{beauti::kFirstRetryDelay};
	while (delays.size() < 8)
		delays.push_back(beauti::next_retry_delay(delays.back()));
	const std::vector<seconds> expected{seconds(2),  seconds(4),  seconds(8),  seconds(16),
					    seconds(32), seconds(60), seconds(60), seconds(60)};
	CHECK(delays == expected);
}

TEST_CASE("Immediate readback needs an analysis that finishes before the next capture", "[pacing]")
{
	const beauti::ReadbackPolicy policy;
	const uint64_t at_30 = kSecond / 30, at_60 = kSecond / 60;
	CHECK(policy.immediate(kSecond, at_30, 0));
	CHECK(policy.immediate(kSecond, at_30, 15));
	CHECK(policy.immediate(kSecond, at_60, 13));
	CHECK_FALSE(policy.immediate(kSecond, at_60, 15));
	CHECK_FALSE(policy.immediate(kSecond, at_30, 100));
}

TEST_CASE("A slow GPU wait returns to the later readback, for longer each time", "[pacing]")
{
	beauti::ReadbackPolicy policy;
	const uint64_t frame = kSecond / 30, slow = frame / 2, fast = frame / 10;
	uint64_t now = kSecond;
	policy.observe(now, fast, frame);
	CHECK(policy.immediate(now, frame, 15));
	policy.observe(now, slow, frame);
	CHECK_FALSE(policy.immediate(now + 5 * kSecond - 1, frame, 15));
	now += 5 * kSecond;
	CHECK(policy.immediate(now, frame, 15));
	policy.observe(now, slow, frame);
	CHECK_FALSE(policy.immediate(now + 10 * kSecond - 1, frame, 15));
	CHECK(policy.immediate(now + 10 * kSecond, frame, 15));
	for (int i = 0; i < 8; ++i) {
		now += 60 * kSecond;
		policy.observe(now, slow, frame);
	}
	CHECK_FALSE(policy.immediate(now + 60 * kSecond - 1, frame, 15));
	CHECK(policy.immediate(now + 60 * kSecond, frame, 15));
	now += 60 * kSecond;
	policy.observe(now + 60 * kSecond, fast, frame);
	now += 60 * kSecond;
	policy.observe(now, slow, frame);
	CHECK(policy.immediate(now + 5 * kSecond, frame, 15));
}

namespace {
// Runs an automatic rate for the given seconds at a fixed analysis time and frame time.
void run_rate(beauti::AnalysisRate &rate, uint64_t &now, double seconds, uint64_t frame, double analysis_ms,
	      double frame_ms)
{
	for (auto ticks = uint64_t(seconds * double(kSecond) / double(frame)); ticks; --ticks) {
		now += frame;
		rate.analyzed(analysis_ms);
		rate.tick(now, frame, frame_ms);
	}
}
}

TEST_CASE("The automatic rate starts safely and keeps a canvas it can hold", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30, at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate slow_canvas;
	run_rate(slow_canvas, now, 30, at_30, 15, 6);
	CHECK(slow_canvas.divisor() == 1);
	beauti::AnalysisRate fast_canvas;
	fast_canvas.tick(now, at_60, 0);
	CHECK(fast_canvas.divisor() == 2);
	CHECK(fast_canvas.interval(at_60) == 2 * at_60);
	run_rate(fast_canvas, now, 30, at_60, 8, 3);
	CHECK(fast_canvas.divisor() == 1);
}

TEST_CASE("The automatic rate never exceeds 60 per second", "[pacing]")
{
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 30, kSecond / 120, 1, 1);
	CHECK(rate.divisor() == 2);
}

TEST_CASE("The automatic rate follows a slow analysis", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 30, at_30, 105, 6);
	CHECK(rate.divisor() == 4);
	CHECK(rate.interval(at_30) == 4 * at_30);
}

TEST_CASE("The automatic rate stays on an even cadence when analysis nearly fits", "[pacing]")
{
	const uint64_t at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 60, at_60, 16, 5);
	CHECK(rate.divisor() == 2);
}

TEST_CASE("A full frame budget lowers the rate, down to a floor", "[pacing]")
{
	const uint64_t at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 60, at_60, 5, 16);
	CHECK(rate.divisor() == 4); // 15 per second, however busy OBS is
}

TEST_CASE("An increase that fails waits longer before the next attempt", "[pacing]")
{
	const uint64_t at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	rate.tick(now, at_60, 0);
	run_rate(rate, now, 11, at_60, 8, 3);
	REQUIRE(rate.divisor() == 1);
	run_rate(rate, now, 3, at_60, 8, 15);
	REQUIRE(rate.divisor() == 2);
	run_rate(rate, now, 15, at_60, 8, 3);
	CHECK(rate.divisor() == 2);
	run_rate(rate, now, 10, at_60, 8, 3);
	CHECK(rate.divisor() == 1);
}
