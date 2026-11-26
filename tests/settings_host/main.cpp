// Console harness for the persistent launcher configuration (plan section 9.4).
//
// The settings module has no UI dependency, so everything here runs without an
// interactive desktop: defaults, JSON round-trip, corrupt-file recovery, hotkey
// parsing/validation and the auto-capture matching/anti-loop logic.

#include <windows.h>

#include <stdio.h>

#include <algorithm>
#include <string.h>

#include <string>
#include <vector>

#include "emoji_icon.h"
#include "self_delete.h"
#include "self_delete_plan.h"
#include "settings.h"
#include "shortcuts.h"

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
           left.lastTab == right.lastTab && left.selfDelete == right.selfDelete &&
           left.deleteOnExit == right.deleteOnExit;
}

bool SameTabs(const std::vector<gf::TabPreference>& left,
              const std::vector<gf::TabPreference>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].id != right[i].id || left[i].visible != right[i].visible) {
            return false;
        }
    }
    return true;
}

bool SameAppearance(const gf::AppearanceSettings& left, const gf::AppearanceSettings& right) {
    return left.theme == right.theme && left.customAccent == right.customAccent &&
           left.accent == right.accent && left.customBackground == right.customBackground &&
           left.background == right.background && left.backgroundTop == right.backgroundTop &&
           left.backgroundBottom == right.backgroundBottom &&
           left.backgroundImage == right.backgroundImage && left.density == right.density &&
           left.iconEmoji == right.iconEmoji && SameTabs(left.tabs, right.tabs);
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
    Check(settings.appearance.theme == gf::ThemePreset::Graphite &&
              settings.appearance.density == gf::DensityMode::Comfortable,
          L"the default appearance is Graphite and comfortable");
    Check(!settings.appearance.customAccent && !settings.appearance.customBackground,
          L"the default appearance follows the theme palette");
    Check(settings.appearance.iconEmoji.empty(), L"the bundled icon is the default");
    Check(settings.ui.selfDelete == gf::SelfDeleteMode::Never && !settings.ui.deleteOnExit,
          L"self-delete is off by default");

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
    original.ui.selfDelete = gf::SelfDeleteMode::Everything;
    original.ui.deleteOnExit = true;
    original.hotkeys[static_cast<int>(gf::HotkeyAction::Close)] =
        gf::HotkeyBinding{MOD_CONTROL | MOD_ALT, VK_F5};
    original.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", L"notepad.exe", true});
    original.autoAttach.push_back(gf::AutoAttachRule{false, L"title", L"Chrome", false});
    original.foregroundTracking = true;
    original.sticky.push_back(gf::StickyTarget{L"C:\\Apps\\notepad.exe", L"Notepad"});
    original.appearance.theme = gf::ThemePreset::Midnight;
    original.appearance.customAccent = true;
    original.appearance.accent = RGB(12, 200, 240);
    original.appearance.customBackground = true;
    original.appearance.background = gf::BackgroundStyle::Gradient;
    original.appearance.backgroundTop = RGB(8, 15, 30);
    original.appearance.backgroundBottom = RGB(14, 28, 54);
    original.appearance.backgroundImage = L"C:\\Pictures\\missing\\wall.png";
    original.appearance.density = gf::DensityMode::Compact;
    original.appearance.iconEmoji = L"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67";
    original.appearance.tabs.push_back(gf::TabPreference{L"windows", false});
    original.appearance.tabs.push_back(gf::TabPreference{L"targets", true});

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
    Check(parsed.foregroundTracking, L"foreground tracking survives the round-trip");
    Check(parsed.sticky.size() == 1 && parsed.sticky[0].exePath == L"C:\\Apps\\notepad.exe" &&
              parsed.sticky[0].className == L"Notepad",
          L"a sticky identity survives the round-trip");
    Check(gf::StickyMatches(parsed.sticky[0], L"c:\\apps\\NOTEPAD.exe", L"notepad"),
          L"sticky matching is case-insensitive");
    Check(!gf::StickyMatches(parsed.sticky[0], L"C:\\Apps\\other.exe", L"Notepad"),
          L"a different executable does not match a sticky identity");
    Check(SameAppearance(original.appearance, parsed.appearance),
          L"every appearance field survives the round-trip");
    Check(parsed.appearance.backgroundImage == L"C:\\Pictures\\missing\\wall.png",
          L"a background path is preserved even when the file is absent");

    // Unknown keys are ignored rather than treated as an error.
    const std::wstring withExtra =
        L"{ \"version\": 1, \"future\": { \"nested\": [1, 2, 3] }, \"ui\": { \"x\": 5 } }";
    gf::Settings extra;
    Check(gf::ParseSettings(withExtra, extra, error), L"unknown keys are ignored");
    Check(extra.ui.x == 5 && extra.ui.y == 100, L"known keys are still applied");

    // An unknown or missing mode normalises to Never, and Never clears
    // delete-on-exit so a hand-edited file cannot arm an accidental delete.
    gf::Settings unknownMode;
    Check(gf::ParseSettings(
              L"{ \"ui\": { \"selfDelete\": \"sometimes\", \"deleteOnExit\": true } }",
              unknownMode, error),
          L"an unknown self-delete mode is parsed");
    Check(unknownMode.ui.selfDelete == gf::SelfDeleteMode::Never && !unknownMode.ui.deleteOnExit,
          L"an unknown self-delete mode falls back to Never");
    Check(gf::SelfDeleteModeFromKey(L"exe") == gf::SelfDeleteMode::ExeOnly &&
              gf::SelfDeleteModeFromKey(L"everything") == gf::SelfDeleteMode::Everything &&
              gf::SelfDeleteModeFromKey(L"never") == gf::SelfDeleteMode::Never,
          L"the self-delete mode keys map both ways");
}

void TestAppearanceTabs() {
    gf::AppearanceSettings appearance;
    appearance.tabs.push_back(gf::TabPreference{L"windows", false});
    appearance.tabs.push_back(gf::TabPreference{L"unknown", true});
    appearance.tabs.push_back(gf::TabPreference{L"targets", true});
    appearance.tabs.push_back(gf::TabPreference{L"windows", true});

    gf::NormalizeTabs(appearance, {L"targets", L"windows", L"settings"});
    Check(appearance.tabs.size() == 3, L"normalising drops unknown and duplicate tabs");
    Check(appearance.tabs.size() == 3 && appearance.tabs[0].id == L"windows" &&
              !appearance.tabs[0].visible && appearance.tabs[1].id == L"targets" &&
              appearance.tabs[2].id == L"settings" && appearance.tabs[2].visible,
          L"normalising keeps the saved order and appends missing views visible");

    gf::AppearanceSettings allHidden;
    allHidden.tabs.push_back(gf::TabPreference{L"targets", false});
    allHidden.tabs.push_back(gf::TabPreference{L"windows", false});
    allHidden.tabs.push_back(gf::TabPreference{L"settings", false});
    gf::NormalizeTabs(allHidden, {L"targets", L"windows", L"settings"});
    Check(allHidden.tabs.size() == 3 && allHidden.tabs[0].visible,
          L"normalising never hides every tab");

    gf::AppearanceSettings empty;
    gf::NormalizeTabs(empty, {L"targets", L"windows", L"settings"});
    Check(empty.tabs.size() == 3 && empty.tabs[0].id == L"targets" && empty.tabs[0].visible,
          L"an empty list becomes the whole registry");
}

void TestEmojiNormalization() {
    const std::wstring cat = L"\xD83D\xDE3A";
    const std::wstring family =
        L"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67";
    const std::wstring flag = L"\xD83C\xDDFA\xD83C\xDDF8";

    Check(gf::NormalizeEmoji(L"   ").empty(), L"whitespace normalises to no emoji");
    Check(gf::NormalizeEmoji(L"  " + cat + L" \t\r\n") == cat,
          L"an emoji is trimmed to one cluster");
    Check(gf::NormalizeEmoji(cat + L"hello") == cat,
          L"text after the emoji is dropped");
    Check(gf::NormalizeEmoji(family) == family,
          L"a ZWJ family sequence survives intact");
    Check(gf::NormalizeEmoji(flag) == flag, L"a regional-indicator flag survives intact");
    Check(gf::NormalizeEmoji(flag + flag) == flag, L"a second flag is dropped");
}

void TestEmojiIconBuild() {
    const std::wstring path = TempFile(L"emoji-test.ico");
    ::DeleteFileW(path.c_str());

    std::wstring error;
    const bool rendered = gf::RenderEmojiIcon(L"\xD83D\xDE3A", path, error);
    Check(rendered, L"the emoji icon renders headlessly");
    if (rendered) {
        Check(::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
              L"the emoji icon file is written");
        Check(gf::IsValidIcoFile(path, error), L"the generated icon has a valid ICONDIR");
        Check(gf::CountIcoImages(path) == 7, L"the icon contains all seven sizes");
    }

    const std::wstring missing = TempFile(L"missing-icon.ico");
    ::DeleteFileW(missing.c_str());
    Check(!gf::IsValidIcoFile(missing, error), L"a missing icon file is rejected cleanly");

    bool owned = false;
    HICON icon = gf::LoadConfiguredIcon(nullptr, 0, L"\xD83D\xDE3A", 32, owned);
    Check(icon != nullptr, L"a configured icon falls back to a system icon");
    if (icon != nullptr && owned) {
        ::DestroyIcon(icon);
    }
    ::DeleteFileW(path.c_str());
}

void TestShortcuts() {
    gf::ShortcutEntry entry;
    entry.path = L"C:\\Users\\Test\\Desktop\\GhostFrame.lnk";
    entry.kind = gf::ShortcutKindKey(gf::ShortcutKind::Ui);
    entry.name = L"GhostFrame";
    entry.target = L"C:\\Apps\\GhostFrame.exe";
    entry.arguments = L"";
    entry.created = L"2026-11-26T12:00:00";

    const std::wstring json = gf::SerializeShortcutManifest({entry});
    std::vector<gf::ShortcutEntry> parsed;
    std::wstring error;
    Check(gf::ParseShortcutManifest(json, parsed, error),
          L"the shortcut manifest JSON parses back");
    Check(parsed.size() == 1 && parsed[0].path == entry.path && parsed[0].kind == entry.kind &&
              parsed[0].name == entry.name && parsed[0].target == entry.target &&
              parsed[0].arguments == entry.arguments && parsed[0].created == entry.created,
          L"every manifest field survives the round-trip");

    Check(gf::ShortcutFileStem(L"a/b:c*?") == L"a_b_c__",
          L"invalid file-name characters are replaced");
    Check(gf::ShortcutFileStem(L"   ") == L"GhostFrame",
          L"an empty file stem falls back to GhostFrame");
    Check(gf::ShortcutWrapperArguments(L"C:\\Apps\\game.exe", L"--capture") ==
              L"--launch \"C:\\Apps\\game.exe\" --capture",
          L"the wrapper arguments are built correctly");

    for (int i = 0; i < static_cast<int>(gf::ShortcutLocation::Count); ++i) {
        Check(!gf::ShortcutLocationPath(static_cast<gf::ShortcutLocation>(i)).empty(),
              L"every shortcut destination resolves");
    }

    const std::wstring linkPath = TempFile(L"gf-shortcut-test.lnk");
    ::DeleteFileW(linkPath.c_str());
    gf::ShortcutSpec spec;
    spec.kind = gf::ShortcutKind::Ui;
    spec.name = L"GfSettingsHost test";
    spec.description = L"Created by the settings harness";
    gf::ShortcutEntry created;
    const bool ok = gf::CreateShortcutAt(linkPath, spec, gf::ExecutablePath(),
                                         gf::ExecutablePath(), created, error);
    Check(ok, L"a .lnk file is created with IShellLinkW");
    Check(ok && gf::ShortcutExists(linkPath), L"the created .lnk exists on disk");
    Check(ok && created.path == linkPath && created.name == spec.name,
          L"the created entry describes the link");
    std::wstring deleteError;
    Check(gf::DeleteShortcutFile(linkPath, deleteError), L"the test link is removed");
    Check(!gf::ShortcutExists(linkPath), L"the removed .lnk is gone");
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

    gf::Settings optional = gf::DefaultSettings();
    Check(optional.hotkeys[static_cast<int>(gf::HotkeyAction::SelfDestruct)].empty(),
          L"the self-destruct hotkey starts unset");
    Check(gf::ValidateHotkeys(optional, error),
          L"an unset self-destruct hotkey is valid");
    optional.hotkeys[static_cast<int>(gf::HotkeyAction::SelfDestruct)] =
        gf::HotkeyBinding{MOD_CONTROL | MOD_SHIFT, VK_F12};
    Check(gf::ValidateHotkeys(optional, error),
          L"a rebound self-destruct hotkey is valid");
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

void TestSelfDeletePlan() {
    gf::SelfDeletePaths paths;
    paths.executable = L"C:\\Apps\\GhostFrame\\bin\\GhostFrame.exe";
    paths.dataDirectory = L"C:\\Users\\Test\\AppData\\Local\\GhostFrame";
    paths.installDirectory = L"C:\\Apps\\GhostFrame\\bin";
    paths.generatedIcon = paths.dataDirectory + L"\\GhostFrame.ico";

    const gf::SelfDeletePlan never = gf::BuildSelfDeletePlan(gf::SelfDeleteMode::Never, paths);
    Check(!never.removeShortcuts && !never.removeAutostart && never.files.empty() &&
              never.directories.empty() && never.executable.empty(),
          L"Never plans nothing");

    const gf::SelfDeletePlan exe = gf::BuildSelfDeletePlan(gf::SelfDeleteMode::ExeOnly, paths);
    Check(exe.removeShortcuts && exe.removeAutostart,
          L"ExeOnly removes shortcuts and the startup entry");
    Check(exe.executable == paths.executable, L"ExeOnly removes the executable last");
    Check(std::find(exe.files.begin(), exe.files.end(), paths.generatedIcon) != exe.files.end(),
          L"ExeOnly removes the generated icon");
    Check(std::find(exe.files.begin(), exe.files.end(),
                    paths.dataDirectory + L"\\settings.json") == exe.files.end(),
          L"ExeOnly keeps settings.json");
    Check(exe.directories.empty(), L"ExeOnly keeps the data directory");

    const gf::SelfDeletePlan all =
        gf::BuildSelfDeletePlan(gf::SelfDeleteMode::Everything, paths);
    Check(all.removeShortcuts && all.removeAutostart,
          L"Everything removes shortcuts and the startup entry");
    Check(std::find(all.directories.begin(), all.directories.end(), paths.dataDirectory) !=
              all.directories.end(),
          L"Everything removes the whole data directory");
    Check(std::find(all.emptyDirectories.begin(), all.emptyDirectories.end(),
                    paths.installDirectory) != all.emptyDirectories.end(),
          L"Everything removes the install directory when it is empty");
    Check(!gf::IsSafeDataDirectory(L"C:\\"), L"a drive root is not a safe data directory");
    Check(!gf::IsSafeDataDirectory(L"C:\\Users\\Test\\AppData\\Local"),
          L"only a path ending in GhostFrame is safe");
    Check(gf::IsSafeDataDirectory(paths.dataDirectory), L"the normal data directory is safe");
}

void TestDeleteRetry() {
    const std::wstring path = TempFile(L"held-file.txt");
    RemoveFile(path);

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, L"the held test file is created");
    Check(!gf::DeleteFileWithRetry(path, 250),
          L"a file held by another handle is not deleted immediately");
    if (file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file);
    }
    Check(gf::DeleteFileWithRetry(path, 2000),
          L"the file is deleted once the handle is released");
    Check(gf::DeleteFileWithRetry(path, 100), L"a missing file counts as removed");
}

}  // namespace

int wmain() {
    TestDefaults();
    TestRoundTrip();
    TestAppearanceTabs();
    TestEmojiNormalization();
    TestEmojiIconBuild();
    TestShortcuts();
    TestCorruptFile();
    TestSaveLoad();
    TestHotkeyParsing();
    TestHotkeyValidation();
    TestMatching();
    TestGuard();
    TestSelfDeletePlan();
    TestDeleteRetry();

    if (g_failures == 0) {
        wprintf(L"PASS\n");
        return 0;
    }
    wprintf(L"FAILED: %d\n", g_failures);
    return 1;
}
