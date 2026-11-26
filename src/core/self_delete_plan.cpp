#include "self_delete_plan.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace gf {

namespace {

std::wstring DirectoryOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring TrimTrailingSlashes(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

std::wstring FileNameOf(const std::wstring& path) {
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                  static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::wstring();
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

void AddUnique(std::vector<std::wstring>& list, const std::wstring& value) {
    if (value.empty()) {
        return;
    }
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

}  // namespace

SelfDeletePaths DefaultSelfDeletePaths() {
    SelfDeletePaths paths;
    paths.executable = ModulePath();
    paths.dataDirectory = SettingsDirectory();
    paths.installDirectory = DirectoryOf(paths.executable);
    paths.generatedIcon = paths.dataDirectory.empty() ? std::wstring()
                                                      : paths.dataDirectory + L"\\GhostFrame.ico";
    return paths;
}

SelfDeletePlan BuildSelfDeletePlan(SelfDeleteMode mode, const SelfDeletePaths& paths) {
    SelfDeletePlan plan;
    plan.mode = mode;
    if (mode == SelfDeleteMode::Never || mode == SelfDeleteMode::Count) {
        plan.mode = SelfDeleteMode::Never;
        return plan;
    }

    plan.removeShortcuts = true;
    plan.removeAutostart = true;
    plan.executable = paths.executable;

    // The generated product icon lives in the data directory.  It is removed in
    // both modes because a shortcut/tray never points at it again once the
    // executable is gone.
    AddUnique(plan.files, paths.generatedIcon);

    if (mode == SelfDeleteMode::Everything) {
        // The recursive data-directory removal already covers the icon; keep the
        // plan free of a duplicate that would be deleted twice.
        plan.files.erase(
            std::remove(plan.files.begin(), plan.files.end(), paths.generatedIcon),
            plan.files.end());
        AddUnique(plan.directories, TrimTrailingSlashes(paths.dataDirectory));
        AddUnique(plan.emptyDirectories, TrimTrailingSlashes(paths.installDirectory));
    } else {
        // ExeOnly deliberately keeps settings.json, the logs and the manifests.
        // The install directory is left alone because it still holds data only
        // in the user's imagination: the executable is the only file we created.
    }

    return plan;
}

bool IsSafeDataDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    const std::wstring trimmed = TrimTrailingSlashes(path);
    if (trimmed.size() <= 3) {
        return false;  // "C:\" and friends
    }
    if (trimmed.find(L"..") != std::wstring::npos) {
        return false;
    }
    const std::wstring leaf = FileNameOf(trimmed);
    return _wcsicmp(leaf.c_str(), L"GhostFrame") == 0;
}

}  // namespace gf