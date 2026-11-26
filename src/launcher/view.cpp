// Shared helpers for the view modules.

#include "view.h"

namespace gf {

void PaintCtaButton(HDC dc, const ui::Theme& theme, const RECT& rect, const wchar_t* label,
                    ui::Icon icon, ui::Icon trailing, bool primary, bool hovered) {
    if (rect.right <= rect.left) {
        return;
    }
    const int radius = theme.S(10);
    COLORREF background = primary ? ui::kNeon : ui::kPanelHi;
    COLORREF border = primary ? ui::kNeon : ui::kBorderHi;
    COLORREF text = primary ? ui::kOnAccent : ui::kText;
    if (hovered) {
        background = primary ? ui::Blend(ui::kNeon, RGB(255, 255, 255), 14) : ui::kPanelHover;
        border = primary ? ui::kNeon : ui::kNeonDim;
    }
    ui::FillRoundRect(dc, rect, background, radius);
    ui::StrokeRoundFrame(dc, rect, border, radius, 1);

    const int iconSize = theme.S(16);
    int left = rect.left + theme.S(16);
    if (icon != ui::Icon::None) {
        RECT iconRect = {left, rect.top + ((rect.bottom - rect.top) - iconSize) / 2,
                         left + iconSize, 0};
        iconRect.bottom = iconRect.top + iconSize;
        ui::DrawIcon(dc, icon, iconRect, text, theme.S(2));
        left = iconRect.right + theme.S(10);
    }
    RECT labelRect = {left, rect.top, rect.right - theme.S(14), rect.bottom};
    if (trailing != ui::Icon::None) {
        labelRect.right -= theme.S(20);
    }
    ui::DrawLabel(dc, label, labelRect, theme.bodyStrong, text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (trailing != ui::Icon::None) {
        RECT arrow = {rect.right - theme.S(16) - theme.S(12),
                      rect.top + ((rect.bottom - rect.top) - theme.S(12)) / 2,
                      rect.right - theme.S(16), 0};
        arrow.bottom = arrow.top + theme.S(12);
        ui::DrawIcon(dc, trailing, arrow, text, theme.S(2));
    }
}

}  // namespace gf