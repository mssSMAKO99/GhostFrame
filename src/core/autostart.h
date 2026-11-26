// Per-user autostart (HKCU Run key).  No admin rights and no scheduled task: the
// daemon is started for the current user when they log in, so capture is active
// before the UI is opened.
#pragma once

#include <windows.h>

#include <string>

namespace gf {

// The registry value name under HKCU\...\Run.  Branded installs use the custom
// executable stem, so a renamed build never shares an entry with GhostFrame.
std::wstring AutostartValueName();
inline constexpr wchar_t kAutostartKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The command that is stored in the Run value: the current executable started
// with --daemon.
std::wstring AutostartCommand();

// Reads the Run value and checks that it still points at this executable.
bool IsAutostartEnabled();
// Writes (or deletes) the Run value.  Returns false on a registry error.
bool SetAutostartEnabled(bool enabled);

}  // namespace gf