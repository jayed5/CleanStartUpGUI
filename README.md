<div align="center">

# 🧹 CleanStart GUI

**Automatic Windows maintenance at every logon — now with an intuitive installer GUI.**

[![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-blue)](#)
[![Language](https://img.shields.io/badge/language-C%2B%2B%20(Win32)-00599C)](#)
[![Release](https://img.shields.io/badge/release-v1.0.0-green)](bin/RELEASE-v1.0.0.md)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](#)

*The GUI edition of [CleanStartUp](https://github.com/jayed5/CleanStartUp) — same engine, better experience.*

</div>

---

## 📖 About

**CleanStart** is a Windows maintenance tool that keeps your system clean automatically every time you log on. It clears temporary files, rebuilds the Explorer thumbnail cache, flushes the DNS resolver cache, and keeps a detailed log of everything it does.

The **GUI edition** replaces the original batch script with a native Win32 application written in C++ — featuring a live install status, one-click log management, and a **fully self-contained executable** that requires no companion scripts.

### Why GUI instead of a .bat script?

| | CleanStartUp (bat) | CleanStart GUI |
|---|:---:|:---:|
| Installation | run a script | **Install** button |
| Install status | none | ✓ live (green/gray) |
| Uninstallation | manual | ✓ one click |
| Log browsing | manual | ✓ built-in list |
| Dependencies | — | — (static exe, ~1.2 MB) |

---

## ✨ Features

- **Live install status** — the app detects whether the `CleanStart` scheduled task exists and whether a Startup-folder entry is present, shown at a glance in the Status field (green = installed, gray = not installed).
- **One-click Install / Uninstall** — registers a hidden Scheduled Task (`ONLOGON`, highest privileges) that runs the cleanup at every logon. Uninstall removes the task and any legacy Startup-folder entries.
- **Never hangs** — every operation (including status checks and task creation) runs on worker threads; the UI stays responsive and the progress bar animates while working.
- **Self-contained cleanup engine** — the logic is built into the exe (`CleanStartInstaller.exe /run`); no `.bat` file needed.
- **Log manager** — lists files from `C:\CleanLogs` (name, last run, size), opens the folder, and wipes all logs with a single click.
- **Automatic maintenance** — logs older than 3 days are removed on every cleanup run.
- **No external DLLs** — fully statically linked; a single, portable file.

### What does the cleanup do?

1. 🗑️ clears the user temp folder (`%TEMP%`), including leftover subfolders,
2. 🗑️ clears the Windows temp folder (`%SystemRoot%\Temp`),
3. 🖼️ deletes corrupted Explorer thumbnail caches (`thumbcache_*.db`),
4. 🌐 flushes the DNS resolver cache (`ipconfig /flushdns`),
5. 📝 writes a timestamped log to `C:\CleanLogs` and prunes old entries.

---

## 🚀 Quick start

1. Download [`CleanStartInstaller.exe`](../../releases).
2. Run the app — the **Status** field shows the current install state.
3. Click **Install** and accept the UAC prompt.
4. Done — the cleanup now runs automatically at every logon.

> Want to clean up right away? Click **Run cleanup now**.

### Command-line modes

| Command | Description |
| --- | --- |
| `CleanStartInstaller.exe /run` | one-time cleanup (used by the scheduled task) |
| `CleanStartInstaller.exe /install` | register the scheduled task (self-elevating) |
| `CleanStartInstaller.exe /uninstall` | remove the task and Startup entries |
| `CleanStartInstaller.exe /clearlogs` | delete all logs |

---

## 🔨 Building from source

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File src\build.ps1
```

The script generates the icon and manifest, locates a compiler (MSYS2 UCRT64 / CodeBlocks / PATH), and builds `bin\CleanStartInstaller.exe`. Requires any MinGW-w64 g++ (e.g. `pacman -S mingw-w64-ucrt-x86_64-gcc` in MSYS2).

Source details: [`src/README.md`](src/README.md).

---

## 📋 Requirements

- **OS:** Windows 10 / 11 (x64)
- **Permissions:** administrator for install/uninstall (the app opens the UAC prompt automatically)

## 📝 Logs

`C:\CleanLogs\clean_log_YYYY-MM-DD_HH-MM-SS.txt`

## 🔗 Orginal batch-script

- [**CleanStartUp**](https://github.com/jayed5/CleanStartUp) — the original batch-script version of CleanStart. Same engine and behavior; this repository is its GUI edition.

## ⚠️ Disclaimer

*This tool deletes files from temporary directories and flushes network caches. These are standard maintenance procedures, but you use it at your own risk. The author is not responsible for accidental data loss or system instability.*
