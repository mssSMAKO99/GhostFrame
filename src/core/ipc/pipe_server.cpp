#include "pipe_server.h"

#include "log.h"

namespace gf {

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(const std::wstring& pipeName, Handler handler, std::wstring& error) {
    if (thread_.joinable()) {
        error = L"the pipe server is already running";
        return false;
    }
    if (!handler) {
        error = L"the pipe server needs a handler";
        return false;
    }

    pipeName_ = pipeName;
    handler_ = std::move(handler);
    stop_.store(false);
    thread_ = std::thread(&PipeServer::Run, this);
    return true;
}

void PipeServer::Stop() {
    if (!thread_.joinable()) {
        return;
    }

    stop_.store(true);
    // A blocked ConnectNamedPipe cannot be cancelled; a throwaway client wakes
    // it so the loop can observe `stop_`.  If no instance is listening the open
    // simply fails and the loop exits on the flag anyway.
    HANDLE wake = ::CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) {
        ::CloseHandle(wake);
    }
    thread_.join();
}

void PipeServer::Run() {
    const DWORD bufferSize = kIpcMaxPayload + sizeof(IpcHeader);

    while (!stop_.load()) {
        HANDLE pipe = ::CreateNamedPipeW(pipeName_.c_str(), PIPE_ACCESS_DUPLEX,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                         PIPE_UNLIMITED_INSTANCES, bufferSize, bufferSize, 0,
                                         nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LogLine(L"daemon pipe: CreateNamedPipe failed: %s",
                    Win32Message(::GetLastError()).c_str());
            break;
        }

        const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
        const DWORD lastError = connected ? 0 : ::GetLastError();

        if (stop_.load()) {
            ::CloseHandle(pipe);
            break;
        }
        if (connected || lastError == ERROR_PIPE_CONNECTED) {
            ServeClient(pipe);
        }

        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
    }
}

void PipeServer::ServeClient(HANDLE pipe) {
    for (;;) {
        IpcFrame request;
        if (!ReadFrame(pipe, request)) {
            return;  // client closed or sent garbage
        }

        IpcFrame reply;
        const bool keepServing = handler_(request, reply);
        if (!WriteFrame(pipe, reply)) {
            return;
        }
        if (!keepServing) {
            stop_.store(true);
            return;
        }
    }
}

}  // namespace gf
