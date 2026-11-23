#include "session.h"

#include <stdlib.h>
#include <string.h>

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
    if (!ValidateTarget(hwnd, error)) {
        return false;
    }
    if (IndexOf(hwnd) >= 0) {
        error = L"the window is already tracked";
        return false;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);

    ProcessInfo info;
    if (!DescribeProcess(pid, info, error)) {
        return false;
    }

    Session session;
    session.hwnd = hwnd;
    session.pid = pid;
    session.createTime = info.createTime;
    session.x86 = info.arch == Arch::X86;
    session.id = NewSessionId();

    if (!session.ipc.Create(pid, session.id)) {
        error = L"cannot create the shared state block: " + Win32Message(::GetLastError());
        return false;
    }

    if (!injector_.Inject(hwnd, ::GetCurrentProcessId(), session.id, InjectionStrategy::CallWndProc,
                          error)) {
        session.ipc.Close();
        return false;
    }

    bool attached = session.ipc.WaitForFlag(FlagAttached, 1600);
    if (!attached) {
        // Some environments filter WH_CALLWNDPROC; try the asynchronous hook.
        LogLine(L"WH_CALLWNDPROC bootstrap did not attach, retrying with WH_GETMESSAGE");
        if (injector_.Inject(hwnd, ::GetCurrentProcessId(), session.id,
                             InjectionStrategy::GetMessage, error)) {
            attached = session.ipc.WaitForFlag(FlagAttached, 2500);
        }
    }
    // The WH_GETMESSAGE hook must be released on both paths: after a successful
    // retry, and also when the first attempt succeeded but a pending hook was
    // installed by an earlier injection.  The agent de-duplicates repeated
    // bootstraps by (launcher pid, session id), so a late message is harmless.
    injector_.ReleasePendingHook();

    if (!attached) {
        session.ipc.SignalStop();
        session.ipc.Close();
        error = L"the target process refused the agent (protected process or security software?)";
        return false;
    }

    wchar_t title[196] = {};
    ::GetWindowTextW(hwnd, title, _countof(title) - 1);
    session.title = title[0] != L'\0' ? title : L"(untitled window)";

    session.status = static_cast<Status>(session.ipc.state()->status);
    session.flags = session.ipc.state()->flags;
    session.wda = session.ipc.state()->wda;
    session.lastSeq = session.ipc.state()->seq;

    LogLine(L"attached to '%s' (pid %u, %s)", session.title.c_str(), pid,
            session.x86 ? L"x86" : L"x64");

    sessions_.push_back(std::move(session));
    return true;
}

bool SessionManager::Send(size_t index, Cmd cmd, std::wstring& error) {
    if (index >= sessions_.size()) {
        error = L"no target selected";
        return false;
    }

    Session& session = sessions_[index];
    if (!session.ipc.valid()) {
        error = L"the target is no longer connected";
        return false;
    }

    if (!session.ipc.Send(cmd, 2000)) {
        error = L"the target did not acknowledge the command";
        return false;
    }

    session.status = static_cast<Status>(session.ipc.state()->status);
    session.flags = session.ipc.state()->flags;
    session.wda = session.ipc.state()->wda;
    session.lastError = session.ipc.state()->lastError;
    session.lastSeq = session.ipc.state()->seq;

    if (session.lastError != 0) {
        error = Win32Message(static_cast<DWORD>(session.lastError));
        return false;
    }
    return true;
}

void SessionManager::Remove(size_t index) {
    if (index >= sessions_.size()) {
        return;
    }
    Session& session = sessions_[index];
    if (session.ipc.valid()) {
        session.ipc.Send(Cmd::Uninstall, 1000);
        session.ipc.SignalStop();
    }
    session.ipc.Close();
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
}

void SessionManager::RestoreAll() {
    for (Session& session : sessions_) {
        if (session.ipc.valid()) {
            session.ipc.Send(Cmd::Uninstall, 1000);
            session.ipc.SignalStop();
        }
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

        if (state->seq != session.lastSeq) {
            session.lastSeq = state->seq;
            session.status = static_cast<Status>(state->status);
            session.flags = state->flags;
            session.wda = state->wda;
            session.lastError = state->lastError;
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
