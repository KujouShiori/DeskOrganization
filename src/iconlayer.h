#pragma once

#include <windows.h>

// 隐藏/恢复原生桌面图标层（SHELLDLL_DefView）。
// 隐藏期间会留一个标记文件：进程被强杀时来不及清理，下次启动靠它把图标恢复回来。
bool IconLayerSet(bool hide);
bool IconLayerIsHidden();
bool IconLayerSelfHeal();
