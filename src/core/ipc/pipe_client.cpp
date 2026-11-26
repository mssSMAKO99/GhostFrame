#include "pipe_client.h"

namespace gf {

HANDLE PipeClient::Open(DWORD timeoutMs) const {
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        HANDLE pipe = ::CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return pipe;
        }
        if (::GetTickCount64() >= deadline) {
            return INVALID_HANDLE_VALUE;
        }
        // ERROR_FILE_NOT_FOUND means the daemon has not created its first
        // instance yet; ERROR_PIPE_BUSY means all instances are in use.  Both are
        // worth retrying until the deadline.
        ::Sleep(20);
    }
}

bool PipeClient::WaitAvailable(DWORD timeoutMs) const {
    HANDLE pipe = Open(timeoutMs);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    ::CloseHandle(pipe);
    return true;
}

bool PipeClient::Call(const IpcFrame& request, IpcFrame& reply, DWORD connectTimeoutMs,
                      std::wstring& error) const {
    HANDLE pipe = Open(connectTimeoutMs);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = L"the GhostFrame daemon is not running";
        return false;
    }

    bool ok = WriteFrame(pipe, request);
    if (ok) {
        ok = ReadFrame(pipe, reply);
    }
    if (!ok) {
        error = L"the daemon connection was lost";
    }

    ::CloseHandle(pipe);
    return ok;
}

}  // namespace gf
