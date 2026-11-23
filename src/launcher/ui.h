#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ui {

// Active palette.  These are variables rather than constexpr so that
// Theme::Create can swap them when the DPI, the system theme or the high
// contrast setting changes.
extern COLORREF kBg;
extern COLORREF kPanel;
extern COLORREF kPanelHi;
extern COLORREF kPanelHover;
extern COLORREF kBorder;
extern COLORREF kNeon;
extern COLORREF kNeonBg;
extern COLORREF kNeonDim;
extern COLORREF kText;
extern COLORREF kMuted;
extern COLORREF kWarn;
extern COLORREF kWarnBg;
extern COLORREF kFocus;
extern COLORREF kDisabled;
extern COLORREF kToastBg;

// Layout metrics, expressed in 96-dpi units; Theme::S scales them.  Keeping
// them in one place is what lets Layout() and PaintInto() agree without a pile
// of hard-coded Y coordinates.
struct Metrics {
    int pad = 16;
    int gap = 8;
    int titleHeight = 40;
    int tabTop = 48;
    int tabWidth = 120;
    int tabStep = 128;
    int tabHeight = 28;
    int statusHeight = 22;
    int headerHeight = 20;
    int footerHeight = 18;
    int buttonHeight = 30;
    int rowHeight = 38;
    int cornerRadius = 8;
};

struct Theme {
    UINT dpi = 96;
    bool highContrast = false;
    Metrics m;

    HFONT title = nullptr;
    HFONT body = nullptr;
    HFONT small = nullptr;
    HFONT mono = nullptr;  // Consolas, reserved for PID / hex / keys

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    void Create(UINT dpiValue);
    void Destroy();
};

// SPI_GETHIGHCONTRAST, so the custom drawing can fall back to a readable,
// system-coloured palette instead of the neon theme.
bool SystemHighContrast();

void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void FillRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius);
void StrokeFrame(HDC dc, const RECT& rect, COLORREF color, int width = 1);
void StrokeRoundFrame(HDC dc, const RECT& rect, COLORREF color, int radius, int width = 1);
void DrawLabel(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT format);
SIZE MeasureTextSize(HDC dc, HFONT font, const std::wstring& text);
void DrawButton(const DRAWITEMSTRUCT* item, const Theme& theme, bool active, bool danger,
                bool hovered);
void DrawBadge(HDC dc, const Theme& theme, RECT rect, const wchar_t* text, COLORREF color);

struct ListBadge {
    std::wstring text;
    COLORREF color = kMuted;
};

// One per-row action button, drawn at the right edge of the row.  `id` is an
// opaque caller value returned in ListWidget::Action::actionId.
struct ListAction {
    int id = 0;
    std::wstring label;
    COLORREF color = kMuted;
    bool active = false;
    bool danger = false;
};

// One row of a ListWidget.  `data` is an opaque caller value (a session index
// for TARGETS, an HWND for WINDOWS) that survives row rebuilding.
struct ListRow {
    std::wstring title;
    std::wstring meta;
    bool metaMono = false;  // draw the meta text with the monospace font
    std::vector<ListBadge> badges;
    std::vector<ListAction> actions;
    HICON icon = nullptr;
    bool dim = false;
    bool selectable = true;
    intptr_t data = 0;
};

// Scrollable, hoverable and keyboard-navigable row list used by the TARGETS,
// WINDOWS and SETTINGS tabs.  There is no artificial row limit: the visible
// height comes from the client area.
class ListWidget {
public:
    enum class ActionKind { None, Select, Activate, Toggle, RowAction };
    struct Action {
        ActionKind kind = ActionKind::None;
        int index = -1;
        int actionId = 0;
    };

    void SetBounds(const RECT& bounds, const Theme& theme);
    void SetRows(std::vector<ListRow> rows);
    void SetEmptyText(std::wstring text) { emptyText_ = std::move(text); }

    // `focused` draws a focus ring around the list.
    void Paint(HDC dc, const Theme& theme, bool focused) const;

    bool Contains(int x, int y) const;
    bool OnMouseMove(int x, int y, bool inside);
    bool ClearHover();
    Action OnLButtonDown(int x, int y, bool ctrl, bool shift);
    Action OnLButtonDblClk(int x, int y);
    Action OnMouseWheel(int delta);
    Action OnVScroll(int code, int position = 0);
    Action OnKeyDown(int virtualKey);

    // Row under a client point (dim/tracked rows included), or -1.
    int IndexAt(int x, int y) const { return RowAt(x, y); }

    // The cursor row (keyboard focus); -1 when nothing is selected.
    int Selected() const { return selected_; }

    // Selection model.  `selected_` is the cursor/anchor; `multi_` may hold more
    // than one row (Ctrl/Shift).  Both survive SetRows by matching row data.
    void Select(int index);            // replace the selection with a single row
    void ToggleSelection(int index);   // Ctrl-click / Ctrl-Space
    void ExtendSelection(int index);   // Shift-click / Shift-arrow
    void SelectAll();
    void ClearSelection();
    bool IsSelected(int index) const;
    int SelectedCount() const { return static_cast<int>(multi_.size()); }
    std::vector<int> SelectedIndices() const { return multi_; }

    int Count() const { return static_cast<int>(rows_.size()); }
    int SelectableCount() const;
    intptr_t DataAt(int index) const;

private:
    int VisibleRows() const;
    int MaxScroll() const;
    bool NeedsScrollbar() const;
    RECT RowRect(int index) const;
    RECT ScrollbarThumb() const;
    RECT ActionRect(int index, int actionIndex) const;
    int RowAt(int x, int y) const;
    int ActionAt(int index, int x, int y) const;  // action id, or 0
    void ClampScroll();
    void ScrollToSelection();
    int NextSelectable(int from, int delta) const;
    void SetSingle(int index);
    void AddToSelection(int index);

    std::vector<ListRow> rows_;
    std::wstring emptyText_;
    RECT bounds_ = {};
    int rowHeight_ = 0;
    int gap_ = 0;
    int scrollbarWidth_ = 0;
    int minThumbHeight_ = 0;
    int pad_ = 8;
    int actionWidth_ = 66;
    int actionGap_ = 6;
    int scroll_ = 0;
    int selected_ = -1;
    int hover_ = -1;
    int hoverAction_ = 0;
    int hoverActionRow_ = -1;
    int anchor_ = -1;
    std::vector<int> multi_;
};

}  // namespace ui
