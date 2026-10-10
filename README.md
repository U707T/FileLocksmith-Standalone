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
| `FileLocksmithLibInterop`（`FileLocksmith.cpp` / `NtdllBase` / `NtdllExtensions` / `NativeMethods`） | **核心算法**：枚举全系统句柄 → 匹配文件内核路径 → 反查进程/用户 | ✅ 保留（剥掉 `pch.h` 里的 C++/WinRT 依赖，`FILELOCKSMITH_LIB_STATIC` 那条分支本来就不需要它；2.0.3 又在并发细节上做了加固，见第 2 节） |
| `FileLocksmithCLI` | 命令行前端 | ✅ 保留设计，重写：去掉 spdlog、PowerToys 遥测、C++/WinRT json，改自带参数解析 + 手写 JSON |
| `FileLocksmithUI`（WinUI 3 + Windows App SDK） | 图形界面 | ✅ 保留功能，界面换成 **Win32 原生控件**（ListView + 按钮 + 拖放），彻底摆脱 Windows App SDK |
| `FileLocksmithExt`（`IExplorerCommand` 外壳扩展 DLL） | 右键菜单 | ❌ 去掉，改用 **HKCU 注册表 verb**（无需 MSIX、无需管理员、无需注册 DLL） |
| `FileLocksmithContextMenu`（稀疏包 `AppxManifest.xml`） | Win11 一级右键菜单 | ❌ 去掉（见第 6 节：要复刻它就必须做 MSIX 打包） |
| `FileLocksmithLib`（`Settings` / `Trace` / `IPC` / `Constants`） | 模块设置、遥测、UI 间通信 | ❌ 去掉：遥测不需要、GPO 不需要、设置文件不需要；IPC 用自实现的"单实例 + `WM_COPYDATA`"替代 |

**真正的技术内核只有约 400 行 C++**（`NtdllExtensions` 里的句柄枚举 + 反挂起保护），
其余全是 PowerToys 的基础设施（日志、设置、遥测、打包、UI 框架）。
所以"抽出来做 exe"是个很划算的重构。

---

## 2. 核心原理（保留原设计，未改语义）

1. `NtQuerySystemInformation(SystemExtendedHandleInformation)` 拿到全系统句柄表；
2. 逐个 `DuplicateHandle` 到本进程，用 `NtQueryObject(ObjectTypeInformation)` 判断类型是不是 `File`；
3. 是文件就用 `NtQueryObject(ObjectNameInformation)` 拿到**内核路径**（`\Device\HarddiskVolume3\...`）；
4. `CreateFileW(path, ...)` 打开用户给的路径，同样取内核路径，做**归一化匹配**（目录还会做前缀匹配，于是"某个目录被占用"也能查出来）；
5. 反过来枚举进程（`SystemProcessInformation`）+ 各进程加载的模块（`EnumProcessModules`），模块被占用也算；
6. `OpenProcessToken` + `LookupAccountSid` 拿到占用者用户名。

**关键保护（机制保留，2.0.3 起加固）**：第 3 步的 `NtQueryObject` 在个别句柄上会永久卡死，且没有超时参数。
原实现把这一轮循环丢到独立线程里跑，主线程每 200 ms 检查进度；如果 200 ms 内进度没增长，
就 `TerminateThread` 干掉它并跳过该句柄继续。这是这个工具能在真有问题的机器上跑完的原因，
**不要为了"优雅"把它删掉**。

配套加固（2.0.3）：既然这条路径上会杀线程，工作线程就**不允许写任何共享容器** —— 进程句柄表改成
"起线程前一次性打开（有序 POD 数组 + 只读二分查找）"，结果向量预留容量避开扩容重分配窗口；
`NtQueryObject` 名字缓冲区不足（超长路径）时换局部缓冲区重试，不再静默漏句柄。

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
│   ├── win_common.h            # 公共头 + MinGW 需要的 NT 兼容定义 + load_proc（GetProcAddress 中转）
│   ├── ntdll_base.{h,cpp}      # ntdll 函数指针封装
│   ├── ntdll_extensions.{h,cpp}# 句柄/进程/用户枚举（源自 PowerToys；含反挂起 + 2.0.3 并发加固）
│   ├── file_locksmith.{h,cpp}  # 匹配/解锁/删除等动作（源自 PowerToys 的 find_locks）
│   ├── process_result.h        # 结果结构（源自 PowerToys）
│   ├── app_utils.{h,cpp}       # 控制台/提权/结束进程等 Win32 工具
│   ├── cli.{h,cpp}             # 命令行模式
│   ├── gui.h                   # 图形界面对外入口（gui::run）
│   ├── gui_internal.h          # GUI 内部共享声明：常量、UiState、各文件函数
│   ├── gui_chrome.cpp          # 窗口/控件/布局/字体与 DPI/菜单/图标/状态栏
│   ├── gui_search.cpp          # 扫描线程、路径收集、结果填充
│   ├── gui_actions.cpp         # 结束/解锁/删除/定位/复制/文件对话框
│   ├── gui_main.cpp            # 窗口过程与程序入口
│   ├── ui_state.{h,cpp}        # 窗口位置/大小、列宽持久化（HKCU\Software\FileLocksmith\Ui）
│   ├── shell_integration.{h,cpp} # 资源管理器集成：右键菜单注册 / 「发送到」快捷方式（CLI 与 GUI 共用）
│   └── main.cpp                # 入口：自动决定命令行还是图形界面
└── dist/FileLocksmith.exe      # 本地构建产物（被 .gitignore 忽略，仓库不放二进制）
```

> 想直接拿 exe：到 [Releases](https://github.com/U707T/FileLocksmith-Standalone/releases) 下载
> `FileLocksmith-x64.exe`（CI 用 MSVC 编译，已在 Windows runner 上跑过真实占用检测自测）。

---

## 4. 怎么用

### 图形界面
- 直接双击 `FileLocksmith.exe`；把文件/文件夹**拖进窗口**或**拖到 exe 图标上**；
- **拖入应用快捷方式（.lnk）或 exe**：自动解析出目标程序，把它的**整棵进程树**（主进程 + 子进程 + 孙进程）
  列出来，`全部结束` 一次收干净 —— 适合 Chrome / VS Code 这种一主多子的程序；
- `删除文件` 按钮 —— **最多三道确认**：
  1. 确认删除：逐条列出路径（文件夹会标注"里面的内容会一起删除"），说明进回收站、
     以及"该驱动器禁用回收站时会变成永久删除"；
  2. 若因占用失败 → 单独确认"是否强制关闭占用者的句柄"，**列出将被影响的进程**并写明风险
     （可能报错/崩溃/丢数据，且不会结束进程），不想动它们可以改走「结束进程」；
  3. 仍失败 → 再问一次"是否安排重启后删除"，点明不可还原；

- **菜单栏「文件」里一键开关资源管理器右键菜单**（勾选项的状态就是当前状态），
  还能添加「发送到」入口、打开「发送到」文件夹、用对话框**添加文件…（Ctrl+O）/ 添加文件夹…**；
- 结果列表：**进程 / PID / 用户 / 占用方式 / 文件**（进程带图标；「占用方式」区分`句柄 ×N`与`模块映射`）；
  **多选**（Ctrl / Shift）后可批量结束、批量定位；**右键菜单**可结束 / 定位 / 复制进程路径 / 复制 PID / 复制整行 / 全选；
- 快捷键：`F5` 重新检查、`Delete` 结束选中的进程、`Ctrl+C` 复制整行、`Ctrl+A` 全选、`Ctrl+O` 添加文件；
- 列表里选中进程 → `结束进程`；`全部结束` 一键清理；`强制解锁` → 关句柄不杀进程（见第 5 节）；
  双击某行 → 在资源管理器中定位该文件；没选中时相关按钮是灰的；
- 扫描时会显示已用秒数；窗口位置/大小（含最大化）与列宽会记住；
 - 「帮助 → 关于」里能看到系统版本、是否管理员、右键菜单与「发送到」的当前状态；
- 权限不够（系统进程看不到/杀不掉）时点 `以管理员身份重启`。

### 命令行
```powershell
FileLocksmith.exe report.docx               # 文本列出占用者
FileLocksmith.exe report.docx --json        # JSON 输出（方便脚本处理）
FileLocksmith.exe report.docx --unlock      # 强制解除占用（关句柄，不结束进程）
FileLocksmith.exe app.lnk --kill            # 拖动快捷方式：结束该程序的整棵进程树
FileLocksmith.exe report.docx --delete      # 删除文件（进回收站；被占用时先解除占用再删）
FileLocksmith.exe report.docx --delete --no-unlock  # 同上，但绝不自动去关占用者的句柄
FileLocksmith.exe locked.sys --delete-reboot # 安排重启后删除（需要管理员）
FileLocksmith.exe C:\build --kill           # 结束所有占用该目录的进程
FileLocksmith.exe setup.exe --wait --timeout 60000   # 等文件解锁，最多 60 秒
FileLocksmith.exe --elevate report.docx     # 提权后重开
FileLocksmith.exe --register                # 添加右键菜单项（仅当前用户 HKCU）
FileLocksmith.exe --unregister              # 移除右键菜单项
FileLocksmith.exe --sendto                  # 加到「发送到」菜单（Win11 一级菜单可见）
```

### 拖入的语义

**拖放 / 打开方式 / 命令行传参 = 新的一次检查**：会清空上一次的路径与结果再扫描（只想看新的那个文件时最顺手）。
想同时查多个路径时，用「文件 → **追加文件到当前检查…（Ctrl+Shift+O）**」，或者一次多选后一起拖进来。

### 拖不进去？看这里

拖放失效几乎都是这两类原因，2.0.4 已经把管理员模式下的拖放**显式放行**：

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| 拖到窗口上没反应（鼠标显示禁止） | 程序**以管理员身份运行**时，Windows 默认禁止从中完整性进程（资源管理器）拖放到提权窗口（UIPI）。2.0.4 起提权窗口会放行 `WM_DROPFILES` / `WM_COPYDATA` / `0x0049` 三个消息，传统拖放路径应当正常工作 | 若仍拖不进来（个别系统 / 策略组合）：用「文件 → 添加文件…（Ctrl+O）/ 添加文件夹…」，或改用命令行传路径 |
| 拖到 exe 图标 / 快捷方式上没反应 | 已有的那个窗口是管理员权限时，转发路径（`WM_COPYDATA`）同样会被 UIPI 拦掉；1.2.1 之前是静默退出 | 2.0.4 起提权窗口也放行了 `WM_COPYDATA`：能送进去就直接出现在已有窗口里；送不进去才自己另开一个窗口显示结果 |

> **为什么别的软件（VS Code、Notepad++ 这类）提权后拖放救不回来？** 它们走的是 OLE drag-drop
> （`RegisterDragDrop`），UIPI 拦在 OLE 层，消息过滤无能为力；本程序走的是传统
> `DragAcceptFiles` + `WM_DROPFILES`，拦在**消息层**，可以显式放行 —— 这就是差距所在。
> 放行的只是消息通道：结束进程 / 强制解锁 / 删除这些破坏性操作仍然逐次弹窗确认，不做任何静默动作。

另外：**窗口任意位置**都能接（列表、按钮、状态栏区域都注册了拖放）；如果查询已经在跑，
拖进来的新文件会排队，等这轮结果回来立刻重查，不会被丢掉。

**模式是怎么决定的**：带任一 CLI 开关，或者父进程有控制台（即在 cmd/PowerShell 里运行）
→ 命令行模式；否则（双击、右键"打开方式"、右键菜单）→ 图形界面。
所以从资源管理器启动永远不会闪黑框。

JSON 输出**只走 stdout**（警告、进度、"已强制关闭 N 个句柄"这类说明全部走 stderr），
所以可以直接 `| ConvertFrom-Json`。`unresolved` 是"打不开、没被检查"的路径：

```json
{"processes":[{"pid":1234,"name":"foo.exe","user":"me","files":["C:\\x\\y.txt"]}],"unresolved":[]}
```

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

## 5. 强制解除占用（`--unlock` / 「强制解锁」按钮）

不结束进程，直接把**别的进程持有的、指向该文件的文件句柄**关掉，于是锁被释放。

**原理**：

```cpp
// 复制句柄的同时，把源进程里的那个句柄关掉
DuplicateHandle(hProcess, (HANDLE)remoteHandle, GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
```

需要对该进程有 `PROCESS_DUP_HANDLE` 权限 —— 自己启动的进程普通权限即可，系统进程要管理员。

**能做到什么 / 做不到什么**：

| 情况 | 结果 |
| --- | --- |
| 进程持有文件句柄（读写文件、日志、数据库文件） | ✅ 可以解除 |
| 文件被当成 DLL/EXE **映射**进进程（进程列表里有，但句柄数是 0） | ❌ 没有句柄可关，只能结束进程（界面会提示） |
| 系统进程（需要管理员） | ⚠️ 非管理员会失败并给出错误码，用 `--elevate` 或界面上的「以管理员身份重启」 |

**风险（务必知道）**：

- 关掉别人的句柄 ≈ "不结束程序就直接拔 U 盘"：那个程序之后再访问该句柄会报错，
  严重时崩溃、丢未保存的数据。界面有二次确认并写明这一点；命令行不会替你确认。
- 有些程序会立刻重新打开文件（杀毒、索引服务、同步盘），解锁可能只是暂时的。
- 只对"文件句柄"有效，改动不了别的进程的内存/映射。

**推荐顺序**：`--unlock`（最轻）→ 不行再 `--kill`（结束进程）→ 还不行就说明是映射占用或权限不足。

两个可以组合：`--unlock --kill` 会先尝试关句柄，仍然占用才结束进程。

## 6. 系统要求与兼容性

| 项 | 说明 |
| --- | --- |
| 目标版本 | `_WIN32_WINNT=0x0601`（Windows 7 SP1 起），PE 子系统版本 6.0 |
| **Windows 10** | **支持**。用到的 API 全是 Win7 就有的：`NtQuerySystemInformation`(class 64)、`NtQueryObject`、`DuplicateHandle`、`EnumProcessModules`、`LookupAccountSid`、`OpenProcessToken`；manifest 里声明了 Win8.1/Win10/Win11 的 `supportedOS` GUID |
| Windows 11 | 支持 |
| 架构 | x64（ARM64 设备走 x64 模拟；也可自行用 llvm-mingw 编 ARM64） |
| 运行依赖 | 无。静态链接 CRT，不需要 .NET / Windows App SDK / VC++ 运行时 |
| 实测环境 | Windows Server 2022（10.0.20348，CI 每个版本都跑）+ **Windows 10 22H2（10.0.19045）真机**（2026-10-10，v2.0.4，一键自测 11/11 全过）；`--version` 会打印它所在系统的版本号 |

> CI 已经**不再提供 Windows 10 的 runner**（`windows-2019` 已下线，只剩 Server 2022/2025 和 Win11-ARM），
> 所以"适配 Win10"的依据是：目标版本/子系统版本声明 + 只用 Win7 起的 API + manifest 兼容性声明 +
> **真机验证**（2026-10-10 在 Windows 10 22H2 上跑通 `tools\run-local-tests.ps1` 全部 11 个用例），
> 并且 CI 会守住 PE/manifest 那几条（`Smoke test` 里断言 PE 机器类型、子系统版本、supportedOS GUID）。
> 真机若在 Win10 上出问题，`--version` 的输出（含系统版本）可以直接贴进 issue。

## 7. 自己编译

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

## 8. 关于右键菜单（和原版的最大差别）

**怎么开、怎么关**：打开 exe → 菜单栏「**文件 → 添加到资源管理器右键菜单**」，勾选状态就是当前状态；
再点一次就是关闭。命令行对应 `--register` / `--unregister`。
另外「文件 → 添加到「发送到」菜单」会在 `%APPDATA%\Microsoft\Windows\SendTo` 放一个快捷方式 ——
Win11 的「发送到」在**一级菜单**里，比传统 verb 少点一步。

- 写入的是 `HKCU\Software\Classes\...`（传统 verb），**只影响当前用户、不需要管理员**：
  - `*\shell\FileLocksmith` —— 任意文件
  - `Directory\shell\FileLocksmith` —— 文件夹
  - `Drive\shell\FileLocksmith` —— 驱动器
  - `Directory\Background\shell\FileLocksmith` —— 目录空白处（命令用 `%V` 取当前目录）
- Windows 10 直接可见；**Windows 11 需要点"显示更多选项"（Shift+F10）**才会出现在经典菜单里。
- PowerToys 原版能出现在 Win11 **一级菜单**，靠的是 `IExplorerCommand` + **稀疏 MSIX 包**
  （`FileLocksmithContextMenu/AppxManifest.xml`）。那条路的代价是：
  必须打包签名、必须安装包、必须部署外壳扩展 DLL——一个裸 exe 做不到，
  而且 `IExplorerCommand` 不允许"单文件绿色分发"。想要一级菜单就只能做 MSIX。
- 另一个零成本的替代方案：给 exe 建快捷方式放到 `%APPDATA%\Microsoft\Windows\SendTo`，
  然后在"发送到"里使用。

---

## 9. 体积与依赖对比

| | 原版模块 | 本独立版 |
| --- | --- | --- |
| 界面框架 | WinUI 3 / Windows App SDK（按官方配置是 **self-contained** 部署，运行时几十 MB 起） | Win32 原生控件（系统自带） |
| 运行依赖 | Windows App SDK 运行时、VC++ 运行时、MSIX 外壳扩展 | 仅系统 DLL（kernel32/user32/shell32/advapi32/comctl32），不需要 VC++ 运行时 |
| 分发 | 安装包 + 签名 + 注册外壳扩展 | 单个 exe，拷走就能用 |
| 进程间通信 | 命名管道/临时文件 + 两个进程 | 单实例 + `WM_COPYDATA` |

---

## 10. 已知限制

1. **提权才能看全**：非管理员下，部分系统级进程的句柄 `OpenProcess`/`DuplicateHandle` 会失败，
   结果会偏少。程序启动时会尝试开启 `SeDebugPrivilege`，但不是万能药；界面上有"以管理员身份重启"。
2. **`TerminateThread` 是有意为之**：见第 2 节。它是"不安全的"API，但在 `NtQueryObject` 卡死时
   没有别的办法，原版 PowerToys 就是这么做的。2.0.3 已把"工作线程会碰到的共享状态"压到最小
   （进程句柄预开只读 + 结果向量预留容量），但仍有一个 Windows 固有性质躲不掉：
   线程被杀的那一刻若正持着堆锁，后续内存分配可能阻塞 —— 这是该机制的已知代价。
3. **枚举有开销**：每次查询要遍历全系统句柄表，**并且为了查出"被当成 DLL/EXE 加载"的情况，
   还会对每个进程枚举模块**（`EnumProcessModules` + 逐个取模块路径）。进程多、模块多的机器上可能几秒
   （GUI 里放在后台线程，不会卡界面；2.0.3 起 `--wait` 的轮询会跳过模块枚举，只有判定"解锁"时才完整查一次）。
4. **结束进程 = `TerminateProcess`**：没有"保存数据"的优雅退出，界面上有二次确认。
   「强制解锁」同样有副作用（关别人的句柄），见第 5 节。
5. **图形界面没有自动化测试**：CI 只覆盖命令行与核心逻辑（真机端到端），
   按钮点击、拖放、列表交互这些只能靠手点 —— 「打开所在位置」按钮的 ID 串位 bug 就是这么漏出去的，
   1.1.0 已修（见 CHANGELOG）。
6. **不写配置文件**：没有设置文件、没有日志、没有遥测（这也是它能缩到 1 MB 的原因）。
   唯一持久化的数据是窗口位置/大小与列宽，存在 `HKCU\Software\FileLocksmith\Ui`（`ui_state`），
   删掉它不影响任何功能。
7. **搜索状态机还没有单元测试**：`start_search` / 过期结果丢弃 / 排队重扫这套逻辑目前埋在 GUI 里，
   只能靠时序推演和手点验证 —— 2.0.1 的拖放死锁正是这里漏了一次状态复位造成的
   （编译器与 CI 都抓不到）。计划把它抽成纯函数（输入：新请求/结果到达，输出：是否渲染/是否重扫）
   并加进 CI 用例。
8. **运行时验证**：开发沙箱是 Linux(aarch64)，编不出也跑不了 Windows 程序，所以真机验证
   由两处承担 —— CI 的 `windows-2022` job（11 个端到端用例）与真机一键自测
   （2026-10-10 在 Windows 10 22H2 上 11/11 全过）。GUI 窗口的交互（点击、拖放）两者都覆盖不到，
   有问题请提 issue。

---

## 11. CI 与发布

`.github/workflows/ci.yml` 在 push 到 `main` 后自动跑：

| Job | 环境 | 干什么 |
| --- | --- | --- |
| `build-msvc` | `windows-2022` | CMake + VS 2022 编译 x64 → **功能自测** → 上传 `FileLocksmith-x64.exe` |
| `build-mingw` | `ubuntu-latest` | MinGW-w64 交叉编译 → 上传 `FileLocksmith-x64-mingw.exe` |
| `release` | `ubuntu-latest` | 若 `VERSION` 对应的 tag 还不存在 → 创建 Release 并附上两个 exe |

**Windows runner 上的功能自测是真的端到端验证**（本地 Linux 沙箱跑不了 Windows 程序），一共 **11 个用例**：

1. 检出占用者（本进程用 `FileShare::None` 独占打开临时文件 → 断言输出里有该 PID）；
2. 打不开的路径进 `unresolved`，不能被当成"没有占用"；
3. `--kill` 结束占用进程；
4. `--unlock` 关句柄但不结束进程（断言文件能打开了、进程还活着）；
5. `--json` 的 stdout 只剩一行合法 JSON（`已强制关闭 …` 这类说明全在 stderr）；
6. 右键菜单注册 / 移除（含 `%V` 与 `%1` 的区分）；
7. 「发送到」快捷方式创建（走 IShellLink COM）；
8. 没有占用时不误报；
9. 快捷方式 → 展开整棵进程树 → 全部结束（含子进程）；
10. `--delete`：被占用的文件先关句柄再删除，**占用进程不被杀**；
11. `--delete --no-unlock`：不碰别人的句柄，失败即失败。

另有 `--version` 与 manifest / PE 兼容性断言。本机跑的是同一套：`tools\run-local-tests.ps1`。

发版流程（与本工作区其他项目一致）：

```bash
# 1. 改版本号（唯一来源）
echo "2.0.5" > VERSION          # 带 -rc 后缀（如 2.0.5-rc1）会自动发成 prerelease
# 2. CHANGELOG.md 顶部加一段
# 3. 推送 → CI 全绿 → 自动创建 Release
git add -A && git commit -m "chore: 发布 2.0.5" && git push origin main
```

tag 已存在时 `release` job 只编译不重发，避免重复发布。

## 12. 怎么测试

### 一键自测（与 CI 同一套用例）

```powershell
# 解压后，在含 exe 的目录里：
pwsh -File tools\run-local-tests.ps1 -Exe .\FileLocksmith-x64.exe
# 若报 "SecurityError: …未进行数字签名"（执行策略为 AllSigned），给这次运行放行：
pwsh -ExecutionPolicy Bypass -File tools\run-local-tests.ps1 -Exe .\FileLocksmith-x64.exe
# Windows PowerShell 5.1 也行：
powershell -ExecutionPolicy Bypass -File tools\run-local-tests.ps1
```

脚本会自己造"被独占占用的文件"、起测试进程、跑 **11 个用例**（与 CI 同一套），最后打印通过/失败数；
全部通过退出码 0，有失败退出码 1。它只动自己起的测试进程和 `%TEMP%\fl-local-test`，
右键菜单与「发送到」快捷方式是**临时注册后立刻清理**。

> ⚠️ 别用管理员权限跑这个脚本：一是没必要，二是提权后能检测到/能结束的进程范围会变大，
> 用例里"权限不足"那类预期可能变样。想专门测提权路径，手工跑 `FileLocksmith-x64.exe --elevate <文件>`，
> 以及第 12 节图形界面清单里的"管理员模式拖放"一条。

### 手工验证清单

命令行（cmd，`FileLocksmith-x64.exe` 换成你的路径）：

| 步骤 | 命令 | 期望 |
| --- | --- | --- |
| 造一个被占用的文件 | `powershell -NoProfile -Command "$f=[IO.File]::Open('D:\test\locked.txt','Open','ReadWrite','None'); Read-Host '占用中，按回车释放'"` | 窗口挂着不关 |
| 检出占用者 | `FileLocksmith-x64.exe D:\test\locked.txt` | 表格里出现那个 powershell 的 PID |
| 结束占用者 | `FileLocksmith-x64.exe D:\test\locked.txt --kill` | 打印"已结束进程 …"，占用窗口消失 |
| 强制解锁 | 重新造占用后 `FileLocksmith-x64.exe D:\test\locked.txt --unlock` | 打印"已强制关闭 1/1 个文件句柄"，**占用窗口还活着**，文件能打开了 |
| 等待解锁 | `FileLocksmith-x64.exe D:\test\locked.txt --wait --timeout 30000` | 你在另一个窗口按回车释放后，它打印"文件已解锁"；不释放则 30 秒后超时 |
| 无效路径 | `FileLocksmith-x64.exe D:\nope.txt` | 警告 + "没有任何路径被成功检查"（不能说"没有占用"） |
| 脚本友好 | `FileLocksmith-x64.exe D:\test\locked.txt --json \| ConvertFrom-Json` | 能解析成功，说明 stdout 里只有 JSON |
| 提权 | `FileLocksmith-x64.exe --elevate D:\test\locked.txt` | UAC → 新控制台窗口，且"管理员权限: 是" |

图形界面：

- 双击 exe → 把文件**拖到列表区域**（重点：1.2.1 之前这里没反应）、拖到按钮上、拖到状态栏上都要能接；
- 拖到 **exe 图标 / 快捷方式**上：已有窗口的话应该合并到同一个窗口，没开的话新开一个；
- **管理员模式（2.0.4 起应可用）**：右键 exe →「以管理员身份运行」，再拖文件进窗口应能接住；
  保持该窗口打开，把第二个文件拖到 exe 图标上应合并进已有窗口（而不是新开）；
  若无效请回报系统版本 —— 退路是「文件 → 添加文件…」；
- 「文件 → 添加文件…（Ctrl+O）/ 添加文件夹…」能选多选文件；
- 「文件 → 添加到资源管理器右键菜单」勾上后，去资源管理器右键验证（Win10 在一级菜单，Win11 在"显示更多选项"）；
- 选中进程 → `结束进程` / `强制解锁`（后者会弹风险确认）；
- 「帮助 → 关于」能看到系统版本、是否管理员、两个集成的状态；
- 有第二块屏且缩放比不同的话，把窗口拖过去，字体和布局应该跟着变（1.2.1 修复）。

### 反馈问题时请带上

1. `FileLocksmith-x64.exe --version` 的完整输出（含系统版本与是否管理员）；
2. 你执行的完整命令 / 在界面上点的按钮；
3. 看到的现象（报错文字、截图）；
4. 如果是一键脚本失败，直接把脚本输出的**全部内容**贴出来。

## 13. 许可

衍生自 Microsoft PowerToys（MIT License，Copyright (c) Microsoft Corporation），
完整许可证见 `LICENSE-PowerToys-MIT.txt`。保留原版权声明即可自由用于商业/非商业项目。
