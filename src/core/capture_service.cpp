#include "capture_service.h"

#include <algorithm>
#include <cwchar>
#include <utility>

#include "log.h"
#include "state_snapshot.h"

namespace gf {

namespace {

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

bool CaptureService::Initialize(std::wstring& error) {
    return EnsureInitialized(error);
}

bool CaptureService::EnsureInitialized(std::wstring& error) {
    if (initialized_) {
        return true;
    }
    if (!injector_.Initialize(error)) {
        return false;
    }
    initialized_ = true;
    return true;
}

void CaptureService::Shutdown() {
    injector_.Shutdown();
    initialized_ = false;
}

bool CaptureService::Attach(HWND hwnd, std::wstring& error) {
    if (IndexOf(hwnd) >= 0) {
        error = L"the window is already tracked";
        return false;
    }

    if (!EnsureInitialized(error)) {
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

void CaptureService::Add(Session&& session) {
    sessions_.push_back(std::move(session));
}

SendOutcome CaptureService::Send(size_t index, Cmd cmd, std::wstring& error) {
    if (index >= sessions_.size()) {
        error = L"no target selected";
        return SendOutcome::Error;
    }
    return SendOne(sessions_[index], cmd, 500, error);
}

SendOutcome CaptureService::SendOne(Session& session, Cmd cmd, DWORD timeoutMs,
                                    std::wstring& error) {
    if (!session.ipc.valid()) {
        error = L"the target is no longer connected";
        return SendOutcome::Error;
    }

    if (!session.ipc.Send(cmd, timeoutMs)) {
        error = L"the target did not acknowledge the command";
        return SendOutcome::Timeout;
    }

    detail::StateSnapshot snapshot;
    if (!detail::ReadSnapshot(session.ipc.state(), snapshot)) {
        error = L"the target state is not readable";
        return SendOutcome::Error;
    }
    detail::ApplySnapshot(session, snapshot);

    if (session.lastError != 0) {
        error = Win32Message(static_cast<DWORD>(session.lastError));
        return SendOutcome::Error;
    }
    return SendOutcome::Ok;
}

BatchSendResult CaptureService::SendToMany(const std::vector<size_t>& indices, Cmd cmd) {
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

void CaptureService::Remove(size_t index) {
    if (index >= sessions_.size()) {
        return;
    }
    Session& session = sessions_[index];
    if (primaryId_ != 0 && session.id == primaryId_) {
        primaryId_ = 0;
    }
    if (session.ipc.valid()) {
        session.ipc.Send(Cmd::Uninstall, 500);
        session.ipc.SignalStop();
    }
    session.ipc.Close();
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
}

void CaptureService::RestoreAll() {
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
    primaryId_ = 0;
}

bool CaptureService::SetPrimary(HWND hwnd) {
    const int index = IndexOf(hwnd);
    if (index < 0) {
        primaryId_ = 0;
        return false;
    }
    primaryId_ = sessions_[static_cast<size_t>(index)].id;
    return true;
}

void CaptureService::ClearPrimary() { primaryId_ = 0; }

int CaptureService::PrimaryIndex() const {
    if (primaryId_ == 0) {
        return -1;
    }
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i].id == primaryId_) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool CaptureService::Refresh() {
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

        detail::StateSnapshot snapshot;
        if (!detail::ReadSnapshot(state, snapshot)) {
            // A publish is in flight; retry on the next tick.
            continue;
        }

        if (snapshot.seq != session.lastSeq) {
            detail::ApplySnapshot(session, snapshot);
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
            if (primaryId_ != 0 && session.id == primaryId_) {
                primaryId_ = 0;
            }
            session.ipc.Close();
            sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }

    return changed || sessions_.size() != before;
}

int CaptureService::IndexOf(HWND hwnd) const {
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i].hwnd == hwnd) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t CaptureService::GetSnapshot(SessionInfo* out, size_t capacity) const {
    if (out == nullptr) {
        return 0;
    }

    size_t written = 0;
    for (const Session& session : sessions_) {
        if (written >= capacity) {
            break;
        }

        SessionInfo& info = out[written];
        info = SessionInfo{};
        info.hwnd = reinterpret_cast<uint64_t>(session.hwnd);
        info.pid = session.pid;
        info.id = session.id;
        info.status = static_cast<int32_t>(session.status);
        info.flags = static_cast<int32_t>(session.flags);
        info.wda = static_cast<int32_t>(session.wda);
        info.lastError = static_cast<int32_t>(session.lastError);
        info.x86 = session.x86 ? 1u : 0u;
        info.sticky = session.sticky ? 1u : 0u;

        const size_t copy = (std::min)(session.title.size(), kTitleChars - 1);
        if (copy > 0) {
            std::wmemcpy(info.title, session.title.c_str(), copy);
        }
        ++written;
    }
    return written;
}

}  // namespace gf
