// FileLocksmith standalone - small Win32 helpers (no PowerToys dependency)
#pragma once
#include "win_common.h"

namespace app
{
    // ---- console / output -------------------------------------------------
    // Tries to make stdout usable. Returns true if there is a console or a
    // redirected stdout we can write to.
    bool setup_output();
    // Creates a console if there is none (used when a CLI flag is passed while
    // the process was started without a console).
    bool ensure_own_console();
    // True when the process had to create its own console (so we should pause
    // before exiting, otherwise the window vanishes).
    bool own_console_created();
    bool output_is_console();
    // Writes a wide string either to the console (proper Unicode) or as UTF-8
    // bytes when stdout is redirected to a file/pipe.
    void out(const std::wstring& text);

    // ---- process helpers --------------------------------------------------
    bool is_process_elevated();
    bool enable_debug_privilege();
    bool terminate_process(DWORD pid);
    std::wstring executable_path();
    std::wstring quote_arg(const std::wstring& arg);
    bool relaunch_elevated(const std::vector<std::wstring>& args);

    // ---- 操作系统版本（--version 与排障用）---------------------------------
    std::wstring os_version_string();  // 例如 "10.0.19045"
    std::wstring os_friendly_name();   // 例如 "Windows 10 22H2"

    // ---- misc -------------------------------------------------------------
    std::string to_utf8(const std::wstring& text);
    std::wstring to_wstring(const char* utf8_text);
    std::wstring exe_directory();
}
