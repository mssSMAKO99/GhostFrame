#pragma once

#include <windows.h>

// Small vector icon set used by the redesigned launcher chrome.  The icons are
// rendered with GDI+ so they stay anti-aliased at every DPI; every function is a
// no-op when GDI+ could not be started, so the window still works on a stripped
// down system.
namespace ui {

enum class Icon {
    None,
    Ghost,
    Target,
    Grid,
    Gear,
    Plus,
    ChevronRight,
    ChevronDown,
    Search,
    Refresh,
    Keyboard,
    Camera,
    Layers,
    Shield,
    Trash,
    Folder,
    Monitor,
    Eye,
    EyeOff,
    Warning,
    Info,
    Check,
    Minimize,
    Maximize,
    Restore,
    Close,
    Bolt,
    ArrowUpRight,
    Dot,
};

// Reference counted; call once from the UI thread before the first paint.
void InitIcons();
void ShutdownIcons();

// `stroke` is in device pixels and is scaled by the caller when needed.
void DrawIcon(HDC dc, Icon icon, const RECT& rect, COLORREF color, int stroke = 2);

// The brand mark: a rounded ghost silhouette with a soft emerald halo.  `size`
// is the height of the mark; it is centred on (cx, cy).
void DrawGhostMark(HDC dc, int cx, int cy, int size, COLORREF color, COLORREF eyeColor,
                   bool halo);

// Radial glow made of a handful of translucent ellipses.
void DrawGlow(HDC dc, int cx, int cy, int rx, int ry, COLORREF color, int alpha);

}  // namespace ui