#include "window_list.h"

#include <shellapi.h>

#include <utility>

namespace gf {

namespace {

struct EnumContext {
    WindowEnumerator* self = nullptr;
    HWND exclude = nullptr;
    std::map<DWORD, ProcessInfo> processes;
};

bool IsShellWindow(HWND hwnd) {
    if (hwnd == ::GetShellWindow() || hwnd == ::GetDesktopWindow()) {
        return true;
    }
    wchar_t className[128] = {};
    ::GetClassNameW(hwnd, className, _countof(className) - 1);
    static const wchar_t* const kExcluded[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd",
                                                L"Shell_SecondaryTrayWnd"};
    for (const wchar_t* name : kExcluded) {
        if (_wcsicmp(className, name) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::wstring IconKey(const std::wstring& exePath, HWND hwnd) {
    if (!exePath.empty()) {
        return exePath;
    }
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"#%p", hwnd);
    return buffer;
}

}  // namespace

WindowEnumerator::~WindowEnumerator() {
    for (const auto& entry : icons_) {
        if (entry.second != nullptr) {
            ::DestroyIcon(entry.second);
        }
    }
}

const wchar_t* WindowEnumerator::ArchLabel(Arch arch) {
    switch (arch) {
        case Arch::X86:
            return L"x86";
        case Arch::X64:
            return L"x64";
        case Arch::Arm64:
            return L"ARM64";
        default:
            return L"other";
    }
}

const std::wstring& WindowEnumerator::ProcessPath(DWORD pid) {
    const auto cached = paths_.find(pid);
    if (cached != paths_.end()) {
        return cached->second;
    }

    std::wstring path;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr) {
        wchar_t buffer[1024] = {};
        DWORD size = _countof(buffer);
        if (::QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            path.assign(buffer, size);
        }
        ::CloseHandle(process);
    }

    return paths_.emplace(pid, std::move(path)).first->second;
}

HICON WindowEnumerator::CacheIcon(const std::wstring& key, const std::wstring& exePath, HWND hwnd) {
    const auto cached = icons_.find(key);
    if (cached != icons_.end()) {
        return cached->second;
    }

    HICON icon = nullptr;
    if (hwnd != nullptr) {
        DWORD_PTR result = 0;
        const UINT flags = SMTO_ABORTIFHUNG | SMTO_BLOCK;
        // WM_GETICON is sent with a short timeout: a hung target must not stall
        // the whole enumeration.
        if (::SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, flags, 50, &result) != 0 &&
            result != 0) {
            icon = reinterpret_cast<HICON>(result);
        } else if (::SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, flags, 50, &result) != 0 &&
                   result != 0) {
            icon = reinterpret_cast<HICON>(result);
        } else if (::SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, flags, 50, &result) != 0 &&
                   result != 0) {
            icon = reinterpret_cast<HICON>(result);
        }
    }

    if (icon != nullptr) {
        // The window may vanish at any time; keep our own copy.
        icon = ::CopyIcon(icon);
    } else if (!exePath.empty()) {
        SHFILEINFOW info = {};
        if (::SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info),
                             SHGFI_ICON | SHGFI_SMALLICON) != 0) {
            icon = info.hIcon;
        }
    }

    return icons_.emplace(key, icon).first->second;
}

HICON WindowEnumerator::IconForWindow(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd)) {
        return nullptr;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return nullptr;
    }
    const std::wstring& path = ProcessPath(pid);
    return CacheIcon(IconKey(path, hwnd), path, hwnd);
}

BOOL CALLBACK WindowEnumerator::EnumProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<EnumContext*>(param);

    if (hwnd == context->exclude || !::IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
    }
    if (::GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    if (IsShellWindow(hwnd)) {
        return TRUE;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        return TRUE;
    }

    ProcessInfo info;
    const auto cachedInfo = context->processes.find(pid);
    if (cachedInfo != context->processes.end()) {
        info = cachedInfo->second;
    } else {
        std::wstring ignored;
        if (!DescribeProcess(pid, info, ignored)) {
            info = ProcessInfo{};
            info.pid = pid;
        }
        context->processes.emplace(pid, info);
    }

    const std::wstring& path = context->self->ProcessPath(pid);

    WindowInfo window;
    window.hwnd = hwnd;
    window.pid = pid;
    window.exePath = path;
    window.processName = FileNameOf(path);

    wchar_t title[512] = {};
    ::GetWindowTextW(hwnd, title, _countof(title) - 1);
    window.title = title;

    wchar_t className[128] = {};
    ::GetClassNameW(hwnd, className, _countof(className) - 1);
    window.className = className;

    window.arch = info.arch;
    window.integrity = info.integrity;
    window.createTime = info.createTime;
    window.elevated = context->self->selfIntegrity_ != 0 && info.integrity != 0 &&
                      info.integrity > context->self->selfIntegrity_;
    window.uwp = _wcsicmp(window.processName.c_str(), L"ApplicationFrameHost.exe") == 0 ||
                 _wcsicmp(window.className.c_str(), L"ApplicationFrameWindow") == 0;
    window.icon = context->self->CacheIcon(IconKey(path, hwnd), path, hwnd);

    context->self->windows_.push_back(std::move(window));
    return TRUE;
}

void WindowEnumerator::Refresh(HWND exclude) {
    windows_.clear();
    paths_.clear();

    ProcessInfo self;
    std::wstring ignored;
    if (DescribeProcess(::GetCurrentProcessId(), self, ignored)) {
        selfIntegrity_ = self.integrity;
    } else {
        selfIntegrity_ = 0;
    }

    EnumContext context;
    context.self = this;
    context.exclude = exclude;
    ::EnumWindows(&WindowEnumerator::EnumProc, reinterpret_cast<LPARAM>(&context));
}

}  // namespace gf
