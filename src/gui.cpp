// FileLocksmith standalone - minimal Win32 GUI
// This file is new (PowerToys used a WinUI 3 / Windows App SDK app here).
#include "gui.h"
#include "app_utils.h"
#include "file_locksmith.h"
#include "shell_integration.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <version.h>

namespace
{
    constexpr wchar_t WndClassName[] = L"FileLocksmithStandaloneWindow";
    constexpr wchar_t WindowTitle[] = L"FileLocksmith - 什么正在占用这个文件？";

    constexpr int IDC_LIST = 1001;
    constexpr int IDC_KILL_SELECTED = 1002;
    constexpr int IDC_KILL_ALL = 1003;
    constexpr int IDC_REFRESH = 1004;
    constexpr int IDC_ELEVATE = 1005;
    constexpr int IDC_OPEN_LOCATION = 1006;
    constexpr int IDC_PATH_LABEL = 1007;
    constexpr int IDC_STATUS = 1008;
    constexpr int IDC_FORCE_UNLOCK = 1009;

    // 菜单命令（ID 从 2000 起，和控件 ID 分开）
    constexpr int IDM_TOGGLE_CONTEXT_MENU = 2001;
    constexpr int IDM_INSTALL_SENDTO = 2002;
    constexpr int IDM_OPEN_SENDTO = 2003;
    constexpr int IDM_REFRESH = 2004;
    constexpr int IDM_ELEVATE = 2005;
    constexpr int IDM_ABOUT = 2006;
    constexpr int IDM_HOMEPAGE = 2007;
    constexpr int IDM_EXIT = 2008;
    constexpr int IDM_ADD_FILES = 2009;
    constexpr int IDM_ADD_FOLDER = 2010;

    // 子控件转发拖放用的 subclass id
    constexpr UINT_PTR DROP_SUBCLASS_ID = 1;

    // 按钮：ID / 文字 / 宽度。以前是按 “IDC_KILL_SELECTED + 序号” 分配 ID 的，
    // 顺序和常量对不上，导致“打开所在位置”实际会触发“以管理员身份重启”，
    // 这里改成显式声明。
    struct ButtonSpec
    {
        int id;
        const wchar_t* text;
        int width;
    };

    const ButtonSpec g_buttons_spec[] = {
        { IDC_KILL_SELECTED, L"结束进程", 110 },
        { IDC_KILL_ALL, L"全部结束", 110 },
        { IDC_FORCE_UNLOCK, L"强制解锁", 110 },
        { IDC_REFRESH, L"重新检查", 90 },
        { IDC_OPEN_LOCATION, L"打开所在位置", 140 },
        { IDC_ELEVATE, L"以管理员身份重启", 170 },
    };
    constexpr int BUTTON_COUNT = static_cast<int>(sizeof(g_buttons_spec) / sizeof(g_buttons_spec[0]));

    constexpr UINT WM_FL_SEARCH_DONE = WM_APP + 1;
    constexpr ULONG FL_COPYDATA_MAGIC = 0x464C534D; // 'FLSM'

    HINSTANCE g_instance = nullptr;
    HWND g_window = nullptr;
    HWND g_list = nullptr;
    HWND g_status = nullptr;
    HWND g_path_label = nullptr;
    HWND g_buttons[BUTTON_COUNT] = {};
    HFONT g_font = nullptr;
    HMENU g_main_menu = nullptr;

    bool g_searching = false;
    // 搜索进行中又来了新路径（拖放经常接着上一轮）时，等这轮结束再查一次
    bool g_search_pending = false;
    // 后台枚举线程是否还在跑：退出时用它等一小会儿，避免线程还在系统调用里就拆 CRT
    std::atomic<bool> g_worker_running{ false };
    // Written by the UI thread, read by the worker thread that posts results.
    std::atomic<HWND> g_post_target{ nullptr };
    std::vector<std::wstring> g_paths;
    std::vector<ProcessResult> g_results;
    std::vector<LockedHandle> g_handles;
    bool g_handles_query_ok = true;

    struct SearchPayload
    {
        LocksmithResult locks;
        std::wstring error;
    };

    // Control IDs are passed through the HMENU parameter of CreateWindowExW;
    // the INT_PTR round trip keeps 64-bit MSVC happy (C4312).
    inline HMENU control_id(int id)
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    std::wstring join(const std::vector<std::wstring>& items, const wchar_t* separator)
    {
        std::wstring result;
        for (const auto& item : items)
        {
            if (!result.empty())
            {
                result += separator;
            }
            result += item;
        }
        return result;
    }

    void set_status(const std::wstring& text)
    {
        SetWindowTextW(g_status, text.c_str());
    }

    void update_path_label()
    {
        std::wstring text;
        if (g_paths.empty())
        {
            text = L"把文件或文件夹拖到窗口里（也可以拖到 exe 图标上）";
            if (app::is_process_elevated())
            {
                text += L"\n当前是管理员模式：Windows 不允许从资源管理器拖放到提权窗口，请用「文件 → 添加文件…」";
            }
        }
        else if (g_paths.size() <= 2)
        {
            text = L"正在检查: " + join(g_paths, L"\n");
        }
        else
        {
            // 标签只有两行高，路径多了要收敛，不然用户以为只查了前面的
            text = L"正在检查 " + std::to_wstring(g_paths.size()) + L" 个路径: " + g_paths[0] +
                   L"、" + g_paths[1] + L" …";
        }
        SetWindowTextW(g_path_label, text.c_str());
    }

    void set_busy(bool busy)
    {
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            EnableWindow(g_buttons[i], !busy);
        }
        if (busy)
        {
            set_status(L"正在枚举系统句柄，请稍候…（文件较多时可能需要几秒）");
        }
    }

    void fill_list()
    {
        SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_list);

        int index = 0;
        for (const auto& result : g_results)
        {
            auto pid_text = std::to_wstring(result.pid);
            auto files_text = join(result.files, L"; ");

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = index;
            item.lParam = index;
            item.pszText = pid_text.data();
            int row = ListView_InsertItem(g_list, &item);
            if (row < 0)
            {
                break;
            }

            ListView_SetItemText(g_list, row, 1, const_cast<wchar_t*>(result.name.c_str()));
            ListView_SetItemText(g_list, row, 2, const_cast<wchar_t*>(result.user.c_str()));
            ListView_SetItemText(g_list, row, 3, const_cast<wchar_t*>(files_text.c_str()));
            ++index;
        }

        SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_list, nullptr, TRUE);
    }

    void resize_columns(int list_width)
    {
        if (!IsWindow(g_list) || ListView_GetHeader(g_list) == nullptr)
        {
            return;
        }
        const int pid_width = 80;
        const int name_width = 190;
        const int user_width = 150;
        int files_width = list_width - pid_width - name_width - user_width - 30;
        if (files_width < 120)
        {
            files_width = 120;
        }
        ListView_SetColumnWidth(g_list, 0, pid_width);
        ListView_SetColumnWidth(g_list, 1, name_width);
        ListView_SetColumnWidth(g_list, 2, user_width);
        ListView_SetColumnWidth(g_list, 3, files_width);
    }

    void layout(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);

        const int margin = 10;
        const int gap = 6;
        const int label_height = 46;
        const int button_height = 30;
        const int status_height = 22;

        int y = margin;
        MoveWindow(g_path_label, margin, y, client.right - 2 * margin, label_height, TRUE);
        y += label_height + gap;

        int remaining = client.bottom - y - button_height - status_height - margin - gap;
        if (remaining < 60)
        {
            remaining = 60;
        }
        MoveWindow(g_list, margin, y, client.right - 2 * margin, remaining, TRUE);
        y += remaining + gap;

        int x = margin;
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            MoveWindow(g_buttons[i], x, y, g_buttons_spec[i].width, button_height, TRUE);
            x += g_buttons_spec[i].width + gap;
        }

        MoveWindow(g_status, margin, y + button_height + 2, client.right - 2 * margin, status_height, TRUE);

        resize_columns(client.right - 2 * margin);
    }

    // 系统控件（列表、静态文字、按钮）自己不处理 WM_DROPFILES：给它们也注册拖放，
    // 收到后转给主窗口。少了这一步，把文件"拖到列表区域"是完全没有反应的。
    LRESULT CALLBACK drop_subclass_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param, UINT_PTR subclass_id, DWORD_PTR ref_data)
    {
        if (message == WM_DROPFILES)
        {
            if (g_window != nullptr && g_window != hwnd)
            {
                SendMessageW(g_window, WM_DROPFILES, w_param, l_param);
                return 0;
            }
        }
        else if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hwnd, drop_subclass_proc, subclass_id);
        }

        return DefSubclassProc(hwnd, message, w_param, l_param);
    }

    // WM_DPICHANGED 在 _WIN32_WINNT < 0x0603 的头文件里没有定义，自己写常量
    constexpr UINT kDpiChangedMessage = 0x02E0;

    UINT window_dpi(HWND hwnd)
    {
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        static auto get_dpi = reinterpret_cast<GetDpiForWindow_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        return (get_dpi != nullptr && hwnd != nullptr) ? get_dpi(hwnd) : 0;
    }

    HFONT create_ui_font(UINT dpi)
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);

        // SystemParametersInfoForDpi 是 Win10 1607+（5 个参数），老系统退回普通版
        using SystemParametersInfoForDpi_t = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
        static auto spi_for_dpi = reinterpret_cast<SystemParametersInfoForDpi_t>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));

        bool ok = false;
        if (spi_for_dpi != nullptr && dpi != 0)
        {
            ok = spi_for_dpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi) != 0;
        }
        if (!ok)
        {
            ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != 0;
        }
        return ok ? CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
    }

    void apply_ui_font()
    {
        if (g_font == nullptr)
        {
            return;
        }
        SendMessageW(g_path_label, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            SendMessageW(g_buttons[i], WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
    }

    void update_menu_state()
    {
        if (g_main_menu == nullptr)
        {
            return;
        }
        if (HMENU file_menu = GetSubMenu(g_main_menu, 0))
        {
            CheckMenuItem(file_menu, IDM_TOGGLE_CONTEXT_MENU,
                          MF_BYCOMMAND | (shell_integration::is_context_menu_installed() ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void build_menu(HWND hwnd)
    {
        HMENU menu = CreateMenu();
        HMENU file_menu = CreatePopupMenu();
        HMENU tools_menu = CreatePopupMenu();
        HMENU help_menu = CreatePopupMenu();
        if (menu == nullptr || file_menu == nullptr || tools_menu == nullptr || help_menu == nullptr)
        {
            return;
        }

        AppendMenuW(file_menu, MF_STRING, IDM_TOGGLE_CONTEXT_MENU, L"添加到资源管理器右键菜单(&C)");
        AppendMenuW(file_menu, MF_STRING, IDM_INSTALL_SENDTO, L"添加到「发送到」菜单(&S)");
        AppendMenuW(file_menu, MF_STRING, IDM_OPEN_SENDTO, L"打开「发送到」文件夹(&O)");
        AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FILES, L"添加文件…(&F)\tCtrl+O");
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FOLDER, L"添加文件夹…(&D)");
        AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"退出(&X)");

        AppendMenuW(tools_menu, MF_STRING, IDM_REFRESH, L"重新检查(&R)\tF5");
        AppendMenuW(tools_menu, MF_STRING, IDM_ELEVATE, L"以管理员身份重启(&A)");

        AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"关于 FileLocksmith(&A)");
        AppendMenuW(help_menu, MF_STRING, IDM_HOMEPAGE, L"项目主页 (GitHub)(&H)");

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"文件(&F)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_menu), L"工具(&T)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"帮助(&H)");

        SetMenu(hwnd, menu);
        g_main_menu = menu;
        update_menu_state();
    }

    void toggle_context_menu()
    {
        const bool was_installed = shell_integration::is_context_menu_installed();
        const bool ok = was_installed ? shell_integration::uninstall_context_menu()
                                      : shell_integration::install_context_menu();
        update_menu_state();

        if (!ok)
        {
            set_status(was_installed ? L"移除右键菜单失败。" : L"添加右键菜单失败（注册表写入被拒绝？）");
            return;
        }

        set_status(was_installed
                       ? L"已移除资源管理器右键菜单项。"
                       : L"已添加右键菜单项（文件 / 文件夹 / 驱动器 / 目录空白处，仅当前用户）。"
                         L"Windows 11 要进“显示更多选项”才看得到；想出现在一级菜单请用「文件 → 添加到「发送到」菜单」。");
    }

    void add_sendto_menu()
    {
        if (shell_integration::is_sendto_installed())
        {
            set_status(L"「发送到」里已经有 FileLocksmith 了");
            return;
        }

        set_status(shell_integration::install_sendto_shortcut()
                       ? L"已添加到「发送到」菜单（Win11 一级右键菜单里直接可见）。"
                       : L"创建「发送到」快捷方式失败。");
    }

    // 定义在文件后面（拖放 / 菜单 / 对话框都会用到）
    void add_paths(const std::vector<std::wstring>& new_paths);

    bool pick_files(std::vector<std::wstring>& chosen)
    {
        // 多选时缓冲区格式是 "目录\0文件1\0文件2\0\0"；单选时就是完整路径
        std::vector<wchar_t> buffer(32 * 1024, L'\0');

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = g_window;
        dialog.lpstrFilter = L"所有文件\0*.*\0\0";
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrTitle = L"选择要检查的文件（可多选）";
        dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&dialog))
        {
            return false;
        }

        const std::wstring first(buffer.data());
        const wchar_t* item = buffer.data() + first.size() + 1;
        if (*item == L'\0')
        {
            chosen.push_back(first);
            return true;
        }

        while (*item != L'\0')
        {
            const std::wstring name(item);
            chosen.push_back(first + L"\\" + name);
            item += name.size() + 1;
        }
        return true;
    }

    bool pick_folder(std::wstring& chosen)
    {
        // BIF_NEWDIALOGSTYLE 需要调用线程已初始化 COM
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        BROWSEINFOW browse{};
        browse.hwndOwner = g_window;
        browse.lpszTitle = L"选择要检查的文件夹";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        bool ok = false;
        if (LPITEMIDLIST item = SHBrowseForFolderW(&browse))
        {
            wchar_t path[MAX_PATH * 2]{};
            if (SHGetPathFromIDListW(item, path))
            {
                chosen = path;
                ok = true;
            }
            CoTaskMemFree(item);
        }

        if (SUCCEEDED(com_status))
        {
            CoUninitialize();
        }
        return ok;
    }

    void add_files_via_dialog()
    {
        std::vector<std::wstring> chosen;
        if (pick_files(chosen))
        {
            add_paths(chosen);
        }
    }

    void add_folder_via_dialog()
    {
        std::wstring chosen;
        if (pick_folder(chosen))
        {
            add_paths({ chosen });
        }
    }

    void show_about()
    {
        const std::wstring text =
            std::wstring(L"FileLocksmith ") + app::to_wstring(FL_VERSION_STRING) + L"\n"
            L"由 PowerToys FileLocksmith 模块抽离的独立版本（MIT License）\n\n"
            L"运行环境: " + app::os_friendly_name() + L" (" + app::os_version_string() + L")\n" +
            std::wstring(L"管理员权限: ") + (app::is_process_elevated() ? L"是" : L"否") + L"\n" +
            std::wstring(L"右键菜单: ") + (shell_integration::is_context_menu_installed() ? L"已添加" : L"未添加") + L"\n" +
            std::wstring(L"「发送到」: ") + (shell_integration::is_sendto_installed() ? L"已添加" : L"未添加") + L"\n\n"
            L"https://github.com/U707T/FileLocksmith-Standalone";
        MessageBoxW(g_window, text.c_str(), L"关于 FileLocksmith", MB_OK | MB_ICONINFORMATION);
    }

    void start_search()
    {
        if (g_searching)
        {
            // 正在查：排队，等这次结果回来立刻带着新路径再查一遍
            g_search_pending = true;
            return;
        }
        if (g_paths.empty())
        {
            set_status(L"请先添加要检查的文件。");
            return;
        }

        g_searching = true;
        set_busy(true);
        g_worker_running.store(true);

        auto paths = g_paths;
        std::thread([paths] {
            auto payload = new SearchPayload();
            try
            {
                payload->locks = find_locks(paths);
            }
            catch (const std::exception& ex)
            {
                payload->error = L"枚举失败: " + app::to_wstring(ex.what());
            }
            catch (...)
            {
                payload->error = L"枚举失败: 未知错误";
            }

            HWND target = g_post_target.load();
            const bool posted = target != nullptr && PostMessageW(target, WM_FL_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(payload)) != 0;
            if (!posted)
            {
                delete payload;
            }
            g_worker_running.store(false);
        }).detach();
    }

    void kill_selected()
    {
        int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (row < 0 || row >= static_cast<int>(g_results.size()))
        {
            set_status(L"请先在列表里选中一个进程。");
            return;
        }

        auto& process = g_results[row];
        std::wstring question = L"确定要结束 " + process.name + L" (PID " + std::to_wstring(process.pid) + L") 吗？\n\n"
                                L"进程里的未保存数据会丢失。";
        if (MessageBoxW(g_window, question.c_str(), L"结束进程", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        if (app::terminate_process(process.pid))
        {
            set_status(L"已结束进程 " + std::to_wstring(process.pid) + L"，正在重新检查…");
            start_search();
        }
        else
        {
            set_status(L"无法结束该进程，权限不足。可点击“以管理员身份重启”。");
        }
    }

    void kill_all()
    {
        if (g_results.empty())
        {
            return;
        }

        std::wstring question = L"确定要结束全部 " + std::to_wstring(g_results.size()) + L" 个进程吗？\n\n"
                                L"进程里的未保存数据会丢失。";
        if (MessageBoxW(g_window, question.c_str(), L"结束全部进程", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        int failed = 0;
        for (const auto& process : g_results)
        {
            if (!app::terminate_process(process.pid))
            {
                ++failed;
            }
        }

        if (failed == 0)
        {
            set_status(L"已结束全部进程，正在重新检查…");
        }
        else
        {
            set_status(L"有 " + std::to_wstring(failed) + L" 个进程无法结束（权限不足），可点击“以管理员身份重启”。");
        }
        start_search();
    }

    void force_unlock()
    {
        if (g_handles.empty())
        {
            set_status(g_results.empty() ? L"没有可解锁的占用。"
                                         : L"只找到模块映射（文件被当成 DLL/EXE 加载进去了），这种情况没法强制解除，只能结束进程。");
            return;
        }

        std::wstring question = L"强制解除占用？\n\n"
                                L"将关闭其他进程持有的 " + std::to_wstring(g_handles.size()) + L" 个文件句柄，不会结束任何进程。\n\n"
                                L"⚠ 这相当于“不结束程序就直接拔掉 U 盘”：持有该句柄的程序之后可能报错、\n"
                                L"   显示异常甚至崩溃，未保存的数据也可能丢失。\n\n"
                                L"请先保存工作；能结束进程时优先用“结束进程”。\n\n确定继续吗？";
        if (MessageBoxW(g_window, question.c_str(), L"强制解除占用", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        std::vector<std::wstring> failures;
        const auto closed = release_handles(g_handles, failures);

        std::wstring status = L"已强制关闭 " + std::to_wstring(closed) + L"/" + std::to_wstring(g_handles.size()) + L" 个句柄";
        if (!failures.empty())
        {
            status += L"（" + std::to_wstring(failures.size()) + L" 个失败，多半是权限不足）";
        }
        status += L"，正在重新检查…";
        set_status(status);
        start_search();
    }

    void open_file_location()
    {
        int row = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (row < 0 || row >= static_cast<int>(g_results.size()) || g_results[row].files.empty())
        {
            return;
        }

        std::wstring parameters = L"/select,\"" + g_results[row].files.front() + L"\"";
        ShellExecuteW(g_window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void restart_elevated()
    {
        auto args = g_paths;
        if (args.empty())
        {
            args.push_back(L"--gui");
        }
        if (!app::relaunch_elevated(args))
        {
            set_status(L"提权被取消，或无法获取管理员权限。");
            return;
        }
        PostMessageW(g_window, WM_CLOSE, 0, 0);
    }

    void add_paths(const std::vector<std::wstring>& new_paths)
    {
        bool changed = false;
        for (const auto& path : new_paths)
        {
            if (std::find(g_paths.begin(), g_paths.end(), path) == g_paths.end())
            {
                g_paths.push_back(path);
                changed = true;
            }
        }

        if (changed)
        {
            update_path_label();
            start_search();
        }
    }

    HWND find_other_instance()
    {
        struct Context
        {
            DWORD current_pid;
            HWND found;
        } context{ GetCurrentProcessId(), nullptr };

        EnumWindows(
            [](HWND hwnd, LPARAM param) -> BOOL {
                auto ctx = reinterpret_cast<Context*>(param);
                wchar_t class_name[64]{};
                if (GetClassNameW(hwnd, class_name, 64) == 0 || wcscmp(class_name, WndClassName) != 0)
                {
                    return TRUE;
                }

                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != ctx->current_pid)
                {
                    ctx->found = hwnd;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&context));

        return context.found;
    }

    // 返回 false 表示没送出去（例如对方是管理员权限：UIPI 会拦掉 WM_COPYDATA）。
    bool send_paths_to_instance(HWND target, const std::vector<std::wstring>& paths)
    {
        auto payload = join(paths, L"\n");
        COPYDATASTRUCT data{};
        data.dwData = FL_COPYDATA_MAGIC;
        data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(payload.c_str());
        return SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) != 0;
    }

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
    {
        switch (message)
        {
        case WM_CREATE:
            g_window = hwnd;
            g_post_target.store(hwnd);
            g_font = create_ui_font(window_dpi(hwnd));

            g_path_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           0, 0, 0, 0, hwnd, control_id(IDC_PATH_LABEL), g_instance, nullptr);

            g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, hwnd, control_id(IDC_LIST), g_instance, nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            {
                const wchar_t* titles[4] = { L"PID", L"进程", L"用户", L"占用的文件" };
                for (int i = 0; i < 4; ++i)
                {
                    LVCOLUMNW column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    column.pszText = const_cast<wchar_t*>(titles[i]);
                    column.cx = 120;
                    column.iSubItem = i;
                    ListView_InsertColumn(g_list, i, &column);
                }
            }

            for (int i = 0; i < BUTTON_COUNT; ++i)
            {
                g_buttons[i] = CreateWindowExW(0, L"BUTTON", g_buttons_spec[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               0, 0, 0, 0, hwnd, control_id(g_buttons_spec[i].id), g_instance, nullptr);
            }

            g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                       0, 0, 0, 0, hwnd, control_id(IDC_STATUS), g_instance, nullptr);

            apply_ui_font();

            build_menu(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            for (HWND child : { g_list, g_path_label, g_status })
            {
                SetWindowSubclass(child, drop_subclass_proc, DROP_SUBCLASS_ID, 0);
                DragAcceptFiles(child, TRUE);
            }
            for (int i = 0; i < BUTTON_COUNT; ++i)
            {
                SetWindowSubclass(g_buttons[i], drop_subclass_proc, DROP_SUBCLASS_ID, 0);
                DragAcceptFiles(g_buttons[i], TRUE);
            }
            update_path_label();
            layout(hwnd);
            return 0;

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case kDpiChangedMessage:
        {
            // 拖到另一块不同缩放比的显示器：按新 DPI 重建字体，并按系统建议调整窗口大小
            const UINT new_dpi = HIWORD(w_param);
            if (g_font != nullptr)
            {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            g_font = create_ui_font(new_dpi);
            apply_ui_font();

            const RECT* suggested = reinterpret_cast<const RECT*>(l_param);
            if (suggested != nullptr)
            {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            layout(hwnd);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(w_param))
            {
            case IDC_KILL_SELECTED: kill_selected(); return 0;
            case IDC_KILL_ALL: kill_all(); return 0;
            case IDC_FORCE_UNLOCK: force_unlock(); return 0;
            case IDC_REFRESH: start_search(); return 0;
            case IDC_OPEN_LOCATION: open_file_location(); return 0;
            case IDC_ELEVATE: restart_elevated(); return 0;

            case IDM_TOGGLE_CONTEXT_MENU: toggle_context_menu(); return 0;
            case IDM_INSTALL_SENDTO: add_sendto_menu(); return 0;
            case IDM_OPEN_SENDTO:
                if (!shell_integration::open_sendto_folder())
                {
                    set_status(L"打不开「发送到」文件夹。");
                }
                return 0;
            case IDM_ADD_FILES: add_files_via_dialog(); return 0;
            case IDM_ADD_FOLDER: add_folder_via_dialog(); return 0;
            case IDM_REFRESH: start_search(); return 0;
            case IDM_ELEVATE: restart_elevated(); return 0;
            case IDM_ABOUT: show_about(); return 0;
            case IDM_HOMEPAGE:
                ShellExecuteW(g_window, L"open", L"https://github.com/U707T/FileLocksmith-Standalone", nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            case IDM_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;

            default: break;
            }
            break;

        case WM_DROPFILES:
        {
            HDROP drop = reinterpret_cast<HDROP>(w_param);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> dropped;
            for (UINT i = 0; i < count; ++i)
            {
                UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length, L'\0');
                if (DragQueryFileW(drop, i, path.data(), length + 1) != 0)
                {
                    dropped.push_back(path);
                }
            }
            DragFinish(drop);
            add_paths(dropped);
            return 0;
        }

        case WM_COPYDATA:
        {
            auto data = reinterpret_cast<const COPYDATASTRUCT*>(l_param);
            if (data == nullptr || data->dwData != FL_COPYDATA_MAGIC || data->lpData == nullptr)
            {
                return FALSE;
            }

            auto text = reinterpret_cast<const wchar_t*>(data->lpData);
            size_t length = data->cbData / sizeof(wchar_t);
            std::wstring joined(text, length);

            std::vector<std::wstring> incoming;
            size_t start = 0;
            while (start < joined.size())
            {
                auto end = joined.find(L'\n', start);
                if (end == std::wstring::npos)
                {
                    end = joined.size();
                }
                if (end > start)
                {
                    incoming.push_back(joined.substr(start, end - start));
                }
                start = end + 1;
            }

            SetForegroundWindow(g_window);
            add_paths(incoming);
            return TRUE;
        }

        case WM_INITMENUPOPUP:
            // 菜单里的勾选状态每次弹出时重新读一遍注册表（可能在外部被改动）
            if (l_param == 0)
            {
                update_menu_state();
            }
            return 0;

        case WM_KEYDOWN:
            if (w_param == VK_F5)
            {
                start_search();
                return 0;
            }
            break;

        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l_param);
            if (header->hwndFrom == g_list && header->code == NM_DBLCLK)
            {
                open_file_location();
                return 0;
            }
            break;
        }

        case WM_FL_SEARCH_DONE:
        {
            g_searching = false;
            std::unique_ptr<SearchPayload> payload(reinterpret_cast<SearchPayload*>(l_param));
            set_busy(false);

            if (!payload->error.empty())
            {
                set_status(payload->error);
            }
            else
            {
                g_results = payload->locks.processes;
                g_handles = payload->locks.handles;
                g_handles_query_ok = payload->locks.handles_query_ok;
                fill_list();

                const auto unresolved = payload->locks.unresolved_paths.size();
                if (!g_handles_query_ok)
                {
                    set_status(L"句柄枚举失败：结果不可信，请点“以管理员身份重启”后重试。");
                }
                else if (g_results.empty())
                {
                    set_status(unresolved == 0
                                   ? L"没有进程正在占用这些文件。"
                                   : L"没有进程占用 —— 但有 " + std::to_wstring(unresolved) + L" 个路径打不开（不存在或没权限），实际没有被检查。");
                }
                else
                {
                    std::wstring text = L"找到 " + std::to_wstring(g_results.size()) + L" 个进程正在占用" +
                                        L"（可强制关闭的文件句柄 " + std::to_wstring(g_handles.size()) + L" 个）";
                    if (unresolved > 0)
                    {
                        text += L"；另有 " + std::to_wstring(unresolved) + L" 个路径打不开，已跳过";
                    }
                    if (!app::is_process_elevated())
                    {
                        text += L"，系统进程可能因权限不足而看不到";
                    }
                    set_status(text);
                }
            }

            if (g_search_pending)
            {
                g_search_pending = false;
                start_search();
            }
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_font)
            {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            g_post_target.store(nullptr);
            g_window = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, message, w_param, l_param);
    }
}

namespace gui
{
    int run(const std::vector<std::wstring>& paths)
    {
        g_instance = GetModuleHandleW(nullptr);
        g_paths = paths;

        // Multi-select from the shell starts one process per file: hand the paths
        // over to the instance that is already running instead of opening a pile
        // of windows.
        if (!paths.empty())
        {
            if (HWND other = find_other_instance())
            {
                if (send_paths_to_instance(other, paths))
                {
                    return 0;
                }
                // 送不出去（对方提权 / UIPI 拦截）就自己开一个窗口，
                // 不能静默退出 —— 否则用户看到的就是"拖进来完全没反应"。
            }
        }

        INITCOMMONCONTROLSEX controls{};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = wnd_proc;
        window_class.hInstance = g_instance;
        window_class.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(101));
        window_class.hIconSm = window_class.hIcon;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = WndClassName;
        RegisterClassExW(&window_class);

        HWND window = CreateWindowExW(0, WndClassName, WindowTitle, WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 940, 580,
                                      nullptr, nullptr, g_instance, nullptr);
        if (window == nullptr)
        {
            return 1;
        }

        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        if (!g_paths.empty())
        {
            start_search();
        }
        else
        {
            SetFocus(g_list);
            if (app::is_process_elevated())
            {
                set_status(L"管理员模式：拖放被 Windows 禁止（UIPI）。请用「文件 → 添加文件… / 添加文件夹…」，或把文件拖到 exe 图标上。");
            }
            else
            {
                set_status(shell_integration::is_context_menu_installed()
                               ? L"把文件拖进来，或在资源管理器里右键 → 什么正在占用这个文件？"
                               : L"提示: 还没添加右键菜单 —— 点上面的「文件」菜单 → 添加到资源管理器右键菜单。");
            }
        }

        // F5 必须走加速键表：键盘消息是发给焦点控件的（这里是 ListView），
        // 光在主窗口的 WM_KEYDOWN 里判断收不到。
        ACCEL accelerators[] = {
            { FVIRTKEY, VK_F5, IDM_REFRESH },
            { FVIRTKEY | FCONTROL, 'O', IDM_ADD_FILES },
        };
        HACCEL accelerator_table = CreateAcceleratorTableW(accelerators, ARRAYSIZE(accelerators));

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0))
        {
            if (accelerator_table != nullptr && TranslateAcceleratorW(window, accelerator_table, &message) != 0)
            {
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (accelerator_table != nullptr)
        {
            DestroyAcceleratorTable(accelerator_table);
        }

        // 等后台枚举线程收尾（最多 2 秒），别在线程还卡在系统调用里的时候拆 CRT
        for (int i = 0; i < 40 && g_worker_running.load(); ++i)
        {
            Sleep(50);
        }
        return 0;
    }
}
