# Current Task

Task ID: T09.2
Title: README redesign, documentation assets and identity rebrand
Phase: 8 — Self-delete (maintenance)
Status: done

## Goal

Turn the README into a polished open-source project page with real screenshots,
a real demo recording, a hero banner and verified documentation, and replace the
old author/repository identity with J.K. Omen / overcumatosis.

## Result

### README

- Rewritten `README.md` (480 lines, CRLF, UTF-8 without BOM) with:
  - a centered hero banner and tagline;
  - build, release, license, platform and C++20 badges;
  - quick links to Install, Usage, How it works, Build and Releases;
  - a "Not DRM" note next to the feature claim;
  - six real screenshots in a 3x2 table;
  - a real animated demonstration (`demo.gif`, plus `demo.mp4`);
  - a feature-complete usage guide (selection, batch attach, primary/sticky,
    auto-capture, hotkeys, tray, shortcuts, self-delete, settings file);
  - a Mermaid architecture diagram for UI ↔ daemon ↔ agent, with the
    named-pipe and shared-memory details;
  - the real command-line flags, install parameters, limitations/FAQ,
    build/test instructions and an accurate project layout.
- Removed stale content: "three tabs" (now four), "no installer" (there is an
  optional installer), a nonexistent `docs/AUDIT-v1.md`, and the incomplete CLI
  flag list.

### Assets

`docs/assets/` (1.9 MB total):

| File | Contents |
| :--- | :--- |
| `hero.png` | 1600x500 AI-generated banner, no text, no fake UI |
| `screenshot-targets.png` | real Targets tab, one visible + one hidden target |
| `screenshot-windows.png` | real picker filtered to a clean demo window |
| `screenshot-shortcuts.png` | real Shortcuts tab |
| `screenshot-appearance.png` | real Appearance settings |
| `screenshot-hotkeys.png` | real Hotkeys settings |
| `screenshot-advanced.png` | real Advanced settings |
| `demo.gif` | 1000 px wide, 12 fps, real UI recording, 857 KB |
| `demo.mp4` | source recording, 271 KB |

Screenshots and the recording contain only the GhostFrame window; no personal
desktop windows, names or paths are visible.

### Identity rebrand

- The previous repository/account name → `overcumatosis`
- The previous copyright holder → `J.K. Omen`

Applied in `README.md`, `install.ps1` (comments and `$BaseUrl`), `LICENSE`,
`.github/workflows/pages.yml` and `.github/workflows/publish-installer.yml`.

The short installer URL is now `https://overcumatosis.github.io/gf`, and the
installer publishes to `overcumatosis/overcumatosis.github.io`. The GitHub
account, repository and profile site still need to be renamed outside this
working tree; until then the new URLs and badges return 404.

### Removed

- `README_REDESIGN.md` (the design brief)
- `concept.png` (a mockup of the old v2.0.0 UI)

## Verification

- All README asset paths resolve.
- No previous-identity strings remain anywhere in the working tree.
- `install.ps1` stays ASCII-only with no BOM, and still parses in PowerShell.
- `README.md` is valid GitHub-flavored Markdown with HTML tables and Mermaid.
- Harness counts confirmed: agent 48, settings 113, brand 28 assertions.
- `git diff --check` clean for the changed files.

## Next

T10 (optional runtime plugins, Phase 9) stays blocked until the user explicitly
asks for it. Do not edit `ROADMAP.md` while executing a task.

## Verify

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Configuration Release
.\build\out\x64\Release\GfAgentHost.exe .\build\payload\Release\GhostFrameAgent64.dll
.\build\out\x64\Release\GfSettingsHost.exe
.\build\out\x64\Release\GfBrandHost.exe
.\build\out\x64\Release\GhostFrame.exe --daemon-check
```

Run the binaries from a location other than the project folder in this sandbox.