// TARGETS tab: the tracked-window list, its row actions and the commands that
// operate on the current selection.

#include "targets_view.h"

#include <windows.h>

#include <algorithm>

#include "resource.h"

namespace gf {

namespace {

const ViewInfo kInfo = {L"targets", L"Targets", L"Tracked Targets",
                        L"Manage the windows GhostFrame monitors and hides.",
                        ui::Icon::Target};

const wchar_t* StateLabel(const RemoteSession& session) {
    if (session.errored()) {
        return L"ERROR";
    }
    if (session.hidden()) {
        return L"HIDDEN";
    }
    return L"VISIBLE";
}

COLORREF StateColor(const RemoteSession& session) {
    if (session.errored()) {
        return ui::kWarn;
    }
    if (session.hidden()) {
        return ui::kNeon;
    }
    return ui::kMuted;
}

void PlaceChild(HWND parent, int id, const RECT& rect) {
    HWND child = ::GetDlgItem(parent, id);
    if (child != nullptr) {
        ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
                       rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

}  // namespace

const ViewInfo& TargetsView::Info() const { return kInfo; }

void TargetsView::CreateControls(HWND parent, HINSTANCE instance) {
    (void)parent;
    (void)instance;
    host_.AddButton(ID_BTN_TOGGLE, L"Toggle stealth",
                    L"Toggle capture exclusion for the selection");
    host_.AddButton(ID_BTN_RESTORE_ALL, L"Restore all", L"Restore every tracked window");
    host_.AddButton(ID_BTN_MIN, L"Minimize", L"Minimize the selected targets");
    host_.AddButton(ID_BTN_RESTORE, L"Restore", L"Restore the selected targets");
    host_.AddButton(ID_BTN_CLOSE, L"Close", L"Ask the selected targets to close");
}

void TargetsView::Build() {
    RemoteCapture& capture = host_.Capture();
    std::vector<ui::ListRow> rows;
    rows.reserve(capture.Count());

    for (size_t i = 0; i < capture.Count(); ++i) {
        const RemoteSession& session = capture.At(i);
        ui::ListRow row;
        row.title = session.title;
        row.icon = host_.Windows().IconForWindow(session.hwnd);
        row.data = reinterpret_cast<intptr_t>(session.hwnd);

        const std::wstring process = host_.Windows().ProcessNameForWindow(session.hwnd);
        wchar_t meta[192] = {};
        if (process.empty()) {
            swprintf_s(meta, L"PID %u  ·  %s", session.pid, session.x86 ? L"x86" : L"x64");
        } else {
            swprintf_s(meta, L"%s  ·  PID %u  ·  %s", process.c_str(), session.pid,
                       session.x86 ? L"x86" : L"x64");
        }
        row.meta = meta;

        row.badges.push_back({StateLabel(session), StateColor(session)});
        if (session.sticky) {
            row.badges.push_back({L"STICKY", ui::kNeonDim});
        }
        if (capture.IsPrimary(session)) {
            row.badges.push_back({L"PRIMARY", ui::kNeon});
        }

        row.actions.push_back(ui::ListAction{ID_ROW_STEALTH, session.hidden() ? L"Show" : L"Hide",
                                             ui::kNeon, session.hidden(), false});
        row.actions.push_back(ui::ListAction{ID_ROW_MIN, L"Min", ui::kMuted, false, false});
        row.actions.push_back(ui::ListAction{ID_ROW_CLOSE, L"Close", ui::kWarn, false, true});

        rows.push_back(std::move(row));
    }

    // The empty state is drawn as a full card by Paint().
    list_.SetEmptyText(L"");
    list_.SetRows(std::move(rows));
    ApplyControls();
}

void TargetsView::SetActive(bool active) {
    active_ = active;
    ApplyControls();
    if (active) {
        if (host_.Capture().Empty()) {
            host_.SetStatus(L"Ready — focus a window and press Ctrl+Shift+H, or add a target");
        } else {
            host_.SetStatus(L"Select a target to manage it, or use the actions below");
        }
    }
}

void TargetsView::ApplyControls() {
    const bool show = active_ && !host_.Capture().Empty();
    for (int id : {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_MIN, ID_BTN_RESTORE, ID_BTN_CLOSE}) {
        if (HWND child = ::GetDlgItem(host_.Window(), id)) {
            ::ShowWindow(child, show ? SW_SHOW : SW_HIDE);
        }
    }
}

void TargetsView::Layout() {
    LayoutInfo& layout = host_.Layout();
    const ui::Theme& theme = host_.Theme();

    layout.cta = {layout.body.right - theme.S(196), layout.pageHeader.top + theme.S(9),
                  layout.body.right, layout.pageHeader.top + theme.S(9) + theme.S(40)};

    const int toolbarH = theme.S(36);
    layout.toolbar = {layout.body.left, layout.body.bottom - toolbarH, layout.body.right,
                      layout.body.bottom};
    layout.list = {layout.body.left, layout.body.top, layout.body.right,
                   layout.toolbar.top - theme.S(12)};

    layout.emptyCard = layout.list;
    const int cardPad = theme.S(44);
    layout.emptyArt = {layout.emptyCard.left + cardPad, layout.emptyCard.top + theme.S(28),
                       layout.emptyCard.left +
                           (layout.emptyCard.right - layout.emptyCard.left) * 45 / 100,
                       layout.emptyCard.bottom - theme.S(28)};
    const int blockH = theme.S(200);
    const int blockTop =
        layout.emptyCard.top + ((layout.emptyCard.bottom - layout.emptyCard.top) - blockH) / 2;
    layout.emptyText = {layout.emptyArt.right + theme.S(20), blockTop,
                        layout.emptyCard.right - theme.S(36), blockTop + theme.S(108)};
    layout.emptyCta = {layout.emptyText.left, blockTop + theme.S(114),
                       layout.emptyText.left + theme.S(196), blockTop + theme.S(152)};

    if (layout.toolbar.right > layout.toolbar.left) {
        const int gap = theme.S(8);
        const int count = 5;
        const int total = layout.toolbar.right - layout.toolbar.left;
        const int buttonWidth = (total - gap * (count - 1)) / count;
        const int ids[count] = {ID_BTN_TOGGLE, ID_BTN_RESTORE_ALL, ID_BTN_MIN, ID_BTN_RESTORE,
                                ID_BTN_CLOSE};
        for (int i = 0; i < count; ++i) {
            RECT rect = {layout.toolbar.left + i * (buttonWidth + gap), layout.toolbar.top,
                         layout.toolbar.left + i * (buttonWidth + gap) + buttonWidth,
                         layout.toolbar.bottom};
            PlaceChild(host_.Window(), ids[i], rect);
        }
    }

    list_.SetRowHeight(host_.Theme().RowHeight(58));
    list_.SetColumns({});
    list_.SetCheckboxes(false);
    list_.SetBounds(layout.list, host_.Theme());
}

void TargetsView::Paint(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const RemoteCapture& capture = host_.Capture();
    if (capture.Empty()) {
        PaintEmpty(dc);
        return;
    }

    wchar_t line[96] = {};
    swprintf_s(line, L"%zu window%s tracked", capture.Count(), capture.Count() == 1 ? L"" : L"s");
    const LayoutInfo& layout = host_.Layout();
    RECT caption = {layout.list.left, layout.list.top - theme.S(18), layout.list.right,
                    layout.list.top - theme.S(3)};
    ui::DrawLabel(dc, line, caption, theme.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const int selected = list_.SelectedCount();
    if (selected > 0) {
        wchar_t selection[64] = {};
        swprintf_s(selection, L"%d selected", selected);
        ui::DrawLabel(dc, selection, caption, theme.tiny, ui::kNeonDim,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    ui::FillRoundRect(dc, layout.list, ui::kPanel, theme.S(14));
    ui::StrokeRoundFrame(dc, layout.list, ui::kBorder, theme.S(14), 1);
    list_.Paint(dc, theme, ::GetFocus() == host_.Window());
}

void TargetsView::PaintHeader(HDC dc) {
    PaintCtaButton(dc, host_.Theme(), host_.Layout().cta, L"Add Target", ui::Icon::Plus,
                   ui::Icon::ChevronRight, true, hoverCta_);
}

void TargetsView::PaintEmpty(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const LayoutInfo& layout = host_.Layout();
    const RECT card = layout.emptyCard;
    ui::FillRoundGradient(dc, card, ui::Blend(ui::kPanel, ui::kNeonBg, 10), ui::kPanel,
                          theme.S(14));
    ui::StrokeRoundFrame(dc, card, ui::kBorder, theme.S(14), 1);

    // Illustration: a window frame with the brand mark and a plus badge.
    const RECT art = layout.emptyArt;
    const int artW = art.right - art.left;
    const int artH = art.bottom - art.top;
    const int frameW = (std::min)(artW - theme.S(24), theme.S(230));
    const int frameH = (std::min)(artH - theme.S(16), theme.S(150));
    RECT frame = {art.left + (artW - frameW) / 2, art.top + (artH - frameH) / 2,
                  art.left + (artW - frameW) / 2 + frameW,
                  art.top + (artH - frameH) / 2 + frameH};
    ui::DrawGlow(dc, (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2,
                 frameW * 3 / 5, frameH * 3 / 5, ui::kNeon, 12);
    ui::FillRoundRect(dc, frame, ui::kPanel, theme.S(12));
    ui::StrokeRoundFrame(dc, frame, ui::kBorderHi, theme.S(12), 1);
    const RECT frameBar = {frame.left, frame.top, frame.right, frame.top + theme.S(26)};
    ui::FillRoundRect(dc, frameBar, ui::kPanelHi, theme.S(12));
    for (int i = 0; i < 3; ++i) {
        ui::FillCircle(dc, frame.left + theme.S(16) + i * theme.S(11), frame.top + theme.S(13),
                       theme.S(3), i == 0 ? ui::kNeonDim : ui::kBorderHi);
    }
    ui::DrawGhostMark(dc, (frame.left + frame.right) / 2,
                      (frame.top + frame.bottom) / 2 + theme.S(6), theme.S(84), ui::kNeon,
                      ui::kPanel, true);

    const int plus = theme.S(26);
    RECT plusRect = {frame.right - plus + theme.S(4), frame.bottom - plus + theme.S(3),
                     frame.right + theme.S(4), frame.bottom + theme.S(3)};
    ui::FillCircle(dc, (plusRect.left + plusRect.right) / 2, (plusRect.top + plusRect.bottom) / 2,
                   plus / 2 + theme.S(3), ui::kPanel);
    ui::FillCircle(dc, (plusRect.left + plusRect.right) / 2, (plusRect.top + plusRect.bottom) / 2,
                   plus / 2, ui::kNeon);
    const int plusGlyph = theme.S(13);
    RECT plusIcon = {(plusRect.left + plusRect.right - plusGlyph) / 2,
                     (plusRect.top + plusRect.bottom - plusGlyph) / 2, 0, 0};
    plusIcon.right = plusIcon.left + plusGlyph;
    plusIcon.bottom = plusIcon.top + plusGlyph;
    ui::DrawIcon(dc, ui::Icon::Plus, plusIcon, ui::kOnAccent, theme.S(2));

    RECT title = {layout.emptyText.left, layout.emptyText.top, layout.emptyText.right,
                  layout.emptyText.top + theme.S(38)};
    ui::DrawLabel(dc, L"No targets yet", title, theme.title, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT bodyRect = {layout.emptyText.left, title.bottom + theme.S(4), layout.emptyText.right,
                     layout.emptyCta.top - theme.S(10)};
    ui::DrawLabel(dc,
                  L"Add a target window to start tracking it, or select one on the Windows tab. "
                  L"GhostFrame keeps it visible to you and invisible to capture.",
                  bodyRect, theme.small, ui::kMuted,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    PaintCtaButton(dc, theme, layout.emptyCta, L"Go to Windows", ui::Icon::Grid,
                   ui::Icon::ChevronRight, false, hoverCta_);

    RECT hint = {layout.emptyCta.left, layout.emptyCta.bottom + theme.S(14),
                 layout.emptyText.right, layout.emptyCta.bottom + theme.S(34)};
    ui::DrawLabel(dc, L"or press Ctrl+Shift+H to capture the focused window", hint, theme.tiny,
                  ui::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

bool TargetsView::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    if (::PtInRect(&layout.cta, point)) {
        host_.SelectView(L"windows");
        host_.FocusWindowSearch();
        return true;
    }
    if (host_.Capture().Empty()) {
        if (layout.emptyCta.right > layout.emptyCta.left && ::PtInRect(&layout.emptyCta, point)) {
            host_.SelectView(L"windows");
            return true;
        }
        return false;
    }
    if (list_.Contains(x, y)) {
        ApplyListAction(list_.OnLButtonDown(x, y, ctrl, shift));
        host_.Invalidate();
        return true;
    }
    return false;
}

bool TargetsView::OnLButtonDblClk(int x, int y) {
    if (host_.Capture().Empty()) {
        return false;
    }
    ApplyListAction(list_.OnLButtonDblClk(x, y));
    host_.Invalidate();
    return true;
}

bool TargetsView::OnRButtonDown(int x, int y) {
    RemoteCapture& capture = host_.Capture();
    if (capture.Empty()) {
        return false;
    }
    const int row = list_.IndexAt(x, y);
    if (row < 0) {
        return false;
    }
    const HWND hwnd = reinterpret_cast<HWND>(list_.DataAt(row));
    const int index = capture.IndexOf(hwnd);
    if (index < 0) {
        return false;
    }
    SelectIndex(index);
    list_.Select(row);

    const RemoteSession& session = capture.At(static_cast<size_t>(index));
    const bool primary = capture.IsPrimary(session);
    const bool sticky = session.sticky;
    const bool hidden = session.hidden();

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return true;
    }
    ::AppendMenuW(menu, MF_STRING, ID_MENU_TGT_PRIMARY,
                  primary ? L"Clear primary target" : L"Set as primary target");
    ::AppendMenuW(menu, MF_STRING | (sticky ? MF_CHECKED : 0), ID_MENU_TGT_STICKY,
                  L"Sticky (re-capture when it reopens)");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_MENU_TGT_TOGGLE,
                  hidden ? L"Show (lift exclusion)" : L"Hide (exclude from capture)");
    ::AppendMenuW(menu, MF_STRING, ID_MENU_TGT_REMOVE, L"Stop tracking");

    POINT point = {x, y};
    const HWND window = host_.Window();
    ::ClientToScreen(window, &point);
    ::SetForegroundWindow(window);
    const int command = ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, point.x, point.y, 0,
                                         window, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(window, WM_NULL, 0, 0);

    std::wstring error;
    switch (command) {
        case ID_MENU_TGT_PRIMARY:
            if (capture.SetPrimary(primary ? nullptr : hwnd, error)) {
                host_.SetStatus(primary ? L"Primary target cleared" : L"Primary target set");
            } else {
                host_.SetStatus(L"Error: " + error, true);
            }
            break;
        case ID_MENU_TGT_STICKY: {
            bool result = false;
            if (capture.SetSticky(hwnd, !sticky, result, error)) {
                host_.SetStatus(result ? L"Target pinned — it will be captured when it reopens"
                                       : L"Target unpinned");
            } else {
                host_.SetStatus(L"Error: " + error, true);
            }
            break;
        }
        case ID_MENU_TGT_TOGGLE:
            ToggleStealth(index);
            break;
        case ID_MENU_TGT_REMOVE:
            RemoveSelected();
            break;
        default:
            break;
    }
    Build();
    host_.Invalidate();
    return true;
}

bool TargetsView::OnKeyDown(int virtualKey) {
    if (virtualKey == VK_DELETE) {
        RemoveSelected();
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

void TargetsView::OnControlChanged(int controlId) { (void)controlId; }

void TargetsView::OnMouseMove(int x, int y, bool& changed) {
    POINT point = {x, y};
    const bool hover = ::PtInRect(&host_.Layout().cta, point) ||
                       (host_.Capture().Empty() &&
                        ::PtInRect(&host_.Layout().emptyCta, point));
    if (hover != hoverCta_) {
        hoverCta_ = hover;
        changed = true;
    }
    changed |= list_.OnMouseMove(x, y, true);
}

void TargetsView::OnMouseLeave(bool& changed) {
    if (hoverCta_) {
        hoverCta_ = false;
        changed = true;
    }
    changed |= list_.ClearHover();
}

bool TargetsView::OnMouseWheel(int x, int y, int delta) {
    if (list_.Contains(x, y)) {
        list_.OnMouseWheel(delta);
        host_.Invalidate();
        return true;
    }
    return false;
}

bool TargetsView::OnVScroll(int code, int position) {
    list_.OnVScroll(code, position);
    host_.Invalidate();
    return true;
}

bool TargetsView::OnCommand(int id) {
    switch (id) {
        case ID_BTN_TOGGLE:
            ToggleSelection();
            return true;
        case ID_BTN_RESTORE_ALL:
            RestoreAll();
            return true;
        case ID_BTN_MIN:
            ShowSelected(SW_MINIMIZE);
            return true;
        case ID_BTN_RESTORE:
            ShowSelected(SW_RESTORE);
            return true;
        case ID_BTN_CLOSE:
            CloseSelected();
            return true;
        default:
            return false;
    }
}

bool TargetsView::HandCursorAt(int x, int y) const {
    POINT point = {x, y};
    if (::PtInRect(&host_.Layout().cta, point)) {
        return true;
    }
    if (host_.Capture().Empty()) {
        return host_.Layout().emptyCta.right > host_.Layout().emptyCta.left &&
               ::PtInRect(&host_.Layout().emptyCta, point);
    }
    return list_.Contains(x, y);
}

bool TargetsView::ButtonActive(int id) const {
    if (id != ID_BTN_TOGGLE || !SelectedValid()) {
        return false;
    }
    const RemoteCapture& capture = host_.Capture();
    return static_cast<size_t>(selected_) < capture.Count() &&
           capture.At(static_cast<size_t>(selected_)).hidden();
}

void TargetsView::OnBeforeCaptureRefresh() { refreshKeep_ = SelectedHwnd(); }

void TargetsView::OnCaptureChanged() {
    RemoteCapture& capture = host_.Capture();
    Build();
    int index = refreshKeep_ != nullptr ? capture.IndexOf(refreshKeep_) : -1;
    if (index < 0) {
        if (capture.Empty()) {
            index = -1;
        } else if (selected_ >= 0) {
            index = (std::min)(selected_, static_cast<int>(capture.Count()) - 1);
        } else {
            index = static_cast<int>(capture.Count()) - 1;
        }
    }
    if (list_.Selected() != index) {
        list_.Select(index);
    }
    selected_ = index;
    selectedHwnd_ = index >= 0 ? capture.At(static_cast<size_t>(index)).hwnd : nullptr;
    refreshKeep_ = nullptr;
    host_.Invalidate();
}

void TargetsView::OnAttachFinished(HWND lastHwnd, bool active) {
    (void)active;
    Build();
    if (lastHwnd != nullptr) {
        const int index = host_.Capture().IndexOf(lastHwnd);
        if (index >= 0) {
            SelectIndex(index);
        }
    }
}

void TargetsView::ApplyListAction(ui::ListWidget::Action action) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }

    RemoteCapture& capture = host_.Capture();
    const HWND hwnd = reinterpret_cast<HWND>(list_.DataAt(action.index));
    const int index = capture.IndexOf(hwnd);
    if (index < 0) {
        return;
    }
    selected_ = index;
    selectedHwnd_ = hwnd;

    if (action.kind == ui::ListWidget::ActionKind::RowAction) {
        ApplyRowAction(action.index, action.actionId);
        return;
    }

    if (action.kind == ui::ListWidget::ActionKind::Activate ||
        action.kind == ui::ListWidget::ActionKind::Toggle) {
        if (list_.SelectedCount() > 1) {
            ToggleSelection();
        } else {
            ToggleStealth(index);
        }
    }
}

void TargetsView::ApplyRowAction(int row, int actionId) {
    RemoteCapture& capture = host_.Capture();
    const int index = capture.IndexOf(reinterpret_cast<HWND>(list_.DataAt(row)));
    if (index < 0) {
        return;
    }
    selected_ = index;
    selectedHwnd_ = capture.At(static_cast<size_t>(index)).hwnd;
    list_.Select(row);

    switch (actionId) {
        case ID_ROW_STEALTH:
            ToggleStealth(index);
            break;
        case ID_ROW_MIN:
            ShowSelected(SW_MINIMIZE);
            break;
        case ID_ROW_CLOSE:
            CloseSelected();
            break;
        default:
            break;
    }
    host_.Invalidate();
}

void TargetsView::SelectIndex(int index) {
    selected_ = index;
    selectedHwnd_ = SelectedValid() ? host_.Capture().At(static_cast<size_t>(index)).hwnd : nullptr;
    list_.Select(index);
    host_.Invalidate();
}

bool TargetsView::SelectedValid() const {
    return selected_ >= 0 && static_cast<size_t>(selected_) < host_.Capture().Count();
}

HWND TargetsView::SelectedHwnd() const {
    const int row = list_.Selected();
    if (row >= 0) {
        return reinterpret_cast<HWND>(list_.DataAt(row));
    }
    return selectedHwnd_;
}

std::vector<size_t> TargetsView::SelectedIndices() const {
    RemoteCapture& capture = host_.Capture();
    std::vector<size_t> indices;
    for (int row : list_.SelectedIndices()) {
        const int index = capture.IndexOf(reinterpret_cast<HWND>(list_.DataAt(row)));
        if (index >= 0) {
            indices.push_back(static_cast<size_t>(index));
        }
    }
    if (indices.empty() && SelectedValid()) {
        indices.push_back(static_cast<size_t>(selected_));
    }
    return indices;
}

void TargetsView::ToggleStealth(int index) {
    RemoteCapture& capture = host_.Capture();
    if (index < 0 || static_cast<size_t>(index) >= capture.Count()) {
        host_.AttachForegroundTarget(host_.Settings().ui.hideOnAttach);
        return;
    }

    const bool hide = !capture.At(static_cast<size_t>(index)).hidden();
    std::wstring error;
    const SendOutcome outcome = host_.SendCommand(index, hide ? Cmd::Hide : Cmd::Show, error);

    selected_ = index;
    selectedHwnd_ = capture.At(static_cast<size_t>(index)).hwnd;
    list_.Select(index);
    if (outcome == SendOutcome::Ok) {
        host_.SetStatus(hide ? L"Stealth engaged — window excluded from capture"
                             : L"Capture exclusion lifted");
    } else if (outcome == SendOutcome::Error) {
        host_.SetStatus(L"ERROR: " + error, true);
    }
    host_.Invalidate();
}

void TargetsView::ToggleSelection() {
    RemoteCapture& capture = host_.Capture();
    const std::vector<size_t> indices = SelectedIndices();
    if (indices.empty()) {
        host_.AttachForegroundTarget(host_.Settings().ui.hideOnAttach);
        return;
    }
    if (indices.size() == 1) {
        ToggleStealth(static_cast<int>(indices.front()));
        return;
    }

    bool anyVisible = false;
    for (size_t index : indices) {
        if (!capture.At(index).hidden()) {
            anyVisible = true;
            break;
        }
    }
    const Cmd cmd = anyVisible ? Cmd::Hide : Cmd::Show;
    const BatchSendResult result = capture.SendToMany(indices, cmd);

    wchar_t status[192] = {};
    if (result.error == 0 && result.timeout == 0) {
        swprintf_s(status,
                   cmd == Cmd::Hide ? L"Stealth engaged — %d targets"
                                    : L"Capture exclusion lifted — %d targets",
                   result.ok);
        host_.SetStatus(status);
    } else {
        swprintf_s(status, L"%d/%zu %s — %d busy, %d failed", result.ok, indices.size(),
                   cmd == Cmd::Hide ? L"hidden" : L"restored", result.timeout, result.error);
        host_.SetStatus(status, true);
    }
    host_.Invalidate();
}

void TargetsView::RestoreAll() {
    host_.RestoreAllTargets();
    selected_ = -1;
    selectedHwnd_ = nullptr;
    Build();
    list_.Select(-1);
    host_.Invalidate();
}

void TargetsView::ShowSelected(int command) {
    RemoteCapture& capture = host_.Capture();
    const std::vector<size_t> indices = SelectedIndices();
    if (indices.empty()) {
        host_.SetStatus(L"No target selected", true);
        return;
    }

    int applied = 0;
    for (size_t index : indices) {
        const HWND target = capture.At(index).hwnd;
        if (::IsWindow(target)) {
            ::ShowWindow(target, command);
            ++applied;
        }
    }

    if (applied == 0) {
        host_.SetStatus(L"The target window is gone", true);
        return;
    }
    const bool minimize = command == SW_MINIMIZE;
    if (applied == 1) {
        host_.SetStatus(minimize ? L"Target minimized" : L"Target restored");
    } else {
        wchar_t status[96] = {};
        swprintf_s(status, minimize ? L"%d targets minimized" : L"%d targets restored", applied);
        host_.SetStatus(status);
    }
}

void TargetsView::CloseSelected() {
    RemoteCapture& capture = host_.Capture();
    const std::vector<size_t> indices = SelectedIndices();
    if (indices.empty()) {
        host_.SetStatus(L"No target selected", true);
        return;
    }

    for (size_t index : indices) {
        const HWND target = capture.At(index).hwnd;
        if (::IsWindow(target)) {
            ::PostMessageW(target, WM_CLOSE, 0, 0);
        }
    }
    RemoveSelected();
    host_.SetStatus(indices.size() > 1 ? L"Close signal sent to the selected targets"
                                       : L"Close signal sent");
}

void TargetsView::RemoveSelected() {
    RemoteCapture& capture = host_.Capture();
    std::vector<size_t> indices = SelectedIndices();
    if (indices.empty()) {
        host_.SetStatus(L"No target selected", true);
        return;
    }

    // Remove from the end so earlier indices stay valid while erasing.  The
    // daemon suppresses each removed window in its auto-capture guard, so a
    // manual removal is not undone by the next scan.
    std::sort(indices.begin(), indices.end());
    for (size_t i = indices.size(); i > 0; --i) {
        capture.Remove(indices[i - 1]);
    }

    selected_ = capture.Empty() ? -1 : 0;
    selectedHwnd_ = SelectedValid() ? capture.At(0).hwnd : nullptr;
    Build();
    list_.Select(selected_);
    host_.Invalidate();
}

}  // namespace gf