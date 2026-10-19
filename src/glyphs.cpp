#include "glyphs.h"

#include <cmath>
#include <cstdlib>

namespace {

// 所有图形都画在 16x16 的设计格上（坐标 0..15），每个设计格映射到若干真实像素，
// 所以 16px 和 32px 拿到的是同一个形状，不用为某个 DPI 单独调。
constexpr int kGrid = 16;

struct Canvas {
    int size = kGrid;
    uint32_t* px = nullptr;
    uint32_t ink = 0xFF000000u;

    void cell(int x, int y) {
        if (x < 0 || y < 0 || x >= kGrid || y >= kGrid || !px) return;
        const int x0 = x * size / kGrid;
        const int x1 = (x + 1) * size / kGrid;
        const int y0 = y * size / kGrid;
        const int y1 = (y + 1) * size / kGrid;
        for (int py = y0; py < y1; ++py) {
            for (int pxx = x0; pxx < x1; ++pxx) px[py * size + pxx] = ink;
        }
    }
    void hline(int x0, int x1, int y) {
        for (int x = x0; x <= x1; ++x) cell(x, y);
    }
    void vline(int x, int y0, int y1) {
        for (int y = y0; y <= y1; ++y) cell(x, y);
    }
    void frame(int x0, int y0, int x1, int y1) {
        hline(x0, x1, y0);
        hline(x0, x1, y1);
        vline(x0, y0, y1);
        vline(x1, y0, y1);
    }
    void fill(int x0, int y0, int x1, int y1) {
        for (int y = y0; y <= y1; ++y) hline(x0, x1, y);
    }
    void line(int xa, int ya, int xb, int yb) {
        int dx = abs(xb - xa), dy = -abs(yb - ya);
        int sx = xa < xb ? 1 : -1, sy = ya < yb ? 1 : -1;
        int err = dx + dy;
        int x = xa, y = ya;
        for (;;) {
            cell(x, y);
            if (x == xb && y == yb) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    }
    // 空心椭圆轮廓：用归一化半径的带宽判定，保证在任何尺寸下都是一圈而不是糊成一团。
    void ring(double cx, double cy, double rx, double ry) {
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                const double nx = (x + 0.5 - cx) / rx;
                const double ny = (y + 0.5 - cy) / ry;
                const double d = nx * nx + ny * ny;
                if (d > 0.45 && d < 1.35) cell(x, y);
            }
        }
    }
    void disc(double cx, double cy, double r) {
        for (int y = 0; y < kGrid; ++y) {
            for (int x = 0; x < kGrid; ++x) {
                const double nx = x + 0.5 - cx;
                const double ny = y + 0.5 - cy;
                if (nx * nx + ny * ny <= r * r) cell(x, y);
            }
        }
    }
};

void boxOutline(Canvas& c) {
    c.frame(2, 3, 13, 13);
    c.hline(2, 13, 6);  // 标题带
}

void slash(Canvas& c) { c.line(2, 13, 13, 2); }

void eye(Canvas& c) {
    c.ring(7.5, 8.0, 5.6, 3.4);
    c.disc(7.5, 8.0, 1.6);
}

void twoBoxes(Canvas& c) {
    c.frame(2, 3, 13, 7);
    c.frame(2, 9, 13, 13);
}

void iconGrid(Canvas& c) {
    c.disc(4.0, 4.0, 1.8);
    c.disc(11.0, 4.0, 1.8);
    c.disc(4.0, 11.0, 1.8);
    c.disc(11.0, 11.0, 1.8);
}

void tabs(Canvas& c) {
    c.frame(2, 9, 13, 14);   // 内容框
    c.frame(3, 6, 7, 8);     // 两张标签凸起
    c.frame(9, 6, 13, 8);
}

void arrowDownToBar(Canvas& c) {
    c.vline(8, 2, 8);
    c.line(5, 6, 8, 9);
    c.line(11, 6, 8, 9);
    c.hline(3, 12, 13);
}

void arrowUpFromBar(Canvas& c) {
    c.vline(8, 5, 12);
    c.line(5, 8, 8, 5);
    c.line(11, 8, 8, 5);
    c.hline(3, 12, 13);
}

void undoArrow(Canvas& c) {
    c.hline(4, 12, 7);
    c.line(7, 4, 4, 7);
    c.line(7, 10, 4, 7);
    c.vline(12, 7, 11);
    c.hline(9, 12, 11);
}

void checklist(Canvas& c) {
    c.hline(2, 9, 3);
    c.hline(2, 9, 7);
    c.hline(2, 9, 11);
    c.hline(9, 13, 13);
    c.line(11, 11, 13, 13);
    c.line(11, 15, 13, 13);
}

void broom(Canvas& c) {
    c.line(12, 2, 6, 8);
    c.line(6, 8, 3, 13);
    c.line(6, 8, 7, 13);
    c.line(6, 8, 11, 13);
    c.hline(3, 11, 10);
}

void copyBoxes(Canvas& c) {
    c.frame(2, 2, 9, 9);
    c.frame(6, 6, 13, 13);
}

void pointerInBox(Canvas& c) {
    c.frame(2, 2, 13, 13);
    c.line(5, 5, 5, 11);
    c.line(5, 5, 10, 10);
    c.line(5, 11, 8, 9);
}

void pencil(Canvas& c) {
    c.line(3, 12, 11, 4);
    c.line(4, 13, 12, 5);
    c.line(11, 4, 12, 5);
    c.line(3, 12, 4, 13);
    c.hline(2, 3, 14);
}

void folder(Canvas& c) {
    c.frame(2, 5, 13, 13);
    c.hline(2, 6, 4);
    c.vline(6, 4, 5);
}

void cross(Canvas& c) {
    c.line(3, 3, 12, 12);
    c.line(3, 12, 12, 3);
}

void powerMark(Canvas& c) {
    c.ring(8, 9, 5.0, 5.0);
    c.vline(8, 1, 8);
}

void chevron(Canvas& c, bool up) {
    // 两条平行斜线，粗一点：单线在 16px 上只有十来个点，菜单里几乎看不见。
    if (up) {
        c.line(3, 11, 8, 6);
        c.line(8, 6, 13, 11);
        c.line(4, 11, 8, 7);
        c.line(8, 7, 12, 11);
    } else {
        c.line(3, 6, 8, 11);
        c.line(8, 11, 13, 6);
        c.line(4, 6, 8, 10);
        c.line(8, 10, 12, 6);
    }
}

void category(Canvas& c) {
    c.hline(2, 13, 4);
    c.hline(2, 10, 8);
    c.hline(2, 12, 12);
}

void Draw(Glyph g, Canvas& c) {
    switch (g) {
        case Glyph::None: break;
        case Glyph::NewBox:
            boxOutline(c);
            c.vline(8, 8, 12);
            c.hline(6, 10, 10);
            break;
        case Glyph::ShowBoxes: twoBoxes(c); break;
        case Glyph::HideBoxes:
            twoBoxes(c);
            slash(c);
            break;
        case Glyph::SaveNow: arrowDownToBar(c); break;
        case Glyph::ShowIcons: iconGrid(c); break;
        case Glyph::HideIcons:
            iconGrid(c);
            slash(c);
            break;
        case Glyph::Undo: undoArrow(c); break;
        case Glyph::RunRules: checklist(c); break;
        case Glyph::SweepRules: broom(c); break;
        case Glyph::Backup: copyBoxes(c); break;
        case Glyph::Restore: arrowUpFromBar(c); break;
        case Glyph::ShellMenuOn: pointerInBox(c); break;
        case Glyph::ShellMenuOff:
            pointerInBox(c);
            slash(c);
            break;
        case Glyph::Quit: powerMark(c); break;
        case Glyph::Collapse: chevron(c, true); break;
        case Glyph::Expand: chevron(c, false); break;
        case Glyph::Rename: pencil(c); break;
        case Glyph::Merge: tabs(c); break;
        case Glyph::OpenFolder: folder(c); break;
        case Glyph::Category: category(c); break;
        case Glyph::Ungroup:
            category(c);
            slash(c);
            break;
        case Glyph::Delete: cross(c); break;
        case Glyph::BoxItem: boxOutline(c); break;
    }
}

}  // namespace

void BuildGlyphPixels(Glyph glyph, int size, unsigned inkRgb, std::vector<uint32_t>& argb) {
    if (size < 1) size = 1;
    argb.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0u);
    Canvas c;
    c.size = size;
    c.px = argb.data();
    c.ink = 0xFF000000u | (inkRgb & 0x00FFFFFFu);
    Draw(glyph, c);
}

void PaintGlyphOnHdc(HDC dc, Glyph glyph, unsigned inkRgb, int x, int y, int size) {
    if (!dc || size < 1) return;
    std::vector<uint32_t> argb;
    BuildGlyphPixels(glyph, size, inkRgb, argb);
    const COLORREF ink = RGB((inkRgb >> 16) & 0xFF, (inkRgb >> 8) & 0xFF, inkRgb & 0xFF);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            if ((argb[static_cast<size_t>(py) * size + px] & 0xFF000000u) == 0) continue;
            SetPixelV(dc, x + px, y + py, ink);
        }
    }
}
