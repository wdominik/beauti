// SPDX-License-Identifier: GPL-3.0-or-later
// Independent local lightness correction, not Retouch4me code or a trained model.
// See docs/DODGE-BURN.md for references, parameters, and limitations.
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

// Three separable sliding box averages approximate a Gaussian. Double sums keep
// large regions stable. Mask-weighted normalization avoids excluded neighbors.
Plane box_mean(const Plane &src, int w, int h, int r)
{
	Plane tmp(src.size()), out(src.size());
	const double divisor = 2 * r + 1;
	for (int y = 0; y < h; ++y) {
		const size_t row = static_cast<size_t>(y) * w;
		double sum = 0;
		for (int k = -r; k <= r; ++k)
			sum += src[row + std::clamp(k, 0, w - 1)];
		for (int x = 0; x < w; ++x) {
			tmp[row + x] = static_cast<float>(sum / divisor);
			sum += src[row + std::clamp(x + r + 1, 0, w - 1)] - src[row + std::clamp(x - r, 0, w - 1)];
		}
	}
	for (int x = 0; x < w; ++x) {
		double sum = 0;
		for (int k = -r; k <= r; ++k)
			sum += tmp[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + x];
		for (int y = 0; y < h; ++y) {
			out[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / divisor);
			sum += tmp[static_cast<size_t>(std::clamp(y + r + 1, 0, h - 1)) * w + x] -
			       tmp[static_cast<size_t>(std::clamp(y - r, 0, h - 1)) * w + x];
		}
	}
	return out;
}
Plane blur(Plane p, int w, int h, int r)
{
	for (int k = 0; k < 3; ++k)
		p = box_mean(p, w, h, r);
	return p;
}
Plane normalized(const Plane &p, const float *mask, const Plane &norm, int w, int h, int r)
{
	Plane weighted(p.size());
	for (size_t i = 0; i < p.size(); ++i)
		weighted[i] = p[i] * mask[i];
	auto out = blur(std::move(weighted), w, h, r);
	for (size_t i = 0; i < p.size(); ++i)
		out[i] = norm[i] > 1e-6f ? out[i] / norm[i] : p[i];
	return out;
}
float ramp(float x, float lo, float hi)
{
	const float t = std::clamp((x - lo) / (hi - lo), 0.f, 1.f);
	return t * t * (3 - 2 * t);
}
}

extern "C" int beauti_dodge_burn_rgb(const uint8_t *rgb, const float *mask, int width, int height, float amount,
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
			throw std::invalid_argument("Dodge & Burn strength must be between 0 and 1.");
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
		Plane light(n), a(n), b(n);
		for (size_t i = 0; i < n; ++i) {
			const auto lab = to_lab(rgb + 3 * i, lut);
			light[i] = lab[0];
			a[i] = lab[1];
			b[i] = lab[2];
		}
		namespace tuning = beauti::tuning;
		const int small = std::clamp(static_cast<int>(std::lround(float(std::min(width, height)) *
									  tuning::kDodgeSmallRadiusFraction)),
					     tuning::kDodgeSmallRadiusMin, tuning::kDodgeSmallRadiusMax);
		const int large = std::clamp(static_cast<int>(std::lround(float(std::min(width, height)) *
									  tuning::kDodgeLargeRadiusFraction)),
					     tuning::kDodgeLargeRadiusMin, tuning::kDodgeLargeRadiusMax);
		const Plane weight(mask, mask + n);
		const auto norm_small = blur(weight, width, height, small);
		const auto norm_large = blur(weight, width, height, large);
		const auto base = normalized(light, mask, norm_small, width, height, small);
		const auto target = normalized(base, mask, norm_large, width, height, large);
		Plane squared(n);
		for (size_t i = 0; i < n; ++i)
			squared[i] = base[i] * base[i];
		const auto second = normalized(squared, mask, norm_large, width, height, large);
		double corrected = 0, dodged = 0, burned = 0;
		for (size_t i = 0; i < n; ++i) {
			if (mask[i] <= 0)
				continue;
			const float variance = std::max(0.f, second[i] - target[i] * target[i]);
			const float v = variance / 36.f;
			const float edge_gate = 1.f / (1.f + v * v);
			const float exposure_gate = ramp(light[i], 3, 15) * (1 - ramp(light[i], 85, 98));
			// Correct a low-frequency illumination field on ORIGINAL L*, leaving
			// fine texture in place. Avoid replacing pixels with a blurred base.
			float d = amount * mask[i] * edge_gate * exposure_gate * 4.f *
				  std::tanh(.85f * (target[i] - base[i]) / 4.f);
			if (std::abs(d) < 1e-4f)
				continue;
			auto linear = to_linear(light[i] + d, a[i], b[i]);
			if (!in_gamut(linear)) {
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
			for (int c = 0; c < 3; ++c)
				output[3 * i + c] = static_cast<uint8_t>(
					std::lround(255 * encode(std::clamp(linear[c], 0.f, 1.f))));
			corrected += std::abs(d);
			dodged += d > 0;
			burned += d < 0;
		}
		if (metrics) {
			metrics[0] = static_cast<float>(corrected / double(n));
			metrics[1] = static_cast<float>(dodged / double(n));
			metrics[2] = static_cast<float>(burned / double(n));
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
			std::snprintf(error, error_size, "Dodge & Burn processing failed.");
		return 1;
	}
}
