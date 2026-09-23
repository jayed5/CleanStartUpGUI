# CleanStart GUI — source

This folder contains everything needed to build `CleanStartInstaller.exe`.

## Files

| File | Purpose |
| --- | --- |
| `installer.cpp` | Full C++ source (Win32 GUI + cleanup engine + console modes) |
| `installer.rc` | Windows resources (icon, version info, manifest) |
| `build.ps1` | Build script: generates the icon, manifest is embedded, compiles to `bin/CleanStartInstaller.exe` |
| `app.template.manifest` | Manifest template (comctl32 v6 + UAC); copied to `app.manifest` by the build |
| `icon.template.ps1` | Icon generator library (sourced by `build.ps1`) |

`CleanStart.ico` and `app.manifest` are **generated** by the build script and git-ignored.

## Build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

Output: `../bin/CleanStartInstaller.exe` (statically linked, no external DLLs).

Requires Windows with PowerShell 5+ and a MinGW-w64 g++ (auto-detected: MSYS2 UCRT64, CodeBlocks, or PATH).

## Repository layout

- `src/` — this folder (source only)
- `bin/` — prebuilt exe + release notes (also published on the GitHub Releases page)
