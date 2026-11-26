#include "daemon.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <stdio.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "auto_capture.h"
#include "autostart.h"
#include "branding.h"
#include "capture_service.h"
#include "emoji_icon.h"
#include "ghostframe/ipc_protocol.h"
#include "ghostframe/ui_messages.h"
#include "ghostframe/version.h"
#include "hotkeys.h"
#include "ipc/daemon_client.h"
#include "ipc/pipe_client.h"
#include "ipc/pipe_server.h"
#include "log.h"
#include "self_delete.h"
#include "settings.h"
#include "tray.h"

namespace gf {

namespace {

constexpr const wchar_t* kDaemonClass = kDaemonWindowClass;
constexpr wchar_t kDaemonMutex[] = L"Local\\GhostFrame.Daemon.Instance.v1";
constexpr UINT kDaemonStopMessage = WM_APP + 0x51;
constexpr UINT kAutoCaptureMessage = WM_APP + 0x52;
constexpr UINT kTrayMessage = WM_APP + 0x53;
constexpr UINT kApplySettingsMessage = WM_APP + 0x54;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kAutoCaptureTimer = 2;
constexpr UINT_PTR kSelfDeleteTimer = 3;
constexpr UINT kRefreshTickMs = 500;
constexpr UINT kAutoCaptureTickMs = 1000;
constexpr UINT kSelfDeleteArmMs = 4000;
constexpr int kAppIconId = 101;  // IDI_APP in the launcher resources

// Tray menu commands (private to the daemon; the UI uses its own ids).
constexpr UINT kMenuShow = 1;
constexpr UINT kMenuRestoreAll = 2;
constexpr UINT kMenuLog = 3;
constexpr UINT kMenuExit = 4;
constexpr UINT kMenuSelfDelete = 5;
constexpr UINT kMenuTargetBase = 100;
constexpr UINT kMenuTargetStride = 5;
constexpr UINT kMenuTargetToggle = 0;
constexpr UINT kMenuTargetRestore = 1;
constexpr UINT kMenuTargetClose = 2;
constexpr UINT kMenuTargetPrimary = 3;
constexpr UINT kMenuTargetSticky = 4;

struct DaemonState {
    std::mutex mutex;
    CaptureService capture;
    AutoCaptureEngine autoCapture{capture};
    Settings settings;
    HotkeyManager hotkeys;
    TrayIcon tray;
    HWND window = nullptr;
    HWINEVENTHOOK hook = nullptr;
    UINT taskbarCreated = 0;
    HICON icon = nullptr;
    bool iconOwned = false;
    // The two-step self-destruct action is armed between the first and second
    // trigger; the timer cancels it.
    bool selfDeleteArmed = false;
};

// Hotkeys are applied on the daemon's own thread: they were registered there,
// and re-registering from the pipe thread makes UnregisterHotKey fail silently,
// after which every RegisterHotKey looks like a conflict.
struct ApplySettingsContext {
    const Settings* settings = nullptr;
    bool ok = false;
    std::wstring error;
};

// SetWinEventHook has no user data; the daemon has one window, so a file-static
// pointer is enough to route the callback.
DaemonState* g_daemon = nullptr;

// The daemon logs to the same place as the UI: %LOCALAPPDATA%\GhostFrame, with a
// %TEMP% fallback for locked-down profiles.
std::wstring ResolveLogDirectory() {
    wchar_t localAppData[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
        std::wstring dir = std::wstring(localAppData) + L"\\GhostFrame";
        if (::CreateDirectoryW(dir.c_str(), nullptr) ||
            ::GetLastError() == ERROR_ALREADY_EXISTS) {
            return dir;
        }
    }

    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) != 0) {
        return std::wstring(temp) + L"GhostFrame";
    }
    return {};
}

void CopyError(const std::wstring& text, wchar_t* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    const size_t copy = text.size() < capacity - 1 ? text.size() : capacity - 1;
    if (copy > 0) {
        std::wmemcpy(out, text.c_str(), copy);
    }
    out[copy] = L'\0';
}

std::wstring ModulePath() {
    wchar_t module[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) {
        return std::wstring();
    }
    return module;
}

// --- UI window helpers ----------------------------------------------------

HWND FindUiWindow() { return ::FindWindowW(kUiWindowClass, nullptr); }

void StartUi() {
    const std::wstring module = ModulePath();
    if (module.empty()) {
        return;
    }
    std::wstring commandLine = L"\"" + module + L"\"";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (::CreateProcessW(module.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &startup, &process)) {
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
}

void ShowUi() {
    if (HWND ui = FindUiWindow()) {
        ::PostMessageW(ui, kUiShowMessage, 0, 0);
        return;
    }
    StartUi();
}

void ToggleUi() {
    if (HWND ui = FindUiWindow()) {
        ::PostMessageW(ui, kUiToggleMessage, 0, 0);
        return;
    }
    StartUi();
}

// --- capture actions shared by hotkeys and the tray -----------------------

size_t RefreshLocked(DaemonState& state) {
    state.capture.Refresh();
    return state.capture.Count();
}

void SendToAll(DaemonState& state, Cmd cmd) {
    std::vector<size_t> all;
    all.reserve(state.capture.Count());
    for (size_t i = 0; i < state.capture.Count(); ++i) {
        all.push_back(i);
    }
    state.capture.SendToMany(all, cmd);
}

bool HasStickyIdentity(const Settings& settings, const std::wstring& exePath,
                       const std::wstring& className) {
    for (const StickyTarget& target : settings.sticky) {
        if (StickyMatches(target, exePath, className)) {
            return true;
        }
    }
    return false;
}

// Re-applies the sticky flag to every tracked session.  The caller holds the
// capture lock (the only place the session list may be touched).
void ApplyStickyFlags(DaemonState& state) {
    for (size_t i = 0; i < state.capture.Count(); ++i) {
        Session& session = state.capture.At(i);
        session.sticky = HasStickyIdentity(state.settings, session.exePath, session.className);
    }
}

// Persists the sticky list and keeps the engine and the visible flags in sync.
void CommitSticky(DaemonState& state) {
    std::wstring error;
    if (!SaveSettings(SettingsPath(), state.settings, error)) {
        LogLine(L"daemon: cannot save sticky targets: %s", error.c_str());
    }
    state.autoCapture.SetSticky(state.settings.sticky);
    ApplyStickyFlags(state);
}

// A manually captured window becomes sticky unless its identity is already
// remembered; this is what makes the capture survive an app restart.
void RememberSticky(DaemonState& state, const Session& session) {
    if (session.exePath.empty() ||
        HasStickyIdentity(state.settings, session.exePath, session.className)) {
        return;
    }
    state.settings.sticky.push_back(StickyTarget{session.exePath, session.className});
    CommitSticky(state);
}

// Toggles the sticky identity of one tracked window.  Returns false when the
// window is not tracked.
bool SetSessionSticky(DaemonState& state, HWND hwnd, bool sticky) {
    const int index = state.capture.IndexOf(hwnd);
    if (index < 0) {
        return false;
    }
    const Session& session = state.capture.At(static_cast<size_t>(index));
    if (sticky) {
        if (!HasStickyIdentity(state.settings, session.exePath, session.className)) {
            state.settings.sticky.push_back(
                StickyTarget{session.exePath, session.className});
        }
    } else {
        const std::wstring exePath = session.exePath;
        const std::wstring className = session.className;
        state.settings.sticky.erase(
            std::remove_if(state.settings.sticky.begin(), state.settings.sticky.end(),
                           [&](const StickyTarget& target) {
                               return StickyMatches(target, exePath, className);
                           }),
            state.settings.sticky.end());
    }
    CommitSticky(state);
    return true;
}



void AttachForeground(DaemonState& state) {
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr || foreground == state.window) {
        return;
    }

    wchar_t className[128] = {};
    ::GetClassNameW(foreground, className, _countof(className) - 1);
    if (_wcsicmp(className, kUiWindowClass) == 0) {
        return;
    }

    const int existing = state.capture.IndexOf(foreground);
    if (existing >= 0) {
        // Already tracked: the shortcut simply retargets the primary instead of
        // attaching a second agent.
        state.capture.SetPrimary(foreground);
        return;
    }

    std::wstring error;
    if (!state.capture.Attach(foreground, error)) {
        LogLine(L"hotkey attach failed: %s", error.c_str());
        return;
    }

    const int index = state.capture.IndexOf(foreground);
    if (index < 0) {
        return;
    }
    state.capture.SetPrimary(foreground);
    // A manual capture is sticky: the identity is remembered so it is captured
    // again after the application restarts.
    RememberSticky(state, state.capture.At(static_cast<size_t>(index)));
    if (state.settings.ui.hideOnAttach) {
        std::wstring ignored;
        state.capture.Send(static_cast<size_t>(index), Cmd::Hide, ignored);
    }
}

void ToggleSelection(DaemonState& state) {
    const int primary = state.capture.PrimaryIndex();
    if (primary >= 0) {
        const bool hide = !state.capture.At(static_cast<size_t>(primary)).hidden();
        std::wstring ignored;
        state.capture.Send(static_cast<size_t>(primary), hide ? Cmd::Hide : Cmd::Show, ignored);
        return;
    }

    if (state.capture.Empty()) {
        AttachForeground(state);
        return;
    }

    bool anyVisible = false;
    for (size_t i = 0; i < state.capture.Count(); ++i) {
        if (!state.capture.At(i).hidden()) {
            anyVisible = true;
            break;
        }
    }
    SendToAll(state, anyVisible ? Cmd::Hide : Cmd::Show);
}

void ShowAll(DaemonState& state, int command) {
    for (size_t i = 0; i < state.capture.Count(); ++i) {
        const HWND target = state.capture.At(i).hwnd;
        if (::IsWindow(target)) {
            ::ShowWindow(target, command);
        }
    }
}

// Hotkey window commands act on the primary target when one is set; without a
// primary they keep the old "all targets" behavior.
void ShowPrimaryOrAll(DaemonState& state, int command) {
    const int primary = state.capture.PrimaryIndex();
    if (primary >= 0) {
        const HWND target = state.capture.At(static_cast<size_t>(primary)).hwnd;
        if (::IsWindow(target)) {
            ::ShowWindow(target, command);
            return;
        }
    }
    ShowAll(state, command);
}

void CloseAll(DaemonState& state) {
    for (size_t i = state.capture.Count(); i > 0; --i) {
        const Session& session = state.capture.At(i - 1);
        state.autoCapture.Suppress(session.pid, session.createTime, session.hwnd);
        if (::IsWindow(session.hwnd)) {
            ::PostMessageW(session.hwnd, WM_CLOSE, 0, 0);
        }
        state.capture.Remove(i - 1);
    }
}

void ClosePrimaryOrAll(DaemonState& state) {
    const int primary = state.capture.PrimaryIndex();
    if (primary < 0) {
        CloseAll(state);
        return;
    }
    const Session& session = state.capture.At(static_cast<size_t>(primary));
    state.autoCapture.Suppress(session.pid, session.createTime, session.hwnd);
    if (::IsWindow(session.hwnd)) {
        ::PostMessageW(session.hwnd, WM_CLOSE, 0, 0);
    }
    state.capture.Remove(static_cast<size_t>(primary));
}

// Follows the focused window while the feature is enabled.  Only an already
// tracked window (or one that becomes primary through capture) is adopted; the
// user's primary is never stolen by an unrelated application.  Our own windows
// are ignored.
void TrackForeground(DaemonState& state) {
    if (!state.settings.foregroundTracking) {
        return;
    }
    const HWND foreground = ::GetForegroundWindow();
    if (foreground == nullptr || foreground == state.window) {
        return;
    }
    wchar_t className[128] = {};
    ::GetClassNameW(foreground, className, _countof(className) - 1);
    if (_wcsicmp(className, kUiWindowClass) == 0) {
        return;
    }
    const int index = state.capture.IndexOf(foreground);
    if (index >= 0 && state.capture.PrimaryIndex() != index) {
        state.capture.SetPrimary(foreground);
        LogLine(L"foreground tracking: primary -> hwnd %p", foreground);
    }
}

void CancelSelfDeleteArm(DaemonState& state) {
    if (!state.selfDeleteArmed) {
        return;
    }
    state.selfDeleteArmed = false;
    ::KillTimer(state.window, kSelfDeleteTimer);
}

// Arms on the first call and performs the delete on the second.  Shared by the
// optional hotkey and the tray item so both ask twice.
void RequestSelfDelete(DaemonState& state) {
    SelfDeleteMode mode = SelfDeleteMode::Never;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        mode = state.settings.ui.selfDelete;
    }
    if (mode == SelfDeleteMode::Never) {
        state.tray.ShowBalloon(
            L"GhostFrame",
            L"Choose Exe only or Everything in Settings > Advanced before deleting.");
        return;
    }

    if (!state.selfDeleteArmed) {
        state.selfDeleteArmed = true;
        ::SetTimer(state.window, kSelfDeleteTimer, kSelfDeleteArmMs, nullptr);
        state.tray.ShowBalloon(
            L"Delete GhostFrame?",
            L"Trigger the self-destruct hotkey or this tray item again within 4 seconds. "
            L"Any other hotkey cancels.");
        return;
    }

    CancelSelfDeleteArm(state);

    SelfDeleteStartRequest request;
    request.mode = mode;
    request.executablePath = ModulePath();
    request.dataDirectory = SettingsDirectory();
    request.waitPids.push_back(::GetCurrentProcessId());

    if (HWND ui = FindUiWindow()) {
        DWORD uiPid = 0;
        ::GetWindowThreadProcessId(ui, &uiPid);
        if (uiPid != 0) {
            request.waitPids.push_back(uiPid);
        }
        ::PostMessageW(ui, kUiSelfDeleteMessage, 0, 0);
    }

    request.autostartValues.push_back(AutostartValueName());
    {
        BrandingManifest manifest;
        std::wstring manifestError;
        if (LoadBrandingManifest(manifest, manifestError) &&
            !manifest.autostartValue.empty()) {
            request.autostartValues.push_back(manifest.autostartValue);
        }
        if (!manifest.shortcutPath.empty()) {
            request.extraFiles.push_back(manifest.shortcutPath);
        }
    }

    std::wstring error;
    DWORD helperPid = 0;
    if (!StartSelfDeleteHelper(request, helperPid, error)) {
        LogLine(L"daemon self-delete: %s", error.c_str());
        state.tray.ShowBalloon(L"GhostFrame",
                               L"Could not start the cleanup helper: " + error);
        return;
    }

    LogLine(L"daemon self-delete: helper %lu started (mode=%s)", helperPid,
            SelfDeleteModeKey(mode));
    ::PostMessageW(state.window, kDaemonStopMessage, 0, 0);
}

void OnHotkey(DaemonState& state, int id) {
    const HotkeyAction action = HotkeyManager::ActionFromId(id);
    if (action == HotkeyAction::Count) {
        return;
    }
    LogLine(L"hotkey: action %d", static_cast<int>(action));

    if (action == HotkeyAction::SelfDestruct) {
        RequestSelfDelete(state);
        return;
    }

    // "Any other key cancels": another GhostFrame hotkey disarms the pending
    // self-destruct before running its own action.
    if (state.selfDeleteArmed) {
        CancelSelfDeleteArm(state);
        state.tray.ShowBalloon(L"GhostFrame", L"Self-delete cancelled.");
    }

    if (action == HotkeyAction::ToggleUi) {
        ToggleUi();
        return;
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    switch (action) {
        case HotkeyAction::ToggleSelection:
            ToggleSelection(state);
            break;
        case HotkeyAction::RestoreAll:
            state.capture.RestoreAll();
            break;
        case HotkeyAction::Minimize:
            ShowPrimaryOrAll(state, SW_MINIMIZE);
            break;
        case HotkeyAction::Restore:
            ShowPrimaryOrAll(state, SW_RESTORE);
            break;
        case HotkeyAction::Close:
            ClosePrimaryOrAll(state);
            break;
        default:
            break;
    }
}

void OnTray(DaemonState& state, LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowUi();
            return;
        case WM_RBUTTONUP:
            break;
        default:
            return;
    }

    // Copy what the menu needs, then release the capture lock before opening a
    // modal menu: the menu pumps messages, and holding the lock across that
    // could stall the pipe server or another handler.
    struct TrayTarget {
        HWND hwnd = nullptr;
        std::wstring title;
        bool hidden = false;
        bool primary = false;
        bool sticky = false;
    };
    std::vector<TrayTarget> targets;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.capture.Refresh();
        targets.reserve(state.capture.Count());
        for (size_t i = 0; i < state.capture.Count(); ++i) {
            const Session& session = state.capture.At(i);
            targets.push_back(TrayTarget{session.hwnd, session.title, session.hidden(),
                                         state.capture.IsPrimary(session), session.sticky});
        }
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    ::AppendMenuW(menu, MF_STRING, kMenuShow, L"Show GhostFrame");

    if (!targets.empty()) {
        HMENU submenu = ::CreatePopupMenu();
        for (size_t i = 0; i < targets.size() && submenu != nullptr; ++i) {
            HMENU sub = ::CreatePopupMenu();
            if (sub == nullptr) {
                continue;
            }
            const UINT base = kMenuTargetBase + static_cast<UINT>(i) * kMenuTargetStride;
            ::AppendMenuW(sub, MF_STRING, base + kMenuTargetToggle,
                          targets[i].hidden ? L"Show (lift exclusion)"
                                            : L"Hide (exclude from capture)");
            ::AppendMenuW(sub, MF_STRING, base + kMenuTargetRestore, L"Restore window");
            ::AppendMenuW(sub, MF_STRING, base + kMenuTargetClose, L"Close window");
            ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
            ::AppendMenuW(sub, MF_STRING | (targets[i].primary ? MF_CHECKED : 0),
                          base + kMenuTargetPrimary, L"Primary target");
            ::AppendMenuW(sub, MF_STRING | (targets[i].sticky ? MF_CHECKED : 0),
                          base + kMenuTargetSticky, L"Sticky (capture on reopen)");

            std::wstring label = targets[i].title.empty() ? L"(untitled)" : targets[i].title;
            if (label.size() > 48) {
                label.resize(48);
                label += L"\x2026";
            }
            if (targets[i].primary) {
                label = L"\x25CF " + label;  // PRIMARY marker
            }
            if (targets[i].sticky) {
                label += L"  [sticky]";
            }
            ::AppendMenuW(submenu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), label.c_str());
        }
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), L"Targets");
    }

    ::AppendMenuW(menu, MF_STRING, kMenuRestoreAll, L"Restore all targets");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuLog, L"Open log folder");
    ::AppendMenuW(menu, MF_STRING, kMenuSelfDelete,
                  state.selfDeleteArmed ? L"Delete GhostFrame now (confirm)"
                                        : L"Delete GhostFrame…");
    ::AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit GhostFrame");

    POINT point = {};
    ::GetCursorPos(&point);
    ::SetForegroundWindow(state.window);
    const UINT command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0,
                                          state.window, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(state.window, WM_NULL, 0, 0);

    if (command != kMenuSelfDelete && state.selfDeleteArmed) {
        CancelSelfDeleteArm(state);
        state.tray.ShowBalloon(L"GhostFrame", L"Self-delete cancelled.");
    }

    switch (command) {
        case kMenuShow:
            ShowUi();
            return;
        case kMenuRestoreAll: {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.capture.RestoreAll();
            return;
        }
        case kMenuLog: {
            const std::wstring& dir = LogDirectory();
            if (!dir.empty()) {
                ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        case kMenuSelfDelete:
            RequestSelfDelete(state);
            return;
        case kMenuExit: {
            if (HWND ui = FindUiWindow()) {
                ::PostMessageW(ui, kUiQuitMessage, 0, 0);
            }
            ::PostMessageW(state.window, kDaemonStopMessage, 0, 0);
            return;
        }
        default:
            break;
    }

    if (command < kMenuTargetBase) {
        return;
    }
    const UINT offset = command - kMenuTargetBase;
    const size_t index = offset / kMenuTargetStride;
    const UINT action = offset % kMenuTargetStride;
    if (index >= targets.size()) {
        return;
    }
    const HWND hwnd = targets[index].hwnd;

    switch (action) {
        case kMenuTargetToggle: {
            std::lock_guard<std::mutex> lock(state.mutex);
            const int target = state.capture.IndexOf(hwnd);
            if (target >= 0) {
                const bool hide = !state.capture.At(static_cast<size_t>(target)).hidden();
                std::wstring ignored;
                state.capture.Send(static_cast<size_t>(target), hide ? Cmd::Hide : Cmd::Show,
                                   ignored);
            }
            break;
        }
        case kMenuTargetRestore:
            if (::IsWindow(hwnd)) {
                ::ShowWindow(hwnd, SW_RESTORE);
            }
            break;
        case kMenuTargetClose: {
            std::lock_guard<std::mutex> lock(state.mutex);
            const int target = state.capture.IndexOf(hwnd);
            if (target >= 0) {
                const Session& session = state.capture.At(static_cast<size_t>(target));
                state.autoCapture.Suppress(session.pid, session.createTime, session.hwnd);
                if (::IsWindow(session.hwnd)) {
                    ::PostMessageW(session.hwnd, WM_CLOSE, 0, 0);
                }
                state.capture.Remove(static_cast<size_t>(target));
            }
            break;
        }
        case kMenuTargetPrimary: {
            std::lock_guard<std::mutex> lock(state.mutex);
            const int target = state.capture.IndexOf(hwnd);
            if (target >= 0) {
                if (state.capture.At(static_cast<size_t>(target)).id ==
                    state.capture.PrimaryId()) {
                    state.capture.ClearPrimary();
                } else {
                    state.capture.SetPrimary(hwnd);
                }
            }
            break;
        }
        case kMenuTargetSticky: {
            std::lock_guard<std::mutex> lock(state.mutex);
            const int target = state.capture.IndexOf(hwnd);
            if (target >= 0) {
                const bool sticky = !state.capture.At(static_cast<size_t>(target)).sticky;
                SetSessionSticky(state, hwnd, sticky);
            }
            break;
        }
        default:
            break;
    }
}

// --- pipe protocol --------------------------------------------------------

bool Dispatch(DaemonState& state, const IpcFrame& request, IpcFrame& reply) {
    switch (request.type) {
        case IpcType::Hello: {
            HelloReply hello = {};
            hello.protocolVersion = kIpcVersion;
            hello.daemonPid = ::GetCurrentProcessId();
            reply.type = IpcType::HelloAck;
            SetPayload(reply, hello);
            return true;
        }

        case IpcType::GetSnapshot: {
            std::lock_guard<std::mutex> lock(state.mutex);
            RefreshLocked(state);

            SnapshotReply snapshot = {};
            snapshot.count = static_cast<uint32_t>(
                state.capture.GetSnapshot(snapshot.sessions, kMaxSnapshotSessions));
            snapshot.primaryId = state.capture.PrimaryId();
            reply.type = IpcType::Snapshot;
            SetPayload(reply, snapshot);
            return true;
        }

        case IpcType::Send: {
            SendRequest send = {};
            GetPayload(request, send);

            std::lock_guard<std::mutex> lock(state.mutex);
            SendReply result = {};
            const HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(send.target));
            if (target == nullptr) {
                std::vector<size_t> all;
                all.reserve(state.capture.Count());
                for (size_t i = 0; i < state.capture.Count(); ++i) {
                    all.push_back(i);
                }
                const BatchSendResult batch =
                    state.capture.SendToMany(all, static_cast<Cmd>(send.cmd));
                result.ok = batch.ok;
                result.timeout = batch.timeout;
                result.error = batch.error;
            } else {
                const int index = state.capture.IndexOf(target);
                if (index < 0) {
                    result.error = 1;
                } else {
                    std::wstring ignored;
                    switch (state.capture.Send(static_cast<size_t>(index),
                                               static_cast<Cmd>(send.cmd), ignored)) {
                        case SendOutcome::Ok:
                            ++result.ok;
                            break;
                        case SendOutcome::Timeout:
                            ++result.timeout;
                            break;
                        case SendOutcome::Error:
                            ++result.error;
                            break;
                    }
                }
            }
            reply.type = IpcType::SendAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::Attach: {
            AttachRequest attach = {};
            GetPayload(request, attach);

            std::lock_guard<std::mutex> lock(state.mutex);
            AttachReply result = {};
            std::wstring error;
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(attach.hwnd));
            result.ok = state.capture.Attach(hwnd, error) ? 1 : 0;
            if (result.ok == 0) {
                CopyError(error, result.error, _countof(result.error));
            } else {
                // An explicit attach makes the window primary and remembers its
                // identity for the next session.
                state.capture.SetPrimary(hwnd);
                const int index = state.capture.IndexOf(hwnd);
                if (index >= 0) {
                    RememberSticky(state, state.capture.At(static_cast<size_t>(index)));
                }
            }
            reply.type = IpcType::AttachAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::Detach: {
            DetachRequest detach = {};
            GetPayload(request, detach);

            std::lock_guard<std::mutex> lock(state.mutex);
            DetachReply result = {};
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(detach.hwnd));
            const int index = state.capture.IndexOf(hwnd);
            if (index >= 0) {
                const Session& session = state.capture.At(static_cast<size_t>(index));
                state.autoCapture.Suppress(session.pid, session.createTime, session.hwnd);
                state.capture.Remove(static_cast<size_t>(index));
                result.ok = 1;
            }
            result.count = static_cast<int32_t>(state.capture.Count());
            reply.type = IpcType::DetachAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::RestoreAll: {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.capture.RestoreAll();
            RestoreAllReply result = {};
            result.count = static_cast<int32_t>(state.capture.Count());
            reply.type = IpcType::RestoreAllAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::GetSettings: {
            std::lock_guard<std::mutex> lock(state.mutex);
            reply.type = IpcType::Settings;
            SetTextPayload(reply, SerializeSettings(state.settings));
            return true;
        }

        case IpcType::SetSettings: {
            std::wstring json;
            SetSettingsReply result = {};
            reply.type = IpcType::SetSettingsAck;
            if (!GetTextPayload(request, json)) {
                result.ok = 0;
                CopyError(L"the settings document is malformed", result.error,
                          _countof(result.error));
                SetPayload(reply, result);
                return true;
            }

            LogLine(L"daemon: SetSettings %zu chars", json.size());
            Settings parsed;
            std::wstring error;
            if (!ParseSettings(json, parsed, error)) {
                result.ok = 0;
                CopyError(error, result.error, _countof(result.error));
                SetPayload(reply, result);
                return true;
            }

            // Marshal onto the daemon thread; see ApplySettingsContext.
            ApplySettingsContext context;
            context.settings = &parsed;
            ::SendMessageW(state.window, kApplySettingsMessage, 0,
                           reinterpret_cast<LPARAM>(&context));
            if (!context.ok) {
                LogLine(L"daemon: SetSettings rejected: %s", context.error.c_str());
                result.ok = 0;
                CopyError(context.error, result.error, _countof(result.error));
                SetPayload(reply, result);
                return true;
            }
            LogLine(L"daemon: SetSettings applied");

            result.ok = 1;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::ApplyAutoCapture: {
            std::lock_guard<std::mutex> lock(state.mutex);
            ApplyAutoCaptureReply result = {};
            result.attached = state.autoCapture.Scan(state.window, true);
            reply.type = IpcType::ApplyAutoCaptureAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::SetPrimary: {
            SetPrimaryRequest payload = {};
            GetPayload(request, payload);

            std::lock_guard<std::mutex> lock(state.mutex);
            SetPrimaryReply result = {};
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(payload.hwnd));
            if (hwnd == nullptr) {
                state.capture.ClearPrimary();
                result.ok = 1;
            } else {
                result.ok = state.capture.SetPrimary(hwnd) ? 1 : 0;
            }
            result.primaryId = state.capture.PrimaryId();
            reply.type = IpcType::SetPrimaryAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::SetSticky: {
            SetStickyRequest payload = {};
            GetPayload(request, payload);

            std::lock_guard<std::mutex> lock(state.mutex);
            SetStickyReply result = {};
            const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(payload.hwnd));
            if (SetSessionSticky(state, hwnd, payload.sticky != 0)) {
                const int index = state.capture.IndexOf(hwnd);
                result.ok = 1;
                result.sticky = index >= 0 && state.capture.At(static_cast<size_t>(index)).sticky
                                    ? 1
                                    : 0;
            }
            reply.type = IpcType::SetStickyAck;
            SetPayload(reply, result);
            return true;
        }

        case IpcType::Shutdown: {
            reply.type = IpcType::ShutdownAck;
            if (state.window != nullptr) {
                ::PostMessageW(state.window, kDaemonStopMessage, 0, 0);
            }
            return false;
        }

        default: {
            // Unknown request: answer with a handshake so the client's reply-type
            // check fails rather than the connection hanging.
            HelloReply hello = {};
            hello.protocolVersion = kIpcVersion;
            hello.daemonPid = ::GetCurrentProcessId();
            reply.type = IpcType::HelloAck;
            SetPayload(reply, hello);
            return true;
        }
    }
}

// --- window procedure -----------------------------------------------------

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                           DWORD, DWORD) {
    if (g_daemon == nullptr || event != EVENT_OBJECT_SHOW || idObject != OBJID_WINDOW ||
        idChild != CHILDID_SELF || hwnd == nullptr) {
        return;
    }
    ::PostMessageW(g_daemon->window, kAutoCaptureMessage, 0, 0);
}

void UpdateTrayTip(DaemonState& state, size_t count) {
    wchar_t tip[128] = {};
    swprintf_s(tip, L"GhostFrame \x2014 %zu tracked", count);
    state.tray.UpdateTip(tip);
}

// Reloads the tray icon after an appearance push.  A failed render keeps the
// previous icon so a bad emoji can never leave the daemon without a tray.
void UpdateDaemonIcon(DaemonState& state) {
    std::wstring error;
    if (!EnsureEmojiIcon(state.settings.appearance.iconEmoji, error)) {
        LogLine(L"daemon emoji icon: %s", error.c_str());
        return;
    }
    bool owned = false;
    HICON icon = LoadConfiguredIcon(::GetModuleHandleW(nullptr), kAppIconId,
                                    state.settings.appearance.iconEmoji, 32, owned);
    if (icon == nullptr) {
        LogLine(L"daemon emoji icon: no icon could be loaded");
        return;
    }
    if (state.icon != nullptr && state.iconOwned) {
        ::DestroyIcon(state.icon);
    }
    state.icon = icon;
    state.iconOwned = owned;
    state.tray.Add(state.window, kTrayMessage, state.icon,
                   L"GhostFrame \x2014 capture exclusion");
    LogLine(L"daemon emoji icon: %s",
            state.settings.appearance.iconEmoji.empty()
                ? L"bundled"
                : state.settings.appearance.iconEmoji.c_str());
}

// Runs on the daemon window thread.  Only replaces the stored document when the
// hotkeys could really be registered, so a rejected combination leaves both the
// registration and the file untouched.
void ApplySettings(DaemonState& state, ApplySettingsContext& context) {
    if (context.settings == nullptr) {
        context.ok = false;
        return;
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.hotkeys.Apply(state.window, *context.settings, context.error)) {
        std::wstring ignored;
        state.hotkeys.Apply(state.window, state.settings, ignored);
        context.ok = false;
        return;
    }

    // The UI edits everything except the sticky list, which it only toggles per
    // target through SetSticky at runtime.  Keeping the daemon's copy here makes
    // a settings push unable to erase identities captured while the UI ran.
    Settings next = *context.settings;
    next.sticky = state.settings.sticky;
    const std::wstring previousEmoji = state.settings.appearance.iconEmoji;
    state.settings = std::move(next);
    state.autoCapture.SetSettings(state.settings);
    state.autoCapture.SetSticky(state.settings.sticky);
    ApplyStickyFlags(state);
    if (previousEmoji != state.settings.appearance.iconEmoji) {
        UpdateDaemonIcon(state);
    }
    if (!SaveSettings(SettingsPath(), state.settings, context.error)) {
        context.ok = false;
        return;
    }
    context.ok = true;
}

LRESULT CALLBACK DaemonWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<DaemonState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case kDaemonStopMessage:
            ::PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        case WM_TIMER:
            if (state == nullptr) {
                return 0;
            }
            if (wParam == kRefreshTimer) {
                std::lock_guard<std::mutex> lock(state->mutex);
                RefreshLocked(*state);
                TrackForeground(*state);
                UpdateTrayTip(*state, state->capture.Count());
            } else if (wParam == kAutoCaptureTimer) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->autoCapture.Scan(hwnd, false);
            } else if (wParam == kSelfDeleteTimer) {
                if (state->selfDeleteArmed) {
                    state->selfDeleteArmed = false;
                    ::KillTimer(hwnd, kSelfDeleteTimer);
                    state->tray.ShowBalloon(L"GhostFrame",
                                            L"Self-delete cancelled (timed out).");
                }
            }
            return 0;
        case WM_HOTKEY:
            if (state != nullptr) {
                OnHotkey(*state, static_cast<int>(wParam));
            }
            return 0;
        case kApplySettingsMessage:
            if (state != nullptr) {
                ApplySettings(*state,
                              *reinterpret_cast<ApplySettingsContext*>(static_cast<uintptr_t>(lParam)));
            }
            return 0;
        default:
            if (state != nullptr && state->taskbarCreated != 0 &&
                message == state->taskbarCreated) {
                state->tray.ReAdd();
                return 0;
            }
            if (state != nullptr && message == kTrayMessage) {
                OnTray(*state, lParam);
                return 0;
            }
            if (state != nullptr && message == kAutoCaptureMessage) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->autoCapture.Scan(hwnd, false);
                return 0;
            }
            break;
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

// --- daemon-check helpers -------------------------------------------------

std::wstring CheckLogPath() {
    const std::wstring module = ModulePath();
    if (module.empty()) {
        return L"daemon-check.log";
    }
    const size_t slash = module.find_last_of(L'\\');
    return module.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"daemon-check.log";
}

void Say(const std::wstring& text) {
    wprintf(L"%s\n", text.c_str());

    FILE* file = nullptr;
    if (_wfopen_s(&file, CheckLogPath().c_str(), L"a, ccs=UTF-8") == 0 && file != nullptr) {
        fwprintf(file, L"%s\n", text.c_str());
        fclose(file);
    }
}

}  // namespace

int RunDaemon() {
    LogInit(ResolveLogDirectory());
    LogLine(L"daemon starting GhostFrame %s", GF_VERSION_STRING);

    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kDaemonMutex);
    if (mutex == nullptr) {
        LogLine(L"daemon: cannot create the single-instance mutex: %s",
                Win32Message(::GetLastError()).c_str());
        LogShutdown();
        return 1;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        LogLine(L"daemon already running");
        ::CloseHandle(mutex);
        LogShutdown();
        return 0;
    }

    DaemonState state;
    std::wstring error;
    LoadSettings(SettingsPath(), state.settings, error);
    if (!error.empty()) {
        LogLine(L"daemon settings: %s", error.c_str());
    }
    state.autoCapture.SetSettings(state.settings);
    state.autoCapture.SetSticky(state.settings.sticky);
    LogLine(L"daemon: foreground tracking %s, %zu sticky target(s)",
            state.settings.foregroundTracking ? L"on" : L"off", state.settings.sticky.size());

    if (!state.capture.Initialize(error)) {
        // Not fatal: the daemon still answers the protocol and retries the
        // payload extraction on the first Attach.
        LogLine(L"daemon payload preparation deferred: %s", error.c_str());
    }

    state.taskbarCreated = ::RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DaemonWndProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kDaemonClass;
    ::RegisterClassExW(&windowClass);

    g_daemon = &state;
    // A hidden top-level tool window rather than HWND_MESSAGE: the tray icon and
    // a scripted WM_HOTKEY test both need a window FindWindow can see, while
    // WS_EX_TOOLWINDOW keeps it out of Alt+Tab and the taskbar.
    state.window = ::CreateWindowExW(WS_EX_TOOLWINDOW, kDaemonClass, L"GhostFrame Daemon",
                                     WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                     windowClass.hInstance, &state);
    if (state.window == nullptr) {
        g_daemon = nullptr;
        LogLine(L"daemon: cannot create the message window: %s",
                Win32Message(::GetLastError()).c_str());
        state.capture.Shutdown();
        ::CloseHandle(mutex);
        LogShutdown();
        return 1;
    }
    ::SetWindowLongPtrW(state.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    state.hotkeys.Apply(state.window, state.settings, error);
    if (!error.empty()) {
        LogLine(L"daemon hotkeys: %s", error.c_str());
    }

    state.icon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId));
    if (state.icon == nullptr) {
        state.icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }
    state.iconOwned = false;
    {
        std::wstring iconError;
        if (!state.settings.appearance.iconEmoji.empty() &&
            !EnsureEmojiIcon(state.settings.appearance.iconEmoji, iconError)) {
            LogLine(L"daemon emoji icon: %s", iconError.c_str());
        }
        state.icon = LoadConfiguredIcon(::GetModuleHandleW(nullptr), kAppIconId,
                                        state.settings.appearance.iconEmoji, 32,
                                        state.iconOwned);
        if (state.icon == nullptr) {
            state.icon = ::LoadIconW(nullptr, IDI_APPLICATION);
            state.iconOwned = false;
        }
    }
    state.tray.Add(state.window, kTrayMessage, state.icon,
                   L"GhostFrame \x2014 capture exclusion");

    // New top-level windows are picked up immediately; the timer is a safety net.
    state.hook = ::SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &WinEventProc,
                                   0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    ::SetTimer(state.window, kRefreshTimer, kRefreshTickMs, nullptr);
    ::SetTimer(state.window, kAutoCaptureTimer, kAutoCaptureTickMs, nullptr);

    // Re-attach the sticky targets that are already running before the pipe is
    // published, so a client that connects immediately sees the restored set.
    state.autoCapture.Scan(state.window, true);

    PipeServer server;
    if (!server.Start(kPipeName,
                      [&state](const IpcFrame& request, IpcFrame& reply) {
                          return Dispatch(state, request, reply);
                      },
                      error)) {
        LogLine(L"daemon: cannot start the pipe server: %s", error.c_str());
        ::UnhookWinEvent(state.hook);
        ::KillTimer(state.window, kRefreshTimer);
        ::KillTimer(state.window, kAutoCaptureTimer);
        ::KillTimer(state.window, kSelfDeleteTimer);
        state.tray.Remove();
        state.hotkeys.Unregister(state.window);
        ::DestroyWindow(state.window);
        g_daemon = nullptr;
        state.capture.Shutdown();
        ::CloseHandle(mutex);
        LogShutdown();
        return 1;
    }
    LogLine(L"daemon listening on %s", kPipeName);

    MSG message = {};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    server.Stop();
    ::UnhookWinEvent(state.hook);
    state.hook = nullptr;
    ::KillTimer(state.window, kRefreshTimer);
    ::KillTimer(state.window, kAutoCaptureTimer);
    state.hotkeys.Unregister(state.window);
    state.tray.Remove();
    if (state.icon != nullptr && state.iconOwned) {
        ::DestroyIcon(state.icon);
    }
    state.icon = nullptr;
    state.iconOwned = false;

    state.capture.RestoreAll();
    state.capture.Shutdown();
    ::DestroyWindow(state.window);
    g_daemon = nullptr;
    ::CloseHandle(mutex);
    LogLine(L"daemon stopped");
    LogShutdown();
    return 0;
}

int RunDaemonCheck() {
    ::DeleteFileW(CheckLogPath().c_str());

    DaemonClient client;
    std::wstring error;

    if (!client.EnsureRunning(error)) {
        Say(L"FAIL: ensure daemon: " + error);
        return 1;
    }
    Say(L"[ok  ] daemon reachable");

    // On an early return after this point, ask the daemon to stop: a failed
    // diagnostic must not leave a capture daemon (and its targets) behind.
    bool shutdownSent = false;
    struct ShutdownOnExit {
        DaemonClient& client;
        bool& sent;
        ~ShutdownOnExit() {
            if (!sent) {
                std::wstring ignored;
                client.Shutdown(ignored);
            }
        }
    } cleanup{client, shutdownSent};

    SnapshotReply snapshot = {};
    if (!client.GetSnapshot(snapshot, error)) {
        Say(L"FAIL: get snapshot: " + error);
        return 1;
    }
    uint32_t stickyCount = 0;
    uint32_t hiddenCount = 0;
    for (uint32_t i = 0; i < snapshot.count; ++i) {
        stickyCount += snapshot.sessions[i].sticky != 0 ? 1u : 0u;
        hiddenCount += (snapshot.sessions[i].flags & FlagHidden) != 0 ? 1u : 0u;
    }
    Say(FormatString(L"[ok  ] snapshot: %u session(s), primary=%u, sticky=%u, hidden=%u",
                     snapshot.count, snapshot.primaryId, stickyCount, hiddenCount));

    SendReply send = {};
    if (!client.Send(0, Cmd::Ping, send, error)) {
        Say(L"FAIL: send: " + error);
        return 1;
    }
    Say(FormatString(L"[ok  ] send (all): ok=%d timeout=%d error=%d", send.ok, send.timeout,
                     send.error));

    std::wstring json;
    if (!client.GetSettings(json, error) || json.empty()) {
        Say(L"FAIL: get settings: " + error);
        return 1;
    }
    Say(L"[ok  ] settings round trip: " + std::to_wstring(json.size()) + L" chars");

    SetSettingsReply settingsReply = {};
    if (!client.SetSettings(json, settingsReply, error) || settingsReply.ok == 0) {
        Say(L"FAIL: set settings: " + (error.empty() ? std::wstring(settingsReply.error) : error));
        return 1;
    }
    Say(L"[ok  ] settings accepted by the daemon");

    ApplyAutoCaptureReply applied = {};
    if (!client.ApplyAutoCapture(applied, error)) {
        Say(L"FAIL: apply auto-capture: " + error);
        return 1;
    }
    Say(FormatString(L"[ok  ] auto-capture scan: %d attached", applied.attached));

    SetPrimaryReply primary = {};
    if (snapshot.count > 0) {
        if (!client.SetPrimary(snapshot.sessions[0].hwnd, primary, error) || primary.ok == 0) {
            Say(L"FAIL: set primary: " + error);
            return 1;
        }
        if (primary.primaryId != snapshot.sessions[0].id) {
            Say(L"FAIL: the primary id does not match the requested target");
            return 1;
        }
        Say(L"[ok  ] primary target set");
    }
    if (!client.SetPrimary(0, primary, error) || primary.ok == 0 || primary.primaryId != 0) {
        Say(L"FAIL: clear primary: " + error);
        return 1;
    }
    Say(L"[ok  ] primary target cleared");

    SetStickyReply sticky = {};
    if (!client.SetSticky(0, true, sticky, error) || sticky.ok != 0) {
        Say(L"FAIL: an invalid sticky target must be rejected");
        return 1;
    }
    Say(L"[ok  ] sticky rejects an untracked window");

    if (!client.Shutdown(error)) {
        Say(L"FAIL: shutdown: " + error);
        return 1;
    }
    shutdownSent = true;
    Say(L"[ok  ] shutdown acknowledged");

    // The daemon must release its pipe and exit.
    PipeClient probe;
    for (int i = 0; i < 100; ++i) {
        if (!probe.WaitAvailable(100)) {
            Say(L"[ok  ] daemon stopped");
            return 0;
        }
        ::Sleep(50);
    }
    Say(L"FAIL: the daemon is still listening");
    return 1;
}

}  // namespace gf