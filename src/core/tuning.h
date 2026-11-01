// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tuning constants shared by the GPU compositor (src/plugin/gpu.cpp with
// data/beauti.effect) and the CPU references (src/effects/). Both sides must use
// the same values so the numerical tests compare like with like. Fractions are
// of the shorter side of the face region.
namespace beauti::tuning {
// Eyes: maximum exposure in stops and fractional iris chroma increase.
constexpr float kIrisExposure = .45f, kScleraExposure = .16f, kIrisChroma = .04f;
// Eyes: no correction below linear luminance .008 (pupil) or above .9 (catchlights),
// full correction between .045 and .55.
constexpr float kEyeGates[4] = {.008f, .045f, .55f, .9f};
// Sclera: requires luminance from .06 (full at .2) and fades out for sRGB channel
// spread .12-.32, so tinted or shadowed whites are not brightened.
constexpr float kScleraGates[4] = {.06f, .2f, .12f, .32f};
// Micro Dodge & Burn: small-radius detail field and large-radius statistics.
constexpr float kDodgeSmallRadiusFraction = .0025f, kDodgeLargeRadiusFraction = .018f;
constexpr int kDodgeSmallRadiusMin = 1, kDodgeSmallRadiusMax = 12;
constexpr int kDodgeLargeRadiusMin = 3, kDodgeLargeRadiusMax = 80;
// Texture Smooth: four scales with a base tap spacing derived from the region size.
// Keep some finest texture, attenuate intermediate relief more strongly, and
// leave the coarsest base intact.
constexpr float kTextureSpacingDivisor = 700.f;
constexpr int kTextureSpacingMin = 1, kTextureSpacingMax = 6;
constexpr int kTextureLevels = 4;
constexpr float kTextureThresholds[kTextureLevels] = {6.f, 7.f, 9.f, 12.f};
constexpr float kTextureGains[kTextureLevels] = {.45f, .85f, .70f, .20f};
// Local color: two separable binomial rounds, guided by unchanged Lab and skin mask.
constexpr float kLocalSpacingFraction = .012f, kLocalSpacingMin = 1.f, kLocalSpacingMax = 32.f;
constexpr float kLocalLightThreshold = 8.f, kLocalChromaThreshold = 6.f, kLocalChromaCap = 6.f;
}
