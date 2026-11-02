// SPDX-License-Identifier: GPL-3.0-or-later
// Headless benchmark adapter only; OBS calls Engine directly, with no child process.
//
// Binary protocol on stdin/stdout, little-endian, mirrored by
// tools/beauti_tools/bench/protocol.py:
//   request:  uint32 magic, width, height, retouch, smooth, sequence; width*height*4 RGBA bytes
//   response: uint32 magic, width, height, status, sequence, used_backend, blend_size,
//             mask_size, raw_mask; int32 box[4]; float total_ms, detect_ms, infer_ms, post_ms;
//             blend_size*blend_size*4 floats; mask_size*mask_size*4 floats
// Nonzero retouch/smooth request the blend map / skin mask. No pixels are returned.
#include "engine.h"
#include "paths.h"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include <cerrno>
static bool read_exact(void *data, size_t size, bool eof = false)
{
	size_t at = 0;
	while (at < size) {
		size_t n = std::fread(static_cast<char *>(data) + at, 1, size - at, stdin);
		if (!n) {
			if (std::ferror(stdin) && errno == EINTR) {
				std::clearerr(stdin);
				continue;
			}
			if (!at && eof && std::feof(stdin))
				return false;
			throw std::runtime_error("Incomplete input");
		}
		at += n;
	}
	return true;
}
static void write_exact(const void *data, size_t size)
{
	size_t at = 0;
	while (at < size) {
		size_t n = std::fwrite(static_cast<const char *>(data) + at, 1, size - at, stdout);
		if (!n) {
			if (std::ferror(stdout) && errno == EINTR) {
				std::clearerr(stdout);
				continue;
			}
			throw std::runtime_error("Output closed");
		}
		at += n;
	}
}
constexpr uint32_t kMagic = 0x324c4b53; // "SKL2"
constexpr uint32_t kMaxSide = 4096, kMaxStrength = 100;
int beauti_main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	try {
		std::string models, cache = beauti::utf8_string(beauti::cache_root() / "benchmark");
		uint32_t backend = beauti::default_backend(), quality = 0, eyes = 0;
		double frame_ms = 0;
		for (int i = 1; i < argc; ++i) {
			std::string a = argv[i];
			if (a == "--models" && i + 1 < argc)
				models = argv[++i];
			else if (a == "--cache" && i + 1 < argc)
				cache = argv[++i];
			else if (a == "--frame-ms" && i + 1 < argc) // deterministic capture times for clips
				frame_ms = std::stod(argv[++i]);
			else if (a == "--quality" && i + 1 < argc)
				quality = std::string(argv[++i]) == "full" ? 1 : 0;
			else if (a == "--eyes" &&
				 i + 1 < argc) // run the eye models too (timing; maps are not returned)
				eyes = uint32_t(std::stoul(argv[++i]));
			else if (a == "--backend" && i + 1 < argc) {
				std::string b = argv[++i];
				if (b == "cpu")
					backend = beauti::CPU;
				else if (b == "coreai")
					backend = beauti::CoreAI;
				else if (b == "tensorrt")
					backend = beauti::TensorRT;
				else if (b == "tensorrt-fp32")
					backend = beauti::TensorRTFullPrecision;
				else
					throw std::runtime_error("Unknown backend: " + b);
			} else
				throw std::runtime_error("Unknown argument: " + a);
		}
		if (models.empty())
			throw std::runtime_error("--models required");
		beauti::Engine engine(models, cache, backend, quality);
		for (;;) {
			uint32_t request[6];
			if (!read_exact(request, sizeof(request), true))
				return 0;
			if (request[0] != kMagic || !request[1] || !request[2] || request[1] > kMaxSide ||
			    request[2] > kMaxSide || request[3] > kMaxStrength || request[4] > kMaxStrength)
				throw std::runtime_error("Invalid frame header");
			beauti::Frame f;
			f.width = request[1];
			f.height = request[2];
			f.retouch = request[3];
			f.smooth = request[4];
			f.eyes = eyes;
			f.sequence = request[5];
			f.captured_ns = uint64_t(frame_ms * 1e6 * f.sequence);
			f.pixels.resize(size_t(f.width) * f.height * 4);
			read_exact(f.pixels.data(), f.pixels.size());
			beauti::Maps maps;
			engine.analyze(f, maps);
			const uint32_t response[] = {kMagic,          f.width,        f.height,
						     f.status,        f.sequence,     f.used_backend,
						     maps.blend_size, maps.mask_size, maps.raw_mask ? 1u : 0u};
			const float timings[] = {f.milliseconds, f.detect_ms, f.infer_ms, f.post_ms};
			write_exact(response, sizeof(response));
			write_exact(f.box, sizeof(f.box));
			write_exact(timings, sizeof(timings));
			write_exact(maps.blend.data(), maps.blend.size() * sizeof(float));
			write_exact(maps.mask.data(), maps.mask.size() * sizeof(float));
			if (std::fflush(stdout))
				throw std::runtime_error("Output closed");
			std::fprintf(stderr,
				     "frame=%u status=%u total=%.2f detect=%.2f infer=%.2f post=%.2f box=%d,%d,%d,%d\n",
				     f.sequence, f.status, f.milliseconds, f.detect_ms, f.infer_ms, f.post_ms, f.box[0],
				     f.box[1], f.box[2], f.box[3]);
		}
	} catch (const std::exception &e) {
		std::cerr << "Beauti: " << e.what() << std::endl;
		return 1;
	}
}

#include "cli_entry.h"
