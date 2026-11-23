#pragma once

#include <windows.h>

#include <string>

#include "injector.h"
#include "session.h"
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
    enum class Tab { Targets, Windows };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI AttachThreadProc(LPVOID param);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnClose();
    void OnPaint();
    void OnTimer();
    void OnCommand(int id);
    void OnHotkey(int id);
    void OnTray(LPARAM lParam);
    void OnDpiChanged(UINT dpi, const RECT* suggested);
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
    void OnFilterChanged();

    void Layout();
    void PaintInto(HDC dc, const RECT& client);
    void CreateChildren();
    void RegisterHotkeys();
    void UnregisterHotkeys();

    void SelectTab(Tab tab);
    void RefreshWindows();
    void BuildTargetRows();
    void BuildWindowRows();
    void ApplyListAction(ui::ListWidget::Action action, bool windowsTab);
    void AttachSelectedWindow();
    void AttachProcessWindows(HWND sample);
    RECT TabRect(int index) const;
    RECT ListRect() const;

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

    void SetStatus(std::wstring text, bool error = false);
    void ShowError(const std::wstring& text);
    void InvalidateAll() { ::InvalidateRect(hwnd_, nullptr, FALSE); }

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
    bool trackingMouse_ = false;
    HBRUSH editBrush_ = nullptr;

    // Background attach: the UI thread never runs the injection itself.  A batch
    // job walks its target list on the worker thread, reports progress and posts
    // one completion; the UI thread stays the only owner of SessionManager.
    HANDLE attachThread_ = nullptr;
    volatile LONG attachCancel_ = 0;
    bool attachHide_ = false;
    int attachTotal_ = 0;
    PendingCommand pendingCommand_;
};
