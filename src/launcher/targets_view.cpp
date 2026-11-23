// TARGETS tab: the tracked-window list, its row actions and the commands that
// operate on the current selection.

#include <windows.h>

#include <algorithm>

#include "app.h"
#include "resource.h"

namespace {

const wchar_t* StateLabel(const gf::Session& session) {
    if (session.errored()) {
        return L"ERROR";
    }
    if (session.hidden()) {
        return L"HIDDEN";
    }
    return L"VISIBLE";
}

COLORREF StateColor(const gf::Session& session) {
    if (session.errored()) {
        return ui::kWarn;
    }
    if (session.hidden()) {
        return ui::kNeon;
    }
    return ui::kMuted;
}

}  // namespace

void App::BuildTargetRows() {
    std::vector<ui::ListRow> rows;
    rows.reserve(sessions_.Count());

    for (size_t i = 0; i < sessions_.Count(); ++i) {
        const gf::Session& session = sessions_.At(i);
        ui::ListRow row;
        row.title = session.title;
        row.icon = windowEnumerator_.IconForWindow(session.hwnd);
        row.data = reinterpret_cast<intptr_t>(session.hwnd);

        const std::wstring process = windowEnumerator_.ProcessNameForWindow(session.hwnd);
        wchar_t meta[192] = {};
        if (process.empty()) {
            swprintf_s(meta, L"PID %u  ·  %s", session.pid, session.x86 ? L"x86" : L"x64");
        } else {
            swprintf_s(meta, L"%s  ·  PID %u  ·  %s", process.c_str(), session.pid,
                       session.x86 ? L"x86" : L"x64");
        }
        row.meta = meta;

        row.badges.push_back({StateLabel(session), StateColor(session)});

        row.actions.push_back(ui::ListAction{ID_ROW_STEALTH, session.hidden() ? L"SHOW" : L"HIDE",
                                             ui::kNeon, session.hidden(), false});
        row.actions.push_back(ui::ListAction{ID_ROW_MIN, L"MIN", ui::kMuted, false, false});
        row.actions.push_back(ui::ListAction{ID_ROW_CLOSE, L"CLOSE", ui::kWarn, false, true});

        rows.push_back(std::move(row));
    }

    targetsList_.SetEmptyText(L"NO TARGETS — PICK A WINDOW ON THE WINDOWS TAB");
    targetsList_.SetRows(std::move(rows));
}

void App::ApplyListAction(ui::ListWidget::Action action, bool windowsTab) {
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }

    if (windowsTab) {
        if (action.kind == ui::ListWidget::ActionKind::Activate) {
            AttachSelectedWindow();
        } else {
            UpdateAttachButton();
        }
        return;
    }

    const int index = sessions_.IndexOf(reinterpret_cast<HWND>(targetsList_.DataAt(action.index)));
    if (index < 0) {
        return;
    }
    selected_ = index;

    if (action.kind == ui::ListWidget::ActionKind::RowAction) {
        ApplyRowAction(action.index, action.actionId);
        return;
    }

    if (action.kind == ui::ListWidget::ActionKind::Activate ||
        action.kind == ui::ListWidget::ActionKind::Toggle) {
        if (targetsList_.SelectedCount() > 1) {
            ToggleSelection();
        } else {
            ToggleStealth(index);
        }
    }
    InvalidateAll();
}

void App::ApplyRowAction(int row, int actionId) {
    const int index = sessions_.IndexOf(reinterpret_cast<HWND>(targetsList_.DataAt(row)));
    if (index < 0) {
        return;
    }
    selected_ = index;
    targetsList_.Select(row);

    switch (actionId) {
        case ID_ROW_STEALTH:
            ToggleStealth(index);
            break;
        case ID_ROW_MIN:
            TargetShow(SW_MINIMIZE);
            break;
        case ID_ROW_CLOSE:
            TargetClose();
            break;
        default:
            break;
    }
    InvalidateAll();
}

void App::SelectTarget(int index) {
    SetSelected(index);
}

void App::SetSelected(int index) {
    selected_ = index;
    targetsList_.Select(index);
    if (tab_ == Tab::Targets) {
        InvalidateAll();
    }
}

std::vector<size_t> App::SelectedTargetIndices() const {
    std::vector<size_t> indices;
    for (int index : targetsList_.SelectedIndices()) {
        if (index >= 0 && static_cast<size_t>(index) < sessions_.Count()) {
            indices.push_back(static_cast<size_t>(index));
        }
    }
    if (indices.empty() && SelectedValid()) {
        indices.push_back(static_cast<size_t>(selected_));
    }
    return indices;
}

void App::ToggleStealth(int index) {
    if (index < 0 || static_cast<size_t>(index) >= sessions_.Count()) {
        AttachForeground(settings_.ui.hideOnAttach);
        return;
    }

    const bool hide = !sessions_.At(static_cast<size_t>(index)).hidden();
    std::wstring error;
    const gf::SendOutcome outcome =
        SendCommand(index, hide ? gf::Cmd::Hide : gf::Cmd::Show, error);

    selected_ = index;
    targetsList_.Select(index);
    if (outcome == gf::SendOutcome::Ok) {
        SetStatus(hide ? L"STEALTH ENGAGED — WINDOW EXCLUDED FROM CAPTURE"
                       : L"CAPTURE EXCLUSION LIFTED");
    } else if (outcome == gf::SendOutcome::Error) {
        SetStatus(L"ERROR: " + error, true);
    }
    InvalidateAll();
}

void App::ToggleSelection() {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        AttachForeground(settings_.ui.hideOnAttach);
        return;
    }
    if (indices.size() == 1) {
        ToggleStealth(static_cast<int>(indices.front()));
        return;
    }

    // One direction for the whole selection: if anything is still visible, hide
    // everything; otherwise lift the exclusion from everything.
    bool anyVisible = false;
    for (size_t index : indices) {
        if (!sessions_.At(index).hidden()) {
            anyVisible = true;
            break;
        }
    }
    const gf::Cmd cmd = anyVisible ? gf::Cmd::Hide : gf::Cmd::Show;
    const gf::BatchSendResult result = sessions_.SendToMany(indices, cmd);

    wchar_t status[192] = {};
    if (result.error == 0 && result.timeout == 0) {
        swprintf_s(status,
                   cmd == gf::Cmd::Hide ? L"STEALTH ENGAGED — %d TARGETS"
                                        : L"CAPTURE EXCLUSION LIFTED — %d TARGETS",
                   result.ok);
        SetStatus(status);
    } else {
        swprintf_s(status, L"%d/%zu %s — %d BUSY, %d FAILED", result.ok, indices.size(),
                   cmd == gf::Cmd::Hide ? L"HIDDEN" : L"RESTORED", result.timeout, result.error);
        SetStatus(status, true);
    }
    InvalidateAll();
}

void App::RestoreAll() {
    pendingCommand_ = PendingCommand{};
    sessions_.RestoreAll();
    selected_ = -1;
    BuildTargetRows();
    SetStatus(L"ALL TARGETS RESTORED");
    UpdateTray();
    InvalidateAll();
}

void App::TargetShow(int command) {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    int applied = 0;
    for (size_t index : indices) {
        const HWND target = sessions_.At(index).hwnd;
        if (::IsWindow(target)) {
            ::ShowWindow(target, command);
            ++applied;
        }
    }

    if (applied == 0) {
        SetStatus(L"THE TARGET WINDOW IS GONE", true);
        return;
    }
    const bool minimize = command == SW_MINIMIZE;
    if (applied == 1) {
        SetStatus(minimize ? L"TARGET MINIMIZED" : L"TARGET RESTORED");
    } else {
        wchar_t status[96] = {};
        swprintf_s(status, minimize ? L"%d TARGETS MINIMIZED" : L"%d TARGETS RESTORED", applied);
        SetStatus(status);
    }
}

void App::TargetClose() {
    const std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    for (size_t index : indices) {
        const HWND target = sessions_.At(index).hwnd;
        if (::IsWindow(target)) {
            ::PostMessageW(target, WM_CLOSE, 0, 0);
        }
    }
    RemoveSelected();
    SetStatus(indices.size() > 1 ? L"CLOSE SIGNAL SENT TO SELECTED TARGETS"
                                 : L"CLOSE SIGNAL SENT");
}

void App::RemoveSelected() {
    std::vector<size_t> indices = SelectedTargetIndices();
    if (indices.empty()) {
        SetStatus(L"NO TARGET SELECTED", true);
        return;
    }

    // Remove from the end so earlier indices stay valid while erasing.
    std::sort(indices.begin(), indices.end());
    for (size_t i = indices.size(); i > 0; --i) {
        const gf::Session& session = sessions_.At(indices[i - 1]);
        // The user removed this window on purpose: never auto-capture it again
        // until its process recreates it.
        autoGuard_.Suppress(session.pid, session.createTime, session.hwnd);
        sessions_.Remove(indices[i - 1]);
    }

    selected_ = sessions_.Empty() ? -1 : 0;
    BuildTargetRows();
    targetsList_.Select(selected_);
    UpdateTray();
    InvalidateAll();
}
