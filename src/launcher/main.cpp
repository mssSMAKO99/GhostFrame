#include <windows.h>
#include <shellapi.h>

#include <cwchar>

#include "app.h"
#include "selftest.h"

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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    const bool wantsSelfTest = HasFlag(argc, argv, L"--selftest");
    const bool noSelfExclude = HasFlag(argc, argv, L"--no-self-exclude");
    if (argv != nullptr) {
        ::LocalFree(argv);
    }

    if (wantsSelfTest) {
        return gf::RunSelfTest();
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
