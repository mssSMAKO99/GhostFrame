// Console harness for the persistent launcher configuration (plan section 9.4).
//
// The settings module has no UI dependency, so everything here runs without an
// interactive desktop: defaults, JSON round-trip, corrupt-file recovery, hotkey
// parsing/validation and the auto-capture matching/anti-loop logic.

#include <windows.h>

#include <stdio.h>

#include <string.h>

#include <string>
#include <vector>

#include "settings.h"

namespace {

int g_failures = 0;

void Check(bool condition, const wchar_t* what) {
    wprintf(condition ? L"[ok  ] %s\n" : L"[FAIL] %s\n", what);
    if (!condition) {
        ++g_failures;
    }
}

std::wstring EnsureWritableDir() {
    std::vector<std::wstring> candidates;
    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) != 0) {
        candidates.push_back(std::wstring(temp) + L"opencode\\GfSettingsHost");
        candidates.push_back(std::wstring(temp) + L"GfSettingsHost");
    }
    wchar_t cwd[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd) != 0) {
        candidates.push_back(std::wstring(cwd) + L"\\GfSettingsHost.test");
    }
    for (const std::wstring& dir : candidates) {
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring probe = dir + L"\\.probe";
        HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file);
            ::DeleteFileW(probe.c_str());
            return dir;
        }
    }
    return std::wstring();
}

std::wstring TempFile(const wchar_t* name) {
    static const std::wstring dir = EnsureWritableDir();
    return dir + L"\\" + name;
}

void RemoveFile(const std::wstring& path) {
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L".bad").c_str());
    ::DeleteFileW((path + L".tmp").c_str());
}

bool SameUi(const gf::UiSettings& left, const gf::UiSettings& right) {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height && left.minimizeToTray == right.minimizeToTray &&
           left.noSelfExclude == right.noSelfExclude && left.hideOnAttach == right.hideOnAttach &&
           left.lastTab == right.lastTab;
}

void TestDefaults() {
    const std::wstring path = TempFile(L"defaults.json");
    RemoveFile(path);

    gf::Settings settings;
    std::wstring error;
    const bool ok = gf::LoadSettings(path, settings, error);
    Check(ok, L"a missing settings file loads with defaults");
    Check(!settings.badFile, L"a missing file is not flagged as corrupt");
    Check(settings.version == 1, L"the default version is 1");
    Check(settings.ui.width == 760 && settings.ui.height == 560,
          L"the default window size matches the design");
    Check(settings.ui.hideOnAttach, L"hide-on-attach is on by default");
    Check(settings.autoAttach.empty(), L"there are no default auto-capture rules");

    const gf::HotkeyBinding toggle =
        settings.hotkeys[static_cast<int>(gf::HotkeyAction::ToggleSelection)];
    Check(toggle.virtualKey == 'H' && (toggle.modifiers & MOD_CONTROL) != 0 &&
              (toggle.modifiers & MOD_SHIFT) != 0,
          L"the default toggle hotkey is Ctrl+Shift+H");
    Check(gf::ValidateHotkeys(settings, error), L"the defaults pass hotkey validation");
}

void TestRoundTrip() {
    gf::Settings original = gf::DefaultSettings();
    original.version = 1;
    original.ui.x = 321;
    original.ui.y = 654;
    original.ui.width = 900;
    original.ui.height = 640;
    original.ui.minimizeToTray = false;
    original.ui.noSelfExclude = true;
    original.ui.hideOnAttach = false;
    original.ui.lastTab = L"windows";
    original.hotkeys[static_cast<int>(gf::HotkeyAction::Close)] =
        gf::HotkeyBinding{MOD_CONTROL | MOD_ALT, VK_F5};
    original.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", L"notepad.exe", true});
    original.autoAttach.push_back(gf::AutoAttachRule{false, L"title", L"Chrome", false});

    const std::wstring json = gf::SerializeSettings(original);

    gf::Settings parsed;
    std::wstring error;
    Check(gf::ParseSettings(json, parsed, error), L"the serialised settings parse back");
    Check(SameUi(original.ui, parsed.ui), L"every UI field survives the round-trip");
    Check(parsed.hotkeys[static_cast<int>(gf::HotkeyAction::Close)].virtualKey == VK_F5 &&
              parsed.hotkeys[static_cast<int>(gf::HotkeyAction::Close)].modifiers ==
                  (MOD_CONTROL | MOD_ALT),
          L"a custom hotkey survives the round-trip");
    Check(parsed.autoAttach.size() == 2, L"both auto-capture rules survive");
    Check(parsed.autoAttach.size() == 2 && parsed.autoAttach[0].value == L"notepad.exe" &&
              parsed.autoAttach[0].hide && parsed.autoAttach[1].match == L"title" &&
              !parsed.autoAttach[1].enabled && !parsed.autoAttach[1].hide,
          L"the auto-capture rule fields survive");

    // Unknown keys are ignored rather than treated as an error.
    const std::wstring withExtra =
        L"{ \"version\": 1, \"future\": { \"nested\": [1, 2, 3] }, \"ui\": { \"x\": 5 } }";
    gf::Settings extra;
    Check(gf::ParseSettings(withExtra, extra, error), L"unknown keys are ignored");
    Check(extra.ui.x == 5 && extra.ui.y == 100, L"known keys are still applied");
}

void TestCorruptFile() {
    const std::wstring path = TempFile(L"corrupt.json");
    RemoveFile(path);

    const char* broken = "{ this is not json ";
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, L"the corrupt test file is created");
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(file, broken, static_cast<DWORD>(strlen(broken)), &written, nullptr);
        ::CloseHandle(file);
    }

    gf::Settings settings;
    std::wstring error;
    const bool ok = gf::LoadSettings(path, settings, error);
    Check(ok, L"a corrupt file still yields usable settings");
    Check(settings.badFile, L"the corrupt file is flagged");
    Check(settings.ui.width == 760, L"the defaults are used after a corrupt file");
    Check(::GetFileAttributesW((path + L".bad").c_str()) != INVALID_FILE_ATTRIBUTES,
          L"the corrupt file is renamed to .bad");
    Check(::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"the corrupt file is gone after the rename");
    RemoveFile(path);
}

void TestSaveLoad() {
    const std::wstring path = TempFile(L"save.json");
    RemoveFile(path);

    gf::Settings original = gf::DefaultSettings();
    original.ui.lastTab = L"settings";
    original.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", L"mspaint.exe", true});

    std::wstring error;
    Check(gf::SaveSettings(path, original, error), L"the settings file is written");
    Check(::GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
          L"the temporary file is gone after an atomic save");

    gf::Settings loaded;
    Check(gf::LoadSettings(path, loaded, error), L"the written file loads");
    Check(loaded.ui.lastTab == L"settings" && loaded.autoAttach.size() == 1 &&
              loaded.autoAttach[0].value == L"mspaint.exe",
          L"the saved values come back");
    RemoveFile(path);
}

void TestHotkeyParsing() {
    UINT mods = 0;
    UINT key = 0;

    Check(gf::ParseHotkey(L"Ctrl+Shift+H", mods, key) && key == 'H' &&
              mods == (MOD_CONTROL | MOD_SHIFT),
          L"Ctrl+Shift+H parses");
    Check(gf::ParseHotkey(L"ctrl+alt+f5", mods, key) && key == VK_F5 &&
              mods == (MOD_CONTROL | MOD_ALT),
          L"parsing is case-insensitive and understands F-keys");
    Check(gf::ParseHotkey(L"Ctrl+Shift+Down", mods, key) && key == VK_DOWN,
          L"named keys parse");
    Check(!gf::ParseHotkey(L"Ctrl+H+Shift", mods, key), L"a key that is not last is rejected");
    Check(!gf::ParseHotkey(L"Ctrl+Nonsense", mods, key), L"an unknown key is rejected");
    Check(!gf::ParseHotkey(L"", mods, key), L"an empty combination is rejected");

    Check(gf::FormatHotkey(MOD_CONTROL | MOD_SHIFT, 'H') == L"Ctrl+Shift+H",
          L"formatting uses the canonical order");
    Check(gf::FormatHotkey(MOD_CONTROL | MOD_ALT, VK_F5) == L"Ctrl+Alt+F5",
          L"F-keys format correctly");
    Check(gf::FormatHotkey(MOD_CONTROL | MOD_SHIFT, VK_DOWN) == L"Ctrl+Shift+Down",
          L"named keys format correctly");
}

void TestHotkeyValidation() {
    std::wstring error;

    gf::Settings ok = gf::DefaultSettings();
    Check(gf::ValidateHotkeys(ok, error), L"the default set is valid");

    gf::Settings duplicate = gf::DefaultSettings();
    duplicate.hotkeys[static_cast<int>(gf::HotkeyAction::Close)] =
        duplicate.hotkeys[static_cast<int>(gf::HotkeyAction::ToggleSelection)];
    Check(!gf::ValidateHotkeys(duplicate, error), L"a duplicate combination is rejected");

    gf::Settings empty = gf::DefaultSettings();
    empty.hotkeys[static_cast<int>(gf::HotkeyAction::Minimize)] = gf::HotkeyBinding{};
    Check(!gf::ValidateHotkeys(empty, error), L"an empty combination is rejected");

    gf::Settings bare = gf::DefaultSettings();
    bare.hotkeys[static_cast<int>(gf::HotkeyAction::Restore)] =
        gf::HotkeyBinding{0, 'K'};
    Check(!gf::ValidateHotkeys(bare, error), L"a modifier-less combination is rejected");
}

void TestMatching() {
    const gf::AutoAttachRule byExe{true, L"exe", L"notepad.exe", true};
    Check(gf::RuleMatches(byExe, L"notepad.exe", L"Untitled - Notepad"),
          L"an exe rule matches exactly");
    Check(gf::RuleMatches(byExe, L"NOTEPAD.EXE", L"anything"),
          L"exe matching ignores case");
    Check(!gf::RuleMatches(byExe, L"notepad++.exe", L"anything"),
          L"an exe rule does not match a different executable");

    const gf::AutoAttachRule byTitle{true, L"title", L"chrome", true};
    Check(gf::RuleMatches(byTitle, L"chrome.exe", L"New Tab - Google Chrome"),
          L"a title rule matches a substring, ignoring case");
    Check(!gf::RuleMatches(byTitle, L"chrome.exe", L"Firefox"), L"a title rule rejects others");

    gf::AutoAttachRule disabled = byExe;
    disabled.enabled = false;
    Check(!gf::RuleMatches(disabled, L"notepad.exe", L"Notepad"),
          L"a disabled rule never matches");

    const gf::AutoAttachRule emptyValue{true, L"exe", L"", true};
    Check(!gf::RuleMatches(emptyValue, L"notepad.exe", L"Notepad"),
          L"a rule without a value never matches");
}

void TestGuard() {
    gf::AutoCaptureGuard guard;
    const DWORD pid = 42;
    const ULONGLONG created = 1000;
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1234));

    Check(guard.Allow(pid, created, hwnd), L"a fresh window is allowed");
    guard.Suppress(pid, created, hwnd);
    Check(!guard.Allow(pid, created, hwnd), L"a manually removed window stays suppressed");
    Check(guard.Allow(pid, created + 1, hwnd),
          L"a recreated window (new create time) is allowed again");

    const HWND other = reinterpret_cast<HWND>(static_cast<INT_PTR>(0x5678));
    Check(guard.Allow(pid, created, other), L"another window of the same process is allowed");
    guard.NoteFailure(pid, created, other);
    guard.NoteFailure(pid, created, other);
    Check(guard.Allow(pid, created, other), L"the window is still allowed before the limit");
    guard.NoteFailure(pid, created, other);
    Check(!guard.Allow(pid, created, other),
          L"the window is suppressed after the attempt limit");

    guard.Clear();
    Check(guard.Allow(pid, created, hwnd), L"clearing the guard forgets everything");
}

}  // namespace

int wmain() {
    TestDefaults();
    TestRoundTrip();
    TestCorruptFile();
    TestSaveLoad();
    TestHotkeyParsing();
    TestHotkeyValidation();
    TestMatching();
    TestGuard();

    if (g_failures == 0) {
        wprintf(L"PASS\n");
        return 0;
    }
    wprintf(L"FAILED: %d\n", g_failures);
    return 1;
}
