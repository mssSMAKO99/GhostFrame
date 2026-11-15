#include <windows.h>
#include <string>
#include <vector>
#include "resource.h"

// Цвета в стиле киберпанк / ретро-терминал
const COLORREF COL_BG = RGB(13, 15, 20);
const COLORREF COL_PANEL = RGB(22, 26, 35);
const COLORREF COL_NEON = RGB(0, 255, 136);
const COLORREF COL_NEON_DIM = RGB(0, 140, 75);
const COLORREF COL_TEXT = RGB(220, 230, 240);
const COLORREF COL_WARN = RGB(255, 70, 90);

enum ControlID {
    ID_BTN_STEALTH = 101,
    ID_BTN_BORDER = 102,
    ID_BTN_MIN = 103,
    ID_BTN_RESTORE = 104,
    ID_BTN_CLOSE_T = 105,
    ID_BTN_HIDE_UI = 106,
    ID_BTN_EXIT = 107,
    ID_TIMER_MARQUEE = 1
};

// Глобальное состояние
HWND g_MainHwnd = nullptr;
HWND g_TargetHwnd = nullptr;
HMODULE g_hDll64 = nullptr;
HOOKPROC g_HookProc64 = nullptr;
std::wstring g_TempDir;

bool g_IsHidden = false;
bool g_IsBorderless = false;
bool g_IsLauncherVisible = true;
LONG_PTR g_OrigStyle = 0;
LONG_PTR g_OrigExStyle = 0;

std::wstring g_StatusText = L"STATUS: READY [WAITING FOR TARGET]";
std::wstring g_TargetTitle = L"TARGET: NONE";
std::wstring g_MarqueeText = L"   *** GHOSTFRAME v1.0 // STANDALONE STEALTH INJECTOR (x64/x86) // WDA_EXCLUDEFROMCAPTURE [0x11] + TOOLWINDOW BYPASS // PORTABLE EDITION ***   ";
size_t g_MarqueeOffset = 0;

HFONT g_FontTitle = nullptr;
HFONT g_FontNormal = nullptr;
HFONT g_FontSmall = nullptr;
HBRUSH g_BrushBg = nullptr;
HBRUSH g_BrushPanel = nullptr;

// Распаковка файла из ресурсов .exe на диск
bool ExtractResourceToFile(int resourceId, const std::wstring& outputPath) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return false;

    DWORD dataSize = SizeofResource(nullptr, hRes);
    void* pData = LockResource(hData);
    if (!pData || dataSize == 0) return false;

    HANDLE hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD bytesWritten = 0;
    BOOL success = WriteFile(hFile, pData, dataSize, &bytesWritten, nullptr);
    CloseHandle(hFile);

    return (success && bytesWritten == dataSize);
}

// Подготовка временной директории и распаковка всех компонентов
bool InitEmbeddedPayload() {
    wchar_t tempPath[MAX_PATH] = { 0 };
    if (!GetTempPathW(MAX_PATH, tempPath)) return false;

    g_TempDir = std::wstring(tempPath) + L"GhostFrame_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_TempDir.c_str(), nullptr);

    if (!ExtractResourceToFile(IDR_HOOK_DLL_64, g_TempDir + L"\\HideHook64.dll")) return false;
    if (!ExtractResourceToFile(IDR_HELPER_EXE_32, g_TempDir + L"\\Helper32.exe"))   return false;
    if (!ExtractResourceToFile(IDR_HOOK_DLL_32, g_TempDir + L"\\HideHook32.dll")) return false;

    return true;
}

// Удаление временных файлов при выходе
void CleanupEmbeddedPayload() {
    if (g_hDll64) {
        FreeLibrary(g_hDll64);
        g_hDll64 = nullptr;
    }
    if (!g_TempDir.empty()) {
        DeleteFileW((g_TempDir + L"\\HideHook64.dll").c_str());
        DeleteFileW((g_TempDir + L"\\Helper32.exe").c_str());
        DeleteFileW((g_TempDir + L"\\HideHook32.dll").c_str());
        RemoveDirectoryW(g_TempDir.c_str());
    }
}

bool IsWindow32Bit(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return false;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    BOOL isWow64 = FALSE;
    IsWow64Process(hProcess, &isWow64);
    CloseHandle(hProcess);
    return isWow64 == TRUE;
}

bool ApplyCaptureAffinity(HWND hwnd, bool hide) {
    if (IsWindow32Bit(hwnd)) {
        std::wstring exePath = g_TempDir + L"\\Helper32.exe";
        std::wstring cmd = L"\"" + exePath + L"\" " + std::to_wstring(reinterpret_cast<ULONG_PTR>(hwnd)) + (hide ? L" 1" : L" 0");

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_TempDir.c_str(), &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return true;
        }
        return false;
    }
    else {
        DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
        HHOOK hHook = SetWindowsHookExW(WH_CALLWNDPROC, g_HookProc64, g_hDll64, threadId);
        if (!hHook) return false;
        SendMessageW(hwnd, WM_USER + 777, hide ? 1 : 0, 0);
        UnhookWindowsHookEx(hHook);
        return true;
    }
}

void SetTaskbarAndAltTabVisibility(HWND hwnd, bool hide) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (hide) {
        exStyle &= ~WS_EX_APPWINDOW;
        exStyle |= WS_EX_TOOLWINDOW;
    }
    else {
        exStyle = g_OrigExStyle;
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    ShowWindow(hwnd, SW_HIDE);
    ShowWindow(hwnd, SW_SHOW);
}

void ToggleWindowBorder(HWND hwnd, bool borderless) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (borderless) {
        style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    }
    else {
        style = g_OrigStyle;
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

void UpdateTargetInfo(HWND hwnd) {
    wchar_t title[256] = { 0 };
    GetWindowTextW(hwnd, title, 255);
    std::wstring t(title);
    if (t.length() > 32) t = t.substr(0, 29) + L"...";
    if (t.empty()) t = L"[UNTITLED WINDOW]";
    bool is32 = IsWindow32Bit(hwnd);
    g_TargetTitle = L"TARGET: " + t + (is32 ? L" [x86]" : L" [x64]");
}

void CaptureForegroundTarget() {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_MainHwnd && fg != g_TargetHwnd) {
        g_TargetHwnd = fg;
        g_IsHidden = false;
        g_IsBorderless = false;
        g_OrigStyle = GetWindowLongPtrW(g_TargetHwnd, GWL_STYLE);
        g_OrigExStyle = GetWindowLongPtrW(g_TargetHwnd, GWL_EXSTYLE);
        UpdateTargetInfo(g_TargetHwnd);
    }
}

void ExecuteAction(int actionId) {
    if (actionId == ID_BTN_HIDE_UI) {
        g_IsLauncherVisible = !g_IsLauncherVisible;
        ShowWindow(g_MainHwnd, g_IsLauncherVisible ? SW_SHOW : SW_HIDE);
        return;
    }
    if (actionId == ID_BTN_EXIT) {
        PostQuitMessage(0);
        return;
    }

    if (actionId == ID_BTN_STEALTH || !IsWindow(g_TargetHwnd)) {
        CaptureForegroundTarget();
    }

    if (!IsWindow(g_TargetHwnd)) {
        g_StatusText = L"ERROR: NO ACTIVE TARGET WINDOW";
        InvalidateRect(g_MainHwnd, nullptr, FALSE);
        return;
    }

    switch (actionId) {
    case ID_BTN_STEALTH: {
        g_IsHidden = !g_IsHidden;
        if (ApplyCaptureAffinity(g_TargetHwnd, g_IsHidden)) {
            SetTaskbarAndAltTabVisibility(g_TargetHwnd, g_IsHidden);
            g_StatusText = g_IsHidden ? L"STATUS: STEALTH ENGAGED [0x11 + HIDDEN]" : L"STATUS: TARGET RESTORED [VISIBLE]";
        }
        else {
            g_IsHidden = !g_IsHidden;
            g_StatusText = L"ERROR: HOOK FAILED (ACCESS DENIED)";
        }
        break;
    }
    case ID_BTN_BORDER:
        g_IsBorderless = !g_IsBorderless;
        ToggleWindowBorder(g_TargetHwnd, g_IsBorderless);
        g_StatusText = g_IsBorderless ? L"STATUS: BORDERLESS MODE ON" : L"STATUS: WINDOW FRAME RESTORED";
        break;

    case ID_BTN_MIN:
        ShowWindow(g_TargetHwnd, SW_MINIMIZE);
        g_StatusText = L"STATUS: TARGET MINIMIZED";
        break;

    case ID_BTN_RESTORE:
        ShowWindow(g_TargetHwnd, SW_RESTORE);
        SetForegroundWindow(g_TargetHwnd);
        g_StatusText = L"STATUS: TARGET RESTORED";
        break;

    case ID_BTN_CLOSE_T:
        PostMessageW(g_TargetHwnd, WM_CLOSE, 0, 0);
        g_TargetHwnd = nullptr;
        g_TargetTitle = L"TARGET: NONE";
        g_StatusText = L"STATUS: CLOSE SIGNAL SENT";
        break;
    }
    InvalidateRect(g_MainHwnd, nullptr, FALSE);
}

void DrawCustomButton(DRAWITEMSTRUCT* dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    bool isPressed = (dis->itemState & ODS_SELECTED);
    bool isActive = false;

    if (dis->CtlID == ID_BTN_STEALTH && g_IsHidden) isActive = true;
    if (dis->CtlID == ID_BTN_BORDER && g_IsBorderless) isActive = true;

    COLORREF bgCol = isPressed ? COL_NEON_DIM : (isActive ? RGB(15, 55, 38) : COL_PANEL);
    COLORREF borderCol = (dis->CtlID == ID_BTN_EXIT || dis->CtlID == ID_BTN_CLOSE_T) ? COL_WARN : (isActive ? COL_NEON : COL_NEON_DIM);
    COLORREF textCol = (dis->CtlID == ID_BTN_EXIT || dis->CtlID == ID_BTN_CLOSE_T) ? COL_WARN : (isActive ? COL_NEON : COL_TEXT);

    HBRUSH hBrush = CreateSolidBrush(bgCol);
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HPEN hPen = CreatePen(PS_SOLID, 1, borderCol);
    HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(hPen);

    wchar_t text[128] = { 0 };
    GetWindowTextW(dis->hwndItem, text, 127);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textCol);
    SelectObject(hdc, g_FontNormal);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SetWindowDisplayAffinity(hwnd, 0x00000011);

        g_FontTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        g_FontNormal = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        g_FontSmall = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        g_BrushBg = CreateSolidBrush(COL_BG);
        g_BrushPanel = CreateSolidBrush(COL_PANEL);

        CreateWindowW(L"BUTTON", L"X", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 425, 10, 25, 22, hwnd, (HMENU)ID_BTN_EXIT, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"_", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 395, 10, 25, 22, hwnd, (HMENU)ID_BTN_HIDE_UI, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"[CTRL+SHIFT+H] TOGGLE STEALTH (0x11 + TASKBAR)", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 20, 105, 430, 32, hwnd, (HMENU)ID_BTN_STEALTH, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"[CTRL+SHIFT+B] TOGGLE WINDOW BORDER", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 20, 145, 430, 32, hwnd, (HMENU)ID_BTN_BORDER, nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"[DOWN] MINIMIZE", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 20, 185, 138, 30, hwnd, (HMENU)ID_BTN_MIN, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"[UP] RESTORE", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 166, 185, 138, 30, hwnd, (HMENU)ID_BTN_RESTORE, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"[Q] KILL TARGET", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 312, 185, 138, 30, hwnd, (HMENU)ID_BTN_CLOSE_T, nullptr, nullptr);

        RegisterHotKey(hwnd, ID_BTN_STEALTH, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'H');
        RegisterHotKey(hwnd, ID_BTN_BORDER, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'B');
        RegisterHotKey(hwnd, ID_BTN_MIN, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_DOWN);
        RegisterHotKey(hwnd, ID_BTN_RESTORE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_UP);
        RegisterHotKey(hwnd, ID_BTN_CLOSE_T, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q');
        RegisterHotKey(hwnd, ID_BTN_HIDE_UI, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'M');

        SetTimer(hwnd, ID_TIMER_MARQUEE, 90, nullptr);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER_MARQUEE) {
            g_MarqueeOffset = (g_MarqueeOffset + 1) % g_MarqueeText.length();
            RECT rcMarquee = { 10, 232, 460, 252 };
            InvalidateRect(hwnd, &rcMarquee, FALSE);
        }
        return 0;
    }

    case WM_HOTKEY:
        ExecuteAction(static_cast<int>(wParam));
        return 0;

    case WM_COMMAND:
        ExecuteAction(LOWORD(wParam));
        return 0;

    case WM_DRAWITEM:
        DrawCustomButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit == HTCLIENT) return HTCAPTION;
        return hit;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        FillRect(hdc, &rc, g_BrushBg);

        HPEN hPen = CreatePen(PS_SOLID, 2, COL_NEON);
        HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 1, 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(hPen);

        SetBkMode(hdc, TRANSPARENT);

        SetTextColor(hdc, COL_NEON);
        SelectObject(hdc, g_FontTitle);
        RECT rcTitle = { 20, 10, 380, 35 };
        DrawTextW(hdc, L"// GHOSTFRAME_STEALTH_TOOL", -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT rcInfo = { 20, 42, 450, 95 };
        FillRect(hdc, &rcInfo, g_BrushPanel);

        SelectObject(hdc, g_FontNormal);
        SetTextColor(hdc, COL_TEXT);
        RECT rcTarget = { 30, 48, 440, 68 };
        DrawTextW(hdc, g_TargetTitle.c_str(), -1, &rcTarget, DT_LEFT | DT_SINGLELINE);

        SetTextColor(hdc, g_IsHidden ? COL_NEON : RGB(150, 160, 175));
        RECT rcStatus = { 30, 70, 440, 90 };
        DrawTextW(hdc, g_StatusText.c_str(), -1, &rcStatus, DT_LEFT | DT_SINGLELINE);

        SetTextColor(hdc, COL_NEON_DIM);
        SelectObject(hdc, g_FontSmall);
        std::wstring scrolled = g_MarqueeText.substr(g_MarqueeOffset) + g_MarqueeText.substr(0, g_MarqueeOffset);
        RECT rcMarquee = { 15, 232, 455, 252 };
        DrawTextW(hdc, scrolled.c_str(), -1, &rcMarquee, DT_LEFT | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_MARQUEE);
        for (int id : { ID_BTN_STEALTH, ID_BTN_BORDER, ID_BTN_MIN, ID_BTN_RESTORE, ID_BTN_CLOSE_T, ID_BTN_HIDE_UI }) {
            UnregisterHotKey(hwnd, id);
        }
        DeleteObject(g_FontTitle);
        DeleteObject(g_FontNormal);
        DeleteObject(g_FontSmall);
        DeleteObject(g_BrushBg);
        DeleteObject(g_BrushPanel);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    if (!InitEmbeddedPayload()) {
        MessageBoxW(nullptr, L"Ошибка распаковки встроенных компонентов!", L"GhostFrame Error", MB_ICONERROR);
        CleanupEmbeddedPayload();
        return 1;
    }

    g_hDll64 = LoadLibraryW((g_TempDir + L"\\HideHook64.dll").c_str());
    if (!g_hDll64) {
        MessageBoxW(nullptr, L"Не удалось загрузить HideHook64.dll из временной папки!", L"GhostFrame Error", MB_ICONERROR);
        CleanupEmbeddedPayload();
        return 1;
    }

    g_HookProc64 = reinterpret_cast<HOOKPROC>(GetProcAddress(g_hDll64, "HookProc"));
    if (!g_HookProc64) {
        MessageBoxW(nullptr, L"Функция HookProc не найдена!", L"GhostFrame Error", MB_ICONERROR);
        CleanupEmbeddedPayload();
        return 1;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"GhostFrameRetroClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int winW = 470, winH = 260;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    g_MainHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"GhostFrame",
        WS_POPUP | WS_VISIBLE,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(g_MainHwnd, nCmdShow);
    UpdateWindow(g_MainHwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CleanupEmbeddedPayload();
    return 0;
}
