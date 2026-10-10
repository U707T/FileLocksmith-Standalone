// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once
#include "win_common.h"
#include "ntdll_base.h"

class NtdllExtensions : protected Ntdll
{
private:
    constexpr static size_t DefaultResultBufferSize = 64 * 1024;
    constexpr static size_t MaxResultBufferSize = 1024 * 1024 * 1024;

    struct MemoryLoopResult
    {
        NTSTATUS status = 0;
        std::vector<BYTE> memory;
    };

    // Calls NtQuerySystemInformation and returns a buffer containing the result.
    MemoryLoopResult NtQuerySystemInformationMemoryLoop(ULONG SystemInformationClass);

    std::wstring file_handle_to_kernel_name(HANDLE file_handle, std::vector<BYTE>& buffer);

public:
    struct ProcessInfo
    {
        DWORD pid = 0;
        std::wstring name;
        std::wstring user;
        std::vector<std::wstring> modules;
    };

    struct HandleInfo
    {
        ULONG_PTR pid;
        ULONG_PTR handle;
        std::wstring type_name;
        std::wstring kernel_file_name;
    };

    std::wstring file_handle_to_kernel_name(HANDLE file_handle);

    std::wstring path_to_kernel_name(LPCWSTR path);

    // Gives the user name of the account running this process
    std::wstring pid_to_user(DWORD pid);

    // Returns the list of handles currently existing in the system.
    // *ok is set to false when the query itself failed (so the caller can tell
    // "cannot enumerate" apart from "nothing is holding the file").
    std::vector<HandleInfo> handles(bool* ok = nullptr) noexcept;

    // Returns the list of all processes.
    // On failure, returns an empty vector and sets *ok to false.
    std::vector<ProcessInfo> processes(bool* ok = nullptr) noexcept;
};
