#include "branding.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "json.h"
#include "settings.h"

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

std::wstring ReadString(const JsonValue* value, const std::wstring& fallback) {
    if (value != nullptr && value->type == JsonValue::Type::String) {
        return value->text;
    }
    return fallback;
}

int ReadInt(const JsonValue* value, int fallback) {
    if (value != nullptr && value->type == JsonValue::Type::Number) {
        return static_cast<int>(value->number);
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

}  // namespace

std::wstring BrandingManifestPath() { return SettingsDirectory() + L"\\branding.json"; }

std::wstring NowLocalIso8601() {
    SYSTEMTIME time = {};
    ::GetLocalTime(&time);
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::wstring SerializeBrandingManifest(const BrandingManifest& manifest) {
    std::wstring out = L"{\n  \"version\": ";
    wchar_t number[16] = {};
    swprintf_s(number, L"%d", manifest.version);
    out += number;
    out += L",\n  \"originalExeName\": ";
    AppendJsonString(out, manifest.originalExeName);
    out += L",\n  \"originalExePath\": ";
    AppendJsonString(out, manifest.originalExePath);
    out += L",\n  \"name\": ";
    AppendJsonString(out, manifest.name);
    out += L",\n  \"description\": ";
    AppendJsonString(out, manifest.description);
    out += L",\n  \"company\": ";
    AppendJsonString(out, manifest.company);
    out += L",\n  \"iconKind\": ";
    AppendJsonString(out, manifest.iconKind);
    out += L",\n  \"iconSource\": ";
    AppendJsonString(out, manifest.iconSource);
    out += L",\n  \"installedExePath\": ";
    AppendJsonString(out, manifest.installedExePath);
    out += L",\n  \"installDir\": ";
    AppendJsonString(out, manifest.installDir);
    out += L",\n  \"shortcutPath\": ";
    AppendJsonString(out, manifest.shortcutPath);
    out += L",\n  \"autostartValue\": ";
    AppendJsonString(out, manifest.autostartValue);
    out += L",\n  \"appliedAt\": ";
    AppendJsonString(out, manifest.appliedAt);
    out += L"\n}\n";
    return out;
}

bool ParseBrandingManifest(const std::wstring& json, BrandingManifest& out,
                           std::wstring& error) {
    JsonValue root;
    if (!ParseJson(json, root, error)) {
        return false;
    }
    if (root.type != JsonValue::Type::Object) {
        error = L"the branding manifest root must be an object";
        return false;
    }

    BrandingManifest parsed;
    parsed.version = ReadInt(root.Get(L"version"), parsed.version);
    parsed.originalExeName = ReadString(root.Get(L"originalExeName"), L"");
    parsed.originalExePath = ReadString(root.Get(L"originalExePath"), L"");
    parsed.name = ReadString(root.Get(L"name"), L"");
    parsed.description = ReadString(root.Get(L"description"), L"");
    parsed.company = ReadString(root.Get(L"company"), L"");
    parsed.iconKind = ReadString(root.Get(L"iconKind"), L"bundled");
    parsed.iconSource = ReadString(root.Get(L"iconSource"), L"");
    parsed.installedExePath = ReadString(root.Get(L"installedExePath"), L"");
    parsed.installDir = ReadString(root.Get(L"installDir"), L"");
    parsed.shortcutPath = ReadString(root.Get(L"shortcutPath"), L"");
    parsed.autostartValue = ReadString(root.Get(L"autostartValue"), L"");
    parsed.appliedAt = ReadString(root.Get(L"appliedAt"), L"");

    if (parsed.iconKind.empty()) {
        parsed.iconKind = L"bundled";
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

bool LoadBrandingManifestFile(const std::wstring& path, BrandingManifest& out,
                              std::wstring& error) {
    out = BrandingManifest{};
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            error = L"the branding manifest does not exist";
        } else {
            error = L"cannot open " + path + L": " + Win32Text(code);
        }
        return false;
    }

    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (1ll << 20)) {
        ::CloseHandle(file);
        error = L"the branding manifest is too large";
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

    // UTF-8 with an optional BOM, matching the settings file.
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
        error = L"the branding manifest is empty";
        return false;
    }
    return ParseBrandingManifest(text, out, error);
}

bool SaveBrandingManifestFile(const std::wstring& path, const BrandingManifest& manifest,
                              std::wstring& error) {
    const std::wstring directory = DirectoryOf(path);
    if (!directory.empty()) {
        ::CreateDirectoryW(directory.c_str(), nullptr);
    }

    const std::wstring text = SerializeBrandingManifest(manifest);
    const int utf8Size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
    if (utf8Size <= 0) {
        error = L"cannot encode the branding manifest";
        return false;
    }
    std::string bytes("\xEF\xBB\xBF", 3);
    bytes.resize(3 + static_cast<size_t>(utf8Size));
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          bytes.data() + 3, utf8Size, nullptr, nullptr);

    const std::wstring temporary = path + L".tmp";
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

bool LoadBrandingManifest(BrandingManifest& out, std::wstring& error) {
    return LoadBrandingManifestFile(BrandingManifestPath(), out, error);
}

bool SaveBrandingManifest(const BrandingManifest& manifest, std::wstring& error) {
    return SaveBrandingManifestFile(BrandingManifestPath(), manifest, error);
}

bool DeleteBrandingManifest(std::wstring& error) {
    const std::wstring path = BrandingManifestPath();
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

}  // namespace gf