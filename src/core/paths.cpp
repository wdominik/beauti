// SPDX-License-Identifier: GPL-3.0-or-later
#include "paths.h"
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
namespace beauti {
std::filesystem::path cache_root()
{
#ifdef _WIN32
	PWSTR folder = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
		throw std::runtime_error("The cache directory is unavailable");
	std::filesystem::path path(folder);
	CoTaskMemFree(folder);
	return path / L"Beauti" / L"Cache";
#else
	const char *home = std::getenv("HOME");
	if (!home || !*home)
		throw std::runtime_error("The cache directory is unavailable");
#ifdef __APPLE__
	return utf8_path(home) / "Library/Caches/Beauti";
#else
	const char *xdg = std::getenv("XDG_CACHE_HOME");
	return (xdg && *xdg ? utf8_path(xdg) : utf8_path(home) / ".cache") / "Beauti";
#endif
#endif
}
}
