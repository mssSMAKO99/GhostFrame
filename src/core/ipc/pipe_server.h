// Named-pipe server side of the daemon protocol.
//
// The daemon owns exactly one PipeServer.  It listens on a background thread so
// the message loop stays responsive; the handler it is given runs entirely on
// that thread, which is therefore the single owner of the capture pipeline.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "ipc/pipe_io.h"

namespace gf {

class PipeServer {
public:
    // Called on the server thread for every request.  The handler fills `reply`
    // and returns true to keep serving; returning false stops the server after
    // the reply is sent (this is how a Shutdown request ends the daemon).
    using Handler = std::function<bool(const IpcFrame& request, IpcFrame& reply)>;

    PipeServer() = default;
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    ~PipeServer();

    // Starts listening.  Returns as soon as the thread is running; use the
    // client's connect timeout to wait for the first instance.
    bool Start(const std::wstring& pipeName, Handler handler, std::wstring& error);

    // Stops the server and joins the thread.  Safe to call more than once.
    void Stop();

private:
    void Run();
    void ServeClient(HANDLE pipe);

    std::wstring pipeName_;
    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace gf
