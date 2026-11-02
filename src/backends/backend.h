// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "onnxruntime_c_api.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace beauti {
// Numeric IDs are persisted in OBS settings: 0 Apple (Core AI), 2 CPU, 3 and 4 the
// NVIDIA devices. 1 is unassigned.
enum Backend : uint32_t { CoreAI = 0, CPU = 2, TensorRT = 3, TensorRTFullPrecision = 4 };
struct BackendChoice {
	uint32_t id;
	const char *locale_key;
};
// Model files relative to the model directory. Core AI plans name .aimodel assets
// that bypass ONNX Runtime; every other plan names ONNX graphs. The detector and the
// landmark models (FaceMesh, Iris) are always ONNX graphs.
struct ModelPlan {
	std::string retouch, skin;
	uint32_t retouch_backend, skin_backend;
	uint32_t detector_backend = CPU, landmarks_backend = CPU;
	int retouch_size, skin_size;
	bool coreai = false;
};
// Intra-op threads for CPU sessions next to an accelerator: a quarter of the physical
// cores, two to four, so OBS keeps most of the CPU (docs/DECISIONS.md). The pure rule
// is auxiliary_threads_for; only Windows measures the cores, elsewhere it stays two.
int auxiliary_threads_for(unsigned physical_cores);
int auxiliary_cpu_threads();
// Whether the build's accelerator can run on this machine, probed once: Windows asks
// the NVIDIA driver for a CUDA 13 driver and a supported RTX GPU and checks the
// user-installed NVIDIA runtime files; on macOS, Core AI is part of the minimum OS.
// accelerator_reason() is empty when available.
bool accelerator_available();
const std::string &accelerator_reason();
// Why a user-installed runtime file cannot be used, or empty when its size matches the
// pinned version. is_runtime_file_problem recognizes these reasons for the status line.
std::string runtime_file_problem(const std::filesystem::path &file, uintmax_t expected_bytes);
bool is_runtime_file_problem(const std::string &reason);
// The default for new filters: the build's first choice when its accelerator is
// available, otherwise CPU (docs/DECISIONS.md). choose_default is the pure rule.
uint32_t choose_default(bool accelerator);
uint32_t default_backend();
std::vector<BackendChoice> backend_choices();
// known: an assigned ID on any platform; supported: offered by this build.
bool known_backend(uint32_t backend);
bool backend_supported(uint32_t backend);
uint32_t normalize_backend(int64_t backend);
ModelPlan model_plan(uint32_t backend, uint32_t quality);
struct ProviderState;
// ONNX Runtime environment for the CPU and NVIDIA sessions. NVIDIA registration is
// shared for the module lifetime; CPU sessions do not load it.
// Engines retain the environment until all their sessions have been destroyed.
class ProviderEnvironment {
	std::shared_ptr<ProviderState> state;

public:
	ProviderEnvironment(const OrtApi *api, uint32_t backend);
	OrtEnv *get() const;
};
void configure_provider(const OrtApi *api, OrtEnv *env, OrtSessionOptions *options, uint32_t backend,
			const std::string &cache);
}
