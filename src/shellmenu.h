#pragma once

#include <string>

#include "util.h"

// 桌面右键菜单里的「撤销上一次整理」。
// 用的是桌面静态动词（HKCU\Software\Classes\DesktopBackground\Shell），不是 COM 扩展：
// 不需要管理员权限，卸载就是删掉那一个键。代价是它只能"启动一个命令行"，
// 而撤销栈已经落盘，所以由新进程执行撤销是可行的。
namespace shellmenu {

// 供拼命令行的 exe 路径；取不到时返回空串。
std::wstring ExePath();

// 形如 "C:\path\DeskOrganization.exe" --undo，路径含空格也安全。
std::wstring UndoCommandLine();

std::wstring VerbKeyPath();

bool Install(std::wstring& err);
bool Uninstall(std::wstring& err);

// 已安装时返回 true，并把 command 的内容带回来，用来核对指向的是不是当前这个 exe。
bool IsInstalled(std::wstring& installedCmd);

}  // namespace shellmenu
