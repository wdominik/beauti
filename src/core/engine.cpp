// SPDX-License-Identifier: GPL-3.0-or-later
// Native inference: BlazeFace face detection, the retouch blend map, the MediaPipe skin
// segmentation with FaceMesh feature exclusion, and the optional eye landmarks. Retouch
// and segmentation run through Core AI on macOS and ONNX Runtime elsewhere; detection
// and the landmarks use ONNX Runtime on the CPU. Compositing happens on the GPU (src/plugin/gpu.cpp).
#include "engine.h"
#include "eyes.h"
#include "face_tracker.h"
#include "backend.h"
#include "paths.h"
#include "timing.h"
#include "pixel_quantize.h"
#include "onnxruntime_c_api.h"
#ifdef __APPLE__
#include "coreai_model.h"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <numeric>
namespace beauti {
namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMaxImageSide = 4096;
// Retouch input sides for the Performance and High Quality modes (see backend.cpp).
constexpr int kRetouchSideFast = 384, kRetouchSideFull = 512;
// MediaPipe BlazeFace full range: the frame letterboxed into 192 px, one anchor per
// cell of a 48x48 grid, and MediaPipe's score threshold for this model.
constexpr int kDetectorSide = 192, kDetectorGrid = 48;
constexpr float kDetectorScoreThreshold = .6f, kDetectorIouThreshold = .3f;
constexpr size_t kDetectorMaxCandidates = 100;
// The detector misses a face that nearly fills the image. Searching again with the image at
// this share of the input brings such a face into the range the model was trained for.
constexpr float kDetectorSearchShrink = .7f;
// Square analysis crop around the detected box. BlazeFace boxes are tight; 1.86x gives the
// crop size the retouch output was validated with (docs/decisions/0021).
constexpr float kFaceBoxScale = 1.86f;
constexpr int kMinFaceSide = FaceTracker::min_face_side;
// MediaPipe Multiclass segmentation: 256 px input, six classes; face skin is class 3.
constexpr int kSegmenterSide = 256, kSegmenterClasses = 6, kSegmenterFaceSkin = 3;
constexpr const char *kDetectorModel = "native/face_detector.onnx", *kMeshModel = "native/face_landmarks.onnx",
		     *kIrisModel = "native/iris_landmarks.onnx";
// The FaceMesh eye, brow, and lip contours are grown by this many mask pixels.
constexpr float kFeatureMargin = 1.5f;
// Zero delegates CPU-only sizing to ORT; auxiliary CPU work keeps its bounded pool
// (auxiliary_cpu_threads).
constexpr int kAutomaticCpuThreads = 0;
float elapsed(Clock::time_point t)
{
	return std::chrono::duration<float, std::milli>(Clock::now() - t).count();
}
const OrtApi *api()
{
	static const OrtApi *a = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	if (!a)
		throw std::runtime_error("Incompatible ONNX Runtime API");
	return a;
}
void check(OrtStatus *s)
{
	if (s) {
		std::string message = api()->GetErrorMessage(s);
		api()->ReleaseStatus(s);
		throw std::runtime_error(message);
	}
}
// Owns the OrtValue tensors of one Run() call.
struct Values {
	std::vector<OrtValue *> v;
	explicit Values(size_t n) : v(n, nullptr) {}
	~Values()
	{
		for (auto p : v)
			if (p)
				api()->ReleaseValue(p);
	}
	Values(const Values &) = delete;
	Values &operator=(const Values &) = delete;
	Values(Values &&other) noexcept : v(std::move(other.v)) { other.v.clear(); }
	float *data(size_t i)
	{
		void *p = nullptr;
		check(api()->GetTensorMutableData(v.at(i), &p));
		return static_cast<float *>(p);
	}
};
// One ONNX Runtime session with its CPU memory descriptor and optional profiling.
struct Session {
	OrtSession *p = nullptr;
	OrtMemoryInfo *memory = nullptr;
	bool profiling = false;
	Session(OrtEnv *env, const std::filesystem::path &file, int backend, const std::string &cache, int cpu_threads)
	{
		OrtSessionOptions *opts = nullptr;
		check(api()->CreateSessionOptions(&opts));
		try {
			// Device sessions retain one host thread; only CPU sessions use the engine's CPU budget.
			check(api()->SetIntraOpNumThreads(opts, backend == CPU ? cpu_threads : 1));
			check(api()->SetInterOpNumThreads(opts, 1));
			check(api()->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
			check(api()->SetSessionLogSeverityLevel(opts, 3));
#ifdef _WIN32
			const auto *profile_dir = _wgetenv(L"BEAUTI_ORT_PROFILE_DIR");
#else
			const auto *profile_dir = std::getenv("BEAUTI_ORT_PROFILE_DIR");
#endif
			if (profile_dir && *profile_dir) {
				const std::filesystem::path directory(profile_dir);
				std::filesystem::create_directories(directory);
				const auto prefix = directory / file.stem();
				check(api()->EnableProfiling(opts, prefix.c_str()));
				profiling = true;
			}
			configure_provider(api(), env, opts, uint32_t(backend), cache);
			check(api()->CreateSession(env, file.c_str(), opts, &p));
			check(api()->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
		} catch (...) {
			api()->ReleaseSessionOptions(opts);
			if (p)
				api()->ReleaseSession(p);
			throw;
		}
		api()->ReleaseSessionOptions(opts);
	}
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;
	~Session()
	{
		if (p && profiling) {
			OrtAllocator *allocator = nullptr;
			char *filename = nullptr;
			auto *status = api()->GetAllocatorWithDefaultOptions(&allocator);
			if (!status)
				status = api()->SessionEndProfiling(p, allocator, &filename);
			if (status) {
				std::fprintf(stderr, "Beauti profiling: %s\n", api()->GetErrorMessage(status));
				api()->ReleaseStatus(status);
			}
			if (filename)
				allocator->Free(allocator, filename);
		}
		if (p)
			api()->ReleaseSession(p);
		if (memory)
			api()->ReleaseMemoryInfo(memory);
	}
	Values run(std::vector<float> &input, int h, int w, const char *name, const std::vector<const char *> &outputs)
	{
		int64_t dims[] = {1, 3, h, w};
		Values value(1);
		check(api()->CreateTensorWithDataAsOrtValue(memory, input.data(), input.size() * sizeof(float), dims, 4,
							    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &value.v[0]));
		Values result(outputs.size());
		const OrtValue *in = value.v[0];
		check(api()->Run(p, nullptr, &name, &in, 1, outputs.data(), outputs.size(), result.v.data()));
		return result;
	}
};
// The retouch or mask stage: ONNX Runtime on the CPU and NVIDIA, Core AI on macOS.
// run() returns the NCHW float32 output, valid until the next call.
class StageModel {
public:
	virtual ~StageModel() = default;
	virtual const float *run(std::vector<float> &input, int side) = 0;
};
class OrtStage final : public StageModel {
	Session session;
	const char *input_name, *output_name;
	Values last{0};

public:
	OrtStage(OrtEnv *env, const std::filesystem::path &file, uint32_t backend, const std::string &cache,
		 int cpu_threads, const char *input, const char *output)
		: session(env, file, int(backend), cache, cpu_threads), input_name(input), output_name(output)
	{
	}
	const float *run(std::vector<float> &input, int side) override
	{
		auto result = session.run(input, side, side, input_name, {output_name});
		last.v.swap(result.v);
		return last.data(0);
	}
};
#ifdef __APPLE__
class CoreAIStage final : public StageModel {
	CoreAIModel model;

public:
	CoreAIStage(const std::filesystem::path &asset, const CoreAIModelSpec &spec) : model(asset, spec) {}
	const float *run(std::vector<float> &input, int) override { return model.run(input); }
};
#endif
// Bilinear resampling coordinates: source indices a, b and the fraction f per output index.
struct Axis {
	int a, b;
	float f;
};
// Half-pixel-centered mapping (OpenCV INTER_LINEAR convention) with clamped edges.
std::vector<Axis> axis(int from, int to)
{
	std::vector<Axis> a(to);
	for (int x = 0; x < to; ++x) {
		float p = (float(x) + .5f) * float(from) / float(to) - .5f;
		int i = int(std::floor(p));
		a[x] = {std::clamp(i, 0, from - 1), std::clamp(i + 1, 0, from - 1), p - float(i)};
	}
	return a;
}
// One input tensor per model: resize, straight alpha, normalization, and CHW planar
// RGB in a single pass over the RGBA readback. Every model (face detector, retouch,
// skin segmenter) takes RGB in [-1, 1]; a per-byte table holds the mapping.
struct InputBuffer {
	std::vector<float> values;
	std::vector<Axis> columns, rows;
	std::array<float, 256> normalized{};
	int source_w = 0, source_h = 0, width = 0, height = 0, padded_w = 0, padded_h = 0;
	InputBuffer()
	{
		for (int v = 0; v < 256; ++v)
			normalized[size_t(v)] = float(v) / 127.5f - 1;
	}
	// Un-premultiplies one channel; a fully transparent pixel reads as black.
	static float straight(const uint8_t *p, int c)
	{
		return p[3] == 255 ? p[c] : p[3] ? std::min(255.f, float(std::nearbyint(p[c] * 255.f / p[3]))) : 0;
	}
	void prepare(const Frame &f, int l, int t, int w, int h, int ow, int oh, int pw, int ph)
	{
		if (w != source_w || ow != width) {
			columns = axis(w, ow);
			source_w = w;
		}
		if (h != source_h || oh != height) {
			rows = axis(h, oh);
			source_h = h;
		}
		// Padding (the detector's letterbox) must be cleared when dimensions change.
		if (ow != width || oh != height || pw != padded_w || ph != padded_h) {
			values.assign(size_t(pw) * ph * 3, 0.f);
			width = ow;
			height = oh;
			padded_w = pw;
			padded_h = ph;
		}
		float *planes[] = {values.data(), values.data() + size_t(pw) * ph, values.data() + size_t(pw) * ph * 2};
		for (int y = 0; y < oh; ++y) {
			const auto Y = rows[y];
			const auto *top = f.pixels.data() + (size_t(t + Y.a) * f.width + l) * 4;
			const auto *bottom = f.pixels.data() + (size_t(t + Y.b) * f.width + l) * 4;
			for (int x = 0; x < ow; ++x) {
				const auto X = columns[x];
				const auto *a = top + X.a * 4, *b = top + X.b * 4, *d = bottom + X.a * 4,
					   *e = bottom + X.b * 4;
				const bool opaque = (a[3] & b[3] & d[3] & e[3]) == 255;
#if defined(_M_X64)
				if (opaque) {
					const auto interpolated = interpolate_opaque_bytes(a, b, d, e, X.f, Y.f);
					for (int c = 0; c < 3; ++c)
						planes[c][size_t(y) * pw + x] = normalized[interpolated[c]];
					continue;
				}
#endif
				for (int c = 0; c < 3; ++c) {
					const int channel = c;
					float A = opaque ? a[channel] : straight(a, channel),
					      B = opaque ? b[channel] : straight(b, channel);
					float D = opaque ? d[channel] : straight(d, channel),
					      E = opaque ? e[channel] : straight(e, channel);
					// Preserve the original RGB8 rounding before normalization.
					auto value = quantize_input_byte((A + (B - A) * X.f) * (1 - Y.f) +
									 (D + (E - D) * X.f) * Y.f);
					planes[c][size_t(y) * pw + x] = normalized[value];
				}
			}
		}
	}
};
struct Box {
	float x, y, w, h, score;
	float eye_angle = 0;
};
// Intersection over union on integer-truncated boxes.
float iou(const Box &a, const Box &b)
{
	int ax = int(a.x), ay = int(a.y), aw = int(a.w), ah = int(a.h), bx = int(b.x), by = int(b.y), bw = int(b.w),
	    bh = int(b.h);
	float intersection = float(std::max(0, std::min(ax + aw, bx + bw) - std::max(ax, bx)) *
				   std::max(0, std::min(ay + ah, by + bh) - std::max(ay, by)));
	return intersection / std::max(1.f, float(aw * ah + bw * bh) - intersection);
}
}
struct Engine::Impl {
	ProviderEnvironment environment;
	std::unique_ptr<Session> detector, face_landmarks, iris_landmarks;
	std::unique_ptr<StageModel> retouch, skin;
	std::filesystem::path model_root;
	ModelPlan placement;
	std::string cache_root;
	// Each accelerator session keeps its own provider cache below the engine's cache.
	std::string model_cache(const std::string &model, const char *tag) const
	{
		return cache_root + "/" + model + "/" + tag;
	}
	std::vector<float> eye_input;
	bool eyes_unavailable = false; // Set after the iris model failed; cleared with the engine.
	FaceTracker tracker;
	// FaceMesh landmarks of the last accepted analysis; the skin mask and the eye correction
	// share them. A bridged analysis keeps them, so a failed model run only fills the candidates.
	std::vector<EyePoint> mesh_points, mesh_candidate;
	uint32_t backend;
	int nr, ns = kSegmenterSide;
	InputBuffer detector_input, retouch_input, skin_input;
	Impl(const std::string &models, const std::string &cache, uint32_t b, uint32_t quality)
		: environment(api(), b), backend(b), nr(quality ? kRetouchSideFull : kRetouchSideFast)
	{
		OrtEnv *env = environment.get();
		try {
			auto root = utf8_path(models);
			model_root = root;
			const auto plan = model_plan(b, quality);
			placement = plan;
			cache_root = cache;
			// ORT sizes the pool itself only in CPU mode; accelerator pipelines keep the
			// bounded auxiliary budget (docs/decisions/0016, 0022).
			const int cpu_threads = b == CPU ? kAutomaticCpuThreads : auxiliary_cpu_threads();
			detector = std::make_unique<Session>(env, root / kDetectorModel, plan.detector_backend,
							     model_cache(kDetectorModel, "detector"), cpu_threads);
			// FaceMesh is required: the skin mask cuts eyes, brows, and lips out along its contours.
			// Landmarks overlap the retouch and mask models, so they keep the auxiliary
			// budget even in CPU mode.
			face_landmarks =
				std::make_unique<Session>(env, root / kMeshModel, plan.landmarks_backend,
							  model_cache(kMeshModel, "mesh"), auxiliary_cpu_threads());
			if (plan.coreai)
				create_coreai_stages(root, plan);
			else {
				retouch = std::make_unique<OrtStage>(env, root / plan.retouch, plan.retouch_backend,
								     model_cache(plan.retouch, "retouch"), cpu_threads,
								     "image", "blend");
				skin = std::make_unique<OrtStage>(env, root / plan.skin, plan.skin_backend,
								  model_cache(plan.skin, "skin"), cpu_threads, "image",
								  "classes");
			}
		} catch (...) {
			skin.reset();
			retouch.reset();
			face_landmarks.reset();
			detector.reset();
			throw;
		}
	}
	// The retouch asset has float16 tensors; the segmenter keeps float32 interfaces and
	// writes its six class probabilities channels-last.
	void create_coreai_stages(const std::filesystem::path &root, const ModelPlan &plan)
	{
#ifdef __APPLE__
		retouch = std::make_unique<CoreAIStage>(
			root / plan.retouch,
			CoreAIModelSpec{"image", "blend", plan.retouch_size, 3, true, ComputeUnit::NeuralEngine});
		skin = std::make_unique<CoreAIStage>(root / plan.skin,
						     CoreAIModelSpec{"image", "classes", plan.skin_size,
								     kSegmenterClasses, false, ComputeUnit::GPU});
#else
		(void)root;
		(void)plan;
		throw std::runtime_error("Core AI is available on macOS only");
#endif
	}
	Impl(const Impl &) = delete;
	Impl &operator=(const Impl &) = delete;
	~Impl()
	{
		iris_landmarks.reset();
		face_landmarks.reset();
		skin.reset();
		retouch.reset();
		detector.reset();
	}
	// A face the detector found: its analysis box and the crop for the landmark model.
	struct DetectedFace {
		FaceBox box;
		EyeCrop crop;
	};
	// Greedy NMS on boxes in detector coordinates (frame = box / scale), then the largest
	// face of at least kMinFaceSide: a kFaceBoxScale square around the box.
	bool choose_face(const Frame &f, std::vector<Box> boxes, float scale, DetectedFace &face)
	{
		std::stable_sort(boxes.begin(), boxes.end(),
				 [](const Box &a, const Box &b) { return a.score > b.score; });
		if (boxes.size() > kDetectorMaxCandidates)
			boxes.resize(kDetectorMaxCandidates);
		std::vector<Box> keep;
		for (const auto &b : boxes) {
			bool suppressed = false;
			for (const auto &k : keep)
				if (iou(b, k) > kDetectorIouThreshold) {
					suppressed = true;
					break;
				}
			if (!suppressed)
				keep.push_back(b);
		}
		std::sort(keep.begin(), keep.end(), [](const Box &a, const Box &b) { return a.w * a.h > b.w * b.h; });
		for (auto b : keep) {
			float x = b.x / scale, y = b.y / scale, fw = b.w / scale, fh = b.h / scale,
			      s = std::max(fw, fh) * kFaceBoxScale;
			int l = std::max(0, int(x + fw / 2 - s / 2)), t = std::max(0, int(y + fh / 2 - s / 2)),
			    r = std::min(int(f.width), int(x + fw / 2 + s / 2)),
			    d = std::min(int(f.height), int(y + fh / 2 + s / 2));
			if (r - l >= kMinFaceSide && d - t >= kMinFaceSide) {
				face = {{l, t, r, d}, {{x + fw / 2, y + fh / 2}, s, b.eye_angle}};
				return true;
			}
		}
		return false;
	}
	// BlazeFace: the frame letterboxed into 192 px, at the given share of that size; per
	// anchor, box and eye points are offsets in input pixels.
	bool detect(const Frame &f, float shrink, DetectedFace &face)
	{
		const float scale = float(kDetectorSide) * shrink / float(std::max(f.width, f.height));
		const int w = std::max(1, int(std::nearbyint(float(f.width) * scale))),
			  h = std::max(1, int(std::nearbyint(float(f.height) * scale)));
		{
			TimingSpan span("detector_prepare", f.sequence);
			detector_input.prepare(f, 0, 0, int(f.width), int(f.height), w, h, kDetectorSide,
					       kDetectorSide);
		}
		auto outputs = [&] {
			TimingSpan span("detector_run", f.sequence);
			return detector->run(detector_input.values, kDetectorSide, kDetectorSide, "image",
					     {"boxes", "scores"});
		}();
		const float *regressors = outputs.data(0), *logits = outputs.data(1);
		std::vector<Box> boxes;
		for (int i = 0; i < kDetectorGrid * kDetectorGrid; ++i) {
			const float score = 1.f / (1.f + std::exp(-std::clamp(logits[i], -100.f, 100.f)));
			if (score < kDetectorScoreThreshold)
				continue;
			const float *r = regressors + 16 * i, cell = float(kDetectorSide) / float(kDetectorGrid);
			const float ax = (float(i % kDetectorGrid) + .5f) * cell,
				    ay = (float(i / kDetectorGrid) + .5f) * cell;
			// Keypoints 0 and 1 are the eyes, from the observer's left to right.
			boxes.push_back({r[0] + ax - r[2] / 2, r[1] + ay - r[3] / 2, r[2], r[3], score,
					 std::atan2(r[7] - r[5], r[6] - r[4])});
		}
		return choose_face(f, boxes, scale, face);
	}
	// Landmark model geometry: FaceMesh V2 takes a 256 px face crop and returns 478
	// points; the iris model takes a 64 px eye crop 2.3 eye widths wide. Points
	// outside one crop width around the crop are treated as a failed prediction.
	static constexpr int kMeshSide = 256, kIrisSide = 64, kMeshPoints = 478;
	static constexpr float kIrisCropWidths = 2.3f;
	// Face-presence logit for sigmoid 0.8: log(0.8 / 0.2).
	static constexpr float kMinFaceLogit = 1.3862944f;
	// Eye corners in the FaceMesh topology: outer and inner corner of each eye.
	static constexpr int kEyeCorners[2][2] = {{33, 133}, {362, 263}};
	// FaceMesh on a rotated face crop, into mesh_candidate; false when the face score or any
	// landmark is implausible.
	bool run_mesh(const Frame &f, const EyeCrop &crop)
	{
		TimingSpan span("face_mesh", f.sequence);
		if (!std::isfinite(crop.angle))
			return false;
		sample_eye_crop(f, crop, kMeshSide, false, eye_input);
		auto mesh = face_landmarks->run(eye_input, kMeshSide, kMeshSide, "input", {"landmarks", "score"});
		const float score = mesh.data(1)[0];
		if (!std::isfinite(score) || score < kMinFaceLogit)
			return false;
		mesh_candidate.resize(kMeshPoints);
		const float *landmarks = mesh.data(0);
		constexpr float lo = -float(kMeshSide), hi = 2.f * float(kMeshSide);
		for (size_t i = 0; i < mesh_candidate.size(); ++i) {
			float x = landmarks[3 * i], y = landmarks[3 * i + 1];
			if (!std::isfinite(x) || !std::isfinite(y) || x < lo || x > hi || y < lo || y > hi)
				return false;
			mesh_candidate[i] = crop.project(x, y, kMeshSide);
		}
		return true;
	}
	// Accepts mesh_candidate as the landmarks of this analysis.
	bool accept_mesh(uint64_t now)
	{
		if (!tracker.update(now, mesh_candidate, mesh_candidate[size_t(kEyeCorners[0][0])],
				    mesh_candidate[size_t(kEyeCorners[1][1])]))
			return false;
		mesh_points.swap(mesh_candidate);
		return true;
	}
	// A periodic detector pass: move to a face that stays clearly larger than the tracked one.
	void check_for_larger_face(const Frame &f, uint64_t now)
	{
		DetectedFace face;
		const bool found = detect(f, 1, face);
		if (tracker.larger_face_seen(now, found ? &face.box : nullptr) && run_mesh(f, face.crop))
			accept_mesh(now);
	}
	// Finds the face region of this analysis. Fresh: landmarks of this frame. Bridged: the last
	// region and landmarks, on this frame. None: no usable face.
	enum class FaceFound { Fresh, Bridged, None };
	FaceFound locate_face(Frame &f, uint64_t now)
	{
		tracker.begin(now, f.width, f.height, f.generation);
		bool found = tracker.tracking() && run_mesh(f, tracker.region()) && accept_mesh(now);
		if (found && tracker.detector_due(now))
			check_for_larger_face(f, now);
		if (!found) {
			DetectedFace face;
			found = (detect(f, 1, face) || detect(f, kDetectorSearchShrink, face)) &&
				run_mesh(f, face.crop) && accept_mesh(now);
		}
		const bool bridged = !found && tracker.may_bridge(now);
		if (!found && !bridged) {
			tracker.reset();
			return FaceFound::None;
		}
		const auto &box = tracker.box();
		f.box[0] = box.left;
		f.box[1] = box.top;
		f.box[2] = box.right;
		f.box[3] = box.bottom;
		return found ? FaceFound::Fresh : FaceFound::Bridged;
	}
	// The Iris model loads lazily, on the landmark device with the auxiliary CPU budget.
	void analyze_eyes(const Frame &f, Maps &maps)
	{
		TimingSpan span("eyes_run", f.sequence);
		if (!iris_landmarks)
			iris_landmarks = std::make_unique<Session>(
				environment.get(), model_root / kIrisModel, placement.landmarks_backend,
				model_cache(kIrisModel, "iris"), auxiliary_cpu_threads());
		const auto &points = mesh_points;
		for (int eye = 0; eye < 2; ++eye) {
			EyeContour lid;
			for (size_t i = 0; i < lid.size(); ++i)
				lid[i] = points[size_t(kEyeContours[eye][i])];
			if (eye_openness(lid) <= 0)
				continue;
			const auto a = points[size_t(kEyeCorners[eye][0])], b = points[size_t(kEyeCorners[eye][1])];
			EyeCrop crop{{(a.x + b.x) * .5f, (a.y + b.y) * .5f},
				     std::hypot(b.x - a.x, b.y - a.y) * kIrisCropWidths,
				     std::atan2(b.y - a.y, b.x - a.x)};
			// The second eye is mirrored so the model always sees a left eye.
			sample_eye_crop(f, crop, kIrisSide, eye == 1, eye_input);
			auto refined = iris_landmarks->run(eye_input, kIrisSide, kIrisSide, "input_1", {"output_iris"});
			IrisContour iris;
			for (size_t i = 0; i < iris.size(); ++i) {
				float x = refined.data(0)[3 * i], y = refined.data(0)[3 * i + 1];
				iris[i] = crop.project(eye ? float(kIrisSide) - x : x, y, kIrisSide);
			}
			raster_eye(f, lid, iris, maps);
		}
	}
	// Raw mask from the segmenter's probabilities (NHWC): R = face-skin label, G = its
	// probability; FaceMesh features are cleared, so no eye, brow, or lip pixel counts as skin.
	void pack_mask(const Frame &f, const float *probabilities, Maps &maps)
	{
		const size_t count = size_t(ns) * size_t(ns);
		maps.raw_mask = true;
		maps.mask_size = uint32_t(ns);
		maps.mask.assign(count * 4, 0.f);
		TimingSpan span("mask_pack", f.sequence);
		for (size_t i = 0; i < count; ++i) {
			const float *p = probabilities + i * kSegmenterClasses;
			const auto label = std::max_element(p, p + kSegmenterClasses) - p;
			maps.mask[4 * i] = label == kSegmenterFaceSkin ? 1.f : 0.f;
			maps.mask[4 * i + 1] = p[kSegmenterFaceSkin];
			maps.mask[4 * i + 3] = 1;
		}
		exclude_features(f, mesh_points, uint32_t(ns), kFeatureMargin, maps.mask);
	}
	// Eyes are optional: a missing or failing Iris model disables only this stage
	// for the engine's lifetime instead of discarding the retouch and skin maps.
	void try_eyes(Frame &f, Maps &maps)
	{
		try {
			analyze_eyes(f, maps);
		} catch (const std::exception &e) {
			eyes_unavailable = true;
			iris_landmarks.reset();
			maps.eye_size = 0;
			maps.eyes.clear();
			f.warning = std::string("Eyes disabled: ") + e.what();
		}
	}
	// Locate the face, prepare both model inputs from its region, run the skin mask on a
	// second thread while the retouch graph runs, validate the outputs, and pack
	// them as RGBA float maps for the compositor.
	void analyze(Frame &f, Maps &maps)
	{
		TimingSpan total_span("engine", f.sequence);
		maps = Maps{};
		f.warning.clear();
		auto start = Clock::now();
		if (f.width == 0 || f.height == 0 || f.width > kMaxImageSide || f.height > kMaxImageSide ||
		    f.pixels.size() != size_t(f.width) * f.height * 4)
			throw std::runtime_error("Invalid image dimensions");
		f.used_backend = backend;
		f.status = 0;
		if (!f.retouch && !f.smooth && !f.eyes) {
			f.milliseconds = elapsed(start);
			return;
		}
		// The bench and tests pass no capture time; the tracker then uses the wall clock.
		const uint64_t now = f.captured_ns ? f.captured_ns
						   : uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
								      start.time_since_epoch())
								      .count());
		const FaceFound face = locate_face(f, now);
		f.landmarks_valid = face == FaceFound::Fresh;
		if (face == FaceFound::None) {
			f.status = 1;
			f.milliseconds = elapsed(start);
			return;
		}
		f.detect_ms = elapsed(start);
		int l = f.box[0], t = f.box[1], w = f.box[2] - l, h = f.box[3] - t;
		const int NR = nr * nr, NS = ns * ns;
		{
			TimingSpan span("model_prepare", f.sequence);
			if (f.retouch)
				retouch_input.prepare(f, l, t, w, h, nr, nr, nr, nr);
			if (f.smooth)
				skin_input.prepare(f, l, t, w, h, ns, ns, ns, ns);
		}
		auto inference = Clock::now();
		const float *B = nullptr;
		std::future<const float *> future;
		const auto dispatched = timing_enabled() ? Clock::now() : Clock::time_point{};
		// Eye maps need landmarks of this frame. With the skin mask active, the eye models
		// run next to the segmenter, which waits on the GPU or its own pool.
		const bool eyes_wanted = f.eyes && !eyes_unavailable && face == FaceFound::Fresh;
		const bool eyes_with_mask = f.smooth && eyes_wanted;
		if (f.smooth) {
			// The whole mask (segmenter, feature cut-out) runs on this second thread while
			// the retouch model occupies the Neural Engine or the device. Only maps.mask is
			// written here, and maps.eyes on the eye thread.
			TimingSpan span("skin_dispatch", f.sequence);
			future = std::async(std::launch::async, [&, dispatched] {
				if (timing_enabled())
					timing_log("skin_start_delay", f.sequence, elapsed(dispatched));
				TimingSpan span("skin_run", f.sequence);
				std::future<void> eye_models;
				if (eyes_with_mask)
					eye_models = std::async(std::launch::async, [&] { try_eyes(f, maps); });
				const float *classes = [&] {
					TimingSpan run_span("segmenter_run", f.sequence);
					return skin->run(skin_input.values, ns);
				}();
				if (eye_models.valid())
					eye_models.get();
				for (int i = 0; i < kSegmenterClasses * NS; ++i)
					if (!std::isfinite(classes[i]))
						throw std::runtime_error("Invalid skin mask");
				pack_mask(f, classes, maps);
				return classes;
			});
		}
		// The future joins before its inputs die, also when retouch throws.
		if (f.retouch) {
			TimingSpan span("retouch_run", f.sequence);
			B = retouch->run(retouch_input.values, nr);
		}
		if (f.smooth) {
			TimingSpan span("skin_wait", f.sequence);
			static_cast<void>(future.get()); // rethrows a segmentation failure
		}
		if (eyes_wanted && !eyes_with_mask)
			try_eyes(f, maps);
		if (eyes_unavailable)
			f.eyes = 0;
		f.infer_ms = elapsed(inference);
		TimingSpan post_span("postprocess", f.sequence);
		auto post = Clock::now();
		if (B)
			for (int i = 0; i < 3 * NR; ++i)
				if (!std::isfinite(B[i]))
					throw std::runtime_error("Invalid retouch output");
		if (B) {
			maps.blend_size = nr;
			maps.blend.resize(size_t(NR) * 4);
			for (int i = 0; i < NR; ++i) {
				for (int c = 0; c < 3; ++c)
					maps.blend[4 * i + c] = B[c * NR + i];
				maps.blend[4 * i + 3] = 1;
			}
		}
		f.post_ms = elapsed(post);
		f.milliseconds = elapsed(start);
	}
};
Engine::Engine(const std::string &models, const std::string &cache, uint32_t backend, uint32_t quality)
	: impl(std::make_unique<Impl>(models, cache, backend, quality))
{
}
Engine::~Engine() = default;
void Engine::analyze(Frame &f, Maps &maps)
{
	impl->analyze(f, maps);
}
}
