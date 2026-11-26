#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#include "app.h"
#include "daemon.h"
#include "launch.h"
#include "self_delete.h"
#include "selftest.h"
#include "shortcuts.h"

namespace {

// CommandLineToArgvW follows the same rules as the CRT, so a file name or an
// unrelated argument that merely contains "--selftest" is not treated as the
// flag.
bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

// `--launch "<app.exe>"` (or `--launch=<app.exe>`) names the application to
// start; `--capture` and `--hide` control whether it is attached right away.
std::wstring LaunchTarget(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--launch") == 0 && i + 1 < argc) {
            return argv[i + 1];
        }
        static const wchar_t kPrefix[] = L"--launch=";
        const size_t prefixLength = (sizeof(kPrefix) / sizeof(kPrefix[0])) - 1;
        if (std::wcsncmp(argv[i], kPrefix, prefixLength) == 0) {
            return argv[i] + prefixLength;
        }
    }
    return std::wstring();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    const bool wantsSelfTest = HasFlag(argc, argv, L"--selftest");
    const bool wantsDaemon = HasFlag(argc, argv, L"--daemon");
    const bool wantsDaemonCheck = HasFlag(argc, argv, L"--daemon-check");
    const bool wantsRemoveShortcuts = HasFlag(argc, argv, L"--remove-shortcuts");
    const bool noSelfExclude = HasFlag(argc, argv, L"--no-self-exclude");
    const bool launchCapture = HasFlag(argc, argv, L"--capture");
    const bool launchHide = HasFlag(argc, argv, L"--hide");
    std::wstring launchTarget = LaunchTarget(argc, argv);

    // The helper runs from a detached %TEMP% copy after the UI and the daemon
    // have exited, so parse its arguments before `argv` is released.
    gf::SelfDeleteHelperOptions selfDeleteHelper;
    std::wstring selfDeleteError;
    const bool wantsSelfDeleteHelper =
        gf::ParseSelfDeleteHelperArguments(argc, argv, selfDeleteHelper, selfDeleteError);

    if (argv != nullptr) {
        ::LocalFree(argv);
    }

    if (wantsSelfDeleteHelper) {
        if (!selfDeleteError.empty()) {
            fwprintf(stderr, L"GhostFrame self-delete: %s\n", selfDeleteError.c_str());
            return 1;
        }
        return gf::RunSelfDeleteHelper(selfDeleteHelper);
    }

    if (wantsSelfTest) {
        return gf::RunSelfTest();
    }
    if (wantsDaemon) {
        return gf::RunDaemon();
    }
    if (wantsDaemonCheck) {
        return gf::RunDaemonCheck();
    }
    if (wantsRemoveShortcuts) {
        // Uninstall helper: delete every link recorded by the shortcut manager
        // before the data directory and the executable are removed.
        std::wstring error;
        gf::RemoveAllShortcuts(error);
        return error.empty() ? 0 : 1;
    }
    if (!launchTarget.empty()) {
        return gf::RunLaunch(launchTarget, launchCapture, launchHide);
    }

    App app;
    const App::InitResult result = app.Init(instance, noSelfExclude);

    if (result == App::InitResult::AlreadyRunning) {
        return 0;
    }
    if (result == App::InitResult::Failed) {
        app.Shutdown();
        return 1;
    }

    const int exitCode = app.Run(showCommand);
    app.Shutdown();
    return exitCode;
}