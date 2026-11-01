// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define BEAUTI_SOFTEN_API __declspec(dllexport)
#else
#define BEAUTI_SOFTEN_API __attribute__((visibility("default")))
#endif

// Packed RGB8 and a row-major float skin mask in [0,1]. No face generation,
// geometry changes, model runtime, or platform-specific dependencies.
extern "C" BEAUTI_SOFTEN_API int beauti_effects_api_version();

// Skin Tone: independently authored chroma equalization. See docs/SKIN-TONE.md
// and docs/PROVENANCE.md. Preserves CIELAB D65 L*, with gamut-safe chroma limiting.
// metrics: target a*, target b*, mean applied chroma distance, processing ms.
extern "C" BEAUTI_SOFTEN_API int beauti_skin_tone_rgb(const uint8_t *rgb, const float *mask, int width, int height,
						      float amount, uint8_t *output, float *metrics, char *error,
						      size_t error_size);

// Texture: scale-selective L* contrast reduction with fixed Lab a*/b*.
// metrics: base tap spacing, level count, mean absolute L* correction, total ms.
extern "C" BEAUTI_SOFTEN_API int beauti_texture_rgb(const uint8_t *rgb, const float *mask, int width, int height,
						    float amount, uint8_t *output, float *metrics, char *error,
						    size_t error_size);

// Micro Dodge & Burn: bounded local L* correction, before smoothing.
// metrics: mean absolute L* change, dodged fraction, burned fraction, total ms.
extern "C" BEAUTI_SOFTEN_API int beauti_dodge_burn_rgb(const uint8_t *rgb, const float *mask, int width, int height,
						       float amount, uint8_t *output, float *metrics, char *error,
						       size_t error_size);
