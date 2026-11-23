#pragma once

#include <windows.h>

#include <string>

#include "ghostframe/protocol.h"

namespace gf {

enum class Arch { X86, X64, Arm64, Other };

// WH_CALLWNDPROC is synchronous and the most reliable bootstrap.  Some
// environments filter it, so the launcher falls back to WH_GETMESSAGE.
enum class InjectionStrategy { CallWndProc, GetMessage };

struct ProcessInfo {
    DWORD pid = 0;
    ULONGLONG createTime = 0;
    Arch arch = Arch::Other;
    DWORD integrity = 0;
};

bool DescribeProcess(DWORD pid, ProcessInfo& info, std::wstring& error);

// Owns the extracted payloads and performs the one-time injection into a target
// process.  The agent keeps itself alive afterwards; this object only needs to
// exist while the launcher runs.
class Injector {
public:
    Injector() = default;
    Injector(const Injector&) = delete;
    Injector& operator=(const Injector&) = delete;

    ~Injector() { Shutdown(); }

    bool Initialize(std::wstring& error);
    void Shutdown();

    bool Inject(HWND target, DWORD launcherPid, uint32_t sessionId, InjectionStrategy strategy,
                std::wstring& error);

    // WH_GETMESSAGE hooks stay installed until the agent proves it attached.
    void ReleasePendingHook();

private:
    bool Extract(int resourceId, const wchar_t* fileName, std::wstring& error);
    bool InjectHook(HWND target, DWORD launcherPid, uint32_t sessionId, InjectionStrategy strategy,
                    std::wstring& error);
    // 32-bit targets go through the helper executable.  It ignores `strategy`
    // and always bootstraps with its own WH_GETMESSAGE hook, so the caller's
    // retry in SessionManager is effectively a third attempt.  This is safe:
    // the agent de-duplicates bootstraps by (launcher pid, session id).
    bool InjectHelper(HWND target, DWORD launcherPid, uint32_t sessionId, std::wstring& error);
    void SweepStalePayloads();

    std::wstring baseDir_;
    std::wstring dir_;
    HMODULE agent64_ = nullptr;
    HOOKPROC hook64_ = nullptr;
    HHOOK pendingHook_ = nullptr;
    UINT message_ = 0;
};

}  // namespace gf
