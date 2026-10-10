// FileLocksmith standalone - 资源管理器集成：右键菜单 + 「发送到」
// 由 cli.cpp 与 gui.cpp 共用，所以放在这里而不是留在各自的文件里。
#pragma once
#include "win_common.h"

namespace shell_integration
{
    // 资源管理器右键菜单：HKCU\Software\Classes\*\shell\FileLocksmith
    // （只写当前用户，不需要管理员；Win11 会出现在"显示更多选项"里）
    bool install_context_menu();
    bool uninstall_context_menu();
    bool is_context_menu_installed();

    // 「发送到」菜单：%APPDATA%\Microsoft\Windows\SendTo\FileLocksmith.lnk
    // Win11 的「发送到」在一级菜单里，所以这是 Win11 上更顺手的入口。
    bool install_sendto_shortcut();
    bool is_sendto_installed();
    std::wstring sendto_folder();
    bool open_sendto_folder();

    // 供界面展示
    std::wstring context_menu_command();

    // .lnk 解析：拿到快捷方式指向的目标（通常是 exe）与参数
    bool resolve_shortcut(const std::wstring& link_path, std::wstring& target, std::wstring& arguments);
    bool is_shortcut(const std::wstring& path);     // 扩展名是不是 .lnk
    bool is_executable(const std::wstring& path);   // 扩展名是不是 .exe / .com / .bat / .cmd
}
