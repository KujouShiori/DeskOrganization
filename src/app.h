#pragma once

#include <memory>
#include <vector>

#include "box.h"
#include "layout.h"
#include "messages.h"

class App {
 public:
    bool Init();
    void Run();
    void Shutdown();
    static App* The() { return the_; }
    HWND trayWnd() const { return tray_; }

    enum Command {
        CmdNewBox = 100,
        CmdToggleVisible = 101,
        CmdSaveNow = 102,
        CmdQuit = 103,
        CmdToggleIcons = 104,
        CmdUndo = 105,
        CmdBackupLayout = 106,
        CmdRestoreLayout = 107,
        CmdRunRules = 108,
        CmdSweepNow = 109,
        CmdToggleShellMenu = 110,
    };

 private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    void AddTray();
    void RunRules(bool fullSweep = false);
    void RenameBoxTo(const std::wstring& oldName, const std::wstring& newName);
    std::vector<std::wstring> MemberNames() const;
    std::vector<std::pair<std::wstring, std::wstring>> MemberTargets(const BoxConfig& cfg) const;
    void ApplyMembershipVisibility();
    // 改分辨率或拔掉显示器后，把落到屏幕外的盒子拉回来。
    void ClampBoxesToScreens(const wchar_t* reason);
    std::wstring FolderForBoxName(const std::wstring& name) const;
    void ShowBalloon(const std::wstring& title, const std::wstring& text);
    void RemoveTray();
    void ShowTrayMenu(POINT screenPt);
    void OnCommand(int id);

    bool AttachToDesktop();
    void RebuildBoxes();
    void SyncLayoutFromBoxes();
    void CreateOneBox(BoxConfig cfg);
    BoxConfig NextDefaultBox();
    void OnHeartbeat();
    void DeleteBoxByHwnd(HWND h);

    HWND tray_ = nullptr;
    HICON icon_ = nullptr;
    HWND progman_ = nullptr;
    HWND defView_ = nullptr;
    std::vector<std::unique_ptr<Box>> boxes_;
    Layout layout_{};
    std::wstring boxRoot_;
    bool hidden_ = false;
    UINT_PTR timer_ = 0;
    int tick_ = 0;
    inline static App* the_ = nullptr;
};

// 无界面跑一遍关键断言，结果写 selftest.txt，全部通过返回 0。
int AppSelfTest();

// 会真的移动桌面图标再还原，需要单独触发：--layouttest
int AppLayoutTest();

// 桌面右键菜单的"撤销上一次整理"入口走的进程。
int AppRunUndo();
