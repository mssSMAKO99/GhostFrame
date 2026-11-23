// Shared helper for the per-session identifier the launcher hands to the agent.
// Kept in one place so the launcher and the session manager cannot drift apart.
#pragma once

#include <windows.h>

#include <bcrypt.h>

namespace gf {

// A non-zero, unpredictable session id.  BCryptGenRandom is preferred (the
// launcher already links bcrypt.lib for the payload hash); a cheap
// time/pid/tick mix is only a fallback for the unlikely RNG failure.
inline uint32_t NewSessionId() {
    uint32_t id = 0;
    if (::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&id), sizeof(id),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        id = 0;
    }
    if (id == 0) {
        LARGE_INTEGER counter = {};
        ::QueryPerformanceCounter(&counter);
        id = static_cast<uint32_t>(counter.QuadPart ^ (counter.QuadPart >> 32)) ^
             ::GetCurrentProcessId() * 2654435761u ^ ::GetTickCount();
    }
    return id != 0 ? id : 1;
}

}  // namespace gf
