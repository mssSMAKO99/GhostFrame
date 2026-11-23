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
constexpr int kHandshakeIterations = 40;

using HookProcFn = LRESULT(CALLBACK*)(int, WPARAM, LPARAM);

bool g_pass = true;
volatile LONG g_resetAffinity = 1;

struct Host {
    HWND window = nullptr;
    HWND secondWindow = nullptr;
    PROCESS_INFORMATION lease = {};
    HMODULE agent = nullptr;
    gf::SessionIpc ipc;
    HookProcFn hookProc = nullptr;
    UINT attachMessage = 0;
    uint32_t nextSessionId = kSessionId + 1;
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

// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires a fully qualified path, so resolve
// relative arguments (as used in the README) before loading the agent.
std::wstring AbsolutePath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return path;
    }
    return buffer;
}

// Delivers the same CWPSTRUCT the WH_CALLWNDPROC/WH_GETMESSAGE chain would
// hand to the agent.  Calling it twice with the same session id simulates the
// launcher's bootstrap retry.
void DeliverAttach(const Host& host, HWND window, uint32_t sessionId) {
    CWPSTRUCT attach = {};
    attach.hwnd = window;
    attach.message = host.attachMessage;
    attach.wParam = static_cast<WPARAM>(host.lease.dwProcessId);
    attach.lParam = static_cast<LPARAM>(sessionId);
    host.hookProc(HC_ACTION, 0, reinterpret_cast<LPARAM>(&attach));
}

bool AttachSession(Host& host, HWND window, uint32_t sessionId, gf::SessionIpc& ipc) {
    if (!ipc.Create(::GetCurrentProcessId(), sessionId)) {
        return false;
    }
    DeliverAttach(host, window, sessionId);
    return ipc.WaitForFlag(gf::FlagAttached, 3000);
}

bool WaitForDetached(const gf::SessionIpc& ipc, DWORD timeoutMs) {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        const gf::SharedState* state = ipc.state();
        if (state != nullptr && state->status == static_cast<LONG>(gf::Status::Detached)) {
            return true;
        }
        if (::GetTickCount64() >= deadline) {
            return false;
        }
        ::Sleep(10);
    }
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            ::SetTimer(hwnd, kResetTimer, kResetIntervalMs, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kResetTimer && g_resetAffinity) {
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

    // 1.6: the agent must never touch the window frame.  Capture the style,
    // ex-style and rect, hide and show the window again, and require them to
    // come back unchanged (only the taskbar ex-style bits may differ, and they
    // were just restored above).
    const LONG_PTR styleBefore = ::GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR exStyleBefore = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    RECT rectBefore = {};
    ::GetWindowRect(window, &rectBefore);

    Check(host->ipc.Send(gf::Cmd::Hide, 3000), L"hide for the frame check acknowledged");
    Check(host->ipc.Send(gf::Cmd::Show, 3000), L"show for the frame check acknowledged");
    ::Sleep(250);

    RECT rectAfter = {};
    ::GetWindowRect(window, &rectAfter);
    Check(::GetWindowLongPtrW(window, GWL_STYLE) == styleBefore, L"frame style untouched");
    Check((::GetWindowLongPtrW(window, GWL_EXSTYLE) &
           ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_TOOLWINDOW)) ==
              (exStyleBefore & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_TOOLWINDOW)),
          L"non-taskbar ex-style untouched");
    Check(rectBefore.left == rectAfter.left && rectBefore.top == rectAfter.top &&
              rectBefore.right == rectAfter.right && rectBefore.bottom == rectAfter.bottom,
          L"window rect untouched");

    // From here on affinity values are compared exactly, so stop the window's
    // own reset timer from racing the checks.
    g_resetAffinity = 0;

    // 1.1: a repeated bootstrap for the same (launcher pid, session id) must
    // not start a second worker.  Such a worker would save 0x11 as the
    // "original" affinity and leave the window excluded after Uninstall.
    Check(host->ipc.Send(gf::Cmd::Hide, 3000), L"hide before the duplicate bootstrap");
    Check(CurrentAffinity(window) == 0x11, L"excluded before the duplicate bootstrap");

    DeliverAttach(*host, window, kSessionId);
    ::Sleep(250);
    Check(host->ipc.Send(gf::Cmd::Hide, 3000),
          L"duplicate bootstrap keeps the command path alive");

    host->ipc.Send(gf::Cmd::Uninstall, 200);
    Check(WaitForDetached(host->ipc, 3000), L"agent detached after the duplicate bootstrap");
    // Give a surviving duplicate worker (if any) a couple of guard ticks to
    // re-exclude the window before checking the restored value.
    ::Sleep(500);
    Check(CurrentAffinity(window) == 0,
          L"duplicate bootstrap still restores the original affinity");
    host->ipc.Close();

    // 1.1: after Uninstall the registry entry is gone, so a fresh session for
    // the same window must attach normally.
    gf::SessionIpc second;
    const uint32_t secondId = host->nextSessionId++;
    Check(AttachSession(*host, window, secondId, second), L"new session attaches after uninstall");
    Check(second.Send(gf::Cmd::Hide, 3000), L"new session hide acknowledged");
    Check(CurrentAffinity(window) == 0x11, L"new session excludes the window");
    second.Send(gf::Cmd::Uninstall, 200);
    Check(WaitForDetached(second, 3000), L"new session detached");
    ::Sleep(300);
    Check(CurrentAffinity(window) == 0, L"new session restores the original affinity");
    second.Close();

    // 1.2: the launcher sends Hide as soon as it observes FlagAttached.  The
    // agent must read cmdSeq before publishing Attached, otherwise the command
    // is treated as already applied and lost.
    int handshakeFailures = 0;
    for (int i = 0; i < kHandshakeIterations; ++i) {
        gf::SessionIpc ipc;
        const uint32_t id = host->nextSessionId++;
        if (!ipc.Create(::GetCurrentProcessId(), id)) {
            wprintf(L"      handshake iteration %d: cannot create the state block\n", i);
            ++handshakeFailures;
            break;
        }

        DeliverAttach(*host, window, id);
        const bool attached = ipc.WaitForFlag(gf::FlagAttached, 2000);
        const bool acked = attached && ipc.Send(gf::Cmd::Hide, 2000);
        if (!acked || CurrentAffinity(window) != 0x11) {
            wprintf(L"      handshake iteration %d: attached=%d acked=%d affinity=0x%X\n", i,
                    attached ? 1 : 0, acked ? 1 : 0, CurrentAffinity(window));
            ++handshakeFailures;
        }

        ipc.SignalStop();
        if (!WaitForDetached(ipc, 2000)) {
            ++handshakeFailures;
        }
        ipc.Close();
    }
    Check(handshakeFailures == 0,
          L"hide immediately after attach acknowledged in every iteration");

    // 1.1: the de-duplication key is (launcher pid, session id), not hwnd.  Two
    // different windows of the same process (and the same launcher) must be
    // attachable at the same time.
    gf::SessionIpc twinA;
    gf::SessionIpc twinB;
    const uint32_t twinAId = host->nextSessionId++;
    const uint32_t twinBId = host->nextSessionId++;
    Check(AttachSession(*host, window, twinAId, twinA), L"first twin session attached");
    Check(AttachSession(*host, host->secondWindow, twinBId, twinB),
          L"second session of the same process attached");
    Check(twinA.Send(gf::Cmd::Hide, 3000) && twinB.Send(gf::Cmd::Hide, 3000),
          L"both twin sessions hid their windows");
    Check(CurrentAffinity(window) == 0x11 && CurrentAffinity(host->secondWindow) == 0x11,
          L"both twin windows are excluded");
    twinA.Send(gf::Cmd::Uninstall, 200);
    twinB.Send(gf::Cmd::Uninstall, 200);
    Check(WaitForDetached(twinA, 3000) && WaitForDetached(twinB, 3000),
          L"both twin sessions detached");
    ::Sleep(400);
    Check(CurrentAffinity(window) == 0 && CurrentAffinity(host->secondWindow) == 0,
          L"both twin windows restored their affinity");
    twinA.Close();
    twinB.Close();

    // Lease/watchdog test must be last: it kills the process that plays the
    // launcher, so no later bootstrap can attach.
    gf::SessionIpc leased;
    const uint32_t leasedId = host->nextSessionId++;
    Check(AttachSession(*host, window, leasedId, leased), L"session for the lease test attached");
    Check(leased.Send(gf::Cmd::Hide, 3000), L"hide before the lease test");
    Check(CurrentAffinity(window) == 0x11, L"excluded before lease loss");

    ::TerminateProcess(host->lease.hProcess, 0);
    ::Sleep(1800);
    Check(CurrentAffinity(window) == 0, L"lease loss restored the affinity");
    Check(leased.state()->status == static_cast<LONG>(gf::Status::Detached),
          L"agent detached after lease loss");
    leased.Close();

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
    const std::wstring agentPath = AbsolutePath(argv[1]);

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

    // A second window in the same process, used to prove that the agent's
    // de-duplication key is the session, not the window handle.
    host.secondWindow = ::CreateWindowExW(0, kClassName, L"GhostFrame Agent Host (second)",
                                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, 520, 80, 320, 200,
                                          nullptr, nullptr, instance, nullptr);
    if (host.secondWindow == nullptr) {
        wprintf(L"cannot create the second host window\n");
        return 2;
    }
    ::ShowWindow(host.secondWindow, SW_SHOW);
    ::UpdateWindow(host.secondWindow);

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

    host.hookProc = reinterpret_cast<HookProcFn>(::GetProcAddress(host.agent, "GfAgentHookProc"));
    if (host.hookProc == nullptr) {
        wprintf(L"the agent does not export GfAgentHookProc\n");
        return 2;
    }

    host.attachMessage = ::RegisterWindowMessageW(gf::kAttachMessageName);
    if (host.attachMessage == 0) {
        wprintf(L"RegisterWindowMessage failed\n");
        return 2;
    }

    Check(host.ipc.Create(::GetCurrentProcessId(), kSessionId), L"shared state created");
    DeliverAttach(host, host.window, kSessionId);

    HANDLE driver = ::CreateThread(nullptr, 0, DriverThread, &host, 0, nullptr);

    MSG message = {};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    if (driver != nullptr) {
        ::WaitForSingleObject(driver, 60000);
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
