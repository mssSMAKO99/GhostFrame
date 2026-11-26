#include "emoji_icon.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

#include "settings.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace gf {

namespace {

constexpr int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256};
constexpr int kIconCount = static_cast<int>(sizeof(kIconSizes) / sizeof(kIconSizes[0]));

struct RenderedImage {
    int size = 0;
    // BGRA, straight alpha, top-down.
    std::vector<uint8_t> pixels;
};

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

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

std::wstring DirectoryOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, slash);
}

void EnsureDirectory(const std::wstring& path) {
    const std::wstring directory = DirectoryOf(path);
    if (!directory.empty()) {
        ::CreateDirectoryW(directory.c_str(), nullptr);
    }
}

bool WriteAll(HANDLE file, const void* data, size_t size) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(size, static_cast<size_t>(1u << 20)));
        if (!::WriteFile(file, cursor, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

bool WriteIco(const std::wstring& path, const std::vector<RenderedImage>& images,
              std::wstring& error) {
    if (images.empty()) {
        error = L"no icon images were rendered";
        return false;
    }

#pragma pack(push, 1)
    struct IcoDirectory {
        WORD reserved;
        WORD type;
        WORD count;
    };
    struct IcoEntry {
        BYTE width;
        BYTE height;
        BYTE colorCount;
        BYTE reserved;
        WORD planes;
        WORD bitCount;
        DWORD bytes;
        DWORD offset;
    };
#pragma pack(pop)

    std::vector<std::vector<uint8_t>> payloads;
    payloads.reserve(images.size());
    for (const RenderedImage& image : images) {
        const int size = image.size;
        if (size <= 0 || size > 256 ||
            image.pixels.size() != static_cast<size_t>(size) * static_cast<size_t>(size) * 4) {
            error = L"an icon image has an invalid size";
            return false;
        }
        const int maskStride = ((size + 31) / 32) * 4;
        const size_t pixelBytes = static_cast<size_t>(size) * static_cast<size_t>(size) * 4;
        const size_t maskBytes = static_cast<size_t>(maskStride) * static_cast<size_t>(size);
        std::vector<uint8_t>& payload = payloads.emplace_back();
        payload.resize(sizeof(BITMAPINFOHEADER) + pixelBytes + maskBytes, 0);

        BITMAPINFOHEADER header = {};
        header.biSize = sizeof(BITMAPINFOHEADER);
        header.biWidth = size;
        header.biHeight = size * 2;  // XOR image + AND mask
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        header.biSizeImage = static_cast<DWORD>(pixelBytes + maskBytes);
        std::memcpy(payload.data(), &header, sizeof(header));

        uint8_t* pixels = payload.data() + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < size; ++y) {
            const int sourceRow = size - 1 - y;  // bottom-up in the file
            std::memcpy(pixels + static_cast<size_t>(y) * size * 4,
                        image.pixels.data() + static_cast<size_t>(sourceRow) * size * 4,
                        static_cast<size_t>(size) * 4);
        }

        uint8_t* mask = pixels + pixelBytes;
        for (int y = 0; y < size; ++y) {
            const int sourceRow = size - 1 - y;
            uint8_t* row = mask + static_cast<size_t>(y) * maskStride;
            for (int x = 0; x < size; ++x) {
                const uint8_t alpha =
                    image.pixels[(static_cast<size_t>(sourceRow) * size + x) * 4 + 3];
                if (alpha < 128) {
                    row[x / 8] |= static_cast<uint8_t>(0x80u >> (x % 8));
                }
            }
        }
    }

    IcoDirectory directory = {};
    directory.reserved = 0;
    directory.type = 1;
    directory.count = static_cast<WORD>(payloads.size());

    std::vector<IcoEntry> entries(payloads.size());
    DWORD offset = static_cast<DWORD>(sizeof(IcoDirectory) + entries.size() * sizeof(IcoEntry));
    for (size_t i = 0; i < payloads.size(); ++i) {
        IcoEntry& entry = entries[i];
        const int size = images[i].size;
        entry.width = size >= 256 ? 0 : static_cast<BYTE>(size);
        entry.height = size >= 256 ? 0 : static_cast<BYTE>(size);
        entry.colorCount = 0;
        entry.reserved = 0;
        entry.planes = 1;
        entry.bitCount = 32;
        entry.bytes = static_cast<DWORD>(payloads[i].size());
        entry.offset = offset;
        offset += entry.bytes;
    }

    EnsureDirectory(path);
    const std::wstring temporary = path + L".tmp";
    HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot create " + temporary + L": " + Win32Text(::GetLastError());
        return false;
    }

    bool ok = WriteAll(file, &directory, sizeof(directory));
    if (ok) {
        ok = WriteAll(file, entries.data(), entries.size() * sizeof(IcoEntry));
    }
    for (size_t i = 0; ok && i < payloads.size(); ++i) {
        ok = WriteAll(file, payloads[i].data(), payloads[i].size());
    }
    ::CloseHandle(file);

    if (!ok) {
        error = L"cannot write " + temporary + L": " + Win32Text(::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"cannot replace " + path + L": " + Win32Text(::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::vector<uint8_t> ReadEntireFile(const std::wstring& path, std::wstring& error) {
    std::vector<uint8_t> data;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"cannot open " + path + L": " + Win32Text(::GetLastError());
        return data;
    }
    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64ll << 20)) {
        ::CloseHandle(file);
        error = L"invalid icon file size";
        return data;
    }
    data.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < data.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - done, static_cast<size_t>(1u << 20)));
        if (!::ReadFile(file, data.data() + done, chunk, &read, nullptr) || read == 0) {
            break;
        }
        done += read;
    }
    ::CloseHandle(file);
    if (done != data.size()) {
        error = L"cannot read " + path;
        data.clear();
    }
    return data;
}

// ---------------------------------------------------------------------------
// Direct2D colour-glyph renderer
// ---------------------------------------------------------------------------

class ComApartment {
public:
    ComApartment() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitialize_ = SUCCEEDED(hr);
    }
    ~ComApartment() {
        if (uninitialize_) {
            ::CoUninitialize();
        }
    }

private:
    bool uninitialize_ = false;
};

bool RenderWithDirect2D(const std::wstring& emoji, std::vector<RenderedImage>& images,
                        std::wstring& error) {
    ComApartment apartment;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deviceContext;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                     static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                     D3D11_SDK_VERSION, &device, &featureLevel, &deviceContext);
    if (FAILED(hr)) {
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                 static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                 D3D11_SDK_VERSION, &device, &featureLevel, &deviceContext);
    }
    if (FAILED(hr)) {
        error = L"no Direct3D device is available";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.As(&dxgiDevice))) {
        error = L"Direct3D is not backed by DXGI";
        return false;
    }

    ComPtr<ID2D1Factory1> factory;
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                             reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        error = L"cannot create the Direct2D factory";
        return false;
    }

    ComPtr<ID2D1Device> d2dDevice;
    if (FAILED(factory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) {
        error = L"cannot create the Direct2D device";
        return false;
    }
    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) {
        error = L"cannot create the Direct2D device context";
        return false;
    }
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    ComPtr<IDWriteFactory> writeFactory;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(writeFactory.GetAddressOf())))) {
        error = L"cannot create the DirectWrite factory";
        return false;
    }

    const D2D1_COLOR_F clear = {0.0f, 0.0f, 0.0f, 0.0f};
    images.clear();
    images.reserve(kIconCount);
    for (int index = 0; index < kIconCount; ++index) {
        const int size = kIconSizes[index];
        const FLOAT fontSize = static_cast<FLOAT>(size) * 0.80f;

        ComPtr<IDWriteTextFormat> format;
        if (FAILED(writeFactory->CreateTextFormat(L"Segoe UI Emoji", nullptr,
                                                  DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, fontSize, L"",
                                                  &format))) {
            error = L"cannot create the emoji text format";
            return false;
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(writeFactory->CreateTextLayout(emoji.c_str(), static_cast<UINT32>(emoji.size()),
                                                  format.Get(), static_cast<FLOAT>(size),
                                                  static_cast<FLOAT>(size), &layout))) {
            error = L"cannot lay out the emoji";
            return false;
        }

        D3D11_TEXTURE2D_DESC description = {};
        description.Width = static_cast<UINT>(size);
        description.Height = static_cast<UINT>(size);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(device->CreateTexture2D(&description, nullptr, &texture))) {
            error = L"cannot create the icon render target";
            return false;
        }
        ComPtr<IDXGISurface> surface;
        if (FAILED(texture.As(&surface))) {
            error = L"cannot access the icon render target";
            return false;
        }

        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> target;
        if (FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &target))) {
            error = L"cannot bind the Direct2D target";
            return false;
        }

        ComPtr<ID2D1SolidColorBrush> brush;
        const D2D1_COLOR_F accent = {0.184f, 0.886f, 0.482f, 1.0f};
        if (FAILED(context->CreateSolidColorBrush(accent, &brush))) {
            error = L"cannot create the emoji brush";
            return false;
        }

        context->SetTarget(target.Get());
        context->BeginDraw();
        context->Clear(&clear);
        context->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), layout.Get(), brush.Get(),
                                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT |
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
        hr = context->EndDraw();
        context->SetTarget(nullptr);
        if (FAILED(hr)) {
            error = L"Direct2D could not draw the emoji";
            return false;
        }

        D3D11_TEXTURE2D_DESC stagingDescription = description;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging))) {
            error = L"cannot create the icon readback texture";
            return false;
        }
        deviceContext->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(deviceContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            error = L"cannot read the icon pixels";
            return false;
        }

        RenderedImage image;
        image.size = size;
        image.pixels.resize(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            const uint8_t* source =
                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* destination = image.pixels.data() + static_cast<size_t>(y) * size * 4;
            for (int x = 0; x < size; ++x) {
                const uint8_t alpha = source[x * 4 + 3];
                const uint8_t blue = source[x * 4 + 0];
                const uint8_t green = source[x * 4 + 1];
                const uint8_t red = source[x * 4 + 2];
                if (alpha == 0) {
                    destination[x * 4 + 0] = 0;
                    destination[x * 4 + 1] = 0;
                    destination[x * 4 + 2] = 0;
                    destination[x * 4 + 3] = 0;
                    continue;
                }
                // Direct2D stores premultiplied alpha; ICO expects straight alpha.
                const auto unpremultiply = [alpha](uint8_t value) {
                    return static_cast<uint8_t>((std::min)(255u, static_cast<unsigned>(value) * 255u / alpha));
                };
                destination[x * 4 + 0] = unpremultiply(blue);
                destination[x * 4 + 1] = unpremultiply(green);
                destination[x * 4 + 2] = unpremultiply(red);
                destination[x * 4 + 3] = alpha;
            }
        }
        deviceContext->Unmap(staging.Get(), 0);
        images.push_back(std::move(image));
    }
    return true;
}

// ---------------------------------------------------------------------------
// GDI fallback: a monochrome emoji silhouette in the GhostFrame accent colour.
// ---------------------------------------------------------------------------

bool RenderWithGdi(const std::wstring& emoji, std::vector<RenderedImage>& images) {
    images.clear();
    images.reserve(kIconCount);
    for (int index = 0; index < kIconCount; ++index) {
        const int size = kIconSizes[index];

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = ::CreateCompatibleDC(nullptr);
        if (bitmap == nullptr || bits == nullptr || dc == nullptr) {
            if (bitmap != nullptr) {
                ::DeleteObject(bitmap);
            }
            if (dc != nullptr) {
                ::DeleteDC(dc);
            }
            return false;
        }
        HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);

        RECT rect = {0, 0, size, size};
        HBRUSH white = ::CreateSolidBrush(RGB(255, 255, 255));
        ::FillRect(dc, &rect, white);
        ::DeleteObject(white);

        LOGFONTW font = {};
        font.lfHeight = -static_cast<LONG>(size * 0.80f);
        font.lfWeight = FW_NORMAL;
        wcscpy_s(font.lfFaceName, L"Segoe UI Emoji");
        HFONT textFont = ::CreateFontIndirectW(&font);
        HGDIOBJ oldFont = ::SelectObject(dc, textFont);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(0, 0, 0));
        ::DrawTextW(dc, emoji.c_str(), static_cast<int>(emoji.size()), &rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(dc, oldFont);
        ::DeleteObject(textFont);

        ::SelectObject(dc, oldBitmap);
        ::DeleteDC(dc);

        RenderedImage image;
        image.size = size;
        image.pixels.resize(static_cast<size_t>(size) * size * 4);
        const uint8_t* source = static_cast<const uint8_t*>(bits);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const size_t offset = (static_cast<size_t>(y) * size + x) * 4;
                const uint8_t blue = source[offset + 0];
                const uint8_t green = source[offset + 1];
                const uint8_t red = source[offset + 2];
                const uint8_t luminance = static_cast<uint8_t>(
                    (static_cast<unsigned>(red) * 299 + static_cast<unsigned>(green) * 587 +
                     static_cast<unsigned>(blue) * 114) /
                    1000);
                const uint8_t alpha = luminance < 8 ? 0 : static_cast<uint8_t>(255 - luminance);
                image.pixels[offset + 0] = alpha == 0 ? 0 : 123;  // B
                image.pixels[offset + 1] = alpha == 0 ? 0 : 226;  // G
                image.pixels[offset + 2] = alpha == 0 ? 0 : 47;   // R
                image.pixels[offset + 3] = alpha;
            }
        }
        ::DeleteObject(bitmap);
        images.push_back(std::move(image));
    }
    return true;
}

}  // namespace

std::wstring EmojiIconPath() { return SettingsDirectory() + L"\\GhostFrame.ico"; }

bool RenderEmojiIcon(const std::wstring& emoji, const std::wstring& path, std::wstring& error) {
    const std::wstring normalized = NormalizeEmoji(emoji);
    if (normalized.empty()) {
        error = L"the emoji is empty";
        return false;
    }

    std::vector<RenderedImage> images;
    std::wstring renderError;
    if (!RenderWithDirect2D(normalized, images, renderError)) {
        if (!RenderWithGdi(normalized, images)) {
            error = renderError.empty() ? L"cannot render the emoji" : renderError;
            return false;
        }
    }
    if (!WriteIco(path, images, error)) {
        return false;
    }
    return true;
}

bool EnsureEmojiIcon(const std::wstring& emoji, std::wstring& error) {
    static std::wstring lastEmoji;
    const std::wstring normalized = NormalizeEmoji(emoji);
    const std::wstring path = EmojiIconPath();
    if (normalized.empty()) {
        lastEmoji.clear();
        if (FileExists(path)) {
            ::DeleteFileW(path.c_str());
        }
        error.clear();
        return true;
    }
    if (normalized == lastEmoji && FileExists(path)) {
        error.clear();
        return true;
    }
    if (!RenderEmojiIcon(normalized, path, error)) {
        return false;
    }
    lastEmoji = normalized;
    return true;
}

HICON LoadConfiguredIcon(HMODULE module, int resourceId, const std::wstring& emoji, int size,
                         bool& owned) {
    owned = false;
    if (!NormalizeEmoji(emoji).empty()) {
        const HICON icon = static_cast<HICON>(::LoadImageW(
            nullptr, EmojiIconPath().c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        if (icon != nullptr) {
            owned = true;
            return icon;
        }
    }
    HICON icon = nullptr;
    if (module != nullptr && resourceId != 0) {
        icon = ::LoadIconW(module, MAKEINTRESOURCEW(resourceId));
    }
    if (icon == nullptr) {
        icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

bool IsValidIcoFile(const std::wstring& path, std::wstring& error) {
    std::wstring readError;
    const std::vector<uint8_t> data = ReadEntireFile(path, readError);
    if (data.empty()) {
        error = readError;
        return false;
    }
    if (data.size() < 6) {
        error = L"the icon header is truncated";
        return false;
    }
    const uint16_t reserved = static_cast<uint16_t>(data[0] | (data[1] << 8));
    const uint16_t type = static_cast<uint16_t>(data[2] | (data[3] << 8));
    const uint16_t count = static_cast<uint16_t>(data[4] | (data[5] << 8));
    if (reserved != 0 || type != 1 || count == 0 || count > 16) {
        error = L"the icon directory header is invalid";
        return false;
    }
    if (data.size() < 6 + static_cast<size_t>(count) * 16) {
        error = L"the icon directory is truncated";
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = data.data() + 6 + static_cast<size_t>(i) * 16;
        const uint16_t planes = static_cast<uint16_t>(entry[4] | (entry[5] << 8));
        const uint16_t bitCount = static_cast<uint16_t>(entry[6] | (entry[7] << 8));
        const uint32_t bytes =
            static_cast<uint32_t>(entry[8] | (entry[9] << 8) | (entry[10] << 16) |
                                  (static_cast<uint32_t>(entry[11]) << 24));
        const uint32_t offset =
            static_cast<uint32_t>(entry[12] | (entry[13] << 8) | (entry[14] << 16) |
                                  (static_cast<uint32_t>(entry[15]) << 24));
        if (planes != 1 || bitCount != 32 || bytes < sizeof(BITMAPINFOHEADER) ||
            static_cast<uint64_t>(offset) + bytes > data.size()) {
            error = L"an icon directory entry is invalid";
            return false;
        }
        const BITMAPINFOHEADER* header =
            reinterpret_cast<const BITMAPINFOHEADER*>(data.data() + offset);
        if (header->biSize != sizeof(BITMAPINFOHEADER) || header->biWidth <= 0 ||
            header->biWidth > 256 || header->biBitCount != 32) {
            error = L"an icon image header is invalid";
            return false;
        }
    }
    error.clear();
    return true;
}

int CountIcoImages(const std::wstring& path) {
    std::wstring error;
    if (!IsValidIcoFile(path, error)) {
        return 0;
    }
    std::wstring readError;
    const std::vector<uint8_t> data = ReadEntireFile(path, readError);
    if (data.size() < 6) {
        return 0;
    }
    return static_cast<int>(data[4] | (data[5] << 8));
}

}  // namespace gf