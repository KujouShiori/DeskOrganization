// 桌面层挂载探针：测试两种把窗口放到桌面层的做法，结果写成机器可读的 PASS/FAIL。
// 只创建自己的窗口，不隐藏系统图标层，退出即恢复。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
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

static void Line(const std::string& s) { fputs(s.c_str(), g_log); fputc('\n', g_log); }
static void Verdict(const char* what, bool ok) { Line(std::string("  ") + what + " : " + (ok ? "PASS" : "FAIL")); }

static std::string Hex(HWND h) {
    char b[32];
    snprintf(b, sizeof b, "%p", static_cast<void*>(h));
    return b;
}

static std::string ClassName(HWND h) {
    if (!h) return "null";
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 255);
    return U8(cls);
}

static std::string Describe(HWND h) {
    if (!h) return "null";
    wchar_t txt[256] = {0};
    GetWindowTextW(h, txt, 255);
    return ClassName(h) + " \"" + U8(txt) + "\" " + Hex(h);
}

static int CountWorkerW() {
    struct Ctx { int n; } ctx{0};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (ClassName(h) == "WorkerW") reinterpret_cast<Ctx*>(l)->n++;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.n;
}

static HWND FindWorkerWWithDefView() {
    struct Ctx { HWND found; } ctx{nullptr};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (ClassName(h) != "WorkerW") return TRUE;
        if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) {
            reinterpret_cast<Ctx*>(l)->found = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

static void ReportDefView(const char* stage) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND atProgman = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND workerw = FindWorkerWWithDefView();
    Line(std::string("  ") + stage + ": defView-in-Progman=" + (atProgman ? "yes" : "no") +
         " defView-in-WorkerW=" + (workerw ? "yes" : "no") +
         " workerWCount=" + std::to_string(CountWorkerW()) +
         " listview-under-progman-defview=" +
         (atProgman ? (FindWindowExW(atProgman, nullptr, L"SysListView32", nullptr) ? "yes" : "no") : "n/a"));
}

static bool g_painted = false;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH br = CreateSolidBrush(RGB(40, 90, 200));
        FillRect(dc, &ps.rcPaint, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        g_painted = true;
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProc(h, msg, w, l);
}

static void Pump(DWORD ms) {
    MSG m;
    DWORD until = GetTickCount() + ms;
    while (GetTickCount() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

static const wchar_t* kClass = L"DeskOrgProbeWnd";
static const wchar_t* kAppClass = L"DeskOrgProbeAppWnd";

static void RegisterClass(const wchar_t* name) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    RegisterClassW(&wc);
}

static void ForcePaint(HWND ours) {
    g_painted = false;
    InvalidateRect(ours, nullptr, FALSE);
    Pump(150);
}

// 我们想知道两件事：我们的窗口能不能盖在桌面层被别的应用正常遮挡；
// 以及在"应用窗口最小化后"那个像素点上到底是谁 —— 后者决定我们相对原生图标层的位置。
static void TestBehavior(const char* label, HWND ours, POINT pt) {
    Line("");
    Line(std::string("=== ") + label + " ===");
    ForcePaint(ours);

    RECT rc = {};
    GetWindowRect(ours, &rc);
    Line(std::string("  our parent = ") + Describe(GetParent(ours)));
    Line(std::string("  our rect = [") + std::to_string(rc.left) + "," + std::to_string(rc.top) + " " +
         std::to_string(rc.right - rc.left) + "x" + std::to_string(rc.bottom - rc.top) + "]" +
         " visible=" + (IsWindowVisible(ours) ? "1" : "0") + " painted=" + (g_painted ? "1" : "0"));
    Verdict("we actually painted", g_painted);
    Verdict("we stay visible while not covered", IsWindowVisible(ours));

    HWND overlap = CreateWindowExW(0, kAppClass, L"Probe app window", WS_OVERLAPPEDWINDOW,
                                   rc.left, rc.top, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(overlap, SW_SHOW);
    SetForegroundWindow(overlap);
    Pump(400);

    HWND found = WindowFromPoint(pt);
    HWND root = GetAncestor(found, GA_ROOT);
    Line(std::string("  app window covers us -> hit: ") + Describe(found) + " root: " + Describe(root));
    Verdict("yields to a normal app window", root == overlap);

    ShowWindow(overlap, SW_MINIMIZE);
    Pump(400);
    found = WindowFromPoint(pt);
    root = GetAncestor(found, GA_ROOT);
    Line(std::string("  app window minimized -> hit root: ") + Describe(root));
    Verdict("is revealed again after minimize", root == ours);
    Line(std::string("  note: if the revealed window is SHELLDLL_DefView or SysListView32, the native icon layer sits ABOVE us"));

    DestroyWindow(overlap);
    Pump(200);
}

int main() {
    g_log = fopen("probe_workerw.txt", "wb");
    if (!g_log) return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RegisterClass(kClass);
    RegisterClass(kAppClass);

    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    Line(std::string("primary monitor = [") + std::to_string(mi.rcMonitor.left) + "," + std::to_string(mi.rcMonitor.top) +
         " " + std::to_string(mi.rcMonitor.right - mi.rcMonitor.left) + "x" +
         std::to_string(mi.rcMonitor.bottom - mi.rcMonitor.top) + "]");
    Line("GetDpiForSystem = " + std::to_string(GetDpiForSystem()));
    Line("");
    Line("=== baseline ===");
    ReportDefView("baseline");

    int before = CountWorkerW();

    HWND plain = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Probe B bottom-level top window",
                                 WS_POPUP, 80, 80, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetLayeredWindowAttributes(plain, 0, 150, LWA_ALPHA);
    ShowWindow(plain, SW_SHOWNA);
    SetWindowPos(plain, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Pump(300);
    TestBehavior("Variant B: top-level pinned to HWND_BOTTOM", plain, POINT{100, 100});
    DestroyWindow(plain);
    Pump(200);

    Line("");
    Line("=== sending Progman 0x052C to spawn a desktop WorkerW ===");
    HWND progman = FindWindowW(L"Progman", nullptr);
    struct Try { WPARAM wp; LPARAM lp; } tries[] = {{0, 0}, {0, 1}, {0xD, 1}};
    HWND target = nullptr;
    for (auto& t : tries) {
        LRESULT ok = 0;
        SendMessageTimeoutW(progman, 0x052C, t.wp, t.lp, SMTO_NORMAL, 1000, reinterpret_cast<ULONG_PTR*>(&ok));
        Pump(300);
        int now = CountWorkerW();
        Line(std::string("  wParam=") + std::to_string(static_cast<unsigned long long>(t.wp)) +
             " lParam=" + std::to_string(static_cast<unsigned long long>(t.lp)) +
             " returned=" + std::to_string(static_cast<long long>(ok)) +
             " workerW " + std::to_string(before) + " -> " + std::to_string(now));
        target = FindWorkerWWithDefView();
        if (now > before || target) { Line("  -> stopped after this try"); break; }
    }
    ReportDefView("after 0x052C");

    if (!target) {
        Line("  no WorkerW hosts SHELLDLL_DefView; using Progman as the parent instead");
        target = progman;
    }
    Line(std::string("  attach target = ") + Describe(target));

    HWND child = CreateWindowExW(WS_EX_LAYERED, kClass, L"Probe A reparented into desktop", WS_POPUP,
                                 80, 80, 320, 200, target, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!child) {
        char e[128];
        snprintf(e, sizeof e, "  CreateWindowEx as child of a foreign-process window FAILED, gle=%lu", GetLastError());
        Line(e);
        Line("  trying SetParent instead");
        HWND alt = CreateWindowExW(WS_EX_LAYERED, kClass, L"Probe A alt", WS_POPUP,
                                   80, 80, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetParent(alt, target);
        child = alt;
    }
    if (child) {
        SetLayeredWindowAttributes(child, 0, 150, LWA_ALPHA);
        ShowWindow(child, SW_SHOWNA);
        Pump(400);
        RECT cr = {};
        GetWindowRect(child, &cr);
        POINT pt{cr.left + 20, cr.top + 20};
        Line(std::string("  child screen origin = (") + std::to_string(cr.left) + "," + std::to_string(cr.top) +
             ")  (expected near 80,80 -> a mismatch means cross-process DPI weirdness)");
        TestBehavior("Variant A: child of desktop WorkerW/Progman", child, pt);
        DestroyWindow(child);
    }

    Line("");
    Line("cleanup: our own windows destroyed. Any extra WorkerW created by 0x052C is empty and disappears on next Explorer restart.");
    ReportDefView("final");

    fflush(g_log);
    fclose(g_log);
    return 0;
}
