#include "pipe_io.h"

namespace gf {

namespace {

bool ReadAll(HANDLE pipe, void* buffer, DWORD size) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    DWORD total = 0;
    while (total < size) {
        DWORD got = 0;
        if (!::ReadFile(pipe, cursor + total, size - total, &got, nullptr) || got == 0) {
            return false;
        }
        total += got;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void* buffer, DWORD size) {
    const auto* cursor = static_cast<const uint8_t*>(buffer);
    DWORD total = 0;
    while (total < size) {
        DWORD wrote = 0;
        if (!::WriteFile(pipe, cursor + total, size - total, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        total += wrote;
    }
    return true;
}

}  // namespace

bool ReadFrame(HANDLE pipe, IpcFrame& frame) {
    IpcHeader header = {};
    if (!ReadAll(pipe, &header, sizeof(header))) {
        return false;
    }
    if (header.magic != kIpcMagic || header.version != kIpcVersion ||
        header.size > kIpcMaxPayload) {
        return false;
    }

    frame.type = static_cast<IpcType>(header.type);
    frame.payload.resize(header.size);
    if (header.size > 0 && !ReadAll(pipe, frame.payload.data(), header.size)) {
        frame.payload.clear();
        return false;
    }
    return true;
}

bool WriteFrame(HANDLE pipe, const IpcFrame& frame) {
    if (frame.payload.size() > kIpcMaxPayload) {
        return false;
    }

    IpcHeader header = {};
    header.magic = kIpcMagic;
    header.version = kIpcVersion;
    header.type = static_cast<uint32_t>(frame.type);
    header.size = static_cast<uint32_t>(frame.payload.size());

    if (!WriteAll(pipe, &header, sizeof(header))) {
        return false;
    }
    if (!frame.payload.empty() &&
        !WriteAll(pipe, frame.payload.data(), static_cast<DWORD>(frame.payload.size()))) {
        return false;
    }
    return true;
}

bool SetTextPayload(IpcFrame& frame, const std::wstring& text) {
    TextPayload payload = {};
    if (text.size() >= kMaxTextChars) {
        return false;
    }
    payload.length = static_cast<uint32_t>(text.size());
    if (!text.empty()) {
        std::wmemcpy(payload.text, text.c_str(), text.size());
    }
    SetPayload(frame, payload);
    return true;
}

bool GetTextPayload(const IpcFrame& frame, std::wstring& text) {
    TextPayload payload = {};
    if (!GetPayload(frame, payload) || payload.length >= kMaxTextChars) {
        return false;
    }
    text.assign(payload.text, payload.text + payload.length);
    return true;
}

}  // namespace gf
