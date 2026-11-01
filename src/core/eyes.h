// SPDX-License-Identifier: GPL-3.0-or-later
// Eye crop geometry and conservative masks, independent of inference and graphics.
#pragma once
#include "frame.h"
#include <array>

namespace beauti {
struct EyePoint {
	float x = 0, y = 0;
};
struct EyeCrop {
	EyePoint center;
	float side = 0, angle = 0;
	EyePoint project(float x, float y, int size) const;
};
// The eye map covers the analyzed face box at this resolution (R=iris, G=sclera).
inline constexpr uint32_t kEyeMapSize = 512;
using EyeContour = std::array<EyePoint, 16>;
using IrisContour = std::array<EyePoint, 5>;
inline constexpr int kEyeContours[2][16] = {
	{33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246},
	{263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466}};
void sample_eye_crop(const Frame &frame, const EyeCrop &crop, int size, bool flip, std::vector<float> &input);
float eye_openness(const EyeContour &lid);
void raster_eye(const Frame &frame, const EyeContour &lid, const IrisContour &iris, Maps &maps);
// Clears R and G of a raw size x size mask over the face box wherever FaceMesh places
// eyes, brows, or lips (plus margin pixels). points are the 478 FaceMesh landmarks.
void exclude_features(const Frame &frame, const std::vector<EyePoint> &points, uint32_t size, float margin,
		      std::vector<float> &mask);
}
