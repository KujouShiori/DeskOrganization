// 决定性探针：盒子必须收到鼠标左键按下。上一轮 WS_CHILD 版收到 move 却没收到 down，
// 这里同时测三种窗口形态，并用绝对坐标注入点击消除 SetCursorPos 竞态。
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
static int g_activated = 0;

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

static std::string Title(HWND h) {
    if (!h) return "null";
    int len = GetWindowTextLengthW(h);
    wchar_t t[512] = {0};
    GetWindowTextW(h, t, 511);
    return "len=" + std::to_string(len) + " text=\"" + U8(t) + "\"";
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_LBUTTONDOWN) g_clicks++;
    if (msg == WM_MOUSEMOVE) g_moves++;
    if (msg == WM_ACTIVATE || msg == WM_MOUSEACTIVATE) g_activated++;
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

static void ClickAt(POINT pt) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    INPUT mv = {};
    mv.type = INPUT_MOUSE;
    mv.mi.dx = static_cast<LONG>(pt.x * 65535 / (sw - 1));
    mv.mi.dy = static_cast<LONG>(pt.y * 65535 / (sh - 1));
    mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &mv, sizeof(INPUT));
    Pump(200);

    INPUT btn[2] = {};
    btn[0].type = INPUT_MOUSE;
    btn[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    btn[1].type = INPUT_MOUSE;
    btn[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, btn, sizeof(INPUT));
    Pump(400);
}

static void RunVariant(const char* label, HWND progman, DWORD style, DWORD exStyle, bool asChild) {
    Line("");
    Line(std::string("=== ") + label + " ===");
    static int xOff = 60;
    int x = xOff;
    xOff += 360;

    HWND w = CreateWindowExW(exStyle, L"DeskOrgProbe4", L"Probe4 box title", style,
                              x, 80, 320, 200, asChild ? progman : nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!w) {
        char e[128];
        snprintf(e, sizeof e, "  CreateWindowExW FAILED gle=%lu", GetLastError());
        Line(e);
        return;
    }
    Line("  created: " + Title(w));
    if (!asChild) ShowWindow(w, SW_SHOW);
    SetWindowPos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (asChild ? SWP_NOACTIVATE : 0));
    Pump(300);

    RECT rc = {};
    GetWindowRect(w, &rc);
    POINT pt{rc.left + 40, rc.top + 40};
    Line(std::string("  rect=[") + std::to_string(rc.left) + "," + std::to_string(rc.top) + " " +
         std::to_string(rc.right - rc.left) + "x" + std::to_string(rc.bottom - rc.top) + "]");
    HWND hit = WindowFromPoint(pt);
    Line("  WindowFromPoint = " + ClassName(hit) + " title=" + Title(hit));
    if (hit != w) {
        Line("  skipped click: that pixel is not ours");
        DestroyWindow(w);
        return;
    }

    g_clicks = g_moves = g_activated = 0;
    POINT saved{};
    GetCursorPos(&saved);
    ClickAt(pt);
    Line(std::string("  after click: clicks=") + std::to_string(g_clicks) +
         " moves=" + std::to_string(g_moves) + " activateMsgs=" + std::to_string(g_activated) +
         " foreground=" + ClassName(GetForegroundWindow()));
    Verdict("receives WM_LBUTTONDOWN", g_clicks > 0);
    SetCursorPos(saved.x, saved.y);
    DestroyWindow(w);
    Pump(100);
}

int main() {
    g_log = fopen("probe_click.txt", "wb");
    if (!g_log) return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 先判定标题异常：字面量本身在运行时的长度，以及注册类时的名字。
    Line(std::string("runtime wcslen(L\"Probe4 box title\") = ") + std::to_string(wcslen(L"Probe4 box title")));
    Line("runtime U8 of that literal = \"" + U8(L"Probe4 box title") + "\"");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DeskOrgProbe4";
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    HWND progman = FindWindowW(L"Progman", nullptr);
    Line("progman = " + ClassName(progman) + " " + Title(progman));

    RunVariant("Control: ordinary top-level window (must work)", progman, WS_OVERLAPPEDWINDOW, 0, false);
    RunVariant("Variant A: WS_CHILD parented into Progman", progman, WS_CHILD | WS_VISIBLE, 0, true);
    RunVariant("Variant B: WS_POPUP parented into Progman", progman, WS_POPUP, 0, true);
    RunVariant("Variant C: WS_CHILD in Progman, WS_EX_NOACTIVATE", progman, WS_CHILD | WS_VISIBLE, WS_EX_NOACTIVATE, true);

    fflush(g_log);
    fclose(g_log);
    return 0;
}
