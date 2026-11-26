#include "launch.h"

#include <windows.h>

#include <vector>

#include "ghostframe/ipc_protocol.h"
#include "ghostframe/protocol.h"
#include "ipc/daemon_client.h"
#include "log.h"
#include "session.h"

namespace gf {

namespace {

std::wstring ResolveLogDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
        std::wstring dir = std::wstring(localAppData) + L"\\GhostFrame";
        if (::CreateDirectoryW(dir.c_str(), nullptr) ||
            ::GetLastError() == ERROR_ALREADY_EXISTS) {
            return dir;
        }
    }

    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) != 0) {
        return std::wstring(temp) + L"GhostFrame";
    }
    return {};
}

struct FindContext {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<FindContext*>(param);
    if (context->found != nullptr || hwnd == ::GetShellWindow()) {
        return FALSE;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != context->pid) {
        return TRUE;
    }
    if (::GetWindow(hwnd, GW_OWNER) != nullptr || ::IsWindowVisible(hwnd) == FALSE) {
        return TRUE;
    }

    wchar_t title[8] = {};
    if (::GetWindowTextW(hwnd, title, _countof(title)) <= 0) {
        return TRUE;
    }

    std::wstring reason;
    if (!ValidateTarget(hwnd, reason)) {
        return TRUE;
    }

    context->found = hwnd;
    return FALSE;
}

HWND WaitForMainWindow(DWORD pid, DWORD timeoutMs) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (::GetTickCount64() < deadline) {
        FindContext context;
        context.pid = pid;
        ::EnumWindows(&FindWindowProc, reinterpret_cast<LPARAM>(&context));
        if (context.found != nullptr) {
            return context.found;
        }
        ::Sleep(100);
    }
    return nullptr;
}

}  // namespace

int RunLaunch(const std::wstring& application, bool capture, bool hide) {
    LogInit(ResolveLogDirectory());
    LogLine(L"launch requested: %s", application.c_str());

    DaemonClient client;
    std::wstring error;
    if (!client.EnsureRunning(error)) {
        LogLine(L"launch: %s", error.c_str());
        LogShutdown();
        return 1;
    }

    std::wstring commandLine = L"\"" + application + L"\"";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!::CreateProcessW(application.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          nullptr, &startup, &process)) {
        LogLine(L"launch: cannot start %s: %s", application.c_str(),
                Win32Message(::GetLastError()).c_str());
        LogShutdown();
        return 1;
    }

    ::CloseHandle(process.hThread);
    ::WaitForInputIdle(process.hProcess, 10000);

    int exitCode = 0;
    if (capture) {
        const HWND target = WaitForMainWindow(process.dwProcessId, 10000);
        if (target == nullptr) {
            LogLine(L"launch: no capturable window appeared for %s", application.c_str());
            exitCode = 1;
        } else {
            AttachReply attached = {};
            if (!client.Attach(reinterpret_cast<uint64_t>(target), attached, error) ||
                attached.ok == 0) {
                const std::wstring reason = error.empty() ? std::wstring(attached.error) : error;
                LogLine(L"launch: capture failed for %s: %s", application.c_str(), reason.c_str());
                exitCode = 1;
            } else if (hide) {
                SendReply sent = {};
                client.Send(reinterpret_cast<uint64_t>(target), Cmd::Hide, sent, error);
                LogLine(L"launch: captured and hidden %s", application.c_str());
            } else {
                LogLine(L"launch: captured %s", application.c_str());
            }
        }
    } else {
        LogLine(L"launch: started %s", application.c_str());
    }

    ::CloseHandle(process.hProcess);
    LogShutdown();
    return exitCode;
}

}  // namespace gf