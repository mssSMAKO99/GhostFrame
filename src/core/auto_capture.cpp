#include "auto_capture.h"

#include "log.h"

namespace gf {

namespace {

constexpr DWORD kMinScanIntervalMs = 400;

bool Enabled(const AutoAttachRule& rule) {
    return rule.enabled && !rule.value.empty();
}

}  // namespace

void AutoCaptureEngine::SetSettings(const Settings& settings) {
    rules_ = settings.autoAttach;
}

void AutoCaptureEngine::SetSticky(const std::vector<StickyTarget>& sticky) {
    sticky_ = sticky;
}

bool AutoCaptureEngine::MatchesSticky(const std::wstring& exePath,
                                      const std::wstring& className) const {
    for (const StickyTarget& target : sticky_) {
        if (StickyMatches(target, exePath, className)) {
            return true;
        }
    }
    return false;
}

bool AutoCaptureEngine::HasEnabledRules() const {
    for (const AutoAttachRule& rule : rules_) {
        if (Enabled(rule)) {
            return true;
        }
    }
    return false;
}

int AutoCaptureEngine::Scan(HWND exclude, bool force) {
    if (!HasEnabledRules() && sticky_.empty()) {
        return 0;
    }

    const ULONGLONG now = ::GetTickCount64();
    if (!force && now - lastScan_ < kMinScanIntervalMs) {
        return 0;
    }
    lastScan_ = now;

    windows_.Refresh(exclude);

    int attached = 0;
    for (const WindowInfo& window : windows_.windows()) {
        if (window.hwnd == nullptr || capture_.IndexOf(window.hwnd) >= 0) {
            continue;
        }
        if (window.arch == Arch::Arm64) {
            continue;  // ARM64 targets are not supported
        }
        if (window.elevated) {
            continue;  // cannot inject into a higher integrity level
        }
        if (!guard_.Allow(window.pid, window.createTime, window.hwnd)) {
            continue;
        }

        const bool stickyMatch = MatchesSticky(window.exePath, window.className);
        bool matched = stickyMatch;
        bool matchedHide = false;
        for (const AutoAttachRule& rule : rules_) {
            if (RuleMatches(rule, window.processName, window.title)) {
                matched = true;
                matchedHide = matchedHide || rule.hide;
            }
        }
        if (!matched) {
            continue;
        }

        std::wstring reason;
        if (!ValidateTarget(window.hwnd, reason)) {
            LogLine(L"auto-capture skipped %s: %s", window.processName.c_str(), reason.c_str());
            continue;
        }

        std::wstring error;
        if (!capture_.Attach(window.hwnd, error)) {
            LogLine(L"auto-capture failed for %s: %s", window.processName.c_str(), error.c_str());
            guard_.NoteFailure(window.pid, window.createTime, window.hwnd);
            continue;
        }
        const int index = capture_.IndexOf(window.hwnd);
        if (index >= 0) {
            capture_.At(static_cast<size_t>(index)).sticky = stickyMatch;
            if (matchedHide) {
                std::wstring ignored;
                capture_.Send(static_cast<size_t>(index), Cmd::Hide, ignored);
            }
        }
        ++attached;
    }

    if (attached > 0) {
        LogLine(L"auto-capture: attached %d window(s)", attached);
    }
    return attached;
}

void AutoCaptureEngine::Suppress(DWORD pid, ULONGLONG createTime, HWND hwnd) {
    guard_.Suppress(pid, createTime, hwnd);
}

void AutoCaptureEngine::NoteFailure(DWORD pid, ULONGLONG createTime, HWND hwnd) {
    guard_.NoteFailure(pid, createTime, hwnd);
}

void AutoCaptureEngine::ClearGuard() {
    guard_.Clear();
}

}  // namespace gf