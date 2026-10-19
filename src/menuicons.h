#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "glyphs.h"

// 带小图标的弹出菜单。每一项都是 MF_OWNERDRAW，尺寸自己量、画面自己画：
// 左边一列留给勾选标记，然后是小图形，再是文字。背景取当前配色的盒子底色，
// 所以系统深色时菜单是深的、浅色时是浅的，不会画出白底菜单这种突兀东西。
//
// 用法：Add/Separator 攒好条目，Track 弹出来；owner 窗口的窗口过程必须把
// WM_MEASUREITEM 和 WM_DRAWITEM 交给 HandleMeasureItem/HandleDrawItem。
// 同一线程同时只允许有一个正在显示的菜单，实现里用一个活动指针找回条目。
class IconMenu {
 public:
    struct Item {
        int id = 0;
        Glyph glyph = Glyph::None;
        std::wstring text;
        bool enabled = true;
        bool checked = false;
        bool separator = false;
    };

    void Add(int id, Glyph g, const std::wstring& text, bool enabled = true, bool checked = false);
    void Separator();
    size_t itemCount() const { return items_.size(); }

    // 返回被点中的 id，没点中返回 0。owner 既是菜单宿主，也是测量/绘制消息的接收者。
    int Track(HWND owner, POINT screenPt);

    static bool HandleMeasureItem(LPARAM lp);
    static bool HandleDrawItem(LPARAM lp);

 private:
    struct PaintInfo {
        int checkCol = 0;
        int glyphPx = 0;
        int textH = 0;
        HFONT font = nullptr;
    };

    const Item* find(int id) const;
    int textWidth(const wchar_t* s, const PaintInfo& pi) const;
    void paint(HDC dc, const RECT& rc, const Item& item, bool selected, const PaintInfo& pi);
    HMENU build();

    std::vector<Item> items_;
    PaintInfo pi_{};
    static IconMenu* active_;
};
