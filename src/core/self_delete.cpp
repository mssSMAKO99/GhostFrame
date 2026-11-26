#include "self_delete.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "ghostframe/ui_messages.h"
#include "ipc/daemon_client.h"
#include "shortcuts.h"

namespace gf {

namespace {

constexpr DWORD kDeleteRetryStepMs = 100;
constexpr DWORD kExecutableDeleteTimeoutMs = 15000;

std::wstring Win32Text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
        while (!text.empty() &&
               (text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.')) {
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

std::wstring FileNameOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ModulePath() {
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

std::wstring TempDirectory() {
    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) == 0) {
        return L".";
    }
    return temp;
}

bool IsUnder(const std::wstring& path, const std::wstring& directory) {
    if (path.empty() || directory.empty()) {
        return false;
    }
    std::wstring prefix = directory;
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') {
        prefix.push_back(L'\\');
    }
    return _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// The installer records its Start Menu link in branding.json.  Only a `.lnk` is
// accepted here, so a corrupted manifest can never name an arbitrary file.
bool IsShortcutPath(const std::wstring& path) {
    if (path.size() < 4) {
        return false;
    }
    return _wcsicmp(path.c_str() + (path.size() - 4), L".lnk") == 0;
}

bool RemoveRunValue(const std::wstring& valueName) {
    if (valueName.empty()) {
        return true;
    }
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    const LONG status = ::RegDeleteValueW(key, valueName.c_str());
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void AppendLog(const std::wstring& line) {
    const std::wstring path = SelfDeleteLogPath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") == 0 && file != nullptr) {
        fwprintf(file, L"%s\n", line.c_str());
        fclose(file);
    }
}

bool RemoveTree(const std::wstring& path, ULONGLONG deadline) {
    const auto remaining = [deadline]() -> DWORD {
        const ULONGLONG now = ::GetTickCount64();
        return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    };

    for (;;) {
        bool allChildren = true;
        const std::wstring pattern = path + L"\\*";
        WIN32_FIND_DATAW data = {};
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(data.cFileName, L".") == 0 ||
                    std::wcscmp(data.cFileName, L"..") == 0) {
                    continue;
                }
                const std::wstring child = path + L"\\" + data.cFileName;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                        // Remove the link itself, never its target.
                        if (!::RemoveDirectoryW(child.c_str())) {
                            allChildren = false;
                        }
                    } else {
                        const DWORD left = remaining();
                        if (left == 0 || !RemoveTree(child, deadline)) {
                            allChildren = false;
                        }
                    }
                } else {
                    const DWORD left = remaining();
                    if (left == 0 || !DeleteFileWithRetry(child, left)) {
                        allChildren = false;
                    }
                }
            } while (::FindNextFileW(find, &data));
            ::FindClose(find);
        } else {
            const DWORD code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                return true;
            }
            allChildren = false;
        }

        if (allChildren && ::RemoveDirectoryW(path.c_str())) {
            return true;
        }
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (remaining() == 0) {
            return false;
        }
        ::Sleep(kDeleteRetryStepMs);
    }
}

bool DeleteOneFile(const std::wstring& path, DWORD timeoutMs, std::vector<std::wstring>& failures) {
    if (path.empty()) {
        return true;
    }
    if (DeleteFileWithRetry(path, timeoutMs)) {
        return true;
    }
    failures.push_back(L"cannot remove " + path);
    return false;
}

bool RemoveOneDirectory(const std::wstring& path, DWORD timeoutMs,
                        std::vector<std::wstring>& failures) {
    if (path.empty()) {
        return true;
    }
    if (RemoveDirectoryTreeWithRetry(path, timeoutMs)) {
        return true;
    }
    failures.push_back(L"cannot remove the directory " + path);
    return false;
}

std::wstring QuoteArgument(const std::wstring& value) {
    // All helper arguments are paths or simple identifiers.  Doubling an
    // embedded quote is enough for the Windows command-line parser.
    std::wstring quoted = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') {
            quoted += L'\\';
        }
        quoted.push_back(ch);
    }
    quoted += L'"';
    return quoted;
}

std::wstring BuildHelperCommandLine(const std::wstring& helperPath,
                                    const SelfDeleteHelperOptions& options) {
    std::wstring commandLine = QuoteArgument(helperPath);
    commandLine += L" --self-delete-helper";
    commandLine += L" --mode ";
    commandLine += SelfDeleteModeKey(options.mode);
    if (!options.executable.empty()) {
        commandLine += L" --exe " + QuoteArgument(options.executable);
    }
    if (!options.dataDirectory.empty()) {
        commandLine += L" --data-dir " + QuoteArgument(options.dataDirectory);
    }
    for (DWORD pid : options.waitPids) {
        commandLine += L" --wait-pid ";
        commandLine += std::to_wstring(pid);
    }
    for (const std::wstring& value : options.autostartValues) {
        if (!value.empty()) {
            commandLine += L" --run-value " + QuoteArgument(value);
        }
    }
    for (const std::wstring& file : options.extraFiles) {
        if (!file.empty()) {
            commandLine += L" --remove-file " + QuoteArgument(file);
        }
    }
    commandLine += L" --wait-timeout ";
    commandLine += std::to_wstring(options.waitTimeoutMs);
    return commandLine;
}

bool TakeValue(int argc, wchar_t** argv, int& index, std::wstring& out) {
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

void WaitForPids(const std::vector<DWORD>& pids, DWORD timeoutMs) {
    std::vector<HANDLE> handles;
    handles.reserve(pids.size());
    for (DWORD pid : pids) {
        if (pid == 0) {
            continue;
        }
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process != nullptr) {
            handles.push_back(process);
        }
    }
    if (handles.empty()) {
        return;
    }

    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    size_t remaining = handles.size();
    while (remaining > 0 && ::GetTickCount64() < deadline) {
        DWORD left = static_cast<DWORD>(deadline - ::GetTickCount64());
        if (left > 200) {
            left = 200;
        }
        const DWORD result = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                                      handles.data(), FALSE, left);
        if (result == WAIT_TIMEOUT) {
            continue;
        }
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.size()) {
            // One handle was signalled; wait for all of them.  Re-waiting on a
            // signalled handle returns immediately, which is fine.
            remaining = 0;
            for (HANDLE handle : handles) {
                if (::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) {
                    ++remaining;
                }
            }
        } else {
            break;
        }
    }

    for (HANDLE handle : handles) {
        ::CloseHandle(handle);
    }
}

bool WriteSelfRemovalBatch(const std::wstring& selfPath) {
    const std::wstring batch = TempDirectory() + L"GhostFrame-self-delete-" +
                               std::to_wstring(::GetCurrentProcessId()) + L".cmd";
    const std::wstring text = L"@echo off\r\n"
                              L"ping -n 2 127.0.0.1 >nul\r\n"
                              L"del /f /q " + QuoteArgument(selfPath) + L"\r\n"
                              L"del /f /q \"%~f0\"\r\n";

    HANDLE file = ::CreateFileW(batch.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const int utf8Size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                              nullptr, nullptr);
    std::string bytes(static_cast<size_t>(utf8Size > 0 ? utf8Size : 0), '\0');
    if (utf8Size > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              bytes.data(), utf8Size, nullptr, nullptr);
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr);
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(batch.c_str());
        return false;
    }

    std::wstring commandLine = L"cmd.exe /c " + QuoteArgument(batch);
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL started = ::CreateProcessW(
        nullptr, buffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, nullptr, &startup,
        &process);
    if (!started) {
        ::DeleteFileW(batch.c_str());
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

void ScheduleHelperSelfRemoval() {
    const std::wstring self = ModulePath();
    if (self.empty()) {
        return;
    }
    if (WriteSelfRemovalBatch(self)) {
        return;
    }
    // Best effort: the helper lives in %TEMP%, so a marker for the next boot is
    // preferable to leaving it around forever.
    ::MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

}  // namespace

std::wstring SelfDeleteLogPath() {
    return TempDirectory() + L"GhostFrame-self-delete.log";
}

bool ParseSelfDeleteHelperArguments(int argc, wchar_t** argv, SelfDeleteHelperOptions& out,
                                    std::wstring& error) {
    bool helper = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--self-delete-helper") {
            helper = true;
            continue;
        }
        std::wstring value;
        if (argument == L"--mode") {
            if (!TakeValue(argc, argv, i, value)) {
                error = L"--mode needs a value";
                return false;
            }
            out.mode = SelfDeleteModeFromKey(value);
            if (out.mode == SelfDeleteMode::Count) {
                out.mode = SelfDeleteMode::Never;
            }
        } else if (argument == L"--exe") {
            if (!TakeValue(argc, argv, i, out.executable)) {
                error = L"--exe needs a value";
                return false;
            }
        } else if (argument == L"--data-dir") {
            if (!TakeValue(argc, argv, i, out.dataDirectory)) {
                error = L"--data-dir needs a value";
                return false;
            }
        } else if (argument == L"--wait-pid") {
            if (!TakeValue(argc, argv, i, value)) {
                error = L"--wait-pid needs a value";
                return false;
            }
            out.waitPids.push_back(static_cast<DWORD>(_wtoi(value.c_str())));
        } else if (argument == L"--run-value") {
            if (!TakeValue(argc, argv, i, value)) {
                error = L"--run-value needs a value";
                return false;
            }
            out.autostartValues.push_back(value);
        } else if (argument == L"--remove-file") {
            if (!TakeValue(argc, argv, i, value)) {
                error = L"--remove-file needs a value";
                return false;
            }
            out.extraFiles.push_back(value);
        } else if (argument == L"--wait-timeout") {
            if (!TakeValue(argc, argv, i, value)) {
                error = L"--wait-timeout needs a value";
                return false;
            }
            out.waitTimeoutMs = static_cast<DWORD>(_wtoi(value.c_str()));
        }
    }
    if (!helper) {
        return false;
    }
    if (out.executable.empty()) {
        // The helper runs from a %TEMP% copy, so it cannot infer which install to
        // remove.  Requiring the path also prevents an accidental hidden flag
        // from deleting the running copy.
        error = L"--exe is required";
        return false;
    }
    if (out.dataDirectory.empty()) {
        out.dataDirectory = SettingsDirectory();
    }
    error.clear();
    return true;
}

int RunSelfDeleteHelper(const SelfDeleteHelperOptions& options) {
    if (options.mode == SelfDeleteMode::Never) {
        return 0;
    }
    ::DeleteFileW(SelfDeleteLogPath().c_str());
    AppendLog(L"GhostFrame self-delete helper: mode=" + std::wstring(SelfDeleteModeKey(options.mode)));

    WaitForPids(options.waitPids, options.waitTimeoutMs);

    SelfDeletePaths paths;
    paths.executable = options.executable.empty() ? ModulePath() : options.executable;
    paths.dataDirectory = options.dataDirectory.empty() ? SettingsDirectory() : options.dataDirectory;
    paths.installDirectory = DirectoryOf(paths.executable);
    paths.generatedIcon = paths.dataDirectory.empty() ? std::wstring()
                                                      : paths.dataDirectory + L"\\GhostFrame.ico";

    const SelfDeletePlan plan = BuildSelfDeletePlan(options.mode, paths);
    std::vector<std::wstring> failures;

    for (const std::wstring& value : options.autostartValues) {
        if (!RemoveRunValue(value)) {
            failures.push_back(L"cannot remove the startup entry " + value);
        }
    }
    // A plain build always uses the executable stem; the caller normally passes
    // it, but removing it again is harmless.  A branded build must not touch a
    // separate GhostFrame installation's value.
    const std::wstring stem = FileNameOf(paths.executable);
    const std::wstring::size_type dot = stem.find_last_of(L'.');
    if (_wcsicmp((dot == std::wstring::npos ? stem : stem.substr(0, dot)).c_str(),
                 L"GhostFrame") == 0) {
        RemoveRunValue(L"GhostFrame");
    }

    std::wstring shortcutError;
    RemoveAllShortcuts(shortcutError);
    if (!shortcutError.empty()) {
        failures.push_back(shortcutError);
    }

    // Links that the installer recorded only in branding.json.
    for (const std::wstring& file : options.extraFiles) {
        if (IsShortcutPath(file)) {
            DeleteOneFile(file, 5000, failures);
        }
    }

    bool executableRemoved = false;
    const bool executableInData =
        IsUnder(plan.executable, paths.dataDirectory) && !paths.dataDirectory.empty();
    if (plan.mode == SelfDeleteMode::Everything && executableInData) {
        executableRemoved = DeleteOneFile(plan.executable, kExecutableDeleteTimeoutMs, failures);
    }

    for (const std::wstring& file : plan.files) {
        DeleteOneFile(file, 5000, failures);
    }

    for (const std::wstring& directory : plan.directories) {
        if (!IsSafeDataDirectory(directory)) {
            failures.push_back(L"refusing to remove an unsafe data directory " + directory);
            continue;
        }
        RemoveOneDirectory(directory, options.waitTimeoutMs + kExecutableDeleteTimeoutMs, failures);
    }

    if (!executableRemoved) {
        DeleteOneFile(plan.executable, kExecutableDeleteTimeoutMs, failures);
    }

    for (const std::wstring& directory : plan.emptyDirectories) {
        if (directory.empty() || directory.size() <= 3) {
            continue;
        }
        // Only remove it when empty; another application may own the directory.
        ::RemoveDirectoryW(directory.c_str());
    }

    if (failures.empty()) {
        AppendLog(L"self-delete completed");
    } else {
        AppendLog(L"self-delete finished with " + std::to_wstring(failures.size()) +
                  L" failure(s):");
        for (const std::wstring& failure : failures) {
            AppendLog(L"  " + failure);
        }
    }

    ScheduleHelperSelfRemoval();
    return failures.empty() ? 0 : 1;
}

bool StartSelfDeleteHelper(const SelfDeleteStartRequest& request, DWORD& helperPid,
                           std::wstring& error) {
    helperPid = 0;
    if (request.mode == SelfDeleteMode::Never) {
        error = L"self-delete is disabled";
        return false;
    }
    if (request.executablePath.empty()) {
        error = L"the executable path is missing";
        return false;
    }

    SelfDeleteHelperOptions options;
    options.mode = request.mode;
    options.executable = request.executablePath;
    options.dataDirectory = request.dataDirectory.empty() ? SettingsDirectory()
                                                           : request.dataDirectory;
    options.waitPids = request.waitPids;
    options.autostartValues = request.autostartValues;
    options.extraFiles = request.extraFiles;
    if (!options.executable.empty()) {
        const std::wstring stem = FileNameOf(options.executable);
        const std::wstring::size_type dot = stem.find_last_of(L'.');
        options.autostartValues.push_back(dot == std::wstring::npos ? stem : stem.substr(0, dot));
    }

    const std::wstring helperPath = TempDirectory() + L"GhostFrame-self-delete-" +
                                    std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                    std::to_wstring(::GetTickCount64()) + L".exe";
    ::DeleteFileW(helperPath.c_str());
    if (!::CopyFileW(request.executablePath.c_str(), helperPath.c_str(), FALSE)) {
        error = L"cannot stage the cleanup helper: " + Win32Text(::GetLastError());
        return false;
    }

    std::wstring commandLine = BuildHelperCommandLine(helperPath, options);
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const BOOL started = ::CreateProcessW(
        helperPath.c_str(), buffer.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, nullptr, nullptr, &startup,
        &process);
    if (!started) {
        error = L"cannot start the cleanup helper: " + Win32Text(::GetLastError());
        ::DeleteFileW(helperPath.c_str());
        return false;
    }
    helperPid = process.dwProcessId;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    error.clear();
    return true;
}

DWORD FindDaemonProcessId() {
    const HWND window = ::FindWindowW(kDaemonWindowClass, nullptr);
    if (window == nullptr) {
        return 0;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    return pid;
}

bool StopDaemonForSelfDelete(std::wstring& error) {
    DaemonClient client;
    std::wstring connectError;
    if (client.Connect(500, connectError)) {
        return client.Shutdown(error);
    }
    // The pipe can be temporarily unavailable (for example during startup).
    // Closing the daemon's message window still runs its normal shutdown path,
    // which restores the targets before the helper deletes anything.
    if (const HWND window = ::FindWindowW(kDaemonWindowClass, nullptr)) {
        ::PostMessageW(window, WM_CLOSE, 0, 0);
    }
    error.clear();
    return true;
}

bool DeleteFileWithRetry(const std::wstring& path, DWORD timeoutMs) {
    if (path.empty()) {
        return true;
    }
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        if (::DeleteFileW(path.c_str())) {
            return true;
        }
        const DWORD code = ::GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (code == ERROR_ACCESS_DENIED) {
            ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
        if (::GetTickCount64() >= deadline) {
            return false;
        }
        ::Sleep(kDeleteRetryStepMs);
    }
}

bool RemoveDirectoryTreeWithRetry(const std::wstring& path, DWORD timeoutMs) {
    if (path.empty()) {
        return true;
    }
    if (!IsSafeDataDirectory(path)) {
        // Callers also validate; this keeps an accidental `C:\` from ever
        // reaching the recursive delete.
        return false;
    }
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    return RemoveTree(path, deadline);
}

}  // namespace gf