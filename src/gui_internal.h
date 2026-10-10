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
    constexpr int IDC_OPEN_LOCATION = 1006;
    constexpr int IDC_SUBLINE = 1007;
    constexpr int IDC_STATUS = 1008;
    constexpr int IDC_FORCE_UNLOCK = 1009;
    constexpr int IDC_DELETE_FILES = 1010;
    constexpr int IDC_HEADER_ICON = 1012;
    constexpr int IDC_HEADLINE = 1013;
    constexpr int IDC_EMPTY_HINT = 1014;
    constexpr int IDM_TOGGLE_CONTEXT_MENU = 2001;
    constexpr int IDM_INSTALL_SENDTO = 2002;
    constexpr int IDM_OPEN_SENDTO = 2003;
    constexpr int IDM_REFRESH = 2004;
    constexpr int IDM_ELEVATE = 2005;
    constexpr int IDM_ABOUT = 2006;
    constexpr int IDM_HOMEPAGE = 2007;
    constexpr int IDM_EXIT = 2008;
    constexpr int IDM_ADD_FILES = 2009;
    constexpr int IDM_APPEND_FILES = 2012;
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
        const wchar_t* tip;    // 悬停提示：三个"危险动作"的区别只靠文案是分不清的
        bool starts_group;     // true = 与上一组之间留出分组间距（查看类动作另起一组）
    };
    inline constexpr ButtonSpec kButtonSpec[] = {
        // 左边一组 = 对结果的动作（都有确认框）；右边一组 = 查看类动作
        { IDC_KILL_SELECTED, L"结束进程", 100, L"结束选中的进程（未保存的数据会丢失）", false },
        { IDC_KILL_ALL, L"全部结束", 100, L"结束列表里的全部占用进程（不只是选中的）", false },
        { IDC_FORCE_UNLOCK, L"强制解锁", 100, L"只关闭它们的文件句柄、不结束进程；持有句柄的程序可能出现异常", false },
        { IDC_DELETE_FILES, L"删除检查的文件…", 170, L"删除正在检查的这些文件（进回收站；被占用时会单独问你一次）", false },
        { IDC_REFRESH, L"重新检查", 90, L"重新扫描（F5）", true },
        { IDC_OPEN_LOCATION, L"打开所在位置", 140, L"在资源管理器中定位选中的文件（双击列表也可以）", true },
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
    void update_header();
    struct SearchPayload
    {
        LocksmithResult locks;
        std::wstring error;
        std::wstring app_note;   // 快捷方式展开的说明
        unsigned generation = 0; // 发起这次扫描时的代号
    };
    constexpr UINT kDpiChangedMessage = 0x02E0;
    // replace=true（拖放/打开方式的默认语义）：这是新的一次检查，先清空上一次的路径与结果
    void add_paths(const std::vector<std::wstring>& new_paths, bool replace);
    void clear_current_check();

    // 界面状态（原来散在匿名命名空间里的全局变量）
    struct UiState
    {
        HINSTANCE instance = nullptr;
        HWND window = nullptr;
        HWND list = nullptr;
        HWND status = nullptr;
        HWND subline = nullptr;
        HWND header_icon = nullptr;
        HWND headline = nullptr;
        HWND empty_hint = nullptr;
        HWND tooltip = nullptr;
        HWND buttons [BUTTON_COUNT] = {};
        HFONT font = nullptr;
        HFONT headline_font = nullptr;
        HICON header_icon_handle = nullptr;
        HMENU main_menu = nullptr;
        HMENU tools_menu = nullptr;
        UINT dpi = 96;
        HIMAGELIST icons = nullptr;
        std::map<std::wstring, int> icon_cache;
        ui_state::WindowState saved_state;
        bool columns_restored = false;
        DWORD busy_started = 0;
        bool searching = false;
        bool search_pending = false;
        // 每次重新检查自增：旧扫描的结果回来时用它判断是否已经过期（用户又拖了新文件）
        unsigned search_generation = 0;
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
    void add_files_via_dialog(bool replace);
    void add_folder_via_dialog(bool replace);
    void add_paths(const std::vector<std::wstring>& new_paths);
    void add_sendto_menu();
    void apply_ui_font();
    // 按窗口 DPI 缩放布局常量（字体早就按 DPI 建了，布局此前是固定像素）
    int scale(int value);
    // 按钮行需要的总宽度（含边距与分组间距），布局与最小窗口尺寸共用同一份算法
    int buttons_row_width();
    // 按控件 ID 找按钮：避免"数组下标 = 语义"的隐式耦合（改一次按钮表就可能错位）
    HWND button_by_id(int id);
    void init_list_extras();
    void init_tooltips(HWND parent);
    // 列表为空时的居中提示（贴在列表区域里，拖放同样能接）
    void update_empty_hint();
    void build_menu(HWND hwnd);
    // 控件 ID 通过 CreateWindowExW 的 HMENU 参数传递；经 INT_PTR 中转以满足 64 位 MSVC（C4312）
    inline HMENU control_id(int id)
    {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }
    void copy_selected(bool exe_path, bool pid_only, bool whole_row);
    void copy_to_clipboard(const std::wstring& text);
    HFONT create_ui_font(UINT dpi, LONG weight = FW_NORMAL);
    void delete_checked_paths();
    std::wstring describe_paths_for_dialog();
    LRESULT CALLBACK drop_subclass_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param, UINT_PTR subclass_id, DWORD_PTR ref_data);
    void allow_unelevated_drop(HWND wnd);
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
    void update_header();
    UINT window_dpi(HWND hwnd);
    LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param);
}
