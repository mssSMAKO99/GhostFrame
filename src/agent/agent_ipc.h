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

    bool Open(DWORD targetPid, uint32_t sessionId);
    void Close();

    gf::SharedState* state() const { return state_; }
    HANDLE cmd_event() const { return cmd_; }
    HANDLE stop_event() const { return stop_; }

    // Publishes a full snapshot.  ackSeq < 0 leaves the ack untouched.
    void Publish(gf::Status status, LONG setFlags, LONG clearFlags,
                 DWORD wda, LONG error, LONG ackSeq);

    // Publishes only the affinity/error pair (used by the guard tick).
    void PublishAffinity(DWORD wda, LONG error);

    void Ack(LONG seq);

private:
    HANDLE map_ = nullptr;
    HANDLE cmd_ = nullptr;
    HANDLE stop_ = nullptr;
    HANDLE changed_ = nullptr;
    gf::SharedState* state_ = nullptr;
};

}  // namespace gf::agent
