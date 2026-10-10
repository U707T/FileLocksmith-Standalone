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
};

// Second version, checks handles towards files and all subfiles and folders of given dirs, if any.
LocksmithResult find_locks(const std::vector<std::wstring>& paths);

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

// Gives the full path of the executable, given the process id
std::wstring pid_to_full_path(DWORD pid);
