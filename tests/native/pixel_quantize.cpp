// SPDX-License-Identifier: GPL-3.0-or-later
// RGB input quantization (src/core/pixel_quantize.h) must round to nearest
// regardless of the process rounding mode, and the packed interpolation must
// match the scalar reference.
#include "pixel_quantize.h"
#include <catch_amalgamated.hpp>
#include <cfenv>
#include <limits>

namespace {
// Restores the rounding mode that was active when the test started.
struct RoundingMode {
	int saved = std::fegetround();
	explicit RoundingMode(int mode) { REQUIRE(std::fesetround(mode) == 0); }
	~RoundingMode() { std::fesetround(saved); }
	RoundingMode(const RoundingMode &) = delete;
	RoundingMode &operator=(const RoundingMode &) = delete;
};

uint8_t reference_byte(float value)
{
	return uint8_t(std::clamp(std::nearbyint(value), 0.f, 255.f));
}
}

TEST_CASE("Input bytes round to nearest in every rounding mode", "[pixel-quantize]")
{
	const int mode = GENERATE(FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO);
	CAPTURE(mode);
	RoundingMode rounding(mode);
	auto check = [](float value) {
		if (beauti::quantize_input_byte(value) != reference_byte(value)) {
			CAPTURE(value);
			FAIL("RGB quantization changed rounding");
		}
	};
	SECTION("integers, ties, and their float neighbours")
	{
		for (int i = -2; i <= 257; ++i) {
			const float tie = float(i) + .5f;
			check(float(i));
			check(tie);
			check(std::nextafter(tie, -std::numeric_limits<float>::infinity()));
			check(std::nextafter(tie, std::numeric_limits<float>::infinity()));
		}
	}
	SECTION("dense fractions cover interpolation values and saturation")
	{
		for (int i = -1024; i <= 262144; ++i)
			check(float(i) / 1024.f);
		check(-1e30f);
		check(1e30f);
	}
}

#if defined(_M_X64)
TEST_CASE("Packed interpolation matches the scalar reference in every rounding mode", "[pixel-quantize]")
{
	const int mode = GENERATE(FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO);
	CAPTURE(mode);
	RoundingMode rounding(mode);
	uint32_t seed = 17;
	auto random = [&] {
		seed = 1664525u * seed + 1013904223u;
		return seed;
	};
	for (int i = 0; i < 100000; ++i) {
		uint8_t pixels[4][4];
		for (auto &pixel : pixels) {
			for (auto &channel : pixel)
				channel = uint8_t(random() >> 24);
			pixel[3] = 255;
		}
		const float x = float(random() % 1025) / 1024.f, y = float(random() % 1025) / 1024.f;
		const auto actual = beauti::interpolate_opaque_bytes(pixels[0], pixels[1], pixels[2], pixels[3], x, y);
		for (int c = 0; c < 4; ++c) {
			const float A = pixels[0][c], B = pixels[1][c], D = pixels[2][c], E = pixels[3][c];
			const auto expected = reference_byte((A + (B - A) * x) * (1 - y) + (D + (E - D) * x) * y);
			if (actual[c] != expected) {
				CAPTURE(i, c, x, y, actual[c], expected);
				FAIL("Packed interpolation changed pixels");
			}
		}
	}
}
#endif
