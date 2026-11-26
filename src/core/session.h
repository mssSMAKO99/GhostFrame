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

    // Stable identity used for sticky capture: the executable path and the
    // window class survive a process restart, unlike the HWND and PID.
    std::wstring exePath;
    std::wstring className;
    bool sticky = false;

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
// `ok` is true; it is moved into the CaptureService on the UI thread.
struct AttachResult {
    bool ok = false;
    std::wstring error;
    std::wstring title;  // best-effort title, filled in even when the attach fails
    Session session;
};

// Blocking part of an attach: validates the window, creates the shared state,
// injects the agent and waits for the attach handshake.  It never touches the
// CaptureService, so it is safe to run on a worker thread; the finished
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

}  // namespace gf
