// SPDX-License-Identifier: GPL-3.0-or-later
// Independent implementation; algorithm references and adaptations are recorded
// in docs/SKIN-TONE.md and docs/provenance.json. No upstream source was copied.
#include "effects.h"
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
	const float x = .95047f * lab_inverse(fy + a / 500.f);
	const float y = lab_inverse(fy);
	const float z = 1.08883f * lab_inverse(fy - b / 200.f);
	return {3.2404542f * x - 1.5371385f * y - .4985314f * z, -.9692660f * x + 1.8760108f * y + .0415560f * z,
		.0556434f * x - .2040259f * y + 1.0572252f * z};
}
bool in_gamut(const Triple &v)
{
	return std::all_of(v.begin(), v.end(), [](float x) { return x >= -1e-6f && x <= 1.000001f; });
}
// Smoothstep between lo and hi.
float ramp(float x, float lo, float hi)
{
	const float t = std::clamp((x - lo) / (hi - lo), 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}
// Median with the lower/upper average for even counts; reorders v.
float median(Plane &v)
{
	const auto mid = v.begin() + v.size() / 2;
	std::nth_element(v.begin(), mid, v.end());
	const float upper = *mid;
	return v.size() % 2 ? upper : .5f * (upper + *std::max_element(v.begin(), mid));
}
// Separable sliding sums with replicated borders. Three passes approximate a
// Gaussian; the mask is filtered identically to each weighted chroma channel.
Plane box_mean(const Plane &src, int w, int h, int radius)
{
	Plane tmp(src.size()), out(src.size());
	const double divisor = 2 * radius + 1;
	for (int y = 0; y < h; ++y) {
		const size_t row = static_cast<size_t>(y) * w;
		double sum = 0;
		for (int k = -radius; k <= radius; ++k)
			sum += src[row + std::clamp(k, 0, w - 1)];
		for (int x = 0; x < w; ++x) {
			tmp[row + x] = static_cast<float>(sum / divisor);
			sum += src[row + std::clamp(x + radius + 1, 0, w - 1)] -
			       src[row + std::clamp(x - radius, 0, w - 1)];
		}
	}
	for (int x = 0; x < w; ++x) {
		double sum = 0;
		for (int k = -radius; k <= radius; ++k)
			sum += tmp[static_cast<size_t>(std::clamp(k, 0, h - 1)) * w + x];
		for (int y = 0; y < h; ++y) {
			out[static_cast<size_t>(y) * w + x] = static_cast<float>(sum / divisor);
			sum += tmp[static_cast<size_t>(std::clamp(y + radius + 1, 0, h - 1)) * w + x] -
			       tmp[static_cast<size_t>(std::clamp(y - radius, 0, h - 1)) * w + x];
		}
	}
	return out;
}
Plane lowpass(Plane p, int w, int h, int radius)
{
	for (int k = 0; k < 3; ++k)
		p = box_mean(p, w, h, radius);
	return p;
}
}

extern "C" int beauti_skin_tone_rgb(const uint8_t *rgb, const float *mask, int width, int height, float amount,
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
				"Expected RGB8 and mask buffers, dimensions of at least 2, and at most 24 million pixels.");
		if (!std::isfinite(amount) || amount < 0 || amount > 1)
			throw std::invalid_argument("Skin Tone strength must be between 0 and 1.");
		const size_t n = static_cast<size_t>(width) * height;
		for (size_t i = 0; i < n; ++i)
			if (!std::isfinite(mask[i]) || mask[i] < 0 || mask[i] > 1)
				throw std::invalid_argument(
					"The skin mask must contain finite values between 0 and 1.");
		// Allows an in-place output and guarantees an exact bypass.
		if (output != rgb)
			std::memcpy(output, rgb, n * 3);
		if (amount == 0 || std::none_of(mask, mask + n, [](float v) { return v > 0; }))
			return 0;
		std::array<float, 256> lut{};
		for (size_t i = 0; i < lut.size(); ++i)
			lut[i] = decode(static_cast<float>(i) / 255.f);
		// Target = median chroma of confident skin samples; field = mask-weighted,
		// low-passed chroma. Each pixel moves toward the target by 90% of the local
		// difference, capped at 10 Lab units, with L* preserved.
		Plane light(n), a(n), b(n), weight(n), wa(n), wb(n), samples_a, samples_b;
		for (size_t i = 0; i < n; ++i) {
			const auto lab = to_lab(rgb + 3 * i, lut);
			light[i] = lab[0];
			a[i] = lab[1];
			b[i] = lab[2];
			// Avoid imposing the face's median color on near-black or specular areas.
			weight[i] = mask[i] * ramp(light[i], 3, 15) * (1 - ramp(light[i], 85, 98));
			wa[i] = weight[i] * a[i];
			wb[i] = weight[i] * b[i];
			if (mask[i] > .65f && light[i] > 15 && light[i] < 85) {
				samples_a.push_back(a[i]);
				samples_b.push_back(b[i]);
			}
		}
		if (samples_a.size() < 16)
			return 0;
		const float target_a = median(samples_a), target_b = median(samples_b);
		const int radius = std::clamp(static_cast<int>(std::lround(std::min(width, height) * .025)), 2, 64);
		const auto norm = lowpass(weight, width, height, radius);
		const auto field_a = lowpass(std::move(wa), width, height, radius);
		const auto field_b = lowpass(std::move(wb), width, height, radius);
		double change_sum = 0, active_weight = 0;
		for (size_t i = 0; i < n; ++i) {
			if (weight[i] <= 0 || norm[i] < .01f)
				continue;
			float da = target_a - field_a[i] / norm[i], db = target_b - field_b[i] / norm[i];
			const float distance = std::hypot(da, db);
			const float scale = amount * weight[i] * .9f * std::min(1.f, 10.f / std::max(distance, 1e-6f));
			da *= scale;
			db *= scale;
			if (std::hypot(da, db) < 1e-4f)
				continue;
			auto linear = to_linear(light[i], a[i] + da, b[i] + db);
			float gamut_scale = 1;
			if (!in_gamut(linear)) {
				float lo = 0, hi = 1;
				for (int step = 0; step < 12; ++step) {
					const float mid = .5f * (lo + hi);
					if (in_gamut(to_linear(light[i], a[i] + mid * da, b[i] + mid * db)))
						lo = mid;
					else
						hi = mid;
				}
				gamut_scale = lo;
				linear = to_linear(light[i], a[i] + lo * da, b[i] + lo * db);
			}
			for (int c = 0; c < 3; ++c)
				output[3 * i + c] = static_cast<uint8_t>(
					std::lround(255 * encode(std::clamp(linear[c], 0.f, 1.f))));
			change_sum += gamut_scale * std::hypot(da, db);
			active_weight += weight[i];
		}
		if (metrics) {
			metrics[0] = target_a;
			metrics[1] = target_b;
			metrics[2] = static_cast<float>(change_sum / std::max(active_weight, 1.));
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
			std::snprintf(error, error_size, "Skin Tone processing failed.");
		return 1;
	}
}
