#include "gui_internal.h"

namespace gui::detail
{
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
        SetWindowTextW(ui().status, text.c_str());
    }

    void update_path_label()
    {
        std::wstring text;
        if (ui().paths.empty())
        {
            text = L"把文件或文件夹拖到窗口里（也可以拖到 exe 图标上）";
            if (app::is_process_elevated())
            {
                text += L"\n当前是管理员模式：若拖放无反应（UIPI），请用「文件 → 添加文件… / 添加文件夹…」";
            }
        }
        else if (ui().paths.size() <= 2)
        {
            text = L"正在检查: " + join(ui().paths, L"\n");
        }
        else
        {
            // 标签只有两行高，路径多了要收敛，不然用户以为只查了前面的
            text = L"正在检查 " + std::to_wstring(ui().paths.size()) + L" 个路径: " + ui().paths[0] +
                   L"、" + ui().paths[1] + L" …";
        }
        SetWindowTextW(ui().path_label, text.c_str());

        std::wstring title = L"FileLocksmith";
        title += ui().paths.empty() ? L" — 什么正在占用这个文件？"
                                 : L" — " + std::to_wstring(ui().paths.size()) + L" 个路径";
        if (app::is_process_elevated())
        {
            title += L"（管理员）";
        }
        if (ui().window != nullptr)
        {
            SetWindowTextW(ui().window, title.c_str());
        }
    }

    void resize_columns(int list_width)
    {
        if (!IsWindow(ui().list) || ListView_GetHeader(ui().list) == nullptr)
        {
            return;
        }
        // 用户手动调过列宽（或上次退出时记下来的）就别再自作聪明
        if (ui().saved_state.column_count >= COLUMN_COUNT && !ui().columns_restored)
        {
            for (int i = 0; i < COLUMN_COUNT; ++i)
            {
                if (ui().saved_state.columns[i] > 0)
                {
                    ListView_SetColumnWidth(ui().list, i, ui().saved_state.columns[i]);
                }
            }
            ui().columns_restored = true;
            return;
        }
        if (ui().columns_restored)
        {
            return;
        }

        const int name_width = 190;
        const int pid_width = 80;
        const int user_width = 140;
        const int kind_width = 90;
        int files_width = list_width - name_width - pid_width - user_width - kind_width - 30;
        if (files_width < 120)
        {
            files_width = 120;
        }
        ListView_SetColumnWidth(ui().list, COL_PROCESS, name_width);
        ListView_SetColumnWidth(ui().list, COL_PID, pid_width);
        ListView_SetColumnWidth(ui().list, COL_USER, user_width);
        ListView_SetColumnWidth(ui().list, COL_KIND, kind_width);
        ListView_SetColumnWidth(ui().list, COL_FILES, files_width);
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
        MoveWindow(ui().path_label, margin, y, client.right - 2 * margin, label_height, TRUE);
        y += label_height + gap;

        int remaining = client.bottom - y - button_height - status_height - margin - gap;
        if (remaining < 60)
        {
            remaining = 60;
        }
        MoveWindow(ui().list, margin, y, client.right - 2 * margin, remaining, TRUE);
        y += remaining + gap;

        int x = margin;
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            MoveWindow(ui().buttons[i], x, y, kButtonSpec[i].width, button_height, TRUE);
            x += kButtonSpec[i].width + gap;
        }

        MoveWindow(ui().status, margin, y + button_height + 2, client.right - 2 * margin, status_height, TRUE);

        resize_columns(client.right - 2 * margin);
    }

    // 提权窗口默认收不到低完整性进程（资源管理器）的拖放：UIPI 既拦消息、也拦共享内存访问。
    // 把这三个消息显式放行后，传统 WM_DROPFILES 路径才能在提权窗口里工作：
    //   WM_DROPFILES            —— 拖放动作本身
    //   WM_COPYDATA             —— 第二个实例转发路径（拖到 exe 图标 / 打开方式）
    //   0x0049 (COPYGLOBALDATA) —— 文件列表本体在共享内存里，DragQueryFileW 读它走这条消息；
    //                              漏放它会出现"能拖进来、但文件名是空的"
    // 走 OLE drag-drop（RegisterDragDrop）的程序在这里救不回来——UIPI 拦在 OLE 层；
    // 本程序用的是传统 DragAcceptFiles，拦在消息层，所以可以放行。
    //
    // 放行的只是消息通道：破坏性操作（结束进程 / 强制解锁 / 删除）仍然逐次弹窗确认，
    // 低完整性进程塞进来的只是路径列表，不会产生任何静默动作。
    void allow_unelevated_drop(HWND wnd)
    {
        constexpr UINT WmCopyGlobalData = 0x0049;
        using ChangeWindowMessageFilterEx_t = BOOL(WINAPI*)(HWND, UINT, DWORD, void*);
        static auto change_filter = load_proc<ChangeWindowMessageFilterEx_t>(
            GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
        if (change_filter == nullptr)
        {
            return;   // 没有这个 API 就维持原状：拖不进来而已，不影响其它功能
        }
        const UINT messages[] = { WM_DROPFILES, WM_COPYDATA, WmCopyGlobalData };
        for (UINT message : messages)
        {
            change_filter(wnd, message, MSGFLT_ALLOW, nullptr);
        }
    }

    LRESULT CALLBACK drop_subclass_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param, UINT_PTR subclass_id, DWORD_PTR ref_data)
    {
        if (message == WM_DROPFILES)
        {
            if (ui().window != nullptr && ui().window != hwnd)
            {
                SendMessageW(ui().window, WM_DROPFILES, w_param, l_param);
                return 0;
            }
        }
        else if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hwnd, drop_subclass_proc, subclass_id);
        }

        return DefSubclassProc(hwnd, message, w_param, l_param);
    }

    UINT window_dpi(HWND hwnd)
    {
        using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
        static auto get_dpi = load_proc<GetDpiForWindow_t>(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        return (get_dpi != nullptr && hwnd != nullptr) ? get_dpi(hwnd) : 0;
    }

    HFONT create_ui_font(UINT dpi)
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);

        // SystemParametersInfoForDpi 是 Win10 1607+（5 个参数），老系统退回普通版
        using SystemParametersInfoForDpi_t = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
        static auto spi_for_dpi = load_proc<SystemParametersInfoForDpi_t>(
            GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");

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
        if (ui().font == nullptr)
        {
            return;
        }
        SendMessageW(ui().path_label, WM_SETFONT, reinterpret_cast<WPARAM>(ui().font), TRUE);
        SendMessageW(ui().list, WM_SETFONT, reinterpret_cast<WPARAM>(ui().font), TRUE);
        SendMessageW(ui().status, WM_SETFONT, reinterpret_cast<WPARAM>(ui().font), TRUE);
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            SendMessageW(ui().buttons[i], WM_SETFONT, reinterpret_cast<WPARAM>(ui().font), TRUE);
        }
    }

    void init_icons()
    {
        const int size = GetSystemMetrics(SM_CXSMICON);
        ui().icons = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 8, 8);
        if (ui().icons != nullptr)
        {
            ListView_SetImageList(ui().list, ui().icons, LVSIL_SMALL);
        }
    }

    int icon_index_for(const std::wstring& path)
    {
        if (ui().icons == nullptr || path.empty())
        {
            return -1;
        }

        if (auto it = ui().icon_cache.find(path); it != ui().icon_cache.end())
        {
            return it->second;
        }

        int index = -1;
        HICON large_icon = nullptr;
        HICON small_icon = nullptr;
        if (ExtractIconExW(path.c_str(), 0, &large_icon, &small_icon, 1) > 0)
        {
            if (small_icon != nullptr)
            {
                index = ImageList_AddIcon(ui().icons, small_icon);
                DestroyIcon(small_icon);
            }
            if (large_icon != nullptr)
            {
                DestroyIcon(large_icon);
            }
        }

        ui().icon_cache[path] = index;
        return index;
    }

    void update_button_states()
    {
        const bool has_selection = !selected_rows().empty();
        EnableWindow(ui().buttons[0], !ui().searching && has_selection);       // 结束进程
        EnableWindow(ui().buttons[1], !ui().searching && !ui().results.empty());  // 全部结束
        EnableWindow(ui().buttons[2], !ui().searching && !ui().results.empty());  // 强制解锁
        EnableWindow(ui().buttons[3], !ui().searching && !ui().paths.empty());    // 删除文件
        EnableWindow(ui().buttons[4], !ui().searching);                        // 重新检查
        EnableWindow(ui().buttons[5], !ui().searching && has_selection);       // 打开所在位置
        EnableWindow(ui().buttons[6], !ui().searching);                        // 以管理员身份重启
    }

    void update_menu_state()
    {
        if (ui().main_menu == nullptr)
        {
            return;
        }
        if (HMENU file_menu = GetSubMenu(ui().main_menu, 0))
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
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FILES, L"检查文件…(&F)\tCtrl+O");
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FOLDER, L"检查文件夹…(&D)");
        AppendMenuW(file_menu, MF_STRING, IDM_APPEND_FILES, L"追加文件到当前检查…(&A)\tCtrl+Shift+O");
        AppendMenuW(file_menu, MF_STRING, IDM_DELETE_FILES, L"删除检查的文件…(&E)");
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
        ui().main_menu = menu;
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
        MessageBoxW(ui().window, text.c_str(), L"关于 FileLocksmith", MB_OK | MB_ICONINFORMATION);
    }
}
