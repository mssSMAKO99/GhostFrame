#include "ui.h"

#include <stdlib.h>

namespace ui {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

COLORREF kBg = RGB(9, 11, 16);
COLORREF kPanel = RGB(19, 23, 32);
COLORREF kPanelHi = RGB(28, 34, 47);
COLORREF kPanelHover = RGB(35, 42, 58);
COLORREF kBorder = RGB(38, 45, 60);
COLORREF kNeon = RGB(0, 255, 136);
COLORREF kNeonBg = RGB(10, 48, 33);
COLORREF kNeonDim = RGB(0, 148, 80);
COLORREF kText = RGB(224, 232, 240);
COLORREF kMuted = RGB(138, 150, 166);
COLORREF kWarn = RGB(255, 78, 96);
COLORREF kWarnBg = RGB(52, 16, 24);
COLORREF kFocus = RGB(120, 180, 255);
COLORREF kDisabled = RGB(70, 78, 90);
COLORREF kToastBg = RGB(30, 36, 50);

namespace {

void ApplyNeonPalette() {
    kBg = RGB(9, 11, 16);
    kPanel = RGB(19, 23, 32);
    kPanelHi = RGB(28, 34, 47);
    kPanelHover = RGB(35, 42, 58);
    kBorder = RGB(38, 45, 60);
    kNeon = RGB(0, 255, 136);
    kNeonBg = RGB(10, 48, 33);
    kNeonDim = RGB(0, 148, 80);
    kText = RGB(224, 232, 240);
    kMuted = RGB(138, 150, 166);
    kWarn = RGB(255, 78, 96);
    kWarnBg = RGB(52, 16, 24);
    kFocus = RGB(120, 180, 255);
    kDisabled = RGB(70, 78, 90);
    kToastBg = RGB(30, 36, 50);
}

// High contrast: fall back to the system palette so custom drawing stays
// readable.  We deliberately keep the layout identical; only the colours move.
void ApplySystemPalette() {
    kBg = ::GetSysColor(COLOR_WINDOW);
    kPanel = ::GetSysColor(COLOR_WINDOW);
    kPanelHi = ::GetSysColor(COLOR_BTNFACE);
    kPanelHover = ::GetSysColor(COLOR_BTNFACE);
    kBorder = ::GetSysColor(COLOR_WINDOWFRAME);
    kNeon = ::GetSysColor(COLOR_HIGHLIGHT);
    kNeonBg = ::GetSysColor(COLOR_WINDOW);
    kNeonDim = ::GetSysColor(COLOR_HIGHLIGHT);
    kText = ::GetSysColor(COLOR_WINDOWTEXT);
    kMuted = ::GetSysColor(COLOR_GRAYTEXT);
    kWarn = ::GetSysColor(COLOR_HIGHLIGHT);
    kWarnBg = ::GetSysColor(COLOR_WINDOW);
    kFocus = ::GetSysColor(COLOR_HIGHLIGHT);
    kDisabled = ::GetSysColor(COLOR_GRAYTEXT);
    kToastBg = ::GetSysColor(COLOR_INFOBK);
}

COLORREF Darken(COLORREF color, int divisor) {
    if (divisor < 1) {
        divisor = 1;
    }
    return RGB(GetRValue(color) / divisor, GetGValue(color) / divisor, GetBValue(color) / divisor);
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

// Segoe UI Variable on Windows 11, plain Segoe UI on Windows 10.
const wchar_t* UiFace() {
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

HFONT MakeFont(const wchar_t* face, int pixels, int weight) {
    return ::CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, face);
}

void DrawActionButton(HDC dc, const Theme& theme, const RECT& rect, const ListAction& action,
                      bool hovered) {
    const int radius = theme.S(4);
    COLORREF background = action.active ? kNeonBg : (hovered ? kPanelHover : kPanel);
    FillRoundRect(dc, rect, background, radius);

    COLORREF border = action.danger ? kWarn : (action.active ? kNeon : kBorder);
    if (action.color != kMuted && !action.danger && !action.active) {
        border = action.color;
    }
    StrokeRoundFrame(dc, rect, border, radius, 1);

    const COLORREF text = action.danger ? kWarn
                                        : (action.active ? kNeon
                                                         : (action.color != kMuted ? action.color
                                                                                   : kText));
    DrawLabel(dc, action.label.c_str(), rect, theme.small, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
        ApplyNeonPalette();
    }

    const wchar_t* face = UiFace();
    title = MakeFont(face, S(18), 600);
    body = MakeFont(face, S(14), FW_NORMAL);
    small = MakeFont(face, S(12), FW_NORMAL);
    mono = MakeFont(L"Consolas", S(12), FW_NORMAL);
}

void Theme::Destroy() {
    auto destroy = [](HFONT& font) {
        if (font != nullptr) {
            ::DeleteObject(font);
            font = nullptr;
        }
    };
    destroy(title);
    destroy(body);
    destroy(small);
    destroy(mono);
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

void FillRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius) {
    if (radius < 1) {
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
                bool hovered) {
    if (item == nullptr) {
        return;
    }

    HDC dc = item->hDC;
    const RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    COLORREF background = active ? kNeonBg : kPanel;
    if (hovered && !pressed) {
        background = kPanelHover;
    }
    if (pressed) {
        background = kPanelHi;
    }
    if (disabled) {
        background = kBg;
    }

    COLORREF border = danger ? kWarn : (active ? kNeon : kBorder);
    COLORREF text = danger ? kWarn : (active ? kNeon : kText);
    if (disabled) {
        border = kBorder;
        text = kDisabled;
    }
    if (focused && !disabled) {
        border = kFocus;
    }

    const int radius = theme.S(6);
    FillRoundRect(dc, rect, background, radius);
    StrokeRoundFrame(dc, rect, border, radius, 1);

    RECT accent = {rect.left + 2, rect.top + theme.S(6), rect.left + theme.S(5),
                   rect.bottom - theme.S(6)};
    FillRoundRect(dc, accent, active ? kNeon : (danger ? kWarn : kBorder), theme.S(2));

    wchar_t label[192] = {};
    ::GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)) - 1);
    DrawLabel(dc, label, rect, theme.body, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawBadge(HDC dc, const Theme& theme, RECT rect, const wchar_t* text, COLORREF color) {
    FillRoundRect(dc, rect, Darken(color, 7), theme.S(4));
    StrokeRoundFrame(dc, rect, color, theme.S(4), 1);
    DrawLabel(dc, text, rect, theme.small, color,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// ListWidget
// ---------------------------------------------------------------------------

void ListWidget::SetBounds(const RECT& bounds, const Theme& theme) {
    bounds_ = bounds;
    rowHeight_ = theme.S(theme.m.rowHeight);
    gap_ = theme.S(4);
    scrollbarWidth_ = theme.S(10);
    minThumbHeight_ = theme.S(24);
    pad_ = theme.S(10);
    actionWidth_ = theme.S(66);
    actionGap_ = theme.S(6);
    if (rowHeight_ < 1) {
        rowHeight_ = 1;
    }
    ClampScroll();
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
    const int height = bounds_.bottom - bounds_.top;
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
    rect.top = bounds_.top + (index - scroll_) * rowHeight_;
    rect.bottom = rect.top + rowHeight_ - gap_;
    return rect;
}

RECT ListWidget::ActionRect(int index, int actionIndex) const {
    const RECT row = RowRect(index);
    const int count = static_cast<int>(rows_[index].actions.size());
    const int totalWidth = count * actionWidth_ + (count - 1) * actionGap_;
    const int start = row.right - pad_ - totalWidth;
    const int height = actionWidth_ > 0 ? (row.bottom - row.top) - pad_ : 0;
    const int top = row.top + ((row.bottom - row.top) - height) / 2;

    RECT rect = {};
    rect.left = start + actionIndex * (actionWidth_ + actionGap_);
    rect.right = rect.left + actionWidth_;
    rect.top = top;
    rect.bottom = top + height;
    return rect;
}

RECT ListWidget::ScrollbarThumb() const {
    RECT track = {bounds_.right - scrollbarWidth_, bounds_.top, bounds_.right, bounds_.bottom};
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
    if (rowHeight_ < 1 || !Contains(x, y)) {
        return -1;
    }
    if (NeedsScrollbar() && x >= bounds_.right - scrollbarWidth_) {
        return -1;
    }
    const int visible = (y - bounds_.top) / rowHeight_;
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
    const std::vector<ListAction>& actions = rows_[index].actions;
    for (size_t i = 0; i < actions.size(); ++i) {
        const RECT rect = ActionRect(index, static_cast<int>(i));
        POINT point = {x, y};
        if (::PtInRect(&rect, point)) {
            return actions[i].id;
        }
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

void ListWidget::Paint(HDC dc, const Theme& theme, bool focused) const {
    if (Count() == 0) {
        if (emptyText_.empty()) {
            return;
        }
        RECT rect = bounds_;
        rect.bottom = rect.top + rowHeight_ - gap_;
        FillRoundRect(dc, rect, kPanel, theme.S(6));
        StrokeRoundFrame(dc, rect, kBorder, theme.S(6), 1);
        DrawLabel(dc, emptyText_.c_str(), rect, theme.small, kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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

        FillRoundRect(dc, row, selected || hovered ? kPanelHi : kPanel, theme.S(6));
        StrokeRoundFrame(dc, row, cursor ? kNeon : (selected ? kNeonDim : kBorder), theme.S(6), 1);

        RECT accent = {row.left + 2, row.top + theme.S(6), row.left + theme.S(5),
                       row.bottom - theme.S(6)};
        FillRoundRect(dc, accent,
                      cursor ? kNeon : (selected ? kNeonDim : (item.dim ? kBorder : kNeonDim)),
                      theme.S(2));

        int left = row.left + pad_;
        if (item.icon != nullptr) {
            const int size = theme.S(16);
            const int top = row.top + (row.bottom - row.top - size) / 2;
            ::DrawIconEx(dc, left, top, item.icon, size, size, 0, nullptr, DI_NORMAL);
            left += size + theme.S(8);
        }

        int right = row.right - pad_;

        // Per-row action buttons live at the far right.
        const int actionCount = static_cast<int>(item.actions.size());
        for (int i = 0; i < actionCount; ++i) {
            const RECT rect = ActionRect(index, i);
            const bool actionHover = hoverActionRow_ == index && hoverAction_ == item.actions[i].id;
            DrawActionButton(dc, theme, rect, item.actions[i], actionHover);
        }
        if (actionCount > 0) {
            const RECT firstAction = ActionRect(index, 0);
            right = firstAction.left - theme.S(8);
        }

        const int badgeHeight = theme.S(18);
        const int badgeTop = row.top + (row.bottom - row.top - badgeHeight) / 2;
        for (auto it = item.badges.rbegin(); it != item.badges.rend(); ++it) {
            const SIZE size = MeasureText(dc, theme.small, it->text);
            const int width = size.cx + theme.S(12);
            RECT badge = {right - width, badgeTop, right, badgeTop + badgeHeight};
            DrawBadge(dc, theme, badge, it->text.c_str(), it->color);
            right -= width + theme.S(6);
        }

        int textRight = right;
        if (!item.meta.empty()) {
            HFONT metaFont = item.metaMono ? theme.mono : theme.small;
            const SIZE size = MeasureText(dc, metaFont, item.meta);
            const int metaLeft = textRight - size.cx;
            RECT meta = {metaLeft, row.top, textRight, row.bottom};
            DrawLabel(dc, item.meta.c_str(), meta, metaFont, item.dim ? kBorder : kMuted,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            textRight = metaLeft - theme.S(10);
        }

        if (textRight <= left + theme.S(8)) {
            continue;
        }
        RECT title = {left, row.top, textRight, row.bottom};
        DrawLabel(dc, item.title.c_str(), title, theme.body, item.dim ? kMuted : kText,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (NeedsScrollbar()) {
        RECT track = {bounds_.right - scrollbarWidth_, bounds_.top, bounds_.right, bounds_.bottom};
        FillRoundRect(dc, track, kPanel, scrollbarWidth_ / 2);
        const RECT thumb = ScrollbarThumb();
        FillRoundRect(dc, thumb, kBorder, scrollbarWidth_ / 2);
        StrokeRoundFrame(dc, thumb, kMuted, scrollbarWidth_ / 2, 1);
    }

    if (focused) {
        RECT ring = bounds_;
        ::InflateRect(&ring, 1, 1);
        StrokeRoundFrame(dc, ring, kFocus, theme.S(6), 1);
    }
}

}  // namespace ui
