// 只读：把桌面图标位置的两个候选存储位置都 dump 出来，并列出桌面文件夹的真实条目，
// 用来反推 IconLayouts 的二进制结构，而不是照抄网上的说法。
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string>
#include <vector>

static FILE* g_log = nullptr;

static std::string U8(LPCWSTR w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void Line(const std::string& s) { fputs(s.c_str(), g_log); fputc('\n', g_log); }

static bool ReadValue(HKEY root, const std::string& key, const wchar_t* value, std::vector<BYTE>& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExA(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(h, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        RegCloseKey(h);
        return false;
    }
    if (type != REG_BINARY) {
        char b[128];
        snprintf(b, sizeof b, "  %ls: type=%lu (不是 REG_BINARY)", value, type);
        Line(b);
        RegCloseKey(h);
        return false;
    }
    out.resize(size);
    RegQueryValueExW(h, value, nullptr, nullptr, out.data(), &size);
    out.resize(size);
    RegCloseKey(h);
    return true;
}

static void DumpHex(const std::vector<BYTE>& bytes, const char* label) {
    char head[128];
    snprintf(head, sizeof head, "%s total=%zu bytes", label, bytes.size());
    Line(head);
    for (size_t off = 0; off < bytes.size(); off += 16) {
        char line[256];
        int n = snprintf(line, sizeof line, "  %04zX  ", off);
        for (size_t i = 0; i < 16 && off + i < bytes.size(); ++i) {
            n += snprintf(line + n, sizeof(line) - n, "%02X ", bytes[off + i]);
        }
        while (n < 10 + 48) n += snprintf(line + n, sizeof(line) - n, " ");
        n += snprintf(line + n, sizeof(line) - n, " ");
        for (size_t i = 0; i < 16 && off + i < bytes.size(); ++i) {
            unsigned char c = bytes[off + i];
            line[n++] = (c >= 0x20 && c < 0x7F) ? char(c) : '.';
        }
        line[n] = 0;
        Line(line);
    }
}

static void ListValues(const std::string& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        Line("  KEY ABSENT: " + key);
        return;
    }
    Line("  KEY: " + key);
    DWORD count = 0;
    RegQueryInfoKeyA(h, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr, nullptr);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[256];
        DWORD nameLen = _countof(name);
        DWORD type = 0, size = 0;
        if (RegEnumValueW(h, i, name, &nameLen, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
            char b[200];
            snprintf(b, sizeof b, "    %-28ls type=%lu size=%lu", name, type, size);
            Line(b);
        }
    }
    RegCloseKey(h);
}

int main() {
    g_log = fopen("probe_bag.txt", "wb");
    if (!g_log) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const char* candidates[] = {
        "Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
        "Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
    };

    for (const char* key : candidates) {
        Line("");
        Line(std::string("=== ") + key + " ===");
        ListValues(key);
        std::vector<BYTE> bytes;
        if (ReadValue(HKEY_CURRENT_USER, key, L"IconLayouts", bytes)) DumpHex(bytes, "  IconLayouts hex:");
        else Line("  没有 IconLayouts");
    }

    Line("");
    Line("=== 桌面文件夹真实条目（IShellFolder 枚举）===");
    IShellFolder* desktop = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop) {
        IEnumIDList* en = nullptr;
        HRESULT hr = desktop->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &en);
        if (SUCCEEDED(hr) && en) {
            LPITEMIDLIST pidl = nullptr;
            ULONG fetched = 0;
            while (en->Next(1, &pidl, &fetched) == S_OK) {
                STRRET sr{};
                wchar_t path[1024] = {};
                if (SUCCEEDED(desktop->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr))) {
                    StrRetToBufW(&sr, pidl, path, _countof(path));
                }
                char mb[512];
                snprintf(mb, sizeof mb, "  %s  (pidl %lu bytes)", U8(path).c_str(), (unsigned long)ILGetSize(pidl));
                Line(mb);
                CoTaskMemFree(pidl);
            }
            en->Release();
        }
        desktop->Release();
    }

    fflush(g_log);
    fclose(g_log);
    CoUninitialize();
    return 0;
}
