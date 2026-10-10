// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#pragma once
#include "process_result.h"

// Second version, checks handles towards files and all subfiles and folders of given dirs, if any.
std::vector<ProcessResult> find_processes_recursive(const std::vector<std::wstring>& paths);

// Gives the full path of the executable, given the process id
std::wstring pid_to_full_path(DWORD pid);
