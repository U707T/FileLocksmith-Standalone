# FileLocksmith 独立版

**[English](README.en.md)** · [下载](https://github.com/U707T/FileLocksmith-Standalone/releases) · [更新日志](CHANGELOG.md) · [详细文档](docs/details-zh.md)

[![ci](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml/badge.svg)](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/U707T/FileLocksmith-Standalone?include_prereleases)](https://github.com/U707T/FileLocksmith-Standalone/releases)

「谁在占用这个文件？」—— 把 PowerToys 的 FileLocksmith 抽成**单文件 exe**：
能查占用者、能结束进程、能强制解锁、能删除。约 550 KB，只用系统 DLL
（不需要 PowerToys / Windows App SDK / .NET / VC++ 运行时）。

## 快速开始

**图形界面**：从 Release 下载 `FileLocksmith-x64.exe` → 双击 → 把文件/文件夹**拖进窗口**
（也可以拖到 exe 图标上，或右键菜单）。

**命令行**（常用）：

| 命令 | 作用 |
| --- | --- |
| `FileLocksmith.exe report.docx` | 列出占用者 |
| `FileLocksmith.exe report.docx --json` | JSON 输出（脚本友好，stdout 只有一行 JSON） |
| `FileLocksmith.exe report.docx --unlock` | 强制解锁：关句柄，**不结束进程** |
| `FileLocksmith.exe report.docx --kill` | 结束占用进程 |
| `FileLocksmith.exe app.lnk --kill` | 结束该程序的整棵进程树（Chrome / VS Code 这种一主多子） |
| `FileLocksmith.exe report.docx --delete` | 删除（进回收站；被占用时先单独确认再解锁） |
| `FileLocksmith.exe --register` | 添加资源管理器右键菜单（HKCU，不需要管理员） |

全部参数见 `FileLocksmith.exe --help`。

## 功能

- 占用者列表：进程 / PID / 用户 / 占用方式（`句柄 ×N` 还是 `模块映射`）/ 文件，支持多选与右键菜单；
- `强制解锁`：关掉别人的文件句柄而不结束进程（有风险确认，只对选中的进程生效）；
- `删除文件`：最多三道确认（删除 → 强制关句柄 → 重启后删除），被占用的文件也能删；
- 拖入 `.lnk` / `.exe`：展开整棵进程树，一键全部结束；
- 右键菜单 +「发送到」入口；**管理员模式下拖放同样可用**（2.0.4 起）；
- 记住窗口位置/大小与列宽；无设置文件、无日志、无遥测。

## 系统要求

Windows 10 / 11 x64（按 API 声明支持 Win7 SP1 起）；实测 Windows 10 22H2 与 Server 2022；无运行依赖。

## 自己编译

```bat
build-msvc.bat        :: Windows + VS 2022
```
```bash
# Linux 交叉编译（就是产出 Release 里 mingw 版的方式）
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## 本地自测（与 CI 同一套 11 个用例）

```powershell
pwsh -ExecutionPolicy Bypass -File tools\run-local-tests.ps1 -Exe .\FileLocksmith-x64.exe
```

## 已知限制

- 非管理员看不到/动不了部分系统进程（界面有「以管理员身份重启」）；
- 强制解锁 ≈「不结束程序直接拔 U 盘」：占用者之后可能报错、崩溃或丢未保存的数据 —— 有确认，但请自行判断；
- 结束进程 = 强杀，没有「保存数据」的优雅退出；
- 图形界面没有自动化测试（CI 只覆盖命令行），拖放/按钮类问题请提 issue。

## 更多

原理（句柄枚举 + 反挂起）、目录结构、兼容性依据、CI 细节、完整测试清单 → **[详细文档](docs/details-zh.md)**。

衍生自 Microsoft PowerToys（MIT，见 `LICENSE-PowerToys-MIT.txt`），完整许可见 `LICENSE`。
