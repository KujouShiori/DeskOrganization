#include <ole2.h>

#include "app.h"
#include "util.h"

static HANDLE g_singleInstance = nullptr;
static bool g_oleInitialized = false;

static void InitCom() {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        Log(L"设置 Per-Monitor V2 DPI 失败 gle=%lu", GetLastError());
    }
    // 拖放和 IFileOperation 都需要 OleInitialize（内部会按 APARTMENTTHREADED 初始化套间）。
    if (FAILED(OleInitialize(nullptr))) {
        Log(L"OleInitialize 失败，拖放不可用");
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }
    g_oleInitialized = true;
}

static void ShutdownCom() {
    if (g_oleInitialized) OleUninitialize();
    else CoUninitialize();
    if (g_singleInstance) {
        CloseHandle(g_singleInstance);
        g_singleInstance = nullptr;
    }
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdline, int) {
    const bool selftest = cmdline && wcsstr(cmdline, L"--selftest") != nullptr;
    const bool layouttest = cmdline && wcsstr(cmdline, L"--layouttest") != nullptr;
    const bool undoOnly = cmdline && wcsstr(cmdline, L"--undo") != nullptr;

    // --undo 是桌面右键菜单那条路。它必须在拿单实例锁之前处理：
    // 平时界面实例一直在跑，若先拿锁就会被"已有实例"直接退出，撤销永远轮不到执行。
    // 有实例在跑时这条命令只是转一个消息过去，不会和它抢着写撤销文件。
    if (undoOnly && !selftest && !layouttest) {
        InitCom();
        int result = AppRunUndo();
        ShutdownCom();
        return result;
    }

    g_singleInstance = CreateMutexW(nullptr, TRUE, L"DeskOrganization.single.instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"已有实例在运行，本进程退出");
        return 0;
    }

    InitCom();

    int result = 0;
    if (selftest) {
        result = AppSelfTest();
    } else if (layouttest) {
        result = AppLayoutTest();
    } else {
        App app;
        if (app.Init()) app.Run();
        else result = 1;
        app.Shutdown();
    }

    ShutdownCom();
    return result;
}
