#include "remote_capture.h"

#include <cwchar>

namespace gf {

RemoteSession RemoteCapture::FromWire(const SessionInfo& info) {
    RemoteSession session;
    session.hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(info.hwnd));
    session.pid = info.pid;
    session.id = info.id;
    session.x86 = info.x86 != 0;
    session.status = info.status;
    session.flags = info.flags;
    session.wda = info.wda;
    session.lastError = info.lastError;
    session.sticky = info.sticky != 0;
    session.title = info.title;
    return session;
}

bool RemoteCapture::Connect(std::wstring& error) {
    connected_ = client_.EnsureRunning(error);
    return connected_;
}

bool RemoteCapture::Refresh() {
    SnapshotReply snapshot = {};
    std::wstring error;
    if (!client_.GetSnapshot(snapshot, error)) {
        connected_ = false;
        return false;
    }
    connected_ = true;

    std::vector<RemoteSession> next;
    next.reserve(snapshot.count);
    bool changed = snapshot.count != sessions_.size() || primaryId_ != snapshot.primaryId;
    for (uint32_t i = 0; i < snapshot.count && i < kMaxSnapshotSessions; ++i) {
        RemoteSession session = FromWire(snapshot.sessions[i]);
        if (!changed && i < sessions_.size()) {
            const RemoteSession& previous = sessions_[i];
            if (previous.hwnd != session.hwnd || previous.id != session.id ||
                previous.status != session.status || previous.flags != session.flags ||
                previous.wda != session.wda || previous.lastError != session.lastError ||
                previous.sticky != session.sticky || previous.title != session.title) {
                changed = true;
            }
        }
        next.push_back(std::move(session));
    }

    sessions_ = std::move(next);
    primaryId_ = snapshot.primaryId;
    return changed;
}

int RemoteCapture::IndexOf(HWND hwnd) const {
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i].hwnd == hwnd) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

SendOutcome RemoteCapture::Send(size_t index, Cmd cmd, std::wstring& error) {
    if (index >= sessions_.size()) {
        error = L"no target selected";
        return SendOutcome::Error;
    }

    SendReply reply = {};
    if (!client_.Send(reinterpret_cast<uint64_t>(sessions_[index].hwnd), cmd, reply, error)) {
        connected_ = false;
        return SendOutcome::Error;
    }
    if (reply.ok > 0) {
        return SendOutcome::Ok;
    }
    if (reply.timeout > 0) {
        return SendOutcome::Timeout;
    }
    if (reply.error > 0) {
        if (error.empty()) {
            error = L"the target did not accept the command";
        }
        return SendOutcome::Error;
    }
    return SendOutcome::Ok;
}

BatchSendResult RemoteCapture::SendToMany(const std::vector<size_t>& indices, Cmd cmd) {
    BatchSendResult result;
    std::wstring ignored;
    for (size_t index : indices) {
        if (index >= sessions_.size()) {
            ++result.error;
            continue;
        }
        switch (Send(index, cmd, ignored)) {
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

bool RemoteCapture::Remove(size_t index) {
    if (index >= sessions_.size()) {
        return false;
    }

    DetachReply reply = {};
    std::wstring error;
    const bool ok = client_.Detach(reinterpret_cast<uint64_t>(sessions_[index].hwnd), reply, error);
    sessions_.erase(sessions_.begin() + static_cast<std::ptrdiff_t>(index));
    return ok;
}

bool RemoteCapture::RestoreAll() {
    RestoreAllReply reply = {};
    std::wstring error;
    if (!client_.RestoreAll(reply, error)) {
        connected_ = false;
        return false;
    }
    sessions_.clear();
    return true;
}

bool RemoteCapture::ApplySettings(const Settings& settings, std::wstring& error) {
    const std::wstring json = SerializeSettings(settings);
    SetSettingsReply reply = {};
    if (!client_.SetSettings(json, reply, error)) {
        connected_ = false;
        return false;
    }
    if (reply.ok == 0) {
        error = reply.error;
        return false;
    }
    return true;
}

bool RemoteCapture::ApplyAutoCaptureNow(int& attached, std::wstring& error) {
    ApplyAutoCaptureReply reply = {};
    if (!client_.ApplyAutoCapture(reply, error)) {
        connected_ = false;
        return false;
    }
    attached = reply.attached;
    return true;
}

bool RemoteCapture::SetPrimary(HWND hwnd, std::wstring& error) {
    SetPrimaryReply reply = {};
    if (!client_.SetPrimary(reinterpret_cast<uint64_t>(hwnd), reply, error)) {
        connected_ = false;
        return false;
    }
    if (reply.ok == 0) {
        if (error.empty()) {
            error = L"the target is no longer tracked";
        }
        return false;
    }
    primaryId_ = reply.primaryId;
    return true;
}

bool RemoteCapture::SetSticky(HWND hwnd, bool sticky, bool& result, std::wstring& error) {
    SetStickyReply reply = {};
    if (!client_.SetSticky(reinterpret_cast<uint64_t>(hwnd), sticky, reply, error)) {
        connected_ = false;
        return false;
    }
    if (reply.ok == 0) {
        if (error.empty()) {
            error = L"the target is no longer tracked";
        }
        return false;
    }
    const int index = IndexOf(hwnd);
    if (index >= 0) {
        sessions_[static_cast<size_t>(index)].sticky = reply.sticky != 0;
    }
    result = reply.sticky != 0;
    return true;
}

}  // namespace gf