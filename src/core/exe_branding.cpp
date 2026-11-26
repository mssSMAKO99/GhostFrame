#include "exe_branding.h"

#include <winver.h>

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace gf {

namespace {

struct IcoImage {
    WORD width = 0;
    WORD height = 0;
    WORD colorCount = 0;
    WORD planes = 1;
    WORD bitCount = 32;
    std::vector<uint8_t> bytes;
};

struct ResourceId {
    WORD id = 0;
    std::vector<WORD> languages;
};

#pragma pack(push, 1)
struct IcoDirEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytes;
    DWORD offset;
};

struct GrpIconDir {
    WORD reserved;
    WORD type;
    WORD count;
};

struct GrpIconEntry {
    BYTE width;
    BYTE height;
    BYTE colorCount;
    BYTE reserved;
    WORD planes;
    WORD bitCount;
    DWORD bytes;
    WORD id;
};
#pragma pack(pop)

std::wstring Win32Text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr) {
        text.assign(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L'.')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (text.empty()) {
        wchar_t fallback[64] = {};
        swprintf_s(fallback, L"Windows error %lu", code);
        text = fallback;
    }
    return text;
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH * 4] = {};
    const DWORD length = ::GetFullPathNameW(path.c_str(), _countof(buffer), buffer, nullptr);
    if (length == 0 || length >= _countof(buffer)) {
        return path;
    }
    return buffer;
}

bool ReadAllBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring& error) {
    out.clear();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot open " + path + L": " + Win32Text(::GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64ll << 20)) {
        ::CloseHandle(file);
        error = L"invalid icon file size";
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(out.size() - done, static_cast<size_t>(1u << 20)));
        if (!::ReadFile(file, out.data() + done, chunk, &read, nullptr) || read == 0) {
            break;
        }
        done += read;
    }
    ::CloseHandle(file);
    if (done != out.size()) {
        error = L"cannot read " + path;
        out.clear();
        return false;
    }
    error.clear();
    return true;
}

bool LoadIco(const std::wstring& path, std::vector<IcoImage>& images, std::wstring& error) {
    images.clear();
    std::vector<uint8_t> data;
    if (!ReadAllBytes(path, data, error)) {
        return false;
    }
    if (data.size() < 6) {
        error = L"the icon file is truncated";
        return false;
    }
    const uint16_t reserved = static_cast<uint16_t>(data[0] | (data[1] << 8));
    const uint16_t type = static_cast<uint16_t>(data[2] | (data[3] << 8));
    const uint16_t count = static_cast<uint16_t>(data[4] | (data[5] << 8));
    if (reserved != 0 || type != 1 || count == 0 || count > 32 ||
        data.size() < 6 + static_cast<size_t>(count) * sizeof(IcoDirEntry)) {
        error = L"the icon directory is invalid";
        return false;
    }

    for (uint16_t i = 0; i < count; ++i) {
        IcoDirEntry entry = {};
        std::memcpy(&entry, data.data() + 6 + static_cast<size_t>(i) * sizeof(entry),
                    sizeof(entry));
        if (entry.bytes == 0 ||
            static_cast<uint64_t>(entry.offset) + entry.bytes > data.size()) {
            error = L"an icon image is outside the file";
            return false;
        }
        IcoImage image;
        image.width = entry.width == 0 ? 256 : entry.width;
        image.height = entry.height == 0 ? 256 : entry.height;
        image.colorCount = entry.colorCount;
        image.planes = entry.planes == 0 ? 1 : entry.planes;
        image.bitCount = entry.bitCount == 0 ? 32 : entry.bitCount;
        image.bytes.assign(data.begin() + entry.offset,
                           data.begin() + entry.offset + entry.bytes);
        images.push_back(std::move(image));
    }
    error.clear();
    return true;
}

BOOL CALLBACK CollectLanguageProc(HMODULE, LPCWSTR, LPCWSTR, WORD language, LONG_PTR param) {
    reinterpret_cast<std::vector<WORD>*>(param)->push_back(language);
    return TRUE;
}

BOOL CALLBACK CollectNameProc(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
    if (IS_INTRESOURCE(name)) {
        ResourceId id;
        id.id = static_cast<WORD>(reinterpret_cast<INT_PTR>(name));
        ::EnumResourceLanguagesW(module, type, name, &CollectLanguageProc,
                                 reinterpret_cast<LONG_PTR>(&id.languages));
        reinterpret_cast<std::vector<ResourceId>*>(param)->push_back(std::move(id));
    }
    return TRUE;
}

bool CollectResources(const std::wstring& path, LPCWSTR type,
                      std::vector<ResourceId>& out, std::wstring& error) {
    out.clear();
    HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (module == nullptr) {
        error = L"cannot open " + path + L" for resource inspection: " +
                Win32Text(::GetLastError());
        return false;
    }
    const BOOL ok = ::EnumResourceNamesW(module, type, &CollectNameProc,
                                         reinterpret_cast<LONG_PTR>(&out));
    const DWORD code = ok ? ERROR_SUCCESS : ::GetLastError();
    ::FreeLibrary(module);
    if (!ok && code != ERROR_RESOURCE_TYPE_NOT_FOUND) {
        error = L"cannot enumerate resources in " + path + L": " + Win32Text(code);
        return false;
    }
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// VERSIONINFO writer
// ---------------------------------------------------------------------------

void AppendUtf16(std::vector<uint8_t>& out, const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    const size_t start = out.size();
    out.resize(start + bytes);
    std::memcpy(out.data() + start, text.c_str(), bytes);
}

void Pad4(std::vector<uint8_t>& out) {
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
}

void PatchHeader(std::vector<uint8_t>& node, uint16_t valueLength, uint16_t type) {
    const uint16_t length = static_cast<uint16_t>(node.size());
    std::memcpy(node.data(), &length, sizeof(length));
    std::memcpy(node.data() + 2, &valueLength, sizeof(valueLength));
    std::memcpy(node.data() + 4, &type, sizeof(type));
}

std::vector<uint8_t> StringNode(const wchar_t* key, const std::wstring& value) {
    std::vector<uint8_t> node(6, 0);
    AppendUtf16(node, key);
    Pad4(node);
    AppendUtf16(node, value);
    Pad4(node);
    PatchHeader(node, static_cast<uint16_t>((value.size() + 1) * sizeof(wchar_t)), 1);
    return node;
}

std::vector<uint8_t> CompositeNode(const wchar_t* key,
                                   const std::vector<std::vector<uint8_t>>& children) {
    std::vector<uint8_t> node(6, 0);
    AppendUtf16(node, key);
    Pad4(node);
    for (const std::vector<uint8_t>& child : children) {
        node.insert(node.end(), child.begin(), child.end());
    }
    Pad4(node);
    PatchHeader(node, 0, 1);
    return node;
}

std::vector<uint8_t> TranslationNode() {
    std::vector<uint8_t> node(6, 0);
    AppendUtf16(node, L"Translation");
    Pad4(node);
    const WORD language = 0x0409;
    const WORD codePage = 1200;
    const size_t start = node.size();
    node.resize(start + 4);
    std::memcpy(node.data() + start, &language, sizeof(language));
    std::memcpy(node.data() + start + 2, &codePage, sizeof(codePage));
    Pad4(node);
    PatchHeader(node, 4, 0);
    return node;
}

}  // namespace

bool ReadVersionInfo(const std::wstring& path, VersionStrings& strings, VersionFixed& fixed,
                     std::wstring& error) {
    strings = VersionStrings{};
    fixed = VersionFixed{};

    DWORD handle = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        error = L"the executable has no version information";
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        error = L"cannot read the version information: " + Win32Text(::GetLastError());
        return false;
    }

    void* value = nullptr;
    UINT valueSize = 0;
    if (::VerQueryValueW(data.data(), L"\\", &value, &valueSize) && value != nullptr &&
        valueSize >= sizeof(VS_FIXEDFILEINFO)) {
        const auto* info = static_cast<const VS_FIXEDFILEINFO*>(value);
        fixed.fileVersionMS = info->dwFileVersionMS;
        fixed.fileVersionLS = info->dwFileVersionLS;
        fixed.productVersionMS = info->dwProductVersionMS;
        fixed.productVersionLS = info->dwProductVersionLS;
    }

    WORD language = 0x0409;
    WORD codePage = 1200;
    struct LangAndCodePage {
        WORD language;
        WORD codePage;
    };
    if (::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &value, &valueSize) &&
        value != nullptr && valueSize >= sizeof(LangAndCodePage)) {
        const auto* translation = static_cast<const LangAndCodePage*>(value);
        language = translation->language;
        codePage = translation->codePage;
    }

    const auto query = [&](const wchar_t* key, std::wstring& out) {
        wchar_t pathBuffer[128] = {};
        swprintf_s(pathBuffer, L"\\StringFileInfo\\%04X%04X\\%s",
                   static_cast<unsigned>(language), static_cast<unsigned>(codePage), key);
        void* text = nullptr;
        UINT textSize = 0;
        if (!::VerQueryValueW(data.data(), pathBuffer, &text, &textSize) || text == nullptr ||
            textSize == 0) {
            return false;
        }
        out.assign(static_cast<const wchar_t*>(text));
        return true;
    };

    query(L"CompanyName", strings.company);
    query(L"FileDescription", strings.description);
    query(L"FileVersion", strings.fileVersion);
    query(L"InternalName", strings.internalName);
    query(L"LegalCopyright", strings.legalCopyright);
    query(L"OriginalFilename", strings.originalFilename);
    query(L"ProductName", strings.productName);
    query(L"ProductVersion", strings.productVersion);
    error.clear();
    return true;
}

std::vector<uint8_t> BuildVersionInfoBlob(const VersionStrings& strings,
                                          const VersionFixed& fixed) {
    std::vector<uint8_t> stringNodes;
    const auto addString = [&stringNodes](const wchar_t* key, const std::wstring& value) {
        const std::vector<uint8_t> node = StringNode(key, value);
        stringNodes.insert(stringNodes.end(), node.begin(), node.end());
    };
    addString(L"CompanyName", strings.company);
    addString(L"FileDescription", strings.description);
    addString(L"FileVersion", strings.fileVersion);
    addString(L"InternalName", strings.internalName);
    addString(L"LegalCopyright", strings.legalCopyright);
    addString(L"OriginalFilename", strings.originalFilename);
    addString(L"ProductName", strings.productName);
    addString(L"ProductVersion", strings.productVersion);

    // The string table has a fixed 6-byte header, its key and padding, then the
    // individual String nodes.  Build it manually so the table length covers
    // every child.
    std::vector<uint8_t> table(6, 0);
    AppendUtf16(table, L"040904B0");
    Pad4(table);
    table.insert(table.end(), stringNodes.begin(), stringNodes.end());
    PatchHeader(table, 0, 1);

    const std::vector<uint8_t> stringFileInfo = CompositeNode(L"StringFileInfo", {table});
    const std::vector<uint8_t> varFileInfo =
        CompositeNode(L"VarFileInfo", {TranslationNode()});

    std::vector<uint8_t> root(6, 0);
    AppendUtf16(root, L"VS_VERSION_INFO");
    Pad4(root);

    VS_FIXEDFILEINFO info = {};
    info.dwSignature = VS_FFI_SIGNATURE;
    info.dwStrucVersion = VS_FFI_STRUCVERSION;
    info.dwFileVersionMS = fixed.fileVersionMS;
    info.dwFileVersionLS = fixed.fileVersionLS;
    info.dwProductVersionMS = fixed.productVersionMS;
    info.dwProductVersionLS = fixed.productVersionLS;
    info.dwFileFlagsMask = 0x3F;
    info.dwFileFlags = 0;
    info.dwFileOS = VOS_NT_WINDOWS32;
    info.dwFileType = VFT_APP;
    const size_t fixedStart = root.size();
    root.resize(fixedStart + sizeof(info));
    std::memcpy(root.data() + fixedStart, &info, sizeof(info));
    Pad4(root);
    root.insert(root.end(), stringFileInfo.begin(), stringFileInfo.end());
    root.insert(root.end(), varFileInfo.begin(), varFileInfo.end());
    Pad4(root);
    PatchHeader(root, static_cast<uint16_t>(sizeof(VS_FIXEDFILEINFO)), 0);
    return root;
}

bool IsExecutableSigned(const std::wstring& path, std::wstring& error) {
    error.clear();

    // Check for the PE certificate table rather than asking WinVerifyTrust.  We
    // only need to know that a signature is present: patching any signed file
    // invalidates it, and the presence check works offline and without a trust
    // store lookup.
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot open " + path + L": " + Win32Text(::GetLastError());
        return false;
    }
    LARGE_INTEGER fileSize = {};
    if (!::GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 64) {
        ::CloseHandle(file);
        return false;
    }
    const size_t toRead = static_cast<size_t>(
        (std::min)(static_cast<LONGLONG>(64 * 1024), fileSize.QuadPart));
    std::vector<uint8_t> header(toRead);
    DWORD read = 0;
    const BOOL readOk = ::ReadFile(file, header.data(), static_cast<DWORD>(header.size()), &read,
                                   nullptr);
    ::CloseHandle(file);
    if (!readOk || read == 0) {
        return false;
    }
    header.resize(read);

    if (header.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    IMAGE_DOS_HEADER dos = {};
    std::memcpy(&dos, header.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }
    const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
    if (ntOffset + 4 + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD) > header.size()) {
        return false;
    }

    DWORD signature = 0;
    std::memcpy(&signature, header.data() + ntOffset, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_FILE_HEADER fileHeader = {};
    std::memcpy(&fileHeader, header.data() + ntOffset + 4, sizeof(fileHeader));

    const size_t optionalOffset = ntOffset + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0;
    std::memcpy(&magic, header.data() + optionalOffset, sizeof(magic));

    size_t dataDirectoryOffset = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        dataDirectoryOffset =
            optionalOffset + offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory);
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        dataDirectoryOffset =
            optionalOffset + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    } else {
        return false;
    }

    const size_t securityOffset =
        dataDirectoryOffset + IMAGE_DIRECTORY_ENTRY_SECURITY * sizeof(IMAGE_DATA_DIRECTORY);
    if (securityOffset + sizeof(IMAGE_DATA_DIRECTORY) > header.size()) {
        return false;
    }
    IMAGE_DATA_DIRECTORY security = {};
    std::memcpy(&security, header.data() + securityOffset, sizeof(security));
    (void)fileHeader;
    return security.Size != 0;
}

int CountIconResources(const std::wstring& path) {
    std::vector<ResourceId> ids;
    std::wstring error;
    return CollectResources(path, RT_ICON, ids, error) ? static_cast<int>(ids.size()) : 0;
}

int CountIconGroups(const std::wstring& path) {
    std::vector<ResourceId> ids;
    std::wstring error;
    return CollectResources(path, RT_GROUP_ICON, ids, error) ? static_cast<int>(ids.size()) : 0;
}

namespace {

bool ApplyBrandingResources(const std::wstring& output, const VersionStrings& strings,
                            const VersionFixed& fixed, const std::wstring& iconPath,
                            std::wstring& error) {
    std::vector<IcoImage> images;
    if (!iconPath.empty() && !LoadIco(iconPath, images, error)) {
        return false;
    }

    std::vector<ResourceId> groups;
    std::vector<ResourceId> icons;
    std::vector<ResourceId> versions;
    if (!CollectResources(output, RT_GROUP_ICON, groups, error) ||
        !CollectResources(output, RT_ICON, icons, error) ||
        !CollectResources(output, RT_VERSION, versions, error)) {
        return false;
    }

    HANDLE update = ::BeginUpdateResourceW(output.c_str(), FALSE);
    if (update == nullptr) {
        error = L"cannot open " + output + L" for patching: " + Win32Text(::GetLastError());
        return false;
    }

    bool ok = true;
    const auto remove = [&](LPCWSTR type, const std::vector<ResourceId>& ids) {
        for (const ResourceId& id : ids) {
            for (WORD language : id.languages) {
                if (!::UpdateResourceW(update, type, MAKEINTRESOURCEW(id.id), language, nullptr,
                                       0)) {
                    ok = false;
                }
            }
        }
    };
    remove(RT_GROUP_ICON, groups);
    remove(RT_ICON, icons);
    remove(RT_VERSION, versions);

    if (ok && !images.empty()) {
        const WORD groupId = groups.empty() ? 101 : groups.front().id;

        std::vector<GrpIconEntry> entries;
        entries.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i) {
            const WORD iconId = static_cast<WORD>(i + 1);
            if (!::UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(iconId), 0,
                                   images[i].bytes.data(),
                                   static_cast<DWORD>(images[i].bytes.size()))) {
                ok = false;
                break;
            }
            GrpIconEntry entry = {};
            entry.width = images[i].width >= 256 ? 0 : static_cast<BYTE>(images[i].width);
            entry.height = images[i].height >= 256 ? 0 : static_cast<BYTE>(images[i].height);
            entry.colorCount = static_cast<BYTE>(images[i].colorCount);
            entry.planes = images[i].planes;
            entry.bitCount = images[i].bitCount;
            entry.bytes = static_cast<DWORD>(images[i].bytes.size());
            entry.id = iconId;
            entries.push_back(entry);
        }

        if (ok) {
            GrpIconDir directory = {};
            directory.reserved = 0;
            directory.type = 1;
            directory.count = static_cast<WORD>(entries.size());
            std::vector<uint8_t> group(sizeof(directory) + entries.size() * sizeof(GrpIconEntry));
            std::memcpy(group.data(), &directory, sizeof(directory));
            std::memcpy(group.data() + sizeof(directory), entries.data(),
                        entries.size() * sizeof(GrpIconEntry));
            if (!::UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(groupId), 0,
                                   group.data(), static_cast<DWORD>(group.size()))) {
                ok = false;
            }
        }
    }

    if (ok) {
        const std::vector<uint8_t> blob = BuildVersionInfoBlob(strings, fixed);
        if (!::UpdateResourceW(update, RT_VERSION, MAKEINTRESOURCEW(1), 0x0409,
                               const_cast<uint8_t*>(blob.data()),
                               static_cast<DWORD>(blob.size()))) {
            ok = false;
        }
    }

    const DWORD commitError = ok ? ERROR_SUCCESS : ::GetLastError();
    if (!::EndUpdateResourceW(update, ok ? FALSE : TRUE)) {
        error = L"cannot write the branded resources: " + Win32Text(::GetLastError());
        return false;
    }
    if (!ok) {
        error = L"cannot replace an executable resource: " + Win32Text(commitError);
        return false;
    }
    error.clear();
    return true;
}

}  // namespace

BrandStatus BrandExecutable(const BrandOptions& options, std::wstring& error) {
    if (options.inputPath.empty() || options.outputPath.empty()) {
        error = L"an input and an output path are required";
        return BrandStatus::InvalidArguments;
    }
    if (_wcsicmp(FullPath(options.inputPath).c_str(),
                 FullPath(options.outputPath).c_str()) == 0) {
        error = L"the input and output must be different files";
        return BrandStatus::InvalidArguments;
    }
    if (!FileExists(options.inputPath)) {
        error = L"the input executable does not exist: " + options.inputPath;
        return BrandStatus::IoError;
    }
    if (!options.iconPath.empty() && !FileExists(options.iconPath)) {
        error = L"the icon file does not exist: " + options.iconPath;
        return BrandStatus::IconError;
    }

    std::wstring signatureError;
    const bool signedInput = IsExecutableSigned(options.inputPath, signatureError);
    if (signedInput && !options.force) {
        error = L"the base executable is Authenticode-signed, and patching it would "
                L"invalidate the signature. Re-run with --force to override.";
        return BrandStatus::SignedRefused;
    }

    // Validate the icon before creating the output.
    {
        std::vector<IcoImage> probe;
        if (!options.iconPath.empty() && !LoadIco(options.iconPath, probe, error)) {
            return BrandStatus::IconError;
        }
    }

    const std::wstring directory = DirectoryOf(options.outputPath);
    if (!directory.empty() &&
        !::CreateDirectoryW(directory.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"cannot create " + directory + L": " + Win32Text(::GetLastError());
        return BrandStatus::IoError;
    }

    if (!::CopyFileW(options.inputPath.c_str(), options.outputPath.c_str(), FALSE)) {
        error = L"cannot copy the base executable: " + Win32Text(::GetLastError());
        return BrandStatus::IoError;
    }
    ::SetFileAttributesW(options.outputPath.c_str(), FILE_ATTRIBUTE_NORMAL);

    VersionStrings strings;
    VersionFixed fixed;
    std::wstring readError;
    if (!ReadVersionInfo(options.outputPath, strings, fixed, readError)) {
        strings.fileVersion = L"1.0.0.0";
        strings.productVersion = L"1.0.0.0";
    }
    if (strings.fileVersion.empty()) {
        strings.fileVersion = L"1.0.0.0";
    }
    if (strings.productVersion.empty()) {
        strings.productVersion = strings.fileVersion;
    }

    const std::wstring outputName = FileNameOf(options.outputPath);
    const std::wstring baseProduct = strings.productName;
    strings.internalName = outputName;
    strings.originalFilename = outputName;
    if (!options.name.empty()) {
        strings.productName = options.name;
    }
    if (strings.productName.empty()) {
        strings.productName = options.name;
    }
    const bool productChanged =
        !options.name.empty() && _wcsicmp(baseProduct.c_str(), options.name.c_str()) != 0;

    if (!options.description.empty()) {
        strings.description = options.description;
    } else if (productChanged || strings.description.empty()) {
        strings.description = strings.productName + L" - window capture exclusion utility";
    }

    if (!options.company.empty()) {
        strings.company = options.company;
    } else if (productChanged) {
        strings.company = strings.productName;
    }
    if (strings.company.empty()) {
        strings.company = strings.productName.empty() ? outputName : strings.productName;
    }

    if (!ApplyBrandingResources(options.outputPath, strings, fixed, options.iconPath, error)) {
        ::DeleteFileW(options.outputPath.c_str());
        return BrandStatus::ResourceError;
    }

    error.clear();
    return BrandStatus::Ok;
}

}  // namespace gf