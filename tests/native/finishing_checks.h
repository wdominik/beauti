// SPDX-License-Identifier: GPL-3.0-or-later
// Real-face comparisons plus a controlled color/texture chart for the NumPy oracle.
#pragma once
static void check_finishing(SkinGPU &gpu, const beauti::Frame &frame, const beauti::Maps &maps, gs_texture_t *original,
			    uint32_t quality, const std::filesystem::path &out)
{
	auto render = [&](const char *name, const beauti::FinishingControls &c, bool real = true,
			  gs_texture_t *input = nullptr, const beauti::Maps *m = nullptr, uint64_t revision = 0) {
		auto pixels = read_texture(gpu.compose(input ? input : original, frame, m ? *m : maps, quality,
						       real ? 100.f : 0.f, real ? 100.f : 0.f, real ? 80.f : 0.f,
						       real ? 60.f : 0.f, revision, real ? 80.f : 0.f, c),
					   frame.width, frame.height);
		if (name)
			write(out / (std::string(name) + ".rgba"), pixels);
		return pixels;
	};
	beauti::FinishingControls defaults, texture, color, combined;
	texture.gains = {.25f, .95f, .90f, .35f};
	texture.size = 1.3f;
	texture.edges = 1.15f;
	color.color = .6f;
	color.color_size = 1.2f;
	combined = texture;
	combined.color = color.color;
	combined.color_size = color.color_size;
	const auto baseline = render("realistic", defaults);
	render("texture-study", texture);
	render("color-study", color);
	const auto together = render("combined-study", combined, true, nullptr, nullptr, 80);
	if (together != render(nullptr, combined, true))
		throw std::runtime_error("Local color cache mismatch");
	if (baseline != render(nullptr, defaults, true))
		throw std::runtime_error("Local color bypass retained state");
	if (frame.pixels != render(nullptr, defaults, false))
		throw std::runtime_error("Finishing zero is not exact");
	auto empty = maps;
	std::fill(empty.mask.begin(), empty.mask.end(), 0.f);
	if (frame.pixels != render(nullptr, color, false, nullptr, &empty))
		throw std::runtime_error("Local color changed an empty skin mask");
	// Synthetic input has fine luminance texture, chroma variation, and a sharp
	// color boundary. A protected central rectangle exercises hard exclusions.
	auto chart = frame.pixels;
	for (uint32_t y = 0; y < frame.height; ++y)
		for (uint32_t x = 0; x < frame.width; ++x) {
			const size_t i = (size_t(y) * frame.width + x) * 4;
			const float detail = 8.f * std::sin(float(x) * 1.7f) * std::cos(float(y) * 1.3f);
			const float tint = 12.f * std::sin(float(x) * .23f) * std::cos(float(y) * .19f);
			chart[i] = uint8_t(std::lround(155.f + detail + tint + (x > frame.width / 2 ? 20.f : 0.f)));
			chart[i + 1] = uint8_t(std::lround(111.f + detail - tint * .3f));
			chart[i + 2] = uint8_t(std::lround(94.f + detail - tint));
			chart[i + 3] = 255;
		}
	write(out / "chart-original.rgba", chart);
	auto chart_maps = maps;
	chart_maps.raw_mask = false;
	for (uint32_t y = 0; y < maps.mask_size; ++y)
		for (uint32_t x = 0; x < maps.mask_size; ++x) {
			const size_t i = (size_t(y) * maps.mask_size + x) * 4;
			const bool excluded = x > maps.mask_size / 3 && x < maps.mask_size / 2 &&
					      y > maps.mask_size / 3 && y < maps.mask_size / 2;
			chart_maps.mask[i] = chart_maps.mask[i + 1] = excluded ? 0.f : 1.f;
		}
	write(out / "chart-mask.f32", chart_maps.mask);
	const uint8_t *bytes = chart.data();
	beauti::Texture chart_texture(gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0));
	if (!chart_texture)
		throw std::runtime_error("Could not upload finishing chart");
	auto no_bands = defaults;
	no_bands.gains.fill(0.f);
	if (read_texture(gpu.compose(chart_texture.get(), frame, chart_maps, quality, 0, 0, 0, 100, 0, 0, no_bands),
			 frame.width, frame.height) != chart)
		throw std::runtime_error("Zero detail reductions changed the source");
	for (int i = 0; i < 4; ++i) {
		auto c = color;
		c.color = i == 0 ? .25f : 1.f;
		c.color_size = i == 2 ? 2.f : 1.f;
		c.color_edges = i == 3 ? 0.f : 1.f;
		const auto name = "chart-color-" + std::to_string(i);
		render(name.c_str(), c, false, chart_texture.get(), &chart_maps);
	}
	for (int i = 0; i < 5; ++i) {
		auto c = defaults;
		c.gains.fill(0.f);
		c.gains[size_t(i % 4)] = 1.f;
		if (i == 4) {
			c.size = 2;
			c.edges = .5f;
		}
		auto pixels = read_texture(
			gpu.compose(chart_texture.get(), frame, chart_maps, quality, 0, 0, 0, 100, 0, 0, c),
			frame.width, frame.height);
		write(out / ("chart-texture-" + std::to_string(i) + ".rgba"), pixels);
	}
	// Alpha is preserved with the optional precomposition active.
	for (size_t i = 0; i < chart.size(); i += 4) {
		const uint8_t alpha = uint8_t((i / 4) % 3 ? 127 : 0);
		for (int c = 0; c < 3; ++c)
			chart[i + c] = uint8_t(std::lround(float(chart[i + c]) * alpha / 255.f));
		chart[i + 3] = alpha;
	}
	bytes = chart.data();
	chart_texture.reset(gs_texture_create(frame.width, frame.height, GS_RGBA, 1, &bytes, 0));
	const auto alpha = render("chart-alpha", combined, false, chart_texture.get(), &chart_maps);
	write(out / "chart-alpha-original.rgba", chart);
	for (size_t i = 0; i < alpha.size(); i += 4)
		if (alpha[i + 3] != chart[i + 3] || (!alpha[i + 3] && (alpha[i] || alpha[i + 1] || alpha[i + 2])))
			throw std::runtime_error("Local color changed alpha or transparent RGB");
}
