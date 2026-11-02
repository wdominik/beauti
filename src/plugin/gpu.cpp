// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpu.h"
#include "timing.h"
#include "tuning.h"
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace tuning = beauti::tuning;
namespace {
// Compositor-only constants; values shared with the CPU references are in tuning.h.
constexpr float kDefaultRadiusFraction = .018f;
constexpr int kDefaultRadiusMin = 3;
constexpr int kBlurPasses = 3; // three box passes approximate a Gaussian
// Skin Tone works on a reduced chroma field and a 64x64 sample of the face.
constexpr int kToneFieldLongSidePerformance = 256, kToneFieldLongSideHighQuality = 384;
constexpr float kToneRadiusFraction = .025f, kToneRadiusMin = 2.f, kToneRadiusMax = 64.f;
constexpr uint32_t kToneSampleGrid = 64, kToneHistogramBins = 32;
}
SkinGPU::SkinGPU(const std::string &path)
{
	char *error = nullptr;
	effect.reset(gs_effect_create_from_file(path.c_str(), &error));
	std::string message = error ? error : "Could not load the GPU effect";
	bfree(error);
	if (!effect)
		throw std::runtime_error(message);
}
// Upload an n x n RGBA32F map; the dynamic texture is recreated when the size changes.
bool SkinGPU::upload(beauti::Texture &texture, uint32_t &size, uint32_t n, const std::vector<float> &values)
{
	beauti::TimingSpan span("upload_cpu_submission");
	if (!n)
		return true;
	if (values.size() != size_t(n) * n * 4)
		return false;
	if (!texture || size != n) {
		texture.reset(gs_texture_create(n, n, GS_RGBA32F, 1, nullptr, GS_DYNAMIC));
		size = n;
	}
	if (!texture)
		return false;
	gs_texture_set_image(texture.get(), reinterpret_cast<const uint8_t *>(values.data()),
			     uint32_t(size_t(n) * 4 * sizeof(float)), false);
	return true;
}
// The final passes hold display pixels. Intermediate fields need more than 8 bits, and half
// floats cost clearly less than 32-bit floats at 4K. Two kinds of field stay in 32-bit floats:
// the tone histograms count up to 4096 samples, which half floats cannot represent exactly, and
// the Micro Dodge & Burn statistics hold squares, whose differences half floats would lose.
gs_color_format SkinGPU::format_of(Pass slot)
{
	switch (slot) {
	case Pass::Composite:
	case Pass::Eyes:
		return GS_RGBA;
	case Pass::ToneHistogram:
	case Pass::ToneHistogramRefined:
	case Pass::ToneMedian:
	case Pass::ToneMedianRefined:
	case Pass::DodgeBase:
	case Pass::DodgeStatsH:
	case Pass::DodgeStatsV:
		return GS_RGBA32F;
	default:
		return GS_RGBA16F;
	}
}
// Render one technique of beauti.effect into the slot's render target with input
// (and optionally second) bound; returns the result texture, or nullptr on failure.
gs_texture_t *SkinGPU::pass(Pass slot, const char *technique, uint32_t w, uint32_t h, gs_texture_t *input,
			    gs_texture_t *second)
{
	auto &target = passes[size_t(slot)];
	if (!target)
		target.reset(gs_texrender_create(format_of(slot), GS_ZS_NONE));
	if (!target)
		return nullptr;
	gs_texrender_reset(target.get());
	if (!gs_texrender_begin_with_color_space(target.get(), w, h, GS_CS_SRGB))
		return nullptr;
	gs_ortho(0, float(w), 0, float(h), -100, 100);
	// OBS clears parameter values when a technique ends: bind every pass.
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "roi"), roi_value, sizeof(roi_value));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "roi_pixels"), pixel_value, sizeof(pixel_value));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "grid"), grid_value, sizeof(grid_value));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "strengths"), strength_value,
			  sizeof(strength_value));
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "eye_map"), eyes ? eyes.get() : source);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "eye_strength"), eye_strength);
	const float eye_tuning[3] = {tuning::kIrisExposure, tuning::kScleraExposure, tuning::kIrisChroma};
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "eye_tuning"), eye_tuning, sizeof(eye_tuning));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "eye_gates"), tuning::kEyeGates,
			  sizeof(tuning::kEyeGates));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "sclera_gates"), tuning::kScleraGates,
			  sizeof(tuning::kScleraGates));
	gs_effect_set_int(gs_effect_get_param_by_name(effect.get(), "radius"), blur_radius);
	gs_effect_set_int(gs_effect_get_param_by_name(effect.get(), "tone_radius"), tone_radius);
	gs_effect_set_int(gs_effect_get_param_by_name(effect.get(), "histogram_refine"), histogram_refine);
	gs_effect_set_int(gs_effect_get_param_by_name(effect.get(), "histogram_samples"), 64);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "mask_pixels"), float(mask_size));
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "tone_field"),
			      tone_field ? tone_field : source);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "tone_target"),
			      tone_target ? tone_target : source);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "original_image"), source);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "blend_map"), blend ? blend.get() : source);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "skin_map"),
			      mask_field ? mask_field : (mask ? mask.get() : source));
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "image"), input);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "second_image"), second ? second : input);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "dodge_base"),
			      dodge_base ? dodge_base : source);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect.get(), "dodge_stats"),
			      dodge_stats ? dodge_stats : source);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "texture_threshold"), texture_threshold);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "texture_gain"), texture_gain);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "texture_scale"), finishing.size);
	gs_effect_set_float(gs_effect_get_param_by_name(effect.get(), "texture_edges"), finishing.edges);
	const float local_control[3] = {finishing.color, local_spacing, finishing.color_edges};
	const float local_tuning[3] = {tuning::kLocalLightThreshold, tuning::kLocalChromaThreshold,
				       tuning::kLocalChromaCap};
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "local_control"), local_control,
			  sizeof(local_control));
	gs_effect_set_val(gs_effect_get_param_by_name(effect.get(), "local_tuning"), local_tuning,
			  sizeof(local_tuning));
	while (gs_effect_loop(effect.get(), technique))
		gs_draw_sprite(input, 0, w, h);
	gs_texrender_end(target.get());
	return gs_texrender_get_texture(target.get());
}
// Feather the raw skin mask: erosion, then a separable Gaussian. Shared OBS
// graphics path for every inference provider; the result lives in passes[].
bool SkinGPU::prepare_mask()
{
	auto *inset = pass(Pass::MaskInset, "MaskInset", mask_size, mask_size, mask.get());
	auto *horizontal = inset ? pass(Pass::MaskFeatherH, "MaskFeatherH", mask_size, mask_size, inset) : nullptr;
	mask_field = horizontal ? pass(Pass::MaskFeatherV, "MaskFeatherV", mask_size, mask_size, horizontal) : nullptr;
	return mask_field != nullptr;
}
// Skin Tone: a reduced chroma field and the median target of a 64x64 face sample.
// The low-frequency correction is small; full-resolution pixels retain their own
// lightness and texture. No GPU readback or CPU color work.
bool SkinGPU::prepare_skin_tone(gs_texture_t *original, uint32_t quality, const vec2 &pixels, const vec2 &grid)
{
	const float tone_scale =
		std::min(1.f, float(quality ? kToneFieldLongSideHighQuality : kToneFieldLongSidePerformance) /
				      std::max(pixels.x, pixels.y));
	const uint32_t tw = std::max(2u, uint32_t(std::round(pixels.x * tone_scale)));
	const uint32_t th = std::max(2u, uint32_t(std::round(pixels.y * tone_scale)));
	grid_value[0] = float(tw);
	grid_value[1] = float(th);
	tone_radius =
		std::max(1, int(std::round(std::clamp(std::round(std::min(pixels.x, pixels.y) * kToneRadiusFraction),
						      kToneRadiusMin, kToneRadiusMax) *
					   tone_scale)));
	auto *field = pass(Pass::ToneLab, "ToneLab", tw, th, original);
	for (int i = 0; i < kBlurPasses && field; ++i) {
		auto *tmp = pass(Pass::ToneBlurH, "ToneBlurH", tw, th, field);
		field = tmp ? pass(Pass::ToneBlurV, "ToneBlurV", tw, th, tmp) : nullptr;
	}
	tone_field = field;
	auto *samples =
		field ? pass(Pass::ToneSamples, "ToneLab", kToneSampleGrid, kToneSampleGrid, original) : nullptr;
	histogram_refine = 0;
	auto *hist = samples ? pass(Pass::ToneHistogram, "ToneHistogram", kToneHistogramBins, 1, samples) : nullptr;
	tone_target = hist ? pass(Pass::ToneMedian, "ToneMedian", 1, 1, hist) : nullptr;
	histogram_refine = 1;
	hist = tone_target ? pass(Pass::ToneHistogramRefined, "ToneHistogram", kToneHistogramBins, 1, samples)
			   : nullptr;
	tone_target = hist ? pass(Pass::ToneMedianRefined, "ToneMedian", 1, 1, hist) : nullptr;
	memcpy(grid_value, &grid, sizeof(grid_value));
	return tone_field && tone_target;
}
// Local chroma processing retains the full-resolution L* guide and skin exclusions.
// RGBA32F precomposition avoids another 8-bit premultiplication before finishing.
gs_texture_t *SkinGPU::prepare_local_color(uint32_t w, uint32_t h, uint32_t width, uint32_t height)
{
	local_spacing = std::clamp(float(std::min(w, h)) * tuning::kLocalSpacingFraction * finishing.color_size,
				   tuning::kLocalSpacingMin, tuning::kLocalSpacingMax);
	auto *guide = pass(Pass::LocalInit, "LocalInit", w, h, source);
	auto *field = guide;
	for (int i = 0; i < 2 && field; ++i) {
		auto *tmp = pass(Pass::LocalH, "LocalH", w, h, field, guide);
		field = tmp ? pass(i ? Pass::LocalVOdd : Pass::LocalVEven, "LocalV", w, h, tmp, guide) : nullptr;
	}
	return field ? pass(Pass::LocalComposite, "LocalComposite", width, height, field) : nullptr;
}
// Micro Dodge & Burn: a small-radius lightness field (dodge_base) and large-radius
// statistics (dodge_stats) over the full-resolution face region.
bool SkinGPU::prepare_dodge_burn(gs_texture_t *original, uint32_t w, uint32_t h)
{
	blur_radius = std::clamp(int(std::round(float(std::min(w, h)) * tuning::kDodgeSmallRadiusFraction)),
				 tuning::kDodgeSmallRadiusMin, tuning::kDodgeSmallRadiusMax);
	auto *field = pass(Pass::DodgeInit, "DodgeInit", w, h, original);
	for (int i = 0; i < kBlurPasses && field; ++i) {
		auto *tmp = pass(Pass::DodgeFieldH, "FieldH", w, h, field);
		field = tmp ? pass(Pass::DodgeFieldV, "FieldV", w, h, tmp) : nullptr;
	}
	dodge_base = field ? pass(Pass::DodgeBase, "DodgeBase", w, h, field) : nullptr;
	blur_radius = std::clamp(int(std::round(float(std::min(w, h)) * tuning::kDodgeLargeRadiusFraction)),
				 tuning::kDodgeLargeRadiusMin, tuning::kDodgeLargeRadiusMax);
	field = dodge_base;
	for (int i = 0; i < kBlurPasses && field; ++i) {
		auto *tmp = pass(Pass::DodgeStatsH, "FieldH", w, h, field);
		field = tmp ? pass(Pass::DodgeStatsV, "FieldV", w, h, tmp) : nullptr;
	}
	dodge_stats = field;
	return dodge_base && dodge_stats;
}
// Texture Smooth: four scale levels of range-weighted L* contrast reduction.
// Returns the finished level, or nullptr when a pass failed.
gs_texture_t *SkinGPU::smooth_texture(gs_texture_t *original, uint32_t w, uint32_t h)
{
	auto *level = pass(Pass::TextureInit, "TextureInit", w, h, original);
	const int spacing = std::clamp(int(std::round(float(std::min(w, h)) / tuning::kTextureSpacingDivisor)),
				       tuning::kTextureSpacingMin, tuning::kTextureSpacingMax);
	for (int i = 0; i < tuning::kTextureLevels && level; ++i) {
		blur_radius = spacing * (1 << i);
		texture_threshold = tuning::kTextureThresholds[i];
		texture_gain = finishing.gains[size_t(i)];
		auto *tmp = pass(Pass::TextureH, "TextureH", w, h, level);
		// Alternate completed levels: vertical pass reads the previous one.
		level = tmp ? pass(i % 2 ? Pass::TextureVOdd : Pass::TextureVEven, "TextureV", w, h, tmp, level)
			    : nullptr;
	}
	return level;
}
// Uploads changed maps, or reports that the uploaded ones still match this revision.
bool SkinGPU::sync_maps(const beauti::Maps &maps, uint64_t map_revision, bool &reuse)
{
	reuse = map_revision && map_revision == uploaded_revision && blend_size == maps.blend_size &&
		mask_size == maps.mask_size && uploaded_eye_size == maps.eye_size && uploaded_raw_mask == maps.raw_mask;
	if (reuse)
		return true;
	// Invalidate before work that can fail. No stale revision may survive a
	// partial upload or failed mask pass.
	uploaded_revision = 0;
	mask_field = nullptr;
	if (!maps.blend_size)
		blend_size = 0; // A paused retouch: compared above, and its stale texture has strength zero.
	return upload(blend, blend_size, maps.blend_size, maps.blend) &&
	       upload(mask, mask_size, maps.mask_size, maps.mask) && upload(eyes, eye_size, maps.eye_size, maps.eyes);
}
// Face region in normalized and pixel units; texture scales use the full-resolution
// face region in both quality modes.
void SkinGPU::set_geometry(const beauti::Frame &f, uint32_t width, uint32_t height, FaceGrid &grid)
{
	vec4 roi;
	vec4_set(&roi, float(f.box[0]) / float(f.width), float(f.box[1]) / float(f.height),
		 float(f.box[2] - f.box[0]) / float(f.width), float(f.box[3] - f.box[1]) / float(f.height));
	vec2_set(&grid.pixels, roi.z * float(width), roi.w * float(height));
	grid.w = std::max(2u, uint32_t(std::round(grid.pixels.x)));
	grid.h = std::max(2u, uint32_t(std::round(grid.pixels.y)));
	vec2_set(&grid.size, float(grid.w), float(grid.h));
	memcpy(roi_value, &roi, sizeof(roi_value));
	memcpy(pixel_value, &grid.pixels, sizeof(pixel_value));
	memcpy(grid_value, &grid.size, sizeof(grid_value));
	blur_radius = std::max(kDefaultRadiusMin,
			       int(std::nearbyint(float(std::min(grid.w, grid.h)) * kDefaultRadiusFraction)));
}
// Runs the enabled stages in pipeline order and returns the composed frame.
gs_texture_t *SkinGPU::run_stages(gs_texture_t *original, uint32_t quality, const FaceGrid &grid, uint32_t width,
				  uint32_t height)
{
	gs_texture_t *finish = original;
	bool ready = true;
	if (strength_value[1] > 0)
		ready = prepare_skin_tone(original, quality, grid.pixels, grid.size);
	if (ready && finishing.color > 0) {
		auto *prepared = prepare_local_color(grid.w, grid.h, width, height);
		ready = prepared != nullptr;
		if (ready) {
			// Materialize the early stages once; later stages consume their result.
			// This also keeps Composite within OBS's eight-texture limit.
			source = prepared;
			strength_value[0] = strength_value[1] = 0;
			finish = prepared;
		}
	}
	if (ready && strength_value[2] > 0)
		ready = prepare_dodge_burn(source, grid.w, grid.h);
	if (ready && strength_value[3] > 0) {
		finish = smooth_texture(source, grid.w, grid.h);
		ready = finish != nullptr;
	}
	auto *out = ready ? pass(Pass::Composite, "Composite", width, height, finish) : nullptr;
	if (out && eye_strength > 0)
		out = pass(Pass::Eyes, "Eyes", width, height, out);
	return out;
}
gs_texture_t *SkinGPU::compose(gs_texture_t *original, const beauti::Frame &f, const beauti::Maps &maps,
			       uint32_t quality, float retouch, float skin_tone, float dodge_burn, float smooth,
			       uint64_t map_revision, float eye_amount, const beauti::FinishingControls &controls)
{
	if (f.status != 0)
		return nullptr;
	finishing = controls;
	bool reuse_maps = false;
	if (!sync_maps(maps, map_revision, reuse_maps))
		return nullptr;
	const uint32_t width = gs_texture_get_width(original), height = gs_texture_get_height(original);
	FaceGrid grid;
	set_geometry(f, width, height, grid);
	eye_strength = maps.eye_size ? std::clamp(eye_amount / 100.f, 0.f, 1.f) : 0.f;
	strength_value[0] = retouch / 100.f;
	strength_value[1] = skin_tone / 100.f;
	strength_value[2] = dodge_burn / 100.f;
	strength_value[3] = smooth / 100.f;
	source = original;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	dodge_base = nullptr;
	dodge_stats = nullptr;
	tone_field = nullptr;
	tone_target = nullptr;
	bool ready = true;
	if (!reuse_maps && maps.raw_mask && maps.mask_size)
		ready = prepare_mask();
	if (ready) {
		uploaded_revision = map_revision;
		uploaded_raw_mask = maps.raw_mask;
		uploaded_eye_size = maps.eye_size;
	}
	auto *out = ready ? run_stages(original, quality, grid, width, height) : nullptr;
	gs_enable_framebuffer_srgb(srgb);
	gs_blend_state_pop();
	return out;
}
