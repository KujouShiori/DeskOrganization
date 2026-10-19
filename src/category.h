#pragma once

#include <string>
#include <vector>

#include "util.h"

// 盒内二级分类：只是盒子的显示分组，不移动任何文件。
// 硬性不变量：任何文件都恰好落在一个分类里，所以分类页永远能显示全部内容 ——
// 否则没被覆盖到的文件会在盒子里"消失"，那比不分分类更糟。
namespace category {

// 固定顺序，「其他」必须在最后且吞掉一切未覆盖的项。
const std::vector<std::wstring>& Names();

// 返回某个条目所属分类名（一定非空）。isDirectory 时目录归到「文件夹」。
std::wstring CategoryOf(const std::wstring& itemName, bool isDirectory);

bool IsKnown(const std::wstring& name);

}  // namespace category
