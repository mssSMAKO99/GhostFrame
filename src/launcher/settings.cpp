#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <utility>

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

// ---------------------------------------------------------------------------
// Minimal JSON reader
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::wstring text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::wstring, JsonValue>> object;

    const JsonValue* Get(const wchar_t* key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::wstring& text) : text_(text) {}

    bool Parse(JsonValue& out) {
        SkipSpace();
        if (!ParseValue(out)) {
            return false;
        }
        SkipSpace();
        if (pos_ != text_.size()) {
            return Fail(L"trailing characters");
        }
        return true;
    }

    const std::wstring& error() const { return error_; }

private:
    bool Fail(const wchar_t* message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    bool Eof() const { return pos_ >= text_.size(); }
    wchar_t Peek() const { return Eof() ? L'\0' : text_[pos_]; }
    wchar_t Next() { return Eof() ? L'\0' : text_[pos_++]; }

    void SkipSpace() {
        while (!Eof()) {
            const wchar_t ch = text_[pos_];
            if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool Literal(const wchar_t* word) {
        const size_t length = wcslen(word);
        if (text_.compare(pos_, length, word) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    bool ParseValue(JsonValue& out) {
        SkipSpace();
        if (Eof()) {
            return Fail(L"unexpected end of input");
        }
        switch (Peek()) {
            case L'{':
                return ParseObject(out);
            case L'[':
                return ParseArray(out);
            case L'"': {
                out.type = JsonValue::Type::String;
                return ParseString(out.text);
            }
            case L't':
                if (Literal(L"true")) {
                    out.type = JsonValue::Type::Bool;
                    out.boolean = true;
                    return true;
                }
                return Fail(L"invalid literal");
            case L'f':
                if (Literal(L"false")) {
                    out.type = JsonValue::Type::Bool;
                    out.boolean = false;
                    return true;
                }
                return Fail(L"invalid literal");
            case L'n':
                if (Literal(L"null")) {
                    out.type = JsonValue::Type::Null;
                    return true;
                }
                return Fail(L"invalid literal");
            default:
                return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++pos_;  // '{'
        SkipSpace();
        if (Peek() == L'}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipSpace();
            if (Peek() != L'"') {
                return Fail(L"expected a key");
            }
            std::wstring key;
            if (!ParseString(key)) {
                return false;
            }
            SkipSpace();
            if (Next() != L':') {
                return Fail(L"expected ':'");
            }
            JsonValue value;
            if (!ParseValue(value)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            SkipSpace();
            const wchar_t separator = Next();
            if (separator == L',') {
                continue;
            }
            if (separator == L'}') {
                return true;
            }
            return Fail(L"expected ',' or '}'");
        }
    }

    bool ParseArray(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++pos_;  // '['
        SkipSpace();
        if (Peek() == L']') {
            ++pos_;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!ParseValue(value)) {
                return false;
            }
            out.array.push_back(std::move(value));
            SkipSpace();
            const wchar_t separator = Next();
            if (separator == L',') {
                continue;
            }
            if (separator == L']') {
                return true;
            }
            return Fail(L"expected ',' or ']'");
        }
    }

    bool ParseString(std::wstring& out) {
        out.clear();
        ++pos_;  // '"'
        while (!Eof()) {
            wchar_t ch = Next();
            if (ch == L'"') {
                return true;
            }
            if (ch != L'\\') {
                out.push_back(ch);
                continue;
            }
            if (Eof()) {
                return Fail(L"unterminated escape");
            }
            const wchar_t escape = Next();
            switch (escape) {
                case L'"':
                    out.push_back(L'"');
                    break;
                case L'\\':
                    out.push_back(L'\\');
                    break;
                case L'/':
                    out.push_back(L'/');
                    break;
                case L'b':
                    out.push_back(L'\b');
                    break;
                case L'f':
                    out.push_back(L'\f');
                    break;
                case L'n':
                    out.push_back(L'\n');
                    break;
                case L'r':
                    out.push_back(L'\r');
                    break;
                case L't':
                    out.push_back(L'\t');
                    break;
                case L'u': {
                    unsigned code = 0;
                    if (!ParseHex4(code)) {
                        return Fail(L"invalid \\u escape");
                    }
                    // Combine a UTF-16 surrogate pair when present.
                    if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
                        text_[pos_] == L'\\' && text_[pos_ + 1] == L'u') {
                        pos_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low)) {
                            return Fail(L"invalid low surrogate");
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    if (code <= 0xFFFF) {
                        out.push_back(static_cast<wchar_t>(code));
                    } else {
                        code -= 0x10000;
                        out.push_back(static_cast<wchar_t>(0xD800 + (code >> 10)));
                        out.push_back(static_cast<wchar_t>(0xDC00 + (code & 0x3FF)));
                    }
                    break;
                }
                default:
                    return Fail(L"invalid escape");
            }
        }
        return Fail(L"unterminated string");
    }

    bool ParseHex4(unsigned& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (Eof()) {
                return false;
            }
            const wchar_t ch = Next();
            unsigned digit = 0;
            if (ch >= L'0' && ch <= L'9') {
                digit = static_cast<unsigned>(ch - L'0');
            } else if (ch >= L'a' && ch <= L'f') {
                digit = static_cast<unsigned>(ch - L'a' + 10);
            } else if (ch >= L'A' && ch <= L'F') {
                digit = static_cast<unsigned>(ch - L'A' + 10);
            } else {
                return false;
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const size_t start = pos_;
        if (Peek() == L'-') {
            ++pos_;
        }
        bool digits = false;
        while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
            ++pos_;
            digits = true;
        }
        if (Peek() == L'.') {
            ++pos_;
            while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
                ++pos_;
                digits = true;
            }
        }
        if (Peek() == L'e' || Peek() == L'E') {
            ++pos_;
            if (Peek() == L'+' || Peek() == L'-') {
                ++pos_;
            }
            while (!Eof() && Peek() >= L'0' && Peek() <= L'9') {
                ++pos_;
            }
        }
        if (!digits) {
            return Fail(L"expected a value");
        }
        out.type = JsonValue::Type::Number;
        out.number = wcstod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::wstring& text_;
    size_t pos_ = 0;
    std::wstring error_;
};

// ---------------------------------------------------------------------------
// JSON writer
// ---------------------------------------------------------------------------

void AppendEscaped(std::wstring& out, const std::wstring& text) {
    out.push_back(L'"');
    for (wchar_t ch : text) {
        switch (ch) {
            case L'"':
                out += L"\\\"";
                break;
            case L'\\':
                out += L"\\\\";
                break;
            case L'\n':
                out += L"\\n";
                break;
            case L'\r':
                out += L"\\r";
                break;
            case L'\t':
                out += L"\\t";
                break;
            default:
                if (ch < 0x20) {
                    wchar_t buffer[8] = {};
                    swprintf_s(buffer, L"\\u%04X", static_cast<unsigned>(ch));
                    out += buffer;
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    out.push_back(L'"');
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
    out += L"\n  },\n";

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
        out += L"]\n";
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
        out += L"  ]\n";
    }

    out += L"}\n";
    return out;
}

bool ParseSettings(const std::wstring& json, Settings& out, std::wstring& error) {
    JsonParser parser(json);
    JsonValue root;
    if (!parser.Parse(root)) {
        error = parser.error();
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = L"the settings root must be an object";
        return false;
    }

    Settings parsed = DefaultSettings();
    parsed.version = ReadInt(root.Get(L"version"), parsed.version);
    ParseUi(root, parsed.ui);
    ParseHotkeys(root, parsed);
    ParseAutoAttach(root, parsed);
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
        if (binding.empty()) {
            error = std::wstring(HotkeyActionLabel(static_cast<HotkeyAction>(i))) +
                    L" has no key combination";
            return false;
        }
        if ((binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) == 0) {
            error = std::wstring(HotkeyActionLabel(static_cast<HotkeyAction>(i))) +
                    L" needs at least one modifier (Ctrl, Alt, Shift or Win)";
            return false;
        }
        for (int j = i + 1; j < kHotkeyCount; ++j) {
            if (SameHotkey(binding, settings.hotkeys[j])) {
                error = std::wstring(HotkeyActionLabel(static_cast<HotkeyAction>(i))) + L" and " +
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
