// SPDX-License-Identifier: GPL-3.0-or-later
// C++ side of the Core AI adapter: converts adapter failures into exceptions.
#include "coreai_model.h"
#include "coreai.h"
#include <array>
#include <stdexcept>
namespace beauti {
namespace {
// Adapter messages name the asset and Core AI's reason; longer text is truncated.
constexpr size_t kErrorCapacity = 512;
}
void CoreAIModel::Release::operator()(void *model) const
{
	beauti_coreai_release(model);
}
CoreAIModel::CoreAIModel(const std::filesystem::path &asset, const CoreAIModelSpec &spec)
	: output(size_t(spec.channels) * size_t(spec.side) * size_t(spec.side)),
	  input_size(3 * size_t(spec.side) * size_t(spec.side))
{
	std::array<char, kErrorCapacity> error{};
	const int32_t unit = spec.unit == ComputeUnit::GPU ? BEAUTI_COREAI_GPU : BEAUTI_COREAI_NEURAL_ENGINE;
	handle.reset(beauti_coreai_load(asset.c_str(), spec.input.c_str(), spec.output.c_str(), spec.side,
					spec.channels, spec.half ? 1 : 0, unit, error.data(), error.size()));
	if (!handle)
		throw std::runtime_error(error.data());
}
const float *CoreAIModel::run(const std::vector<float> &input)
{
	if (input.size() != input_size)
		throw std::invalid_argument("Core AI input has the wrong size");
	std::array<char, kErrorCapacity> error{};
	if (beauti_coreai_run(handle.get(), input.data(), output.data(), error.data(), error.size()) != 0)
		throw std::runtime_error(error.data());
	return output.data();
}
}
