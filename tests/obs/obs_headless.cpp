// SPDX-License-Identifier: GPL-3.0-or-later
// Real libobs render/readback/inference/output test, without the OBS app/UI.
// A synthetic source draws the RGBA fixture (optionally shifted per frame) with a
// 16-bit frame marker in rows 0 to 15 and carries the Beauti filter. The raw
// output callback decodes the marker, measures the face-region difference to the
// fixture and the white fraction, and the run writes those samples as a report.
// Arguments: see the usage line in beauti_main.
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif
#include <filesystem>
#include <cstdlib>
#include <obs.h>
#include <obs-module.h>
#include "paths.h"
#include <graphics/vec4.h>
#include <util/platform.h>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <format>
#include <string_view>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
static std::vector<uint8_t> original;
static uint32_t W = 1280, H = 720;
static bool motion = false;
static bool fixture_cut = false;
// Source metadata for the opt-in status probe, written on the graphics thread.
// OBS may query it on other threads. The fixture
// texture stays at W x H; no oversized allocations or production fault hooks.
static std::atomic<uint32_t> source_width{W}, source_height{H};
static std::atomic<gs_color_space> source_space{GS_CS_SRGB};
static int shift(uint32_t id)
{
	return motion ? int(std::round(std::sin(id * .13) * W * .035)) : 0;
}
static std::atomic<uint32_t> ticks{0};
// std::quoted escapes quotes and backslashes, but JSON also forbids raw control
// characters. Provider error text in a property can contain newlines or tabs.
static std::string json_string(std::string_view value)
{
	std::string result = "\"";
	for (char c : value) {
		if (c == '"' || c == '\\') {
			result += '\\';
			result += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			result += std::format("\\u{:04x}", static_cast<unsigned char>(c));
		} else {
			result += c;
		}
	}
	return result + '"';
}
struct Source {
	gs_texture_t *texture = nullptr, *counter = nullptr;
	std::vector<uint8_t> bars;
	uint32_t uploaded = 0;
};
static void *create_source(obs_data_t *, obs_source_t *)
{
	auto *s = new Source;
	s->bars.resize(256 * 16 * 4, 255);
	const uint8_t *pixels = original.data();
	obs_enter_graphics();
	s->texture = gs_texture_create(W, H, GS_RGBA, 1, &pixels, 0);
	s->counter = gs_texture_create(256, 16, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	obs_leave_graphics();
	return s;
}
static void destroy_source(void *data)
{
	auto *s = static_cast<Source *>(data);
	obs_enter_graphics();
	gs_texture_destroy(s->texture);
	gs_texture_destroy(s->counter);
	obs_leave_graphics();
	delete s;
}
static void tick_source(void *, float)
{
	ticks.fetch_add(1);
}
static void draw(gs_texture_t *texture, uint32_t w, uint32_t h)
{
	auto *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"), texture);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(texture, 0, w, h);
}
static void render_source(void *data, gs_effect_t *)
{
	auto *s = static_cast<Source *>(data);
	uint32_t id = ticks.load();
	if (id != s->uploaded) {
		for (int bit = 0; bit < 16; ++bit)
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x) {
					auto *p = &s->bars[(y * 256 + bit * 16 + x) * 4];
					p[0] = p[1] = p[2] = (id & (1u << bit)) ? 255 : 0;
				}
		gs_texture_set_image(s->counter, s->bars.data(), 256 * 4, false);
		s->uploaded = id;
	}
	bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	if (fixture_cut && id >= 150 && id < 210) {
		vec4 white;
		vec4_set(&white, 1, 1, 1, 1);
		gs_clear(GS_CLEAR_COLOR, &white, 0, 0);
	} else {
		gs_matrix_push();
		gs_matrix_translate3f(float(shift(id)), 0, 0);
		draw(s->texture, W, H);
		gs_matrix_pop();
	}
	draw(s->counter, 256, 16);
	gs_enable_framebuffer_srgb(srgb);
}
static std::mutex mutex;
struct Sample {
	uint32_t id, current;
	double time;
	double delta;
	double white;
};
static std::vector<Sample> samples;
static uint64_t start;
static std::vector<uint8_t> last_pixels;
static void receive(void *, video_data *frame)
{
	uint32_t id = 0;
	for (int bit = 0; bit < 16; ++bit)
		if (frame->data[0][8 * frame->linesize[0] + (bit * 16 + 8) * 4] > 128)
			id |= 1 << bit;
	double difference = 0;
	size_t n = 0, white = 0;
	for (int y = 230 * H / 720; y < int(450 * H / 720); y += 4)
		for (int x = 540 * W / 1280; x < int(790 * W / 1280); x += 4)
			for (int c = 0; c < 3; ++c) {
				auto value = frame->data[0][y * frame->linesize[0] + x * 4 +
							    (c == 0   ? 2
							     : c == 2 ? 0
								      : 1)];
				white += value > 250;
				difference += std::abs(
					int(value) -
					int(original[(y * W + std::clamp(x - shift(id), 0, int(W) - 1)) * 4 + c]));
				++n;
			}
	std::lock_guard<std::mutex> lock(mutex);
	samples.push_back({id, ticks.load(), double(os_gettime_ns() - start) / 1e9, difference / double(n),
			   double(white) / double(n)});
	last_pixels.resize(W * H * 4);
	for (unsigned y = 0; y < H; ++y)
		memcpy(last_pixels.data() + y * W * 4, frame->data[0] + y * frame->linesize[0], W * 4);
}
static std::string status_text(obs_properties_t *properties)
{
	return obs_property_description(obs_properties_get(properties, "status"));
}
static void probe_source_status(obs_source_t *source, obs_source_t *filter, std::ostream &out)
{
	struct Probe {
		const char *name;
		uint32_t width, height;
		gs_color_space source_color, render_color;
		obs_source_t *render;
		bool rendered = false;
	};
	Probe cases[] = {{"missing", W, H, GS_CS_SRGB, GS_CS_SRGB, filter},
			 {"zero-width", 0, H, GS_CS_SRGB, GS_CS_SRGB, source},
			 {"zero-height", W, 0, GS_CS_SRGB, GS_CS_SRGB, source},
			 {"large-width", 4097, H, GS_CS_SRGB, GS_CS_SRGB, source},
			 {"large-height", W, 4097, GS_CS_SRGB, GS_CS_SRGB, source},
			 {"source-16f", W, H, GS_CS_SRGB_16F, GS_CS_SRGB, source},
			 {"source-hdr", W, H, GS_CS_709_EXTENDED, GS_CS_SRGB, source},
			 {"source-scrgb", W, H, GS_CS_709_SCRGB, GS_CS_SRGB, source},
			 {"canvas-16f", W, H, GS_CS_SRGB, GS_CS_SRGB_16F, source},
			 {"canvas-hdr", W, H, GS_CS_SRGB, GS_CS_709_EXTENDED, source},
			 {"canvas-scrgb", W, H, GS_CS_SRGB, GS_CS_709_SCRGB, source},
			 {"ready", W, H, GS_CS_SRGB, GS_CS_SRGB, source}};
	auto *properties = obs_source_properties(filter);
	out << "[{\"case\":\"before\",\"status\":" << json_string(status_text(properties)) << "}";
	obs_set_output_source(0, nullptr);
	for (auto &probe : cases) {
		if (probe.render == filter)
			obs_source_filter_remove(source, filter);
		obs_queue_task(
			OBS_TASK_GRAPHICS,
			[](void *data) {
				auto &p = *static_cast<Probe *>(data);
				obs_enter_graphics();
				source_width = p.width;
				source_height = p.height;
				source_space = p.source_color;
				auto *target = gs_texrender_create(p.render_color == GS_CS_SRGB ? GS_RGBA : GS_RGBA16F,
								   GS_ZS_NONE);
				if (gs_texrender_begin_with_color_space(target, W, H, p.render_color)) {
					gs_ortho(0, float(W), 0, float(H), -100, 100);
					obs_source_video_render(p.render);
					gs_texrender_end(target);
					p.rendered = true;
				}
				gs_texrender_destroy(target);
				obs_leave_graphics();
			},
			&probe, true);
		// Let an already-running worker finish: its result must not hide a blocker.
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		obs_property_button_clicked(obs_properties_get(properties, "refresh"), filter);
		auto *reopened = obs_source_properties(filter);
		out << ",{\"case\":" << json_string(probe.name)
		    << ",\"rendered\":" << (probe.rendered ? "true" : "false")
		    << ",\"status\":" << json_string(status_text(properties))
		    << ",\"reopened\":" << json_string(status_text(reopened)) << "}";
		obs_properties_destroy(reopened);
		if (probe.render == filter)
			obs_source_filter_add(source, filter);
	}
	obs_properties_destroy(properties);
	obs_set_output_source(0, source);
}
// Flatten properties for existing UI assertions, retaining each child's section.
static void dump_properties(std::ostream &out, obs_properties_t *props, bool &first, const char *group = "",
			    bool parent_visible = true)
{
	for (auto *prop = obs_properties_first(props); prop; obs_property_next(&prop)) {
		if (!first)
			out << ",";
		first = false;
		out << "{\"name\":" << json_string(obs_property_name(prop))
		    << ",\"label\":" << json_string(obs_property_description(prop))
		    << ",\"group\":" << json_string(group)
		    << ",\"visible\":" << (parent_visible && obs_property_visible(prop) ? "true" : "false")
		    << ",\"items\":[";
		if (obs_property_get_type(prop) == OBS_PROPERTY_LIST)
			for (size_t i = 0; i < obs_property_list_item_count(prop); ++i) {
				if (i)
					out << ",";
				out << json_string(obs_property_list_item_name(prop, i));
			}
		out << "]}";
		if (obs_property_get_type(prop) == OBS_PROPERTY_GROUP) {
			if (obs_property_group_type(prop) != OBS_GROUP_NORMAL)
				throw std::runtime_error("Effect sections must not add enable checkboxes");
			dump_properties(out, obs_property_group_content(prop), first, obs_property_name(prop),
					parent_visible && obs_property_visible(prop));
		}
	}
}
#include "preset_probe.h"
// The fixed Smooth profile of the C++ reference (tuning.h), Local Color off.
static constexpr std::array<int, beauti::kFinishSettings.size()> kReferenceFinishing{45,  85, 70,  20, 100,
										     100, 0,  100, 100};
int beauti_main(int argc, char **argv)
{
#ifdef __APPLE__
	@autoreleasepool {
#endif
		if (argc < 4) {
			std::cerr
				<< "bundle raw_rgba report [seconds quality backend retouch smooth width height motion transition reserved locale title skin_tone dodge_burn eyes]\n";
			return 2;
		}
#ifdef __APPLE__
		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
#endif
		if (argc > 10) {
			W = std::atoi(argv[9]);
			H = std::atoi(argv[10]);
		}
		motion = argc > 11 && std::atoi(argv[11]);
		source_width = W;
		source_height = H;
		fixture_cut = std::getenv("BEAUTI_FIXTURE_CUT") != nullptr;
		original.resize(W * H * 4);
		std::ifstream input(beauti::utf8_path(argv[2]), std::ios::binary);
		input.read(reinterpret_cast<char *>(original.data()), original.size());
		if (!input)
			return 3;
		const std::string bundle = beauti::utf8_string(std::filesystem::absolute(beauti::utf8_path(argv[1]))),
				  report = beauti::utf8_string(std::filesystem::absolute(beauti::utf8_path(argv[3])));
		if (!obs_startup(argc > 14 ? argv[14] : "en-US", nullptr, nullptr))
			return 4;
		const char *runtime = std::getenv("BEAUTI_OBS_RUNTIME");
#ifdef __APPLE__
		std::string obs = runtime ? runtime : "/Applications/OBS.app/Contents/Frameworks";
		std::string graphics = obs + "/libobs-opengl.dylib";
#else
	if (!runtime) {
		std::cerr << "Set BEAUTI_OBS_RUNTIME to the OBS installation directory\n";
		return 2;
	}
	std::string obs =
		beauti::utf8_string(std::filesystem::absolute(std::filesystem::path(_wgetenv(L"BEAUTI_OBS_RUNTIME"))));
	// libobs loads modules with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which ignores PATH.
	const auto obs_bin = std::filesystem::absolute(beauti::utf8_path(obs) / "bin/64bit");
	if (!AddDllDirectory(obs_bin.c_str())) {
		std::cerr << "Could not register OBS runtime directory\n";
		return 2;
	}
	// OBS 33 resolves core data relative to its bin/64bit working directory.
	std::filesystem::current_path(obs_bin);
	std::string graphics = obs + "/bin/64bit/libobs-d3d11.dll";
#endif
		obs_video_info video = {};
		video.graphics_module = graphics.c_str();
		video.fps_num = 30;
		if (const char *fps = std::getenv("BEAUTI_CANVAS_FPS"))
			video.fps_num = uint32_t(std::clamp(std::atoi(fps), 1, 120));
		video.fps_den = 1;
		video.base_width = W;
		video.base_height = H;
		video.output_width = W;
		video.output_height = H;
		video.output_format = VIDEO_FORMAT_BGRA;
		video.colorspace = VIDEO_CS_709;
		video.range = VIDEO_RANGE_FULL;
		video.scale_type = OBS_SCALE_BICUBIC;
		if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) {
			obs_shutdown();
			return 5;
		}
		obs_module_t *module = nullptr;
#ifdef __APPLE__
		const std::string module_path = bundle + "/Contents/MacOS/beauti",
				  data_path = bundle + "/Contents/Resources";
#else
	const std::string module_path = bundle + "/beauti.dll", data_path = bundle + "/data";
#endif
		if (obs_open_module(&module, module_path.c_str(), data_path.c_str()) != MODULE_SUCCESS ||
		    !obs_init_module(module)) {
			obs_shutdown();
			return 6;
		}
		obs_source_info info = {};
		info.id = "beauti_bench_source";
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
		info.get_name = [](void *) { return "Headless fixture"; };
		info.create = create_source;
		info.destroy = destroy_source;
		info.video_tick = tick_source;
		info.video_render = render_source;
		info.get_width = [](void *) { return source_width.load(); };
		info.get_height = [](void *) { return source_height.load(); };
		info.video_get_color_space = [](void *, size_t, const gs_color_space *) { return source_space.load(); };
		obs_register_source(&info);
		auto *source = obs_source_create_private(info.id, "fixture", nullptr);
		auto *settings = obs_data_create();
		// The runner sets strengths directly: Manual, with the reference Smooth profile
		// and Local Color off unless BEAUTI_FIXTURE_SETTINGS overrides them.
		obs_data_set_int(settings, "preset", beauti::kManualPreset);
		for (size_t i = 0; i < beauti::kFinishSettings.size(); ++i)
			obs_data_set_int(settings, beauti::kFinishSettings[i].key, kReferenceFinishing[i]);
		obs_data_set_int(settings, "retouch", argc > 7 ? std::atoi(argv[7]) : 60);
		obs_data_set_int(settings, "smooth", argc > 8 ? std::atoi(argv[8]) : 50);
		obs_data_set_int(settings, "native_quality", argc > 5 ? std::atoi(argv[5]) : 0);
		if (argc > 6 && std::strcmp(argv[6], "default"))
			obs_data_set_int(settings, "native_backend", std::atoi(argv[6]));
		obs_data_set_int(settings, "native_size", 1280);
		obs_data_set_int(settings, "native_fps", 30);
		if (const char *fps = std::getenv("BEAUTI_FIXTURE_FPS"))
			obs_data_set_int(settings, "native_fps", std::atoi(fps));
		const int initial_tone = argc > 16 ? std::atoi(argv[16]) : 0;
		obs_data_set_int(settings, "skin_tone", initial_tone);
		const int initial_dodge = argc > 17 ? std::atoi(argv[17]) : 50;
		obs_data_set_int(settings, "dodge_burn", initial_dodge);
		const int initial_eyes = argc > 18 ? std::atoi(argv[18]) : 0;
		obs_data_set_int(settings, "eyes", initial_eyes);
#ifdef _WIN32
		const wchar_t *settings_path = _wgetenv(L"BEAUTI_FIXTURE_SETTINGS");
		const std::string settings_file =
			settings_path ? beauti::utf8_string(std::filesystem::path(settings_path)) : "";
#else
	const char *settings_path = std::getenv("BEAUTI_FIXTURE_SETTINGS");
	const std::string settings_file = settings_path ? settings_path : "";
#endif
		if (settings_path) {
			auto *extra = obs_data_create_from_json_file(settings_file.c_str());
			if (!extra)
				throw std::runtime_error("Could not read fixture settings JSON");
			obs_data_apply(settings, extra);
			obs_data_release(extra);
		}
		const char *filter_title = argc > 15 ? argv[15] : "Beauti";
		auto *filter = obs_source_create_private("beauti_filter", filter_title, settings);
		obs_data_release(settings);
		if (!source || !filter)
			return 7;
		if (std::getenv("BEAUTI_FIXTURE_PRESETS"))
			probe_presets(filter, report);
		obs_source_filter_add(source, filter);
		obs_set_output_source(0, source);
		start = os_gettime_ns();
		obs_add_raw_video_callback(nullptr, receive, nullptr);
		double seconds = argc > 4 ? std::atof(argv[4]) : 20;
		int transition = -1;
		int switch_controls = argc > 12 ? std::atoi(argv[12]) : 0;
		const char *control_delay = std::getenv("BEAUTI_FIXTURE_CONTROL_START_SECONDS");
		const double control_start = control_delay ? std::max(0.0, std::atof(control_delay)) : 0.0;
		uint32_t lagged_at_warmup = 0, total_at_warmup = 0;
		bool warmed = false;
		bool status_probed = false;
		std::ofstream status_report;
		if (std::getenv("BEAUTI_FIXTURE_SOURCE_STATUS"))
			status_report.open(beauti::utf8_path(report + ".status.json"));
		auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
		while (std::chrono::steady_clock::now() < until) {
			if (status_report.is_open() && !status_probed && double(os_gettime_ns() - start) / 1e9 >= 3) {
				probe_source_status(source, filter, status_report);
				status_probed = true;
			}
			if (!warmed && double(os_gettime_ns() - start) / 1e9 >= 2.0) {
				warmed = true;
				lagged_at_warmup = obs_get_lagged_frames();
				total_at_warmup = obs_get_total_frames();
			}
			if (switch_controls && double(os_gettime_ns() - start) / 1e9 >= control_start) {
				double elapsed = double(os_gettime_ns() - start) / 1e9 - control_start;
				int next = (switch_controls == 2 || switch_controls == 3 || switch_controls == 5 ||
					    switch_controls == 6 || switch_controls == 8 || switch_controls == 10 ||
					    switch_controls == 12 || switch_controls == 14)
						   ? int(elapsed * 30)
						   : int(elapsed / 2);
				if (next != transition) {
					transition = next;
					if (switch_controls == 13) {
						const size_t choice = size_t(next) % (beauti::kLookPresets.size() + 1);
						select_preset(filter, choice ? beauti::kLookPresets[choice - 1].id : 0);
						continue;
					}
					int r = 60, s = 50, t = initial_tone, d = initial_dodge, e = initial_eyes;
					if (switch_controls == 1) {
						r = next % 2 ? 0 : 60;
						s = next % 2 ? 0 : 50;
						t = next % 2 ? 0 : initial_tone;
						d = next % 2 ? 0 : initial_dodge;
					} else if (switch_controls == 8 || switch_controls == 9) {
						r = s = t = d = 0;
						e = switch_controls == 9 ? (next % 2 ? 0 : 100)
									 : 60 + int(30 * std::sin(elapsed * 7));
					} else if (switch_controls == 6) {
						r = 0;
						s = 0;
						t = 0;
						d = elapsed >= 2 && elapsed < 8 ? 60 + int(30 * std::sin(elapsed * 7))
										: 60;
					} else if (switch_controls == 7) {
						r = 0;
						s = 0;
						t = 0;
						d = next % 2 ? 0 : 100;
					} else if (switch_controls == 4) {
						r = 0;
						s = 0;
						d = 0;
						t = next % 2 ? 0 : 100;
					} else if (switch_controls == 3) {
						r = 0;
						s = 0;
						d = 0;
						t = elapsed >= 2 && elapsed < 8 ? 60 + int(30 * std::sin(elapsed * 7))
										: 60;
					} else if (elapsed >= 2 && elapsed < 8) {
						r = 60 + int(30 * std::sin(elapsed * 7));
						s = 50 + int(25 * std::cos(elapsed * 9));
						if (switch_controls == 5) {
							t = 60 + int(30 * std::sin(elapsed * 5));
							d = 50 + int(25 * std::cos(elapsed * 6));
						}
					}
					auto *change = obs_source_get_settings(filter);
					if (switch_controls == 14) {
						// All effects off between 4 and 10 seconds.
						const bool off = elapsed >= 4 && elapsed < 10;
						r = off ? 0 : 60;
						s = off ? 0 : 50;
						t = off ? 0 : 50;
						d = off ? 0 : 50;
						e = 0;
						obs_data_set_int(change, "local_color", off ? 0 : 30);
					}
					if (switch_controls == 12) {
						r = t = d = e = 0;
						s = 80;
						obs_data_set_int(change, "smooth_fine",
								 25 + int(20 * std::sin(elapsed * 7)));
						obs_data_set_int(change, "smooth_medium",
								 70 + int(20 * std::sin(elapsed * 6)));
						obs_data_set_int(change, "smooth_size",
								 100 + int(40 * std::sin(elapsed * 5)));
						obs_data_set_int(change, "smooth_edges",
								 100 + int(80 * std::sin(elapsed * 4)));
					}
					if (switch_controls == 10 || switch_controls == 11) {
						r = s = t = d = e = 0;
						obs_data_set_int(change, "local_color",
								 switch_controls == 11
									 ? (next % 2 ? 0 : 100)
									 : 60 + int(30 * std::sin(elapsed * 7)));
						if (switch_controls == 10) {
							obs_data_set_int(change, "smooth_size",
									 100 + int(40 * std::sin(elapsed * 5)));
							obs_data_set_int(change, "local_color_size",
									 100 + int(50 * std::sin(elapsed * 4)));
						}
					}
					obs_data_set_int(change, "retouch", r);
					obs_data_set_int(change, "skin_tone", t);
					obs_data_set_int(change, "dodge_burn", d);
					obs_data_set_int(change, "smooth", s);
					obs_data_set_int(change, "eyes", e);
					obs_source_update(filter, change);
					obs_data_release(change);
				}
			}
#ifdef __APPLE__
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
#else
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
		}
		obs_remove_raw_video_callback(receive, nullptr);
		uint32_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
		double render_ms = double(obs_get_average_frame_time_ns()) / 1e6;
		auto *effective_settings = obs_source_get_settings(filter);
		std::ofstream(beauti::utf8_path(report + ".settings.json"))
			<< obs_data_get_json_with_defaults(effective_settings) << "\n";
		std::ofstream ui(beauti::utf8_path(report + ".ui.json"));
		ui << "{\"filter_name\":" << json_string(obs_source_get_name(filter))
		   << ",\"selected_backend\":" << obs_data_get_int(effective_settings, "native_backend")
		   << ",\"backend_has_user_value\":"
		   << (obs_data_has_user_value(effective_settings, "native_backend") ? "true" : "false")
		   << ",\"properties\":[";
		bool ui_first = true;
		obs_data_release(effective_settings);
		auto *props = obs_source_properties(filter);
		dump_properties(ui, props, ui_first);
		ui << "]}\n";
		std::cerr << "VERSION: " << obs_property_description(obs_properties_get(props, "version")) << "\n";
		const char *description = obs_property_description(obs_properties_get(props, "status"));
		if (status_probed)
			status_report << ",{\"case\":\"recovered\",\"status\":" << json_string(description) << "}]\n";
		std::cerr << "STATUS: " << (description ? description : "") << "\n";
		obs_properties_destroy(props);
		obs_set_output_source(0, nullptr);
		obs_source_filter_remove(source, filter);
		obs_source_release(filter);
		obs_source_release(source);
		// libobs destroys released sources on a task thread; like the OBS frontend,
		// wait for that before shutdown so the filter's worker is joined while the
		// module is still loaded.
		obs_wait_for_destroy_queue();
		obs_shutdown();
		std::ofstream out(beauti::utf8_path(report));
		out << "{\"width\":" << W << ",\"height\":" << H << ",\"motion\":" << (motion ? "true" : "false")
		    << ",\"render_ms\":" << render_ms << ",\"total_frames\":" << total
		    << ",\"lagged_frames\":" << lagged
		    << ",\"lagged_frames_after_warmup\":" << (warmed ? lagged - lagged_at_warmup : lagged)
		    << ",\"frames_after_warmup\":" << (warmed ? total - total_at_warmup : total)
		    << ",\"warmup_seconds\":2,\"control_start_seconds\":" << control_start << ",\"samples\":[";
		bool first = true;
		for (auto s : samples) {
			if (!first)
				out << ",";
			first = false;
			out << "[" << s.time << "," << s.id << "," << s.current << "," << s.delta << "," << s.white
			    << "]";
		}
		out << "]}\n";
		std::ofstream raw(beauti::utf8_path(report + ".rgba"), std::ios::binary);
		raw.write(reinterpret_cast<const char *>(last_pixels.data()), last_pixels.size());
		return 0;
#ifdef __APPLE__
	}
#endif
}
#include "cli_entry.h"
