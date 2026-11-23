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

Hotkeys act on the whole selection in the `TARGETS` list (a plain click selects one;
`Ctrl`/`Shift` extends the selection, `Ctrl+A` selects all). With no selection, `H`
captures the current foreground window and starts tracking it. `Del` removes the selected
targets (uninstall + forget), and `Ctrl + Shift + Up/Down` minimize/restore all selected
ones.

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
.\build.ps1                 # Release -> dist\GhostFrame.exe (+ test binaries)
.\build.ps1 -Configuration Debug
.\build.ps1 -SkipTests
```

The script locates MSBuild through `vswhere` and works with Visual Studio 2022 (v143) and
Visual Studio 2026 (v145); the toolset is selected automatically by `msbuild/Toolset.props`.
The launcher project builds the x64/x86 agent and the 32-bit helper itself and embeds them
as `RCDATA`, so there is no manual five-step build order any more.

You can also open `GhostFrame.slnx` in Visual Studio and build the launcher project.

## Tests

| Test | What it covers | Where it runs |
| :--- | :--- | :--- |
| `GfAgentHost.exe <agent.dll>` | Agent in-process: attach, hide/show, guard re-assertion, taskbar styles, untouched window frame, bootstrap de-duplication, attach→command handshake, batch attach (10 windows of one process), lease watchdog, self-detach (48 assertions) | Everywhere, no injection needed |
| `GfTestTarget.exe` + `GhostFrame.exe --selftest` | Full pipeline including cross-process injection | Normal desktop; blocked by some sandboxes/EDR |

```powershell
$env:GF_PAYLOAD_ROOT = "$PWD\build\payload\Release"   # optional: extract payloads elsewhere
.\build\out\x64\Release\GfAgentHost.exe .\build\payload\Release\GhostFrameAgent64.dll
```

## Project layout

```
GhostFrame.slnx
Directory.Build.props        common paths and output layout
msbuild/                     shared compiler/toolset props
include/ghostframe/          protocol + version, shared by launcher and agent
src/agent/                   injected DLL (x64 and x86), no CRT dependency beyond system DLLs
src/helper/                  32-bit one-shot bootstrap executable
src/launcher/                UI, injector, session manager, IPC client, self-test
tests/target/                window that keeps resetting its affinity
tests/agent_host/            deterministic in-process harness for the agent
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
  so instead of pretending to work.
- ARM64 targets are not supported (the launcher is x64).
- Anti-cheat and EDR products may flag any `SetWindowsHookEx` injection; that is expected.
- The tool changes windows of other processes. Use it on applications you own or have
  permission to modify, and do not use it to defeat monitoring, proctoring or anti-cheat
  systems.
- `WDA_EXCLUDEFROMCAPTURE` has no effect on exclusive-fullscreen rendering paths.

## License

MIT — see [LICENSE](LICENSE).
