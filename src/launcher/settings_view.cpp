// SETTINGS tab: hotkeys, auto-capture rules, general behavior and maintenance.

#include "settings_view.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cwchar>

#include "autostart.h"
#include "emoji_icon.h"
#include "resource.h"

namespace gf {

namespace {

const ViewInfo kInfo = {L"settings", L"Settings", L"Settings",
                        L"Tune hotkeys, auto-capture rules and behavior.", ui::Icon::Gear};

void PlaceChild(HWND parent, int id, const RECT& rect) {
    HWND child = ::GetDlgItem(parent, id);
    if (child != nullptr) {
        ::SetWindowPos(child, nullptr, rect.left, rect.top, rect.right - rect.left,
                       rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

const COLORREF kAccentSwatches[5] = {
    RGB(47, 226, 123), RGB(72, 170, 255), RGB(168, 120, 255), RGB(245, 184, 64),
    RGB(255, 104, 148),
};
const wchar_t* const kAccentLabels[6] = {L"Auto",  L"Emerald", L"Sky",
                                         L"Violet", L"Amber",  L"Rose"};

// Index 0 means "follow the theme"; the other entries are fixed backgrounds.
const COLORREF kBackgroundTops[5] = {
    RGB(0, 0, 0), RGB(7, 12, 10), RGB(20, 26, 32), RGB(8, 15, 30), RGB(240, 243, 241),
};
const COLORREF kBackgroundBottoms[5] = {
    RGB(0, 0, 0), RGB(13, 26, 19), RGB(31, 40, 48), RGB(14, 28, 54), RGB(224, 231, 227),
};
const wchar_t* const kBackgroundLabels[5] = {L"Theme", L"Ink", L"Slate", L"Navy", L"Paper"};

}  // namespace

const ViewInfo& SettingsView::Info() const { return kInfo; }

void SettingsView::CreateControls(HWND parent, HINSTANCE instance) {
    (void)parent;
    (void)instance;
    host_.AddButton(ID_BTN_SET_RESET, L"Reset", L"Restore the default hotkeys");
    host_.AddButton(ID_BTN_SET_ADD_FG, L"Add app", L"Auto-capture the current foreground app");
    host_.AddButton(ID_BTN_SET_BROWSE, L"Browse exe…", L"Pick an executable to auto-capture");
    host_.AddButton(ID_BTN_SET_APPLY, L"Apply now",
                    L"Run the rules against the windows open right now");
    host_.AddButton(ID_BTN_APPEAR_RESET, L"Reset to defaults",
                    L"Restore the default theme, colors, density and tab order");
    host_.AddButton(ID_BTN_APPEAR_IMAGE, L"Choose image…",
                    L"Pick a background picture (PNG, JPEG or BMP)");
    host_.AddButton(ID_BTN_APPEAR_CLEAR, L"Clear", L"Remove the background picture");
    host_.AddEdit(ID_EDIT_APPEAR_EMOJI, L"Paste an emoji (Win+.)");
    host_.AddButton(ID_BTN_ICON_APPLY, L"Apply", L"Use the emoji as the GhostFrame icon");
    host_.AddButton(ID_BTN_ICON_CLEAR, L"Reset", L"Go back to the bundled GhostFrame icon");
    host_.AddButton(ID_BTN_SET_DELETE_NOW, L"Delete now…",
                    L"Remove GhostFrame using the selected self-delete mode");
}

ui::ListWidget* SettingsView::ActiveList() {
    if (section_ == Section::AutoCapture) {
        return &rules_;
    }
    if (section_ == Section::Appearance) {
        return &tabs_;
    }
    return &hotkeys_;
}

const ui::ListWidget* SettingsView::ActiveList() const {
    if (section_ == Section::AutoCapture) {
        return &rules_;
    }
    if (section_ == Section::Appearance) {
        return &tabs_;
    }
    return &hotkeys_;
}

void SettingsView::Build() {
    Settings& settings = host_.Settings();
    std::vector<ui::ListRow> hotkeys;
    const int hotkeyCount = section_ == Section::General ? 3 : kHotkeyCount;
    for (int i = 0; i < hotkeyCount; ++i) {
        const HotkeyAction action = static_cast<HotkeyAction>(i);
        ui::ListRow row;
        row.title = HotkeyActionLabel(action);
        row.meta = rebinding_ && rebindAction_ == action
                       ? L"press a combination…"
                       : FormatHotkey(settings.hotkeys[i].modifiers,
                                      settings.hotkeys[i].virtualKey);
        if (row.meta.empty()) {
            row.meta = L"Not set";
        }
        row.metaMono = true;
        row.metaChip = true;
        row.data = i;
        hotkeys.push_back(std::move(row));
    }
    hotkeys_.SetEmptyText(L"No hotkeys");
    hotkeys_.SetRows(std::move(hotkeys));

    std::vector<ui::ListRow> rules;
    for (size_t i = 0; i < settings.autoAttach.size(); ++i) {
        const AutoAttachRule& rule = settings.autoAttach[i];
        ui::ListRow row;
        row.title = rule.value;
        row.meta = (rule.match == L"exe" ? L"Executable" : L"Window title") +
                   std::wstring(L"  ·  ") + (rule.hide ? L"hide on attach" : L"keep visible");
        row.data = static_cast<intptr_t>(i);
        row.actions.push_back(ui::ListAction{ID_BTN_SET_ENABLE, rule.enabled ? L"On" : L"Off",
                                             ui::kNeon, rule.enabled, false});
        row.actions.push_back(ui::ListAction{ID_BTN_SET_HIDE, rule.hide ? L"Hide" : L"Show",
                                             ui::kNeon, rule.hide, false});
        row.actions.push_back(ui::ListAction{ID_BTN_SET_REMOVE, L"", ui::kWarn, false, true,
                                             ui::Icon::Trash, true});
        rules.push_back(std::move(row));
    }
    rules_.SetEmptyText(L"No auto-capture rules yet — add the foreground app below");
    rules_.SetRows(std::move(rules));
    if (selectedRule_ >= static_cast<int>(settings.autoAttach.size())) {
        selectedRule_ = settings.autoAttach.empty() ? -1
                                                    : static_cast<int>(settings.autoAttach.size()) - 1;
    }

    // APPEARANCE: the tab strip, in the user's order.
    std::vector<std::wstring> knownIds;
    for (const ViewInfo& info : host_.ViewCatalog()) {
        knownIds.push_back(info.id);
    }
    NormalizeTabs(settings.appearance, knownIds);
    std::vector<ui::ListRow> tabs;
    for (size_t i = 0; i < settings.appearance.tabs.size(); ++i) {
        const TabPreference& preference = settings.appearance.tabs[i];
        ui::ListRow row;
        const wchar_t* label = nullptr;
        for (const ViewInfo& info : host_.ViewCatalog()) {
            if (preference.id == info.id) {
                label = info.label;
                break;
            }
        }
        row.title = label != nullptr ? label : preference.id;
        row.meta = preference.visible ? L"shown in the tab strip" : L"hidden";
        row.data = static_cast<intptr_t>(i);
        if (i > 0) {
            row.actions.push_back(
                ui::ListAction{ID_BTN_TAB_UP, L"Up", ui::kMuted, false, false});
        }
        if (i + 1 < settings.appearance.tabs.size()) {
            row.actions.push_back(
                ui::ListAction{ID_BTN_TAB_DOWN, L"Down", ui::kMuted, false, false});
        }
        // The settings tab hosts this editor, so it must stay reachable.
        if (preference.id != L"settings") {
            row.actions.push_back(ui::ListAction{ID_BTN_TAB_VISIBLE,
                                                 preference.visible ? L"Hide" : L"Show",
                                                 ui::kNeon, preference.visible, false});
        }
        tabs.push_back(std::move(row));
    }
    tabs_.SetEmptyText(L"No tabs");
    tabs_.SetRows(std::move(tabs));
    ApplyControls();
}

void SettingsView::SetActive(bool active) {
    active_ = active;
    ApplyControls();
    if (active) {
        host_.SetStatus(L"Settings — click a hotkey to rebind; Esc cancels");
    }
}

void SettingsView::ApplyControls() {
    const bool resetVisible = active_ && (section_ == Section::General || section_ == Section::Hotkeys);
    const bool rulesVisible = active_ && section_ == Section::AutoCapture;
    const bool appearanceVisible = active_ && section_ == Section::Appearance;
    const bool advancedVisible = active_ && section_ == Section::Advanced;
    for (int id : {ID_BTN_SET_RESET, ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE, ID_BTN_SET_APPLY,
                   ID_BTN_APPEAR_RESET, ID_BTN_APPEAR_IMAGE, ID_BTN_APPEAR_CLEAR,
                   ID_BTN_ICON_APPLY, ID_BTN_ICON_CLEAR, ID_BTN_SET_DELETE_NOW}) {
        if (HWND child = ::GetDlgItem(host_.Window(), id)) {
            bool show = false;
            if (id == ID_BTN_SET_RESET) {
                show = resetVisible;
            } else if (id == ID_BTN_SET_ADD_FG || id == ID_BTN_SET_BROWSE ||
                       id == ID_BTN_SET_APPLY) {
                show = rulesVisible;
            } else if (id == ID_BTN_SET_DELETE_NOW) {
                show = advancedVisible;
            } else {
                show = appearanceVisible;
            }
            if (show) {
                show = ChildInsideArea(id);
            }
            ::ShowWindow(child, show ? SW_SHOW : SW_HIDE);
        }
    }
    if (HWND edit = ::GetDlgItem(host_.Window(), ID_EDIT_APPEAR_EMOJI)) {
        ::ShowWindow(edit, appearanceVisible && ChildInsideArea(ID_EDIT_APPEAR_EMOJI) ? SW_SHOW
                                                                                      : SW_HIDE);
    }
}

bool SettingsView::ChildInsideArea(int id) const {
    HWND child = ::GetDlgItem(host_.Window(), id);
    if (child == nullptr) {
        return false;
    }
    RECT rect = {};
    if (!::GetWindowRect(child, &rect)) {
        return false;
    }
    POINT points[2] = {{rect.left, rect.top}, {rect.right, rect.bottom}};
    ::MapWindowPoints(HWND_DESKTOP, host_.Window(), points, 2);
    const RECT area = host_.Layout().settingsArea;
    return points[0].x >= area.left && points[1].x <= area.right &&
           points[0].y >= area.top && points[1].y <= area.bottom;
}

int SettingsView::MaxScroll() const {
    const RECT area = host_.Layout().settingsArea;
    const int areaHeight = area.bottom - area.top;
    if (areaHeight <= 0) {
        return 0;
    }
    return (std::max)(0, contentHeight_ - areaHeight);
}

void SettingsView::ScrollBy(int amount) {
    const int next = (std::max)(0, (std::min)(scroll_ + amount, MaxScroll()));
    if (next == scroll_) {
        return;
    }
    scroll_ = next;
    host_.Relayout();
    host_.Invalidate();
}

void SettingsView::SetSection(Section section) {
    if (section_ == section) {
        return;
    }
    section_ = section;
    scroll_ = 0;
    if (section_ == Section::Appearance) {
        LoadEmojiField();
    }
    host_.Relayout();
    Build();
    host_.Invalidate();
}

void SettingsView::LoadEmojiField() {
    HWND edit = ::GetDlgItem(host_.Window(), ID_EDIT_APPEAR_EMOJI);
    if (edit == nullptr) {
        return;
    }
    emojiUpdating_ = true;
    ::SetWindowTextW(edit, host_.Settings().appearance.iconEmoji.c_str());
    emojiUpdating_ = false;
}

void SettingsView::ApplyEmoji() {
    HWND edit = ::GetDlgItem(host_.Window(), ID_EDIT_APPEAR_EMOJI);
    if (edit == nullptr) {
        return;
    }
    wchar_t buffer[128] = {};
    ::GetWindowTextW(edit, buffer, _countof(buffer));
    const std::wstring normalized = NormalizeEmoji(buffer);
    if (normalized.empty()) {
        host_.SetStatus(L"Error: paste or type a single emoji first", true);
        return;
    }
    Settings& settings = host_.Settings();
    if (settings.appearance.iconEmoji == normalized &&
        !host_.Settings().appearance.iconEmoji.empty()) {
        host_.SetStatus(L"The emoji icon is already applied");
        return;
    }
    const std::wstring previous = settings.appearance.iconEmoji;
    settings.appearance.iconEmoji = normalized;
    if (!ApplyAppearanceChange()) {
        settings.appearance.iconEmoji = previous;
        ApplyAppearanceChange();
        LoadEmojiField();
        host_.SetStatus(L"Error: that emoji could not be rendered — keeping the previous icon",
                        true);
        return;
    }
    emojiUpdating_ = true;
    ::SetWindowTextW(edit, normalized.c_str());
    emojiUpdating_ = false;
    host_.SetStatus(L"Emoji icon applied");
}

void SettingsView::ClearEmojiIcon() {
    Settings& settings = host_.Settings();
    if (settings.appearance.iconEmoji.empty()) {
        host_.SetStatus(L"The bundled icon is already in use");
        return;
    }
    const std::wstring previous = settings.appearance.iconEmoji;
    settings.appearance.iconEmoji.clear();
    if (!ApplyAppearanceChange()) {
        settings.appearance.iconEmoji = previous;
        ApplyAppearanceChange();
        LoadEmojiField();
        host_.SetStatus(L"Error: the bundled icon could not be loaded — keeping the emoji icon",
                        true);
        return;
    }
    LoadEmojiField();
    host_.SetStatus(L"Bundled GhostFrame icon restored");
}

void SettingsView::OnControlChanged(int controlId) {
    if (controlId != ID_EDIT_APPEAR_EMOJI || emojiUpdating_) {
        return;
    }
    HWND edit = ::GetDlgItem(host_.Window(), ID_EDIT_APPEAR_EMOJI);
    if (edit == nullptr) {
        return;
    }
    wchar_t buffer[128] = {};
    ::GetWindowTextW(edit, buffer, _countof(buffer));
    const std::wstring normalized = NormalizeEmoji(buffer);
    Settings& settings = host_.Settings();
    if (normalized == settings.appearance.iconEmoji) {
        return;
    }
    if (normalized.empty()) {
        // Keep the current icon while the field is temporarily empty.
        host_.Invalidate();
        return;
    }
    const std::wstring previous = settings.appearance.iconEmoji;
    settings.appearance.iconEmoji = normalized;
    if (!ApplyAppearanceChange()) {
        settings.appearance.iconEmoji = previous;
        ApplyAppearanceChange();
        host_.SetStatus(L"Error: that emoji could not be rendered — keeping the previous icon",
                        true);
        return;
    }
    host_.SetStatus(L"Emoji icon applied");
}

void SettingsView::Layout() {
    LayoutInfo& layout = host_.Layout();
    const ui::Theme& theme = host_.Theme();
    const int cx = layout.body.left;
    const int areaTop = layout.body.top;
    const int areaBottom = layout.body.bottom;
    const int areaHeight = (std::max)(0, areaBottom - areaTop);

    const int navW = theme.S(theme.m.settingsNavWidth);
    layout.settingsNav = {cx, areaTop, cx + navW, areaBottom};
    for (int i = 0; i < 5; ++i) {
        const int itemTop = areaTop + theme.S(10) + i * theme.S(46);
        layout.settingsNavItems[i] = {layout.settingsNav.left + theme.S(8), itemTop,
                                      layout.settingsNav.right - theme.S(8),
                                      itemTop + theme.S(40)};
    }

    layout.settingsArea = {layout.settingsNav.right + theme.S(16), areaTop,
                           layout.body.right, areaBottom};
    const int gap = theme.S(12);

    // Natural height of the current section.  The page scrolls only when it is
    // taller than the body, so on a normal desktop nothing moves and on a short
    // screen the footer never collides with the last card.
    int contentHeight = areaHeight;
    if (section_ == Section::Appearance) {
        const int leftNatural =
            theme.S(156) + theme.S(10) + theme.S(84) + theme.S(10) + theme.S(100) +
            theme.S(10) + theme.S(34);
        const int rightNatural = theme.S(196) + theme.S(10) + theme.S(112) + theme.S(10) +
                                 theme.S(120);
        contentHeight = (std::max)(areaHeight, (std::max)(leftNatural, rightNatural));
    } else if (section_ == Section::Hotkeys) {
        const int cardH = theme.S(46) + kHotkeyCount * theme.S(34) + theme.S(46);
        contentHeight = (std::max)(areaHeight, cardH);
    } else if (section_ == Section::Advanced) {
        const int leftNatural = theme.S(46) + 5 * theme.S(44);
        const int rightNatural = theme.S(214) + gap + theme.S(192);
        contentHeight =
            (std::max)(areaHeight, (std::max)(leftNatural, rightNatural));
    }
    contentHeight_ = contentHeight;
    scroll_ = (std::max)(0, (std::min)(scroll_, contentHeight - areaHeight));

    const int top = areaTop - scroll_;
    const int bottom = top + contentHeight;

    if (section_ == Section::General) {
        // Two columns: the shortcut list gets the full page height, while the
        // behavior switches and the auto-capture summary share the right column.
        // This keeps all three shortcuts visible even at the minimum window size.
        const int areaW = layout.settingsArea.right - layout.settingsArea.left;
        const int columnGap = theme.S(12);
        const int leftW = (areaW - columnGap) / 2;
        const int rightX = layout.settingsArea.left + leftW + columnGap;
        const int behaviorH = theme.S(222);

        layout.settingsCard[0] = {layout.settingsArea.left, top,
                                  layout.settingsArea.left + leftW, bottom};
        layout.settingsCard[1] = {rightX, top, layout.settingsArea.right, top + behaviorH};
        layout.settingsCard[2] = {rightX, top + behaviorH + gap, layout.settingsArea.right,
                                  bottom};

        layout.settingsHotkeyList = {layout.settingsCard[0].left + theme.S(14),
                                     layout.settingsCard[0].top + theme.S(46),
                                     layout.settingsCard[0].right - theme.S(14),
                                     layout.settingsCard[0].bottom - theme.S(12)};

        const int autoH = layout.settingsCard[2].bottom - layout.settingsCard[2].top;
        layout.settingsManage = {layout.settingsCard[2].right - theme.S(14) - theme.S(108),
                                 layout.settingsCard[2].top + (autoH - theme.S(30)) / 2,
                                 layout.settingsCard[2].right - theme.S(14),
                                 layout.settingsCard[2].top + (autoH + theme.S(30)) / 2};
    } else if (section_ == Section::Appearance) {
        const int areaW = layout.settingsArea.right - layout.settingsArea.left;
        const int leftW = areaW * 46 / 100;
        const int colGap = theme.S(10);
        const int rightW = areaW - leftW - colGap;
        const int leftX = layout.settingsArea.left;
        const int rightX = leftX + leftW + colGap;

        const int themeH = theme.S(156);
        const int densityH = theme.S(84);
        const int accentH = theme.S(100);
        layout.appearanceCard[0] = {leftX, top, leftX + leftW, top + themeH};
        layout.appearanceCard[1] = {leftX, layout.appearanceCard[0].bottom + colGap, leftX + leftW,
                                    layout.appearanceCard[0].bottom + colGap + densityH};
        layout.appearanceCard[2] = {leftX, layout.appearanceCard[1].bottom + colGap, leftX + leftW,
                                    layout.appearanceCard[1].bottom + colGap + accentH};
        layout.appearanceReset = {leftX, layout.appearanceCard[2].bottom + colGap,
                                  leftX + theme.S(176),
                                  layout.appearanceCard[2].bottom + colGap + theme.S(34)};

        // Theme tiles, 2x2.
        {
            const RECT card = layout.appearanceCard[0];
            const int innerLeft = card.left + theme.S(14);
            const int innerRight = card.right - theme.S(14);
            const int innerTop = card.top + theme.S(48);
            const int tileGap = theme.S(8);
            const int tileW = (innerRight - innerLeft - tileGap) / 2;
            const int tileH = (card.bottom - theme.S(12) - innerTop - tileGap) / 2;
            for (int i = 0; i < 4; ++i) {
                const int column = i % 2;
                const int row = i / 2;
                const int x = innerLeft + column * (tileW + tileGap);
                const int y = innerTop + row * (tileH + tileGap);
                layout.appearancePresets[i] = {x, y, x + tileW, y + tileH};
            }
        }

        // Density pills.
        {
            const RECT card = layout.appearanceCard[1];
            const int innerLeft = card.left + theme.S(14);
            const int innerRight = card.right - theme.S(14);
            const int pillGap = theme.S(8);
            const int pillW = (innerRight - innerLeft - pillGap) / 2;
            const int pillTop = card.top + theme.S(50);
            for (int i = 0; i < 2; ++i) {
                const int x = innerLeft + i * (pillW + pillGap);
                layout.appearanceDensity[i] = {x, pillTop, x + pillW, pillTop + theme.S(26)};
            }
        }

        // Accent swatches.
        {
            const RECT card = layout.appearanceCard[2];
            const int innerLeft = card.left + theme.S(14);
            const int innerRight = card.right - theme.S(14);
            const int cellGap = theme.S(4);
            const int cellW = (innerRight - innerLeft - cellGap * 5) / 6;
            const int cellTop = card.top + theme.S(50);
            for (int i = 0; i < 6; ++i) {
                const int x = innerLeft + i * (cellW + cellGap);
                layout.appearanceAccents[i] = {x, cellTop, x + cellW, cellTop + theme.S(40)};
            }
        }

        // Background card.
        {
            layout.appearanceCard[3] = {rightX, top, rightX + rightW, top + theme.S(196)};
            const RECT card = layout.appearanceCard[3];
            const int innerLeft = card.left + theme.S(14);
            const int innerRight = card.right - theme.S(14);

            const int modeGap = theme.S(8);
            const int modeW = (innerRight - innerLeft - modeGap) / 2;
            const int modeTop = card.top + theme.S(50);
            for (int i = 0; i < 2; ++i) {
                const int x = innerLeft + i * (modeW + modeGap);
                layout.appearanceModes[i] = {x, modeTop, x + modeW, modeTop + theme.S(28)};
            }

            const int swatchGap = theme.S(6);
            const int swatchW = (innerRight - innerLeft - swatchGap * 4) / 5;
            const int swatchTop = modeTop + theme.S(36);
            for (int i = 0; i < 5; ++i) {
                const int x = innerLeft + i * (swatchW + swatchGap);
                layout.appearanceSwatches[i] = {x, swatchTop, x + swatchW,
                                                swatchTop + theme.S(40)};
            }

            const int buttonBottom = card.bottom - theme.S(14);
            const int buttonTop = buttonBottom - theme.S(30);
            const int clearW = theme.S(84);
            layout.appearanceImage = {innerLeft, buttonTop, innerRight - clearW - theme.S(8),
                                      buttonBottom};
            layout.appearanceImageClear = {innerRight - clearW, buttonTop, innerRight,
                                           buttonBottom};
        }

        // Icon card: emoji preview, edit and apply/reset.  The hint gets its own
        // row below the controls so it can never overlap the edit field.
        {
            layout.appearanceCard[5] = {rightX, layout.appearanceCard[3].bottom + colGap,
                                        rightX + rightW, layout.appearanceCard[3].bottom +
                                                             colGap + theme.S(112)};
            const RECT card = layout.appearanceCard[5];
            const int innerLeft = card.left + theme.S(14);
            const int innerRight = card.right - theme.S(14);
            const int rowTop = card.top + theme.S(50);
            const int rowH = theme.S(30);
            const int preview = theme.S(30);
            layout.appearanceEmojiPreview = {innerLeft, rowTop, innerLeft + preview,
                                             rowTop + preview};
            const int applyW = theme.S(54);
            const int clearW = theme.S(62);
            const int buttonGap = theme.S(6);
            layout.appearanceEmojiClear = {innerRight - clearW, rowTop, innerRight, rowTop + rowH};
            layout.appearanceEmojiApply = {layout.appearanceEmojiClear.left - buttonGap - applyW,
                                           rowTop,
                                           layout.appearanceEmojiClear.left - buttonGap,
                                           rowTop + rowH};
            layout.appearanceEmoji = {layout.appearanceEmojiPreview.right + theme.S(8), rowTop,
                                      layout.appearanceEmojiApply.left - buttonGap,
                                      rowTop + rowH};
        }

        // Tabs card fills the rest of the column.
        layout.appearanceCard[4] = {rightX, layout.appearanceCard[5].bottom + colGap,
                                    rightX + rightW, bottom};
        layout.appearanceTabs = {layout.appearanceCard[4].left + theme.S(14),
                                 layout.appearanceCard[4].top + theme.S(46),
                                 layout.appearanceCard[4].right - theme.S(14),
                                 layout.appearanceCard[4].bottom - theme.S(12)};
    } else if (section_ == Section::Hotkeys) {
        const int cardH = theme.S(46) + kHotkeyCount * theme.S(34) + theme.S(46);
        layout.settingsCard[0] = {layout.settingsArea.left, top, layout.settingsArea.right,
                                  (std::min)(bottom, top + cardH)};
        layout.settingsHotkeyList = {layout.settingsCard[0].left + theme.S(14),
                                     layout.settingsCard[0].top + theme.S(46),
                                     layout.settingsCard[0].right - theme.S(14),
                                     layout.settingsCard[0].bottom - theme.S(46)};
    } else if (section_ == Section::AutoCapture) {
        layout.settingsCard[0] = {layout.settingsArea.left, top, layout.settingsArea.right, bottom};
        layout.settingsActionRow = {layout.settingsCard[0].left + theme.S(14),
                                    layout.settingsCard[0].bottom - theme.S(50),
                                    layout.settingsCard[0].right - theme.S(14),
                                    layout.settingsCard[0].bottom - theme.S(14)};
        layout.settingsRuleList = {layout.settingsCard[0].left + theme.S(14),
                                   layout.settingsCard[0].top + theme.S(46),
                                   layout.settingsCard[0].right - theme.S(14),
                                   layout.settingsActionRow.top - theme.S(10)};
    } else {
        // Two columns: behavior on the left, self-delete above maintenance on
        // the right.  This gives every control a home even at the minimum
        // window height instead of stacking a tall page past the footer.
        const int areaW = layout.settingsArea.right - layout.settingsArea.left;
        const int columnGap = theme.S(12);
        const int leftW = (areaW - columnGap) / 2;
        const int rightX = layout.settingsArea.left + leftW + columnGap;
        const int selfH = theme.S(214);

        layout.settingsCard[0] = {layout.settingsArea.left, top,
                                  layout.settingsArea.left + leftW, bottom};
        layout.settingsCard[1] = {rightX, top, layout.settingsArea.right, top + selfH};
        layout.settingsCard[2] = {rightX, top + selfH + gap, layout.settingsArea.right, bottom};

        // Self-delete card: description, mode pills, delete-on-exit switch and
        // the explicit action.
        const RECT selfCard = layout.settingsCard[1];
        const int selfLeft = selfCard.left + theme.S(14);
        const int selfRight = selfCard.right - theme.S(14);
        const int modeGap = theme.S(6);
        const int modeW = (selfRight - selfLeft - modeGap * 2) / 3;
        const int modeTop = selfCard.top + theme.S(88);
        for (int i = 0; i < 3; ++i) {
            const int x = selfLeft + i * (modeW + modeGap);
            layout.settingsSelfModes[i] = {x, modeTop, x + modeW, modeTop + theme.S(28)};
        }
        layout.settingsSelfExit = {selfLeft, selfCard.top + theme.S(122), selfRight,
                                   selfCard.top + theme.S(166)};
        layout.settingsSelfDeleteNow = {selfLeft, selfCard.top + theme.S(170), selfRight,
                                        selfCard.top + theme.S(206)};

        // Maintenance card: the explanation, then the two actions pinned to the
        // bottom so they stay inside the card whatever its height.
        const RECT maintenance = layout.settingsCard[2];
        layout.settingsLogCta = {maintenance.left + theme.S(14),
                                 maintenance.bottom - theme.S(84),
                                 maintenance.right - theme.S(14),
                                 maintenance.bottom - theme.S(48)};
        layout.settingsRestoreCta = {maintenance.left + theme.S(14),
                                     maintenance.bottom - theme.S(44),
                                     maintenance.right - theme.S(14),
                                     maintenance.bottom - theme.S(8)};
    }

    // Child controls.
    if (section_ == Section::General || section_ == Section::Hotkeys) {
        RECT reset = {layout.settingsCard[0].right - theme.S(14) - theme.S(84),
                      layout.settingsCard[0].top + theme.S(11),
                      layout.settingsCard[0].right - theme.S(14),
                      layout.settingsCard[0].top + theme.S(11) + theme.S(30)};
        PlaceChild(host_.Window(), ID_BTN_SET_RESET, reset);
    } else if (section_ == Section::AutoCapture) {
        const RECT row = layout.settingsActionRow;
        const int gap2 = theme.S(10);
        const int buttonWidth = (row.right - row.left - 2 * gap2) / 3;
        const int ids[3] = {ID_BTN_SET_ADD_FG, ID_BTN_SET_BROWSE, ID_BTN_SET_APPLY};
        for (int i = 0; i < 3; ++i) {
            RECT rect = {row.left + i * (buttonWidth + gap2), row.top,
                         row.left + i * (buttonWidth + gap2) + buttonWidth, row.bottom};
            PlaceChild(host_.Window(), ids[i], rect);
        }
    } else if (section_ == Section::Appearance) {
        PlaceChild(host_.Window(), ID_BTN_APPEAR_RESET, layout.appearanceReset);
        PlaceChild(host_.Window(), ID_BTN_APPEAR_IMAGE, layout.appearanceImage);
        PlaceChild(host_.Window(), ID_BTN_APPEAR_CLEAR, layout.appearanceImageClear);
        PlaceChild(host_.Window(), ID_EDIT_APPEAR_EMOJI, layout.appearanceEmoji);
        PlaceChild(host_.Window(), ID_BTN_ICON_APPLY, layout.appearanceEmojiApply);
        PlaceChild(host_.Window(), ID_BTN_ICON_CLEAR, layout.appearanceEmojiClear);
    } else if (section_ == Section::Advanced) {
        PlaceChild(host_.Window(), ID_BTN_SET_DELETE_NOW, layout.settingsSelfDeleteNow);
    }

    hotkeys_.SetRowHeight(theme.RowHeight(34));
    hotkeys_.SetColumns({});
    hotkeys_.SetCheckboxes(false);
    hotkeys_.SetBounds(layout.settingsHotkeyList, host_.Theme());

    rules_.SetRowHeight(theme.RowHeight(43));
    rules_.SetColumns({});
    rules_.SetCheckboxes(false);
    rules_.SetBounds(layout.settingsRuleList, host_.Theme());

    tabs_.SetRowHeight(theme.RowHeight(43));
    tabs_.SetColumns({});
    tabs_.SetCheckboxes(false);
    tabs_.SetBounds(layout.appearanceTabs, host_.Theme());

    // Re-evaluate child visibility after placing them: controls that scrolled
    // past the viewport must be hidden, and controls that scrolled back in must
    // reappear.
    ApplyControls();
}

void SettingsView::Paint(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const Settings& settings = host_.Settings();
    const LayoutInfo& layout = host_.Layout();

    // Secondary navigation rail.
    ui::FillRoundRect(dc, layout.settingsNav, ui::kPanel, theme.S(14));
    ui::StrokeRoundFrame(dc, layout.settingsNav, ui::kBorder, theme.S(14), 1);

    static const wchar_t* const kNavNames[] = {L"General", L"Appearance", L"Hotkeys",
                                               L"Auto-Capture", L"Advanced"};
    static const ui::Icon kNavIcons[] = {ui::Icon::Gear, ui::Icon::Dot, ui::Icon::Keyboard,
                                         ui::Icon::Camera, ui::Icon::Layers};
    const int active = static_cast<int>(section_);
    for (int i = 0; i < 5; ++i) {
        const RECT item = layout.settingsNavItems[i];
        const bool isActive = i == active;
        const bool hovered = hoverNav_ == i;
        if (isActive) {
            ui::FillRoundRect(dc, item, ui::Blend(ui::kPanelHi, ui::kNeonBg, 50), theme.S(9));
            ui::StrokeRoundFrame(dc, item, ui::kNeonDim, theme.S(9), 1);
        } else if (hovered) {
            ui::FillRoundRect(dc, item, ui::kPanelHi, theme.S(9));
        }
        const int iconSize = theme.S(16);
        RECT icon = {item.left + theme.S(12), item.top + ((item.bottom - item.top) - iconSize) / 2,
                     0, 0};
        icon.right = icon.left + iconSize;
        icon.bottom = icon.top + iconSize;
        ui::DrawIcon(dc, kNavIcons[i], icon,
                     isActive ? ui::kNeon : (hovered ? ui::kText : ui::kMuted), theme.S(2));
        RECT label = {icon.right + theme.S(11), item.top, item.right, item.bottom};
        ui::DrawLabel(dc, kNavNames[i], label, theme.bodyStrong,
                      isActive ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // The scrollable card area is clipped so nothing can paint over the rail,
// the page header or the footer while it is scrolled.
    const int savedDc = ::SaveDC(dc);
    ::IntersectClipRect(dc, layout.settingsArea.left, layout.settingsArea.top,
                        layout.settingsArea.right, layout.settingsArea.bottom);

    const bool settingsFocus = ::GetFocus() == host_.Window();
    if (section_ == Section::General) {
        PaintCard(dc, layout.settingsCard[0], ui::Icon::Keyboard, L"Keyboard Shortcuts",
                  L"Control GhostFrame from anywhere.");
        hotkeys_.Paint(dc, theme, settingsFocus);

        PaintCard(dc, layout.settingsCard[1], ui::Icon::Shield, L"General Behavior",
                  L"How GhostFrame behaves around your desktop.");
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[1], 0), L"Hide on attach",
                    L"Engage capture exclusion as soon as a target is attached.",
                    settings.ui.hideOnAttach, hoverSwitch_ == 0);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[1], 1), L"Minimize to tray",
                    L"Keep GhostFrame in the notification area instead of the taskbar.",
                    settings.ui.minimizeToTray, hoverSwitch_ == 1);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[1], 2), L"Exclude self",
                    L"GhostFrame stays invisible to capture as well.",
                    !settings.ui.noSelfExclude, hoverSwitch_ == 2);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[1], 3), L"Start with Windows",
                    L"Run the capture daemon automatically when you sign in.",
                    IsAutostartEnabled(), hoverSwitch_ == 3);

        PaintCard(dc, layout.settingsCard[2], ui::Icon::Camera, L"Auto-Capture",
                  L"Attach matching applications automatically.");
        wchar_t summary[128] = {};
        swprintf_s(summary, L"%zu rule%s configured", settings.autoAttach.size(),
                   settings.autoAttach.size() == 1 ? L"" : L"s");
        RECT summaryRect = {layout.settingsCard[2].left + theme.S(18),
                            layout.settingsCard[2].top + theme.S(54),
                            layout.settingsManage.left - theme.S(12),
                            layout.settingsCard[2].bottom - theme.S(8)};
        ui::DrawLabel(dc, summary, summaryRect, theme.small, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        PaintCtaButton(dc, theme, layout.settingsManage, L"Manage", ui::Icon::None,
                       ui::Icon::ChevronRight, false, hoverAction_ == 0);
    } else if (section_ == Section::Appearance) {
        PaintAppearance(dc);
    } else if (section_ == Section::Hotkeys) {
        PaintCard(dc, layout.settingsCard[0], ui::Icon::Keyboard, L"Keyboard Shortcuts",
                  L"Click a row, then press the new combination. Esc cancels.");
        hotkeys_.Paint(dc, theme, settingsFocus);
        RECT hint = {layout.settingsCard[0].left + theme.S(16),
                     layout.settingsCard[0].bottom - theme.S(34),
                     layout.settingsCard[0].right - theme.S(16),
                     layout.settingsCard[0].bottom - theme.S(12)};
        ui::DrawLabel(dc, L"At least one of Ctrl, Alt, Shift or Win is required.", hint, theme.tiny,
                      ui::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (section_ == Section::AutoCapture) {
        PaintCard(dc, layout.settingsCard[0], ui::Icon::Camera, L"Auto-Capture Rules",
                  L"These applications are attached automatically when their windows appear.");
        rules_.Paint(dc, theme, settingsFocus);
    } else {
        PaintCard(dc, layout.settingsCard[0], ui::Icon::Shield, L"General Behavior",
                  L"How GhostFrame behaves around your desktop.");
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[0], 0), L"Hide on attach",
                    L"Engage capture exclusion as soon as a target is attached.",
                    settings.ui.hideOnAttach, hoverSwitch_ == 0);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[0], 1), L"Minimize to tray",
                    L"Keep GhostFrame in the notification area instead of the taskbar.",
                    settings.ui.minimizeToTray, hoverSwitch_ == 1);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[0], 2), L"Exclude self",
                    L"GhostFrame stays invisible to capture as well.",
                    !settings.ui.noSelfExclude, hoverSwitch_ == 2);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[0], 3), L"Start with Windows",
                    L"Run the capture daemon automatically when you sign in.",
                    IsAutostartEnabled(), hoverSwitch_ == 3);
        PaintSwitch(dc, SwitchRowRect(layout.settingsCard[0], 4), L"Follow foreground",
                    L"Make the focused tracked window the primary target.",
                    settings.foregroundTracking, hoverSwitch_ == 4);

        PaintCard(dc, layout.settingsCard[1], ui::Icon::Trash, L"Self-delete",
                  L"Remove GhostFrame from this PC.");
        const wchar_t* explanation = L"";
        switch (settings.ui.selfDelete) {
            case SelfDeleteMode::ExeOnly:
                explanation = L"Removes the app, icon, shortcuts and startup entry. "
                              L"Settings and logs stay.";
                break;
            case SelfDeleteMode::Everything:
                explanation = L"Removes the app, icon, shortcuts, startup entry and the "
                              L"whole data folder.";
                break;
            default:
                explanation = L"Nothing is deleted automatically. Pick a mode, then use "
                              L"the button below.";
                break;
        }
        {
            const RECT card = layout.settingsCard[1];
            RECT text = {card.left + theme.S(16), card.top + theme.S(52),
                         card.right - theme.S(16), card.top + theme.S(86)};
            ui::DrawLabel(dc, explanation, text, theme.tiny, ui::kTextDim,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        for (int i = 0; i < 3; ++i) {
            const SelfDeleteMode mode = static_cast<SelfDeleteMode>(i);
            PaintPill(dc, layout.settingsSelfModes[i], SelfDeleteModeLabel(mode),
                      settings.ui.selfDelete == mode, hoverSelfMode_ == i);
        }
        const bool canDeleteOnExit = settings.ui.selfDelete != SelfDeleteMode::Never;
        PaintSwitch(dc, layout.settingsSelfExit, L"Delete on exit",
                    canDeleteOnExit ? L"Remove the selected files when you close GhostFrame."
                                    : L"Choose Exe only or Everything first.",
                    settings.ui.deleteOnExit && canDeleteOnExit, hoverSelfExit_);

        PaintCard(dc, layout.settingsCard[2], ui::Icon::Layers, L"Maintenance",
                  L"Diagnostics and last-resort actions.");
        RECT maintenanceText = {layout.settingsCard[2].left + theme.S(18),
                                layout.settingsCard[2].top + theme.S(52),
                                layout.settingsCard[2].right - theme.S(18),
                                layout.settingsLogCta.top - theme.S(6)};
        ui::DrawLabel(dc,
                      L"Settings live in %LOCALAPPDATA%\\GhostFrame\\settings.json. "
                      L"Closing the window leaves the capture daemon running.",
                      maintenanceText, theme.tiny, ui::kTextDim,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        PaintCtaButton(dc, theme, layout.settingsLogCta, L"Open log folder", ui::Icon::Folder,
                       ui::Icon::ArrowUpRight, false, hoverAction_ == 1);
        PaintCtaButton(dc, theme, layout.settingsRestoreCta, L"Restore all", ui::Icon::Eye,
                       ui::Icon::None, false, hoverAction_ == 2);
    }

    ::RestoreDC(dc, savedDc);

    // A slim scrollbar appears only when the page is taller than the viewport.
    const int maxScroll = MaxScroll();
    if (maxScroll > 0) {
        const int areaHeight = layout.settingsArea.bottom - layout.settingsArea.top;
        const int trackWidth = theme.S(5);
        RECT track = {layout.settingsArea.right - trackWidth - theme.S(2),
                      layout.settingsArea.top + theme.S(4),
                      layout.settingsArea.right - theme.S(2),
                      layout.settingsArea.bottom - theme.S(4)};
        ui::FillRoundRect(dc, track, ui::kPanelHi, trackWidth / 2);
        const int trackHeight = track.bottom - track.top;
        int thumbHeight = areaHeight * trackHeight / contentHeight_;
        thumbHeight = (std::max)(theme.S(28), thumbHeight);
        if (thumbHeight > trackHeight) {
            thumbHeight = trackHeight;
        }
        const int travel = trackHeight - thumbHeight;
        const int thumbTop = track.top + travel * scroll_ / maxScroll;
        RECT thumb = {track.left, thumbTop, track.right, thumbTop + thumbHeight};
        ui::FillRoundRect(dc, thumb, ui::kBorderHi, trackWidth / 2);
    }
}

void SettingsView::PaintPill(HDC dc, const RECT& rect, const wchar_t* label, bool selected,
                             bool hovered) {
    const ui::Theme& theme = host_.Theme();
    const int radius = theme.S(8);
    COLORREF background = selected ? ui::Blend(ui::kPanelHi, ui::kNeonBg, 70)
                                   : (hovered ? ui::kPanelHover : ui::kPanelHi);
    ui::FillRoundRect(dc, rect, background, radius);
    ui::StrokeRoundFrame(dc, rect, selected ? ui::kNeonDim : ui::kBorder, radius, 1);
    ui::DrawLabel(dc, label, rect, theme.small,
                  selected ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void SettingsView::PaintSwatch(HDC dc, const RECT& rect, COLORREF color, bool selected,
                               bool hovered, const wchar_t* label) {
    const ui::Theme& theme = host_.Theme();
    const int diameter = theme.S(22);
    const int cx = (rect.left + rect.right) / 2;
    const int cy = rect.top + diameter / 2 + theme.S(2);
    if (selected) {
        ui::FillCircle(dc, cx, cy, diameter / 2 + theme.S(3), ui::kPanelHi);
        ui::StrokeRoundFrame(dc, RECT{cx - diameter / 2 - theme.S(3), cy - diameter / 2 - theme.S(3),
                                      cx + diameter / 2 + theme.S(3),
                                      cy + diameter / 2 + theme.S(3)},
                             ui::kNeon, diameter / 2 + theme.S(3), 1);
    } else if (hovered) {
        ui::FillCircle(dc, cx, cy, diameter / 2 + theme.S(3), ui::kPanelHover);
    }
    ui::FillCircle(dc, cx, cy, diameter / 2, color);
    ui::StrokeRoundFrame(dc, RECT{cx - diameter / 2, cy - diameter / 2, cx + diameter / 2,
                                  cy + diameter / 2},
                         selected ? ui::kNeon : ui::kBorderHi, diameter / 2, 1);
    RECT text = {rect.left, cy + diameter / 2 + theme.S(3), rect.right, rect.bottom};
    ui::DrawLabel(dc, label, text, theme.tiny,
                  selected ? ui::kText : ui::kTextDim,
                  DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void SettingsView::PaintTabsCard(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    // The list paints its own empty state and scrollbar.
    tabs_.Paint(dc, theme, ::GetFocus() == host_.Window());
}

void SettingsView::PaintAppearance(HDC dc) {
    const ui::Theme& theme = host_.Theme();
    const AppearanceSettings& appearance = host_.Settings().appearance;
    const LayoutInfo& layout = host_.Layout();

    PaintCard(dc, layout.appearanceCard[0], ui::Icon::Dot, L"Theme",
              L"Palette for the whole window. Changes apply instantly.");
    for (int i = 0; i < 4; ++i) {
        const RECT tile = layout.appearancePresets[i];
        const bool selected = static_cast<int>(appearance.theme) == i;
        const bool hovered = hoverPreset_ == i;
        const int radius = theme.S(8);
        ui::FillRoundRect(dc, tile,
                          selected ? ui::Blend(ui::kPanelHi, ui::kNeonBg, 70)
                                   : (hovered ? ui::kPanelHover : ui::kPanelHi),
                          radius);
        ui::StrokeRoundFrame(dc, tile, selected ? ui::kNeonDim : ui::kBorder, radius, 1);
        ui::DrawLabel(dc, ThemePresetLabel(static_cast<ThemePreset>(i)), tile, theme.bodyStrong,
                      selected ? ui::kText : (hovered ? ui::kText : ui::kMuted),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    PaintCard(dc, layout.appearanceCard[1], ui::Icon::Grid, L"Density",
              L"Comfortable spacing or a tighter layout.");
    for (int i = 0; i < 2; ++i) {
        const DensityMode mode = i == 0 ? DensityMode::Comfortable : DensityMode::Compact;
        PaintPill(dc, layout.appearanceDensity[i], DensityLabel(mode),
                  appearance.density == mode, hoverDensity_ == i);
    }

    PaintCard(dc, layout.appearanceCard[2], ui::Icon::Dot, L"Accent",
              L"Tints highlights, switches and links.");
    for (int i = 0; i < 6; ++i) {
        COLORREF color = i == 0 ? ui::kNeon : kAccentSwatches[i - 1];
        const bool selected =
            i == 0 ? !appearance.customAccent : (appearance.customAccent && appearance.accent == color);
        PaintSwatch(dc, layout.appearanceAccents[i], color, selected, hoverAccent_ == i,
                    kAccentLabels[i]);
    }

    PaintCard(dc, layout.appearanceCard[3], ui::Icon::Monitor, L"Background",
              L"Solid, vertical gradient or a picture behind the frame.");
    PaintPill(dc, layout.appearanceModes[0], L"Solid",
              appearance.background == BackgroundStyle::Solid, hoverMode_ == 0);
    PaintPill(dc, layout.appearanceModes[1], L"Gradient",
              appearance.background == BackgroundStyle::Gradient, hoverMode_ == 1);
    for (int i = 0; i < 5; ++i) {
        const COLORREF color = i == 0 ? ui::kPanel : kBackgroundTops[i];
        const bool selected =
            i == 0 ? !appearance.customBackground : (appearance.customBackground &&
                                                     appearance.backgroundTop == kBackgroundTops[i]);
        PaintSwatch(dc, layout.appearanceSwatches[i], color, selected, hoverSwatch_ == i,
                    kBackgroundLabels[i]);
    }

    // Picture status between the swatches and the buttons.
    RECT imageStatus = {layout.appearanceCard[3].left + theme.S(16),
                        layout.appearanceSwatches[0].bottom + theme.S(6),
                        layout.appearanceCard[3].right - theme.S(16),
                        layout.appearanceImage.top - theme.S(4)};
    if (appearance.backgroundImage.empty()) {
        ui::DrawLabel(dc, L"No picture", imageStatus, theme.tiny, ui::kTextDim,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        const std::wstring::size_type slash =
            appearance.backgroundImage.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos
                                      ? appearance.backgroundImage
                                      : appearance.backgroundImage.substr(slash + 1);
        ui::DrawLabel(dc, name.c_str(), imageStatus, theme.tiny, ui::kMuted,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    PaintCard(dc, layout.appearanceCard[5], ui::Icon::Ghost, L"Icon",
              L"Emoji used for the tray, the window and new shortcuts.");
    {
        const RECT card = layout.appearanceCard[5];
        const RECT preview = layout.appearanceEmojiPreview;
        ui::FillRoundRect(dc, preview, ui::kPanelHi, theme.S(7));
        ui::StrokeRoundFrame(dc, preview, ui::kBorder, theme.S(7), 1);
        const int inner = preview.right - preview.left - theme.S(8);
        const int cx = (preview.left + preview.right) / 2;
        const int cy = (preview.top + preview.bottom) / 2;
        HICON icon = nullptr;
        if (!appearance.iconEmoji.empty()) {
            icon = static_cast<HICON>(::LoadImageW(nullptr, EmojiIconPath().c_str(), IMAGE_ICON,
                                                   inner, inner, LR_LOADFROMFILE));
        }
        if (icon != nullptr) {
            ::DrawIconEx(dc, cx - inner / 2, cy - inner / 2, icon, inner, inner, 0, nullptr,
                         DI_NORMAL);
            ::DestroyIcon(icon);
        } else if (!appearance.iconEmoji.empty()) {
            RECT text = preview;
            ui::DrawLabel(dc, appearance.iconEmoji.c_str(), text, theme.body, ui::kText,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            ui::DrawGhostMark(dc, cx, cy, theme.S(16), ui::kNeon, ui::kPanelHi, false);
        }
        // A short hint next to the buttons, above the edit.
        RECT hint = {card.left + theme.S(14), card.bottom - theme.S(22),
                     card.right - theme.S(14), card.bottom - theme.S(8)};
        ui::DrawLabel(dc,
                      appearance.iconEmoji.empty()
                          ? L"Bundled icon — paste an emoji and press Apply"
                          : L"Applied to the tray, taskbar and shortcuts",
                      hint, theme.tiny, ui::kTextDim,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    PaintCard(dc, layout.appearanceCard[4], ui::Icon::Layers, L"Tabs",
              L"Order the strip and hide tabs you do not use.");
    PaintTabsCard(dc);
}

void SettingsView::PaintCard(HDC dc, const RECT& card, ui::Icon icon, const wchar_t* title,
                             const wchar_t* subtitle) {
    const ui::Theme& theme = host_.Theme();
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

RECT SettingsView::SwitchRowRect(const RECT& card, int index) const {
    const ui::Theme& theme = host_.Theme();
    const int rowHeight = theme.S(44);
    const int top = card.top + theme.S(46) + index * rowHeight;
    return RECT{card.left + theme.S(14), top, card.right - theme.S(14), top + rowHeight};
}

void SettingsView::PaintSwitch(HDC dc, const RECT& rect, const wchar_t* title,
                               const wchar_t* subtitle, bool on, bool hovered) {
    const ui::Theme& theme = host_.Theme();
    if (hovered) {
        ui::FillRoundRect(dc, rect, ui::kPanelHi, theme.S(8));
    }
    RECT titleRect = {rect.left + theme.S(6), rect.top + theme.S(5), rect.right - theme.S(70),
                      rect.top + theme.S(24)};
    ui::DrawLabel(dc, title, titleRect, theme.bodyStrong, ui::kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT subtitleRect = {titleRect.left, rect.top + theme.S(22), titleRect.right,
                         rect.bottom - theme.S(3)};
    ui::DrawLabel(dc, subtitle, subtitleRect, theme.tiny, ui::kTextDim,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const int switchW = theme.S(38);
    const int switchH = theme.S(20);
    RECT sw = {rect.right - theme.S(10) - switchW,
               rect.top + ((rect.bottom - rect.top) - switchH) / 2, 0, 0};
    sw.right = sw.left + switchW;
    sw.bottom = sw.top + switchH;
    ui::DrawSwitch(dc, theme, sw, on);

    const RECT separator = {rect.left + theme.S(6), rect.bottom - 1, rect.right - theme.S(6),
                            rect.bottom};
    ui::FillSolid(dc, separator, ui::kBorder);
}

int SettingsView::NavAt(int x, int y) const {
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int index = 0; index < 5; ++index) {
        if (::PtInRect(&layout.settingsNavItems[index], point)) {
            return index;
        }
    }
    return -1;
}

int SettingsView::AppearancePresetAt(int x, int y) const {
    if (section_ != Section::Appearance) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 4; ++i) {
        if (::PtInRect(&layout.appearancePresets[i], point)) {
            return i;
        }
    }
    return -1;
}

int SettingsView::AppearanceDensityAt(int x, int y) const {
    if (section_ != Section::Appearance) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 2; ++i) {
        if (::PtInRect(&layout.appearanceDensity[i], point)) {
            return i;
        }
    }
    return -1;
}

int SettingsView::AppearanceAccentAt(int x, int y) const {
    if (section_ != Section::Appearance) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 6; ++i) {
        if (::PtInRect(&layout.appearanceAccents[i], point)) {
            return i;
        }
    }
    return -1;
}

int SettingsView::AppearanceModeAt(int x, int y) const {
    if (section_ != Section::Appearance) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 2; ++i) {
        if (::PtInRect(&layout.appearanceModes[i], point)) {
            return i;
        }
    }
    return -1;
}

int SettingsView::AppearanceSwatchAt(int x, int y) const {
    if (section_ != Section::Appearance) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 5; ++i) {
        if (::PtInRect(&layout.appearanceSwatches[i], point)) {
            return i;
        }
    }
    return -1;
}

int SettingsView::SelfDeleteModeAt(int x, int y) const {
    if (section_ != Section::Advanced) {
        return -1;
    }
    POINT point = {x, y};
    const LayoutInfo& layout = host_.Layout();
    for (int i = 0; i < 3; ++i) {
        if (::PtInRect(&layout.settingsSelfModes[i], point)) {
            return i;
        }
    }
    return -1;
}

bool SettingsView::SelfDeleteExitAt(int x, int y) const {
    if (section_ != Section::Advanced) {
        return false;
    }
    POINT point = {x, y};
    return ::PtInRect(&host_.Layout().settingsSelfExit, point) != FALSE;
}

int SettingsView::ActionAt(int x, int y) const {
    const LayoutInfo& layout = host_.Layout();
    POINT point = {x, y};
    if (section_ == Section::General &&
        layout.settingsManage.right > layout.settingsManage.left &&
        ::PtInRect(&layout.settingsManage, point)) {
        return 0;
    }
    if (section_ == Section::Advanced) {
        if (layout.settingsLogCta.right > layout.settingsLogCta.left &&
            ::PtInRect(&layout.settingsLogCta, point)) {
            return 1;
        }
        if (layout.settingsRestoreCta.right > layout.settingsRestoreCta.left &&
            ::PtInRect(&layout.settingsRestoreCta, point)) {
            return 2;
        }
    }
    return -1;
}

int SettingsView::BehaviorSwitchAt(int x, int y) const {
    RECT card = {};
    int rows = 0;
    const LayoutInfo& layout = host_.Layout();
    if (section_ == Section::General) {
        card = layout.settingsCard[1];
        rows = 4;
    } else if (section_ == Section::Advanced) {
        card = layout.settingsCard[0];
        rows = 5;
    } else {
        return -1;
    }
    POINT point = {x, y};
    for (int index = 0; index < rows; ++index) {
        RECT row = SwitchRowRect(card, index);
        if (::PtInRect(&row, point)) {
            return index;
        }
    }
    return -1;
}

bool SettingsView::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    const int nav = NavAt(x, y);
    if (nav >= 0) {
        SetSection(nav == 0 ? Section::General
                            : (nav == 1 ? Section::Appearance
                                        : (nav == 2 ? Section::Hotkeys
                                                    : (nav == 3 ? Section::AutoCapture
                                                                : Section::Advanced))));
        return true;
    }

    // Cards may be scrolled partly outside the viewport; never treat a click in
    // the page header or the footer as a card hit.
    POINT point = {x, y};
    if (!::PtInRect(&host_.Layout().settingsArea, point)) {
        return false;
    }

    const int sw = BehaviorSwitchAt(x, y);
    if (sw >= 0) {
        OnCommand(sw == 0 ? ID_BTN_SET_HIDE_ATTACH
                          : (sw == 1 ? ID_BTN_SET_TRAY
                                     : (sw == 2 ? ID_BTN_SET_SELF
                                                : (sw == 3 ? ID_BTN_SET_AUTOSTART
                                                           : ID_BTN_SET_FOREGROUND))));
        return true;
    }

    const int selfMode = SelfDeleteModeAt(x, y);
    if (selfMode >= 0) {
        Settings& settings = host_.Settings();
        settings.ui.selfDelete = static_cast<SelfDeleteMode>(selfMode);
        if (settings.ui.selfDelete == SelfDeleteMode::Never) {
            settings.ui.deleteOnExit = false;
        }
        host_.MarkSettingsDirty();
        host_.SaveSettingsNow();
        host_.SetStatus(std::wstring(L"Self-delete mode: ") + SelfDeleteModeLabel(settings.ui.selfDelete));
        host_.Invalidate();
        return true;
    }
    if (SelfDeleteExitAt(x, y)) {
        Settings& settings = host_.Settings();
        if (settings.ui.selfDelete == SelfDeleteMode::Never) {
            host_.SetStatus(L"Choose Exe only or Everything before enabling delete on exit", true);
            return true;
        }
        settings.ui.deleteOnExit = !settings.ui.deleteOnExit;
        host_.MarkSettingsDirty();
        host_.SaveSettingsNow();
        host_.SetStatus(settings.ui.deleteOnExit
                            ? L"GhostFrame will delete the selected files when it exits"
                            : L"GhostFrame will not delete anything on exit");
        host_.Invalidate();
        return true;
    }

    const int action = ActionAt(x, y);
    if (action == 0) {
        SetSection(Section::AutoCapture);
        return true;
    }
    if (action == 1) {
        host_.OpenLogFolder();
        return true;
    }
    if (action == 2) {
        host_.RestoreAllTargets();
        return true;
    }

    if (section_ == Section::Appearance) {
        AppearanceSettings& appearance = host_.Settings().appearance;
        int hit = AppearancePresetAt(x, y);
        if (hit >= 0) {
            appearance.theme = static_cast<ThemePreset>(hit);
            ApplyAppearanceChange();
            return true;
        }
        hit = AppearanceDensityAt(x, y);
        if (hit >= 0) {
            appearance.density = hit == 0 ? DensityMode::Comfortable : DensityMode::Compact;
            ApplyAppearanceChange();
            return true;
        }
        hit = AppearanceAccentAt(x, y);
        if (hit >= 0) {
            if (hit == 0) {
                appearance.customAccent = false;
            } else {
                appearance.customAccent = true;
                appearance.accent = kAccentSwatches[hit - 1];
            }
            ApplyAppearanceChange();
            return true;
        }
        hit = AppearanceModeAt(x, y);
        if (hit >= 0) {
            appearance.background =
                hit == 0 ? BackgroundStyle::Solid : BackgroundStyle::Gradient;
            ApplyAppearanceChange();
            return true;
        }
        hit = AppearanceSwatchAt(x, y);
        if (hit >= 0) {
            if (hit == 0) {
                appearance.customBackground = false;
            } else {
                appearance.customBackground = true;
                appearance.backgroundTop = kBackgroundTops[hit];
                appearance.backgroundBottom = kBackgroundBottoms[hit];
            }
            ApplyAppearanceChange();
            return true;
        }
        if (tabs_.Contains(x, y)) {
            const ui::ListWidget::Action listAction = tabs_.OnLButtonDown(x, y, ctrl, shift);
            if (listAction.kind == ui::ListWidget::ActionKind::RowAction) {
                ApplyTabRowAction(listAction.index, listAction.actionId);
            } else {
                host_.Invalidate();
            }
            return true;
        }
        return true;
    }

    if (section_ == Section::AutoCapture && rules_.Contains(x, y)) {
        ApplyListAction(rules_.OnLButtonDown(x, y, ctrl, shift), false);
    } else if ((section_ == Section::General || section_ == Section::Hotkeys) &&
               hotkeys_.Contains(x, y)) {
        ApplyListAction(hotkeys_.OnLButtonDown(x, y, ctrl, shift), true);
    }
    host_.Invalidate();
    return true;
}

bool SettingsView::OnLButtonDblClk(int x, int y) {
    if ((section_ == Section::General || section_ == Section::Hotkeys) &&
        hotkeys_.Contains(x, y)) {
        ApplyListAction(hotkeys_.OnLButtonDblClk(x, y), true);
    } else if (section_ == Section::AutoCapture && rules_.Contains(x, y)) {
        const int row = rules_.IndexAt(x, y);
        const int rule = row >= 0 ? static_cast<int>(rules_.DataAt(row)) : -1;
        if (rule >= 0 && rule < static_cast<int>(host_.Settings().autoAttach.size())) {
            selectedRule_ = rule;
            host_.Settings().autoAttach[static_cast<size_t>(rule)].enabled =
                !host_.Settings().autoAttach[static_cast<size_t>(rule)].enabled;
            host_.MarkSettingsDirty();
            Build();
            rules_.Select(row);
        }
    } else if (section_ == Section::Appearance && tabs_.Contains(x, y)) {
        ApplyTabRowAction(tabs_.IndexAt(x, y), ID_BTN_TAB_VISIBLE);
        return true;
    }
    host_.Invalidate();
    return true;
}

bool SettingsView::OnKeyDown(int virtualKey) {
    Settings& settings = host_.Settings();
    if (!rebinding_ && MaxScroll() > 0 &&
        (virtualKey == VK_PRIOR || virtualKey == VK_NEXT)) {
        const int page = (host_.Layout().settingsArea.bottom - host_.Layout().settingsArea.top) -
                         host_.Theme().S(48);
        ScrollBy(virtualKey == VK_PRIOR ? -page : page);
        return true;
    }
    if (rebinding_) {
        if (virtualKey == VK_ESCAPE) {
            rebinding_ = false;
            rebindAction_ = HotkeyAction::Count;
            Build();
            host_.SetStatus(L"Hotkey rebind cancelled");
            host_.Invalidate();
            return true;
        }
        if (virtualKey == VK_CONTROL || virtualKey == VK_SHIFT || virtualKey == VK_MENU ||
            virtualKey == VK_LWIN || virtualKey == VK_RWIN || virtualKey == VK_LCONTROL ||
            virtualKey == VK_RCONTROL || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
            virtualKey == VK_LMENU || virtualKey == VK_RMENU) {
            return true;
        }

        UINT modifiers = 0;
        if (::GetKeyState(VK_CONTROL) & 0x8000) {
            modifiers |= MOD_CONTROL;
        }
        if (::GetKeyState(VK_MENU) & 0x8000) {
            modifiers |= MOD_ALT;
        }
        if (::GetKeyState(VK_SHIFT) & 0x8000) {
            modifiers |= MOD_SHIFT;
        }
        if ((::GetKeyState(VK_LWIN) & 0x8000) || (::GetKeyState(VK_RWIN) & 0x8000)) {
            modifiers |= MOD_WIN;
        }
        FinishRebind(modifiers, static_cast<UINT>(virtualKey));
        return true;
    }

    if (section_ == Section::Appearance) {
        const ui::ListWidget::Action action = tabs_.OnKeyDown(virtualKey);
        if (action.kind == ui::ListWidget::ActionKind::RowAction) {
            ApplyTabRowAction(action.index, action.actionId);
            return true;
        }
        if (action.kind == ui::ListWidget::ActionKind::Activate ||
            action.kind == ui::ListWidget::ActionKind::Toggle) {
            ApplyTabRowAction(action.index, ID_BTN_TAB_VISIBLE);
            return true;
        }
        if (action.kind != ui::ListWidget::ActionKind::None) {
            host_.Invalidate();
            return true;
        }
        return false;
    }

    if (section_ == Section::AutoCapture) {
        if (virtualKey == VK_DELETE && selectedRule_ >= 0 &&
            selectedRule_ < static_cast<int>(settings.autoAttach.size())) {
            settings.autoAttach.erase(settings.autoAttach.begin() + selectedRule_);
            selectedRule_ = -1;
            host_.MarkSettingsDirty();
            Build();
            host_.SetStatus(L"Auto-capture rule removed");
            host_.Invalidate();
            return true;
        }
        const ui::ListWidget::Action action = rules_.OnKeyDown(virtualKey);
        if (action.kind != ui::ListWidget::ActionKind::None) {
            ApplyListAction(action, false);
            host_.Invalidate();
            return true;
        }
        return false;
    }
    if (section_ == Section::General || section_ == Section::Hotkeys) {
        const ui::ListWidget::Action action = hotkeys_.OnKeyDown(virtualKey);
        if (action.kind == ui::ListWidget::ActionKind::Activate ||
            action.kind == ui::ListWidget::ActionKind::Toggle) {
            const int index = static_cast<int>(hotkeys_.DataAt(action.index));
            if (index >= 0 && index < kHotkeyCount) {
                BeginRebind(static_cast<HotkeyAction>(index));
                return true;
            }
        }
        if (action.kind != ui::ListWidget::ActionKind::None) {
            host_.Invalidate();
            return true;
        }
    }
    return false;
}

void SettingsView::OnMouseMove(int x, int y, bool& changed) {
    const int nav = NavAt(x, y);
    POINT point = {x, y};
    if (nav < 0 && !::PtInRect(&host_.Layout().settingsArea, point)) {
        // Off the page: clear every card hover so a control that scrolled past
        // the viewport cannot look active from the header or footer.
        const bool had = hoverSwitch_ >= 0 || hoverAction_ >= 0 || hoverPreset_ >= 0 ||
                         hoverAccent_ >= 0 || hoverMode_ >= 0 || hoverDensity_ >= 0 ||
                         hoverSwatch_ >= 0 || hoverSelfMode_ >= 0 || hoverSelfExit_;
        hoverNav_ = -1;
        hoverSwitch_ = -1;
        hoverAction_ = -1;
        hoverPreset_ = -1;
        hoverAccent_ = -1;
        hoverMode_ = -1;
        hoverDensity_ = -1;
        hoverSwatch_ = -1;
        hoverSelfMode_ = -1;
        hoverSelfExit_ = false;
        changed |= had;
        return;
    }
    const int sw = BehaviorSwitchAt(x, y);
    const int action = ActionAt(x, y);
    const int preset = AppearancePresetAt(x, y);
    const int accent = AppearanceAccentAt(x, y);
    const int mode = AppearanceModeAt(x, y);
    const int density = AppearanceDensityAt(x, y);
    const int swatch = AppearanceSwatchAt(x, y);
    const int selfMode = SelfDeleteModeAt(x, y);
    const bool selfExit = SelfDeleteExitAt(x, y);
    if (nav != hoverNav_ || sw != hoverSwitch_ || action != hoverAction_ ||
        preset != hoverPreset_ || accent != hoverAccent_ || mode != hoverMode_ ||
        density != hoverDensity_ || swatch != hoverSwatch_ || selfMode != hoverSelfMode_ ||
        selfExit != hoverSelfExit_) {
        hoverNav_ = nav;
        hoverSwitch_ = sw;
        hoverAction_ = action;
        hoverPreset_ = preset;
        hoverAccent_ = accent;
        hoverMode_ = mode;
        hoverDensity_ = density;
        hoverSwatch_ = swatch;
        hoverSelfMode_ = selfMode;
        hoverSelfExit_ = selfExit;
        changed = true;
    }
    changed |= ActiveList()->OnMouseMove(x, y, true);
}

void SettingsView::OnMouseLeave(bool& changed) {
    const bool chrome = hoverNav_ >= 0 || hoverSwitch_ >= 0 || hoverAction_ >= 0 ||
                        hoverPreset_ >= 0 || hoverAccent_ >= 0 || hoverMode_ >= 0 ||
                        hoverDensity_ >= 0 || hoverSwatch_ >= 0 || hoverSelfMode_ >= 0 ||
                        hoverSelfExit_;
    hoverNav_ = -1;
    hoverSwitch_ = -1;
    hoverAction_ = -1;
    hoverPreset_ = -1;
    hoverAccent_ = -1;
    hoverMode_ = -1;
    hoverDensity_ = -1;
    hoverSwatch_ = -1;
    hoverSelfMode_ = -1;
    hoverSelfExit_ = false;
    changed |= chrome;
    changed |= hotkeys_.ClearHover();
    changed |= rules_.ClearHover();
    changed |= tabs_.ClearHover();
}

bool SettingsView::OnMouseWheel(int x, int y, int delta) {
    ui::ListWidget* list = ActiveList();
    const int maxScroll = MaxScroll();
    if (maxScroll > 0 && (list == nullptr || !list->Contains(x, y))) {
        ScrollBy(delta > 0 ? -host_.Theme().S(64) : host_.Theme().S(64));
        return true;
    }
    if (list != nullptr && list->Contains(x, y)) {
        list->OnMouseWheel(delta);
        host_.Invalidate();
        return true;
    }
    return false;
}

bool SettingsView::OnVScroll(int code, int position) {
    ActiveList()->OnVScroll(code, position);
    host_.Invalidate();
    return true;
}

bool SettingsView::OnCommand(int id) {
    Settings& settings = host_.Settings();
    switch (id) {
        case ID_BTN_SET_RESET:
            ResetHotkeys();
            return true;
        case ID_BTN_SET_ADD_FG:
            AddRuleForWindow(::GetForegroundWindow());
            return true;
        case ID_BTN_SET_BROWSE: {
            wchar_t file[MAX_PATH] = {};
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = host_.Window();
            ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = _countof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (::GetOpenFileNameW(&ofn)) {
                AddRuleForExePath(file);
            }
            return true;
        }
        case ID_BTN_SET_ENABLE:
            if (selectedRule_ >= 0 &&
                selectedRule_ < static_cast<int>(settings.autoAttach.size())) {
                settings.autoAttach[static_cast<size_t>(selectedRule_)].enabled =
                    !settings.autoAttach[static_cast<size_t>(selectedRule_)].enabled;
                host_.MarkSettingsDirty();
                Build();
                rules_.Select(selectedRule_);
                host_.Invalidate();
            }
            return true;
        case ID_BTN_SET_HIDE:
            if (selectedRule_ >= 0 &&
                selectedRule_ < static_cast<int>(settings.autoAttach.size())) {
                settings.autoAttach[static_cast<size_t>(selectedRule_)].hide =
                    !settings.autoAttach[static_cast<size_t>(selectedRule_)].hide;
                host_.MarkSettingsDirty();
                Build();
                rules_.Select(selectedRule_);
                host_.Invalidate();
            }
            return true;
        case ID_BTN_SET_REMOVE:
            if (selectedRule_ >= 0 &&
                selectedRule_ < static_cast<int>(settings.autoAttach.size())) {
                settings.autoAttach.erase(settings.autoAttach.begin() + selectedRule_);
                selectedRule_ = -1;
                host_.MarkSettingsDirty();
                Build();
                host_.SetStatus(L"AUTO-CAPTURE RULE REMOVED");
                host_.Invalidate();
            }
            return true;
        case ID_BTN_SET_APPLY:
            ApplyAutoRulesNow();
            return true;
        case ID_BTN_SET_HIDE_ATTACH:
            settings.ui.hideOnAttach = !settings.ui.hideOnAttach;
            host_.MarkSettingsDirty();
            host_.Invalidate();
            return true;
        case ID_BTN_SET_TRAY:
            settings.ui.minimizeToTray = !settings.ui.minimizeToTray;
            host_.MarkSettingsDirty();
            host_.Invalidate();
            return true;
        case ID_BTN_SET_SELF:
            settings.ui.noSelfExclude = !settings.ui.noSelfExclude;
            host_.ApplySelfExclude();
            host_.MarkSettingsDirty();
            host_.Invalidate();
            return true;
        case ID_BTN_SET_FOREGROUND:
            settings.foregroundTracking = !settings.foregroundTracking;
            host_.MarkSettingsDirty();
            host_.SetStatus(settings.foregroundTracking
                                ? L"Following the foreground window for the primary target"
                                : L"Foreground tracking disabled");
            host_.Invalidate();
            return true;
        case ID_BTN_SET_AUTOSTART: {
            const bool enabled = !IsAutostartEnabled();
            if (SetAutostartEnabled(enabled)) {
                host_.SetStatus(enabled ? L"GhostFrame will start with Windows"
                                        : L"GhostFrame will no longer start with Windows");
            } else {
                host_.SetStatus(L"Error: cannot update the startup entry", true);
            }
            host_.Invalidate();
            return true;
        }
        case ID_BTN_APPEAR_RESET:
            ResetAppearance();
            return true;
        case ID_BTN_APPEAR_IMAGE:
            ChooseBackgroundImage();
            return true;
        case ID_BTN_APPEAR_CLEAR:
            settings.appearance.backgroundImage.clear();
            ApplyAppearanceChange();
            host_.SetStatus(L"Background picture removed");
            return true;
        case ID_BTN_ICON_APPLY:
            ApplyEmoji();
            return true;
        case ID_BTN_ICON_CLEAR:
            ClearEmojiIcon();
            return true;
        case ID_BTN_SET_DELETE_NOW:
            host_.RequestSelfDelete();
            return true;
        default:
            return false;
    }
}

bool SettingsView::HandCursorAt(int x, int y) const {
    POINT point = {x, y};
    if (!::PtInRect(&host_.Layout().settingsArea, point) && NavAt(x, y) < 0) {
        return false;
    }
    if (NavAt(x, y) >= 0 || BehaviorSwitchAt(x, y) >= 0 || ActionAt(x, y) >= 0 ||
        AppearancePresetAt(x, y) >= 0 || AppearanceAccentAt(x, y) >= 0 ||
        AppearanceModeAt(x, y) >= 0 || AppearanceDensityAt(x, y) >= 0 ||
        AppearanceSwatchAt(x, y) >= 0 || SelfDeleteModeAt(x, y) >= 0 ||
        SelfDeleteExitAt(x, y)) {
        return true;
    }
    return ActiveList()->Contains(x, y);
}

void SettingsView::ApplyTabRowAction(int row, int actionId) {
    Settings& settings = host_.Settings();
    const int index = row >= 0 ? static_cast<int>(tabs_.DataAt(row)) : -1;
    if (index < 0 || index >= static_cast<int>(settings.appearance.tabs.size())) {
        return;
    }
    std::vector<TabPreference>& tabs = settings.appearance.tabs;
    if (actionId == ID_BTN_TAB_UP && index > 0) {
        std::swap(tabs[static_cast<size_t>(index)], tabs[static_cast<size_t>(index) - 1]);
    } else if (actionId == ID_BTN_TAB_DOWN &&
               index + 1 < static_cast<int>(tabs.size())) {
        std::swap(tabs[static_cast<size_t>(index)], tabs[static_cast<size_t>(index) + 1]);
    } else if (actionId == ID_BTN_TAB_VISIBLE) {
        if (tabs[static_cast<size_t>(index)].id == L"settings") {
            host_.SetStatus(L"The Settings tab must stay visible — it hosts this editor", true);
            return;
        }
        const bool visible = !tabs[static_cast<size_t>(index)].visible;
        tabs[static_cast<size_t>(index)].visible = visible;
        bool anyVisible = false;
        for (const TabPreference& tab : tabs) {
            anyVisible = anyVisible || tab.visible;
        }
        if (!anyVisible) {
            tabs[static_cast<size_t>(index)].visible = true;
            host_.SetStatus(L"At least one tab must stay visible", true);
            return;
        }
        host_.SetStatus(visible ? L"Tab shown" : L"Tab hidden");
    } else {
        return;
    }
    ApplyAppearanceChange();
}

bool SettingsView::ApplyAppearanceChange() {
    // App::ApplyAppearance normalises the tab list, rebuilds the theme, the
    // background and the layout, then asks this view to Build again.
    const bool iconApplied = host_.ApplyAppearance();
    host_.MarkSettingsDirty();
    host_.SaveSettingsNow();
    Build();
    host_.Invalidate();
    return iconApplied;
}

void SettingsView::ResetAppearance() {
    host_.Settings().appearance = AppearanceSettings{};
    ApplyAppearanceChange();
    LoadEmojiField();
    host_.SetStatus(L"Appearance reset to defaults");
}

void SettingsView::ChooseBackgroundImage() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = host_.Window();
    ofn.lpstrFilter =
        L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\\0"
        L"All files (*.*)\\0*.*\\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&ofn)) {
        return;
    }
    host_.Settings().appearance.backgroundImage = file;
    host_.SetStatus(L"Background picture set");
    ApplyAppearanceChange();
}

void SettingsView::ApplyListAction(ui::ListWidget::Action action, bool hotkeyList) {
    Settings& settings = host_.Settings();
    if (action.kind == ui::ListWidget::ActionKind::None || action.index < 0) {
        return;
    }
    if (hotkeyList) {
        const int index = static_cast<int>(hotkeys_.DataAt(action.index));
        if (index >= 0 && index < kHotkeyCount) {
            BeginRebind(static_cast<HotkeyAction>(index));
        }
        return;
    }

    const int rule = static_cast<int>(rules_.DataAt(action.index));
    if (rule < 0 || rule >= static_cast<int>(settings.autoAttach.size())) {
        return;
    }
    selectedRule_ = rule;
    rules_.Select(action.index);

    AutoAttachRule& entry = settings.autoAttach[static_cast<size_t>(rule)];
    if (action.kind == ui::ListWidget::ActionKind::RowAction) {
        if (action.actionId == ID_BTN_SET_ENABLE) {
            entry.enabled = !entry.enabled;
            host_.SetStatus(std::wstring(L"Auto-capture rule ") +
                            (entry.enabled ? L"enabled: " : L"disabled: ") + entry.value);
        } else if (action.actionId == ID_BTN_SET_HIDE) {
            entry.hide = !entry.hide;
            host_.SetStatus(std::wstring(L"Rule ") + entry.value +
                            (entry.hide ? L" will hide new windows"
                                        : L" will keep new windows visible"));
        } else if (action.actionId == ID_BTN_SET_REMOVE) {
            const std::wstring removed = entry.value;
            settings.autoAttach.erase(settings.autoAttach.begin() + rule);
            selectedRule_ = -1;
            host_.MarkSettingsDirty();
            Build();
            host_.SetStatus(L"Auto-capture rule removed: " + removed);
            host_.Invalidate();
            return;
        }
        host_.MarkSettingsDirty();
        Build();
        if (selectedRule_ >= 0) {
            rules_.Select(selectedRule_);
        }
        host_.Invalidate();
        return;
    }
    host_.Invalidate();
}

void SettingsView::ResetHotkeys() {
    Settings defaults = DefaultSettings();
    std::wstring error;
    if (host_.ApplyHotkeys(defaults, error)) {
        Build();
        host_.SetStatus(L"Hotkeys reset to defaults");
    } else {
        host_.SetStatus(L"Error: " + error, true);
    }
}

void SettingsView::BeginRebind(HotkeyAction action) {
    rebinding_ = true;
    rebindAction_ = action;
    host_.SetStatus(std::wstring(L"Press a combination for ") + HotkeyActionLabel(action) +
                    L" — Esc to cancel");
    ::SetFocus(host_.Window());
    Build();
    host_.Invalidate();
}

void SettingsView::FinishRebind(UINT modifiers, UINT virtualKey) {
    const HotkeyAction action = rebindAction_;
    rebinding_ = false;
    rebindAction_ = HotkeyAction::Count;

    if (action == HotkeyAction::Count) {
        return;
    }

    Settings candidate = host_.Settings();
    candidate.hotkeys[static_cast<int>(action)] = HotkeyBinding{modifiers, virtualKey};

    std::wstring error;
    if (host_.ApplyHotkeys(candidate, error)) {
        Build();
        host_.SetStatus(std::wstring(HotkeyActionLabel(action)) + L" = " +
                        FormatHotkey(modifiers, virtualKey));
    } else {
        Build();
        host_.SetStatus(L"Error: " + error, true);
        host_.ShowToast(L"Hotkey rejected — " + error, true);
    }
    host_.Invalidate();
}

void SettingsView::AddRuleForWindow(HWND hwnd) {
    if (hwnd == nullptr || !::IsWindow(hwnd) || hwnd == host_.Window()) {
        host_.SetStatus(L"No foreground window to add", true);
        return;
    }

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) {
        host_.SetStatus(L"GhostFrame cannot auto-capture itself", true);
        return;
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
    if (path.empty()) {
        host_.SetStatus(L"Cannot read the process image name", true);
        return;
    }

    AddRuleForExePath(path);
}

void SettingsView::AddRuleForExePath(const std::wstring& path) {
    Settings& settings = host_.Settings();
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        host_.SetStatus(L"That file has no name", true);
        return;
    }

    for (const AutoAttachRule& rule : settings.autoAttach) {
        if (_wcsicmp(rule.match.c_str(), L"exe") == 0 &&
            _wcsicmp(rule.value.c_str(), name.c_str()) == 0) {
            host_.SetStatus(L"An auto-capture rule for " + name + L" already exists", true);
            return;
        }
    }

    settings.autoAttach.push_back(AutoAttachRule{true, L"exe", name, true});
    host_.MarkSettingsDirty();
    Build();
    host_.SetStatus(L"Auto-capture rule added for " + name);
}

void SettingsView::ApplyAutoRulesNow() {
    if (host_.AttachBusy()) {
        host_.SetStatus(L"ATTACHING… — WAIT FOR THE CURRENT ATTACH TO FINISH", true);
        return;
    }

    // Make sure the daemon has the rules the user is looking at; the normal
    // debounce may not have fired yet.
    host_.SaveSettingsNow();

    int attached = 0;
    std::wstring error;
    if (!host_.Capture().ApplyAutoCaptureNow(attached, error)) {
        host_.SetStatus(L"Error: " + error, true);
        return;
    }

    host_.Capture().Refresh();
    host_.Invalidate();

    if (attached > 0) {
        wchar_t status[96] = {};
        swprintf_s(status, L"Auto-capture attached %d new window%s", attached,
                   attached == 1 ? L"" : L"s");
        host_.SetStatus(status);
    } else {
        host_.SetStatus(L"Auto-capture rules applied — no new windows matched");
    }
}

}  // namespace gf