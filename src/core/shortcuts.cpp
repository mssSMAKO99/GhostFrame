#include "shortcuts.h"

#include <shlobj.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

#include "json.h"
#include "settings.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace gf {

namespace {

std::wstring Win32Text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (text.empty()) {
        wchar_t fallback[64] = {};
        swprintf_s(fallback, L"Windows error %lu", code);
        text = fallback;
    }
    return text;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Trim(std::wstring text) {
    const wchar_t* spaces = L" \t\r\n";
    const std::wstring::size_type first = text.find_first_not_of(spaces);
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const std::wstring::size_type last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

std::wstring NowLocalIso() {
    SYSTEMTIME time = {};
    ::GetLocalTime(&time);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::wstring ReadString(const JsonValue* value, const std::wstring& fallback) {
    if (value != nullptr && value->type == JsonValue::Type::String) {
        return value->text;
    }
    return fallback;
}

bool WriteAll(HANDLE file, const std::string& bytes) {
    const char* data = bytes.data();
    size_t size = bytes.size();
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(size, static_cast<size_t>(1u << 20)));
        if (!::WriteFile(file, data, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        data += written;
        size -= written;
    }
    return true;
}

// Shell links are a COM apartment client; the UI and daemon threads may already
// be initialised, so only balance our own successful CoInitializeEx.
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = SUCCEEDED(hr);
    }
    ~ComApartment() {
        if (uninitialize_) {
            ::CoUninitialize();
        }
    }

private:
    bool uninitialize_ = false;
};

}  // namespace

const wchar_t* ShortcutKindKey(ShortcutKind kind) {
    switch (kind) {
        case ShortcutKind::Ui:
            return L"ui";
        case ShortcutKind::Daemon:
            return L"daemon";
        case ShortcutKind::AppWrapper:
            return L"app";
        default:
            return L"ui";
    }
}

const wchar_t* ShortcutKindLabel(ShortcutKind kind) {
    switch (kind) {
        case ShortcutKind::Ui:
            return L"GhostFrame UI";
        case ShortcutKind::Daemon:
            return L"Background daemon";
        case ShortcutKind::AppWrapper:
            return L"App wrapper";
        default:
            return L"GhostFrame UI";
    }
}

ShortcutKind ShortcutKindFromKey(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"daemon") == 0) {
        return ShortcutKind::Daemon;
    }
    if (_wcsicmp(key.c_str(), L"app") == 0 || _wcsicmp(key.c_str(), L"wrapper") == 0) {
        return ShortcutKind::AppWrapper;
    }
    return ShortcutKind::Ui;
}

const wchar_t* ShortcutLocationKey(ShortcutLocation location) {
    switch (location) {
        case ShortcutLocation::StartMenu:
            return L"startmenu";
        case ShortcutLocation::Startup:
            return L"startup";
        default:
            return L"desktop";
    }
}

const wchar_t* ShortcutLocationLabel(ShortcutLocation location) {
    switch (location) {
        case ShortcutLocation::StartMenu:
            return L"Start Menu";
        case ShortcutLocation::Startup:
            return L"Startup";
        default:
            return L"Desktop";
    }
}

ShortcutLocation ShortcutLocationFromKey(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"startmenu") == 0) {
        return ShortcutLocation::StartMenu;
    }
    if (_wcsicmp(key.c_str(), L"startup") == 0) {
        return ShortcutLocation::Startup;
    }
    return ShortcutLocation::Desktop;
}

std::wstring ShortcutLocationPath(ShortcutLocation location) {
    KNOWNFOLDERID id = FOLDERID_Desktop;
    if (location == ShortcutLocation::StartMenu) {
        id = FOLDERID_Programs;
    } else if (location == ShortcutLocation::Startup) {
        id = FOLDERID_Startup;
    }
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) || path == nullptr) {
        return std::wstring();
    }
    std::wstring result = path;
    ::CoTaskMemFree(path);
    return result;
}

std::wstring ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ShortcutManifestPath() { return SettingsDirectory() + L"\\shortcuts.json"; }

const wchar_t* ShortcutKindDefaultName(ShortcutKind kind) {
    switch (kind) {
        case ShortcutKind::Daemon:
            return L"GhostFrame Daemon";
        case ShortcutKind::AppWrapper:
            return L"GhostFrame App";
        default:
            return L"GhostFrame";
    }
}

std::wstring ShortcutWrapperArguments(const std::wstring& appPath, const std::wstring& extra) {
    std::wstring arguments = L"--launch \"";
    arguments += appPath;
    arguments += L"\"";
    const std::wstring tail = Trim(extra);
    if (!tail.empty()) {
        arguments += L" ";
        arguments += tail;
    }
    return arguments;
}

std::wstring ShortcutFileStem(const std::wstring& name) {
    std::wstring result;
    for (wchar_t ch : name) {
        if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            result.push_back(L'_');
        } else {
            result.push_back(ch);
        }
    }
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    if (result.empty()) {
        result = L"GhostFrame";
    }
    return result;
}

bool CreateShortcutAt(const std::wstring& linkPath, const ShortcutSpec& spec,
                      const std::wstring& exePath, const std::wstring& iconPath,
                      ShortcutEntry& out, std::wstring& error) {
    if (linkPath.empty() || exePath.empty()) {
        error = L"the shortcut path or the GhostFrame executable is missing";
        return false;
    }
    const std::wstring directory = DirectoryOf(linkPath);
    if (!directory.empty() && !::CreateDirectoryW(directory.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"cannot create " + directory + L": " + Win32Text(::GetLastError());
        return false;
    }

    ComApartment apartment;
    ComPtr<IShellLinkW> link;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(link.GetAddressOf()));
    if (FAILED(hr)) {
        error = L"cannot create the shell link object";
        return false;
    }

    link->SetPath(exePath.c_str());
    link->SetArguments(spec.arguments.c_str());
    if (!spec.description.empty()) {
        link->SetDescription(spec.description.c_str());
    }
    const std::wstring working = spec.workingDirectory.empty() ? DirectoryOf(exePath)
                                                               : spec.workingDirectory;
    if (!working.empty()) {
        link->SetWorkingDirectory(working.c_str());
    }
    link->SetIconLocation(iconPath.empty() ? exePath.c_str() : iconPath.c_str(), 0);

    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist))) {
        error = L"the shell link cannot be saved";
        return false;
    }
    hr = persist->Save(linkPath.c_str(), TRUE);
    if (FAILED(hr)) {
        wchar_t code[16] = {};
        swprintf_s(code, L"%08lX", static_cast<unsigned long>(hr));
        error = L"cannot save " + linkPath + L" (0x" + code + L")";
        return false;
    }

    out.path = linkPath;
    out.kind = ShortcutKindKey(spec.kind);
    out.name = spec.name;
    out.target = exePath;
    out.arguments = spec.arguments;
    out.created = NowLocalIso();
    return true;
}

bool CreateShortcut(const ShortcutSpec& spec, const std::wstring& iconPath, ShortcutEntry& out,
                    std::wstring& error) {
    const std::wstring folder = ShortcutLocationPath(spec.location);
    if (folder.empty()) {
        error = L"the destination folder could not be resolved";
        return false;
    }
    const std::wstring stem =
        ShortcutFileStem(spec.name.empty() ? ShortcutKindDefaultName(spec.kind) : spec.name);
    const std::wstring linkPath = folder + L"\\" + stem + L".lnk";
    ShortcutSpec normalized = spec;
    if (normalized.name.empty()) {
        normalized.name = ShortcutKindDefaultName(spec.kind);
    }
    return CreateShortcutAt(linkPath, normalized, ExecutablePath(), iconPath, out, error);
}

bool DeleteShortcutFile(const std::wstring& path, std::wstring& error) {
    if (path.empty()) {
        error = L"the shortcut path is empty";
        return false;
    }
    if (!::DeleteFileW(path.c_str())) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            error.clear();
            return true;
        }
        error = L"cannot remove " + path + L": " + Win32Text(code);
        return false;
    }
    error.clear();
    return true;
}

bool ShortcutExists(const std::wstring& path) { return FileExists(path); }

std::wstring SerializeShortcutManifest(const std::vector<ShortcutEntry>& entries) {
    std::wstring out = L"{\n  \"version\": 1,\n  \"shortcuts\": [";
    if (entries.empty()) {
        out += L"]\n}\n";
        return out;
    }
    out += L"\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        const ShortcutEntry& entry = entries[i];
        out += L"    { \"path\": ";
        AppendJsonString(out, entry.path);
        out += L", \"kind\": ";
        AppendJsonString(out, entry.kind);
        out += L", \"name\": ";
        AppendJsonString(out, entry.name);
        out += L",\n      \"target\": ";
        AppendJsonString(out, entry.target);
        out += L", \"arguments\": ";
        AppendJsonString(out, entry.arguments);
        out += L", \"created\": ";
        AppendJsonString(out, entry.created);
        out += L" }";
        out += (i + 1 < entries.size()) ? L",\n" : L"\n";
    }
    out += L"  ]\n}\n";
    return out;
}

bool ParseShortcutManifest(const std::wstring& json, std::vector<ShortcutEntry>& out,
                           std::wstring& error) {
    JsonValue root;
    if (!ParseJson(json, root, error)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = L"the shortcut manifest root must be an object";
        return false;
    }
    out.clear();
    const JsonValue* list = root.Get(L"shortcuts");
    if (list == nullptr || list->type != JsonValue::Type::Array) {
        return true;
    }
    for (const JsonValue& item : list->array) {
        if (item.type != JsonValue::Type::Object) {
            continue;
        }
        ShortcutEntry entry;
        entry.path = ReadString(item.Get(L"path"), L"");
        entry.kind = ReadString(item.Get(L"kind"), L"ui");
        entry.name = ReadString(item.Get(L"name"), L"");
        entry.target = ReadString(item.Get(L"target"), L"");
        entry.arguments = ReadString(item.Get(L"arguments"), L"");
        entry.created = ReadString(item.Get(L"created"), L"");
        if (!entry.path.empty()) {
            out.push_back(std::move(entry));
        }
    }
    error.clear();
    return true;
}

bool LoadShortcutManifest(std::vector<ShortcutEntry>& out, std::wstring& error) {
    out.clear();
    const std::wstring path = ShortcutManifestPath();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            error.clear();
            return true;
        }
        error = L"cannot open " + path + L": " + Win32Text(code);
        return false;
    }
    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (8ll << 20)) {
        ::CloseHandle(file);
        error = L"the shortcut manifest is too large";
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(bytes.size() - done, static_cast<size_t>(1u << 20)));
        if (!::ReadFile(file, bytes.data() + done, chunk, &read, nullptr) || read == 0) {
            break;
        }
        done += read;
    }
    ::CloseHandle(file);
    bytes.resize(done);

    // The file is UTF-8; drop a possible BOM before decoding.
    const char* begin = bytes.data();
    int count = static_cast<int>(bytes.size());
    if (count >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF &&
        static_cast<unsigned char>(begin[1]) == 0xBB &&
        static_cast<unsigned char>(begin[2]) == 0xBF) {
        begin += 3;
        count -= 3;
    }
    std::wstring text;
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, begin, count, nullptr, 0);
    if (wide > 0) {
        text.resize(static_cast<size_t>(wide));
        ::MultiByteToWideChar(CP_UTF8, 0, begin, count, text.data(), wide);
    }
    if (text.empty()) {
        error = L"the shortcut manifest is empty";
        return false;
    }

    std::wstring parseError;
    if (!ParseShortcutManifest(text, out, parseError)) {
        const std::wstring bad = path + L".bad";
        ::MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        out.clear();
        error = L"the shortcut manifest was corrupt; renamed to .bad (" + parseError + L")";
        return false;
    }
    error.clear();
    return true;
}

bool SaveShortcutManifest(const std::vector<ShortcutEntry>& entries, std::wstring& error) {
    const std::wstring path = ShortcutManifestPath();
    const std::wstring temporary = path + L".tmp";
    ::CreateDirectoryW(SettingsDirectory().c_str(), nullptr);
    const std::wstring text = SerializeShortcutManifest(entries);

    // Keep the same UTF-8-with-BOM convention as settings.json.
    const int utf8Size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
    if (utf8Size <= 0) {
        error = L"cannot encode the shortcut manifest";
        return false;
    }
    std::string bytes("\xEF\xBB\xBF", 3);
    bytes.resize(3 + static_cast<size_t>(utf8Size));
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          bytes.data() + 3, utf8Size, nullptr, nullptr);

    HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot create " + temporary + L": " + Win32Text(::GetLastError());
        return false;
    }
    const bool ok = WriteAll(file, bytes);
    ::CloseHandle(file);
    if (!ok) {
        error = L"cannot write " + temporary + L": " + Win32Text(::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"cannot replace " + path + L": " + Win32Text(::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    error.clear();
    return true;
}

int RemoveAllShortcuts(std::wstring& error) {
    std::vector<ShortcutEntry> entries;
    std::wstring loadError;
    if (!LoadShortcutManifest(entries, loadError)) {
        // A corrupt manifest has already been moved aside, so there is nothing
        // left to remove reliably.
        error = loadError;
        return 0;
    }
    int removed = 0;
    for (const ShortcutEntry& entry : entries) {
        std::wstring deleteError;
        if (DeleteShortcutFile(entry.path, deleteError)) {
            ++removed;
        }
    }
    const std::vector<ShortcutEntry> empty;
    if (!SaveShortcutManifest(empty, error)) {
        return removed;
    }
    error.clear();
    return removed;
}

}  // namespace gf