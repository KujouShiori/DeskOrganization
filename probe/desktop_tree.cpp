// 只读探针：不修改任何系统状态，只把本机桌面窗口的真实层级打印出来。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>
#include <vector>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(-4))
#endif

static FILE* g_log = nullptr;

static std::string U8(LPCWSTR w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static void Line(const std::string& s) {
    fputs(s.c_str(), g_log);
    fputc('\n', g_log);
}

static std::string Indent(int depth) { return std::string(static_cast<size_t>(depth) * 2, ' '); }

static void DumpNode(HWND hwnd, int depth) {
    wchar_t cls[256] = {0};
    wchar_t txt[512] = {0};
    GetClassNameW(hwnd, cls, 255);
    GetWindowTextW(hwnd, txt, 511);
    RECT rc = {};
    GetWindowRect(hwnd, &rc);
    LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    char line[2048];
    snprintf(line, sizeof line,
             "%s hwnd=%p class=\"%s\" title=\"%s\" visible=%d rect=[%ld,%ld %ldx%ld] style=%08lX exstyle=%08lX",
             Indent(depth).c_str(), static_cast<void*>(hwnd), U8(cls).c_str(), U8(txt).c_str(),
             IsWindowVisible(hwnd) ? 1 : 0, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
             static_cast<unsigned long>(st), static_cast<unsigned long>(ex));
    Line(line);

    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        DumpNode(child, depth + 1);
    }
}

static std::vector<HWND> g_shellRoots;

static BOOL CALLBACK TopEnum(HWND h, LPARAM) {
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 255);
    std::string c = U8(cls);
    if (c == "Progman" || c == "WorkerW") g_shellRoots.push_back(h);
    return TRUE;
}

static void DumpReg(HKEY root, const std::string& path, int depth, int maxDepth) {
    HKEY key = nullptr;
    LONG r = RegOpenKeyExA(root, path.c_str(), 0, KEY_READ, &key);
    if (r != ERROR_SUCCESS) {
        Line(Indent(depth) + "OPEN FAILED (" + std::to_string(r) + "): " + path);
        return;
    }
    Line(Indent(depth) + "[key] " + path);

    DWORD subCount = 0, valCount = 0;
    RegQueryInfoKeyA(key, nullptr, nullptr, nullptr, &subCount, nullptr, nullptr, &valCount, nullptr, nullptr, nullptr, nullptr);

    for (DWORD i = 0; i < valCount; ++i) {
        char name[512];
        DWORD nameLen = sizeof name - 1;
        DWORD type = 0, size = 0;
        name[0] = 0;
        if (RegEnumValueA(key, i, name, &nameLen, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
            Line(Indent(depth) + "  (val) " + name + "  type=" + std::to_string(type) + " bytes=" + std::to_string(size));
        }
    }

    if (depth < maxDepth) {
        for (DWORD i = 0; i < subCount; ++i) {
            char sub[256];
            DWORD subLen = sizeof sub - 1;
            if (RegEnumKeyExA(key, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                DumpReg(root, path + "\\" + sub, depth + 1, maxDepth);
            }
        }
    } else if (subCount > 0) {
        Line(Indent(depth) + "  ... " + std::to_string(subCount) + " subkeys omitted");
    }
    RegCloseKey(key);
}

// shcore.h 在这个 SDK 里拿不到 GetDpiForMonitor 的声明，直接动态取。
typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

static BOOL CALLBACK MonEnum(HMONITOR m, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, &mi)) return TRUE;
    static GetDpiForMonitorFn fn = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
    UINT dpiX = 0, dpiY = 0;
    if (fn) fn(m, 0 /* MDT_EFFECTIVE_DPI */, &dpiX, &dpiY);
    char buf[512];
    snprintf(buf, sizeof buf, "  %s primary=%d monitor=[%ld,%ld %ldx%ld] work=[%ld,%ld %ldx%ld] effectiveDPI=%u (%u%%)",
             U8(mi.szDevice).c_str(), mi.dwFlags == MONITORINFOF_PRIMARY ? 1 : 0,
             mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
             mi.rcWork.left, mi.rcWork.top,
             mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top,
             dpiX, static_cast<unsigned>(dpiX * 100 / 96));
    Line(buf);
    return TRUE;
}

static void DumpReadFlag(HKEY root, const std::string& path, const char* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExA(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        Line(std::string("  ") + value + " = <key missing>");
        return;
    }
    DWORD type = 0, size = sizeof(DWORD), data = 0;
    LONG r = RegQueryValueExA(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size);
    if (r == ERROR_SUCCESS && type == REG_DWORD) Line(std::string("  ") + value + " = " + std::to_string(data));
    else Line(std::string("  ") + value + " = <absent, err=" + std::to_string(r) + ">");
    RegCloseKey(key);
}

static HWND g_probeWnd = nullptr;
static bool g_gotPaint = false;

static LRESULT CALLBACK ProbeProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        g_gotPaint = true;
        return 0;
    }
    return DefWindowProc(h, msg, w, l);
}

// 已知健康对照：证明"建窗口 -> 显示 -> 收到 WM_PAINT -> 记录"这条链路本身是好的。
// 若桌面层挂载失败而这一段成功，就能确定坏点在 WorkerW，而不是探针自己。
static void RunControl() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DeskOrgProbeControl";
    RegisterClassW(&wc);

    g_probeWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Probe control", WS_POPUP,
                                 40, 40, 220, 90, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g_probeWnd, SW_SHOWNA);
    UpdateWindow(g_probeWnd);

    MSG msg;
    DWORD until = GetTickCount() + 300;
    while (GetTickCount() < until) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
    Line(std::string("  created=") + (g_probeWnd ? "1" : "0") +
         " visible=" + (IsWindowVisible(g_probeWnd) ? "1" : "0") +
         " gotPaint=" + (g_gotPaint ? "1" : "0"));
    if (g_probeWnd) DestroyWindow(g_probeWnd);
    g_probeWnd = nullptr;
    g_gotPaint = false;
}

int main() {
    g_log = fopen("probe_out.txt", "wb");
    if (!g_log) return 1;

    typedef long(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (fn) {
        OSVERSIONINFOW ov = {sizeof ov};
        fn(&ov);
        char buf[256];
        snprintf(buf, sizeof buf, "OS = %lu.%lu build %lu", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
        Line(buf);
    }

    Line("");
    Line("=== DPI awareness -> Per-Monitor V2 ===");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    fprintf(g_log, "  GetDpiForSystem = %u\n", GetDpiForSystem());

    Line("");
    Line("=== Monitors ===");
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, 0);

    Line("");
    Line("=== Desktop icon flags (HKCU Explorer\\Advanced) ===");
    const std::string adv = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    for (const char* v : {"HideIcons", "AutoArrange", "NoAutoArrange", "ShowTypeOverlay"}) {
        DumpReadFlag(HKEY_CURRENT_USER, adv, v);
    }

    Line("");
    Line("=== Targeted lookups ===");
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND listview = defview ? FindWindowExW(defview, nullptr, L"SysListView32", nullptr) : nullptr;
    fprintf(g_log, "  Progman                = %p\n", static_cast<void*>(progman));
    fprintf(g_log, "  SHELLDLL_DefView parent= %p\n", static_cast<void*>(defview ? progman : nullptr));
    fprintf(g_log, "  SysListView32          = %p\n", static_cast<void*>(listview));
    if (listview) {
        LRESULT res = 0;
        SendMessageTimeoutW(listview, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 1000,
                            reinterpret_cast<ULONG_PTR*>(&res));
        fprintf(g_log, "  icon count (LVM_GETITEMCOUNT) = %lld\n", static_cast<long long>(res));
    } else {
        Line("  no SysListView32 found under Progman's DefView");
    }

    Line("");
    Line("=== Top-level shell desktop roots (Progman / WorkerW) ===");
    EnumWindows(TopEnum, 0);
    fprintf(g_log, "  matched %zu root(s)\n", g_shellRoots.size());
    for (HWND h : g_shellRoots) DumpNode(h, 0);

    Line("");
    Line("=== Icon position storage: HKCU\\Software\\Microsoft\\Windows\\Shell\\Bags (depth 2) ===");
    DumpReg(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\Shell\\Bags", 0, 2);

    Line("");
    Line("=== Known-healthy control ===");
    RunControl();

    fflush(g_log);
    fclose(g_log);
    return 0;
}
