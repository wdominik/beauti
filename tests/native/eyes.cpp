// SPDX-License-Identifier: GPL-3.0-or-later
// Eye masks must not brighten closed lids, the pupil center, or pixels outside the eye.
#include "eyes.h"
#include <catch_amalgamated.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
beauti::EyeContour contour(float height)
{
	beauti::EyeContour lid;
	for (size_t i = 0; i < lid.size(); ++i) {
		float angle = float(i) * 6.2831853f / float(lid.size());
		lid[i] = {64.f - 30.f * std::cos(angle), 64.f + height * std::sin(angle)};
	}
	return lid;
}
beauti::Frame frame()
{
	beauti::Frame f;
	f.width = f.height = 128;
	f.box[2] = f.box[3] = 128;
	return f;
}
const beauti::IrisContour iris{{{64, 64}, {74, 64}, {64, 54}, {54, 64}, {64, 74}}};
}
TEST_CASE("Eye masks protect closed lids, pupils and surroundings", "[eyes]")
{
	auto f = frame();
	beauti::Maps maps;
	beauti::raster_eye(f, contour(12), iris, maps);
	REQUIRE(maps.eye_size == 512);
	float iris_sum = 0, white_sum = 0;
	for (uint32_t y = 0; y < 512; ++y)
		for (uint32_t x = 0; x < 512; ++x) {
			size_t i = (size_t(y) * 512 + x) * 4;
			float dx = (float(x) + .5f) / 4 - 64, dy = (float(y) + .5f) / 4 - 64;
			REQUIRE(maps.eyes[i] >= 0);
			REQUIRE(maps.eyes[i] <= 1);
			REQUIRE(maps.eyes[i + 1] >= 0);
			REQUIRE(maps.eyes[i + 1] <= 1);
			if (dx * dx / 900 + dy * dy / 144 >= 1 || std::hypot(dx, dy) <= 3) {
				REQUIRE(maps.eyes[i] == 0);
				REQUIRE(maps.eyes[i + 1] == 0);
			}
			iris_sum += maps.eyes[i];
			white_sum += maps.eyes[i + 1];
		}
	REQUIRE(iris_sum > 100);
	REQUIRE(white_sum > 100);
	for (float height : {0.f, .5f, 2.f}) {
		maps = {};
		beauti::raster_eye(f, contour(height), iris, maps);
		REQUIRE(maps.eyes.empty());
	}
}
TEST_CASE("Invalid and displaced iris predictions fail closed", "[eyes]")
{
	auto f = frame();
	for (float bad : {std::numeric_limits<float>::quiet_NaN(), 1000.f}) {
		auto invalid = iris;
		invalid[0].x = bad;
		beauti::Maps maps;
		beauti::raster_eye(f, contour(12), invalid, maps);
		REQUIRE(maps.eyes.empty());
	}
	auto lid = contour(12);
	lid[3].y = std::numeric_limits<float>::infinity();
	REQUIRE(beauti::eye_openness(lid) == 0);
}
TEST_CASE("Eye crops use RGB straight alpha, rotation, padding and mirrored sampling", "[eyes]")
{
	beauti::Frame f;
	f.width = f.height = 2;
	f.pixels = {64, 0, 0, 128, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0};
	beauti::EyeCrop crop{{1, 1}, 2, 0};
	std::vector<float> input;
	beauti::sample_eye_crop(f, crop, 2, false, input);
	REQUIRE(input[0] == .5f);
	REQUIRE(input[5] == 1);
	REQUIRE(input[10] == 1);
	beauti::sample_eye_crop(f, crop, 2, true, input);
	REQUIRE(input[0] == 0);
	REQUIRE(input[1] == .5f);
	crop.center = {10, 10};
	beauti::sample_eye_crop(f, crop, 2, false, input);
	REQUIRE(std::all_of(input.begin(), input.end(), [](float v) { return v == 0; }));
	crop = {{10, 20}, 100, 1.57079633f};
	auto p = crop.project(100, 50, 100);
	REQUIRE(p.x == Catch::Approx(10).margin(.0001));
	REQUIRE(p.y == Catch::Approx(70).margin(.0001));
}
