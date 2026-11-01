// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include "backend.h"
#include "frame.h"
#include <memory>
#include <string>
#include <vector>
namespace beauti {
class Engine {
public:
	// Backend IDs are defined in backend.h; unsupported selections fail explicitly.
	Engine(const std::string &models, const std::string &cache, uint32_t backend, uint32_t quality = 0);
	~Engine();
	Engine(const Engine &) = delete;
	Engine &operator=(const Engine &) = delete;
	// Detects the largest face and produces the requested maps. Pixels are read only.
	void analyze(Frame &frame, Maps &maps);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
}
