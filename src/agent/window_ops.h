#pragma once

#include <windows.h>

// Defined by <shobjidl.h> in the implementation file.  Declared at global
// scope on purpose: inside gf::agent this would introduce a new type.
struct ITaskbarList;

namespace gf::agent {

// Thin wrapper around the shell ITaskbarList object.  Used to add/remove the
// taskbar button without hiding and re-showing the window (which steals focus
// and can pause games).
class Taskbar {
public:
    Taskbar() = default;
    Taskbar(const Taskbar&) = delete;
    Taskbar& operator=(const Taskbar&) = delete;

    void Init();
    void Shutdown();

    // Returns false when the shell object is unavailable.  Callers must treat
    // that as an error and leave the window itself untouched: hiding/showing
    // the window as a fallback steals focus and can pause games.
    bool DeleteTab(HWND hwnd);
    bool AddTab(HWND hwnd);

private:
    ::ITaskbarList* list_ = nullptr;
    bool com_ = false;
};

// Applies and, more importantly, reverts every persistent change made to a
// target window.  All calls run inside the target process, which is the only
// place where SetWindowDisplayAffinity is allowed anyway.
class WindowState {
public:
    WindowState() = default;
    WindowState(const WindowState&) = delete;
    WindowState& operator=(const WindowState&) = delete;

    void Init(HWND root) { root_ = root; }

    bool IsAlive() const { return root_ && ::IsWindow(root_) != FALSE; }
    bool IsHidden() const { return hidden_; }

    DWORD SetHidden(bool on, Taskbar& taskbar);
    DWORD RestoreAll(Taskbar& taskbar);

    // Re-asserts the invariants while hidden.  Returns the last Win32 error.
    DWORD Enforce();

    DWORD CurrentWda() const;

private:
    void ApplyFrame() const;

    HWND root_ = nullptr;

    bool hidden_ = false;

    bool haveWda_ = false;
    DWORD savedWda_ = 0;

    bool haveExStyle_ = false;
    LONG_PTR savedExStyle_ = 0;
    LONG_PTR exStyleMask_ = 0;

    // Whether the window actually owned a taskbar button before we hid it.
    // Tool windows never do, so DeleteTab/AddTab are skipped for them.
    bool hadButton_ = false;

    // Whether DeleteTab succeeded, so AddTab is only called when something was
    // actually removed.
    bool tabRemoved_ = false;
};

}  // namespace gf::agent
