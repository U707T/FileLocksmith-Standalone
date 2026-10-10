// FileLocksmith standalone - Win32 GUI (replaces the WinUI 3 app of PowerToys)
#pragma once
#include "win_common.h"

namespace gui
{
    // Shows the results window. Returns the process exit code.
    int run(const std::vector<std::wstring>& paths);
}
