#include "session.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "state_snapshot.h"

#include "ghostframe/session_id.h"

namespace gf {

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
    session.exePath = info.exePath;

    wchar_t className[128] = {};
    ::GetClassNameW(hwnd, className, _countof(className) - 1);
    session.className = className;

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
        detail::StateSnapshot probe;
        if (detail::ReadSnapshot(session.ipc.state(), probe) &&
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
        detail::StateSnapshot probe;
        const bool limited = detail::ReadSnapshot(session.ipc.state(), probe) &&
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

    detail::StateSnapshot snapshot;
    if (detail::ReadSnapshot(session.ipc.state(), snapshot)) {
        detail::ApplySnapshot(session, snapshot);
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

}  // namespace gf
