#include "menuicons.h"

#include <algorithm>

#include "theme.h"
#include "util.h"

IconMenu* IconMenu::active_ = nullptr;

namespace {

COLORREF crefOf(unsigned rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

}  // namespace

void IconMenu::Add(int id, Glyph g, const std::wstring& text, bool enabled, bool checked) {
    Item it;
    it.id = id;
    it.glyph = g;
    it.text = text;
    it.enabled = enabled;
    it.checked = checked;
    items_.push_back(std::move(it));
}

void IconMenu::Separator() {
    Item sep;
    sep.separator = true;
    items_.push_back(sep);
}

const IconMenu::Item* IconMenu::find(int id) const {
    if (id == 0) return nullptr;
    for (const auto& it : items_) {
        if (!it.separator && it.id == id) return &it;
    }
    return nullptr;
}

HMENU IconMenu::build() {
    HMENU m = CreatePopupMenu();
    if (!m) return nullptr;
    for (const auto& it : items_) {
        if (it.separator) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_OWNERDRAW;
        if (!it.enabled) flags |= MF_GRAYED;  // 灰掉的项不会被 TPM_RETURNCMD 返回，点击由系统挡住
        AppendMenuW(m, flags, static_cast<UINT_PTR>(it.id), nullptr);
    }
    return m;
}

int IconMenu::textWidth(const wchar_t* s, const PaintInfo& pi) const {
    HDC dc = GetDC(nullptr);
    if (!dc) return 0;
    HFONT old = pi.font ? (HFONT)SelectObject(dc, pi.font) : nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, static_cast<int>(wcslen(s)), &sz);
    if (old) SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    return sz.cx;
}

int IconMenu::Track(HWND owner, POINT screenPt) {
    if (!owner || items_.empty()) return 0;

    // 测量消息在 TrackPopupMenu 内部就会发过来，所以先把自己登记成活动菜单。
    active_ = this;

    const UINT dpi = GetDpiForWindow(owner);
    const int metrics = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    pi_.glyphPx = metrics > 8 ? metrics : 16;
    pi_.checkCol = pi_.glyphPx;  // 勾选列和图形列一样宽，所有项的文字才对齐

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        pi_.font = CreateFontIndirectW(&ncm.lfMenuFont);
    }
    if (!pi_.font) pi_.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    {
        HDC dc = GetDC(owner);
        HFONT old = dc && pi_.font ? (HFONT)SelectObject(dc, pi_.font) : nullptr;
        SIZE sz{};
        if (dc) GetTextExtentPoint32W(dc, L"国Ag", 4, &sz);
        pi_.textH = sz.cy > 0 ? sz.cy : 16;
        if (old) SelectObject(dc, old);
        if (dc) ReleaseDC(owner, dc);
    }

    HMENU menu = build();
    if (!menu) {
        active_ = nullptr;
        if (pi_.font) DeleteObject(pi_.font);
        pi_.font = nullptr;
        return 0;
    }

    SetForegroundWindow(owner);
    const int picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, screenPt.x, screenPt.y, 0,
                                      owner, nullptr);
    // 和原来一样：不补这一下，菜单会留在屏幕上直到下一次点击。
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    active_ = nullptr;
    if (pi_.font) {
        DeleteObject(pi_.font);
        pi_.font = nullptr;
    }
    return picked;
}

bool IconMenu::HandleMeasureItem(LPARAM lp) {
    if (!active_) return false;
    auto* mis = reinterpret_cast<LPMEASUREITEMSTRUCT>(lp);
    const Item* it = active_->find(static_cast<int>(mis->itemID));
    if (!it) return false;
    const PaintInfo& pi = active_->pi_;
    mis->itemHeight = static_cast<UINT>(std::max(pi.textH + 6, pi.glyphPx + 6));
    mis->itemWidth = static_cast<UINT>(6 + pi.checkCol + pi.glyphPx + 10 +
                                       active_->textWidth(it->text.c_str(), pi) + 24);
    return true;
}

bool IconMenu::HandleDrawItem(LPARAM lp) {
    if (!active_) return false;
    const auto& dis = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
    const Item* it = active_->find(static_cast<int>(dis.itemID));
    if (!it) return false;
    active_->paint(dis.hDC, dis.rcItem, *it, (dis.itemState & ODS_SELECTED) != 0, active_->pi_);
    return true;
}

void IconMenu::paint(HDC dc, const RECT& rc, const Item& item, bool selected, const PaintInfo& pi) {
    if (!dc) return;
    const Palette pal = ThemeCurrent();

    // 背景取当前配色的盒子底色：系统深色时菜单是深的，浅色时是浅的，
    // 不会出现"整个界面是暗的、弹出来的菜单是白的"那种割裂感。
    HBRUSH bg = CreateSolidBrush(crefOf(selected ? pal.title : pal.body));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    RECT textRc = rc;
    textRc.left += 6;
    if (item.checked) {
        // 自己画的勾选标记：owner-draw 之后系统不会替我们留那一列。
        const int cx = rc.left + 6;
        const int cy = rc.top + (rc.bottom - rc.top) / 2;
        HPEN pen = CreatePen(PS_SOLID, 2, crefOf(pal.text));
        HPEN oldPen = pen ? (HPEN)SelectObject(dc, pen) : nullptr;
        MoveToEx(dc, cx, cy, nullptr);
        LineTo(dc, cx + 4, cy + 4);
        LineTo(dc, cx + 9, cy - 4);
        if (oldPen) SelectObject(dc, oldPen);
        if (pen) DeleteObject(pen);
    }
    textRc.left += pi.checkCol;

    const unsigned ink = item.enabled ? pal.text : pal.dim;
    if (item.glyph != Glyph::None) {
        const int gy = rc.top + ((rc.bottom - rc.top) - pi.glyphPx) / 2;
        PaintGlyphOnHdc(dc, item.glyph, ink, textRc.left, gy, pi.glyphPx);
        textRc.left += pi.glyphPx + 10;
    } else {
        textRc.left += 4;
    }
    textRc.right -= 8;

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, crefOf(item.enabled ? pal.text : pal.dim));
    HFONT oldFont = pi.font ? (HFONT)SelectObject(dc, pi.font) : nullptr;
    DrawTextW(dc, item.text.c_str(), static_cast<int>(item.text.size()), &textRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}
