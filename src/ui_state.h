// FileLocksmith standalone - 界面状态持久化（窗口位置/大小、列宽）
// 存在 HKCU\Software\FileLocksmith\Ui 下，不需要管理员。
#pragma once
#include "win_common.h"

namespace ui_state
{
    struct WindowState
    {
        int x = INT_MIN;
        int y = INT_MIN;
        int width = 0;
        int height = 0;
        int columns[6] = { 0, 0, 0, 0, 0, 0 };
        int column_count = 0;
        bool maximized = false;
    };

    WindowState load();
    void save(const WindowState& state);
}
