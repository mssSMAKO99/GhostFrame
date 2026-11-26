// Client-side view of the daemon's capture service.
//
// The UI does not own a CaptureService any more: it renders this cached snapshot
// and forwards every command to the daemon.  The class deliberately mirrors the
// small part of the CaptureService interface the UI uses, so the UI code stays
// about presentation instead of transport.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ghostframe/ipc_protocol.h"
#include "ghostframe/protocol.h"
#include "ipc/daemon_client.h"
#include "session.h"
#include "settings.h"

namespace gf {

// One tracked target as received from the daemon.
struct RemoteSession {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    uint32_t id = 0;
    bool x86 = false;
    std::wstring title;

    int32_t status = 0;
    int32_t flags = 0;
    int32_t wda = 0;
    int32_t lastError = 0;
    bool sticky = false;

    bool hidden() const { return (flags & FlagHidden) != 0; }
    bool errored() const { return status == static_cast<int32_t>(Status::Error); }
};

class RemoteCapture {
public:
    RemoteCapture() = default;

    // Connects to the daemon, starting it when needed.  The UI calls this once at
    // startup; every later call reconnects per request.
    bool Connect(std::wstring& error);
    bool connected() const { return connected_; }

    // Polls the daemon and replaces the cached list.  Returns true when anything
    // the UI renders changed.
    bool Refresh();

    size_t Count() const { return sessions_.size(); }
    bool Empty() const { return sessions_.empty(); }
    const RemoteSession& At(size_t index) const { return sessions_[index]; }
    int IndexOf(HWND hwnd) const;

    // Session id of the primary target as last reported by the daemon, or 0.
    uint32_t PrimaryId() const { return primaryId_; }
    bool IsPrimary(const RemoteSession& session) const {
        return primaryId_ != 0 && session.id == primaryId_;
    }

    // Set/clear the primary target.  `hwnd` 0 clears it.
    bool SetPrimary(HWND hwnd, std::wstring& error);
    // Toggle sticky capture for one tracked window.
    bool SetSticky(HWND hwnd, bool sticky, bool& result, std::wstring& error);

    SendOutcome Send(size_t index, Cmd cmd, std::wstring& error);
    BatchSendResult SendToMany(const std::vector<size_t>& indices, Cmd cmd);

    // The daemon suppresses a detached window in its own auto-capture guard, so
    // a manual removal is not re-attached.
    bool Remove(size_t index);
    bool RestoreAll();

    // Pushes the whole settings document; the daemon applies the hotkeys, the
    // auto-capture rules and persists the file.
    bool ApplySettings(const Settings& settings, std::wstring& error);
    // Runs the auto-capture rules against the windows open right now.
    bool ApplyAutoCaptureNow(int& attached, std::wstring& error);

private:
    static RemoteSession FromWire(const SessionInfo& info);

    DaemonClient client_;
    std::vector<RemoteSession> sessions_;
    uint32_t primaryId_ = 0;
    bool connected_ = false;
};

}  // namespace gf