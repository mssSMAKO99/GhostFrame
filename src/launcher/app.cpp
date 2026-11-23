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
constexpr int kMinWidth = 560;
constexpr int kMinHeight = 440;
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
    const int width = MulDiv(760, dpi, 96);
    const int height = MulDiv(560, dpi, 96);

    POINT cursor = {};
    ::GetCursorPos(&cursor);
    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    const RECT& work = monitorInfo.rcWork;
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + ((work.bottom - work.top) - height) / 2;

    // Restore the saved geometry when it still fits on a monitor; otherwise fall
    // back to a centred window so a disconnected display cannot hide the app.
    if (settings_.ui.width >= 400 && settings_.ui.height >= 300) {
        RECT saved = {settings_.ui.x, settings_.ui.y, settings_.ui.x + settings_.ui.width,
                      settings_.ui.y + settings_.ui.height};
        const HMONITOR target = ::MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
        if (target != nullptr) {
            x = settings_.ui.x;
            y = settings_.ui.y;
        }
    }
    const int windowWidth = settings_.ui.width >= 400 ? settings_.ui.width : width;
    const int windowHeight = settings_.ui.height >= 300 ? settings_.ui.height : height;

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
            ::SetBkColor(dc, ui::kPanel);
            return reinterpret_cast<LRESULT>(editBrush_ != nullptr ? editBrush_
                                                                   : ::GetStockObject(BLACK_BRUSH));
        }

        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            bool active = false;
            if (SelectedValid() && item->CtlID == ID_BTN_TOGGLE) {
                active = sessions_.At(static_cast<size_t>(selected_)).hidden();
            } else if (item->CtlID == ID_BTN_SET_HIDE_ATTACH) {
                active = settings_.ui.hideOnAttach;
            } else if (item->CtlID == ID_BTN_SET_TRAY) {
                active = settings_.ui.minimizeToTray;
            } else if (item->CtlID == ID_BTN_SET_SELF) {
                active = !settings_.ui.noSelfExclude;
            }
            const bool hovered = hoveredButton_ != 0 && static_cast<int>(item->CtlID) == hoveredButton_;
            ui::DrawButton(item, theme_, active, item->CtlID == ID_BTN_CLOSE, hovered);
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
                return HTCAPTION;
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
    theme_.Create(::GetDpiForWindow(hwnd_));
    editBrush_ = ::CreateSolidBrush(ui::kPanel);
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
        SetStatus(L"SETTINGS.JSON WAS CORRUPT — RENAMED TO .BAD, DEFAULTS RESTORED", true);
    } else {
        SetStatus(L"READY — PICK A WINDOW ON THE WINDOWS TAB, OR FOCUS ONE AND PRESS CTRL+SHIFT+H");
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
        {ID_BTN_HIDE_UI, L"_", L"Hide the window to the tray"},
        {ID_BTN_EXIT, L"X", L"Exit GhostFrame and restore every target"},
        {ID_BTN_TOGGLE, L"TOGGLE STEALTH", L"Toggle capture exclusion for the selection"},
        {ID_BTN_RESTORE_ALL, L"RESTORE ALL", L"Restore every tracked window"},
        {ID_BTN_MIN, L"MINIMIZE", L"Minimize the selected targets"},
        {ID_BTN_RESTORE, L"RESTORE", L"Restore the selected targets"},
        {ID_BTN_CLOSE, L"CLOSE", L"Ask the selected targets to close"},
        {ID_BTN_REFRESH, L"REFRESH", L"Refresh the window list (F5)"},
        {ID_BTN_ATTACH, L"ATTACH SELECTED", L"Attach the selected windows"},
        {ID_BTN_SET_RESET, L"RESET HOTKEYS", L"Restore the default hotkeys"},
        {ID_BTN_SET_ADD_FG, L"ADD FOREGROUND APP", L"Auto-capture the current foreground app"},
        {ID_BTN_SET_BROWSE, L"BROWSE EXE…", L"Pick an executable to auto-capture"},
        {ID_BTN_SET_ENABLE, L"ENABLE / DISABLE", L"Enable or disable the selected rule"},
        {ID_BTN_SET_HIDE, L"TOGGLE HIDE", L"Hide the rule's windows on attach"},
        {ID_BTN_SET_REMOVE, L"REMOVE RULE", L"Remove the selected rule"},
        {ID_BTN_SET_APPLY, L"APPLY NOW", L"Run the rules against the windows open right now"},
        {ID_BTN_SET_HIDE_ATTACH, L"HIDE ON ATTACH", L"Hide a target right after attaching it"},
        {ID_BTN_SET_TRAY, L"MINIMIZE TO TRAY", L"Minimize to the tray instead of closing"},
        {ID_BTN_SET_SELF, L"EXCLUDE SELF", L"Keep GhostFrame itself out of capture"},
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

    ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_SEARCH)), instance_,
                      nullptr);
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
        ::SendMessageW(search, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(L"filter by title or process…"));
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
        {ID_BTN_HIDE_UI, L"Hide to tray  ·  Ctrl+Shift+M"},
        {ID_BTN_EXIT, L"Exit GhostFrame"},
        {ID_BTN_TOGGLE, L"Toggle stealth  ·  Ctrl+Shift+H"},
        {ID_BTN_RESTORE_ALL, L"Restore all  ·  Ctrl+Shift+R"},
        {ID_BTN_MIN, L"Minimize target  ·  Ctrl+Shift+Down"},
        {ID_BTN_RESTORE, L"Restore target  ·  Ctrl+Shift+Up"},
        {ID_BTN_CLOSE, L"Close target  ·  Ctrl+Shift+Q"},
        {ID_BTN_REFRESH, L"Refresh (F5)"},
        {ID_BTN_ATTACH, L"Attach the selected windows (Enter)"},
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
    const int pad = theme_.S(theme_.m.pad);

    int cursor = theme_.S(theme_.m.titleHeight);

    const int tabTop = cursor + theme_.S(6);
    const int tabH = theme_.S(theme_.m.tabHeight);
    for (int i = 0; i < 3; ++i) {
        const int left = theme_.S(theme_.m.pad) + i * theme_.S(theme_.m.tabStep);
        info.tabs[i] = {left, tabTop, left + theme_.S(theme_.m.tabWidth), tabTop + tabH};
    }
    cursor = tabTop + tabH + theme_.S(6);

    const int statusH = theme_.S(theme_.m.statusHeight);
    info.statusBar = {pad, cursor, width - pad, cursor + statusH};
    cursor = info.statusBar.bottom + theme_.S(6);

    const int headerH = theme_.S(theme_.m.headerHeight);
    info.header = {pad, cursor, width - pad, cursor + headerH};
    cursor = info.header.bottom + theme_.S(8);

    const int footerH = theme_.S(theme_.m.footerHeight);
    const int footerTop = client.bottom - pad - footerH;
    info.footer = {pad, footerTop, width - pad, footerTop + footerH};
    info.logLink = {width - pad - theme_.S(96), footerTop, width - pad, footerTop + footerH};
    const int contentBottom = footerTop - theme_.S(8);

    if (tab_ == Tab::Targets) {
        const int controlsH = theme_.S(theme_.m.buttonHeight) + theme_.S(8) + theme_.S(28);
        info.controls = {pad, contentBottom - controlsH, width - pad, contentBottom};
        info.list = {pad, cursor, width - pad, info.controls.top - theme_.S(10)};
    } else if (tab_ == Tab::Windows) {
        info.controls = {};
        info.list = {pad, cursor, width - pad, contentBottom};
    } else {
        const int listH = theme_.S(150);
        info.list = {pad, cursor, width - pad, cursor + listH};
        int y = info.list.bottom + theme_.S(8);

        info.settingsRowA = {pad, y, width - pad, y + theme_.S(28)};
        y += theme_.S(28) + theme_.S(14);

        info.rulesHeader = {pad, y, width - pad, y + theme_.S(18)};
        y += theme_.S(18) + theme_.S(4);

        const int bottomBlock = theme_.S(28) + theme_.S(8) + theme_.S(28) + theme_.S(12) +
                                theme_.S(30);
        info.rulesList = {pad, y, width - pad, contentBottom - bottomBlock - theme_.S(8)};
        y = info.rulesList.bottom + theme_.S(8);

        info.settingsRowB = {pad, y, width - pad, y + theme_.S(28)};
        y += theme_.S(28) + theme_.S(8);
        info.settingsRowC = {pad, y, width - pad, y + theme_.S(28)};
        y += theme_.S(28) + theme_.S(12);
        info.settingsRowD = {pad, y, width - pad, y + theme_.S(30)};
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
    auto place = [this, &placeRect](int id, int x, int y, int w, int h) {
        placeRect(id, RECT{theme_.S(x), theme_.S(y), theme_.S(x + w), theme_.S(y + h)});
    };

    RECT full = {};
    ::GetClientRect(hwnd_, &full);
    const int width = MulDiv(full.right, 96, static_cast<int>(theme_.dpi));

    place(ID_BTN_HIDE_UI, width - 78, 8, 30, 24);
    place(ID_BTN_EXIT, width - 44, 8, 30, 24);

    // TARGETS controls (shown only while that tab is active).
    if (tab_ == Tab::Targets) {
        const int controlWidth = (width - 32 - 16) / 2;
        const int controlTop = MulDiv(layout_.controls.top, 96, static_cast<int>(theme_.dpi));
        place(ID_BTN_TOGGLE, 16, controlTop, controlWidth, 30);
        place(ID_BTN_RESTORE_ALL, 16 + controlWidth + 16, controlTop, controlWidth, 30);
        const int rowTop = controlTop + 38;
        const int third = (width - 32 - 2 * 12) / 3;
        place(ID_BTN_MIN, 16, rowTop, third, 28);
        place(ID_BTN_RESTORE, 16 + third + 12, rowTop, third, 28);
        place(ID_BTN_CLOSE, 16 + 2 * (third + 12), rowTop, third, 28);
    }

    // WINDOWS-tab header actions.
    const int headerTop = MulDiv(layout_.header.top, 96, static_cast<int>(theme_.dpi));
    place(ID_BTN_ATTACH, width - 16 - 190, headerTop, 190, 22);
    place(ID_BTN_REFRESH, width - 16 - 190 - 8 - 90, headerTop, 90, 22);
    const int tabStep = theme_.m.tabStep;
    place(ID_EDIT_SEARCH, theme_.m.pad + tabStep * 3 + 20, theme_.m.tabTop,
          width - (theme_.m.pad + tabStep * 3 + 20) - 16, 24);

    // SETTINGS tab.
    auto placeRow = [&](int id, const RECT& rect, int slot, int slots) {
        const int gap = theme_.S(10);
        const int totalWidth = rect.right - rect.left;
        const int slotWidth = (totalWidth - (slots - 1) * gap) / slots;
        RECT r = {rect.left + slot * (slotWidth + gap), rect.top,
                  rect.left + slot * (slotWidth + gap) + slotWidth, rect.bottom};
        placeRect(id, r);
    };

    if (layout_.settingsRowA.right > layout_.settingsRowA.left) {
        placeRow(ID_BTN_SET_RESET, layout_.settingsRowA, 0, 2);
        placeRow(ID_BTN_SET_ADD_FG, layout_.settingsRowA, 1, 2);
        placeRect(ID_BTN_SET_BROWSE, layout_.settingsRowB);
        placeRow(ID_BTN_SET_ENABLE, layout_.settingsRowC, 0, 4);
        placeRow(ID_BTN_SET_HIDE, layout_.settingsRowC, 1, 4);
        placeRow(ID_BTN_SET_REMOVE, layout_.settingsRowC, 2, 4);
        placeRow(ID_BTN_SET_APPLY, layout_.settingsRowC, 3, 4);
        placeRow(ID_BTN_SET_HIDE_ATTACH, layout_.settingsRowD, 0, 3);
        placeRow(ID_BTN_SET_TRAY, layout_.settingsRowD, 1, 3);
        placeRow(ID_BTN_SET_SELF, layout_.settingsRowD, 2, 3);
    }

    targetsList_.SetBounds(layout_.list, theme_);
    windowsList_.SetBounds(layout_.list, theme_);
    hotkeysList_.SetBounds(layout_.list, theme_);
    rulesList_.SetBounds(layout_.rulesList, theme_);
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
        SetStatus(L"HOTKEY CONFLICT — ALREADY IN USE: " + failed, true);
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

    const bool focused = ::GetFocus() == hwnd_;

    // Title bar.
    const RECT titleBar = {0, 0, client.right, theme_.S(theme_.m.titleHeight)};
    ui::FillSolid(dc, titleBar, ui::kPanel);
    const RECT titleLine = {0, titleBar.bottom - 1, client.right, titleBar.bottom};
    ui::FillSolid(dc, titleLine, ui::kNeonDim);

    RECT titleRect = {theme_.S(theme_.m.pad), 0, client.right - theme_.S(150), titleBar.bottom};
    ui::DrawLabel(dc, L"// GHOSTFRAME", titleRect, theme_.title, ui::kNeon,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT versionRect = {client.right - theme_.S(140), 0, client.right - theme_.S(84),
                        titleBar.bottom};
    ui::DrawLabel(dc, L"v" GF_VERSION_STRING, versionRect, theme_.small, ui::kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // Tabs.
    static const wchar_t* const kTabNames[] = {L"TARGETS", L"WINDOWS", L"SETTINGS"};
    const int activeTab = tab_ == Tab::Targets ? 0 : (tab_ == Tab::Windows ? 1 : 2);
    for (int index = 0; index < 3; ++index) {
        const bool active = activeTab == index;
        const RECT tab = layout_.tabs[index];
        ui::FillRoundRect(dc, tab, active ? ui::kPanelHi : ui::kPanel, theme_.S(6));
        ui::StrokeRoundFrame(dc, tab, active ? ui::kNeon : ui::kBorder, theme_.S(6), 1);
        ui::DrawLabel(dc, kTabNames[index], tab, theme_.small, active ? ui::kNeon : ui::kMuted,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Status bar: the transient status on the left, the live summary on the
    // right.
    wchar_t summary[96] = {};
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
    swprintf_s(summary, L"%zu tracked · %d hidden · %d error", sessions_.Count(), hidden, errored);

    const SIZE summarySize = ui::MeasureTextSize(dc, theme_.small, summary);
    RECT summaryRect = {layout_.statusBar.right - summarySize.cx, layout_.statusBar.top,
                        layout_.statusBar.right, layout_.statusBar.bottom};
    ui::DrawLabel(dc, summary, summaryRect, theme_.small, errored > 0 ? ui::kWarn : ui::kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT statusRect = layout_.statusBar;
    statusRect.right = summaryRect.left - theme_.S(12);
    ui::DrawLabel(dc, status_.c_str(), statusRect, theme_.body,
                  statusError_ ? ui::kWarn : ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Per-tab header.
    if (tab_ == Tab::Targets) {
        ui::DrawLabel(dc, L"TRACKED TARGETS", layout_.header, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else if (tab_ == Tab::Windows) {
        wchar_t header[96] = {};
        swprintf_s(header, L"AVAILABLE WINDOWS — %d", windowsList_.SelectableCount());
        ui::DrawLabel(dc, header, layout_.header, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        ui::DrawLabel(dc, L"GLOBAL HOTKEYS — CLICK A ROW TO REBIND", layout_.header, theme_.small,
                      ui::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (layout_.rulesHeader.right > layout_.rulesHeader.left) {
            ui::DrawLabel(dc, L"AUTO-CAPTURE RULES — CAPTURE THESE APPS WHEN THEY APPEAR",
                          layout_.rulesHeader, theme_.small, ui::kMuted,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    if (tab_ == Tab::Targets) {
        targetsList_.Paint(dc, theme_, focused);
    } else if (tab_ == Tab::Windows) {
        windowsList_.Paint(dc, theme_, focused);
    } else {
        hotkeysList_.Paint(dc, theme_, focused);
        rulesList_.Paint(dc, theme_, false);
    }

    // Footer.
    ui::DrawLabel(dc, L"WDA_EXCLUDEFROMCAPTURE · PER-MONITOR DPI · NO ADMIN REQUIRED",
                  layout_.footer, theme_.small, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT logRect = layout_.logLink;
    ui::DrawLabel(dc, L"OPEN LOG", logRect, theme_.small, ui::kNeon,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Transient toast.
    if (!toast_.empty() && ::GetTickCount64() < toastUntil_) {
        const SIZE size = ui::MeasureTextSize(dc, theme_.body, toast_);
        const int width = size.cx + theme_.S(32);
        const int height = theme_.S(34);
        const int left = (client.right - width) / 2;
        RECT toast = {left, titleBar.bottom + theme_.S(10), left + width,
                      titleBar.bottom + theme_.S(10) + height};
        ui::FillRoundRect(dc, toast, ui::kToastBg, theme_.S(8));
        ui::StrokeRoundFrame(dc, toast, toastError_ ? ui::kWarn : ui::kNeon, theme_.S(8), 1);
        ui::DrawLabel(dc, toast_.c_str(), toast, theme_.body,
                      toastError_ ? ui::kWarn : ui::kText,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
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
    const bool targets = tab_ == Tab::Targets;
    const bool windows = tab_ == Tab::Windows;
    const bool settings = tab_ == Tab::Settings;

    for (int id : {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_MIN, ID_BTN_RESTORE, ID_BTN_CLOSE}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, targets ? SW_SHOW : SW_HIDE);
        }
    }

    for (int id : {ID_BTN_REFRESH, ID_BTN_ATTACH}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, windows ? SW_SHOW : SW_HIDE);
        }
    }
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::ShowWindow(search, windows ? SW_SHOW : SW_HIDE);
    }

    const int settingsVisibility = settings ? SW_SHOW : SW_HIDE;
    for (int id : {ID_BTN_SET_RESET, ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE, ID_BTN_SET_ENABLE,
                   ID_BTN_SET_HIDE, ID_BTN_SET_REMOVE, ID_BTN_SET_APPLY, ID_BTN_SET_HIDE_ATTACH,
                   ID_BTN_SET_TRAY, ID_BTN_SET_SELF}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, settingsVisibility);
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
        SetStatus(L"PICK A WINDOW — DOUBLE-CLICK OR SELECT AND PRESS ATTACH");
    } else {
        ::KillTimer(hwnd_, kWindowsTimer);
        if (tab_ == Tab::Settings) {
            BuildSettingsRows();
            SetStatus(L"SETTINGS — CLICK A HOTKEY TO REBIND; ESC CANCELS");
        } else {
            BuildTargetRows();
        }
    }

    settings_.ui.lastTab = tab_ == Tab::Windows ? L"windows"
                           : tab_ == Tab::Settings ? L"settings"
                                                   : L"targets";
    MarkSettingsDirty();
    InvalidateAll();
}

void App::OnSize() {
    Layout();
    InvalidateAll();
}

void App::OnSettingChange() {
    theme_.Create(::GetDpiForWindow(hwnd_));
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
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
    if (tab_ == Tab::Windows) {
        changed |= windowsList_.OnMouseMove(x, y, true);
        changed |= targetsList_.ClearHover();
        changed |= hotkeysList_.ClearHover();
        changed |= rulesList_.ClearHover();
    } else if (tab_ == Tab::Settings) {
        changed |= hotkeysList_.OnMouseMove(x, y, true);
        changed |= rulesList_.OnMouseMove(x, y, true);
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
    if (a || b || c || d) {
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
        if (hotkeysList_.Contains(point.x, point.y)) {
            list = &hotkeysList_;
        } else if (rulesList_.Contains(point.x, point.y)) {
            list = &rulesList_;
        }
    } else {
        list = &targetsList_;
    }
    if (list != nullptr && list->Contains(point.x, point.y)) {
        list->OnMouseWheel(delta);
        InvalidateAll();
    }
}

void App::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    // The clickable "open log" affordance in the footer.
    {
        POINT point = {x, y};
        if (::PtInRect(&layout_.logLink, point)) {
            OpenLogFolder();
            return;
        }
    }

    // Tab switching.
    for (int index = 0; index < 3; ++index) {
        const RECT tab = TabRect(index);
        if (::PtInRect(&tab, POINT{x, y})) {
            SelectTab(index == 0 ? Tab::Targets : (index == 1 ? Tab::Windows : Tab::Settings));
            return;
        }
    }

    if (tab_ == Tab::Settings) {
        if (hotkeysList_.Contains(x, y)) {
            OnSettingsListAction(hotkeysList_.OnLButtonDown(x, y, ctrl, shift), true);
        } else if (rulesList_.Contains(x, y)) {
            OnSettingsListAction(rulesList_.OnLButtonDown(x, y, ctrl, shift), false);
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
        if (hotkeysList_.Contains(x, y)) {
            OnSettingsListAction(hotkeysList_.OnLButtonDblClk(x, y), true);
        } else if (rulesList_.Contains(x, y)) {
            // Double-click a rule to flip its enabled state.
            const int row = rulesList_.IndexAt(x, y);
            if (row >= 0 && row < static_cast<int>(settings_.autoAttach.size())) {
                selectedRule_ = row;
                settings_.autoAttach[static_cast<size_t>(row)].enabled =
                    !settings_.autoAttach[static_cast<size_t>(row)].enabled;
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
            SetStatus(L"HOTKEY REBIND CANCELLED");
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
            SetStatus(L"WINDOW LIST REFRESHED");
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
        SelectTab(virtualKey == VK_LEFT ? Tab::Targets : Tab::Windows);
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
        if (virtualKey == VK_DELETE && selectedRule_ >= 0 &&
            selectedRule_ < static_cast<int>(settings_.autoAttach.size())) {
            settings_.autoAttach.erase(settings_.autoAttach.begin() + selectedRule_);
            selectedRule_ = -1;
            MarkSettingsDirty();
            BuildSettingsRows();
            SetStatus(L"AUTO-CAPTURE RULE REMOVED");
            InvalidateAll();
            return;
        }
        ui::ListWidget& list = rulesList_.Selected() >= 0 ? rulesList_ : hotkeysList_;
        const ui::ListWidget::Action action = list.OnKeyDown(virtualKey);
        if (&list == &hotkeysList_ &&
            (action.kind == ui::ListWidget::ActionKind::Activate ||
             action.kind == ui::ListWidget::ActionKind::Toggle)) {
            BeginRebind(static_cast<gf::HotkeyAction>(action.index));
            return;
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
                SetStatus(L"WINDOW LIST REFRESHED");
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
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
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
        SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
        return;
    }

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

    std::wstring reason;
    if (!gf::ValidateTarget(foreground, reason)) {
        SetStatus(L"ATTACH FAILED: " + reason, true);
        return;
    }
    if (sessions_.IndexOf(foreground) >= 0) {
        SetStatus(L"THE WINDOW IS ALREADY TRACKED", true);
        return;
    }

    StartAttachBatch({foreground}, hideAfter);
}

void App::StartAttachBatch(std::vector<HWND> targets, bool hideAfter) {
    if (attachThread_ != nullptr) {
        SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
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
        SetStatus(L"NOTHING TO ATTACH", true);
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
        SetStatus(L"ATTACH FAILED: cannot start the attach worker", true);
        return;
    }

    UpdateAttachButton();
    SetStatus(L"ATTACHING…");
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
    swprintf_s(status, L"ATTACHING %zu/%d — %s", static_cast<size_t>(done), attachTotal_,
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
        swprintf_s(status, L"ATTACH CANCELLED — %d ATTACHED", attached);
        SetStatus(status, attached == 0);
    } else if (failed == 0) {
        swprintf_s(status, L"ATTACHED %d/%d", attached, requested);
        SetStatus(status);
    } else {
        std::wstring text;
        wchar_t head[64] = {};
        swprintf_s(head, L"ATTACHED %d/%d — FAILED: ", attached, requested);
        text = std::wstring(head) + failedText;
        SetStatus(text, true);
        ShowToast(L"SOME WINDOWS COULD NOT BE ATTACHED", true);
    }
}

void App::CancelAttach() {
    if (attachThread_ == nullptr) {
        return;
    }
    ::InterlockedExchange(&attachCancel_, 1);
    SetStatus(L"CANCELLING — FINISHING THE CURRENT TARGET…");
}

void App::UpdateAttachButton() {
    HWND button = ::GetDlgItem(hwnd_, ID_BTN_ATTACH);
    if (button == nullptr) {
        return;
    }
    wchar_t text[96] = {};
    if (attachThread_ != nullptr) {
        wcscpy_s(text, L"STOP");
    } else {
        const int selected = windowsList_.SelectedCount();
        if (selected > 0) {
            swprintf_s(text, L"ATTACH SELECTED (%d)", selected);
        } else {
            wcscpy_s(text, L"ATTACH SELECTED");
        }
    }
    ::SetWindowTextW(button, text);
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
        SetStatus(L"BUSY — THE TARGET IS NOT RESPONDING, RETRYING…", true);
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
        SetStatus(cmd == gf::Cmd::Hide ? L"STEALTH ENGAGED — WINDOW EXCLUDED FROM CAPTURE"
                                       : L"CAPTURE EXCLUSION LIFTED");
        InvalidateAll();
        return;
    }
    if (outcome == gf::SendOutcome::Error) {
        pendingCommand_ = PendingCommand{};
        SetStatus(L"ERROR: " + error, true);
        return;
    }
    if (++pendingCommand_.attempts >= kMaxSendAttempts) {
        pendingCommand_ = PendingCommand{};
        SetStatus(L"ERROR: THE TARGET DID NOT ACKNOWLEDGE THE COMMAND", true);
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
        SetStatus(L"THE LOG IS NOT AVAILABLE", true);
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
