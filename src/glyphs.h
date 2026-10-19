#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

#include "theme.h"

// 菜单项左侧的小图形。和托盘图标、文件图标一样是运行时按配色现画的位图，
// 仓库里不放二进制资源；画在 16x16 的设计格上再按比例放大，所以 DPI 变化不用改代码。

enum class Glyph {
    None,
    NewBox,       // 盒子 + 加号
    ShowBoxes,    // 眼睛
    HideBoxes,    // 眼睛 + 斜线
    SaveNow,      // 向下箭头落到底条
    ShowIcons,    // 2x2 点阵
    HideIcons,    // 点阵 + 斜线
    Undo,         // 回退箭头
    RunRules,     // 清单 + 向右箭头
    SweepRules,   // 扫帚
    Backup,       // 两个错开的框
    Restore,      // 向上箭头从底条升起
    ShellMenuOn,  // 方框 + 指针
    ShellMenuOff, // 同上 + 斜线
    Quit,         // 叉
    Collapse,     // 上尖角
    Expand,       // 下尖角
    Rename,       // 铅笔
    Merge,        // 标签卡
    OpenFolder,   // 文件夹
    Category,     // 三行清单
    Ungroup,      // 三行清单 + 斜线
    Delete,       // 叉
    BoxItem,      // 盒子轮廓
};

// argb 是 size*size 个 0xAARRGGBB，自上而下；没画到的地方 alpha=0。
// 颜色只用一个墨色（palette.text），和菜单文字同色，深浅色切换时一起变。
void BuildGlyphPixels(Glyph glyph, int size, unsigned inkRgb, std::vector<uint32_t>& argb);

// 直接把图形画到任何 DC 上（菜单 DC、自检用的内存 DC 都行）。只写有墨的像素，
// 用 SetPixelV 逐个点亮，省掉位图句柄、混合模式和 Alpha 通道这些麻烦：
// 16x16 一共两百多个点，重绘一次菜单的开销可以忽略。
void PaintGlyphOnHdc(HDC dc, Glyph glyph, unsigned inkRgb, int x, int y, int size);
