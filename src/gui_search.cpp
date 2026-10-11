#include "gui_internal.h"

namespace gui::detail
{
    namespace
    {
        void resolve_app_target_for_path(const std::wstring& path);

        bool append_path_to_model(const std::wstring& path)
        {
            if (path.empty() || std::find(ui().paths.begin(), ui().paths.end(), path) != ui().paths.end())
            {
                return false;
            }

            ui().paths.push_back(path);
            resolve_app_target_for_path(path);
            return true;
        }

        void invalidate_result_model()
        {
            ui().results.clear();
            ui().handles.clear();
            ui().handles_query_ok = false;
            ui().processes_query_ok = false;
            ui().result_valid = false;
            if (ui().list != nullptr)
            {
                ListView_DeleteAllItems(ui().list);
            }
        }

        void resolve_app_target_for_path(const std::wstring& path)
        {
            // 快捷方式：解析出目标程序，按"这个程序的所有进程"处理。
            if (shell_integration::is_shortcut(path))
            {
                std::wstring target;
                std::wstring arguments;
                if (shell_integration::resolve_shortcut(path, target, arguments))
                {
                    add_app_target(target);
                }
                else if (ui().status != nullptr)
                {
                    set_status(L"解析快捷方式失败，按普通文件处理: " + path);
                }
            }
            else if (shell_integration::is_executable(path))
            {
                add_app_target(path);
            }
        }
    }

    void set_busy(bool busy)
    {
        ui().searching = busy;
        if (busy)
        {
            set_status(L"正在扫描系统句柄…");
        }
        update_button_states();
        update_menu_state();
    }

    bool is_search_busy()
    {
        return ui().searching || ui().worker_running.load();
    }

    bool ensure_result_action(const wchar_t* action)
    {
        if (is_search_busy())
        {
            set_status(std::wstring(action) + L"：请等待当前扫描完成。");
            return false;
        }
        if (!ui().result_valid || !ui().handles_query_ok || !ui().processes_query_ok)
        {
            set_status(std::wstring(action) + L"：当前结果不可用，请重新检查。");
            return false;
        }
        return true;
    }

    bool ensure_path_action(const wchar_t* action)
    {
        if (is_search_busy())
        {
            set_status(std::wstring(action) + L"：请等待当前扫描完成。");
            return false;
        }
        if (ui().paths.empty())
        {
            set_status(std::wstring(action) + L"：没有要处理的路径。");
            return false;
        }
        return true;
    }

    void fill_list()
    {        // 每个进程占了多少个句柄：0 个说明只是模块映射（文件被当成 DLL/EXE 加载）
        std::map<DWORD, int> handle_counts;
        for (const auto& handle : ui().handles)
        {
            ++handle_counts[handle.pid];
        }

        if (ui().list == nullptr)
        {
            return;
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
        if (ui().closing)
        {
            return;
        }

        // 只允许一个任务处于"等待主线程消费完成消息"的状态。新的输入先排队，
        // 这样旧任务不会和新任务同时运行，也不会用旧完成消息清掉新任务的 busy 状态。
        if (ui().worker_running.load())
        {
            ui().search_pending = true;
            return;
        }
        if (ui().paths.empty())
        {
            set_status(L"请先添加要检查的文件。");
            return;
        }

        ui().search_pending = false;
        invalidate_result_model();
        ui().searching = true;
        set_busy(true);
        InvalidateRect(ui().list, nullptr, TRUE);   // 空态提示画在列表 WM_PAINT 里，扫描开始时刷掉
        ui().busy_started = GetTickCount();
        SetTimer(ui().window, BUSY_TIMER_ID, 500, nullptr);

        const uint64_t input_generation = ui().input_generation;
        const uint64_t job_id = ++ui().next_job_id;
        ui().active_job_id = job_id;
        ui().worker_running.store(true);

        auto paths = ui().paths;
        auto app_targets = ui().app_targets;
        try
        {
            ui().worker = std::thread([paths, app_targets, input_generation, job_id] {
                auto payload = new SearchPayload();
                payload->job_id = job_id;
                payload->input_generation = input_generation;
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

                // worker_running 要等主线程消费完成消息后才复位，避免新的 worker 与旧 payload 同时在途。
                HWND target = ui().post_target.load();
                if (target != nullptr && !ui().closing &&
                    PostMessageW(target, WM_FL_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(payload)) != 0)
                {
                    return;
                }

                if (target != nullptr && !ui().closing)
                {
                    // 极少数情况下消息队列投递失败：交给 busy timer 兜底消费，避免界面
                    // 永远停在"正在扫描"。关闭窗口时则不再把结果交回 UI。
                    SearchPayload* expected = nullptr;
                    if (!ui().completion_fallback.compare_exchange_strong(expected, payload))
                    {
                        delete payload;
                    }
                    return;
                }

                delete payload;
                ui().worker_running.store(false);
            });
        }
        catch (...)
        {
            ui().worker_running.store(false);
            KillTimer(ui().window, BUSY_TIMER_ID);
            ui().searching = false;
            set_status(L"无法启动后台扫描线程。");
            update_button_states();
            update_menu_state();
        }
    }

    void add_paths(const std::vector<std::wstring>& new_paths, bool replace)
    {
        if (new_paths.empty())
        {
            return;
        }

        if (replace)
        {
            ++ui().input_generation;
            // 只是想看新的东西：先把上一次的记录清掉，不要累加
            clear_current_check();
        }

        bool changed = false;
        for (const auto& path : new_paths)
        {
            changed = append_path_to_model(path) || changed;
        }

        if (!replace && changed)
        {
            ++ui().input_generation;
        }

        if (!changed)
        {
            update_header();
            return;
        }

        update_header();
        start_search();
    }

    void initialize_paths(const std::vector<std::wstring>& paths)
    {
        ui().paths.clear();
        ui().app_targets.clear();
        ++ui().input_generation;
        for (const auto& path : paths)
        {
            append_path_to_model(path);
        }
    }

    void rebuild_app_targets()
    {
        ui().app_targets.clear();
        for (const auto& path : ui().paths)
        {
            resolve_app_target_for_path(path);
        }
    }

    // 清空上一次的检查（路径、结果、句柄、列表）
    void clear_current_check()
    {
        ui().paths.clear();
        ui().app_targets.clear();
        ui().search_pending = false;   // 排队中的旧扫描没意义了
        invalidate_result_model();
        update_button_states();
        update_menu_state();
        if (ui().list != nullptr)
        {
            InvalidateRect(ui().list, nullptr, TRUE);
        }
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
        if (ui().list == nullptr)
        {
            return rows;
        }

        int row = -1;
        while ((row = ListView_GetNextItem(ui().list, row, LVNI_SELECTED)) != -1)
        {
            rows.push_back(row);
        }
        return rows;
    }
}
