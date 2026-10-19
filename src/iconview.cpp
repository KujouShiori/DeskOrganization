#include "iconview.h"

#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>

#include "util.h"

static HWND DesktopListView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    return FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
}

bool IconViewAvailable() { return DesktopListView() != nullptr; }

// 读名字要把缓冲区开在 Explorer 的进程里；读坐标也是同一个套路。
static bool RemoteReadText(HWND lv, int index, std::wstring& outName, HANDLE proc, BYTE* remoteItem,
                           BYTE* remoteText, size_t textChars) {
    LVITEMW li{};
    li.mask = LVIF_TEXT;
    li.iItem = index;
    li.pszText = reinterpret_cast<LPWSTR>(remoteText);
    li.cchTextMax = static_cast<int>(textChars);

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remoteItem, &li, sizeof(li), &written) || written != sizeof(li)) return false;
    SetLastError(0);
    SendMessageW(lv, LVM_GETITEMTEXTW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(remoteItem));
    std::vector<wchar_t> buf(textChars, 0);
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, remoteText, buf.data(), (textChars - 1) * sizeof(wchar_t), &got) || got == 0) {
        return false;
    }
    outName = buf.data();
    return true;
}

bool IconViewRead(std::vector<IconPosition>& out, std::wstring& err) {
    out.clear();
    err.clear();
    HWND lv = DesktopListView();
    if (!lv) {
        err = L"找不到桌面图标视图（SysListView32）";
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                   PROCESS_VM_WRITE,
                              FALSE, pid);
    if (!proc) {
        err = L"打不开 Explorer 进程读内存";
        return false;
    }

    const size_t textChars = 512;
    const size_t bytes = sizeof(LVITEMW) + textChars * sizeof(wchar_t) + sizeof(POINT);
    BYTE* remote = static_cast<BYTE*>(VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT, PAGE_READWRITE));
    if (!remote) {
        CloseHandle(proc);
        err = L"在 Explorer 里分配缓冲区失败";
        return false;
    }
    BYTE* remoteItem = remote;
    BYTE* remoteText = remote + sizeof(LVITEMW);
    BYTE* remotePoint = remoteText + textChars * sizeof(wchar_t);

    int count = static_cast<int>(SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0));
    bool anyPosFailed = false;
    for (int i = 0; i < count; ++i) {
        IconPosition p;
        POINT local{};
        SIZE_T got = 0;
        LRESULT pr = SendMessageW(lv, LVM_GETITEMPOSITION, static_cast<WPARAM>(i),
                                  reinterpret_cast<LPARAM>(remotePoint));
        if (pr && ReadProcessMemory(proc, remotePoint, &local, sizeof(local), &got) && got == sizeof(local)) {
            p.x = local.x;
            p.y = local.y;
        } else {
            anyPosFailed = true;
        }
        RemoteReadText(lv, i, p.name, proc, remoteItem, remoteText, textChars);
        if (p.name.empty()) continue;
        out.push_back(std::move(p));
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (out.empty()) err = L"桌面图标视图里没有项目";
    if (anyPosFailed && !out.empty()) err = L"部分图标坐标读取失败";
    return !out.empty();
}

bool IconViewApply(const std::vector<IconPosition>& in, std::wstring& err, int& moved, int& unmatched) {
    moved = 0;
    unmatched = 0;
    err.clear();

    HWND lv = DesktopListView();
    if (!lv) {
        err = L"找不到桌面图标视图";
        return false;
    }

    std::vector<IconPosition> current;
    if (!IconViewRead(current, err)) return false;

    for (const auto& want : in) {
        auto it = std::find_if(current.begin(), current.end(),
                               [&](const IconPosition& c) { return _wcsicmp(c.name.c_str(), want.name.c_str()) == 0; });
        if (it == current.end()) {
            unmatched++;
            continue;
        }
        int index = static_cast<int>(std::distance(current.begin(), it));
        LRESULT r = SendMessageW(lv, LVM_SETITEMPOSITION, static_cast<WPARAM>(index),
                                 MAKELPARAM(static_cast<WORD>(want.x), static_cast<WORD>(want.y)));
        if (r == 0) {
            err = L"LVM_SETITEMPOSITION 被拒绝（桌面可能开着自动排列）";
            return false;
        }
        moved++;
    }

    // Shell 会把坐标吸附到自己的网格里，所以移动请求不保证逐像素命中，只能事后核对。
    std::vector<IconPosition> after;
    if (!IconViewRead(after, err)) return false;
    int offGrid = 0;
    for (const auto& want : in) {
        auto a = std::find_if(after.begin(), after.end(),
                              [&](const IconPosition& c) { return _wcsicmp(c.name.c_str(), want.name.c_str()) == 0; });
        if (a == after.end()) continue;
        if (abs(static_cast<int>(a->x - want.x)) > 4 || abs(static_cast<int>(a->y - want.y)) > 4) offGrid++;
    }
    if (offGrid > 0) {
        wchar_t b[160];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%d 个图标被 Shell 吸附到网格，未落在请求坐标上", offGrid);
        err = b;
    }
    Log(L"图标位置还原：移动 %d 个，未匹配 %d 个，偏移 %d 个", moved, unmatched, offGrid);
    return true;
}

static std::wstring StripExtension(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return name;
    return name.substr(0, dot);
}

// 桌面列表给的是显示名：已注册的扩展名（.lnk）会被藏起来，未注册的（.ods）不会。
// 所以匹配必须同时接受全名和去后缀两种形态，否则快捷方式永远对不上。
static bool NameMatches(const std::wstring& shown, const std::wstring& filePart) {
    if (shown.empty() || filePart.empty()) return false;
    if (_wcsicmp(shown.c_str(), filePart.c_str()) == 0) return true;
    return _wcsicmp(shown.c_str(), StripExtension(filePart).c_str()) == 0;
}

std::vector<IconPosition> IconViewCaptureInDir(const std::vector<std::wstring>& paths, const std::wstring& dir) {
    std::vector<IconPosition> captured;
    if (paths.empty() || dir.empty()) return captured;

    std::vector<IconPosition> live;
    std::wstring err;
    if (!IconViewRead(live, err)) return captured;

    for (const auto& path : paths) {
        size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) continue;
        std::wstring parent = path.substr(0, slash);
        if (_wcsicmp(parent.c_str(), dir.c_str()) != 0) continue;
        std::wstring filePart = path.substr(slash + 1);
        for (const auto& it : live) {
            if (NameMatches(it.name, filePart)) {
                captured.push_back(it);
                break;
            }
        }
    }
    return captured;
}

bool IconViewRestore(const std::vector<IconPosition>& wanted, std::wstring& err, DWORD timeoutMs) {
    err.clear();
    if (wanted.empty()) return true;

    std::vector<IconPosition> pending = wanted;
    DWORD start = GetTickCount();
    int lastUnmatched = static_cast<int>(pending.size());

    while (!pending.empty()) {
        std::vector<IconPosition> stillMissing;
        for (const auto& w : pending) {
            bool present = false;
            std::vector<IconPosition> live;
            std::wstring readErr;
            if (IconViewRead(live, readErr)) {
                for (const auto& c : live) {
                    if (_wcsicmp(c.name.c_str(), w.name.c_str()) == 0) { present = true; break; }
                }
            }
            if (present) {
                int moved = 0, unmatched = 0;
                std::wstring applyErr;
                IconViewApply(std::vector<IconPosition>{w}, applyErr, moved, unmatched);
                if (moved == 0) stillMissing.push_back(w);
            } else {
                stillMissing.push_back(w);
            }
        }
        pending = stillMissing;
        lastUnmatched = static_cast<int>(pending.size());
        if (pending.empty()) break;
        if (GetTickCount() - start >= timeoutMs) break;
        Sleep(200);
    }

    if (lastUnmatched > 0) {
        wchar_t b[160];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"有 %d 个图标没能等到出现，位置未还原", lastUnmatched);
        err = b;
    }
    return lastUnmatched == 0;
}
