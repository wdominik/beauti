// SPDX-License-Identifier: GPL-3.0-or-later
// Which face region an analysis uses, independent of the models and of OBS. An established
// face is followed through the FaceMesh landmarks of the previous analysis, as a landmark
// tracker does. The detector only finds a face, and looks for a clearly larger one about
// once per second. A short failure is bridged with the last region on the current frame, so
// neither a blink of the model nor a brief occlusion switches the effect off. A region that
// barely moved is kept, because even a one-pixel shift of the crop changes the model outputs
// and would show as flicker on a still face. Times are ns.
#pragma once
#include "eyes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace beauti {
struct FaceBox {
	int left = 0, top = 0, right = 0, bottom = 0;
	int area() const { return std::max(0, right - left) * std::max(0, bottom - top); }
};
// Overlap of two regions in [0, 1].
inline float box_iou(const FaceBox &a, const FaceBox &b)
{
	const FaceBox shared{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
			     std::min(a.bottom, b.bottom)};
	const int both = shared.area();
	return float(both) / float(std::max(1, a.area() + b.area() - both));
}
// Extent of the landmarks: its center and its longer side.
inline void landmark_extent(const std::vector<EyePoint> &points, EyePoint &center, float &side)
{
	float left = points[0].x, right = left, top = points[0].y, bottom = top;
	for (const auto &p : points) {
		left = std::min(left, p.x);
		right = std::max(right, p.x);
		top = std::min(top, p.y);
		bottom = std::max(bottom, p.y);
	}
	center = {(left + right) * .5f, (top + bottom) * .5f};
	side = std::max(right - left, bottom - top);
}
class FaceTracker {
public:
	static constexpr uint64_t ms = 1000000;
	static constexpr uint64_t bridge = 300 * ms, stale_gap = 2000 * ms, detector_interval = 1000 * ms;
	// The landmark model's crop is 1.5 landmark extents wide, as in MediaPipe's landmarker.
	static constexpr float roi_scale = 1.5f;
	// The detector box measures 0.94 landmark extents, so its 1.86x analysis crop is 1.75 extents.
	static constexpr float box_scale = 1.75f;
	static constexpr int min_face_side = 32;
	// The region follows once the center or size moved by this share of the extent, or the
	// rotation by this many radians (about one degree).
	static constexpr float hold_share = .01f, hold_angle = .02f;
	// Above this rate of change a face counts as moving, and the region is updated before the
	// models run. The crop has room for less, so a one-analysis lag does not cut the face.
	static constexpr float moving_share = .05f;
	static constexpr uint64_t settled_gap = 110 * ms;
	// A switch needs a face that is separate from the current one, clearly larger, and seen
	// by consecutive detector checks.
	static constexpr float switch_area_ratio = 1.5f, switch_max_iou = .1f;
	static constexpr unsigned switch_checks = 2;

	// Starts one analysis. A new scene, another image size, or a long pause forgets the face.
	void begin(uint64_t now, uint32_t width, uint32_t height, uint32_t generation)
	{
		if (width != frame_w || height != frame_h || generation != frame_generation ||
		    (last_analysis && (now < last_analysis || now - last_analysis > stale_gap)))
			reset();
		if (last_analysis && now > last_analysis)
			interval = now - last_analysis;
		last_analysis = now;
		frame_w = width;
		frame_h = height;
		frame_generation = generation;
	}
	bool tracking() const { return active; }
	const EyeCrop &region() const { return roi; }
	const FaceBox &box() const { return current; }
	// Landmarks were accepted for this analysis. False when they leave too little of the face
	// inside the image; the caller then treats the analysis as failed.
	bool update(uint64_t now, const std::vector<EyePoint> &points, const EyePoint &right_corner,
		    const EyePoint &left_corner)
	{
		EyePoint center;
		float side = 0;
		landmark_extent(points, center, side);
		const float angle = std::atan2(left_corner.y - right_corner.y, left_corner.x - right_corner.x);
		if (active && std::abs(center.x - held_center.x) <= side * hold_share &&
		    std::abs(center.y - held_center.y) <= side * hold_share &&
		    std::abs(side - held_side) <= side * hold_share && std::abs(angle - roi.angle) <= hold_angle) {
			last_valid = now;
			moving = false;
			return true;
		}
		const float half = side * box_scale * .5f;
		const FaceBox box{std::max(0, int(center.x - half)), std::max(0, int(center.y - half)),
				  std::min(int(frame_w), int(center.x + half)),
				  std::min(int(frame_h), int(center.y + half))};
		if (box.right - box.left < min_face_side || box.bottom - box.top < min_face_side)
			return false;
		current = box;
		moving = active && std::max({std::abs(center.x - held_center.x), std::abs(center.y - held_center.y),
					     std::abs(side - held_side)}) > side * moving_share;
		roi = {center, side * roi_scale, angle};
		held_center = center;
		held_side = side;
		active = true;
		last_valid = now;
		if (!last_detector)
			last_detector = now;
		return true;
	}
	// Whether this failed analysis may reuse the last region. A slow analysis rate gets at
	// least one bridged analysis, so that the behavior does not depend on the device.
	bool may_bridge(uint64_t now) const
	{
		return active && now >= last_valid &&
		       now - last_valid <= std::max(bridge, std::min(interval + interval / 4, stale_gap));
	}
	// Whether the next models can run on the current region while its landmarks are computed:
	// the face barely moved at the last analysis, and that analysis was recent.
	bool settled(uint64_t now) const
	{
		return active && !moving && now >= last_valid && now - last_valid <= settled_gap;
	}
	bool detector_due(uint64_t now) const
	{
		return active && now >= last_detector && now - last_detector >= detector_interval;
	}
	// Result of a periodic detector check; true when the tracker should move to that face.
	bool larger_face_seen(uint64_t now, const FaceBox *candidate)
	{
		last_detector = now;
		const bool larger = candidate && float(candidate->area()) > float(current.area()) * switch_area_ratio &&
				    box_iou(*candidate, current) < switch_max_iou;
		larger_checks = larger ? larger_checks + 1 : 0;
		if (larger_checks < switch_checks)
			return false;
		larger_checks = 0;
		return true;
	}
	void reset()
	{
		active = moving = false;
		last_valid = last_analysis = last_detector = interval = 0;
		larger_checks = 0;
	}

private:
	EyeCrop roi;
	FaceBox current;
	EyePoint held_center;
	float held_side = 0;
	bool active = false, moving = false;
	uint32_t frame_w = 0, frame_h = 0, frame_generation = 0;
	uint64_t last_valid = 0, last_analysis = 0, last_detector = 0, interval = 0;
	unsigned larger_checks = 0;
};
}
