// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "app_utils.h"

namespace app
{
    namespace
    {
        HANDLE g_out = NULL;
        bool g_out_is_console = false;
        bool g_own_console = false;
        bool g_setup_done = false;

        bool try_open_conout()
        {
            HANDLE h = CreateFileW(L"CONOUT$",
                                   GENERIC_WRITE | GENERIC_READ,
                                   FILE_SHARE_WRITE | FILE_SHARE_READ,
                                   NULL,
                                   OPEN_EXISTING,
                                   0,
                                   NULL);
            if (h == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            g_out = h;
            g_out_is_console = true;
            return true;
        }
    }

    bool setup_output()
    {
        if (g_setup_done)
        {
            return g_out != NULL;
        }
        g_setup_done = true;

        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != NULL && h != INVALID_HANDLE_VALUE)
        {
            DWORD mode = 0;
            if (GetConsoleMode(h, &mode))
            {
                g_out = h;
                g_out_is_console = true;
                return true;
            }
            if (GetFileType(h) != FILE_TYPE_UNKNOWN)
            {
                // stdout is redirected to a file or pipe: keep it.
                g_out = h;
                g_out_is_console = false;
                return true;
            }
        }

        // GUI subsystem process started from a shell that does have a console.
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            if (try_open_conout())
            {
                return true;
            }
        }
        return false;
    }

    bool ensure_own_console()
    {
        if (setup_output())
        {
            return true;
        }
        if (AllocConsole())
        {
            g_own_console = true;
            if (try_open_conout())
            {
                return true;
            }
        }
        return false;
    }

    bool own_console_created()
    {
        return g_own_console;
    }

    bool output_is_console()
    {
        return g_out_is_console;
    }

    void out(const std::wstring& text)
    {
        if (g_out == NULL || text.empty())
        {
            return;
        }

        if (g_out_is_console)
        {
            DWORD written = 0;
            WriteConsoleW(g_out, text.c_str(), static_cast<DWORD>(text.size()), &written, NULL);
        }
        else
        {
            auto utf8 = to_utf8(text);
            DWORD written = 0;
            WriteFile(g_out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, NULL);
        }
    }

    std::string to_utf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring to_wstring(const char* utf8_text)
    {
        if (utf8_text == nullptr || *utf8_text == '\0')
        {
            return {};
        }
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, nullptr, 0);
        if (size <= 1)
        {
            return {};
        }
        std::wstring result(static_cast<size_t>(size - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, result.data(), size);
        return result;
    }

    bool is_process_elevated()
    {
        HANDLE token = nullptr;
        bool elevated = false;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
            {
                elevated = (elevation.TokenIsElevated != 0);
            }
            CloseHandle(token);
        }
        return elevated;
    }

    bool enable_debug_privilege()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
        {
            return false;
        }

        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid))
        {
            CloseHandle(token);
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        bool ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != 0 &&
                  GetLastError() != ERROR_NOT_ALL_ASSIGNED;
        CloseHandle(token);
        return ok;
    }

    bool terminate_process(DWORD pid)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!process)
        {
            return false;
        }
        bool result = TerminateProcess(process, 0) != 0;
        CloseHandle(process);
        return result;
    }

    std::wstring executable_path()
    {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0)
            {
                return {};
            }
            if (written < buffer.size())
            {
                buffer.resize(written);
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring exe_directory()
    {
        auto path = executable_path();
        auto pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? std::wstring{} : path.substr(0, pos);
    }

    std::wstring quote_arg(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        {
            return arg;
        }

        std::wstring result = L"\"";
        for (auto it = arg.begin();; ++it)
        {
            unsigned backslashes = 0;
            while (it != arg.end() && *it == L'\\')
            {
                ++it;
                ++backslashes;
            }

            if (it == arg.end())
            {
                result.append(backslashes * 2, L'\\');
                break;
            }
            else if (*it == L'"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(*it);
            }
            else
            {
                result.append(backslashes, L'\\');
                result.push_back(*it);
            }
        }
        result.push_back(L'"');
        return result;
    }

    bool relaunch_elevated(const std::vector<std::wstring>& args)
    {
        auto exe = executable_path();
        if (exe.empty())
        {
            return false;
        }

        std::wstring parameters;
        for (const auto& arg : args)
        {
            if (!parameters.empty())
            {
                parameters += L' ';
            }
            parameters += quote_arg(arg);
        }

        SHELLEXECUTEINFOW exec_info{};
        exec_info.cbSize = sizeof(exec_info);
        exec_info.fMask = SEE_MASK_NOCLOSEPROCESS;
        exec_info.hwnd = nullptr;
        exec_info.lpVerb = L"runas";
        exec_info.lpFile = exe.c_str();
        exec_info.lpParameters = parameters.c_str();
        exec_info.lpDirectory = nullptr;
        exec_info.nShow = SW_SHOWNORMAL;

        if (ShellExecuteExW(&exec_info))
        {
            if (exec_info.hProcess)
            {
                CloseHandle(exec_info.hProcess);
            }
            return true;
        }
        return false;
    }
}
