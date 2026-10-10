// FileLocksmith standalone - command line interface
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "cli.h"
#include "app_utils.h"
#include "file_locksmith.h"
#include "shell_integration.h"

#include <version.h>

namespace
{
    constexpr wchar_t ProductName[] = L"FileLocksmith";

    std::wstring product_version()
    {
        return app::to_wstring(FL_VERSION_STRING) + L" (standalone extraction of PowerToys FileLocksmith)";
    }

    std::wstring usage()
    {
        std::wstring text;
        text += L"FileLocksmith - 查看/结束正在占用文件的进程\n";
        text += L"版本 " + app::to_wstring(FL_VERSION_STRING) + L"，由 PowerToys FileLocksmith 模块独立提取而来\n\n";
        text += L"用法: FileLocksmith.exe [选项] <文件或目录> [更多路径...]\n\n";
        text += L"选项:\n";
        text += L"  --json             以 JSON 输出结果\n";
        text += L"  --kill             结束所有占用这些文件的进程\n";
        text += L"  --unlock           强制解除占用（关闭别的进程持有的文件句柄，不结束进程）\n";
        text += L"  --wait             等待直到文件被解锁\n";
        text += L"  --timeout <毫秒>   --wait 的超时时间\n";
        text += L"  --gui              强制打开图形界面\n";
        text += L"  --cli              强制使用命令行模式\n";
        text += L"  --elevate          以管理员身份重新启动\n";
        text += L"  --register         添加右键菜单项（仅当前用户）\n";
        text += L"  --unregister       移除右键菜单项\n";
        text += L"  --sendto           加到「发送到」菜单（Win11 一级右键菜单可见）\n";
        text += L"  --version          显示版本\n";
        text += L"  -h, --help         显示本帮助\n";
        return text;
    }

    std::wstring json_escape(const std::wstring& text)
    {
        std::wstring result;
        result.reserve(text.size() + 8);
        for (wchar_t ch : text)
        {
            switch (ch)
            {
            case L'"': result += L"\\\""; break;
            case L'\\': result += L"\\\\"; break;
            case L'\b': result += L"\\b"; break;
            case L'\f': result += L"\\f"; break;
            case L'\n': result += L"\\n"; break;
            case L'\r': result += L"\\r"; break;
            case L'\t': result += L"\\t"; break;
            default:
                if (ch < 0x20)
                {
                    const wchar_t* hex = L"0123456789abcdef";
                    result += L"\\u";
                    result.push_back(hex[(static_cast<unsigned>(ch) >> 12) & 0xF]);
                    result.push_back(hex[(static_cast<unsigned>(ch) >> 8) & 0xF]);
                    result.push_back(hex[(static_cast<unsigned>(ch) >> 4) & 0xF]);
                    result.push_back(hex[static_cast<unsigned>(ch) & 0xF]);
                }
                else
                {
                    result.push_back(ch);
                }
            }
        }
        return result;
    }

    std::wstring to_json(const std::vector<ProcessResult>& results, const std::vector<std::wstring>& unresolved)
    {
        std::wstring out = L"{\"processes\":[";
        bool first_process = true;
        for (const auto& result : results)
        {
            if (!first_process)
            {
                out += L',';
            }
            first_process = false;

            out += L"{\"pid\":" + std::to_wstring(result.pid);
            out += L",\"name\":\"" + json_escape(result.name) + L"\"";
            out += L",\"user\":\"" + json_escape(result.user) + L"\"";
            out += L",\"files\":[";
            bool first_file = true;
            for (const auto& file : result.files)
            {
                if (!first_file)
                {
                    out += L',';
                }
                first_file = false;
                out += L"\"" + json_escape(file) + L"\"";
            }
            out += L"]}";
        }
        out += L"],\"unresolved\":[";
        bool first_unresolved = true;
        for (const auto& path : unresolved)
        {
            if (!first_unresolved)
            {
                out += L',';
            }
            first_unresolved = false;
            out += L"\"" + json_escape(path) + L"\"";
        }
        out += L"]}";
        return out;
    }

    std::wstring to_text(const std::vector<ProcessResult>& results, bool any_path_checked)
    {
        if (results.empty())
        {
            // 一个路径都没查成功的时候，说"没有占用"是误导
            return any_path_checked ? L"没有找到占用该文件的进程。\n"
                                    : L"没有任何路径被成功检查（见上面的警告）。\n";
        }

        std::wstring out = L"PID\t用户\t进程\n";
        for (const auto& result : results)
        {
            out += std::to_wstring(result.pid) + L"\t" + result.user + L"\t" + result.name + L"\n";
        }
        return out;
    }

    std::wstring kill_processes(const std::vector<ProcessResult>& results)
    {
        std::wstring out;
        for (const auto& result : results)
        {
            if (app::terminate_process(result.pid))
            {
                out += L"已结束进程 " + std::to_wstring(result.pid) + L" (" + result.name + L")\n";
            }
            else
            {
                out += L"无法结束进程 " + std::to_wstring(result.pid) + L" (" + result.name + L")"
                       + (app::is_process_elevated() ? L"，权限不足。\n" : L"，请以管理员身份重试。\n");
            }
        }
        return out;
    }

    // ------------------------------------------------------------------
    //  资源管理器集成（实现见 shell_integration.cpp，GUI 走同一套）
    // ------------------------------------------------------------------
    int register_context_menu()
    {
        if (!shell_integration::install_context_menu())
        {
            app::out(L"写入注册表失败。\n");
            return 1;
        }

        app::out(L"已添加右键菜单项（文件 / 文件夹 / 驱动器 / 目录空白处，仅当前用户）。\n"
                 L"提示: Windows 11 要在“显示更多选项”(Shift+F10) 里查看；\n"
                 L"      想出现在 Win11 一级菜单，可以再加 --sendto 装到「发送到」里。\n");
        return 0;
    }

    int unregister_context_menu()
    {
        if (!shell_integration::uninstall_context_menu())
        {
            app::out(L"移除右键菜单项失败。\n");
            return 1;
        }

        app::out(L"已移除右键菜单项。\n");
        return 0;
    }

    int install_sendto()
    {
        if (!shell_integration::install_sendto_shortcut())
        {
            app::out(L"创建「发送到」快捷方式失败。\n");
            return 1;
        }

        app::out(L"已添加到「发送到」菜单: " + shell_integration::sendto_folder() + L"\n");
        return 0;
    }

    bool is_cli_flag(const std::wstring& arg)
    {
        return arg == L"--json" || arg == L"--kill" || arg == L"--unlock" || arg == L"--wait" || arg == L"--timeout" ||
               arg == L"--sendto" ||
               arg == L"--help" || arg == L"-h" || arg == L"--version" || arg == L"--cli" ||
               arg == L"--register" || arg == L"--unregister" || arg == L"--elevate";
    }
}

namespace cli
{
    bool has_cli_arguments(const std::vector<std::wstring>& args)
    {
        for (const auto& arg : args)
        {
            if (is_cli_flag(arg))
            {
                return true;
            }
        }
        return false;
    }
}

int cli_main(const std::vector<std::wstring>& args)
{
    bool json_output = false;
    bool kill = false;
    bool unlock = false;
    bool wait = false;
    bool elevate = false;
    int timeout_ms = -1;
    std::vector<std::wstring> paths;

    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& arg = args[i];
        if (arg == L"--json")
        {
            json_output = true;
        }
        else if (arg == L"--kill")
        {
            kill = true;
        }
        else if (arg == L"--unlock")
        {
            unlock = true;
        }
        else if (arg == L"--wait")
        {
            wait = true;
        }
        else if (arg == L"--elevate")
        {
            elevate = true;
        }
        else if (arg == L"--timeout")
        {
            if (i + 1 >= args.size())
            {
                app::out(L"错误: --timeout 需要一个参数。\n");
                return 1;
            }
            try
            {
                timeout_ms = std::stoi(args[++i]);
            }
            catch (...)
            {
                app::out(L"错误: --timeout 的值无效。\n");
                return 1;
            }
        }
        else if (arg == L"--help" || arg == L"-h")
        {
            app::out(usage());
            return 0;
        }
        else if (arg == L"--version")
        {
            app::out(std::wstring(ProductName) + L" " + product_version() + L"\n");
            app::out(L"运行环境: " + app::os_friendly_name() + L" (" + app::os_version_string() + L")\n");
            app::out(std::wstring(L"管理员权限: ") + (app::is_process_elevated() ? L"是" : L"否") + L"\n");
            return 0;
        }
        else if (arg == L"--register")
        {
            return register_context_menu();
        }
        else if (arg == L"--unregister")
        {
            return unregister_context_menu();
        }
        else if (arg == L"--sendto")
        {
            return install_sendto();
        }
        else if (arg == L"--cli" || arg == L"--gui")
        {
            // Handled by main().
        }
        else
        {
            paths.push_back(arg);
        }
    }

    if (elevate)
    {
        if (app::is_process_elevated())
        {
            app::out(L"当前已经是管理员权限。\n");
            return 0;
        }
        // 提权后要执行的是同一件事：只摘掉 --elevate，其余开关全部保留
        std::vector<std::wstring> new_args;
        for (const auto& arg : args)
        {
            if (arg != L"--elevate")
            {
                new_args.push_back(arg);
            }
        }
        if (new_args.empty())
        {
            new_args.push_back(L"--gui");
        }
        return app::relaunch_elevated(new_args) ? 0 : 1;
    }

    if (paths.empty())
    {
        app::out(L"错误: 没有指定任何路径。\n\n" + usage());
        return 1;
    }

    app::enable_debug_privilege();

    if (wait)
    {
        std::wstring out;
        if (json_output)
        {
            out += L"警告: --wait 与 --json 不能同时使用，已忽略 --json。\n";
            json_output = false;
        }

        out += L"正在等待文件被解锁...\n";
        app::err(out);

        auto start_time = std::chrono::steady_clock::now();
        bool first_pass = true;
        for (;;)
        {
            LocksmithResult locks;
            try
            {
                // 轮询时跳过模块扫描（那是最慢的一步），判定解锁前再完整查一次
                locks = find_locks(paths, /* include_modules */ false);
            }
            catch (const std::exception& ex)
            {
                app::out(L"枚举进程失败: " + app::to_wstring(ex.what()) + L"\n");
                return 1;
            }

            // 打不开的路径永远等不到解锁，直接报错（否则会立刻打印"文件已解锁"）
            if (first_pass && !locks.unresolved_paths.empty())
            {
                for (const auto& path : locks.unresolved_paths)
                {
                    app::out(L"错误: 打不开 " + path + L"（不存在 / 没权限）\n");
                }
                return 1;
            }
            first_pass = false;

            if (locks.processes.empty())
            {
                bool still_locked = false;
                try
                {
                    still_locked = !find_locks(paths, /* include_modules */ true).processes.empty();
                }
                catch (const std::exception&)
                {
                }

                if (!still_locked)
                {
                    app::out(L"文件已解锁。\n");
                    return 0;
                }
            }

            if (timeout_ms >= 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count();
                if (elapsed > timeout_ms)
                {
                    app::out(L"等待超时: 文件仍被占用。\n");
                    return 1;
                }
            }

            Sleep(200);
        }
    }

    LocksmithResult locks;
    try
    {
        locks = find_locks(paths);
    }
    catch (const std::exception& ex)
    {
        app::out(L"枚举进程失败: " + app::to_wstring(ex.what()) + L"\n");
        return 1;
    }

    std::vector<ProcessResult> results = locks.processes;

    // 诊断信息（警告 / 进度 / 结果说明）走 stderr：
    // --json 模式下 stdout 必须只有一行 JSON，脚本才能直接解析。
    auto note = [json_output](const std::wstring& text) {
        if (json_output)
        {
            app::err(text);
        }
        else
        {
            app::out(text);
        }
    };

    if (!locks.unresolved_paths.empty())
    {
        std::wstring warning = L"警告: 下面 " + std::to_wstring(locks.unresolved_paths.size()) +
                               L" 个路径打不开（不存在 / 没权限 / 不是本地对象），没有被检查:\n";
        for (const auto& path : locks.unresolved_paths)
        {
            warning += L"  ? " + path + L"\n";
        }
        note(warning);
    }

    if (!locks.handles_query_ok)
    {
        note(L"警告: 句柄枚举失败，“没有占用”这个结论不可信（可试试 --elevate）。\n");
    }

    // 1) 先尝试强制解除占用（关句柄，不结束进程）
    if (unlock)
    {
        if (locks.handles.empty())
        {
            note(L"没有找到可强制关闭的文件句柄。\n");
        }
        else
        {
            std::vector<std::wstring> failures;
            const auto closed = release_handles(locks.handles, failures);
            note(L"已强制关闭 " + std::to_wstring(closed) + L"/" + std::to_wstring(locks.handles.size()) +
                 L" 个文件句柄（进程没有被结束，但持有它的程序可能出现异常，请自行确认）。\n");
            for (const auto& failure : failures)
            {
                note(L"  ! " + failure + L"\n");
            }
        }

        try
        {
            locks = find_locks(paths);
            results = locks.processes;
        }
        catch (const std::exception&)
        {
            results.clear();
        }

        note(results.empty() ? L"文件已经解锁。\n"
                             : L"仍然被 " + std::to_wstring(results.size()) + L" 个进程占用（可用 --kill 结束它们，或用 --elevate 提权后重试）。\n");
    }

    // 2) 仍然占用时，按需结束进程
    if (kill)
    {
        note(kill_processes(results));
        try
        {
            results = find_processes_recursive(paths);
        }
        catch (const std::exception&)
        {
            results.clear();
        }
    }

    if (json_output)
    {
        app::out(to_json(results, locks.unresolved_paths) + L"\n");
    }
    else
    {
        std::wstring text = to_text(results, locks.unresolved_paths.size() < paths.size());
        if (!results.empty() && !app::is_process_elevated())
        {
            text += L"\n提示: 部分系统级进程需要管理员权限才能看到或结束，可使用 --elevate。\n";
        }
        app::out(text);
    }
    return 0;
}
