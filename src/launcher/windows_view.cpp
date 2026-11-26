// WINDOWS tab: the live window enumerator, its filter and the attach actions.

#include "windows_view.h"

#include <windows.h>

#include <cwctype>

#include "resource.h"

namespace gf {

namespace {

const ViewInfo kInfo = {L"windows", L"Windows", L"Windows",
                        L"Pick a window to attach and hide from capture.", ui::Icon::Grid};

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return text;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

void PlaceChild(HWND parent, int id, const RECT& rect) {
    HWND child = ::GetDlgItem(parent, id);
    if (child != nullptr) {
        ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
                       rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

}  // namespace

const ViewInfo& WindowsView::Info() const { return kInfo; }

void WindowsView::CreateControls(HWND parent, HINSTANCE instance) {
    (void)parent;
    (void)instance;
    host_.AddSearchEdit(ID_EDIT_SEARCH, L"Search windows, apps or PIDs…");
}

void WindowsView::Refresh() {
    host_.Windows().Refresh(host_.Window());
    Build();
    host_.Invalidate();
}

void WindowsView::Build() {
    Settings& settings = host_.Settings();
    RemoteCapture& capture = host_.Capture();
    const std::wstring filter = ToLower(filter_);

    std::vector<ui::ListRow> rows;
    for (const WindowInfo& window : host_.Windows().windows()) {
        const bool tracked = capture.IndexOf(window.hwnd) >= 0;

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

        wchar_t pidText[16] = {};
        swprintf_s(pidText, L"%u", window.pid);
        row.cells.push_back(window.processName);
        row.cells.push_back(pidText);
        row.cells.push_back(WindowEnumerator::ArchLabel(window.arch));

        if (tracked) {
            row.badges.push_back({L"TRACKED", ui::kMuted});
        } else {
            bool autoMatch = false;
            for (const AutoAttachRule& rule : settings.autoAttach) {
                if (RuleMatches(rule, window.processName, window.title)) {
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

    list_.SetEmptyText(filter.empty() ? L"NO VISIBLE WINDOWS — OPEN AN APP AND PRESS F5"
                                      : L"NO WINDOWS MATCH THIS SEARCH");
    list_.SetRows(std::move(rows));
    ApplyControls();
}

void WindowsView::SetActive(bool active) {
    active_ = active;
    ApplyControls();
    if (active) {
        host_.Windows().Refresh(host_.Window());
        Build();
        host_.SetStatus(L"Pick one or more windows, then attach them in one action");
    }
}

void WindowsView::ApplyControls() {
    if (HWND search = ::GetDlgItem(host_.Window(), ID_EDIT_SEARCH)) {
        ::ShowWindow(search, active_ ? SW_SHOW : SW_HIDE);
    }
}

void WindowsView::Layout() {
    LayoutInfo& layout = host_.Layout();
    const ui::Theme& theme = host_.Theme();

    layout.cta = {layout.body.right - theme.S(196), layout.pageHeader.top + theme.S(9),
                  layout.body.right, layout.pageHeader.top + theme.S(9) + theme.S(40)};
    const int secW = theme.S(124);
    layout.secondaryCta = {layout.cta.left - theme.S(8) - secW, layout.cta.top,
                           layout.cta.left - theme.S(8), layout.cta.bottom};

    layout.searchBox = {layout.body.left, layout.body.top, layout.body.left + theme.S(390),
                        layout.body.top + theme.S(40)};
    layout.list = {layout.body.left, layout.searchBox.bottom + theme.S(12), layout.body.right,
                   layout.body.bottom};

    RECT edit = {layout.searchBox.left + theme.S(34), layout.searchBox.top + theme.S(8),
                 layout.searchBox.right - theme.S(8), layout.searchBox.bottom - theme.S(8)};
    PlaceChild(host_.Window(), ID_EDIT_SEARCH, edit);

    list_.SetRowHeight(host_.Theme().RowHeight(43));
    list_.SetColumns({
        {L"WINDOW", -1, DT_LEFT, false},
        {L"APPLICATION", 128, DT_LEFT, false},
        {L"PID", 62, DT_RIGHT, true},
        {L"ARCH", 52, DT_LEFT, true},
        {L"TYPE", 104, DT_LEFT, false},
    });
    list_.SetCheckboxes(true);
    list_.SetBounds(layout.list, host_.Theme());
}

void WindowsView::Paint(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const LayoutInfo& layout = host_.Layout();

    ui::FillRoundRect(dc, layout.searchBox, ui::kPanelHi, theme.S(10));
    ui::StrokeRoundFrame(dc, layout.searchBox, ui::kBorder, theme.S(10), 1);
    const int iconSize = theme.S(16);
    RECT searchIcon = {
        layout.searchBox.left + theme.S(13),
        layout.searchBox.top + (layout.searchBox.bottom - layout.searchBox.top - iconSize) / 2, 0,
        0};
    searchIcon.right = searchIcon.left + iconSize;
    searchIcon.bottom = searchIcon.top + iconSize;
    ui::DrawIcon(dc, ui::Icon::Search, searchIcon, ui::kMuted, theme.S(2));

    ui::FillRoundRect(dc, layout.list, ui::kPanel, theme.S(14));
    ui::StrokeRoundFrame(dc, layout.list, ui::kBorder, theme.S(14), 1);
    list_.Paint(dc, theme, ::GetFocus() == host_.Window());
}

void WindowsView::PaintHeader(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const LayoutInfo& layout = host_.Layout();

    wchar_t attachLabel[96] = {};
    if (host_.AttachBusy()) {
        wcscpy_s(attachLabel, L"Stop attaching");
    } else {
        const int count = list_.SelectedCount();
        if (count > 0) {
            swprintf_s(attachLabel, L"Attach %d window%s", count, count == 1 ? L"" : L"s");
        } else {
            wcscpy_s(attachLabel, L"Attach Selected");
        }
    }
    PaintCtaButton(dc, theme, layout.secondaryCta, L"Refresh", ui::Icon::Refresh, ui::Icon::None,
                   false, hoverRefresh_);
    const bool canAttach = host_.AttachBusy() || list_.SelectedCount() > 0;
    PaintCtaButton(dc, theme, layout.cta, attachLabel, ui::Icon::Plus, ui::Icon::None, canAttach,
                   hoverCta_);
}

bool WindowsView::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    if (layout.secondaryCta.right > layout.secondaryCta.left &&
        ::PtInRect(&layout.secondaryCta, point)) {
        OnCommand(ID_BTN_REFRESH);
        return true;
    }
    if (::PtInRect(&layout.cta, point)) {
        OnCommand(ID_BTN_ATTACH);
        return true;
    }
    if (list_.Contains(x, y)) {
        ApplyListAction(list_.OnLButtonDown(x, y, ctrl, shift));
        host_.Invalidate();
        return true;
    }
    return false;
}

bool WindowsView::OnLButtonDblClk(int x, int y) {
    if (!list_.Contains(x, y)) {
        return false;
    }
    ApplyListAction(list_.OnLButtonDblClk(x, y));
    host_.Invalidate();
    return true;
}

bool WindowsView::OnRButtonDown(int x, int y) {
    const int row = list_.IndexAt(x, y);
    if (row < 0) {
        return false;
    }
    const HWND sample = reinterpret_cast<HWND>(list_.DataAt(row));
    if (sample == nullptr) {
        return false;
    }

    // Count the visible windows of the same process that are not tracked yet,
    // so the menu can show how many would be attached.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    size_t count = 0;
    for (const WindowInfo& window : host_.Windows().windows()) {
        if (window.pid == pid && window.hwnd != nullptr &&
            host_.Capture().IndexOf(window.hwnd) < 0) {
            ++count;
        }
    }
    if (count == 0) {
        host_.SetStatus(L"No attachable windows in this process", true);
        return true;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return true;
    }
    wchar_t label[128] = {};
    swprintf_s(label, L"Attach all %zu window(s) of this process", count);
    ::AppendMenuW(menu, MF_STRING, ID_MENU_ATTACH_PROCESS, label);

    POINT point = {x, y};
    const HWND window = host_.Window();
    ::ClientToScreen(window, &point);
    ::SetForegroundWindow(window);
    const int command =
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0, window,
                         nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(window, WM_NULL, 0, 0);

    if (command == ID_MENU_ATTACH_PROCESS) {
        AttachProcess(sample);
    }
    return true;
}

bool WindowsView::OnKeyDown(int virtualKey) {
    if (virtualKey == VK_F5) {
        Refresh();
        host_.SetStatus(L"Window list refreshed");
        return true;
    }
    const ui::ListWidget::Action action = list_.OnKeyDown(virtualKey);
    if (action.kind != ui::ListWidget::ActionKind::None) {
        ApplyListAction(action);
        host_.Invalidate();
        return true;
    }
    return false;
}

void WindowsView::OnMouseMove(int x, int y, bool& changed) {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    const bool cta = ::PtInRect(&layout.cta, point);
    const bool refresh =
        layout.secondaryCta.right > layout.secondaryCta.left &&
        ::PtInRect(&layout.secondaryCta, point);
    if (cta != hoverCta_ || refresh != hoverRefresh_) {
        hoverCta_ = cta;
        hoverRefresh_ = refresh;
        changed = true;
    }
    changed |= list_.OnMouseMove(x, y, true);
}

void WindowsView::OnMouseLeave(bool& changed) {
    if (hoverCta_ || hoverRefresh_) {
        hoverCta_ = false;
        hoverRefresh_ = false;
        changed = true;
    }
    changed |= list_.ClearHover();
}

bool WindowsView::OnMouseWheel(int x, int y, int delta) {
    if (list_.Contains(x, y)) {
        list_.OnMouseWheel(delta);
        host_.Invalidate();
        return true;
    }
    return false;
}

bool WindowsView::OnVScroll(int code, int position) {
    list_.OnVScroll(code, position);
    host_.Invalidate();
    return true;
}

bool WindowsView::OnCommand(int id) {
    switch (id) {
        case ID_BTN_REFRESH:
            Refresh();
            host_.SetStatus(L"Window list refreshed");
            return true;
        case ID_BTN_ATTACH:
            if (host_.AttachBusy()) {
                host_.CancelAttach();
            } else {
                AttachSelected();
            }
            return true;
        case ID_MENU_ATTACH_PROCESS:
            return true;
        default:
            return false;
    }
}

bool WindowsView::HandCursorAt(int x, int y) const {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    return ::PtInRect(&layout.cta, point) ||
           (layout.secondaryCta.right > layout.secondaryCta.left &&
            ::PtInRect(&layout.secondaryCta, point)) ||
           list_.Contains(x, y);
}

bool WindowsView::ButtonActive(int) const { return false; }

void WindowsView::OnAttachFinished(HWND lastHwnd, bool active) {
    if (active && lastHwnd != nullptr) {
        host_.SelectView(L"targets");
    } else {
        Build();
    }
}

void WindowsView::OnTimer() { Refresh(); }

void WindowsView::ApplyListAction(ui::ListWidget::Action action) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }
    if (action.kind == ui::ListWidget::ActionKind::Activate) {
        AttachSelected();
    } else {
        host_.Invalidate();
    }
}

void WindowsView::AttachSelected() {
    const std::vector<int> selected = list_.SelectedIndices();
    if (selected.empty()) {
        host_.SetStatus(L"No window selected", true);
        return;
    }

    std::vector<HWND> targets;
    std::wstring firstReason;
    for (int index : selected) {
        const HWND target = reinterpret_cast<HWND>(list_.DataAt(index));
        if (target == nullptr) {
            continue;
        }
        std::wstring reason;
        if (!ValidateTarget(target, reason)) {
            if (firstReason.empty()) {
                firstReason = reason;
            }
            continue;
        }
        if (host_.Capture().IndexOf(target) >= 0) {
            continue;
        }
        targets.push_back(target);
    }

    if (targets.empty()) {
        host_.SetStatus(firstReason.empty() ? L"The window is already tracked"
                                            : L"Attach failed: " + firstReason,
                        true);
        return;
    }
    host_.StartAttachBatch(std::move(targets), host_.Settings().ui.hideOnAttach);
}

void WindowsView::AttachProcess(HWND sample) {
    if (sample == nullptr) {
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(sample, &pid);
    if (pid == 0) {
        return;
    }

    std::vector<HWND> targets;
    for (const WindowInfo& window : host_.Windows().windows()) {
        if (window.pid != pid || window.hwnd == nullptr) {
            continue;
        }
        if (host_.Capture().IndexOf(window.hwnd) >= 0) {
            continue;
        }
        std::wstring reason;
        if (!ValidateTarget(window.hwnd, reason)) {
            continue;
        }
        targets.push_back(window.hwnd);
    }

    if (targets.empty()) {
        host_.SetStatus(L"No attachable windows in this process", true);
        return;
    }
    host_.StartAttachBatch(std::move(targets), host_.Settings().ui.hideOnAttach);
}

void WindowsView::OnControlChanged(int controlId) {
    if (controlId != ID_EDIT_SEARCH) {
        return;
    }
    wchar_t buffer[128] = {};
    if (HWND search = ::GetDlgItem(host_.Window(), ID_EDIT_SEARCH)) {
        ::GetWindowTextW(search, buffer, _countof(buffer) - 1);
    }
    filter_ = buffer;
    Build();
    host_.Invalidate();
}

}  // namespace gf