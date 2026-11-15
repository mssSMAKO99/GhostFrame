<div align="center">

```text
   ________               __  ______                         
  / ____/ /_  ____  _____/ /_/ ____/________ _____ ___  ___  
 / / __/ __ \/ __ \/ ___/ __/ /_  / ___/ __ `/ __ `__ \/ _ \ 
/ /_/ / / / / /_/ (__  ) /_/ __/ / /  / /_/ / / / / / /  __/ 
\____/_/ /_/\____/____/\__/_/   /_/   \__,_/_/ /_/ /_/\___/  
                                                             
    // UNIVERSAL WINDOW STEALTH UTILITY [x64 + x86] //
```

**A lightweight, standalone WinAPI utility that hides target windows from screen capture, taskbar, and `Alt+Tab` without administrator privileges.**

</div>

---

### `// FEATURES`

* **Capture Exclusion (`0x11`):** Applies `WDA_EXCLUDEFROMCAPTURE` via a clean `SetWindowsHookEx` (`WH_CALLWNDPROC`) thread hook, making windows invisible to OBS, Discord, and screen-sharing tools while remaining visible on your monitor.
* **Taskbar & `Alt+Tab` Stealth:** Strips `WS_EX_APPWINDOW` and injects `WS_EX_TOOLWINDOW` to completely vanish from the taskbar and window switcher.
* **Borderless Toggle:** Dynamically strips window captions and frames (`WS_CAPTION`, `WS_THICKFRAME`) on the fly.
* **Universal Architecture (`x64` / `x86`):** Automatically detects target process bitness via `IsWow64Process` and routes the hook through an embedded 32-bit helper when needed.
* **Self-Contained & Stealthy:** Packs all payloads into a single `~163 KB` portable executable, self-extracts to `%TEMP%` at runtime, cleans up on exit, and hides its own UI from screen capture.

---

### `// HOTKEYS`

| Shortcut | Action |
| :--- | :--- |
| `Ctrl + Shift + H` | **Toggle Stealth** (`0x11` Capture Exclusion + Taskbar / `Alt+Tab` Hide) |
| `Ctrl + Shift + B` | **Toggle Borderless** (Strip / Restore Window Frame) |
| `Ctrl + Shift + Down` | **Minimize** Target Window |
| `Ctrl + Shift + Up` | **Restore** Target Window |
| `Ctrl + Shift + Q` | **Close** Target Window |
| `Ctrl + Shift + M` | **Hide / Show** GhostFrame UI |

---

### `// BUILD INSTRUCTIONS (VISUAL STUDIO)`

Because `HookLauncher64.exe` embeds the x64/x86 payloads as `RCDATA` resources, build the solution in the following order using **Release** configuration and **Multi-threaded (`/MT`)** runtime library:

1. **`HideHook` (`x64`):** Build `Release | x64` → rename output to `HideHook64.dll`.
2. **`HideHook` (`Win32 / x86`):** Build `Release | x86` → rename output to `HideHook32.dll`.
3. **`Helper32` (`Win32 / x86`):** Build `Release | x86` → take `Helper32.exe`.
4. **Stage Payloads:** Place `HideHook64.dll`, `HideHook32.dll`, and `Helper32.exe` inside the `HookLauncher64/` source directory (next to `GhostFrame.rc`).
5. **`HookLauncher64` (`x64`):** Build `Release | x64` → standalone executable ready.

---

### `// NOTES & LIMITATIONS`

* **OS Requirement:** `WDA_EXCLUDEFROMCAPTURE` (`0x11`) requires **Windows 10 (build 2004+)** or **Windows 11**.
* **Rendering Mode:** Works with DWM-composed windows (desktop apps, browsers, Windowed / Borderless games). Exclusive fullscreen apps bypass DWM.
* **Elevated Processes:** Cannot hook processes running as `Administrator` or protected by kernel-level anti-cheats when launched from a standard user account.

---

<div align="center">
  <sub>Built with pure C++ & WinAPI // Zero external dependencies</sub>
</div>
