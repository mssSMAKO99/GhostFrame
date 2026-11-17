// In-process harness for the agent DLL.
//
// Cross-process injection cannot be exercised in every environment (some
// sandboxes and security products block it), but everything the agent does can
// be validated in-process: this host creates a window that keeps resetting its
// own display affinity, loads the agent, invokes its hook callback with a
// synthetic CWPSTRUCT (exactly what the hook chain would deliver) and then
// drives the shared-state protocol the way the launcher does.
//
// The window's owning thread must keep pumping: SetWindowLongPtr/SetWindowPos
// from the agent worker send messages to that thread, so a blocked UI thread
// would stall the agent.  A separate driver thread performs the checks while
// the main thread runs the message loop.
//
// It also verifies the lease/watchdog: a child process plays the launcher, and
// killing it must restore the window.

#include <windows.h>

#include <stdio.h>

#include <string>

#include "ghostframe/protocol.h"
#include "ipc_client.h"

namespace {

constexpr wchar_t kClassName[] = L"GfAgentHost";
constexpr UINT_PTR kResetTimer = 1;
constexpr DWORD kResetIntervalMs = 1000;
constexpr uint32_t kSessionId = 0x51E55101u;

bool g_pass = true;

struct Host {
    HWND window = nullptr;
    PROCESS_INFORMATION lease = {};
    HMODULE agent = nullptr;
    gf::SessionIpc ipc;
};

void Check(bool condition, const wchar_t* what) {
    wprintf(L"[%s] %s\n", condition ? L"ok  " : L"FAIL", what);
    if (!condition) {
        g_pass = false;
    }
}

DWORD CurrentAffinity(HWND hwnd) {
    DWORD value = 0;
    return ::GetWindowDisplayAffinity(hwnd, &value) ? value : 0xFFFFFFFFu;
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            ::SetTimer(hwnd, kResetTimer, kResetIntervalMs, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kResetTimer) {
                // Pretend to be an application that keeps undoing the exclusion.
                ::SetWindowDisplayAffinity(hwnd, WDA_NONE);
            }
            return 0;

        case WM_DESTROY:
            ::KillTimer(hwnd, kResetTimer);
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

DWORD WINAPI DriverThread(LPVOID parameter) {
    Host* host = static_cast<Host*>(parameter);
    const HWND window = host->window;

    Check(host->ipc.WaitForFlag(gf::FlagAttached, 3000), L"agent attached");

    Check(host->ipc.Send(gf::Cmd::Hide, 3000), L"hide acknowledged");
    Check(CurrentAffinity(window) == 0x11, L"window excluded from capture");
    Check((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0,
          L"taskbar style applied");

    int excluded = 0;
    for (int i = 0; i < 10; ++i) {
        ::Sleep(100);
        if (CurrentAffinity(window) == 0x11) {
            ++excluded;
        }
    }
    Check(excluded >= 6, L"guard keeps the window excluded while the app resets it");

    Check(host->ipc.Send(gf::Cmd::Show, 3000), L"show acknowledged");
    ::Sleep(250);
    Check(CurrentAffinity(window) == 0, L"capture affinity restored");
    Check((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0,
          L"taskbar style restored");

    Check(host->ipc.Send(gf::Cmd::BorderlessOn, 3000), L"borderless on acknowledged");
    Check((::GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == 0, L"caption removed");
    Check(host->ipc.Send(gf::Cmd::BorderlessOff, 3000), L"borderless off acknowledged");
    Check((::GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != 0, L"caption restored");

    Check(host->ipc.Send(gf::Cmd::Hide, 3000), L"hide before the lease test");
    Check(CurrentAffinity(window) == 0x11, L"excluded before lease loss");

    ::TerminateProcess(host->lease.hProcess, 0);
    ::Sleep(1800);
    Check(CurrentAffinity(window) == 0, L"lease loss restored the affinity");
    Check(host->ipc.state()->status == static_cast<LONG>(gf::Status::Detached),
          L"agent detached after lease loss");

    ::PostMessageW(window, WM_CLOSE, 0, 0);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--lease") == 0) {
        ::Sleep(120000);
        return 0;
    }

    if (argc < 2) {
        wprintf(L"usage: GfAgentHost <path-to-GhostFrameAgent64.dll>\n");
        return 2;
    }
    const std::wstring agentPath = argv[1];

    Host host;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &HostProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    if (!::RegisterClassExW(&windowClass)) {
        wprintf(L"cannot register the window class\n");
        return 2;
    }

    host.window = ::CreateWindowExW(0, kClassName, L"GhostFrame Agent Host",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 400, 260, nullptr,
                                    nullptr, instance, nullptr);
    if (host.window == nullptr) {
        wprintf(L"cannot create the host window\n");
        return 2;
    }
    ::ShowWindow(host.window, SW_SHOW);
    ::UpdateWindow(host.window);

    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t commandLine[MAX_PATH + 32] = {};
    swprintf_s(commandLine, L"\"%s\" --lease", self);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    if (!::CreateProcessW(self, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &host.lease)) {
        wprintf(L"cannot start the lease process\n");
        return 2;
    }
    ::CloseHandle(host.lease.hThread);

    host.agent = ::LoadLibraryExW(agentPath.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (host.agent == nullptr) {
        wprintf(L"cannot load the agent (error %u)\n", ::GetLastError());
        return 2;
    }

    using HookProcFn = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);
    auto hookProc = reinterpret_cast<HookProcFn>(::GetProcAddress(host.agent, "GfAgentHookProc"));
    if (hookProc == nullptr) {
        wprintf(L"the agent does not export GfAgentHookProc\n");
        return 2;
    }

    const UINT attachMessage = ::RegisterWindowMessageW(gf::kAttachMessageName);
    if (attachMessage == 0) {
        wprintf(L"RegisterWindowMessage failed\n");
        return 2;
    }

    Check(host.ipc.Create(::GetCurrentProcessId(), kSessionId), L"shared state created");

    CWPSTRUCT attach = {};
    attach.hwnd = host.window;
    attach.message = attachMessage;
    attach.wParam = static_cast<WPARAM>(host.lease.dwProcessId);
    attach.lParam = static_cast<LPARAM>(kSessionId);
    hookProc(HC_ACTION, 0, reinterpret_cast<LPARAM>(&attach));

    HANDLE driver = ::CreateThread(nullptr, 0, DriverThread, &host, 0, nullptr);

    MSG message = {};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (driver != nullptr) {
        ::WaitForSingleObject(driver, 5000);
        ::CloseHandle(driver);
    }

    if (host.lease.hProcess != nullptr) {
        ::TerminateProcess(host.lease.hProcess, 0);
        ::CloseHandle(host.lease.hProcess);
    }
    host.ipc.Close();
    ::FreeLibrary(host.agent);

    wprintf(L"%s\n", g_pass ? L"PASS" : L"FAIL");
    return g_pass ? 0 : 1;
}
