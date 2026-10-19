#pragma once

#include <string>
#include <vector>

#include "layout.h"
#include "util.h"

// 把若干盒子合并成一个多标签盒子：members 是逗号分隔的成员盒子名。
// 这里只管布局里的引用关系，不碰磁盘。

// 切换某个成员。成功时已就地改好 layout；失败时 layout 保持不变。
bool ToggleMergeMember(Layout& layout, const std::wstring& boxName, const std::wstring& memberName,
                       std::wstring& err);

// 它已经是这个盒子的成员吗。
bool IsMergeMember(const Layout& layout, const std::wstring& boxName, const std::wstring& memberName);

// 这个盒子当前是不是一个合并出来的标签盒。
bool IsAggregate(const Layout& layout, const std::wstring& boxName);
