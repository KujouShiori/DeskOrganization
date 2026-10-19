// Show Desktop 影响测试：Win+D 会不会把我们的盒子一起最小化。
// 兜底：先记录用户本来就最小化着的窗口，MinimizeAll + UndoMinimizeALL 之后把它们重新最小化。
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <initguid.h>
#include <shlguid.h>
#include <oleauto.h>
#include <stdio.h>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static std::vector<HWND> g_wereIconic;

static void Line(const std::string& s) { fputs(s.c_str(), g_log); fputc('\n', g_log); }

static std::string ClassName(HWND h) {
    wchar_t c[256] = {0};
    GetClassNameW(h, c, 255);
    int n = WideCharToMultiByte(CP_UTF8, 0, c, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "null";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, c, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static BOOL CALLBACK SnapEnum(HWND h, LPARAM) {
    if (!IsWindowVisible(h) || !IsIconic(h)) return TRUE;
    RECT rc{};
    GetWindowRect(h, &rc);
    if (rc.right - rc.left <= 0) return TRUE;
    if (ClassName(h) == "DeskOrgCandidate") return TRUE;
    g_wereIconic.push_back(h);
    return TRUE;
}

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void Pump(DWORD ms) {
    MSG m;
    DWORD until = GetTickCount() + ms;
    while (GetTickCount() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(15);
    }
}

static bool IsRed(HWND h, const char* label) {
    if (!h) return false;
    RECT rc{};
    GetWindowRect(h, &rc);
    HDC screen = GetDC(nullptr);
    COLORREF c = GetPixel(screen, rc.left + 40, rc.top + 40);
    ReleaseDC(nullptr, screen);
    bool red = GetRValue(c) > 200 && GetGValue(c) < 60 && GetBValue(c) < 60;
    char b[256];
    snprintf(b, sizeof b, "%-42s visible=%d iconic=%d pixel=(%d,%d,%d) -> %s", label, IsWindowVisible(h) ? 1 : 0,
             IsIconic(h) ? 1 : 0, GetRValue(c), GetGValue(c), GetBValue(c), red ? "VISIBLE" : "NOT ON SCREEN");
    Line(b);
    return red;
}

static IDispatch* g_shell = nullptr;

static bool InvokeShell(const wchar_t* method) {
    DISPID id = 0;
    BSTR name = SysAllocString(method);
    HRESULT hr = g_shell->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id);
    SysFreeString(name);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof b, "GetIDsOfNames(%ls) failed hr=0x%08lX", method, static_cast<unsigned long>(hr));
        Line(b);
        return false;
    }
    DISPPARAMS p = {nullptr, nullptr, 0, 0};
    VARIANT res{};
    EXCEPINFO ei{};
    UINT err = 0;
    hr = g_shell->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &p, &res, &ei, &err);
    VariantClear(&res);
    char b[160];
    snprintf(b, sizeof b, "%ls -> hr=0x%08lX argerr=%u", method, static_cast<unsigned long>(hr), err);
    Line(b);
    return SUCCEEDED(hr);
}

static bool GetShellDispatch() {
    CLSID clsid{};
    HRESULT hr = CLSIDFromProgID(L"Shell.Application", &clsid);
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof b, "CLSIDFromProgID(Shell.Application) hr=0x%08lX", static_cast<unsigned long>(hr));
        Line(b);
        return false;
    }
    for (DWORD ctx : {static_cast<DWORD>(CLSCTX_LOCAL_SERVER), static_cast<DWORD>(CLSCTX_ALL),
                      static_cast<DWORD>(CLSCTX_INPROC_SERVER)}) {
        hr = CoCreateInstance(clsid, nullptr, ctx, IID_IDispatch, reinterpret_cast<void**>(&g_shell));
        char b[192];
        snprintf(b, sizeof b, "CoCreateInstance(Shell.Application, ctx=%lu) hr=0x%08lX", ctx,
                 static_cast<unsigned long>(hr));
        Line(b);
        if (SUCCEEDED(hr) && g_shell) return true;
    }
    return false;
}

static void SendWinD() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 'D';
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = 'D';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    UINT sent = SendInput(4, in, sizeof(INPUT));
    Line("SendInput Win+D accepted=" + std::to_string(sent));
    Pump(1200);
}

static int CountShownTopLevels() {
    struct Ctx { int n; } ctx{0};
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
        RECT rc{};
        GetWindowRect(h, &rc);
        if (rc.right - rc.left < 40 || rc.bottom - rc.top < 20) return TRUE;
        reinterpret_cast<Ctx*>(l)->n++;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.n;
}

static HWND MakeCandidate(const char* label, DWORD ex, int x) {
    HWND h = CreateWindowExW(ex, L"DeskOrgCandidate", L"candidate", WS_POPUP, x, 60, 300, 200, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Pump(250);
    char b[128];
    snprintf(b, sizeof b, "-- created %s hwnd=%p", label, static_cast<void*>(h));
    Line(b);
    return h;
}

int main() {
    g_log = fopen("probe_showdesktop.txt", "wb");
    if (!g_log) return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DeskOrgCandidate";
    RegisterClassExW(&wc);

    HWND app = MakeCandidate("app style (TOOLWINDOW|NOACTIVATE)", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, 60);
    HWND plain = MakeCandidate("plain style (no ex)", 0, 420);

    // 已知健康对照：一个再普通不过的应用窗口，Show Desktop 必须把它最小化。
    // 如果连它都不动，说明 MinimizeAll 在本测试里是空操作，上面两个"幸存"不作数。
    HWND control = CreateWindowExW(0, L"DeskOrgCandidate", L"control", WS_OVERLAPPEDWINDOW, 780, 60, 300, 200,
                                   nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(control, SW_SHOW);
    Pump(300);
    Line("-- created CONTROL WS_OVERLAPPEDWINDOW hwnd=" + [&] {
        char b[32];
        snprintf(b, sizeof b, "%p", static_cast<void*>(control));
        return std::string(b);
    }());
    IsRed(control, "CONTROL overlapped window");

    Line("");
    Line("before MinimizeAll:");
    bool appRedBefore = IsRed(app, "app-style candidate");
    bool plainRedBefore = IsRed(plain, "plain candidate");
    IsRed(control, "CONTROL overlapped window");
    int shownBefore = CountShownTopLevels();
    Line("visible non-iconic top-level windows before: " + std::to_string(shownBefore));

    EnumWindows(SnapEnum, 0);
    Line("");
    Line("user windows already minimized before the test: " + std::to_string(g_wereIconic.size()));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        char b[96];
        snprintf(b, sizeof b, "CoInitializeEx failed hr=0x%08lX", static_cast<unsigned long>(hr));
        Line(b);
        fflush(g_log);
        fclose(g_log);
        return 1;
    }
    bool viaCom = GetShellDispatch();

    Line("");
    Line(std::string("=== trigger Show Desktop via ") + (viaCom ? "Shell.Application MinimizeAll" : "SendInput Win+D") + " ===");
    if (viaCom) {
        InvokeShell(L"MinimizeAll");
        Pump(1200);
    } else {
        SendWinD();
    }
    bool appRedAfter = IsRed(app, "app-style candidate");
    bool plainRedAfter = IsRed(plain, "plain candidate");
    bool controlMinimized = IsIconic(control);
    IsRed(control, "CONTROL after MinimizeAll");
    int shownAfter = CountShownTopLevels();
    Line("visible non-iconic top-level windows after: " + std::to_string(shownAfter));
    Line("MinimizeAll actually did something (control minimized or count dropped): " +
         std::string(controlMinimized || shownAfter < shownBefore ? "YES" : "NO - TEST INVALID"));

    Line("");
    Line(std::string("=== undo, via ") + (viaCom ? "UndoMinimizeALL" : "Win+D again") + " ===");
    if (viaCom) {
        InvokeShell(L"UndoMinimizeALL");
        Pump(1500);
    } else {
        SendWinD();
    }
    IsRed(app, "app-style after undo");
    IsRed(plain, "plain after undo");

    // 把用户本来最小化着的窗口重新最小化回去。
    int restored = 0;
    for (HWND h : g_wereIconic) {
        if (!IsWindow(h)) continue;
        ShowWindow(h, SW_MINIMIZE);
        restored++;
    }
    Pump(600);
    Line("re-minimized windows that were already iconic: " + std::to_string(restored) + "/" +
         std::to_string(g_wereIconic.size()));

    Line("");
    Line("verdict: app-style box survives Show Desktop = " + std::string(appRedAfter ? "YES" : "NO") +
         ", plain-style box survives = " + std::string(plainRedAfter ? "YES" : "NO"));
    Line("baseline sanity (both were visible before): " + std::string(appRedBefore && plainRedBefore ? "YES" : "NO"));

    ShowWindow(control, SW_MINIMIZE);
    Pump(200);
    DestroyWindow(control);
    DestroyWindow(app);
    DestroyWindow(plain);
    if (g_shell) g_shell->Release();
    CoUninitialize();

    fflush(g_log);
    fclose(g_log);
    return 0;
}
