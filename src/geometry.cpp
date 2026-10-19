#include "geometry.h"

#include <algorithm>
#include <climits>

namespace {

long AreaWidth(const RECT& r) { return r.right - r.left; }
long AreaHeight(const RECT& r) { return r.bottom - r.top; }

// 一个显示器上"够得着"的最小区域：左上角那一小块标题栏。
RECT GrabBand(const RECT& box, int minVisibleW, int minVisibleH) {
    return RECT{box.left, box.top, box.left + minVisibleW, box.top + minVisibleH};
}

bool BandInside(const RECT& band, const RECT& work) {
    return band.left >= work.left && band.right <= work.right && band.top >= work.top && band.bottom <= work.bottom;
}

// 中心点到某个矩形的距离，落在里面就是 0。
long DistanceToRect(long x, long y, const RECT& r) {
    const long dx = x < r.left ? r.left - x : (x > r.right ? x - r.right : 0);
    const long dy = y < r.top ? r.top - y : (y > r.bottom ? y - r.bottom : 0);
    return dx * dx + dy * dy;
}

const RECT* Nearest(const RECT& box, const std::vector<RECT>& works) {
    const long cx = box.left + AreaWidth(box) / 2;
    const long cy = box.top + AreaHeight(box) / 2;
    const RECT* best = nullptr;
    long bestDist = LONG_MAX;
    for (const auto& w : works) {
        const long d = DistanceToRect(cx, cy, w);
        if (d < bestDist) {
            bestDist = d;
            best = &w;
        }
    }
    return best;
}

long ClampLong(long v, long lo, long hi) {
    if (hi < lo) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

namespace geometry {

std::vector<RECT> MonitorWorkAreas() {
    std::vector<RECT> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM l) -> BOOL {
            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof mi;
            if (GetMonitorInfoW(m, &mi)) reinterpret_cast<std::vector<RECT>*>(l)->push_back(mi.rcWork);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

bool NeedsClamp(const RECT& box, const std::vector<RECT>& workAreas, int minVisibleW, int minVisibleH) {
    if (workAreas.empty()) return false;
    const RECT band = GrabBand(box, minVisibleW, minVisibleH);
    for (const auto& w : workAreas) {
        if (BandInside(band, w)) return false;
    }
    return true;
}

RECT ClampIntoMonitors(const RECT& box, const std::vector<RECT>& workAreas, int minVisibleW, int minVisibleH) {
    if (workAreas.empty()) return box;
    if (!NeedsClamp(box, workAreas, minVisibleW, minVisibleH)) return box;

    const RECT* home = Nearest(box, workAreas);
    if (!home) return box;

    // 只保证那一小块标题栏落在可用区内，盒体允许超出屏幕边缘：
    // 半截探出屏幕是正常状态，拽回来反而会让人莫名。
    const long maxLeft = home->right - std::min<long>(minVisibleW, AreaWidth(*home));
    const long maxTop = home->bottom - std::min<long>(minVisibleH, AreaHeight(*home));

    // 算的是位移，不是新左上角。单独改写 left/top 会把 right/bottom 留在原地，
    // 盒子被拉成几千像素宽而不是平移回来。
    const long dx = ClampLong(box.left, home->left, maxLeft) - static_cast<long>(box.left);
    const long dy = ClampLong(box.top, home->top, maxTop) - static_cast<long>(box.top);

    RECT out = box;
    out.left = static_cast<LONG>(box.left + dx);
    out.top = static_cast<LONG>(box.top + dy);
    out.right = static_cast<LONG>(box.right + dx);
    out.bottom = static_cast<LONG>(box.bottom + dy);
    return out;
}

}  // namespace geometry
