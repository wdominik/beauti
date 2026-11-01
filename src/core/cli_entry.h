// SPDX-License-Identifier: GPL-3.0-or-later
// Windows passes UTF-16 command-line arguments. Preserve paths outside the ACP.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <string>
#include <vector>
int wmain(int argc, wchar_t **argv)
{
	std::vector<std::string> values;
	std::vector<char *> pointers;
	values.reserve(argc);
	pointers.reserve(argc + 1);
	for (int i = 0; i < argc; ++i) {
		int size =
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
		if (!size)
			return 2;
		std::string value(size, '\0');
		if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), size, nullptr,
					 nullptr))
			return 2;
		value.pop_back();
		values.push_back(std::move(value));
	}
	for (auto &value : values)
		pointers.push_back(value.data());
	pointers.push_back(nullptr);
	return beauti_main(argc, pointers.data());
}
#else
int main(int argc, char **argv)
{
	return beauti_main(argc, argv);
}
#endif
