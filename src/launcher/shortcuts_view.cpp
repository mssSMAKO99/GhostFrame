// SHORTCUTS tab: create and manage Desktop / Start Menu / Startup links.

#include "shortcuts_view.h"

#include <commdlg.h>
#include <windows.h>

#include <algorithm>
#include <cwchar>

#include "emoji_icon.h"
#include "resource.h"
#include "shortcuts.h"

namespace gf {

namespace {

const ViewInfo kInfo = {L"shortcuts", L"Shortcuts", L"Shortcuts",
                        L"Create Desktop, Start Menu and Startup shortcuts.",
                        ui::Icon::Bolt};

std::wstring Trim(const std::wstring& text) {
    const wchar_t* spaces = L" \t\r\n";
    const std::wstring::size_type first = text.find_first_not_of(spaces);
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const std::wstring::size_type last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

const wchar_t* DefaultDescription(ShortcutKind kind) {
    switch (kind) {
        case ShortcutKind::Daemon:
            return L"Run GhostFrame capture without opening the window.";
        case ShortcutKind::AppWrapper:
            return L"Launch an application through GhostFrame.";
        default:
            return L"Open the GhostFrame window; the daemon starts automatically.";
    }
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void PlaceChild(HWND parent, int id, const RECT& rect) {
    HWND child = ::GetDlgItem(parent, id);
    if (child != nullptr) {
        ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
                       rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void PaintCard(HDC dc, const ui::Theme& theme, const RECT& card, ui::Icon icon,
               const wchar_t* title, const wchar_t* subtitle) {
    ui::FillRoundRect(dc, card, ui::kPanel, theme.S(14));
    ui::StrokeRoundFrame(dc, card, ui::kBorder, theme.S(14), 1);

    const int iconSize = theme.S(18);
    RECT iconRect = {card.left + theme.S(18), card.top + theme.S(13), 0, 0};
    iconRect.right = iconRect.left + iconSize;
    iconRect.bottom = iconRect.top + iconSize;
    ui::DrawIcon(dc, icon, iconRect, ui::kNeon, theme.S(2));

    RECT titleRect = {iconRect.right + theme.S(10), card.top + theme.S(9),
                      card.right - theme.S(14), card.top + theme.S(28)};
    ui::DrawLabel(dc, title, titleRect, theme.title, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT subtitleRect = {titleRect.left, card.top + theme.S(28), card.right - theme.S(14),
                         card.top + theme.S(44)};
    ui::DrawLabel(dc, subtitle, subtitleRect, theme.tiny, ui::kMuted,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    const RECT divider = {card.left + theme.S(14), card.top + theme.S(46),
                          card.right - theme.S(14), card.top + theme.S(47)};
    ui::FillSolid(dc, divider, ui::kBorder);
}

void PaintChoice(HDC dc, const ui::Theme& theme, const RECT& rect, const wchar_t* label,
                 bool selected, bool hovered) {
    const int radius = theme.S(8);
    const COLORREF background = selected ? ui::Blend(ui::kPanelHi, ui::kNeonBg, 70)
                                         : (hovered ? ui::kPanelHover : ui::kPanelHi);
    ui::FillRoundRect(dc, rect, background, radius);
    ui::StrokeRoundFrame(dc, rect, selected ? ui::kNeonDim : ui::kBorder, radius, 1);
    ui::DrawLabel(dc, label, rect, theme.small,
                  selected ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void PaintFieldLabel(HDC dc, const ui::Theme& theme, const RECT& label, const wchar_t* text) {
    ui::DrawLabel(dc, text, label, theme.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

const wchar_t* const kDestinationLabels[3] = {L"Desktop", L"Start Menu", L"Startup"};
const wchar_t* const kKindLabels[3] = {L"GhostFrame UI", L"Background daemon", L"App wrapper"};

}  // namespace

const ViewInfo& ShortcutsView::Info() const { return kInfo; }

void ShortcutsView::CreateControls(HWND parent, HINSTANCE instance) {
    (void)parent;
    (void)instance;
    host_.AddEdit(ID_EDIT_SC_NAME, L"Shortcut name");
    host_.AddEdit(ID_EDIT_SC_DESC, L"Description (optional)");
    host_.AddEdit(ID_EDIT_SC_APP, L"C:\\Path\\to\\app.exe");
    host_.AddEdit(ID_EDIT_SC_ARGS, L"Extra arguments (optional)");
    host_.AddButton(ID_BTN_SC_BROWSE, L"Browse…", L"Pick the application to wrap");
    ApplyControls();
}

std::wstring ShortcutsView::EditText(int id) const {
    HWND edit = ::GetDlgItem(host_.Window(), id);
    if (edit == nullptr) {
        return std::wstring();
    }
    wchar_t buffer[1024] = {};
    ::GetWindowTextW(edit, buffer, _countof(buffer));
    return buffer;
}

void ShortcutsView::SetEditText(int id, const std::wstring& text) {
    if (HWND edit = ::GetDlgItem(host_.Window(), id)) {
        ::SetWindowTextW(edit, text.c_str());
    }
}

void ShortcutsView::ApplyControls() {
    const bool wrapper = kind_ == ShortcutKind::AppWrapper;
    for (int id : {ID_EDIT_SC_NAME, ID_EDIT_SC_DESC, ID_EDIT_SC_APP, ID_EDIT_SC_ARGS,
                   ID_BTN_SC_BROWSE}) {
        if (HWND child = ::GetDlgItem(host_.Window(), id)) {
            const bool show = active_ && (wrapper || (id != ID_EDIT_SC_APP &&
                                                      id != ID_EDIT_SC_ARGS &&
                                                      id != ID_BTN_SC_BROWSE));
            ::ShowWindow(child, show ? SW_SHOW : SW_HIDE);
        }
    }
}

void ShortcutsView::LoadEntries() {
    std::wstring error;
    std::vector<ShortcutEntry> loaded;
    if (!LoadShortcutManifest(loaded, error)) {
        host_.SetStatus(L"Error: " + error, true);
        loaded.clear();
    }
    entries_ = std::move(loaded);
}

void ShortcutsView::Build() {
    LoadEntries();

    std::vector<ui::ListRow> rows;
    rows.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        const ShortcutEntry& entry = entries_[i];
        ui::ListRow row;
        row.title = entry.name.empty() ? entry.path : entry.name;
        row.data = static_cast<intptr_t>(i);

        ShortcutLocation location = ShortcutLocation::Desktop;
        for (int candidate = 0; candidate < static_cast<int>(ShortcutLocation::Count);
             ++candidate) {
            const auto value = static_cast<ShortcutLocation>(candidate);
            const std::wstring folder = ShortcutLocationPath(value);
            if (!folder.empty() && _wcsnicmp(entry.path.c_str(), folder.c_str(), folder.size()) ==
                                       0) {
                location = value;
                break;
            }
        }
        row.cells.push_back(ShortcutLocationLabel(location));
        row.cells.push_back(ShortcutKindLabel(ShortcutKindFromKey(entry.kind)));
        row.cells.push_back(entry.path);
        row.actions.push_back(
            ui::ListAction{ID_BTN_SC_REMOVE, L"Remove", ui::kWarn, false, false});
        rows.push_back(std::move(row));
    }
    list_.SetEmptyText(L"NO SHORTCUTS YET \u2014 CHOOSE A DESTINATION AND PRESS CREATE");
    list_.SetRows(std::move(rows));
    ApplyControls();
}

void ShortcutsView::SetActive(bool active) {
    active_ = active;
    ApplyControls();
    if (!active) {
        return;
    }
    Build();
    if (!initialized_) {
        initialized_ = true;
        SetEditText(ID_EDIT_SC_NAME, ShortcutKindDefaultName(kind_));
        SetEditText(ID_EDIT_SC_DESC, DefaultDescription(kind_));
    }
    host_.SetStatus(L"Create a Desktop, Start Menu or Startup shortcut for GhostFrame");
}

void ShortcutsView::Layout() {
    LayoutInfo& layout = host_.Layout();
    const ui::Theme& theme = host_.Theme();
    const RECT body = layout.body;
    const int gap = theme.S(10);

    layout.cta = {body.right - theme.S(186), layout.pageHeader.top + theme.S(9), body.right,
                  layout.pageHeader.top + theme.S(9) + theme.S(40)};
    layout.secondaryCta = {layout.cta.left - theme.S(8) - theme.S(136), layout.cta.top,
                           layout.cta.left - theme.S(8), layout.cta.bottom};

    // The header has two actions, so the title and subtitle must stop before
    // them instead of running underneath.
    layout.pageTitle.right = layout.secondaryCta.left - theme.S(12);
    layout.pageSubtitle.right = layout.secondaryCta.left - theme.S(12);

    layout.shortcutsToolbar = {body.left, body.top, body.right, body.top + theme.S(108)};
    layout.shortcutsForm = {body.left, layout.shortcutsToolbar.bottom + gap,
                            body.left + (body.right - body.left) * 52 / 100, body.bottom};
    layout.shortcutsListCard = {layout.shortcutsForm.right + gap, layout.shortcutsForm.top,
                                body.right, body.bottom};
    layout.shortcutsList = {layout.shortcutsListCard.left + theme.S(14),
                            layout.shortcutsListCard.top + theme.S(52),
                            layout.shortcutsListCard.right - theme.S(14),
                            layout.shortcutsListCard.bottom - theme.S(12)};

    // Destination and kind choices side by side in the toolbar.
    const int innerLeft = layout.shortcutsToolbar.left + theme.S(16);
    const int innerRight = layout.shortcutsToolbar.right - theme.S(16);
    const int columnGap = theme.S(24);
    const int columnW = (innerRight - innerLeft - columnGap) / 2;
    const int pillTop = layout.shortcutsToolbar.top + theme.S(32);
    const int pillH = theme.S(28);
    for (int i = 0; i < 3; ++i) {
        const int leftW = (columnW - theme.S(12)) / 3;
        layout.shortcutsDestination[i] = {innerLeft + i * (leftW + theme.S(6)), pillTop,
                                          innerLeft + i * (leftW + theme.S(6)) + leftW,
                                          pillTop + pillH};
        const int rightX = innerLeft + columnW + columnGap;
        layout.shortcutsKind[i] = {rightX + i * (leftW + theme.S(6)), pillTop,
                                   rightX + i * (leftW + theme.S(6)) + leftW, pillTop + pillH};
    }

    // Details form.  The spacing shrinks on short screens so the last edit stays
    // inside the card instead of overlapping the footer.
    const RECT form = layout.shortcutsForm;
    const int formLeft = form.left + theme.S(16);
    const int formRight = form.right - theme.S(16);
    const int available = form.bottom - form.top;
    int start = theme.S(50);
    const int labelH = theme.S(13);
    int labelGap = theme.S(4);
    int editH = theme.S(28);
    int rowGap = theme.S(6);
    const int browseW = theme.S(84);

    const auto neededHeight = [&]() {
        const int block = labelH + labelGap + editH + rowGap;
        return start + 3 * block + (labelH + labelGap + editH) + theme.S(10);
    };
    while (neededHeight() > available && rowGap > theme.S(3)) {
        --rowGap;
    }
    while (neededHeight() > available && start > theme.S(46)) {
        --start;
    }
    while (neededHeight() > available && labelGap > theme.S(2)) {
        --labelGap;
    }
    while (neededHeight() > available && editH > theme.S(18)) {
        --editH;
    }

    int rowTop = form.top + start;

    layout.shortcutsNameLabel = {formLeft, rowTop, formRight, rowTop + labelH};
    layout.shortcutsName = {formLeft, rowTop + labelH + labelGap, formRight,
                            rowTop + labelH + labelGap + editH};
    rowTop = layout.shortcutsName.bottom + rowGap;

    layout.shortcutsDescLabel = {formLeft, rowTop, formRight, rowTop + labelH};
    layout.shortcutsDesc = {formLeft, rowTop + labelH + labelGap, formRight,
                            rowTop + labelH + labelGap + editH};
    rowTop = layout.shortcutsDesc.bottom + rowGap;

    layout.shortcutsAppLabel = {formLeft, rowTop, formRight, rowTop + labelH};
    layout.shortcutsApp = {formLeft, rowTop + labelH + labelGap,
                           formRight - browseW - theme.S(8),
                           rowTop + labelH + labelGap + editH};
    layout.shortcutsBrowse = {formRight - browseW, rowTop + labelH + labelGap, formRight,
                              rowTop + labelH + labelGap + editH};
    rowTop = layout.shortcutsApp.bottom + rowGap;

    layout.shortcutsArgsLabel = {formLeft, rowTop, formRight, rowTop + labelH};
    layout.shortcutsArgs = {formLeft, rowTop + labelH + labelGap, formRight,
                            rowTop + labelH + labelGap + editH};

    PlaceChild(host_.Window(), ID_EDIT_SC_NAME, layout.shortcutsName);
    PlaceChild(host_.Window(), ID_EDIT_SC_DESC, layout.shortcutsDesc);
    PlaceChild(host_.Window(), ID_EDIT_SC_APP, layout.shortcutsApp);
    PlaceChild(host_.Window(), ID_EDIT_SC_ARGS, layout.shortcutsArgs);
    PlaceChild(host_.Window(), ID_BTN_SC_BROWSE, layout.shortcutsBrowse);

    list_.SetRowHeight(theme.RowHeight(44));
    list_.SetColumns({
        {L"SHORTCUT", -1, DT_LEFT, false},
        {L"DESTINATION", 108, DT_LEFT, true},
        {L"KIND", 132, DT_LEFT, true},
        {L"PATH", 260, DT_LEFT, true},
    });
    list_.SetCheckboxes(false);
    list_.SetBounds(layout.shortcutsList, theme);
}

void ShortcutsView::Paint(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const LayoutInfo& layout = host_.Layout();

    // Toolbar with the two choice groups.
    ui::FillRoundRect(dc, layout.shortcutsToolbar, ui::kPanel, theme.S(14));
    ui::StrokeRoundFrame(dc, layout.shortcutsToolbar, ui::kBorder, theme.S(14), 1);
    const int innerLeft = layout.shortcutsToolbar.left + theme.S(16);
    const int columnW =
        (layout.shortcutsToolbar.right - theme.S(16) - innerLeft - theme.S(24)) / 2;
    RECT destinationLabel = {innerLeft, layout.shortcutsToolbar.top + theme.S(12), 0, 0};
    destinationLabel.right = innerLeft + columnW;
    destinationLabel.bottom = destinationLabel.top + theme.S(14);
    PaintFieldLabel(dc, theme, destinationLabel, L"DESTINATION");
    RECT kindLabel = {innerLeft + columnW + theme.S(24), destinationLabel.top,
                      layout.shortcutsToolbar.right - theme.S(16), destinationLabel.bottom};
    PaintFieldLabel(dc, theme, kindLabel, L"KIND");

    for (int i = 0; i < 3; ++i) {
        PaintChoice(dc, theme, layout.shortcutsDestination[i], kDestinationLabels[i],
                    static_cast<int>(location_) == i, hoverDestination_ == i);
        PaintChoice(dc, theme, layout.shortcutsKind[i], kKindLabels[i],
                    static_cast<int>(kind_) == i, hoverKind_ == i);
    }

    RECT hint = {innerLeft, layout.shortcutsToolbar.bottom - theme.S(34),
                 layout.shortcutsToolbar.right - theme.S(16),
                 layout.shortcutsToolbar.bottom - theme.S(10)};
    ui::DrawLabel(dc,
                  kind_ == ShortcutKind::AppWrapper
                      ? L"The link starts GhostFrame with --launch \"<app>\" and any extra arguments."
                      : (kind_ == ShortcutKind::Daemon
                             ? L"The link starts the capture daemon with --daemon."
                             : L"The link opens this window; the connect path starts the daemon."),
                  hint, theme.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    PaintCard(dc, theme, layout.shortcutsForm, ui::Icon::Plus, L"Details",
              L"The shortcut name, description and wrapped application.");
    PaintFieldLabel(dc, theme, layout.shortcutsNameLabel, L"NAME");
    PaintFieldLabel(dc, theme, layout.shortcutsDescLabel, L"DESCRIPTION");
    if (kind_ == ShortcutKind::AppWrapper) {
        PaintFieldLabel(dc, theme, layout.shortcutsAppLabel, L"APPLICATION");
        PaintFieldLabel(dc, theme, layout.shortcutsArgsLabel, L"EXTRA ARGUMENTS");
    }

    PaintCard(dc, theme, layout.shortcutsListCard, ui::Icon::Bolt, L"Created shortcuts",
              L"Remove a link here to delete its file.");
    list_.Paint(dc, theme, ::GetFocus() == host_.Window());
}

void ShortcutsView::PaintHeader(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const LayoutInfo& layout = host_.Layout();
    PaintCtaButton(dc, theme, layout.secondaryCta, L"Remove all", ui::Icon::Trash, ui::Icon::None,
                   false, hoverCta_ == 1);
    PaintCtaButton(dc, theme, layout.cta, L"Create shortcut", ui::Icon::Plus, ui::Icon::None,
                   true, hoverCta_ == 0);
}

int ShortcutsView::DestinationAt(int x, int y) const {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 3; ++i) {
        if (::PtInRect(&layout.shortcutsDestination[i], point)) {
            return i;
        }
    }
    return -1;
}

int ShortcutsView::KindAt(int x, int y) const {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 3; ++i) {
        if (::PtInRect(&layout.shortcutsKind[i], point)) {
            return i;
        }
    }
    return -1;
}

bool ShortcutsView::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    if (layout.cta.right > layout.cta.left && ::PtInRect(&layout.cta, point)) {
        CreateFromForm();
        return true;
    }
    if (layout.secondaryCta.right > layout.secondaryCta.left &&
        ::PtInRect(&layout.secondaryCta, point)) {
        RemoveAll();
        return true;
    }
    const int destination = DestinationAt(x, y);
    if (destination >= 0) {
        location_ = static_cast<ShortcutLocation>(destination);
        host_.Invalidate();
        return true;
    }
    const int kind = KindAt(x, y);
    if (kind >= 0) {
        SetKind(static_cast<ShortcutKind>(kind));
        return true;
    }
    if (list_.Contains(x, y)) {
        const ui::ListWidget::Action action = list_.OnLButtonDown(x, y, ctrl, shift);
        if (action.kind == ui::ListWidget::ActionKind::RowAction &&
            action.actionId == ID_BTN_SC_REMOVE) {
            RemoveEntry(action.index);
        } else {
            host_.Invalidate();
        }
        return true;
    }
    return true;
}

void ShortcutsView::SetKind(ShortcutKind kind) {
    if (kind == kind_) {
        return;
    }
    const ShortcutKind previous = kind_;
    const std::wstring currentName = EditText(ID_EDIT_SC_NAME);
    const std::wstring currentDesc = EditText(ID_EDIT_SC_DESC);
    kind_ = kind;
    if (currentName.empty() || currentName == ShortcutKindDefaultName(previous)) {
        SetEditText(ID_EDIT_SC_NAME, ShortcutKindDefaultName(kind_));
    }
    if (currentDesc.empty() || currentDesc == DefaultDescription(previous)) {
        SetEditText(ID_EDIT_SC_DESC, DefaultDescription(kind_));
    }
    ApplyControls();
    host_.Invalidate();
}

void ShortcutsView::BrowseForApp() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_.Window();
    ofn.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&ofn)) {
        return;
    }
    SetEditText(ID_EDIT_SC_APP, file);
    const std::wstring currentName = EditText(ID_EDIT_SC_NAME);
    if (currentName.empty() || currentName == ShortcutKindDefaultName(kind_)) {
        std::wstring stem = file;
        const std::wstring::size_type slash = stem.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            stem = stem.substr(slash + 1);
        }
        const std::wstring::size_type dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) {
            stem = stem.substr(0, dot);
        }
        SetEditText(ID_EDIT_SC_NAME, stem);
    }
    host_.Invalidate();
}

void ShortcutsView::CreateFromForm() {
    ShortcutSpec spec;
    spec.kind = kind_;
    spec.location = location_;
    spec.name = Trim(EditText(ID_EDIT_SC_NAME));
    spec.description = Trim(EditText(ID_EDIT_SC_DESC));
    if (spec.name.empty()) {
        spec.name = ShortcutKindDefaultName(kind_);
    }
    if (spec.description.empty()) {
        spec.description = DefaultDescription(kind_);
    }

    if (kind_ == ShortcutKind::AppWrapper) {
        const std::wstring app = Trim(EditText(ID_EDIT_SC_APP));
        if (app.empty()) {
            host_.SetStatus(L"Error: pick the application to wrap first", true);
            return;
        }
        if (!FileExists(app)) {
            host_.SetStatus(L"Error: the wrapped application does not exist", true);
            return;
        }
        spec.arguments = ShortcutWrapperArguments(app, Trim(EditText(ID_EDIT_SC_ARGS)));
    } else if (kind_ == ShortcutKind::Daemon) {
        spec.arguments = L"--daemon";
    }

    std::wstring iconPath = ExecutablePath();
    if (!host_.Settings().appearance.iconEmoji.empty() && FileExists(EmojiIconPath())) {
        iconPath = EmojiIconPath();
    }

    ShortcutEntry entry;
    std::wstring error;
    if (!CreateShortcut(spec, iconPath, entry, error)) {
        host_.SetStatus(L"Error: " + error, true);
        return;
    }
    if (!SaveEntriesWith(entry)) {
        std::wstring deleteError;
        DeleteShortcutFile(entry.path, deleteError);
        host_.SetStatus(L"Error: the shortcut could not be recorded in the manifest", true);
        return;
    }
    Build();
    host_.SetStatus(L"Created \"" + spec.name + L"\" on " +
                    ShortcutLocationLabel(location_));
}

bool ShortcutsView::SaveEntriesWith(const ShortcutEntry& entry) {
    std::vector<ShortcutEntry> next = entries_;
    bool replaced = false;
    for (ShortcutEntry& existing : next) {
        if (_wcsicmp(existing.path.c_str(), entry.path.c_str()) == 0) {
            existing = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        next.push_back(entry);
    }
    std::wstring error;
    if (!SaveShortcutManifest(next, error)) {
        host_.SetStatus(L"Error: " + error, true);
        return false;
    }
    entries_ = std::move(next);
    return true;
}

void ShortcutsView::RemoveEntry(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return;
    }
    const ShortcutEntry entry = entries_[static_cast<size_t>(index)];
    std::wstring error;
    if (!DeleteShortcutFile(entry.path, error)) {
        host_.SetStatus(L"Error: " + error, true);
        return;
    }
    entries_.erase(entries_.begin() + index);
    if (!SaveShortcutManifest(entries_, error)) {
        host_.SetStatus(L"Error: " + error, true);
        return;
    }
    Build();
    host_.SetStatus(L"Shortcut removed");
}

void ShortcutsView::RemoveAll() {
    if (entries_.empty()) {
        host_.SetStatus(L"There are no shortcuts to remove");
        return;
    }
    std::wstring error;
    const int removed = RemoveAllShortcuts(error);
    if (!error.empty()) {
        host_.SetStatus(L"Error: " + error, true);
        return;
    }
    entries_.clear();
    Build();
    wchar_t message[96] = {};
    swprintf_s(message, L"Removed %d shortcut%s", removed, removed == 1 ? L"" : L"s");
    host_.SetStatus(message);
}

bool ShortcutsView::OnKeyDown(int virtualKey) {
    const ui::ListWidget::Action action = list_.OnKeyDown(virtualKey);
    if (action.kind == ui::ListWidget::ActionKind::RowAction &&
        action.actionId == ID_BTN_SC_REMOVE) {
        RemoveEntry(action.index);
        return true;
    }
    if (action.kind != ui::ListWidget::ActionKind::None) {
        host_.Invalidate();
        return true;
    }
    if (virtualKey == VK_DELETE && list_.Selected() >= 0) {
        RemoveEntry(list_.Selected());
        return true;
    }
    return false;
}

void ShortcutsView::OnMouseMove(int x, int y, bool& changed) {
    const int destination = DestinationAt(x, y);
    const int kind = KindAt(x, y);
    int cta = -1;
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    if (layout.cta.right > layout.cta.left && ::PtInRect(&layout.cta, point)) {
        cta = 0;
    } else if (layout.secondaryCta.right > layout.secondaryCta.left &&
               ::PtInRect(&layout.secondaryCta, point)) {
        cta = 1;
    }
    if (destination != hoverDestination_ || kind != hoverKind_ || cta != hoverCta_) {
        hoverDestination_ = destination;
        hoverKind_ = kind;
        hoverCta_ = cta;
        changed = true;
    }
    changed |= list_.OnMouseMove(x, y, true);
}

void ShortcutsView::OnMouseLeave(bool& changed) {
    const bool had = hoverDestination_ >= 0 || hoverKind_ >= 0 || hoverCta_ >= 0;
    hoverDestination_ = -1;
    hoverKind_ = -1;
    hoverCta_ = -1;
    changed |= had;
    changed |= list_.ClearHover();
}

bool ShortcutsView::OnMouseWheel(int x, int y, int delta) {
    if (list_.Contains(x, y)) {
        list_.OnMouseWheel(delta);
        host_.Invalidate();
        return true;
    }
    return false;
}

bool ShortcutsView::OnVScroll(int code, int position) {
    list_.OnVScroll(code, position);
    host_.Invalidate();
    return true;
}

bool ShortcutsView::OnCommand(int id) {
    switch (id) {
        case ID_BTN_SC_BROWSE:
            BrowseForApp();
            return true;
        case ID_BTN_SC_CREATE:
            CreateFromForm();
            return true;
        case ID_BTN_SC_REMOVE_ALL:
            RemoveAll();
            return true;
        default:
            return false;
    }
}

bool ShortcutsView::HandCursorAt(int x, int y) const {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    if (layout.cta.right > layout.cta.left && ::PtInRect(&layout.cta, point)) {
        return true;
    }
    if (layout.secondaryCta.right > layout.secondaryCta.left &&
        ::PtInRect(&layout.secondaryCta, point)) {
        return true;
    }
    if (DestinationAt(x, y) >= 0 || KindAt(x, y) >= 0) {
        return true;
    }
    return list_.Contains(x, y);
}

}  // namespace gf