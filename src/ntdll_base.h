// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once
#include "win_common.h"

#define DECLARE_NTDLL_FUNCTION(name, ...)       \
private:                                        \
    typedef NTSTATUS(NTAPI* name##_t)(          \
        __VA_ARGS__);                           \
    name##_t m_##name;                          \
public:                                         \
    NTSTATUS name(__VA_ARGS__);

class Ntdll
{
private:
    HMODULE m_module;

public:
    struct SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
    {
        PVOID Object;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR HandleValue;
        ULONG GrantedAccess;
        USHORT CreatorBackTraceIndex;
        USHORT ObjectTypeIndex;
        ULONG HandleAttributes;
        ULONG Reserved;
    };

    struct SYSTEM_HANDLE_INFORMATION_EX
    {
        ULONG_PTR NumberOfHandles;
        ULONG_PTR Reserved;
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
    };

    Ntdll();

    DECLARE_NTDLL_FUNCTION(NtQuerySystemInformation,
                           ULONG SystemInformationClass,
                           PVOID SystemInformation,
                           ULONG SystemInformationLength,
                           PULONG ReturnLength)

    DECLARE_NTDLL_FUNCTION(NtDuplicateObject,
                           HANDLE SourceProcessHandle,
                           HANDLE SourceHandle,
                           HANDLE TargetProcessHandle,
                           PHANDLE TargetHandle,
                           ACCESS_MASK DesiredAccess,
                           ULONG Attributes,
                           ULONG Options)

    DECLARE_NTDLL_FUNCTION(NtQueryObject,
                           HANDLE ObjectHandle,
                           ULONG ObjectInformationClass,
                           PVOID ObjectInformation,
                           ULONG ObjectInformationLength,
                           PULONG ReturnLength);
};
