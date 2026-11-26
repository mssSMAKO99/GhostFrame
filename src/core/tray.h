// Notification-area icon, owned by the daemon.
//
// The UI can be closed while capture keeps running, so the tray icon has to
// belong to the daemon.  This wrapper only manages the Shell_NotifyIcon
// lifecycle; the menu is built by the owner (it needs the capture snapshot).
#pragma once

#include <windows.h>

#include <string>

namespace gf {

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Adds the icon; `icon` stays owned by the caller.  Returns false when the
    // shell refuses it (for example while Explorer is restarting).
    bool Add(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tip);
    void Remove();
    // Re-creates the icon after Explorer broadcasts TaskbarCreated.
    void ReAdd();
    void UpdateTip(const std::wstring& tip);
    // Shows a balloon notification (used for the two-step self-delete warning).
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

    bool visible() const { return visible_; }

private:
    HWND owner_ = nullptr;
    UINT callback_ = 0;
    HICON icon_ = nullptr;
    std::wstring tip_;
    bool visible_ = false;
};

}  // namespace gf