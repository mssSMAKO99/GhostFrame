#include "log.h"

#include <stdarg.h>
#include <stdio.h>

namespace gf {

namespace {

constexpr unsigned long long kRotateBytes = 512ull * 1024ull;

std::wstring g_logPath;
std::wstring g_logDir;

void WriteRaw(const wchar_t* text, int length) {
    if (g_logPath.empty() || length <= 0) {
        return;
    }
    HANDLE file = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size = {};
    if (::GetFileSizeEx(file, &size) && size.QuadPart == 0) {
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        ::WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    }

    DWORD written = 0;
    ::WriteFile(file, text, static_cast<DWORD>(length) * sizeof(wchar_t), &written, nullptr);
    ::CloseHandle(file);
}

}  // namespace

std::wstring FormatString(const wchar_t* format, ...) {
    wchar_t buffer[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring Win32Message(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
        while (!text.empty() &&
               (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }

    if (text.empty()) {
        text = L"unknown error";
    }

    wchar_t suffix[32] = {};
    swprintf_s(suffix, L" (0x%08X)", error);
    return text + suffix;
}

void LogInit(const std::wstring& directory) {
    ::CreateDirectoryW(directory.c_str(), nullptr);
    g_logPath = directory + L"\\ghostframe.log";
    g_logDir = directory;

    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (::GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER size = {};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        if (size.QuadPart > kRotateBytes) {
            const std::wstring previous = g_logPath + L".old";
            ::DeleteFileW(previous.c_str());
            ::MoveFileExW(g_logPath.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
}

void LogShutdown() {
    g_logPath.clear();
    g_logDir.clear();
}

const std::wstring& LogDirectory() { return g_logDir; }

void LogLine(const wchar_t* format, ...) {
    if (g_logPath.empty()) {
        return;
    }

    wchar_t message[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now = {};
    ::GetLocalTime(&now);

    wchar_t line[2200] = {};
    const int length = swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %s\r\n", now.wYear,
                                  now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                  now.wMilliseconds, message);
    WriteRaw(line, length);
}

}  // namespace gf
