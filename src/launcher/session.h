#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ghostframe/protocol.h"
#include "injector.h"
#include "ipc_client.h"

namespace gf {

struct Session {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    ULONGLONG createTime = 0;
    uint32_t id = 0;
    bool x86 = false;
    std::wstring title;

    SessionIpc ipc;

    Status status = Status::Empty;
    LONG flags = 0;
    LONG wda = 0;
    LONG lastError = 0;
    LONG lastSeq = -1;

    bool hidden() const { return (flags & FlagHidden) != 0; }
    bool errored() const { return status == Status::Error; }
};

bool ValidateTarget(HWND hwnd, std::wstring& reason);

// Result of the blocking part of an attach.  `session` is only meaningful when
// `ok` is true; it is moved into the SessionManager on the UI thread.
struct AttachResult {
    bool ok = false;
    std::wstring error;
    std::wstring title;  // best-effort title, filled in even when the attach fails
    Session session;
};

// Blocking part of an attach: validates the window, creates the shared state,
// injects the agent and waits for the attach handshake.  It never touches the
// SessionManager, so it is safe to run on a worker thread; the finished
// Session is handed to the UI thread, which stays the only owner of the list.
bool PrepareAttach(HWND hwnd, Injector& injector, AttachResult& result);

// Outcome of a command.  `Timeout` means the command was queued but the agent
// has not acknowledged it yet; the caller may retry later without re-sending
// from a blocking context.
enum class SendOutcome { Ok, Timeout, Error };

// Aggregate result of a batch command: how many targets acknowledged it, how
// many timed out (still busy) and how many failed outright.
struct BatchSendResult {
    int ok = 0;
    int timeout = 0;
    int error = 0;
};

class SessionManager {
public:
    explicit SessionManager(Injector& injector) : injector_(injector) {}

    bool Attach(HWND hwnd, std::wstring& error);
    SendOutcome Send(size_t index, Cmd cmd, std::wstring& error);

    // Applies one command to several targets under a single shared deadline:
    // up to 300 ms per target, but never more than 1.5 s for the whole batch.
    BatchSendResult SendToMany(const std::vector<size_t>& indices, Cmd cmd);

    void Add(Session&& session) { sessions_.push_back(std::move(session)); }
    void Remove(size_t index);
    void RestoreAll();

    // Drops dead sessions and refreshes the cached state.  Returns true when
    // anything visible changed.
    bool Refresh();

    size_t Count() const { return sessions_.size(); }
    bool Empty() const { return sessions_.empty(); }
    Session& At(size_t index) { return sessions_[index]; }
    const Session& At(size_t index) const { return sessions_[index]; }
    int IndexOf(HWND hwnd) const;

private:
    SendOutcome SendOne(Session& session, Cmd cmd, DWORD timeoutMs, std::wstring& error);

    Injector& injector_;
    std::vector<Session> sessions_;
};

}  // namespace gf
