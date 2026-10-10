// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "file_locksmith.h"
#include "ntdll_extensions.h"

static bool is_directory(const std::wstring path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static constexpr bool starts_with(std::wstring_view whole, std::wstring_view part)
{
    return whole.size() >= part.size() && whole.substr(0, part.size()) == part;
}

LocksmithResult find_locks(const std::vector<std::wstring>& paths)
{
    NtdllExtensions nt_ext;

    LocksmithResult result;

    // This maps kernel names of files within `paths` to their normal paths.
    std::map<std::wstring, std::wstring> kernel_names_files;

    // This maps kernel names of directories within `paths` to their normal paths.
    std::map<std::wstring, std::wstring> kernel_names_dirs;

    for (const auto& path : paths)
    {
        auto kernel_path = nt_ext.path_to_kernel_name(path.c_str());
        if (!kernel_path.empty())
        {
            (is_directory(path) ? kernel_names_dirs : kernel_names_files)[kernel_path] = path;
        }
    }

    std::map<ULONG_PTR, std::set<std::wstring>> pid_files;

    // Returns a normal path of the file specified by kernel_name, if it matches
    // the search criteria. Otherwise, return an empty string.
    auto kernel_paths_contain = [&](const std::wstring& kernel_name) -> std::wstring
    {
        // Normal equivalence
        if (auto it = kernel_names_files.find(kernel_name); it != kernel_names_files.end())
        {
            return it->second;
        }

        if (auto it = kernel_names_dirs.find(kernel_name); it != kernel_names_dirs.end())
        {
            return it->second;
        }

        for (const auto& [dir_kernel_name, dir_path] : kernel_names_dirs)
        {
            if (starts_with(kernel_name, dir_kernel_name + (dir_kernel_name.length() > 0 && dir_kernel_name[dir_kernel_name.length() - 1] != L'\\' ? L"\\" : L"")))
            {
                return dir_path + kernel_name.substr(dir_kernel_name.size());
            }
        }

        return {};
    };

    for (const auto& handle_info : nt_ext.handles(&result.handles_query_ok))
    {
        if (handle_info.type_name == L"File")
        {
            auto path = kernel_paths_contain(handle_info.kernel_file_name);
            if (!path.empty())
            {
                pid_files[handle_info.pid].insert(path);
                result.handles.push_back(LockedHandle{
                    static_cast<DWORD>(handle_info.pid),
                    handle_info.handle,
                    path,
                    handle_info.kernel_file_name });
            }
        }
    }

    // Check all modules used by processes
    auto processes = nt_ext.processes(&result.processes_query_ok);

    for (const auto& process : processes)
    {
        for (const auto& path : process.modules)
        {
            auto kernel_name = nt_ext.path_to_kernel_name(path.c_str());

            auto found_path = kernel_paths_contain(kernel_name);
            if (!found_path.empty())
            {
                pid_files[process.pid].insert(std::move(found_path));
            }
        }
    }

    for (const auto& process_info : processes)
    {
        if (auto it = pid_files.find(process_info.pid); it != pid_files.end())
        {
            result.processes.push_back(ProcessResult{
                process_info.name,
                process_info.pid,
                process_info.user,
                std::vector(it->second.begin(), it->second.end()) });
        }
    }

    return result;
}

std::vector<ProcessResult> find_processes_recursive(const std::vector<std::wstring>& paths)
{
    return find_locks(paths).processes;
}

namespace
{
    std::wstring to_hex(ULONG_PTR value)
    {
        const wchar_t* digits = L"0123456789abcdef";
        std::wstring text;
        bool started = false;
        for (int shift = static_cast<int>(sizeof(ULONG_PTR) * 8) - 4; shift >= 0; shift -= 4)
        {
            auto nibble = static_cast<unsigned>((value >> shift) & 0xF);
            if (nibble != 0 || started || shift == 0)
            {
                started = true;
                text.push_back(digits[nibble]);
            }
        }
        return L"0x" + text;
    }
}

size_t release_handles(const std::vector<LockedHandle>& handles, std::vector<std::wstring>& failures)
{
    size_t closed = 0;

    // 同一个进程只 OpenProcess 一次
    std::map<DWORD, HANDLE> process_cache;
    // 打开失败的进程只报一次错（同一个 PID 的多个句柄不必刷屏）
    std::map<DWORD, DWORD> open_errors;

    for (const auto& item : handles)
    {
        HANDLE process = nullptr;
        if (auto it = process_cache.find(item.pid); it != process_cache.end())
        {
            process = it->second;
        }
        else
        {
            process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, item.pid);
            process_cache[item.pid] = process;
            if (process == nullptr)
            {
                open_errors[item.pid] = GetLastError();
            }
        }

        if (process == nullptr)
        {
            if (auto it = open_errors.find(item.pid); it != open_errors.end() && it->second != 0)
            {
                failures.push_back(L"PID " + std::to_wstring(item.pid) + L": 无法打开进程 (错误码 " +
                                   std::to_wstring(it->second) + L")，需要管理员权限？");
                it->second = 0;
            }
            continue;
        }

        // DUPLICATE_CLOSE_SOURCE 会在复制的同时关掉源进程里的那个句柄，
        // 于是它持有的文件锁就被释放了 —— 进程本身不受影响（但那个句柄值作废）。
        HANDLE duplicate = nullptr;
        if (DuplicateHandle(process,
                            reinterpret_cast<HANDLE>(item.handle),
                            GetCurrentProcess(),
                            &duplicate,
                            0,
                            FALSE,
                            DUPLICATE_CLOSE_SOURCE))
        {
            if (duplicate != nullptr)
            {
                CloseHandle(duplicate);
            }
            ++closed;
        }
        else
        {
            failures.push_back(L"PID " + std::to_wstring(item.pid) + L" 句柄 " + to_hex(item.handle) +
                               L": 关闭失败 (错误码 " + std::to_wstring(GetLastError()) + L")");
        }
    }

    for (auto& [pid, process] : process_cache)
    {
        if (process != nullptr)
        {
            CloseHandle(process);
        }
    }

    return closed;
}

constexpr size_t LongMaxPathSize = 65536;

std::wstring pid_to_full_path(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);

    std::wstring result(LongMaxPathSize, L'\0');

    // Returns zero on failure, so it's okay to resize to zero.
    auto length = GetModuleFileNameExW(process, NULL, result.data(), static_cast<DWORD>(result.size()));
    result.resize(length);

    CloseHandle(process);
    return result;
}
