// SPDX-License-Identifier: GPL-3.0-or-later
// One Core AI model for the engine on macOS: owns the adapter handle (coreai.h) and
// the float32 output buffer that the last run() filled.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace beauti {
enum class ComputeUnit { NeuralEngine, GPU };
struct CoreAIModelSpec {
	std::string input, output;
	int side, channels;
	bool half; // the asset's tensors are float16
	ComputeUnit unit;
};
class CoreAIModel {
	struct Release {
		void operator()(void *handle) const;
	};
	std::unique_ptr<void, Release> handle;
	std::vector<float> output;
	size_t input_size;

public:
	CoreAIModel(const std::filesystem::path &asset, const CoreAIModelSpec &spec);
	// Runs one NCHW float32 input; the result stays valid until the next run.
	const float *run(const std::vector<float> &input);
};
}
