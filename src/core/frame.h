// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace beauti {
// One analysis request and its result. Nonzero retouch/smooth request the blend
// map / skin mask; eyes requests optional eye masks. Strengths are applied by the GPU compositor.
// backend is the requested Backend, used_backend the Backend the engine ran.
// status 0 = face analyzed, 1 = no face found. box is the analyzed face region.
struct Frame {
	uint32_t width = 0, height = 0, sequence = 0, retouch = 100, smooth = 100, generation = 0, status = 0;
	uint32_t eyes = 0; // Request optional eye maps; the engine clears it when the eye models are unavailable.
	uint32_t backend = 0, used_backend = 0;
	uint64_t captured_ns = 0;
	bool landmarks_valid = true; // False when the analysis bridged a failure with the last face region.
	std::string warning;         // Nonfatal engine message, such as a disabled optional stage.
	float milliseconds = 0, detect_ms = 0, infer_ms = 0, post_ms = 0;
	int box[4] = {0, 0, 0, 0};
	std::vector<uint8_t> pixels;
};
// GPU-ready maps, interleaved RGBA floats; input pixels are never modified.
// raw_mask: R=hard-region model output, G=confidence; the compositor feathers it.
// Otherwise R=feathered weight, G=hard-region output (CPU/reference format).
struct Maps {
	uint32_t blend_size = 0, mask_size = 0, eye_size = 0;
	bool raw_mask = false;
	std::vector<float> blend, mask, eyes; // eyes: R=iris ring, G=sclera; zero outside inset lids.
};
}
