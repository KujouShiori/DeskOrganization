// 模拟最坏情况：图标层被隐藏后进程直接消失，既没清理标记也没恢复图标。
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>

int main() {
    wchar_t appdata[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    wchar_t dir[MAX_PATH + 64];
    _snwprintf_s(dir, _countof(dir), _TRUNCATE, L"%s\\DeskOrganization", appdata);
    CreateDirectoryW(dir, nullptr);

    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    ShowWindow(defView, SW_HIDE);
    Sleep(200);

    wchar_t marker[MAX_PATH + 64];
    _snwprintf_s(marker, _countof(marker), _TRUNCATE, L"%s\\icons_hidden.marker", dir);
    HANDLE h = CreateFileW(marker, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, "hidden", 6, &w, nullptr);
        CloseHandle(h);
    }

    wchar_t out[256];
    _snwprintf_s(out, _countof(out), _TRUNCATE, L"defView visible=%d marker=%d\n", IsWindowVisible(defView) ? 1 : 0,
                 GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES ? 1 : 0);
    HANDLE con = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(con, out, (DWORD)(wcslen(out) * 2), &written, nullptr);
    return 0;
}
