// FileLocksmith standalone
#include "shell_integration.h"
#include "app_utils.h"

#include <shlobj.h>
#include <shobjidl.h>

namespace shell_integration
{
    namespace
    {
        constexpr wchar_t ContextMenuKeyFile[] = L"Software\\Classes\\*\\shell\\FileLocksmith";
        constexpr wchar_t ContextMenuKeyDir[] = L"Software\\Classes\\Directory\\shell\\FileLocksmith";
        constexpr wchar_t ContextMenuKeyDrive[] = L"Software\\Classes\\Drive\\shell\\FileLocksmith";
        constexpr wchar_t ContextMenuKeyBackground[] = L"Software\\Classes\\Directory\\Background\\shell\\FileLocksmith";
        constexpr wchar_t ContextMenuVerb[] = L"什么正在占用这个文件？";
        constexpr wchar_t SendToLinkName[] = L"FileLocksmith.lnk";
        constexpr wchar_t HomePageUrl[] = L"https://github.com/U707T/FileLocksmith-Standalone";

        // MinGW-w64 的 win_common 头里没有 CLSID_ShellLink / IID_IShellLinkW 的定义
        // （需要再链一个 libuuid），这里直接把三个 GUID 写死，两个工具链通用。
        const GUID ShellLinkClsid = { 0x00021401, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        const GUID ShellLinkIid = { 0x000214F9, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        const GUID PersistFileIid = { 0x0000010b, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

        bool set_reg_string(HKEY root, const std::wstring& sub_key, const wchar_t* name, const std::wstring& value)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExW(root, sub_key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            {
                return false;
            }

            const auto status = RegSetValueExW(key, name, 0, REG_SZ,
                                               reinterpret_cast<const BYTE*>(value.c_str()),
                                               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
            return status == ERROR_SUCCESS;
        }

        bool reg_key_exists(HKEY root, const std::wstring& sub_key)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, sub_key.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return false;
            }
            RegCloseKey(key);
            return true;
        }

        std::wstring sendto_link_path()
        {
            return sendto_folder() + L"\\" + SendToLinkName;
        }

        bool create_shortcut(const std::wstring& link_path, const std::wstring& target, const std::wstring& arguments)
        {
            const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

            IShellLinkW* link = nullptr;
            bool created = false;
            if (SUCCEEDED(CoCreateInstance(ShellLinkClsid, nullptr, CLSCTX_INPROC_SERVER, ShellLinkIid, reinterpret_cast<void**>(&link))))
            {
                link->SetPath(target.c_str());
                link->SetArguments(arguments.c_str());
                link->SetDescription(L"查看/解除文件占用（FileLocksmith）");
                link->SetIconLocation(target.c_str(), 0);

                IPersistFile* file = nullptr;
                if (SUCCEEDED(link->QueryInterface(PersistFileIid, reinterpret_cast<void**>(&file))))
                {
                    created = SUCCEEDED(file->Save(link_path.c_str(), TRUE));
                    file->Release();
                }
                link->Release();
            }

            if (SUCCEEDED(com_status))
            {
                CoUninitialize();
            }
            return created;
        }
    }

    std::wstring extension_of(const std::wstring& path)
    {
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos)
        {
            return {};
        }
        std::wstring extension = path.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        return extension;
    }

    std::wstring sendto_folder()
    {
        // %APPDATA%\Microsoft\Windows\SendTo 就是「发送到」菜单目录，
        // 用环境变量拼比 SHGetFolderPath 少一层依赖。
        wchar_t buffer[MAX_PATH * 2]{};
        if (GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(ARRAYSIZE(buffer))) > 0)
        {
            return std::wstring(buffer) + L"\\Microsoft\\Windows\\SendTo";
        }
        return {};
    }

    bool is_context_menu_installed()
    {
        return reg_key_exists(HKEY_CURRENT_USER, ContextMenuKeyFile) ||
               reg_key_exists(HKEY_CURRENT_USER, ContextMenuKeyDir);
    }

    bool install_context_menu()
    {
        const auto exe = app::executable_path();
        if (exe.empty())
        {
            return false;
        }

        const auto icon = exe + L",0";
        const auto quoted_exe = app::quote_arg(exe);

        // 目录空白处的右键菜单拿不到 %1，必须用 %V（当前目录）
        struct MenuLocation
        {
            const wchar_t* key;
            const wchar_t* argument;
        };
        const MenuLocation locations[] = {
            { ContextMenuKeyFile, L"%1" },
            { ContextMenuKeyDir, L"%1" },
            { ContextMenuKeyDrive, L"%1" },
            { ContextMenuKeyBackground, L"%V" },
        };

        bool ok = true;
        for (const auto& location : locations)
        {
            const std::wstring base(location.key);
            const std::wstring command = quoted_exe + L" \"" + location.argument + L"\"";
            ok = set_reg_string(HKEY_CURRENT_USER, base, nullptr, ContextMenuVerb) && ok;
            ok = set_reg_string(HKEY_CURRENT_USER, base, L"Icon", icon) && ok;
            ok = set_reg_string(HKEY_CURRENT_USER, base, L"MultiSelectModel", L"Player") && ok;
            ok = set_reg_string(HKEY_CURRENT_USER, base + L"\\command", nullptr, command) && ok;
        }
        return ok;
    }

    bool uninstall_context_menu()
    {
        bool ok = true;
        for (const auto* key : { ContextMenuKeyFile, ContextMenuKeyDir, ContextMenuKeyDrive, ContextMenuKeyBackground })
        {
            if (RegDeleteTreeW(HKEY_CURRENT_USER, key) != ERROR_SUCCESS &&
                reg_key_exists(HKEY_CURRENT_USER, key))
            {
                ok = false;
            }
        }
        return ok;
    }

    bool is_sendto_installed()
    {
        const auto path = sendto_link_path();
        return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool install_sendto_shortcut()
    {
        const auto path = sendto_link_path();
        const auto exe = app::executable_path();
        if (path.empty() || exe.empty())
        {
            return false;
        }
        return create_shortcut(path, exe, L"");
    }

    bool is_shortcut(const std::wstring& path)
    {
        return extension_of(path) == L".lnk";
    }

    bool is_executable(const std::wstring& path)
    {
        const auto extension = extension_of(path);
        return extension == L".exe" || extension == L".com" || extension == L".bat" || extension == L".cmd";
    }

    bool resolve_shortcut(const std::wstring& link_path, std::wstring& target, std::wstring& arguments)
    {
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        bool ok = false;
        IShellLinkW* link = nullptr;
        if (SUCCEEDED(CoCreateInstance(ShellLinkClsid, nullptr, CLSCTX_INPROC_SERVER, ShellLinkIid, reinterpret_cast<void**>(&link))))
        {
            IPersistFile* file = nullptr;
            if (SUCCEEDED(link->QueryInterface(PersistFileIid, reinterpret_cast<void**>(&file))))
            {
                if (SUCCEEDED(file->Load(link_path.c_str(), STGM_READ)))
                {
                    std::vector<wchar_t> buffer(MAX_PATH * 2, L'\0');
                    WIN32_FIND_DATAW find_data{};
                    if (link->GetPath(buffer.data(), static_cast<int>(buffer.size()), &find_data, SLGP_UNCPRIORITY) == S_OK &&
                        buffer[0] != L'\0')
                    {
                        target = buffer.data();
                        ok = !target.empty();
                    }

                    std::vector<wchar_t> argument_buffer(MAX_PATH * 2, L'\0');
                    if (ok && link->GetArguments(argument_buffer.data(), static_cast<int>(argument_buffer.size())) == S_OK)
                    {
                        arguments = argument_buffer.data();
                    }
                }
                file->Release();
            }
            link->Release();
        }

        if (SUCCEEDED(com_status))
        {
            CoUninitialize();
        }
        return ok;
    }

    bool open_sendto_folder()
    {
        const auto folder = sendto_folder();
        if (folder.empty())
        {
            return false;
        }
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
}
