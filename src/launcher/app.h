#pragma once

#include <windows.h>

#include <string>

#include "injector.h"
#include "session.h"
#include "settings.h"
#include "ui.h"
#include "window_list.h"

// A command that timed out and is retried from the refresh timer instead of
// blocking the UI thread for another full wait.
struct PendingCommand {
    HWND hwnd = nullptr;
    gf::Cmd cmd = gf::Cmd::Hide;
    ULONGLONG nextTry = 0;
    int attempts = 0;
};

class App {
public:
    enum class InitResult { Ok, AlreadyRunning, Failed };

    App() : sessions_(injector_) {}

    InitResult Init(HINSTANCE instance, bool noSelfExclude);
    int Run(int showCommand);
    void Shutdown();

private:
    enum class Tab { Targets, Windows, Settings };
    enum class SettingsSection { General, Hotkeys, AutoCapture, Advanced };
    enum class HotZone {
        None,
        Hide,
        Maximize,
        Close,
        Log,
        Cta,
        Secondary,
        EmptyCta,
        SettingsManage,
        SettingsLog,
        SettingsRestore,
    };

    // Chrome geometry computed once per resize/DPI change and shared by
    // Layout() (child controls) and PaintInto() (everything drawn by hand), so
    // the two can never drift apart.
    struct LayoutInfo {
        RECT topBar = {};
        RECT sidebar = {};
        RECT content = {};
        RECT logoMark = {};
        RECT tabs[3] = {};
        RECT windowButtons[3] = {};  // hide to tray, maximize, close
        RECT pageHeader = {};
        RECT pageIcon = {};
        RECT pageTitle = {};
        RECT pageSubtitle = {};
        RECT cta = {};
        RECT secondaryCta = {};
        RECT searchBox = {};
        RECT toolbar = {};
        RECT list = {};
        RECT footer = {};
        RECT footerLine = {};
        RECT logLink = {};
        RECT emptyCard = {};
        RECT emptyArt = {};
        RECT emptyText = {};
        RECT emptyCta = {};
        RECT settingsNav = {};
        RECT settingsNavItems[4] = {};
        RECT settingsArea = {};
        RECT settingsCard[3] = {};
        RECT settingsHotkeyList = {};
        RECT settingsRuleList = {};
        RECT settingsManage = {};
        RECT settingsActionRow = {};
        RECT settingsLogCta = {};
        RECT settingsRestoreCta = {};
        int tabCount = 3;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI AttachThreadProc(LPVOID param);
    static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR id, DWORD_PTR ref);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnClose();
    void OnPaint();
    void OnSize();
    void OnTimer();
    void OnCommand(int id);
    void OnHotkey(int id);
    void OnTray(LPARAM lParam);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
    void OnSettingChange();
    void OnAttachDone(LPARAM lParam);
    void OnAttachProgress(WPARAM done, LPARAM target);
    void OnAutoCaptureMessage(HWND hwnd);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnMouseWheel(int x, int y, int delta);
    void OnLButtonDown(int x, int y, bool ctrl, bool shift);
    void OnLButtonDblClk(int x, int y);
    void OnRButtonDown(int x, int y);
    void OnKeyDown(int virtualKey);
    void OnVScroll(int code, int position);
    void OnFilterChanged();
    void OnButtonHover(HWND button, bool hovered);

    void Layout();
    LayoutInfo ComputeLayout() const;
    void PaintInto(HDC dc, const RECT& client);
    void PaintTopBar(HDC dc, const RECT& client);
    void PaintSidebar(HDC dc, const RECT& client);
    void PaintPageChrome(HDC dc);
    void PaintTargetsPage(HDC dc);
    void PaintWindowsPage(HDC dc);
    void PaintSettingsPage(HDC dc);
    void PaintFooter(HDC dc);
    void PaintEmptyTargets(HDC dc);
    void PaintSettingsCard(HDC dc, const RECT& card, ui::Icon icon, const wchar_t* title,
                           const wchar_t* subtitle);
    void PaintSwitchRow(HDC dc, const RECT& rect, const wchar_t* title, const wchar_t* subtitle,
                        bool on, bool hovered);
    void PaintCta(HDC dc, const RECT& rect, const wchar_t* label, ui::Icon icon,
                  ui::Icon trailing, bool primary, bool hovered);
    RECT SwitchRowRect(const RECT& card, int index) const;
    void CreateChildren();
    void SetupTooltips();
    void RelayTooltip(HWND button, UINT message, WPARAM wParam, LPARAM lParam);
    void RegisterHotkeys();
    void UnregisterHotkeys();

    // Settings: persistence, hotkeys and auto-capture.
    void LoadSettings();
    void SaveSettings();
    void CaptureGeometry();
    void MarkSettingsDirty();
    bool ApplyHotkeys(const gf::Settings& candidate, std::wstring& error);
    int HotkeyCommandId(gf::HotkeyAction action) const;
    void ResetHotkeysToDefaults();
    void BeginRebind(gf::HotkeyAction action);
    void FinishRebind(UINT modifiers, UINT virtualKey);

    void ScanAutoCapture();
    void ApplyAutoRulesNow();
    void AddRuleForWindow(HWND hwnd);
    void AddRuleForExePath(const std::wstring& path);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD thread, DWORD time);

    void BuildSettingsRows();
    void OnSettingsListAction(ui::ListWidget::Action action, bool hotkeyList);
    void SetSettingsSection(SettingsSection section);

    void SelectTab(Tab tab);
    void ApplyTabControls();
    void RefreshWindows();
    void BuildTargetRows();
    void BuildWindowRows();
    void ApplyListAction(ui::ListWidget::Action action, bool windowsTab);
    void ApplyRowAction(int row, int actionId);
    void AttachSelectedWindow();
    void AttachProcessWindows(HWND sample);
    RECT TabRect(int index) const;

    void SelectTarget(int index);
    void SetSelected(int index);
    std::vector<size_t> SelectedTargetIndices() const;
    void AttachForeground(bool hideAfter);
    void StartAttachBatch(std::vector<HWND> targets, bool hideAfter);
    void CancelAttach();
    void UpdateAttachButton();
    void JoinAttachWorker();
    void DrainAttachResults();
    gf::SendOutcome SendCommand(int index, gf::Cmd cmd, std::wstring& error);
    void RetryPendingCommand();
    void ToggleStealth(int index);
    void ToggleSelection();
    void RestoreAll();
    void TargetShow(int command);
    void TargetClose();
    void RemoveSelected();
    void ShowUi(bool show);

    void AddTray();
    void RemoveTray();
    void UpdateTray();
    void OpenLogFolder();

    void SetStatus(std::wstring text, bool error = false);
    void ShowToast(std::wstring text, bool error);
    void ShowError(const std::wstring& text);
    void InvalidateAll() { ::InvalidateRect(hwnd_, nullptr, FALSE); }

    HotZone ZoneAt(int x, int y) const;
    int TabAt(int x, int y) const;
    int SettingsNavAt(int x, int y) const;
    int BehaviorSwitchAt(int x, int y) const;
    void ActivateZone(HotZone zone);

    bool SelectedValid() const {
        return selected_ >= 0 && static_cast<size_t>(selected_) < sessions_.Count();
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE mutex_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;

    ui::Theme theme_;
    gf::Injector injector_;
    gf::SessionManager sessions_;
    gf::WindowEnumerator windowEnumerator_;

    ui::ListWidget targetsList_;
    ui::ListWidget windowsList_;

    std::wstring status_;
    std::wstring windowFilter_;
    bool statusError_ = false;
    bool noSelfExclude_ = false;
    bool tray_ = false;
    int selected_ = -1;
    Tab tab_ = Tab::Targets;
    SettingsSection settingsSection_ = SettingsSection::General;
    HotZone hotZone_ = HotZone::None;
    int hoverTab_ = -1;
    int hoverNav_ = -1;
    int hoverSwitch_ = -1;
    bool trackingMouse_ = false;
    HBRUSH editBrush_ = nullptr;
    LayoutInfo layout_;
    HWND tooltip_ = nullptr;
    int hoveredButton_ = 0;

    // Transient message drawn over the window for a few seconds (errors and
    // notable events), independent of the persistent status bar.
    std::wstring toast_;
    bool toastError_ = false;
    ULONGLONG toastUntil_ = 0;

    // Background attach: the UI thread never runs the injection itself.  A batch
    // job walks its target list on the worker thread, reports progress and posts
    // one completion; the UI thread stays the only owner of SessionManager.
    HANDLE attachThread_ = nullptr;
    volatile LONG attachCancel_ = 0;
    bool attachHide_ = false;
    int attachTotal_ = 0;
    PendingCommand pendingCommand_;

    // Persistent configuration and the auto-capture engine.
    gf::Settings settings_;
    bool settingsDirty_ = false;
    ULONGLONG settingsSaveDue_ = 0;
    HWINEVENTHOOK winEventHook_ = nullptr;
    gf::AutoCaptureGuard autoGuard_;
    ULONGLONG autoLastScan_ = 0;
    struct AutoRequest {
        HWND hwnd = nullptr;
        DWORD pid = 0;
        ULONGLONG createTime = 0;
    };
    std::vector<AutoRequest> autoRequests_;

    // SETTINGS tab.
    ui::ListWidget hotkeysList_;
    ui::ListWidget rulesList_;
    int selectedRule_ = -1;
    bool rebinding_ = false;
    gf::HotkeyAction rebindAction_ = gf::HotkeyAction::Count;
};