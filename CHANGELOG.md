# Changelog

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
