#include <windows.h>

#include <stdio.h>

namespace {

constexpr wchar_t kClassName[] = L"GfTestTarget";
constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT_PTR kResetTimer = 2;

unsigned long long g_samples = 0;
unsigned long long g_excluded = 0;
DWORD g_last = 0;
wchar_t g_statePath[MAX_PATH] = {};

void WriteState() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_statePath, L"wb") != 0 || file == nullptr) {
        return;
    }
    fprintf(file, "samples=%llu excluded=%llu last=0x%08X\n", g_samples, g_excluded, g_last);
    fclose(file);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            ::SetTimer(hwnd, kSampleTimer, 50, nullptr);
            ::SetTimer(hwnd, kResetTimer, 1000, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == kSampleTimer) {
                DWORD value = 0;
                if (::GetWindowDisplayAffinity(hwnd, &value)) {
                    ++g_samples;
                    if (value == 0x11) {
                        ++g_excluded;
                    }
                    g_last = value;
                }
                WriteState();
            } else if (wParam == kResetTimer) {
                // Pretend to be an application that keeps resetting the
                // affinity; the agent has to win this race while hidden.
                ::SetWindowDisplayAffinity(hwnd, WDA_NONE);
            }
            return 0;

        case WM_DESTROY:
            ::KillTimer(hwnd, kSampleTimer);
            ::KillTimer(hwnd, kResetTimer);
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    wcscpy_s(g_statePath, temp);
    wcscat_s(g_statePath, L"GfTestTarget.state");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));

    if (!::RegisterClassExW(&windowClass)) {
        return 1;
    }

    HWND window = ::CreateWindowExW(0, kClassName, L"GhostFrame Test Target",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, 140, 140, 420, 300, nullptr,
                                    nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    ::ShowWindow(window, SW_SHOW);
    ::UpdateWindow(window);

    MSG message = {};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return 0;
}
