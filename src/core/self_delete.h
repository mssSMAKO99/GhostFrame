// Detached cleanup helper and the shared delete operation.
//
// Self-delete cannot remove the executable that is running it, so the launcher
// copies itself to %TEMP%, starts that copy with `--self-delete-helper` and
// exits.  The helper waits (bounded) for the UI and the daemon, then removes the
// files, shortcuts and startup entry selected by the mode.  It retries deletes
// so a process that has just exited cannot make the cleanup fail permanently.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "self_delete_plan.h"

namespace gf {

// What the UI (or the daemon) asks StartSelfDeleteHelper to do.
struct SelfDeleteStartRequest {
    SelfDeleteMode mode = SelfDeleteMode::Never;
    std::wstring executablePath;
    std::wstring dataDirectory;
    // Processes that must exit before deletion: the caller and the daemon.
    std::vector<DWORD> waitPids;
    // Run value names to remove.  Usually the executable stem plus the branded
    // `autostartValue` from branding.json.
    std::vector<std::wstring> autostartValues;
    // Extra links recorded outside shortcuts.json (the installer's Start Menu
    // shortcut in branding.json).  Only `.lnk` files are accepted.
    std::vector<std::wstring> extraFiles;
};

// Command line of the helper copy.
struct SelfDeleteHelperOptions {
    SelfDeleteMode mode = SelfDeleteMode::Never;
    std::wstring executable;
    std::wstring dataDirectory;
    std::vector<DWORD> waitPids;
    std::vector<std::wstring> autostartValues;
    std::vector<std::wstring> extraFiles;
    DWORD waitTimeoutMs = 15000;
};

// Parses `--self-delete-helper --mode <never|exe|everything> ...`.  Returns
// true when the helper flag is present.  A malformed argument sets `error`.
bool ParseSelfDeleteHelperArguments(int argc, wchar_t** argv, SelfDeleteHelperOptions& out,
                                    std::wstring& error);

// Body of the detached helper.  Returns 0 when everything was removed.
int RunSelfDeleteHelper(const SelfDeleteHelperOptions& options);

// Copies the running executable to %TEMP% and starts the helper detached.
bool StartSelfDeleteHelper(const SelfDeleteStartRequest& request, DWORD& helperPid,
                           std::wstring& error);

// PID of the daemon message window, or 0 when no daemon is running.
DWORD FindDaemonProcessId();

// Asks the daemon to shut down and restore its targets.  Best effort: a missing
// daemon is not an error.
bool StopDaemonForSelfDelete(std::wstring& error);

// Deletes one file, retrying while another process releases it.  A missing file
// is success.
bool DeleteFileWithRetry(const std::wstring& path, DWORD timeoutMs);

// Removes a directory tree, retrying files that are briefly held open.
bool RemoveDirectoryTreeWithRetry(const std::wstring& path, DWORD timeoutMs);

// The log the helper writes when something could not be removed.
std::wstring SelfDeleteLogPath();

}  // namespace gf