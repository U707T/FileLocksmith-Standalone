// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "file_locksmith.h"
#include "ntdll_extensions.h"

#include <shellapi.h>

static bool is_directory(const std::wstring path)
{
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static constexpr bool starts_with(std::wstring_view whole, std::wstring_view part)
{
    return whole.size() >= part.size() && whole.substr(0, part.size()) == part;
}

LocksmithResult find_locks(const std::vector<std::wstring>& paths, bool include_modules)
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
        else
        {
            // 打不开就永远匹配不到句柄，必须让调用方知道这个路径压根没被检查
            result.unresolved_paths.push_back(path);
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

    // Check all modules used by processes.
    // include_modules=false 时连模块枚举都不做：processes_ex 会跳过 EnumProcessModules +
    // 每模块一次 GetModuleFileNameExW —— 那才是这一步慢的原因，--wait 的轮询靠它做廉价检查。
    auto processes = nt_ext.processes_ex(&result.processes_query_ok, include_modules);

    if (include_modules)
    {
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
    }

    for (const auto& process_info : processes)
    {
        if (auto it = pid_files.find(process_info.pid); it != pid_files.end())
        {
            result.processes.push_back(ProcessResult{
                process_info.name,
                process_info.pid,
                process_info.user,
                std::vector(it->second.begin(), it->second.end()),
                process_info.image_path });
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

std::vector<ProcessResult> find_application_processes(const std::wstring& executable, bool include_children)
{
    std::vector<ProcessResult> result;

    NtdllExtensions nt_ext;
    bool ok = false;
    auto processes = nt_ext.processes_ex(&ok, /* include_modules */ false);
    if (!ok)
    {
        return result;
    }

    auto same_path = [](const std::wstring& left, const std::wstring& right) {
        return !left.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
    };

    std::set<DWORD> pids;
    for (const auto& process : processes)
    {
        if (same_path(process.image_path, executable))
        {
            pids.insert(process.pid);
        }
    }

    if (include_children && !pids.empty())
    {
        // 反复扫直到不再增长（进程树通常只有两三层，O(n^2) 在几百个进程规模下无所谓）
        bool grew = true;
        while (grew)
        {
            grew = false;
            for (const auto& process : processes)
            {
                if (pids.count(process.parent_pid) != 0 && pids.count(process.pid) == 0)
                {
                    pids.insert(process.pid);
                    grew = true;
                }
            }
        }
    }

    for (const auto& process : processes)
    {
        if (pids.count(process.pid) != 0)
        {
            result.push_back(ProcessResult{ process.name, process.pid, process.user, { process.image_path }, process.image_path });
        }
    }

    return result;
}

void merge_process_results(std::vector<ProcessResult>& into, const std::vector<ProcessResult>& extra)
{
    for (const auto& item : extra)
    {
        auto it = std::find_if(into.begin(), into.end(), [&](const ProcessResult& existing) { return existing.pid == item.pid; });
        if (it == into.end())
        {
            into.push_back(item);
            continue;
        }

        for (const auto& file : item.files)
        {
            if (std::find(it->files.begin(), it->files.end(), file) == it->files.end())
            {
                it->files.push_back(file);
            }
        }
    }
}

namespace
{
    // SHFileOperation 和 MoveFileEx 都要求完整路径：相对路径在 SHFileOperation 里没有
    // 明确定义的行为（README 的示例恰好就是 `FileLocksmith.exe report.docx --delete`
    // 这种相对写法）。统一在这里补成绝对路径。
    std::wstring absolute_path(const std::wstring& path)
    {
        std::vector<wchar_t> buffer(MAX_PATH * 2);
        for (;;)
        {
            const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (length == 0)
            {
                return path;  // 解析失败：原样返回，让系统调用给出它自己的错误码
            }
            if (length < buffer.size())
            {
                return std::wstring(buffer.data(), length);
            }
            buffer.resize(length);   // 缓冲区太小时返回的是"含结尾 \0 的所需长度"
        }
    }

    std::wstring describe_shell_error(int code)
    {
        switch (code)
        {
        case 0: return {};
        case ERROR_FILE_NOT_FOUND: return L"文件不存在";
        case ERROR_PATH_NOT_FOUND: return L"路径不存在";
        case ERROR_ACCESS_DENIED: return L"拒绝访问（权限不足或被只读属性挡住）";
        case ERROR_SHARING_VIOLATION: return L"文件正被占用";
        case ERROR_DIR_NOT_EMPTY: return L"目录不是空的";
        case ERROR_DISK_FULL: return L"磁盘空间不足";
        case 0x75: return L"操作被取消";               // DE_OPCANCELLED
        case 0x78: return L"拒绝访问";                 // DE_ACCESSDENIEDSRC
        case 0x7C: return L"文件或路径无效";            // DE_INVALIDFILES
        case 0x7E: return L"目标位置被一个同名文件挡住";  // DE_FLDDESTISFILE
        case 0x81: return L"文件名太长";                // DE_FILENAMETOOLONG
        default: break;
        }

        wchar_t buffer[128];
        swprintf(buffer, 128, L"错误码 %d (0x%X)", code, code);
        return buffer;
    }
}

bool delete_paths(const std::vector<std::wstring>& paths, std::wstring& error, int* error_code)
{
    if (error_code != nullptr)
    {
        *error_code = 0;
    }

    if (paths.empty())
    {
        return true;
    }

    // SHFileOperation 要求路径列表以双 \0 结尾
    std::vector<wchar_t> list;
    for (const auto& path : paths)
    {
        const std::wstring full = absolute_path(path);
        list.insert(list.end(), full.begin(), full.end());
        list.push_back(L'\0');
    }
    list.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = list.data();
    // FOF_ALLOWUNDO：进回收站（可撤销）；其余标志位是"别弹系统 UI，我们自己确认过"
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int status = SHFileOperationW(&operation);
    if (status == 0 && !operation.fAnyOperationsAborted)
    {
        return true;
    }

    if (error_code != nullptr)
    {
        *error_code = status;
    }
    error = operation.fAnyOperationsAborted ? L"操作被取消" : describe_shell_error(status);
    return false;
}

bool delete_on_reboot(const std::vector<std::wstring>& paths, std::wstring& error)
{
    for (const auto& path : paths)
    {
        // MOVEFILE_DELAY_UNTIL_REBOOT 会把删除操作写进 PendingFileRenameOperations，
        // 需要管理员权限；而且不进回收站。
        const std::wstring full = absolute_path(path);
        if (!MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT))
        {
            const auto code = GetLastError();
            error = describe_shell_error(static_cast<int>(code));
            if (code == ERROR_ACCESS_DENIED)
            {
                error += L"（“重启后删除”需要管理员权限）";
            }
            return false;
        }
    }
    return true;
}

bool is_file_locked_error(int code)
{
    // ERROR_SHARING_VIOLATION / ERROR_ACCESS_DENIED / DE_ACCESSDENIEDSRC(0x78)：
    // 删除失败时只有这几类错误码意味着"文件被占用、重试可能有用"。
    return code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == 0x78;
}
