#include "daemon_client.h"

#include <vector>

#include "log.h"

namespace gf {

namespace {

// A short probe used by `EnsureRunning` before deciding to spawn a daemon: a
// running daemon answers immediately, a missing one should not stall startup.
constexpr DWORD kProbeTimeoutMs = 300;

// A normal request waits this long for the daemon to accept the connection.
constexpr DWORD kConnectTimeoutMs = 5000;

// How long `EnsureRunning` waits for a freshly spawned daemon to publish its
// pipe.
constexpr ULONGLONG kEnsureTimeoutMs = 8000;

}  // namespace

bool DaemonClient::Connect(DWORD timeoutMs, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::Hello;

    IpcFrame reply;
    if (!client_.Call(request, reply, timeoutMs, error)) {
        return false;
    }
    if (reply.type != IpcType::HelloAck) {
        error = L"the endpoint is not a GhostFrame daemon";
        return false;
    }

    HelloReply hello = {};
    if (!GetPayload(reply, hello)) {
        error = L"the daemon handshake was malformed";
        return false;
    }
    if (hello.protocolVersion != kIpcVersion) {
        error = L"the daemon speaks a different protocol version";
        return false;
    }
    return true;
}

bool DaemonClient::EnsureRunning(std::wstring& error) {
    if (Connect(kProbeTimeoutMs, error)) {
        return true;
    }

    wchar_t module[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) {
        error = L"cannot resolve the executable path";
        return false;
    }

    std::wstring commandLine = L"\"" + std::wstring(module) + L"\" --daemon";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!::CreateProcessW(module, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process)) {
        error = L"cannot start the GhostFrame daemon: " + Win32Message(::GetLastError());
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    const ULONGLONG deadline = ::GetTickCount64() + kEnsureTimeoutMs;
    while (::GetTickCount64() < deadline) {
        if (Connect(kProbeTimeoutMs, error)) {
            return true;
        }
        ::Sleep(50);
    }

    error = L"the GhostFrame daemon did not start in time";
    return false;
}

bool DaemonClient::Exchange(const IpcFrame& request, IpcType expectedReply, IpcFrame& reply,
                            std::wstring& error) {
    if (!client_.Call(request, reply, kConnectTimeoutMs, error)) {
        return false;
    }
    if (reply.type != expectedReply) {
        error = L"the daemon replied with an unexpected message";
        return false;
    }
    return true;
}

bool DaemonClient::GetSnapshot(SnapshotReply& out, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::GetSnapshot;

    IpcFrame reply;
    if (!Exchange(request, IpcType::Snapshot, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed snapshot";
        return false;
    }
    return true;
}

bool DaemonClient::Send(uint64_t target, Cmd cmd, SendReply& out, std::wstring& error) {
    SendRequest payload = {};
    payload.target = target;
    payload.cmd = static_cast<int32_t>(cmd);

    IpcFrame request;
    request.type = IpcType::Send;
    SetPayload(request, payload);

    IpcFrame reply;
    if (!Exchange(request, IpcType::SendAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed send reply";
        return false;
    }
    return true;
}

bool DaemonClient::Attach(uint64_t hwnd, AttachReply& out, std::wstring& error) {
    AttachRequest payload = {};
    payload.hwnd = hwnd;

    IpcFrame request;
    request.type = IpcType::Attach;
    SetPayload(request, payload);

    IpcFrame reply;
    if (!Exchange(request, IpcType::AttachAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed attach reply";
        return false;
    }
    return true;
}

bool DaemonClient::Detach(uint64_t hwnd, DetachReply& out, std::wstring& error) {
    DetachRequest payload = {};
    payload.hwnd = hwnd;

    IpcFrame request;
    request.type = IpcType::Detach;
    SetPayload(request, payload);

    IpcFrame reply;
    if (!Exchange(request, IpcType::DetachAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed detach reply";
        return false;
    }
    return true;
}

bool DaemonClient::RestoreAll(RestoreAllReply& out, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::RestoreAll;

    IpcFrame reply;
    if (!Exchange(request, IpcType::RestoreAllAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed restore reply";
        return false;
    }
    return true;
}

bool DaemonClient::Shutdown(std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::Shutdown;

    IpcFrame reply;
    return Exchange(request, IpcType::ShutdownAck, reply, error);
}

bool DaemonClient::GetSettings(std::wstring& json, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::GetSettings;

    IpcFrame reply;
    if (!Exchange(request, IpcType::Settings, reply, error)) {
        return false;
    }
    if (!GetTextPayload(reply, json)) {
        error = L"the daemon sent a malformed settings reply";
        return false;
    }
    return true;
}

bool DaemonClient::SetSettings(const std::wstring& json, SetSettingsReply& out,
                               std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::SetSettings;
    if (!SetTextPayload(request, json)) {
        error = L"the settings document is too large";
        return false;
    }

    IpcFrame reply;
    if (!Exchange(request, IpcType::SetSettingsAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed settings acknowledgement";
        return false;
    }
    return true;
}

bool DaemonClient::ApplyAutoCapture(ApplyAutoCaptureReply& out, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::ApplyAutoCapture;

    IpcFrame reply;
    if (!Exchange(request, IpcType::ApplyAutoCaptureAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed auto-capture reply";
        return false;
    }
    return true;
}

bool DaemonClient::SetPrimary(uint64_t hwnd, SetPrimaryReply& out, std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::SetPrimary;
    SetPrimaryRequest payload = {};
    payload.hwnd = hwnd;
    SetPayload(request, payload);

    IpcFrame reply;
    if (!Exchange(request, IpcType::SetPrimaryAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed primary reply";
        return false;
    }
    return true;
}

bool DaemonClient::SetSticky(uint64_t hwnd, bool sticky, SetStickyReply& out,
                             std::wstring& error) {
    IpcFrame request;
    request.type = IpcType::SetSticky;
    SetStickyRequest payload = {};
    payload.hwnd = hwnd;
    payload.sticky = sticky ? 1 : 0;
    SetPayload(request, payload);

    IpcFrame reply;
    if (!Exchange(request, IpcType::SetStickyAck, reply, error)) {
        return false;
    }
    if (!GetPayload(reply, out)) {
        error = L"the daemon sent a malformed sticky reply";
        return false;
    }
    return true;
}

}  // namespace gf
