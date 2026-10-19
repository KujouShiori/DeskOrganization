#include "desklayout.h"

#include <algorithm>
#include <cstdio>

#include "iconview.h"

static constexpr size_t kMaxKeep = 10;

std::wstring BackupsDir() {
    std::wstring dir = AppDir() + L"\\layouts";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring NewBackupPath() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"\\layout-%04d%02d%02d-%02d%02d%02d.desklayout", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return BackupsDir() + name;
}

std::vector<std::wstring> ListBackups() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(BackupsDir(), L"layout-*.desklayout").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(JoinPath(BackupsDir(), fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::wstring NewestBackup() {
    auto all = ListBackups();
    return all.empty() ? std::wstring() : all.back();
}

bool DesktopLayoutBackup(const std::wstring& file, std::wstring& err) {
    std::vector<IconPosition> items;
    if (!IconViewRead(items, err)) return false;

    std::wstring text = L"# DeskOrganization 桌面图标布局\r\n";
    text += L"# 每行一个图标：名字 = x,y（相对桌面左上角的像素）\r\n";
    text += L"# Shell 会把坐标吸附到自己的网格上，所以还原时允许几个像素的偏差。\r\n";
    text += L"# 想删掉某个图标的位置记忆，直接删掉那一行即可；名字要和桌面上显示的名字完全一致。\r\n";
    for (const auto& it : items) {
        text += it.name + L" = " + std::to_wstring(it.x) + L"," + std::to_wstring(it.y) + L"\r\n";
    }
    if (!WriteFileBytes(file, WideToUtf8(text))) {
        err = L"写入备份文件失败";
        return false;
    }
    Log(L"桌面布局已备份：%zu 个图标 -> %s", items.size(), file.c_str());

    auto all = ListBackups();
    if (all.size() > kMaxKeep) {
        for (size_t i = 0; i + kMaxKeep < all.size(); ++i) DeleteFileW(all[i].c_str());
    }
    return true;
}

static bool ParseLayout(const std::wstring& text, std::vector<IconPosition>& out, std::wstring& err) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = nl == std::wstring::npos ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (nl == std::wstring::npos) break;
        start = nl + 1;

        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        line = line.substr(b);
        if (line.front() == L'#') continue;

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring name = line.substr(0, eq);
        while (!name.empty() && name.back() == L' ') name.pop_back();
        std::wstring coords = line.substr(eq + 1);
        size_t comma = coords.find(L',');
        if (comma == std::wstring::npos) continue;

        IconPosition p;
        p.name = name;
        p.x = static_cast<LONG>(_wtoi(coords.substr(0, comma).c_str()));
        p.y = static_cast<LONG>(_wtoi(coords.substr(comma + 1).c_str()));
        out.push_back(std::move(p));
    }
    if (out.empty()) err = L"文件里没有解析出任何图标位置";
    return !out.empty();
}

bool DesktopLayoutPeek(const std::wstring& file, size_t& outCount, std::wstring& err) {
    outCount = 0;
    std::string raw;
    if (!ReadFileBytes(file, raw)) {
        err = L"读不到备份文件";
        return false;
    }
    std::vector<IconPosition> items;
    if (!ParseLayout(Utf8ToWide(raw.c_str()), items, err)) return false;
    outCount = items.size();
    return true;
}

bool DesktopLayoutRestore(const std::wstring& file, std::wstring& err) {
    std::string raw;
    if (!ReadFileBytes(file, raw)) {
        err = L"读不到备份文件";
        return false;
    }
    std::vector<IconPosition> wanted;
    if (!ParseLayout(Utf8ToWide(raw.c_str()), wanted, err)) return false;

    // 还原前先存一份当前状态，摆坏了还能退回去。
    std::wstring safetyErr;
    DesktopLayoutBackup(NewBackupPath(), safetyErr);

    int moved = 0, unmatched = 0;
    if (!IconViewApply(wanted, err, moved, unmatched)) return false;
    Log(L"桌面布局还原完成：文件 %zu 项，移动 %d 项，桌面上找不到 %d 项", wanted.size(), moved, unmatched);
    return true;
}
