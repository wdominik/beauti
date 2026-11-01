// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace beauti {
// Per-user cache for compiled provider programs (TensorRT RTX) and benchmark runs:
// ~/Library/Caches/Beauti, %LocalAppData%\Beauti\Cache, or $XDG_CACHE_HOME/Beauti.
std::filesystem::path cache_root();

// std::string values in Beauti hold UTF-8 on every platform. These helpers
// replace std::filesystem::u8path and path::u8string, which C++20 deprecated
// and changed to std::u8string respectively.
inline std::filesystem::path utf8_path(std::string_view utf8)
{
	return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}
inline std::string utf8_string(const std::filesystem::path &path)
{
	const std::u8string u8 = path.u8string();
	return std::string(u8.begin(), u8.end());
}
}
