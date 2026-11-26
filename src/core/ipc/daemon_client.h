// Typed client for the daemon protocol.
//
// Wraps the raw pipe transport with one method per request and validates that the
// daemon answers with the reply that matches.  It also knows how to bring a
// daemon up: `EnsureRunning` connects to an existing one or starts the current
// executable with `--daemon` and waits for the pipe.
#pragma once

#include <windows.h>

#include <string>

#include "ghostframe/ipc_protocol.h"
#include "ghostframe/protocol.h"
#include "ipc/pipe_client.h"

namespace gf {

class DaemonClient {
public:
    explicit DaemonClient(std::wstring pipeName = kPipeName) : client_(std::move(pipeName)) {}

    // Performs a Hello handshake.  Returns true only when a daemon running the
    // same protocol version answers within `timeoutMs`.
    bool Connect(DWORD timeoutMs, std::wstring& error);

    // Connects to a running daemon; if none answers, starts `self --daemon` and
    // waits for the pipe.  No admin rights and no service are involved.
    bool EnsureRunning(std::wstring& error);

    bool GetSnapshot(SnapshotReply& out, std::wstring& error);
    bool Send(uint64_t target, Cmd cmd, SendReply& out, std::wstring& error);
    bool Attach(uint64_t hwnd, AttachReply& out, std::wstring& error);
    bool Detach(uint64_t hwnd, DetachReply& out, std::wstring& error);
    bool RestoreAll(RestoreAllReply& out, std::wstring& error);
    bool Shutdown(std::wstring& error);

    // Settings travel as the settings.json document.
    bool GetSettings(std::wstring& json, std::wstring& error);
    bool SetSettings(const std::wstring& json, SetSettingsReply& out, std::wstring& error);
    bool ApplyAutoCapture(ApplyAutoCaptureReply& out, std::wstring& error);

    // Primary target: `hwnd` 0 clears it.
    bool SetPrimary(uint64_t hwnd, SetPrimaryReply& out, std::wstring& error);
    // Sticky capture toggle for one tracked window.
    bool SetSticky(uint64_t hwnd, bool sticky, SetStickyReply& out, std::wstring& error);

private:
    bool Exchange(const IpcFrame& request, IpcType expectedReply, IpcFrame& reply,
                  std::wstring& error);

    PipeClient client_;
};

}  // namespace gf
