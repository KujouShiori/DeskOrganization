#pragma once

#include <string>
#include <vector>

#include "iconview.h"
#include "util.h"

// 撤销之后这段时间内，自动整理不再碰被放回来的文件。
inline constexpr DWORD kUndoHoldMs = 60000;

struct MovedEntry {
    std::wstring from;
    std::wstring to;
};

std::wstring FileNameOf(const std::wstring& path);
std::wstring ParentOf(const std::wstring& path);

// 自己先算好不冲突的名字再交给 Shell，这样撤销记录里的目标路径是确定的。
std::wstring PickFreeName(const std::wstring& destDir, const std::wstring& fileName,
                          const std::vector<std::wstring>& reserved = std::vector<std::wstring>());

bool MoveFilesTo(const std::vector<std::wstring>& sources, const std::wstring& destDir, HWND owner,
                 std::vector<MovedEntry>& outMoves, std::wstring& errorText);

// 原地改名。不能拿 MoveFilesTo 代替：它会跳过"源已经在目标目录里"的项，
// 而改名恰好就是这种情况。
bool RenamePath(const std::wstring& path, const std::wstring& newName, HWND owner, MovedEntry& outMove,
                std::wstring& errorText);
bool UndoMoves(const std::vector<MovedEntry>& moves, HWND owner, std::wstring& errorText);

// 撤销栈：自己实现。系统撤销栈（IUndoManager）不在这套 SDK 的公开头文件里，驱动不了。
void UndoHistoryAdd(std::vector<MovedEntry>&& moves, const std::wstring& label,
                    std::vector<IconPosition> positionsBefore = std::vector<IconPosition>());
bool UndoHistoryLastLabel(std::wstring& outLabel);
bool UndoHistoryUndoLast(HWND owner, std::wstring& errorText);
size_t UndoHistorySize();

// 撤销栈落盘：重启程序之后还能撤销上一轮自动整理。
bool UndoHistorySave();
void UndoHistoryLoad();
void UndoHistoryClear();

// 走 Shell 删除。DeleteFileW 不会通知 Explorer，删掉桌面文件时会短暂留一个幽灵图标。
bool DeleteFiles(const std::vector<std::wstring>& paths, HWND owner, std::wstring& errorText);
