// GhostFrame inter-process protocol.
//
// The launcher and the in-target agent share a small block of shared memory and
// three named events.  Everything in this header must stay POD and must be
// identical in x86 and x64 builds.
#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

namespace gf {

// Registered window message used to bootstrap the agent.  Using
// RegisterWindowMessage instead of WM_USER+n avoids collisions with the
// target's own messages.
inline constexpr wchar_t kAttachMessageName[] = L"GhostFrame.Attach.v2";

// Shared memory layout.
//
// Version 3 dropped the old frame-mode command/status/flag values.  The agent
// never modifies the window frame any more, so those numbers are gone.  Do not
// reuse them: an old agent that somehow survives keeps its own layout and
// version, and a mismatch is rejected when the state block is opened.
inline constexpr uint32_t kStateMagic = 0x33465347u;  // 'GSF3'
inline constexpr uint32_t kStateVersion = 3u;

enum class Cmd : LONG {
    None = 0,
    Hide = 1,
    Show = 2,
    RestoreAll = 3,
    Ping = 4,
    Uninstall = 5,
};

enum class Status : LONG {
    Empty = 0,
    Attached = 1,
    Hidden = 2,
    Visible = 3,
    Error = 4,
    Leaving = 5,
    Detached = 6,
};

enum Flags : LONG {
    FlagAttached = 1 << 0,
    FlagHidden = 1 << 1,
    FlagLeaseLost = 1 << 2,
};

struct SharedState {
    uint32_t magic;
    uint32_t version;
    volatile LONG seq;        // bumped by the agent after any visible change
    volatile LONG cmd;        // gf::Cmd, written by the launcher
    volatile LONG cmdSeq;     // bumped by the launcher before signalling
    volatile LONG ackSeq;     // set by the agent after applying cmdSeq
    volatile LONG status;     // gf::Status
    volatile LONG flags;      // gf::Flags
    volatile LONG wda;        // last observed display affinity
    volatile LONG lastError;  // Win32 code of the last failure
    volatile LONG targetPid;  // filled in by both sides, for sanity checks
    volatile LONG reserved[4];
};

// Object names are per target process + random 32 bit session id, so several
// launchers/targets never collide.
inline void MakeObjectName(wchar_t* out, size_t cch, const wchar_t* kind,
                           DWORD targetPid, uint32_t sessionId) {
    swprintf_s(out, cch, L"Local\\GhostFrame.%u.%08X.%s", targetPid, sessionId, kind);
}

}  // namespace gf
