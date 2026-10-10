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

    void update_header()
    {
        std::wstring headline;
        std::wstring subline;

        if (ui().paths.empty())
        {
            headline = L"拖入文件或文件夹开始检查";
            subline = L"支持 .lnk / .exe（展开整棵进程树）；也可以按 Ctrl+O 选择文件";
        }
        else if (ui().paths.size() == 1)
        {
            headline = L"正在检查 1 个文件";
            subline = ui().paths[0];
        }
        else
        {
            headline = L"正在检查 " + std::to_wstring(ui().paths.size()) + L" 个路径";
            subline = join(ui().paths, L"    ");
        }

        if (app::is_process_elevated())
        {
            subline += L"　（管理员模式：若拖放无反应，请用「文件 → 检查文件…」）";
        }

        SetWindowTextW(ui().headline, headline.c_str());
        SetWindowTextW(ui().subline, subline.c_str());

        // 标题：一个路径时直接显示文件名（比"N 个路径"有信息量），空的时候保留标语
        std::wstring title = L"FileLocksmith";
        if (ui().paths.empty())
        {
            title += L" — 什么正在占用这个文件？";
        }
        else if (ui().paths.size() == 1)
        {
            const auto& path = ui().paths[0];
            const auto pos = path.find_last_of(L"\\/");
            title += L" — " + (pos == std::wstring::npos ? path : path.substr(pos + 1));
        }
        else
        {
            title += L" — " + std::to_wstring(ui().paths.size()) + L" 个路径";
        }
        if (app::is_process_elevated())
        {
            title += L"（管理员）";
        }
        if (ui().window != nullptr)
        {
            SetWindowTextW(ui().window, title.c_str());
        }
    }

    void update_empty_hint()
    {
        if (ui().empty_hint == nullptr)
        {
            return;
        }
        // 只在"没有结果、也没在扫描"时显示（列表为空时它就是视觉重心）
        const bool show = ui().results.empty() && !ui().searching;
        if (show)
        {
            SetWindowTextW(ui().empty_hint,
                           ui().paths.empty()
                               ? L"把文件或文件夹拖到这里\n或按 Ctrl+O 选择文件"
                               : L"没有进程占用这些文件\n（列表里只列出占用者）");
        }
        ShowWindow(ui().empty_hint, show ? SW_SHOW : SW_HIDE);
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

        const int name_width = scale(190);
        const int pid_width = scale(80);
        const int user_width = scale(140);
        const int kind_width = scale(90);
        int files_width = list_width - name_width - pid_width - user_width - kind_width - scale(30);
        if (files_width < scale(120))
        {
            files_width = scale(120);
        }
        ListView_SetColumnWidth(ui().list, COL_PROCESS, name_width);
        ListView_SetColumnWidth(ui().list, COL_PID, pid_width);
        ListView_SetColumnWidth(ui().list, COL_USER, user_width);
        ListView_SetColumnWidth(ui().list, COL_KIND, kind_width);
        ListView_SetColumnWidth(ui().list, COL_FILES, files_width);
    }

    int scale(int value)
    {
        const int dpi = ui().dpi != 0 ? static_cast<int>(ui().dpi) : 96;
        return MulDiv(value, dpi, 96);
    }

    int pixel_width_of_text(const wchar_t* text)
    {
        HDC dc = GetDC(ui().window);
        if (dc == nullptr)
        {
            return 0;
        }
        HGDIOBJ previous = SelectObject(dc, ui().font != nullptr ? ui().font : GetStockObject(DEFAULT_GUI_FONT));
        SIZE size{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        SelectObject(dc, previous);
        ReleaseDC(ui().window, dc);
        return size.cx;
    }

    void compute_button_widths()
    {
        // 宽度按文字实测（不再写死 100/170/90 那种）—— 一行里宽窄不一，看起来就是"没对齐"。
        // 字体或 DPI 变了要重算（WM_CREATE / WM_DPICHANGED 里调用）。
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            const int width = pixel_width_of_text(kButtonSpec[i].text) + scale(14) * 2;
            const int minimum = scale(76);
            ui().button_widths[i] = width < minimum ? minimum : width;
        }
    }

    int buttons_row_width()
    {
        // 窄到极限时的最小宽度：所有按钮 + 最小间距 + 左右边距
        int total = 0;
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            total += ui().button_widths[i];
        }
        return scale(12) * 2 + total + scale(8) * (BUTTON_COUNT - 1);
    }

    HWND button_by_id(int id)
    {
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            if (kButtonSpec[i].id == id)
            {
                return ui().buttons[i];
            }
        }
        return nullptr;
    }

    void layout(HWND hwnd)
    {
        RECT client{};
        GetClientRect(hwnd, &client);

        const int margin = scale(12);
        const int gap = scale(8);
        const int icon_size = scale(32);
        const int headline_height = scale(22);
        const int subline_height = scale(20);
        const int header_height = scale(56);
        const int button_height = scale(32);
        const int status_height = scale(22);

        // 头部：图标 + 半粗标题 + 灰色副标题（三行信息，取代原来一行 12px 小字）
        MoveWindow(ui().header_icon, margin, margin + scale(6), icon_size, icon_size, TRUE);
        const int text_x = margin + icon_size + scale(12);
        const int text_width = client.right - text_x - margin;
        MoveWindow(ui().headline, text_x, margin + scale(3), text_width, headline_height, TRUE);
        MoveWindow(ui().subline, text_x, margin + scale(3) + headline_height, text_width, subline_height, TRUE);

        int y = margin + header_height + gap;
        int remaining = client.bottom - y - button_height - status_height - margin - gap - scale(4);
        if (remaining < scale(60))
        {
            remaining = scale(60);
        }
        MoveWindow(ui().list, margin, y, client.right - 2 * margin, remaining, TRUE);

        // 空态提示：居中放在列表区域里（列表为空时它就是视觉重心）
        if (ui().empty_hint != nullptr)
        {
            const int hint_width = scale(340);
            const int hint_height = scale(56);
            MoveWindow(ui().empty_hint,
                       margin + (client.right - 2 * margin - hint_width) / 2,
                       y + (remaining - hint_height) / 2,
                       hint_width, hint_height, TRUE);
        }
        y += remaining + gap + scale(4);

        // 按钮全行等距：间隔一致，窗口富余时整行居中（不硬撑、也不挤在左边）
        int row_total = 0;
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            row_total += ui().button_widths[i];
        }
        const int available = client.right - margin * 2;
        int slot_gap = scale(8);
        if (BUTTON_COUNT > 1)
        {
            slot_gap = (available - row_total) / (BUTTON_COUNT - 1);
            if (slot_gap < scale(8))
            {
                slot_gap = scale(8);
            }
            if (slot_gap > scale(20))
            {
                slot_gap = scale(20);
            }
        }
        int x = margin + (available - (row_total + slot_gap * (BUTTON_COUNT - 1))) / 2;
        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            MoveWindow(ui().buttons[i], x, y, ui().button_widths[i], button_height, TRUE);
            x += ui().button_widths[i] + slot_gap;
        }

        MoveWindow(ui().status, margin, y + button_height + scale(4), client.right - 2 * margin, status_height, TRUE);

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
        else if (message == WM_MOUSEMOVE && (GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_OWNERDRAW) != 0)
        {
            // 自绘按钮的悬停反馈（原生按钮自带，owner-draw 之后要自己跟踪）
            if (ui().hot_button != hwnd)
            {
                ui().hot_button = hwnd;
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
        }
        else if (message == WM_MOUSELEAVE)
        {
            if (ui().hot_button == hwnd)
            {
                ui().hot_button = nullptr;
                InvalidateRect(hwnd, nullptr, TRUE);
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

    HFONT create_ui_font(UINT dpi, LONG weight)
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
        if (!ok)
        {
            return nullptr;
        }
        metrics.lfMessageFont.lfWeight = weight;   // 标题行走半粗体（FW_SEMIBOLD）
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }

    void apply_ui_font()
    {
        if (ui().font == nullptr)
        {
            return;
        }
        SendMessageW(ui().headline, WM_SETFONT,
                     reinterpret_cast<WPARAM>(ui().headline_font != nullptr ? ui().headline_font : ui().font), TRUE);
        for (HWND control : { ui().subline, ui().list, ui().status, ui().empty_hint })
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui().font), TRUE);
        }
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

    namespace
    {
        // ---- 圆角按钮：GDI+ 抗锯齿 ------------------------------------------------
        // gdiplus.dll 系统自带；这里动态加载（不新增链接依赖），拿不到就退回 GDI 的
        // RoundRect —— 边缘没有抗锯齿，但按钮一定画得出来。
        using ArgsColor = DWORD;
        constexpr INT kSmoothingAntiAlias = 4;
        constexpr INT kFillModeWinding = 1;
        constexpr INT kUnitPixel = 2;

        struct GdiPlusApi
        {
            HMODULE module = nullptr;
            INT(WINAPI* startup)(ULONG_PTR*, const void*, void**) = nullptr;
            void(WINAPI* shutdown)(ULONG_PTR) = nullptr;
            INT(WINAPI* create_graphics)(HDC, void**) = nullptr;
            INT(WINAPI* delete_graphics)(void*) = nullptr;
            INT(WINAPI* set_smoothing)(void*, INT) = nullptr;
            INT(WINAPI* create_path)(INT, void**) = nullptr;
            INT(WINAPI* add_arc)(void*, float, float, float, float, float, float) = nullptr;
            INT(WINAPI* close_figure)(void*) = nullptr;
            INT(WINAPI* create_fill)(ArgsColor, void**) = nullptr;
            INT(WINAPI* fill_path)(void*, void*, void*) = nullptr;
            INT(WINAPI* create_pen)(ArgsColor, float, INT, void**) = nullptr;
            INT(WINAPI* draw_path)(void*, void*, void*) = nullptr;
            INT(WINAPI* delete_brush)(void*) = nullptr;
            INT(WINAPI* delete_pen)(void*) = nullptr;
            INT(WINAPI* delete_path)(void*) = nullptr;

            template <typename Fn>
            bool bind(Fn& target, const char* name)
            {
                target = load_proc<Fn>(module, name);
                return target != nullptr;
            }
        };

        GdiPlusApi g_gdiplus;
        bool g_gdiplus_ready = false;
        bool g_gdiplus_probed = false;
        ULONG_PTR g_gdiplus_token = 0;

        ArgsColor gdiplus_color(COLORREF color)
        {
            return 0xFF000000u | (static_cast<ArgsColor>(GetRValue(color)) << 16) |
                   (static_cast<ArgsColor>(GetGValue(color)) << 8) | static_cast<ArgsColor>(GetBValue(color));
        }

        void fill_rounded_rect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border, bool antialias)
        {
            const float left = static_cast<float>(rect.left) + 0.5f;
            const float top = static_cast<float>(rect.top) + 0.5f;
            const float width = static_cast<float>(rect.right - rect.left) - 1.0f;
            const float height = static_cast<float>(rect.bottom - rect.top) - 1.0f;
            const float diameter = static_cast<float>(radius) * 2.0f;

            if (antialias && g_gdiplus_ready)
            {
                void* graphics = nullptr;
                void* path = nullptr;
                if (g_gdiplus.create_graphics(dc, &graphics) == 0 && graphics != nullptr &&
                    g_gdiplus.create_path(kFillModeWinding, &path) == 0 && path != nullptr)
                {
                    g_gdiplus.add_arc(path, left, top, diameter, diameter, 180.0f, 90.0f);
                    g_gdiplus.add_arc(path, left + width - diameter, top, diameter, diameter, 270.0f, 90.0f);
                    g_gdiplus.add_arc(path, left + width - diameter, top + height - diameter, diameter, diameter, 0.0f, 90.0f);
                    g_gdiplus.add_arc(path, left, top + height - diameter, diameter, diameter, 90.0f, 90.0f);
                    g_gdiplus.close_figure(path);
                    g_gdiplus.set_smoothing(graphics, kSmoothingAntiAlias);

                    void* brush = nullptr;
                    if (g_gdiplus.create_fill(gdiplus_color(fill), &brush) == 0 && brush != nullptr)
                    {
                        g_gdiplus.fill_path(graphics, path, brush);
                        g_gdiplus.delete_brush(brush);
                    }
                    void* pen = nullptr;
                    if (g_gdiplus.create_pen(gdiplus_color(border), 1.0f, kUnitPixel, &pen) == 0 && pen != nullptr)
                    {
                        g_gdiplus.draw_path(graphics, path, pen);
                        g_gdiplus.delete_pen(pen);
                    }
                    g_gdiplus.delete_path(path);
                }
                if (graphics != nullptr)
                {
                    g_gdiplus.delete_graphics(graphics);
                }
                return;
            }

            HBRUSH brush = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, 1, border);
            HGDIOBJ previous_brush = SelectObject(dc, brush);
            HGDIOBJ previous_pen = SelectObject(dc, pen);
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
            SelectObject(dc, previous_pen);
            SelectObject(dc, previous_brush);
            DeleteObject(pen);
            DeleteObject(brush);
        }
    }

    bool init_button_rendering()
    {
        if (!g_gdiplus_probed)
        {
            g_gdiplus_probed = true;
            g_gdiplus.module = LoadLibraryW(L"gdiplus.dll");
            if (g_gdiplus.module != nullptr)
            {
                bool ok = true;
                ok = g_gdiplus.bind(g_gdiplus.startup, "GdiplusStartup") && ok;
                ok = g_gdiplus.bind(g_gdiplus.shutdown, "GdiplusShutdown") && ok;
                ok = g_gdiplus.bind(g_gdiplus.create_graphics, "GdipCreateFromHDC") && ok;
                ok = g_gdiplus.bind(g_gdiplus.delete_graphics, "GdipDeleteGraphics") && ok;
                ok = g_gdiplus.bind(g_gdiplus.set_smoothing, "GdipSetSmoothingMode") && ok;
                ok = g_gdiplus.bind(g_gdiplus.create_path, "GdipCreatePath") && ok;
                ok = g_gdiplus.bind(g_gdiplus.add_arc, "GdipAddPathArc") && ok;
                ok = g_gdiplus.bind(g_gdiplus.close_figure, "GdipClosePathFigure") && ok;
                ok = g_gdiplus.bind(g_gdiplus.create_fill, "GdipCreateSolidFill") && ok;
                ok = g_gdiplus.bind(g_gdiplus.fill_path, "GdipFillPath") && ok;
                ok = g_gdiplus.bind(g_gdiplus.create_pen, "GdipCreatePen1") && ok;
                ok = g_gdiplus.bind(g_gdiplus.draw_path, "GdipDrawPath") && ok;
                ok = g_gdiplus.bind(g_gdiplus.delete_brush, "GdipDeleteBrush") && ok;
                ok = g_gdiplus.bind(g_gdiplus.delete_pen, "GdipDeletePen") && ok;
                ok = g_gdiplus.bind(g_gdiplus.delete_path, "GdipDeletePath") && ok;
                if (ok)
                {
                    struct StartupInput
                    {
                        UINT32 version;
                        void* callback;
                        BOOL suppress_background;
                        BOOL suppress_codecs;
                    } input{ 1, nullptr, FALSE, FALSE };
                    g_gdiplus_ready = g_gdiplus.startup(&g_gdiplus_token, &input, nullptr) == 0;
                }
            }
        }
        return g_gdiplus_ready;
    }

    void shutdown_button_rendering()
    {
        if (g_gdiplus_ready && g_gdiplus.shutdown != nullptr)
        {
            g_gdiplus.shutdown(g_gdiplus_token);
            g_gdiplus_ready = false;
        }
    }

    void draw_button(const DRAWITEMSTRUCT& item)
    {
        HDC dc = item.hDC;
        RECT rect = item.rcItem;
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool hot = !disabled && ui().hot_button == item.hwndItem;

        // 按钮画的是圆角：四角必须先露出窗口底色
        FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));

        COLORREF fill = RGB(253, 253, 253);
        COLORREF border = RGB(214, 214, 214);
        if (disabled)
        {
            fill = RGB(250, 250, 250);
            border = RGB(232, 232, 232);
        }
        else if (pressed)
        {
            fill = RGB(233, 233, 233);
            border = RGB(188, 188, 188);
        }
        else if (hot)
        {
            fill = RGB(245, 245, 245);
            border = RGB(188, 188, 188);
        }

        fill_rounded_rect(dc, rect, scale(6), fill, border, /* antialias */ true);

        wchar_t text[128]{};
        const int length = GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
        RECT text_rect = rect;
        if (pressed)
        {
            OffsetRect(&text_rect, 0, scale(1));   // 按下时文字下沉 1px
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_WINDOWTEXT));
        HGDIOBJ previous = SelectObject(dc, ui().font != nullptr ? ui().font : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, text, length, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);

        if (!disabled && (item.itemState & ODS_FOCUS) != 0)
        {
            RECT focus = rect;
            InflateRect(&focus, -scale(4), -scale(4));
            DrawFocusRect(dc, &focus);
        }
    }

    void init_list_extras()
    {
        // ListView 换成资源管理器风格（表头/选中高亮更现代）。uxtheme 是老系统就有的
        // 系统 DLL，缺了也只是保持默认外观，不影响功能。
        using SetWindowTheme_t = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        static auto set_window_theme = load_proc<SetWindowTheme_t>(GetModuleHandleW(L"uxtheme.dll"), "SetWindowTheme");
        if (set_window_theme != nullptr && ui().list != nullptr)
        {
            set_window_theme(ui().list, L"Explorer", nullptr);
        }
    }

    void init_tooltips(HWND parent)
    {
        // "结束进程 / 强制解锁 / 删除" 三个动作的区别光看按钮文字分不出来，必须有悬停说明
        ui().tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                       WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                       CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                       parent, nullptr, ui().instance, nullptr);
        if (ui().tooltip == nullptr)
        {
            return;
        }

        for (int i = 0; i < BUTTON_COUNT; ++i)
        {
            TOOLINFOW info{};
            info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = parent;
            info.uId = reinterpret_cast<UINT_PTR>(ui().buttons[i]);
            info.lpszText = const_cast<wchar_t*>(kButtonSpec[i].tip);
            SendMessageW(ui().tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }

    void update_button_states()
    {
        const bool busy = ui().searching;
        const bool has_selection = !selected_rows().empty();

        EnableWindow(button_by_id(IDC_KILL_SELECTED), !busy && has_selection);
        EnableWindow(button_by_id(IDC_KILL_ALL), !busy && !ui().results.empty());
        // 只有真的存在可关闭的文件句柄才让点"强制解锁"（纯模块映射点了什么也做不了）
        EnableWindow(button_by_id(IDC_FORCE_UNLOCK), !busy && !ui().handles.empty());
        EnableWindow(button_by_id(IDC_DELETE_FILES), !busy && !ui().paths.empty());
        EnableWindow(button_by_id(IDC_REFRESH), !busy);
        EnableWindow(button_by_id(IDC_OPEN_LOCATION), !busy && has_selection);
    }

    void update_menu_state()
    {
        if (ui().tools_menu == nullptr)
        {
            return;
        }
        // 勾选状态每次弹出菜单时重新读注册表（可能在外部被改动）。
        // 这里用的是 build_menu 存下来的子菜单句柄，不依赖"第几个子菜单"这种下标约定。
        CheckMenuItem(ui().tools_menu, IDM_TOGGLE_CONTEXT_MENU,
                      MF_BYCOMMAND | (shell_integration::is_context_menu_installed() ? MF_CHECKED : MF_UNCHECKED));
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

        // 文件：只放"检查哪些文件"和"对当前检查做什么"
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FILES, L"检查文件…(&F)\tCtrl+O");
        AppendMenuW(file_menu, MF_STRING, IDM_ADD_FOLDER, L"检查文件夹…(&D)");
        AppendMenuW(file_menu, MF_STRING, IDM_APPEND_FILES, L"追加文件到当前检查…(&A)\tCtrl+Shift+O");
        AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file_menu, MF_STRING, IDM_DELETE_FILES, L"删除检查的文件…(&E)");
        AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"退出(&X)");

        // 工具：重新检查 / 提权 / 资源管理器集成（"安装类"操作原来在「文件」菜单里，反直觉）
        AppendMenuW(tools_menu, MF_STRING, IDM_REFRESH, L"重新检查(&R)\tF5");
        AppendMenuW(tools_menu, MF_STRING, IDM_ELEVATE, L"以管理员身份重启(&A)");
        AppendMenuW(tools_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tools_menu, MF_STRING, IDM_TOGGLE_CONTEXT_MENU, L"资源管理器右键菜单(&C)");
        AppendMenuW(tools_menu, MF_STRING, IDM_INSTALL_SENDTO, L"添加到「发送到」菜单(&S)");
        AppendMenuW(tools_menu, MF_STRING, IDM_OPEN_SENDTO, L"打开「发送到」文件夹(&O)");

        AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"关于 FileLocksmith(&A)");
        AppendMenuW(help_menu, MF_STRING, IDM_HOMEPAGE, L"项目主页 (GitHub)(&H)");

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"文件(&F)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_menu), L"工具(&T)");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"帮助(&H)");

        SetMenu(hwnd, menu);
        ui().main_menu = menu;
        ui().tools_menu = tools_menu;
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
                         L"Windows 11 要进“显示更多选项”才看得到；想出现在一级菜单请用「工具 → 添加到「发送到」菜单」。");
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
