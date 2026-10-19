// 对照实验：同样是 Progman 的 WS_CHILD 窗口，GDI 画的和 Direct2D 画的，屏幕上到底看得见哪个。
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <stdio.h>
#include <string>

static FILE* g_log = nullptr;
static ID2D1Factory* g_factory = nullptr;
static ID2D1HwndRenderTarget* g_rt = nullptr;
static ID2D1SolidColorBrush* g_d2dbrush = nullptr;
static bool g_useD2D = false;

static void Line(const std::string& s) { fputs(s.c_str(), g_log); fputc('\n', g_log); }

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        if (g_useD2D) {
            if (!g_rt) {
                UINT dpi = GetDpiForWindow(h);
                D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                  reinterpret_cast<void**>(&g_factory));
                g_factory->CreateHwndRenderTarget(
                    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                 (FLOAT)dpi, (FLOAT)dpi),
                    D2D1::HwndRenderTargetProperties(h, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
                D2D1_COLOR_F c = {1.0f, 0.0f, 0.0f, 1.0f};
                g_rt->CreateSolidColorBrush(c, &g_d2dbrush);
            }
            if (g_rt) {
                g_rt->BeginDraw();
                g_rt->Clear(D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f));
                g_rt->EndDraw();
            }
        } else {
            HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
            FillRect(dc, &rc, red);
            DeleteObject(red);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static void SampleAndReport(const char* label, HWND h) {
    RECT rc{};
    GetWindowRect(h, &rc);
    HDC screen = GetDC(nullptr);
    COLORREF c = GetPixel(screen, rc.left + 60, rc.top + 60);
    ReleaseDC(nullptr, screen);
    char b[256];
    snprintf(b, sizeof b, "%-22s window=[%ld,%ld %ldx%ld] center pixel RGB=(%d,%d,%d)", label, rc.left, rc.top,
             rc.right - rc.left, rc.bottom - rc.top, GetRValue(c), GetGValue(c), GetBValue(c));
    Line(b);
}

int main() {
    g_log = fopen("probe_visible.txt", "wb");
    if (!g_log) return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VisibleProbe";
    RegisterClassExW(&wc);

    HWND progman = FindWindowW(L"Progman", nullptr);
    Line(std::string("progman = ") + (progman ? "found" : "NULL"));

    g_useD2D = false;
    HWND gdi = CreateWindowExW(0, L"VisibleProbe", L"gdi", WS_CHILD, 60, 60, 300, 200, progman, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ShowWindow(gdi, SW_SHOWNA);
    UpdateWindow(gdi);
    MSG m;
    for (int i = 0; i < 40; ++i) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(20);
    }
    SampleAndReport("GDI red fill", gdi);

    g_useD2D = true;
    HWND d2d = CreateWindowExW(0, L"VisibleProbe", L"d2d", WS_CHILD, 420, 60, 300, 200, progman, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ShowWindow(d2d, SW_SHOWNA);
    UpdateWindow(d2d);
    for (int i = 0; i < 60; ++i) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(20);
    }
    SampleAndReport("Direct2D green clear", d2d);

    // 已知健康对照：一个普通顶层窗口。如果连它都取不到颜色，说明是 GetPixel 取不到合成画面，
    // 而不是 Progman 子窗口画不出来。
    g_useD2D = false;
    HWND control = CreateWindowExW(0, L"VisibleProbe", L"control", WS_POPUP, 780, 60, 300, 200, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    ShowWindow(control, SW_SHOW);
    UpdateWindow(control);
    for (int i = 0; i < 40; ++i) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(20);
    }
    SampleAndReport("CONTROL top-level red", control);

    // 再用 PrintWindow 直接问窗口自己要画面，绕开桌面合成。
    {
        RECT rc{};
        GetWindowRect(control, &rc);
        HDC mem = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateCompatibleBitmap(GetDC(nullptr), 300, 200);
        HBITMAP old = reinterpret_cast<HBITMAP>(SelectObject(mem, bmp));
        BOOL pw = PrintWindow(control, mem, PW_RENDERFULLCONTENT);
        COLORREF viaPrint = GetPixel(mem, 60, 60);
        char b[200];
        snprintf(b, sizeof b, "PrintWindow on control -> ok=%d pixel RGB=(%d,%d,%d)", pw,
                 GetRValue(viaPrint), GetGValue(viaPrint), GetBValue(viaPrint));
        Line(b);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    DestroyWindow(control);
    DestroyWindow(d2d);
    DestroyWindow(gdi);
    if (g_rt) g_rt->Release();
    if (g_factory) g_factory->Release();

    fflush(g_log);
    fclose(g_log);
    return 0;
}
