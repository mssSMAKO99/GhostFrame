#include "app.h"

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
constexpr UINT_PTR kWindowsTimer = 2;
constexpr UINT kWindowsRefreshMs = 2000;
constexpr int kMaxSendAttempts = 6;
constexpr DWORD kSendRetryDelayMs = 500;
constexpr DWORD kAttachJoinTimeoutMs = 10000;

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
    ::InterlockedExchange(&attachCancel_, 1);
    JoinAttachWorker();
    const bool workerGone = attachThread_ == nullptr;

    sessions_.RestoreAll();

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
            }
            ui::DrawButton(item, theme_, active, item->CtlID == ID_BTN_CLOSE);
            return TRUE;
        }

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
            break;
    }

    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void App::OnCreate() {
    theme_.Create(::GetDpiForWindow(hwnd_));
    editBrush_ = ::CreateSolidBrush(ui::kPanel);
    CreateChildren();
    Layout();

    for (int id : {ID_BTN_REFRESH, ID_BTN_ATTACH, ID_EDIT_SEARCH}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, SW_HIDE);
        }
    }
    BuildTargetRows();

    ::SetTimer(hwnd_, ID_TIMER_REFRESH, 250, nullptr);
    RegisterHotkeys();
    AddTray();

    if (!noSelfExclude_) {
        ::SetWindowDisplayAffinity(hwnd_, 0x11);
    }

    SetStatus(L"READY — PICK A WINDOW ON THE WINDOWS TAB, OR FOCUS ONE AND PRESS CTRL+SHIFT+H");
}

void App::OnDestroy() {
    ::KillTimer(hwnd_, ID_TIMER_REFRESH);
    ::KillTimer(hwnd_, kWindowsTimer);
    UnregisterHotkeys();
    RemoveTray();
    theme_.Destroy();

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

    targetsList_.SetBounds(ListRect(), theme_);
    windowsList_.SetBounds(ListRect(), theme_);
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
    static const wchar_t* const kTabNames[] = {L"TARGETS", L"WINDOWS"};
    for (int index = 0; index < 2; ++index) {
        const bool active = (tab_ == Tab::Targets ? 0 : 1) == index;
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
    } else {
        RECT headerLeft = {theme_.S(16), theme_.S(kHeaderTop), theme_.S(320),
                           theme_.S(kHeaderTop + 20)};
        wchar_t header[96] = {};
        swprintf_s(header, L"WINDOWS — %d AVAILABLE", windowsList_.SelectableCount());
        ui::DrawLabel(dc, header, headerLeft, theme_.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (tab_ == Tab::Targets) {
        targetsList_.Paint(dc, theme_);
    } else {
        windowsList_.Paint(dc, theme_);
    }

    RECT footer = {theme_.S(16), theme_.S(kListBottom + 76), theme_.S(width - 16),
                   theme_.S(kListBottom + 98)};
    ui::DrawLabel(dc, L"WDA_EXCLUDEFROMCAPTURE · PER-MONITOR DPI · NO ADMIN REQUIRED", footer,
                  theme_.small, ui::kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void App::OnTimer() {
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

void App::SelectTab(Tab tab) {
    if (tab_ == tab) {
        return;
    }
    tab_ = tab;

    const int visibility = tab_ == Tab::Windows ? SW_SHOW : SW_HIDE;
    for (int id : {ID_BTN_REFRESH, ID_BTN_ATTACH}) {
        if (HWND child = ::GetDlgItem(hwnd_, id)) {
            ::ShowWindow(child, visibility);
        }
    }
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::ShowWindow(search, visibility);
    }

    if (tab_ == Tab::Windows) {
        RefreshWindows();
        ::SetTimer(hwnd_, kWindowsTimer, kWindowsRefreshMs, nullptr);
        SetStatus(L"PICK A WINDOW — DOUBLE-CLICK OR SELECT AND PRESS ATTACH");
    } else {
        ::KillTimer(hwnd_, kWindowsTimer);
        BuildTargetRows();
    }
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
    StartAttachBatch(std::move(targets), true);
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
    StartAttachBatch(std::move(targets), true);
}

void App::OnMouseMove(int x, int y) {
    bool changed = false;
    if (tab_ == Tab::Windows) {
        changed |= windowsList_.OnMouseMove(x, y, true);
        changed |= targetsList_.ClearHover();
    } else {
        changed |= targetsList_.OnMouseMove(x, y, true);
        changed |= windowsList_.ClearHover();
    }
    if (changed) {
        InvalidateAll();
    }
}

void App::OnMouseLeave() {
    const bool a = targetsList_.ClearHover();
    const bool b = windowsList_.ClearHover();
    if (a || b) {
        InvalidateAll();
    }
}

void App::OnMouseWheel(int x, int y, int delta) {
    POINT point = {x, y};
    ::ScreenToClient(hwnd_, &point);
    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    if (list.Contains(point.x, point.y)) {
        list.OnMouseWheel(delta);
        InvalidateAll();
    }
}

void App::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    // Tab switching first.
    for (int index = 0; index < 2; ++index) {
        const RECT tab = TabRect(index);
        if (::PtInRect(&tab, POINT{x, y})) {
            SelectTab(index == 0 ? Tab::Targets : Tab::Windows);
            return;
        }
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
    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    ApplyListAction(list.OnLButtonDblClk(x, y), tab_ == Tab::Windows);
    InvalidateAll();
}

void App::OnKeyDown(int virtualKey) {
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

    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    ApplyListAction(list.OnKeyDown(virtualKey), tab_ == Tab::Windows);
    InvalidateAll();
}

void App::OnVScroll(int code, int position) {
    ui::ListWidget& list = tab_ == Tab::Windows ? windowsList_ : targetsList_;
    list.OnVScroll(code, position);
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

    for (BatchItem& item : batch->items) {
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
        AttachForeground(true);
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
        AttachForeground(true);
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
