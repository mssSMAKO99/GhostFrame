// Named-pipe client side of the daemon protocol.
//
// The client connects per call: it opens the pipe, exchanges one request and
// reply, then closes.  That keeps the daemon's server loop simple (it is only
// ever blocked in ConnectNamedPipe when idle, so it can be stopped reliably) and
// makes every call independent of the previous one.
#pragma once

#include <windows.h>

#include <string>

#include "ipc/pipe_io.h"

namespace gf {

class PipeClient {
public:
    explicit PipeClient(std::wstring pipeName = kPipeName) : pipeName_(std::move(pipeName)) {}

    // Opens the pipe, retrying until `timeoutMs` elapses.  Returns false if no
    // instance ever accepts the connection.
    bool WaitAvailable(DWORD timeoutMs) const;

    // One request/reply exchange over a fresh connection.  `reply` receives the
    // daemon's answer; the caller checks its type.
    bool Call(const IpcFrame& request, IpcFrame& reply, DWORD connectTimeoutMs,
              std::wstring& error) const;

private:
    HANDLE Open(DWORD timeoutMs) const;

    std::wstring pipeName_;
};

}  // namespace gf
