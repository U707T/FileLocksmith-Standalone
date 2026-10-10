// FileLocksmith standalone
#pragma once
#include "win_common.h"

namespace cli
{
    // True when any switch that only makes sense in a console is present.
    bool has_cli_arguments(const std::vector<std::wstring>& args);
}

// Returns the process exit code.
int cli_main(const std::vector<std::wstring>& args);
