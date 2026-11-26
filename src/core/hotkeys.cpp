#include "hotkeys.h"

#include "log.h"

namespace gf {

bool HotkeyManager::Apply(HWND window, const Settings& settings, std::wstring& error) {
    Unregister(window);

    if (!ValidateHotkeys(settings, error)) {
        return false;
    }
    LogLine(L"hotkeys: registering the set");

    std::wstring failed;
    for (int i = 0; i < kHotkeyCount; ++i) {
        const HotkeyBinding& binding = settings.hotkeys[i];
        if (binding.empty()) {
            continue;
        }
        if (!::RegisterHotKey(window, IdFor(static_cast<HotkeyAction>(i)),
                              binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
            if (!failed.empty()) {
                failed += L", ";
            }
            failed += FormatHotkey(binding.modifiers, binding.virtualKey);
        }
    }

    if (!failed.empty()) {
        Unregister(window);
        error = L"HOTKEY " + failed + L" IS TAKEN BY ANOTHER APP";
        return false;
    }

    active_ = true;
    LogLine(L"hotkeys: set registered");
    return true;
}

void HotkeyManager::Unregister(HWND window) {
    for (int i = 0; i < kHotkeyCount; ++i) {
        ::UnregisterHotKey(window, IdFor(static_cast<HotkeyAction>(i)));
    }
    active_ = false;
}

}  // namespace gf