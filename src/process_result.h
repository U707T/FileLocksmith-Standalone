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
    // 进程自己的可执行文件完整路径（取图标、复制进程路径用）
    std::wstring image_path;
};
