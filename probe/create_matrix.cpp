// 建窗参数矩阵：找出哪一种组合在 Progman 下建不出来。
// 第一行是 input_probe 里已经成功过的写法，作为已知健康对照。
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

static FILE* g_log = nullptr;

static void Line(const std::string& s) { fputs(s.c_str(), g_log); fputc('\n', g_log); }

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, w, l);
    }
    return DefWindowProcW(h, msg, w, l);
}

struct Case {
    const char* label;
    DWORD ex;
    const wchar_t* title;
    DWORD style;
    HMENU menu;
};

int main() {
    g_log = fopen("probe_matrix.txt", "wb");
    if (!g_log) return 1;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MatrixBox";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    Line(std::string("class registered with hbrBackground=nullptr, CS_DBLCLKS: ") +
         (RegisterClassExW(&wc) ? "ok" : "FAILED"));

    WNDCLASSEXW wc2 = wc;
    wc2.lpszClassName = L"MatrixBoxBrush";
    wc2.style = 0;
    wc2.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    Line(std::string("class registered with a brush, no CS_DBLCLKS: ") +
         (RegisterClassExW(&wc2) ? "ok" : "FAILED"));

    HWND progman = FindWindowW(L"Progman", nullptr);
    char pb[128];
    snprintf(pb, sizeof pb, "progman=%p visible=%d", static_cast<void*>(progman), IsWindowVisible(progman));
    Line(pb);

    std::vector<Case> cases = {
        {"control: exactly what input_probe did", 0, L"Probe3 box", WS_CHILD | WS_VISIBLE, nullptr},
        {"WS_CHILD only, empty title", 0, L"", WS_CHILD, nullptr},
        {"WS_CHILD, non-empty title", 0, L"x", WS_CHILD, nullptr},
        {"WS_CHILD|WS_VISIBLE, non-empty title", 0, L"x", WS_CHILD | WS_VISIBLE, nullptr},
        {"WS_EX_LAYERED + WS_CHILD|WS_VISIBLE", WS_EX_LAYERED, L"x", WS_CHILD | WS_VISIBLE, nullptr},
        {"WS_EX_LAYERED + WS_CHILD only", WS_EX_LAYERED, L"", WS_CHILD, nullptr},
        {"WS_EX_LAYERED + WS_CHILD, empty title", WS_EX_LAYERED, L"", WS_CHILD, nullptr},
        {"control style + hMenu=(HMENU)1", 0, L"Probe3 box", WS_CHILD | WS_VISIBLE, reinterpret_cast<HMENU>(static_cast<intptr_t>(1))},
        {"WS_EX_LAYERED + WS_POPUP (expected to differ)", WS_EX_LAYERED, L"x", WS_POPUP, nullptr},
    };

    static int marker = 0;
    for (auto& c : cases) {
        SetLastError(0);
        HWND h = CreateWindowExW(c.ex, L"MatrixBox", c.title, c.style, 100, 120, 300, 200,
                                 progman, c.menu, GetModuleHandleW(nullptr),
                                 reinterpret_cast<void*>(static_cast<intptr_t>(++marker)));
        DWORD gle = GetLastError();
        char buf[512];
        snprintf(buf, sizeof buf, "%-46s -> hwnd=%p gle=%lu", c.label, static_cast<void*>(h), gle);
        Line(buf);
        if (h) {
            RECT rc{};
            GetWindowRect(h, &rc);
            char b2[256];
            snprintf(b2, sizeof b2, "     then: parent=%p visible=%d rect=[%ld,%ld %ldx%ld] userdata=%s",
                     static_cast<void*>(GetParent(h)), IsWindowVisible(h) ? 1 : 0, rc.left, rc.top,
                     rc.right - rc.left, rc.bottom - rc.top,
                     GetWindowLongPtrW(h, GWLP_USERDATA) ? "set" : "MISSING");
            Line(b2);
            SetLastError(0);
            BOOL r = SetLayeredWindowAttributes(h, RGB(255, 0, 255), 235, LWA_COLORKEY | LWA_ALPHA);
            snprintf(b2, sizeof b2, "     SetLayeredWindowAttributes -> %d gle=%lu", r, GetLastError());
            Line(b2);
            DestroyWindow(h);
        }
    }

    SetLastError(0);
    HWND orphan = CreateWindowExW(WS_EX_LAYERED, L"MatrixBox", L"x", WS_POPUP, 100, 120, 300, 200,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    char ob[256];
    snprintf(ob, sizeof ob, "no-parent layered popup -> hwnd=%p gle=%lu", static_cast<void*>(orphan), GetLastError());
    Line(ob);
    if (orphan) {
        SetLastError(0);
        HWND r = SetParent(orphan, progman);
        snprintf(ob, sizeof ob, "     SetParent into Progman -> %p gle=%lu parent=%p",
                 static_cast<void*>(r), GetLastError(), static_cast<void*>(GetParent(orphan)));
        Line(ob);
        DestroyWindow(orphan);
    }

    fflush(g_log);
    fclose(g_log);
    return 0;
}
