# FileLocksmith Standalone

**[中文](README.md)** · [Download](https://github.com/U707T/FileLocksmith-Standalone/releases) · [Changelog](CHANGELOG.md) · [Details (中文)](docs/details-zh.md)

[![ci](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml/badge.svg)](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/U707T/FileLocksmith-Standalone?include_prereleases)](https://github.com/U707T/FileLocksmith-Standalone/releases)

"Who is locking this file?" — Microsoft PowerToys' FileLocksmith, extracted into a **single exe**:
see what's holding a file, kill the process, force-unlock it, or delete it. ~550 KB, system DLLs only
(no PowerToys, no Windows App SDK, no .NET, no VC++ runtime).

## Quick start

**GUI**: download `FileLocksmith-x64.exe` from Releases → double-click → **drag a file or folder into the window**
(or drop it onto the exe icon, or use the Explorer context menu).

**CLI** (the common ones):

| Command | What it does |
| --- | --- |
| `FileLocksmith.exe report.docx` | list whoever holds the file |
| `FileLocksmith.exe report.docx --json` | machine-readable output (stdout is exactly one JSON line) |
| `FileLocksmith.exe report.docx --unlock` | force-unlock: close their handles, **do not kill the process** |
| `FileLocksmith.exe report.docx --kill` | kill the holding processes |
| `FileLocksmith.exe app.lnk --kill` | kill that app's whole process tree (the Chrome / VS Code case) |
| `FileLocksmith.exe report.docx --delete` | delete it (to the Recycle Bin; asks before force-closing handles) |
| `FileLocksmith.exe --register` | add an Explorer context-menu entry (HKCU, no admin needed) |

Everything else: `FileLocksmith.exe --help`.

## Features

- Culprit list: process / PID / user / how it holds the file (`handle ×N` or `module mapping`) / files,
  with multi-select and a right-click menu;
- `Force unlock`: close other processes' file handles without killing them (with a risk prompt; scoped to the selection);
- `Delete`: up to three confirmations (delete → force-close handles → delete on reboot) — works on locked files;
- Drop a `.lnk` / `.exe`: expand the whole process tree and kill it in one go;
- Explorer context menu + "Send to" entry; **drag & drop works in elevated mode too** (since 2.0.4);
- Remembers window position/size and column widths; no config file, no logs, no telemetry.

## Requirements

Windows 10 / 11 x64 (Win7 SP1+ per API declarations); verified on Windows 10 22H2 and Server 2022;
no runtime dependencies.

## Build

```bat
build-msvc.bat        :: Windows + VS 2022
```
```bash
# Cross-compile from Linux (this is how the mingw asset in each Release is built)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## Local tests (same 11 cases as CI)

```powershell
pwsh -ExecutionPolicy Bypass -File tools\run-local-tests.ps1 -Exe .\FileLocksmith-x64.exe
```

## Known limitations

- Without admin rights some system processes stay invisible / untouchable (the UI offers "restart as administrator");
- Force unlock is like yanking a USB drive from a running program: the holder may error out, crash or lose
  unsaved data — you get a confirmation, but it's your call;
- Killing a process is a hard kill; there is no "save your work" graceful shutdown;
- The GUI has no automated tests (CI covers the CLI paths only) — report drag & drop / button issues as an issue.

## More

Internals (handle enumeration + hang protection), repo layout, compatibility rationale, CI details and the
full test checklist → **[details (in Chinese)](docs/details-zh.md)**.

Derived from Microsoft PowerToys (MIT, see `LICENSE-PowerToys-MIT.txt`); full license in `LICENSE`.
