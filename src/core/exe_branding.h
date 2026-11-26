// Executable resource patching for install-time branding.
//
// GfBrand.exe copies the verified base launcher, then rewrites its icon group
// and VS_VERSION_INFO with BeginUpdateResource/UpdateResource/EndUpdateResource.
// This module is UI-free so GfBrandHost can exercise it headlessly.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gf {

struct VersionFixed {
    DWORD fileVersionMS = 0;
    DWORD fileVersionLS = 0;
    DWORD productVersionMS = 0;
    DWORD productVersionLS = 0;
};

struct VersionStrings {
    std::wstring company;
    std::wstring description;
    std::wstring productName;
    std::wstring internalName;
    std::wstring originalFilename;
    std::wstring legalCopyright;
    std::wstring fileVersion;
    std::wstring productVersion;
};

struct BrandOptions {
    std::wstring inputPath;
    std::wstring outputPath;
    std::wstring name;
    std::wstring description;
    std::wstring company;
    // A validated .ico file, or empty to keep the bundled icon.
    std::wstring iconPath;
    bool force = false;
};

enum class BrandStatus {
    Ok = 0,
    InvalidArguments,
    IoError,
    SignedRefused,
    IconError,
    ResourceError,
};

// Reads VERSIONINFO from an executable.  Returns false when it has none.
bool ReadVersionInfo(const std::wstring& path, VersionStrings& strings, VersionFixed& fixed,
                     std::wstring& error);

// Builds a complete VS_VERSION_INFO resource image.
std::vector<uint8_t> BuildVersionInfoBlob(const VersionStrings& strings,
                                          const VersionFixed& fixed);

// True when the file carries an Authenticode signature (valid or not).  Patching
// any signed file invalidates it, so GfBrand refuses unless forced.
bool IsExecutableSigned(const std::wstring& path, std::wstring& error);

// Number of RT_ICON resources in a file.
int CountIconResources(const std::wstring& path);
// Number of RT_GROUP_ICON resources in a file.
int CountIconGroups(const std::wstring& path);

// Copies input to output and applies the icon/version resources.  On failure the
// partial output is removed.
BrandStatus BrandExecutable(const BrandOptions& options, std::wstring& error);

}  // namespace gf