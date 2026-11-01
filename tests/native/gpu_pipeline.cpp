// SPDX-License-Identifier: GPL-3.0-or-later
// Real OBS graphics tests without the user's scenes, camera, or application UI.
// Runs the engine on one RGBA fixture, composes it through SkinGPU at a grid of
// control settings, and writes the results and the engine maps as raw files for
// tests/obs/check_finishing_pixels.py.
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif
#include <obs.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include "gpu.h"
#include "engine.h"
#include "paths.h"

static std::vector<uint8_t> read_texture(gs_texture_t *texture, uint32_t w, uint32_t h)
{
	if (!texture)
		throw std::runtime_error("GPU composition returned no texture");
	auto *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (!stage)
		throw std::runtime_error("Could not allocate readback");
	gs_stage_texture(stage, texture);
	uint8_t *data = nullptr;
	uint32_t stride = 0;
	if (!gs_stagesurface_map(stage, &data, &stride)) {
		gs_stagesurface_destroy(stage);
		throw std::runtime_error("Could not read GPU result");
	}
	std::vector<uint8_t> out(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		std::memcpy(out.data() + size_t(y) * w * 4, data + size_t(y) * stride, w * 4);
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
	return out;
}
template<class T> static void write(const std::filesystem::path &path, const std::vector<T> &data)
{
	std::ofstream stream(path, std::ios::binary);
	stream.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size() * sizeof(T)));
	if (!stream)
		throw std::runtime_error("Could not write fixture result");
}
#include "finishing_checks.h"
int beauti_main(int argc, char **argv)
{
#ifdef __APPLE__
	@autoreleasepool {
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
#endif
		if (argc != 6 && argc != 7 && argc != 8 && argc != 9) {
			std::cerr << "bundle input.rgba output_directory width height [backend] [quality] [eyes]\n";
			return 2;
		}
		namespace fs = std::filesystem;
		const fs::path bundle = fs::absolute(beauti::utf8_path(argv[1])),
			       out = fs::absolute(beauti::utf8_path(argv[3]));
		fs::create_directories(out);
		beauti::Frame frame;
		frame.width = uint32_t(std::stoul(argv[4]));
		frame.height = uint32_t(std::stoul(argv[5]));
		frame.retouch = frame.smooth = 100;
		frame.eyes = argc == 9 ? uint32_t(std::stoul(argv[8])) : 0;
		if (!frame.width || !frame.height || uint64_t(frame.width) * frame.height > 24000000)
			return 2;
		frame.pixels.resize(size_t(frame.width) * frame.height * 4);
		std::ifstream input(beauti::utf8_path(argv[2]), std::ios::binary);
		input.read(reinterpret_cast<char *>(frame.pixels.data()), frame.pixels.size());
		if (!input)
			return 3;
#ifdef __APPLE__
		const fs::path resources = bundle / "Contents/Resources";
		std::string obs = "/Applications/OBS.app/Contents/Frameworks", graphics = obs + "/libobs-opengl.dylib";
		if (!obs_startup("en-US", nullptr, nullptr))
			return 4;
#else
	const fs::path resources = bundle / "data";
	const auto *runtime = _wgetenv(L"BEAUTI_OBS_RUNTIME");
	if (!runtime)
		return 2;
	std::string obs = beauti::utf8_string(fs::absolute(fs::path(runtime))),
		    graphics = obs + "/bin/64bit/libobs-d3d11.dll";
	const auto obs_bin = fs::absolute(beauti::utf8_path(obs) / "bin/64bit");
	if (!AddDllDirectory(obs_bin.c_str()))
		return 2;
	if (!obs_startup("en-US", nullptr, nullptr))
		return 4;
	// OBS 33 resolves core data relative to its bin/64bit working directory.
	fs::current_path(obs_bin);
#endif
		obs_video_info video = {};
		video.graphics_module = graphics.c_str();
		video.fps_num = 30;
		video.fps_den = 1;
		video.base_width = video.output_width = frame.width;
		video.base_height = video.output_height = frame.height;
		video.output_format = VIDEO_FORMAT_RGBA;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_FULL;
		if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
			obs_shutdown();
			return 5;
		}
		int result = 0;
		try {
			const uint32_t backend = argc >= 7 ? uint32_t(std::stoul(argv[6])) : beauti::CPU;
			const uint32_t quality = argc >= 8 ? uint32_t(std::stoul(argv[7])) : 0;
			beauti::Engine engine(beauti::utf8_string(resources / "models"),
					      beauti::utf8_string(out / "cache"), backend, quality);
			beauti::Maps maps;
			engine.analyze(frame, maps);
			if (frame.status != 0)
				throw std::runtime_error("The fixture must contain a detected face");
			write(out / "original.rgba", frame.pixels);
			write(out / "blend.f32", maps.blend);
			write(out / "mask.f32", maps.mask);
			write(out / "eyes.f32", maps.eyes);
			std::ofstream metadata(out / "maps.json");
			metadata << "{\"width\":" << frame.width << ",\"height\":" << frame.height
				 << ",\"blend_size\":" << maps.blend_size << ",\"mask_size\":" << maps.mask_size
				 << ",\"eye_size\":" << maps.eye_size << ",\"finishing_quality\":" << quality
				 << ",\"raw_mask\":" << (maps.raw_mask ? "true" : "false") << ",\"box\":[";
			for (int i = 0; i < 4; ++i)
				metadata << (i ? "," : "") << frame.box[i];
			metadata << "]}\n";
			obs_enter_graphics();
			try {
				SkinGPU gpu(beauti::utf8_string(resources / "beauti.effect"));
				const uint8_t *bytes = frame.pixels.data();
				auto *original = gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0);
				if (!original)
					throw std::runtime_error("Could not upload fixture");
				auto render = [&](const std::string &name, float r, float t, float d, float s,
						  gs_texture_t *source, const beauti::Maps &m, int q = -1) {
					auto pixels =
						read_texture(gpu.compose(source, frame, m,
									 q < 0 ? quality : uint32_t(q), r, t, d, s),
							     frame.width, frame.height);
					write(out / (name + ".rgba"), pixels);
					return pixels;
				};
				try {
					check_finishing(gpu, frame, maps, original, quality, out);
					for (auto r : {0, 60, 100})
						for (auto t : {0, 50, 100})
							for (auto d : {0, 50, 100})
								for (auto s : {0, 50, 100})
									render("r" + std::to_string(r) + "-t" +
										       std::to_string(t) + "-d" +
										       std::to_string(d) + "-s" +
										       std::to_string(s),
									       float(r), float(t), float(d), float(s),
									       original, maps);
					if (frame.eyes) {
						auto eye_render = [&](const beauti::Maps &m, float amount,
								      uint64_t revision) {
							return read_texture(gpu.compose(original, frame, m, quality, 0,
											0, 0, 0, revision, amount),
									    frame.width, frame.height);
						};
						if (eye_render(maps, 0, 10) != frame.pixels)
							throw std::runtime_error("Disabled eyes changed pixels");
						for (int amount : {25, 50, 100}) {
							auto pixels = eye_render(maps, float(amount), 10);
							if (pixels != eye_render(maps, float(amount), 0))
								throw std::runtime_error(
									"Cached eye map changed pixels");
							write(out / ("eyes-" + std::to_string(amount) + ".rgba"),
							      pixels);
						}
						auto no_eyes = maps;
						no_eyes.eye_size = 0;
						no_eyes.eyes.clear();
						if (eye_render(no_eyes, 100, 11) != frame.pixels)
							throw std::runtime_error(
								"Missing eye map reused stale correction");
						if (eye_render(maps, 100, 12) != eye_render(maps, 100, 0))
							throw std::runtime_error("Eye map failed to recover");
					}
					const auto tone = render("tone-before-finish", 60, 70, 0, 0, original, maps);
					bytes = tone.data();
					auto *middle =
						gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0);
					auto balanced = render("explicit-dodge", 0, 0, 65, 0, middle, maps);
					gs_texture_destroy(middle);
					bytes = balanced.data();
					middle = gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0);
					render("explicit-sequence", 0, 0, 0, 50, middle, maps);
					gs_texture_destroy(middle);
					render("combined-sequence", 60, 70, 65, 50, original, maps);
					// Redraw one immutable analysis at different strengths and sources.
					// Compare to forced uploads, then replace the maps under a new ID.
					SkinGPU uncached(beauti::utf8_string(resources / "beauti.effect"));
					auto check_cached = [&](gs_texture_t *src, const beauti::Maps &m,
								uint64_t revision, float r, float t, float d, float s) {
						const auto first = read_texture(
							gpu.compose(src, frame, m, quality, r, t, d, s, revision),
							frame.width, frame.height);
						const auto redraw =
							read_texture(gpu.compose(src, frame, m, quality, 100 - r,
										 100 - t, 100 - d, 100 - s, revision),
								     frame.width, frame.height);
						const auto fresh =
							read_texture(uncached.compose(src, frame, m, quality, 100 - r,
										      100 - t, 100 - d, 100 - s),
								     frame.width, frame.height);
						if (redraw != fresh)
							throw std::runtime_error("Cached maps changed output pixels");
						const auto original_strength = read_texture(
							uncached.compose(src, frame, m, quality, r, t, d, s),
							frame.width, frame.height);
						if (first != original_strength)
							throw std::runtime_error(
								"Map revision did not refresh output pixels");
					};
					check_cached(original, maps, 1, 60, 50, 50, 50);
					auto empty = maps;
					std::fill(empty.mask.begin(), empty.mask.end(), 0.f);
					check_cached(original, empty, 2, 60, 50, 50, 50);
					check_cached(original, maps, 3, 0, 100, 0, 100);
					auto prepared = maps;
					prepared.raw_mask = false;
					check_cached(original, prepared, 3, 60, 50, 50, 50);
					// Same revision but changed dimensions must also refresh uploads.
					auto small = empty;
					small.mask_size = 2;
					small.mask.assign(16, 0.f);
					check_cached(original, small, 3, 60, 50, 50, 50);

					render("empty-mask", 0, 100, 100, 100, original, empty);
					render("high-quality", 60, 70, 65, 50, original, maps, 1);
					auto transparent = frame.pixels;
					for (size_t i = 0; i < transparent.size(); i += 4) {
						uint8_t alpha = (i / 4) % 3 == 0 ? 0 : 127;
						for (int c = 0; c < 3; ++c)
							transparent[i + c] = uint8_t(
								std::lround(transparent[i + c] * alpha / 255.f));
						transparent[i + 3] = alpha;
					}
					write(out / "alpha-original.rgba", transparent);
					bytes = transparent.data();
					middle = gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0);
					render("alpha-result", 60, 70, 65, 50, middle, maps);
					if (frame.eyes) {
						auto pixels = read_texture(
							gpu.compose(middle, frame, maps, quality, 0, 0, 0, 0, 0, 100),
							frame.width, frame.height);
						write(out / "alpha-eyes.rgba", pixels);
						for (size_t i = 0; i < pixels.size(); i += 4) {
							if (pixels[i + 3] != transparent[i + 3] ||
							    (!pixels[i + 3] &&
							     (pixels[i] || pixels[i + 1] || pixels[i + 2])))
								throw std::runtime_error(
									"Eyes changed alpha or transparent RGB");
						}
					}
					check_cached(original, maps, 4, 60, 70, 65, 50);
					check_cached(middle, maps, 4, 60, 70, 65, 50);
					gs_texture_destroy(middle);
					std::ofstream(out / "map-cache.json")
						<< "{\"passed\":true,\"pixel_exact\":true,\"cases\":7}\n";
				} catch (...) {
					gs_texture_destroy(original);
					throw;
				}
				gs_texture_destroy(original);
			} catch (...) {
				obs_leave_graphics();
				throw;
			}
			obs_leave_graphics();
		} catch (const std::exception &e) {
			std::cerr << e.what() << "\n";
			result = 1;
		}
		obs_shutdown();
		return result;
#ifdef __APPLE__
	}
#endif
}
#include "cli_entry.h"
