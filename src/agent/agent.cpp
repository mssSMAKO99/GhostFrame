// In-target agent.
//
// The launcher injects this DLL with SetWindowsHookEx(WH_CALLWNDPROC) and sends
// a registered message.  The hook callback runs on the target's UI thread, so
// this is the only place where SetWindowDisplayAffinity is allowed to be called
// for the target window (the API requires the window to belong to the calling
// process).
//
// The callback only bootstraps a worker thread and returns immediately; all the
// real work happens there.  The worker owns the reversible state, re-asserts the
// invariants while hidden, and tears everything down when the launcher dies or
// explicitly asks for an uninstall.

#include <windows.h>

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string>

#include "agent_ipc.h"
#include "ghostframe/protocol.h"
#include "window_ops.h"

#ifdef _DEBUG
#define GF_AGENT_TRACE 1
#endif

namespace {

constexpr int kMaxSessions = 8;
constexpr DWORD kGuardTickMs = 250;

// Active sessions, keyed by (launcher pid, session id).  The launcher retries
// the bootstrap with WH_GETMESSAGE when WH_CALLWNDPROC looks slow, and the
// 32-bit helper has its own internal retry, so the same attach message can be
// delivered more than once.  A second worker for the same session would save
// the already-excluded affinity (0x11) as the "original" value and restore it
// later, leaving the window hidden after Restore all.  Registration is done
// with a static SRWLOCK; no DllMain work is required.
struct SessionKey {
    DWORD launcherPid = 0;
    uint32_t sessionId = 0;
};

SRWLOCK g_sessionLock = SRWLOCK_INIT;
SessionKey g_sessions[kMaxSessions] = {};

bool RegisterSession(DWORD launcherPid, uint32_t sessionId) {
    bool registered = false;
    ::AcquireSRWLockExclusive(&g_sessionLock);
    int freeSlot = -1;
    for (int i = 0; i < kMaxSessions; ++i) {
        if (g_sessions[i].sessionId == 0) {
            if (freeSlot < 0) {
                freeSlot = i;
            }
        } else if (g_sessions[i].launcherPid == launcherPid &&
                   g_sessions[i].sessionId == sessionId) {
            ::ReleaseSRWLockExclusive(&g_sessionLock);
            return false;  // duplicate bootstrap, not an error
        }
    }
    if (freeSlot >= 0) {
        g_sessions[freeSlot].launcherPid = launcherPid;
        g_sessions[freeSlot].sessionId = sessionId;
        registered = true;
    }
    ::ReleaseSRWLockExclusive(&g_sessionLock);
    return registered;
}

void UnregisterSession(DWORD launcherPid, uint32_t sessionId) {
    ::AcquireSRWLockExclusive(&g_sessionLock);
    for (int i = 0; i < kMaxSessions; ++i) {
        if (g_sessions[i].launcherPid == launcherPid &&
            g_sessions[i].sessionId == sessionId) {
            g_sessions[i] = SessionKey{};
            break;
        }
    }
    ::ReleaseSRWLockExclusive(&g_sessionLock);
}

#ifdef GF_AGENT_TRACE
void Trace(const wchar_t* format, ...) {
    wchar_t message[1024] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    const std::wstring path = std::wstring(temp) + L"GhostFrame.agent.log";

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    wchar_t line[1200] = {};
    const int length = swprintf_s(line, L"[pid %u] %s\r\n", ::GetCurrentProcessId(), message);
    DWORD written = 0;
    if (length > 0) {
        ::WriteFile(file, line, static_cast<DWORD>(length) * sizeof(wchar_t), &written, nullptr);
    }
    ::CloseHandle(file);
}
#define GF_TRACE(...) Trace(__VA_ARGS__)
#else
#define GF_TRACE(...) ((void)0)
#endif

UINT AttachMessage() {
    static const UINT id = ::RegisterWindowMessageW(gf::kAttachMessageName);
    return id;
}

struct WorkerContext {
    HWND hwnd;
    DWORD targetPid;
    DWORD launcherPid;
    uint32_t sessionId;
    HMODULE self;
};

bool StartWorker(HWND hwnd, DWORD launcherPid, uint32_t sessionId);
bool ApplyCommand(gf::agent::WindowState& window, gf::agent::Taskbar& taskbar,
                  gf::agent::AgentIpc& ipc, gf::Cmd cmd, LONG seq);
void RunWorker(const WorkerContext& context);
DWORD WINAPI WorkerThread(LPVOID parameter);

}  // namespace

extern "C" LRESULT CALLBACK GfAgentHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam != 0) {
        const UINT attach = AttachMessage();
        if (attach != 0) {
            const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(lParam);
            if (message->message == attach && message->hwnd != nullptr) {
                GF_TRACE(L"hook: attach hwnd=%p launcher=%u session=%u", message->hwnd,
                         static_cast<unsigned>(message->wParam),
                         static_cast<unsigned>(message->lParam));
                StartWorker(message->hwnd, static_cast<DWORD>(message->wParam),
                            static_cast<uint32_t>(message->lParam));
            }
        }
    }
    return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
}

namespace {

bool StartWorker(HWND hwnd, DWORD launcherPid, uint32_t sessionId) {
    if (hwnd == nullptr || launcherPid == 0 || sessionId == 0) {
        GF_TRACE(L"start: bad arguments");
        return false;
    }
    if (launcherPid == ::GetCurrentProcessId()) {
        GF_TRACE(L"start: launcher pid equals target pid");
        return false;
    }
    if (!RegisterSession(launcherPid, sessionId)) {
        // Either a duplicate bootstrap for this session or the process has no
        // free worker slot; both are rejected without touching existing state.
        GF_TRACE(L"start: duplicate session or session limit reached");
        return false;
    }

    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (root == nullptr) {
        UnregisterSession(launcherPid, sessionId);
        return false;
    }

    HMODULE self = nullptr;
#pragma warning(push)
#pragma warning(disable : 4191)  // function pointer -> LPCWSTR, Win32 idiom
    const BOOL found = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                            reinterpret_cast<LPCWSTR>(&GfAgentHookProc), &self);
#pragma warning(pop)
    if (!found) {
        GF_TRACE(L"start: GetModuleHandleEx failed");
        UnregisterSession(launcherPid, sessionId);
        return false;
    }

    WorkerContext* context = new (std::nothrow)
        WorkerContext{root, ::GetCurrentProcessId(), launcherPid, sessionId, self};
    if (context == nullptr) {
        ::FreeLibrary(self);
        UnregisterSession(launcherPid, sessionId);
        return false;
    }

    HANDLE thread = ::CreateThread(nullptr, 0, WorkerThread, context, 0, nullptr);
    if (thread == nullptr) {
        GF_TRACE(L"start: CreateThread failed");
        delete context;
        ::FreeLibrary(self);
        UnregisterSession(launcherPid, sessionId);
        return false;
    }

    GF_TRACE(L"start: worker created for hwnd=%p", root);
    ::CloseHandle(thread);
    return true;
}

bool ApplyCommand(gf::agent::WindowState& window, gf::agent::Taskbar& taskbar,
                  gf::agent::AgentIpc& ipc, gf::Cmd cmd, LONG seq) {
    DWORD error = 0;
    LONG setFlags = 0;
    LONG clearFlags = 0;
    gf::Status status = gf::Status::Attached;

    switch (cmd) {
    case gf::Cmd::Hide:
        error = window.SetHidden(true, taskbar);
        setFlags = gf::FlagHidden;
        status = error != 0 ? gf::Status::Error : gf::Status::Hidden;
        break;
    case gf::Cmd::Show:
        error = window.SetHidden(false, taskbar);
        clearFlags = gf::FlagHidden;
        status = error != 0 ? gf::Status::Error : gf::Status::Visible;
        break;
    case gf::Cmd::BorderlessOn:
        error = window.SetBorderless(true);
        setFlags = gf::FlagBorderless;
        status = error != 0 ? gf::Status::Error : gf::Status::BorderlessOn;
        break;
    case gf::Cmd::BorderlessOff:
        error = window.SetBorderless(false);
        clearFlags = gf::FlagBorderless;
        status = error != 0 ? gf::Status::Error : gf::Status::BorderlessOff;
        break;
    case gf::Cmd::RestoreAll:
        error = window.RestoreAll(taskbar);
        clearFlags = gf::FlagHidden | gf::FlagBorderless;
        status = error != 0 ? gf::Status::Error : gf::Status::Visible;
        break;
    case gf::Cmd::Ping:
        ipc.Ack(seq);
        return true;
    case gf::Cmd::Uninstall:
        return false;
    case gf::Cmd::None:
    default:
        ipc.Ack(seq);
        return true;
    }

    ipc.Publish(status, setFlags, clearFlags, window.CurrentWda(), static_cast<LONG>(error), seq);
    return true;
}

void RunWorker(const WorkerContext& context) {
    gf::agent::AgentIpc ipc;
    if (!ipc.Open(context.targetPid, context.sessionId)) {
        GF_TRACE(L"worker: cannot open IPC for session %u", context.sessionId);
        return;
    }

    HANDLE lease = ::OpenProcess(SYNCHRONIZE, FALSE, context.launcherPid);
    if (lease == nullptr) {
        GF_TRACE(L"worker: cannot open launcher process %u", context.launcherPid);
        ipc.Publish(gf::Status::Error, 0, 0, 0, static_cast<LONG>(::GetLastError()), -1);
        return;
    }

    gf::agent::WindowState window;
    window.Init(context.hwnd);

    gf::agent::Taskbar taskbar;
    taskbar.Init();

    // Read the sequence and the cached state *before* announcing the attach.
    // The launcher sends its first command as soon as it sees FlagAttached; a
    // sequence read after the publish would already include that command and
    // the worker would consider it applied (the command would be lost).
    LONG lastSeq = ipc.state()->cmdSeq;
    LONG lastWda = static_cast<LONG>(window.CurrentWda());
    LONG lastError = 0;

    ipc.Publish(gf::Status::Attached, gf::FlagAttached,
                gf::FlagHidden | gf::FlagBorderless | gf::FlagLeaseLost, window.CurrentWda(), 0,
                -1);

    bool running = true;

    HANDLE waits[3] = {lease, ipc.stop_event(), ipc.cmd_event()};

    while (running) {
        const DWORD wait = ::WaitForMultipleObjects(3, waits, FALSE, kGuardTickMs);

        if (wait == WAIT_OBJECT_0) {
            // The launcher exited (or was killed): restore and unload.
            ipc.Publish(gf::Status::Leaving, gf::FlagLeaseLost, 0, window.CurrentWda(), 0, -1);
            break;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            break;
        }
        if (wait == WAIT_OBJECT_0 + 2) {
            const LONG seq = ipc.state()->cmdSeq;
            if (seq != lastSeq) {
                lastSeq = seq;
                const gf::Cmd cmd = static_cast<gf::Cmd>(ipc.state()->cmd);
                if (!ApplyCommand(window, taskbar, ipc, cmd, seq)) {
                    running = false;
                    break;
                }
                lastWda = static_cast<LONG>(window.CurrentWda());
                lastError = 0;
            }
        }

        if (!window.IsAlive()) {
            // The window is gone; nothing left to restore.
            running = false;
            break;
        }

        const DWORD error = window.Enforce();
        const DWORD wda = window.CurrentWda();
        if (static_cast<LONG>(wda) != lastWda || static_cast<LONG>(error) != lastError) {
            ipc.PublishAffinity(wda, static_cast<LONG>(error));
            lastWda = static_cast<LONG>(wda);
            lastError = static_cast<LONG>(error);
        }
    }

    ipc.Publish(gf::Status::Leaving, 0, 0, window.CurrentWda(), 0, -1);
    window.RestoreAll(taskbar);
    ipc.Publish(gf::Status::Detached, 0,
                gf::FlagAttached | gf::FlagHidden | gf::FlagBorderless, window.CurrentWda(), 0,
                ipc.state()->cmdSeq);

    taskbar.Shutdown();
    ::CloseHandle(lease);
    ipc.Close();
}

DWORD WINAPI WorkerThread(LPVOID parameter) {
    WorkerContext* context = static_cast<WorkerContext*>(parameter);
    const HMODULE self = context->self;
    const DWORD launcherPid = context->launcherPid;
    const uint32_t sessionId = context->sessionId;

    RunWorker(*context);

    // Free the slot only after the worker is completely done restoring, so a
    // retried bootstrap while the worker still runs stays a no-op.
    UnregisterSession(launcherPid, sessionId);
    delete context;
    ::FreeLibraryAndExitThread(self, 0);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
