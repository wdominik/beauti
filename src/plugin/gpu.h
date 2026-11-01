// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <obs-module.h>
#include <graphics/vec2.h>
#include "frame.h"
#include "finishing.h"
#include "gs_handles.h"
#include <string>
class SkinGPU {
	// One render target per intermediate result; values index passes[].
	// Composite and Eyes are RGBA8 outputs; every other slot is RGBA32F.
	enum class Pass : int {
		DodgeInit = 0,
		DodgeFieldH,
		DodgeFieldV,
		DodgeBase,
		DodgeStatsH,
		DodgeStatsV,
		TextureInit,
		TextureH,
		TextureVEven,
		TextureVOdd,
		Composite = 11,
		ToneLab,
		ToneBlurH,
		ToneBlurV,
		ToneSamples,
		ToneHistogram,
		ToneMedian,
		ToneHistogramRefined,
		ToneMedianRefined,
		MaskInset,
		MaskFeatherH,
		MaskFeatherV,
		Eyes,
		LocalInit,
		LocalH,
		LocalVEven,
		LocalVOdd,
		LocalComposite,
		Count
	};
	beauti::Effect effect;
	beauti::TexRender passes[size_t(Pass::Count)];
	beauti::Texture blend, mask, eyes;
	uint32_t blend_size = 0, mask_size = 0, eye_size = 0;
	float eye_strength = 0;
	uint64_t uploaded_revision = 0;
	uint32_t uploaded_eye_size = 0;
	bool uploaded_raw_mask = false;
	// Borrowed for the duration of one compose(): the caller's snapshot and
	// results that live in passes[].
	gs_texture_t *source = nullptr, *mask_field = nullptr;
	gs_texture_t *tone_field = nullptr, *tone_target = nullptr;
	float roi_value[4] = {}, pixel_value[2] = {}, grid_value[2] = {}, strength_value[4] = {};
	gs_texture_t *dodge_base = nullptr, *dodge_stats = nullptr;
	float texture_threshold = 6, texture_gain = .45f;
	beauti::FinishingControls finishing;
	float local_spacing = 1;
	int blur_radius = 3, tone_radius = 3, histogram_refine = 0;
	bool upload(beauti::Texture &texture, uint32_t &size, uint32_t n, const std::vector<float> &values);
	gs_texture_t *pass(Pass slot, const char *technique, uint32_t w, uint32_t h, gs_texture_t *input,
			   gs_texture_t *second = nullptr);
	struct FaceGrid {
		vec2 pixels, size; // face region in output pixels; analysis grid
		uint32_t w = 0, h = 0;
	};
	bool sync_maps(const beauti::Maps &maps, uint64_t map_revision, bool &reuse);
	void set_geometry(const beauti::Frame &frame, uint32_t width, uint32_t height, FaceGrid &grid);
	gs_texture_t *run_stages(gs_texture_t *original, uint32_t quality, const FaceGrid &grid, uint32_t width,
				 uint32_t height);
	// Stages of compose(), in pipeline order. Each returns false when a pass failed.
	bool prepare_mask();
	bool prepare_skin_tone(gs_texture_t *original, uint32_t quality, const vec2 &pixels, const vec2 &grid);
	gs_texture_t *prepare_local_color(uint32_t w, uint32_t h, uint32_t width, uint32_t height);
	bool prepare_dodge_burn(gs_texture_t *original, uint32_t w, uint32_t h);
	gs_texture_t *smooth_texture(gs_texture_t *original, uint32_t w, uint32_t h);

public:
	explicit SkinGPU(const std::string &path);
	~SkinGPU() = default;
	SkinGPU(const SkinGPU &) = delete;
	SkinGPU &operator=(const SkinGPU &) = delete;
	// All methods, including lifetime, run inside the OBS graphics context.
	// Nonzero revisions identify immutable maps; zero forces a fresh upload.
	// Source pixels and strengths may change while the maps stay the same.
	gs_texture_t *compose(gs_texture_t *original, const beauti::Frame &frame, const beauti::Maps &maps,
			      uint32_t quality, float retouch, float skin_tone, float dodge_burn, float smooth,
			      uint64_t map_revision = 0, float eye_amount = 0,
			      const beauti::FinishingControls &controls = {});
};
