// 关键风险探针：
// 1) 挂在 Progman 下的窗口能不能收到真实鼠标输入（收不到则整条方案作废）。
// 2) 隐藏原生图标层时，我们的盒子会不会被一起藏掉（接管模式必须能只藏图标、留盒子）。
// 隐藏动作无论成功失败都会恢复，并在退出前再恢复一次。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(-4))
#endif

static FILE* g_log = nullptr;
static int g_clicks = 0;
static int g_moves = 0;

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

static std::string ClassName(HWND h) {
    if (!h) return "null";
    wchar_t c[256] = {0};
    GetClassNameW(h, c, 255);
    return U8(c);
}

static std::string TextInfo(HWND h) {
    if (!h) return "null";
    int len = GetWindowTextLengthW(h);
    wchar_t t[512] = {0};
    GetWindowTextW(h, t, 511);
    char b[64];
    snprintf(b, sizeof b, "len=%d -> ", len);
    return b + U8(t);
}

static LRESULT IconCount(HWND lv) {
    LRESULT res = 0;
    SendMessageTimeoutW(lv, LVM_GETITEMCOUNT, 0, 0, SMTO_ABORTIFHUNG, 1000, reinterpret_cast<ULONG_PTR*>(&res));
    return res;
}

// 同进程自己创建的窗口，标题应当能完整读回；用来判定上一个探针里标题只剩一个字符是不是真的。
static HWND g_sameProcessBox = nullptr;

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_LBUTTONDOWN) g_clicks++;
    if (msg == WM_MOUSEMOVE) g_moves++;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HBRUSH br = CreateSolidBrush(RGB(30, 70, 190));
        FillRect(dc, &ps.rcPaint, br);
        DeleteObject(br);
        EndPaint(h, &ps);
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

static void SetIconLayer(HWND target, bool show, const char* label) {
    ShowWindow(target, show ? SW_SHOW : SW_HIDE);
    Pump(250);
    char b[256];
    snprintf(b, sizeof b, "  %s: visible=%d", label, IsWindowVisible(target) ? 1 : 0);
    Line(b);
}

int main() {
    g_log = fopen("probe_input.txt", "wb");
    if (!g_log) return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DeskOrgProbe3";
    RegisterClassW(&wc);

    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND listview = FindWindowExW(defview, nullptr, L"SysListView32", nullptr);

    Line("progman = " + ClassName(progman) + " " + TextInfo(progman));
    Line("defview = " + ClassName(defview) + " visible=" + (IsWindowVisible(defview) ? "1" : "0"));
    Line("listview= " + ClassName(listview) + " " + TextInfo(listview));

    Line(std::string("icon count = ") + std::to_string(IconCount(listview)));

    // --- 1. 真实鼠标输入 ---
    Line("");
    Line("=== test 1: does a Progman child receive real mouse input? ===");
    HWND box = CreateWindowExW(0, L"DeskOrgProbe3", L"Probe3 box", WS_CHILD | WS_VISIBLE,
                               80, 80, 320, 200, progman, nullptr, GetModuleHandleW(nullptr), nullptr);
    Verdict("child window created under Progman", box != nullptr);
    g_sameProcessBox = box;
    Line("  our own same-process window title reads back as: " + TextInfo(box));
    Verdict("same-process window title read back intact", TextInfo(box) == "len=10 -> Probe3 box");
    SetWindowPos(box, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Pump(300);

    RECT rc = {};
    GetWindowRect(box, &rc);
    POINT pt{rc.left + 40, rc.top + 40};
    Line(std::string("  box screen rect = [") + std::to_string(rc.left) + "," + std::to_string(rc.top) + " " +
         std::to_string(rc.right - rc.left) + "x" + std::to_string(rc.bottom - rc.top) + "]");

    HWND hit = WindowFromPoint(pt);
    Line("  WindowFromPoint target = " + ClassName(hit) + " (expect DeskOrgProbe3)");
    bool canHit = hit == box;
    Verdict("we are the hit-test target at that point", canHit);

    if (canHit) {
        POINT saved{};
        GetCursorPos(&saved);
        SetCursorPos(pt.x, pt.y);
        Pump(150);
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        UINT sent = SendInput(2, in, sizeof(INPUT));
        Pump(400);
        char sb[256];
        snprintf(sb, sizeof sb, "  SendInput accepted=%u clicks=%d moves=%d", sent, g_clicks, g_moves);
        Line(sb);
        Verdict("real mouse click reached our Progman-child window", g_clicks > 0);
        SetCursorPos(saved.x, saved.y);
    } else {
        Line("  skipped SendInput: something else owns that pixel, clicking would act on it");
    }

    // --- 2. 藏图标层，看我们是否幸存 ---
    Line("");
    Line("=== test 2: hide the native icon layer, are our boxes still visible? ===");
    bool ourBoxVisibleBefore = IsWindowVisible(box);

    SetIconLayer(defview, false, "hiding SHELLDLL_DefView");
    char av[256];
    snprintf(av, sizeof av, "  our box while icons hidden: visible=%d class=%s",
             IsWindowVisible(box) ? 1 : 0, ClassName(box).c_str());
    Line(av);
    Verdict("our box survives while the icon layer is hidden", IsWindowVisible(box));
    hit = WindowFromPoint(pt);
    Line("  hit-test with icons hidden = " + ClassName(hit));

    SetIconLayer(defview, true, "restoring SHELLDLL_DefView");
    Verdict("icon layer restored", IsWindowVisible(defview));

    Line("");
    Line("=== test 3: hide only the SysListView32 instead ===");
    SetIconLayer(listview, false, "hiding SysListView32");
    SetIconLayer(listview, true, "restoring SysListView32");
    Verdict("listview restored", IsWindowVisible(listview));

    Line(std::string("icon count after restore = ") + std::to_string(IconCount(listview)));
    Verdict("box visibility unchanged by the whole hide/show cycle", IsWindowVisible(box) == ourBoxVisibleBefore);

    if (defview) ShowWindow(defview, SW_SHOW);
    if (listview) ShowWindow(listview, SW_SHOW);
    DestroyWindow(box);
    Pump(150);

    Line("");
    Line(std::string("final state: defview visible=") + (IsWindowVisible(defview) ? "1" : "0") +
         " listview visible=" + (IsWindowVisible(listview) ? "1" : "0"));

    fflush(g_log);
    fclose(g_log);
    return 0;
}
