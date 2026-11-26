#include "autostart.h"

#include <cwchar>

namespace gf {

namespace {

std::wstring ModulePath() {
    wchar_t module[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) {
        return std::wstring();
    }
    return module;
}

}  // namespace

std::wstring AutostartValueName() {
    const std::wstring module = ModulePath();
    if (module.empty()) {
        return L"GhostFrame";
    }
    const std::wstring::size_type slash = module.find_last_of(L"\\/");
    std::wstring name =
        slash == std::wstring::npos ? module : module.substr(slash + 1);
    const std::wstring::size_type dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name.resize(dot);
    }
    return name.empty() ? L"GhostFrame" : name;
}

std::wstring AutostartCommand() {
    const std::wstring module = ModulePath();
    if (module.empty()) {
        return std::wstring();
    }
    return L"\"" + module + L"\" --daemon";
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kAutostartKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    wchar_t buffer[1024] = {};
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    const std::wstring valueName = AutostartValueName();
    const LONG status =
        ::RegQueryValueExW(key, valueName.c_str(), nullptr, &type,
                           reinterpret_cast<BYTE*>(buffer), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }

    // Only count a value that points at this executable: a stale entry left by
    // an older copy must not look enabled.
    const std::wstring module = ModulePath();
    if (module.empty()) {
        return false;
    }
    return _wcsicmp(buffer, (L"\"" + module + L"\" --daemon").c_str()) == 0;
}

bool SetAutostartEnabled(bool enabled) {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kAutostartKey, 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG status = ERROR_SUCCESS;
    const std::wstring valueName = AutostartValueName();
    if (enabled) {
        const std::wstring command = AutostartCommand();
        if (command.empty()) {
            status = ERROR_FILE_NOT_FOUND;
        } else {
            status = ::RegSetValueExW(
                key, valueName.c_str(), 0, REG_SZ,
                reinterpret_cast<const BYTE*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        status = ::RegDeleteValueW(key, valueName.c_str());
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }

    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace gf