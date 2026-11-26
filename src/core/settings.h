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
    // Optional: empty by default, so an accidental key never wipes an install.
    SelfDestruct,
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

// What the in-app self-delete flow removes.  `Never` is the default: the user
// has to opt in explicitly.
enum class SelfDeleteMode { Never = 0, ExeOnly, Everything, Count };

struct UiSettings {
    int x = 100;
    int y = 100;
    int width = 760;
    int height = 560;
    bool minimizeToTray = true;
    bool noSelfExclude = false;
    bool hideOnAttach = true;
    std::wstring lastTab = L"targets";

    // Self-delete is opt-in: `Never` disables every automatic path.  The mode is
    // what `Delete now`, the two-step hotkey/tray action and delete-on-exit use.
    SelfDeleteMode selfDelete = SelfDeleteMode::Never;
    // Only meaningful when selfDelete != Never; normalised to false otherwise.
    bool deleteOnExit = false;
};

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------

enum class ThemePreset { Graphite = 0, Light, Midnight, System, Count };
enum class DensityMode { Comfortable = 0, Compact, Count };
enum class BackgroundStyle { Solid = 0, Gradient, Count };

// One tab's place in the strip.  The list order is the display order; a view
// that is missing from the list is appended visible when the list is
// normalised, so adding a view to the registry never needs a settings migration.
struct TabPreference {
    std::wstring id;
    bool visible = true;
};

struct AppearanceSettings {
    ThemePreset theme = ThemePreset::Graphite;

    // When false the active theme's built-in accent is used; when true `accent`
    // overrides it.
    bool customAccent = false;
    COLORREF accent = RGB(47, 226, 123);

    // When false the background follows the theme preset; when true the two
    // colours below are used (a vertical gradient when `background` is
    // Gradient).
    bool customBackground = false;
    BackgroundStyle background = BackgroundStyle::Solid;
    COLORREF backgroundTop = RGB(7, 12, 10);
    COLORREF backgroundBottom = RGB(7, 12, 10);

    // Optional picture painted behind the frame.  A missing or unreadable file
    // is ignored at runtime.
    std::wstring backgroundImage;

    DensityMode density = DensityMode::Comfortable;

    // A single emoji used as the product icon (tray, taskbar, shortcuts).  The
    // string is normalised to one grapheme cluster; empty means the bundled icon.
    std::wstring iconEmoji;

    std::vector<TabPreference> tabs;
};

const wchar_t* ThemePresetKey(ThemePreset preset);
const wchar_t* ThemePresetLabel(ThemePreset preset);
ThemePreset ThemePresetFromKey(const std::wstring& key);

const wchar_t* DensityKey(DensityMode density);
const wchar_t* DensityLabel(DensityMode density);
DensityMode DensityFromKey(const std::wstring& key);

const wchar_t* BackgroundStyleKey(BackgroundStyle style);
BackgroundStyle BackgroundStyleFromKey(const std::wstring& key);

const wchar_t* SelfDeleteModeKey(SelfDeleteMode mode);
const wchar_t* SelfDeleteModeLabel(SelfDeleteMode mode);
// Finds the mode for a settings-file key; unknown values map to Never.
SelfDeleteMode SelfDeleteModeFromKey(const std::wstring& key);

// Trims the text and keeps a single grapheme cluster: a base emoji, its
// variation selectors / skin-tone modifiers, regional-indicator pairs and ZWJ
// sequences such as a family emoji.  Returns an empty string when no visible
// character remains.
std::wstring NormalizeEmoji(const std::wstring& text);

// Fits the tab preferences to the known view ids: unknown ids are dropped,
// duplicates removed, missing ids appended visible and at least one tab left
// visible.  Called after loading and after the registry changes.
void NormalizeTabs(AppearanceSettings& appearance, const std::vector<std::wstring>& knownIds);

// A window identity that survives a process restart: the executable path and
// the window class are stable, while the HWND and PID are not.  A sticky target
// is re-attached automatically once a window with the same identity appears.
struct StickyTarget {
    std::wstring exePath;
    std::wstring className;
};

// True when an executable path and window class describe the sticky entry.
bool StickyMatches(const StickyTarget& target, const std::wstring& exePath,
                   const std::wstring& className);

struct Settings {
    int version = 1;
    UiSettings ui;
    AppearanceSettings appearance;
    HotkeyBinding hotkeys[kHotkeyCount];
    std::vector<AutoAttachRule> autoAttach;

    // The focused attachable window becomes the primary target while this is on.
    bool foregroundTracking = false;

    // Identities the daemon re-attaches whenever their window appears.  The
    // daemon is the only writer of this list (the user toggles it per target).
    std::vector<StickyTarget> sticky;

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
