// FileLocksmith standalone - minimal Win32 GUI
// This file is new (PowerToys used a WinUI 3 / Windows App SDK app here).
#include "gui.h"
#include "app_utils.h"
#include "file_locksmith.h"
#include "shell_integration.h"
#include "ui_state.h"

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
    constexpr int IDC_DELETE_FILES = 1010;

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
    constexpr int IDM_LIST_KILL = 2100;
    constexpr int IDM_LIST_LOCATE = 2101;
    constexpr int IDM_LIST_COPY_EXE = 2102;
    constexpr int IDM_LIST_COPY_PID = 2103;
    constexpr int IDM_LIST_COPY_ROW = 2104;
    constexpr int IDM_LIST_SELECT_ALL = 2105;
    constexpr int IDM_DELETE_FILES = 2011;

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
        { IDC_DELETE_FILES, L"删除文件", 110 },
        { IDC_REFRESH, L"重新检查", 90 },
        { IDC_OPEN_LOCATION, L"打开所在位置", 140 },
        { IDC_ELEVATE, L"以管理员身份重启", 170 },
    };
    constexpr int BUTTON_COUNT = static_cast<int>(sizeof(g_buttons_spec) / sizeof(g_buttons_spec[0]));

    constexpr UINT WM_FL_SEARCH_DONE = WM_APP + 1;
    constexpr ULONG FL_COPYDATA_MAGIC = 0x464C534D; // 'FLSM'

    // 列：进程 / PID / 用户 / 占用方式 / 文件
    constexpr int COL_PROCESS = 0;
    constexpr int COL_PID = 1;
    constexpr int COL_USER = 2;
    constexpr int COL_KIND = 3;
    constexpr int COL_FILES = 4;
    constexpr int COLUMN_COUNT = 5;

    constexpr UINT_PTR BUSY_TIMER_ID = 1;

    HINSTANCE g_instance = nullptr;
    HWND g_window = nullptr;
    HWND g_list = nullptr;
    HWND g_status = nullptr;
    HWND g_path_label = nullptr;
    HWND g_buttons[BUTTON_COUNT] = {};
    HFONT g_font = nullptr;
    HMENU g_main_menu = nullptr;
    HIMAGELIST g_icons = nullptr;
    std::map<std::wstring, int> g_icon_cache;
    ui_state::WindowState g_saved_state;
    bool g_columns_restored = false;
    DWORD g_busy_started = 0;

    // 前向声明（这几个 helper 定义在文件后半段）
    void update_button_states();
    int icon_index_for(const std::wstring& path);
    std::vector<int> selected_rows();
    void update_path_label();

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
    // 拖进来的快捷方式 / exe 解析出的目标程序：查询时展开成"它的所有进程（含子进程）"
    std::vector<std::wstring> g_app_targets;
    bool g_handles_query_ok = true;

    struct SearchPayload
    {
        LocksmithResult locks;
        std::wstring error;
        std::wstring app_note;   // 快捷方式展开的说明
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

        std::wstring title = L"FileLocksmith";
        title += g_paths.empty() ? L" — 什么正在占用这个文件？"
                                 : L" — " + std::to_wstring(g_paths.size()) + L" 个路径";
        if (app::is_process_elevated())
        {
            title += L"（管理员）";
        }
        if (g_window != nullptr)
        {
            SetWindowTextW(g_window, title.c_str());
        }
    }

    void set_busy(bool busy)
    {
        g_searching = busy;
        if (busy)
        {
            set_status(L"正在扫描系统句柄…");
        }
        update_button_states();
    }

    void fill_list()
    {
        // 每个进程占了多少个句柄：0 个说明只是模块映射（文件被当成 DLL/EXE 加载）
        std::map<DWORD, int> handle_counts;
        for (const auto& handle : g_handles)
        {
            ++handle_counts[handle.pid];
        }

        SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(g_list);

        int index = 0;
        for (const auto& result : g_results)
        {
            auto pid_text = std::to_wstring(result.pid);
            auto files_text = join(result.files, L"; ");

            const int handle_count = handle_counts.count(result.pid) != 0 ? handle_counts[result.pid] : 0;
            const std::wstring kind_text = handle_count > 0
                                               ? L"句柄 ×" + std::to_wstring(handle_count)
                                               : L"模块映射";

            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            item.iItem = index;
            item.lParam = index;
            item.iImage = icon_index_for(result.image_path);
            item.pszText = const_cast<wchar_t*>(result.name.c_str());
            int row = ListView_InsertItem(g_list, &item);
            if (row < 0)
            {
                break;
            }

            ListView_SetItemText(g_list, row, COL_PID, pid_text.data());
            ListView_SetItemText(g_list, row, COL_USER, const_cast<wchar_t*>(result.user.c_str()));
            ListView_SetItemText(g_list, row, COL_KIND, const_cast<wchar_t*>(kind_text.c_str()));
            ListView_SetItemText(g_list, row, COL_FILES, const_cast<wchar_t*>(files_text.c_str()));
            ++index;
        }

        SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_list, nullptr, TRUE);
        update_button_states();
    }

    void resize_columns(int list_width)
    {
        if (!IsWindow(g_list) || ListView_GetHeader(g_list) == nullptr)
        {
            return;
        }
        // 用户手动调过列宽（或上次退出时记下来的）就别再自作聪明
        if (g_saved_state.column_count >= COLUMN_COUNT && !g_columns_restored)
        {
            for (int i = 0; i < COLUMN_COUNT; ++i)
            {
                if (g_saved_state.columns[i] > 0)
                {
                    ListView_SetColumnWidth(g_list, i, g_saved_state.columns[i]);
                }
            }
            g_columns_restored = true;
            return;
        }
        if (g_columns_restored)
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
        ListView_SetColumnWidth(g_list, COL_PROCESS, name_width);
        ListView_SetColumnWidth(g_list, COL_PID, pid_width);
        ListView_SetColumnWidth(g_list, COL_USER, user_width);
        ListView_SetColumnWidth(g_list, COL_KIND, kind_width);
        ListView_SetColumnWidth(g_list, COL_FILES, files_width);
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

    void init_icons()
    {
        const int size = GetSystemMetrics(SM_CXSMICON);
        g_icons = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 8, 8);
        if (g_icons != nullptr)
        {
            ListView_SetImageList(g_list, g_icons, LVSIL_SMALL);
        }
    }

    // 进程的 exe 图标（同类工具都有，列表一眼能分辨）；取不到就返回 -1，不影响其它功能
    int icon_index_for(const std::wstring& path)
    {
        if (g_icons == nullptr || path.empty())
        {
            return -1;
        }

        if (auto it = g_icon_cache.find(path); it != g_icon_cache.end())
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
                index = ImageList_AddIcon(g_icons, small_icon);
                DestroyIcon(small_icon);
            }
            if (large_icon != nullptr)
            {
                DestroyIcon(large_icon);
            }
        }

        g_icon_cache[path] = index;
        return index;
    }

    void copy_to_clipboard(const std::wstring& text)
    {
        if (text.empty() || !OpenClipboard(g_window))
        {
            return;
        }

        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes))
        {
            if (void* target = GlobalLock(memory))
            {
                memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);   // 成功后所有权归系统
            }
            else
            {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    std::vector<int> selected_rows()
    {
        std::vector<int> rows;
        int row = -1;
        while ((row = ListView_GetNextItem(g_list, row, LVNI_SELECTED)) != -1)
        {
            rows.push_back(row);
        }
        return rows;
    }

    const ProcessResult* result_at(int row)
    {
        if (row < 0 || row >= static_cast<int>(g_results.size()))
        {
            return nullptr;
        }
        return &g_results[row];
    }

    // 没有选中项时那几个按钮应该是灰的（同类工具都这样，别让用户点了才知道）
    void update_button_states()
    {
        const bool has_selection = !selected_rows().empty();
        EnableWindow(g_buttons[0], !g_searching && has_selection);       // 结束进程
        EnableWindow(g_buttons[1], !g_searching && !g_results.empty());  // 全部结束
        EnableWindow(g_buttons[2], !g_searching && !g_results.empty());  // 强制解锁
        EnableWindow(g_buttons[3], !g_searching && !g_paths.empty());    // 删除文件
        EnableWindow(g_buttons[4], !g_searching);                        // 重新检查
        EnableWindow(g_buttons[5], !g_searching && has_selection);       // 打开所在位置
        EnableWindow(g_buttons[6], !g_searching);                        // 以管理员身份重启
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
        g_busy_started = GetTickCount();
        SetTimer(g_window, BUSY_TIMER_ID, 500, nullptr);
        g_worker_running.store(true);

        auto paths = g_paths;
        auto app_targets = g_app_targets;
        std::thread([paths, app_targets] {
            auto payload = new SearchPayload();
            try
            {
                if (!paths.empty())
                {
                    payload->locks = find_locks(paths);
                }
                else
                {
                    payload->locks.handles_query_ok = true;
                    payload->locks.processes_query_ok = true;
                }

                // 快捷方式 / exe：展开成整棵进程树后并入结果
                for (const auto& target : app_targets)
                {
                    auto extra = find_application_processes(target, /* include_children */ true);
                    if (!extra.empty())
                    {
                        payload->app_note += L"「" + target + L"」展开出 " + std::to_wstring(extra.size()) + L" 个进程；";
                    }
                    merge_process_results(payload->locks.processes, extra);
                }
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
        const auto rows = selected_rows();
        if (rows.empty())
        {
            set_status(L"请先在列表里选中进程（按住 Ctrl / Shift 可以多选）。");
            return;
        }

        std::wstring list;
        for (size_t i = 0; i < rows.size() && i < 8; ++i)
        {
            const auto* process = result_at(rows[i]);
            if (process != nullptr)
            {
                list += L"  " + process->name + L" (PID " + std::to_wstring(process->pid) + L")\n";
            }
        }
        if (rows.size() > 8)
        {
            list += L"  …还有 " + std::to_wstring(rows.size() - 8) + L" 个\n";
        }

        const std::wstring question = L"确定要结束下面 " + std::to_wstring(rows.size()) + L" 个进程吗？\n\n" + list +
                                      L"\n进程里的未保存数据会丢失。";
        if (MessageBoxW(g_window, question.c_str(), L"结束进程", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        int failed = 0;
        for (int row : rows)
        {
            const auto* process = result_at(row);
            if (process == nullptr || !app::terminate_process(process->pid))
            {
                ++failed;
            }
        }

        if (failed == 0)
        {
            set_status(L"已结束 " + std::to_wstring(rows.size()) + L" 个进程，正在重新检查…");
        }
        else
        {
            set_status(L"有 " + std::to_wstring(failed) + L" 个进程无法结束（权限不足），可点「以管理员身份重启」。");
        }
        start_search();
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

    // 删除失败后重新核对哪些还在，并把已删除的从列表里拿掉
    void finish_delete()
    {
        std::vector<std::wstring> remaining;
        for (const auto& path : g_paths)
        {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                remaining.push_back(path);
            }
        }

        const size_t deleted = g_paths.size() - remaining.size();
        g_paths = remaining;
        if (g_paths.empty())
        {
            g_app_targets.clear();
        }

        if (deleted > 0)
        {
            set_status(L"已删除 " + std::to_wstring(deleted) + L" 个路径"
                       + (remaining.empty() ? L"。" : L"，还有 " + std::to_wstring(remaining.size()) + L" 个没删掉。"));
        }

        update_path_label();
        if (g_paths.empty())
        {
            g_results.clear();
            g_handles.clear();
            fill_list();
        }
        else
        {
            start_search();
        }
    }

    void run_delete(bool already_released_handles)
    {
        std::vector<std::wstring> targets = g_paths;
        std::wstring error;
        int code = 0;

        if (delete_paths(targets, error, &code))
        {
            finish_delete();
            return;
        }

        const bool lock_like = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == 0x78;
        if (!already_released_handles && lock_like && !g_handles.empty())
        {
            // 第 2 道确认：强制关别人的句柄会影响别的程序，不能替用户点头
            std::wstring holders;
            size_t shown = 0;
            for (const auto& process : g_results)
            {
                if (shown >= 8)
                {
                    holders += L"  …还有其它进程\n";
                    break;
                }
                holders += L"  " + process.name + L" (PID " + std::to_wstring(process.pid) + L")\n";
                ++shown;
            }

            const std::wstring question =
                L"删除失败：" + error + L"\n\n"
                L"要强制关闭占用者的文件句柄后再删一次吗？\n\n"
                L"将关闭 " + std::to_wstring(g_handles.size()) + L" 个句柄，涉及下面这些进程：\n" + holders +
                L"\n⚠ 风险：这相当于不结束程序就直接拔 U 盘 —— 它们之后可能报错、崩溃，\n"
                L"   或者丢掉未保存的数据。这一步不会结束任何进程。\n\n"
                L"如果不想动它们，点「否」，改用「结束进程」或「全部结束」。";

            if (MessageBoxW(g_window, question.c_str(), L"删除文件 — 需要强制解除占用", MB_YESNO | MB_ICONEXCLAMATION) != IDYES)
            {
                set_status(L"已取消删除（文件仍被占用）：" + error);
                return;
            }

            std::vector<std::wstring> failures;
            const auto closed = release_handles(g_handles, failures);
            set_status(L"已强制关闭 " + std::to_wstring(closed) + L" 个句柄，正在重试删除…");
            run_delete(true);
            return;
        }

        // 第 3 道确认：这一步不可还原，措辞要更重
        const std::wstring question =
            L"删除失败：" + error + L"\n\n"
            L"要不要安排「重启后删除」？\n\n"
            L"• 会写进系统的 PendingFileRenameOperations，重启时自动删除\n"
            L"• 不进回收站 —— 删掉就找不回来了\n"
            L"• 需要管理员权限，而且重启之前文件仍然在";

        if (MessageBoxW(g_window, question.c_str(), L"删除文件 — 重启后删除（不可还原）", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            set_status(L"删除已取消：" + error);
            return;
        }

        std::wstring reboot_error;
        if (delete_on_reboot(targets, reboot_error))
        {
            set_status(L"已安排重启后删除（重启时生效）");
            finish_delete();
        }
        else
        {
            set_status(L"安排重启后删除失败：" + reboot_error);
        }
    }

    // 对话框里逐条列出要删的路径；文件夹必须标出来（删文件夹 = 连里面的东西一起删）
    std::wstring describe_paths_for_dialog()
    {
        constexpr size_t max_shown = 10;
        std::wstring text;
        const size_t count = std::min(g_paths.size(), max_shown);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& path = g_paths[i];
            const DWORD attributes = GetFileAttributesW(path.c_str());
            const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            text += L"  " + path + L"\n";
            if (is_directory)
            {
                text += L"      ↑ 文件夹：里面的内容会一起删除\n";
            }
        }
        if (g_paths.size() > count)
        {
            text += L"  …以及另外 " + std::to_wstring(g_paths.size() - count) + L" 个路径\n";
        }
        return text;
    }

    // 第 1 道确认：删什么、删到哪去
    void delete_checked_paths()
    {
        if (g_paths.empty())
        {
            set_status(L"没有要删除的路径。");
            return;
        }

        const std::wstring question =
            L"确定要删除下面 " + std::to_wstring(g_paths.size()) + L" 个路径吗？\n\n" +
            describe_paths_for_dialog() +
            L"\n删除后它们会进入回收站，可以从回收站还原。\n"
            L"（如果该驱动器禁用了回收站，则会变成永久删除）\n\n"
            L"若文件正被程序占用，下一步会单独问你一次 —— 那一步才会去动别人的句柄。";

        if (MessageBoxW(g_window, question.c_str(), L"删除文件 — 确认删除", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            set_status(L"已取消删除。");
            return;
        }

        run_delete(false);
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
        // 多选时按顺序打开每一个（资源管理器会自己并成窗口）
        const auto rows = selected_rows();
        bool opened = false;
        for (int row : rows)
        {
            const auto* process = result_at(row);
            if (process == nullptr || process->files.empty())
            {
                continue;
            }

            std::wstring parameters = L"/select,\"" + process->files.front() + L"\"";
            ShellExecuteW(g_window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
            opened = true;
        }

        if (!opened)
        {
            set_status(L"选中的行里没有可定位的文件。");
        }
    }

    // 列表里的右键菜单 / 复制 / 快捷键（同类工具都有）
    void copy_selected(bool exe_path, bool pid_only, bool whole_row)
    {
        const auto rows = selected_rows();
        std::wstring text;
        for (int row : rows)
        {
            const auto* process = result_at(row);
            if (process == nullptr)
            {
                continue;
            }

            if (!text.empty())
            {
                text += L"\r\n";
            }

            if (exe_path)
            {
                text += process->image_path.empty() ? process->name : process->image_path;
            }
            else if (pid_only)
            {
                text += std::to_wstring(process->pid);
            }
            else
            {
                text += process->name + L"\t" + std::to_wstring(process->pid) + L"\t" + process->user + L"\t" +
                        join(process->files, L"; ");
            }
        }

        if (text.empty())
        {
            set_status(L"没有可复制的内容（先选中几行）。");
            return;
        }

        copy_to_clipboard(text);
        set_status(L"已复制 " + std::to_wstring(rows.size()) + L" 行到剪贴板。");
    }

    void show_list_menu()
    {
        const bool has_selection = !selected_rows().empty();

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const UINT state = has_selection ? MF_ENABLED : MF_GRAYED;
        AppendMenuW(menu, MF_STRING | state, IDM_LIST_KILL, L"结束选中的进程(&K)");
        AppendMenuW(menu, MF_STRING | state, IDM_LIST_LOCATE, L"打开所在位置(&L)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | state, IDM_LIST_COPY_EXE, L"复制进程路径(&P)");
        AppendMenuW(menu, MF_STRING | state, IDM_LIST_COPY_PID, L"复制 PID(&I)");
        AppendMenuW(menu, MF_STRING | state, IDM_LIST_COPY_ROW, L"复制整行(&R)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_LIST_SELECT_ALL, L"全选(&A)");

        POINT cursor{};
        GetCursorPos(&cursor);
        const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
        DestroyMenu(menu);

        if (command != 0)
        {
            SendMessageW(g_window, WM_COMMAND, static_cast<WPARAM>(command), 0);
        }
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

    void add_app_target(const std::wstring& target)
    {
        if (!target.empty() && std::find(g_app_targets.begin(), g_app_targets.end(), target) == g_app_targets.end())
        {
            g_app_targets.push_back(target);
        }
    }

    void add_paths(const std::vector<std::wstring>& new_paths)
    {
        bool changed = false;
        for (const auto& path : new_paths)
        {
            if (std::find(g_paths.begin(), g_paths.end(), path) != g_paths.end())
            {
                continue;
            }

            g_paths.push_back(path);
            changed = true;

            // 快捷方式：解析出目标程序，按"这个程序的所有进程"处理
            if (shell_integration::is_shortcut(path))
            {
                std::wstring target;
                std::wstring arguments;
                if (shell_integration::resolve_shortcut(path, target, arguments))
                {
                    add_app_target(target);
                }
                else
                {
                    set_status(L"解析快捷方式失败，按普通文件处理: " + path);
                }
            }
            else if (shell_integration::is_executable(path))
            {
                add_app_target(path);
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
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                     0, 0, 0, 0, hwnd, control_id(IDC_LIST), g_instance, nullptr);
            ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            {
                const wchar_t* titles[COLUMN_COUNT] = { L"进程", L"PID", L"用户", L"占用方式", L"文件" };
                for (int i = 0; i < COLUMN_COUNT; ++i)
                {
                    LVCOLUMNW column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    column.pszText = const_cast<wchar_t*>(titles[i]);
                    column.cx = 120;
                    column.iSubItem = i;
                    ListView_InsertColumn(g_list, i, &column);
                }
                init_icons();
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
                ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED);
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
            case IDM_ADD_FILES: add_files_via_dialog(); return 0;
            case IDM_ADD_FOLDER: add_folder_via_dialog(); return 0;
            case IDM_DELETE_FILES: delete_checked_paths(); return 0;
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

        case WM_TIMER:
            if (w_param == BUSY_TIMER_ID && g_searching)
            {
                const auto elapsed = (GetTickCount() - g_busy_started) / 1000;
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
            if (header->hwndFrom != g_list)
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
                ScreenToClient(g_list, &client);
                LVHITTESTINFO hit{};
                hit.pt = client;
                const int row = ListView_HitTest(g_list, &hit);
                if (row >= 0)
                {
                    if ((ListView_GetItemState(g_list, row, LVIS_SELECTED) & LVIS_SELECTED) == 0)
                    {
                        ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
                        ListView_SetItemState(g_list, row, LVIS_SELECTED, LVIS_SELECTED);
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
            KillTimer(g_window, BUSY_TIMER_ID);
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

            if (g_search_pending)
            {
                g_search_pending = false;
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
            if (g_list != nullptr)
            {
                state.column_count = COLUMN_COUNT;
                for (int i = 0; i < COLUMN_COUNT; ++i)
                {
                    state.columns[i] = ListView_GetColumnWidth(g_list, i);
                }
            }
            ui_state::save(state);

            DestroyWindow(hwnd);
            return 0;
        }

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

        g_saved_state = ui_state::load();
        const int window_x = g_saved_state.x != INT_MIN ? g_saved_state.x : CW_USEDEFAULT;
        const int window_y = g_saved_state.y != INT_MIN ? g_saved_state.y : CW_USEDEFAULT;
        const int window_width = g_saved_state.width > 480 ? g_saved_state.width : 980;
        const int window_height = g_saved_state.height > 320 ? g_saved_state.height : 620;

        HWND window = CreateWindowExW(0, WndClassName, WindowTitle, WS_OVERLAPPEDWINDOW,
                                      window_x, window_y, window_width, window_height,
                                      nullptr, nullptr, g_instance, nullptr);
        if (window == nullptr)
        {
            return 1;
        }

        ShowWindow(window, g_saved_state.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
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
            { FVIRTKEY, VK_DELETE, IDM_LIST_KILL },
            { FVIRTKEY | FCONTROL, 'O', IDM_ADD_FILES },
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
        for (int i = 0; i < 40 && g_worker_running.load(); ++i)
        {
            Sleep(50);
        }
        return 0;
    }
}
