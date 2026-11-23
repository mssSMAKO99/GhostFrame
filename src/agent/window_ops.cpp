#include "window_ops.h"

#include <shobjidl.h>

namespace gf::agent {

namespace {

constexpr DWORD kWdaExclude = 0x00000011;  // WDA_EXCLUDEFROMCAPTURE

constexpr LONG_PTR kTaskbarMask = WS_EX_APPWINDOW | WS_EX_TOOLWINDOW;

void FrameChanged(HWND hwnd) {
    // Asynchronous flavor: a hung target UI thread must never block the agent
    // worker while it refreshes the (taskbar) frame bits.
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_ASYNCWINDOWPOS | SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

}  // namespace

void Taskbar::Init() {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(hr)) {
        com_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        // Someone else already initialised COM for this thread; the in-proc
        // shell object still works, so keep going.
        com_ = false;
    } else {
        return;
    }

    ITaskbarList* list = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&list)))) {
        if (SUCCEEDED(list->HrInit())) {
            list_ = list;
        } else {
            list->Release();
        }
    }
}

void Taskbar::Shutdown() {
    if (list_) {
        list_->Release();
        list_ = nullptr;
    }
    if (com_) {
        ::CoUninitialize();
        com_ = false;
    }
}

bool Taskbar::DeleteTab(HWND hwnd) {
    if (!list_) {
        return false;
    }
    return SUCCEEDED(list_->DeleteTab(hwnd));
}

bool Taskbar::AddTab(HWND hwnd) {
    if (!list_) {
        return false;
    }
    return SUCCEEDED(list_->AddTab(hwnd));
}

DWORD WindowState::CurrentWda() const {
    DWORD value = 0;
    if (root_ && ::GetWindowDisplayAffinity(root_, &value)) {
        return value;
    }
    return 0;
}

void WindowState::ApplyFrame() const {
    if (root_) {
        FrameChanged(root_);
    }
}

DWORD WindowState::SetHidden(bool on, Taskbar& taskbar) {
    if (!root_ || !::IsWindow(root_)) {
        return ERROR_INVALID_WINDOW_HANDLE;
    }
    if (on == hidden_) {
        return 0;
    }

    // Changing the ex-style cross-thread sends messages to the target's UI
    // thread and blocks while that thread is hung.  Skip this tick and leave
    // the state alone; the next tick retries once the window responds again.
    if (::IsHungAppWindow(root_)) {
        return 0;
    }

    DWORD error = 0;

    if (on) {
        DWORD current = 0;
        if (::GetWindowDisplayAffinity(root_, &current)) {
            savedWda_ = current;
        } else {
            savedWda_ = 0;
        }

        // 2.2: never apply a partial hide.  If the exclusion failed, leave the
        // window exactly as it was instead of touching styles or the taskbar.
        if (!::SetWindowDisplayAffinity(root_, kWdaExclude)) {
            return ::GetLastError();
        }
        haveWda_ = true;

        savedExStyle_ = ::GetWindowLongPtrW(root_, GWL_EXSTYLE);
        exStyleMask_ = kTaskbarMask;
        haveExStyle_ = true;
        // APPWINDOW forces a taskbar button even when TOOLWINDOW is also set.
        hadButton_ = (savedExStyle_ & WS_EX_TOOLWINDOW) == 0 ||
                     (savedExStyle_ & WS_EX_APPWINDOW) != 0;

        LONG_PTR ex = savedExStyle_;
        LONG_PTR updated = (ex & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                           static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
        if (updated != ex) {
            ::SetWindowLongPtrW(root_, GWL_EXSTYLE, updated);
            ApplyFrame();
        }

        // 1.5: only remove a button that existed, and report a missing shell
        // object as an error instead of hiding/showing the window.
        if (hadButton_) {
            if (taskbar.DeleteTab(root_)) {
                tabRemoved_ = true;
            } else if (error == 0) {
                error = ERROR_NOT_READY;
            }
        }

        hidden_ = true;
    } else {
        if (haveWda_) {
            if (!::SetWindowDisplayAffinity(root_, savedWda_) && error == 0) {
                error = ::GetLastError();
            }
        }

        if (haveExStyle_) {
            LONG_PTR ex = ::GetWindowLongPtrW(root_, GWL_EXSTYLE);
            LONG_PTR updated = (ex & ~exStyleMask_) | (savedExStyle_ & exStyleMask_);
            if (updated != ex) {
                ::SetWindowLongPtrW(root_, GWL_EXSTYLE, updated);
                ApplyFrame();
            }
        }

        // 1.5: restore the button only when the hide actually removed it.
        if (tabRemoved_) {
            if (taskbar.AddTab(root_)) {
                tabRemoved_ = false;
            } else if (error == 0) {
                error = ERROR_NOT_READY;
            }
        }

        hidden_ = false;
    }

    return error;
}

DWORD WindowState::Enforce() {
    if (!root_ || !::IsWindow(root_)) {
        return ERROR_INVALID_WINDOW_HANDLE;
    }

    // Do not touch a window whose UI thread is hung: cross-thread style
    // operations would block the worker.  Not an error, just a skipped tick.
    if (::IsHungAppWindow(root_)) {
        return 0;
    }

    DWORD error = 0;

    if (hidden_) {
        DWORD current = 0;
        if (!::GetWindowDisplayAffinity(root_, &current) || current != kWdaExclude) {
            if (!::SetWindowDisplayAffinity(root_, kWdaExclude)) {
                error = ::GetLastError();
            }
        }

        LONG_PTR ex = ::GetWindowLongPtrW(root_, GWL_EXSTYLE);
        if ((ex & kTaskbarMask) != static_cast<LONG_PTR>(WS_EX_TOOLWINDOW)) {
            ::SetWindowLongPtrW(root_, GWL_EXSTYLE,
                                (ex & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                                    static_cast<LONG_PTR>(WS_EX_TOOLWINDOW));
            ApplyFrame();
        }
    }

    return error;
}

DWORD WindowState::RestoreAll(Taskbar& taskbar) {
    DWORD error = 0;
    if (hidden_) {
        error = SetHidden(false, taskbar);
    }

    // Forget the saved state so the next hide re-reads the live values.
    haveWda_ = false;
    savedWda_ = 0;
    haveExStyle_ = false;
    savedExStyle_ = 0;
    exStyleMask_ = 0;
    hadButton_ = false;
    tabRemoved_ = false;

    return error;
}

}  // namespace gf::agent
