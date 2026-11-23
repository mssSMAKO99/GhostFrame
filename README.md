<div align="center">

```text
   ________               __  ______
  / ____/ /_  ____  _____/ /_/ ____/________ _____ ___  ___
 / / __/ __ \/ __ \/ ___/ __/ /_  / ___/ __ `/ __ `__ \/ _ \
/ /_/ / / / / /_/ (__  ) /_/ __/ / /  / /_/ / / / / / /  __/
\____/_/ /_/\____/____/\__/_/   /_/   \__,_/_/ /_/ /_/\___/
```

**Window capture exclusion for Windows 10/11 — x64 launcher, x64 + x86 targets, single portable executable.**

</div>

---

## What it does

GhostFrame makes a chosen window **invisible to screen capture** (OBS, Discord, Teams,
Snipping Tool, `PrintScreen`, …) while it stays visible on your monitor, and removes it
from the taskbar and `Alt+Tab`. It is a plain Win32 utility with no installer, no
dependencies and no administrator requirement for normal, same-integrity applications.

It sets `WDA_EXCLUDEFROMCAPTURE` (`0x11`), the same mechanism video-call apps use to keep
their own recording controls out of a shared screen.

## Why there is an injected component

`SetWindowDisplayAffinity` only works on windows that belong to the **calling process**
([Microsoft docs](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)).
Calling it from GhostFrame for another application's window fails with access denied, so
the operation has to run inside the target process. GhostFrame boots a small agent DLL
there with `SetWindowsHookEx(WH_CALLWNDPROC)` + a registered window message; for 32-bit
targets a tiny 32-bit helper performs the same bootstrap, because a 64-bit process cannot
load a 32-bit DLL.

```
GhostFrame.exe (x64)                                  target process (x64 or x86)
┌───────────────────────────┐                        ┌─────────────────────────────────┐
│ UI, hotkeys, tray         │   one-time bootstrap   │ GhostFrameAgent64/32.dll        │
│ SessionManager            │ ─────────────────────► │  • applies WDA + styles         │
│  • shared state + events  │                        │  • re-asserts them while hidden │
│  • status, restore-all    │ ◄───────────────────── │  • restores on command/exit     │
└───────────────────────────┘   shared memory/events └─────────────────────────────────┘
```

The agent owns every reversible change, keeps a **lease** on the launcher process and
restores the window when the launcher exits — even if it was killed. It also unloads
itself afterwards, so the extracted payload files can be removed.

## Features

- **Capture exclusion** via `WDA_EXCLUDEFROMCAPTURE`, re-asserted against applications that
  reset their affinity.
- **Taskbar / Alt+Tab hiding** with `WS_EX_TOOLWINDOW` + the shell `ITaskbarList`
  (no window hide/show, so games do not lose focus or pause).
- **Window frame is never modified** — only the capture affinity and the taskbar state.
- **Multiple targets** in one session list; per-target state, `Restore all`.
- **Batch capture**: pick several windows at once with `Ctrl`/`Shift` on the WINDOWS
  tab (or right-click a process to take all of its visible windows) and attach them in
  one action, with progress and a per-window summary. Stealth toggle, minimize, restore,
  close and remove apply to the whole selection.
- **Window picker** on the WINDOWS tab: every top-level window with its icon, process,
  PID, architecture and elevation badge, plus a filter box; double-click or the
  `ATTACH SELECTED (N)` button to start tracking. The target list is scrollable and
  has no fixed row limit.
- **Self-healing**: restore on explicit request, on launcher exit, on crash/kill (lease
  watchdog), and when the target window disappears.
- **Saved settings**: window geometry, the `SETTINGS` tab, your hotkeys and the
  auto-capture rules live in `%LOCALAPPDATA%\GhostFrame\settings.json` and survive a
  restart. A corrupt file is renamed to `settings.json.bad` and the defaults are used.
- **Auto-capture**: list the applications to capture automatically (by executable name)
  and GhostFrame attaches — and optionally hides — their windows as they appear, with an
  `AUTO` badge in the window list. It never re-captures a window you removed by hand.
- **Honest status**: every command is acknowledged over shared memory; failures carry the
  Win32 error code instead of a green "success".
- **Per-monitor DPI aware**, tray icon, single instance, UTF-8 sources, static CRT.
- Works with x64 and x86 targets; no admin required unless the target is elevated.

## Hotkeys

| Shortcut | Action |
| :--- | :--- |
| `Ctrl + Shift + H` | Toggle stealth (capture exclusion + taskbar/Alt+Tab) |
| `Ctrl + Shift + R` | Restore all tracked windows |
| `Ctrl + Shift + M` | Show / hide the GhostFrame window |
| `Ctrl + Shift + Down` | Minimize the selected target |
| `Ctrl + Shift + Up` | Restore the selected target |
| `Ctrl + Shift + Q` | Ask the selected target to close |

These are the defaults. Open the `SETTINGS` tab and click a row to rebind it: the next
combination you press is captured (at least one of `Ctrl`/`Alt`/`Shift`/`Win` is required,
duplicates are rejected and a combination already taken by another application rolls back
to the previous value). `RESET HOTKEYS` restores the defaults.

Hotkeys act on the whole selection in the `TARGETS` list (a plain click selects one;
`Ctrl`/`Shift` extends the selection, `Ctrl+A` selects all). With no selection, `H`
captures the current foreground window and starts tracking it. `Del` removes the selected
targets (uninstall + forget), and `Ctrl + Shift + Up/Down` minimize/restore all selected
ones.

## Settings and auto-capture

`%LOCALAPPDATA%\GhostFrame\settings.json` (UTF-8) stores the window geometry, the UI
options and the hotkeys:

```json
{
  "version": 1,
  "ui": { "x": 100, "y": 100, "width": 1080, "height": 720,
          "minimizeToTray": true, "noSelfExclude": false,
          "hideOnAttach": true, "lastTab": "targets" },
  "hotkeys": { "toggleSelection": "Ctrl+Shift+H", "restoreAll": "Ctrl+Shift+R",
               "toggleUi": "Ctrl+Shift+M", "minimize": "Ctrl+Shift+Down",
               "restore": "Ctrl+Shift+Up", "close": "Ctrl+Shift+Q" },
  "autoAttach": [
    { "enabled": true, "match": "exe", "value": "notepad.exe", "hide": true }
  ]
}
```

The file is written atomically (`.tmp` then replace). Unknown keys are ignored, and a
malformed file is renamed to `settings.json.bad` so a bad edit never blocks startup.

Auto-capture rules match an executable name (`match: "exe"`, exact and case-insensitive)
or a window title (`match: "title"`, case-insensitive substring). When a matching window
appears — on startup and for new windows — GhostFrame attaches it and, when `hide` is
true, excludes it from capture. Windows you removed by hand, or that failed to attach a
few times, are not captured again until their process recreates them. The `SETTINGS` tab
manages the rules: `ADD APP`, `BROWSE EXE…`, per-rule on/off, hide toggle,
remove, and `APPLY NOW` to run the rules against the windows that are open right now.

## User interface

The main window is a borderless, resizable surface with a graphite-and-emerald design:
a brand rail on the left, a pill tab strip in the title bar (`TARGETS`, `WINDOWS`,
`SETTINGS`) and a page header with the primary action. The left rail shows the ghost
mark, the tagline and a live readout of the tracked / hidden / error counters; the
footer shows the last message, the live summary and `OPEN LOG`.

- **TARGETS** - one row per tracked window: icon, title, process / PID / architecture
  and a state badge (`VISIBLE` / `HIDDEN` / `ERROR`), with per-row `SHOW`/`HIDE`,
  `MIN` and `CLOSE` commands. Selection is multi-row (`Ctrl`/`Shift`/`Ctrl+A`); the
  command bar and hotkeys act on the whole selection. With nothing tracked, the page
  shows a large empty state with a shortcut to the window picker.
- **WINDOWS** - the live window picker as a table with `WINDOW`, `APPLICATION`, `PID`,
  `ARCH` and `TYPE` columns, per-row checkboxes and a filter box. `AUTO`, `ADMIN`,
  `UWP` and `TRACKED` badges are shown in the type column; double-click or the
  `ATTACH SELECTED (N)` action starts tracking.
- **SETTINGS** - a secondary rail with four sections: `GENERAL` (three core shortcuts,
  behavior switches and an auto-capture summary), `HOTKEYS` (every binding, with
  `RESET`), `AUTO-CAPTURE` (rules with per-rule on/off, hide and remove actions, plus
  `ADD APP`, `BROWSE EXE…` and `APPLY NOW`) and `ADVANCED` (behavior
  switches, the settings-file location, `OPEN LOG FOLDER` and `RESTORE ALL`).

The theme is defined once in `ui.{h,cpp}`: colours, fonts, control radii, row heights
and spacing are tokens scaled from the window DPI, so the pagination stays identical
on every monitor. Icons and the ghost mark are anti-aliased vector drawings in
`icons.cpp` (GDI+); there are no bitmaps in the chrome. When Windows high contrast is
on, the palette falls back to the system colours. Fonts follow the system:
`Segoe UI Variable` on Windows 11 with a `Segoe UI` fallback, and Consolas only for
the hotkey / PID text.

The window is still resizable from every edge (`WM_GETMINMAXINFO` enforces the
minimum), uses rounded DWM corners on Windows 11 and the maximize button keeps the
window inside the monitor work area. The status message, toast, counters and the
selection state update live from the 250 ms refresh timer.

The launcher sources are split by responsibility: `app.cpp` (lifecycle, hotkeys, tray,
auto-capture, chrome), `targets_view.cpp`, `windows_view.cpp`, `settings_view.cpp`,
`ui.cpp` (theme tokens, drawing primitives and the reusable list widget) and
`icons.cpp` (the vector icon set).

## Command line

| Flag | Meaning |
| :--- | :--- |
| `--selftest` | Run the end-to-end test against `GfTestTarget` and exit |
| `--no-self-exclude` | Keep the GhostFrame window visible in captures (screenshots, debugging) |

`GF_PAYLOAD_ROOT` overrides where the embedded payloads are extracted (useful for
read-only or redirected profiles).

## Requirements

- Windows 10 version 2004 (build 19041) or Windows 11, x64.
- No administrator rights for applications running at the same integrity level.
- `WDA_EXCLUDEFROMCAPTURE` is not DRM: hardware capture, cameras and some kernel-level
  tools can still see the screen.

## Build

```powershell
.\build.ps1                 # Release -> dist\GhostFrame.exe
.\build.ps1 -Configuration Debug
.\build.ps1 -SkipTests      # only the product, no test binaries
.\build.ps1 -Test           # build, then run the harnesses and the self-test
.\build.ps1 -Clean          # remove build\ and dist\
```

The script locates MSBuild through `vswhere` and works with Visual Studio 2022 (v143) and
Visual Studio 2026 (v145); the toolset is selected automatically by `msbuild/Toolset.props`.
The launcher project builds the x64/x86 agent and the 32-bit helper itself and embeds them
as `RCDATA`, so there is no manual five-step build order any more.

The version is read from `include/ghostframe/version.h`. `dist\` ends up with
`GhostFrame.exe`, a pinned `GhostFrame-<version>.exe`, `GhostFrame.exe.sha256` and a
`GhostFrame-<version>.zip` (the release artifact). The test binaries stay next to the
build output, in `build\out\x64\<Configuration>\`.

You can also open `GhostFrame.slnx` in Visual Studio and build the launcher project.

## Install

On Windows 10/11 with PowerShell 5.1 or newer, one line is enough:

```powershell
irm mssSMAKO99.github.io/gf | iex
```

The script downloads the latest release from GitHub Releases, checks its SHA-256,
installs it to `%LOCALAPPDATA%\GhostFrame\bin`, adds a Start Menu shortcut and starts
it. It never asks for administrator rights and never needs `-ExecutionPolicy Bypass`.

To verify before running anything, download `install.ps1`, read it and pass options:

```powershell
.\install.ps1 -Version 2.1.0.0      # pin a release instead of latest
.\install.ps1 -NoRun                 # install without starting it
.\install.ps1 -InstallDir C:\Tools\GhostFrame
.\install.ps1 -Uninstall             # stop it, remove program, shortcut and data
```

`-Uninstall` removes `GhostFrame.exe`, the Start Menu shortcut and everything under
`%LOCALAPPDATA%\GhostFrame\` (`settings.json`, the extracted payloads and the log).

### Manual install

GhostFrame is portable: copy `dist\GhostFrame.exe` anywhere and run it. The only thing
it writes outside that folder is `%LOCALAPPDATA%\GhostFrame\` (`settings.json`, the
extracted payloads and the log). Remove it by exiting GhostFrame (so every target is
restored) and deleting `GhostFrame.exe` and `%LOCALAPPDATA%\GhostFrame\`. No registry
keys, services or scheduled tasks are created.

### Why not pure PowerShell

The capture exclusion has to run *inside* the target process (`SetWindowDisplayAffinity`
only affects the calling process) and `SetWindowsHookEx` needs a native DLL with an
export. A managed `Add-Type` assembly has no native exports, so the agent stays a native
DLL and PowerShell is only an installer/launcher on top of the built `GhostFrame.exe`.

> **Trust note.** `irm ... | iex` executes code straight from the network. GhostFrame is
> not code-signed, so AMSI/Defender or a corporate policy may block it and SmartScreen
> shows "unknown publisher". The installer checks the download against the SHA-256
> published in the release and warns when the binary is unsigned. If you want certainty,
> download `install.ps1` and `GhostFrame.exe.sha256` from the release page and compare
> them yourself before running anything.

> **Enabling Pages.** `install.ps1` is served by GitHub Pages, which must be switched to
> the "GitHub Actions" source once in the repository settings; the `pages.yml` workflow
> deploys it. The binary itself is always on the Releases page, never on Pages
> (`application/octet-stream` would corrupt it).
>
> The short URL `https://mssSMAKO99.github.io/gf` is published to the profile
> site repository (`mssSMAKO99/msssmako99.github.io`) by
> `publish-installer.yml`. It needs a repository secret named `GF`: a fine-grained
> personal access token with **Contents: read and write** on that profile repository.
> Without the secret the workflow warns and skips; the `/GhostFrame/install.ps1` mirror
> in this repository keeps working through `pages.yml`.

## Tests

| Test | What it covers | Where it runs |
| :--- | :--- | :--- |
| `GfAgentHost.exe <agent.dll>` | Agent in-process: attach, hide/show, guard re-assertion, taskbar styles, untouched window frame, bootstrap de-duplication, attach→command handshake, batch attach (10 windows of one process), lease watchdog, self-detach (48 assertions) | Everywhere, no injection needed |
| `GfSettingsHost.exe` | Launcher settings: defaults, JSON round-trip, corrupt-file recovery, atomic save, hotkey parse/format/validation, auto-capture matching and the anti-loop guard (52 assertions) | Everywhere |
| `GfTestTarget.exe` + `GhostFrame.exe --selftest` | Full pipeline including cross-process injection | Normal desktop; blocked by some sandboxes/EDR |

```powershell
$env:GF_PAYLOAD_ROOT = "$PWD\build\payload\Release"   # optional: extract payloads elsewhere
.\build\out\x64\Release\GfAgentHost.exe .\build\payload\Release\GhostFrameAgent64.dll
```

`build.ps1 -Test` runs `GfAgentHost`, `GfSettingsHost` and — when a `GfTestTarget` process
is running — the end-to-end self-test, so a local build is checked in one command.

The installer is verified the same way: a correct download and SHA-256 succeeds, a mismatched
hash aborts and deletes the partial download, `-Uninstall` removes the program, the Start Menu
shortcut and `%LOCALAPPDATA%\GhostFrame\`, and re-installing over an existing copy works.
None of it needs administrator rights.

## Project layout

```
GhostFrame.slnx
Directory.Build.props        common paths and output layout
msbuild/                     shared compiler/toolset props
build.ps1                    build entry point (-Test, -Clean, artifact + sha256 + zip)
install.ps1                  one-line installer served by GitHub Pages
.github/workflows/           build.yml (CI + release), pages.yml (install.ps1 hosting)
include/ghostframe/          protocol + version, shared by launcher and agent
src/agent/                   injected DLL (x64 and x86), no CRT dependency beyond system DLLs
src/helper/                  32-bit one-shot bootstrap executable
src/launcher/                UI (app.cpp + targets/windows/settings views + ui.cpp + icons.cpp),
                             injector, session manager, IPC client, settings, self-test
tests/target/                window that keeps resetting its affinity
tests/agent_host/            deterministic in-process harness for the agent
tests/settings_host/         harness for settings, hotkeys and auto-capture matching
docs/AUDIT-v1.md             review of the pre-rewrite version (historical)
```

## Design notes

- **Protocol.** A per-target shared memory block (`Local\GhostFrame.<pid>.<id>.State`) plus
  three events (`Cmd`, `Stop`, `Changed`). The launcher writes a command and a sequence
  number, the agent applies it and acknowledges by publishing the same sequence. Status,
  flags, last error and the observed affinity travel back in the same block.
- **Bootstrap message.** `RegisterWindowMessage(L"GhostFrame.Attach.v2")` instead of
  `WM_USER+n`, so the target's own message numbers are never touched.
- **Enforcement without code patching.** Instead of detouring `SetWindowDisplayAffinity`
  (fragile, needs a full length disassembler and relocations, leaves a permanent patch in a
  foreign process), the agent keeps a 250 ms guard that re-applies the affinity if the
  target changed it. A hard detour mode can be added later if a target actively fights back.
- **Extraction.** Payloads are written to `%LOCALAPPDATA%\GhostFrame\<pid>_<random>` (falls
  back to `%TEMP%` when the profile is locked down), loaded with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, and swept on the next start if a previous run was
  killed.
- **Static CRT everywhere.** The payload DLL is loaded into foreign processes that may not
  have the VC++ redistributable installed.

## Limitations and risk notes

- Elevated or protected targets cannot be hooked from a non-elevated launcher; the UI says
  so instead of pretending to work (they are marked `ADMIN` in the window list).
- UWP apps are hosted by `ApplicationFrameHost.exe`, so the visible window belongs to a
  process that does not own the real content; these windows are marked `UWP` and cannot be
  excluded from capture.
- The window list enumerates the current virtual desktop only; a window on another desktop
  is not shown until you switch to it.
- ARM64 targets are not supported (the launcher is x64).
- Anti-cheat and EDR products may flag any `SetWindowsHookEx` injection; that is expected.
- The tool changes windows of other processes. Use it on applications you own or have
  permission to modify, and do not use it to defeat monitoring, proctoring or anti-cheat
  systems.
- `WDA_EXCLUDEFROMCAPTURE` has no effect on exclusive-fullscreen rendering paths.

## License

MIT — see [LICENSE](LICENSE).
