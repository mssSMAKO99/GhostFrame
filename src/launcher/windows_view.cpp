// WINDOWS tab: the live window enumerator, its filter and the attach actions.

#include <windows.h>

#include <cwctype>

#include "app.h"
#include "resource.h"

namespace {

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

void App::RefreshWindows() {
    windowEnumerator_.Refresh(hwnd_);
    BuildWindowRows();
    InvalidateAll();
}

void App::BuildWindowRows() {
    const std::wstring filter = ToLower(windowFilter_);

    std::vector<ui::ListRow> rows;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        const bool tracked = sessions_.IndexOf(window.hwnd) >= 0;

        if (!filter.empty()) {
            const std::wstring title = ToLower(window.title);
            const std::wstring process = ToLower(window.processName);
            if (!ContainsNoCase(title, filter) && !ContainsNoCase(process, filter)) {
                continue;
            }
        }

        ui::ListRow row;
        row.title = window.title;
        row.icon = window.icon;
        row.data = reinterpret_cast<intptr_t>(window.hwnd);
        row.selectable = !tracked;
        row.dim = tracked;

        wchar_t meta[160] = {};
        swprintf_s(meta, L"%s  ·  PID %u  ·  %s", window.processName.c_str(), window.pid,
                   gf::WindowEnumerator::ArchLabel(window.arch));
        row.meta = meta;

        if (tracked) {
            row.badges.push_back({L"TRACKED", ui::kMuted});
        } else {
            bool autoMatch = false;
            for (const gf::AutoAttachRule& rule : settings_.autoAttach) {
                if (gf::RuleMatches(rule, window.processName, window.title)) {
                    autoMatch = true;
                    break;
                }
            }
            if (autoMatch) {
                row.badges.push_back({L"AUTO", ui::kNeon});
            }
        }
        if (window.elevated) {
            row.badges.push_back({L"ADMIN", ui::kWarn});
        }
        if (window.uwp) {
            row.badges.push_back({L"UWP", ui::kWarn});
        }

        rows.push_back(std::move(row));
    }

    windowsList_.SetEmptyText(filter.empty() ? L"NO WINDOWS FOUND"
                                             : L"NO WINDOWS MATCH THE FILTER");
    windowsList_.SetRows(std::move(rows));
    UpdateAttachButton();
}

void App::OnFilterChanged() {
    wchar_t buffer[128] = {};
    if (HWND search = ::GetDlgItem(hwnd_, ID_EDIT_SEARCH)) {
        ::GetWindowTextW(search, buffer, _countof(buffer) - 1);
    }
    windowFilter_ = buffer;
    BuildWindowRows();
    InvalidateAll();
}

void App::AttachSelectedWindow() {
    if (tab_ != Tab::Windows) {
        return;
    }

    const std::vector<int> selected = windowsList_.SelectedIndices();
    if (selected.empty()) {
        SetStatus(L"NO WINDOW SELECTED", true);
        return;
    }

    std::vector<HWND> targets;
    std::wstring firstReason;
    for (int index : selected) {
        const HWND target = reinterpret_cast<HWND>(windowsList_.DataAt(index));
        if (target == nullptr) {
            continue;
        }
        std::wstring reason;
        if (!gf::ValidateTarget(target, reason)) {
            if (firstReason.empty()) {
                firstReason = reason;
            }
            continue;
        }
        if (sessions_.IndexOf(target) >= 0) {
            continue;
        }
        targets.push_back(target);
    }

    if (targets.empty()) {
        SetStatus(firstReason.empty() ? L"THE WINDOW IS ALREADY TRACKED"
                                      : L"ATTACH FAILED: " + firstReason,
                  true);
        return;
    }
    StartAttachBatch(std::move(targets), settings_.ui.hideOnAttach);
}

void App::AttachProcessWindows(HWND sample) {
    if (sample == nullptr) {
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    if (pid == 0) {
        return;
    }

    std::vector<HWND> targets;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        if (window.pid != pid || window.hwnd == nullptr) {
            continue;
        }
        if (sessions_.IndexOf(window.hwnd) >= 0) {
            continue;
        }
        std::wstring reason;
        if (!gf::ValidateTarget(window.hwnd, reason)) {
            continue;
        }
        targets.push_back(window.hwnd);
    }

    if (targets.empty()) {
        SetStatus(L"NO ATTACHABLE WINDOWS IN THIS PROCESS", true);
        return;
    }
    StartAttachBatch(std::move(targets), settings_.ui.hideOnAttach);
}

void App::OnRButtonDown(int x, int y) {
    if (tab_ != Tab::Windows) {
        return;
    }
    const int row = windowsList_.IndexAt(x, y);
    if (row < 0) {
        return;
    }
    const HWND sample = reinterpret_cast<HWND>(windowsList_.DataAt(row));
    if (sample == nullptr) {
        return;
    }

    // Count the visible windows of the same process that are not tracked yet,
    // so the menu can show how many would be attached.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    size_t count = 0;
    for (const gf::WindowInfo& window : windowEnumerator_.windows()) {
        if (window.pid == pid && window.hwnd != nullptr && sessions_.IndexOf(window.hwnd) < 0) {
            ++count;
        }
    }
    if (count == 0) {
        SetStatus(L"NO ATTACHABLE WINDOWS IN THIS PROCESS", true);
        return;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    wchar_t label[128] = {};
    swprintf_s(label, L"Attach all %zu window(s) of this process", count);
    ::AppendMenuW(menu, MF_STRING, ID_MENU_ATTACH_PROCESS, label);

    POINT point = {x, y};
    ::ClientToScreen(hwnd_, &point);
    ::SetForegroundWindow(hwnd_);
    const int command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0,
                                         hwnd_, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (command == ID_MENU_ATTACH_PROCESS) {
        AttachProcessWindows(sample);
    }
}
