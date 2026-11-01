// SPDX-License-Identifier: GPL-3.0-or-later
// Independent scale-selective luminance processing. Not Adobe Texture or an
// implementation of the Local Laplacian paper. See docs/TEXTURE.md for sources.
#include "effects.h"
#include "tuning.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

namespace {
using Plane = std::vector<float>;
using Triple = std::array<float, 3>;
// Same independently authored D65/sRGB math as Beauti's Skin Tone core.
float decode(float c)
{
	return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
float encode(float c)
{
	return c <= .0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - .055f;
}
float lab_f(float t)
{
	return t > 216.f / 24389.f ? std::cbrt(t) : (24389.f / 27.f * t + 16.f) / 116.f;
}
float lab_inverse(float t)
{
	return t > 6.f / 29.f ? t * t * t : (116.f * t - 16.f) * 27.f / 24389.f;
}
Triple to_lab(const uint8_t *p, const std::array<float, 256> &lut)
{
	const float r = lut[p[0]], g = lut[p[1]], b = lut[p[2]];
	const float x = lab_f((.4124564f * r + .3575761f * g + .1804375f * b) / .95047f);
	const float y = lab_f(.2126729f * r + .7151522f * g + .0721750f * b);
	const float z = lab_f((.0193339f * r + .1191920f * g + .9503041f * b) / 1.08883f);
	return {116.f * y - 16.f, 500.f * (x - y), 200.f * (y - z)};
}
Triple to_linear(float l, float a, float b)
{
	const float fy = (l + 16.f) / 116.f;
	const float x = .95047f * lab_inverse(fy + a / 500.f), y = lab_inverse(fy),
		    z = 1.08883f * lab_inverse(fy - b / 200.f);
	return {3.2404542f * x - 1.5371385f * y - .4985314f * z, -.9692660f * x + 1.8760108f * y + .0415560f * z,
		.0556434f * x - .2040259f * y + 1.0572252f * z};
}
bool in_gamut(const Triple &v)
{
	return std::all_of(v.begin(), v.end(), [](float x) { return x >= -1e-6f && x <= 1.000001f; });
}
// REFLECT_101 index, matching the shader's reflected().
int reflect(int p, int n)
{
	const int period = 2 * (n - 1);
	p %= period;
	if (p < 0)
		p += period;
	return p < n ? p : period - p;
}
// Stationary, separable five-tap binomial filtering. Range weights use the
// original L* guide in BOTH passes. Mask normalization excludes protected skin
// boundaries from the neighborhood, not just from the final output.
Plane lowpass(const Plane &src, const Plane &guide, const float *mask, int w, int h, int spacing, float threshold)
{
	constexpr float kernel[5] = {1, 4, 6, 4, 1};
	Plane tmp(src.size()), out(src.size());
	for (int axis = 0; axis < 2; ++axis) {
		const auto &input = axis == 0 ? src : tmp;
		auto &dest = axis == 0 ? tmp : out;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) {
				const size_t i = static_cast<size_t>(y) * w + x;
				if (mask[i] <= 0) {
					dest[i] = input[i];
					continue;
				}
				float sum = 0, weight = 0;
				for (int k = -2; k <= 2; ++k) {
					const int xx = axis == 0 ? reflect(x + k * spacing, w) : x;
					const int yy = axis == 1 ? reflect(y + k * spacing, h) : y;
					const size_t j = static_cast<size_t>(yy) * w + xx;
					const float d = (guide[i] - guide[j]) / threshold, d2 = d * d;
					const float q = kernel[k + 2] * mask[j] / (1.f + d2 * d2);
					sum += q * input[j];
					weight += q;
				}
				dest[i] = weight > 0 ? sum / weight : input[i];
			}
	}
	return out;
}
}

extern "C" int beauti_texture_rgb(const uint8_t *rgb, const float *mask, int width, int height, float amount,
				  uint8_t *output, float *metrics, char *error, size_t error_size)
{
	if (error && error_size)
		error[0] = '\0';
	if (metrics)
		std::fill(metrics, metrics + 4, 0.f);
	try {
		const auto start = std::chrono::steady_clock::now();
		if (!rgb || !mask || !output || width < 2 || height < 2 ||
		    static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > 24000000ULL)
			throw std::invalid_argument(
				"Expected RGB8 and a mask, dimensions of at least 2, and at most 24 million pixels.");
		if (!std::isfinite(amount) || amount < 0 || amount > 1)
			throw std::invalid_argument("Texture strength must be between 0 and 1.");
		const size_t n = static_cast<size_t>(width) * height;
		for (size_t i = 0; i < n; ++i)
			if (!std::isfinite(mask[i]) || mask[i] < 0 || mask[i] > 1)
				throw std::invalid_argument(
					"The skin mask must contain finite values between 0 and 1.");
		if (output != rgb)
			std::memcpy(output, rgb, n * 3);
		if (amount == 0 || std::none_of(mask, mask + n, [](float v) { return v > 0; }))
			return 0;
		std::array<float, 256> lut{};
		for (size_t i = 0; i < lut.size(); ++i)
			lut[i] = decode(static_cast<float>(i) / 255.f);
		Plane light(n), a(n), b(n), delta(n, 0.f);
		for (size_t i = 0; i < n; ++i) {
			const auto lab = to_lab(rgb + 3 * i, lut);
			light[i] = lab[0];
			a[i] = lab[1];
			b[i] = lab[2];
		}
		namespace tuning = beauti::tuning;
		const int spacing = std::clamp(
			static_cast<int>(std::lround(float(std::min(width, height)) / tuning::kTextureSpacingDivisor)),
			tuning::kTextureSpacingMin, tuning::kTextureSpacingMax);
		// All orientations are processed; scale weights are shared with the GPU path.
		Plane level = light;
		for (int s = 0; s < tuning::kTextureLevels; ++s) {
			auto next = lowpass(level, light, mask, width, height, spacing * (1 << s),
					    tuning::kTextureThresholds[s]);
			for (size_t i = 0; i < n; ++i)
				delta[i] += tuning::kTextureGains[s] * (next[i] - level[i]);
			level = std::move(next);
		}
		double total = 0, weight = 0;
		for (size_t i = 0; i < n; ++i) {
			if (mask[i] <= 0)
				continue;
			float d = mask[i] * 6.f * std::tanh(delta[i] / 6.f);
			if (std::abs(d) < 1e-4f)
				continue;
			auto linear = to_linear(light[i] + d, a[i], b[i]);
			if (!in_gamut(linear)) {
				// Back off the L* correction; do not desaturate to fit gamut.
				float lo = 0, hi = 1;
				for (int step = 0; step < 12; ++step) {
					const float mid = .5f * (lo + hi);
					if (in_gamut(to_linear(light[i] + mid * d, a[i], b[i])))
						lo = mid;
					else
						hi = mid;
				}
				d *= lo;
				linear = to_linear(light[i] + d, a[i], b[i]);
			}
			for (int c = 0; c < 3; ++c) {
				const float endpoint = 255.f * encode(std::clamp(linear[c], 0.f, 1.f));
				output[3 * i + c] = static_cast<uint8_t>(
					std::lround(rgb[3 * i + c] + amount * (endpoint - rgb[3 * i + c])));
			}
			total += amount * std::abs(d);
			weight += mask[i];
		}
		if (metrics) {
			metrics[0] = static_cast<float>(spacing);
			metrics[1] = 4;
			metrics[2] = static_cast<float>(total / std::max(weight, 1.));
			metrics[3] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start)
					     .count();
		}
		return 0;
	} catch (const std::exception &e) {
		if (error && error_size)
			std::snprintf(error, error_size, "%s", e.what());
		return 1;
	} catch (...) {
		if (error && error_size)
			std::snprintf(error, error_size, "Texture processing failed.");
		return 1;
	}
}
