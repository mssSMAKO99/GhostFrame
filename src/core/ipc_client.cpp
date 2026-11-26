#include "ipc_client.h"

#include <stdlib.h>
#include <string.h>

namespace gf {

bool SessionIpc::Create(DWORD targetPid, uint32_t sessionId) {
    Close();

    wchar_t name[160] = {};

    gf::MakeObjectName(name, _countof(name), L"State", targetPid, sessionId);
    map_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                sizeof(gf::SharedState), name);
    if (map_ == nullptr) {
        Close();
        return false;
    }

    state_ = static_cast<gf::SharedState*>(
        ::MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(gf::SharedState)));
    if (state_ == nullptr) {
        Close();
        return false;
    }

    memset(state_, 0, sizeof(*state_));
    state_->magic = gf::kStateMagic;
    state_->version = gf::kStateVersion;
    state_->targetPid = static_cast<LONG>(targetPid);

    gf::MakeObjectName(name, _countof(name), L"Cmd", targetPid, sessionId);
    cmd_ = ::CreateEventW(nullptr, FALSE, FALSE, name);

    gf::MakeObjectName(name, _countof(name), L"Stop", targetPid, sessionId);
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, name);

    gf::MakeObjectName(name, _countof(name), L"Changed", targetPid, sessionId);
    changed_ = ::CreateEventW(nullptr, FALSE, FALSE, name);

    if (cmd_ == nullptr || stop_ == nullptr || changed_ == nullptr) {
        Close();
        return false;
    }
    return true;
}

void SessionIpc::Close() {
    if (state_ != nullptr) {
        ::UnmapViewOfFile(state_);
        state_ = nullptr;
    }
    if (changed_ != nullptr) {
        ::CloseHandle(changed_);
        changed_ = nullptr;
    }
    if (stop_ != nullptr) {
        ::CloseHandle(stop_);
        stop_ = nullptr;
    }
    if (cmd_ != nullptr) {
        ::CloseHandle(cmd_);
        cmd_ = nullptr;
    }
    if (map_ != nullptr) {
        ::CloseHandle(map_);
        map_ = nullptr;
    }
}

bool SessionIpc::Send(gf::Cmd cmd, DWORD timeoutMs) {
    if (state_ == nullptr) {
        return false;
    }

    state_->cmd = static_cast<LONG>(cmd);
    ::MemoryBarrier();
    const LONG sequence = ::InterlockedIncrement(&state_->cmdSeq);
    ::SetEvent(cmd_);

    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        if (state_->ackSeq == sequence) {
            return true;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            break;
        }
        const DWORD slice = static_cast<DWORD>(deadline - now);
        ::WaitForSingleObject(changed_, slice < 25 ? slice : 25);
    }
    return state_->ackSeq == sequence;
}

bool SessionIpc::WaitForFlag(LONG flag, DWORD timeoutMs) {
    if (state_ == nullptr) {
        return false;
    }
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    for (;;) {
        if ((state_->flags & flag) == flag) {
            return true;
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) {
            return false;
        }
        const DWORD slice = static_cast<DWORD>(deadline - now);
        ::WaitForSingleObject(changed_, slice < 20 ? slice : 20);
    }
}

void SessionIpc::SignalStop() {
    if (stop_ != nullptr) {
        ::SetEvent(stop_);
    }
}

}  // namespace gf
