// Global hotkeys, owned by the daemon.
//
// `RegisterHotKey` binds a combination to the process; the window passed here
// only receives WM_HOTKEY.  The daemon registers the whole set, so the shortcuts
// keep working when the UI is closed or was never opened.
#pragma once

#include <windows.h>

#include <string>

#include "settings.h"

namespace gf {

class HotkeyManager {
public:
    // Hotkey ids live above the command ids used by the UI so a stray WM_HOTKEY
    // can never be mistaken for a control command.
    static constexpr int kIdBase = 0x4000;

    static int IdFor(HotkeyAction action) {
        return kIdBase + static_cast<int>(action);
    }
    static HotkeyAction ActionFromId(int id) {
        const int action = id - kIdBase;
        if (action < 0 || action >= static_cast<int>(HotkeyAction::Count)) {
            return HotkeyAction::Count;
        }
        return static_cast<HotkeyAction>(action);
    }

    // Validates the bindings and registers them, replacing the previous set.  On
    // failure nothing stays registered and `error` explains which combination was
    // rejected.
    bool Apply(HWND window, const Settings& settings, std::wstring& error);

    // Releases every combination this manager registered.
    void Unregister(HWND window);

    bool active() const { return active_; }

private:
    bool active_ = false;
};

}  // namespace gf