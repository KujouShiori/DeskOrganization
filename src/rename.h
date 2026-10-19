#pragma once

#include <map>
#include <string>
#include <vector>

#include "layout.h"
#include "util.h"

struct FolderRename {
    std::wstring from;  // 现有目录
    std::wstring to;    // 改名后的目录
};

// 盒子改名不是一个字符串赋值：目录名是从盒子名派生的，光改名字会让盒子指向一个
// 新建的空目录，原目录里的文件看起来就"消失"了。所以改名要连带处理
// 派生目录、规则的 box=、多标签的 members= 和当前 tab=。
//
// newFolderForDerived 由调用方算好传入（只有目录是从名字派生时才用得上），
// 这样本文件不必知道 box_root 和桌面回退那套解析规则。
//
// 这个函数是纯的：成功时已把 layout 改好，并把需要重命名的真实目录放进 folderRenames
// （由调用方执行并登记撤销）。任何拒绝情况下 layout 保持原样不动。
bool PlanBoxRename(Layout& layout, const std::wstring& oldName, const std::wstring& newName,
                   const std::map<std::wstring, std::wstring>& resolvedFolders,
                   const std::wstring& newFolderForDerived, std::vector<FolderRename>& folderRenames,
                   std::wstring& err);

// 执行目录改名，并把这一批登记进撤销栈（撤销时顺带把桌面图标放回原位）。
bool ApplyFolderRenames(const std::vector<FolderRename>& renames, HWND owner, std::wstring& err);

// 按大小写无关的方式找盒子；没找到返回 layout.boxes.end()。
size_t FindBoxIndex(const Layout& layout, const std::wstring& name);
