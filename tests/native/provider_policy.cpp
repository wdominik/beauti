// SPDX-License-Identifier: GPL-3.0-or-later
// Backend selection and model planning (src/backends/backend.h): which model
// files and devices each backend and quality use, and how saved settings map.
#include "backend.h"
#include <catch_amalgamated.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace beauti;

TEST_CASE("Each backend plans the retouch sizes and the shared segmenter", "[provider-policy]")
{
	const uint32_t backend = GENERATE(CoreAI, CPU, TensorRT, TensorRTFullPrecision);
	CAPTURE(backend);
	const auto fast = model_plan(backend, 0), full = model_plan(backend, 1);
	CHECK(fast.retouch_size == 384);
	CHECK(full.retouch_size == 512);
	CHECK(fast.skin_size == 256); // MediaPipe Multiclass in both quality modes
	CHECK(full.skin_size == 256);
	CHECK(full.retouch_backend == backend);
	CHECK(fast.coreai == (backend == CoreAI));
	CHECK(full.coreai == (backend == CoreAI));
	if (backend == CoreAI) {
		// Converted assets replace the ONNX graphs; nothing runs through ONNX Runtime.
		CHECK(fast.retouch == "coreai/retouch-384.aimodel");
		CHECK(full.retouch == "coreai/retouch-512.aimodel");
		CHECK(full.skin == "coreai/skin-segmenter.aimodel");
		CHECK(full.skin_backend == CoreAI);
	} else {
		// Elsewhere the segmenter runs on the selected device, TensorRT RTX included.
		CHECK(full.skin == "native/skin_segmenter.onnx");
		CHECK(full.skin_backend == backend);
		CHECK(fast.skin_backend == backend);
	}
	CHECK(fast.detector_backend == CPU);
	CHECK(full.landmarks_backend == CPU);
	if (backend == TensorRT) {
		CHECK(fast.retouch == "native/fp16-fast/skin_retouching.onnx");
		CHECK(full.retouch == "fp16/skin_retouching.onnx");
	}
	if (backend == CPU || backend == TensorRTFullPrecision) {
		CHECK(fast.retouch == "native/optimized-fast/skin_retouching.onnx");
		CHECK(full.retouch == "optimized/skin_retouching.onnx");
	}
}

TEST_CASE("The default is the build's accelerator when the machine has it, else CPU", "[provider-policy]")
{
	CHECK(backend_supported(default_backend()));
	CHECK(backend_supported(CPU));
	CHECK(choose_default(false) == CPU);
	CHECK(choose_default(true) == backend_choices().front().id);
	CHECK(default_backend() == choose_default(accelerator_available()));
	CHECK(accelerator_available() == accelerator_reason().empty());
#ifdef __APPLE__
	CHECK(backend_choices().front().id == CoreAI);
	CHECK(backend_choices().size() == 2); // Core AI and CPU
#elif BEAUTI_ENABLE_TENSORRT
	CHECK(backend_choices().front().id == TensorRTFullPrecision); // Full Precision on every NVIDIA GPU
#else
	CHECK(backend_choices().front().id == CPU);
	CHECK_FALSE(accelerator_available()); // a CPU-only build has nothing to probe
#endif
}

TEST_CASE("User-installed runtime files must exist and match the pinned size", "[provider-policy]")
{
	const auto file = std::filesystem::temp_directory_path() / "beauti-runtime-file-test.dll";
	std::filesystem::remove(file);
	const auto missing = runtime_file_problem(file, 4);
	CHECK(is_runtime_file_problem(missing));
	CHECK(missing.find("beauti-runtime-file-test.dll is missing") != std::string::npos);
	std::ofstream(file) << "four";
	CHECK(runtime_file_problem(file, 4).empty());
	const auto wrong = runtime_file_problem(file, 5);
	CHECK(is_runtime_file_problem(wrong));
	CHECK(wrong.find("wrong version") != std::string::npos);
	std::filesystem::remove(file);
	CHECK_FALSE(is_runtime_file_problem("no NVIDIA driver (nvcuda.dll not found)"));
	CHECK_FALSE(is_runtime_file_problem(""));
}

TEST_CASE("Saved selections are kept; invalid ones fall back to the default", "[provider-policy]")
{
	for (const auto &choice : backend_choices()) {
		CAPTURE(choice.id);
		CHECK(normalize_backend(choice.id) == choice.id);
	}
	CHECK(normalize_backend(-1) == default_backend());
	CHECK(normalize_backend(1) == default_backend()); // unassigned ID
	CHECK(normalize_backend(999) == default_backend());
}

TEST_CASE("CPU sessions next to an accelerator use a quarter of the cores, two to four", "[provider-policy]")
{
	CHECK(auxiliary_threads_for(0) == 2); // unknown topology
	CHECK(auxiliary_threads_for(4) == 2);
	CHECK(auxiliary_threads_for(8) == 2);
	CHECK(auxiliary_threads_for(12) == 3);
	CHECK(auxiliary_threads_for(16) == 4);
	CHECK(auxiliary_threads_for(64) == 4);
	const int threads = auxiliary_cpu_threads();
	CHECK(threads >= 2);
	CHECK(threads <= 4);
}

TEST_CASE("Unknown backends are rejected when planning", "[provider-policy]")
{
	CHECK_THROWS_AS(model_plan(99, 0), std::invalid_argument);
	CHECK_THROWS_AS(model_plan(1, 0), std::invalid_argument);
}
