// Pure self-delete planner.
//
// The UI and the detached cleanup helper both need to agree on exactly what a
// mode removes.  Keeping that decision free of Win32 calls makes it testable in
// the settings harness: given a mode and a set of paths it returns the files,
// directories and shell objects to remove.
#pragma once

#include <string>
#include <vector>

#include "settings.h"

namespace gf {

// The paths a self-delete can touch.  `dataDirectory` is
// %LOCALAPPDATA%\GhostFrame (or its temporary fallback); `installDirectory` is
// the directory that holds the executable.
struct SelfDeletePaths {
    std::wstring executable;
    std::wstring dataDirectory;
    std::wstring installDirectory;
    std::wstring generatedIcon;  // dataDirectory\GhostFrame.ico
};

// The paths derived from the running executable and the settings directory.
SelfDeletePaths DefaultSelfDeletePaths();

struct SelfDeletePlan {
    SelfDeleteMode mode = SelfDeleteMode::Never;

    // Shell objects that would be left broken by removing the executable.
    bool removeShortcuts = false;
    bool removeAutostart = false;

    // Individual files, deleted before the executable.
    std::vector<std::wstring> files;

    // Directories whose whole contents are removed (Everything).
    std::vector<std::wstring> directories;

    // Directories removed only when they are already empty (the install
    // directory after the executable is gone).
    std::vector<std::wstring> emptyDirectories;

    // Deleted last; it is the file this process was started from.
    std::wstring executable;
};

SelfDeletePlan BuildSelfDeletePlan(SelfDeleteMode mode, const SelfDeletePaths& paths);

// True for a data directory that is safe to remove recursively.  It must be a
// real path below LOCALAPPDATA or TEMP whose final component is GhostFrame.
// This is the guard against a corrupted settings document naming `C:\`.
bool IsSafeDataDirectory(const std::wstring& path);

}  // namespace gf