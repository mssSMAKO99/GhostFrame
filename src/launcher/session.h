#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ghostframe/protocol.h"
#include "injector.h"
#include "ipc_client.h"

namespace gf {

struct Session {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    ULONGLONG createTime = 0;
    uint32_t id = 0;
    bool x86 = false;
    std::wstring title;

    SessionIpc ipc;

    Status status = Status::Empty;
    LONG flags = 0;
    LONG wda = 0;
    LONG lastError = 0;
    LONG lastSeq = -1;

    bool attached() const { return (flags & FlagAttached) != 0; }
    bool hidden() const { return (flags & FlagHidden) != 0; }
    bool errored() const { return status == Status::Error; }
};

bool ValidateTarget(HWND hwnd, std::wstring& reason);

class SessionManager {
public:
    explicit SessionManager(Injector& injector) : injector_(injector) {}

    bool Attach(HWND hwnd, std::wstring& error);
    bool Send(size_t index, Cmd cmd, std::wstring& error);
    void Remove(size_t index);
    void RestoreAll();

    // Drops dead sessions and refreshes the cached state.  Returns true when
    // anything visible changed.
    bool Refresh();

    size_t Count() const { return sessions_.size(); }
    bool Empty() const { return sessions_.empty(); }
    Session& At(size_t index) { return sessions_[index]; }
    const Session& At(size_t index) const { return sessions_[index]; }
    int IndexOf(HWND hwnd) const;

private:
    Injector& injector_;
    std::vector<Session> sessions_;
};

}  // namespace gf
