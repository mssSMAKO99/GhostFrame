#include <windows.h>
#include <string>

// Запуск: Helper32.exe <HWND_в_виде_числа> <1_или_0>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR pCmdLine, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 3) return 1;

    // Восстанавливаем дескриптор окна из переданного числа
    HWND targetHwnd = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(std::stoull(argv[1])));
    int hideFlag = std::stoi(argv[2]);
    LocalFree(argv);

    if (!IsWindow(targetHwnd)) return 1;

    HMODULE hDll = LoadLibraryW(L"HideHook32.dll");
    if (!hDll) return 1;

    HOOKPROC hookProc = reinterpret_cast<HOOKPROC>(GetProcAddress(hDll, "HookProc"));
    if (hookProc) {
        DWORD threadId = GetWindowThreadProcessId(targetHwnd, nullptr);
        HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, hDll, threadId);
        if (hHook) {
            SendMessageW(targetHwnd, WM_USER + 777, hideFlag, 0);
            UnhookWindowsHookEx(hHook);
        }
    }

    FreeLibrary(hDll);
    return 0;
}
