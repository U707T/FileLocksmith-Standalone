// FileLocksmith standalone
// Portions derived from Microsoft PowerToys (MIT License, (c) Microsoft Corporation).
#include "app_utils.h"

namespace app
{
    namespace
    {
        HANDLE g_out = NULL;
        HANDLE g_err = NULL;
        bool g_out_is_console = false;
        bool g_err_is_console = false;
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
                g_err = GetStdHandle(STD_ERROR_HANDLE);
                g_err_is_console = true;
                return true;
            }
            if (GetFileType(h) != FILE_TYPE_UNKNOWN)
            {
                // stdout is redirected to a file or pipe: keep it.
                g_out = h;
                g_out_is_console = false;
                g_err = GetStdHandle(STD_ERROR_HANDLE);
                g_err_is_console = false;
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
                if (g_err == NULL)
                {
                    g_err = g_out;
                    g_err_is_console = g_out_is_console;
                }
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

    namespace
    {
        void write_to(HANDLE handle, bool is_console, const std::wstring& text)
        {
            if (handle == NULL || text.empty())
            {
                return;
            }

            if (is_console)
            {
                DWORD written = 0;
                WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, NULL);
            }
            else
            {
                auto utf8 = to_utf8(text);
                DWORD written = 0;
                WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, NULL);
            }
        }
    }

    void out(const std::wstring& text)
    {
        write_to(g_out, g_out_is_console, text);
    }

    void err(const std::wstring& text)
    {
        // 没单独拿到 stderr（例如同一个控制台句柄）时退回 stdout，别把消息丢了
        if (g_err != NULL)
        {
            write_to(g_err, g_err_is_console, text);
        }
        else
        {
            write_to(g_out, g_out_is_console, text);
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

    namespace
    {
        // GetVersionExW lies unless the manifest declares supportedOS entries;
        // RtlGetVersion always reports the real build number. Not a documented
        // Win32 API, but it is stable and used by every diagnostic tool.
        bool query_os_version(DWORD& major, DWORD& minor, DWORD& build)
        {
            using RtlGetVersion_t = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            auto get_version = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
            if (get_version == nullptr)
            {
                return false;
            }

            RTL_OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (get_version(&info) != 0)
            {
                return false;
            }

            major = info.dwMajorVersion;
            minor = info.dwMinorVersion;
            build = info.dwBuildNumber;
            return true;
        }
    }

    std::wstring os_version_string()
    {
        DWORD major = 0, minor = 0, build = 0;
        if (!query_os_version(major, minor, build))
        {
            return L"未知";
        }
        return std::to_wstring(major) + L"." + std::to_wstring(minor) + L"." + std::to_wstring(build);
    }

    std::wstring os_friendly_name()
    {
        DWORD major = 0, minor = 0, build = 0;
        if (!query_os_version(major, minor, build))
        {
            return L"未知系统";
        }

        switch (build)
        {
        case 7601: return L"Windows 7 SP1";
        case 9600: return L"Windows 8.1";
        case 10240: return L"Windows 10 1507";
        case 10586: return L"Windows 10 1511";
        case 14393: return L"Windows 10 1607 / Server 2016";
        case 15063: return L"Windows 10 1703";
        case 16299: return L"Windows 10 1709";
        case 17134: return L"Windows 10 1803";
        case 17763: return L"Windows 10 1809 / Server 2019";
        case 18362: return L"Windows 10 1903";
        case 18363: return L"Windows 10 1909";
        case 19041: return L"Windows 10 2004";
        case 19042: return L"Windows 10 20H2";
        case 19043: return L"Windows 10 21H1";
        case 19044: return L"Windows 10 21H2";
        case 19045: return L"Windows 10 22H2";
        case 20348: return L"Windows Server 2022";
        case 22000: return L"Windows 11 21H2";
        case 22621: return L"Windows 11 22H2";
        case 22631: return L"Windows 11 23H2";
        case 26100: return L"Windows 11 24H2 / Server 2025";
        default: break;
        }

        if (major == 10 && build >= 22000)
        {
            return L"Windows 11 (较新版本)";
        }
        if (major == 10)
        {
            return L"Windows 10 (较新版本)";
        }
        return L"Windows " + std::to_wstring(major) + L"." + std::to_wstring(minor);
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
