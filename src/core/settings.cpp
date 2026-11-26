#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <cstdint>
#include <utility>

#include "json.h"

namespace gf {

namespace {

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::wstring ToLowerCopy(const std::wstring& text) {
    std::wstring result = text;
    for (wchar_t& ch : result) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return result;
}

std::wstring Trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r' ||
                           text[begin] == L'\n')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r' ||
            text[end - 1] == L'\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) {
        return false;
    }
    return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::wstring::npos;
}

bool SameModifiers(UINT left, UINT right) {
    constexpr UINT kMask = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    return (left & kMask) == (right & kMask);
}

// Kept as a local name so the settings writer reads naturally; the
// implementation is shared with the shortcut manifest.
void AppendEscaped(std::wstring& out, const std::wstring& text) {
    AppendJsonString(out, text);
}

void AppendBool(std::wstring& out, bool value) { out += value ? L"true" : L"false"; }

void AppendInt(std::wstring& out, int value) {
    wchar_t buffer[24] = {};
    swprintf_s(buffer, L"%d", value);
    out += buffer;
}

// ---------------------------------------------------------------------------
// Mapping between JSON and Settings
// ---------------------------------------------------------------------------

bool ReadBool(const JsonValue* value, bool fallback) {
    if (value != nullptr && value->type == JsonValue::Type::Bool) {
        return value->boolean;
    }
    return fallback;
}

int ReadInt(const JsonValue* value, int fallback) {
    if (value != nullptr && value->type == JsonValue::Type::Number) {
        return static_cast<int>(value->number);
    }
    return fallback;
}

std::wstring ReadString(const JsonValue* value, const std::wstring& fallback) {
    if (value != nullptr && value->type == JsonValue::Type::String) {
        return value->text;
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Colour helpers.  The file stores #RRGGBB; COLORREF is 0x00BBGGRR.
// ---------------------------------------------------------------------------

std::wstring FormatColor(COLORREF color) {
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"#%02X%02X%02X", static_cast<unsigned>(GetRValue(color)),
               static_cast<unsigned>(GetGValue(color)), static_cast<unsigned>(GetBValue(color)));
    return buffer;
}

int HexDigit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') {
        return ch - L'0';
    }
    if (ch >= L'a' && ch <= L'f') {
        return ch - L'a' + 10;
    }
    if (ch >= L'A' && ch <= L'F') {
        return ch - L'A' + 10;
    }
    return -1;
}

bool ParseColor(const std::wstring& text, COLORREF& out) {
    if (text.size() != 7 || text[0] != L'#') {
        return false;
    }
    int digits[6] = {};
    for (int i = 0; i < 6; ++i) {
        digits[i] = HexDigit(text[static_cast<size_t>(i) + 1]);
        if (digits[i] < 0) {
            return false;
        }
    }
    const int red = digits[0] * 16 + digits[1];
    const int green = digits[2] * 16 + digits[3];
    const int blue = digits[4] * 16 + digits[5];
    out = RGB(red, green, blue);
    return true;
}

COLORREF ReadColor(const JsonValue* value, COLORREF fallback) {
    if (value != nullptr && value->type == JsonValue::Type::String) {
        COLORREF parsed = fallback;
        if (ParseColor(value->text, parsed)) {
            return parsed;
        }
    }
    return fallback;
}

void ParseAppearance(const JsonValue& root, AppearanceSettings& appearance) {
    const JsonValue* object = root.Get(L"appearance");
    if (object == nullptr || object->type != JsonValue::Type::Object) {
        return;
    }
    appearance.theme = ThemePresetFromKey(ReadString(object->Get(L"theme"), L"graphite"));
    if (appearance.theme == ThemePreset::Count) {
        appearance.theme = ThemePreset::Graphite;
    }
    appearance.customAccent = ReadBool(object->Get(L"customAccent"), appearance.customAccent);
    appearance.accent = ReadColor(object->Get(L"accent"), appearance.accent);
    appearance.customBackground =
        ReadBool(object->Get(L"customBackground"), appearance.customBackground);
    appearance.background =
        BackgroundStyleFromKey(ReadString(object->Get(L"background"), L"solid"));
    if (appearance.background == BackgroundStyle::Count) {
        appearance.background = BackgroundStyle::Solid;
    }
    appearance.backgroundTop = ReadColor(object->Get(L"backgroundTop"), appearance.backgroundTop);
    appearance.backgroundBottom =
        ReadColor(object->Get(L"backgroundBottom"), appearance.backgroundBottom);
    appearance.backgroundImage =
        ReadString(object->Get(L"backgroundImage"), appearance.backgroundImage);
    appearance.density = DensityFromKey(ReadString(object->Get(L"density"), L"comfortable"));
    if (appearance.density == DensityMode::Count) {
        appearance.density = DensityMode::Comfortable;
    }
    appearance.iconEmoji =
        NormalizeEmoji(ReadString(object->Get(L"iconEmoji"), appearance.iconEmoji));

    const JsonValue* tabs = object->Get(L"tabs");
    if (tabs != nullptr && tabs->type == JsonValue::Type::Array) {
        appearance.tabs.clear();
        for (const JsonValue& item : tabs->array) {
            if (item.type != JsonValue::Type::Object) {
                continue;
            }
            TabPreference preference;
            preference.id = ReadString(item.Get(L"id"), L"");
            preference.visible = ReadBool(item.Get(L"visible"), true);
            if (!preference.id.empty()) {
                appearance.tabs.push_back(std::move(preference));
            }
        }
    }
}

void ParseUi(const JsonValue& root, UiSettings& ui) {
    const JsonValue* object = root.Get(L"ui");
    if (object == nullptr || object->type != JsonValue::Type::Object) {
        return;
    }
    ui.x = ReadInt(object->Get(L"x"), ui.x);
    ui.y = ReadInt(object->Get(L"y"), ui.y);
    ui.width = ReadInt(object->Get(L"width"), ui.width);
    ui.height = ReadInt(object->Get(L"height"), ui.height);
    ui.minimizeToTray = ReadBool(object->Get(L"minimizeToTray"), ui.minimizeToTray);
    ui.noSelfExclude = ReadBool(object->Get(L"noSelfExclude"), ui.noSelfExclude);
    ui.hideOnAttach = ReadBool(object->Get(L"hideOnAttach"), ui.hideOnAttach);
    ui.lastTab = ReadString(object->Get(L"lastTab"), ui.lastTab);
    ui.selfDelete = SelfDeleteModeFromKey(ReadString(object->Get(L"selfDelete"), L"never"));
    if (ui.selfDelete == SelfDeleteMode::Count) {
        ui.selfDelete = SelfDeleteMode::Never;
    }
    ui.deleteOnExit = ReadBool(object->Get(L"deleteOnExit"), ui.deleteOnExit);
    if (ui.selfDelete == SelfDeleteMode::Never) {
        ui.deleteOnExit = false;
    }
}

void ParseHotkeys(const JsonValue& root, Settings& out) {
    const JsonValue* object = root.Get(L"hotkeys");
    if (object == nullptr || object->type != JsonValue::Type::Object) {
        return;
    }
    for (const auto& entry : object->object) {
        const HotkeyAction action = HotkeyActionFromKey(entry.first);
        if (action == HotkeyAction::Count) {
            continue;  // unknown action: ignore
        }
        if (entry.second.type != JsonValue::Type::String) {
            continue;
        }
        UINT modifiers = 0;
        UINT virtualKey = 0;
        if (ParseHotkey(entry.second.text, modifiers, virtualKey)) {
            out.hotkeys[static_cast<int>(action)] = HotkeyBinding{modifiers, virtualKey};
        }
    }
}

void ParseSticky(const JsonValue& root, Settings& out) {
    const JsonValue* array = root.Get(L"sticky");
    if (array == nullptr || array->type != JsonValue::Type::Array) {
        return;
    }
    out.sticky.clear();
    for (const JsonValue& item : array->array) {
        if (item.type != JsonValue::Type::Object) {
            continue;
        }
        StickyTarget target;
        target.exePath = ReadString(item.Get(L"exe"), target.exePath);
        target.className = ReadString(item.Get(L"class"), target.className);
        if (target.exePath.empty()) {
            continue;  // an identity without an executable can never match
        }
        out.sticky.push_back(std::move(target));
    }
}

void ParseAutoAttach(const JsonValue& root, Settings& out) {
    const JsonValue* array = root.Get(L"autoAttach");
    if (array == nullptr || array->type != JsonValue::Type::Array) {
        return;
    }
    out.autoAttach.clear();
    for (const JsonValue& item : array->array) {
        if (item.type != JsonValue::Type::Object) {
            continue;
        }
        AutoAttachRule rule;
        rule.enabled = ReadBool(item.Get(L"enabled"), rule.enabled);
        rule.match = ReadString(item.Get(L"match"), rule.match);
        rule.value = ReadString(item.Get(L"value"), rule.value);
        rule.hide = ReadBool(item.Get(L"hide"), rule.hide);
        if (rule.value.empty()) {
            continue;  // a rule without a value can never match
        }
        out.autoAttach.push_back(std::move(rule));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Paths and defaults
// ---------------------------------------------------------------------------

std::wstring SettingsDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH) != 0) {
        return std::wstring(buffer) + L"\\GhostFrame";
    }
    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) != 0) {
        return std::wstring(temp) + L"GhostFrame";
    }
    return L"GhostFrame";
}

std::wstring SettingsPath() { return SettingsDirectory() + L"\\settings.json"; }

std::wstring SettingsBadPath() { return SettingsDirectory() + L"\\settings.json.bad"; }

Settings DefaultSettings() {
    Settings settings;
    settings.hotkeys[static_cast<int>(HotkeyAction::ToggleSelection)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, 'H'};
    settings.hotkeys[static_cast<int>(HotkeyAction::RestoreAll)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, 'R'};
    settings.hotkeys[static_cast<int>(HotkeyAction::ToggleUi)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, 'M'};
    settings.hotkeys[static_cast<int>(HotkeyAction::Minimize)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, VK_DOWN};
    settings.hotkeys[static_cast<int>(HotkeyAction::Restore)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, VK_UP};
    settings.hotkeys[static_cast<int>(HotkeyAction::Close)] =
        HotkeyBinding{MOD_CONTROL | MOD_SHIFT, 'Q'};
    return settings;
}

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------

std::wstring SerializeSettings(const Settings& settings) {
    std::wstring out;
    out += L"{\n";
    out += L"  \"version\": ";
    AppendInt(out, settings.version);
    out += L",\n";
    out += L"  \"foregroundTracking\": ";
    AppendBool(out, settings.foregroundTracking);
    out += L",\n";

    out += L"  \"ui\": {\n";
    out += L"    \"x\": ";
    AppendInt(out, settings.ui.x);
    out += L", \"y\": ";
    AppendInt(out, settings.ui.y);
    out += L", \"width\": ";
    AppendInt(out, settings.ui.width);
    out += L", \"height\": ";
    AppendInt(out, settings.ui.height);
    out += L",\n";
    out += L"    \"minimizeToTray\": ";
    AppendBool(out, settings.ui.minimizeToTray);
    out += L", \"noSelfExclude\": ";
    AppendBool(out, settings.ui.noSelfExclude);
    out += L", \"hideOnAttach\": ";
    AppendBool(out, settings.ui.hideOnAttach);
    out += L",\n";
    out += L"    \"lastTab\": ";
    AppendEscaped(out, settings.ui.lastTab);
    out += L",\n";
    out += L"    \"selfDelete\": ";
    AppendEscaped(out, SelfDeleteModeKey(settings.ui.selfDelete));
    out += L", \"deleteOnExit\": ";
    AppendBool(out, settings.ui.deleteOnExit);
    out += L"\n  },\n";

    out += L"  \"appearance\": {\n";
    out += L"    \"theme\": ";
    AppendEscaped(out, ThemePresetKey(settings.appearance.theme));
    out += L", \"customAccent\": ";
    AppendBool(out, settings.appearance.customAccent);
    out += L", \"accent\": ";
    AppendEscaped(out, FormatColor(settings.appearance.accent));
    out += L",\n";
    out += L"    \"customBackground\": ";
    AppendBool(out, settings.appearance.customBackground);
    out += L", \"background\": ";
    AppendEscaped(out, BackgroundStyleKey(settings.appearance.background));
    out += L", \"backgroundTop\": ";
    AppendEscaped(out, FormatColor(settings.appearance.backgroundTop));
    out += L",\n";
    out += L"    \"backgroundBottom\": ";
    AppendEscaped(out, FormatColor(settings.appearance.backgroundBottom));
    out += L", \"backgroundImage\": ";
    AppendEscaped(out, settings.appearance.backgroundImage);
    out += L",\n";
    out += L"    \"density\": ";
    AppendEscaped(out, DensityKey(settings.appearance.density));
    out += L", \"iconEmoji\": ";
    AppendEscaped(out, settings.appearance.iconEmoji);
    out += L",\n";
    out += L"    \"tabs\": [";
    if (settings.appearance.tabs.empty()) {
        out += L"]\n";
    } else {
        out += L"\n";
        for (size_t i = 0; i < settings.appearance.tabs.size(); ++i) {
            const TabPreference& tab = settings.appearance.tabs[i];
            out += L"      { \"id\": ";
            AppendEscaped(out, tab.id);
            out += L", \"visible\": ";
            AppendBool(out, tab.visible);
            out += L" }";
            out += (i + 1 < settings.appearance.tabs.size()) ? L",\n" : L"\n";
        }
        out += L"    ]\n";
    }
    out += L"  },\n";

    out += L"  \"hotkeys\": {\n";
    for (int i = 0; i < kHotkeyCount; ++i) {
        out += L"    ";
        AppendEscaped(out, HotkeyActionKey(static_cast<HotkeyAction>(i)));
        out += L": ";
        AppendEscaped(out, FormatHotkey(settings.hotkeys[i].modifiers,
                                        settings.hotkeys[i].virtualKey));
        out += (i + 1 < kHotkeyCount) ? L",\n" : L"\n";
    }
    out += L"  },\n";

    out += L"  \"autoAttach\": [";
    if (settings.autoAttach.empty()) {
        out += L"],\n";
    } else {
        out += L"\n";
        for (size_t i = 0; i < settings.autoAttach.size(); ++i) {
            const AutoAttachRule& rule = settings.autoAttach[i];
            out += L"    { \"enabled\": ";
            AppendBool(out, rule.enabled);
            out += L", \"match\": ";
            AppendEscaped(out, rule.match);
            out += L", \"value\": ";
            AppendEscaped(out, rule.value);
            out += L", \"hide\": ";
            AppendBool(out, rule.hide);
            out += L" }";
            out += (i + 1 < settings.autoAttach.size()) ? L",\n" : L"\n";
        }
        out += L"  ],\n";
    }

    out += L"  \"sticky\": [";
    if (settings.sticky.empty()) {
        out += L"]\n";
    } else {
        out += L"\n";
        for (size_t i = 0; i < settings.sticky.size(); ++i) {
            const StickyTarget& target = settings.sticky[i];
            out += L"    { \"exe\": ";
            AppendEscaped(out, target.exePath);
            out += L", \"class\": ";
            AppendEscaped(out, target.className);
            out += L" }";
            out += (i + 1 < settings.sticky.size()) ? L",\n" : L"\n";
        }
        out += L"  ]\n";
    }

    out += L"}\n";
    return out;
}

bool ParseSettings(const std::wstring& json, Settings& out, std::wstring& error) {
    JsonValue root;
    if (!ParseJson(json, root, error)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = L"the settings root must be an object";
        return false;
    }

    Settings parsed = DefaultSettings();
    parsed.version = ReadInt(root.Get(L"version"), parsed.version);
    parsed.foregroundTracking =
        ReadBool(root.Get(L"foregroundTracking"), parsed.foregroundTracking);
    ParseUi(root, parsed.ui);
    ParseAppearance(root, parsed.appearance);
    ParseHotkeys(root, parsed);
    ParseAutoAttach(root, parsed);
    ParseSticky(root, parsed);
    parsed.badFile = false;

    out = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

bool LoadSettings(const std::wstring& path, Settings& out, std::wstring& error) {
    out = DefaultSettings();
    error.clear();

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD last = ::GetLastError();
        if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
            return true;  // first run: defaults, nothing to report
        }
        error = L"cannot open the settings file";
        return false;
    }

    std::string bytes;
    char buffer[4096] = {};
    DWORD read = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        bytes.append(buffer, read);
    }
    ::CloseHandle(file);

    // The file is UTF-8.  Convert to UTF-16 and drop a possible BOM.
    std::wstring text;
    if (!bytes.empty()) {
        const char* begin = bytes.data();
        int count = static_cast<int>(bytes.size());
        if (count >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
            static_cast<unsigned char>(begin[1]) == 0xBB &&
            static_cast<unsigned char>(begin[2]) == 0xBF) {
            begin += 3;
            count -= 3;
        }
        const int wide = ::MultiByteToWideChar(CP_UTF8, 0, begin, count, nullptr, 0);
        if (wide > 0) {
            text.resize(static_cast<size_t>(wide));
            ::MultiByteToWideChar(CP_UTF8, 0, begin, count, text.data(), wide);
        }
    }

    if (!ParseSettings(text, out, error)) {
        // Rename the corrupt file and fall back to the defaults, so a bad edit
        // never blocks startup and the user can inspect what went wrong.
        ::MoveFileExW(path.c_str(), (path + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        out = DefaultSettings();
        out.badFile = true;
        return true;
    }
    return true;
}

bool SaveSettings(const std::wstring& path, const Settings& settings, std::wstring& error) {
    const std::wstring directory = SettingsDirectory();
    ::CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring text = SerializeSettings(settings);

    // The file is UTF-8; add a BOM so editors and PowerShell agree on it.
    const int utf8Size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
    if (utf8Size <= 0) {
        error = L"cannot encode the settings";
        return false;
    }
    std::string bytes("\xEF\xBB\xBF", 3);
    bytes.resize(3 + static_cast<size_t>(utf8Size));
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          bytes.data() + 3, utf8Size, nullptr, nullptr);

    const std::wstring temp = path + L".tmp";
    HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot create the settings file";
        return false;
    }

    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr);
    ::CloseHandle(file);
    if (!ok || written != bytes.size()) {
        ::DeleteFileW(temp.c_str());
        error = L"cannot write the settings file";
        return false;
    }

    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temp.c_str());
        error = L"cannot replace the settings file";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------

namespace {

// Canonical display names (mixed case) used by FormatHotkey.
struct KeyName {
    const wchar_t* name;
    UINT virtualKey;
};

const KeyName kKeyNames[] = {
    {L"Up", VK_UP},          {L"Down", VK_DOWN},       {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT},    {L"Home", VK_HOME},       {L"End", VK_END},
    {L"PageUp", VK_PRIOR},   {L"PageDown", VK_NEXT},   {L"Insert", VK_INSERT},
    {L"Delete", VK_DELETE},  {L"Space", VK_SPACE},     {L"Tab", VK_TAB},
    {L"Enter", VK_RETURN},   {L"Escape", VK_ESCAPE},   {L"Backspace", VK_BACK},
    {L"Minus", VK_OEM_MINUS}, {L"Plus", VK_OEM_PLUS},  {L"Comma", VK_OEM_COMMA},
    {L"Period", VK_OEM_PERIOD},
};

// Extra spellings accepted while parsing but never produced by FormatHotkey.
const KeyName kKeyAliases[] = {
    {L"PGUP", VK_PRIOR},   {L"PGDN", VK_NEXT},    {L"DEL", VK_DELETE},
    {L"RETURN", VK_RETURN}, {L"ESC", VK_ESCAPE},   {L"BACK", VK_BACK},
};

UINT KeyFromName(const std::wstring& name) {
    std::wstring canonical = name;
    for (wchar_t& ch : canonical) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
    }

    if (canonical.size() == 1) {
        const wchar_t ch = canonical[0];
        if (ch >= L'A' && ch <= L'Z') {
            return ch;
        }
        if (ch >= L'0' && ch <= L'9') {
            return ch;
        }
    }
    if (canonical.size() >= 2 && canonical[0] == L'F') {
        const int number = _wtoi(canonical.c_str() + 1);
        if (number >= 1 && number <= 24) {
            return VK_F1 + static_cast<UINT>(number - 1);
        }
    }
    for (const KeyName& entry : kKeyNames) {
        if (_wcsicmp(canonical.c_str(), entry.name) == 0) {
            return entry.virtualKey;
        }
    }
    for (const KeyName& entry : kKeyAliases) {
        if (_wcsicmp(canonical.c_str(), entry.name) == 0) {
            return entry.virtualKey;
        }
    }
    return 0;
}

std::wstring NameFromKey(UINT virtualKey) {
    if (virtualKey >= 'A' && virtualKey <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= '0' && virtualKey <= '9') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        wchar_t buffer[8] = {};
        swprintf_s(buffer, L"F%u", virtualKey - VK_F1 + 1);
        return buffer;
    }
    for (const KeyName& entry : kKeyNames) {
        if (entry.virtualKey == virtualKey) {
            return entry.name;
        }
    }
    wchar_t buffer[16] = {};
    swprintf_s(buffer, L"0x%02X", virtualKey);
    return buffer;
}

}  // namespace

bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& virtualKey) {
    modifiers = 0;
    virtualKey = 0;

    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'+') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);

    for (size_t i = 0; i < parts.size(); ++i) {
        const std::wstring token = Trim(parts[i]);
        if (token.empty()) {
            return false;
        }
        const std::wstring lower = ToLowerCopy(token);
        if (lower == L"ctrl" || lower == L"control") {
            modifiers |= MOD_CONTROL;
        } else if (lower == L"alt") {
            modifiers |= MOD_ALT;
        } else if (lower == L"shift") {
            modifiers |= MOD_SHIFT;
        } else if (lower == L"win" || lower == L"meta" || lower == L"cmd" ||
                   lower == L"super") {
            modifiers |= MOD_WIN;
        } else {
            // The key must be the last token.
            if (i + 1 != parts.size()) {
                return false;
            }
            const UINT key = KeyFromName(token);
            if (key == 0) {
                return false;
            }
            virtualKey = key;
        }
    }
    return virtualKey != 0;
}

std::wstring FormatHotkey(UINT modifiers, UINT virtualKey) {
    if (virtualKey == 0) {
        return std::wstring();
    }
    std::wstring text;
    if (modifiers & MOD_CONTROL) {
        text += L"Ctrl+";
    }
    if (modifiers & MOD_ALT) {
        text += L"Alt+";
    }
    if (modifiers & MOD_SHIFT) {
        text += L"Shift+";
    }
    if (modifiers & MOD_WIN) {
        text += L"Win+";
    }
    text += NameFromKey(virtualKey);
    return text;
}

bool SameHotkey(const HotkeyBinding& left, const HotkeyBinding& right) {
    return left.virtualKey == right.virtualKey && SameModifiers(left.modifiers, right.modifiers);
}

bool ValidateHotkeys(const Settings& settings, std::wstring& error) {
    for (int i = 0; i < kHotkeyCount; ++i) {
        const HotkeyBinding& binding = settings.hotkeys[i];
        const HotkeyAction action = static_cast<HotkeyAction>(i);
        if (binding.empty()) {
            // Self-destruct has no default binding; leaving it empty is valid.
            if (action == HotkeyAction::SelfDestruct) {
                continue;
            }
            error = std::wstring(HotkeyActionLabel(action)) + L" has no key combination";
            return false;
        }
        if ((binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == 0) {
            error = std::wstring(HotkeyActionLabel(action)) +
                    L" needs at least one modifier (Ctrl, Alt, Shift or Win)";
            return false;
        }
        for (int j = i + 1; j < kHotkeyCount; ++j) {
            if (settings.hotkeys[j].empty()) {
                continue;
            }
            if (SameHotkey(binding, settings.hotkeys[j])) {
                error = std::wstring(HotkeyActionLabel(action)) + L" and " +
                        HotkeyActionLabel(static_cast<HotkeyAction>(j)) +
                        L" share the same combination";
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Auto-attach
// ---------------------------------------------------------------------------

bool RuleMatches(const AutoAttachRule& rule, const std::wstring& exeName,
                 const std::wstring& title) {
    if (!rule.enabled || rule.value.empty()) {
        return false;
    }
    if (_wcsicmp(rule.match.c_str(), L"exe") == 0) {
        return _wcsicmp(exeName.c_str(), rule.value.c_str()) == 0;
    }
    if (_wcsicmp(rule.match.c_str(), L"title") == 0) {
        return ContainsNoCase(title, rule.value);
    }
    return false;
}

bool StickyMatches(const StickyTarget& target, const std::wstring& exePath,
                   const std::wstring& className) {
    if (target.exePath.empty() || exePath.empty()) {
        return false;
    }
    if (_wcsicmp(target.exePath.c_str(), exePath.c_str()) != 0) {
        return false;
    }
    // An empty stored class acts as a wildcard, so an application can opt into
    // "any window of this executable".
    return target.className.empty() || _wcsicmp(target.className.c_str(), className.c_str()) == 0;
}

AutoCaptureGuard::Entry* AutoCaptureGuard::Find(DWORD pid, ULONGLONG createTime, HWND hwnd) {
    for (Entry& entry : entries_) {
        if (entry.pid == pid && entry.createTime == createTime && entry.hwnd == hwnd) {
            return &entry;
        }
    }
    return nullptr;
}

bool AutoCaptureGuard::Allow(DWORD pid, ULONGLONG createTime, HWND hwnd) const {
    for (const Entry& entry : entries_) {
        if (entry.pid == pid && entry.createTime == createTime && entry.hwnd == hwnd) {
            return !entry.suppressed && entry.attempts < kMaxAttempts;
        }
    }
    return true;
}

void AutoCaptureGuard::NoteFailure(DWORD pid, ULONGLONG createTime, HWND hwnd) {
    Entry* entry = Find(pid, createTime, hwnd);
    if (entry == nullptr) {
        entries_.push_back(Entry{pid, createTime, hwnd, 1, false});
        return;
    }
    ++entry->attempts;
    if (entry->attempts >= kMaxAttempts) {
        entry->suppressed = true;
    }
}

void AutoCaptureGuard::Suppress(DWORD pid, ULONGLONG createTime, HWND hwnd) {
    Entry* entry = Find(pid, createTime, hwnd);
    if (entry != nullptr) {
        entry->suppressed = true;
        return;
    }
    entries_.push_back(Entry{pid, createTime, hwnd, 0, true});
}

void AutoCaptureGuard::Clear() { entries_.clear(); }

// ---------------------------------------------------------------------------
// Appearance metadata
// ---------------------------------------------------------------------------

const wchar_t* ThemePresetKey(ThemePreset preset) {
    switch (preset) {
        case ThemePreset::Graphite:
            return L"graphite";
        case ThemePreset::Light:
            return L"light";
        case ThemePreset::Midnight:
            return L"midnight";
        case ThemePreset::System:
            return L"system";
        default:
            return L"graphite";
    }
}

const wchar_t* ThemePresetLabel(ThemePreset preset) {
    switch (preset) {
        case ThemePreset::Graphite:
            return L"Graphite";
        case ThemePreset::Light:
            return L"Light";
        case ThemePreset::Midnight:
            return L"Midnight";
        case ThemePreset::System:
            return L"System";
        default:
            return L"Graphite";
    }
}

ThemePreset ThemePresetFromKey(const std::wstring& key) {
    for (int i = 0; i < static_cast<int>(ThemePreset::Count); ++i) {
        const auto preset = static_cast<ThemePreset>(i);
        if (_wcsicmp(key.c_str(), ThemePresetKey(preset)) == 0) {
            return preset;
        }
    }
    return ThemePreset::Count;
}

const wchar_t* DensityKey(DensityMode density) {
    return density == DensityMode::Compact ? L"compact" : L"comfortable";
}

const wchar_t* DensityLabel(DensityMode density) {
    return density == DensityMode::Compact ? L"Compact" : L"Comfortable";
}

DensityMode DensityFromKey(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"compact") == 0) {
        return DensityMode::Compact;
    }
    if (_wcsicmp(key.c_str(), L"comfortable") == 0) {
        return DensityMode::Comfortable;
    }
    return DensityMode::Count;
}

const wchar_t* BackgroundStyleKey(BackgroundStyle style) {
    return style == BackgroundStyle::Gradient ? L"gradient" : L"solid";
}

BackgroundStyle BackgroundStyleFromKey(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"gradient") == 0) {
        return BackgroundStyle::Gradient;
    }
    if (_wcsicmp(key.c_str(), L"solid") == 0) {
        return BackgroundStyle::Solid;
    }
    return BackgroundStyle::Count;
}

const wchar_t* SelfDeleteModeKey(SelfDeleteMode mode) {
    switch (mode) {
        case SelfDeleteMode::ExeOnly:
            return L"exe";
        case SelfDeleteMode::Everything:
            return L"everything";
        default:
            return L"never";
    }
}

const wchar_t* SelfDeleteModeLabel(SelfDeleteMode mode) {
    switch (mode) {
        case SelfDeleteMode::ExeOnly:
            return L"Exe only";
        case SelfDeleteMode::Everything:
            return L"Everything";
        default:
            return L"Never";
    }
}

SelfDeleteMode SelfDeleteModeFromKey(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"exe") == 0 || _wcsicmp(key.c_str(), L"exeonly") == 0) {
        return SelfDeleteMode::ExeOnly;
    }
    if (_wcsicmp(key.c_str(), L"everything") == 0 || _wcsicmp(key.c_str(), L"all") == 0) {
        return SelfDeleteMode::Everything;
    }
    if (_wcsicmp(key.c_str(), L"never") == 0) {
        return SelfDeleteMode::Never;
    }
    return SelfDeleteMode::Count;
}

namespace {

bool IsEmojiWhitespace(uint32_t cp) {
    return cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

// Characters that extend the previous glyph: variation selectors, combining
// marks, skin-tone modifiers, the keycap mark and emoji tag characters.
bool IsEmojiExtend(uint32_t cp) {
    return cp == 0xFE0E || cp == 0xFE0F || (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || cp == 0x20E3 ||
           (cp >= 0xE0020 && cp <= 0xE007F);
}

bool IsRegionalIndicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }

std::vector<uint32_t> ToCodePoints(const std::wstring& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < text.size()) {
            const wchar_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                out.push_back(0x10000 + ((static_cast<uint32_t>(ch) - 0xD800) << 10) +
                              (static_cast<uint32_t>(low) - 0xDC00));
                ++i;
                continue;
            }
        }
        out.push_back(static_cast<uint32_t>(ch));
    }
    return out;
}

void AppendCodePoint(std::wstring& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<wchar_t>(cp));
        return;
    }
    cp -= 0x10000;
    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
}

}  // namespace

std::wstring NormalizeEmoji(const std::wstring& text) {
    std::vector<uint32_t> codePoints = ToCodePoints(text);
    size_t begin = 0;
    while (begin < codePoints.size() && IsEmojiWhitespace(codePoints[begin])) {
        ++begin;
    }
    // Some paste paths prefix the sequence with a variation selector.
    while (begin < codePoints.size() && codePoints[begin] == 0xFE0F) {
        ++begin;
    }
    if (begin >= codePoints.size()) {
        return std::wstring();
    }

    std::wstring out;
    AppendCodePoint(out, codePoints[begin]);
    int regionalCount = IsRegionalIndicator(codePoints[begin]) ? 1 : 0;
    size_t i = begin + 1;
    while (i < codePoints.size()) {
        const uint32_t cp = codePoints[i];
        if (cp == 0x200D) {  // zero-width joiner: keep the next glyph
            if (i + 1 >= codePoints.size()) {
                break;
            }
            AppendCodePoint(out, cp);
            AppendCodePoint(out, codePoints[i + 1]);
            regionalCount = IsRegionalIndicator(codePoints[i + 1]) ? regionalCount + 1 : 0;
            i += 2;
            continue;
        }
        if (IsEmojiExtend(cp)) {
            AppendCodePoint(out, cp);
            ++i;
            continue;
        }
        // A flag is exactly two regional indicators.
        if (IsRegionalIndicator(cp) && regionalCount == 1) {
            AppendCodePoint(out, cp);
            regionalCount = 2;
            ++i;
            continue;
        }
        break;
    }
    return out;
}

void NormalizeTabs(AppearanceSettings& appearance, const std::vector<std::wstring>& knownIds) {
    if (knownIds.empty()) {
        appearance.tabs.clear();
        return;
    }

    std::vector<TabPreference> result;
    std::vector<bool> seen(knownIds.size(), false);
    for (const TabPreference& tab : appearance.tabs) {
        size_t index = knownIds.size();
        for (size_t i = 0; i < knownIds.size(); ++i) {
            if (tab.id == knownIds[i]) {
                index = i;
                break;
            }
        }
        if (index == knownIds.size() || seen[index]) {
            continue;
        }
        seen[index] = true;
        result.push_back(tab);
    }
    for (size_t i = 0; i < knownIds.size(); ++i) {
        if (!seen[i]) {
            result.push_back(TabPreference{knownIds[i], true});
        }
    }

    bool anyVisible = false;
    for (const TabPreference& tab : result) {
        anyVisible = anyVisible || tab.visible;
    }
    if (!anyVisible && !result.empty()) {
        result.front().visible = true;
    }
    appearance.tabs = std::move(result);
}

// ---------------------------------------------------------------------------
// Action metadata
// ---------------------------------------------------------------------------

const wchar_t* HotkeyActionKey(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::ToggleSelection:
            return L"toggleSelection";
        case HotkeyAction::RestoreAll:
            return L"restoreAll";
        case HotkeyAction::ToggleUi:
            return L"toggleUi";
        case HotkeyAction::Minimize:
            return L"minimize";
        case HotkeyAction::Restore:
            return L"restore";
        case HotkeyAction::Close:
            return L"close";
        case HotkeyAction::SelfDestruct:
            return L"selfDestruct";
        default:
            return L"";
    }
}

const wchar_t* HotkeyActionLabel(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::ToggleSelection:
            return L"Toggle stealth";
        case HotkeyAction::RestoreAll:
            return L"Restore all";
        case HotkeyAction::ToggleUi:
            return L"Toggle window";
        case HotkeyAction::Minimize:
            return L"Minimize target";
        case HotkeyAction::Restore:
            return L"Restore target";
        case HotkeyAction::Close:
            return L"Close target";
        case HotkeyAction::SelfDestruct:
            return L"Self-destruct";
        default:
            return L"";
    }
}

HotkeyAction HotkeyActionFromKey(const std::wstring& key) {
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (key == HotkeyActionKey(static_cast<HotkeyAction>(i))) {
            return static_cast<HotkeyAction>(i);
        }
    }
    return HotkeyAction::Count;
}

}  // namespace gf
