// Framing for the daemon named-pipe protocol.
//
// A pipe is a byte stream, so every message is framed as an IpcHeader followed
// by its payload.  These helpers read and write exactly one frame at a time and
// validate the header, so a truncated or foreign stream fails cleanly instead of
// desynchronising the connection.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <cstring>
#include <string>
#include <vector>

#include "ghostframe/ipc_protocol.h"

namespace gf {

// One framed message: its type plus the raw payload bytes.
struct IpcFrame {
    IpcType type = IpcType::Hello;
    std::vector<uint8_t> payload;
};

// Stores a POD value as the frame payload.
template <typename T>
void SetPayload(IpcFrame& frame, const T& value) {
    static_assert(sizeof(T) <= kIpcMaxPayload, "payload too large for the wire");
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    frame.payload.assign(bytes, bytes + sizeof(T));
}

// Reads a POD value out of the frame payload.  Returns false when the payload is
// too small (a short or foreign message).
template <typename T>
bool GetPayload(const IpcFrame& frame, T& out) {
    if (frame.payload.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, frame.payload.data(), sizeof(T));
    return true;
}

// Reads one frame, blocking until it arrives.  Returns false on a closed pipe, a
// header mismatch or a payload that exceeds the protocol limit.
bool ReadFrame(HANDLE pipe, IpcFrame& frame);

// Writes one frame, blocking until it is fully sent.  Returns false on error.
bool WriteFrame(HANDLE pipe, const IpcFrame& frame);

// Stores a string as a TextPayload.  Returns false when it does not fit.
bool SetTextPayload(IpcFrame& frame, const std::wstring& text);
// Reads a TextPayload back into a string.  Returns false on a malformed frame.
bool GetTextPayload(const IpcFrame& frame, std::wstring& text);

}  // namespace gf
