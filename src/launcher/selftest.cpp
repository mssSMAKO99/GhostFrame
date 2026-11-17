#include "selftest.h"

#include <stdio.h>

#include <string>

#include "injector.h"
#include "log.h"
#include "session.h"

namespace gf {

namespace {

constexpr wchar_t kTargetClass[] = L"GfTestTarget";

std::wstring TargetStatePath() {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"GfTestTarget.state";
}

std::wstring SelfTestLogPath() {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    return std::wstring(temp) + L"GhostFrame.selftest.log";
}

void Say(const std::wstring& text) {
    wprintf(L"%s\n", text.c_str());

    FILE* file = nullptr;
    if (_wfopen_s(&file, SelfTestLogPath().c_str(), L"a, ccs=UTF-8") == 0 && file != nullptr) {
        fwprintf(file, L"%s\n", text.c_str());
        fclose(file);
    }
}

struct TargetStats {
    unsigned long long samples = 0;
    unsigned long long excluded = 0;
    unsigned last = 0;
    bool valid = false;
};

TargetStats ReadTargetStats() {
    TargetStats stats;

    FILE* file = nullptr;
    if (_wfopen_s(&file, TargetStatePath().c_str(), L"rb") != 0 || file == nullptr) {
        return stats;
    }

    char buffer[256] = {};
    const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (read == 0) {
        return stats;
    }

    unsigned long long samples = 0;
    unsigned long long excluded = 0;
    unsigned last = 0;
    if (sscanf_s(buffer, "samples=%llu excluded=%llu last=0x%X", &samples, &excluded, &last) == 3) {
        stats.samples = samples;
        stats.excluded = excluded;
        stats.last = last;
        stats.valid = true;
    }
    return stats;
}

int Fail(const std::wstring& what, const std::wstring& detail = L"") {
    Say(L"FAIL: " + what + L" " + detail);
    return 1;
}

}  // namespace

int RunSelfTest() {
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        ::AllocConsole();
    }
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);

    wchar_t tempDir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempDir);
    LogInit(std::wstring(tempDir) + L"GhostFrame");
    LogLine(L"selftest start");

    HWND target = nullptr;
    for (int attempt = 0; attempt < 60 && target == nullptr; ++attempt) {
        target = ::FindWindowW(kTargetClass, nullptr);
        if (target == nullptr) {
            ::Sleep(100);
        }
    }
    if (target == nullptr) {
        Say(L"SKIP: the test target window was not found");
        return 2;
    }

    Injector injector;
    std::wstring error;
    if (!injector.Initialize(error)) {
        return Fail(L"payload preparation:", error);
    }

    SessionManager sessions(injector);

    if (!sessions.Attach(target, error)) {
        injector.Shutdown();
        return Fail(L"attach:", error);
    }

    if (!sessions.Send(0, Cmd::Hide, error)) {
        injector.Shutdown();
        return Fail(L"hide:", error);
    }

    ::Sleep(1500);

    const TargetStats hidden = ReadTargetStats();
    if (!sessions.At(0).hidden()) {
        injector.Shutdown();
        return Fail(L"the session is not marked as hidden");
    }
    if (sessions.At(0).wda != 0x11) {
        injector.Shutdown();
        return Fail(L"the agent reports an unexpected display affinity");
    }
    if (!hidden.valid || hidden.samples == 0) {
        injector.Shutdown();
        return Fail(L"the test target did not report any samples");
    }
    if (hidden.excluded * 2 < hidden.samples) {
        injector.Shutdown();
        return Fail(L"the guard did not keep the window excluded (excluded=" +
                    std::to_wstring(hidden.excluded) + L", samples=" +
                    std::to_wstring(hidden.samples) + L")");
    }

    if (!sessions.Send(0, Cmd::Show, error)) {
        injector.Shutdown();
        return Fail(L"show:", error);
    }

    ::Sleep(1500);
    const TargetStats shown = ReadTargetStats();
    if (sessions.At(0).hidden()) {
        injector.Shutdown();
        return Fail(L"the session is still marked as hidden after show");
    }

    const unsigned long long sampledAfterShow = shown.samples - hidden.samples;
    const unsigned long long excludedAfterShow = shown.excluded - hidden.excluded;
    if (sampledAfterShow == 0) {
        injector.Shutdown();
        return Fail(L"the test target stopped reporting samples");
    }
    if (excludedAfterShow * 4 > sampledAfterShow) {
        injector.Shutdown();
        return Fail(L"the window is still excluded after show (excluded=" +
                    std::to_wstring(excludedAfterShow) + L", samples=" +
                    std::to_wstring(sampledAfterShow) + L")");
    }

    if (!sessions.Send(0, Cmd::Uninstall, error)) {
        injector.Shutdown();
        return Fail(L"uninstall:", error);
    }

    if (sessions.At(0).ipc.state()->status != static_cast<LONG>(Status::Detached)) {
        injector.Shutdown();
        return Fail(L"the agent did not detach");
    }

    sessions.RestoreAll();
    injector.Shutdown();
    Say(L"PASS");
    return 0;
}

}  // namespace gf
