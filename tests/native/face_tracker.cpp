// SPDX-License-Identifier: GPL-3.0-or-later
// Contract of the face region policy (src/core/face_tracker.h).
#include "face_tracker.h"
#include <catch_amalgamated.hpp>
#include <vector>

namespace {
using beauti::FaceBox;
using beauti::FaceTracker;
constexpr uint64_t ms = FaceTracker::ms;
constexpr uint32_t kW = 1280, kH = 720;

// A square of landmarks, centered and with the given extent.
std::vector<beauti::EyePoint> face(float x, float y, float extent)
{
	const float h = extent / 2;
	return {{x - h, y - h}, {x + h, y - h}, {x - h, y + h}, {x + h, y + h}};
}

// A tracker following a 200 px face at (640, 360), last updated at the given time.
FaceTracker following(uint64_t now = 1000 * ms)
{
	FaceTracker tracker;
	tracker.begin(now, kW, kH, 1);
	const auto points = face(640, 360, 200);
	REQUIRE(tracker.update(now, points, points[0], points[1]));
	return tracker;
}
}

TEST_CASE("Landmarks give the crop of the landmark model and the analysis box", "[face-tracker]")
{
	const auto tracker = following();
	CHECK(tracker.tracking());
	CHECK(tracker.region().center.x == 640);
	CHECK(tracker.region().center.y == 360);
	CHECK(tracker.region().side == Catch::Approx(300));
	CHECK(tracker.region().angle == Catch::Approx(0));
	const auto box = tracker.box();
	CHECK(box.left == 465);
	CHECK(box.top == 185);
	CHECK(box.right == 815);
	CHECK(box.bottom == 535);
}

TEST_CASE("A region stays square when it reaches beyond the image", "[face-tracker]")
{
	FaceTracker tracker;
	tracker.begin(ms, kW, kH, 1);
	const auto points = face(640, 360, 700);
	REQUIRE(tracker.update(ms, points, points[0], points[1]));
	const auto box = tracker.box();
	CHECK(box.top < 0);
	CHECK(box.bottom > int(kH));
	CHECK(box.right - box.left == box.bottom - box.top);
	CHECK(tracker.region().side == Catch::Approx(1050));
}

TEST_CASE("The region of a face larger than the image is bounded", "[face-tracker]")
{
	FaceTracker tracker;
	tracker.begin(ms, kW, kH, 1);
	const auto points = face(640, 360, 3000);
	REQUIRE(tracker.update(ms, points, points[0], points[1]));
	CHECK(tracker.box().right - tracker.box().left == int(float(kW) * FaceTracker::max_box_share));
}

TEST_CASE("Rotation of the crop comes from the eye corners", "[face-tracker]")
{
	FaceTracker tracker;
	tracker.begin(ms, kW, kH, 1);
	const auto points = face(640, 360, 200);
	REQUIRE(tracker.update(ms, points, {0, 0}, {1, 1}));
	CHECK(tracker.region().angle == Catch::Approx(std::atan(1.f)));
}

TEST_CASE("Landmarks that leave too little of the face inside the image are refused", "[face-tracker]")
{
	FaceTracker tracker;
	tracker.begin(ms, kW, kH, 1);
	const auto points = face(-200, 360, 200);
	CHECK_FALSE(tracker.update(ms, points, points[0], points[1]));
	CHECK_FALSE(tracker.tracking());
}

TEST_CASE("A region that barely moved is kept, a moving one follows", "[face-tracker]")
{
	auto tracker = following();
	const auto box = tracker.box();
	const auto still = face(641.5f, 361.5f, 201);
	REQUIRE(tracker.update(1033 * ms, still, still[0], still[1]));
	CHECK(tracker.box().left == box.left);
	CHECK(tracker.region().center.x == 640);
	CHECK(tracker.may_bridge(1033 * ms + FaceTracker::bridge));
	const auto moved = face(650, 360, 200);
	REQUIRE(tracker.update(1066 * ms, moved, moved[0], moved[1]));
	CHECK(tracker.box().left == box.left + 10);
	CHECK(tracker.region().center.x == 650);
	const auto larger = face(650, 360, 210);
	REQUIRE(tracker.update(1100 * ms, larger, larger[0], larger[1]));
	CHECK(tracker.region().side == Catch::Approx(315));
	const auto turned = face(650, 360, 210);
	REQUIRE(tracker.update(1133 * ms, turned, {0, 0}, {10, 1}));
	CHECK(tracker.region().angle == Catch::Approx(std::atan(.1f)));
}

TEST_CASE("A short failure is bridged, a longer one is not", "[face-tracker]")
{
	const auto tracker = following();
	CHECK(tracker.may_bridge(1000 * ms + FaceTracker::bridge));
	CHECK_FALSE(tracker.may_bridge(1000 * ms + FaceTracker::bridge + 1));
	CHECK_FALSE(FaceTracker{}.may_bridge(1000 * ms));
}

TEST_CASE("A slow analysis rate still bridges one analysis", "[face-tracker]")
{
	auto tracker = following();
	tracker.begin(1800 * ms, kW, kH, 1);
	const auto points = face(640, 360, 200);
	REQUIRE(tracker.update(1800 * ms, points, points[0], points[1]));
	tracker.begin(2600 * ms, kW, kH, 1);
	CHECK(tracker.may_bridge(2600 * ms));
	CHECK_FALSE(tracker.may_bridge(1800 * ms + 1001 * ms));
}

TEST_CASE("A new scene, another image size, or a long pause forgets the face", "[face-tracker]")
{
	auto scene = following();
	scene.begin(1033 * ms, kW, kH, 2);
	CHECK_FALSE(scene.tracking());
	auto size = following();
	size.begin(1033 * ms, 640, kH, 1);
	CHECK_FALSE(size.tracking());
	auto pause = following();
	pause.begin(1000 * ms + FaceTracker::stale_gap + 1, kW, kH, 1);
	CHECK_FALSE(pause.tracking());
	auto backward = following();
	backward.begin(500 * ms, kW, kH, 1);
	CHECK_FALSE(backward.tracking());
	auto next = following();
	next.begin(1033 * ms, kW, kH, 1);
	CHECK(next.tracking());
}

TEST_CASE("Only a recent, barely moving face counts as settled", "[face-tracker]")
{
	CHECK_FALSE(FaceTracker{}.settled(1000 * ms));
	auto tracker = following();
	CHECK(tracker.settled(1033 * ms));
	CHECK(tracker.settled(1000 * ms + FaceTracker::settled_gap));
	CHECK_FALSE(tracker.settled(1000 * ms + FaceTracker::settled_gap + 1));
	const auto small_step = face(650, 360, 200);
	REQUIRE(tracker.update(1033 * ms, small_step, small_step[0], small_step[1]));
	CHECK(tracker.settled(1066 * ms));
	const auto fast = face(700, 360, 200);
	REQUIRE(tracker.update(1066 * ms, fast, fast[0], fast[1]));
	CHECK_FALSE(tracker.settled(1100 * ms));
	const auto still = fast;
	REQUIRE(tracker.update(1100 * ms, still, still[0], still[1]));
	CHECK(tracker.settled(1133 * ms));
}

TEST_CASE("The detector looks for another face about once per second", "[face-tracker]")
{
	auto tracker = following();
	CHECK_FALSE(tracker.detector_due(1000 * ms + FaceTracker::detector_interval - 1));
	CHECK(tracker.detector_due(1000 * ms + FaceTracker::detector_interval));
	tracker.larger_face_seen(2000 * ms, nullptr);
	CHECK_FALSE(tracker.detector_due(2500 * ms));
}

TEST_CASE("Only a separate, clearly larger face takes over, and only when it stays", "[face-tracker]")
{
	auto tracker = following();
	const FaceBox larger{0, 0, 460, 460}, smaller{0, 0, 200, 200}, same_place{400, 100, 900, 600};
	CHECK_FALSE(tracker.larger_face_seen(2000 * ms, &smaller));
	CHECK_FALSE(tracker.larger_face_seen(3000 * ms, &same_place));
	CHECK_FALSE(tracker.larger_face_seen(4000 * ms, &larger));
	CHECK_FALSE(tracker.larger_face_seen(5000 * ms, nullptr));
	CHECK_FALSE(tracker.larger_face_seen(6000 * ms, &larger));
	CHECK(tracker.larger_face_seen(7000 * ms, &larger));
	CHECK_FALSE(tracker.larger_face_seen(8000 * ms, &larger));
}

TEST_CASE("Overlap of two regions", "[face-tracker]")
{
	const FaceBox a{0, 0, 100, 100}, b{50, 0, 150, 100}, c{200, 200, 300, 300};
	CHECK(beauti::box_iou(a, a) == 1);
	CHECK(beauti::box_iou(a, b) == Catch::Approx(1.f / 3));
	CHECK(beauti::box_iou(a, c) == 0);
}
