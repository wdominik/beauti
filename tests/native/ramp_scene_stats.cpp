// SPDX-License-Identifier: GPL-3.0-or-later
// Contract of the strength ramp, the scene-cut signal, and the live statistics.
#include "live_stats.h"
#include "ramp.h"
#include "scene_cut.h"
#include <catch_amalgamated.hpp>
#include <vector>

namespace {
using beauti::kStrengthRampNs;
constexpr uint32_t kW = 64, kH = 36;

std::vector<uint8_t> flat(uint8_t value)
{
	return std::vector<uint8_t>(size_t(kW) * kH * 4, value);
}
}

TEST_CASE("A ramp reaches its target after 150 ms along a smoothstep", "[ramp]")
{
	beauti::StrengthRamp ramp;
	CHECK(ramp.sample(100, 1000) == 0);
	CHECK(ramp.sample(100, 1000 + kStrengthRampNs / 2) == Catch::Approx(50));
	CHECK(ramp.sample(100, 1000 + kStrengthRampNs) == 100);
	CHECK(ramp.sample(100, 1000 + 5 * kStrengthRampNs) == 100);
}

TEST_CASE("A retargeted ramp continues from its current value", "[ramp]")
{
	beauti::StrengthRamp ramp;
	ramp.sample(100, 1000);
	const float middle = ramp.sample(0, 1000 + kStrengthRampNs / 2);
	CHECK(middle == Catch::Approx(50));
	const float next = ramp.sample(0, 1000 + kStrengthRampNs / 2 + 1000);
	CHECK(next == Catch::Approx(middle).margin(.01));
	CHECK(ramp.sample(0, 1000 + kStrengthRampNs / 2 + kStrengthRampNs) == 0);
}

TEST_CASE("A clock running backward finishes the ramp", "[ramp]")
{
	beauti::StrengthRamp ramp;
	ramp.sample(100, 1000);
	CHECK(ramp.sample(100, 500) == 100);
}

TEST_CASE("A scene cut needs a previous frame and a large change", "[scene-cut]")
{
	beauti::SceneCut cut;
	const auto dark = flat(10), bright = flat(240), near = flat(60);
	CHECK_FALSE(cut.observe(bright.data(), kW, kH));
	CHECK_FALSE(cut.observe(bright.data(), kW, kH));
	CHECK(cut.observe(dark.data(), kW, kH));
	CHECK_FALSE(cut.observe(near.data(), kW, kH));
}

TEST_CASE("A change in a part of the frame is no scene cut", "[scene-cut]")
{
	beauti::SceneCut cut;
	const auto base = flat(10);
	auto changed = base;
	std::fill(changed.begin(), changed.begin() + std::ptrdiff_t(changed.size() / 2), uint8_t(240));
	CHECK_FALSE(cut.observe(base.data(), kW, kH));
	CHECK_FALSE(cut.observe(changed.data(), kW, kH));
}

TEST_CASE("Histogram percentiles report the bucket's upper edge", "[live-stats]")
{
	beauti::MsHistogram histogram;
	CHECK(histogram.percentile(.5) == 0);
	for (int i = 0; i < 100; ++i)
		histogram.add(i);
	CHECK(histogram.count() == 100);
	CHECK(histogram.percentile(.5) == 50);
	CHECK(histogram.percentile(.95) == 95);
	CHECK(histogram.maximum() == 99);
	histogram.add(1e9);
	CHECK(histogram.maximum() == 1e9);
	histogram.add(-5);
	CHECK(histogram.count() == 102);
}

TEST_CASE("Live statistics separate new, repeated, and unprocessed output", "[live-stats]")
{
	beauti::LiveStats stats;
	constexpr uint64_t ms = 1000000;
	stats.tick(100 * ms, false, 0, false, "startup");
	stats.accept(20);
	stats.tick(133 * ms, true, 60, true, "processed");
	stats.tick(166 * ms, true, 90, true, "processed");
	const auto text = stats.summary();
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("ticks=3 processed=2 passthrough=1 accepted=1 repeated=1"));
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("startup_ms=33"));
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("age_max=90"));
}

TEST_CASE("Live statistics count losses by reason and time the recovery", "[live-stats]")
{
	beauti::LiveStats stats;
	constexpr uint64_t ms = 1000000;
	stats.tick(0 + ms, true, 10, true, "processed");
	stats.result(false, false);
	stats.result(true, false);
	stats.tick(100 * ms, false, 0, false, "confirmed_no_face");
	stats.tick(133 * ms, false, 0, false, "confirmed_no_face");
	stats.tick(300 * ms, true, 10, true, "processed");
	const auto text = stats.summary();
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("results=2 no_face=1 bridged=1"));
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("losses=1"));
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("loss_confirmed_no_face=1"));
	CHECK_THAT(text, Catch::Matchers::ContainsSubstring("recovery_max=200"));
}

TEST_CASE("A report is due every ten seconds", "[live-stats]")
{
	beauti::LiveStats stats;
	const uint64_t start = 5000;
	CHECK_FALSE(stats.report_due(start));
	stats.tick(start, false, 0, false, "startup");
	CHECK_FALSE(stats.report_due(start + beauti::LiveStats::report_interval - 1));
	CHECK(stats.report_due(start + beauti::LiveStats::report_interval));
	stats.reported(start + beauti::LiveStats::report_interval);
	CHECK_FALSE(stats.report_due(start + beauti::LiveStats::report_interval + 1));
}
