// GfBrand.exe - install-time branding helper.
//
// Copies the verified GhostFrame base executable, replaces its icon group and
// VS_VERSION_INFO, and records the result in branding.json.

#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>

#include <cwchar>
#include <string>
#include <vector>

#include "branding.h"
#include "emoji_icon.h"
#include "exe_branding.h"
#include "settings.h"

namespace {

enum ExitCode {
    kOk = 0,
    kUsage = 1,
    kIo = 2,
    kSigned = 3,
    kResource = 4,
    kIcon = 5,
    kManifest = 6,
};

struct Options {
    std::wstring input;
    std::wstring output;
    std::wstring name;
    std::wstring description;
    std::wstring company;
    std::wstring emoji;
    std::wstring icon;
    std::wstring manifest;
    std::wstring installDir;
    std::wstring shortcut;
    std::wstring autostart;
    bool force = false;
    bool quiet = false;
    bool help = false;
};

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return text;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    const std::wstring::size_type dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    return name;
}

std::wstring FullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = ::GetFullPathNameW(path.c_str(), _countof(buffer), buffer, nullptr);
    return (length == 0 || length >= _countof(buffer)) ? path : std::wstring(buffer);
}

void Print(const wchar_t* text) {
    fputws(text, stdout);
    fputws(L"\n", stdout);
}

bool TakeValue(int argc, wchar_t** argv, int& index, std::wstring& out) {
    const std::wstring argument = argv[index];
    const std::wstring::size_type equals = argument.find(L'=');
    if (equals != std::wstring::npos) {
        out = argument.substr(equals + 1);
        return true;
    }
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

bool ParseArguments(int argc, wchar_t** argv, Options& options, std::wstring& error) {
    for (int i = 1; i < argc; ++i) {
        std::wstring argument = argv[i];
        std::wstring key = argument;
        const std::wstring::size_type equals = key.find(L'=');
        if (equals != std::wstring::npos) {
            key.resize(equals);
        }
        key = Lower(key);

        if (key == L"--help" || key == L"-h" || key == L"/?") {
            options.help = true;
        } else if (key == L"--force") {
            options.force = true;
        } else if (key == L"--quiet") {
            options.quiet = true;
        } else if (key == L"--input") {
            if (!TakeValue(argc, argv, i, options.input)) {
                error = L"--input needs a value";
                return false;
            }
        } else if (key == L"--output") {
            if (!TakeValue(argc, argv, i, options.output)) {
                error = L"--output needs a value";
                return false;
            }
        } else if (key == L"--name") {
            if (!TakeValue(argc, argv, i, options.name)) {
                error = L"--name needs a value";
                return false;
            }
        } else if (key == L"--description") {
            if (!TakeValue(argc, argv, i, options.description)) {
                error = L"--description needs a value";
                return false;
            }
        } else if (key == L"--company") {
            if (!TakeValue(argc, argv, i, options.company)) {
                error = L"--company needs a value";
                return false;
            }
        } else if (key == L"--emoji") {
            if (!TakeValue(argc, argv, i, options.emoji)) {
                error = L"--emoji needs a value";
                return false;
            }
        } else if (key == L"--icon") {
            if (!TakeValue(argc, argv, i, options.icon)) {
                error = L"--icon needs a value";
                return false;
            }
        } else if (key == L"--manifest") {
            if (!TakeValue(argc, argv, i, options.manifest)) {
                error = L"--manifest needs a value";
                return false;
            }
        } else if (key == L"--install-dir" || key == L"--installdir") {
            if (!TakeValue(argc, argv, i, options.installDir)) {
                error = L"--install-dir needs a value";
                return false;
            }
        } else if (key == L"--shortcut") {
            if (!TakeValue(argc, argv, i, options.shortcut)) {
                error = L"--shortcut needs a value";
                return false;
            }
        } else if (key == L"--autostart") {
            if (!TakeValue(argc, argv, i, options.autostart)) {
                error = L"--autostart needs a value";
                return false;
            }
        } else {
            error = L"unknown argument: " + argument;
            return false;
        }
    }
    return true;
}

void Usage() {
    Print(L"GfBrand.exe --input <base.exe> --output <branded.exe> [options]");
    Print(L"");
    Print(L"  --name <text>          product/display name and manifest value");
    Print(L"  --description <text>   FileDescription and manifest description");
    Print(L"  --company <text>       CompanyName");
    Print(L"  --emoji <emoji>        render the product icon from one emoji");
    Print(L"  --icon <file.ico>      use a supplied icon instead (overrides --emoji)");
    Print(L"  --manifest <path>      branding.json path (default %LOCALAPPDATA%)");
    Print(L"  --install-dir <path>   recorded install directory");
    Print(L"  --shortcut <path>      recorded shortcut path");
    Print(L"  --autostart <name>     recorded Run value name");
    Print(L"  --force                patch a signed base executable anyway");
    Print(L"  --quiet                only report errors");
    Print(L"");
    Print(L"Exit codes: 0 ok, 1 usage, 2 I/O, 3 signed base, 4 resource, 5 icon,");
    Print(L"            6 manifest.");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    std::wstring error;
    if (!ParseArguments(argc, argv, options, error)) {
        fwprintf(stderr, L"GfBrand: %s\n", error.c_str());
        Usage();
        return kUsage;
    }
    if (options.help) {
        Usage();
        return kOk;
    }
    if (options.input.empty() || options.output.empty()) {
        fwprintf(stderr, L"GfBrand: --input and --output are required\n");
        Usage();
        return kUsage;
    }
    if (!options.emoji.empty() && !options.icon.empty()) {
        fwprintf(stderr, L"GfBrand: choose either --emoji or --icon, not both\n");
        return kUsage;
    }
    if (options.name.empty()) {
        options.name = StemOf(options.output);
    }
    if (options.autostart.empty()) {
        options.autostart = options.name;
    }

    // Resolve the icon first: either a supplied .ico or a freshly rendered
    // emoji.  The generated file is temporary and removed after patching.
    std::wstring iconPath = options.icon.empty() ? std::wstring() : FullPath(options.icon);
    std::wstring iconKind = L"bundled";
    std::wstring iconSource;
    std::wstring temporaryIcon;
    if (!iconPath.empty()) {
        iconKind = L"file";
        iconSource = iconPath;
    } else if (!options.emoji.empty()) {
        const std::wstring normalized = gf::NormalizeEmoji(options.emoji);
        if (normalized.empty()) {
            fwprintf(stderr, L"GfBrand: --emoji did not contain a usable emoji\n");
            return kIcon;
        }
        wchar_t temp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, temp) == 0) {
            fwprintf(stderr, L"GfBrand: cannot resolve the temporary directory\n");
            return kIcon;
        }
        wchar_t name[64] = {};
        swprintf_s(name, L"GfBrand-%lu-%llu.ico", ::GetCurrentProcessId(),
                   static_cast<unsigned long long>(::GetTickCount64()));
        temporaryIcon = std::wstring(temp) + name;
        if (!gf::RenderEmojiIcon(normalized, temporaryIcon, error)) {
            fwprintf(stderr, L"GfBrand: cannot render the emoji icon: %s\n", error.c_str());
            return kIcon;
        }
        iconPath = temporaryIcon;
        iconKind = L"emoji";
        iconSource = normalized;
    }

    gf::BrandOptions brand;
    brand.inputPath = options.input;
    brand.outputPath = options.output;
    brand.name = options.name;
    brand.description = options.description;
    brand.company = options.company;
    brand.iconPath = iconPath;
    brand.force = options.force;

    const gf::BrandStatus status = gf::BrandExecutable(brand, error);
    if (!temporaryIcon.empty()) {
        ::DeleteFileW(temporaryIcon.c_str());
    }
    if (status != gf::BrandStatus::Ok) {
        int code = kResource;
        switch (status) {
            case gf::BrandStatus::InvalidArguments:
                code = kUsage;
                break;
            case gf::BrandStatus::IoError:
                code = kIo;
                break;
            case gf::BrandStatus::SignedRefused:
                code = kSigned;
                break;
            case gf::BrandStatus::IconError:
                code = kIcon;
                break;
            case gf::BrandStatus::ResourceError:
            default:
                code = kResource;
                break;
        }
        fwprintf(stderr, L"GfBrand: %s\n", error.c_str());
        return code;
    }

    // Re-read the written VersionInfo so the manifest records exactly what is
    // in the file rather than the requested values.
    gf::VersionStrings strings;
    gf::VersionFixed fixed;
    if (!gf::ReadVersionInfo(options.output, strings, fixed, error)) {
        strings.description = options.description;
        strings.company = options.company;
    }

    gf::BrandingManifest manifest;
    manifest.version = 1;
    manifest.originalExeName = FileNameOf(options.input);
    manifest.originalExePath = FullPath(options.input);
    manifest.name = options.name;
    manifest.description = strings.description;
    manifest.company = strings.company;
    manifest.iconKind = iconKind;
    manifest.iconSource = iconSource;
    manifest.installedExePath = FullPath(options.output);
    manifest.installDir = options.installDir.empty() ? DirectoryOf(FullPath(options.output))
                                                     : FullPath(options.installDir);
    manifest.shortcutPath = options.shortcut;
    manifest.autostartValue = options.autostart;
    manifest.appliedAt = gf::NowLocalIso8601();

    const std::wstring manifestPath =
        options.manifest.empty() ? gf::BrandingManifestPath() : FullPath(options.manifest);
    if (!gf::SaveBrandingManifestFile(manifestPath, manifest, error)) {
        fwprintf(stderr, L"GfBrand: %s\n", error.c_str());
        return kManifest;
    }

    if (!options.quiet) {
        wprintf(L"Branded: %s\n", manifest.installedExePath.c_str());
        wprintf(L"  name:        %s\n", manifest.name.c_str());
        wprintf(L"  description: %s\n", manifest.description.c_str());
        wprintf(L"  company:     %s\n", manifest.company.c_str());
        wprintf(L"  icon:        %s\n", iconKind.c_str());
        wprintf(L"  manifest:    %s\n", manifestPath.c_str());
    }
    return kOk;
}