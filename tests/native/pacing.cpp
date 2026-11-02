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
