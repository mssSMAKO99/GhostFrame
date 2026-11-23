#pragma once

#include <windows.h>

#include "ghostframe/protocol.h"

namespace gf::agent {

// Client side of the shared state block, opened inside the target process.
class AgentIpc {
public:
    AgentIpc() = default;
    AgentIpc(const AgentIpc&) = delete;
    AgentIpc& operator=(const AgentIpc&) = delete;

    AgentIpc(AgentIpc&& other) noexcept { MoveFrom(other); }
    AgentIpc& operator=(AgentIpc&& other) noexcept {
        if (this != &other) {
            Close();
            MoveFrom(other);
        }
        return *this;
    }

    // Closes the mapping and events on every exit path (including early
    // returns from RunWorker), so no handle can leak into the target process.
    ~AgentIpc() { Close(); }

    bool Open(DWORD targetPid, uint32_t sessionId);
    void Close();

    gf::SharedState* state() const { return state_; }
    HANDLE cmd_event() const { return cmd_; }
    HANDLE stop_event() const { return stop_; }

    // Publishes a full snapshot.  ackSeq < 0 leaves the ack untouched.
    void Publish(gf::Status status, LONG setFlags, LONG clearFlags,
                 DWORD wda, LONG error, LONG ackSeq);

    // Publishes only the observed affinity (used by the guard tick).  Command
    // errors are owned by Publish and must not be overwritten here.
    void PublishAffinity(DWORD wda);

    void Ack(LONG seq);

private:
    void MoveFrom(AgentIpc& other) noexcept {
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

    // Seqlock: `seq` is odd while fields are being written and even again once
    // the snapshot is complete.  The launcher reads it with a retry loop.
    void BeginWrite();
    void EndWrite();

    HANDLE map_ = nullptr;
    HANDLE cmd_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE changed_ = nullptr;
    gf::SharedState* state_ = nullptr;
};

}  // namespace gf::agent
