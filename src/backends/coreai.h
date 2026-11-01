// SPDX-License-Identifier: GPL-3.0-or-later
// C interface of the Core AI adapter (coreai.swift), used by the engine on macOS.
// Every call blocks the calling thread until Core AI has finished; never call it
// from a Swift concurrency task.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Preferred compute unit: the retouch model runs on the Neural Engine, the skin
// mask on the GPU next to it (docs/APPLE-ACCELERATION.md).
enum { BEAUTI_COREAI_NEURAL_ENGINE = 0, BEAUTI_COREAI_GPU = 1 };
// Loads the asset's "main" function with one [1,3,side,side] input and one output of
// channels * side * side values in the model's own layout; half selects float16
// input and output tensors.
// Returns NULL and writes a message into error when loading fails.
void *beauti_coreai_load(const char *asset, const char *input, const char *output, int32_t side, int32_t channels,
			 int32_t half, int32_t unit, char *error, size_t error_size);
// Converts the float32 input, runs the model, and writes float32 output.
// Returns 0, or nonzero with a message in error.
int32_t beauti_coreai_run(void *model, const float *input, float *output, char *error, size_t error_size);
void beauti_coreai_release(void *model);
#ifdef __cplusplus
}
#endif
