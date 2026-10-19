#include "iconlayer.h"

#include "util.h"

static HWND DefView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    return FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
}

static std::wstring MarkerPath() { return AppFile(L"icons_hidden.marker"); }

bool IconLayerIsHidden() {
    HWND dv = DefView();
    return dv != nullptr && !IsWindowVisible(dv);
}

bool IconLayerSet(bool hide) {
    HWND dv = DefView();
    if (!dv) {
        Log(L"IconLayerSet: 找不到 SHELLDLL_DefView");
        return false;
    }
    ShowWindow(dv, hide ? SW_HIDE : SW_SHOW);
    PumpMessages(150);

    bool achieved = hide ? !IsWindowVisible(dv) : IsWindowVisible(dv);
    Log(L"图标层 请求hide=%d 实际visible=%d 结果=%s", hide ? 1 : 0, IsWindowVisible(dv) ? 1 : 0,
        achieved ? (hide ? L"已隐藏" : L"已显示") : L"未生效");
    if (!achieved) return false;

    if (hide) {
        std::string info = "hidden";
        WriteFileBytes(MarkerPath(), info);
    } else {
        DeleteFileW(MarkerPath().c_str());
    }
    return true;
}

bool IconLayerSelfHeal() {
    if (GetFileAttributesW(MarkerPath().c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    Log(L"检测到上次遗留的 icons_hidden 标记，恢复桌面图标");
    IconLayerSet(false);
    return true;
}
