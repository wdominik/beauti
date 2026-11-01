// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in integration check: concurrent engines share the registered provider,
// survive another engine's failed construction, and release sessions safely.
#include "engine.h"
#include <catch_amalgamated.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>

TEST_CASE("Concurrent engines survive failure, destruction, and recreation", "[.provider-lifetime]")
{
	const char *models = std::getenv("BEAUTI_TEST_MODELS"), *fixture = std::getenv("BEAUTI_TEST_FIXTURE"),
		   *cache = std::getenv("BEAUTI_TEST_CACHE");
	if (!models || !fixture || !cache)
		SKIP("Set BEAUTI_TEST_MODELS, BEAUTI_TEST_FIXTURE (1280x720 RGBA), and BEAUTI_TEST_CACHE");
	const auto backend = beauti::default_backend();
	beauti::Frame input;
	input.width = 1280;
	input.height = 720;
	input.retouch = input.smooth = 100;
	input.pixels.resize(size_t(input.width) * input.height * 4);
	std::ifstream stream(fixture, std::ios::binary);
	stream.read(reinterpret_cast<char *>(input.pixels.data()), std::streamsize(input.pixels.size()));
	REQUIRE(stream.gcount() == std::streamsize(input.pixels.size()));
	// Keep CPU and accelerated sessions alive in the same ORT process environment.
	beauti::Engine cpu(models, cache, beauti::CPU);
	auto create = [&] { return std::make_unique<beauti::Engine>(models, cache, backend); };
	auto pending = std::async(std::launch::async, create);
	auto first = create();
	auto second = pending.get();
	auto analyze = [&](beauti::Engine &engine) {
		auto frame = input;
		beauti::Maps maps;
		engine.analyze(frame, maps);
		REQUIRE(frame.status == 0);
		REQUIRE(!maps.blend.empty());
		REQUIRE(!maps.mask.empty());
		return maps;
	};
	const auto expected = analyze(*first);
	REQUIRE_THROWS_AS(beauti::Engine(std::string(models) + "/absent", cache, backend), std::runtime_error);
	first.reset();
	const auto surviving = analyze(*second);
	CHECK(surviving.blend == expected.blend);
	CHECK(surviving.mask == expected.mask);
	second.reset();
	first = create();
	const auto recreated = analyze(*first);
	CHECK(recreated.blend == expected.blend);
	CHECK(recreated.mask == expected.mask);
	analyze(cpu);
}
