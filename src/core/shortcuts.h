// Shell shortcut manager.
//
// Every link GhostFrame creates is recorded in
// %LOCALAPPDATA%\GhostFrame\shortcuts.json so Phase 8 can remove them during
// self-delete.  Links are ordinary .lnk files written with IShellLinkW.
#pragma once

#include <string>
#include <vector>

namespace gf {

enum class ShortcutKind { Ui = 0, Daemon, AppWrapper, Count };
enum class ShortcutLocation { Desktop = 0, StartMenu, Startup, Count };

const wchar_t* ShortcutKindKey(ShortcutKind kind);
const wchar_t* ShortcutKindLabel(ShortcutKind kind);
ShortcutKind ShortcutKindFromKey(const std::wstring& key);

const wchar_t* ShortcutLocationKey(ShortcutLocation location);
const wchar_t* ShortcutLocationLabel(ShortcutLocation location);
ShortcutLocation ShortcutLocationFromKey(const std::wstring& key);

// The shell folder for a destination, or an empty string when it cannot be
// resolved.
std::wstring ShortcutLocationPath(ShortcutLocation location);

// The full path of the running executable (the target of every link).
std::wstring ExecutablePath();

std::wstring ShortcutManifestPath();

struct ShortcutSpec {
    ShortcutKind kind = ShortcutKind::Ui;
    ShortcutLocation location = ShortcutLocation::Desktop;
    std::wstring name;
    std::wstring description;
    std::wstring arguments;
    std::wstring workingDirectory;
};

struct ShortcutEntry {
    std::wstring path;
    std::wstring kind;  // ShortcutKindKey
    std::wstring name;
    std::wstring target;
    std::wstring arguments;
    std::wstring created;  // local ISO-8601
};

const wchar_t* ShortcutKindDefaultName(ShortcutKind kind);
// `--launch "<app>"` plus optional extra flags (for AppWrapper).
std::wstring ShortcutWrapperArguments(const std::wstring& appPath, const std::wstring& extra);

// Turns a user-visible name into a safe file stem.  Empty input becomes
// "GhostFrame".
std::wstring ShortcutFileStem(const std::wstring& name);

// Creates a link at an explicit path; used by the view and by the tests.
bool CreateShortcutAt(const std::wstring& linkPath, const ShortcutSpec& spec,
                      const std::wstring& exePath, const std::wstring& iconPath,
                      ShortcutEntry& out, std::wstring& error);

// Resolves the destination folder and builds <name>.lnk inside it.
bool CreateShortcut(const ShortcutSpec& spec, const std::wstring& iconPath, ShortcutEntry& out,
                    std::wstring& error);

bool DeleteShortcutFile(const std::wstring& path, std::wstring& error);
bool ShortcutExists(const std::wstring& path);

std::wstring SerializeShortcutManifest(const std::vector<ShortcutEntry>& entries);
bool ParseShortcutManifest(const std::wstring& json, std::vector<ShortcutEntry>& out,
                           std::wstring& error);

// A missing manifest is an empty list; a corrupt one is renamed to `.bad`.
bool LoadShortcutManifest(std::vector<ShortcutEntry>& out, std::wstring& error);
bool SaveShortcutManifest(const std::vector<ShortcutEntry>& entries, std::wstring& error);

// Deletes every link in the manifest, clears it and returns how many files were
// removed.  Phase 8 calls this from self-delete.
int RemoveAllShortcuts(std::wstring& error);

}  // namespace gf