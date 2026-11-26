// Emoji product icon.
//
// The user picks one emoji in APPEARANCE; we render it into a multi-size .ico
// with DirectWrite/Direct2D (colour glyphs where the font provides them) and
// fall back to a monochrome GDI render on systems without a usable D3D/D2D
// device.  The generated file is shared by the UI window, the daemon tray and
// the shortcut manager; the bundled resource icon is the final fallback.
#pragma once

#include <windows.h>

#include <string>

namespace gf {

// %LOCALAPPDATA%\GhostFrame\GhostFrame.ico (with the same temp fallback as the
// settings file).
std::wstring EmojiIconPath();

// Renders (or reuses) the icon for `emoji`.  An empty emoji removes the
// generated file and reports success.  `error` explains a render failure; the
// caller should keep the previous icon in that case.
bool EnsureEmojiIcon(const std::wstring& emoji, std::wstring& error);

// Renders to an explicit path.  Used by the icon test; EnsureEmojiIcon caches
// the last emoji for the normal path.
bool RenderEmojiIcon(const std::wstring& emoji, const std::wstring& path, std::wstring& error);

// Loads the generated icon at `size`, falling back to `resourceId` in `module`
// (and then the stock application icon).  `owned` tells the caller whether the
// returned handle must be destroyed with DestroyIcon.
HICON LoadConfiguredIcon(HMODULE module, int resourceId, const std::wstring& emoji, int size,
                         bool& owned);

// Header sanity check for the generated file (ICONDIR + every ICONDIRENTRY).
bool IsValidIcoFile(const std::wstring& path, std::wstring& error);
int CountIcoImages(const std::wstring& path);

}  // namespace gf