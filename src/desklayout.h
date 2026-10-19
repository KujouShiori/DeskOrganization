#pragma once

#include <string>
#include <vector>

#include "util.h"

// 桌面图标布局的备份与还原。
// 备份的是"图标名 -> 坐标"的明文表，而不是注册表里那段不透明的 IconLayouts 字节：
// 实测写 IconLayouts 不生效（Explorer 会用内存缓存覆盖），而坐标是直接可读写的。
bool DesktopLayoutBackup(const std::wstring& file, std::wstring& err);
bool DesktopLayoutRestore(const std::wstring& file, std::wstring& err);

std::wstring BackupsDir();
std::vector<std::wstring> ListBackups();
std::wstring NewestBackup();
std::wstring NewBackupPath();

// 还原前先看一眼文件里有什么，用来把菜单文案写具体。
bool DesktopLayoutPeek(const std::wstring& file, size_t& outCount, std::wstring& err);
