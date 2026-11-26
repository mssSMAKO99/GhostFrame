// Console harness for GfBrand: manifest round-trip, VERSIONINFO replacement,
// icon group replacement and the signed-input refusal path.
//
// The harness brands a copy of its own executable so it needs no external
// fixture and never patches a file that is in use.

#include <windows.h>

#include <stdio.h>

#include <cwchar>
#include <string>
#include <vector>

#include "branding.h"
#include "emoji_icon.h"
#include "exe_branding.h"

namespace {

int g_failures = 0;

void Check(bool condition, const wchar_t* what) {
    wprintf(condition ? L"[ok  ] %s\n" : L"[FAIL] %s\n", what);
    if (!condition) {
        ++g_failures;
    }
}

std::wstring EnsureWritableDir() {
    std::vector<std::wstring> candidates;
    wchar_t temp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, temp) != 0) {
        candidates.push_back(std::wstring(temp) + L"opencode\\GfBrandHost");
        candidates.push_back(std::wstring(temp) + L"GfBrandHost");
    }
    wchar_t cwd[MAX_PATH] = {};
    if (::GetCurrentDirectoryW(MAX_PATH, cwd) != 0) {
        candidates.push_back(std::wstring(cwd) + L"\\GfBrandHost.test");
    }
    for (const std::wstring& dir : candidates) {
        ::CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring probe = dir + L"\\.probe";
        HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file);
            ::DeleteFileW(probe.c_str());
            return dir;
        }
    }
    return std::wstring();
}

std::wstring TempFile(const wchar_t* name) {
    static const std::wstring dir = EnsureWritableDir();
    return dir + L"\\" + name;
}

void RemoveFile(const std::wstring& path) {
    ::DeleteFileW(path.c_str());
    ::DeleteFileW((path + L".tmp").c_str());
}

std::wstring SelfPath() {
    std::vector<wchar_t> buffer(MAX_PATH * 4);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::wstring(buffer.data(), buffer.size());
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool SameManifest(const gf::BrandingManifest& left, const gf::BrandingManifest& right) {
    return left.version == right.version && left.originalExeName == right.originalExeName &&
           left.originalExePath == right.originalExePath && left.name == right.name &&
           left.description == right.description && left.company == right.company &&
           left.iconKind == right.iconKind && left.iconSource == right.iconSource &&
           left.installedExePath == right.installedExePath &&
           left.installDir == right.installDir && left.shortcutPath == right.shortcutPath &&
           left.autostartValue == right.autostartValue && left.appliedAt == right.appliedAt;
}

void TestManifestRoundTrip() {
    gf::BrandingManifest manifest;
    manifest.version = 1;
    manifest.originalExeName = L"GhostFrame.exe";
    manifest.originalExePath = L"C:\\Temp\\GhostFrame.exe";
    manifest.name = L"MyApp";
    manifest.description = L"MyApp - capture exclusion";
    manifest.company = L"Example Co";
    manifest.iconKind = L"emoji";
    manifest.iconSource = L"\xD83D\xDC68\x200D\xD83D\xDC69\x200D\xD83D\xDC67";
    manifest.installedExePath = L"C:\\Apps\\MyApp.exe";
    manifest.installDir = L"C:\\Apps";
    manifest.shortcutPath = L"C:\\Menu\\MyApp.lnk";
    manifest.autostartValue = L"MyApp";
    manifest.appliedAt = L"2026-11-26T12:34:56";

    const std::wstring json = gf::SerializeBrandingManifest(manifest);
    gf::BrandingManifest parsed;
    std::wstring error;
    Check(gf::ParseBrandingManifest(json, parsed, error),
          L"the branding manifest JSON parses back");
    Check(SameManifest(manifest, parsed), L"every branding field survives the round-trip");

    const std::wstring path = TempFile(L"branding-test.json");
    RemoveFile(path);
    Check(gf::SaveBrandingManifestFile(path, manifest, error), L"the branding manifest is saved");
    gf::BrandingManifest loaded;
    Check(gf::LoadBrandingManifestFile(path, loaded, error),
          L"the branding manifest is loaded from disk");
    Check(SameManifest(manifest, loaded), L"the saved manifest loads back identically");
    RemoveFile(path);

    gf::BrandingManifest missing;
    Check(!gf::LoadBrandingManifestFile(TempFile(L"no-such-branding.json"), missing, error),
          L"a missing branding manifest is reported");
}

void TestBrandExecutable() {
    const std::wstring input = SelfPath();
    const std::wstring output = TempFile(L"BrandedApp.exe");
    const std::wstring icon = TempFile(L"brand-icon.ico");
    RemoveFile(output);
    RemoveFile(icon);

    std::wstring error;
    Check(gf::RenderEmojiIcon(L"\xD83D\xDC7B", icon, error),
          L"the emoji icon renders for branding");

    gf::BrandOptions options;
    options.inputPath = input;
    options.outputPath = output;
    options.name = L"BrandedApp";
    options.description = L"Branded harness application";
    options.company = L"Brand Host";
    options.iconPath = icon;

    const gf::BrandStatus status = gf::BrandExecutable(options, error);
    Check(status == gf::BrandStatus::Ok, L"BrandExecutable patches a copy");
    Check(::GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"the branded executable exists");

    gf::VersionStrings strings;
    gf::VersionFixed fixed;
    Check(gf::ReadVersionInfo(output, strings, fixed, error),
          L"the branded executable has readable VERSIONINFO");
    Check(strings.productName == L"BrandedApp", L"ProductName is the custom name");
    Check(strings.description == L"Branded harness application",
          L"FileDescription is the custom description");
    Check(strings.company == L"Brand Host", L"CompanyName is the custom company");
    Check(strings.originalFilename == L"BrandedApp.exe",
          L"OriginalFilename follows the output file name");
    Check(strings.internalName == L"BrandedApp.exe",
          L"InternalName follows the output file name");
    Check(!strings.fileVersion.empty(), L"FileVersion is present");

    Check(gf::CountIconGroups(output) == 1, L"the branded executable has one icon group");
    Check(gf::CountIconResources(output) == 7,
          L"the icon group contains the seven rendered images");

    gf::VersionStrings originalStrings;
    gf::VersionFixed originalFixed;
    if (gf::ReadVersionInfo(input, originalStrings, originalFixed, error)) {
        Check(fixed.fileVersionMS == originalFixed.fileVersionMS &&
                  fixed.fileVersionLS == originalFixed.fileVersionLS,
              L"the numeric file version is preserved");
    }

    // Same input and output must be refused before touching anything.
    gf::BrandOptions same = options;
    same.outputPath = input;
    Check(gf::BrandExecutable(same, error) == gf::BrandStatus::InvalidArguments,
          L"an in-place patch is refused");

    // Missing input.
    gf::BrandOptions missingInput = options;
    missingInput.inputPath = TempFile(L"does-not-exist.exe");
    missingInput.outputPath = TempFile(L"should-not-exist.exe");
    RemoveFile(missingInput.outputPath);
    Check(gf::BrandExecutable(missingInput, error) == gf::BrandStatus::IoError,
          L"a missing input executable is reported");
    Check(::GetFileAttributesW(missingInput.outputPath.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"no output is created for a missing input");

    // Missing icon.
    gf::BrandOptions missingIcon = options;
    missingIcon.iconPath = TempFile(L"does-not-exist.ico");
    missingIcon.outputPath = TempFile(L"should-not-exist-2.exe");
    RemoveFile(missingIcon.outputPath);
    Check(gf::BrandExecutable(missingIcon, error) == gf::BrandStatus::IconError,
          L"a missing icon file is reported");
    Check(::GetFileAttributesW(missingIcon.outputPath.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"no output is created for a missing icon");

    RemoveFile(output);
    RemoveFile(icon);
}

void TestSignedRefusal() {
    // Windows inbox binaries are catalog-signed (no embedded certificate
    // table), so look for an Authenticode-signed development tool instead.
    const wchar_t* const candidates[] = {
        L"\\Program Files\\Git\\cmd\\git.exe",
        L"\\Program Files\\dotnet\\dotnet.exe",
        L"\\Program Files\\nodejs\\node.exe",
        L"\\Program Files\\PowerShell\\7\\pwsh.exe",
    };
    std::wstring signedCandidate;
    wchar_t systemDrive[MAX_PATH] = {};
    const UINT driveLength = ::GetEnvironmentVariableW(L"SystemDrive", systemDrive, MAX_PATH);
    if (driveLength != 0) {
        for (const wchar_t* relative : candidates) {
            const std::wstring path = std::wstring(systemDrive) + relative;
            std::wstring probeError;
            if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
                gf::IsExecutableSigned(path, probeError)) {
                signedCandidate = path;
                break;
            }
        }
    }
    if (signedCandidate.empty()) {
        wprintf(L"[skip] no embedded-signed executable is available\n");
    } else {
        std::wstring error;
        Check(gf::IsExecutableSigned(signedCandidate, error),
              L"an Authenticode-signed executable is detected");

        gf::BrandOptions options;
        options.inputPath = signedCandidate;
        options.outputPath = TempFile(L"SignedRefusal.exe");
        options.name = L"Refused";
        RemoveFile(options.outputPath);
        Check(gf::BrandExecutable(options, error) == gf::BrandStatus::SignedRefused,
              L"a signed base is refused by default");
        Check(::GetFileAttributesW(options.outputPath.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"the refusal does not leave an output file");
        Check(error.find(L"signature") != std::wstring::npos,
              L"the refusal explains the signature problem");
        RemoveFile(options.outputPath);
    }

    std::wstring error;
    Check(!gf::IsExecutableSigned(SelfPath(), error),
          L"the unsigned harness executable is not mistaken for a signed one");
}

}  // namespace

int wmain() {
    TestManifestRoundTrip();
    TestBrandExecutable();
    TestSignedRefusal();

    wprintf(g_failures == 0 ? L"PASS\n" : L"FAIL (%d)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}