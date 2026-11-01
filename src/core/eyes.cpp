// SPDX-License-Identifier: GPL-3.0-or-later
// Rotated RGB crops and inset lid/iris masks. Closed or implausible eyes fail closed.
#include "eyes.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace beauti {
namespace {
// Eyes narrower than this many analysis pixels carry too little detail for a reliable lid.
constexpr float kMinEyeWidth = 12.f;
// Lid area / width^2: about .075 for a nearly closed eye, .16 and above for an open one.
constexpr float kClosedLidRatio = .075f, kOpenLidRatio = .16f;
// Plausible iris: radii of at least 2 px and at most 35% of eye width, aspect within 2:1,
// center within 30% of a radius from the rim midpoints.
constexpr float kMinIrisRadius = 2.f, kMaxIrisWidthFraction = .35f, kMinIrisAspect = .5f, kIrisCenterTolerance = .3f;
// Radial profile in iris radii: dark pupil below .3, full ring .55-.8, sclera from 1.08-1.3.
constexpr float kPupilEdge = .3f, kRingStart = .55f, kRingEnd = .8f, kSclera = 1.08f, kScleraFull = 1.3f;
// Inward lid feather: at least 1.5 px, otherwise 6% of eye width.
constexpr float kLidFeatherMin = 1.5f, kLidFeatherFraction = .06f;
float distance(EyePoint a, EyePoint b)
{
	return std::hypot(a.x - b.x, a.y - b.y);
}
float ramp(float low, float high, float value)
{
	float t = std::clamp((value - low) / (high - low), 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}
// Signed distance to a simple polygon; negative outside. Feathering only inward
// avoids adding brightness to eyelashes or skin outside the predicted lid.
float lid_distance(const EyeContour &lid, EyePoint p)
{
	bool inside = false;
	float result = std::numeric_limits<float>::max();
	for (size_t i = 0, j = lid.size() - 1; i < lid.size(); j = i++) {
		const auto a = lid[j], b = lid[i];
		float dx = b.x - a.x, dy = b.y - a.y;
		float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(dx * dx + dy * dy, .0001f), 0.f,
				     1.f);
		result = std::min(result, distance(p, {a.x + t * dx, a.y + t * dy}));
		if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
			inside = !inside;
	}
	return inside ? result : -result;
}
float channel(const Frame &f, int x, int y, int c)
{
	if (x < 0 || y < 0 || x >= int(f.width) || y >= int(f.height))
		return 0;
	const auto *p = &f.pixels[(size_t(y) * f.width + size_t(x)) * 4];
	return p[3] ? std::min(1.f, float(p[c]) / float(p[3])) : 0.f;
}
}
namespace {
// FaceMesh topology: both eye contours, both brows (upper edge, then lower edge back),
// and the outer lip contour.
const std::vector<std::vector<int>> kFeatureContours = {
	{33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246},
	{263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466},
	{70, 63, 105, 66, 107, 55, 65, 52, 53, 46},
	{300, 293, 334, 296, 336, 285, 295, 282, 283, 276},
	{61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185},
};
// Signed distance to a simple polygon; positive inside.
float polygon_distance(const std::vector<EyePoint> &polygon, EyePoint p)
{
	bool inside = false;
	float result = std::numeric_limits<float>::max();
	for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
		const auto a = polygon[j], b = polygon[i];
		float dx = b.x - a.x, dy = b.y - a.y;
		float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / std::max(dx * dx + dy * dy, .0001f), 0.f,
				     1.f);
		result = std::min(result, distance(p, {a.x + t * dx, a.y + t * dy}));
		if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + dx * (p.y - a.y) / dy)
			inside = !inside;
	}
	return inside ? result : -result;
}
}
void exclude_features(const Frame &f, const std::vector<EyePoint> &points, uint32_t size, float margin,
		      std::vector<float> &mask)
{
	const float sx = float(size) / float(f.box[2] - f.box[0]), sy = float(size) / float(f.box[3] - f.box[1]);
	for (const auto &contour : kFeatureContours) {
		std::vector<EyePoint> polygon;
		float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
		for (int index : contour) {
			// Mask-grid coordinates with pixel centers at +0.5.
			EyePoint p{(points[size_t(index)].x - float(f.box[0])) * sx,
				   (points[size_t(index)].y - float(f.box[1])) * sy};
			polygon.push_back(p);
			left = std::min(left, p.x);
			right = std::max(right, p.x);
			top = std::min(top, p.y);
			bottom = std::max(bottom, p.y);
		}
		const int x0 = std::clamp(int(std::floor(left - margin)), 0, int(size));
		const int x1 = std::clamp(int(std::ceil(right + margin)), 0, int(size));
		const int y0 = std::clamp(int(std::floor(top - margin)), 0, int(size));
		const int y1 = std::clamp(int(std::ceil(bottom + margin)), 0, int(size));
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
				if (polygon_distance(polygon, {float(x) + .5f, float(y) + .5f}) >= -margin) {
					const size_t i = (size_t(y) * size + size_t(x)) * 4;
					mask[i] = mask[i + 1] = 0;
				}
	}
}
EyePoint EyeCrop::project(float x, float y, int size) const
{
	x = (x / float(size) - .5f) * side;
	y = (y / float(size) - .5f) * side;
	return {center.x + std::cos(angle) * x - std::sin(angle) * y,
		center.y + std::sin(angle) * x + std::cos(angle) * y};
}
void sample_eye_crop(const Frame &f, const EyeCrop &crop, int size, bool flip, std::vector<float> &input)
{
	input.resize(size_t(size) * size_t(size) * 3);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) {
			// Crop and image coordinates are continuous with pixel centers at +0.5, the
			// MediaPipe landmark convention; mirroring u therefore maps to size - u.
			auto p = crop.project(float(flip ? size - 1 - x : x) + .5f, float(y) + .5f, size);
			p = {p.x - .5f, p.y - .5f};
			int ix = int(std::floor(p.x)), iy = int(std::floor(p.y));
			float fx = p.x - float(ix), fy = p.y - float(iy);
			for (int c = 0; c < 3; ++c) {
				float top = std::lerp(channel(f, ix, iy, c), channel(f, ix + 1, iy, c), fx);
				float bottom = std::lerp(channel(f, ix, iy + 1, c), channel(f, ix + 1, iy + 1, c), fx);
				input[(size_t(c) * size_t(size) + size_t(y)) * size_t(size) + size_t(x)] =
					std::lerp(top, bottom, fy);
			}
		}
}
float eye_openness(const EyeContour &lid)
{
	float width = distance(lid[0], lid[8]), area = 0;
	for (size_t i = 0; i < lid.size(); ++i) {
		auto a = lid[i], b = lid[(i + 1) % lid.size()];
		if (!std::isfinite(a.x) || !std::isfinite(a.y))
			return 0;
		area += a.x * b.y - b.x * a.y;
	}
	if (width < kMinEyeWidth)
		return 0;
	return ramp(kClosedLidRatio, kOpenLidRatio, std::abs(area) * .5f / (width * width));
}
void raster_eye(const Frame &f, const EyeContour &lid, const IrisContour &iris, Maps &maps)
{
	const float open = eye_openness(lid), width = distance(lid[0], lid[8]);
	if (open <= 0)
		return;
	for (auto p : iris)
		if (!std::isfinite(p.x) || !std::isfinite(p.y))
			return;
	const float rx = distance(iris[1], iris[3]) * .5f, ry = distance(iris[2], iris[4]) * .5f;
	if (std::min(rx, ry) < kMinIrisRadius || std::max(rx, ry) > width * kMaxIrisWidthFraction ||
	    std::min(rx, ry) < std::max(rx, ry) * kMinIrisAspect || lid_distance(lid, iris[0]) <= 0 ||
	    distance(iris[0], {(iris[1].x + iris[3].x) * .5f, (iris[1].y + iris[3].y) * .5f}) >
		    rx * kIrisCenterTolerance ||
	    distance(iris[0], {(iris[2].x + iris[4].x) * .5f, (iris[2].y + iris[4].y) * .5f}) >
		    ry * kIrisCenterTolerance)
		return;
	const float angle = std::atan2(iris[1].y - iris[3].y, iris[1].x - iris[3].x);
	float left = lid[0].x, right = left, top = lid[0].y, bottom = top;
	for (auto p : lid) {
		left = std::min(left, p.x);
		right = std::max(right, p.x);
		top = std::min(top, p.y);
		bottom = std::max(bottom, p.y);
	}
	constexpr int size = int(kEyeMapSize);
	const float sx = float(size) / float(f.box[2] - f.box[0]), sy = float(size) / float(f.box[3] - f.box[1]);
	const int x0 = std::clamp(int(std::floor((left - float(f.box[0])) * sx)), 0, size);
	const int x1 = std::clamp(int(std::ceil((right - float(f.box[0])) * sx)), 0, size);
	const int y0 = std::clamp(int(std::floor((top - float(f.box[1])) * sy)), 0, size);
	const int y1 = std::clamp(int(std::ceil((bottom - float(f.box[1])) * sy)), 0, size);
	if (maps.eye_size == 0) {
		maps.eye_size = size;
		maps.eyes.assign(size * size * 4, 0.f);
	}
	for (int y = y0; y < y1; ++y)
		for (int x = x0; x < x1; ++x) {
			EyePoint p{float(f.box[0]) + (float(x) + .5f) / sx, float(f.box[1]) + (float(y) + .5f) / sy};
			float edge = open * ramp(.5f, std::max(kLidFeatherMin, width * kLidFeatherFraction),
						 lid_distance(lid, p));
			float dx = p.x - iris[0].x, dy = p.y - iris[0].y;
			float r = std::hypot((std::cos(angle) * dx + std::sin(angle) * dy) / rx,
					     (-std::sin(angle) * dx + std::cos(angle) * dy) / ry);
			size_t index = (size_t(y) * size + size_t(x)) * 4;
			maps.eyes[index] = std::max(maps.eyes[index], edge * ramp(kPupilEdge, kRingStart, r) *
									      (1.f - ramp(kRingEnd, 1.f, r)));
			maps.eyes[index + 1] = std::max(maps.eyes[index + 1], edge * ramp(kSclera, kScleraFull, r));
		}
}
}
