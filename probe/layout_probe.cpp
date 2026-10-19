// 桌面布局实验探针。每一步都 dump 出每个图标的真实坐标，让"有没有移动"变成数字对比。
// 用法：
//   save     把 IconLayouts 原样存成 layout_raw.bin
//   clear    删除 IconLayouts 并通知桌面
//   restore  把 layout_raw.bin 写回并通知桌面
//   dump     只读坐标
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include <vector>

static const wchar_t* kRaw = L"layout_raw.bin";

static std::string U8(LPCWSTR w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void Line(FILE* f, const std::string& s) { fputs(s.c_str(), f); fputc('\n', f); fflush(f); }

static HWND FindDesktopListView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    return FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
}

static std::string BagKey() {
    HKEY root = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\Shell\\Bags", 0, KEY_READ, &root) !=
        ERROR_SUCCESS)
        return std::string();
    DWORD subs = 0;
    RegQueryInfoKeyA(root, nullptr, nullptr, nullptr, &subs, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     nullptr);
    std::string found;
    for (DWORD i = 0; i < subs && found.empty(); ++i) {
        char bag[256];
        DWORD len = sizeof bag;
        if (RegEnumKeyExA(root, i, bag, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) continue;
        std::string cand = std::string("Software\\Microsoft\\Windows\\Shell\\Bags\\") + bag + "\\Desktop";
        HKEY h = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, cand.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(h, L"IconLayouts", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size > 0) {
            found = cand;
        }
        RegCloseKey(h);
    }
    RegCloseKey(root);
    return found;
}

static bool ReadLayout(std::vector<BYTE>& out) {
    std::string key = BagKey();
    if (key.empty()) return false;
    HKEY h = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = RegQueryValueExW(h, L"IconLayouts", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_BINARY;
    if (ok) {
        out.resize(size);
        ok = RegQueryValueExW(h, L"IconLayouts", nullptr, nullptr, out.data(), &size) == ERROR_SUCCESS;
        out.resize(size);
    }
    RegCloseKey(h);
    return ok;
}

static void NotifyShell() {
    // 和程序本体走同一个 API：SHGetFolderPath 拿到的是老式路径，
    // 桌面被重定向（比如 OneDrive）时就不是用户实际看到的那个桌面。
    PWSTR desk = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) || !desk) return;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(desk);
    CoTaskMemFree(desk);
    if (!pidl) return;
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSH, pidl, nullptr);
    CoTaskMemFree(pidl);
}

struct ItemPos {
    int index = 0;
    std::wstring name;
    LONG x = -1;
    LONG y = -1;
    bool posOk = false;
};

static std::vector<ItemPos> ReadPositions(HWND lv) {
    std::vector<ItemPos> out;
    if (!lv) return out;

    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                   PROCESS_VM_WRITE,
                               FALSE, pid);
    if (!proc) return out;

    const size_t textChars = 512;
    BYTE* remote = static_cast<BYTE*>(
        VirtualAllocEx(proc, nullptr, sizeof(LVITEMW) + textChars * sizeof(wchar_t) + sizeof(POINT), MEM_COMMIT,
                       PAGE_READWRITE));
    if (!remote) {
        CloseHandle(proc);
        return out;
    }
    BYTE* remoteItem = remote;
    BYTE* remoteText = remote + sizeof(LVITEMW);
    BYTE* remotePoint = remoteText + textChars * sizeof(wchar_t);

    int count = static_cast<int>(SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        ItemPos it;
        it.index = i;

        POINT local{};
        SetLastError(0);
        LRESULT pr = SendMessageW(lv, LVM_GETITEMPOSITION, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(remotePoint));
        SIZE_T got = 0;
        if (pr && ReadProcessMemory(proc, remotePoint, &local, sizeof(local), &got) && got == sizeof(local)) {
            it.x = local.x;
            it.y = local.y;
            it.posOk = true;
        }

        LVITEMW li{};
        li.mask = LVIF_TEXT;
        li.iItem = i;
        li.pszText = reinterpret_cast<LPWSTR>(remoteText);
        li.cchTextMax = static_cast<int>(textChars);
        if (WriteProcessMemory(proc, remoteItem, &li, sizeof(li), &got) && got == sizeof(li)) {
            SetLastError(0);
            SendMessageW(lv, LVM_GETITEMTEXTW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(remoteItem));
            wchar_t buf[textChars] = {};
            if (ReadProcessMemory(proc, remoteText, buf, (textChars - 1) * sizeof(wchar_t), &got)) {
                buf[textChars - 1] = 0;
                it.name = buf;
            }
        }
        out.push_back(std::move(it));
    }

    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return out;
}

static void Dump(FILE* f, const char* stage, const std::vector<BYTE>& layout) {
    HWND lv = FindDesktopListView();
    auto items = ReadPositions(lv);
    char head[256];
    snprintf(head, sizeof head, "--- %s: listview=%p items=%zu layoutBytes=%zu posReadOk=%d", stage,
             static_cast<void*>(lv), items.size(), layout.size(),
             static_cast<int>(!items.empty() && items[0].posOk));
    Line(f, head);
    unsigned long fingerprint = 0;
    for (const auto& it : items) {
        char b[512];
        snprintf(b, sizeof b, "    [%d] (%ld,%ld) %s", it.index, it.x, it.y, U8(it.name.c_str()).c_str());
        Line(f, b);
        fingerprint = fingerprint * 131u + static_cast<unsigned long>(it.x) * 31u + static_cast<unsigned long>(it.y);
    }
    char fp[128];
    snprintf(fp, sizeof fp, "    position fingerprint = %lu", fingerprint);
    Line(f, fp);
}

static unsigned long LayoutChecksum() {
    std::vector<BYTE> b;
    if (!ReadLayout(b)) return 0;
    unsigned long h = 2166136261u;
    for (BYTE x : b) {
        h ^= x;
        h *= 16777619u;
    }
    return h;
}

// LVM_SETITEMPOSITION 直接吃 MAKELPARAM(x,y)，不需要跨进程共享内存。
static bool ApplyPosition(HWND lv, int index, LONG x, LONG y) {
    if (!lv) return false;
    LRESULT r = SendMessageW(lv, LVM_SETITEMPOSITION, static_cast<WPARAM>(index),
                             MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y)));
    return r != 0;
}

static long long Fingerprint(const std::vector<ItemPos>& items) {
    long long f = 0;
    for (const auto& it : items) f = f * 131 + it.x * 31 + it.y;
    return f;
}

static void RunMoveTest(FILE* f) {
    HWND lv = FindDesktopListView();
    auto p0 = ReadPositions(lv);
    long long fp0 = Fingerprint(p0);
    unsigned long c0 = LayoutChecksum();
    if (p0.empty()) {
        Line(f, "movetest: cannot read positions, abort");
        return;
    }
    int idx = static_cast<int>(p0.size()) - 1;
    LONG ox = p0[idx].x, oy = p0[idx].y;
    char b0[256];
    snprintf(b0, sizeof b0, "movetest: item[%d]=(%ld,%ld) checksum=%lu -> move to (%ld,%ld)", idx, ox, oy, c0,
             ox + 120, oy + 120);
    Line(f, b0);

    if (!ApplyPosition(lv, idx, ox + 120, oy + 120)) {
        Line(f, "movetest: LVM_SETITEMPOSITION 被拒绝");
        return;
    }
    Sleep(2000);
    auto p1 = ReadPositions(lv);
    long long fp1 = Fingerprint(p1);
    unsigned long c1 = LayoutChecksum();
    char b1[256];
    snprintf(b1, sizeof b1, "  after move: item=(%ld,%ld) fingerprint %s checksum %lu -> live=%s registryFollows=%s",
             p1.size() > static_cast<size_t>(idx) ? p1[idx].x : -999,
             p1.size() > static_cast<size_t>(idx) ? p1[idx].y : -999, fp1 == fp0 ? "same" : "CHANGED", c1,
             fp1 != fp0 ? "YES" : "NO", c1 != c0 ? "YES" : "NO");
    Line(f, b1);

    ApplyPosition(lv, idx, ox, oy);
    Sleep(2000);
    auto p2 = ReadPositions(lv);
    long long fp2 = Fingerprint(p2);
    unsigned long c2 = LayoutChecksum();
    char b2[256];
    snprintf(b2, sizeof b2, "  after restore: item=(%ld,%ld) fingerprint %s checksum %lu (original %lu)",
             p2.size() > static_cast<size_t>(idx) ? p2[idx].x : -999,
             p2.size() > static_cast<size_t>(idx) ? p2[idx].y : -999, fp2 == fp0 ? "MATCHES original" : "DIFFERS", c2,
             c0);
    Line(f, b2);
    Line(f, std::string("  net effect: ") + (fp2 == fp0 ? "桌面已回到实验前的排列" : "桌面未能回到原排列，需要手工调整"));
}

int main(int argc, char** argv) {
    FILE* f = nullptr;
    fopen_s(&f, "probe_layout.txt", "a");
    if (!f) return 1;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const char* action = argc > 1 ? argv[1] : "dump";
    std::vector<BYTE> layout;
    ReadLayout(layout);

    if (strcmp(action, "save") == 0) {
        Dump(f, "save/before", layout);
        HANDLE h = CreateFileW(kRaw, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE && !layout.empty()) {
            DWORD w = 0;
            WriteFile(h, layout.data(), static_cast<DWORD>(layout.size()), &w, nullptr);
            CloseHandle(h);
            Line(f, std::string("    saved ") + std::to_string(layout.size()) + " bytes to layout_raw.bin");
        } else {
            Line(f, "    SAVE FAILED (no layout bytes or cannot write)");
        }
    } else if (strcmp(action, "clear") == 0) {
        std::string key = BagKey();
        HKEY h = nullptr;
        LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &h);
        if (r == ERROR_SUCCESS) {
            LONG d = RegDeleteValueW(h, L"IconLayouts");
            RegCloseKey(h);
            char b[256];
            snprintf(b, sizeof b, "    RegDeleteValueW(IconLayouts) -> %ld on %s", d, key.c_str());
            Line(f, b);
        } else {
            char b[128];
            snprintf(b, sizeof b, "    open for write failed %ld", r);
            Line(f, b);
        }
        NotifyShell();
        Sleep(2500);
        std::vector<BYTE> after;
        ReadLayout(after);
        Dump(f, "clear/after", after);
    } else if (strcmp(action, "restore") == 0) {
        HANDLE h = CreateFileW(kRaw, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Line(f, "    no layout_raw.bin to restore from");
        } else {
            DWORD size = GetFileSize(h, nullptr);
            std::vector<BYTE> bytes(size);
            DWORD got = 0;
            ReadFile(h, bytes.data(), size, &got, nullptr);
            CloseHandle(h);
            bytes.resize(got);
            std::string key = BagKey();
            if (key.empty()) key = "Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop";
            HKEY kh = nullptr;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &kh) == ERROR_SUCCESS) {
                LONG r = RegSetValueExW(kh, L"IconLayouts", 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size()));
                RegCloseKey(kh);
                char b[256];
                snprintf(b, sizeof b, "    RegSetValueExW -> %ld, wrote %lu bytes", r, bytes.size());
                Line(f, b);
                NotifyShell();
                Sleep(2500);
                std::vector<BYTE> after;
                ReadLayout(after);
                Dump(f, "restore/after", after);
            }
        }
    } else if (strcmp(action, "movetest") == 0) {
        RunMoveTest(f);
    } else if (strcmp(action, "apply") == 0 && argc >= 5) {
        int index = atoi(argv[2]);
        LONG x = static_cast<LONG>(atol(argv[3]));
        LONG y = static_cast<LONG>(atol(argv[4]));
        bool r = ApplyPosition(FindDesktopListView(), index, x, y);
        char m[128];
        snprintf(m, sizeof m, "apply index=%d -> (%ld,%ld) result=%d", index, x, y, r ? 1 : 0);
        Line(f, m);
        Sleep(1500);
        Dump(f, "apply/after", layout);
    } else if (strcmp(action, "pixel") == 0 && argc >= 4) {
        int x = atoi(argv[2]);
        int y = atoi(argv[3]);
        HDC s = GetDC(nullptr);
        COLORREF c = GetPixel(s, x, y);
        ReleaseDC(nullptr, s);
        POINT p{x, y};
        HWND under = WindowFromPoint(p);
        wchar_t cls[128] = {};
        if (under) GetClassNameW(under, cls, 127);
        char m[200];
        snprintf(m, sizeof m, "pixel(%d,%d)=(%d,%d,%d) on %hs", x, y, GetRValue(c), GetGValue(c), GetBValue(c),
                 U8(cls).c_str());
        Line(f, m);
    } else if (strcmp(action, "nudge") == 0 && argc >= 3) {
        // 把盒子窗口挪到屏幕外：用来在不动用户分辨率的前提下验证屏幕外回收。
        // 按序号而不是按标题选窗口：控制台代码页是 GBK，argv 里的中文不是 UTF-8，
        // 按名字匹配会先撞上编码问题而不是验证到逻辑。
        const int wantIndex = atoi(argv[2]);
        struct FindCtx {
            int seen;
            int want;
            HWND found;
        } ctx{0, wantIndex, nullptr};
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(l);
            wchar_t cls[128] = {};
            GetClassNameW(h, cls, 127);
            if (lstrcmpW(cls, L"DeskOrgBox") != 0) return TRUE;
            if (c->seen++ != c->want) return TRUE;
            c->found = h;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&ctx));

        char m[200];
        if (!ctx.found) {
            snprintf(m, sizeof m, "nudge: no box at index %d", wantIndex);
        } else {
            const BOOL ok = SetWindowPos(ctx.found, HWND_BOTTOM, 9000, 9000, 340, 260, SWP_NOACTIVATE);
            snprintf(m, sizeof m, "nudge: hwnd=%p moved off screen ok=%d", static_cast<void*>(ctx.found), ok);
        }
        Line(f, m);
    } else if (strcmp(action, "displaychange") == 0) {
        // 系统改分辨率时就是这么广播的，手动发一条来验那个处理器。
        HWND tray = FindWindowW(L"DeskOrgTray", nullptr);
        char m[160];
        if (!tray) {
            snprintf(m, sizeof m, "displaychange: no running instance");
        } else {
            snprintf(m, sizeof m, "displaychange: posted ok=%d", PostMessageW(tray, WM_DISPLAYCHANGE, 0, 0));
        }
        Line(f, m);
    } else if (strcmp(action, "boxes") == 0) {
        struct Ctx { FILE* f; int n; } ctx{f, 0};
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            wchar_t cls[128] = {};
            GetClassNameW(h, cls, 127);
            if (lstrcmpW(cls, L"DeskOrgBox") != 0) return TRUE;
            auto* c = reinterpret_cast<Ctx*>(l);
            wchar_t title[128] = {};
            GetWindowTextW(h, title, 127);
            RECT rc{};
            GetWindowRect(h, &rc);
            char b[256];
            snprintf(b, sizeof b, "  box %s visible=%d rect=[%ld,%ld %ldx%ld]", U8(title).c_str(),
                     IsWindowVisible(h) ? 1 : 0, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
            Line(c->f, b);
            c->n++;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        char t[64];
        snprintf(t, sizeof t, "boxes found=%d", ctx.n);
        Line(f, t);
    } else {
        Dump(f, "dump", layout);
    }

    fclose(f);
    CoUninitialize();
    return 0;
}
