// FileLocksmith standalone - entry point
// Hybrid console/GUI front end:
//   * started from a shell (console available) or with a CLI switch -> text mode
//   * started by double click / "open with" / context menu        -> GUI window
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "win_common.h"
#include "app_utils.h"
#include "cli.h"
#include "gui.h"

namespace
{
    void pause_before_exit()
    {
        app::out(L"\n按回车键退出…");
        HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (input != INVALID_HANDLE_VALUE)
        {
            wchar_t buffer[8];
            DWORD read = 0;
            ReadConsoleW(input, buffer, 1, &read, nullptr);
            CloseHandle(input);
        }
    }
}

int main()
{
    std::vector<std::wstring> args;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }

    bool force_gui = false;
    for (const auto& arg : args)
    {
        if (arg == L"--gui")
        {
            force_gui = true;
        }
    }

    // setup_output() attaches to the console of the parent shell when there is
    // one, so "FileLocksmith.exe somefile --json" behaves like a normal CLI
    // tool while a double click never flashes a console window.
    const bool console_ready = app::setup_output();
    const bool text_mode = !force_gui && (console_ready || cli::has_cli_arguments(args));

    if (text_mode)
    {
        if (!console_ready)
        {
            app::ensure_own_console();
        }

        const int exit_code = cli_main(args);

        if (app::own_console_created())
        {
            pause_before_exit();
        }
        return exit_code;
    }

    std::vector<std::wstring> paths;
    for (const auto& arg : args)
    {
        if (arg != L"--gui")
        {
            paths.push_back(arg);
        }
    }

    app::enable_debug_privilege();
    return gui::run(paths);
}
