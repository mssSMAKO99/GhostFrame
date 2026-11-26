#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ghostframe/ipc_protocol.h"
#include "injector.h"
#include "session.h"

namespace gf {

// The core's capture boundary.
//
// CaptureService owns the extracted payloads (Injector) and the list of tracked
// sessions, and is the only place that talks to the shared state block.  The UI
// (and, later, the daemon) drives it through this interface and never touches
// the injector or a SessionIpc directly.
class CaptureService {
public:
    CaptureService() = default;
    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    // Prepares the embedded payloads.  Must succeed before Attach.  Idempotent:
    // a second call after a success is a no-op.
    bool Initialize(std::wstring& error);

    // Prepares the payloads on demand.  Attach calls this first, so a daemon that
    // could not extract them at start still works once the environment allows it.
    bool EnsureInitialized(std::wstring& error);

    // Releases the payloads.  Does not restore tracked windows; call RestoreAll
    // first for a clean teardown.
    void Shutdown();

    // Exposed only for the attach worker, which runs the blocking PrepareAttach
    // off the UI thread and hands the finished Session back through Add().
    Injector& InjectorRef() { return injector_; }

    // Blocking single-window attach.  The caller must not already track `hwnd`.
    bool Attach(HWND hwnd, std::wstring& error);

    SendOutcome Send(size_t index, Cmd cmd, std::wstring& error);

    // Applies one command to several targets under a single shared deadline:
    // up to 300 ms per target, but never more than 1.5 s for the whole batch.
    BatchSendResult SendToMany(const std::vector<size_t>& indices, Cmd cmd);

    void Add(Session&& session);
    void Remove(size_t index);
    void RestoreAll();

    // The primary target is the one hotkeys and the tray act on.  It is tracked
    // by session id, so adding or reordering sessions cannot retarget it.
    // SetPrimary returns false (and clears the primary) when `hwnd` is not
    // tracked.  A cleared primary falls back to the previous "all targets"
    // behavior in the daemon.
    bool SetPrimary(HWND hwnd);
    void ClearPrimary();
    uint32_t PrimaryId() const { return primaryId_; }
    int PrimaryIndex() const;
    bool IsPrimary(const Session& session) const { return session.id == primaryId_ && primaryId_ != 0; }
    bool HasPrimary() const { return PrimaryIndex() >= 0; }

    // Drops dead sessions and refreshes the cached state.  Returns true when
    // anything visible changed.
    bool Refresh();

    // Copies up to `capacity` plain views of the tracked sessions and returns how
    // many were written.  The result holds no handle or SessionIpc, so it is safe
    // to put on the wire or hand to another process.
    size_t GetSnapshot(SessionInfo* out, size_t capacity) const;

    size_t Count() const { return sessions_.size(); }
    bool Empty() const { return sessions_.empty(); }
    Session& At(size_t index) { return sessions_[index]; }
    const Session& At(size_t index) const { return sessions_[index]; }
    int IndexOf(HWND hwnd) const;

private:
    SendOutcome SendOne(Session& session, Cmd cmd, DWORD timeoutMs, std::wstring& error);

    Injector injector_;
    std::vector<Session> sessions_;
    uint32_t primaryId_ = 0;
    bool initialized_ = false;
};

}  // namespace gf
