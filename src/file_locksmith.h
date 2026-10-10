// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once
#include "process_result.h"

// 一个指向被查询文件的文件句柄（用于"强制解除占用"）。
struct LockedHandle
{
    DWORD pid = 0;
    ULONG_PTR handle = 0;
    std::wstring path;         // 归一化后的用户路径
    std::wstring kernel_name;  // 内核路径（\Device\...）
};

struct LocksmithResult
{
    std::vector<ProcessResult> processes;
    // 指向目标文件的句柄。"强制解除占用"会逐个关掉它们。
    std::vector<LockedHandle> handles;
    // 枚举是否成功。false 表示"查不到"，而不是"没有占用" —— 调用方必须区别对待。
    bool handles_query_ok = false;
    bool processes_query_ok = false;
    // 传进来但没法解析的路径（不存在 / 没权限 / 不是本地磁盘上的对象）。
    // 这些路径不可能匹配到任何句柄，必须单独报出来，否则用户会以为"没有占用"。
    std::vector<std::wstring> unresolved_paths;
};

// Second version, checks handles towards files and all subfiles and folders of given dirs, if any.
//
// include_modules=false 时跳过"枚举每个进程的模块"这一步：慢的那部分就是它。
// --wait 的轮询用它做廉价检查，真正判定解锁前再跑一次完整的。
LocksmithResult find_locks(const std::vector<std::wstring>& paths, bool include_modules = true);

// 兼容接口：只要进程列表。
std::vector<ProcessResult> find_processes_recursive(const std::vector<std::wstring>& paths);

// 强制解除占用：把**别的进程**里指向目标文件的句柄关掉，但不结束那个进程。
//
// 原理是 DuplicateHandle(..., DUPLICATE_CLOSE_SOURCE)：把源进程的句柄复制到本进程
// 的同时关掉源句柄，于是文件锁被释放。需要对该进程有 PROCESS_DUP_HANDLE 权限。
// 代价：目标进程里那个句柄会变成悬空值，它下次使用该句柄时可能出错甚至崩溃。
//
// 返回成功关闭的句柄数；failures 里是每个失败的原因。
size_t release_handles(const std::vector<LockedHandle>& handles, std::vector<std::wstring>& failures);

// 把一个 exe 路径展开成"这个程序的所有进程"：镜像路径匹配的进程，
// include_children=true 时再加上它们的子进程、孙进程……
std::vector<ProcessResult> find_application_processes(const std::wstring& executable, bool include_children);

// 把 extra 里的进程合并进 into（按 PID 去重，files 取并集）
void merge_process_results(std::vector<ProcessResult>& into, const std::vector<ProcessResult>& extra);

// 删除路径（进回收站，可撤销）。部分失败时返回 false 并给出原因。
bool delete_paths(const std::vector<std::wstring>& paths, std::wstring& error, int* error_code = nullptr);

// 安排"重启后删除"（不进回收站；需要管理员权限）
bool delete_on_reboot(const std::vector<std::wstring>& paths, std::wstring& error);

