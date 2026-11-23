#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Persistent launcher configuration: window geometry and UI options, the global
// hotkeys and the auto-capture rules.  It lives in
// %LOCALAPPDATA%\GhostFrame\settings.json and is deliberately free of any UI or
// logging dependency so it can be unit-tested on its own.
namespace gf {

// Hotkey actions, in the order they are written to the file.
enum class HotkeyAction {
    ToggleSelection = 0,
    RestoreAll,
    ToggleUi,
    Minimize,
    Restore,
    Close,
    Count,
};

constexpr int kHotkeyCount = static_cast<int>(HotkeyAction::Count);

struct HotkeyBinding {
    UINT modifiers = 0;   // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    UINT virtualKey = 0;  // a VK_* code

    bool empty() const { return virtualKey == 0; }
};

// One auto-capture rule.  `match` is "exe" (exact, case-insensitive file name)
// or "title" (case-insensitive substring).
struct AutoAttachRule {
    bool enabled = true;
    std::wstring match = L"exe";
    std::wstring value;
    bool hide = true;
};

struct UiSettings {
    int x = 100;
    int y = 100;
    int width = 760;
    int height = 560;
    bool minimizeToTray = true;
    bool noSelfExclude = false;
    bool hideOnAttach = true;
    std::wstring lastTab = L"targets";
};

struct Settings {
    int version = 1;
    UiSettings ui;
    HotkeyBinding hotkeys[kHotkeyCount];
    std::vector<AutoAttachRule> autoAttach;

    // Set by LoadSettings when a corrupt file was renamed to `settings.json.bad`
    // and the defaults were used instead.
    bool badFile = false;
};

// %LOCALAPPDATA%\GhostFrame (falling back to the temp directory), and the two
// files inside it.
std::wstring SettingsDirectory();
std::wstring SettingsPath();
std::wstring SettingsBadPath();

Settings DefaultSettings();

// Pure serialisation: a stable JSON document with a trailing newline.
std::wstring SerializeSettings(const Settings& settings);
// Parses a JSON document.  Missing keys keep their default; unknown keys are
// ignored.  Returns false (with `error`) on malformed input.
bool ParseSettings(const std::wstring& json, Settings& out, std::wstring& error);

// Reads the file.  A missing file yields the defaults; a corrupt file is renamed
// to `settings.json.bad`, the defaults are returned and `out.badFile` is set.
// Returns false only when the file exists but cannot be read at all.
bool LoadSettings(const std::wstring& path, Settings& out, std::wstring& error);
// Writes atomically: `<path>.tmp` then MoveFileEx(MOVEFILE_REPLACE_EXISTING).
bool SaveSettings(const std::wstring& path, const Settings& settings, std::wstring& error);

// "Ctrl+Shift+H" <-> (MOD_*, VK).  The canonical order is Ctrl, Alt, Shift, Win.
bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& virtualKey);
std::wstring FormatHotkey(UINT modifiers, UINT virtualKey);

// Two bindings describe the same combination.
bool SameHotkey(const HotkeyBinding& left, const HotkeyBinding& right);

// An empty combination or a duplicate inside our own set is an error.
bool ValidateHotkeys(const Settings& settings, std::wstring& error);

// Auto-capture matching.  `exeName` is the bare file name; both comparisons are
// case-insensitive.  Disabled rules and empty values never match.
bool RuleMatches(const AutoAttachRule& rule, const std::wstring& exeName,
                 const std::wstring& title);

// Anti-loop state for the auto-capture engine.  A window is keyed by
// (pid, createTime, hwnd), so a recreated window is eligible again.  Windows the
// user removed by hand, or that failed to attach too often, are suppressed until
// the process recreates them.
class AutoCaptureGuard {
public:
    static constexpr int kMaxAttempts = 3;

    // False when the window must not be auto-captured again.
    bool Allow(DWORD pid, ULONGLONG createTime, HWND hwnd) const;
    // Records a failed attach; suppresses the window after kMaxAttempts.
    void NoteFailure(DWORD pid, ULONGLONG createTime, HWND hwnd);
    // The user removed the window: never auto-capture it again.
    void Suppress(DWORD pid, ULONGLONG createTime, HWND hwnd);
    void Clear();

private:
    struct Entry {
        DWORD pid = 0;
        ULONGLONG createTime = 0;
        HWND hwnd = nullptr;
        int attempts = 0;
        bool suppressed = false;
    };
    Entry* Find(DWORD pid, ULONGLONG createTime, HWND hwnd);

    std::vector<Entry> entries_;
};

// The settings-file key and the human-readable label of an action.
const wchar_t* HotkeyActionKey(HotkeyAction action);
const wchar_t* HotkeyActionLabel(HotkeyAction action);
// Finds the action for a settings-file key, or HotkeyAction::Count.
HotkeyAction HotkeyActionFromKey(const std::wstring& key);

}  // namespace gf
