// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once
#include "win_common.h"

struct ProcessResult
{
    std::wstring name;
    DWORD pid;
    std::wstring user;
    std::vector<std::wstring> files;
};
