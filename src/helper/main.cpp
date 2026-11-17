// 32-bit bootstrap for 32-bit targets.
//
// A 64-bit launcher cannot load a 32-bit DLL, and SetWindowsHookEx requires the
// hook procedure to live in a DLL of the same bitness as the target.  This tiny
// executable loads the agent, installs the hook for the target thread, sends the
// bootstrap message and exits.  The agent keeps itself alive from there.
//
// Usage: GhostFrameHelper32.exe <hwnd> <launcherPid> <sessionId> <agentDllPath>
//
// Exit codes:
//   0            success
//   10           bad command line
//   11           LoadLibrary failed (Win32 error is reported separately)
//   12           export not found
//   13           SetWindowsHookEx failed
//   14           SendMessageTimeout failed
//   0x80000000|e Win32 failure with code e

#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>

#include "ghostframe/protocol.h"

namespace {

constexpr int kExitBadArgs = 10;
constexpr int kExitLoadLibrary = 11;
constexpr int kExitNoExport = 12;
constexpr int kExitHook = 13;
constexpr int kExitSend = 14;

bool ParseUnsigned(const wchar_t* text, unsigned long long* value) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = _wcstoui64(text, &end, 10);
    if (end == nullptr || *end != L'\0') {
        return false;
    }
    *value = parsed;
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return kExitBadArgs;
    }
    if (argc < 5) {
        ::LocalFree(argv);
        return kExitBadArgs;
    }

    unsigned long long hwndValue = 0;
    unsigned long long launcherPidValue = 0;
    unsigned long long sessionIdValue = 0;
    if (!ParseUnsigned(argv[1], &hwndValue) || !ParseUnsigned(argv[2], &launcherPidValue) ||
        !ParseUnsigned(argv[3], &sessionIdValue)) {
        ::LocalFree(argv);
        return kExitBadArgs;
    }

    wchar_t agentPath[MAX_PATH] = {};
    wcscpy_s(agentPath, argv[4]);
    ::LocalFree(argv);

    if (!::IsWindow(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndValue)))) {
        return kExitBadArgs;
    }

    const UINT attachMessage = ::RegisterWindowMessageW(gf::kAttachMessageName);
    if (attachMessage == 0) {
        return kExitSend;
    }

    HMODULE agent = ::LoadLibraryExW(
        agentPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (agent == nullptr) {
        return kExitLoadLibrary;
    }

    HOOKPROC hook = reinterpret_cast<HOOKPROC>(::GetProcAddress(agent, "GfAgentHookProc"));
    if (hook == nullptr) {
        ::FreeLibrary(agent);
        return kExitNoExport;
    }

    const DWORD threadId = ::GetWindowThreadProcessId(reinterpret_cast<HWND>(
                                                            static_cast<ULONG_PTR>(hwndValue)),
                                                        nullptr);
    if (threadId == 0) {
        ::FreeLibrary(agent);
        return kExitHook;
    }

    HHOOK installed = ::SetWindowsHookExW(WH_CALLWNDPROC, hook, agent, threadId);
    if (installed == nullptr) {
        ::FreeLibrary(agent);
        return kExitHook;
    }

    DWORD_PTR result = 0;
    const LRESULT sent = ::SendMessageTimeoutW(
        reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndValue)), attachMessage,
        static_cast<WPARAM>(launcherPidValue), static_cast<LPARAM>(sessionIdValue),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);

    const DWORD sendError = ::GetLastError();
    ::UnhookWindowsHookEx(installed);

    if (sent != 0) {
        ::FreeLibrary(agent);
        return 0;
    }

    // Fallback: asynchronous WH_GETMESSAGE, the same trick the launcher uses
    // for 64-bit targets when WH_CALLWNDPROC is filtered.
    HHOOK asynchronous = ::SetWindowsHookExW(WH_GETMESSAGE, hook, agent, threadId);
    if (asynchronous != nullptr) {
        const BOOL posted =
            ::PostMessageW(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(hwndValue)), attachMessage,
                           static_cast<WPARAM>(launcherPidValue),
                           static_cast<LPARAM>(sessionIdValue));
        ::Sleep(600);
        ::UnhookWindowsHookEx(asynchronous);
        if (posted) {
            ::FreeLibrary(agent);
            return 0;
        }
    }

    ::FreeLibrary(agent);
    return static_cast<int>(0x80000000u | (sendError != 0 ? sendError : ERROR_TIMEOUT));
}
