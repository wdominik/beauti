// SPDX-License-Identifier: GPL-3.0-or-later
// Contract of the render-thread continuity policy (src/core/continuity.h):
// hold deadlines, miss handling, recovery, and the delivery-interval measurement.
#include "continuity.h"
#include <catch_amalgamated.hpp>
#include <algorithm>

namespace {
using beauti::Continuity;
constexpr auto m = Continuity::ms;

// A policy that accepted its first good result at 100 ms with the given runtime.
Continuity primed(double runtime)
{
	Continuity c;
	REQUIRE_FALSE(c.visible(1));
	REQUIRE(c.accept(1, 1, 100 * m, runtime, true));
	return c;
}

// The policy after a stall at the first deadline and the two-result recovery.
Continuity recovered(double runtime)
{
	Continuity c = primed(runtime);
	const auto deadline = c.deadline;
	REQUIRE_FALSE(c.accept(2, deadline, deadline + 1, runtime, true));
	REQUIRE(c.accept(3, deadline + 1, deadline + 2, runtime, true));
	return c;
}
}

TEST_CASE("A good result is shown from acceptance until its hold deadline", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c = primed(runtime);
	CHECK(c.visible(300 * m)); // A 200 ms source-age cutoff would drop this frame.
	const auto deadline = c.deadline;
	CHECK(c.visible(deadline - 1));
	CHECK_FALSE(c.visible(deadline));
}

TEST_CASE("After a stall, two good results restore the output and older sequences are ignored", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c = primed(runtime);
	const auto deadline = c.deadline;
	CHECK_FALSE(c.accept(2, deadline, deadline + 1, runtime, true)); // first result after the stall: recovering
	CHECK(c.accept(3, deadline + 1, deadline + 2, runtime, true));
	CHECK_FALSE(c.accept(2, deadline, deadline + 3, runtime, true)); // out of order
}

TEST_CASE("One missed face holds the last result; a second miss confirms no face", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c = recovered(runtime);
	const auto base = c.completed;
	CHECK_FALSE(c.accept(4, base + 2, base + 3, runtime, false));
	CHECK(c.visible(base + 4));
	CHECK(c.accept(5, base + 3, base + 5, runtime, true));
	CHECK_FALSE(c.accept(6, base + 4, base + 6, runtime, false));
	CHECK_FALSE(c.accept(7, base + 5, base + 7, runtime, false));
	CHECK_FALSE(c.visible(base + 8));
}

TEST_CASE("A single miss ends at the earlier of the hold deadline and the miss budget", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c = primed(runtime);
	CHECK_FALSE(c.accept(2, 100 * m, 120 * m, runtime, false));
	const auto end = std::min(c.deadline, 120 * m + c.miss_budget);
	CHECK(c.visible(end - 1));
	CHECK_FALSE(c.visible(end));
}

TEST_CASE("Results older than the maximum age or captured in the future are rejected", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c;
	CHECK_FALSE(c.accept(1, 1, 6000 * m, runtime, true));
	CHECK_FALSE(c.visible(6000 * m));
	CHECK_FALSE(c.accept(2, 7000 * m, 6000 * m, runtime, true));
}

TEST_CASE("An engine failure drops the output until two good results arrive", "[continuity]")
{
	const double runtime = GENERATE(30., 100., 600., 3000.);
	CAPTURE(runtime);
	Continuity c = primed(runtime);
	c.fail("resource_failure");
	CHECK_FALSE(c.visible(101 * m));
	CHECK_FALSE(c.accept(2, 100 * m, 110 * m, runtime, true));
	CHECK(c.accept(3, 110 * m, 120 * m, runtime, true));
	c.reset();
	CHECK_FALSE(c.visible(121 * m));
}

TEST_CASE("The hold follows a slow analysis cadence and shrinks when the cadence changes", "[continuity]")
{
	Continuity slow;
	slow.set_cadence(1000);
	REQUIRE(slow.accept(1, 1, 100 * m, 20, true));
	CHECK(slow.visible(1100 * m));
	const auto deadline = slow.deadline;
	slow.set_cadence(1000); // unchanged cadence keeps the deadline
	CHECK(slow.deadline == deadline);
	CHECK_FALSE(slow.accept(2, 1000 * m, 1100 * m, 20, false));
	CHECK(slow.visible(1900 * m));
	CHECK(slow.accept(3, 2000 * m, 2120 * m, 20, true));
	slow.set_cadence(1000.0 / 30);
	CHECK(slow.deadline == 2470 * m);
	CHECK_FALSE(slow.visible(2470 * m));
}

TEST_CASE("The hold widens with the observed delivery interval; a cadence change forgets it", "[continuity]")
{
	// Results delivered later than the engine time suggests (scheduling delay)
	// widen the hold from the observed interval.
	Continuity late;
	late.set_cadence(1000.0 / 30);
	REQUIRE(late.accept(1, 1, 100 * m, 100, true));
	CHECK(late.deadline == 500 * m);
	CHECK(late.accept(2, 380 * m, 480 * m, 100, true));
	CHECK(late.deadline == 1440 * m);
	CHECK(late.accept(3, 900 * m, 940 * m, 100, true));
	CHECK(late.deadline == 2060 * m);
	late.set_cadence(1000.0 / 60);
	CHECK(late.deadline == 1340 * m);
	CHECK_FALSE(late.visible(1340 * m));
	CHECK_FALSE(late.accept(4, 1300 * m, 1350 * m, 100, true));
	CHECK(late.accept(5, 1400 * m, 1460 * m, 100, true));
	CHECK(late.deadline == 1860 * m);
}
