#include "gui.h"
#include "gui_internal.h"

namespace gui::detail
{
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
            ui().window = hwnd;
            ui().post_target.store(hwnd);
            ui().font = create_ui_font(window_dpi(hwnd));

            ui().path_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           0, 0, 0, 0, hwnd, control_id(IDC_PATH_LABEL), ui().instance, nullptr);

            ui().list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, hwnd, control_id(IDC_LIST), ui().instance, nullptr);
            ListView_SetExtendedListViewStyle(ui().list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            {
                const wchar_t* titles[COLUMN_COUNT] = { L"进程", L"PID", L"用户", L"占用方式", L"文件" };
                for (int i = 0; i < COLUMN_COUNT; ++i)
                {
                    LVCOLUMNW column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    column.pszText = const_cast<wchar_t*>(titles[i]);
                    column.cx = 120;
                    column.iSubItem = i;
                    ListView_InsertColumn(ui().list, i, &column);
                }
                init_icons();
            }

            for (int i = 0; i < BUTTON_COUNT; ++i)
            {
                ui().buttons[i] = CreateWindowExW(0, L"BUTTON", kButtonSpec[i].text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               0, 0, 0, 0, hwnd, control_id(kButtonSpec[i].id), ui().instance, nullptr);
            }

            ui().status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                       0, 0, 0, 0, hwnd, control_id(IDC_STATUS), ui().instance, nullptr);

            apply_ui_font();

            build_menu(hwnd);
            // 提权模式下这些窗口也要能收到资源管理器的拖放（见 allow_unelevated_drop 的说明）
            allow_unelevated_drop(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            for (HWND child : { ui().list, ui().path_label, ui().status })
            {
                SetWindowSubclass(child, drop_subclass_proc, DROP_SUBCLASS_ID, 0);
                allow_unelevated_drop(child);
                DragAcceptFiles(child, TRUE);
            }
            for (int i = 0; i < BUTTON_COUNT; ++i)
            {
                SetWindowSubclass(ui().buttons[i], drop_subclass_proc, DROP_SUBCLASS_ID, 0);
                allow_unelevated_drop(ui().buttons[i]);
                DragAcceptFiles(ui().buttons[i], TRUE);
            }
            update_path_label();
            layout(hwnd);
            return 0;

        case WM_GETMINMAXINFO:
        {
            // 按钮是固定宽度横排的，窗口缩得太小右侧按钮会被裁掉 —— 直接限制最小宽度
            auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
            int buttons_width = 0;
            for (int i = 0; i < BUTTON_COUNT; ++i)
            {
                buttons_width += kButtonSpec[i].width + 6;
            }
            info->ptMinTrackSize.x = buttons_width + 28;
            info->ptMinTrackSize.y = 340;
            return 0;
        }

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case kDpiChangedMessage:
        {
            // 拖到另一块不同缩放比的显示器：按新 DPI 重建字体，并按系统建议调整窗口大小
            const UINT new_dpi = HIWORD(w_param);
            if (ui().font != nullptr)
            {
                DeleteObject(ui().font);
                ui().font = nullptr;
            }
            ui().font = create_ui_font(new_dpi);
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
            case IDC_DELETE_FILES: delete_checked_paths(); return 0;
            case IDC_REFRESH: start_search(); return 0;
            case IDC_OPEN_LOCATION: open_file_location(); return 0;
            case IDC_ELEVATE: restart_elevated(); return 0;

            case IDM_LIST_KILL: kill_selected(); return 0;
            case IDM_LIST_LOCATE: open_file_location(); return 0;
            case IDM_LIST_COPY_EXE: copy_selected(true, false, false); return 0;
            case IDM_LIST_COPY_PID: copy_selected(false, true, false); return 0;
            case IDM_LIST_COPY_ROW: copy_selected(false, false, true); return 0;
            case IDM_LIST_SELECT_ALL:
                ListView_SetItemState(ui().list, -1, LVIS_SELECTED, LVIS_SELECTED);
                update_button_states();
                return 0;

            case IDM_TOGGLE_CONTEXT_MENU: toggle_context_menu(); return 0;
            case IDM_INSTALL_SENDTO: add_sendto_menu(); return 0;
            case IDM_OPEN_SENDTO:
                if (!shell_integration::open_sendto_folder())
                {
                    set_status(L"打不开「发送到」文件夹。");
                }
                return 0;
            case IDM_ADD_FILES: add_files_via_dialog(true); return 0;
            case IDM_ADD_FOLDER: add_folder_via_dialog(true); return 0;
            case IDM_APPEND_FILES: add_files_via_dialog(false); return 0;
            case IDM_DELETE_FILES: delete_checked_paths(); return 0;
            case IDM_REFRESH: start_search(); return 0;
            case IDM_ELEVATE: restart_elevated(); return 0;
            case IDM_ABOUT: show_about(); return 0;
            case IDM_HOMEPAGE:
                ShellExecuteW(ui().window, L"open", L"https://github.com/U707T/FileLocksmith-Standalone", nullptr, nullptr, SW_SHOWNORMAL);
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
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0)
                {
                    continue;
                }
                // 缓冲区长出一格再按查询到的长度截断：直接写 data()[size()] 属于越界写
                std::wstring path(static_cast<size_t>(length) + 2, L'\0');
                if (DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())) != 0)
                {
                    path.resize(length);
                    dropped.push_back(path);
                }
            }
            DragFinish(drop);
            add_paths(dropped, /* replace */ true);
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
            // 发送方把结尾的 '\0' 也算进了 cbData：按 NUL 截断，否则每个路径都会带上
            // 一个内嵌的 '\0'（标题栏显示、去重比较都会变得古怪）。
            const size_t capacity = data->cbData / sizeof(wchar_t);
            const size_t length = wcsnlen(text, capacity);
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

            SetForegroundWindow(ui().window);
            add_paths(incoming, /* replace */ true);
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

        case WM_TIMER:
            if (w_param == BUSY_TIMER_ID && ui().searching)
            {
                const auto elapsed = (GetTickCount() - ui().busy_started) / 1000;
                if (elapsed >= 1)
                {
                    set_status(L"正在扫描系统句柄…已 " + std::to_wstring(elapsed) + L" 秒"
                               L"（进程多、模块多的时候会久一点）");
                }
                return 0;
            }
            break;

        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l_param);
            if (header->hwndFrom != ui().list)
            {
                break;
            }

            if (header->code == NM_DBLCLK)
            {
                open_file_location();
                return 0;
            }

            if (header->code == NM_RCLICK)
            {
                // 右键先选中光标下那一行（Windows 的通用习惯），再弹菜单
                POINT cursor{};
                GetCursorPos(&cursor);
                POINT client = cursor;
                ScreenToClient(ui().list, &client);
                LVHITTESTINFO hit{};
                hit.pt = client;
                const int row = ListView_HitTest(ui().list, &hit);
                if (row >= 0)
                {
                    if ((ListView_GetItemState(ui().list, row, LVIS_SELECTED) & LVIS_SELECTED) == 0)
                    {
                        ListView_SetItemState(ui().list, -1, 0, LVIS_SELECTED);
                        ListView_SetItemState(ui().list, row, LVIS_SELECTED, LVIS_SELECTED);
                    }
                    update_button_states();
                }
                show_list_menu();
                return 0;
            }

            if (header->code == LVN_ITEMCHANGED)
            {
                update_button_states();
                return 0;
            }

            break;
        }

        case WM_FL_SEARCH_DONE:
        {
            std::unique_ptr<SearchPayload> payload(reinterpret_cast<SearchPayload*>(l_param));

            // 用户在这轮扫描跑完之前又拖了新文件：这次结果已经过期，直接丢掉，
            // 不能让它盖掉新一轮的结果（否则界面会先显示旧文件的内容）
            if (payload->generation != ui().search_generation)
            {
                // 这一轮 worker 已经结束：先把界面从"正在扫描"状态收回来，
                // 否则 searching 一直为真，新扫描永远起不来（界面看起来就是"拖不进去"）
                KillTimer(ui().window, BUSY_TIMER_ID);
                set_busy(false);

                if (ui().search_pending)
                {
                    ui().search_pending = false;
                    start_search();
                }
                return 0;
            }

            KillTimer(ui().window, BUSY_TIMER_ID);
            set_busy(false);

            if (!payload->error.empty())
            {
                set_status(payload->error);
            }
            else
            {
                ui().results = payload->locks.processes;
                ui().handles = payload->locks.handles;
                ui().handles_query_ok = payload->locks.handles_query_ok;
                fill_list();

                const auto unresolved = payload->locks.unresolved_paths.size();
                if (!ui().handles_query_ok)
                {
                    set_status(L"句柄枚举失败：结果不可信，请点“以管理员身份重启”后重试。");
                }
                else if (ui().results.empty())
                {
                    set_status(unresolved == 0
                                   ? L"没有进程正在占用这些文件。"
                                   : L"没有进程占用 —— 但有 " + std::to_wstring(unresolved) + L" 个路径打不开（不存在或没权限），实际没有被检查。");
                }
                else
                {
                    std::wstring text = L"找到 " + std::to_wstring(ui().results.size()) + L" 个进程正在占用" +
                                        L"（可强制关闭的文件句柄 " + std::to_wstring(ui().handles.size()) + L" 个）";
                    if (!payload->app_note.empty())
                    {
                        text += L"；" + payload->app_note;
                    }
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

            if (ui().search_pending)
            {
                ui().search_pending = false;
                start_search();
            }
            return 0;
        }

        case WM_CLOSE:
        {
            // 记住窗口位置/大小与列宽（同类工具都会记）
            ui_state::WindowState state;
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (GetWindowPlacement(hwnd, &placement))
            {
                state.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
                state.x = placement.rcNormalPosition.left;
                state.y = placement.rcNormalPosition.top;
                state.width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
                state.height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
            }
            if (ui().list != nullptr)
            {
                state.column_count = COLUMN_COUNT;
                for (int i = 0; i < COLUMN_COUNT; ++i)
                {
                    state.columns[i] = ListView_GetColumnWidth(ui().list, i);
                }
            }
            ui_state::save(state);

            DestroyWindow(hwnd);
            return 0;
        }

        case WM_DESTROY:
            if (ui().font)
            {
                DeleteObject(ui().font);
                ui().font = nullptr;
            }
            ui().post_target.store(nullptr);
            ui().window = nullptr;
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
        // 界面实现都在 gui::detail 里，这里引进来省得每处都写前缀
        using namespace detail;

        ui().instance = GetModuleHandleW(nullptr);
        ui().paths = paths;

        // Multi-select from the shell starts one process per file: hand the paths
        // over to the instance that is already running instead of opening a pile
        // of windows.
        // （提权窗口现在也放行了 WM_COPYDATA，所以"拖到 exe 图标"在管理员模式下同样能转发成功；
        //   只有送不进去时才退化成自己开一个窗口——绝不允许静默退出。）
        if (!paths.empty())
        {
            if (HWND other = detail::find_other_instance())
            {
                if (detail::send_paths_to_instance(other, paths))
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
        window_class.hInstance = ui().instance;
        window_class.hIcon = LoadIconW(ui().instance, MAKEINTRESOURCEW(101));
        window_class.hIconSm = window_class.hIcon;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = WndClassName;
        RegisterClassExW(&window_class);

        ui().saved_state = ui_state::load();
        const int window_x = ui().saved_state.x != INT_MIN ? ui().saved_state.x : CW_USEDEFAULT;
        const int window_y = ui().saved_state.y != INT_MIN ? ui().saved_state.y : CW_USEDEFAULT;
        const int window_width = ui().saved_state.width > 480 ? ui().saved_state.width : 980;
        const int window_height = ui().saved_state.height > 320 ? ui().saved_state.height : 620;

        HWND window = CreateWindowExW(0, WndClassName, WindowTitle, WS_OVERLAPPEDWINDOW,
                                      window_x, window_y, window_width, window_height,
                                      nullptr, nullptr, ui().instance, nullptr);
        if (window == nullptr)
        {
            return 1;
        }

        ShowWindow(window, ui().saved_state.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        UpdateWindow(window);

        if (!ui().paths.empty())
        {
            detail::start_search();
        }
        else
        {
            SetFocus(ui().list);
            if (app::is_process_elevated())
            {
                detail::set_status(L"管理员模式：若拖放没有反应（UIPI 限制），请用「文件 → 添加文件… / 添加文件夹…」，或把文件拖到 exe 图标上。");
            }
            else
            {
                detail::set_status(shell_integration::is_context_menu_installed()
                               ? L"把文件拖进来，或在资源管理器里右键 → 什么正在占用这个文件？"
                               : L"提示: 还没添加右键菜单 —— 点上面的「文件」菜单 → 添加到资源管理器右键菜单。");
            }
        }

        // F5 必须走加速键表：键盘消息是发给焦点控件的（这里是 ListView），
        // 光在主窗口的 WM_KEYDOWN 里判断收不到。
        ACCEL accelerators[] = {
            { FVIRTKEY, VK_F5, IDM_REFRESH },
            { FVIRTKEY, VK_DELETE, IDM_LIST_KILL },
            { FVIRTKEY | FCONTROL, 'O', IDM_ADD_FILES },
            { FVIRTKEY | FCONTROL | FSHIFT, 'O', IDM_APPEND_FILES },
            { FVIRTKEY | FCONTROL, 'C', IDM_LIST_COPY_ROW },
            { FVIRTKEY | FCONTROL, 'A', IDM_LIST_SELECT_ALL },
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
        for (int i = 0; i < 40 && ui().worker_running.load(); ++i)
        {
            Sleep(50);
        }
        return 0;
    }
}
