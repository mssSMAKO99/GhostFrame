#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "injector.h"

namespace gf {

// One top-level window that can be shown on the WINDOWS tab.
struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring className;
    std::wstring processName;  // executable file name, e.g. "notepad.exe"
    std::wstring exePath;
    DWORD pid = 0;
    Arch arch = Arch::Other;
    DWORD integrity = 0;
    bool elevated = false;  // runs at a higher integrity level than GhostFrame
    bool uwp = false;
    HICON icon = nullptr;  // owned by the enumerator's icon cache
};

// Enumerates the windows of the current desktop and caches their icons by
// executable path.  It is deliberately independent from the UI: the caller
// decides when to refresh (tab activation, F5, a slow timer), never the 250 ms
// state timer.
class WindowEnumerator {
public:
    WindowEnumerator() = default;
    ~WindowEnumerator();
    WindowEnumerator(const WindowEnumerator&) = delete;
    WindowEnumerator& operator=(const WindowEnumerator&) = delete;

    // `exclude` is the GhostFrame window; windows of our own process are
    // skipped as well.
    void Refresh(HWND exclude);

    const std::vector<WindowInfo>& windows() const { return windows_; }

    // Icon for an already tracked window, looked up through the same cache.
    HICON IconForWindow(HWND hwnd);

    static const wchar_t* ArchLabel(Arch arch);

private:
    struct CaseInsensitiveLess {
        bool operator()(const std::wstring& left, const std::wstring& right) const {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        }
    };

    HICON CacheIcon(const std::wstring& key, const std::wstring& exePath, HWND hwnd);
    const std::wstring& ProcessPath(DWORD pid);
    static BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param);

    std::vector<WindowInfo> windows_;
    std::map<std::wstring, HICON, CaseInsensitiveLess> icons_;
    std::map<DWORD, std::wstring> paths_;
    DWORD selfIntegrity_ = 0;
};

}  // namespace gf
