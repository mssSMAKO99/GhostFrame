#include "icons.h"

#include <algorithm>
#include <cmath>

#include <objidl.h>

// gdiplus.h historically spells the bounds helpers unqualified; the project
// builds with NOMINMAX, so bring std::min/std::max into scope for this
// translation unit only.
using std::max;
using std::min;
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace ui {

namespace {

using namespace Gdiplus;

ULONG_PTR g_token = 0;
int g_refs = 0;

Color ToColor(COLORREF color, BYTE alpha = 255) {
    return Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

void RoundRectPath(GraphicsPath& path, const RectF& rect, REAL radius) {
    REAL diameter = radius * 2.0f;
    if (diameter > rect.Width) {
        diameter = rect.Width;
    }
    if (diameter > rect.Height) {
        diameter = rect.Height;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter, diameter,
                0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

void Polyline(Graphics& graphics, Pen& pen, std::initializer_list<PointF> points, bool close) {
    if (points.size() < 2) {
        return;
    }
    auto it = points.begin();
    GraphicsPath path;
    path.AddLine(*it, *(it + 1));
    ++it;
    for (++it; it != points.end(); ++it) {
        path.AddLine(*(it - 1), *it);
    }
    if (close) {
        path.CloseFigure();
    }
    graphics.DrawPath(&pen, &path);
}

}  // namespace

void InitIcons() {
    if (g_refs++ != 0) {
        return;
    }
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_token, &input, nullptr) != Ok) {
        g_token = 0;
    }
}

void ShutdownIcons() {
    if (g_refs == 0) {
        return;
    }
    if (--g_refs == 0 && g_token != 0) {
        GdiplusShutdown(g_token);
        g_token = 0;
    }
}

HBITMAP LoadPicture(const wchar_t* path) {
    if (g_token == 0 || path == nullptr || path[0] == L'\0') {
        return nullptr;
    }

    Bitmap image(path);
    if (image.GetLastStatus() != Ok) {
        return nullptr;
    }
    const int width = static_cast<int>(image.GetWidth());
    const int height = static_cast<int>(image.GetHeight());
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down, matching the picture
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        return nullptr;
    }

    HDC dc = ::CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        ::DeleteObject(bitmap);
        return nullptr;
    }
    HGDIOBJ old = ::SelectObject(dc, bitmap);
    {
        Graphics graphics(dc);
        graphics.SetCompositingMode(CompositingModeSourceCopy);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.DrawImage(&image, 0, 0, width, height);
    }
    ::SelectObject(dc, old);
    ::DeleteDC(dc);
    return bitmap;
}

void DrawGlow(HDC dc, int cx, int cy, int rx, int ry, COLORREF color, int alpha) {
    if (g_token == 0 || rx <= 0 || ry <= 0 || alpha <= 0) {
        return;
    }
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    constexpr int kLayers = 12;
    const int perLayer = max(1, alpha / kLayers);
    for (int i = 0; i < kLayers; ++i) {
        const REAL scale = 1.0f - static_cast<REAL>(i) / (kLayers * 1.35f);
        const REAL w = static_cast<REAL>(rx) * 2.0f * scale;
        const REAL h = static_cast<REAL>(ry) * 2.0f * scale;
        SolidBrush brush(ToColor(color, static_cast<BYTE>(perLayer)));
        graphics.FillEllipse(&brush, static_cast<REAL>(cx) - w / 2.0f,
                             static_cast<REAL>(cy) - h / 2.0f, w, h);
    }
}

void DrawGhostMark(HDC dc, int cx, int cy, int size, COLORREF color, COLORREF eyeColor,
                   bool halo) {
    if (g_token == 0 || size <= 0) {
        return;
    }

    if (halo) {
        DrawGlow(dc, cx, cy, size * 4 / 5, size * 7 / 10, color, 18);
    }

    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    const REAL half = static_cast<REAL>(size) / 2.0f;
    const REAL left = static_cast<REAL>(cx) - half * 0.82f;
    const REAL top = static_cast<REAL>(cy) - half * 0.92f;
    const REAL w = half * 1.64f;
    const REAL h = half * 1.84f;
    auto px = [left, w](REAL v) { return left + w * v; };
    auto py = [top, h](REAL v) { return top + h * v; };

    if (halo) {
        Pen ring(ToColor(color, 10), max(1.0f, half * 0.02f));
        for (int i = 1; i <= 3; ++i) {
            const REAL scale = static_cast<REAL>(i) * 0.33f;
            graphics.DrawEllipse(&ring, px(0.5f) - w * scale, py(0.5f) - h * scale,
                                 w * scale * 2.0f, h * scale * 2.0f);
        }
    }

    GraphicsPath body;
    body.StartFigure();
    body.AddBezier(px(0.16f), py(0.58f), px(0.16f), py(0.18f), px(0.28f), py(0.05f),
                   px(0.5f), py(0.05f));
    body.AddBezier(px(0.5f), py(0.05f), px(0.72f), py(0.05f), px(0.84f), py(0.18f),
                   px(0.84f), py(0.58f));
    body.AddLine(px(0.84f), py(0.58f), px(0.84f), py(0.84f));
    body.AddBezier(px(0.84f), py(0.84f), px(0.77f), py(0.94f), px(0.66f), py(0.86f),
                   px(0.58f), py(0.9f));
    body.AddBezier(px(0.58f), py(0.9f), px(0.51f), py(0.95f), px(0.44f), py(0.87f),
                   px(0.36f), py(0.9f));
    body.AddBezier(px(0.36f), py(0.9f), px(0.27f), py(0.95f), px(0.2f), py(0.85f),
                   px(0.16f), py(0.82f));
    body.CloseFigure();

    SolidBrush fill(ToColor(color, halo ? 238 : 246));
    graphics.FillPath(&fill, &body);

    // Two soft eyes, cut out in the surface colour.
    SolidBrush eyes(ToColor(eyeColor, 255));
    graphics.FillEllipse(&eyes, px(0.33f), py(0.34f), w * 0.1f, h * 0.17f);
    graphics.FillEllipse(&eyes, px(0.57f), py(0.34f), w * 0.1f, h * 0.17f);
}

void DrawIcon(HDC dc, Icon icon, const RECT& rect, COLORREF color, int stroke) {
    if (g_token == 0 || icon == Icon::None) {
        return;
    }
    const REAL w = static_cast<REAL>(rect.right - rect.left);
    const REAL h = static_cast<REAL>(rect.bottom - rect.top);
    if (w <= 1.0f || h <= 1.0f) {
        return;
    }

    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);

    const REAL l = static_cast<REAL>(rect.left);
    const REAL t = static_cast<REAL>(rect.top);
    const REAL thickness = max(1.0f, static_cast<REAL>(stroke));
    Pen pen(ToColor(color), thickness);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
    SolidBrush brush(ToColor(color));

    auto px = [l, w](REAL v) { return l + w * v; };
    auto py = [t, h](REAL v) { return t + h * v; };
    auto pt = [&](REAL a, REAL b) { return PointF(px(a), py(b)); };

    switch (icon) {
        case Icon::Target: {
            graphics.DrawEllipse(&pen, px(0.12f), py(0.12f), w * 0.76f, h * 0.76f);
            SolidBrush dot(ToColor(color));
            graphics.FillEllipse(&dot, px(0.43f), py(0.43f), w * 0.14f, h * 0.14f);
            graphics.DrawLine(&pen, pt(0.5f, 0.02f), pt(0.5f, 0.15f));
            graphics.DrawLine(&pen, pt(0.5f, 0.85f), pt(0.5f, 0.98f));
            graphics.DrawLine(&pen, pt(0.02f, 0.5f), pt(0.15f, 0.5f));
            graphics.DrawLine(&pen, pt(0.85f, 0.5f), pt(0.98f, 0.5f));
            break;
        }
        case Icon::Grid: {
            const REAL gap = w * 0.08f;
            const REAL cell = (w - gap) / 2.0f;
            for (int row = 0; row < 2; ++row) {
                for (int col = 0; col < 2; ++col) {
                    RectF box(l + col * (cell + gap) + w * 0.04f,
                              t + row * (cell + gap) + w * 0.04f, cell - w * 0.08f,
                              cell - w * 0.08f);
                    GraphicsPath path;
                    RoundRectPath(path, box, w * 0.1f);
                    graphics.FillPath(&brush, &path);
                }
            }
            break;
        }
        case Icon::Gear: {
            const REAL cx = px(0.5f);
            const REAL cy = py(0.5f);
            const REAL radius = min(w, h) * 0.27f;
            graphics.DrawEllipse(&pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
            graphics.FillEllipse(&brush, cx - radius * 0.28f, cy - radius * 0.28f,
                                 radius * 0.56f, radius * 0.56f);
            const REAL inner = min(w, h) * 0.36f;
            const REAL outer = min(w, h) * 0.47f;
            for (int i = 0; i < 8; ++i) {
                const REAL angle = static_cast<REAL>(i) * 3.14159265f / 4.0f;
                const REAL dx = std::cos(angle);
                const REAL dy = std::sin(angle);
                graphics.DrawLine(&pen, cx + dx * inner, cy + dy * inner, cx + dx * outer,
                                  cy + dy * outer);
            }
            break;
        }
        case Icon::Plus:
            graphics.DrawLine(&pen, pt(0.5f, 0.16f), pt(0.5f, 0.84f));
            graphics.DrawLine(&pen, pt(0.16f, 0.5f), pt(0.84f, 0.5f));
            break;
        case Icon::ChevronRight:
            Polyline(graphics, pen, {pt(0.36f, 0.2f), pt(0.66f, 0.5f), pt(0.36f, 0.8f)}, false);
            break;
        case Icon::ChevronDown:
            Polyline(graphics, pen, {pt(0.2f, 0.36f), pt(0.5f, 0.66f), pt(0.8f, 0.36f)}, false);
            break;
        case Icon::Search:
            graphics.DrawEllipse(&pen, px(0.12f), py(0.12f), w * 0.56f, h * 0.56f);
            graphics.DrawLine(&pen, pt(0.63f, 0.63f), pt(0.88f, 0.88f));
            break;
        case Icon::Refresh: {
            const REAL inset = min(w, h) * 0.16f;
            graphics.DrawArc(&pen, l + inset, t + inset, w - inset * 2.0f, h - inset * 2.0f,
                             40.0f, 285.0f);
            SolidBrush arrow(ToColor(color));
            PointF points[] = {pt(0.72f, 0.06f), pt(0.94f, 0.3f), pt(0.6f, 0.3f)};
            graphics.FillPolygon(&arrow, points, 3);
            break;
        }
        case Icon::Keyboard: {
            GraphicsPath body;
            RoundRectPath(body,
                          RectF(px(0.04f), py(0.22f), w * 0.92f, h * 0.56f), w * 0.09f);
            graphics.DrawPath(&pen, &body);
            for (int row = 0; row < 3; ++row) {
                const REAL y = py(0.36f + static_cast<REAL>(row) * 0.14f);
                for (int col = 0; col < 5; ++col) {
                    const REAL x = px(0.16f + static_cast<REAL>(col) * 0.14f);
                    graphics.FillEllipse(&brush, x, y, w * 0.045f, h * 0.045f);
                }
            }
            graphics.DrawLine(&pen, pt(0.34f, 0.7f), pt(0.66f, 0.7f));
            break;
        }
        case Icon::Camera: {
            GraphicsPath body;
            RoundRectPath(body,
                          RectF(px(0.05f), py(0.3f), w * 0.9f, h * 0.54f), w * 0.1f);
            graphics.DrawPath(&pen, &body);
            graphics.DrawLine(&pen, pt(0.3f, 0.3f), pt(0.38f, 0.18f));
            graphics.DrawLine(&pen, pt(0.38f, 0.18f), pt(0.62f, 0.18f));
            graphics.DrawLine(&pen, pt(0.62f, 0.18f), pt(0.7f, 0.3f));
            graphics.DrawEllipse(&pen, px(0.36f), py(0.38f), w * 0.28f, h * 0.34f);
            break;
        }
        case Icon::Layers:
            Polyline(graphics, pen,
                     {pt(0.5f, 0.14f), pt(0.9f, 0.36f), pt(0.5f, 0.58f), pt(0.1f, 0.36f)}, true);
            Polyline(graphics, pen, {pt(0.1f, 0.54f), pt(0.5f, 0.76f), pt(0.9f, 0.54f)}, false);
            Polyline(graphics, pen, {pt(0.1f, 0.7f), pt(0.5f, 0.92f), pt(0.9f, 0.7f)}, false);
            break;
        case Icon::Shield: {
            GraphicsPath path;
            path.StartFigure();
            path.AddLine(pt(0.5f, 0.06f), pt(0.9f, 0.22f));
            path.AddLine(pt(0.9f, 0.22f), pt(0.87f, 0.56f));
            path.AddBezier(pt(0.87f, 0.56f), pt(0.82f, 0.78f), pt(0.68f, 0.9f), pt(0.5f, 0.95f));
            path.AddBezier(pt(0.5f, 0.95f), pt(0.32f, 0.9f), pt(0.18f, 0.78f), pt(0.13f, 0.56f));
            path.AddLine(pt(0.13f, 0.56f), pt(0.1f, 0.22f));
            path.CloseFigure();
            graphics.DrawPath(&pen, &path);
            break;
        }
        case Icon::Trash:
            graphics.DrawLine(&pen, pt(0.14f, 0.26f), pt(0.86f, 0.26f));
            graphics.DrawLine(&pen, pt(0.38f, 0.26f), pt(0.4f, 0.14f));
            graphics.DrawLine(&pen, pt(0.4f, 0.14f), pt(0.6f, 0.14f));
            graphics.DrawLine(&pen, pt(0.6f, 0.14f), pt(0.62f, 0.26f));
            Polyline(graphics, pen,
                     {pt(0.2f, 0.26f), pt(0.24f, 0.9f), pt(0.76f, 0.9f), pt(0.8f, 0.26f)}, false);
            graphics.DrawLine(&pen, pt(0.4f, 0.4f), pt(0.42f, 0.76f));
            graphics.DrawLine(&pen, pt(0.6f, 0.4f), pt(0.58f, 0.76f));
            break;
        case Icon::Folder: {
            GraphicsPath path;
            path.StartFigure();
            path.AddLine(pt(0.08f, 0.28f), pt(0.38f, 0.28f));
            path.AddLine(pt(0.38f, 0.28f), pt(0.46f, 0.38f));
            path.AddLine(pt(0.46f, 0.38f), pt(0.92f, 0.38f));
            path.AddLine(pt(0.92f, 0.38f), pt(0.92f, 0.82f));
            path.AddLine(pt(0.92f, 0.82f), pt(0.08f, 0.82f));
            path.CloseFigure();
            graphics.DrawPath(&pen, &path);
            break;
        }
        case Icon::Monitor:
            graphics.DrawRectangle(&pen, px(0.07f), py(0.14f), w * 0.86f, h * 0.58f);
            graphics.DrawLine(&pen, pt(0.5f, 0.72f), pt(0.5f, 0.84f));
            graphics.DrawLine(&pen, pt(0.32f, 0.88f), pt(0.68f, 0.88f));
            break;
        case Icon::Eye: {
            GraphicsPath path;
            path.StartFigure();
            path.AddBezier(pt(0.06f, 0.5f), pt(0.28f, 0.18f), pt(0.72f, 0.18f), pt(0.94f, 0.5f));
            path.AddBezier(pt(0.94f, 0.5f), pt(0.72f, 0.82f), pt(0.28f, 0.82f), pt(0.06f, 0.5f));
            graphics.DrawPath(&pen, &path);
            graphics.FillEllipse(&brush, px(0.4f), py(0.38f), w * 0.2f, h * 0.24f);
            break;
        }
        case Icon::EyeOff: {
            GraphicsPath path;
            path.StartFigure();
            path.AddBezier(pt(0.06f, 0.5f), pt(0.28f, 0.18f), pt(0.72f, 0.18f), pt(0.94f, 0.5f));
            path.AddBezier(pt(0.94f, 0.5f), pt(0.72f, 0.82f), pt(0.28f, 0.82f), pt(0.06f, 0.5f));
            graphics.DrawPath(&pen, &path);
            graphics.DrawLine(&pen, pt(0.18f, 0.16f), pt(0.82f, 0.86f));
            break;
        }
        case Icon::Warning: {
            GraphicsPath path;
            path.StartFigure();
            path.AddLine(pt(0.5f, 0.08f), pt(0.94f, 0.88f));
            path.AddLine(pt(0.94f, 0.88f), pt(0.06f, 0.88f));
            path.CloseFigure();
            graphics.DrawPath(&pen, &path);
            graphics.DrawLine(&pen, pt(0.5f, 0.38f), pt(0.5f, 0.62f));
            graphics.FillEllipse(&brush, px(0.47f), py(0.69f), w * 0.06f, h * 0.06f);
            break;
        }
        case Icon::Info:
            graphics.DrawEllipse(&pen, px(0.08f), py(0.08f), w * 0.84f, h * 0.84f);
            graphics.DrawLine(&pen, pt(0.5f, 0.45f), pt(0.5f, 0.72f));
            graphics.FillEllipse(&brush, px(0.47f), py(0.27f), w * 0.06f, h * 0.06f);
            break;
        case Icon::Check:
            Polyline(graphics, pen, {pt(0.18f, 0.52f), pt(0.42f, 0.76f), pt(0.83f, 0.26f)}, false);
            break;
        case Icon::Minimize:
            graphics.DrawLine(&pen, pt(0.2f, 0.5f), pt(0.8f, 0.5f));
            break;
        case Icon::Maximize:
            graphics.DrawRectangle(&pen, px(0.22f), py(0.22f), w * 0.56f, h * 0.56f);
            break;
        case Icon::Restore:
            graphics.DrawRectangle(&pen, px(0.14f), py(0.3f), w * 0.48f, h * 0.48f);
            graphics.DrawRectangle(&pen, px(0.32f), py(0.14f), w * 0.5f, h * 0.5f);
            break;
        case Icon::Close:
            graphics.DrawLine(&pen, pt(0.24f, 0.24f), pt(0.76f, 0.76f));
            graphics.DrawLine(&pen, pt(0.76f, 0.24f), pt(0.24f, 0.76f));
            break;
        case Icon::Bolt: {
            PointF points[] = {pt(0.56f, 0.04f), pt(0.24f, 0.55f), pt(0.46f, 0.55f),
                               pt(0.4f, 0.96f), pt(0.76f, 0.42f), pt(0.53f, 0.42f)};
            graphics.FillPolygon(&brush, points, 6);
            break;
        }
        case Icon::ArrowUpRight:
            graphics.DrawLine(&pen, pt(0.24f, 0.76f), pt(0.76f, 0.24f));
            Polyline(graphics, pen, {pt(0.4f, 0.24f), pt(0.76f, 0.24f), pt(0.76f, 0.6f)}, false);
            break;
        case Icon::Dot:
            graphics.FillEllipse(&brush, px(0.2f), py(0.2f), w * 0.6f, h * 0.6f);
            break;
        case Icon::Ghost:
            // The brand mark is drawn separately so it can carry the halo.
            break;
        case Icon::None:
        default:
            break;
    }
}

}  // namespace ui