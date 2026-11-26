// Internal helpers for reading the agent-owned shared state.
//
// Kept in a private header so the attach path (session.cpp) and the command /
// refresh path (capture_service.cpp) observe the agent's seqlock the same way
// and cannot drift apart.
#pragma once

#include "session.h"

namespace gf {
namespace detail {

// Consistent view of the agent-owned fields.  The agent publishes with a
// seqlock (odd `seq` while writing, even when done), so a snapshot that is
// taken while a command/guard tick is in flight is retried instead of being
// mixed with the previous values.
struct StateSnapshot {
    LONG status = 0;
    LONG flags = 0;
    LONG wda = 0;
    LONG lastError = 0;
    LONG seq = -1;
};

inline bool ReadSnapshot(const gf::SharedState* state, StateSnapshot& out) {
    if (state == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const LONG before = state->seq;
        if ((before & 1) != 0) {
            ::SwitchToThread();
            continue;
        }

        StateSnapshot snapshot;
        snapshot.status = state->status;
        snapshot.flags = state->flags;
        snapshot.wda = state->wda;
        snapshot.lastError = state->lastError;
        snapshot.seq = before;
        ::MemoryBarrier();
        if (state->seq == before) {
            out = snapshot;
            return true;
        }
    }
    return false;
}

inline void ApplySnapshot(Session& session, const StateSnapshot& snapshot) {
    session.status = static_cast<Status>(snapshot.status);
    session.flags = snapshot.flags;
    session.wda = snapshot.wda;
    session.lastError = snapshot.lastError;
    session.lastSeq = snapshot.seq;
}

}  // namespace detail
}  // namespace gf
