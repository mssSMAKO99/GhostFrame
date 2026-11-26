// Auto-capture engine, owned by the daemon.
//
// It scans the desktop for windows that match the user's rules and attaches the
// new ones to the capture service.  It lives in the core so the daemon can run
// it with the UI closed; the UI only edits the rules through IPC.
#pragma once

#include <windows.h>

#include <vector>

#include "capture_service.h"
#include "settings.h"
#include "window_list.h"

namespace gf {

class AutoCaptureEngine {
public:
    explicit AutoCaptureEngine(CaptureService& capture) : capture_(capture) {}

    // Replaces the rule set.  Cheap to call on every settings change.
    void SetSettings(const Settings& settings);
    // Replaces the sticky identities; those windows are re-attached whenever
    // they appear, independently of the auto-capture rules.
    void SetSticky(const std::vector<StickyTarget>& sticky);

    bool HasEnabledRules() const;

    // Scans once and attaches every new matching window.  Returns how many
    // windows were attached.  `force` bypasses the minimum interval between
    // scans (used by "Apply now").
    int Scan(HWND exclude, bool force);

    // Anti-loop bookkeeping for windows the user removed by hand, or that keep
    // failing to attach.
    void Suppress(DWORD pid, ULONGLONG createTime, HWND hwnd);
    void NoteFailure(DWORD pid, ULONGLONG createTime, HWND hwnd);
    void ClearGuard();

private:
    bool MatchesSticky(const std::wstring& exePath, const std::wstring& className) const;

    CaptureService& capture_;
    WindowEnumerator windows_;
    AutoCaptureGuard guard_;
    std::vector<AutoAttachRule> rules_;
    std::vector<StickyTarget> sticky_;
    ULONGLONG lastScan_ = 0;
};

}  // namespace gf