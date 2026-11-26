#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "ipc/remote_capture.h"
#include "settings.h"
#include "ui.h"
#include "view.h"
#include "window_list.h"

// A command that timed out and is retried from the refresh timer instead of
// blocking the UI thread for another full wait.
struct PendingCommand {
    HWND hwnd = nullptr;
    gf::Cmd cmd = gf::Cmd::Hide;
    ULONGLONG nextTry = 0;
    int attempts = 0;
};

// The UI shell.
//
// App owns the window, the frame, the theme, the layout, the status/toast area
// and the shared state.  It does not know what a tab looks like: the tabs live
// in IView implementations listed by the compile-time registry, and the shell
// routes paint and input to the active one.
class App final : public gf::ViewHost {
public:
    enum class InitResult { Ok, AlreadyRunning, Failed };

    App() = default;

    InitResult Init(HINSTANCE instance, bool noSelfExclude);
    int Run(int showCommand);
    void Shutdown();

    // ViewHost
    HWND Window() const override { return hwnd_; }
    HINSTANCE Instance() const override { return instance_; }
    void AddButton(int id, const wchar_t* text, const wchar_t* tooltip) override;
    void AddSearchEdit(int id, const wchar_t* cue) override;
    void AddEdit(int id, const wchar_t* cue) override;
    ui::Theme& Theme() override { return theme_; }
    gf::LayoutInfo& Layout() override { return layout_; }
    gf::Settings& Settings() override { return settings_; }
    gf::RemoteCapture& Capture() override { return capture_; }
    gf::WindowEnumerator& Windows() override { return windowEnumerator_; }
    const std::vector<gf::ViewInfo>& ViewCatalog() const override { return viewCatalog_; }
    bool ApplyAppearance() override;

    void SetStatus(std::wstring text, bool error = false) override;
    void ShowToast(std::wstring text, bool error) override;
    void Invalidate() override;
    void MarkSettingsDirty() override;
    void SaveSettingsNow() override;
    bool ApplyHotkeys(const gf::Settings& candidate, std::wstring& error) override;

    void SelectView(const wchar_t* id) override;
    void FocusWindowSearch() override;
    void Relayout() override;
    void RelayoutInternal();

    gf::SendOutcome SendCommand(int index, gf::Cmd cmd, std::wstring& error) override;
    void RestoreAllTargets() override;

    bool AttachBusy() const override { return attachThread_ != nullptr; }
    void StartAttachBatch(std::vector<HWND> targets, bool hideAfter) override;
    void AttachForegroundTarget(bool hideAfter) override;
    void CancelAttach() override;

    void ApplySelfExclude() override;
    void OpenLogFolder() override;
    void RequestSelfDelete() override;

private:
    enum class HotZone {
        None,
        Hide,
        Maximize,
        Close,
        Log,
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI AttachThreadProc(LPVOID param);
    static LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR id, DWORD_PTR ref);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateViews();
    void SelectViewIndex(int index);
    gf::IView* ActiveView() const;

    // Rebinds the strip to the visible views in the saved order.
    void RebuildTabOrder();
    int StripIndexFor(int viewIndex) const;
    std::vector<std::wstring> ViewIds() const;
    void RefreshThemeFonts();
    bool ReloadBackdrop();
    void PaintBackdrop(HDC dc, const RECT& client);
    bool ApplyWindowIcons();

    void OnCreate();
    void OnDestroy();
    void OnClose();
    void OnPaint();
    void OnSize();
    void OnTimer();
    void OnCommand(int id);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
    void OnSettingChange();
    void OnAttachDone(LPARAM lParam);
    void OnAttachProgress(WPARAM done, LPARAM target);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnMouseWheel(int x, int y, int delta);
    void OnLButtonDown(int x, int y, bool ctrl, bool shift);
    void OnLButtonDblClk(int x, int y);
    void OnRButtonDown(int x, int y);
    void OnKeyDown(int virtualKey);
    void OnVScroll(int code, int position);
    void OnButtonHover(HWND button, bool hovered);

    void ApplyLayout();
    gf::LayoutInfo ComputeLayout() const;
    void ShowError(const std::wstring& text);
    void PaintInto(HDC dc, const RECT& client);
    void PaintTopBar(HDC dc, const RECT& client);
    void PaintSidebar(HDC dc, const RECT& client);
    void PaintPageChrome(HDC dc);
    void PaintFooter(HDC dc);
    void CreateChildren();
    void SetupTooltips();
    void AttachTooltip(HWND child, const wchar_t* text);
    void RelayTooltip(HWND button, UINT message, WPARAM wParam, LPARAM lParam);

    // Settings: persistence and the daemon-owned hotkeys/auto-capture.
    void LoadSettings();
    void SaveSettings();
    void CaptureGeometry();
    bool PushSettings(std::wstring* error);

    void JoinAttachWorker();
    void DrainAttachResults();
    void RetryPendingCommand();
    void ShowUi(bool show);

    // Self-delete confirmation and the detached cleanup hand-off.
    bool ConfirmSelfDelete() const;
    void BeginSelfDelete();

    void SetStatusInternal(std::wstring text, bool error);
    void InvalidateAll() { ::InvalidateRect(hwnd_, nullptr, FALSE); }

    HotZone ZoneAt(int x, int y) const;
    int TabAt(int x, int y) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE mutex_ = nullptr;

    ui::Theme theme_;
    gf::RemoteCapture capture_;
    gf::WindowEnumerator windowEnumerator_;
    std::vector<std::unique_ptr<gf::IView>> views_;
    std::vector<gf::ViewInfo> viewCatalog_;
    std::vector<int> tabOrder_;  // visible views in display order
    int activeIndex_ = -1;

    // Background picture from the appearance settings.  Null when the setting
    // is empty or the file could not be loaded.
    HBITMAP backdrop_ = nullptr;
    std::wstring backdropPath_;

    // Window/taskbar icons: the generated emoji icon when one is configured,
    // otherwise the bundled resource.  `owned` marks handles from LoadImage.
    HICON bigIcon_ = nullptr;
    HICON smallIcon_ = nullptr;
    bool bigIconOwned_ = false;
    bool smallIconOwned_ = false;

    std::wstring status_;
    bool statusError_ = false;
    bool noSelfExclude_ = false;
    HotZone hotZone_ = HotZone::None;
    int hoverTab_ = -1;
    bool trackingMouse_ = false;
    HBRUSH editBrush_ = nullptr;
    gf::LayoutInfo layout_;
    HWND tooltip_ = nullptr;
    int hoveredButton_ = 0;

    // Transient message drawn over the window for a few seconds (errors and
    // notable events), independent of the persistent status bar.
    std::wstring toast_;
    bool toastError_ = false;
    ULONGLONG toastUntil_ = 0;

    // Background attach: the UI thread never runs the injection itself.  A batch
    // job walks its target list on the worker thread, asks the daemon to attach
    // each one and posts one completion; the UI then re-reads a snapshot.
    HANDLE attachThread_ = nullptr;
    volatile LONG attachCancel_ = 0;
    bool attachHide_ = false;
    int attachTotal_ = 0;
    PendingCommand pendingCommand_;

    // Persistent configuration.  The UI renders and edits it, but in daemon mode
    // the daemon is the writer: each change is pushed over IPC.
    gf::Settings settings_;
    bool settingsDirty_ = false;
    ULONGLONG settingsSaveDue_ = 0;

    // True once the helper has been launched; OnClose must not prompt again.
    bool selfDeleteInProgress_ = false;
};