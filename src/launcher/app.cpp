#include "app.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "ghostframe/version.h"
#include "log.h"
#include "resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"GhostFrame.MainWindow";
constexpr int kMinWidth = 860;
constexpr int kMinHeight = 620;
constexpr int kDefaultWidth = 1080;
constexpr int kDefaultHeight = 720;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kAttachDoneMessage = WM_APP + 2;
constexpr UINT kAttachProgressMessage = WM_APP + 3;
constexpr UINT kAutoCaptureMessage = WM_APP + 4;
constexpr UINT_PTR kWindowsTimer = 2;
constexpr UINT_PTR kAutoCaptureTimer = 3;
constexpr UINT kWindowsRefreshMs = 2000;
constexpr UINT kAutoCaptureTickMs = 1000;
constexpr DWORD kAutoScanMinIntervalMs = 400;
constexpr int kMaxSendAttempts = 6;
constexpr DWORD kSendRetryDelayMs = 500;
constexpr DWORD kAttachJoinTimeoutMs = 10000;
constexpr DWORD kSettingsSaveDelayMs = 500;
constexpr ULONGLONG kToastDurationMs = 3500;

// SetWinEventHook has no user-data argument, so the single App instance
// registers itself here for the callback.
App* g_app = nullptr;

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

// One finished attach attempt inside a batch job.
struct BatchItem {
    HWND hwnd = nullptr;
    std::wstring title;
    gf::AttachResult result;
};

// Handed from the attach worker to the UI thread through WM_APP+2 once the
// whole batch is done.  The UI thread owns it and applies every session in one
// pass, so the list, tray and status are refreshed once instead of per target.
struct BatchCompletion {
    std::vector<BatchItem> items;
    bool cancelled = false;
};

// Input for the attach worker: only immutable data leaves the UI thread.
struct AttachJob {
    HWND notify = nullptr;
    gf::Injector* injector = nullptr;
    std::vector<HWND> targets;
    volatile LONG* cancel = nullptr;
    bool hideAfter = false;
};

// DWMWA_WINDOW_CORNER_PREFERENCE / DWMWCP_ROUND; loaded dynamically so the exe
// still starts on Windows 10, where dwmapi.dll has neither.
void ApplyRoundedCorners(HWND hwnd) {
    using SetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) {
        return;
    }
    auto setAttribute = reinterpret_cast<SetWindowAttributeFn>(
        ::GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttribute != nullptr) {
        const DWORD corner = 2;  // DWMWCP_ROUND
        setAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    }
    ::FreeLibrary(dwm);
}

}  // namespace

App::InitResult App::Init(HINSTANCE instance, bool noSelfExclude) {
    instance_ = instance;

    LoadSettings();
    // The command-line flag and the saved option both turn the self-exclusion
    // off; the flag wins so a scripted run always behaves the same way.
    noSelfExclude_ = noSelfExclude || settings_.ui.noSelfExclude;

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

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&controls);

    std::wstring error;
    if (!injector_.Initialize(error)) {
        gf::LogLine(L"payload preparation failed: %s", error.c_str());
        ShowError(L"GhostFrame could not prepare its payloads:\n\n" + error);
        return InitResult::Failed;
    }

    taskbarCreated_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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
    const int width = MulDiv(kDefaultWidth, dpi, 96);
    const int height = MulDiv(kDefaultHeight, dpi, 96);

    POINT cursor = {};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    const RECT& work = monitorInfo.rcWork;
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    int x = work.left + ((workWidth)-width) / 2;
    int y = work.top + ((workHeight)-height) / 2;

    // Restore the saved geometry when it still fits on a monitor; otherwise fall
    // back to a centred window so a disconnected display cannot hide the app.
    int windowWidth = width;
    int windowHeight = height;
    if (settings_.ui.width >= MulDiv(700, dpi, 96) &&
        settings_.ui.height >= MulDiv(480, dpi, 96)) {
        RECT saved = {settings_.ui.x, settings_.ui.y, settings_.ui.x + settings_.ui.width,
                      settings_.ui.y + settings_.ui.height};
        const HMONITOR target = ::MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
        if (target != nullptr) {
            x = settings_.ui.x;
            y = settings_.ui.y;
            windowWidth = settings_.ui.width;
            windowHeight = settings_.ui.height;
        }
    }
    // The grid needs room for the sidebar and the table; never open smaller
    // than the supported minimum, and never larger than the monitor.
    windowWidth = (std::max)(windowWidth, MulDiv(kMinWidth, dpi, 96));
    windowHeight = (std::max)(windowHeight, MulDiv(kMinHeight, dpi, 96));
    windowWidth = (std::min)(windowWidth, workWidth);
    windowHeight = (std::min)(windowHeight, workHeight);
    if (windowWidth >= workWidth) {
        x = work.left;
    }
    if (windowHeight >= workHeight) {
        y = work.top;
    }

    g_app = this;
    hwnd_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"GhostFrame",
                              WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, windowWidth,
                              windowHeight, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        g_app = nullptr;
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
    ::InterlockedExchange(&attachCancel_, 1);
    JoinAttachWorker();
    const bool workerGone = attachThread_ == nullptr;

    sessions_.RestoreAll();
    CaptureGeometry();
    SaveSettings();

    if (workerGone) {
        injector_.Shutdown();
    } else {
        // The worker is still inside the injector; freeing the payloads under
        // it would be a use-after-free.  Process exit closes the lease and the
        // agents restore the windows themselves.
        gf::LogLine(L"attach worker did not stop; payload cleanup skipped");
    }
    gf::LogLine(L"shutdown");
    gf::LogShutdown();
}

void App::OnClose() {
    // Let a running attach finish (bounded by its own timeouts) and collect any
    // completed-but-undispatched result, so RestoreAll cannot miss a session.
    ::InterlockedExchange(&attachCancel_, 1);
    JoinAttachWorker();
    DrainAttachResults();
    RestoreAll();
    CaptureGeometry();
    SaveSettings();
    ::DestroyWindow(hwnd_);
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

        case WM_NCCALCSIZE:
            // Drop the non-client frame of WS_THICKFRAME: no title bar is
            // drawn, but the window can still be resized from its edges.
            if (wParam == TRUE) {
                return 0;
            }
            break;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = theme_.S(kMinWidth);
            info->ptMinTrackSize.y = theme_.S(kMinHeight);
            // A borderless popup would otherwise maximize over the taskbar.
            MONITORINFO monitor = {};
            monitor.cbSize = sizeof(monitor);
            if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            OnSize();
            return 0;

        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_TIMER:
            if (wParam == ID_TIMER_REFRESH) {
                OnTimer();
            } else if (wParam == kWindowsTimer && tab_ == Tab::Windows) {
                RefreshWindows();
            } else if (wParam == kAutoCaptureTimer) {
                ScanAutoCapture();
            }
            return 0;

        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wParam));
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_EDIT_SEARCH && HIWORD(wParam) == EN_CHANGE) {
                OnFilterChanged();
                return 0;
            }
            OnCommand(LOWORD(wParam));
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetTextColor(dc, ui::kText);
            ::SetBkColor(dc, ui::kPanelHi);
            return reinterpret_cast<LRESULT>(editBrush_ != nullptr ? editBrush_
                                                                   : ::GetStockObject(BLACK_BRUSH));
        }

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            bool active = false;
            if (SelectedValid() && item->CtlID == ID_BTN_TOGGLE) {
                active = sessions_.At(static_cast<size_t>(selected_)).hidden();
            }
            const bool hovered = hoveredButton_ != 0 && static_cast<int>(item->CtlID) == hoveredButton_;
            COLORREF surface = ui::kBg;
            for (int id : {ID_BTN_SET_RESET, ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE,
                           ID_BTN_SET_APPLY}) {
                if (item->CtlID == static_cast<UINT>(id)) {
                    surface = ui::kPanel;
                }
            }
            ui::DrawButton(item, theme_, active, item->CtlID == ID_BTN_CLOSE, hovered, false,
                           surface);
            return TRUE;
        }

        case WM_EXITSIZEMOVE:
            CaptureGeometry();
            MarkSettingsDirty();
            return 0;

        case WM_MOUSEMOVE: {
            if (!trackingMouse_) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd_;
                ::TrackMouseEvent(&track);
                trackingMouse_ = true;
            }
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(hwnd_, &point);
            const bool interactive =
                ZoneAt(point.x, point.y) != HotZone::None ||
                TabAt(point.x, point.y) >= 0 || SettingsNavAt(point.x, point.y) >= 0 ||
                BehaviorSwitchAt(point.x, point.y) >= 0;
            if (interactive) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }

        case WM_MOUSELEAVE:
            trackingMouse_ = false;
            OnMouseLeave();
            return 0;

        case WM_MOUSEWHEEL:
            OnMouseWheel(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_VSCROLL:
            OnVScroll(LOWORD(wParam), HIWORD(wParam));
            return 0;

        case WM_KEYDOWN:
            OnKeyDown(static_cast<int>(wParam));
            return 0;

        case WM_SYSKEYDOWN:
            // Only intercept Alt combinations while rebinding a hotkey; the rest
            // must keep reaching DefWindowProc (Alt+F4, the system menu, …).
            if (rebinding_) {
                OnKeyDown(static_cast<int>(wParam));
                return 0;
            }
            break;

        case WM_LBUTTONDOWN: {
            ::SetFocus(hwnd_);
            OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                          (wParam & MK_CONTROL) != 0, (wParam & MK_SHIFT) != 0);
            return 0;
        }

        case WM_RBUTTONDOWN:
            OnRButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONDBLCLK:
            OnLButtonDblClk(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_NCHITTEST: {
            const LRESULT hit = ::DefWindowProcW(hwnd_, message, wParam, lParam);
            if (hit != HTCLIENT) {
                return hit;
            }
            POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(hwnd_, &point);
            RECT client = {};
            ::GetClientRect(hwnd_, &client);
            const int border = theme_.S(6);
            const bool left = point.x < border;
            const bool right = point.x >= client.right - border;
            const bool top = point.y < border;
            const bool bottom = point.y >= client.bottom - border;
            if (top && left) {
                return HTTOPLEFT;
            }
            if (top && right) {
                return HTTOPRIGHT;
            }
            if (bottom && left) {
                return HTBOTTOMLEFT;
            }
            if (bottom && right) {
                return HTBOTTOMRIGHT;
            }
            if (top) {
                return HTTOP;
            }
            if (bottom) {
                return HTBOTTOM;
            }
            if (left) {
                return HTLEFT;
            }
            if (right) {
                return HTRIGHT;
            }
            if (point.y < theme_.S(theme_.m.titleHeight)) {
                // The tab strip and the window buttons are live controls; the
                // logo and the empty space around them drag the window.
                bool overControl = false;
                for (const RECT& tab : layout_.tabs) {
                    if (::PtInRect(&tab, point)) {
                        overControl = true;
                    }
                }
                for (const RECT& button : layout_.windowButtons) {
                    if (::PtInRect(&button, point)) {
                        overControl = true;
                    }
                }
                if (!overControl) {
                    return HTCAPTION;
                }
            }
            return hit;
        }

        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
            return 0;

        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            OnSettingChange();
            return 0;

        case WM_CLOSE:
            OnClose();
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                if (settings_.ui.minimizeToTray) {
                    ShowUi(false);
                } else {
                    ::ShowWindow(hwnd_, SW_MINIMIZE);
                }
                return 0;
            }
            break;

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
            if (message == kAttachDoneMessage) {
                OnAttachDone(lParam);
                return 0;
            }
            if (message == kAttachProgressMessage) {
                OnAttachProgress(wParam, lParam);
                return 0;
            }
            if (message == kAutoCaptureMessage) {
                OnAutoCaptureMessage(reinterpret_cast<HWND>(lParam));
                return 0;
            }
            break;
    }

    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void App::OnCreate() {
    ui::InitIcons();
    theme_.Create(::GetDpiForWindow(hwnd_));
    editBrush_ = ::CreateSolidBrush(ui::kPanelHi);
    CreateChildren();
    Layout();

    BuildTargetRows();
    BuildSettingsRows();

    ::SetTimer(hwnd_, ID_TIMER_REFRESH, 250, nullptr);
    RegisterHotkeys();
    AddTray();
    ApplyRoundedCorners(hwnd_);

    if (!noSelfExclude_) {
        ::SetWindowDisplayAffinity(hwnd_, 0x11);
    }

    // Start on the tab the user left off on.
    const std::wstring last = settings_.ui.lastTab;
    if (last == L"windows") {
        SelectTab(Tab::Windows);
    } else if (last == L"settings") {
        SelectTab(Tab::Settings);
    } else {
        tab_ = Tab::Targets;
        ApplyTabControls();
    }

    // Auto-capture: react to new top-level windows, plus a slow safety timer.
    winEventHook_ = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                      &App::WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    ::SetTimer(hwnd_, kAutoCaptureTimer, kAutoCaptureTickMs, nullptr);
    ScanAutoCapture();

    if (settings_.badFile) {
        SetStatus(L"settings.json was corrupt — renamed to .bad, defaults restored", true);
    } else {
        SetStatus(L"Ready — pick a window on the Windows tab, or focus one and press Ctrl+Shift+H");
    }
}

void App::OnDestroy() {
    ::KillTimer(hwnd_, ID_TIMER_REFRESH);
    ::KillTimer(hwnd_, kWindowsTimer);
    ::KillTimer(hwnd_, kAutoCaptureTimer);
    if (winEventHook_ != nullptr) {
        ::UnhookWinEvent(winEventHook_);
        winEventHook_ = nullptr;
    }
    if (tooltip_ != nullptr) {
        ::DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    UnregisterHotkeys();
    RemoveTray();
    theme_.Destroy();
    ui::ShutdownIcons();
    if (g_app == this) {
        g_app = nullptr;
    }

    if (editBrush_ != nullptr) {
        ::DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }

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
        const wchar_t* tip;
    };
    const ButtonSpec buttons[] = {
        {ID_BTN_TOGGLE, L"Toggle stealth", L"Toggle capture exclusion for the selection"},
        {ID_BTN_RESTORE_ALL, L"Restore all", L"Restore every tracked window"},
        {ID_BTN_MIN, L"Minimize", L"Minimize the selected targets"},
        {ID_BTN_RESTORE, L"Restore", L"Restore the selected targets"},
        {ID_BTN_CLOSE, L"Close", L"Ask the selected targets to close"},
        {ID_BTN_SET_RESET, L"Reset", L"Restore the default hotkeys"},
        {ID_BTN_SET_ADD_FG, L"Add app", L"Auto-capture the current foreground app"},
        {ID_BTN_SET_BROWSE, L"Browse exe…", L"Pick an executable to auto-capture"},
        {ID_BTN_SET_APPLY, L"Apply now", L"Run the rules against the windows open right now"},
    };

    for (const ButtonSpec& spec : buttons) {
        HWND button = ::CreateWindowExW(
            0, L"BUTTON", spec.text, WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)), instance_, nullptr);
        if (button != nullptr) {
            ::SetWindowSubclass(button, &App::ButtonSubclassProc, static_cast<UINT_PTR>(spec.id),
                                reinterpret_cast<DWORD_PTR>(this));
        }
    }

    ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_SEARCH)), instance_,
                      nullptr);
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
        ::SendMessageW(search, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(L"Search windows, apps or PIDs…"));
        ::SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                       MAKELPARAM(theme_.S(34), theme_.S(10)));
    }

    SetupTooltips();
}

void App::SetupTooltips() {
    if (tooltip_ == nullptr) {
        tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                     WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                                     instance_, nullptr);
        if (tooltip_ == nullptr) {
            return;
        }
        ::SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
    }

    struct ToolSpec {
        int id;
        const wchar_t* text;
    };
    const ToolSpec tools[] = {
        {ID_BTN_TOGGLE, L"Toggle stealth  ·  Ctrl+Shift+H"},
        {ID_BTN_RESTORE_ALL, L"Restore all  ·  Ctrl+Shift+R"},
        {ID_BTN_MIN, L"Minimize target  ·  Ctrl+Shift+Down"},
        {ID_BTN_RESTORE, L"Restore target  ·  Ctrl+Shift+Up"},
        {ID_BTN_CLOSE, L"Close target  ·  Ctrl+Shift+Q"},
    };

    for (const ToolSpec& spec : tools) {
        HWND child = ::GetDlgItem(hwnd_, spec.id);
        if (child == nullptr) {
            continue;
        }
        TOOLINFOW tool = {};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = hwnd_;
        tool.uId = reinterpret_cast<UINT_PTR>(child);
        tool.lpszText = const_cast<LPWSTR>(spec.text);
        ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

void App::RelayTooltip(HWND button, UINT message, WPARAM wParam, LPARAM lParam) {
    if (tooltip_ == nullptr) {
        return;
    }
    MSG relay = {};
    relay.hwnd = button;
    relay.message = message;
    relay.wParam = wParam;
    relay.lParam = lParam;
    ::GetCursorPos(&relay.pt);
    ::SendMessageW(tooltip_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&relay));
}

App::LayoutInfo App::ComputeLayout() const {
    LayoutInfo info;
    RECT client = {};
    ::GetClientRect(hwnd_, &client);

    const int width = client.right;
    const int height = client.bottom;
    const int pad = theme_.S(theme_.m.pad);
    const int topH = theme_.S(theme_.m.titleHeight);
    const int sideW = theme_.S(theme_.m.sidebarWidth);

    info.topBar = {0, 0, width, topH};
    info.sidebar = {0, topH, sideW, height};
    info.content = {sideW, topH, width, height};

    // Brand mark and the tab strip live in the title bar.
    const int mark = theme_.S(32);
    info.logoMark = {theme_.S(22), (topH - mark) / 2, theme_.S(22) + mark,
                     (topH - mark) / 2 + mark};

    const int tabW = theme_.S(theme_.m.tabWidth);
    const int tabH = theme_.S(theme_.m.tabHeight);
    const int tabGap = theme_.S(6);
    const int tabLeft = theme_.S(238);
    for (int i = 0; i < 3; ++i) {
        const int left = tabLeft + i * (tabW + tabGap);
        info.tabs[i] = {left, (topH - tabH) / 2, left + tabW, (topH - tabH) / 2 + tabH};
    }

    const int winW = theme_.S(34);
    const int winGap = theme_.S(6);
    int winLeft = width - theme_.S(14) - winW * 3 - winGap * 2;
    for (int i = 0; i < 3; ++i) {
        const int left = winLeft + i * (winW + winGap);
        info.windowButtons[i] = {left, (topH - theme_.S(30)) / 2, left + winW,
                                 (topH - theme_.S(30)) / 2 + theme_.S(30)};
    }

    const int cx = sideW + pad;
    const int cw = width - cx - pad;
    const int headerTop = topH + theme_.S(22);
    info.pageHeader = {cx, headerTop, cx + cw, headerTop + theme_.S(64)};

    const int iconSize = theme_.S(44);
    info.pageIcon = {cx, headerTop + theme_.S(3), cx + iconSize, headerTop + theme_.S(3) + iconSize};
    info.pageTitle = {info.pageIcon.right + theme_.S(14), headerTop - theme_.S(3),
                      cx + cw - theme_.S(220), headerTop + theme_.S(30)};
    info.pageSubtitle = {info.pageTitle.left, headerTop + theme_.S(29),
                         cx + cw - theme_.S(220), headerTop + theme_.S(54)};

    const int ctaH = theme_.S(40);
    const int ctaW = theme_.S(196);
    if (tab_ != Tab::Settings) {
        info.cta = {cx + cw - ctaW, headerTop + theme_.S(9), cx + cw,
                    headerTop + theme_.S(9) + ctaH};
    }
    const int secW = theme_.S(124);
    if (tab_ == Tab::Windows) {
        info.secondaryCta = {info.cta.left - theme_.S(8) - secW, info.cta.top,
                             info.cta.left - theme_.S(8), info.cta.bottom};
    }

    const int footH = theme_.S(18);
    info.footer = {cx, height - pad - footH, cx + cw, height - pad};
    info.footerLine = {cx, info.footer.top - theme_.S(12), cx + cw,
                       info.footer.top - theme_.S(11)};
    info.logLink = {cx + cw - theme_.S(104), info.footer.top, cx + cw, info.footer.bottom};

    int top = info.pageHeader.bottom + theme_.S(18);
    const int bottom = info.footer.top - theme_.S(14);

    if (tab_ == Tab::Windows) {
        info.searchBox = {cx, top, cx + theme_.S(390), top + theme_.S(40)};
        top = info.searchBox.bottom + theme_.S(12);
        info.list = {cx, top, cx + cw, bottom};
    } else if (tab_ == Tab::Targets) {
        const int toolbarH = theme_.S(36);
        info.toolbar = {cx, bottom - toolbarH, cx + cw, bottom};
        info.list = {cx, top, cx + cw, info.toolbar.top - theme_.S(12)};

        // Empty state: the list surface is reused for one large explanatory
        // card when there is nothing to show.
        info.emptyCard = info.list;
        const int cardPad = theme_.S(44);
        info.emptyArt = {info.emptyCard.left + cardPad, info.emptyCard.top + theme_.S(28),
                         info.emptyCard.left +
                             (info.emptyCard.right - info.emptyCard.left) * 45 / 100,
                         info.emptyCard.bottom - theme_.S(28)};
        const int blockH = theme_.S(200);
        const int blockTop =
            info.emptyCard.top +
            ((info.emptyCard.bottom - info.emptyCard.top) - blockH) / 2;
        info.emptyText = {info.emptyArt.right + theme_.S(20), blockTop,
                          info.emptyCard.right - theme_.S(36), blockTop + theme_.S(108)};
        info.emptyCta = {info.emptyText.left, blockTop + theme_.S(114),
                         info.emptyText.left + theme_.S(196), blockTop + theme_.S(152)};
    } else {
        const int navW = theme_.S(theme_.m.settingsNavWidth);
        info.settingsNav = {cx, top, cx + navW, bottom};
        for (int i = 0; i < 4; ++i) {
            const int itemTop = top + theme_.S(10) + i * theme_.S(46);
            info.settingsNavItems[i] = {info.settingsNav.left + theme_.S(8), itemTop,
                                        info.settingsNav.right - theme_.S(8),
                                        itemTop + theme_.S(40)};
        }

        info.settingsArea = {info.settingsNav.right + theme_.S(16), top, cx + cw, bottom};
        const int gap = theme_.S(12);

        if (settingsSection_ == SettingsSection::General) {
            const int autoH = theme_.S(76);
            const int behaviorH = theme_.S(178);
            const int desiredShortcuts = theme_.S(46) + 3 * theme_.S(34) + theme_.S(12);
            const int available = bottom - top;
            int shortcutsH = desiredShortcuts;
            if (shortcutsH + gap + behaviorH + gap + autoH > available) {
                shortcutsH = available - behaviorH - autoH - 2 * gap;
                if (shortcutsH < theme_.S(96)) {
                    shortcutsH = theme_.S(96);
                }
            }
            info.settingsCard[0] = {info.settingsArea.left, top, info.settingsArea.right,
                                    top + shortcutsH};
            info.settingsCard[1] = {info.settingsArea.left,
                                    info.settingsCard[0].bottom + gap,
                                    info.settingsArea.right,
                                    info.settingsCard[0].bottom + gap + behaviorH};
            info.settingsCard[2] = {info.settingsArea.left,
                                    info.settingsCard[1].bottom + gap,
                                    info.settingsArea.right,
                                    info.settingsCard[1].bottom + gap + autoH};
            info.settingsHotkeyList = {info.settingsCard[0].left + theme_.S(14),
                                       info.settingsCard[0].top + theme_.S(46),
                                       info.settingsCard[0].right - theme_.S(14),
                                       info.settingsCard[0].bottom - theme_.S(12)};
            info.settingsManage = {info.settingsCard[2].right - theme_.S(14) - theme_.S(108),
                                   info.settingsCard[2].top + (autoH - theme_.S(30)) / 2,
                                   info.settingsCard[2].right - theme_.S(14),
                                   info.settingsCard[2].top + (autoH + theme_.S(30)) / 2};
        } else if (settingsSection_ == SettingsSection::Hotkeys) {
            const int cardH = theme_.S(46) + 6 * theme_.S(34) + theme_.S(46);
            info.settingsCard[0] = {info.settingsArea.left, top, info.settingsArea.right,
                                    (std::min)(bottom, top + cardH)};
            info.settingsHotkeyList = {info.settingsCard[0].left + theme_.S(14),
                                       info.settingsCard[0].top + theme_.S(46),
                                       info.settingsCard[0].right - theme_.S(14),
                                       info.settingsCard[0].bottom - theme_.S(46)};
        } else if (settingsSection_ == SettingsSection::AutoCapture) {
            info.settingsCard[0] = {info.settingsArea.left, top, info.settingsArea.right, bottom};
            info.settingsActionRow = {info.settingsCard[0].left + theme_.S(14),
                                      info.settingsCard[0].bottom - theme_.S(50),
                                      info.settingsCard[0].right - theme_.S(14),
                                      info.settingsCard[0].bottom - theme_.S(14)};
            info.settingsRuleList = {info.settingsCard[0].left + theme_.S(14),
                                     info.settingsCard[0].top + theme_.S(46),
                                     info.settingsCard[0].right - theme_.S(14),
                                     info.settingsActionRow.top - theme_.S(10)};
        } else {
            info.settingsCard[0] = {info.settingsArea.left, top, info.settingsArea.right,
                                    top + theme_.S(190)};
            info.settingsCard[1] = {info.settingsArea.left, info.settingsCard[0].bottom + gap,
                                    info.settingsArea.right,
                                    (std::min)(bottom, static_cast<int>(info.settingsCard[0].bottom) + gap +
                                                           theme_.S(152))};
            info.settingsLogCta = {info.settingsCard[1].left + theme_.S(16),
                                   info.settingsCard[1].top + theme_.S(82),
                                   info.settingsCard[1].left + theme_.S(200),
                                   info.settingsCard[1].top + theme_.S(118)};
            info.settingsRestoreCta = {info.settingsLogCta.right + theme_.S(10),
                                       info.settingsLogCta.top,
                                       info.settingsLogCta.right + theme_.S(10) + theme_.S(160),
                                       info.settingsLogCta.bottom};
        }
    }

    return info;
}

void App::Layout() {
    layout_ = ComputeLayout();

    auto placeRect = [this](int id, const RECT& rect) {
        HWND child = ::GetDlgItem(hwnd_, id);
        if (child != nullptr) {
            ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
                           rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    // TARGETS action bar.
    if (layout_.toolbar.right > layout_.toolbar.left) {
        const int gap = theme_.S(8);
        const int count = 5;
        const int total = layout_.toolbar.right - layout_.toolbar.left;
        const int buttonWidth = (total - gap * (count - 1)) / count;
        const int ids[count] = {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_MIN, ID_BTN_RESTORE,
                                ID_BTN_CLOSE};
        for (int i = 0; i < count; ++i) {
            RECT rect = {layout_.toolbar.left + i * (buttonWidth + gap), layout_.toolbar.top,
                         layout_.toolbar.left + i * (buttonWidth + gap) + buttonWidth,
                         layout_.toolbar.bottom};
            placeRect(ids[i], rect);
        }
    }

    // Search field inside its painted rounded shell.
    if (layout_.searchBox.right > layout_.searchBox.left) {
        RECT edit = {layout_.searchBox.left + theme_.S(34), layout_.searchBox.top + theme_.S(8),
                     layout_.searchBox.right - theme_.S(8),
                     layout_.searchBox.bottom - theme_.S(8)};
        placeRect(ID_EDIT_SEARCH, edit);
        if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
            ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
            ::SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                           MAKELPARAM(theme_.S(4), theme_.S(10)));
        }
    }

    // SETTINGS actions.
    if (tab_ == Tab::Settings && layout_.settingsCard[0].right > layout_.settingsCard[0].left) {
        if (settingsSection_ == SettingsSection::General ||
            settingsSection_ == SettingsSection::Hotkeys) {
            RECT reset = {layout_.settingsCard[0].right - theme_.S(14) - theme_.S(84),
                          layout_.settingsCard[0].top + theme_.S(11),
                          layout_.settingsCard[0].right - theme_.S(14),
                          layout_.settingsCard[0].top + theme_.S(11) + theme_.S(30)};
            placeRect(ID_BTN_SET_RESET, reset);
        } else if (settingsSection_ == SettingsSection::AutoCapture) {
            const RECT row = layout_.settingsActionRow;
            const int gap = theme_.S(10);
            const int buttonWidth = (row.right - row.left - 2 * gap) / 3;
            const int ids[3] = {ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE, ID_BTN_SET_APPLY};
            for (int i = 0; i < 3; ++i) {
                RECT rect = {row.left + i * (buttonWidth + gap), row.top,
                             row.left + i * (buttonWidth + gap) + buttonWidth, row.bottom};
                placeRect(ids[i], rect);
            }
        }
    }

    targetsList_.SetRowHeight(58);
    targetsList_.SetColumns({});
    targetsList_.SetCheckboxes(false);
    targetsList_.SetBounds(layout_.list, theme_);

    windowsList_.SetRowHeight(43);
    windowsList_.SetColumns({
        {L"WINDOW", -1, DT_LEFT, false},
        {L"APPLICATION", 128, DT_LEFT, false},
        {L"PID", 62, DT_RIGHT, true},
        {L"ARCH", 52, DT_LEFT, true},
        {L"TYPE", 104, DT_LEFT, false},
    });
    windowsList_.SetCheckboxes(true);
    windowsList_.SetBounds(layout_.list, theme_);

    hotkeysList_.SetRowHeight(34);
    hotkeysList_.SetColumns({});
    hotkeysList_.SetCheckboxes(false);
    hotkeysList_.SetBounds(layout_.settingsHotkeyList, theme_);

    rulesList_.SetRowHeight(43);
    rulesList_.SetColumns({});
    rulesList_.SetCheckboxes(false);
    rulesList_.SetBounds(layout_.settingsRuleList, theme_);
}

RECT App::TabRect(int index) const {
    if (index < 0 || index >= 3) {
        return RECT{};
    }
    return layout_.tabs[index];
}

int App::HotkeyCommandId(gf::HotkeyAction action) const {
    switch (action) {
        case gf::HotkeyAction::ToggleSelection:
            return ID_BTN_TOGGLE;
        case gf::HotkeyAction::RestoreAll:
            return ID_BTN_RESTORE_ALL;
        case gf::HotkeyAction::ToggleUi:
            return ID_BTN_HIDE_UI;
        case gf::HotkeyAction::Minimize:
            return ID_BTN_MIN;
        case gf::HotkeyAction::Restore:
            return ID_BTN_RESTORE;
        case gf::HotkeyAction::Close:
            return ID_BTN_CLOSE;
        default:
            return 0;
    }
}

void App::RegisterHotkeys() {
    std::wstring failed;
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        const gf::HotkeyBinding& binding = settings_.hotkeys[i];
        if (binding.empty()) {
            continue;
        }
        const int id = HotkeyCommandId(static_cast<gf::HotkeyAction>(i));
        if (id == 0) {
            continue;
        }
        if (!::RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
            if (!failed.empty()) {
                failed += L", ";
            }
            failed += gf::FormatHotkey(binding.modifiers, binding.virtualKey);
        }
    }

    if (!failed.empty()) {
        SetStatus(L"Hotkey conflict — already in use: " + failed, true);
    }
}

void App::UnregisterHotkeys() {
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        const int id = HotkeyCommandId(static_cast<gf::HotkeyAction>(i));
        if (id != 0) {
            ::UnregisterHotKey(hwnd_, id);
        }
    }
}

// Validates and applies a candidate hotkey set.  On failure the previous
// bindings are re-registered, so one taken combination cannot leave the app
// without any hotkeys.
bool App::ApplyHotkeys(const gf::Settings& candidate, std::wstring& error) {
    if (!gf::ValidateHotkeys(candidate, error)) {
        return false;
    }

    UnregisterHotkeys();

    std::wstring failed;
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        const gf::HotkeyBinding& binding = candidate.hotkeys[i];
        const int id = HotkeyCommandId(static_cast<gf::HotkeyAction>(i));
        if (id == 0 || binding.empty()) {
            continue;
        }
        if (!::RegisterHotKey(hwnd_, id, binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
            if (!failed.empty()) {
                failed += L", ";
            }
            failed += gf::FormatHotkey(binding.modifiers, binding.virtualKey);
        }
    }

    if (!failed.empty()) {
        UnregisterHotkeys();
        RegisterHotkeys();  // restore the previous set
        error = L"HOTKEY " + failed + L" IS TAKEN BY ANOTHER APP";
        return false;
    }

    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        settings_.hotkeys[i] = candidate.hotkeys[i];
    }
    MarkSettingsDirty();
    return true;
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

void App::LoadSettings() {
    std::wstring error;
    if (!gf::LoadSettings(gf::SettingsPath(), settings_, error)) {
        gf::LogLine(L"settings: %s", error.c_str());
    }
}

void App::SaveSettings() {
    settingsDirty_ = false;
    std::wstring error;
    if (!gf::SaveSettings(gf::SettingsPath(), settings_, error)) {
        gf::LogLine(L"cannot save the settings: %s", error.c_str());
    }
}

void App::CaptureGeometry() {
    RECT rect = {};
    if (::GetWindowRect(hwnd_, &rect)) {
        settings_.ui.x = rect.left;
        settings_.ui.y = rect.top;
        settings_.ui.width = rect.right - rect.left;
        settings_.ui.height = rect.bottom - rect.top;
    }
}

void App::MarkSettingsDirty() {
    settingsDirty_ = true;
    settingsSaveDue_ = ::GetTickCount64() + kSettingsSaveDelayMs;
}

// ---------------------------------------------------------------------------
// Auto-capture
// ---------------------------------------------------------------------------

void CALLBACK App::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                DWORD, DWORD) {
    if (g_app == nullptr || event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW ||
        idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }
    // The hook runs on this thread; bounce the work through the queue so a scan
    // never re-enters the enumerator or the attach path.
    ::PostMessageW(g_app->hwnd_, kAutoCaptureMessage, 0, reinterpret_cast<LPARAM>(hwnd));
}

void App::OnAutoCaptureMessage(HWND) {
    ScanAutoCapture();
}

void App::ScanAutoCapture() {
    if (attachThread_ != nullptr) {
        return;  // one batch at a time; the timer will retry
    }

    bool anyEnabled = false;
    for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
        if (rule.enabled && !rule.value.empty()) {
            anyEnabled = true;
            break;
        }
    }
    if (!anyEnabled) {
        return;
    }

    const ULONGLONG now = ::GetTickCount64();
    if (now - autoLastScan_ < kAutoScanMinIntervalMs) {
        return;
    }
    autoLastScan_ = now;

    windowEnumerator_.Refresh(hwnd_);

    std::vector<AutoRequest> pending;
    bool hide = false;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        if (window.hwnd == nullptr || sessions_.IndexOf(window.hwnd) >= 0) {
            continue;
        }
        if (window.arch == gf::Arch::Arm64) {
            continue;  // ARM64 targets are not supported
        }
        if (window.elevated) {
            continue;  // cannot inject into a higher integrity level
        }
        if (!autoGuard_.Allow(window.pid, window.createTime, window.hwnd)) {
            continue;
        }

        bool matched = false;
        bool matchedHide = false;
        for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
            if (gf::RuleMatches(rule, window.processName, window.title)) {
                matched = true;
                matchedHide = matchedHide || rule.hide;
            }
        }
        if (!matched) {
            continue;
        }

        std::wstring reason;
        if (!gf::ValidateTarget(window.hwnd, reason)) {
            gf::LogLine(L"auto-capture skipped %s: %s", window.processName.c_str(),
                        reason.c_str());
            continue;
        }

        pending.push_back(AutoRequest{window.hwnd, window.pid, window.createTime});
        hide = hide || matchedHide;
    }

    if (pending.empty()) {
        return;
    }

    std::vector<HWND> targets;
    targets.reserve(pending.size());
    for (const AutoRequest& request : pending) {
        targets.push_back(request.hwnd);
        autoRequests_.push_back(request);
    }
    gf::LogLine(L"auto-capture: attaching %zu window(s)", targets.size());
    StartAttachBatch(std::move(targets), hide);
    if (attachThread_ == nullptr) {
        // The batch was refused (duplicates or a failed worker): forget the
        // requests so they cannot accumulate.
        autoRequests_.clear();
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

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

    PaintTopBar(dc, client);
    PaintSidebar(dc, client);
    PaintPageChrome(dc);

    if (tab_ == Tab::Targets) {
        PaintTargetsPage(dc);
    } else if (tab_ == Tab::Windows) {
        PaintWindowsPage(dc);
    } else {
        PaintSettingsPage(dc);
    }

    PaintFooter(dc);

    // Transient toast.
    if (!toast_.empty() && ::GetTickCount64() < toastUntil_) {
        const SIZE size = ui::MeasureTextSize(dc, theme_.body, toast_);
        const int width = size.cx + theme_.S(40);
        const int height = theme_.S(36);
        const int left = layout_.content.left + ((layout_.content.right - layout_.content.left) - width) / 2;
        RECT toast = {left, layout_.topBar.bottom + theme_.S(12), left + width,
                      layout_.topBar.bottom + theme_.S(12) + height};
        ui::FillRoundRect(dc, toast, ui::kToastBg, theme_.S(10));
        ui::StrokeRoundFrame(dc, toast, toastError_ ? ui::kWarn : ui::kNeonDim, theme_.S(10), 1);
        ui::DrawLabel(dc, toast_.c_str(), toast, theme_.body,
                      toastError_ ? ui::kWarn : ui::kText,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void App::PaintTopBar(HDC dc, const RECT& client) {
    ui::FillSolid(dc, layout_.topBar, ui::kTopBar);
    const RECT border = {0, layout_.topBar.bottom - 1, client.right, layout_.topBar.bottom};
    ui::FillSolid(dc, border, ui::kBorder);

    const int markSize = theme_.S(30);
    ui::DrawGhostMark(dc, (layout_.logoMark.left + layout_.logoMark.right) / 2,
                      (layout_.logoMark.top + layout_.logoMark.bottom) / 2, markSize, ui::kNeon,
                      ui::kTopBar, false);

    RECT wordmark = {layout_.logoMark.right + theme_.S(10), layout_.topBar.top,
                     layout_.tabs[0].left - theme_.S(14), layout_.topBar.bottom};
    ui::DrawLabel(dc, L"GhostFrame", wordmark, theme_.brand, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    static const wchar_t* const kTabNames[] = {L"Targets", L"Windows", L"Settings"};
    static const ui::Icon kTabIcons[] = {ui::Icon::Target, ui::Icon::Grid, ui::Icon::Gear};
    const int activeTab = tab_ == Tab::Targets ? 0 : (tab_ == Tab::Windows ? 1 : 2);
    for (int index = 0; index < 3; ++index) {
        const bool active = activeTab == index;
        const bool hovered = hoverTab_ == index;
        const RECT tab = layout_.tabs[index];
        const COLORREF background =
            active ? ui::Blend(ui::kPanelHi, ui::kNeonBg, 45)
                   : (hovered ? ui::kPanelHi : ui::kTopBar);
        ui::FillRoundRect(dc, tab, background, theme_.S(9));
        if (active) {
            ui::StrokeRoundFrame(dc, tab, ui::kNeonDim, theme_.S(9), 1);
        }

        const SIZE textSize = ui::MeasureTextSize(dc, theme_.small, kTabNames[index]);
        const int iconSize = theme_.S(15);
        const int total = iconSize + theme_.S(8) + textSize.cx;
        const int startX = tab.left + ((tab.right - tab.left) - total) / 2;
        const int iconY = tab.top + ((tab.bottom - tab.top) - iconSize) / 2;
        RECT iconRect = {startX, iconY, startX + iconSize, iconY + iconSize};
        ui::DrawIcon(dc, kTabIcons[index], iconRect,
                     active ? ui::kNeon : (hovered ? ui::kText : ui::kMuted), theme_.S(2));
        RECT label = {iconRect.right + theme_.S(8), tab.top, tab.right, tab.bottom};
        ui::DrawLabel(dc, kTabNames[index], label, theme_.small,
                      active ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Version and window controls.
    RECT version = {layout_.windowButtons[0].left - theme_.S(108), layout_.topBar.top,
                    layout_.windowButtons[0].left - theme_.S(18), layout_.topBar.bottom};
    ui::DrawLabel(dc, L"v" GF_VERSION_STRING, version, theme_.tiny, ui::kTextDim,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const bool zoomed = ::IsZoomed(hwnd_) != FALSE;
    for (int i = 0; i < 3; ++i) {
        const RECT button = layout_.windowButtons[i];
        const bool hovered = hotZone_ == (i == 0 ? HotZone::Hide
                                                 : (i == 1 ? HotZone::Maximize : HotZone::Close));
        if (hovered) {
            ui::FillRoundRect(dc, button, i == 2 ? ui::kWarnBg : ui::kPanelHi, theme_.S(8));
        }
        const int iconSize = theme_.S(15);
        RECT icon = {button.left + ((button.right - button.left) - iconSize) / 2,
                     button.top + ((button.bottom - button.top) - iconSize) / 2, 0, 0};
        icon.right = icon.left + iconSize;
        icon.bottom = icon.top + iconSize;
        const ui::Icon glyph = i == 0 ? ui::Icon::Minimize
                                      : (i == 1 ? (zoomed ? ui::Icon::Restore : ui::Icon::Maximize)
                                                : ui::Icon::Close);
        ui::DrawIcon(dc, glyph, icon, i == 2 && hovered ? ui::kWarn : ui::kMuted, theme_.S(2));
    }
}

void App::PaintSidebar(HDC dc, const RECT& client) {
    (void)client;
    ui::FillSolid(dc, layout_.sidebar, ui::kSidebar);
    const RECT border = {layout_.sidebar.right - 1, layout_.sidebar.top, layout_.sidebar.right,
                         layout_.sidebar.bottom};
    ui::FillSolid(dc, border, ui::kBorder);

    const int centerX = (layout_.sidebar.left + layout_.sidebar.right) / 2;
    const int ghostCy = layout_.sidebar.top + theme_.S(92);
    ui::DrawGlow(dc, centerX, ghostCy + theme_.S(10), theme_.S(94), theme_.S(86), ui::kNeon, 11);
    ui::DrawGhostMark(dc, centerX, ghostCy, theme_.S(104), ui::kNeon, ui::kSidebar, true);

    RECT tagline = {layout_.sidebar.left + theme_.S(32), ghostCy + theme_.S(62),
                    layout_.sidebar.right - theme_.S(32), ghostCy + theme_.S(110)};
    ui::DrawLabel(dc, L"Control your windows.\nStay hidden.", tagline, theme_.small, ui::kMuted,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    const RECT divider = {layout_.sidebar.left + theme_.S(24), tagline.bottom + theme_.S(10),
                          layout_.sidebar.right - theme_.S(24), tagline.bottom + theme_.S(11)};
    ui::FillSolid(dc, divider, ui::kBorder);

    int hidden = 0;
    int errored = 0;
    for (size_t i = 0; i < sessions_.Count(); ++i) {
        const gf::Session& session = sessions_.At(i);
        if (session.hidden()) {
            ++hidden;
        }
        if (session.errored()) {
            ++errored;
        }
    }

    struct StatRow {
        ui::Icon icon;
        std::wstring text;
        COLORREF color;
    };
    wchar_t trackedText[64] = {};
    wchar_t hiddenText[64] = {};
    wchar_t errorText[64] = {};
    swprintf_s(trackedText, L"%zu tracked", sessions_.Count());
    swprintf_s(hiddenText, L"%d hidden", hidden);
    swprintf_s(errorText, L"%d error%s", errored, errored == 1 ? L"" : L"s");
    const StatRow rows[] = {
        {ui::Icon::Dot, L"Running", ui::kNeon},
        {ui::Icon::Monitor, trackedText, ui::kMuted},
        {ui::Icon::EyeOff, hiddenText, hidden > 0 ? ui::kText : ui::kMuted},
        {ui::Icon::Warning, errorText, errored > 0 ? ui::kWarn : ui::kTextDim},
    };

    int y = divider.bottom + theme_.S(18);
    for (const StatRow& row : rows) {
        const int iconSize = theme_.S(16);
        RECT icon = {layout_.sidebar.left + theme_.S(28), y + theme_.S(2),
                     layout_.sidebar.left + theme_.S(28) + iconSize,
                     y + theme_.S(2) + iconSize};
        ui::DrawIcon(dc, row.icon, icon, row.color, theme_.S(2));
        RECT text = {icon.right + theme_.S(12), y, layout_.sidebar.right - theme_.S(20),
                     y + theme_.S(22)};
        ui::DrawLabel(dc, row.text.c_str(), text, theme_.small, row.color,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        y += theme_.S(30);
    }

    // Sidebar footer: the method and the build stay visible, like a status plate.
    RECT method = {layout_.sidebar.left + theme_.S(26), layout_.sidebar.bottom - theme_.S(72),
                   layout_.sidebar.right - theme_.S(22), layout_.sidebar.bottom - theme_.S(54)};
    ui::DrawLabel(dc, L"WDA_EXCLUDEFROMCAPTURE", method, theme_.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT build = {method.left, method.bottom, method.right, layout_.sidebar.bottom - theme_.S(34)};
    wchar_t buildText[96] = {};
    swprintf_s(buildText, L"Per-monitor DPI · %u%%", static_cast<unsigned>(theme_.dpi * 100 / 96));
    ui::DrawLabel(dc, buildText, build, theme_.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::PaintCta(HDC dc, const RECT& rect, const wchar_t* label, ui::Icon icon,
                   ui::Icon trailing, bool primary, bool hovered) {
    if (rect.right <= rect.left) {
        return;
    }
    const int radius = theme_.S(10);
    COLORREF background = primary ? ui::kNeon : ui::kPanelHi;
    COLORREF border = primary ? ui::kNeon : ui::kBorderHi;
    COLORREF text = primary ? ui::kOnAccent : ui::kText;
    if (hovered) {
        background = primary ? ui::Blend(ui::kNeon, RGB(255, 255, 255), 14) : ui::kPanelHover;
        border = primary ? ui::kNeon : ui::kNeonDim;
    }
    ui::FillRoundRect(dc, rect, background, radius);
    ui::StrokeRoundFrame(dc, rect, border, radius, 1);

    const int iconSize = theme_.S(16);
    int left = rect.left + theme_.S(16);
    if (icon != ui::Icon::None) {
        RECT iconRect = {left, rect.top + ((rect.bottom - rect.top) - iconSize) / 2,
                         left + iconSize, 0};
        iconRect.bottom = iconRect.top + iconSize;
        ui::DrawIcon(dc, icon, iconRect, text, theme_.S(2));
        left = iconRect.right + theme_.S(10);
    }
    RECT labelRect = {left, rect.top, rect.right - theme_.S(14), rect.bottom};
    if (trailing != ui::Icon::None) {
        labelRect.right -= theme_.S(20);
    }
    ui::DrawLabel(dc, label, labelRect, theme_.bodyStrong, text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (trailing != ui::Icon::None) {
        RECT arrow = {rect.right - theme_.S(16) - theme_.S(12),
                      rect.top + ((rect.bottom - rect.top) - theme_.S(12)) / 2,
                      rect.right - theme_.S(16), 0};
        arrow.bottom = arrow.top + theme_.S(12);
        ui::DrawIcon(dc, trailing, arrow, text, theme_.S(2));
    }
}

void App::PaintPageChrome(HDC dc) {
    ui::FillRoundRect(dc, layout_.pageIcon, ui::kNeonBg, theme_.S(12));
    ui::StrokeRoundFrame(dc, layout_.pageIcon, ui::kNeonSoft, theme_.S(12), 1);

    ui::Icon pageIcon = ui::Icon::Target;
    const wchar_t* title = L"Tracked Targets";
    const wchar_t* subtitle = L"Manage the windows GhostFrame monitors and hides.";
    if (tab_ == Tab::Windows) {
        pageIcon = ui::Icon::Grid;
        title = L"Windows";
        subtitle = L"Pick a window to attach and hide from capture.";
    } else if (tab_ == Tab::Settings) {
        pageIcon = ui::Icon::Gear;
        title = L"Settings";
        subtitle = L"Tune hotkeys, auto-capture rules and behavior.";
    }

    const int glyph = theme_.S(22);
    RECT glyphRect = {layout_.pageIcon.left + (layout_.pageIcon.right - layout_.pageIcon.left - glyph) / 2,
                      layout_.pageIcon.top + (layout_.pageIcon.bottom - layout_.pageIcon.top - glyph) / 2,
                      0, 0};
    glyphRect.right = glyphRect.left + glyph;
    glyphRect.bottom = glyphRect.top + glyph;
    ui::DrawIcon(dc, pageIcon, glyphRect, ui::kNeon, theme_.S(2));

    ui::DrawLabel(dc, title, layout_.pageTitle, theme_.display, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ui::DrawLabel(dc, subtitle, layout_.pageSubtitle, theme_.small, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (tab_ == Tab::Settings) {
        return;
    }

    if (tab_ == Tab::Targets) {
        PaintCta(dc, layout_.cta, L"Add Target", ui::Icon::Plus, ui::Icon::ChevronRight, true,
                 hotZone_ == HotZone::Cta);
        return;
    }

    wchar_t attachLabel[96] = {};
    if (attachThread_ != nullptr) {
        wcscpy_s(attachLabel, L"Stop attaching");
    } else {
        const int count = windowsList_.SelectedCount();
        if (count > 0) {
            swprintf_s(attachLabel, L"Attach %d window%s", count, count == 1 ? L"" : L"s");
        } else {
            wcscpy_s(attachLabel, L"Attach Selected");
        }
    }
    PaintCta(dc, layout_.secondaryCta, L"Refresh", ui::Icon::Refresh, ui::Icon::None, false,
             hotZone_ == HotZone::Secondary);
    const bool canAttach = attachThread_ != nullptr || windowsList_.SelectedCount() > 0;
    PaintCta(dc, layout_.cta, attachLabel, ui::Icon::Plus, ui::Icon::None, canAttach,
             hotZone_ == HotZone::Cta);
}

void App::PaintTargetsPage(HDC dc) {
    const bool focused = ::GetFocus() == hwnd_;
    if (sessions_.Empty()) {
        PaintEmptyTargets(dc);
        return;
    }

    wchar_t line[96] = {};
    swprintf_s(line, L"%zu window%s tracked", sessions_.Count(),
               sessions_.Count() == 1 ? L"" : L"s");
    RECT caption = {layout_.list.left, layout_.list.top - theme_.S(18), layout_.list.right,
                    layout_.list.top - theme_.S(3)};
    ui::DrawLabel(dc, line, caption, theme_.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const int selected = targetsList_.SelectedCount();
    if (selected > 0) {
        wchar_t selection[64] = {};
        swprintf_s(selection, L"%d selected", selected);
        ui::DrawLabel(dc, selection, caption, theme_.tiny, ui::kNeonDim,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    ui::FillRoundRect(dc, layout_.list, ui::kPanel, theme_.S(14));
    ui::StrokeRoundFrame(dc, layout_.list, ui::kBorder, theme_.S(14), 1);
    targetsList_.Paint(dc, theme_, focused);
}

void App::PaintWindowsPage(HDC dc) {
    const bool focused = ::GetFocus() == hwnd_;

    ui::FillRoundRect(dc, layout_.searchBox, ui::kPanelHi, theme_.S(10));
    ui::StrokeRoundFrame(dc, layout_.searchBox, ui::kBorder, theme_.S(10), 1);
    const int iconSize = theme_.S(16);
    RECT searchIcon = {layout_.searchBox.left + theme_.S(13),
                       layout_.searchBox.top + (layout_.searchBox.bottom - layout_.searchBox.top - iconSize) / 2,
                       0, 0};
    searchIcon.right = searchIcon.left + iconSize;
    searchIcon.bottom = searchIcon.top + iconSize;
    ui::DrawIcon(dc, ui::Icon::Search, searchIcon, ui::kMuted, theme_.S(2));

    ui::FillRoundRect(dc, layout_.list, ui::kPanel, theme_.S(14));
    ui::StrokeRoundFrame(dc, layout_.list, ui::kBorder, theme_.S(14), 1);
    windowsList_.Paint(dc, theme_, focused);
}

void App::PaintEmptyTargets(HDC dc) {
    const RECT card = layout_.emptyCard;
    ui::FillRoundGradient(dc, card, ui::Blend(ui::kPanel, ui::kNeonBg, 10), ui::kPanel,
                          theme_.S(14));
    ui::StrokeRoundFrame(dc, card, ui::kBorder, theme_.S(14), 1);

    // Illustration: a window frame with the brand mark and a plus badge.
    const RECT art = layout_.emptyArt;
    const int artW = art.right - art.left;
    const int artH = art.bottom - art.top;
    const int frameW = (std::min)(artW - theme_.S(24), theme_.S(230));
    const int frameH = (std::min)(artH - theme_.S(16), theme_.S(150));
    RECT frame = {art.left + (artW - frameW) / 2, art.top + (artH - frameH) / 2,
                  art.left + (artW - frameW) / 2 + frameW,
                  art.top + (artH - frameH) / 2 + frameH};
    ui::DrawGlow(dc, (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2,
                 frameW * 3 / 5, frameH * 3 / 5, ui::kNeon, 12);
    ui::FillRoundRect(dc, frame, ui::kPanel, theme_.S(12));
    ui::StrokeRoundFrame(dc, frame, ui::kBorderHi, theme_.S(12), 1);
    const RECT frameBar = {frame.left, frame.top, frame.right, frame.top + theme_.S(26)};
    ui::FillRoundRect(dc, frameBar, ui::kPanelHi, theme_.S(12));
    for (int i = 0; i < 3; ++i) {
        ui::FillCircle(dc, frame.left + theme_.S(16) + i * theme_.S(11),
                       frame.top + theme_.S(13), theme_.S(3),
                       i == 0 ? ui::kNeonDim : ui::kBorderHi);
    }
    ui::DrawGhostMark(dc, (frame.left + frame.right) / 2,
                      (frame.top + frame.bottom) / 2 + theme_.S(6), theme_.S(84), ui::kNeon,
                      ui::kPanel, true);

    const int plus = theme_.S(26);
    RECT plusRect = {frame.right - plus + theme_.S(4), frame.bottom - plus + theme_.S(3),
                     frame.right + theme_.S(4), frame.bottom + theme_.S(3)};
    ui::FillCircle(dc, (plusRect.left + plusRect.right) / 2, (plusRect.top + plusRect.bottom) / 2,
                   plus / 2 + theme_.S(3), ui::kPanel);
    ui::FillCircle(dc, (plusRect.left + plusRect.right) / 2, (plusRect.top + plusRect.bottom) / 2,
                   plus / 2, ui::kNeon);
    const int plusGlyph = theme_.S(13);
    RECT plusIcon = {(plusRect.left + plusRect.right - plusGlyph) / 2,
                     (plusRect.top + plusRect.bottom - plusGlyph) / 2, 0, 0};
    plusIcon.right = plusIcon.left + plusGlyph;
    plusIcon.bottom = plusIcon.top + plusGlyph;
    ui::DrawIcon(dc, ui::Icon::Plus, plusIcon, ui::kOnAccent, theme_.S(2));

    RECT title = {layout_.emptyText.left, layout_.emptyText.top, layout_.emptyText.right,
                  layout_.emptyText.top + theme_.S(38)};
    ui::DrawLabel(dc, L"No targets yet", title, theme_.title, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    wchar_t body[320] = {};
    swprintf_s(body,
               L"Add a target window to start tracking it, or select one on the Windows tab. "
               L"GhostFrame keeps it visible to you and invisible to capture.");
    RECT bodyRect = {layout_.emptyText.left, title.bottom + theme_.S(4),
                     layout_.emptyText.right, layout_.emptyCta.top - theme_.S(10)};
    ui::DrawLabel(dc, body, bodyRect, theme_.small, ui::kMuted,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    PaintCta(dc, layout_.emptyCta, L"Go to Windows", ui::Icon::Grid, ui::Icon::ChevronRight, false,
             hotZone_ == HotZone::EmptyCta);

    RECT hint = {layout_.emptyCta.left, layout_.emptyCta.bottom + theme_.S(14),
                 layout_.emptyText.right, layout_.emptyCta.bottom + theme_.S(34)};
    ui::DrawLabel(dc, L"or press Ctrl+Shift+H to capture the focused window", hint, theme_.tiny,
                  ui::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::PaintSettingsCard(HDC dc, const RECT& card, ui::Icon icon, const wchar_t* title,
                            const wchar_t* subtitle) {
    ui::FillRoundRect(dc, card, ui::kPanel, theme_.S(14));
    ui::StrokeRoundFrame(dc, card, ui::kBorder, theme_.S(14), 1);

    const int iconSize = theme_.S(18);
    RECT iconRect = {card.left + theme_.S(18), card.top + theme_.S(13), 0, 0};
    iconRect.right = iconRect.left + iconSize;
    iconRect.bottom = iconRect.top + iconSize;
    ui::DrawIcon(dc, icon, iconRect, ui::kNeon, theme_.S(2));

    RECT titleRect = {iconRect.right + theme_.S(10), card.top + theme_.S(9),
                      card.right - theme_.S(14), card.top + theme_.S(28)};
    ui::DrawLabel(dc, title, titleRect, theme_.title, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT subtitleRect = {titleRect.left, card.top + theme_.S(28), card.right - theme_.S(14),
                         card.top + theme_.S(44)};
    ui::DrawLabel(dc, subtitle, subtitleRect, theme_.tiny, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const RECT divider = {card.left + theme_.S(14), card.top + theme_.S(46),
                          card.right - theme_.S(14), card.top + theme_.S(47)};
    ui::FillSolid(dc, divider, ui::kBorder);
}

RECT App::SwitchRowRect(const RECT& card, int index) const {
    const int rowHeight = theme_.S(44);
    const int top = card.top + theme_.S(46) + index * rowHeight;
    return RECT{card.left + theme_.S(14), top, card.right - theme_.S(14), top + rowHeight};
}

void App::PaintSwitchRow(HDC dc, const RECT& rect, const wchar_t* title, const wchar_t* subtitle,
                         bool on, bool hovered) {
    if (hovered) {
        ui::FillRoundRect(dc, rect, ui::kPanelHi, theme_.S(8));
    }
    RECT titleRect = {rect.left + theme_.S(6), rect.top + theme_.S(5), rect.right - theme_.S(70),
                      rect.top + theme_.S(24)};
    ui::DrawLabel(dc, title, titleRect, theme_.bodyStrong, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT subtitleRect = {titleRect.left, rect.top + theme_.S(22), titleRect.right,
                         rect.bottom - theme_.S(3)};
    ui::DrawLabel(dc, subtitle, subtitleRect, theme_.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const int switchW = theme_.S(38);
    const int switchH = theme_.S(20);
    RECT sw = {rect.right - theme_.S(10) - switchW,
               rect.top + ((rect.bottom - rect.top) - switchH) / 2, 0, 0};
    sw.right = sw.left + switchW;
    sw.bottom = sw.top + switchH;
    ui::DrawSwitch(dc, theme_, sw, on);

    const RECT separator = {rect.left + theme_.S(6), rect.bottom - 1, rect.right - theme_.S(6),
                            rect.bottom};
    ui::FillSolid(dc, separator, ui::kBorder);
}

void App::PaintSettingsPage(HDC dc) {
    // Secondary navigation rail.
    ui::FillRoundRect(dc, layout_.settingsNav, ui::kPanel, theme_.S(14));
    ui::StrokeRoundFrame(dc, layout_.settingsNav, ui::kBorder, theme_.S(14), 1);

    static const wchar_t* const kNavNames[] = {L"General", L"Hotkeys", L"Auto-Capture",
                                               L"Advanced"};
    static const ui::Icon kNavIcons[] = {ui::Icon::Gear, ui::Icon::Keyboard, ui::Icon::Camera,
                                         ui::Icon::Layers};
    const int active = static_cast<int>(settingsSection_);
    for (int i = 0; i < 4; ++i) {
        const RECT item = layout_.settingsNavItems[i];
        const bool isActive = i == active;
        const bool hovered = hoverNav_ == i;
        if (isActive) {
            ui::FillRoundRect(dc, item, ui::Blend(ui::kPanelHi, ui::kNeonBg, 50), theme_.S(9));
            ui::StrokeRoundFrame(dc, item, ui::kNeonDim, theme_.S(9), 1);
        } else if (hovered) {
            ui::FillRoundRect(dc, item, ui::kPanelHi, theme_.S(9));
        }
        const int iconSize = theme_.S(16);
        RECT icon = {item.left + theme_.S(12),
                     item.top + ((item.bottom - item.top) - iconSize) / 2, 0, 0};
        icon.right = icon.left + iconSize;
        icon.bottom = icon.top + iconSize;
        ui::DrawIcon(dc, kNavIcons[i], icon,
                     isActive ? ui::kNeon : (hovered ? ui::kText : ui::kMuted), theme_.S(2));
        RECT label = {icon.right + theme_.S(11), item.top, item.right, item.bottom};
        ui::DrawLabel(dc, kNavNames[i], label, theme_.bodyStrong,
                      isActive ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    const bool settingsFocus = ::GetFocus() == hwnd_;
    if (settingsSection_ == SettingsSection::General) {
        PaintSettingsCard(dc, layout_.settingsCard[0], ui::Icon::Keyboard, L"Keyboard Shortcuts",
                          L"Control GhostFrame from anywhere.");
        hotkeysList_.Paint(dc, theme_, settingsFocus);

        PaintSettingsCard(dc, layout_.settingsCard[1], ui::Icon::Shield, L"General Behavior",
                          L"How GhostFrame behaves around your desktop.");
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[1], 0), L"Hide on attach",
                       L"Engage capture exclusion as soon as a target is attached.",
                       settings_.ui.hideOnAttach, hoverSwitch_ == 0);
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[1], 1), L"Minimize to tray",
                       L"Keep GhostFrame in the notification area instead of the taskbar.",
                       settings_.ui.minimizeToTray, hoverSwitch_ == 1);
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[1], 2), L"Exclude self",
                       L"GhostFrame stays invisible to capture as well.",
                       !settings_.ui.noSelfExclude, hoverSwitch_ == 2);

        PaintSettingsCard(dc, layout_.settingsCard[2], ui::Icon::Camera, L"Auto-Capture",
                          L"Attach matching applications automatically.");
        wchar_t summary[128] = {};
        swprintf_s(summary, L"%zu rule%s configured", settings_.autoAttach.size(),
                   settings_.autoAttach.size() == 1 ? L"" : L"s");
        RECT summaryRect = {layout_.settingsCard[2].left + theme_.S(18),
                            layout_.settingsCard[2].top + theme_.S(54),
                            layout_.settingsManage.left - theme_.S(12),
                            layout_.settingsCard[2].bottom - theme_.S(8)};
        ui::DrawLabel(dc, summary, summaryRect, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        PaintCta(dc, layout_.settingsManage, L"Manage", ui::Icon::None, ui::Icon::ChevronRight,
                 false, hotZone_ == HotZone::SettingsManage);
    } else if (settingsSection_ == SettingsSection::Hotkeys) {
        PaintSettingsCard(dc, layout_.settingsCard[0], ui::Icon::Keyboard, L"Keyboard Shortcuts",
                          L"Click a row, then press the new combination. Esc cancels.");
        hotkeysList_.Paint(dc, theme_, settingsFocus);
        RECT hint = {layout_.settingsCard[0].left + theme_.S(16),
                     layout_.settingsCard[0].bottom - theme_.S(34),
                     layout_.settingsCard[0].right - theme_.S(16),
                     layout_.settingsCard[0].bottom - theme_.S(12)};
        ui::DrawLabel(dc, L"At least one of Ctrl, Alt, Shift or Win is required.",
                      hint, theme_.tiny, ui::kTextDim,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (settingsSection_ == SettingsSection::AutoCapture) {
        PaintSettingsCard(dc, layout_.settingsCard[0], ui::Icon::Camera,
                          L"Auto-Capture Rules",
                          L"These applications are attached automatically when their windows appear.");
        rulesList_.Paint(dc, theme_, settingsFocus);
    } else {
        PaintSettingsCard(dc, layout_.settingsCard[0], ui::Icon::Shield, L"General Behavior",
                          L"How GhostFrame behaves around your desktop.");
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[0], 0), L"Hide on attach",
                       L"Engage capture exclusion as soon as a target is attached.",
                       settings_.ui.hideOnAttach, hoverSwitch_ == 0);
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[0], 1), L"Minimize to tray",
                       L"Keep GhostFrame in the notification area instead of the taskbar.",
                       settings_.ui.minimizeToTray, hoverSwitch_ == 1);
        PaintSwitchRow(dc, SwitchRowRect(layout_.settingsCard[0], 2), L"Exclude self",
                       L"GhostFrame stays invisible to capture as well.",
                       !settings_.ui.noSelfExclude, hoverSwitch_ == 2);

        PaintSettingsCard(dc, layout_.settingsCard[1], ui::Icon::Layers, L"Maintenance",
                          L"Diagnostics and last-resort actions.");
        RECT text = {layout_.settingsCard[1].left + theme_.S(18),
                     layout_.settingsCard[1].top + theme_.S(54),
                     layout_.settingsCard[1].right - theme_.S(18),
                     layout_.settingsLogCta.top - theme_.S(6)};
        ui::DrawLabel(dc,
                      L"Settings are stored in %LOCALAPPDATA%\\GhostFrame\\settings.json and "
                      L"saved automatically.",
                      text, theme_.tiny, ui::kTextDim,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        PaintCta(dc, layout_.settingsLogCta, L"Open log folder", ui::Icon::Folder,
                 ui::Icon::ArrowUpRight, false, hotZone_ == HotZone::SettingsLog);
        PaintCta(dc, layout_.settingsRestoreCta, L"Restore all", ui::Icon::Eye,
                 ui::Icon::None, false, hotZone_ == HotZone::SettingsRestore);
    }
}

void App::PaintFooter(HDC dc) {
    ui::FillSolid(dc, layout_.footerLine, ui::kBorder);

    const int iconSize = theme_.S(13);
    RECT info = {layout_.footer.left, layout_.footer.top + (layout_.footer.bottom - layout_.footer.top - iconSize) / 2,
                 0, 0};
    info.right = info.left + iconSize;
    info.bottom = info.top + iconSize;
    ui::DrawIcon(dc, statusError_ ? ui::Icon::Warning : ui::Icon::Info, info,
                 statusError_ ? ui::kWarn : ui::kTextDim, theme_.S(2));

    RECT statusRect = {info.right + theme_.S(8), layout_.footer.top, layout_.logLink.left - theme_.S(190),
                       layout_.footer.bottom};
    ui::DrawLabel(dc, status_.c_str(), statusRect, theme_.small,
                  statusError_ ? ui::kWarn : ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    int hidden = 0;
    int errored = 0;
    for (size_t i = 0; i < sessions_.Count(); ++i) {
        const gf::Session& session = sessions_.At(i);
        if (session.hidden()) {
            ++hidden;
        }
        if (session.errored()) {
            ++errored;
        }
    }
    wchar_t summary[96] = {};
    swprintf_s(summary, L"%zu tracked · %d hidden · %d errors", sessions_.Count(), hidden, errored);
    RECT summaryRect = {layout_.logLink.left - theme_.S(190), layout_.footer.top,
                        layout_.logLink.left - theme_.S(16), layout_.footer.bottom};
    ui::DrawLabel(dc, summary, summaryRect, theme_.tiny, errored > 0 ? ui::kWarn : ui::kTextDim,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT log = layout_.logLink;
    const int arrow = theme_.S(12);
    RECT arrowRect = {log.right - arrow, log.top + (log.bottom - log.top - arrow) / 2, 0, 0};
    arrowRect.right = arrowRect.left + arrow;
    arrowRect.bottom = arrowRect.top + arrow;
    ui::DrawIcon(dc, ui::Icon::ArrowUpRight, arrowRect,
                 hotZone_ == HotZone::Log ? ui::kNeon : ui::kNeonDim, theme_.S(2));
    RECT logText = {log.left, log.top, arrowRect.left - theme_.S(6), log.bottom};
    ui::DrawLabel(dc, L"Open log", logText, theme_.tiny,
                  hotZone_ == HotZone::Log ? ui::kNeon : ui::kNeonDim,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::OnTimer() {
    if (settingsDirty_ && ::GetTickCount64() >= settingsSaveDue_) {
        SaveSettings();
    }

    if (!toast_.empty() && ::GetTickCount64() >= toastUntil_) {
        toast_.clear();
        InvalidateAll();
    }

    RetryPendingCommand();

    const size_t before = sessions_.Count();
    const HWND selectedHwnd = SelectedValid() ? sessions_.At(static_cast<size_t>(selected_)).hwnd
                                              : nullptr;

    if (!sessions_.Refresh() && before == sessions_.Count()) {
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

    BuildTargetRows();
    // BuildTargetRows re-maps the widget selection by row data, so a surviving
    // multi-selection is preserved.  Only pull the cursor back when the tracked
    // window disappeared and the widget could not restore it.
    if (targetsList_.Selected() != selected_) {
        targetsList_.Select(selected_);
    }

    if (before != sessions_.Count()) {
        UpdateTray();
    }
    InvalidateAll();
}

void App::ApplyTabControls() {
    const bool targets = tab_ == Tab::Targets && !sessions_.Empty();
    const bool windows = tab_ == Tab::Windows;
    const bool settings = tab_ == Tab::Settings;

    for (int id : {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_MIN, ID_BTN_RESTORE, ID_BTN_CLOSE}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, targets ? SW_SHOW : SW_HIDE);
        }
    }

    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::ShowWindow(search, windows ? SW_SHOW : SW_HIDE);
    }

    const bool resetVisible =
        settings && (settingsSection_ == SettingsSection::General ||
                     settingsSection_ == SettingsSection::Hotkeys);
    const bool rulesVisible = settings && settingsSection_ == SettingsSection::AutoCapture;
    for (int id : {ID_BTN_SET_RESET, ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE, ID_BTN_SET_APPLY}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            const bool show = id == ID_BTN_SET_RESET ? resetVisible : rulesVisible;
            ::ShowWindow(child, show ? SW_SHOW : SW_HIDE);
        }
    }
}

void App::SelectTab(Tab tab) {
    if (tab_ == tab) {
        return;
    }
    tab_ = tab;
    Layout();
    ApplyTabControls();

    if (tab_ == Tab::Windows) {
        RefreshWindows();
        ::SetTimer(hwnd_, kWindowsTimer, kWindowsRefreshMs, nullptr);
        SetStatus(L"Pick one or more windows, then attach them in one action");
    } else {
        ::KillTimer(hwnd_, kWindowsTimer);
        if (tab_ == Tab::Settings) {
            BuildSettingsRows();
            SetStatus(L"Settings — click a hotkey to rebind; Esc cancels");
        } else {
            BuildTargetRows();
            if (sessions_.Empty()) {
                SetStatus(L"Ready — focus a window and press Ctrl+Shift+H, or add a target");
            } else {
                SetStatus(L"Select a target to manage it, or use the actions below");
            }
        }
    }

    settings_.ui.lastTab = tab_ == Tab::Windows ? L"windows"
                           : tab_ == Tab::Settings ? L"settings"
                                                   : L"targets";
    MarkSettingsDirty();
    InvalidateAll();
}

void App::SetSettingsSection(SettingsSection section) {
    if (settingsSection_ == section) {
        return;
    }
    settingsSection_ = section;
    Layout();
    ApplyTabControls();
    BuildSettingsRows();
    InvalidateAll();
}

void App::OnSize() {
    Layout();
    InvalidateAll();
}

void App::OnSettingChange() {
    theme_.Create(::GetDpiForWindow(hwnd_));
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
    }
    if (tooltip_ != nullptr) {
        ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
    }
    Layout();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Mouse / keyboard
// ---------------------------------------------------------------------------

void App::OnMouseMove(int x, int y) {
    bool changed = false;

    const HotZone zone = ZoneAt(x, y);
    const int tab = TabAt(x, y);
    const int nav = SettingsNavAt(x, y);
    const int sw = BehaviorSwitchAt(x, y);
    if (zone != hotZone_ || tab != hoverTab_ || nav != hoverNav_ || sw != hoverSwitch_) {
        hotZone_ = zone;
        hoverTab_ = tab;
        hoverNav_ = nav;
        hoverSwitch_ = sw;
        changed = true;
    }

    if (tab_ == Tab::Windows) {
        changed |= windowsList_.OnMouseMove(x, y, true);
        changed |= targetsList_.ClearHover();
        changed |= hotkeysList_.ClearHover();
        changed |= rulesList_.ClearHover();
    } else if (tab_ == Tab::Settings) {
        if (settingsSection_ == SettingsSection::AutoCapture) {
            changed |= rulesList_.OnMouseMove(x, y, true);
            changed |= hotkeysList_.ClearHover();
        } else {
            changed |= hotkeysList_.OnMouseMove(x, y, true);
            changed |= rulesList_.ClearHover();
        }
        changed |= targetsList_.ClearHover();
        changed |= windowsList_.ClearHover();
    } else {
        changed |= targetsList_.OnMouseMove(x, y, true);
        changed |= windowsList_.ClearHover();
        changed |= hotkeysList_.ClearHover();
        changed |= rulesList_.ClearHover();
    }
    if (changed) {
        InvalidateAll();
    }
}

void App::OnMouseLeave() {
    const bool a = targetsList_.ClearHover();
    const bool b = windowsList_.ClearHover();
    const bool c = hotkeysList_.ClearHover();
    const bool d = rulesList_.ClearHover();
    const bool chrome = hotZone_ != HotZone::None || hoverTab_ >= 0 || hoverNav_ >= 0 ||
                        hoverSwitch_ >= 0;
    hotZone_ = HotZone::None;
    hoverTab_ = -1;
    hoverNav_ = -1;
    hoverSwitch_ = -1;
    if (a || b || c || d || chrome) {
        InvalidateAll();
    }
}

void App::OnMouseWheel(int x, int y, int delta) {
    POINT point = {x, y};
    ::ScreenToClient(hwnd_, &point);
    ui::ListWidget* list = nullptr;
    if (tab_ == Tab::Windows) {
        list = &windowsList_;
    } else if (tab_ == Tab::Settings) {
        if (settingsSection_ == SettingsSection::AutoCapture) {
            list = &rulesList_;
        } else if (settingsSection_ == SettingsSection::General ||
                   settingsSection_ == SettingsSection::Hotkeys) {
            list = &hotkeysList_;
        }
    } else {
        list = &targetsList_;
    }
    if (list != nullptr && list->Contains(point.x, point.y)) {
        list->OnMouseWheel(delta);
        InvalidateAll();
    }
}

int App::TabAt(int x, int y) const {
    POINT point = {x, y};
    for (int index = 0; index < layout_.tabCount; ++index) {
        if (::PtInRect(&layout_.tabs[index], point)) {
            return index;
        }
    }
    return -1;
}

int App::SettingsNavAt(int x, int y) const {
    if (tab_ != Tab::Settings) {
        return -1;
    }
    POINT point = {x, y};
    for (int index = 0; index < 4; ++index) {
        if (::PtInRect(&layout_.settingsNavItems[index], point)) {
            return index;
        }
    }
    return -1;
}

int App::BehaviorSwitchAt(int x, int y) const {
    if (tab_ != Tab::Settings) {
        return -1;
    }
    RECT card = {};
    if (settingsSection_ == SettingsSection::General) {
        card = layout_.settingsCard[1];
    } else if (settingsSection_ == SettingsSection::Advanced) {
        card = layout_.settingsCard[0];
    } else {
        return -1;
    }
    POINT point = {x, y};
    for (int index = 0; index < 3; ++index) {
        RECT row = SwitchRowRect(card, index);
        if (::PtInRect(&row, point)) {
            return index;
        }
    }
    return -1;
}

App::HotZone App::ZoneAt(int x, int y) const {
    POINT point = {x, y};
    for (int i = 0; i < 3; ++i) {
        if (::PtInRect(&layout_.windowButtons[i], point)) {
            return i == 0 ? HotZone::Hide : (i == 1 ? HotZone::Maximize : HotZone::Close);
        }
    }
    if (::PtInRect(&layout_.logLink, point)) {
        return HotZone::Log;
    }
    if (layout_.cta.right > layout_.cta.left && ::PtInRect(&layout_.cta, point)) {
        return HotZone::Cta;
    }
    if (layout_.secondaryCta.right > layout_.secondaryCta.left &&
        ::PtInRect(&layout_.secondaryCta, point)) {
        return HotZone::Secondary;
    }
    if (tab_ == Tab::Targets && sessions_.Empty() && layout_.emptyCta.right > layout_.emptyCta.left &&
        ::PtInRect(&layout_.emptyCta, point)) {
        return HotZone::EmptyCta;
    }
    if (tab_ == Tab::Settings && settingsSection_ == SettingsSection::General &&
        layout_.settingsManage.right > layout_.settingsManage.left &&
        ::PtInRect(&layout_.settingsManage, point)) {
        return HotZone::SettingsManage;
    }
    if (tab_ == Tab::Settings && settingsSection_ == SettingsSection::Advanced) {
        if (layout_.settingsLogCta.right > layout_.settingsLogCta.left &&
            ::PtInRect(&layout_.settingsLogCta, point)) {
            return HotZone::SettingsLog;
        }
        if (layout_.settingsRestoreCta.right > layout_.settingsRestoreCta.left &&
            ::PtInRect(&layout_.settingsRestoreCta, point)) {
            return HotZone::SettingsRestore;
        }
    }
    return HotZone::None;
}

void App::ActivateZone(HotZone zone) {
    switch (zone) {
        case HotZone::Hide:
            ShowUi(false);
            break;
        case HotZone::Maximize:
            ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case HotZone::Close:
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case HotZone::Log:
            OpenLogFolder();
            break;
        case HotZone::Cta:
            if (tab_ == Tab::Targets) {
                SelectTab(Tab::Windows);
                if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
                    ::SetFocus(search);
                }
            } else if (tab_ == Tab::Windows) {
                OnCommand(ID_BTN_ATTACH);
            }
            break;
        case HotZone::Secondary:
            OnCommand(ID_BTN_REFRESH);
            break;
        case HotZone::EmptyCta:
            SelectTab(Tab::Windows);
            break;
        case HotZone::SettingsManage:
            SetSettingsSection(SettingsSection::AutoCapture);
            break;
        case HotZone::SettingsLog:
            OpenLogFolder();
            break;
        case HotZone::SettingsRestore:
            RestoreAll();
            break;
        case HotZone::None:
        default:
            break;
    }
}

void App::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    const int tab = TabAt(x, y);
    if (tab >= 0) {
        SelectTab(tab == 0 ? Tab::Targets : (tab == 1 ? Tab::Windows : Tab::Settings));
        return;
    }

    const int nav = SettingsNavAt(x, y);
    if (nav >= 0) {
        SetSettingsSection(nav == 0 ? SettingsSection::General
                                    : (nav == 1 ? SettingsSection::Hotkeys
                                                : (nav == 2 ? SettingsSection::AutoCapture
                                                            : SettingsSection::Advanced)));
        return;
    }

    const int sw = BehaviorSwitchAt(x, y);
    if (sw >= 0) {
        OnCommand(sw == 0 ? ID_BTN_SET_HIDE_ATTACH
                          : (sw == 1 ? ID_BTN_SET_TRAY : ID_BTN_SET_SELF));
        return;
    }

    const HotZone zone = ZoneAt(x, y);
    if (zone != HotZone::None) {
        ActivateZone(zone);
        return;
    }

    if (tab_ == Tab::Settings) {
        if (settingsSection_ == SettingsSection::AutoCapture && rulesList_.Contains(x, y)) {
            OnSettingsListAction(rulesList_.OnLButtonDown(x, y, ctrl, shift), false);
        } else if ((settingsSection_ == SettingsSection::General ||
                    settingsSection_ == SettingsSection::Hotkeys) &&
                   hotkeysList_.Contains(x, y)) {
            OnSettingsListAction(hotkeysList_.OnLButtonDown(x, y, ctrl, shift), true);
        }
        InvalidateAll();
        return;
    }

    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    ApplyListAction(list.OnLButtonDown(x, y, ctrl, shift), tab_ == Tab::Windows);
    InvalidateAll();
}

void App::OnLButtonDblClk(int x, int y) {
    if (tab_ == Tab::Settings) {
        if ((settingsSection_ == SettingsSection::General ||
             settingsSection_ == SettingsSection::Hotkeys) &&
            hotkeysList_.Contains(x, y)) {
            OnSettingsListAction(hotkeysList_.OnLButtonDblClk(x, y), true);
        } else if (settingsSection_ == SettingsSection::AutoCapture &&
                   rulesList_.Contains(x, y)) {
            // Double-click a rule to flip its enabled state.
            const int row = rulesList_.IndexAt(x, y);
            const int rule = row >= 0 ? static_cast<int>(rulesList_.DataAt(row)) : -1;
            if (rule >= 0 && rule < static_cast<int>(settings_.autoAttach.size())) {
                selectedRule_ = rule;
                settings_.autoAttach[static_cast<size_t>(rule)].enabled =
                    !settings_.autoAttach[static_cast<size_t>(rule)].enabled;
                MarkSettingsDirty();
                BuildSettingsRows();
                rulesList_.Select(row);
            }
        }
        InvalidateAll();
        return;
    }

    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    ApplyListAction(list.OnLButtonDblClk(x, y), tab_ == Tab::Windows);
    InvalidateAll();
}

void App::OnKeyDown(int virtualKey) {
    if (rebinding_) {
        if (virtualKey == VK_ESCAPE) {
            rebinding_ = false;
            rebindAction_ = gf::HotkeyAction::Count;
            BuildSettingsRows();
            SetStatus(L"Hotkey rebind cancelled");
            InvalidateAll();
            return;
        }
        // Ignore the modifier keys themselves; wait for the real key.
        if (virtualKey == VK_CONTROL || virtualKey == VK_SHIFT || virtualKey == VK_MENU ||
            virtualKey == VK_LWIN || virtualKey == VK_RWIN || virtualKey == VK_LCONTROL ||
            virtualKey == VK_RCONTROL || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
            virtualKey == VK_LMENU || virtualKey == VK_RMENU) {
            return;
        }

        UINT modifiers = 0;
        if (::GetKeyState(VK_CONTROL) & 0x8000) {
            modifiers |= MOD_CONTROL;
        }
        if (::GetKeyState(VK_MENU) & 0x8000) {
            modifiers |= MOD_ALT;
        }
        if (::GetKeyState(VK_SHIFT) & 0x8000) {
            modifiers |= MOD_SHIFT;
        }
        if ((::GetKeyState(VK_LWIN) & 0x8000) || (::GetKeyState(VK_RWIN) & 0x8000)) {
            modifiers |= MOD_WIN;
        }
        FinishRebind(modifiers, static_cast<UINT>(virtualKey));
        return;
    }

    if (virtualKey == VK_F5) {
        if (tab_ == Tab::Windows) {
            RefreshWindows();
            SetStatus(L"Window list refreshed");
        }
        return;
    }

    if (virtualKey == VK_TAB) {
        const bool back = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const int current = tab_ == Tab::Targets ? 0 : (tab_ == Tab::Windows ? 1 : 2);
        const int next = (current + (back ? 2 : 1)) % 3;
        SelectTab(next == 0 ? Tab::Targets : (next == 1 ? Tab::Windows : Tab::Settings));
        return;
    }

    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT) {
        const int current = tab_ == Tab::Targets ? 0 : (tab_ == Tab::Windows ? 1 : 2);
        const int next = (current + (virtualKey == VK_LEFT ? 2 : 1)) % 3;
        SelectTab(next == 0 ? Tab::Targets : (next == 1 ? Tab::Windows : Tab::Settings));
        return;
    }

    if (virtualKey == VK_ESCAPE && attachThread_ != nullptr) {
        CancelAttach();
        return;
    }

    if (virtualKey == VK_DELETE && tab_ == Tab::Targets) {
        RemoveSelected();
        return;
    }

    if (tab_ == Tab::Settings) {
        if (settingsSection_ == SettingsSection::AutoCapture) {
            if (virtualKey == VK_DELETE && selectedRule_ >= 0 &&
                selectedRule_ < static_cast<int>(settings_.autoAttach.size())) {
                settings_.autoAttach.erase(settings_.autoAttach.begin() + selectedRule_);
                selectedRule_ = -1;
                MarkSettingsDirty();
                BuildSettingsRows();
                SetStatus(L"Auto-capture rule removed");
                InvalidateAll();
                return;
            }
            rulesList_.OnKeyDown(virtualKey);
        } else if (settingsSection_ == SettingsSection::General ||
                   settingsSection_ == SettingsSection::Hotkeys) {
            const ui::ListWidget::Action action = hotkeysList_.OnKeyDown(virtualKey);
            if (action.kind == ui::ListWidget::ActionKind::Activate ||
                action.kind == ui::ListWidget::ActionKind::Toggle) {
                const int index = static_cast<int>(hotkeysList_.DataAt(action.index));
                if (index >= 0 && index < gf::kHotkeyCount) {
                    BeginRebind(static_cast<gf::HotkeyAction>(index));
                    return;
                }
            }
        }
        InvalidateAll();
        return;
    }

    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    ApplyListAction(list.OnKeyDown(virtualKey), tab_ == Tab::Windows);
    InvalidateAll();
}

void App::OnVScroll(int code, int position) {
    ui::ListWidget* list = nullptr;
    if (tab_ == Tab::Windows) {
        list = &windowsList_;
    } else if (tab_ == Tab::Settings) {
        list = &hotkeysList_;
    } else {
        list = &targetsList_;
    }
    list->OnVScroll(code, position);
    InvalidateAll();
}

void App::OnButtonHover(HWND button, bool hovered) {
    const int id = hovered ? ::GetDlgCtrlID(button) : 0;
    if (id == hoveredButton_) {
        return;
    }
    if (hovered) {
        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = button;
        ::TrackMouseEvent(&track);
    }
    hoveredButton_ = id;
    ::InvalidateRect(button, nullptr, TRUE);
}

LRESULT CALLBACK App::ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<App*>(ref);
    switch (message) {
        case WM_MOUSEMOVE:
            if (self != nullptr) {
                self->OnButtonHover(hwnd, true);
                self->RelayTooltip(hwnd, message, wParam, lParam);
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            if (self != nullptr) {
                self->RelayTooltip(hwnd, message, wParam, lParam);
            }
            break;
        case WM_MOUSELEAVE:
            if (self != nullptr) {
                self->OnButtonHover(hwnd, false);
            }
            break;
        case WM_ERASEBKGND:
            return 1;  // WM_DRAWITEM fills the whole surface
        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        default:
            break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Commands, hotkeys, tray
// ---------------------------------------------------------------------------

void App::OnCommand(int id) {
    switch (id) {
        case ID_BTN_TOGGLE:
            ToggleSelection();
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
        case ID_BTN_REFRESH:
            if (tab_ == Tab::Windows) {
                RefreshWindows();
                SetStatus(L"Window list refreshed");
            }
            break;
        case ID_BTN_ATTACH:
            if (attachThread_ != nullptr) {
                CancelAttach();
            } else {
                AttachSelectedWindow();
            }
            break;
        case ID_BTN_HIDE_UI:
            ShowUi(::IsWindowVisible(hwnd_) == FALSE);
            break;
        case ID_BTN_SET_RESET:
            ResetHotkeysToDefaults();
            break;
        case ID_BTN_SET_ADD_FG:
            AddRuleForWindow(::GetForegroundWindow());
            break;
        case ID_BTN_SET_BROWSE: {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = _countof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (::GetOpenFileNameW(&ofn)) {
                AddRuleForExePath(file);
            }
            break;
        }
        case ID_BTN_SET_ENABLE:
            if (selectedRule_ >= 0 && selectedRule_ < static_cast<int>(settings_.autoAttach.size())) {
                settings_.autoAttach[static_cast<size_t>(selectedRule_)].enabled =
                    !settings_.autoAttach[static_cast<size_t>(selectedRule_)].enabled;
                MarkSettingsDirty();
                BuildSettingsRows();
                rulesList_.Select(selectedRule_);
                InvalidateAll();
            }
            break;
        case ID_BTN_SET_HIDE:
            if (selectedRule_ >= 0 && selectedRule_ < static_cast<int>(settings_.autoAttach.size())) {
                settings_.autoAttach[static_cast<size_t>(selectedRule_)].hide =
                    !settings_.autoAttach[static_cast<size_t>(selectedRule_)].hide;
                MarkSettingsDirty();
                BuildSettingsRows();
                rulesList_.Select(selectedRule_);
                InvalidateAll();
            }
            break;
        case ID_BTN_SET_REMOVE:
            if (selectedRule_ >= 0 && selectedRule_ < static_cast<int>(settings_.autoAttach.size())) {
                settings_.autoAttach.erase(settings_.autoAttach.begin() + selectedRule_);
                selectedRule_ = -1;
                MarkSettingsDirty();
                BuildSettingsRows();
                SetStatus(L"AUTO-CAPTURE RULE REMOVED");
                InvalidateAll();
            }
            break;
        case ID_BTN_SET_APPLY:
            ApplyAutoRulesNow();
            break;
        case ID_BTN_SET_HIDE_ATTACH:
            settings_.ui.hideOnAttach = !settings_.ui.hideOnAttach;
            MarkSettingsDirty();
            InvalidateAll();
            break;
        case ID_BTN_SET_TRAY:
            settings_.ui.minimizeToTray = !settings_.ui.minimizeToTray;
            MarkSettingsDirty();
            InvalidateAll();
            break;
        case ID_BTN_SET_SELF:
            settings_.ui.noSelfExclude = !settings_.ui.noSelfExclude;
            noSelfExclude_ = settings_.ui.noSelfExclude;
            ::SetWindowDisplayAffinity(hwnd_, noSelfExclude_ ? 0x00 : 0x11);
            MarkSettingsDirty();
            InvalidateAll();
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
        case ID_TRAY_LOG:
            OpenLogFolder();
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

            const size_t count = sessions_.Count();
            if (count > 0) {
                HMENU targets = ::CreatePopupMenu();
                for (size_t i = 0; i < count; ++i) {
                    const gf::Session& session = sessions_.At(i);
                    HMENU sub = ::CreatePopupMenu();
                    if (sub == nullptr) {
                        continue;
                    }
                    const UINT_PTR base = ID_TRAY_TARGET_BASE + i * ID_TRAY_TARGET_STRIDE;
                    ::AppendMenuW(sub, MF_STRING, base + ID_TRAY_TARGET_TOGGLE,
                                  session.hidden() ? L"Show (lift exclusion)"
                                                   : L"Hide (exclude from capture)");
                    ::AppendMenuW(sub, MF_STRING, base + ID_TRAY_TARGET_RESTORE,
                                  L"Restore window");
                    ::AppendMenuW(sub, MF_STRING, base + ID_TRAY_TARGET_CLOSE, L"Close window");

                    std::wstring label = session.title.empty() ? L"(untitled)" : session.title;
                    if (label.size() > 48) {
                        label.resize(48);
                        label += L"…";
                    }
                    ::AppendMenuW(targets, MF_POPUP, reinterpret_cast<UINT_PTR>(sub),
                                  label.c_str());
                }
                ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(targets), L"Targets");
            }

            ::AppendMenuW(menu, MF_STRING, ID_TRAY_RESTORE_ALL, L"Restore all targets");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_LOG, L"Open log folder");
            ::AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

            POINT point = {};
            ::GetCursorPos(&point);
            ::SetForegroundWindow(hwnd_);
            const int command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x,
                                                 point.y, 0, hwnd_, nullptr);
            ::DestroyMenu(menu);
            ::PostMessageW(hwnd_, WM_NULL, 0, 0);

            if (command == ID_TRAY_SHOW) {
                ShowUi(true);
            } else if (command == ID_TRAY_RESTORE_ALL) {
                RestoreAll();
            } else if (command == ID_TRAY_LOG) {
                OpenLogFolder();
            } else if (command == ID_TRAY_EXIT) {
                ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            } else if (command >= ID_TRAY_TARGET_BASE) {
                const int offset = command - ID_TRAY_TARGET_BASE;
                const size_t index = static_cast<size_t>(offset / ID_TRAY_TARGET_STRIDE);
                const int action = offset % ID_TRAY_TARGET_STRIDE;
                if (index < sessions_.Count()) {
                    if (action == ID_TRAY_TARGET_TOGGLE) {
                        selected_ = static_cast<int>(index);
                        ToggleStealth(static_cast<int>(index));
                    } else if (action == ID_TRAY_TARGET_RESTORE) {
                        const HWND target = sessions_.At(index).hwnd;
                        if (::IsWindow(target)) {
                            ::ShowWindow(target, SW_RESTORE);
                        }
                    } else if (action == ID_TRAY_TARGET_CLOSE) {
                        selected_ = static_cast<int>(index);
                        TargetClose();
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested) {
    theme_.Create(dpi);
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
    }
    if (tooltip_ != nullptr) {
        ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
    }
    if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Attach orchestration
// ---------------------------------------------------------------------------

void App::AttachForeground(bool hideAfter) {
    if (attachThread_ != nullptr) {
        SetStatus(L"Attaching… — wait for the current attach to finish", true);
        return;
    }

    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr || foreground == hwnd_) {
        if (!sessions_.Empty()) {
            SelectTarget(0);
            ToggleStealth(0);
        } else {
            SetStatus(L"No foreground window — focus the window you want to track", true);
        }
        return;
    }

    std::wstring reason;
    if (!gf::ValidateTarget(foreground, reason)) {
        SetStatus(L"Attach failed: " + reason, true);
        return;
    }
    if (sessions_.IndexOf(foreground) >= 0) {
        SetStatus(L"The window is already tracked", true);
        return;
    }

    StartAttachBatch({foreground}, hideAfter);
}

void App::StartAttachBatch(std::vector<HWND> targets, bool hideAfter) {
    if (attachThread_ != nullptr) {
        SetStatus(L"Attaching… — wait for the current attach to finish", true);
        return;
    }

    // Drop duplicates (within the request and against the session list) before
    // the worker starts; a window that appears twice must be attempted once.
    std::vector<HWND> pending;
    for (HWND target : targets) {
        if (target == nullptr || sessions_.IndexOf(target) >= 0) {
            continue;
        }
        bool duplicate = false;
        for (HWND existing : pending) {
            if (existing == target) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            pending.push_back(target);
        }
    }

    if (pending.empty()) {
        SetStatus(L"Nothing to attach", true);
        return;
    }

    ::InterlockedExchange(&attachCancel_, 0);
    attachHide_ = hideAfter;
    attachTotal_ = static_cast<int>(pending.size());

    auto* job = new AttachJob{hwnd_, &injector_, std::move(pending), &attachCancel_, hideAfter};
    attachThread_ = ::CreateThread(nullptr, 0, &App::AttachThreadProc, job, 0, nullptr);
    if (attachThread_ == nullptr) {
        delete job;
        attachTotal_ = 0;
        SetStatus(L"Attach failed: cannot start the attach worker", true);
        return;
    }

    UpdateAttachButton();
    SetStatus(L"Attaching…");
    InvalidateAll();
}

DWORD WINAPI App::AttachThreadProc(LPVOID param) {
    AttachJob* job = static_cast<AttachJob*>(param);

    auto* batch = new BatchCompletion;
    const size_t total = job->targets.size();
    for (size_t i = 0; i < total; ++i) {
        // Cancellation is checked between targets only: the attach in flight is
        // already bounded by its own timeouts and must not be interrupted.
        if (::InterlockedCompareExchange(job->cancel, 0, 0) != 0) {
            batch->cancelled = true;
            break;
        }

        const HWND target = job->targets[i];
        ::PostMessageW(job->notify, kAttachProgressMessage, static_cast<WPARAM>(i + 1),
                       reinterpret_cast<LPARAM>(target));

        BatchItem item;
        item.hwnd = target;
        gf::PrepareAttach(target, *job->injector, item.result);
        item.title = item.result.title;
        if (item.result.ok && job->hideAfter) {
            // Hide from the worker: the SessionManager is UI-thread only, but the
            // raw IPC is safe to drive here.
            item.result.session.ipc.Send(gf::Cmd::Hide, 500);
        }
        batch->items.push_back(std::move(item));
    }

    if (!::PostMessageW(job->notify, kAttachDoneMessage, 0, reinterpret_cast<LPARAM>(batch))) {
        // Nobody will ever consume the result: detach every agent we started so
        // no window stays hidden behind a lost session.
        for (BatchItem& item : batch->items) {
            if (item.result.ok) {
                item.result.session.ipc.Send(gf::Cmd::Uninstall, 0);
                item.result.session.ipc.SignalStop();
            }
        }
        delete batch;
    }
    delete job;
    return 0;
}

void App::OnAttachProgress(WPARAM done, LPARAM target) {
    wchar_t title[160] = {};
    ::GetWindowTextW(reinterpret_cast<HWND>(target), title, _countof(title) - 1);

    wchar_t status[320] = {};
    swprintf_s(status, L"Attaching %zu/%d — %s", static_cast<size_t>(done), attachTotal_,
               title[0] != L'\0' ? title : L"(untitled window)");
    SetStatus(status);
}

void App::OnAttachDone(LPARAM lParam) {
    auto* batch = reinterpret_cast<BatchCompletion*>(lParam);

    if (attachThread_ != nullptr) {
        ::CloseHandle(attachThread_);
        attachThread_ = nullptr;
    }
    UpdateAttachButton();

    if (batch == nullptr) {
        return;
    }

    int attached = 0;
    int failed = 0;
    std::wstring failedText;
    int lastIndex = -1;

    // Windows that the auto-capture engine requested: a failure counts against
    // the anti-loop budget so a window that keeps refusing the agent is not
    // retried forever.
    const auto noteAuto = [this](HWND hwnd, bool ok) {
        for (size_t i = 0; i < autoRequests_.size(); ++i) {
            if (autoRequests_[i].hwnd == hwnd) {
                if (!ok) {
                    autoGuard_.NoteFailure(autoRequests_[i].pid, autoRequests_[i].createTime, hwnd);
                }
                autoRequests_.erase(autoRequests_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    };

    for (BatchItem& item : batch->items) {
        noteAuto(item.hwnd, item.result.ok);
        if (!item.result.ok) {
            ++failed;
            if (!failedText.empty()) {
                failedText += L", ";
            }
            failedText += item.title + L" (" +
                          (item.result.error.empty() ? L"failed" : item.result.error) + L")";
            continue;
        }
        // The window may have been tracked through another path while the worker
        // was busy; never add a duplicate session.  The extra agent instance
        // still has to be uninstalled, otherwise its window would stay hidden.
        if (sessions_.IndexOf(item.hwnd) >= 0) {
            item.result.session.ipc.Send(gf::Cmd::Uninstall, 0);
            item.result.session.ipc.SignalStop();
            continue;
        }
        sessions_.Add(std::move(item.result.session));
        ++attached;
        lastIndex = static_cast<int>(sessions_.Count()) - 1;
    }

    const int requested = static_cast<int>(batch->items.size());
    const bool cancelled = batch->cancelled;
    delete batch;
    autoRequests_.clear();  // drop requests that never entered the batch

    // One UI update for the whole batch: list, tray, counter and status.
    if (attached > 0) {
        selected_ = lastIndex;
        if (tab_ == Tab::Windows) {
            SelectTab(Tab::Targets);
        } else {
            BuildTargetRows();
        }
        targetsList_.Select(selected_);
        UpdateTray();
    }
    InvalidateAll();

    wchar_t status[640] = {};
    if (cancelled) {
        swprintf_s(status, L"Attach cancelled — %d attached", attached);
        SetStatus(status, attached == 0);
    } else if (failed == 0) {
        swprintf_s(status, L"Attached %d/%d", attached, requested);
        SetStatus(status);
    } else {
        std::wstring text;
        wchar_t head[64] = {};
        swprintf_s(head, L"Attached %d/%d — failed: ", attached, requested);
        text = std::wstring(head) + failedText;
        SetStatus(text, true);
        ShowToast(L"Some windows could not be attached", true);
    }
}

void App::CancelAttach() {
    if (attachThread_ == nullptr) {
        return;
    }
    ::InterlockedExchange(&attachCancel_, 1);
    SetStatus(L"Cancelling — finishing the current target…");
}

void App::UpdateAttachButton() {
    // The attach action is painted into the page header; nothing to update on a
    // child control, only the painted label.
    InvalidateAll();
}

void App::JoinAttachWorker() {
    if (attachThread_ == nullptr) {
        return;
    }
    if (::WaitForSingleObject(attachThread_, kAttachJoinTimeoutMs) == WAIT_OBJECT_0) {
        ::CloseHandle(attachThread_);
        attachThread_ = nullptr;
        return;
    }
    gf::LogLine(L"attach worker did not stop within %u ms", kAttachJoinTimeoutMs);
}

void App::DrainAttachResults() {
    MSG message = {};
    while (::PeekMessageW(&message, hwnd_, kAttachDoneMessage, kAttachDoneMessage, PM_REMOVE)) {
        auto* batch = reinterpret_cast<BatchCompletion*>(message.lParam);
        if (batch == nullptr) {
            continue;
        }
        // Completed after the last command was processed: undo every attach
        // before dropping our handles.
        for (BatchItem& item : batch->items) {
            if (item.result.ok) {
                item.result.session.ipc.Send(gf::Cmd::Uninstall, 0);
                item.result.session.ipc.SignalStop();
            }
        }
        delete batch;
    }
}

gf::SendOutcome App::SendCommand(int index, gf::Cmd cmd, std::wstring& error) {
    const gf::SendOutcome outcome = sessions_.Send(static_cast<size_t>(index), cmd, error);
    if (outcome == gf::SendOutcome::Timeout) {
        pendingCommand_ = PendingCommand{};
        pendingCommand_.hwnd = sessions_.At(static_cast<size_t>(index)).hwnd;
        pendingCommand_.cmd = cmd;
        pendingCommand_.nextTry = ::GetTickCount64() + kSendRetryDelayMs;
        pendingCommand_.attempts = 1;
        SetStatus(L"Busy — the target is not responding, retrying…", true);
    }
    return outcome;
}

void App::RetryPendingCommand() {
    if (pendingCommand_.hwnd == nullptr) {
        return;
    }
    if (::GetTickCount64() < pendingCommand_.nextTry) {
        return;
    }

    const int index = sessions_.IndexOf(pendingCommand_.hwnd);
    if (index < 0) {
        pendingCommand_ = PendingCommand{};
        return;
    }

    std::wstring error;
    const gf::SendOutcome outcome =
        sessions_.Send(static_cast<size_t>(index), pendingCommand_.cmd, error);
    if (outcome == gf::SendOutcome::Ok) {
        const gf::Cmd cmd = pendingCommand_.cmd;
        pendingCommand_ = PendingCommand{};
        SetStatus(cmd == gf::Cmd::Hide ? L"Stealth engaged — window excluded from capture"
                                       : L"Capture exclusion lifted");
        InvalidateAll();
        return;
    }
    if (outcome == gf::SendOutcome::Error) {
        pendingCommand_ = PendingCommand{};
        SetStatus(L"Error: " + error, true);
        return;
    }
    if (++pendingCommand_.attempts >= kMaxSendAttempts) {
        pendingCommand_ = PendingCommand{};
        SetStatus(L"The target did not acknowledge the command", true);
        return;
    }
    pendingCommand_.nextTry = ::GetTickCount64() + kSendRetryDelayMs;
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

void App::OpenLogFolder() {
    const std::wstring& dir = gf::LogDirectory();
    if (dir.empty()) {
        SetStatus(L"The log is not available", true);
        return;
    }
    ::ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::SetStatus(std::wstring text, bool error) {
    status_ = std::move(text);
    statusError_ = error;
    gf::LogLine(L"status: %s", status_.c_str());
    InvalidateAll();
}

void App::ShowToast(std::wstring text, bool error) {
    toast_ = std::move(text);
    toastError_ = error;
    toastUntil_ = ::GetTickCount64() + kToastDurationMs;
    InvalidateAll();
}

void App::ShowError(const std::wstring& text) {
    gf::LogLine(L"error: %s", text.c_str());
    ::MessageBoxW(nullptr, text.c_str(), L"GhostFrame", MB_ICONERROR | MB_OK);
}
