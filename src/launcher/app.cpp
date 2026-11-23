#include "app.h"

#include <shellapi.h>
#include <windowsx.h>

#include "ghostframe/version.h"
#include "log.h"
#include "resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"GhostFrame.MainWindow";
constexpr int kBaseWidth = 560;
constexpr int kBaseHeight = 452;
constexpr int kTitleHeight = 36;
constexpr int kListTop = 92;
constexpr int kRowHeight = 34;
constexpr int kMaxRows = 5;
constexpr UINT kTrayMessage = WM_APP + 1;

const wchar_t* StateLabel(const gf::Session& session) {
    if (session.errored()) {
        return L"ERROR";
    }
    if (session.hidden()) {
        return L"HIDDEN";
    }
    return L"VISIBLE";
}

COLORREF StateColor(const gf::Session& session) {
    if (session.errored()) {
        return ui::kWarn;
    }
    if (session.hidden()) {
        return ui::kNeon;
    }
    return ui::kMuted;
}

}  // namespace

App::InitResult App::Init(HINSTANCE instance, bool noSelfExclude) {
    instance_ = instance;
    noSelfExclude_ = noSelfExclude;

    wchar_t localAppData[MAX_PATH] = {};
    std::wstring logDir;
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
        logDir = std::wstring(localAppData) + L"\\GhostFrame";
        if (!::CreateDirectoryW(logDir.c_str(), nullptr) &&
            ::GetLastError() != ERROR_ALREADY_EXISTS) {
            logDir.clear();
        }
    }
    if (logDir.empty()) {
        wchar_t tempPath[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, tempPath) != 0) {
            logDir = std::wstring(tempPath) + L"GhostFrame";
        }
    }
    gf::LogInit(logDir);
    gf::LogLine(L"starting GhostFrame %s", GF_VERSION_STRING);

    mutex_ = ::CreateMutexW(nullptr, TRUE, L"Local\\GhostFrame.Instance.v2");
    if (mutex_ == nullptr) {
        ShowError(L"cannot create the single-instance mutex:\n" +
                  gf::Win32Message(::GetLastError()));
        return InitResult::Failed;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = ::FindWindowW(kWindowClass, nullptr)) {
            ::ShowWindow(existing, SW_SHOW);
            ::SetForegroundWindow(existing);
        }
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
        return InitResult::AlreadyRunning;
    }

    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring error;
    if (!injector_.Initialize(error)) {
        gf::LogLine(L"payload preparation failed: %s", error.c_str());
        ShowError(L"GhostFrame could not prepare its payloads:\n\n" + error);
        return InitResult::Failed;
    }

    taskbarCreated_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &App::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = windowClass.hIcon;

    if (!::RegisterClassExW(&windowClass)) {
        ShowError(L"cannot register the window class:\n" + gf::Win32Message(::GetLastError()));
        return InitResult::Failed;
    }

    const UINT dpi = ::GetDpiForSystem();
    const int width = MulDiv(kBaseWidth, dpi, 96);
    const int height = MulDiv(kBaseHeight, dpi, 96);

    POINT cursor = {};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    const RECT& work = monitorInfo.rcWork;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;

    hwnd_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"GhostFrame",
                              WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, nullptr, nullptr,
                              instance, this);
    if (hwnd_ == nullptr) {
        ShowError(L"cannot create the GhostFrame window:\n" + gf::Win32Message(::GetLastError()));
        return InitResult::Failed;
    }

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    return InitResult::Ok;
}

int App::Run(int showCommand) {
    ::ShowWindow(hwnd_, showCommand);
    ::UpdateWindow(hwnd_);

    MSG message = {};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::Shutdown() {
    sessions_.RestoreAll();
    injector_.Shutdown();
    gf::LogLine(L"shutdown");
    gf::LogShutdown();
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }

    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->Handle(message, wParam, lParam);
}

LRESULT App::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            OnCreate();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_REFRESH) {
                OnTimer();
            }
            return 0;

        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wParam));
            return 0;

        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            bool active = false;
            if (SelectedValid() && item->CtlID == ID_BTN_TOGGLE) {
                active = sessions_.At(static_cast<size_t>(selected_)).hidden();
            }
            ui::DrawButton(item, theme_, active, item->CtlID == ID_BTN_CLOSE);
            return TRUE;
        }

        case WM_LBUTTONDOWN: {
            const int baseY = MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(theme_.dpi));
            if (baseY >= kListTop && baseY < kListTop + kMaxRows * kRowHeight) {
                const int row = (baseY - kListTop) / kRowHeight;
                if (row >= 0 && row < static_cast<int>(sessions_.Count())) {
                    SelectTarget(row);
                }
            }
            return 0;
        }

        case WM_NCHITTEST: {
            const LRESULT hit = ::DefWindowProcW(hwnd_, message, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ScreenToClient(hwnd_, &point);
                if (MulDiv(point.y, 96, static_cast<int>(theme_.dpi)) < kTitleHeight) {
                    return HTCAPTION;
                }
            }
            return hit;
        }

        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_CLOSE:
            RestoreAll();
            ::DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            OnDestroy();
            return 0;

        default:
            if (taskbarCreated_ != 0 && message == taskbarCreated_) {
                tray_ = false;
                AddTray();
                return 0;
            }
            if (message == kTrayMessage) {
                OnTray(lParam);
                return 0;
            }
            break;
    }

    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void App::OnCreate() {
    theme_.Create(::GetDpiForWindow(hwnd_));
    CreateChildren();
    Layout();

    ::SetTimer(hwnd_, ID_TIMER_REFRESH, 250, nullptr);
    RegisterHotkeys();
    AddTray();

    if (!noSelfExclude_) {
        ::SetWindowDisplayAffinity(hwnd_, 0x11);
    }

    SetStatus(L"READY — FOCUS A WINDOW AND PRESS CTRL+SHIFT+H");
}

void App::OnDestroy() {
    ::KillTimer(hwnd_, ID_TIMER_REFRESH);
    UnregisterHotkeys();
    RemoveTray();
    theme_.Destroy();

    if (mutex_ != nullptr) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }

    ::PostQuitMessage(0);
}

void App::CreateChildren() {
    struct ButtonSpec {
        int id;
        const wchar_t* text;
    };
    const ButtonSpec buttons[] = {
        {ID_BTN_HIDE_UI, L"_"},
        {ID_BTN_EXIT, L"X"},
        {ID_BTN_TOGGLE, L"[CTRL+SHIFT+H]  TOGGLE STEALTH"},
        {ID_BTN_RESTORE_ALL, L"[CTRL+SHIFT+R]  RESTORE ALL TARGETS"},
        {ID_BTN_MIN, L"[DOWN] MINIMIZE"},
        {ID_BTN_RESTORE, L"[UP] RESTORE"},
        {ID_BTN_CLOSE, L"[Q] CLOSE TARGET"},
    };

    for (const ButtonSpec& spec : buttons) {
        ::CreateWindowExW(0, L"BUTTON", spec.text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                          0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)),
                          instance_, nullptr);
    }
}

void App::Layout() {
    auto place = [this](int id, int x, int y, int width, int height) {
        HWND child = ::GetDlgItem(hwnd_, id);
        if (child != nullptr) {
            ::SetWindowPos(child, nullptr, theme_.S(x), theme_.S(y), theme_.S(width),
                           theme_.S(height), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    place(ID_BTN_HIDE_UI, kBaseWidth - 78, 7, 30, 22);
    place(ID_BTN_EXIT, kBaseWidth - 44, 7, 30, 22);
    place(ID_BTN_TOGGLE, 16, 274, kBaseWidth - 32, 32);
    place(ID_BTN_RESTORE_ALL, 16, 350, kBaseWidth - 32, 30);
    place(ID_BTN_MIN, 16, 388, 168, 30);
    place(ID_BTN_RESTORE, 196, 388, 168, 30);
    place(ID_BTN_CLOSE, 376, 388, 168, 30);
}

void App::RegisterHotkeys() {
    struct HotkeySpec {
        int id;
        UINT virtualKey;
        const wchar_t* label;
    };
    const HotkeySpec hotkeys[] = {
        {ID_BTN_TOGGLE, 'H', L"Ctrl+Shift+H"},
        {ID_BTN_RESTORE_ALL, 'R', L"Ctrl+Shift+R"},
        {ID_BTN_HIDE_UI, 'M', L"Ctrl+Shift+M"},
        {ID_BTN_MIN, VK_DOWN, L"Ctrl+Shift+Down"},
        {ID_BTN_RESTORE, VK_UP, L"Ctrl+Shift+Up"},
        {ID_BTN_CLOSE, 'Q', L"Ctrl+Shift+Q"},
    };

    std::wstring failed;
    for (const HotkeySpec& hotkey : hotkeys) {
        if (!::RegisterHotKey(hwnd_, hotkey.id, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                              hotkey.virtualKey)) {
            if (!failed.empty()) {
                failed += L", ";
            }
            failed += hotkey.label;
        }
    }

    if (!failed.empty()) {
        SetStatus(L"HOTKEY CONFLICT — ALREADY IN USE: " + failed, true);
    }
}

void App::UnregisterHotkeys() {
    const int ids[] = {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_HIDE_UI,
                       ID_BTN_MIN,    ID_BTN_RESTORE,   ID_BTN_CLOSE};
    for (int id : ids) {
        ::UnregisterHotKey(hwnd_, id);
    }
}

void App::OnPaint() {
    PAINTSTRUCT paint = {};
    HDC dc = ::BeginPaint(hwnd_, &paint);

    RECT client = {};
    ::GetClientRect(hwnd_, &client);

    HDC memory = ::CreateCompatibleDC(dc);
    HBITMAP bitmap = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
    HGDIOBJ oldBitmap = ::SelectObject(memory, bitmap);

    PaintInto(memory, client);

    ::BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);

    ::SelectObject(memory, oldBitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::EndPaint(hwnd_, &paint);
}

void App::PaintInto(HDC dc, const RECT& client) {
    ui::FillSolid(dc, client, ui::kBg);

    const RECT titleBar = {0, 0, client.right, theme_.S(kTitleHeight)};
    ui::FillSolid(dc, titleBar, ui::kPanel);
    const RECT titleLine = {0, theme_.S(kTitleHeight) - 1, client.right, theme_.S(kTitleHeight)};
    ui::FillSolid(dc, titleLine, ui::kNeonDim);

    RECT titleRect = {theme_.S(16), 0, theme_.S(kBaseWidth - 150), theme_.S(kTitleHeight)};
    ui::DrawLabel(dc, L"// GHOSTFRAME", titleRect, theme_.title, ui::kNeon,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT versionRect = {theme_.S(kBaseWidth - 140), 0, theme_.S(kBaseWidth - 84),
                        theme_.S(kTitleHeight)};
    ui::DrawLabel(dc, L"v" GF_VERSION_STRING, versionRect, theme_.small, ui::kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT statusRect = {theme_.S(16), theme_.S(42), theme_.S(kBaseWidth - 16), theme_.S(68)};
    ui::DrawLabel(dc, status_.c_str(), statusRect, theme_.body,
                  statusError_ ? ui::kWarn : ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT headerLeft = {theme_.S(16), theme_.S(74), theme_.S(220), theme_.S(90)};
    ui::DrawLabel(dc, L"TARGETS", headerLeft, theme_.small, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    wchar_t countText[64] = {};
    swprintf_s(countText, L"%zu TRACKED", sessions_.Count());
    RECT headerRight = {theme_.S(kBaseWidth - 220), theme_.S(74), theme_.S(kBaseWidth - 16),
                        theme_.S(90)};
    ui::DrawLabel(dc, countText, headerRight, theme_.small, ui::kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    for (int row = 0; row < kMaxRows; ++row) {
        RECT rowRect = {theme_.S(16), theme_.S(kListTop + row * kRowHeight),
                        theme_.S(kBaseWidth - 16),
                        theme_.S(kListTop + row * kRowHeight + kRowHeight - 4)};

        if (row < static_cast<int>(sessions_.Count())) {
            const gf::Session& session = sessions_.At(static_cast<size_t>(row));
            const bool selected = row == selected_;

            ui::FillSolid(dc, rowRect, selected ? ui::kPanelHi : ui::kPanel);
            ui::StrokeFrame(dc, rowRect, selected ? ui::kNeon : ui::kBorder, 1);

            RECT accent = {rowRect.left + 1, rowRect.top + 1, rowRect.left + theme_.S(4),
                           rowRect.bottom - 1};
            ui::FillSolid(dc, accent, selected ? ui::kNeon
                                              : (session.errored() ? ui::kWarn : ui::kNeonDim));

            RECT titleText = {rowRect.left + theme_.S(14), rowRect.top,
                              rowRect.right - theme_.S(230), rowRect.bottom};
            ui::DrawLabel(dc, session.title.c_str(), titleText, theme_.body,
                          session.errored() ? ui::kWarn : ui::kText,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                              DT_NOPREFIX);

            wchar_t meta[96] = {};
            swprintf_s(meta, L"PID %u  %s", session.pid, session.x86 ? L"x86" : L"x64");
            RECT metaRect = {rowRect.right - theme_.S(226), rowRect.top,
                             rowRect.right - theme_.S(112), rowRect.bottom};
            ui::DrawLabel(dc, meta, metaRect, theme_.small, ui::kMuted,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            RECT badge = {rowRect.right - theme_.S(104), rowRect.top + theme_.S(7),
                          rowRect.right - theme_.S(8), rowRect.bottom - theme_.S(7)};
            ui::DrawBadge(dc, theme_, badge, StateLabel(session), StateColor(session));
        } else if (row == 0) {
            ui::FillSolid(dc, rowRect, ui::kPanel);
            ui::StrokeFrame(dc, rowRect, ui::kBorder, 1);
            ui::DrawLabel(dc, L"NO TARGETS — FOCUS A WINDOW AND PRESS CTRL+SHIFT+H", rowRect,
                          theme_.small, ui::kMuted,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    RECT footer = {theme_.S(16), theme_.S(424), theme_.S(kBaseWidth - 16), theme_.S(446)};
    ui::DrawLabel(dc, L"WDA_EXCLUDEFROMCAPTURE · PER-MONITOR DPI · NO ADMIN REQUIRED", footer,
                  theme_.small, ui::kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::OnTimer() {
    const size_t before = sessions_.Count();
    const HWND selectedHwnd = SelectedValid() ? sessions_.At(static_cast<size_t>(selected_)).hwnd
                                              : nullptr;

    if (!sessions_.Refresh()) {
        return;
    }

    if (selectedHwnd != nullptr) {
        const int index = sessions_.IndexOf(selectedHwnd);
        if (index >= 0) {
            selected_ = index;
        } else {
            selected_ = sessions_.Empty() ? -1 : static_cast<int>(sessions_.Count()) - 1;
        }
    } else if (selected_ >= static_cast<int>(sessions_.Count())) {
        selected_ = static_cast<int>(sessions_.Count()) - 1;
    }

    if (before != sessions_.Count()) {
        UpdateTray();
    }
    InvalidateAll();
}

void App::OnCommand(int id) {
    switch (id) {
        case ID_BTN_TOGGLE:
            ToggleStealth(selected_);
            break;
        case ID_BTN_RESTORE_ALL:
            RestoreAll();
            break;
        case ID_BTN_MIN:
            TargetShow(SW_MINIMIZE);
            break;
        case ID_BTN_RESTORE:
            TargetShow(SW_RESTORE);
            break;
        case ID_BTN_CLOSE:
            TargetClose();
            break;
        case ID_BTN_HIDE_UI:
            ShowUi(::IsWindowVisible(hwnd_) == FALSE);
            break;
        case ID_BTN_EXIT:
        case ID_TRAY_EXIT:
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case ID_TRAY_SHOW:
            ShowUi(true);
            break;
        case ID_TRAY_RESTORE_ALL:
            RestoreAll();
            break;
        default:
            break;
    }
}

void App::OnHotkey(int id) {
    OnCommand(id);
}

void App::OnTray(LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowUi(true);
            break;
        case WM_RBUTTONUP: {
            HMENU menu = ::CreatePopupMenu();
            if (menu == nullptr) {
                break;
            }
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show GhostFrame");
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_RESTORE_ALL, L"Restore all targets");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

            POINT point = {};
            ::GetCursorPos(&point);
            ::SetForegroundWindow(hwnd_);
            ::TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd_, nullptr);
            ::DestroyMenu(menu);
            ::PostMessageW(hwnd_, WM_NULL, 0, 0);
            break;
        }
        default:
            break;
    }
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested) {
    theme_.Create(dpi);
    if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::SelectTarget(int index) {
    if (index < 0 || static_cast<size_t>(index) >= sessions_.Count()) {
        selected_ = -1;
        return;
    }
    selected_ = index;
    InvalidateAll();
}

void App::AttachForeground(bool hideAfter) {
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr || foreground == hwnd_) {
        if (!sessions_.Empty()) {
            SelectTarget(0);
            ToggleStealth(0);
        } else {
            SetStatus(L"NO FOREGROUND WINDOW — FOCUS THE WINDOW YOU WANT TO TRACK", true);
        }
        return;
    }

    std::wstring error;
    if (!sessions_.Attach(foreground, error)) {
        SetStatus(L"ATTACH FAILED: " + error, true);
        return;
    }

    SelectTarget(static_cast<int>(sessions_.Count()) - 1);

    if (hideAfter) {
        if (!sessions_.Send(static_cast<size_t>(selected_), gf::Cmd::Hide, error)) {
            SetStatus(L"ERROR: " + error, true);
            return;
        }
        SetStatus(L"STEALTH ENGAGED — WINDOW EXCLUDED FROM CAPTURE");
    } else {
        SetStatus(L"TARGET ATTACHED");
    }
    UpdateTray();
    InvalidateAll();
}

void App::ToggleStealth(int index) {
    if (index < 0 || static_cast<size_t>(index) >= sessions_.Count()) {
        AttachForeground(true);
        return;
    }

    const bool hide = !sessions_.At(static_cast<size_t>(index)).hidden();
    std::wstring error;
    if (!sessions_.Send(static_cast<size_t>(index), hide ? gf::Cmd::Hide : gf::Cmd::Show, error)) {
        SetStatus(L"ERROR: " + error, true);
        return;
    }

    selected_ = index;
    SetStatus(hide ? L"STEALTH ENGAGED — WINDOW EXCLUDED FROM CAPTURE"
                   : L"CAPTURE EXCLUSION LIFTED");
    InvalidateAll();
}

void App::RestoreAll() {
    sessions_.RestoreAll();
    selected_ = -1;
    SetStatus(L"ALL TARGETS RESTORED");
    UpdateTray();
    InvalidateAll();
}

void App::TargetShow(int command) {
    if (!SelectedValid()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }
    const HWND target = sessions_.At(static_cast<size_t>(selected_)).hwnd;
    if (!::IsWindow(target)) {
        SetStatus(L"THE TARGET WINDOW IS GONE", true);
        return;
    }
    ::ShowWindow(target, command);
    SetStatus(command == SW_MINIMIZE ? L"TARGET MINIMIZED" : L"TARGET RESTORED");
}

void App::TargetClose() {
    if (!SelectedValid()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    const HWND target = sessions_.At(static_cast<size_t>(selected_)).hwnd;
    if (::IsWindow(target)) {
        ::PostMessageW(target, WM_CLOSE, 0, 0);
    }

    sessions_.Remove(static_cast<size_t>(selected_));
    selected_ = sessions_.Empty() ? -1 : 0;
    SetStatus(L"CLOSE SIGNAL SENT");
    UpdateTray();
    InvalidateAll();
}

void App::ShowUi(bool show) {
    if (show) {
        ::ShowWindow(hwnd_, SW_SHOW);
        ::SetForegroundWindow(hwnd_);
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    } else {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

void App::AddTray() {
    if (tray_) {
        return;
    }

    if (icon_ == nullptr) {
        icon_ = ::LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        if (icon_ == nullptr) {
            icon_ = ::LoadIconW(nullptr, IDI_APPLICATION);
        }
    }

    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"GhostFrame — capture exclusion");

    tray_ = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (tray_) {
        UpdateTray();
    }
}

void App::RemoveTray() {
    if (!tray_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    tray_ = false;
}

void App::UpdateTray() {
    if (!tray_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_TIP;
    swprintf_s(data.szTip, L"GhostFrame — %zu tracked", sessions_.Count());
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::SetStatus(std::wstring text, bool error) {
    status_ = std::move(text);
    statusError_ = error;
    gf::LogLine(L"status: %s", status_.c_str());
    InvalidateAll();
}

void App::ShowError(const std::wstring& text) {
    gf::LogLine(L"error: %s", text.c_str());
    ::MessageBoxW(nullptr, text.c_str(), L"GhostFrame", MB_ICONERROR | MB_OK);
}
