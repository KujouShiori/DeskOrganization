#pragma once

#include <vector>
#include <windows.h>

#include "util.h"

// 改分辨率、拔掉一台显示器之后，存在 layout.ini 里的盒子坐标可能落在屏幕外，
// 盒子就再也点不到了。这里把它拉回最近的显示器。
namespace geometry {

// 真实显示器的可用区（去掉任务栏），按虚拟屏幕坐标。
std::vector<RECT> MonitorWorkAreas();

// 纯函数：保证盒子至少有一小块留在某个显示器内，尺寸不变。
// minVisibleW / minVisibleH 是"至少让人抓得住"的那一块标题栏尺寸。
RECT ClampIntoMonitors(const RECT& box, const std::vector<RECT>& workAreas, int minVisibleW, int minVisibleH);

// 需要动就返回 true。
bool NeedsClamp(const RECT& box, const std::vector<RECT>& workAreas, int minVisibleW, int minVisibleH);

}  // namespace geometry
