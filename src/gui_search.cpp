#include "gui_internal.h"

namespace gui::detail
{
    void set_busy(bool busy)
    {
        ui().searching = busy;
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
        for (const auto& handle : ui().handles)
        {
            ++handle_counts[handle.pid];
        }

        SendMessageW(ui().list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(ui().list);

        int index = 0;
        for (const auto& result : ui().results)
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
            int row = ListView_InsertItem(ui().list, &item);
            if (row < 0)
            {
                break;
            }

            ListView_SetItemText(ui().list, row, COL_PID, pid_text.data());
            ListView_SetItemText(ui().list, row, COL_USER, const_cast<wchar_t*>(result.user.c_str()));
            ListView_SetItemText(ui().list, row, COL_KIND, const_cast<wchar_t*>(kind_text.c_str()));
            ListView_SetItemText(ui().list, row, COL_FILES, const_cast<wchar_t*>(files_text.c_str()));
            ++index;
        }

        SendMessageW(ui().list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(ui().list, nullptr, TRUE);
        update_button_states();
    }

    void start_search()
    {
        if (ui().searching)
        {
            // 正在查：排队，等这次结果回来立刻带着新路径再查一遍
            ui().search_pending = true;
            return;
        }
        if (ui().paths.empty())
        {
            set_status(L"请先添加要检查的文件。");
            return;
        }

        ui().searching = true;
        set_busy(true);
        ui().busy_started = GetTickCount();
        SetTimer(ui().window, BUSY_TIMER_ID, 500, nullptr);
        ui().worker_running.store(true);

        const unsigned generation = ++ui().search_generation;

        auto paths = ui().paths;
        auto app_targets = ui().app_targets;
        std::thread([paths, app_targets, generation] {
            auto payload = new SearchPayload();
            payload->generation = generation;
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

            HWND target = ui().post_target.load();
            const bool posted = target != nullptr && PostMessageW(target, WM_FL_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(payload)) != 0;
            if (!posted)
            {
                delete payload;
            }
            ui().worker_running.store(false);
        }).detach();
    }

    void add_paths(const std::vector<std::wstring>& new_paths, bool replace)
    {
        // 有任何新输入就把上一轮的代号作废：正在跑的那次扫描结果回来时会被丢掉
        ++ui().search_generation;

        if (replace)
        {
            // 只是想看新的东西：先把上一次的记录清掉，不要累加
            clear_current_check();
        }

        for (const auto& path : new_paths)
        {
            if (std::find(ui().paths.begin(), ui().paths.end(), path) != ui().paths.end())
            {
                continue;
            }

            ui().paths.push_back(path);

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

        update_path_label();
        start_search();
    }

    // 清空上一次的检查（路径、结果、句柄、列表）
    void clear_current_check()
    {
        ui().paths.clear();
        ui().app_targets.clear();
        ui().results.clear();
        ui().handles.clear();
        ui().handles_query_ok = true;
        ui().search_pending = false;   // 排队中的旧扫描没意义了
        if (ui().list != nullptr)
        {
            ListView_DeleteAllItems(ui().list);
        }
        update_button_states();
    }

    void add_app_target(const std::wstring& target)
    {
        if (!target.empty() && std::find(ui().app_targets.begin(), ui().app_targets.end(), target) == ui().app_targets.end())
        {
            ui().app_targets.push_back(target);
        }
    }

    const ProcessResult* result_at(int row)
    {
        if (row < 0 || row >= static_cast<int>(ui().results.size()))
        {
            return nullptr;
        }
        return &ui().results[row];
    }

    std::vector<int> selected_rows()
    {
        std::vector<int> rows;
        int row = -1;
        while ((row = ListView_GetNextItem(ui().list, row, LVNI_SELECTED)) != -1)
        {
            rows.push_back(row);
        }
        return rows;
    }
}
