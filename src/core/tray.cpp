#include "tray.h"

#include <shellapi.h>

#include <cwchar>

namespace gf {

namespace {

NOTIFYICONDATAW MakeData(HWND owner, UINT callback, HICON icon, const std::wstring& tip) {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = callback;
    data.hIcon = icon;
    if (!tip.empty()) {
        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
    }
    return data;
}

}  // namespace

TrayIcon::~TrayIcon() { Remove(); }

bool TrayIcon::Add(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tip) {
    Remove();
    owner_ = owner;
    callback_ = callbackMessage;
    icon_ = icon;
    tip_ = tip;

    NOTIFYICONDATAW data = MakeData(owner_, callback_, icon_, tip_);
    visible_ = ::Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (visible_) {
        UpdateTip(tip_);
    }
    return visible_;
}

void TrayIcon::Remove() {
    if (!visible_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &data);
    visible_ = false;
}

void TrayIcon::ReAdd() {
    if (owner_ == nullptr) {
        return;
    }
    visible_ = false;
    Add(owner_, callback_, icon_, tip_);
}

void TrayIcon::UpdateTip(const std::wstring& tip) {
    tip_ = tip;
    if (!visible_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = 1;
    data.uFlags = NIF_TIP;
    wcsncpy_s(data.szTip, tip_.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!visible_) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
}

}  // namespace gf