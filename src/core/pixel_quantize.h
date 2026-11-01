// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstring>
#if defined(_M_X64)
#include <emmintrin.h>
#endif
namespace beauti {
// Finite RGB interpolation values; preserve nearbyint's current rounding mode.
// SSE is part of the Windows x64 baseline, with no AVX/GPU-specific dispatch.
inline uint8_t quantize_input_byte(float value)
{
#if defined(_M_X64)
	return uint8_t(_mm_cvtss_si32(_mm_set_ss(std::clamp(value, 0.f, 255.f))));
#else
	return uint8_t(std::clamp(std::nearbyint(value), 0.f, 255.f));
#endif
}
#if defined(_M_X64)
// Four RGBA bytes as floats.
inline __m128 load_rgba_bytes(const uint8_t *pixel)
{
	int bytes;
	std::memcpy(&bytes, pixel, sizeof(bytes));
	const auto zero = _mm_setzero_si128();
	return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(bytes), zero), zero));
}
// Bilinear interpolation of four opaque pixels (a, b top row; d, e bottom row) at
// fractions x, y, rounded to bytes exactly like the scalar path in engine.cpp.
inline std::array<uint8_t, 4> interpolate_opaque_bytes(const uint8_t *a, const uint8_t *b, const uint8_t *d,
						       const uint8_t *e, float x, float y)
{
	const auto A = load_rgba_bytes(a), B = load_rgba_bytes(b), D = load_rgba_bytes(d), E = load_rgba_bytes(e);
	const auto X = _mm_set1_ps(x), Y = _mm_set1_ps(y);
	const auto top = _mm_add_ps(A, _mm_mul_ps(_mm_sub_ps(B, A), X));
	const auto bottom = _mm_add_ps(D, _mm_mul_ps(_mm_sub_ps(E, D), X));
	// Keep the scalar expression's operation order, including byte rounding.
	const auto mixed = _mm_add_ps(_mm_mul_ps(top, _mm_sub_ps(_mm_set1_ps(1.f), Y)), _mm_mul_ps(bottom, Y));
	const auto rounded = _mm_cvtps_epi32(_mm_min_ps(_mm_max_ps(mixed, _mm_setzero_ps()), _mm_set1_ps(255.f)));
	const auto words = _mm_packs_epi32(rounded, rounded);
	const int bytes = _mm_cvtsi128_si32(_mm_packus_epi16(words, words));
	std::array<uint8_t, 4> result;
	std::memcpy(result.data(), &bytes, sizeof(bytes));
	return result;
}
#endif
}
