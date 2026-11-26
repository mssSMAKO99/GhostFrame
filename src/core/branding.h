// Installer branding manifest.
//
// install.ps1 asks GfBrand.exe to produce a renamed, re-iconed copy of the
// launcher and then records what it installed in
// %LOCALAPPDATA%\GhostFrame\branding.json.  The manifest is what lets an update
// preserve the previous name/description/icon and lets uninstall remove exactly
// the files that were created, without guessing.
#pragma once

#include <string>

namespace gf {

struct BrandingManifest {
    int version = 1;

    // The base executable that was downloaded and patched.
    std::wstring originalExeName;  // e.g. "GhostFrame.exe"
    std::wstring originalExePath;  // full path to the verified base copy

    // The branding applied to it.
    std::wstring name;  // display name and output file stem
    std::wstring description;
    std::wstring company;
    // "bundled", "emoji" or "file".
    std::wstring iconKind;
    // The emoji itself (kind == emoji) or the source .ico path (kind == file).
    std::wstring iconSource;

    // Where it was installed.
    std::wstring installedExePath;
    std::wstring installDir;
    std::wstring shortcutPath;
    std::wstring autostartValue;
    std::wstring appliedAt;  // local ISO-8601
};

// %LOCALAPPDATA%\GhostFrame\branding.json (same fallback directory as the
// settings file).
std::wstring BrandingManifestPath();

std::wstring SerializeBrandingManifest(const BrandingManifest& manifest);
bool ParseBrandingManifest(const std::wstring& json, BrandingManifest& out,
                           std::wstring& error);

// Explicit-path variants shared by GfBrand and the tests.
bool LoadBrandingManifestFile(const std::wstring& path, BrandingManifest& out,
                              std::wstring& error);
bool SaveBrandingManifestFile(const std::wstring& path, const BrandingManifest& manifest,
                              std::wstring& error);

bool LoadBrandingManifest(BrandingManifest& out, std::wstring& error);
bool SaveBrandingManifest(const BrandingManifest& manifest, std::wstring& error);

// Removes the manifest; a missing file is not an error.
bool DeleteBrandingManifest(std::wstring& error);

std::wstring NowLocalIso8601();

}  // namespace gf