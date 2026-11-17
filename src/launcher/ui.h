#pragma once

#include <windows.h>

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

}  // namespace ui
