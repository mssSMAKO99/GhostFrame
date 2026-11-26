#pragma once

#include <windows.h>

#include <string>

#include "ghostframe/protocol.h"

namespace gf {

// Launcher side of the shared state block.
class SessionIpc {
public:
    SessionIpc() = default;
    SessionIpc(const SessionIpc&) = delete;
    SessionIpc& operator=(const SessionIpc&) = delete;

    SessionIpc(SessionIpc&& other) noexcept { MoveFrom(other); }
    SessionIpc& operator=(SessionIpc&& other) noexcept {
        if (this != &other) {
            Close();
            MoveFrom(other);
        }
        return *this;
    }

    ~SessionIpc() { Close(); }

    bool Create(DWORD targetPid, uint32_t sessionId);
    void Close();
    bool valid() const { return state_ != nullptr; }

    gf::SharedState* state() const { return state_; }

    // Queues a command and waits for the agent to acknowledge it.
    bool Send(gf::Cmd cmd, DWORD timeoutMs);
    bool WaitForFlag(LONG flag, DWORD timeoutMs);
    void SignalStop();

private:
    void MoveFrom(SessionIpc& other) noexcept {
        map_ = other.map_;
        cmd_ = other.cmd_;
        stop_ = other.stop_;
        changed_ = other.changed_;
        state_ = other.state_;

        other.map_ = nullptr;
        other.cmd_ = nullptr;
        other.stop_ = nullptr;
        other.changed_ = nullptr;
        other.state_ = nullptr;
    }

    HANDLE map_ = nullptr;
    HANDLE cmd_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE changed_ = nullptr;
    gf::SharedState* state_ = nullptr;
};

}  // namespace gf
