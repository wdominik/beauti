// SPDX-License-Identifier: GPL-3.0-or-later
// A native dialog, kept apart from the filter so its platform headers stay here. The
// plugin does not link Qt, and the OBS frontend API has no call for showing a message.
#include "notice.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <util/bmem.h>
#include <util/platform.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
namespace beauti {
void show_notice(void *window, const char *title, const char *text)
{
#ifdef _WIN32
	wchar_t *wide_title = nullptr, *wide_text = nullptr;
	os_utf8_to_wcs_ptr(title, 0, &wide_title);
	os_utf8_to_wcs_ptr(text, 0, &wide_text);
	if (wide_title && wide_text)
		MessageBoxW(static_cast<HWND>(window), wide_text, wide_title, MB_OK | MB_ICONINFORMATION);
	bfree(wide_title);
	bfree(wide_text);
#elif defined(__APPLE__)
	(void)window;
	CFStringRef header = CFStringCreateWithCString(nullptr, title, kCFStringEncodingUTF8);
	CFStringRef message = CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
	if (header && message)
		CFUserNotificationDisplayNotice(0, kCFUserNotificationNoteAlertLevel, nullptr, nullptr, nullptr, header,
						message, nullptr);
	if (header)
		CFRelease(header);
	if (message)
		CFRelease(message);
#else
	(void)window;
	(void)title;
	(void)text;
#endif
}
}
