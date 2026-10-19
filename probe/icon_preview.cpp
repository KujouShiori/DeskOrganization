// 把自绘托盘图标导出成放大预览图，用来人工判断像素画得对不对。
// 纯输出，不改任何系统状态。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "iconres.h"
#include "theme.h"
#include "util.h"

static bool WriteBmp32(const std::wstring& path, const std::vector<uint32_t>& pixels, int w, int h) {
    const DWORD imageSize = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    BITMAPFILEHEADER bf = {};
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + imageSize;
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h;  // 正数表示自下而上
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imageSize;

    std::vector<uint32_t> bottomUp(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bottomUp[static_cast<size_t>(y) * w + x] =
                pixels[static_cast<size_t>(h - 1 - y) * w + x];
        }
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, &bf, sizeof(bf), &written, nullptr);
    WriteFile(f, &bi, sizeof(bi), &written, nullptr);
    WriteFile(f, bottomUp.data(), imageSize, &written, nullptr);
    CloseHandle(f);
    return true;
}

int main() {
    const int src = 32;
    const int scale = 8;
    const int w = src * scale * 2 + scale * 8;  // 两张并排，中间留一段空隙
    const int h = src * scale;

    std::vector<uint32_t> dark, light;
    std::vector<uint8_t> mask;
    BuildBoxIconPixels(DarkPalette(), src, dark, mask);
    BuildBoxIconPixels(LightPalette(), src, light, mask);

    // 把透明角画成棋盘格，好一眼看出"这里到底是透明还是画了颜色"。
    auto toVisible = [&](const std::vector<uint32_t>& argb) {
        std::vector<uint32_t> out(static_cast<size_t>(w) * static_cast<size_t>(h), 0xFF202020u);
        for (int block = 0; block < 2; ++block) {
            const auto& srcPix = block == 0 ? argb : light;
            const int originX = block * (src * scale + scale * 8);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < src * scale; ++x) {
                    const uint32_t p = srcPix[static_cast<size_t>(y / scale) * src + static_cast<size_t>(x / scale)];
                    const bool transparent = (p >> 24) == 0;
                    uint32_t color = p;
                    if (transparent) {
                        const bool checker = (((x / scale) + (y / scale)) % 2) == 0;
                        color = checker ? 0xFF808080u : 0xFF404040u;
                    }
                    out[static_cast<size_t>(y) * w + originX + x] = color;
                }
            }
        }
        return out;
    };

    const std::wstring out = L"out";
    CreateDirectoryW(out.c_str(), nullptr);
    if (!WriteBmp32(out + L"\\icon-preview.bmp", toVisible(dark), w, h)) {
        OutputDebugStringA("write failed\n");
        return 1;
    }
    wprintf(L"written: %s\\icon-preview.bmp  (%dx%d, 左边深色配色 右边浅色配色, 各放大 %d 倍)\n",
            AppDir().empty() ? L"out" : out.c_str(), w, h, scale);
    return 0;
}
