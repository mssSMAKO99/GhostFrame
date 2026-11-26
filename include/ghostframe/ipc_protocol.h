// GhostFrame daemon IPC protocol.
//
// The UI (or any other client) talks to the background daemon over a named
// pipe.  Every message is a fixed 16-byte header followed by a POD payload;
// nothing here may contain a pointer, a handle or an STL type.  The protocol is
// versioned, so a client that does not match the daemon fails its handshake
// instead of misreading a struct.
//
// This is deliberately separate from `protocol.h`, which describes the shared
// state block the launcher and the in-target agent use.
#pragma once

#include <windows.h>
#include <stdint.h>

namespace gf {

// One well-known pipe name per protocol version.  A client that cannot open it
// knows no daemon is listening.
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\GhostFrame.Daemon.v1";

inline constexpr uint32_t kIpcMagic = 0x31435047u;  // 'GPC1'
inline constexpr uint32_t kIpcVersion = 1u;

// Upper bound on a payload.  The snapshot is the largest message; the limit is
// generous enough for it and small enough to reject a corrupt header early.
inline constexpr uint32_t kIpcMaxPayload = 64u * 1024u;

// Request/reply kinds.  Each request has exactly one reply; the `*Ack`/reply
// kinds are the answers the daemon sends back.
enum class IpcType : uint32_t {
    Hello = 1,
    HelloAck = 2,
    GetSnapshot = 3,
    Snapshot = 4,
    Send = 5,
    SendAck = 6,
    Attach = 7,
    AttachAck = 8,
    Detach = 9,
    DetachAck = 10,
    RestoreAll = 11,
    RestoreAllAck = 12,
    Shutdown = 13,
    ShutdownAck = 14,
    GetSettings = 15,
    Settings = 16,
    SetSettings = 17,
    SetSettingsAck = 18,
    ApplyAutoCapture = 19,
    ApplyAutoCaptureAck = 20,
    SetPrimary = 21,
    SetPrimaryAck = 22,
    SetSticky = 23,
    SetStickyAck = 24,
};

struct IpcHeader {
    uint32_t magic;    // gf::kIpcMagic
    uint32_t version;  // gf::kIpcVersion
    uint32_t type;     // gf::IpcType
    uint32_t size;     // payload byte count that follows the header
};

static_assert(sizeof(IpcHeader) == 16, "the wire header must stay 16 bytes");

// The pipe is only used by x64 builds, but keep the layout fixed so it would
// survive a 32-bit client too.
inline constexpr uint32_t kMaxSnapshotSessions = 64u;
inline constexpr size_t kTitleChars = 200u;

// A plain snapshot of one tracked session: the fields the UI needs to render a
// target row, with no handle or shared-state pointer on the wire.
struct SessionInfo {
    uint64_t hwnd;
    uint32_t pid;
    uint32_t id;
    int32_t status;     // gf::Status
    int32_t flags;      // gf::Flags
    int32_t wda;        // last observed display affinity
    int32_t lastError;  // Win32 code of the last failure
    uint32_t x86;
    uint32_t sticky;  // 1 when this window's identity is a sticky target
    wchar_t title[kTitleChars];
};

struct HelloReply {
    uint32_t protocolVersion;
    uint32_t daemonPid;
};

struct SnapshotReply {
    uint32_t count;
    uint32_t primaryId;  // session id of the primary target, 0 when none
    SessionInfo sessions[kMaxSnapshotSessions];
};

// `target` is a HWND; 0 means "every tracked target" (used by RestoreAll-style
// commands).  `cmd` is a gf::Cmd.
struct SendRequest {
    uint64_t target;
    int32_t cmd;
};

// How a batch or single command ended.  `ok`/`timeout`/`error` are counts, so a
// single-target send simply sets one of them to 1.
struct SendReply {
    int32_t ok;
    int32_t timeout;
    int32_t error;
};

struct AttachRequest {
    uint64_t hwnd;
};

struct AttachReply {
    int32_t ok;
    wchar_t error[256];
};

struct DetachRequest {
    uint64_t hwnd;
};

struct DetachReply {
    int32_t ok;
    int32_t count;  // remaining tracked sessions
};

struct RestoreAllReply {
    int32_t count;
};

// Settings travel as the same JSON document that lives in settings.json: it is
// already the stable, forward-compatible representation, and keeping it textual
// means a schema change does not also need a wire change.
inline constexpr uint32_t kMaxTextChars = 8192u;

struct TextPayload {
    uint32_t length;  // valid wchar_t count, excluding the terminator
    wchar_t text[kMaxTextChars];
};

struct SetSettingsReply {
    int32_t ok;
    wchar_t error[256];
};

struct ApplyAutoCaptureReply {
    int32_t attached;  // windows attached by this scan
};

// `hwnd` 0 clears the primary target.
struct SetPrimaryRequest {
    uint64_t hwnd;
};

struct SetPrimaryReply {
    int32_t ok;         // 1 when the requested target exists (or was cleared)
    uint32_t primaryId; // resulting primary session id, 0 when none
};

// `hwnd` names the tracked window whose identity is toggled; 0 is rejected.
struct SetStickyRequest {
    uint64_t hwnd;
    int32_t sticky;  // 1 to remember the identity, 0 to forget it
};

struct SetStickyReply {
    int32_t ok;
    int32_t sticky;
};

}  // namespace gf
