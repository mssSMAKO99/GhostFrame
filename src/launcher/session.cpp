#include "session.h"

#include <stdlib.h>
#include <string.h>

#include <utility>

#include "log.h"

namespace gf {

namespace {

uint32_t NewSessionId() {
    FILETIME now = {};
    ::GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER value = {};
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;

    uint32_t id = static_cast<uint32_t>(value.QuadPart ^ (value.QuadPart >> 32));
    id ^= ::GetCurrentProcessId() * 2654435761u;
    id ^= ::GetTickCount();
    return id != 0 ? id : 1;
}

bool ProcessMatches(DWORD pid, ULONGLONG createTime) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }

    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    ULONGLONG actual = 0;
    if (::GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER value = {};
        value.LowPart = created.dwLowDateTime;
        value.HighPart = created.dwHighDateTime;
        actual = value.QuadPart;
    }
    ::CloseHandle(process);

    return actual == createTime;
}

// Consistent view of the agent-owned fields.  The agent publishes with a
// seqlock (odd `seq` while writing, even when done), so a snapshot that is
// taken while a command/guard tick is in flight is retried instead of being
// mixed with the previous values.
struct StateSnapshot {
    LONG status = 0;
    LONG flags = 0;
    LONG wda = 0;
    LONG lastError = 0;
    LONG seq = -1;
};

bool ReadSnapshot(const gf::SharedState* state, StateSnapshot& out) {
    if (state == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const LONG before = state->seq;
        if ((before & 1) != 0) {
            ::SwitchToThread();
            continue;
        }

        StateSnapshot snapshot;
        snapshot.status = state->status;
        snapshot.flags = state->flags;
        snapshot.wda = state->wda;
        snapshot.lastError = state->lastError;
        snapshot.seq = before;
        ::MemoryBarrier();
        if (state->seq == before) {
            out = snapshot;
            return true;
        }
    }
    return false;
}

void ApplySnapshot(Session& session, const StateSnapshot& snapshot) {
    session.status = static_cast<Status>(snapshot.status);
    session.flags = snapshot.flags;
    session.wda = snapshot.wda;
    session.lastError = snapshot.lastError;
    session.lastSeq = snapshot.seq;
}

}  // namespace

bool ValidateTarget(HWND hwnd, std::wstring& reason) {
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        reason = L"no valid window";
        return false;
    }
    if (!::IsWindowVisible(hwnd)) {
        reason = L"the window is not visible";
        return false;
    }
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd) {
        reason = L"only top-level windows are supported";
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        reason = L"cannot resolve the target process";
        return false;
    }
    if (pid == ::GetCurrentProcessId()) {
        reason = L"this window belongs to GhostFrame itself";
        return false;
    }
    if (hwnd == ::GetShellWindow() || hwnd == ::GetDesktopWindow()) {
        reason = L"system windows cannot be tracked";
        return false;
    }

    wchar_t className[128] = {};
    ::GetClassNameW(hwnd, className, _countof(className) - 1);
    static const wchar_t* const kExcluded[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd",
                                                L"Shell_SecondaryTrayWnd"};
    for (const wchar_t* name : kExcluded) {
        if (_wcsicmp(className, name) == 0) {
            reason = L"system windows cannot be tracked";
            return false;
        }
    }

    RECT bounds = {};
    if (::GetWindowRect(hwnd, &bounds)) {
        if ((bounds.right - bounds.left) < 16 || (bounds.bottom - bounds.top) < 16) {
            reason = L"the window is too small to be useful";
            return false;
        }
    }

    return true;
}

bool SessionManager::Attach(HWND hwnd, std::wstring& error) {
    if (IndexOf(hwnd) >= 0) {
        error = L"the window is already tracked";
        return false;
    }

    AttachResult result;
    if (!PrepareAttach(hwnd, injector_, result)) {
        error = result.error;
        return false;
    }

    sessions_.push_back(std::move(result.session));
    return true;
}

bool PrepareAttach(HWND hwnd, Injector& injector, AttachResult& result) {
    // Best-effort title, captured before validation so a batch summary can name
    // a window even when the attach fails.
    wchar_t title[196] = {};
    ::GetWindowTextW(hwnd, title, _countof(title) - 1);
    result.title = title[0] != L'\0' ? title : L"(untitled window)";

    if (!ValidateTarget(hwnd, result.error)) {
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);

    ProcessInfo info;
    if (!DescribeProcess(pid, info, result.error)) {
        return false;
    }

    Session& session = result.session;
    session.hwnd = hwnd;
    session.pid = pid;
    session.createTime = info.createTime;
    session.x86 = info.arch == Arch::X86;
    session.id = NewSessionId();

    if (!session.ipc.Create(pid, session.id)) {
        result.error = L"cannot create the shared state block: " + Win32Message(::GetLastError());
        return false;
    }

    if (!injector.Inject(hwnd, ::GetCurrentProcessId(), session.id, InjectionStrategy::CallWndProc,
                         result.error)) {
        session.ipc.Close();
        return false;
    }

    bool attached = session.ipc.WaitForFlag(FlagAttached, 1000);
    if (!attached) {
        // The agent rejects a window once its process has reached the worker
        // limit; that is final, so do not spend another 1.5 s on a retry.
        StateSnapshot probe;
        if (ReadSnapshot(session.ipc.state(), probe) &&
            static_cast<Status>(probe.status) == Status::Error &&
            probe.lastError == gf::kErrorTooManyTargets) {
            session.ipc.SignalStop();
            session.ipc.Close();
            result.error = L"too many targets in this process";
            return false;
        }
        // Some environments filter WH_CALLWNDPROC; try the asynchronous hook.
        LogLine(L"WH_CALLWNDPROC bootstrap did not attach, retrying with WH_GETMESSAGE");
        if (injector.Inject(hwnd, ::GetCurrentProcessId(), session.id,
                            InjectionStrategy::GetMessage, result.error)) {
            attached = session.ipc.WaitForFlag(FlagAttached, 1500);
        }
    }
    // The WH_GETMESSAGE hook must be released on both paths: after a successful
    // retry, and also when the first attempt succeeded but a pending hook was
    // installed by an earlier injection.  The agent de-duplicates repeated
    // bootstraps by (launcher pid, session id), so a late message is harmless.
    injector.ReleasePendingHook();

    if (!attached) {
        StateSnapshot probe;
        const bool limited = ReadSnapshot(session.ipc.state(), probe) &&
                             static_cast<Status>(probe.status) == Status::Error &&
                             probe.lastError == gf::kErrorTooManyTargets;
        session.ipc.SignalStop();
        session.ipc.Close();
        result.error = limited
                           ? L"too many targets in this process"
                           : L"the target process refused the agent (protected process or "
                             L"security software?)";
        return false;
    }

    session.title = result.title;

    StateSnapshot snapshot;
    if (ReadSnapshot(session.ipc.state(), snapshot)) {
        ApplySnapshot(session, snapshot);
    } else {
        session.status = static_cast<Status>(session.ipc.state()->status);
        session.flags = session.ipc.state()->flags;
        session.wda = session.ipc.state()->wda;
        session.lastSeq = session.ipc.state()->seq;
    }

    LogLine(L"attached to '%s' (pid %u, %s)", session.title.c_str(), pid,
            session.x86 ? L"x86" : L"x64");

    result.ok = true;
    return true;
}

SendOutcome SessionManager::Send(size_t index, Cmd cmd, std::wstring& error) {
    if (index >= sessions_.size()) {
        error = L"no target selected";
        return SendOutcome::Error;
    }
    return SendOne(sessions_[index], cmd, 500, error);
}

SendOutcome SessionManager::SendOne(Session& session, Cmd cmd, DWORD timeoutMs,
                                    std::wstring& error) {
    if (!session.ipc.valid()) {
        error = L"the target is no longer connected";
        return SendOutcome::Error;
    }

    if (!session.ipc.Send(cmd, timeoutMs)) {
        error = L"the target did not acknowledge the command";
        return SendOutcome::Timeout;
    }

    StateSnapshot snapshot;
    if (!ReadSnapshot(session.ipc.state(), snapshot)) {
        error = L"the target state is not readable";
        return SendOutcome::Error;
    }
    ApplySnapshot(session, snapshot);

    if (session.lastError != 0) {
        error = Win32Message(static_cast<DWORD>(session.lastError));
        return SendOutcome::Error;
    }
    return SendOutcome::Ok;
}

BatchSendResult SessionManager::SendToMany(const std::vector<size_t>& indices, Cmd cmd) {
    constexpr DWORD kPerTargetMs = 300;
    constexpr DWORD kBatchMs = 1500;

    BatchSendResult result;
    const ULONGLONG deadline = ::GetTickCount64() + kBatchMs;
    std::wstring ignored;
    for (size_t index : indices) {
        if (index >= sessions_.size()) {
            ++result.error;
            continue;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            // Out of budget: report the rest as timed out instead of blocking.
            ++result.timeout;
            continue;
        }
        const ULONGLONG remaining = deadline - now;
        const DWORD timeout =
            static_cast<DWORD>(remaining < kPerTargetMs ? remaining : kPerTargetMs);
        switch (SendOne(sessions_[index], cmd, timeout, ignored)) {
            case SendOutcome::Ok:
                ++result.ok;
                break;
            case SendOutcome::Timeout:
                ++result.timeout;
                break;
            case SendOutcome::Error:
                ++result.error;
                break;
        }
    }
    return result;
}

void SessionManager::Remove(size_t index) {
    if (index >= sessions_.size()) {
        return;
    }
    Session& session = sessions_[index];
    if (session.ipc.valid()) {
        session.ipc.Send(Cmd::Uninstall, 500);
        session.ipc.SignalStop();
    }
    session.ipc.Close();
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
}

void SessionManager::RestoreAll() {
    if (sessions_.empty()) {
        return;
    }

    // Queue the restore for every target first, without waiting for an
    // acknowledgement each.  `SignalStop` makes the agent restore even if it
    // never processes the command (a hung target), so a single busy window can
    // not stretch shutdown to seconds.
    for (Session& session : sessions_) {
        if (!session.ipc.valid()) {
            continue;
        }
        session.ipc.Send(Cmd::Uninstall, 0);
        session.ipc.SignalStop();
    }

    // Give all agents one shared, bounded slice (500 ms total) to detach before
    // the launcher drops its handles.
    const ULONGLONG deadline = ::GetTickCount64() + 500;
    for (;;) {
        bool pending = false;
        for (const Session& session : sessions_) {
            const gf::SharedState* state = session.ipc.state();
            if (state != nullptr && static_cast<Status>(state->status) != Status::Detached) {
                pending = true;
                break;
            }
        }
        if (!pending || ::GetTickCount64() >= deadline) {
            break;
        }
        ::Sleep(5);
    }

    for (Session& session : sessions_) {
        session.ipc.Close();
    }
    sessions_.clear();
}

bool SessionManager::Refresh() {
    bool changed = false;

    for (Session& session : sessions_) {
        if (!::IsWindow(session.hwnd)) {
            LogLine(L"target window disappeared (pid %u)", session.pid);
            session.lastSeq = -2;
            continue;
        }

        DWORD pid = 0;
        ::GetWindowThreadProcessId(session.hwnd, &pid);
        if (pid != session.pid || !ProcessMatches(session.pid, session.createTime)) {
            LogLine(L"target process changed for hwnd %p", session.hwnd);
            session.lastSeq = -2;
            continue;
        }

        gf::SharedState* state = session.ipc.state();
        if (state == nullptr) {
            session.lastSeq = -2;
            continue;
        }

        // Titles change independently of the agent's sequence (the app itself
        // rewrites them), so refresh them on every tick.
        wchar_t title[196] = {};
        const int titleLength = ::GetWindowTextW(session.hwnd, title, _countof(title) - 1);
        std::wstring current = titleLength > 0 ? std::wstring(title, titleLength)
                                               : std::wstring(L"(untitled window)");
        if (current != session.title) {
            session.title = std::move(current);
            changed = true;
        }

        StateSnapshot snapshot;
        if (!ReadSnapshot(state, snapshot)) {
            // A publish is in flight; retry on the next tick.
            continue;
        }

        if (snapshot.seq != session.lastSeq) {
            ApplySnapshot(session, snapshot);
            changed = true;
        }

        if (session.status == Status::Detached) {
            session.lastSeq = -2;
        }
    }

    const size_t before = sessions_.size();
    for (size_t i = sessions_.size(); i > 0; --i) {
        Session& session = sessions_[i - 1];
        if (session.lastSeq == -2) {
            session.ipc.Close();
            sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }

    return changed || sessions_.size() != before;
}

int SessionManager::IndexOf(HWND hwnd) const {
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i].hwnd == hwnd) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace gf
