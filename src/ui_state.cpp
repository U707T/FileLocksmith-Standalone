// FileLocksmith standalone
#include "ui_state.h"

namespace ui_state
{
    namespace
    {
        constexpr wchar_t UiKey[] = L"Software\\FileLocksmith\\Ui";

        bool read_dword(HKEY key, const wchar_t* name, int& value)
        {
            DWORD data = 0;
            DWORD size = sizeof(data);
            DWORD type = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) != ERROR_SUCCESS)
            {
                return false;
            }
            value = static_cast<int>(data);
            return type == REG_DWORD;
        }

        int read_dword_or(HKEY key, const wchar_t* name, int fallback)
        {
            int value = fallback;
            return read_dword(key, name, value) ? value : fallback;
        }
    }

    WindowState load()
    {
        WindowState state;

        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, UiKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        {
            return state;
        }

        state.x = read_dword_or(key, L"WindowX", INT_MIN);
        state.y = read_dword_or(key, L"WindowY", INT_MIN);
        state.width = read_dword_or(key, L"WindowWidth", 0);
        state.height = read_dword_or(key, L"WindowHeight", 0);
        state.maximized = read_dword_or(key, L"Maximized", 0) != 0;

        for (int i = 0; i < 6; ++i)
        {
            wchar_t name[32];
            wsprintfW(name, L"Column%d", i);
            const int width = read_dword_or(key, name, 0);
            if (width > 0)
            {
                state.columns[i] = width;
                state.column_count = i + 1;
            }
        }

        RegCloseKey(key);
        return state;
    }

    void save(const WindowState& state)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, UiKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return;
        }

        const auto write = [key](const wchar_t* name, int value) {
            const DWORD data = static_cast<DWORD>(value);
            RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
        };

        write(L"WindowX", state.x);
        write(L"WindowY", state.y);
        write(L"WindowWidth", state.width);
        write(L"WindowHeight", state.height);
        write(L"Maximized", state.maximized ? 1 : 0);

        for (int i = 0; i < state.column_count && i < 6; ++i)
        {
            wchar_t name[32];
            wsprintfW(name, L"Column%d", i);
            write(name, state.columns[i]);
        }

        RegCloseKey(key);
    }
}
