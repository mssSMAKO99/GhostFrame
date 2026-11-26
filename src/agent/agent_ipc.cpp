#include "agent_ipc.h"

#include <stdlib.h>

namespace gf::agent {

namespace {

void Signal(HANDLE event) {
    if (event) {
        ::SetEvent(event);
    }
}

}  // namespace

bool AgentIpc::Open(DWORD targetPid, uint32_t sessionId) {
    wchar_t name[160] = {};

    gf::MakeObjectName(name, _countof(name), L"State", targetPid, sessionId);
    map_ = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map_) {
        return false;
    }

    state_ = static_cast<gf::SharedState*>(
        ::MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(gf::SharedState)));
    if (!state_) {
        Close();
        return false;
    }

    if (state_->magic != gf::kStateMagic || state_->version != gf::kStateVersion) {
        Close();
        return false;
    }

    gf::MakeObjectName(name, _countof(name), L"Cmd", targetPid, sessionId);
    cmd_ = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);

    gf::MakeObjectName(name, _countof(name), L"Stop", targetPid, sessionId);
    stop_ = ::OpenEventW(SYNCHRONIZE, FALSE, name);

    gf::MakeObjectName(name, _countof(name), L"Changed", targetPid, sessionId);
    changed_ = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, name);

    if (!cmd_ || !stop_ || !changed_) {
        Close();
        return false;
    }

    state_->targetPid = static_cast<LONG>(targetPid);
    return true;
}

void AgentIpc::Close() {
    if (state_) {
        ::UnmapViewOfFile(state_);
        state_ = nullptr;
    }
    if (changed_) {
        ::CloseHandle(changed_);
        changed_ = nullptr;
    }
    if (stop_) {
        ::CloseHandle(stop_);
        stop_ = nullptr;
    }
    if (cmd_) {
        ::CloseHandle(cmd_);
        cmd_ = nullptr;
    }
    if (map_) {
        ::CloseHandle(map_);
        map_ = nullptr;
    }
}

void AgentIpc::BeginWrite() {
    if (state_) {
        ::InterlockedIncrement(&state_->seq);  // odd while modified
        ::MemoryBarrier();
    }
}

void AgentIpc::EndWrite() {
    if (state_) {
        ::MemoryBarrier();
        ::InterlockedIncrement(&state_->seq);  // even again, snapshot complete
        Signal(changed_);
    }
}

void AgentIpc::Publish(gf::Status status, LONG setFlags, LONG clearFlags,
                       DWORD wda, LONG error, LONG ackSeq) {
    if (!state_) {
        return;
    }
    BeginWrite();
    state_->status = static_cast<LONG>(status);
    LONG flags = state_->flags;
    flags |= setFlags;
    flags &= ~clearFlags;
    state_->flags = flags;
    state_->wda = static_cast<LONG>(wda);
    state_->lastError = error;
    if (ackSeq >= 0) {
        state_->ackSeq = ackSeq;
    }
    EndWrite();
}

void AgentIpc::PublishAffinity(DWORD wda) {
    if (!state_) {
        return;
    }
    BeginWrite();
    state_->wda = static_cast<LONG>(wda);
    EndWrite();
}

void AgentIpc::Ack(LONG seq) {
    if (!state_) {
        return;
    }
    BeginWrite();
    state_->ackSeq = seq;
    EndWrite();
}

}  // namespace gf::agent
