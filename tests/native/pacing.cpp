// SPDX-License-Identifier: GPL-3.0-or-later
// Contract of the capture schedule and the retry delay (src/core/pacing.h).
#include "pacing.h"
#include <catch_amalgamated.hpp>
#include <functional>
#include <string_view>
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

TEST_CASE("A fixed target above the canvas rate has the interval of a tick", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30, at_60 = kSecond / 60;
	CHECK(beauti::fixed_capture_interval(60, at_30) == at_30);
	CHECK(beauti::fixed_capture_interval(30, at_30) == at_30);
	CHECK(beauti::fixed_capture_interval(30, at_60) == at_30);
	CHECK(beauti::fixed_capture_interval(15, at_30) == kSecond / 15);
	CHECK(beauti::fixed_capture_interval(30, kSecond * 1001 / 30000) == kSecond * 1001 / 30000);
	// A 22 ms analysis finishes within a 30 FPS tick, whatever the target says.
	const beauti::ReadbackPolicy policy;
	CHECK(policy.immediate(kSecond, beauti::fixed_capture_interval(60, at_30), 22));
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
// Runs an automatic rate for the given seconds. Results arrive once per capture interval, but
// no faster than the analysis finishes, whose time may depend on the divisor in use. Returns
// how often the rate rose.
unsigned run_rate(beauti::AnalysisRate &rate, uint64_t &now, double seconds, uint64_t frame,
		  const std::function<double(uint32_t)> &analysis_ms, double frame_ms)
{
	double progress = 0;
	unsigned rises = 0;
	for (auto ticks = uint64_t(seconds * double(kSecond) / double(frame)); ticks; --ticks) {
		now += frame;
		const uint32_t divisor = rate.divisor();
		const double capture_ms = double(divisor * frame) / 1e6, analysis = analysis_ms(divisor);
		progress += (double(frame) / 1e6) / std::max(analysis, capture_ms);
		if (progress >= 1) {
			progress -= 1;
			rate.delivered();
			rate.analyzed(analysis);
		}
		rate.tick(now, frame, frame_ms);
		rises += rate.divisor() < divisor;
	}
	return rises;
}
unsigned run_rate(beauti::AnalysisRate &rate, uint64_t &now, double seconds, uint64_t frame, double analysis_ms,
		  double frame_ms)
{
	return run_rate(rate, now, seconds, frame, [analysis_ms](uint32_t) { return analysis_ms; }, frame_ms);
}
// Ticks in which only two of three results arrive, each after the given analysis time.
void run_disturbed(beauti::AnalysisRate &rate, uint64_t &now, double seconds, uint64_t frame, double analysis_ms)
{
	for (auto tick = uint64_t(seconds * double(kSecond) / double(frame)); tick; --tick) {
		now += frame;
		if (tick % 3) {
			rate.delivered();
			rate.analyzed(analysis_ms);
		}
		rate.tick(now, frame, 3);
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
	CHECK(rate.divisor() == 3);
	CHECK(rate.interval(at_30) == 3 * at_30);
}

TEST_CASE("An analysis that nearly fills the interval keeps the rate while its results arrive", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 60, at_30, 31, 8);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("A lowered rate returns once the analysis fits again", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 6, at_30, 50, 8);
	REQUIRE(rate.divisor() == 2);
	run_rate(rate, now, 12, at_30, 26, 8);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("A lowered rate returns although the analysis is slower at the lower rate", "[pacing]")
{
	// Measured with Core AI on an M1 Ultra and an M4 Pro: the analysis is slower at the lower rate.
	const uint64_t at_30 = kSecond / 30;
	const auto analysis = [](uint32_t divisor) { return divisor > 1 ? 34. : 22.; };
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 5, at_30, analysis, 3);
	REQUIRE(rate.divisor() == 1);
	run_disturbed(rate, now, 4, at_30, 34);
	REQUIRE(rate.divisor() == 2);
	CHECK(run_rate(rate, now, 20, at_30, analysis, 3) == 1);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("An analysis that nearly fits is attempted and kept while its results arrive", "[pacing]")
{
	const uint64_t at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	CHECK(run_rate(rate, now, 60, at_60, 16, 5) == 1);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("A rate whose results could not arrive is never attempted", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30, at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate cpu;
	CHECK(run_rate(cpu, now, 300, at_30, 79, 3) == 0);
	CHECK(cpu.divisor() == 3);
	beauti::AnalysisRate fast_canvas;
	CHECK(run_rate(fast_canvas, now, 300, at_60, 19, 3) == 0);
	CHECK(fast_canvas.divisor() == 2);
}

TEST_CASE("Attempts that keep failing become rare", "[pacing]")
{
	// The lower rate looks as if the higher one could deliver, but there the analysis misses its ticks.
	const uint64_t at_30 = kSecond / 30;
	const auto analysis = [](uint32_t divisor) { return divisor > 1 ? 34. : 40.; };
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	const unsigned rises = run_rate(rate, now, 300, at_30, analysis, 3);
	CHECK(rises >= 2);
	CHECK(rises <= 5); // waits of 10, 20, 40, 80, and 160 seconds
	CHECK(run_rate(rate, now, 300, at_30, analysis, 3) <= 1);
}

TEST_CASE("An increase that holds for a minute clears the wait", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	double at_full_rate = 40;
	const auto analysis = [&](uint32_t divisor) { return divisor > 1 ? 34. : at_full_rate; };
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 120, at_30, analysis, 3); // several failed attempts
	REQUIRE(rate.divisor() == 2);
	at_full_rate = 22;
	run_rate(rate, now, 400, at_30, analysis, 3);
	REQUIRE(rate.divisor() == 1);
	run_disturbed(rate, now, 4, at_30, 34);
	REQUIRE(rate.divisor() == 2);
	run_rate(rate, now, 15, at_30, analysis, 3);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("Loading and the first slow results after a start do not lower the rate", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	// The engine loads: no results.
	for (int tick = 0; tick < 36; ++tick)
		rate.tick(now += at_30, at_30, 3);
	// No face in the picture yet: results without an analysis.
	for (int tick = 0; tick < 60; ++tick) {
		rate.delivered();
		rate.tick(now += at_30, at_30, 3);
	}
	// The first analyses with a face are slow and miss ticks.
	run_disturbed(rate, now, 1.5, at_30, 35);
	CHECK(rate.divisor() == 1);
	run_rate(rate, now, 20, at_30, 22, 3);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("A source that was hidden does not lower the rate when it returns", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 5, at_30, 22, 3);
	now += 200 * kSecond; // no ticks; the engine has released its models meanwhile
	run_disturbed(rate, now, 1.5, at_30, 35);
	run_rate(rate, now, 20, at_30, 22, 3);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("Results that miss their ticks lower the rate", "[pacing]")
{
	const uint64_t at_60 = kSecond / 60;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	rate.tick(now, at_60, 0);
	run_rate(rate, now, 12, at_60, 8, 3);
	REQUIRE(rate.divisor() == 1);
	for (int tick = 0; tick < 4 * 60; ++tick) {
		now += at_60;
		if (tick % 3)
			rate.delivered();
		rate.analyzed(8);
		rate.tick(now, at_60, 3);
	}
	CHECK(rate.divisor() == 2);
}

TEST_CASE("Without results the automatic rate holds", "[pacing]")
{
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 3, at_30, 15, 6);
	REQUIRE(rate.divisor() == 1);
	for (int tick = 0; tick < 30 * 30; ++tick) {
		now += at_30;
		rate.tick(now, at_30, 6);
	}
	CHECK(rate.divisor() == 1);
}

TEST_CASE("Loading the engine of a new context does not lower the automatic rate", "[pacing]")
{
	// A setting change drops the old results, then a cached engine loads in less than two
	// evaluation windows. Depending on where that pause falls, both windows around it hold a
	// few results.
	const uint64_t at_30 = kSecond / 30;
	bool lowered_without_restart = false;
	for (int offset = 0; offset < 31; ++offset) {
		for (const bool restart : {false, true}) {
			uint64_t now = kSecond;
			beauti::AnalysisRate rate;
			run_rate(rate, now, 12 + offset / 30.0, at_30, 15, 6);
			REQUIRE(rate.divisor() == 1);
			if (restart)
				rate.restart();
			for (int tick = 0; tick < 50; ++tick) {
				now += at_30;
				rate.tick(now, at_30, 6);
			}
			run_rate(rate, now, 3, at_30, 15, 6);
			if (restart)
				CHECK(rate.divisor() == 1);
			else
				lowered_without_restart = lowered_without_restart || rate.divisor() == 2;
		}
	}
	// Without the restart some offsets halve the rate, so the case does reproduce the problem.
	CHECK(lowered_without_restart);
}

TEST_CASE("A new context starts the automatic rate over", "[pacing]")
{
	// A slow device holds only a few analyses per second; after a switch to a fast one the rate
	// must not keep that divisor.
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 60, at_30, 550, 2);
	REQUIRE(rate.divisor() >= 10);
	rate.restart();
	run_rate(rate, now, 5, at_30, 20, 2);
	CHECK(rate.divisor() == 1);
	CHECK(std::string_view(rate.reason) == "new context");
}

TEST_CASE("Whole results do not keep a low rate from rising", "[pacing]")
{
	// Without a new context the rate rises one step at a time once the analysis is fast. At a
	// few analyses per second a one-second window holds one result more or less by rounding.
	const uint64_t at_30 = kSecond / 30;
	uint64_t now = kSecond;
	beauti::AnalysisRate rate;
	run_rate(rate, now, 60, at_30, 550, 2);
	const uint32_t low = rate.divisor();
	REQUIRE(low >= 10);
	CHECK(low <= 17); // not below the 1.8 analyses per second that 550 ms deliver
	run_rate(rate, now, 240, at_30, 20, 2);
	CHECK(rate.divisor() <= low - 8);
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
	run_rate(rate, now, 4, at_60, 8, 15);
	REQUIRE(rate.divisor() == 2);
	run_rate(rate, now, 15, at_60, 8, 3);
	CHECK(rate.divisor() == 2);
	run_rate(rate, now, 10, at_60, 8, 3);
	CHECK(rate.divisor() == 1);
}

TEST_CASE("Paused work stops two seconds after its control reaches zero", "[pacing]")
{
	beauti::PauseTimer timer;
	CHECK_FALSE(timer.active(false, kSecond));
	CHECK(timer.active(true, kSecond));
	CHECK(timer.active(false, 2 * kSecond));
	CHECK(timer.active(false, 3 * kSecond - 1));
	CHECK_FALSE(timer.active(false, 3 * kSecond));
	CHECK(timer.active(true, 4 * kSecond));
}
