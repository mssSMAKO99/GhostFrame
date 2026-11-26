// Messages shared by the daemon and the UI window.
//
// The daemon owns the tray icon and the global hotkeys, so a few actions (show
// the UI, toggle it, quit it) have to reach the UI even when it was not the
// caller.  Instead of adding a second push channel to the pipe protocol, the
// daemon posts these ordinary window messages to the UI window: the UI window
// runs on the same desktop and is found by its well-known class name.
#pragma once

#include <windows.h>

namespace gf {

// Registered by the UI for its main window.
inline constexpr wchar_t kUiWindowClass[] = L"GhostFrame.MainWindow";
// The daemon's hidden message window.  Shared so the UI can find the daemon's
// process when it has to wait for it before deleting the executable.
inline constexpr wchar_t kDaemonWindowClass[] = L"GhostFrame.DaemonWindow";

// Show and focus the window.
inline constexpr UINT kUiShowMessage = WM_APP + 0x70;
// Toggle its visibility.
inline constexpr UINT kUiToggleMessage = WM_APP + 0x71;
// Close it (used when the daemon is told to exit).
inline constexpr UINT kUiQuitMessage = WM_APP + 0x72;
// Close it without running the delete-on-exit prompt: the daemon already
// started the self-delete flow.
inline constexpr UINT kUiSelfDeleteMessage = WM_APP + 0x73;

}  // namespace gf