#include "ui.h"

#include <stdlib.h>

namespace ui {

namespace {

COLORREF Darken(COLORREF color, int divisor) {
    if (divisor < 1) {
        divisor = 1;
    }
    return RGB(GetRValue(color) / divisor, GetGValue(color) / divisor, GetBValue(color) / divisor);
}

}  // namespace

void Theme::Create(UINT dpiValue) {
    Destroy();
    dpi = dpiValue != 0 ? dpiValue : 96;

    auto make_font = [this](int pixels, int weight) {
        return ::CreateFontW(-S(pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    };

    title = make_font(19, FW_BOLD);
    body = make_font(14, FW_NORMAL);
    small = make_font(12, FW_NORMAL);
}

void Theme::Destroy() {
    if (title != nullptr) {
        ::DeleteObject(title);
        title = nullptr;
    }
    if (body != nullptr) {
        ::DeleteObject(body);
        body = nullptr;
    }
    if (small != nullptr) {
        ::DeleteObject(small);
        small = nullptr;
    }
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rect, brush);
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

void DrawButton(const DRAWITEMSTRUCT* item, const Theme& theme, bool active, bool danger) {
    if (item == nullptr) {
        return;
    }

    HDC dc = item->hDC;
    const RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;

    COLORREF background = active ? RGB(10, 48, 33) : kPanel;
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
        text = kMuted;
    }

    FillSolid(dc, rect, background);
    StrokeFrame(dc, rect, border, 1);

    RECT accent = {rect.left + 1, rect.top + 1, rect.left + theme.S(3), rect.bottom - 1};
    FillSolid(dc, accent, active ? kNeon : (danger ? kWarn : kBorder));

    wchar_t label[192] = {};
    ::GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)) - 1);
    DrawLabel(dc, label, rect, theme.body, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawBadge(HDC dc, const Theme& theme, RECT rect, const wchar_t* text, COLORREF color) {
    FillSolid(dc, rect, Darken(color, 7));
    StrokeFrame(dc, rect, color, 1);
    DrawLabel(dc, text, rect, theme.small, color,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

}  // namespace ui
