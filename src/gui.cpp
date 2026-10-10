// FileLocksmith standalone - minimal Win32 GUI
// This file is new (PowerToys used a WinUI 3 / Windows App SDK app here).
#include "gui.h"
#include "app_utils.h"
#include "file_locksmith.h"

#include <commctrl.h>

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

    constexpr UINT WM_FL_SEARCH_DONE = WM_APP + 1;
    constexpr ULONG FL_COPYDATA_MAGIC = 0x464C534D; // 'FLSM'

    HINSTANCE g_instance = nullptr;
    HWND g_window = nullptr;
    HWND g_list = nullptr;
    HWND g_status = nullptr;
    HWND g_path_label = nullptr;
    HWND g_buttons[5] = {};
    HFONT g_font = nullptr;

    bool g_searching = false;
    // Written by the UI thread, read by the worker thread that posts results.
    std::atomic<HWND> g_post_target{ nullptr };
    std::vector<std::wstring> g_paths;
    std::vector<ProcessResult> g_results;

    struct SearchPayload
    {
        std::vector<ProcessResult> results;
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
        }
        else
        {
            text = L"正在检查: " + join(g_paths, L"\n");
        }
        SetWindowTextW(g_path_label, text.c_str());
    }

    void set_busy(bool busy)
    {
        EnableWindow(g_buttons[0], !busy);
        EnableWindow(g_buttons[1], !busy);
        EnableWindow(g_buttons[2], !busy);
        EnableWindow(g_buttons[3], !busy);
        EnableWindow(g_buttons[4], !busy);
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
        const int widths[5] = { 110, 110, 90, 140, 170 };
        for (int i = 0; i < 5; ++i)
        {
            MoveWindow(g_buttons[i], x, y, widths[i], button_height, TRUE);
            x += widths[i] + gap;
        }

        MoveWindow(g_status, margin, y + button_height + 2, client.right - 2 * margin, status_height, TRUE);

        resize_columns(client.right - 2 * margin);
    }

    void start_search()
    {
        if (g_searching)
        {
            return;
        }
        if (g_paths.empty())
        {
            set_status(L"请先添加要检查的文件。");
            return;
        }

        g_searching = true;
        set_busy(true);

        auto paths = g_paths;
        std::thread([paths] {
            auto payload = new SearchPayload();
            try
            {
                payload->results = find_processes_recursive(paths);
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
            if (target == nullptr || PostMessageW(target, WM_FL_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(payload)) == 0)
            {
                delete payload;
            }
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

    void send_paths_to_instance(HWND target, const std::vector<std::wstring>& paths)
    {
        auto payload = join(paths, L"\n");
        COPYDATASTRUCT data{};
        data.dwData = FL_COPYDATA_MAGIC;
        data.cbData = static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t));
        data.lpData = const_cast<wchar_t*>(payload.c_str());
        SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    }

    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param)
    {
        switch (message)
        {
        case WM_CREATE:
            g_window = hwnd;
            g_post_target.store(hwnd);
            {
                NONCLIENTMETRICSW metrics{};
                metrics.cbSize = sizeof(metrics);
                if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
                {
                    g_font = CreateFontIndirectW(&metrics.lfMessageFont);
                }
            }

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

            for (int i = 0; i < 5; ++i)
            {
                g_buttons[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                               0, 0, 0, 0, hwnd, control_id(IDC_KILL_SELECTED + i), g_instance, nullptr);
            }
            SetWindowTextW(g_buttons[0], L"结束进程");
            SetWindowTextW(g_buttons[1], L"全部结束");
            SetWindowTextW(g_buttons[2], L"重新检查");
            SetWindowTextW(g_buttons[3], L"打开所在位置");
            SetWindowTextW(g_buttons[4], L"以管理员身份重启");

            g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                                       0, 0, 0, 0, hwnd, control_id(IDC_STATUS), g_instance, nullptr);

            for (HWND control : { g_path_label, g_list, g_status, g_buttons[0], g_buttons[1], g_buttons[2], g_buttons[3], g_buttons[4] })
            {
                if (g_font)
                {
                    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
                }
            }

            DragAcceptFiles(hwnd, TRUE);
            update_path_label();
            layout(hwnd);
            return 0;

        case WM_SIZE:
            layout(hwnd);
            return 0;

        case WM_COMMAND:
            switch (LOWORD(w_param))
            {
            case IDC_KILL_SELECTED: kill_selected(); return 0;
            case IDC_KILL_ALL: kill_all(); return 0;
            case IDC_REFRESH: start_search(); return 0;
            case IDC_OPEN_LOCATION: open_file_location(); return 0;
            case IDC_ELEVATE: restart_elevated(); return 0;
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
                g_results = payload->results;
                fill_list();

                if (g_results.empty())
                {
                    set_status(L"没有进程正在占用这些文件。");
                }
                else
                {
                    std::wstring text = L"找到 " + std::to_wstring(g_results.size()) + L" 个进程正在占用";
                    if (!app::is_process_elevated())
                    {
                        text += L"（有系统进程可能因权限不足而看不到）";
                    }
                    set_status(text);
                }
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
                send_paths_to_instance(other, paths);
                return 0;
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
        }

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // Give the (detached) enumeration thread a moment to notice that the
        // window is gone before the process tears down.
        if (g_searching)
        {
            Sleep(300);
        }
        return 0;
    }
}
