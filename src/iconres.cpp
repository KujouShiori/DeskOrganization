#include "iconres.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// 32 位 DIB 在内存里是 B,G,R,A 四字节，小端读成一个 uint32 就是 0xAARRGGBB。
constexpr uint32_t PackRgb(unsigned rgb, unsigned alpha) {
    const unsigned r = (rgb >> 16) & 0xFFu;
    const unsigned g = (rgb >> 8) & 0xFFu;
    const unsigned b = rgb & 0xFFu;
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

// 圆角矩形内部判定：直边部分直接过，四个角按圆心距离判定。
bool InRounded(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    const int right = w - 1 - x;
    const int bottom = h - 1 - y;
    if (x >= r && right >= r) return true;
    if (y >= r && bottom >= r) return true;
    const int dx = x < r ? r - x : (right < r ? r - right : 0);
    const int dy = y < r ? r - y : (bottom < r ? r - bottom : 0);
    if (dx == 0 || dy == 0) return true;
    return dx * dx + dy * dy <= r * r + r;
}

}  // namespace

void BuildBoxIconPixels(const Palette& palette, int size, std::vector<uint32_t>& argb, std::vector<uint8_t>& mask) {
    argb.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0u);
    const size_t strideBytes = (static_cast<size_t>(size) + 7) / 8;
    // 默认全透明：只有画到的地方把掩码位清成 0。
    mask.assign(strideBytes * static_cast<size_t>(size), 0xFFu);
    if (size <= 0) return;

    const int radius = std::max(1, size / 5);
    const int bandH = std::max(3, size / 4);
    const uint32_t body = PackRgb(palette.body, 255u);
    const uint32_t title = PackRgb(palette.title, 255u);
    const uint32_t edge = PackRgb(palette.edge, 255u);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!InRounded(x, y, size, size, radius)) continue;

            const bool border = (x == 0 || y == 0 || x == size - 1 || y == size - 1);
            uint32_t color = (y == bandH || y == bandH - 1) ? edge : (y < bandH ? title : body);
            if (border) color = edge;

            argb[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] = color;
            // 掩码位 1 表示透明，画到的地方清 0。位序 MSB 在前。
            const size_t byteIndex = static_cast<size_t>(y) * strideBytes + static_cast<size_t>(x / 8);
            mask[byteIndex] = static_cast<uint8_t>(mask[byteIndex] & ~(1u << (7 - (x % 8))));
        }
    }
}

HICON CreateBoxIcon(const Palette& palette, int size) {
    if (size <= 1) return nullptr;
    std::vector<uint32_t> argb;
    std::vector<uint8_t> mask;
    BuildBoxIconPixels(palette, size, argb, mask);

    BITMAPINFO ciBmi = {};
    ciBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    ciBmi.bmiHeader.biWidth = size;
    // 颜色位图用自下而上，CreateIconIndirect 期望的是 DIB 常规方向。
    ciBmi.bmiHeader.biHeight = size;
    ciBmi.bmiHeader.biPlanes = 1;
    ciBmi.bmiHeader.biBitCount = 32;
    ciBmi.bmiHeader.biCompression = BI_RGB;

    void* colorBits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(nullptr, &ciBmi, DIB_RGB_COLORS, &colorBits, nullptr, 0);
    if (!colorBmp || !colorBits) {
        if (colorBmp) DeleteObject(colorBmp);
        return nullptr;
    }

    auto* dst = static_cast<uint32_t*>(colorBits);
    for (int y = 0; y < size; ++y) {
        const int srcRow = size - 1 - y;
        for (int x = 0; x < size; ++x) {
            dst[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
                argb[static_cast<size_t>(srcRow) * static_cast<size_t>(size) + static_cast<size_t>(x)];
        }
    }

    BITMAPINFO miBmi = {};
    miBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    miBmi.bmiHeader.biWidth = size;
    miBmi.bmiHeader.biHeight = size;
    miBmi.bmiHeader.biPlanes = 1;
    miBmi.bmiHeader.biBitCount = 1;
    miBmi.bmiHeader.biCompression = BI_RGB;
    void* maskBits = nullptr;
    HBITMAP maskBmp = CreateDIBSection(nullptr, &miBmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (!maskBmp || !maskBits) {
        DeleteObject(colorBmp);
        if (maskBmp) DeleteObject(maskBmp);
        return nullptr;
    }
    // 单色位图每行按 32 位对齐，和掩码缓冲的行宽可能不同，逐行搬。
    const int dstStrideBytes = ((size + 31) / 32) * 4;
    const size_t srcStrideBytes = (static_cast<size_t>(size) + 7) / 8;
    auto* dstMask = static_cast<uint8_t*>(maskBits);
    memset(dstMask, 0, static_cast<size_t>(dstStrideBytes) * static_cast<size_t>(size));
    for (int y = 0; y < size; ++y) {
        const int srcRow = size - 1 - y;
        for (int x = 0; x < size; ++x) {
            const bool transparent =
                (mask[static_cast<size_t>(srcRow) * srcStrideBytes + x / 8] >> (7 - (x % 8))) & 1u;
            if (!transparent) continue;
            dstMask[static_cast<size_t>(y) * dstStrideBytes + x / 8] =
                static_cast<uint8_t>(dstMask[static_cast<size_t>(y) * dstStrideBytes + x / 8] | (1u << (7 - (x % 8))));
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}
