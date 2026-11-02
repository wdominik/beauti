// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
namespace beauti {
// Conservative scene-cut signal: a 16x16 RGB sample grid of each RGBA readback. It is
// not face tracking; subtle cuts still need detector evidence.
struct SceneCut {
	static constexpr unsigned grid = 16, samples = grid * grid * 3;
	static constexpr unsigned block_change = 240, changed_blocks = 180;
	static constexpr double mean_change = 70;
	std::array<uint8_t, samples> previous{};
	bool have_previous = false;
	// Compares a frame with the one before it, and remembers it for the next call.
	bool observe(const uint8_t *pixels, uint32_t w, uint32_t h)
	{
		std::array<uint8_t, samples> sample{};
		unsigned changed = 0;
		double delta = 0;
		for (unsigned y = 0; y < grid; ++y)
			for (unsigned x = 0; x < grid; ++x) {
				unsigned d = 0;
				for (unsigned c = 0; c < 3; ++c) {
					const unsigned i = (y * grid + x) * 3 + c;
					sample[i] = pixels[(size_t((2 * y + 1) * h / (2 * grid)) * w +
							    (2 * x + 1) * w / (2 * grid)) *
								   4 +
							   c];
					const auto diff = unsigned(std::abs(int(sample[i]) - int(previous[i])));
					delta += diff;
					d += diff;
				}
				if (d > block_change)
					++changed;
			}
		const bool cut = have_previous && delta / samples > mean_change && changed > changed_blocks;
		previous = sample;
		have_previous = true;
		return cut;
	}
};
}
