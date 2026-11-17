#include "window_ops.h"

#include <shobjidl.h>

namespace gf::agent {

namespace {

constexpr DWORD kWdaExclude = 0x00000011;  // WDA_EXCLUDEFROMCAPTURE

constexpr LONG_PTR kFrameMask =
    WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;

constexpr LONG_PTR kTaskbarMask = WS_EX_APPWINDOW | WS_EX_TOOLWINDOW;

void FrameChanged(HWND hwnd) {
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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

    DWORD error = 0;

    if (on) {
        DWORD current = 0;
        if (::GetWindowDisplayAffinity(root_, &current)) {
            savedWda_ = current;
        } else {
            savedWda_ = 0;
        }
        haveWda_ = true;

        if (!::SetWindowDisplayAffinity(root_, kWdaExclude)) {
            error = ::GetLastError();
        }

        savedExStyle_ = ::GetWindowLongPtrW(root_, GWL_EXSTYLE);
        exStyleMask_ = kTaskbarMask;
        haveExStyle_ = true;

        LONG_PTR ex = savedExStyle_;
        LONG_PTR updated = (ex & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                           static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
        if (updated != ex) {
            ::SetWindowLongPtrW(root_, GWL_EXSTYLE, updated);
            ApplyFrame();
        }

        if (!taskbar.DeleteTab(root_)) {
            // Fallback for systems where the shell object is unavailable.
            ::ShowWindow(root_, SW_HIDE);
            ::ShowWindow(root_, SW_SHOWNOACTIVATE);
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

        if (!taskbar.AddTab(root_)) {
            ::ShowWindow(root_, SW_HIDE);
            ::ShowWindow(root_, SW_SHOWNOACTIVATE);
        }

        hidden_ = false;
    }

    return error;
}

DWORD WindowState::SetBorderless(bool on) {
    if (!root_ || !::IsWindow(root_)) {
        return ERROR_INVALID_WINDOW_HANDLE;
    }
    if (on == borderless_) {
        return 0;
    }

    const bool wasZoomed = ::IsZoomed(root_) != FALSE;
    if (wasZoomed) {
        ::ShowWindow(root_, SW_RESTORE);
    }

    if (on) {
        savedStyle_ = ::GetWindowLongPtrW(root_, GWL_STYLE);
        styleMask_ = kFrameMask;
        haveStyle_ = true;

        LONG_PTR style = savedStyle_ & ~styleMask_;
        ::SetWindowLongPtrW(root_, GWL_STYLE, style);
    } else if (haveStyle_) {
        LONG_PTR style = ::GetWindowLongPtrW(root_, GWL_STYLE);
        ::SetWindowLongPtrW(root_, GWL_STYLE, (style & ~styleMask_) | (savedStyle_ & styleMask_));
    }

    ApplyFrame();

    if (wasZoomed) {
        ::ShowWindow(root_, SW_MAXIMIZE);
    }

    borderless_ = on;
    return 0;
}

DWORD WindowState::Enforce() {
    if (!root_ || !::IsWindow(root_)) {
        return ERROR_INVALID_WINDOW_HANDLE;
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

    if (borderless_) {
        LONG_PTR style = ::GetWindowLongPtrW(root_, GWL_STYLE);
        if ((style & styleMask_) != 0) {
            ::SetWindowLongPtrW(root_, GWL_STYLE, style & ~styleMask_);
            ApplyFrame();
        }
    }

    return error;
}

DWORD WindowState::RestoreAll(Taskbar& taskbar) {
    DWORD error = 0;
    if (borderless_) {
        error = SetBorderless(false);
    }
    if (hidden_) {
        DWORD hiddenError = SetHidden(false, taskbar);
        if (error == 0) {
            error = hiddenError;
        }
    }
    return error;
}

}  // namespace gf::agent
