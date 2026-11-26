#include "app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

#include "ghostframe/ui_messages.h"
#include "ghostframe/version.h"
#include "autostart.h"
#include "branding.h"
#include "emoji_icon.h"
#include "log.h"
#include "resource.h"
#include "self_delete.h"
#include "shortcuts.h"

namespace {

constexpr const wchar_t* kWindowClass = gf::kUiWindowClass;
constexpr int kMinWidth = 860;
// The lowest height at which every settings page still fits without cards
// running into the footer.  It keeps the window inside a 1366x768 desktop.
constexpr int kMinHeight = 680;
constexpr int kDefaultWidth = 1080;
constexpr int kDefaultHeight = 720;
constexpr UINT kAttachDoneMessage = WM_APP + 2;
constexpr UINT kAttachProgressMessage = WM_APP + 3;
constexpr UINT_PTR kWindowsTimer = 2;
constexpr int kMaxSendAttempts = 6;
constexpr DWORD kSendRetryDelayMs = 500;
constexpr DWORD kAttachJoinTimeoutMs = 10000;
constexpr DWORD kSettingsSaveDelayMs = 500;
constexpr ULONGLONG kToastDurationMs = 3500;

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

// One finished attach attempt inside a batch job.  The worker only carries plain
// data back: the daemon owns the Session object, the UI just re-reads a snapshot.
struct BatchItem {
    HWND hwnd = nullptr;
    std::wstring title;
    bool ok = false;
    std::wstring error;
};

// Handed from the attach worker to the UI thread through WM_APP+2 once the
// whole batch is done.
struct BatchCompletion {
    std::vector<BatchItem> items;
    bool cancelled = false;
};

// Input for the attach worker: it talks to the daemon, never to a CaptureService.
struct AttachJob {
    HWND notify = nullptr;
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

// All edit fields use the theme's body font; the set grows with each view.
BOOL CALLBACK ApplyEditFontProc(HWND child, LPARAM font) {
    wchar_t className[16] = {};
    if (::GetClassNameW(child, className, _countof(className)) != 0 &&
        _wcsicmp(className, L"EDIT") == 0) {
        ::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
    }
    return TRUE;
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
    if (!capture_.Connect(error)) {
        // Keep the UI usable for editing settings, but say clearly that capture
        // is unavailable; the daemon is the only path to the targets.
        gf::LogLine(L"daemon connection failed: %s", error.c_str());
        ShowError(L"GhostFrame could not start its capture daemon:\n\n" + error);
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &App::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;

    // The generated emoji icon (when configured) is loaded before the class is
    // registered so the taskbar picks it up from the first frame.
    {
        std::wstring iconError;
        if (!settings_.appearance.iconEmoji.empty() &&
            !gf::EnsureEmojiIcon(settings_.appearance.iconEmoji, iconError)) {
            gf::LogLine(L"emoji icon: %s", iconError.c_str());
        }
        bigIcon_ = gf::LoadConfiguredIcon(instance, IDI_APP, settings_.appearance.iconEmoji, 32,
                                          bigIconOwned_);
        smallIcon_ = gf::LoadConfiguredIcon(instance, IDI_APP, settings_.appearance.iconEmoji, 16,
                                            smallIconOwned_);
        windowClass.hIcon = bigIcon_;
        windowClass.hIconSm = smallIcon_;
    }

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

    hwnd_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"GhostFrame",
                              WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, windowWidth,
                              windowHeight, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        ShowError(L"cannot create the GhostFrame window:\n" + gf::Win32Message(::GetLastError()));
        return InitResult::Failed;
    }

    if (bigIcon_ != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon_));
    }
    if (smallIcon_ != nullptr) {
        ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));
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

    // Closing the UI must not disturb capture: the daemon keeps the targets
    // attached and restores them only when it exits.
    CaptureGeometry();
    SaveSettings();
    gf::LogLine(L"shutdown");
    gf::LogShutdown();
}

void App::OnClose() {
    // Let a running attach finish (bounded by its own timeouts) and collect any
    // completed-but-undispatched result, then hide/free the UI.  Capture stays
    // with the daemon.
    ::InterlockedExchange(&attachCancel_, 1);
    JoinAttachWorker();
    DrainAttachResults();
    CaptureGeometry();
    SaveSettings();

    // Delete-on-exit is opt-in and always asks first.  A normal close keeps the
    // daemon (and the targets it owns) untouched.
    if (!selfDeleteInProgress_ && settings_.ui.deleteOnExit &&
        settings_.ui.selfDelete != gf::SelfDeleteMode::Never) {
        if (ConfirmSelfDelete()) {
            BeginSelfDelete();
            return;
        }
    }
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
            } else if (wParam == kWindowsTimer) {
                if (gf::IView* view = ActiveView()) {
                    view->OnTimer();
                }
            }
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE) {
                if (gf::IView* view = ActiveView()) {
                    view->OnControlChanged(static_cast<int>(LOWORD(wParam)));
                }
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
            gf::IView* view = ActiveView();
            const bool active = view != nullptr && view->ButtonActive(static_cast<int>(item->CtlID));
            const bool hovered = hoveredButton_ != 0 && static_cast<int>(item->CtlID) == hoveredButton_;
            COLORREF surface = ui::kBg;
            for (int id : {ID_BTN_SET_RESET, ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE,
                           ID_BTN_SET_APPLY, ID_BTN_SET_DELETE_NOW}) {
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
            gf::IView* view = ActiveView();
            const bool interactive = ZoneAt(point.x, point.y) != HotZone::None ||
                                     TabAt(point.x, point.y) >= 0 ||
                                     (view != nullptr && view->HandCursorAt(point.x, point.y));
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
            if (gf::IView* view = ActiveView(); view != nullptr && view->CapturesAlt()) {
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
                for (int i = 0; i < layout_.tabCount; ++i) {
                    if (::PtInRect(&layout_.tabs[i], point)) {
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
            if (message == gf::kUiShowMessage) {
                ShowUi(true);
                return 0;
            }
            if (message == gf::kUiToggleMessage) {
                ShowUi(::IsWindowVisible(hwnd_) == FALSE);
                return 0;
            }
            if (message == gf::kUiQuitMessage) {
                ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return 0;
            }
            if (message == gf::kUiSelfDeleteMessage) {
                selfDeleteInProgress_ = true;
                ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
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

void App::CreateViews() {
    const std::vector<gf::ViewFactory>& factories = gf::ViewFactories();
    views_.reserve(factories.size());
    viewCatalog_.reserve(factories.size());
    for (gf::ViewFactory factory : factories) {
        views_.push_back(factory(*this));
        viewCatalog_.push_back(views_.back()->Info());
    }
}

std::vector<std::wstring> App::ViewIds() const {
    std::vector<std::wstring> ids;
    ids.reserve(viewCatalog_.size());
    for (const gf::ViewInfo& info : viewCatalog_) {
        ids.push_back(info.id);
    }
    return ids;
}

void App::RebuildTabOrder() {
    tabOrder_.clear();
    for (const gf::TabPreference& tab : settings_.appearance.tabs) {
        if (!tab.visible) {
            continue;
        }
        for (size_t i = 0; i < views_.size(); ++i) {
            if (tab.id == views_[i]->Info().id) {
                tabOrder_.push_back(static_cast<int>(i));
                break;
            }
        }
    }
    if (tabOrder_.empty()) {
        for (size_t i = 0; i < views_.size(); ++i) {
            tabOrder_.push_back(static_cast<int>(i));
        }
    }
}

int App::StripIndexFor(int viewIndex) const {
    for (size_t i = 0; i < tabOrder_.size(); ++i) {
        if (tabOrder_[i] == viewIndex) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

gf::IView* App::ActiveView() const {
    if (activeIndex_ < 0 || static_cast<size_t>(activeIndex_) >= views_.size()) {
        return nullptr;
    }
    return views_[static_cast<size_t>(activeIndex_)].get();
}

void App::SelectView(const wchar_t* id) {
    if (id == nullptr) {
        return;
    }
    for (size_t i = 0; i < views_.size(); ++i) {
        if (wcscmp(views_[i]->Info().id, id) == 0) {
            SelectViewIndex(static_cast<int>(i));
            return;
        }
    }
}

void App::SelectViewIndex(int index) {
    if (index < 0 || static_cast<size_t>(index) >= views_.size() || index == activeIndex_) {
        return;
    }
    // Hidden views are not reachable through the strip, keyboard cycling or an
    // internal "go to tab" action.
    if (StripIndexFor(index) < 0) {
        return;
    }
    if (gf::IView* previous = ActiveView()) {
        previous->SetActive(false);
    }
    ::KillTimer(hwnd_, kWindowsTimer);
    activeIndex_ = index;
    settings_.ui.lastTab = views_[static_cast<size_t>(index)]->Info().id;
    MarkSettingsDirty();

    RelayoutInternal();
    gf::IView* view = ActiveView();
    view->SetActive(true);
    view->Build();
    if (const UINT interval = view->RefreshIntervalMs(); interval > 0) {
        ::SetTimer(hwnd_, kWindowsTimer, interval, nullptr);
    }
    InvalidateAll();
}

void App::OnCreate() {
    ui::InitIcons();
    theme_.Create(::GetDpiForWindow(hwnd_), settings_.appearance);
    editBrush_ = ::CreateSolidBrush(ui::kPanelHi);
    CreateViews();
    gf::NormalizeTabs(settings_.appearance, ViewIds());
    RebuildTabOrder();
    CreateChildren();
    const bool backdropLoaded = ReloadBackdrop();
    RelayoutInternal();

    for (auto& view : views_) {
        view->SetActive(false);
        view->Build();
    }

    // Start on the tab the user left off on, if it is still visible.
    int start = -1;
    for (size_t i = 0; i < views_.size(); ++i) {
        if (settings_.ui.lastTab == views_[i]->Info().id && StripIndexFor(static_cast<int>(i)) >= 0) {
            start = static_cast<int>(i);
            break;
        }
    }
    if (start < 0 && !tabOrder_.empty()) {
        start = tabOrder_.front();
    }
    if (start >= 0) {
        SelectViewIndex(start);
    } else if (gf::IView* view = ActiveView()) {
        view->SetActive(true);
    }

    ::SetTimer(hwnd_, ID_TIMER_REFRESH, 250, nullptr);
    ApplyRoundedCorners(hwnd_);

    if (!noSelfExclude_) {
        ::SetWindowDisplayAffinity(hwnd_, 0x11);
    }

    // The daemon owns the hotkeys, the tray icon and auto-capture; hand it the
    // document we just loaded so both sides always agree.
    std::wstring error;
    if (!PushSettings(&error) && !error.empty()) {
        gf::LogLine(L"cannot push settings to the daemon: %s", error.c_str());
    }
    capture_.Refresh();
    if (gf::IView* view = ActiveView()) {
        view->Build();
    }

    if (settings_.badFile) {
        SetStatus(L"settings.json was corrupt — renamed to .bad, defaults restored", true);
    } else if (!backdropLoaded && !settings_.appearance.backgroundImage.empty()) {
        SetStatus(L"The background image could not be loaded — using the theme background", true);
    } else {
        SetStatus(L"Ready — pick a window on the Windows tab, or focus one and press Ctrl+Shift+H");
    }
}

void App::OnDestroy() {
    ::KillTimer(hwnd_, ID_TIMER_REFRESH);
    ::KillTimer(hwnd_, kWindowsTimer);
    if (tooltip_ != nullptr) {
        ::DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    views_.clear();
    viewCatalog_.clear();
    tabOrder_.clear();
    if (backdrop_ != nullptr) {
        ::DeleteObject(backdrop_);
        backdrop_ = nullptr;
    }
    if (bigIcon_ != nullptr && bigIconOwned_) {
        ::DestroyIcon(bigIcon_);
    }
    if (smallIcon_ != nullptr && smallIconOwned_ && smallIcon_ != bigIcon_) {
        ::DestroyIcon(smallIcon_);
    }
    bigIcon_ = nullptr;
    smallIcon_ = nullptr;
    bigIconOwned_ = false;
    smallIconOwned_ = false;
    theme_.Destroy();
    ui::ShutdownIcons();

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
    SetupTooltips();
    // Each view owns its controls; the shell only supplies the window and the
    // owner-draw/tooltip plumbing.
    for (auto& view : views_) {
        view->CreateControls(hwnd_, instance_);
    }
}

void App::SetupTooltips() {
    if (tooltip_ != nullptr) {
        return;
    }
    tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                                 instance_, nullptr);
    if (tooltip_ == nullptr) {
        return;
    }
    ::SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
}

void App::AttachTooltip(HWND child, const wchar_t* text) {
    if (tooltip_ == nullptr || child == nullptr || text == nullptr) {
        return;
    }
    TOOLINFOW tool = {};
    tool.cbSize = sizeof(tool);
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = hwnd_;
    tool.uId = reinterpret_cast<UINT_PTR>(child);
    tool.lpszText = const_cast<LPWSTR>(text);
    ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

void App::AddButton(int id, const wchar_t* text, const wchar_t* tooltip) {
    HWND button = ::CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (button != nullptr) {
        ::SetWindowSubclass(button, &App::ButtonSubclassProc, static_cast<UINT_PTR>(id),
                            reinterpret_cast<DWORD_PTR>(this));
        AttachTooltip(button, tooltip);
    }
}

void App::AddSearchEdit(int id, const wchar_t* cue) {
    ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (HWND search = ::GetDlgItem(hwnd_, id)) {
        ::SendMessageW(search, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
        ::SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        ::SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                       MAKELPARAM(theme_.S(34), theme_.S(10)));
    }
}

void App::AddEdit(int id, const wchar_t* cue) {
    ::CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (HWND edit = ::GetDlgItem(hwnd_, id)) {
        ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
        ::SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        ::SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                       MAKELPARAM(theme_.S(8), theme_.S(8)));
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

gf::LayoutInfo App::ComputeLayout() const {
    gf::LayoutInfo info;
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

    const int tabH = theme_.S(theme_.m.tabHeight);
    const int tabGap = theme_.S(6);
    const int tabLeft = theme_.S(238);
    info.tabCount = static_cast<int>(tabOrder_.size());
    if (info.tabCount > 8) {
        info.tabCount = 8;
    }

    const int winW = theme_.S(34);
    const int winGap = theme_.S(6);
    int winLeft = width - theme_.S(14) - winW * 3 - winGap * 2;
    for (int i = 0; i < 3; ++i) {
        const int left = winLeft + i * (winW + winGap);
        info.windowButtons[i] = {left, (topH - theme_.S(30)) / 2, left + winW,
                                 (topH - theme_.S(30)) / 2 + theme_.S(30)};
    }

    // Shrink the tabs when the registry grows so they never reach the version
    // label or the window buttons.
    int tabW = theme_.S(theme_.m.tabWidth);
    if (info.tabCount > 0) {
        const int tabRightLimit = winLeft - theme_.S(124);
        const int available = tabRightLimit - tabLeft;
        const int fit = (available - (info.tabCount - 1) * tabGap) / info.tabCount;
        tabW = (std::max)(theme_.S(72), (std::min)(tabW, fit));
    }
    for (int i = 0; i < info.tabCount; ++i) {
        const int left = tabLeft + i * (tabW + tabGap);
        info.tabs[i] = {left, (topH - tabH) / 2, left + tabW, (topH - tabH) / 2 + tabH};
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

    const int footH = theme_.S(18);
    info.footer = {cx, height - pad - footH, cx + cw, height - pad};
    info.footerLine = {cx, info.footer.top - theme_.S(12), cx + cw,
                       info.footer.top - theme_.S(11)};
    info.logLink = {cx + cw - theme_.S(104), info.footer.top, cx + cw, info.footer.bottom};

    info.body = {cx, info.pageHeader.bottom + theme_.S(18), cx + cw,
                 info.footer.top - theme_.S(14)};
    return info;
}

void App::Relayout() {
    RelayoutInternal();
}

void App::RelayoutInternal() {
    layout_ = ComputeLayout();
    if (gf::IView* view = ActiveView()) {
        view->Layout();
    }
}

int App::TabAt(int x, int y) const {
    POINT point = {x, y};
    for (int index = 0; index < layout_.tabCount; ++index) {
        if (::PtInRect(&layout_.tabs[index], point)) {
            return tabOrder_[static_cast<size_t>(index)];
        }
    }
    return -1;
}

// Validates and applies a candidate hotkey set.  The daemon owns the actual
// registration, so a combination that is taken by another app is rejected there
// and the previous set stays registered.
bool App::ApplyHotkeys(const gf::Settings& candidate, std::wstring& error) {
    gf::Settings next = settings_;
    for (int i = 0; i < gf::kHotkeyCount; ++i) {
        next.hotkeys[i] = candidate.hotkeys[i];
    }

    if (!gf::ValidateHotkeys(next, error)) {
        return false;
    }
    if (!capture_.ApplySettings(next, error)) {
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

void App::SaveSettingsNow() {
    SaveSettings();
    PushSettings(nullptr);
}

// Pushes the document to the daemon, which applies the hotkeys and the
// auto-capture rules and persists its own copy.  A missing daemon is not fatal:
// the UI keeps working from its local file.
bool App::PushSettings(std::wstring* error) {
    std::wstring local;
    if (!capture_.ApplySettings(settings_, local)) {
        if (error != nullptr) {
            *error = local;
        }
        gf::LogLine(L"cannot push settings to the daemon: %s", local.c_str());
        return false;
    }
    return true;
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
    PaintBackdrop(dc, client);

    PaintTopBar(dc, client);
    PaintSidebar(dc, client);
    PaintPageChrome(dc);

    if (gf::IView* view = ActiveView()) {
        view->Paint(dc);
    }

    PaintFooter(dc);

    // Transient toast.
    if (!toast_.empty() && ::GetTickCount64() < toastUntil_) {
        const SIZE size = ui::MeasureTextSize(dc, theme_.body, toast_);
        const int width = size.cx + theme_.S(40);
        const int height = theme_.S(36);
        const int left = layout_.content.left +
                         ((layout_.content.right - layout_.content.left) - width) / 2;
        RECT toast = {left, layout_.topBar.bottom + theme_.S(12), left + width,
                      layout_.topBar.bottom + theme_.S(12) + height};
        ui::FillRoundRect(dc, toast, ui::kToastBg, theme_.S(10));
        ui::StrokeRoundFrame(dc, toast, toastError_ ? ui::kWarn : ui::kNeonDim, theme_.S(10), 1);
        ui::DrawLabel(dc, toast_.c_str(), toast, theme_.body,
                      toastError_ ? ui::kWarn : ui::kText,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void App::RefreshThemeFonts() {
    ::EnumChildWindows(hwnd_, &ApplyEditFontProc, reinterpret_cast<LPARAM>(theme_.body));
    if (tooltip_ != nullptr) {
        ::SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.small), TRUE);
    }
    if (editBrush_ != nullptr) {
        ::DeleteObject(editBrush_);
    }
    editBrush_ = ::CreateSolidBrush(ui::kPanelHi);
}

bool App::ReloadBackdrop() {
    const std::wstring& path = settings_.appearance.backgroundImage;
    if (backdrop_ != nullptr && path == backdropPath_) {
        return true;
    }
    if (backdrop_ != nullptr) {
        ::DeleteObject(backdrop_);
        backdrop_ = nullptr;
    }
    backdropPath_.clear();
    if (path.empty()) {
        return true;
    }

    backdrop_ = ui::LoadPicture(path.c_str());
    if (backdrop_ == nullptr) {
        gf::LogLine(L"cannot load the background image: %s", path.c_str());
        return false;
    }
    backdropPath_ = path;
    return true;
}

void App::PaintBackdrop(HDC dc, const RECT& client) {
    bool painted = false;
    if (backdrop_ != nullptr && !theme_.highContrast) {
        BITMAP bitmap = {};
        if (::GetObjectW(backdrop_, sizeof(bitmap), &bitmap) == sizeof(bitmap) &&
            bitmap.bmWidth > 0 && bitmap.bmHeight > 0) {
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            if (width > 0 && height > 0) {
                // Cover-fill: crop the source to the client aspect ratio.
                const double scale = (std::max)(static_cast<double>(width) / bitmap.bmWidth,
                                                static_cast<double>(height) / bitmap.bmHeight);
                int sourceW = static_cast<int>(width / scale);
                int sourceH = static_cast<int>(height / scale);
                sourceW = (std::min)(sourceW, static_cast<int>(bitmap.bmWidth));
                sourceH = (std::min)(sourceH, static_cast<int>(bitmap.bmHeight));
                const int sourceX = (bitmap.bmWidth - sourceW) / 2;
                const int sourceY = (bitmap.bmHeight - sourceH) / 2;

                HDC memory = ::CreateCompatibleDC(dc);
                if (memory != nullptr) {
                    HGDIOBJ old = ::SelectObject(memory, backdrop_);
                    const int previous = ::SetStretchBltMode(dc, HALFTONE);
                    ::SetBrushOrgEx(dc, 0, 0, nullptr);
                    ::StretchBlt(dc, client.left, client.top, width, height, memory, sourceX,
                                 sourceY, sourceW, sourceH, SRCCOPY);
                    ::SetStretchBltMode(dc, previous);
                    ::SelectObject(memory, old);
                    ::DeleteDC(memory);
                    painted = true;

                    // Keep the chrome readable over a busy picture.
                    ui::FillSolidAlpha(dc, client, ui::kBg, 170);
                }
            }
        }
    }

    if (painted) {
        return;
    }
    if (theme_.gradientBackground) {
        ui::FillVerticalGradient(dc, client, theme_.backgroundTop, theme_.backgroundBottom);
    } else {
        ui::FillSolid(dc, client, ui::kBg);
    }
}

bool App::ApplyWindowIcons() {
    std::wstring error;
    if (!settings_.appearance.iconEmoji.empty() &&
        !gf::EnsureEmojiIcon(settings_.appearance.iconEmoji, error)) {
        SetStatus(L"Error: the emoji icon could not be rendered — keeping the previous icon", true);
        gf::LogLine(L"emoji icon: %s", error.c_str());
        return false;
    }
    // Clearing the emoji removes the generated file and falls back to the
    // bundled resource icon.
    if (settings_.appearance.iconEmoji.empty()) {
        gf::EnsureEmojiIcon(std::wstring(), error);
    }

    bool bigOwned = false;
    bool smallOwned = false;
    HICON big = gf::LoadConfiguredIcon(instance_, IDI_APP, settings_.appearance.iconEmoji, 32,
                                       bigOwned);
    HICON small = gf::LoadConfiguredIcon(instance_, IDI_APP, settings_.appearance.iconEmoji, 16,
                                         smallOwned);
    if (big == nullptr || small == nullptr) {
        if (big != nullptr && bigOwned) {
            ::DestroyIcon(big);
        }
        if (small != nullptr && smallOwned) {
            ::DestroyIcon(small);
        }
        SetStatus(L"Error: the emoji icon could not be loaded — keeping the previous icon", true);
        return false;
    }

    ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));

    if (bigIcon_ != nullptr && bigIconOwned_) {
        ::DestroyIcon(bigIcon_);
    }
    if (smallIcon_ != nullptr && smallIconOwned_ && smallIcon_ != bigIcon_) {
        ::DestroyIcon(smallIcon_);
    }
    bigIcon_ = big;
    smallIcon_ = small;
    bigIconOwned_ = bigOwned;
    smallIconOwned_ = smallOwned;
    return true;
}

bool App::ApplyAppearance() {
    gf::NormalizeTabs(settings_.appearance, ViewIds());
    theme_.Create(::GetDpiForWindow(hwnd_), settings_.appearance);
    RefreshThemeFonts();
    const bool backdropLoaded = ReloadBackdrop();
    RebuildTabOrder();
    const bool iconApplied = ApplyWindowIcons();

    if (activeIndex_ >= 0 && StripIndexFor(activeIndex_) < 0) {
        // The active tab was hidden: move to the first visible tab.
        if (gf::IView* previous = ActiveView()) {
            previous->SetActive(false);
        }
        activeIndex_ = -1;
        if (!tabOrder_.empty()) {
            SelectViewIndex(tabOrder_.front());
        }
    } else {
        RelayoutInternal();
        if (gf::IView* view = ActiveView()) {
            view->Build();
        }
        InvalidateAll();
    }

    if (!backdropLoaded && !settings_.appearance.backgroundImage.empty()) {
        SetStatus(L"The background image could not be loaded — using the theme background", true);
    }
    return iconApplied;
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

    for (int index = 0; index < layout_.tabCount; ++index) {
        const int viewIndex = tabOrder_[static_cast<size_t>(index)];
        const gf::IView* view = views_[static_cast<size_t>(viewIndex)].get();
        const gf::ViewInfo& info = view->Info();
        const bool active = activeIndex_ == viewIndex;
        const bool hovered = hoverTab_ == viewIndex;
        const RECT tab = layout_.tabs[index];
        const COLORREF background =
            active ? ui::Blend(ui::kPanelHi, ui::kNeonBg, 45)
                   : (hovered ? ui::kPanelHi : ui::kTopBar);
        ui::FillRoundRect(dc, tab, background, theme_.S(9));
        if (active) {
            ui::StrokeRoundFrame(dc, tab, ui::kNeonDim, theme_.S(9), 1);
        }

        const SIZE textSize = ui::MeasureTextSize(dc, theme_.small, info.label);
        const int iconSize = theme_.S(15);
        const int total = iconSize + theme_.S(8) + textSize.cx;
        const int startX = tab.left + ((tab.right - tab.left) - total) / 2;
        const int iconY = tab.top + ((tab.bottom - tab.top) - iconSize) / 2;
        RECT iconRect = {startX, iconY, startX + iconSize, iconY + iconSize};
        ui::DrawIcon(dc, info.icon, iconRect,
                     active ? ui::kNeon : (hovered ? ui::kText : ui::kMuted), theme_.S(2));
        RECT label = {iconRect.right + theme_.S(8), tab.top, tab.right, tab.bottom};
        ui::DrawLabel(dc, info.label, label, theme_.small,
                      active ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Version and window controls.
    RECT version = {layout_.windowButtons[0].left - theme_.S(108), layout_.topBar.top,
                    layout_.windowButtons[0].left - theme_.S(18), layout_.topBar.bottom};
    ui::DrawLabel(dc, L"v" GF_VERSION_STRING4, version, theme_.tiny, ui::kTextDim,
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
    for (size_t i = 0; i < capture_.Count(); ++i) {
        const gf::RemoteSession& session = capture_.At(i);
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
    swprintf_s(trackedText, L"%zu tracked", capture_.Count());
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

void App::PaintPageChrome(HDC dc) {
    gf::IView* view = ActiveView();
    if (view == nullptr) {
        return;
    }
    const gf::ViewInfo& info = view->Info();

    ui::FillRoundRect(dc, layout_.pageIcon, ui::kNeonBg, theme_.S(12));
    ui::StrokeRoundFrame(dc, layout_.pageIcon, ui::kNeonSoft, theme_.S(12), 1);

    const int glyph = theme_.S(22);
    RECT glyphRect = {
        layout_.pageIcon.left + (layout_.pageIcon.right - layout_.pageIcon.left - glyph) / 2,
        layout_.pageIcon.top + (layout_.pageIcon.bottom - layout_.pageIcon.top - glyph) / 2, 0, 0};
    glyphRect.right = glyphRect.left + glyph;
    glyphRect.bottom = glyphRect.top + glyph;
    ui::DrawIcon(dc, info.icon, glyphRect, ui::kNeon, theme_.S(2));

    ui::DrawLabel(dc, info.title, layout_.pageTitle, theme_.display, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ui::DrawLabel(dc, info.subtitle, layout_.pageSubtitle, theme_.small, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    view->PaintHeader(dc);
}

void App::PaintFooter(HDC dc) {
    ui::FillSolid(dc, layout_.footerLine, ui::kBorder);

    const int iconSize = theme_.S(13);
    RECT info = {layout_.footer.left,
                 layout_.footer.top + (layout_.footer.bottom - layout_.footer.top - iconSize) / 2,
                 0, 0};
    info.right = info.left + iconSize;
    info.bottom = info.top + iconSize;
    ui::DrawIcon(dc, statusError_ ? ui::Icon::Warning : ui::Icon::Info, info,
                 statusError_ ? ui::kWarn : ui::kTextDim, theme_.S(2));

    RECT statusRect = {info.right + theme_.S(8), layout_.footer.top,
                       layout_.logLink.left - theme_.S(190), layout_.footer.bottom};
    ui::DrawLabel(dc, status_.c_str(), statusRect, theme_.small,
                  statusError_ ? ui::kWarn : ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    int hidden = 0;
    int errored = 0;
    for (size_t i = 0; i < capture_.Count(); ++i) {
        const gf::RemoteSession& session = capture_.At(i);
        if (session.hidden()) {
            ++hidden;
        }
        if (session.errored()) {
            ++errored;
        }
    }
    wchar_t summary[96] = {};
    swprintf_s(summary, L"%zu tracked · %d hidden · %d errors", capture_.Count(), hidden, errored);
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
        PushSettings(nullptr);
    }

    if (!toast_.empty() && ::GetTickCount64() >= toastUntil_) {
        toast_.clear();
        InvalidateAll();
    }

    RetryPendingCommand();

    gf::IView* view = ActiveView();
    if (view != nullptr) {
        view->OnBeforeCaptureRefresh();
    }
    if (capture_.Refresh() && view != nullptr) {
        view->OnCaptureChanged();
    }
}

void App::OnSize() {
    RelayoutInternal();
    InvalidateAll();
}

void App::OnSettingChange() {
    theme_.Create(::GetDpiForWindow(hwnd_), settings_.appearance);
    RefreshThemeFonts();
    RelayoutInternal();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Mouse / keyboard
// ---------------------------------------------------------------------------

void App::OnMouseMove(int x, int y) {
    bool changed = false;

    const HotZone zone = ZoneAt(x, y);
    const int tab = TabAt(x, y);
    if (zone != hotZone_ || tab != hoverTab_) {
        hotZone_ = zone;
        hoverTab_ = tab;
        changed = true;
    }

    if (gf::IView* view = ActiveView()) {
        view->OnMouseMove(x, y, changed);
    }
    if (changed) {
        InvalidateAll();
    }
}

void App::OnMouseLeave() {
    const bool chrome =
        hotZone_ != HotZone::None || hoverTab_ >= 0;
    hotZone_ = HotZone::None;
    hoverTab_ = -1;
    bool changed = chrome;
    if (gf::IView* view = ActiveView()) {
        view->OnMouseLeave(changed);
    }
    if (changed) {
        InvalidateAll();
    }
}

void App::OnMouseWheel(int x, int y, int delta) {
    POINT point = {x, y};
    ::ScreenToClient(hwnd_, &point);
    if (gf::IView* view = ActiveView()) {
        view->OnMouseWheel(point.x, point.y, delta);
    }
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
    return HotZone::None;
}

void App::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    const int tab = TabAt(x, y);
    if (tab >= 0) {
        SelectViewIndex(tab);
        return;
    }

    switch (ZoneAt(x, y)) {
        case HotZone::Hide:
            ShowUi(false);
            return;
        case HotZone::Maximize:
            ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return;
        case HotZone::Close:
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        case HotZone::Log:
            OpenLogFolder();
            return;
        case HotZone::None:
        default:
            break;
    }

    if (gf::IView* view = ActiveView()) {
        view->OnLButtonDown(x, y, ctrl, shift);
    }
}

void App::OnLButtonDblClk(int x, int y) {
    if (gf::IView* view = ActiveView()) {
        view->OnLButtonDblClk(x, y);
    }
}

void App::OnRButtonDown(int x, int y) {
    if (gf::IView* view = ActiveView()) {
        view->OnRButtonDown(x, y);
    }
}

void App::OnKeyDown(int virtualKey) {
    gf::IView* view = ActiveView();
    const bool capturing = view != nullptr && view->CapturesAlt();

    if (!capturing && (virtualKey == VK_TAB || virtualKey == VK_LEFT || virtualKey == VK_RIGHT)) {
        const int count = static_cast<int>(tabOrder_.size());
        if (count > 0) {
            const bool backward =
                virtualKey == VK_LEFT || (virtualKey == VK_TAB && (::GetKeyState(VK_SHIFT) & 0x8000));
            int position = StripIndexFor(activeIndex_);
            if (position < 0) {
                position = 0;
            }
            const int next = (position + (backward ? count - 1 : 1)) % count;
            SelectViewIndex(tabOrder_[static_cast<size_t>(next)]);
        }
        return;
    }

    if (view != nullptr && view->OnKeyDown(virtualKey)) {
        return;
    }

    if (virtualKey == VK_ESCAPE && attachThread_ != nullptr) {
        CancelAttach();
    }
}

void App::OnVScroll(int code, int position) {
    if (gf::IView* view = ActiveView()) {
        view->OnVScroll(code, position);
    }
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
        case ID_BTN_HIDE_UI:
            ShowUi(::IsWindowVisible(hwnd_) == FALSE);
            return;
        case ID_BTN_EXIT:
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        default:
            break;
    }

    if (gf::IView* view = ActiveView(); view != nullptr && view->OnCommand(id)) {
        InvalidateAll();
    }
}

void App::OnDpiChanged(UINT dpi, const RECT* suggested) {
    theme_.Create(dpi, settings_.appearance);
    RefreshThemeFonts();
    if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RelayoutInternal();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Attach orchestration
// ---------------------------------------------------------------------------

void App::AttachForegroundTarget(bool hideAfter) {
    if (attachThread_ != nullptr) {
        SetStatus(L"Attaching… — wait for the current attach to finish", true);
        return;
    }

    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr || foreground == hwnd_) {
        if (!capture_.Empty()) {
            const bool hide = !capture_.At(0).hidden();
            std::wstring error;
            SendCommand(0, hide ? gf::Cmd::Hide : gf::Cmd::Show, error);
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
    if (capture_.IndexOf(foreground) >= 0) {
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
        if (target == nullptr || capture_.IndexOf(target) >= 0) {
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

    auto* job = new AttachJob{hwnd_, std::move(pending), &attachCancel_, hideAfter};
    attachThread_ = ::CreateThread(nullptr, 0, &App::AttachThreadProc, job, 0, nullptr);
    if (attachThread_ == nullptr) {
        delete job;
        attachTotal_ = 0;
        SetStatus(L"Attach failed: cannot start the attach worker", true);
        return;
    }

    SetStatus(L"Attaching…");
    InvalidateAll();
}

DWORD WINAPI App::AttachThreadProc(LPVOID param) {
    AttachJob* job = static_cast<AttachJob*>(param);

    // The worker talks to the daemon directly: the UI thread stays responsive and
    // the daemon remains the only owner of the capture pipeline.
    gf::DaemonClient client;

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
        wchar_t title[160] = {};
        ::GetWindowTextW(target, title, _countof(title) - 1);
        item.title = title;

        gf::AttachReply reply = {};
        std::wstring error;
        if (!client.Attach(reinterpret_cast<uint64_t>(target), reply, error) || reply.ok == 0) {
            item.ok = false;
            item.error = error.empty() ? std::wstring(reply.error) : error;
        } else {
            item.ok = true;
            if (job->hideAfter) {
                gf::SendReply sent = {};
                std::wstring ignored;
                client.Send(reinterpret_cast<uint64_t>(target), gf::Cmd::Hide, sent, ignored);
            }
        }
        batch->items.push_back(std::move(item));
    }

    if (!::PostMessageW(job->notify, kAttachDoneMessage, 0, reinterpret_cast<LPARAM>(batch))) {
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

    if (batch == nullptr) {
        return;
    }

    int attached = 0;
    int failed = 0;
    std::wstring failedText;
    HWND lastHwnd = nullptr;

    for (const BatchItem& item : batch->items) {
        if (!item.ok) {
            ++failed;
            if (!failedText.empty()) {
                failedText += L", ";
            }
            failedText += item.title + L" (" +
                          (item.error.empty() ? L"failed" : item.error) + L")";
            continue;
        }
        ++attached;
        lastHwnd = item.hwnd;
    }

    const int requested = static_cast<int>(batch->items.size());
    const bool cancelled = batch->cancelled;
    delete batch;

    // Re-read the daemon's list; it owns the sessions and may also have attached
    // other windows (auto-capture) while this batch was running.
    capture_.Refresh();

    // Let every view react; the windows view may switch to the targets tab, and
    // the targets view adopts the last attached window.
    for (size_t i = 0; i < views_.size(); ++i) {
        views_[i]->OnAttachFinished(lastHwnd, static_cast<int>(i) == activeIndex_);
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
        // The daemon owns every session the worker created, so closing the UI
        // only drops the pending notification; it must not detach the targets.
        delete reinterpret_cast<BatchCompletion*>(message.lParam);
    }
}

gf::SendOutcome App::SendCommand(int index, gf::Cmd cmd, std::wstring& error) {
    const gf::SendOutcome outcome = capture_.Send(static_cast<size_t>(index), cmd, error);
    if (outcome == gf::SendOutcome::Timeout) {
        pendingCommand_ = PendingCommand{};
        pendingCommand_.hwnd = capture_.At(static_cast<size_t>(index)).hwnd;
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

    const int index = capture_.IndexOf(pendingCommand_.hwnd);
    if (index < 0) {
        pendingCommand_ = PendingCommand{};
        return;
    }

    std::wstring error;
    const gf::SendOutcome outcome =
        capture_.Send(static_cast<size_t>(index), pendingCommand_.cmd, error);
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

void App::OpenLogFolder() {
    const std::wstring& dir = gf::LogDirectory();
    if (dir.empty()) {
        SetStatus(L"The log is not available", true);
        return;
    }
    ::ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool App::ConfirmSelfDelete() const {
    const gf::SelfDeleteMode mode = settings_.ui.selfDelete;
    const wchar_t* detail =
        mode == gf::SelfDeleteMode::Everything
            ? L"This removes the executable, every shortcut, the startup entry and the "
              L"whole GhostFrame data folder (settings, logs, generated icon)."
            : L"This removes the executable, the generated icon, every shortcut and the "
              L"startup entry. Settings, logs and other data stay behind.";
    const std::wstring text =
        std::wstring(L"Delete GhostFrame now?\n\n") + detail +
        L"\n\nCapture is stopped and all tracked windows are restored first. "
        L"This cannot be undone.";
    return ::MessageBoxW(hwnd_, text.c_str(), L"Delete GhostFrame",
                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

void App::RequestSelfDelete() {
    if (selfDeleteInProgress_) {
        return;
    }
    if (settings_.ui.selfDelete == gf::SelfDeleteMode::Never) {
        SetStatus(L"Choose Exe only or Everything before deleting GhostFrame", true);
        return;
    }
    if (!ConfirmSelfDelete()) {
        return;
    }
    BeginSelfDelete();
}

void App::BeginSelfDelete() {
    selfDeleteInProgress_ = true;

    // Persist the current document first: ExeOnly deliberately keeps it.
    CaptureGeometry();
    SaveSettings();

    gf::SelfDeleteStartRequest request;
    request.mode = settings_.ui.selfDelete;
    request.executablePath = gf::ExecutablePath();
    request.dataDirectory = gf::SettingsDirectory();
    request.waitPids.push_back(::GetCurrentProcessId());

    const DWORD daemonPid = gf::FindDaemonProcessId();
    if (daemonPid != 0) {
        request.waitPids.push_back(daemonPid);
    }
    request.autostartValues.push_back(gf::AutostartValueName());
    {
        gf::BrandingManifest manifest;
        std::wstring manifestError;
        if (gf::LoadBrandingManifest(manifest, manifestError) &&
            !manifest.autostartValue.empty()) {
            request.autostartValues.push_back(manifest.autostartValue);
        }
        if (!manifest.shortcutPath.empty()) {
            request.extraFiles.push_back(manifest.shortcutPath);
        }
    }

    // Ask the daemon to restore every target and exit; the helper waits for the
    // process so the executable is not locked when it is removed.
    std::wstring error;
    gf::StopDaemonForSelfDelete(error);

    DWORD helperPid = 0;
    if (!gf::StartSelfDeleteHelper(request, helperPid, error)) {
        selfDeleteInProgress_ = false;
        SetStatus(L"Error: " + error, true);
        gf::LogLine(L"self-delete failed: %s", error.c_str());
        return;
    }

    gf::LogLine(L"self-delete: helper %lu started (mode=%s)", helperPid,
                gf::SelfDeleteModeKey(request.mode));
    ::DestroyWindow(hwnd_);
}

void App::FocusWindowSearch() {
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::SetFocus(search);
    }
}

void App::ApplySelfExclude() {
    noSelfExclude_ = settings_.ui.noSelfExclude;
    ::SetWindowDisplayAffinity(hwnd_, noSelfExclude_ ? 0x00 : 0x11);
}

void App::RestoreAllTargets() {
    pendingCommand_ = PendingCommand{};
    capture_.RestoreAll();
    SetStatus(L"All targets restored");
    InvalidateAll();
}

void App::Invalidate() { InvalidateAll(); }

void App::SetStatus(std::wstring text, bool error) { SetStatusInternal(std::move(text), error); }

void App::SetStatusInternal(std::wstring text, bool error) {
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