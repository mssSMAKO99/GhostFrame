#include <windows.h>

#include <cwchar>

#include "app.h"
#include "selftest.h"

namespace {

bool HasFlag(const wchar_t* commandLine, const wchar_t* flag) {
    return commandLine != nullptr && std::wcsstr(commandLine, flag) != nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const wchar_t* commandLine = ::GetCommandLineW();

    if (HasFlag(commandLine, L"--selftest")) {
        return gf::RunSelfTest();
    }

    App app;
    const App::InitResult result =
        app.Init(instance, HasFlag(commandLine, L"--no-self-exclude"));

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
