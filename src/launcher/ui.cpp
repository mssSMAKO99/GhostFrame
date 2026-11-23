#include "ui.h"

#include <stdlib.h>

namespace ui {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

COLORREF kBg = RGB(7, 12, 10);
COLORREF kSidebar = RGB(10, 17, 14);
COLORREF kTopBar = RGB(9, 15, 12);
COLORREF kPanel = RGB(14, 22, 18);
COLORREF kPanelHi = RGB(20, 31, 26);
COLORREF kPanelHover = RGB(25, 39, 32);
COLORREF kBorder = RGB(30, 44, 37);
COLORREF kBorderHi = RGB(43, 62, 52);
COLORREF kNeon = RGB(47, 226, 123);
COLORREF kNeonBg = RGB(14, 42, 29);
COLORREF kNeonDim = RGB(21, 154, 85);
COLORREF kNeonSoft = RGB(23, 57, 40);
COLORREF kText = RGB(232, 243, 236);
COLORREF kMuted = RGB(139, 156, 147);
COLORREF kTextDim = RGB(95, 111, 104);
COLORREF kWarn = RGB(255, 93, 104);
COLORREF kWarnBg = RGB(42, 17, 20);
COLORREF kFocus = RGB(127, 224, 174);
COLORREF kDisabled = RGB(74, 88, 81);
COLORREF kToastBg = RGB(22, 34, 28);
COLORREF kOnAccent = RGB(4, 19, 11);

namespace {

void ApplyDarkPalette() {
    kBg = RGB(7, 12, 10);
    kSidebar = RGB(10, 17, 14);
    kTopBar = RGB(9, 15, 12);
    kPanel = RGB(14, 22, 18);
    kPanelHi = RGB(20, 31, 26);
    kPanelHover = RGB(25, 39, 32);
    kBorder = RGB(30, 44, 37);
    kBorderHi = RGB(43, 62, 52);
    kNeon = RGB(47, 226, 123);
    kNeonBg = RGB(14, 42, 29);
    kNeonDim = RGB(21, 154, 85);
    kNeonSoft = RGB(23, 57, 40);
    kText = RGB(232, 243, 236);
    kMuted = RGB(139, 156, 147);
    kTextDim = RGB(95, 111, 104);
    kWarn = RGB(255, 93, 104);
    kWarnBg = RGB(42, 17, 20);
    kFocus = RGB(127, 224, 174);
    kDisabled = RGB(74, 88, 81);
    kToastBg = RGB(22, 34, 28);
    kOnAccent = RGB(4, 19, 11);
}

// High contrast: fall back to the system palette so custom drawing stays
// readable.  We deliberately keep the layout identical; only the colours move.
void ApplySystemPalette() {
    kBg = ::GetSysColor(COLOR_WINDOW);
    kSidebar = ::GetSysColor(COLOR_BTNFACE);
    kTopBar = ::GetSysColor(COLOR_BTNFACE);
    kPanel = ::GetSysColor(COLOR_WINDOW);
    kPanelHi = ::GetSysColor(COLOR_BTNFACE);
    kPanelHover = ::GetSysColor(COLOR_BTNFACE);
    kBorder = ::GetSysColor(COLOR_WINDOWFRAME);
    kBorderHi = ::GetSysColor(COLOR_WINDOWFRAME);
    kNeon = ::GetSysColor(COLOR_HIGHLIGHT);
    kNeonBg = ::GetSysColor(COLOR_WINDOW);
    kNeonDim = ::GetSysColor(COLOR_HIGHLIGHT);
    kNeonSoft = ::GetSysColor(COLOR_WINDOW);
    kText = ::GetSysColor(COLOR_WINDOWTEXT);
    kMuted = ::GetSysColor(COLOR_GRAYTEXT);
    kTextDim = ::GetSysColor(COLOR_GRAYTEXT);
    kWarn = ::GetSysColor(COLOR_HIGHLIGHT);
    kWarnBg = ::GetSysColor(COLOR_WINDOW);
    kFocus = ::GetSysColor(COLOR_HIGHLIGHT);
    kDisabled = ::GetSysColor(COLOR_GRAYTEXT);
    kToastBg = ::GetSysColor(COLOR_INFOBK);
    kOnAccent = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
}

SIZE MeasureText(HDC dc, HFONT font, const std::wstring& text) {
    SIZE size = {};
    if (font == nullptr) {
        return size;
    }
    HGDIOBJ oldFont = ::SelectObject(dc, font);
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    ::SelectObject(dc, oldFont);
    return size;
}

int CALLBACK CountFontProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) {
    *reinterpret_cast<bool*>(param) = true;
    return 0;
}

bool FontExists(const wchar_t* face) {
    HDC dc = ::GetDC(nullptr);
    if (dc == nullptr) {
        return false;
    }
    LOGFONTW filter = {};
    filter.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(filter.lfFaceName, face);
    bool found = false;
    ::EnumFontFamiliesExW(dc, &filter, &CountFontProc, reinterpret_cast<LPARAM>(&found), 0);
    ::ReleaseDC(nullptr, dc);
    return found;
}

// Segoe UI Variable on Windows 11, plain Segoe UI on Windows 10.  Display is
// slightly tighter and used for the large page titles and the wordmark.
const wchar_t* TextFace() {
    static const wchar_t* face = nullptr;
    if (face == nullptr) {
        if (FontExists(L"Segoe UI Variable Text")) {
            face = L"Segoe UI Variable Text";
        } else if (FontExists(L"Segoe UI")) {
            face = L"Segoe UI";
        } else {
            face = L"Arial";
        }
    }
    return face;
}

const wchar_t* DisplayFace() {
    static const wchar_t* face = nullptr;
    if (face == nullptr) {
        if (FontExists(L"Segoe UI Variable Display")) {
            face = L"Segoe UI Variable Display";
        } else {
            face = TextFace();
        }
    }
    return face;
}

HFONT MakeFont(const wchar_t* face, int pixels, int weight) {
    return ::CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, face);
}

void PaintBadges(HDC dc, const Theme& theme, const RECT& area, const std::vector<ListBadge>& badges,
                 bool alignRight) {
    const int height = theme.S(19);
    const int top = area.top + ((area.bottom - area.top) - height) / 2;
    int right = area.right;
    RECT work = area;
    for (auto it = badges.rbegin(); it != badges.rend(); ++it) {
        const SIZE size = MeasureText(dc, theme.tiny, it->text);
        const int width = size.cx + theme.S(16);
        if (!alignRight) {
            const int left = work.left;
            if (left + width > work.right) {
                break;
            }
            RECT badge = {left, top, left + width, top + height};
            DrawBadge(dc, theme, badge, it->text.c_str(), it->color);
            work.left = badge.right + theme.S(6);
            continue;
        }
        RECT badge = {right - width, top, right, top + height};
        if (badge.left < work.left) {
            break;
        }
        DrawBadge(dc, theme, badge, it->text.c_str(), it->color);
        right = badge.left - theme.S(6);
    }
}

void DrawActionButton(HDC dc, const Theme& theme, const RECT& rect, const ListAction& action,
                      bool hovered) {
    const int radius = theme.S(7);
    COLORREF background = action.active ? kNeonBg : (hovered ? kPanelHover : kPanel);
    FillRoundRect(dc, rect, background, radius);

    COLORREF border = action.danger ? Blend(kWarn, kPanel, 45)
                                    : (action.active ? kNeonDim : kBorder);
    if (action.color != kMuted && !action.danger && !action.active) {
        border = Blend(action.color, kPanel, 55);
    }
    StrokeRoundFrame(dc, rect, border, radius, 1);

    const COLORREF text = action.danger ? kWarn
                                        : (action.active ? kNeon
                                                         : (action.color != kMuted ? action.color
                                                                                   : kText));
    if (action.iconOnly && action.icon != Icon::None) {
        const int size = theme.S(15);
        RECT iconRect = {rect.left + (rect.right - rect.left - size) / 2,
                         rect.top + (rect.bottom - rect.top - size) / 2, 0, 0};
        iconRect.right = iconRect.left + size;
        iconRect.bottom = iconRect.top + size;
        DrawIcon(dc, action.icon, iconRect, text, theme.S(2));
        return;
    }
    DrawLabel(dc, action.label.c_str(), rect, theme.small, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawCheckbox(HDC dc, const Theme& theme, const RECT& rect, bool checked) {
    const int radius = theme.S(4);
    FillRoundRect(dc, rect, checked ? kNeon : kPanel, radius);
    StrokeRoundFrame(dc, rect, checked ? kNeon : kBorderHi, radius, 1);
    if (checked) {
        RECT check = rect;
        ::InflateRect(&check, -theme.S(3), -theme.S(3));
        DrawIcon(dc, Icon::Check, check, kOnAccent, theme.S(2));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

bool SystemHighContrast() {
    HIGHCONTRASTW hc = {};
    hc.cbSize = sizeof(hc);
    if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
    return false;
}

void Theme::Create(UINT dpiValue) {
    Destroy();
    dpi = dpiValue != 0 ? dpiValue : 96;
    highContrast = SystemHighContrast();
    if (highContrast) {
        ApplySystemPalette();
    } else {
        ApplyDarkPalette();
    }

    const wchar_t* text = TextFace();
    const wchar_t* displayFace = DisplayFace();
    brand = MakeFont(displayFace, S(20), 700);
    title = MakeFont(text, S(16), 600);
    display = MakeFont(displayFace, S(26), 600);
    body = MakeFont(text, S(14), FW_NORMAL);
    bodyStrong = MakeFont(text, S(14), 600);
    small = MakeFont(text, S(12), FW_NORMAL);
    tiny = MakeFont(text, S(10), FW_NORMAL);
    mono = MakeFont(L"Consolas", S(12), FW_NORMAL);
}

void Theme::Destroy() {
    auto destroy = [](HFONT& font) {
        if (font != nullptr) {
            ::DeleteObject(font);
            font = nullptr;
        }
    };
    destroy(brand);
    destroy(display);
    destroy(title);
    destroy(body);
    destroy(bodyStrong);
    destroy(small);
    destroy(tiny);
    destroy(mono);
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

COLORREF Blend(COLORREF from, COLORREF to, int percent) {
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const auto mix = [percent](int a, int b) {
        return a + (b - a) * percent / 100;
    };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

void FillVerticalGradient(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom) {
    const int height = rect.bottom - rect.top;
    if (height <= 0) {
        return;
    }
    const int bands = height < 48 ? height : 48;
    for (int i = 0; i < bands; ++i) {
        const int y0 = rect.top + height * i / bands;
        const int y1 = rect.top + height * (i + 1) / bands;
        const COLORREF color = Blend(top, bottom, bands > 1 ? i * 100 / (bands - 1) : 0);
        RECT band = {rect.left, y0, rect.right, y1};
        FillSolid(dc, band, color);
    }
}

void FillRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius) {
    if (radius < 1 || rect.right <= rect.left || rect.bottom <= rect.top) {
        FillSolid(dc, rect, color);
        return;
    }
    HBRUSH brush = ::CreateSolidBrush(color);
    HPEN pen = ::CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);
}

void FillRoundGradient(HDC dc, const RECT& rect, COLORREF top, COLORREF bottom, int radius) {
    if (radius < 1) {
        FillVerticalGradient(dc, rect, top, bottom);
        return;
    }
    const int saved = ::SaveDC(dc);
    ::BeginPath(dc);
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    ::EndPath(dc);
    ::SelectClipPath(dc, RGN_AND);
    FillVerticalGradient(dc, rect, top, bottom);
    ::RestoreDC(dc, saved);
}

void FillCircle(HDC dc, int centerX, int centerY, int radius, COLORREF color) {
    RECT rect = {centerX - radius, centerY - radius, centerX + radius, centerY + radius};
    HBRUSH brush = ::CreateSolidBrush(color);
    HPEN pen = ::CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = ::SelectObject(dc, brush);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);
}

void StrokeFrame(HDC dc, const RECT& rect, COLORREF color, int width) {
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void StrokeRoundFrame(HDC dc, const RECT& rect, COLORREF color, int radius, int width) {
    if (radius < 1) {
        StrokeFrame(dc, rect, color, width);
        return;
    }
    HPEN pen = ::CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    ::SelectObject(dc, oldBrush);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);
}

void DrawLabel(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT format) {
    if (text == nullptr || font == nullptr) {
        return;
    }
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, color);
    HGDIOBJ oldFont = ::SelectObject(dc, font);
    ::DrawTextW(dc, text, -1, &rect, format);
    ::SelectObject(dc, oldFont);
}

SIZE MeasureTextSize(HDC dc, HFONT font, const std::wstring& text) {
    return MeasureText(dc, font, text);
}

void DrawButton(const DRAWITEMSTRUCT* item, const Theme& theme, bool active, bool danger,
                bool hovered, bool primary, COLORREF surface) {
    if (item == nullptr) {
        return;
    }

    HDC dc = item->hDC;
    const RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;

    COLORREF background = primary ? kNeon : kPanelHi;
    COLORREF border = primary ? kNeon : kBorder;
    COLORREF text = primary ? kOnAccent : kText;

    if (active && !primary) {
        background = kNeonBg;
        border = kNeonDim;
        text = kNeon;
    }
    if (hovered && !pressed && !disabled) {
        background = primary ? Blend(kNeon, RGB(255, 255, 255), 14) : kPanelHover;
        if (!primary) {
            border = active ? kNeonDim : kBorderHi;
        }
    }
    if (pressed && !disabled) {
        background = primary ? Blend(kNeon, kBg, 12) : Blend(kPanelHi, kBg, 35);
    }
    if (danger) {
        background = kPanel;
        border = Blend(kWarn, kPanel, 35);
        text = kWarn;
    }
    if (disabled) {
        background = primary ? Blend(kNeon, kBg, 60) : kBg;
        border = kBorder;
        text = kDisabled;
    }
    const int radius = theme.S(theme.m.controlRadius);
    FillSolid(dc, rect, surface);
    FillRoundRect(dc, rect, background, radius);
    if (!primary) {
        StrokeRoundFrame(dc, rect, border, radius, 1);
    }

    wchar_t label[192] = {};
    ::GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)) - 1);
    DrawLabel(dc, label, rect, theme.bodyStrong, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawBadge(HDC dc, const Theme& theme, RECT rect, const wchar_t* text, COLORREF color) {
    const COLORREF background = Blend(kPanel, color, 16);
    FillRoundRect(dc, rect, background, theme.S(5));
    StrokeRoundFrame(dc, rect, Blend(color, kPanel, 55), theme.S(5), 1);
    DrawLabel(dc, text, rect, theme.tiny, color,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void DrawSwitch(HDC dc, const Theme& theme, const RECT& rect, bool on) {
    const int radius = (rect.bottom - rect.top) / 2;
    FillRoundRect(dc, rect, on ? kNeon : kBorderHi, radius);
    const int knob = radius - theme.S(2);
    const int cx = on ? rect.right - radius : rect.left + radius;
    ::SetBkMode(dc, TRANSPARENT);
    FillCircle(dc, cx, (rect.top + rect.bottom) / 2, knob, on ? kText : kPanelHi);
}

void DrawKeycap(HDC dc, const Theme& theme, const RECT& rect, const wchar_t* text) {
    FillRoundRect(dc, rect, kPanelHi, theme.S(6));
    StrokeRoundFrame(dc, rect, kBorderHi, theme.S(6), 1);
    DrawLabel(dc, text, rect, theme.mono, kText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// ListWidget
// ---------------------------------------------------------------------------

void ListWidget::SetBounds(const RECT& bounds, const Theme& theme) {
    bounds_ = bounds;
    rowHeight_ = theme.S(rowHeightOverride_ > 0 ? rowHeightOverride_ : theme.m.rowHeight);
    gap_ = theme.S(5);
    headerHeight_ = columns_.empty() ? 0 : theme.S(32);
    scrollbarWidth_ = theme.S(8);
    minThumbHeight_ = theme.S(24);
    pad_ = theme.S(12);
    actionWidth_ = theme.S(58);
    actionGap_ = theme.S(6);
    iconActionWidth_ = theme.S(32);
    actionInset_ = theme.S(8);
    checkboxSize_ = theme.S(17);
    if (rowHeight_ < 1) {
        rowHeight_ = 1;
    }
    ClampScroll();
}

void ListWidget::SetColumns(std::vector<ListColumn> columns) {
    columns_ = std::move(columns);
}

void ListWidget::SetRows(std::vector<ListRow> rows) {
    const auto dataAt = [this](int index) -> intptr_t {
        return index >= 0 && index < Count() ? rows_[index].data : 0;
    };
    const intptr_t selectedData = dataAt(selected_);
    const intptr_t hoverData = dataAt(hover_);
    const intptr_t anchorData = dataAt(anchor_);
    std::vector<intptr_t> selectedDataValues;
    selectedDataValues.reserve(multi_.size());
    for (int index : multi_) {
        if (index >= 0 && index < Count()) {
            selectedDataValues.push_back(rows_[index].data);
        }
    }

    rows_ = std::move(rows);
    selected_ = -1;
    hover_ = -1;
    hoverAction_ = 0;
    hoverActionRow_ = -1;
    anchor_ = -1;
    multi_.clear();

    for (int i = 0; i < Count(); ++i) {
        const intptr_t data = rows_[i].data;
        if (selectedData != 0 && data == selectedData && rows_[i].selectable) {
            selected_ = i;
        }
        if (hoverData != 0 && data == hoverData) {
            hover_ = i;
        }
        if (anchorData != 0 && data == anchorData && rows_[i].selectable) {
            anchor_ = i;
        }
        if (rows_[i].selectable) {
            for (intptr_t value : selectedDataValues) {
                if (data == value) {
                    multi_.push_back(i);
                    break;
                }
            }
        }
    }

    // If the multi-selection was lost but the cursor survived, keep the cursor
    // as a single selection so keyboard actions still have a target.
    if (multi_.empty() && selected_ >= 0) {
        multi_.push_back(selected_);
    }
    if (anchor_ < 0 && selected_ >= 0) {
        anchor_ = selected_;
    }

    ClampScroll();
    ScrollToSelection();
}

int ListWidget::VisibleRows() const {
    const int height = bounds_.bottom - RowsTop();
    if (rowHeight_ < 1 || height <= 0) {
        return 0;
    }
    return height / rowHeight_;
}

int ListWidget::MaxScroll() const {
    const int maxScroll = Count() - VisibleRows();
    return maxScroll > 0 ? maxScroll : 0;
}

bool ListWidget::NeedsScrollbar() const {
    return MaxScroll() > 0;
}

RECT ListWidget::RowRect(int index) const {
    RECT rect = {};
    rect.left = bounds_.left;
    rect.right = bounds_.right - (NeedsScrollbar() ? scrollbarWidth_ + gap_ : 0);
    rect.top = RowsTop() + (index - scroll_) * rowHeight_;
    rect.bottom = rect.top + rowHeight_ - gap_;
    return rect;
}

int ListWidget::ActionAreaWidth(const Theme& theme, const ListRow& row) const {
    (void)theme;
    int width = 0;
    for (const ListAction& action : row.actions) {
        width += action.iconOnly ? iconActionWidth_ : actionWidth_;
    }
    if (!row.actions.empty()) {
        width += static_cast<int>(row.actions.size() - 1) * actionGap_;
    }
    return width;
}

RECT ListWidget::ActionRect(int index, int actionIndex) const {
    if (index < 0 || index >= Count() || actionIndex < 0 ||
        actionIndex >= static_cast<int>(rows_[index].actions.size())) {
        return RECT{};
    }
    const RECT row = RowRect(index);
    const std::vector<ListAction>& actions = rows_[index].actions;
    int total = 0;
    for (size_t i = 0; i < actions.size(); ++i) {
        total += actions[i].iconOnly ? iconActionWidth_ : actionWidth_;
        if (i + 1 < actions.size()) {
            total += actionGap_;
        }
    }
    int x = row.right - pad_ - total;
    for (int i = 0; i < actionIndex; ++i) {
        x += (actions[static_cast<size_t>(i)].iconOnly ? iconActionWidth_ : actionWidth_) +
             actionGap_;
    }
    const int width =
        actions[static_cast<size_t>(actionIndex)].iconOnly ? iconActionWidth_ : actionWidth_;
    RECT rect = {x, row.top + actionInset_, x + width, row.bottom - actionInset_};
    return rect;
}

RECT ListWidget::HeaderCheckboxRect() const {
    RECT rect = {};
    rect.left = bounds_.left + pad_;
    rect.top = bounds_.top + (headerHeight_ - checkboxSize_) / 2;
    rect.right = rect.left + checkboxSize_;
    rect.bottom = rect.top + checkboxSize_;
    return rect;
}

RECT ListWidget::RowCheckboxRect(int index) const {
    const RECT row = RowRect(index);
    RECT rect = {};
    rect.left = row.left + pad_;
    rect.top = row.top + ((row.bottom - row.top) - checkboxSize_) / 2;
    rect.right = rect.left + checkboxSize_;
    rect.bottom = rect.top + checkboxSize_;
    return rect;
}

RECT ListWidget::ScrollbarThumb() const {
    RECT track = {bounds_.right - scrollbarWidth_, RowsTop(), bounds_.right, bounds_.bottom};
    const int trackHeight = track.bottom - track.top;
    const int total = Count();
    int thumbHeight = total > 0 ? trackHeight * VisibleRows() / total : trackHeight;
    if (thumbHeight < minThumbHeight_) {
        thumbHeight = minThumbHeight_;
    }
    if (thumbHeight > trackHeight) {
        thumbHeight = trackHeight;
    }
    const int maxScroll = MaxScroll();
    const int offset = maxScroll > 0 ? (trackHeight - thumbHeight) * scroll_ / maxScroll : 0;

    RECT thumb = {};
    thumb.left = track.left + 1;
    thumb.right = track.right - 1;
    thumb.top = track.top + offset;
    thumb.bottom = thumb.top + thumbHeight;
    return thumb;
}

void ListWidget::ClampScroll() {
    if (scroll_ > MaxScroll()) {
        scroll_ = MaxScroll();
    }
    if (scroll_ < 0) {
        scroll_ = 0;
    }
}

void ListWidget::ScrollToSelection() {
    if (selected_ < 0) {
        return;
    }
    const int visible = VisibleRows();
    if (selected_ < scroll_) {
        scroll_ = selected_;
    } else if (visible > 0 && selected_ >= scroll_ + visible) {
        scroll_ = selected_ - visible + 1;
    }
    ClampScroll();
}

int ListWidget::RowAt(int x, int y) const {
    if (rowHeight_ < 1 || !Contains(x, y) || y < RowsTop()) {
        return -1;
    }
    if (NeedsScrollbar() && x >= bounds_.right - scrollbarWidth_) {
        return -1;
    }
    const int visible = (y - RowsTop()) / rowHeight_;
    if (visible < 0 || visible >= VisibleRows()) {
        return -1;
    }
    const int index = scroll_ + visible;
    if (index < 0 || index >= Count()) {
        return -1;
    }
    const RECT row = RowRect(index);
    if (y > row.bottom) {
        return -1;  // the gap between two rows
    }
    return index;
}

int ListWidget::ActionAt(int index, int x, int y) const {
    if (index < 0 || index >= Count()) {
        return 0;
    }
    const RECT row = RowRect(index);
    const int count = static_cast<int>(rows_[index].actions.size());
    int right = row.right - pad_;
    for (int i = count - 1; i >= 0; --i) {
        const ListAction& action = rows_[index].actions[static_cast<size_t>(i)];
        const int width = action.iconOnly ? iconActionWidth_ : actionWidth_;
        RECT rect = {right - width, row.top + actionInset_, right, row.bottom - actionInset_};
        POINT point = {x, y};
        if (::PtInRect(&rect, point)) {
            return action.id;
        }
        right = rect.left - actionGap_;
    }
    return 0;
}

bool ListWidget::Contains(int x, int y) const {
    POINT point = {x, y};
    return ::PtInRect(&bounds_, point) != FALSE;
}

bool ListWidget::OnMouseMove(int x, int y, bool inside) {
    const int row = inside ? RowAt(x, y) : -1;
    const int next = row >= 0 ? row : -1;
    const int nextAction = row >= 0 ? ActionAt(row, x, y) : 0;
    if (next == hover_ && nextAction == hoverAction_ && next == hoverActionRow_) {
        return false;
    }
    hover_ = next;
    hoverAction_ = nextAction;
    hoverActionRow_ = row;
    return true;
}

bool ListWidget::ClearHover() {
    if (hover_ < 0 && hoverAction_ == 0) {
        return false;
    }
    hover_ = -1;
    hoverAction_ = 0;
    hoverActionRow_ = -1;
    return true;
}

void ListWidget::SetSingle(int index) {
    selected_ = index;
    anchor_ = index;
    multi_.clear();
    if (index >= 0 && index < Count()) {
        multi_.push_back(index);
    }
    ScrollToSelection();
}

void ListWidget::Select(int index) {
    if (index < 0 || index >= Count()) {
        ClearSelection();
        return;
    }
    SetSingle(index);
}

void ListWidget::AddToSelection(int index) {
    for (int selected : multi_) {
        if (selected == index) {
            return;
        }
    }
    multi_.push_back(index);
}

void ListWidget::ToggleSelection(int index) {
    if (index < 0 || index >= Count() || !rows_[index].selectable) {
        return;
    }
    bool removed = false;
    for (size_t i = 0; i < multi_.size(); ++i) {
        if (multi_[i] == index) {
            multi_.erase(multi_.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
            break;
        }
    }
    if (!removed) {
        AddToSelection(index);
    }
    selected_ = index;
    anchor_ = index;
    ScrollToSelection();
}

void ListWidget::ExtendSelection(int index) {
    if (index < 0 || index >= Count() || !rows_[index].selectable) {
        return;
    }
    if (anchor_ < 0 || anchor_ >= Count()) {
        SetSingle(index);
        return;
    }

    const int first = anchor_ < index ? anchor_ : index;
    const int last = anchor_ < index ? index : anchor_;
    multi_.clear();
    for (int i = first; i <= last; ++i) {
        if (rows_[i].selectable) {
            multi_.push_back(i);
        }
    }
    selected_ = index;
    ScrollToSelection();
}

void ListWidget::SelectAll() {
    multi_.clear();
    for (int i = 0; i < Count(); ++i) {
        if (rows_[i].selectable) {
            multi_.push_back(i);
        }
    }
    if (!multi_.empty()) {
        selected_ = multi_.back();
        anchor_ = multi_.front();
    } else {
        selected_ = -1;
        anchor_ = -1;
    }
    ScrollToSelection();
}

void ListWidget::ClearSelection() {
    selected_ = -1;
    anchor_ = -1;
    multi_.clear();
}

bool ListWidget::IsSelected(int index) const {
    for (int selected : multi_) {
        if (selected == index) {
            return true;
        }
    }
    return false;
}

int ListWidget::SelectableCount() const {
    int count = 0;
    for (const ListRow& row : rows_) {
        if (row.selectable) {
            ++count;
        }
    }
    return count;
}

intptr_t ListWidget::DataAt(int index) const {
    if (index < 0 || index >= Count()) {
        return 0;
    }
    return rows_[index].data;
}

ListWidget::Action ListWidget::OnLButtonDown(int x, int y, bool ctrl, bool shift) {
    if (!Contains(x, y)) {
        return {};
    }

    if (headerHeight_ > 0 && y < RowsTop()) {
        POINT point = {x, y};
        RECT headerBox = HeaderCheckboxRect();
        if (checkboxes_ && ::PtInRect(&headerBox, point)) {
            if (SelectedCount() > 0 && SelectedCount() >= SelectableCount()) {
                ClearSelection();
            } else {
                SelectAll();
            }
        }
        return {};
    }

    if (NeedsScrollbar() && x >= bounds_.right - scrollbarWidth_) {
        const RECT thumb = ScrollbarThumb();
        if (y < thumb.top) {
            scroll_ -= VisibleRows();
        } else if (y > thumb.bottom) {
            scroll_ += VisibleRows();
        }
        ClampScroll();
        return {};
    }

    const int row = RowAt(x, y);
    if (row < 0 || !rows_[row].selectable) {
        return {};
    }

    const int action = ActionAt(row, x, y);
    if (action != 0) {
        SetSingle(row);
        return {ActionKind::RowAction, row, action};
    }

    POINT point = {x, y};
    RECT rowBox = RowCheckboxRect(row);
    if (checkboxes_ && ::PtInRect(&rowBox, point)) {
        ToggleSelection(row);
        return {ActionKind::Select, row};
    }

    if (shift) {
        ExtendSelection(row);
    } else if (ctrl) {
        ToggleSelection(row);
    } else {
        SetSingle(row);
    }
    return {ActionKind::Select, row};
}

ListWidget::Action ListWidget::OnLButtonDblClk(int x, int y) {
    const int row = RowAt(x, y);
    if (row < 0 || !rows_[row].selectable) {
        return {};
    }
    if (ActionAt(row, x, y) != 0) {
        return {};  // a double-click on a button is not an activation
    }
    Select(row);
    return {ActionKind::Activate, row};
}

ListWidget::Action ListWidget::OnMouseWheel(int delta) {
    const int steps = delta / WHEEL_DELTA;
    if (steps == 0) {
        return {};
    }
    scroll_ -= steps * 3;
    ClampScroll();
    return {};
}

ListWidget::Action ListWidget::OnVScroll(int code, int position) {
    const int visible = VisibleRows();
    switch (code) {
        case SB_LINEUP:
            scroll_ -= 1;
            break;
        case SB_LINEDOWN:
            scroll_ += 1;
            break;
        case SB_PAGEUP:
            scroll_ -= visible;
            break;
        case SB_PAGEDOWN:
            scroll_ += visible;
            break;
        case SB_TOP:
            scroll_ = 0;
            break;
        case SB_BOTTOM:
            scroll_ = MaxScroll();
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:
            scroll_ = position;
            break;
        default:
            break;
    }
    ClampScroll();
    return {};
}

int ListWidget::NextSelectable(int from, int delta) const {
    if (delta == 0 || Count() == 0) {
        return -1;
    }
    int index = from;
    for (;;) {
        index += delta;
        if (index < 0 || index >= Count()) {
            return -1;
        }
        if (rows_[index].selectable) {
            return index;
        }
    }
}

ListWidget::Action ListWidget::OnKeyDown(int virtualKey) {
    if (Count() == 0) {
        return {};
    }

    const bool extend = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    const auto moveTo = [this, extend](int next) -> Action {
        if (next < 0) {
            return {};
        }
        if (extend) {
            ExtendSelection(next);
        } else {
            SetSingle(next);
        }
        return {ActionKind::Select, next};
    };

    switch (virtualKey) {
        case VK_UP:
            return moveTo(NextSelectable(selected_ < 0 ? Count() : selected_, -1));
        case VK_DOWN:
            return moveTo(NextSelectable(selected_ < 0 ? -1 : selected_, 1));
        case VK_HOME:
            return moveTo(NextSelectable(-1, 1));
        case VK_END:
            return moveTo(NextSelectable(Count(), -1));
        case VK_PRIOR:
        case VK_NEXT: {
            const int delta = virtualKey == VK_PRIOR ? -1 : 1;
            int next = selected_ < 0 ? (delta > 0 ? -1 : Count()) : selected_;
            const int visible = VisibleRows();
            for (int step = 0; step < (visible > 1 ? visible : 1); ++step) {
                const int moved = NextSelectable(next, delta);
                if (moved < 0) {
                    break;
                }
                next = moved;
            }
            if (next >= 0 && next != selected_) {
                return moveTo(next);
            }
            return {};
        }
        case 'A':
            if (control) {
                SelectAll();
                return {ActionKind::Select, selected_};
            }
            return {};
        case VK_SPACE:
            if (selected_ >= 0 && selected_ < Count()) {
                return {ActionKind::Toggle, selected_};
            }
            return {};
        case VK_RETURN:
            if (selected_ >= 0 && selected_ < Count()) {
                return {ActionKind::Activate, selected_};
            }
            return {};
        default:
            return {};
    }
}

void ListWidget::ColumnRects(int index, const Theme& theme, std::vector<RECT>& rects) const {
    rects.clear();
    const RECT row = RowRect(index);
    int left = row.left + pad_;
    if (checkboxes_) {
        left += checkboxSize_ + theme.S(10);
    }
    int right = row.right - pad_;
    const int actionsWidth = ActionAreaWidth(theme, rows_[index]);
    if (actionsWidth > 0) {
        right -= actionsWidth + theme.S(10);
    }
    if (right <= left) {
        return;
    }

    int fixed = 0;
    int flexible = 0;
    for (const ListColumn& column : columns_) {
        if (column.width < 0) {
            ++flexible;
        } else {
            fixed += theme.S(column.width);
        }
    }
    const int available = right - left;
    int flexWidth = 0;
    if (flexible > 0) {
        flexWidth = (available - fixed) / flexible;
        if (flexWidth < theme.S(40)) {
            flexWidth = theme.S(40);
        }
    }

    int x = left;
    for (const ListColumn& column : columns_) {
        const int width = column.width < 0 ? flexWidth : theme.S(column.width);
        RECT cell = {x, row.top, x + width, row.bottom};
        rects.push_back(cell);
        x += width;
    }
}

void ListWidget::Paint(HDC dc, const Theme& theme, bool focused) const {
    (void)focused;
    const int saved = ::SaveDC(dc);
    ::IntersectClipRect(dc, bounds_.left, bounds_.top, bounds_.right, bounds_.bottom);

    // Column header strip.
    if (headerHeight_ > 0) {
        RECT header = {bounds_.left, bounds_.top, bounds_.right, RowsTop()};
        if (checkboxes_) {
            const int all = SelectableCount();
            DrawCheckbox(dc, theme, HeaderCheckboxRect(), all > 0 && SelectedCount() >= all);
        }
        if (!columns_.empty() && !rows_.empty()) {
            std::vector<RECT> rects;
            ColumnRects(0, theme, rects);
            for (size_t i = 0; i < columns_.size() && i < rects.size(); ++i) {
                RECT cell = rects[i];
                cell.top = bounds_.top;
                cell.bottom = RowsTop();
                cell.left += theme.S(8);
                cell.right -= theme.S(8);
                DrawLabel(dc, columns_[i].header.c_str(), cell, theme.small, kMuted,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        RECT line = {bounds_.left, header.bottom - 1, bounds_.right, header.bottom};
        FillSolid(dc, line, kBorder);
    }

    if (Count() == 0) {
        if (!emptyText_.empty()) {
            RECT rect = {bounds_.left, RowsTop(), bounds_.right, bounds_.bottom};
            DrawLabel(dc, emptyText_.c_str(), rect, theme.small, kMuted,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        ::RestoreDC(dc, saved);
        return;
    }

    const int first = scroll_;
    const int last = Count() < scroll_ + VisibleRows() ? Count() : scroll_ + VisibleRows();

    for (int index = first; index < last; ++index) {
        const ListRow& item = rows_[index];
        const RECT row = RowRect(index);
        const bool selected = IsSelected(index);
        const bool cursor = index == selected_;
        const bool hovered = index == hover_ && item.selectable;

        const COLORREF rowBg = selected ? Blend(kPanelHi, kNeonBg, 55)
                                       : (hovered ? kPanelHover : kPanelHi);
        FillRoundRect(dc, row, rowBg, theme.S(9));
        StrokeRoundFrame(dc, row, cursor ? kNeonDim : (selected ? kNeonDim : kBorder),
                         theme.S(9), 1);

        int left = row.left + pad_;
        if (checkboxes_) {
            DrawCheckbox(dc, theme, RowCheckboxRect(index), selected);
            left += checkboxSize_ + theme.S(10);
        }

        if (!columns_.empty()) {
            std::vector<RECT> rects;
            ColumnRects(index, theme, rects);

            // First column: icon + title.
            if (!rects.empty()) {
                RECT cell = rects[0];
                int textLeft = cell.left;
                if (item.icon != nullptr) {
                    const int size = theme.S(18);
                    const int top = row.top + (row.bottom - row.top - size) / 2;
                    ::DrawIconEx(dc, textLeft, top, item.icon, size, size, 0, nullptr, DI_NORMAL);
                    textLeft += size + theme.S(10);
                }
                RECT title = {textLeft, row.top, cell.right - theme.S(6), row.bottom};
                DrawLabel(dc, item.title.c_str(), title, theme.bodyStrong,
                          item.dim ? kMuted : kText,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            for (size_t i = 1; i < rects.size() && i - 1 < item.cells.size(); ++i) {
                RECT cell = rects[i];
                cell.left += theme.S(8);
                cell.right -= theme.S(8);
                DrawLabel(dc, item.cells[i - 1].c_str(), cell,
                          columns_[i].mono ? theme.mono : theme.small,
                          item.dim ? kTextDim : kMuted,
                          columns_[i].align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                              DT_NOPREFIX);
            }
            const size_t badgeColumn = 1 + item.cells.size();
            if (!item.badges.empty() && badgeColumn < rects.size()) {
                RECT badgeArea = rects[badgeColumn];
                badgeArea.left += theme.S(6);
                badgeArea.right -= theme.S(6);
                PaintBadges(dc, theme, badgeArea, item.badges, false);
            }
        } else {
            int right = row.right - pad_;

            const int actionCount = static_cast<int>(item.actions.size());
            for (int i = 0; i < actionCount; ++i) {
                const ListAction& action = item.actions[static_cast<size_t>(i)];
                const RECT rect = ActionRect(index, i);
                const bool actionHover =
                    hoverActionRow_ == index && hoverAction_ == action.id;
                DrawActionButton(dc, theme, rect, action, actionHover);
            }
            if (actionCount > 0) {
                const RECT firstAction = ActionRect(index, 0);
                right = firstAction.left - theme.S(8);
            }

            const int badgeHeight = theme.S(19);
            const int badgeTop = row.top + ((row.bottom - row.top) - badgeHeight) / 2;
            for (auto it = item.badges.rbegin(); it != item.badges.rend(); ++it) {
                const SIZE size = MeasureText(dc, theme.tiny, it->text);
                const int width = size.cx + theme.S(16);
                RECT badge = {right - width, badgeTop, right, badgeTop + badgeHeight};
                if (badge.left < left + theme.S(8)) {
                    break;
                }
                DrawBadge(dc, theme, badge, it->text.c_str(), it->color);
                right = badge.left - theme.S(6);
            }

            int textRight = right;
            if (item.metaChip && !item.meta.empty()) {
                const SIZE size = MeasureText(dc, theme.mono, item.meta);
                const int chipWidth = size.cx + theme.S(20);
                const int chipHeight = theme.S(24);
                RECT chip = {textRight - chipWidth,
                             row.top + (row.bottom - row.top - chipHeight) / 2, textRight,
                             row.top + (row.bottom - row.top - chipHeight) / 2 + chipHeight};
                DrawKeycap(dc, theme, chip, item.meta.c_str());
                textRight = chip.left - theme.S(12);
            }

            if (textRight <= left + theme.S(8)) {
                continue;
            }
            if (!item.meta.empty() && !item.metaChip) {
                const int lineHeight = theme.S(14);
                const int center = row.top + (row.bottom - row.top) / 2;
                RECT title = {left, center - lineHeight - theme.S(2), textRight, center};
                RECT meta = {left, center + theme.S(1), textRight,
                             center + lineHeight + theme.S(2)};
                DrawLabel(dc, item.title.c_str(), title, theme.bodyStrong,
                          item.dim ? kMuted : kText,
                          DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                DrawLabel(dc, item.meta.c_str(), meta, theme.small,
                          item.dim ? kTextDim : kMuted,
                          DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                continue;
            }
            RECT title = {left, row.top, textRight, row.bottom};
            DrawLabel(dc, item.title.c_str(), title, theme.bodyStrong,
                      item.dim ? kMuted : kText,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    if (NeedsScrollbar()) {
        RECT track = {bounds_.right - scrollbarWidth_, RowsTop(), bounds_.right, bounds_.bottom};
        FillRoundRect(dc, track, Blend(kPanel, kBg, 40), scrollbarWidth_ / 2);
        const RECT thumb = ScrollbarThumb();
        FillRoundRect(dc, thumb, kBorderHi, scrollbarWidth_ / 2);
    }

    ::RestoreDC(dc, saved);
}

}  // namespace ui