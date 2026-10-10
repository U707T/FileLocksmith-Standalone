// FileLocksmith standalone - GUI 内部共享声明
// 只有 gui_*.cpp 会 include 这个头文件；对外入口还是 gui.h 里的 gui::run()。
//
// 拆分自原来 1686 行的 gui.cpp：
//   gui_chrome.cpp  —— 窗口/控件/布局/字体与 DPI/菜单/图标/状态栏
//   gui_search.cpp  —— 扫描线程、结果填充、路径收集
//   gui_actions.cpp —— 结束进程/强制解锁/删除/定位/复制/文件对话框
//   gui_main.cpp    —— 窗口过程与程序入口
#pragma once

#include "win_common.h"
#include "app_utils.h"
#include "file_locksmith.h"
#include "shell_integration.h"
#include "ui_state.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <version.h>

namespace gui::detail
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
    constexpr UINT_PTR DROP_SUBCLASS_ID = 1;
    struct ButtonSpec
    {
        int id;
        const wchar_t* text;
        int width;
    };
    inline constexpr ButtonSpec kButtonSpec[] = {
        { IDC_KILL_SELECTED, L"结束进程", 110 },
        { IDC_KILL_ALL, L"全部结束", 110 },
        { IDC_FORCE_UNLOCK, L"强制解锁", 110 },
        { IDC_DELETE_FILES, L"删除文件", 110 },
        { IDC_REFRESH, L"重新检查", 90 },
        { IDC_OPEN_LOCATION, L"打开所在位置", 140 },
        { IDC_ELEVATE, L"以管理员身份重启", 170 },
    };
    constexpr int BUTTON_COUNT = static_cast<int>(sizeof(kButtonSpec) / sizeof(kButtonSpec[0]));
    constexpr UINT WM_FL_SEARCH_DONE = WM_APP + 1;
    constexpr ULONG FL_COPYDATA_MAGIC = 0x464C534D; // 'FLSM'
    constexpr int COL_PROCESS = 0;
    constexpr int COL_PID = 1;
    constexpr int COL_USER = 2;
    constexpr int COL_KIND = 3;
    constexpr int COL_FILES = 4;
    constexpr int COLUMN_COUNT = 5;
    constexpr UINT_PTR BUSY_TIMER_ID = 1;
    void update_button_states();
    int icon_index_for(const std::wstring& path);
    std::vector<int> selected_rows();
    void update_path_label();
    struct SearchPayload
    {
        LocksmithResult locks;
        std::wstring error;
        std::wstring app_note;   // 快捷方式展开的说明
    };
    constexpr UINT kDpiChangedMessage = 0x02E0;
    void add_paths(const std::vector<std::wstring>& new_paths);

    // 界面状态（原来散在匿名命名空间里的全局变量）
    struct UiState
    {
        HINSTANCE instance = nullptr;
        HWND window = nullptr;
        HWND list = nullptr;
        HWND status = nullptr;
        HWND path_label = nullptr;
        HWND buttons [BUTTON_COUNT] = {};
        HFONT font = nullptr;
        HMENU main_menu = nullptr;
        HIMAGELIST icons = nullptr;
        std::map<std::wstring, int> icon_cache;
        ui_state::WindowState saved_state;
        bool columns_restored = false;
        DWORD busy_started = 0;
        bool searching = false;
        bool search_pending = false;
        std::atomic<bool> worker_running { false };
        std::atomic<HWND> post_target { nullptr };
        std::vector<std::wstring> paths;
        std::vector<ProcessResult> results;
        std::vector<LockedHandle> handles;
        std::vector<std::wstring> app_targets;
        bool handles_query_ok = true;
    };

    inline UiState& ui()
    {
        static UiState state;
        return state;
    }

    // ---- 各文件里的函数（模块内部使用）----
    void add_app_target(const std::wstring& target);
    void add_files_via_dialog();
    void add_folder_via_dialog();
    void add_paths(const std::vector<std::wstring>& new_paths);
    void add_sendto_menu();
    void apply_ui_font();
    void build_menu(HWND hwnd);
    // 控件 ID 通过 CreateWindowExW 的 HMENU 参数传递；经 INT_PTR 中转以满足 64 位 MSVC（C4312）
    inline HMENU control_id(int id)
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }
    void copy_selected(bool exe_path, bool pid_only, bool whole_row);
    void copy_to_clipboard(const std::wstring& text);
    HFONT create_ui_font(UINT dpi);
    void delete_checked_paths();
    std::wstring describe_paths_for_dialog();
    LRESULT CALLBACK drop_subclass_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param, UINT_PTR subclass_id, DWORD_PTR ref_data);
    void fill_list();
    HWND find_other_instance();
    void finish_delete();
    void force_unlock();
    int icon_index_for(const std::wstring& path);
    void init_icons();
    std::wstring join(const std::vector<std::wstring>& items, const wchar_t* separator);
    void kill_all();
    void kill_selected();
    void layout(HWND hwnd);
    void open_file_location();
    bool pick_files(std::vector<std::wstring>& chosen);
    bool pick_folder(std::wstring& chosen);
    void resize_columns(int list_width);
    void restart_elevated();
    const ProcessResult* result_at(int row);
    void run_delete(bool already_released_handles);
    std::vector<int> selected_rows();
    bool send_paths_to_instance(HWND target, const std::vector<std::wstring>& paths);
    void set_busy(bool busy);
    void set_status(const std::wstring& text);
    void show_about();
    void show_list_menu();
    void start_search();
    void toggle_context_menu();
    void update_button_states();
    void update_menu_state();
    void update_path_label();
    UINT window_dpi(HWND hwnd);
    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
}
