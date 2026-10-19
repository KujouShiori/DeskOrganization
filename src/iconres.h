#pragma once

#include <cstdint>
#include <vector>
#include <windows.h>

#include "theme.h"
#include "util.h"

// 托盘图标运行时按配色生成，不依赖仓库里的 .ico 资源，也能跟着深浅色一起换。

// 先算像素：argb 是每行 size 个 32 位像素（0xAARRGGBB，自上而下），
// mask 是 CreateIcon 用的 AND 掩码，每行按 32 位对齐、1 表示透明。
// 单独拆出来是为了能直接断言画对了没有，而不是只断言句柄非空。
void BuildBoxIconPixels(const Palette& palette, int size, std::vector<uint32_t>& argb, std::vector<uint8_t>& mask);

// 失败时返回 nullptr，调用方退回系统默认图标。
HICON CreateBoxIcon(const Palette& palette, int size);
