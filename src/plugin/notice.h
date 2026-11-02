// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace beauti {
// Shows a native message to the user, with UTF-8 text. Call from the UI thread; the
// window handle is the host's main window and is used on Windows only.
void show_notice(void *window, const char *title, const char *text);
}
