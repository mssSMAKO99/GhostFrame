#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace ui {

constexpr COLORREF kBg = RGB(9, 11, 16);
constexpr COLORREF kPanel = RGB(19, 23, 32);
constexpr COLORREF kPanelHi = RGB(28, 34, 47);
constexpr COLORREF kBorder = RGB(38, 45, 60);
constexpr COLORREF kNeon = RGB(0, 255, 136);
constexpr COLORREF kNeonDim = RGB(0, 148, 80);
constexpr COLORREF kText = RGB(224, 232, 240);
constexpr COLORREF kMuted = RGB(138, 150, 166);
constexpr COLORREF kWarn = RGB(255, 78, 96);

struct Theme {
    UINT dpi = 96;
    HFONT title = nullptr;
    HFONT body = nullptr;
    HFONT small = nullptr;

    int S(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    void Create(UINT dpiValue);
    void Destroy();
};

void FillSolid(HDC dc, const RECT& rect, COLORREF color);
void StrokeFrame(HDC dc, const RECT& rect, COLORREF color, int width = 1);
void DrawLabel(HDC dc, const wchar_t* text, RECT rect, HFONT font, COLORREF color, UINT format);
void DrawButton(const DRAWITEMSTRUCT* item, const Theme& theme, bool active, bool danger);
void DrawBadge(HDC dc, const Theme& theme, RECT rect, const wchar_t* text, COLORREF color);

struct ListBadge {
    std::wstring text;
    COLORREF color = kMuted;
};

// One row of a ListWidget.  `data` is an opaque caller value (a session index
// for TARGETS, an HWND for WINDOWS) that survives row rebuilding.
struct ListRow {
    std::wstring title;
    std::wstring meta;
    std::vector<ListBadge> badges;
    HICON icon = nullptr;
    bool dim = false;
    bool selectable = true;
    intptr_t data = 0;
};

// Scrollable, hoverable and keyboard-navigable row list used by both the
// TARGETS and the WINDOWS tab.  There is no artificial row limit: the visible
// height comes from the client area.
class ListWidget {
public:
    enum class ActionKind { None, Select, Activate, Toggle };
    struct Action {
        ActionKind kind = ActionKind::None;
        int index = -1;
    };

    void SetBounds(const RECT& bounds, const Theme& theme);
    void SetRows(std::vector<ListRow> rows);
    void SetEmptyText(std::wstring text) { emptyText_ = std::move(text); }

    void Paint(HDC dc, const Theme& theme) const;

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
    int SelectableCount() const {
        int count = 0;
        for (const ListRow& row : rows_) {
            if (row.selectable) {
                ++count;
            }
        }
        return count;
    }
    intptr_t DataAt(int index) const;

private:
    int VisibleRows() const;
    int MaxScroll() const;
    bool NeedsScrollbar() const;
    RECT RowRect(int index) const;
    RECT ScrollbarThumb() const;
    int RowAt(int x, int y) const;
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
    int scroll_ = 0;
    int selected_ = -1;
    int hover_ = -1;
    int anchor_ = -1;
    std::vector<int> multi_;
};

}  // namespace ui
