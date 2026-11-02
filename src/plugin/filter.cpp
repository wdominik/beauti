// SPDX-License-Identifier: GPL-3.0-or-later
// Native, bounded latest-frame pipeline. The graphics thread never runs inference.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include "engine.h"
#include "timing.h"
#include "continuity.h"
#include "pacing.h"
#include "notice.h"
#include <array>
#include "backend.h"
#include "paths.h"
#include "beauti-version.h"
#include "gpu.h"
#include "preset_settings.h"
#include "gs_handles.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
OBS_DECLARE_MODULE()
namespace {
constexpr uint64_t kStrengthRampNs = 150000000; // 150 ms slider transitions
constexpr uint32_t kMaxSourceSide = 4096;
constexpr uint32_t kSnapshotSlots = 8, kReadbackSlots = 2;
// Scene-cut heuristic: a 16x16 RGB sample grid taken from each readback.
constexpr unsigned kSceneGrid = 16, kSceneSamples = kSceneGrid * kSceneGrid * 3;
constexpr unsigned kSceneBlockChange = 240, kSceneChangedBlocks = 180;
constexpr double kSceneMeanChange = 70;
constexpr uint32_t kLogFirstFrames = 5, kLogEveryFrames = 90;
// On-disk layout of the provider cache; change when the layout changes.
constexpr const char *kCacheNamespace = "native-0.6";
}
OBS_MODULE_USE_DEFAULT_LOCALE("beauti", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Beauti skin retouching, tone correction, local lightness correction, and smoothing";
}
MODULE_EXPORT const char *obs_module_name(void)
{
	return "Beauti";
}
MODULE_EXPORT const char *obs_module_author(void)
{
	return "wdominik";
}
using beauti::Frame;
static std::string resource(const char *name)
{
	char *p = obs_module_file(name);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}
// Provider cache directory keyed by the bundle's model identifier, so a model
// update never reads a stale compiled TensorRT program. Core AI keeps its own cache.
static std::string cache_path()
{
	std::string key;
	std::ifstream file(beauti::utf8_path(resource("cache-key.txt")));
	file >> key;
	if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
		throw std::runtime_error("The model identifier is missing from the bundle");
	return beauti::utf8_string(beauti::cache_root() / kCacheNamespace / key);
}
// Tokens have no GPU destructor. Texture ownership stays on the render thread.
struct CaptureToken {
	uint32_t slot = 0, width = 0, height = 0;
};
// One analysis in flight: the reduced frame for the worker, the maps it returns,
// and the token of the full-resolution snapshot the result belongs to.
struct Job {
	Frame frame;
	beauti::Maps maps;
	std::shared_ptr<CaptureToken> token;
	uint32_t quality = 0;
};
// Full-resolution render of the source. The slot is free again once nothing else
// holds its token (use_count() == 1).
struct Snapshot {
	beauti::TexRender render;
	std::shared_ptr<CaptureToken> token;
};
// Staging surface for the reduced analysis image; mapped one tick after it was issued.
struct Readback {
	beauti::StageSurface surface;
	bool valid = false;
	Job job;
};
// Smoothstep over kStrengthRampNs. Retargets from the current value, never from
// zero, so a slider dragged mid-ramp continues without a jump. Render thread only.
struct StrengthRamp {
	float value = 0, from = 0, target = 0;
	uint64_t started = 0;
	float sample(float requested, uint64_t now)
	{
		if (started) {
			float t = now >= started ? std::min(1.f, float(now - started) / float(kStrengthRampNs)) : 1.f;
			value = from + (target - from) * (t * t * (3 - 2 * t));
		}
		if (requested != target) {
			from = value;
			target = requested;
			started = now;
		}
		return value;
	}
};
enum class SourceState { Ready, Missing, Empty, TooLarge, SourceColor, RenderColor, Unseen };

// One filter instance. Fields up to display_valid are render-thread state; from
// stopping on, the state shared with the worker: atomics are written by update(),
// the rest is guarded by mutex and signaled through event.
struct Filter {
	obs_source_t *source = nullptr;
	beauti::TexRender capture;
	gs_texture_t *texture = nullptr; // borrowed: a snapshot or the compositor's output
	std::unique_ptr<SkinGPU> gpu;
	Snapshot snapshots[kSnapshotSlots];
	Job active;
	beauti::Continuity continuity;
	StrengthRamp presentation;
	beauti::Effect continuity_effect;
	std::shared_ptr<CaptureToken> latest;
	obs_source_t *last_target = nullptr;
	uint32_t context_w = 0, context_h = 0, context_gen = 0;
	std::array<uint8_t, kSceneSamples> scene_samples{};
	bool have_scene = false;
	const char *last_continuity_reason = nullptr;
	// Silent pass-through and readback failures are logged when their reason changes
	// (render thread), so a tester can see why the image stays unprocessed.
	SourceState last_source_state = SourceState::Unseen; // first evaluation always logs
	bool readback_failing = false, capture_failing = false;
	std::atomic<uint64_t> errors{0};
	uint64_t seen_errors = 0;
	StrengthRamp retouch_ramp, tone_ramp, dodge_ramp, smooth_ramp, eye_ramp;
	// UI writes atomic targets; ramps and the last composed configuration are render-owned.
	std::array<std::atomic<int>, beauti::kFinishSettings.size()> finish_targets{};
	std::array<StrengthRamp, beauti::kFinishSettings.size()> finish_ramps{};
	beauti::FinishingControls shown_finishing;
	bool finish_initialized = false;
	// The preset applied by the last apply_settings (-1 before the first); written by
	// create and then only by OBS's deferred update on the video thread.
	int64_t applied_preset = -1;
	float shown_retouch = -1, shown_tone = -1, shown_dodge = -1, shown_smooth = -1, shown_eyes = -1;
	Readback stages[kReadbackSlots];
	uint32_t stage_w = 0, stage_h = 0, next_stage = 0, texture_w = 0, texture_h = 0, sequence = 0,
		 shown_generation = 0;
	beauti::CapturePacer capture_pacer;
	uint64_t last_result = 0;
	bool captured_this_tick = false, display_valid = false;
	std::atomic<bool> stopping{false};
	std::atomic<uint32_t> settings_revision{0}; // bumped when the device, quality, or analysis size changes
	std::atomic<uint32_t> generation{1}, retouch{60}, skin_tone{50}, dodge_burn{50}, smooth{50}, eyes{0},
		long_side{1280}, fps{30}, backend{beauti::default_backend()}, quality{0};
	std::mutex mutex;
	std::condition_variable event;
	bool has_job = false, has_result = false, configured = false;
	uint64_t replaced = 0;
	Job job, result;
	std::string status = obs_module_text("Starting");
	// The render thread publishes source blockers under mutex. They take priority
	// over worker status in the UI, including results completed after a source fails.
	std::string source_status;
	const std::array<std::string, 6> source_messages{
		"",
		obs_module_text("SourceMissing"),
		obs_module_text("SourceEmpty"),
		std::format("{} ({} px)", obs_module_text("SourceTooLarge"), kMaxSourceSide),
		obs_module_text("SourceColorUnsupported"),
		obs_module_text("RenderColorUnsupported")};
	// Locale strings the worker thread needs, read once on the OBS thread. libobs
	// defers source destruction and may free the module's locale table before a
	// pending destroy joins the worker, so the worker never calls obs_module_text.
	const std::string text_starting = obs_module_text("Starting"), text_fps = obs_module_text("FramesPerSecond"),
			  text_no_face = obs_module_text("NoFaceSuffix"),
			  text_failed = obs_module_text("ProcessingFailed"),
			  text_cpu_fallback =
				  obs_module_text(beauti::is_runtime_file_problem(beauti::accelerator_reason())
							  ? "NVIDIAFilesMissingSuffix"
							  : "CPUFallbackSuffix");
	// True when this build has an accelerator that the machine lacks: new filters
	// then default to the CPU and the status line says so (docs/decisions/0015).
	const bool accelerator_missing =
		!beauti::accelerator_available() && beauti::backend_choices().front().id != beauti::CPU;
	std::thread thread;
	// Worker thread: one job at a time, in this order. State that only the
	// worker touches lives in WorkerState.
	struct WorkerState {
		std::unique_ptr<beauti::Engine> engine;
		uint32_t backend = UINT32_MAX, quality = UINT32_MAX;
		std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
		unsigned count = 0;
		float measured_fps = 0;
		std::string last_error;
		std::chrono::seconds retry_delay = beauti::kFirstRetryDelay;
		uint32_t retry_revision = 0;
		int last_face_status = -1;
#ifdef BEAUTI_CONTINUITY_TEST
		std::chrono::steady_clock::time_point test_start = std::chrono::steady_clock::now();
		unsigned injected = 0;
		bool wait_for_test_result = false;
#endif
	};
	void worker();
	bool take_job(Job &work);
	void ensure_engine(WorkerState &state, const Job &work);
	void refresh_job(Job &work);
	std::string visible_status();
	void note_passthrough(SourceState state, uint32_t w, uint32_t h, gs_color_space source_space,
			      gs_color_space render_space);
#ifdef BEAUTI_CONTINUITY_TEST
	void inject_test_fault(WorkerState &state, Job &work);
#endif
	void publish_result(WorkerState &state, Job &&work);
	void report_error(WorkerState &state, const std::exception &error);
	// Render-thread steps, called from render() in this order once per video frame.
	struct RenderContext {
		obs_source_t *target = nullptr, *parent = nullptr;
		uint32_t width = 0, height = 0;      // source size
		uint32_t scaled_w = 0, scaled_h = 0; // analysis size after long_side scaling
		uint32_t gen = 0;                    // analysis generation this frame works with
		uint64_t now = 0;
	};
	struct Strengths {
		float retouch, skin_tone, dodge_burn, smooth, eyes;
	};
	void render();
	bool prepare_context(RenderContext &ctx);
	bool receive_result(const RenderContext &ctx);
	void compose_active(const RenderContext &ctx, bool new_analysis);
	void service_readbacks(RenderContext &ctx);
	void consume_readback(Readback &slot, RenderContext &ctx);
	bool detect_scene_cut(const std::vector<uint8_t> &pixels, uint32_t w, uint32_t h);
	void capture_snapshot(const RenderContext &ctx);
	void present(const RenderContext &ctx);
	void clear_stages();
	void reset_display();
	void invalidate_context();
	uint32_t restart_generation();
};
// Wait for the next job; false when the filter is being destroyed.
bool Filter::take_job(Job &work)
{
	std::unique_lock<std::mutex> lock(mutex);
	event.wait(lock, [&] { return stopping.load() || has_job; });
	if (stopping.load())
		return false;
	work = std::move(job);
	has_job = false;
	return true;
}
// (Re)create the engine when the device or quality of the job differs.
void Filter::ensure_engine(WorkerState &state, const Job &work)
{
	if (state.engine && state.backend == work.frame.backend && state.quality == work.quality)
		return;
	state.engine.reset();
	state.engine =
		std::make_unique<beauti::Engine>(resource("models"), cache_path(), work.frame.backend, work.quality);
	state.backend = work.frame.backend;
	state.quality = work.quality;
	blog(LOG_INFO,
	     "[beauti] " BEAUTI_VERSION " engine ready: backend=%u quality=%u; GPU full-resolution compositor",
	     state.backend, state.quality);
}
// Engine creation can take seconds; analyze the newest frame of the same
// generation rather than the one that was current when the wait began.
void Filter::refresh_job(Job &work)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (has_job && job.frame.generation == work.frame.generation) {
		work = std::move(job);
		has_job = false;
	}
}
#ifdef BEAUTI_CONTINUITY_TEST
// Test-only module: BEAUTI_CONTINUITY_TEST selects a fault after four seconds.
void Filter::inject_test_fault(WorkerState &state, Job &work)
{
	const char *test = std::getenv("BEAUTI_CONTINUITY_TEST");
	const double test_seconds =
		std::chrono::duration<double>(std::chrono::steady_clock::now() - state.test_start).count();
	if (!test || test_seconds < 4)
		return;
	const std::string mode = test;
	auto &frame = work.frame;
	if ((mode == "miss" && state.injected < 1) || (mode == "misses" && state.injected < 2)) {
		frame.status = 1;
		work.maps = beauti::Maps{};
		++state.injected;
		state.wait_for_test_result = true;
		blog(LOG_INFO, "[beauti-test] no-face %u", state.injected);
	} else if (mode == "stall" && state.injected++ == 0) {
		blog(LOG_INFO, "[beauti-test] stall");
		std::this_thread::sleep_for(std::chrono::milliseconds(2400));
	} else if (mode == "error" && state.injected++ == 0) {
		throw std::runtime_error("Injected continuity test failure");
	} else if (mode == "errors") {
		blog(LOG_INFO, "[beauti-test] error at %.3f", test_seconds);
		throw std::runtime_error("Injected continuity test failure");
	} else if (mode == "delay" && test_seconds < 8) {
		std::this_thread::sleep_for(std::chrono::milliseconds(260));
	}
}
#endif
// Update the measured rate and status text, log periodically, and hand the
// result to the render thread unless its generation is already obsolete.
void Filter::publish_result(WorkerState &state, Job &&work)
{
	auto &frame = work.frame;
	const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - state.window).count();
	if (seconds >= 1) {
		state.measured_fps = float(state.count) / seconds;
		state.count = 0;
		state.window = std::chrono::steady_clock::now();
	}
	const float age = float(os_gettime_ns() - frame.captured_ns) / 1e6f;
	std::string text =
		std::format("{} × {} · {:.1f} {}{}{}", work.token->width, work.token->height, state.measured_fps,
			    text_fps, frame.status == 1 ? text_no_face : "",
			    accelerator_missing && frame.used_backend == beauti::CPU ? text_cpu_fallback : "");
	if (int(frame.status) != state.last_face_status) {
		blog(LOG_INFO, "[beauti] " BEAUTI_VERSION " analysis: %s",
		     frame.status == 1 ? "no face detected; the image passes through until a face returns"
				       : "face found; processing");
		state.last_face_status = int(frame.status);
	}
	if (frame.sequence < kLogFirstFrames || frame.sequence % kLogEveryFrames == 0)
		blog(LOG_INFO,
		     "[beauti] GPU " BEAUTI_VERSION
		     " frame=%u analysis=%ux%u output=%ux%u backend=%u quality=%u total=%.1fms detect=%.1f infer=%.1f post=%.1f age=%.1fms rate=%.1f",
		     frame.sequence, frame.width, frame.height, work.token->width, work.token->height,
		     frame.used_backend, state.quality, frame.milliseconds, frame.detect_ms, frame.infer_ms,
		     frame.post_ms, age, state.measured_fps);
	std::lock_guard<std::mutex> lock(mutex);
	if (frame.generation == generation.load()) {
		status = std::move(text);
		result = std::move(work);
		has_result = true;
	}
}
// Drop the engine, publish the failure, log it once, and pause before retrying. A
// failure that persists waits longer each time; changed settings retry immediately.
void Filter::report_error(WorkerState &state, const std::exception &error)
{
	errors.fetch_add(1);
	state.engine.reset();
	{
		std::lock_guard<std::mutex> lock(mutex);
		status = text_failed + ": " + error.what();
		has_result = false;
		result = Job{};
	}
	if (state.last_error != error.what())
		blog(LOG_ERROR, "[beauti] native " BEAUTI_VERSION ": %s", error.what());
	state.last_error = error.what();
	const uint32_t revision = settings_revision.load();
	if (revision != state.retry_revision) {
		state.retry_revision = revision;
		state.retry_delay = beauti::kFirstRetryDelay;
	}
	const auto delay = state.retry_delay;
	state.retry_delay = beauti::next_retry_delay(delay);
	std::unique_lock<std::mutex> lock(mutex);
	event.wait_for(lock, delay, [&] { return stopping.load() || settings_revision.load() != revision; });
}
void Filter::worker()
{
	WorkerState state;
	while (!stopping.load()) {
		Job work;
		if (!take_job(work))
			break;
		try {
			ensure_engine(state, work);
			if (stopping.load())
				break;
			refresh_job(work);
			auto &frame = work.frame;
			if (beauti::timing_enabled())
				beauti::timing_log("capture_to_inference", frame.sequence,
						   double(os_gettime_ns() - frame.captured_ns) / 1e6);
			state.engine->analyze(frame, work.maps);
			if (!frame.warning.empty())
				blog(LOG_WARNING, "[beauti] %s", frame.warning.c_str());
			frame.pixels.clear();
			++state.count;
#ifdef BEAUTI_CONTINUITY_TEST
			inject_test_fault(state, work);
#endif
			publish_result(state, std::move(work));
#ifdef BEAUTI_CONTINUITY_TEST
			if (state.wait_for_test_result) {
				// A fast engine can overwrite an injected miss before OBS observes it.
				// Make the test fault observable without changing production scheduling.
				std::unique_lock lock(mutex);
				event.wait_for(lock, std::chrono::milliseconds(100),
					       [&] { return !has_result || stopping.load(); });
				state.wait_for_test_result = false;
			}
#endif
			state.last_error.clear();
			state.retry_delay = beauti::kFirstRetryDelay;
		} catch (const std::exception &e) {
			report_error(state, e);
		}
	}
}
// OBS calls the functions below from C; an exception escaping one of them would
// terminate OBS. Run the body, log a failure, and let the caller degrade.
template<typename Body> static void guarded(const char *callback, Body &&body) noexcept
{
	try {
		body();
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[beauti] " BEAUTI_VERSION " %s: %s", callback, e.what());
	} catch (...) {
		blog(LOG_ERROR, "[beauti] " BEAUTI_VERSION " %s: unknown exception", callback);
	}
}
static const char *filter_name(void *)
{
	return obs_module_text("FilterName");
}
static void defaults(obs_data_t *s)
{
	// The defaults are the default preset, so OBS's Defaults button restores that look.
	const auto &look = beauti::kDefaultLook;
	obs_data_set_default_int(s, "preset", look.id);
	for (size_t i = 0; i < beauti::kStrengthKeys.size(); ++i)
		obs_data_set_default_int(s, beauti::kStrengthKeys[i], look.strengths[i]);
	for (size_t i = 0; i < beauti::kFinishSettings.size(); ++i)
		obs_data_set_default_int(s, beauti::kFinishSettings[i].key, look.finishing[i]);
	obs_data_set_default_int(s, "native_size", 1280);
	obs_data_set_default_int(s, "native_fps", 30);
	obs_data_set_default_int(s, "native_backend", beauti::default_backend());
	obs_data_set_default_int(s, "native_quality", 0);
}
static void apply_settings(Filter *f, obs_data_t *s)
{
	beauti::apply_look_preset(s, obs_data_get_int(s, "preset") != f->applied_preset);
	f->applied_preset = obs_data_get_int(s, "preset");
	for (size_t i = 0; i < beauti::kFinishSettings.size(); ++i) {
		const auto &setting = beauti::kFinishSettings[i];
		f->finish_targets[i] =
			int(std::clamp<int64_t>(obs_data_get_int(s, setting.key), setting.minimum, setting.maximum));
	}
	f->retouch = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "retouch"), 0, 100));
	f->eyes = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "eyes"), 0, 100));
	f->smooth = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "smooth"), 0, 100));
	f->skin_tone = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "skin_tone"), 0, 100));
	f->dodge_burn = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "dodge_burn"), 0, 100));
	const uint32_t side = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "native_size"), 640, 1920));
	const uint32_t backend = beauti::normalize_backend(obs_data_get_int(s, "native_backend"));
	const uint32_t quality = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "native_quality"), 0, 1));
	// Strength/fps changes do not invalidate image analysis or restart the models.
	bool reconfigure = f->long_side.exchange(side) != side;
	reconfigure = (f->backend.exchange(backend) != backend) || reconfigure;
	reconfigure = (f->quality.exchange(quality) != quality) || reconfigure;
	f->fps = uint32_t(std::clamp<int64_t>(obs_data_get_int(s, "native_fps"), 1, 60));
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		if (!f->configured || reconfigure) {
			f->configured = true;
			f->generation.fetch_add(1);
			f->has_job = false;
			f->has_result = false;
			f->job = Job{};
			f->result = Job{};
			f->status = obs_module_text("Starting");
			if (reconfigure)
				f->settings_revision.fetch_add(1);
		}
	}
	if (reconfigure)
		f->event.notify_all();
}
static void update(void *data, obs_data_t *s) noexcept
{
	guarded("update", [&] { apply_settings(static_cast<Filter *>(data), s); });
}
static Filter *create_filter(obs_data_t *s, obs_source_t *source)
{
	auto f = std::make_unique<Filter>();
	f->source = source;
	if (f->accelerator_missing) {
		static bool logged = false; // the probe runs once per process
		if (!logged)
			blog(LOG_INFO,
			     "[beauti] " BEAUTI_VERSION
			     " acceleration unavailable (%s); new filters default to the CPU",
			     beauti::accelerator_reason().c_str());
		logged = true;
	}
	apply_settings(f.get(), s);
	obs_enter_graphics();
	try {
		f->capture.reset(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
		if (!f->capture)
			throw std::runtime_error("Could not create the image buffer");
		f->gpu = std::make_unique<SkinGPU>(resource("beauti.effect"));
		char *error = nullptr;
		f->continuity_effect.reset(gs_effect_create_from_file(resource("continuity.effect").c_str(), &error));
		std::string message = error ? error : "Could not create continuity effect";
		bfree(error);
		if (!f->continuity_effect)
			throw std::runtime_error(message);
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[beauti] " BEAUTI_VERSION " GPU: %s", e.what());
		f.reset();
		obs_leave_graphics();
		return nullptr;
	}
	obs_leave_graphics();
	try {
		Filter *raw = f.get();
		f->thread = std::thread([raw] { raw->worker(); });
	} catch (...) {
		obs_enter_graphics();
		f.reset();
		obs_leave_graphics();
		return nullptr;
	}
	return f.release();
}
static void *create(obs_data_t *s, obs_source_t *source) noexcept
{
	Filter *created = nullptr;
	guarded("create", [&] { created = create_filter(s, source); });
	return created;
}
static void destroy_filter(void *data)
{
	std::unique_ptr<Filter> f(static_cast<Filter *>(data));
	{
		// Under the mutex: the worker cannot be between its check and its wait, where
		// the notification would be lost and the join below would never return.
		std::lock_guard<std::mutex> lock(f->mutex);
		f->stopping = true;
	}
	f->event.notify_all();
	if (f->thread.joinable())
		f->thread.join();
	// Snapshots, readback surfaces, effects, and the compositor are released
	// by their owning handles, which must run inside the graphics context.
	obs_enter_graphics();
	f.reset();
	obs_leave_graphics();
}
static void destroy(void *data) noexcept
{
	guarded("destroy", [&] { destroy_filter(data); });
}
std::string Filter::visible_status()
{
	std::lock_guard<std::mutex> lock(mutex);
	return source_status.empty() ? status : source_status;
}
static bool refresh_status(obs_properties_t *p, obs_property_t *, void *data) noexcept
{
	guarded("refresh_status", [&] {
		auto *f = static_cast<Filter *>(data);
		if (f)
			obs_property_set_description(obs_properties_get(p, "status"), f->visible_status().c_str());
	});
	return true;
}
// Normal groups organize controls without introducing another enable state.
static obs_properties_t *section(obs_properties_t *parent, const char *key, const char *label)
{
	auto *child = obs_properties_create();
	obs_properties_add_group(parent, key, obs_module_text(label), OBS_GROUP_NORMAL, child);
	return child;
}
static obs_property_t *percent_slider(obs_properties_t *p, const char *key, const char *label, int lo = 0, int hi = 100)
{
	auto *slider = obs_properties_add_int_slider(p, key, obs_module_text(label), lo, hi, 1);
	obs_property_int_set_suffix(slider, "%");
	return slider;
}
static void finishing_properties(obs_properties_t *p, size_t begin, size_t end)
{
	for (size_t i = begin; i < end; ++i) {
		const auto &setting = beauti::kFinishSettings[i];
		percent_slider(p, setting.key, setting.label, setting.minimum, setting.maximum);
	}
}
// Visibility is UI-owned. Applying a preset updates ordinary effect settings,
// allowing OBS to serialize them and refresh sliders when Manual is selected.
static void show_manual_controls(obs_properties_t *p, bool manual)
{
	for (const auto *name :
	     {"retouch_section", "tone_section", "color_section", "dodge_section", "smooth_section", "eyes_section"})
		obs_property_set_visible(obs_properties_get(p, name), manual);
}
static bool preset_changed(obs_properties_t *p, obs_property_t *, obs_data_t *s) noexcept
{
	guarded("preset", [&] {
		beauti::apply_look_preset(s, true);
		show_manual_controls(p, !beauti::look_preset(obs_data_get_int(s, "preset")));
	});
	return true;
}
static void fill_properties(obs_properties_t *p, Filter *f)
{
	auto *preset = obs_properties_add_list(p, "preset", obs_module_text("Preset"), OBS_COMBO_TYPE_LIST,
					       OBS_COMBO_FORMAT_INT);
	for (const auto &look : beauti::kLookPresets)
		obs_property_list_add_int(preset, obs_module_text(look.label), look.id);
	obs_property_list_add_int(preset, obs_module_text("Manual"), beauti::kManualPreset);
	obs_property_set_modified_callback(preset, preset_changed);
	percent_slider(section(p, "retouch_section", "Retouch"), "retouch", "Strength");
	percent_slider(section(p, "tone_section", "SkinTone"), "skin_tone", "Strength");
	auto *color = section(p, "color_section", "LocalColor");
	finishing_properties(color, size_t(beauti::FinishControl::Color), beauti::kFinishSettings.size());
	obs_property_set_long_description(obs_properties_get(color, "local_color"), obs_module_text("LocalColorHelp"));
	percent_slider(section(p, "dodge_section", "DodgeBurn"), "dodge_burn", "Strength");
	auto *texture_section = section(p, "smooth_section", "Smooth");
	percent_slider(texture_section, "smooth", "Strength");
	finishing_properties(texture_section, 0, size_t(beauti::FinishControl::Color));
	obs_property_set_long_description(obs_properties_get(texture_section, "smooth"), obs_module_text("SmoothHelp"));
	auto *e = percent_slider(section(p, "eyes_section", "Eyes"), "eyes", "Strength");
	obs_property_set_long_description(e, obs_module_text("EyesHelp"));
	auto *processing = section(p, "processing_section", "Processing");
	auto *q = obs_properties_add_list(processing, "native_quality", obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
					  OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(q, obs_module_text("Performance"), 0);
	obs_property_list_add_int(q, obs_module_text("QualityMode"), 1);
	auto *size = obs_properties_add_list(processing, "native_size", obs_module_text("AnalysisResolution"),
					     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(size, obs_module_text("AnalysisResolutionHelp"));
	obs_property_list_add_int(size, obs_module_text("Resolution960"), 960);
	obs_property_list_add_int(size, obs_module_text("RecommendedResolution"), 1280);
	obs_property_list_add_int(size, obs_module_text("Resolution1920"), 1920);
	auto *b = obs_properties_add_list(processing, "native_backend", obs_module_text("ProcessingDevice"),
					  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (const auto &choice : beauti::backend_choices())
		obs_property_list_add_int(b, obs_module_text(choice.locale_key), choice.id);
	obs_properties_add_int(processing, "native_fps", obs_module_text("TargetFrameRate"), 1, 60, 1);
	const std::string state = f ? f->visible_status() : obs_module_text("Starting");
	obs_properties_add_text(p, "status", state.c_str(), OBS_TEXT_INFO);
	obs_properties_add_button2(p, "refresh", obs_module_text("RefreshStatus"), refresh_status, f);
	std::string version = std::string(obs_module_text("Version")) + " " + BEAUTI_VERSION;
	obs_properties_add_text(p, "version", version.c_str(), OBS_TEXT_INFO);
	bool manual = false;
	if (f) {
		auto *settings = obs_source_get_settings(f->source);
		manual = !beauti::look_preset(obs_data_get_int(settings, "preset"));
		obs_data_release(settings);
	}
	show_manual_controls(p, manual);
}
static obs_properties_t *properties(void *data) noexcept
{
	auto *p = obs_properties_create();
	guarded("properties", [&] { fill_properties(p, static_cast<Filter *>(data)); });
	return p;
}
// One capture per video tick, even when render() runs twice (preview and program).
static void tick(void *data, float) noexcept
{
	static_cast<Filter *>(data)->captured_this_tick = false;
}
// Draw a texture with the default effect, optionally through the sRGB framebuffer path.
static void draw_encoded(gs_texture_t *texture, uint32_t w, uint32_t h, bool srgb)
{
	auto *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(srgb);
	if (srgb)
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"), texture);
	else
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), texture);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(texture, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
}
void Filter::clear_stages()
{
	for (auto &s : stages) {
		s.valid = false;
		s.job = Job{};
	}
}
void Filter::reset_display()
{
	continuity.reset();
	presentation = StrengthRamp{};
	active = Job{};
	display_valid = false;
	latest.reset();
	have_scene = false;
}
// The source is unusable this frame: forget all captured and processed images.
void Filter::invalidate_context()
{
	reset_display();
	context_gen = 0;
	clear_stages();
}
// Start a new analysis generation; pending jobs and results of the old one are dropped.
uint32_t Filter::restart_generation()
{
	std::lock_guard<std::mutex> lock(mutex);
	const uint32_t gen = generation.fetch_add(1) + 1;
	has_job = has_result = false;
	job = Job{};
	result = Job{};
	status = text_starting;
	return gen;
}
static const char *space_name(gs_color_space space)
{
	switch (space) {
	case GS_CS_SRGB:
		return "sRGB";
	case GS_CS_SRGB_16F:
		return "sRGB 16-bit float";
	case GS_CS_709_EXTENDED:
		return "Rec. 709 extended (HDR)";
	case GS_CS_709_SCRGB:
		return "scRGB (HDR)";
	}
	return "unknown";
}
// Publish and log source-state transitions, without touching OBS properties from
// the render thread. Opening properties or Refresh Status reads the shared state.
void Filter::note_passthrough(SourceState state, uint32_t w, uint32_t h, gs_color_space source_space,
			      gs_color_space render_space)
{
	if (state == last_source_state)
		return;
	const char *reasons[] = {nullptr,
				 "the filter has no target source",
				 "the source has no size yet",
				 "the source exceeds 4096 px on a side",
				 "the source is not sRGB (HDR or 10-bit video)",
				 "the render target is not sRGB (HDR or 10-bit canvas)"};
	const auto index = static_cast<size_t>(state);
	const char *reason = reasons[index];
	{
		std::lock_guard<std::mutex> lock(mutex);
		source_status = source_messages[index];
	}
	if (reason)
		blog(LOG_WARNING,
		     "[beauti] " BEAUTI_VERSION " passthrough: %s (source %ux%u, source space %s, render space %s)",
		     reason, w, h, space_name(source_space), space_name(render_space));
	else
		blog(LOG_INFO, "[beauti] " BEAUTI_VERSION " source accepted: %ux%u sRGB", w, h);
	last_source_state = state;
}
// Step 1: validate the target and detect source, size, or settings changes.
bool Filter::prepare_context(RenderContext &ctx)
{
	ctx.target = obs_filter_get_target(source);
	ctx.parent = obs_filter_get_parent(source);
	if (!ctx.target || !ctx.parent) {
		note_passthrough(SourceState::Missing, 0, 0, GS_CS_SRGB, GS_CS_SRGB);
		invalidate_context();
		return false;
	}
	ctx.width = obs_source_get_base_width(ctx.target);
	ctx.height = obs_source_get_base_height(ctx.target);
	const gs_color_space preferred[] = {GS_CS_SRGB};
	const gs_color_space source_space = obs_source_get_color_space(ctx.target, 1, preferred);
	const gs_color_space render_space = gs_get_color_space();
	const SourceState state = !ctx.width || !ctx.height                                   ? SourceState::Empty
				  : ctx.width > kMaxSourceSide || ctx.height > kMaxSourceSide ? SourceState::TooLarge
				  : source_space != GS_CS_SRGB                                ? SourceState::SourceColor
				  : render_space != GS_CS_SRGB                                ? SourceState::RenderColor
											      : SourceState::Ready;
	note_passthrough(state, ctx.width, ctx.height, source_space, render_space);
	if (state != SourceState::Ready) {
		invalidate_context();
		// libobs requires both parent and target for this call. A detached
		// filter above has no source to pass through.
		obs_source_skip_video_filter(source);
		return false;
	}
	const double scale = std::min(1.0, double(long_side.load()) / std::max(ctx.width, ctx.height));
	ctx.scaled_w = std::max(1u, uint32_t(ctx.width * scale));
	ctx.scaled_h = std::max(1u, uint32_t(ctx.height * scale));
	ctx.gen = generation.load();
	ctx.now = os_gettime_ns();
	if (context_gen != ctx.gen || last_target != ctx.target || context_w != ctx.width || context_h != ctx.height) {
		reset_display();
		clear_stages();
		ctx.gen = restart_generation();
		context_gen = ctx.gen;
		last_target = ctx.target;
		context_w = ctx.width;
		context_h = ctx.height;
	}
	continuity.set_cadence(1000.0 / fps.load());
	return true;
}
// Step 3: take a finished analysis from the worker if it matches this context.
// Returns true when it became the active analysis.
bool Filter::receive_result(const RenderContext &ctx)
{
	Job ready;
	{
		std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
		if (lock.owns_lock() && has_result) {
			ready = std::move(result);
			has_result = false;
		}
	}
	const auto failures = errors.load();
	if (failures != seen_errors) {
		continuity.recovery = 0;
		seen_errors = failures;
	}
	auto &r = ready.frame;
	if (!(ready.token && r.generation == ctx.gen && r.width == ctx.scaled_w && r.height == ctx.scaled_h &&
	      ready.token->width == ctx.width && ready.token->height == ctx.height))
		return false;
	if (beauti::timing_enabled())
		beauti::timing_log("result_received_age", r.sequence, double(ctx.now - r.captured_ns) / 1e6);
	if (!continuity.accept(r.sequence, r.captured_ns, ctx.now, r.milliseconds, r.status == 0))
		return false;
	active = std::move(ready);
	return true;
}
// Step 4: recompose when the analysis or a ramped strength changed.
void Filter::compose_active(const RenderContext &ctx, bool new_analysis)
{
	if (!(continuity.visible(ctx.now) && active.token && active.frame.generation == ctx.gen &&
	      active.frame.width == ctx.scaled_w && active.frame.height == ctx.scaled_h &&
	      active.token->width == ctx.width && active.token->height == ctx.height))
		return;
	const uint64_t frame_time = obs_get_video_frame_time();
	std::array<float, beauti::kFinishSettings.size()> values{};
	for (size_t i = 0; i < values.size(); ++i) {
		const float requested = float(finish_targets[i].load());
		// Initialize shape controls at their actual values; only effect strength fades in.
		if (!finish_initialized && i != size_t(beauti::FinishControl::Color))
			finish_ramps[i] = {requested, requested, requested, 0};
		values[i] = finish_ramps[i].sample(requested, frame_time);
	}
	finish_initialized = true;
	const auto finishing = beauti::finishing_controls(values);
	const Strengths s{retouch_ramp.sample(float(retouch.load()), frame_time),
			  tone_ramp.sample(float(skin_tone.load()), frame_time),
			  dodge_ramp.sample(float(dodge_burn.load()), frame_time),
			  smooth_ramp.sample(float(smooth.load()), frame_time),
			  eye_ramp.sample(active.frame.eyes ? float(eyes.load()) : 0.f, frame_time)};
	if (!(new_analysis || s.retouch != shown_retouch || s.skin_tone != shown_tone || s.dodge_burn != shown_dodge ||
	      s.smooth != shown_smooth || s.eyes != shown_eyes || finishing != shown_finishing))
		return;
	beauti::TimingSpan span("compose_cpu_submission", active.frame.sequence);
	auto *original = gs_texrender_get_texture(snapshots[active.token->slot].render.get());
	if (active.frame.status == 0 &&
	    (s.retouch > 0 || s.skin_tone > 0 || s.dodge_burn > 0 || s.smooth > 0 || s.eyes > 0 || finishing.color > 0))
		texture = gpu->compose(
			original, active.frame, active.maps, active.quality, s.retouch, s.skin_tone, s.dodge_burn,
			s.smooth, (uint64_t(active.frame.generation) << 32) | active.frame.sequence, s.eyes, finishing);
	else
		texture = original;
	display_valid = texture != nullptr;
	shown_retouch = s.retouch;
	shown_tone = s.skin_tone;
	shown_dodge = s.dodge_burn;
	shown_smooth = s.smooth;
	shown_eyes = s.eyes;
	shown_finishing = finishing;
	if (!display_valid)
		continuity.fail("resource_failure");
	if (display_valid) {
		texture_w = ctx.width;
		texture_h = ctx.height;
		last_result = active.frame.captured_ns;
		shown_generation = ctx.gen;
	}
}
// Step 2, once per tick: hand finished readbacks to the worker, then capture a new snapshot.
void Filter::service_readbacks(RenderContext &ctx)
{
	if (captured_this_tick)
		return;
	captured_this_tick = true;
	if (stage_w != ctx.scaled_w || stage_h != ctx.scaled_h) {
		clear_stages();
		for (auto &s : stages) {
			s.surface.reset(gs_stagesurface_create(ctx.scaled_w, ctx.scaled_h, GS_RGBA));
		}
		stage_w = ctx.scaled_w;
		stage_h = ctx.scaled_h;
		next_stage = 0;
		display_valid = false;
	}
	// Consume previous tick before staging new pixels. Never wait on a just-issued readback.
	for (auto &slot : stages)
		consume_readback(slot, ctx);
	capture_snapshot(ctx);
}
// Map a finished staging surface, copy its rows into a job, check for a scene cut,
// and hand the job to the worker, replacing one it has not started yet.
void Filter::consume_readback(Readback &slot, RenderContext &ctx)
{
	if (!slot.valid)
		return;
	slot.valid = false;
	if (slot.job.frame.generation != ctx.gen) {
		slot.job = Job{};
		return;
	}
	uint8_t *mapped = nullptr;
	uint32_t stride = 0;
	const bool did_map = [&] {
		beauti::TimingSpan span("readback_map", slot.job.frame.sequence);
		return gs_stagesurface_map(slot.surface.get(), &mapped, &stride);
	}();
	if (!did_map) {
		if (!readback_failing)
			blog(LOG_WARNING,
			     "[beauti] " BEAUTI_VERSION
			     " readback: the staging surface could not be mapped; no frame reaches the analysis");
		readback_failing = true;
		slot.job = Job{};
		return;
	}
	readback_failing = false;
	beauti::TimingSpan span("readback_copy_unmap", slot.job.frame.sequence);
	Job next = std::move(slot.job);
	const uint32_t w = ctx.scaled_w, h = ctx.scaled_h;
	next.frame.pixels.resize(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
		memcpy(next.frame.pixels.data() + size_t(y) * w * 4, mapped + size_t(y) * stride, size_t(w) * 4);
	gs_stagesurface_unmap(slot.surface.get());
	if (detect_scene_cut(next.frame.pixels, w, h)) {
		if (beauti::timing_enabled())
			blog(LOG_INFO, "[beauti-continuity] scene_cut");
		const auto current_scene = scene_samples;
		reset_display();
		scene_samples = current_scene;
		have_scene = true;
		ctx.gen = restart_generation();
		context_gen = ctx.gen;
		next.frame.generation = ctx.gen;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (has_job)
			++replaced;
		job = std::move(next);
		has_job = true;
	}
	event.notify_one();
}
// Conservative scene-cut signal on the actual readback. It is not face
// tracking; subtle cuts still need detector evidence. Updates the stored sample.
bool Filter::detect_scene_cut(const std::vector<uint8_t> &pixels, uint32_t w, uint32_t h)
{
	std::array<uint8_t, kSceneSamples> sample{};
	unsigned changed = 0;
	double delta = 0;
	for (unsigned y = 0; y < kSceneGrid; ++y)
		for (unsigned x = 0; x < kSceneGrid; ++x) {
			unsigned d = 0;
			for (unsigned c = 0; c < 3; ++c) {
				const unsigned i = (y * kSceneGrid + x) * 3 + c;
				sample[i] = pixels[(size_t((2 * y + 1) * h / (2 * kSceneGrid)) * w +
						    (2 * x + 1) * w / (2 * kSceneGrid)) *
							   4 +
						   c];
				const auto diff = unsigned(std::abs(int(sample[i]) - int(scene_samples[i])));
				delta += diff;
				d += diff;
			}
			if (d > kSceneBlockChange)
				++changed;
		}
	const bool cut = have_scene && delta / kSceneSamples > kSceneMeanChange && changed > kSceneChangedBlocks;
	scene_samples = sample;
	have_scene = true;
	return cut;
}
// Render the target into a full-resolution snapshot, scale it down, and start the
// asynchronous readback that feeds the worker. Rate-limited to the requested cadence.
void Filter::capture_snapshot(const RenderContext &ctx)
{
	const uint64_t interval = 1000000000ULL / fps.load(), video_clock = obs_get_video_frame_time();
	if (!capture_pacer.due(video_clock, interval))
		return;
	Snapshot *snapshot = nullptr;
	uint32_t index = 0;
	// A slot is free when only the snapshot itself still holds the token.
	for (uint32_t i = 0; i < kSnapshotSlots; ++i)
		if (!snapshots[i].token || snapshots[i].token.use_count() == 1) {
			snapshot = &snapshots[i];
			index = i;
			break;
		}
	auto &slot = stages[next_stage];
	if (!(snapshot && slot.surface))
		return;
	capture_pacer.taken(video_clock, interval);
	next_stage = (next_stage + 1) % kReadbackSlots;
	if (!snapshot->render)
		snapshot->render.reset(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	bool captured = false;
	if (snapshot->render) {
		gs_texrender_reset(snapshot->render.get());
		if (gs_texrender_begin_with_color_space(snapshot->render.get(), ctx.width, ctx.height, GS_CS_SRGB)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
			gs_ortho(0, float(ctx.width), 0, float(ctx.height), -100, 100);
			// Sources that neither draw themselves nor are async go through the
			// default effect path; the others render through their own video_render.
			uint32_t flags = obs_source_get_output_flags(ctx.target);
			if (ctx.target == ctx.parent && !(flags & OBS_SOURCE_CUSTOM_DRAW) &&
			    !(flags & OBS_SOURCE_ASYNC))
				obs_source_default_render(ctx.target);
			else
				obs_source_video_render(ctx.target);
			gs_texrender_end(snapshot->render.get());
			gs_texrender_reset(capture.get());
			if (gs_texrender_begin_with_color_space(capture.get(), ctx.scaled_w, ctx.scaled_h,
								GS_CS_SRGB)) {
				gs_ortho(0, float(ctx.scaled_w), 0, float(ctx.scaled_h), -100, 100);
				draw_encoded(gs_texrender_get_texture(snapshot->render.get()), ctx.scaled_w,
					     ctx.scaled_h, false);
				gs_texrender_end(capture.get());
				gs_stage_texture(slot.surface.get(), gs_texrender_get_texture(capture.get()));
				snapshot->token = std::make_shared<CaptureToken>();
				snapshot->token->slot = index;
				snapshot->token->width = ctx.width;
				snapshot->token->height = ctx.height;
				latest = snapshot->token;
				slot.job = Job{};
				slot.job.token = snapshot->token;
				slot.job.quality = quality.load();
				auto &m = slot.job.frame;
				m.width = ctx.scaled_w;
				m.height = ctx.scaled_h;
				m.generation = ctx.gen;
				m.sequence = ++sequence;
				// Keep both maps warm, including at 0/0/0/0: sliders only change GPU strengths.
				m.retouch = 100;
				m.smooth = 100;
				m.eyes = (eyes.load() || eye_ramp.value > 0) ? 100 : 0;
				m.backend = backend.load();
				m.captured_ns = ctx.now;
				slot.valid = true;
				captured = true;
			}
		}
	}
	gs_blend_state_pop();
	if (!captured && !capture_failing)
		blog(LOG_WARNING,
		     "[beauti] " BEAUTI_VERSION
		     " capture: the source could not be rendered into the analysis texture (%ux%u); no frame reaches the analysis",
		     ctx.width, ctx.height);
	capture_failing = !captured;
}
// Step 5: draw the composed frame, cross-fade in and out, or pass the source through.
void Filter::present(const RenderContext &ctx)
{
	const bool enabled = retouch.load() || skin_tone.load() || dodge_burn.load() || smooth.load() || eyes.load() ||
			     finish_targets[size_t(beauti::FinishControl::Color)].load();
	// Finish the existing strength ramp before explicit all-zero live bypass.
	const bool ramping = retouch_ramp.value > 0 || tone_ramp.value > 0 || dodge_ramp.value > 0 ||
			     smooth_ramp.value > 0 || eye_ramp.value > 0 ||
			     finish_ramps[size_t(beauti::FinishControl::Color)].value > 0;
	const bool show = continuity.visible(ctx.now) && (enabled || ramping);
	const float amount = presentation.sample(show ? 1.f : 0.f, ctx.now);
	bool diagnostics = beauti::timing_enabled();
#ifdef BEAUTI_CONTINUITY_TEST
	diagnostics = true;
#endif
	if (diagnostics && last_continuity_reason != continuity.reason) {
		blog(LOG_INFO, "[beauti-continuity] reason=%s frame=%u", continuity.reason, sequence);
		last_continuity_reason = continuity.reason;
	}
	if (!display_valid || shown_generation != ctx.gen || texture_w != ctx.width || texture_h != ctx.height ||
	    (!show && (!enabled || amount <= 0))) {
		if (beauti::timing_enabled())
			beauti::timing_log("passthrough", sequence,
					   last_result ? double(ctx.now - last_result) / 1e6 : 0.0);
		obs_source_skip_video_filter(source);
		return;
	}
	if (amount < 1 && latest) {
		auto *e = continuity_effect.get();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"),
					   gs_texrender_get_texture(snapshots[latest->slot].render.get()));
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "previous"), texture);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "amount"), amount);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(texture, 0, ctx.width, ctx.height);
		gs_enable_framebuffer_srgb(previous);
	} else
		draw_encoded(texture, ctx.width, ctx.height, true);
}
void Filter::render()
{
	RenderContext ctx;
	if (!prepare_context(ctx))
		return;
	// Readbacks first: the worker gets its next frame earlier, and a result published
	// meanwhile is received in this tick instead of being overwritten by the next one
	// (one pending result slot).
	service_readbacks(ctx);
	const bool new_analysis = receive_result(ctx);
	compose_active(ctx, new_analysis);
	present(ctx);
}
static void render(void *data, gs_effect_t *) noexcept
{
	auto *f = static_cast<Filter *>(data);
	bool rendered = false;
	guarded("render", [&] {
		f->render();
		rendered = true;
	});
	if (!rendered)
		obs_source_skip_video_filter(f->source);
}
// A direct menu action also makes setup possible without the Qt filter popup.
static bool selected_source(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	if (!obs_sceneitem_selected(item))
		return true;
	*static_cast<obs_source_t **>(data) = obs_source_get_ref(obs_sceneitem_get_source(item));
	return false;
}
static void find_existing(obs_source_t *, obs_source_t *filter, void *data)
{
	auto **found = static_cast<obs_source_t **>(data);
	if (!*found && strcmp(obs_source_get_id(filter), "beauti_filter") == 0)
		*found = obs_source_get_ref(filter);
}
static void add_filter_to_selection()
{
	obs_source_t *scene_source = obs_frontend_preview_program_mode_active()
					     ? obs_frontend_get_current_preview_scene()
					     : obs_frontend_get_current_scene();
	obs_source_t *source = nullptr;
	auto *scene = obs_scene_from_source(scene_source);
	if (scene)
		obs_scene_enum_items(scene, selected_source, &source);
	obs_source_release(scene_source);
	if (!source) {
		beauti::show_notice(obs_frontend_get_main_window_handle(), "Beauti",
				    obs_module_text("SelectSourceFirst"));
		return;
	}
	obs_source_t *existing = nullptr;
	obs_source_enum_filters(source, find_existing, &existing);
	if (existing)
		obs_source_release(existing);
	else {
		auto *filter = obs_source_create_private("beauti_filter", "Beauti", nullptr);
		if (filter) {
			obs_source_filter_add(source, filter);
			obs_source_release(filter);
		}
	}
	obs_frontend_open_source_filters(source);
	obs_source_release(source);
}
static void add_to_selection(void *) noexcept
{
	guarded("add_to_selection", add_filter_to_selection);
}
MODULE_EXPORT bool obs_module_load(void)
{
	static obs_source_info info = {};
	info.id = "beauti_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = filter_name;
	info.create = create;
	info.destroy = destroy;
	info.get_defaults = defaults;
	info.update = update;
	info.get_properties = properties;
	info.video_render = render;
	info.video_tick = tick;
	obs_register_source(&info);
	obs_frontend_add_tools_menu_item(obs_module_text("AddFilter"), add_to_selection, nullptr);
	blog(LOG_INFO, "[beauti] " BEAUTI_VERSION " loaded; full-resolution GPU composite, frame-aligned maps");
	return true;
}
