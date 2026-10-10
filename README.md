# FileLocksmith 独立版

[![ci](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml/badge.svg)](https://github.com/U707T/FileLocksmith-Standalone/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/U707T/FileLocksmith-Standalone?include_prereleases)](https://github.com/U707T/FileLocksmith-Standalone/releases)

把 PowerToys 里的 **FileLocksmith**（"什么正在占用这个文件？"）单独抽出来，做成一个
**1 MB 的单文件 exe**，不依赖 PowerToys、不依赖 Windows App SDK、不依赖 .NET。

- 源码基线：`microsoft/PowerToys` `main` @ `ccdd9b1`（2026-10-10）
- 产物：`dist/FileLocksmith.exe`（x64，PE32+，只依赖系统自带的 DLL）
- 授权：MIT（衍生自微软 PowerToys，见 `LICENSE-PowerToys-MIT.txt`）

---

## 1. 结论：能不能抽出来？

**能，而且核心部分几乎是零成本的。** 原模块由 6 个子工程组成，真正的"功能"和"分发壳"是分开的：

| 子工程 | 作用 | 抽离处理 |
| --- | --- | --- |
| `FileLocksmithLibInterop`（`FileLocksmith.cpp` / `NtdllBase` / `NtdllExtensions` / `NativeMethods`） | **核心算法**：枚举全系统句柄 → 匹配文件内核路径 → 反查进程/用户 | ✅ 原样保留（只把 `pch.h` 里的 C++/WinRT 依赖剥掉，`FILELOCKSMITH_LIB_STATIC` 那条分支本来就不需要它） |
| `FileLocksmithCLI` | 命令行前端 | ✅ 保留设计，重写：去掉 spdlog、PowerToys 遥测、C++/WinRT json，改自带参数解析 + 手写 JSON |
| `FileLocksmithUI`（WinUI 3 + Windows App SDK） | 图形界面 | ✅ 保留功能，界面换成 **Win32 原生控件**（ListView + 按钮 + 拖放），彻底摆脱 Windows App SDK |
| `FileLocksmithExt`（`IExplorerCommand` 外壳扩展 DLL） | 右键菜单 | ❌ 去掉，改用 **HKCU 注册表 verb**（无需 MSIX、无需管理员、无需注册 DLL） |
| `FileLocksmithContextMenu`（稀疏包 `AppxManifest.xml`） | Win11 一级右键菜单 | ❌ 去掉（见第 6 节：要复刻它就必须做 MSIX 打包） |
| `FileLocksmithLib`（`Settings` / `Trace` / `IPC` / `Constants`） | 模块设置、遥测、UI 间通信 | ❌ 去掉：遥测不需要、GPO 不需要、设置文件不需要；IPC 用自实现的"单实例 + `WM_COPYDATA`"替代 |

**真正的技术内核只有约 400 行 C++**（`NtdllExtensions` 里的句柄枚举 + 反挂起保护），
其余全是 PowerToys 的基础设施（日志、设置、遥测、打包、UI 框架）。
所以"抽出来做 exe"是个很划算的重构。

---

## 2. 核心原理（保留原实现，未做更改）

1. `NtQuerySystemInformation(SystemExtendedHandleInformation)` 拿到全系统句柄表；
2. 逐个 `DuplicateHandle` 到本进程，用 `NtQueryObject(ObjectTypeInformation)` 判断类型是不是 `File`；
3. 是文件就用 `NtQueryObject(ObjectNameInformation)` 拿到**内核路径**（`\Device\HarddiskVolume3\...`）；
4. `CreateFileW(path, ...)` 打开用户给的路径，同样取内核路径，做**归一化匹配**（目录还会做前缀匹配，于是"某个目录被占用"也能查出来）；
5. 反过来枚举进程（`SystemProcessInformation`）+ 各进程加载的模块（`EnumProcessModules`），模块被占用也算；
6. `OpenProcessToken` + `LookupAccountSid` 拿到占用者用户名。

**关键保护（原样保留）**：第 3 步的 `NtQueryObject` 在个别句柄上会永久卡死，且没有超时参数。
原实现把这一轮循环丢到独立线程里跑，主线程每 200 ms 检查进度；如果 200 ms 内进度没增长，
就 `TerminateThread` 干掉它并跳过该句柄继续。这是这个工具能在真有问题的机器上跑完的原因，
**不要为了"优雅"把它删掉**。

---

## 3. 目录结构

```
FileLocksmith-Standalone/
├── CMakeLists.txt              # MSVC / MinGW 通用构建脚本
├── VERSION                     # 版本号单一来源（CI 用它决定 Release tag）
├── CHANGELOG.md
├── toolchain-mingw.cmake       # Linux 交叉编译到 Windows x64 用
├── build-msvc.bat              # Windows 上一键编译（VS 2022 + CMake）
├── .github/workflows/ci.yml    # CI：Windows(MSVC) + Linux(MinGW) 编译，功能自测，自动发 Release
├── README.md
├── LICENSE / LICENSE-PowerToys-MIT.txt
├── res/
│   ├── app.manifest            # ComCtl32 v6、DPI 感知、asInvoker
│   ├── app.ico                 # 复用原版图标
│   ├── version.h.in            # 由 CMake 按 VERSION 生成 version.h（C++ 与 .rc 共用）
│   └── resource.rc             # 图标 + 清单 + 版本信息
├── src/
│   ├── win_common.h            # 公共头 + MinGW 需要的 NT 兼容定义
│   ├── ntdll_base.{h,cpp}      # ntdll 函数指针封装（原样）
│   ├── ntdll_extensions.{h,cpp}# 句柄/进程/用户枚举（原样，含反挂起）
│   ├── file_locksmith.{h,cpp}  # find_processes_recursive（原样）
│   ├── process_result.h        # 结果结构（原样）
│   ├── app_utils.{h,cpp}       # 控制台/提权/结束进程等 Win32 工具
│   ├── cli.{h,cpp}             # 命令行模式 + 注册表右键菜单注册
│   ├── gui.{h,cpp}             # Win32 图形界面（新增，替代 WinUI 3）
│   └── main.cpp                # 入口：自动决定命令行还是图形界面
└── dist/FileLocksmith.exe      # 本地构建产物（被 .gitignore 忽略，仓库不放二进制）
```

> 想直接拿 exe：到 [Releases](https://github.com/U707T/FileLocksmith-Standalone/releases) 下载
> `FileLocksmith-x64.exe`（CI 用 MSVC 编译，已在 Windows runner 上跑过真实占用检测自测）。

---

## 4. 怎么用

### 图形界面
- 直接双击 `FileLocksmith.exe`；把文件/文件夹**拖进窗口**或**拖到 exe 图标上**；
- 列表里选中进程 → `结束进程`；`全部结束` 一键清理；双击某行 → 在资源管理器中定位该文件；
- 权限不够（系统进程看不到/杀不掉）时点 `以管理员身份重启`。

### 命令行
```powershell
FileLocksmith.exe report.docx               # 文本列出占用者
FileLocksmith.exe report.docx --json        # JSON 输出（方便脚本处理）
FileLocksmith.exe C:\build --kill           # 结束所有占用该目录的进程
FileLocksmith.exe setup.exe --wait --timeout 60000   # 等文件解锁，最多 60 秒
FileLocksmith.exe --elevate report.docx     # 提权后重开
FileLocksmith.exe --register                # 添加右键菜单项（仅当前用户 HKCU）
FileLocksmith.exe --unregister              # 移除右键菜单项
```

**模式是怎么决定的**：带任一 CLI 开关，或者父进程有控制台（即在 cmd/PowerShell 里运行）
→ 命令行模式；否则（双击、右键"打开方式"、右键菜单）→ 图形界面。
所以从资源管理器启动永远不会闪黑框。

> ⚠️ **PowerShell 用户注意**：这个 exe 是 Windows 子系统（为了双击不闪黑框），
> PowerShell 7 会把它判定为图形程序 —— `& .\FileLocksmith.exe f.txt --json` 这种写法
> **不会等待、也不会把输出装进变量**（`$LASTEXITCODE` 为空）。要拿输出请改用：
>
> ```powershell
> $json = & cmd /c 'FileLocksmith.exe "f.txt" --json'      # 用 cmd 包一层
> # 或
> Start-Process FileLocksmith.exe -ArgumentList 'f.txt','--json' -Wait -RedirectStandardOutput out.json
> ```
>
> `cmd.exe` 里直接用则没有这个问题（cmd 一律等待子进程）。

---

## 5. 自己编译

### Windows + Visual Studio 2022（推荐，产出 x64）
```bat
build-msvc.bat
:: 或手动
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```
关键点（都已在 `CMakeLists.txt` 里配好）：
- `/utf-8`：`cli.cpp` / `gui.cpp` 里的中文界面字符串是 UTF-8 编码，不加这个 MSVC 会按 GBK 解析；
- `UNICODE` + `_UNICODE`：否则 `ListView_SetItemText` 这类宏会展开成 ANSI 版本；
- `/ENTRY:mainCRTStartup` + `WIN32` 子系统：既能当 GUI 用（双击不闪控制台），又能当 CLI 用；
- `/MT` + `/MANIFEST:NO`：静态链接 CRT（不依赖 VC++ 运行时），并关掉链接器自动生成的
  manifest（我们在 `res/resource.rc` 里已经嵌了完整的一份，否则 CVTRES 会报重复资源）。

### Linux 交叉编译（本仓库就是这么产出 `dist/` 里那个 exe 的）
```bash
sudo apt install g++-mingw-w64-x86-64
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 产物：build/FileLocksmith.exe（-static，单文件，不依赖 libstdc++/libwinpthread）
```

编译 ARM64 版本：把工具链换成 `llvm-mingw`（`aarch64-w64-mingw32-g++`）即可，源码无需改动。

---

## 6. 关于右键菜单（和原版的最大差别）

- `--register` 写的是 `HKCU\Software\Classes\*\shell\FileLocksmith`（传统 verb）。
  Windows 10 直接可见；**Windows 11 需要点"显示更多选项"（Shift+F10）**才会出现在经典菜单里。
- PowerToys 原版能出现在 Win11 **一级菜单**，靠的是 `IExplorerCommand` + **稀疏 MSIX 包**
  （`FileLocksmithContextMenu/AppxManifest.xml`）。那条路的代价是：
  必须打包签名、必须安装包、必须部署外壳扩展 DLL——一个裸 exe 做不到，
  而且 `IExplorerCommand` 不允许"单文件绿色分发"。想要一级菜单就只能做 MSIX。
- 另一个零成本的替代方案：给 exe 建快捷方式放到 `%APPDATA%\Microsoft\Windows\SendTo`，
  然后在"发送到"里使用。

---

## 7. 体积与依赖对比

| | 原版模块 | 本独立版 |
| --- | --- | --- |
| 界面框架 | WinUI 3 / Windows App SDK（按官方配置是 **self-contained** 部署，运行时几十 MB 起） | Win32 原生控件（系统自带） |
| 运行依赖 | Windows App SDK 运行时、VC++ 运行时、MSIX 外壳扩展 | 仅系统 DLL（kernel32/user32/shell32/advapi32/comctl32），不需要 VC++ 运行时 |
| 分发 | 安装包 + 签名 + 注册外壳扩展 | 单个 exe，拷走就能用 |
| 进程间通信 | 命名管道/临时文件 + 两个进程 | 单实例 + `WM_COPYDATA` |

---

## 8. 已知限制

1. **提权才能看全**：非管理员下，部分系统级进程的句柄 `OpenProcess`/`DuplicateHandle` 会失败，
   结果会偏少。程序启动时会尝试开启 `SeDebugPrivilege`，但不是万能药；界面上有"以管理员身份重启"。
2. **`TerminateThread` 是有意为之**：见第 2 节。它是"不安全的"API，但在 `NtQueryObject` 卡死时
   没有别的办法，原版 PowerToys 就是这么做的。
3. **枚举有开销**：每次查询都要遍历全系统句柄表，机械盘/句柄多的机器上可能几秒。
   GUI 里这个工作放在后台线程，不会卡界面。
4. **结束进程 = `TerminateProcess`**：没有"保存数据"的优雅退出，界面上有二次确认。
5. **不做持久化**：没有设置文件、没有日志、没有遥测（这也是它能缩到 1 MB 的原因）。
6. **运行时验证靠 CI**：开发沙箱是 Linux(aarch64)，编不出也跑不了 Windows 程序，所以真机验证
   放在冒烟测试里 —— CI 的 `windows-2022` job 会真的创建一个被独占占用的文件，
   断言 exe 能报出占用者的 PID 与用户名，再用 `--kill` 断言占用进程确实被结束。
   这部分逻辑本身没在 Windows 上手点过（GUI 窗口的交互尤其没有），有问题请提 issue。

---

## 9. CI 与发布

`.github/workflows/ci.yml` 在 push 到 `main` 后自动跑：

| Job | 环境 | 干什么 |
| --- | --- | --- |
| `build-msvc` | `windows-2022` | CMake + VS 2022 编译 x64 → **功能自测** → 上传 `FileLocksmith-x64.exe` |
| `build-mingw` | `ubuntu-latest` | MinGW-w64 交叉编译 → 上传 `FileLocksmith-x64-mingw.exe` |
| `release` | `ubuntu-latest` | 若 `VERSION` 对应的 tag 还不存在 → 创建 Release 并附上两个 exe |

**Windows runner 上的功能自测是真的端到端验证**（本地 Linux 沙箱跑不了 Windows 程序）：

1. 让当前 PowerShell 进程用 `FileShare::None` 独占打开一个临时文件，
   然后 `FileLocksmith.exe <file> --json`，断言输出里出现该进程的 PID；
2. 另起一个进程持有文件句柄，断言能被检测到，再 `--kill` 断言它确实被结束；
3. 断言 `--version` 的输出与 `VERSION` 文件一致。

发版流程（与本工作区其他项目一致）：

```bash
# 1. 改版本号（唯一来源）
echo "1.1.0" > VERSION
# 2. CHANGELOG.md 顶部加一段
# 3. 推送 → CI 全绿 → 自动创建 Release（版本号带 -rc 后缀会自动标记为 prerelease）
git add -A && git commit -m "chore: 发布 1.1.0" && git push origin main
```

tag 已存在时 `release` job 只编译不重发，避免重复发布。

## 10. 许可

衍生自 Microsoft PowerToys（MIT License，Copyright (c) Microsoft Corporation），
完整许可证见 `LICENSE-PowerToys-MIT.txt`。保留原版权声明即可自由用于商业/非商业项目。
