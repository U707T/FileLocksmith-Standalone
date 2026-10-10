# Changelog

## 1.1.0 (2026-10-10)

### 新增
- **强制解除占用**（`--unlock` / 界面上的「强制解锁」按钮）：不结束进程，直接关闭别的进程
  持有的、指向目标文件的文件句柄。实现是 `DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE)`，
  需要 `PROCESS_DUP_HANDLE`（系统进程要管理员）。CI 上有端到端用例：断言文件确实解锁了、
  而持有它的进程仍然活着。
- 区分「枚举失败」和「没有占用」：句柄枚举失败时会明确警告，不再把查询失败说成"没进程占用"。
- `--version` 现在会打印系统版本（如 `Windows 10 22H2 (10.0.19045)`）与是否管理员，方便排障。
- CI 冒烟测试新增兼容性断言：manifest 必须声明 Win8.1/10/11 supportedOS、PE 必须是 x64、
  子系统版本不得高于 6.0（否则老系统直接拒绝加载）。

### 修复
- **按钮 ID 串位**：图形界面按 `IDC_KILL_SELECTED + 序号` 分配控件 ID，顺序和常量对不上，
  导致点「打开所在位置」实际触发「以管理员身份重启」，反之亦然。现改为按表显式声明。
- `--unlock`/`--kill` 后重新查询失败时不再丢掉已有结果。

## 1.0.1 (2026-10-10)

### 修复
- MSVC 构建改为静态链接 C 运行时（`/MT`）。1.0.0 的 `FileLocksmith-x64.exe` 依赖
  `MSVCP140.dll` / `VCRUNTIME140.dll`（需要装 VC++ 2015-2022 运行时），
  现在两个产物都只依赖系统 DLL。
- CI 的功能自测改用 `Start-Process` 捕获输出：PowerShell 7 会把 Windows 子系统的
  exe 当图形程序，`& exe args` 不等待也不捕获输出。

### 说明
- 1.0.0 的 release 保留，作为首次通过 CI 端到端验证的版本。

## 1.0.0 (2026-10-10)

首个版本：把 PowerToys 的 **FileLocksmith**（"什么正在占用这个文件？"）抽成一个不依赖
PowerToys / Windows App SDK / .NET 的单文件 exe。

### 核心
- 逐行保留原版句柄枚举实现：`NtQuerySystemInformation(SystemExtendedHandleInformation)`
  → `DuplicateHandle` → `NtQueryObject` 取类型与内核路径，再反查占用进程与用户名。
- 保留了原版针对 `NtQueryObject` 卡死的保护：枚举循环放在独立线程，主线程每 200 ms
  检查进度，200 ms 内无进展就 `TerminateThread` 跳过该句柄。
- 目录也支持：传目录时按内核路径前缀匹配，能查出"谁占用了这个目录/里面的文件"。
- 被进程以 DLL/模块形式加载的文件同样会被列出。

### 新增（相对原版模块）
- Win32 原生图形界面（替代 WinUI 3 + Windows App SDK）：进程列表 + 结束进程 / 全部结束 /
  重新检查 / 打开所在位置 / 以管理员身份重启，支持把文件拖进窗口。
- 单实例 + `WM_COPYDATA`：右键多选文件时不开一堆窗口，路径汇总到同一个窗口。
- 命令行：`--json`、`--kill`、`--wait --timeout <ms>`、`--elevate`、`--register` / `--unregister`。
- 混合子系统：双击不闪控制台，从 cmd/PowerShell 启动时又是正常的命令行工具。
- 版本号单一来源（根目录 `VERSION`），CMake 注入到 exe 的资源信息与 `--version` 输出。

### 去掉
- PowerToys 遥测（`Trace`）、GPO 策略、模块设置文件、spdlog 日志、C++/WinRT、WinUI 3、
  `IExplorerCommand` 外壳扩展与稀疏 MSIX 包。
- 右键菜单改为 HKCU 注册表 verb（Win11 需在"显示更多选项"里查看）。

### 说明
- 首个版本在 Windows CI 上编译并跑通功能自测（真实占用检测）；此前未在真机运行验证过。
