#include "gui_internal.h"

namespace gui::detail
{
    void kill_selected()
    {
        if (!ensure_result_action(L"结束进程"))
        {
            return;
        }

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
        if (MessageBoxW(ui().window, question.c_str(), L"结束进程", MB_YESNO | MB_ICONWARNING) != IDYES)
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
            set_status(L"有 " + std::to_wstring(failed) + L" 个进程无法结束（权限不足），可在「工具」菜单里选「以管理员身份重启」。");
        }
        start_search();
    }

    void kill_all()
    {
        if (!ensure_result_action(L"结束全部进程"))
        {
            return;
        }

        if (ui().results.empty())
        {
            return;
        }

        std::wstring question = L"确定要结束全部 " + std::to_wstring(ui().results.size()) + L" 个进程吗？\n\n"
                                L"进程里的未保存数据会丢失。";
        if (MessageBoxW(ui().window, question.c_str(), L"结束全部进程", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        int failed = 0;
        for (const auto& process : ui().results)
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
            set_status(L"有 " + std::to_wstring(failed) + L" 个进程无法结束（权限不足），可在「工具」菜单里选「以管理员身份重启」。");
        }
        start_search();
    }

    void force_unlock()
    {
        if (!ensure_result_action(L"强制解锁"))
        {
            return;
        }

        // 选中了进程就只关这些进程的句柄，没选才动全部（多选之后必须区分，否则会误伤）
        std::vector<LockedHandle> targets;
        const auto rows = selected_rows();
        if (rows.empty())
        {
            targets = ui().handles;
        }
        else
        {
            std::set<DWORD> selected_pids;
            for (int row : rows)
            {
                if (const auto* process = result_at(row))
                {
                    selected_pids.insert(process->pid);
                }
            }
            for (const auto& handle : ui().handles)
            {
                if (selected_pids.count(handle.pid) != 0)
                {
                    targets.push_back(handle);
                }
            }
        }

        if (targets.empty())
        {
            set_status(ui().results.empty() ? L"没有可解锁的占用。"
                                            : (rows.empty() ? L"只找到模块映射（文件被当成 DLL/EXE 加载进去了），这种情况没法强制解除，只能结束进程。"
                                                            : L"选中的进程只持有模块映射（文件被当成 DLL/EXE 加载），没有可关闭的句柄 —— 只能结束进程。"));
            return;
        }

        const std::wstring scope = rows.empty()
                                       ? L"全部进程"
                                       : L"选中的 " + std::to_wstring(rows.size()) + L" 个进程";

        std::wstring question = L"强制解除占用？\n\n"
                                L"将关闭" + scope + L"持有的 " + std::to_wstring(targets.size()) + L" 个文件句柄，不会结束任何进程。\n\n"
                                L"⚠ 这相当于“不结束程序就直接拔掉 U 盘”：持有该句柄的程序之后可能报错、\n"
                                L"   显示异常甚至崩溃，未保存的数据也可能丢失。\n\n"
                                L"请先保存工作；能结束进程时优先用“结束进程”。\n\n确定继续吗？";
        if (MessageBoxW(ui().window, question.c_str(), L"强制解除占用", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        std::vector<std::wstring> failures;
        const auto closed = release_handles(targets, failures);

        std::wstring status = L"已强制关闭 " + std::to_wstring(closed) + L"/" + std::to_wstring(targets.size()) + L" 个句柄";
        if (!failures.empty())
        {
            status += L"（" + std::to_wstring(failures.size()) + L" 个失败，多半是权限不足）";
        }
        status += L"，正在重新检查…";
        set_status(status);
        start_search();
    }

    void finish_delete()
    {
        std::vector<std::wstring> remaining;
        for (const auto& path : ui().paths)
        {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                remaining.push_back(path);
            }
        }

        const size_t deleted = ui().paths.size() - remaining.size();
        if (remaining != ui().paths)
        {
            ++ui().input_generation;
        }
        ui().paths = remaining;
        rebuild_app_targets();

        if (deleted > 0)
        {
            set_status(L"已删除 " + std::to_wstring(deleted) + L" 个路径"
                       + (remaining.empty() ? L"。" : L"，还有 " + std::to_wstring(remaining.size()) + L" 个没删掉。"));
        }

        update_header();
        if (ui().paths.empty())
        {
            ui().results.clear();
            ui().handles.clear();
            ui().handles_query_ok = false;
            ui().processes_query_ok = false;
            ui().result_valid = false;
            fill_list();
            update_menu_state();
        }
        else
        {
            start_search();
        }
    }

    void run_delete(bool already_released_handles)
    {
        std::vector<std::wstring> targets = ui().paths;
        std::wstring error;
        int code = 0;

        if (delete_paths(targets, error, &code))
        {
            finish_delete();
            return;
        }

        const bool lock_like = is_file_locked_error(code);
        if (!already_released_handles && lock_like && !ui().handles.empty())
        {
            // 第 2 道确认：强制关别人的句柄会影响别的程序，不能替用户点头
            std::wstring holders;
            size_t shown = 0;
            for (const auto& process : ui().results)
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
                L"将关闭 " + std::to_wstring(ui().handles.size()) + L" 个句柄，涉及下面这些进程：\n" + holders +
                L"\n⚠ 风险：这相当于不结束程序就直接拔 U 盘 —— 它们之后可能报错、崩溃，\n"
                L"   或者丢掉未保存的数据。这一步不会结束任何进程。\n\n"
                L"如果不想动它们，点「否」，改用「结束进程」或「全部结束」。";

            if (MessageBoxW(ui().window, question.c_str(), L"删除文件 — 需要强制解除占用", MB_YESNO | MB_ICONEXCLAMATION) != IDYES)
            {
                set_status(L"已取消删除（文件仍被占用）：" + error);
                return;
            }

            std::vector<std::wstring> failures;
            const auto closed = release_handles(ui().handles, failures);
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

        if (MessageBoxW(ui().window, question.c_str(), L"删除文件 — 重启后删除（不可还原）", MB_YESNO | MB_ICONWARNING) != IDYES)
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

    std::wstring describe_paths_for_dialog()
    {
        constexpr size_t max_shown = 10;
        std::wstring text;
        const size_t count = std::min(ui().paths.size(), max_shown);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& path = ui().paths[i];
            const DWORD attributes = GetFileAttributesW(path.c_str());
            const bool is_directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            text += L"  " + path + L"\n";
            if (is_directory)
            {
                text += L"      ↑ 文件夹：里面的内容会一起删除\n";
            }
        }
        if (ui().paths.size() > count)
        {
            text += L"  …以及另外 " + std::to_wstring(ui().paths.size() - count) + L" 个路径\n";
        }
        return text;
    }

    void delete_checked_paths()
    {
        if (!ensure_path_action(L"删除检查的文件"))
        {
            return;
        }

        const std::wstring question =
            L"确定要删除下面 " + std::to_wstring(ui().paths.size()) + L" 个路径吗？\n\n" +
            describe_paths_for_dialog() +
            L"\n删除后它们会进入回收站，可以从回收站还原。\n"
            L"（如果该驱动器禁用了回收站，则会变成永久删除）\n\n"
            L"若文件正被程序占用，下一步会单独问你一次 —— 那一步才会去动别人的句柄。";

        if (MessageBoxW(ui().window, question.c_str(), L"删除文件 — 确认删除", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            set_status(L"已取消删除。");
            return;
        }

        run_delete(false);
    }

    void open_file_location()
    {
        if (!ensure_result_action(L"打开所在位置"))
        {
            return;
        }

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
            ShellExecuteW(ui().window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
            opened = true;
        }

        if (!opened)
        {
            set_status(L"选中的行里没有可定位的文件。");
        }
    }

    void copy_selected(bool exe_path, bool pid_only, bool whole_row)
    {
        if (!ensure_result_action(L"复制结果"))
        {
            return;
        }

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

    void copy_to_clipboard(const std::wstring& text)
    {
        if (text.empty() || !OpenClipboard(ui().window))
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

    void show_list_menu()
    {
        const bool has_selection = !selected_rows().empty();
        const bool result_actions_enabled = !is_search_busy() && ui().result_valid &&
                                            ui().handles_query_ok && ui().processes_query_ok;

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const UINT state = (result_actions_enabled && has_selection) ? MF_ENABLED : MF_GRAYED;
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
        const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, ui().window, nullptr);
        DestroyMenu(menu);

        if (command != 0)
        {
            SendMessageW(ui().window, WM_COMMAND, static_cast<WPARAM>(command), 0);
        }
    }

    bool pick_files(std::vector<std::wstring>& chosen)
    {
        // 多选时缓冲区格式是 "目录\0文件1\0文件2\0\0"；单选时就是完整路径
        std::vector<wchar_t> buffer(32 * 1024, L'\0');

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = ui().window;
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
        browse.hwndOwner = ui().window;
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

    void add_files_via_dialog(bool replace)
    {
        std::vector<std::wstring> chosen;
        if (pick_files(chosen))
        {
            add_paths(chosen, replace);
        }
    }

    void add_folder_via_dialog(bool replace)
    {
        std::wstring chosen;
        if (pick_folder(chosen))
        {
            add_paths({ chosen }, replace);
        }
    }

    void restart_elevated()
    {
        auto args = ui().paths;
        if (args.empty())
        {
            args.push_back(L"--gui");
        }
        if (!app::relaunch_elevated(args))
        {
            set_status(L"提权被取消，或无法获取管理员权限。");
            return;
        }
        PostMessageW(ui().window, WM_CLOSE, 0, 0);
    }
}
