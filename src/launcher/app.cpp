#include "app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

#include "ghostframe/version.h"
#include "log.h"
#include "resource.h"

namespace {

constexpr wchar_t kWindowClass[] = L"GhostFrame.MainWindow";
constexpr int kBaseWidth = 760;
constexpr int kBaseHeight = 560;
constexpr int kTitleHeight = 36;
constexpr int kTabsLeft = 16;
constexpr int kTabsTop = 42;
constexpr int kTabWidth = 118;
constexpr int kTabStep = 124;
constexpr int kTabHeight = 26;
constexpr int kStatusTop = 74;
constexpr int kHeaderTop = 98;
constexpr int kListTop = 122;
constexpr int kListBottom = 400;
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

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
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
    const int width = MulDiv(kBaseWidth, dpi, 96);
    const int height = MulDiv(kBaseHeight, dpi, 96);

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
                              WS_POPUP | WS_CLIPCHILDREN, x, y, windowWidth, windowHeight, nullptr,
                              nullptr, instance, this);
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

        case WM_ERASEBKGND:
            return 1;

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
            ui::DrawButton(item, theme_, active, item->CtlID == ID_BTN_CLOSE);
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
            OnClose();
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
    };
    const ButtonSpec buttons[] = {
        {ID_BTN_HIDE_UI, L"_"},
        {ID_BTN_EXIT, L"X"},
        {ID_BTN_TOGGLE, L"[CTRL+SHIFT+H]  TOGGLE STEALTH"},
        {ID_BTN_RESTORE_ALL, L"[CTRL+SHIFT+R]  RESTORE ALL TARGETS"},
        {ID_BTN_MIN, L"[DOWN] MINIMIZE"},
        {ID_BTN_RESTORE, L"[UP] RESTORE"},
        {ID_BTN_CLOSE, L"[Q] CLOSE TARGET"},
        {ID_BTN_REFRESH, L"REFRESH (F5)"},
        {ID_BTN_ATTACH, L"ATTACH SELECTED"},
        {ID_BTN_SET_RESET, L"RESET HOTKEYS"},
        {ID_BTN_SET_ADD_FG, L"ADD FOREGROUND APP"},
        {ID_BTN_SET_BROWSE, L"BROWSE EXE…"},
        {ID_BTN_SET_ENABLE, L"ENABLE / DISABLE"},
        {ID_BTN_SET_HIDE, L"TOGGLE HIDE"},
        {ID_BTN_SET_REMOVE, L"REMOVE RULE"},
        {ID_BTN_SET_APPLY, L"APPLY NOW"},
        {ID_BTN_SET_HIDE_ATTACH, L"HIDE ON ATTACH"},
        {ID_BTN_SET_TRAY, L"MINIMIZE TO TRAY"},
        {ID_BTN_SET_SELF, L"EXCLUDE SELF FROM CAPTURE"},
    };

    for (const ButtonSpec& spec : buttons) {
        ::CreateWindowExW(0, L"BUTTON", spec.text, WS_CHILD | BS_OWNERDRAW,
                          0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)),
                          instance_, nullptr);
    }

    ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT_SEARCH)), instance_,
                      nullptr);
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
        ::SendMessageW(search, EM_SETCUEBANNER, TRUE,
                       reinterpret_cast<LPARAM>(L"filter by title or process…"));
    }
}

void App::Layout() {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    const int width = MulDiv(client.right, 96, static_cast<int>(theme_.dpi));

    auto place = [this](int id, int x, int y, int w, int h) {
        HWND child = ::GetDlgItem(hwnd_, id);
        if (child != nullptr) {
            ::SetWindowPos(child, nullptr, theme_.S(x), theme_.S(y), theme_.S(w), theme_.S(h),
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    place(ID_BTN_HIDE_UI, width - 78, 7, 30, 22);
    place(ID_BTN_EXIT, width - 44, 7, 30, 22);

    const int controlWidth = (width - 32 - 16) / 2;
    const int controlTop = kListBottom + 8;
    place(ID_BTN_TOGGLE, 16, controlTop, controlWidth, 30);
    place(ID_BTN_RESTORE_ALL, 16 + controlWidth + 16, controlTop, controlWidth, 30);

    const int rowTop = controlTop + 38;
    const int third = (width - 32 - 2 * 12) / 3;
    place(ID_BTN_MIN, 16, rowTop, third, 28);
    place(ID_BTN_RESTORE, 16 + third + 12, rowTop, third, 28);
    place(ID_BTN_CLOSE, 16 + 2 * (third + 12), rowTop, third, 28);

    // WINDOWS-tab header actions; visibility is toggled with the tab.
    place(ID_BTN_ATTACH, width - 16 - 190, kHeaderTop, 190, 22);
    place(ID_BTN_REFRESH, width - 16 - 190 - 8 - 90, kHeaderTop, 90, 22);
    place(ID_EDIT_SEARCH, kTabsLeft + kTabStep * 3 + 20, kTabsTop,
          width - (kTabsLeft + kTabStep * 3 + 20) - 16, 24);

    // SETTINGS tab.  Two lists (hotkeys, auto-capture rules) with their action
    // rows and the three UI option toggles underneath.
    place(ID_BTN_SET_RESET, 16, 258, controlWidth, 26);
    place(ID_BTN_SET_ADD_FG, 16 + controlWidth + 16, 258, controlWidth, 26);

    const int ruleWidth = (width - 32 - 3 * 10) / 4;
    place(ID_BTN_SET_ENABLE, 16, 398, ruleWidth, 26);
    place(ID_BTN_SET_HIDE, 16 + (ruleWidth + 10), 398, ruleWidth, 26);
    place(ID_BTN_SET_REMOVE, 16 + 2 * (ruleWidth + 10), 398, ruleWidth, 26);
    place(ID_BTN_SET_APPLY, 16 + 3 * (ruleWidth + 10), 398, ruleWidth, 26);
    place(ID_BTN_SET_BROWSE, 16, 366, width - 32, 26);

    const int optionWidth = (width - 32 - 2 * 12) / 3;
    place(ID_BTN_SET_HIDE_ATTACH, 16, 434, optionWidth, 28);
    place(ID_BTN_SET_TRAY, 16 + optionWidth + 12, 434, optionWidth, 28);
    place(ID_BTN_SET_SELF, 16 + 2 * (optionWidth + 12), 434, optionWidth, 28);

    targetsList_.SetBounds(ListRect(), theme_);
    windowsList_.SetBounds(ListRect(), theme_);
    hotkeysList_.SetBounds(HotkeysListRect(), theme_);
    rulesList_.SetBounds(RulesListRect(), theme_);
}

RECT App::HotkeysListRect() const {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    RECT list = {};
    list.left = theme_.S(16);
    list.right = client.right - theme_.S(16);
    list.top = theme_.S(kListTop);
    list.bottom = theme_.S(250);
    return list;
}

RECT App::RulesListRect() const {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    RECT list = {};
    list.left = theme_.S(16);
    list.right = client.right - theme_.S(16);
    list.top = theme_.S(292);
    list.bottom = theme_.S(390);
    return list;
}

RECT App::TabRect(int index) const {
    RECT tab = {theme_.S(kTabsLeft + index * kTabStep), theme_.S(kTabsTop),
                theme_.S(kTabsLeft + index * kTabStep + kTabWidth), theme_.S(kTabsTop + kTabHeight)};
    return tab;
}

RECT App::ListRect() const {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    RECT list = {};
    list.left = theme_.S(16);
    list.right = client.right - theme_.S(16);
    list.top = theme_.S(kListTop);
    list.bottom = theme_.S(kListBottom);
    return list;
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

void App::ResetHotkeysToDefaults() {
    gf::Settings defaults = gf::DefaultSettings();
    std::wstring error;
    if (ApplyHotkeys(defaults, error)) {
        BuildSettingsRows();
        SetStatus(L"HOTKEYS RESET TO DEFAULTS");
    } else {
        SetStatus(L"ERROR: " + error, true);
    }
}

void App::BeginRebind(gf::HotkeyAction action) {
    rebinding_ = true;
    rebindAction_ = action;
    SetStatus(std::wstring(L"PRESS A COMBINATION FOR ") + gf::HotkeyActionLabel(action) +
              L" — ESC TO CANCEL");
    ::SetFocus(hwnd_);
    InvalidateAll();
}

void App::FinishRebind(UINT modifiers, UINT virtualKey) {
    const gf::HotkeyAction action = rebindAction_;
    rebinding_ = false;
    rebindAction_ = gf::HotkeyAction::Count;

    if (action == gf::HotkeyAction::Count) {
        return;
    }

    gf::Settings candidate = settings_;
    candidate.hotkeys[static_cast<int>(action)] = gf::HotkeyBinding{modifiers, virtualKey};

    std::wstring error;
    if (ApplyHotkeys(candidate, error)) {
        BuildSettingsRows();
        SetStatus(std::wstring(gf::HotkeyActionLabel(action)) + L" = " +
                  gf::FormatHotkey(modifiers, virtualKey));
    } else {
        SetStatus(L"ERROR: " + error, true);
    }
    InvalidateAll();
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

void App::ApplyAutoRulesNow() {
    if (attachThread_ != nullptr) {
        SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
        return;
    }
    autoLastScan_ = 0;  // bypass the throttle for an explicit request
    ScanAutoCapture();
    if (attachThread_ == nullptr) {
        SetStatus(L"AUTO-CAPTURE RULES APPLIED — NO NEW WINDOWS MATCHED");
    }
}

void App::AddRuleForWindow(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd) || hwnd == hwnd_) {
        SetStatus(L"NO FOREGROUND WINDOW TO ADD", true);
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        SetStatus(L"GHOSTFRAME CANNOT AUTO-CAPTURE ITSELF", true);
        return;
    }
    std::wstring path;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t buffer[1024] = {};
        DWORD size = _countof(buffer);
        if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            path.assign(buffer, size);
        }
        ::CloseHandle(process);
    }
    if (path.empty()) {
        SetStatus(L"CANNOT READ THE PROCESS IMAGE NAME", true);
        return;
    }

    AddRuleForExePath(path);
}

void App::AddRuleForExePath(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        SetStatus(L"THAT FILE HAS NO NAME", true);
        return;
    }

    for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
        if (_wcsicmp(rule.match.c_str(), L"exe") == 0 &&
            _wcsicmp(rule.value.c_str(), name.c_str()) == 0) {
            SetStatus(L"AN AUTO-CAPTURE RULE FOR " + name + L" ALREADY EXISTS", true);
            return;
        }
    }

    settings_.autoAttach.push_back(gf::AutoAttachRule{true, L"exe", name, true});
    MarkSettingsDirty();
    BuildSettingsRows();
    SetStatus(L"AUTO-CAPTURE RULE ADDED FOR " + name);
}

void App::BuildSettingsRows() {
    std::vector<ui::ListRow> hotkeys;
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        const gf::HotkeyAction action = static_cast<gf::HotkeyAction>(i);
        ui::ListRow row;
        row.title = gf::HotkeyActionLabel(action);
        row.meta = rebinding_ && rebindAction_ == action
                       ? L"press a combination…"
                       : gf::FormatHotkey(settings_.hotkeys[i].modifiers,
                                          settings_.hotkeys[i].virtualKey);
        row.data = i;
        hotkeys.push_back(std::move(row));
    }
    hotkeysList_.SetEmptyText(L"NO HOTKEYS");
    hotkeysList_.SetRows(std::move(hotkeys));

    std::vector<ui::ListRow> rules;
    for (size_t i = 0; i < settings_.autoAttach.size(); ++i) {
        const gf::AutoAttachRule& rule = settings_.autoAttach[i];
        ui::ListRow row;
        row.title = rule.value;
        row.meta = rule.match + (rule.hide ? L"  ·  hide on attach" : L"  ·  keep visible");
        row.data = static_cast<intptr_t>(i);
        row.badges.push_back({rule.enabled ? L"ENABLED" : L"DISABLED",
                              rule.enabled ? ui::kNeon : ui::kMuted});
        rules.push_back(std::move(row));
    }
    rulesList_.SetEmptyText(L"NO AUTO-CAPTURE RULES — ADD THE FOREGROUND APP");
    rulesList_.SetRows(std::move(rules));
    if (selectedRule_ >= static_cast<int>(settings_.autoAttach.size())) {
        selectedRule_ = settings_.autoAttach.empty() ? -1
                                                     : static_cast<int>(settings_.autoAttach.size()) - 1;
    }
}

void App::OnSettingsListAction(ui::ListWidget::Action action, bool hotkeyList) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }
    if (hotkeyList) {
        BeginRebind(static_cast<gf::HotkeyAction>(action.index));
        return;
    }
    selectedRule_ = action.index;
    rulesList_.Select(action.index);
    InvalidateAll();
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

    const int width = MulDiv(client.right, 96, static_cast<int>(theme_.dpi));

    const RECT titleBar = {0, 0, client.right, theme_.S(kTitleHeight)};
    ui::FillSolid(dc, titleBar, ui::kPanel);
    const RECT titleLine = {0, theme_.S(kTitleHeight) - 1, client.right, theme_.S(kTitleHeight)};
    ui::FillSolid(dc, titleLine, ui::kNeonDim);

    RECT titleRect = {theme_.S(16), 0, theme_.S(width - 150), theme_.S(kTitleHeight)};
    ui::DrawLabel(dc, L"// GHOSTFRAME", titleRect, theme_.title, ui::kNeon,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT versionRect = {theme_.S(width - 140), 0, theme_.S(width - 84),
                        theme_.S(kTitleHeight)};
    ui::DrawLabel(dc, L"v" GF_VERSION_STRING, versionRect, theme_.small, ui::kMuted,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // Tabs
    static const wchar_t* const kTabNames[] = {L"TARGETS", L"WINDOWS", L"SETTINGS"};
    const int activeTab = tab_ == Tab::Targets ? 0 : (tab_ == Tab::Windows ? 1 : 2);
    for (int index = 0; index < 3; ++index) {
        const bool active = activeTab == index;
        const RECT tab = TabRect(index);
        ui::FillSolid(dc, tab, active ? ui::kPanelHi : ui::kPanel);
        ui::StrokeFrame(dc, tab, active ? ui::kNeon : ui::kBorder, 1);
        ui::DrawLabel(dc, kTabNames[index], tab, theme_.small, active ? ui::kNeon : ui::kMuted,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    RECT statusRect = {theme_.S(16), theme_.S(kStatusTop), theme_.S(width - 16),
                       theme_.S(kHeaderTop - 4)};
    ui::DrawLabel(dc, status_.c_str(), statusRect, theme_.body,
                  statusError_ ? ui::kWarn : ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (tab_ == Tab::Targets) {
        RECT headerLeft = {theme_.S(16), theme_.S(kHeaderTop), theme_.S(220),
                           theme_.S(kHeaderTop + 20)};
        ui::DrawLabel(dc, L"TARGETS", headerLeft, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        wchar_t countText[64] = {};
        swprintf_s(countText, L"%zu TRACKED", sessions_.Count());
        RECT headerRight = {theme_.S(width - 220), theme_.S(kHeaderTop), theme_.S(width - 16),
                            theme_.S(kHeaderTop + 20)};
        ui::DrawLabel(dc, countText, headerRight, theme_.small, ui::kMuted,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    } else if (tab_ == Tab::Windows) {
        RECT headerLeft = {theme_.S(16), theme_.S(kHeaderTop), theme_.S(320),
                           theme_.S(kHeaderTop + 20)};
        wchar_t header[96] = {};
        swprintf_s(header, L"WINDOWS — %d AVAILABLE", windowsList_.SelectableCount());
        ui::DrawLabel(dc, header, headerLeft, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT headerLeft = {theme_.S(16), theme_.S(kHeaderTop), theme_.S(width - 16),
                           theme_.S(kHeaderTop + 20)};
        ui::DrawLabel(dc, L"GLOBAL HOTKEYS — CLICK A ROW TO REBIND", headerLeft, theme_.small,
                      ui::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT rulesHeader = {theme_.S(16), theme_.S(272), theme_.S(width - 16), theme_.S(290)};
        ui::DrawLabel(dc, L"AUTO-CAPTURE RULES — CAPTURE THESE APPS WHEN THEY APPEAR", rulesHeader,
                      theme_.small, ui::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (tab_ == Tab::Targets) {
        targetsList_.Paint(dc, theme_);
    } else if (tab_ == Tab::Windows) {
        windowsList_.Paint(dc, theme_);
    } else {
        hotkeysList_.Paint(dc, theme_);
        rulesList_.Paint(dc, theme_);
    }

    RECT footer = {theme_.S(16), theme_.S(kListBottom + 76), theme_.S(width - 16),
                   theme_.S(kListBottom + 98)};
    ui::DrawLabel(dc, L"WDA_EXCLUDEFROMCAPTURE · PER-MONITOR DPI · NO ADMIN REQUIRED", footer,
                  theme_.small, ui::kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::OnTimer() {
    if (settingsDirty_ && ::GetTickCount64() >= settingsSaveDue_) {
        SaveSettings();
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

void App::BuildTargetRows() {
    std::vector<ui::ListRow> rows;
    rows.reserve(sessions_.Count());

    for (size_t i = 0; i < sessions_.Count(); ++i) {
        const gf::Session& session = sessions_.At(i);
        ui::ListRow row;
        row.title = session.title;
        row.icon = windowEnumerator_.IconForWindow(session.hwnd);
        row.data = reinterpret_cast<intptr_t>(session.hwnd);

        wchar_t meta[96] = {};
        swprintf_s(meta, L"PID %u  %s", session.pid, session.x86 ? L"x86" : L"x64");
        row.meta = meta;

        row.badges.push_back({StateLabel(session), StateColor(session)});

        rows.push_back(std::move(row));
    }

    targetsList_.SetEmptyText(L"NO TARGETS — PICK A WINDOW ON THE WINDOWS TAB");
    targetsList_.SetRows(std::move(rows));
}

void App::BuildWindowRows() {
    const std::wstring filter = ToLower(windowFilter_);

    std::vector<ui::ListRow> rows;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        const bool tracked = sessions_.IndexOf(window.hwnd) >= 0;

        if (!filter.empty()) {
            const std::wstring title = ToLower(window.title);
            const std::wstring process = ToLower(window.processName);
            if (!ContainsNoCase(title, filter) && !ContainsNoCase(process, filter)) {
                continue;
            }
        }

        ui::ListRow row;
        row.title = window.title;
        row.icon = window.icon;
        row.data = reinterpret_cast<intptr_t>(window.hwnd);
        row.selectable = !tracked;
        row.dim = tracked;

        wchar_t meta[160] = {};
        swprintf_s(meta, L"%s  PID %u  %s", window.processName.c_str(), window.pid,
                   gf::WindowEnumerator::ArchLabel(window.arch));
        row.meta = meta;

        if (tracked) {
            row.badges.push_back({L"TRACKED", ui::kMuted});
        } else {
            bool autoMatch = false;
            for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
                if (gf::RuleMatches(rule, window.processName, window.title)) {
                    autoMatch = true;
                    break;
                }
            }
            if (autoMatch) {
                row.badges.push_back({L"AUTO", ui::kNeon});
            }
        }
        if (window.elevated) {
            row.badges.push_back({L"ADMIN", ui::kWarn});
        }
        if (window.uwp) {
            row.badges.push_back({L"UWP", ui::kWarn});
        }

        rows.push_back(std::move(row));
    }

    windowsList_.SetEmptyText(filter.empty() ? L"NO WINDOWS FOUND"
                                             : L"NO WINDOWS MATCH THE FILTER");
    windowsList_.SetRows(std::move(rows));
    UpdateAttachButton();
}

void App::ApplyTabControls() {
    const bool windows = tab_ == Tab::Windows;
    const bool settings = tab_ == Tab::Settings;

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

void App::RefreshWindows() {
    windowEnumerator_.Refresh(hwnd_);
    BuildWindowRows();
    InvalidateAll();
}

void App::OnFilterChanged() {
    wchar_t buffer[128] = {};
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::GetWindowTextW(search, buffer, _countof(buffer) - 1);
    }
    windowFilter_ = buffer;
    BuildWindowRows();
    InvalidateAll();
}

void App::ApplyListAction(ui::ListWidget::Action action, bool windowsTab) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }

    if (windowsTab) {
        if (action.kind == ui::ListWidget::ActionKind::Activate) {
            AttachSelectedWindow();
        } else {
            UpdateAttachButton();
        }
        return;
    }

    const ui::ListWidget& list = targetsList_;
    const int index = sessions_.IndexOf(reinterpret_cast<HWND>(list.DataAt(action.index)));
    if (index < 0) {
        return;
    }
    selected_ = index;
    if (action.kind == ui::ListWidget::ActionKind::Activate ||
        action.kind == ui::ListWidget::ActionKind::Toggle) {
        if (targetsList_.SelectedCount() > 1) {
            ToggleSelection();
        } else {
            ToggleStealth(index);
        }
    }
    InvalidateAll();
}

void App::AttachSelectedWindow() {
    if (tab_ != Tab::Windows) {
        return;
    }

    const std::vector<int> selected = windowsList_.SelectedIndices();
    if (selected.empty()) {
        SetStatus(L"NO WINDOW SELECTED", true);
        return;
    }

    std::vector<HWND> targets;
    std::wstring firstReason;
    for (int index : selected) {
        const HWND target = reinterpret_cast<HWND>(windowsList_.DataAt(index));
        if (target == nullptr) {
            continue;
        }
        std::wstring reason;
        if (!gf::ValidateTarget(target, reason)) {
            if (firstReason.empty()) {
                firstReason = reason;
            }
            continue;
        }
        if (sessions_.IndexOf(target) >= 0) {
            continue;
        }
        targets.push_back(target);
    }

    if (targets.empty()) {
        SetStatus(firstReason.empty() ? L"THE WINDOW IS ALREADY TRACKED"
                                      : L"ATTACH FAILED: " + firstReason,
                  true);
        return;
    }
    StartAttachBatch(std::move(targets), settings_.ui.hideOnAttach);
}

void App::AttachProcessWindows(HWND sample) {
    if (sample == nullptr) {
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    if (pid == 0) {
        return;
    }

    std::vector<HWND> targets;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        if (window.pid != pid || window.hwnd == nullptr) {
            continue;
        }
        if (sessions_.IndexOf(window.hwnd) >= 0) {
            continue;
        }
        std::wstring reason;
        if (!gf::ValidateTarget(window.hwnd, reason)) {
            continue;
        }
        targets.push_back(window.hwnd);
    }

    if (targets.empty()) {
        SetStatus(L"NO ATTACHABLE WINDOWS IN THIS PROCESS", true);
        return;
    }
    StartAttachBatch(std::move(targets), settings_.ui.hideOnAttach);
}

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
    // Tab switching first.
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

void App::OnRButtonDown(int x, int y) {
    if (tab_ != Tab::Windows) {
        return;
    }
    const int row = windowsList_.IndexAt(x, y);
    if (row < 0) {
        return;
    }
    const HWND sample = reinterpret_cast<HWND>(windowsList_.DataAt(row));
    if (sample == nullptr) {
        return;
    }

    // Count the visible windows of the same process that are not tracked yet,
    // so the menu can show how many would be attached.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    size_t count = 0;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        if (window.pid == pid && window.hwnd != nullptr && sessions_.IndexOf(window.hwnd) < 0) {
            ++count;
        }
    }
    if (count == 0) {
        SetStatus(L"NO ATTACHABLE WINDOWS IN THIS PROCESS", true);
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    wchar_t label[128] = {};
    swprintf_s(label, L"Attach all %zu window(s) of this process", count);
    ::AppendMenuW(menu, MF_STRING, ID_MENU_ATTACH_PROCESS, label);

    POINT point = {x, y};
    ::ClientToScreen(hwnd_, &point);
    ::SetForegroundWindow(hwnd_);
    const int command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0,
                                         hwnd_, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (command == ID_MENU_ATTACH_PROCESS) {
        AttachProcessWindows(sample);
    }
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

void App::SetSelected(int index) {
    selected_ = index;
    targetsList_.Select(index);
    if (tab_ == Tab::Targets) {
        InvalidateAll();
    }
}

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
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
    }
    if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    Layout();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::SelectTarget(int index) {
    SetSelected(index);
}

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

std::vector<size_t> App::SelectedTargetIndices() const {
    std::vector<size_t> indices;
    for (int index : targetsList_.SelectedIndices()) {
        if (index >= 0 && static_cast<size_t>(index) < sessions_.Count()) {
            indices.push_back(static_cast<size_t>(index));
        }
    }
    if (indices.empty() && SelectedValid()) {
        indices.push_back(static_cast<size_t>(selected_));
    }
    return indices;
}

void App::ToggleStealth(int index) {
    if (index < 0 || static_cast<size_t>(index) >= sessions_.Count()) {
        AttachForeground(settings_.ui.hideOnAttach);
        return;
    }

    const bool hide = !sessions_.At(static_cast<size_t>(index)).hidden();
    std::wstring error;
    const gf::SendOutcome outcome =
        SendCommand(index, hide ? gf::Cmd::Hide : gf::Cmd::Show, error);

    selected_ = index;
    targetsList_.Select(index);
    if (outcome == gf::SendOutcome::Ok) {
        SetStatus(hide ? L"STEALTH ENGAGED — WINDOW EXCLUDED FROM CAPTURE"
                       : L"CAPTURE EXCLUSION LIFTED");
    } else if (outcome == gf::SendOutcome::Error) {
        SetStatus(L"ERROR: " + error, true);
    }
    InvalidateAll();
}

void App::ToggleSelection() {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        AttachForeground(settings_.ui.hideOnAttach);
        return;
    }
    if (indices.size() == 1) {
        ToggleStealth(static_cast<int>(indices.front()));
        return;
    }

    // One direction for the whole selection: if anything is still visible, hide
    // everything; otherwise lift the exclusion from everything.
    bool anyVisible = false;
    for (size_t index : indices) {
        if (!sessions_.At(index).hidden()) {
            anyVisible = true;
            break;
        }
    }
    const gf::Cmd cmd = anyVisible ? gf::Cmd::Hide : gf::Cmd::Show;
    const gf::BatchSendResult result = sessions_.SendToMany(indices, cmd);

    wchar_t status[192] = {};
    if (result.error == 0 && result.timeout == 0) {
        swprintf_s(status,
                   cmd == gf::Cmd::Hide ? L"STEALTH ENGAGED — %d TARGETS"
                                        : L"CAPTURE EXCLUSION LIFTED — %d TARGETS",
                   result.ok);
        SetStatus(status);
    } else {
        swprintf_s(status, L"%d/%zu %s — %d BUSY, %d FAILED", result.ok, indices.size(),
                   cmd == gf::Cmd::Hide ? L"HIDDEN" : L"RESTORED", result.timeout, result.error);
        SetStatus(status, true);
    }
    InvalidateAll();
}

void App::RestoreAll() {
    pendingCommand_ = PendingCommand{};
    sessions_.RestoreAll();
    selected_ = -1;
    BuildTargetRows();
    SetStatus(L"ALL TARGETS RESTORED");
    UpdateTray();
    InvalidateAll();
}

void App::TargetShow(int command) {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    int applied = 0;
    for (size_t index : indices) {
        const HWND target = sessions_.At(index).hwnd;
        if (::IsWindow(target)) {
            ::ShowWindow(target, command);
            ++applied;
        }
    }

    if (applied == 0) {
        SetStatus(L"THE TARGET WINDOW IS GONE", true);
        return;
    }
    const bool minimize = command == SW_MINIMIZE;
    if (applied == 1) {
        SetStatus(minimize ? L"TARGET MINIMIZED" : L"TARGET RESTORED");
    } else {
        wchar_t status[96] = {};
        swprintf_s(status, minimize ? L"%d TARGETS MINIMIZED" : L"%d TARGETS RESTORED", applied);
        SetStatus(status);
    }
}

void App::TargetClose() {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    for (size_t index : indices) {
        const HWND target = sessions_.At(index).hwnd;
        if (::IsWindow(target)) {
            ::PostMessageW(target, WM_CLOSE, 0, 0);
        }
    }
    RemoveSelected();
    SetStatus(indices.size() > 1 ? L"CLOSE SIGNAL SENT TO SELECTED TARGETS"
                                 : L"CLOSE SIGNAL SENT");
}

void App::RemoveSelected() {
    std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    // Remove from the end so earlier indices stay valid while erasing.
    std::sort(indices.begin(), indices.end());
    for (size_t i = indices.size(); i > 0; --i) {
        const gf::Session& session = sessions_.At(indices[i - 1]);
        // The user removed this window on purpose: never auto-capture it again
        // until its process recreates it.
        autoGuard_.Suppress(session.pid, session.createTime, session.hwnd);
        sessions_.Remove(indices[i - 1]);
    }

    selected_ = sessions_.Empty() ? -1 : 0;
    BuildTargetRows();
    targetsList_.Select(selected_);
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
