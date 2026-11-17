#pragma once

#include <windows.h>

#include <string>

#include "injector.h"
#include "session.h"
#include "ui.h"

class App {
public:
    enum class InitResult { Ok, AlreadyRunning, Failed };

    App() : sessions_(injector_) {}

    InitResult Init(HINSTANCE instance, bool noSelfExclude);
    int Run(int showCommand);
    void Shutdown();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void OnCreate();
    void OnDestroy();
    void OnPaint();
    void OnTimer();
    void OnCommand(int id);
    void OnHotkey(int id);
    void OnTray(LPARAM lParam);
    void OnDpiChanged(UINT dpi, const RECT* suggested);

    void Layout();
    void PaintInto(HDC dc, const RECT& client);
    void CreateChildren();
    void RegisterHotkeys();
    void UnregisterHotkeys();

    void SelectTarget(int index);
    void AttachForeground(bool hideAfter);
    void ToggleStealth(int index);
    void ToggleBorderless(int index);
    void RestoreAll();
    void TargetShow(int command);
    void TargetClose();
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

    std::wstring status_;
    bool statusError_ = false;
    bool noSelfExclude_ = false;
    bool tray_ = false;
    int selected_ = -1;
};
