// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "ntdll_extensions.h"

// 公开头文件里的 SYSTEM_PROCESS_INFORMATION 并不完整：MSVC SDK 的 winternl.h 连
// InheritedFromUniqueProcessId 都没有（MinGW 的反而有）。这里按内核真实布局自己定义一份，
// 只用到前几个字段，两个工具链行为一致。8 字节对齐与内核一致。
#pragma pack(push, 8)
struct FL_SYSTEM_PROCESS_INFORMATION
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER Reserved[3];  // 覆盖 Win7/8 陆续加进来的 WorkingSetPrivateSize / HardFaultCount / CycleTime
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
};
#pragma pack(pop)

namespace
{
    std::wstring unicode_to_str(UNICODE_STRING unicode_str)
    {
        return std::wstring(unicode_str.Buffer, unicode_str.Length / sizeof(WCHAR));
    }

    // 只返回"这个进程里的这个模块"的路径。
    //
    // 这里必须用 GetModuleFileNameExW 重试放大缓冲区：
    //   * 原实现失败时会去调 GetModuleFileNameW(mod) —— mod 属于别的进程，
    //     用本进程的 API 解释它，轻则拿不到名字，重则返回我们自己的同名模块路径（误报）；
    //   * 原实现用未初始化的栈缓冲区构造字符串（wcslen(buffer)），
    //     只要函数返回 0 且 GetLastError 不是 ERROR_INSUFFICIENT_BUFFER 就会把垃圾内存当路径。
    inline std::wstring get_module_name(HANDLE process, HMODULE mod)
    {
        std::wstring buffer(MAX_PATH * 2, L'\0');
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const DWORD length = GetModuleFileNameExW(process, mod, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0)
            {
                return {};
            }
            if (length < buffer.size() - 1)
            {
                buffer.resize(length);
                return buffer;
            }
            buffer.resize(buffer.size() * 4);
        }
        return {};
    }

    constexpr size_t DefaultModulesResultSize = 512;

    struct ProcessDetails
    {
        std::wstring image_path;
        std::vector<std::wstring> modules;
    };

    // 只要镜像路径时走这条：PROCESS_QUERY_LIMITED_INFORMATION 比 VM_READ 便宜得多
    std::wstring process_image_path(DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
        {
            return {};
        }

        std::wstring buffer(MAX_PATH * 2, L'\0');
        DWORD size = static_cast<DWORD>(buffer.size());
        std::wstring result;
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) != 0)
        {
            buffer.resize(size);
            result = std::move(buffer);
        }

        CloseHandle(process);
        return result;
    }

    // 一次 OpenProcess 同时拿到主模块路径与模块列表（比开两次省一半系统调用）
    ProcessDetails process_details(DWORD pid)
    {
        ProcessDetails details;

        HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process)
        {
            return details;
        }

        details.image_path = get_module_name(process, nullptr);

        std::vector<std::wstring>& result = details.modules;

        bool completed = false;
        std::vector<HMODULE> modules(DefaultModulesResultSize);
        while (!completed)
        {
            DWORD needed;
            auto status = EnumProcessModules(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed);

            if (!status)
            {
                // Give up
                details.modules.clear();
                CloseHandle(process);
                return details;
            }

            if (needed > modules.size() * sizeof(HMODULE))
            {
                // Array is too small
                modules.resize(needed / sizeof(HMODULE));
                continue;
            }

            // Okay
            modules.resize(needed / sizeof(HMODULE));

            for (auto mod : modules)
            {
                auto name = get_module_name(process, mod);
                if (!name.empty())
                {
                    result.push_back(std::move(name));
                }
            }

            completed = true;
        }

        CloseHandle(process);
        return details;
    }
}

NtdllExtensions::MemoryLoopResult NtdllExtensions::NtQuerySystemInformationMemoryLoop(ULONG SystemInformationClass)
{
    MemoryLoopResult result;
    result.memory.resize(DefaultResultBufferSize);

    while (result.memory.size() <= MaxResultBufferSize)
    {
        ULONG result_len;
        result.status = NtQuerySystemInformation(SystemInformationClass, result.memory.data(), static_cast<ULONG>(result.memory.size()), &result_len);

        if (result.status == STATUS_INFO_LENGTH_MISMATCH)
        {
            result.memory.resize(result.memory.size() * 2);
            continue;
        }

        if (NT_ERROR(result.status))
        {
            result.memory.clear();
        }

        return result;
    }

    result.status = STATUS_INFO_LENGTH_MISMATCH;
    result.memory.clear();
    return result;
}

std::wstring NtdllExtensions::file_handle_to_kernel_name(HANDLE file_handle, std::vector<BYTE>& buffer)
{
    if (GetFileType(file_handle) != FILE_TYPE_DISK)
    {
        return L"";
    }

    ULONG return_length;
    auto status = NtQueryObject(file_handle, ObjectNameInformationClass, buffer.data(), static_cast<ULONG>(buffer.size()), &return_length);
    if (NT_SUCCESS(status))
    {
        auto object_name_info = reinterpret_cast<UNICODE_STRING*>(buffer.data());
        return unicode_to_str(*object_name_info);
    }

    return L"";
}

std::wstring NtdllExtensions::file_handle_to_kernel_name(HANDLE file_handle)
{
    std::vector<BYTE> buffer(DefaultResultBufferSize);
    return file_handle_to_kernel_name(file_handle, buffer);
}

std::wstring NtdllExtensions::path_to_kernel_name(LPCWSTR path)
{
    HANDLE file_handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file_handle == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    auto kernel_name = file_handle_to_kernel_name(file_handle);
    CloseHandle(file_handle);
    return kernel_name;
}

std::vector<NtdllExtensions::HandleInfo> NtdllExtensions::handles(bool* ok) noexcept
{
    if (ok != nullptr)
    {
        *ok = false;
    }

    auto get_info_result = NtQuerySystemInformationMemoryLoop(SystemExtendedHandleInformationClass);
    if (NT_ERROR(get_info_result.status))
    {
        return {};
    }

    if (ok != nullptr)
    {
        *ok = true;
    }

    auto info_ptr = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(get_info_result.memory.data());

    std::map<ULONG_PTR, HANDLE> pid_to_handle;
    std::vector<HandleInfo> result;

    std::vector<BYTE> object_info_buffer(DefaultResultBufferSize);

    std::atomic<ULONG> i = 0;
    std::atomic<ULONG_PTR> handle_count = info_ptr->NumberOfHandles;
    std::atomic<HANDLE> process_handle = NULL;
    std::atomic<HANDLE> handle_copy = NULL;
    ULONG previous_i;

    while (i < handle_count)
    {
        previous_i = i;

        // The system calls we use in this block were reported to hang on some machines.
        // We need to offload the cycle to another thread and keep track of progress to terminate and resume when needed.
        // Unfortunately, there are no alternative APIs to what we're using that accept timeouts. (NtQueryObject and GetFileType)
        auto offload_function = std::thread([&] {
            for (; i < handle_count; i++)
            {
                process_handle = NULL;
                handle_copy = NULL;

                auto handle_info = info_ptr->Handles + i;
                auto pid = handle_info->UniqueProcessId;

                auto iter = pid_to_handle.find(pid);
                if (iter != pid_to_handle.end())
                {
                    process_handle = iter->second;
                }
                else
                {
                    process_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, static_cast<DWORD>(pid));
                    if (!process_handle)
                    {
                        continue;
                    }
                    pid_to_handle[pid] = process_handle;
                }

                // NtQueryObject can hang on some handles.
                HANDLE local_handle_copy;
                auto dh_result = DuplicateHandle(process_handle, reinterpret_cast<HANDLE>(handle_info->HandleValue), GetCurrentProcess(), &local_handle_copy, 0, 0, DUPLICATE_SAME_ACCESS);
                if (dh_result == 0)
                {
                    // Ignore this handle.
                    continue;
                }
                handle_copy = local_handle_copy;

                ULONG return_length;
                auto status = NtQueryObject(handle_copy, ObjectTypeInformationClass, object_info_buffer.data(), static_cast<ULONG>(object_info_buffer.size()), &return_length);
                if (NT_ERROR(status))
                {
                    // Ignore this handle.
                    CloseHandle(handle_copy);
                    handle_copy = NULL;
                    continue;
                }

                // OBJECT_TYPE_INFORMATION starts with the type name; the public
                // headers call it TypeName, the kernel calls it Name, but the
                // layout is the same, so reading a UNICODE_STRING at offset 0
                // works for both.
                auto object_type_info = reinterpret_cast<UNICODE_STRING*>(object_info_buffer.data());
                auto type_name = unicode_to_str(*object_type_info);

                std::wstring file_name;

                if (type_name == L"File")
                {
                    file_name = file_handle_to_kernel_name(handle_copy, object_info_buffer);
                    result.push_back(HandleInfo{ pid, handle_info->HandleValue, type_name, file_name });
                }

                CloseHandle(handle_copy);
                handle_copy = NULL;
            }
        });

        offload_function.detach();
        do
        {
            Sleep(200); // Timeout in milliseconds for detecting that the system hangs on getting information for a handle.
            if (i >= handle_count)
            {
                // We're done.
                break;
            }

            if (previous_i >= i)
            {
                // The thread looks like it's hanging on some handle. Let's kill it and resume.

                // HACK: This is unsafe and may leak something, but looks like there's no way to properly clean up a thread when it's hanging on a system call.
                TerminateThread(offload_function.native_handle(), 1);

                // Close Handles that might be lingering.
                if (handle_copy != NULL)
                {
                    CloseHandle(handle_copy);
                }
                i++;
                break;
            }
            previous_i = i;
        } while (1);
    }

    for (auto [pid, handle] : pid_to_handle)
    {
        CloseHandle(handle);
    }

    return result;
}

std::wstring NtdllExtensions::pid_to_user(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring user;
    std::wstring domain;

    if (process == nullptr)
    {
        return user;
    }

    HANDLE token = nullptr;

    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
    {
        CloseHandle(process);
        return user;
    }

    DWORD token_size = 0;
    const bool ok = GetTokenInformation(token, TokenUser, nullptr, 0, &token_size);
    if ((!ok && GetLastError() != ERROR_INSUFFICIENT_BUFFER) || !token_size)
    {
        CloseHandle(token);
        CloseHandle(process);
        return user;
    }

    std::vector<BYTE> token_buffer(token_size);
    GetTokenInformation(token, TokenUser, token_buffer.data(), token_size, &token_size);
    TOKEN_USER* user_ptr = reinterpret_cast<TOKEN_USER*>(token_buffer.data());
    PSID psid = user_ptr->User.Sid;
    DWORD user_buf_size = 0;
    DWORD domain_buf_size = 0;
    SID_NAME_USE sid_name;
    LookupAccountSidW(nullptr, psid, nullptr, &user_buf_size, nullptr, &domain_buf_size, &sid_name);
    if (!user_buf_size || !domain_buf_size)
    {
        CloseHandle(token);
        CloseHandle(process);
        return user;
    }

    user.resize(user_buf_size);
    domain.resize(domain_buf_size);
    if (LookupAccountSidW(nullptr, psid, user.data(), &user_buf_size, domain.data(), &domain_buf_size, &sid_name))
    {
        user.resize(user.size() - 1);
        domain.resize(domain.size() - 1);
    }
    else
    {
        user.clear();
    }
    CloseHandle(token);
    CloseHandle(process);

    return user;
}

std::vector<NtdllExtensions::ProcessInfo> NtdllExtensions::processes(bool* ok) noexcept
{
    return processes_ex(ok, /* include_modules */ true);
}

std::vector<NtdllExtensions::ProcessInfo> NtdllExtensions::processes_ex(bool* ok, bool include_modules) noexcept
{
    if (ok != nullptr)
    {
        *ok = false;
    }

    auto get_info_result = NtQuerySystemInformationMemoryLoop(SystemProcessInformation);

    if (NT_ERROR(get_info_result.status))
    {
        return {};
    }

    if (ok != nullptr)
    {
        *ok = true;
    }

    std::vector<ProcessInfo> result;
    auto info_ptr = reinterpret_cast<FL_SYSTEM_PROCESS_INFORMATION*>(get_info_result.memory.data());

    while (info_ptr->NextEntryOffset)
    {
        info_ptr = reinterpret_cast<decltype(info_ptr)>(reinterpret_cast<LPBYTE>(info_ptr) + info_ptr->NextEntryOffset);

        ProcessInfo item;
        item.name = unicode_to_str(info_ptr->ImageName);
        item.pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(info_ptr->UniqueProcessId));
        item.parent_pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(info_ptr->InheritedFromUniqueProcessId));

        if (include_modules)
        {
            auto details = process_details(item.pid);
            item.image_path = std::move(details.image_path);
            item.modules = std::move(details.modules);
        }
        else
        {
            item.image_path = process_image_path(item.pid);
        }

        item.user = pid_to_user(item.pid);

        result.push_back(item);
    }

    return result;
}
