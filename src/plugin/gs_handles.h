// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <obs-module.h>
#include <memory>
namespace beauti {
// Owning handles for OBS graphics objects. Construction and destruction must
// happen inside the graphics context (obs_enter_graphics or a render callback);
// the deleters accept nullptr like the OBS functions they wrap.
struct TexRenderDeleter {
	void operator()(gs_texrender_t *p) const { gs_texrender_destroy(p); }
};
struct TextureDeleter {
	void operator()(gs_texture_t *p) const { gs_texture_destroy(p); }
};
struct StageSurfaceDeleter {
	void operator()(gs_stagesurf_t *p) const { gs_stagesurface_destroy(p); }
};
struct EffectDeleter {
	void operator()(gs_effect_t *p) const { gs_effect_destroy(p); }
};
using TexRender = std::unique_ptr<gs_texrender_t, TexRenderDeleter>;
using Texture = std::unique_ptr<gs_texture_t, TextureDeleter>;
using StageSurface = std::unique_ptr<gs_stagesurf_t, StageSurfaceDeleter>;
using Effect = std::unique_ptr<gs_effect_t, EffectDeleter>;
}
