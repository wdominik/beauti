// SPDX-License-Identifier: GPL-3.0-or-later
// Provider lifetime, capability discovery, and model plans shared by the OBS
// worker and native tools. Windows loads only the pinned TensorRT RTX plugin.
#include "backend.h"
#include "paths.h"
#include <algorithm>
#include <filesystem>
#include <format>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifndef BEAUTI_ENABLE_TENSORRT
#define BEAUTI_ENABLE_TENSORRT 0
#endif
#if defined(_WIN32) && BEAUTI_ENABLE_TENSORRT
#include <windows.h>
#include "beauti-tensorrt.h"
#endif
namespace beauti {
namespace {
constexpr const char *kTensorRTProvider = "NvTensorRTRTXExecutionProvider";
constexpr const char *kRuntimeFileProblem = "NVIDIA runtime file ";
void check(const OrtApi *api, OrtStatus *status)
{
	if (status) {
		std::string message = api->GetErrorMessage(status);
		api->ReleaseStatus(status);
		throw std::runtime_error(message);
	}
}
#if defined(_WIN32) && BEAUTI_ENABLE_TENSORRT
struct LibraryDeleter {
	void operator()(HINSTANCE__ *library) const { FreeLibrary(library); }
};
using Library = std::unique_ptr<HINSTANCE__, LibraryDeleter>;

std::filesystem::path module_directory()
{
	static const int anchor = 0;
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&anchor), &module))
		throw std::runtime_error("Could not locate the TensorRT RTX bundle");
	std::vector<wchar_t> filename(32768);
	const DWORD length = GetModuleFileNameW(module, filename.data(), DWORD(filename.size()));
	if (!length || length >= filename.size())
		throw std::runtime_error("Could not resolve the TensorRT RTX bundle path");
	return std::filesystem::path(filename.data()).parent_path();
}

std::string tensorrt_files_problem()
{
	const auto directory = module_directory();
	for (size_t i = 0; i < kTensorRTLibraries.size(); ++i) {
		auto problem = runtime_file_problem(directory / kTensorRTLibraries[i], kTensorRTLibraryBytes[i]);
		if (!problem.empty())
			return problem;
	}
	return "";
}

std::string probe_nvidia()
{
	// Query the system driver without loading the provider. The driver stays loaded:
	// cuInit state must not be unloaded behind the provider that follows.
	static HMODULE driver = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!driver)
		return "no NVIDIA driver (nvcuda.dll not found)";
	using VersionFn = int(__stdcall *)(int *);
	using InitFn = int(__stdcall *)(unsigned int);
	using AttributeFn = int(__stdcall *)(int *, int, int);
	const auto version = reinterpret_cast<VersionFn>(GetProcAddress(driver, "cuDriverGetVersion"));
	const auto init = reinterpret_cast<InitFn>(GetProcAddress(driver, "cuInit"));
	const auto count = reinterpret_cast<VersionFn>(GetProcAddress(driver, "cuDeviceGetCount"));
	const auto attribute = reinterpret_cast<AttributeFn>(GetProcAddress(driver, "cuDeviceGetAttribute"));
	if (!version || !init || !count || !attribute)
		return "incomplete NVIDIA driver (CUDA entry points missing)";
	int api_version = 0, devices = 0;
	if (version(&api_version) != 0 || api_version < BEAUTI_TENSORRT_DRIVER_MINIMUM)
		return "TensorRT RTX requires an NVIDIA driver supporting CUDA 13 or newer";
	if (init(0) != 0 || count(&devices) != 0)
		return "the NVIDIA driver failed to initialize";
	// The pinned EP accepts RTX SM 8.6, 8.9, and 12.0+. This is a capability
	// requirement, never a per-card performance policy (NVIDIA EP 0.5.0).
	constexpr int kComputeCapabilityMajor = 75, kComputeCapabilityMinor = 76;
	for (int device = 0; device < devices; ++device) {
		int major = 0, minor = 0;
		if (attribute(&major, kComputeCapabilityMajor, device) == 0 &&
		    attribute(&minor, kComputeCapabilityMinor, device) == 0 &&
		    ((major == 8 && (minor == 6 || minor == 9)) || major >= 12))
			return tensorrt_files_problem();
	}
	return "no supported NVIDIA RTX GPU (Ampere or newer required)";
}
#endif

std::string probe_accelerator()
{
#if defined(_WIN32) && BEAUTI_ENABLE_TENSORRT
	return probe_nvidia();
#elif defined(__APPLE__)
	return ""; // Core AI ships with macOS 27, the minimum version, on every Apple silicon Mac
#else
	return "this build has no accelerator";
#endif
}
}

struct ProviderState {
	const OrtApi *api;
#if defined(_WIN32) && BEAUTI_ENABLE_TENSORRT
	// Declared before env so DLL handles are released after the environment.
	std::vector<Library> libraries;
#endif
	std::unique_ptr<OrtEnv, decltype(OrtApi::ReleaseEnv)> env;
	bool registered = false;
	ProviderState(const OrtApi *a, bool nvidia) : api(a), env(nullptr, a->ReleaseEnv)
	{
		OrtEnv *created = nullptr;
		check(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "beauti", &created));
		env.reset(created);
		check(api, api->DisableTelemetryEvents(env.get()));
#if defined(_WIN32) && BEAUTI_ENABLE_TENSORRT
		if (nvidia) {
			const auto directory = module_directory();
			// The user installs these files beside the plugin (docs/INSTALL.md). Load them
			// without changing the host's PATH or DLL search policy.
			for (size_t i = 0; i < kTensorRTLibraries.size(); ++i) {
				const auto *name = kTensorRTLibraries[i];
				auto problem = runtime_file_problem(directory / name, kTensorRTLibraryBytes[i]);
				if (!problem.empty())
					throw std::runtime_error(problem);
				Library library(LoadLibraryExW((directory / name).c_str(), nullptr,
							       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
								       LOAD_LIBRARY_SEARCH_SYSTEM32));
				if (!library)
					throw std::runtime_error(
						std::format("Could not load TensorRT RTX library {} (Windows error {})",
							    name, GetLastError()));
				libraries.push_back(std::move(library));
			}
			check(api,
			      api->RegisterExecutionProviderLibrary(env.get(), kTensorRTProvider,
								    (directory / kTensorRTLibraries.back()).c_str()));
			registered = true;
		}
#else
		(void)nvidia;
#endif
	}
	~ProviderState()
	{
		// Owning engines destroy sessions first. Never throw during module shutdown.
		if (registered) {
			if (auto *status = api->UnregisterExecutionProviderLibrary(env.get(), kTensorRTProvider))
				api->ReleaseStatus(status);
		}
	}
};

ProviderEnvironment::ProviderEnvironment(const OrtApi *api, uint32_t backend)
{
	if (!backend_supported(backend))
		throw std::runtime_error("The selected processing device is not supported by this build");
	if (backend == TensorRT || backend == TensorRTFullPrecision) {
		// ORT reuses its process environment. Register once, retaining the factory
		// until module unload to avoid unregister/register races between filters.
		static std::mutex mutex;
		static std::shared_ptr<ProviderState> shared;
		std::lock_guard lock(mutex);
		if (!shared)
			shared = std::make_shared<ProviderState>(api, true);
		state = shared;
	} else {
		state = std::make_shared<ProviderState>(api, false);
	}
}
OrtEnv *ProviderEnvironment::get() const
{
	return state->env.get();
}

bool known_backend(uint32_t backend)
{
	return backend == CoreAI || backend == CPU || backend == TensorRT || backend == TensorRTFullPrecision;
}
std::vector<BackendChoice> backend_choices()
{
	std::vector<BackendChoice> result;
#ifdef __APPLE__
	result.push_back({CoreAI, "AppleAcceleration"});
#endif
#if BEAUTI_ENABLE_TENSORRT
	result.push_back({TensorRTFullPrecision, "NVIDIAFullPrecision"});
	result.push_back({TensorRT, "NVIDIAAcceleration"});
#endif
	result.push_back({CPU, "CPU"});
	return result;
}
const std::string &accelerator_reason()
{
	static const std::string reason = probe_accelerator();
	return reason;
}
std::string runtime_file_problem(const std::filesystem::path &file, uintmax_t expected_bytes)
{
	std::error_code error;
	const auto bytes = std::filesystem::file_size(file, error);
	if (error)
		return std::format("{}{} is missing; see the installation guide", kRuntimeFileProblem,
				   file.filename().string());
	if (bytes != expected_bytes)
		return std::format("{}{} has the wrong version; see the installation guide", kRuntimeFileProblem,
				   file.filename().string());
	return "";
}
bool is_runtime_file_problem(const std::string &reason)
{
	return reason.starts_with(kRuntimeFileProblem);
}
bool accelerator_available()
{
	return accelerator_reason().empty();
}
uint32_t choose_default(bool accelerator)
{
	return accelerator ? backend_choices().front().id : CPU;
}
uint32_t default_backend()
{
	return choose_default(accelerator_available());
}
bool backend_supported(uint32_t backend)
{
	for (auto choice : backend_choices())
		if (choice.id == backend)
			return true;
	return false;
}
uint32_t normalize_backend(int64_t backend)
{
	return backend >= 0 && backend <= TensorRTFullPrecision && known_backend(uint32_t(backend)) &&
			       backend_supported(uint32_t(backend))
		       ? uint32_t(backend)
		       : default_backend();
}
int auxiliary_threads_for(unsigned physical_cores)
{
	return std::clamp(int(physical_cores / 4), 2, 4);
}
int auxiliary_cpu_threads()
{
#ifdef _WIN32
	static const int threads = [] {
		DWORD size = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
		std::vector<char> buffer(size);
		auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data());
		if (!size || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size))
			return auxiliary_threads_for(0);
		unsigned cores = 0;
		for (DWORD at = 0; at < size; at += info->Size) {
			info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + at);
			cores += info->Relationship == RelationProcessorCore;
		}
		return auxiliary_threads_for(cores);
	}();
	return threads;
#else
	// Core AI's auxiliary budget is unchanged until it is measured on Apple hardware.
	return 2;
#endif
}
ModelPlan model_plan(uint32_t backend, uint32_t quality)
{
	if (!known_backend(backend) || quality > 1)
		throw std::invalid_argument("Invalid processing mode");
	ModelPlan p;
	p.retouch_size = quality ? 512 : 384;
	p.skin_size = 256; // MediaPipe Multiclass has one input size
	p.retouch_backend = backend;
	if (backend == CoreAI) {
		// Converted assets: retouch on the Neural Engine, segmentation on the GPU next to it.
		p.coreai = true;
		p.skin_backend = CoreAI;
		p.retouch = "coreai/retouch-" + std::to_string(p.retouch_size) + ".aimodel";
		p.skin = "coreai/skin-segmenter.aimodel";
		return p;
	}
	// With TensorRT RTX the segmenter runs next to the retouch model; on the CPU it bounded
	// the analysis at about 13 FPS (docs/decisions/0022).
	p.skin_backend = backend;
	p.skin = "native/skin_segmenter.onnx";
	const std::string variant = backend == TensorRT ? "fp16" : "optimized";
	p.retouch = (quality ? variant : "native/" + variant + "-fast") + "/skin_retouching.onnx";
	return p;
}
void configure_provider(const OrtApi *api, OrtEnv *env, OrtSessionOptions *options, uint32_t backend,
			const std::string &cache)
{
	if (!backend_supported(backend))
		throw std::runtime_error("The selected processing device is not supported by this build");
	if (backend == CPU)
		return;
	std::filesystem::create_directories(utf8_path(cache));
	if (backend == TensorRT || backend == TensorRTFullPrecision) {
		const OrtEpDevice *const *devices = nullptr;
		size_t count = 0;
		check(api, api->GetEpDevices(env, &devices, &count));
		for (size_t i = 0; i < count; ++i) {
			if (std::string(api->EpDevice_EpName(devices[i])) != kTensorRTProvider)
				continue;
			// Static model shapes use the provider's standard graph replay and cache.
			// TensorRT validates runtime caches and rebuilds incompatible data.
			const char *keys[] = {"enable_cuda_graph", "nv_runtime_cache_path"};
			const char *values[] = {"1", cache.c_str()};
			check(api, api->SessionOptionsAppendExecutionProvider_V2(options, env, &devices[i], 1, keys,
										 values, 2));
			return;
		}
		throw std::runtime_error("TensorRT RTX has no compatible NVIDIA device");
	}
	throw std::logic_error("Core AI models do not run through ONNX Runtime");
}
}
