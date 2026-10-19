#pragma once

#include "category.h"
#include "content.h"
#include "droptarget.h"
#include "layout.h"
#include "render.h"
#include "theme.h"

class Box {
 public:
    static constexpr int kTitleBarH = 30;
    static constexpr int kRadius = 8;
    static constexpr int kMinW = 140;
    static constexpr int kDefaultH = 240;
    static constexpr unsigned char kDefaultAlpha = 235;
    static constexpr int kTabH = 26;
    static constexpr int kTabW = 76;
    static constexpr int kTabGap = 4;

    static bool Register(HINSTANCE instance);
    static void Unregister(HINSTANCE instance);

    // members 是已经解析好的「成员盒子名 -> 目录」列表，非空时这个盒子变成多标签盒子。
    bool Create(const BoxConfig& cfg, const std::wstring& resolvedFolder,
          const std::vector<std::pair<std::wstring, std::wstring>>& members = std::vector<std::pair<std::wstring, std::wstring>>());
    void Destroy();
    void Show(bool visible);
    void SetAlpha(unsigned char alpha);
    void MoveTo(const RECT& screenRect);

    HWND hwnd() const { return hwnd_; }
    bool alive() const { return hwnd_ != nullptr && IsWindow(hwnd_); }
    bool painted() const { return painted_; }
    bool dropRegistered() const { return dropTarget_ != nullptr; }
    bool IsAggregate() const { return !memberFolders_.empty(); }
    std::wstring activeFolder() const;
    std::wstring activeFolderForTest() const { return activeFolder(); }
    bool visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }

    void RefreshContent();

    // 就地改名：直接在标题区放一个编辑框，回车确认、Esc 或失去焦点放弃。
    void BeginRename();
    void EndRename(bool commit);
    bool renaming() const { return renaming_; }
    // 取走本次确认的新名字；取走后清空，避免重复应用。
    bool takeRenameRequest(std::wstring& outNewName);

    // 分类标签卡：只影响显示，不动文件。
    bool SetCategoryTab(const std::wstring& tab);
    bool Grouped() const { return grouped_; }
    void SetGrouped(bool on);
    std::wstring ActiveTab() const { return activeTab_; }
    size_t shownCount() const;
    int tabAreaHeight() const { return TabAreaHeight(); }
    std::vector<std::wstring> TabNames() const;
    bool AcceptFiles(const std::vector<std::wstring>& paths);
    static bool OnDrop(void* ctx, const std::vector<std::wstring>& paths);
    size_t itemCount() const { return content_.count(); }
    unsigned iconCount() const;
    unsigned uploadedCount() const { return content_.uploadedCount(); }
    const std::wstring& folder() const { return content_.folder(); }

    void Current(BoxConfig& out) const;

 private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);

    struct Metrics {
        float padX = 0;
        float padTop = 0;
        float cellW = 0;
        float cellH = 0;
        float iconPx = 0;
        float labelH = 0;
        int cols = 1;
    };

    float Scale() const { return hwnd_ ? GetDpiForWindow(hwnd_) / 96.0f : 1.0f; }
    int TitleH() const;
    Metrics GetMetrics() const;
    void ClampScroll(const Metrics& m);
    int ItemIndexAt(POINT clientPt, const Metrics& m) const;
    void OpenItem(int index);

    void ReleaseTarget();
    void EnsureTarget();
    void ApplyRegion();
    void ApplyPalette();
    static D2D1_COLOR_F ColorOf(unsigned rgb);
    void Paint();
    bool InTitleBar(int clientY) const;
    int TabAreaHeight() const;
    int TabAt(POINT clientPt) const;
    float BodyTop() const { return static_cast<float>(TitleH() + TabAreaHeight()); }
    void SelectMember(size_t index, bool notify);
    std::vector<size_t> VisibleIndices() const;
    struct TabGeom {
        std::vector<std::wstring> names;
        float x0 = 0, y0 = 0, tabW = 0, tabH = 0, gap = 0;
        int perRow = 1;
    };
    TabGeom TabGeometry() const;
    void StartDrag(LPARAM lp);
    void DragTo(LPARAM lp);
    void EndDrag();
    void ToggleCollapsed();
    void ShowContextMenu(POINT screenPt);

    static Box* From(HWND h) { return reinterpret_cast<Box*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

    HWND hwnd_ = nullptr;
    BoxConfig cfg_{};
    std::wstring resolvedFolder_;
    IDropTarget* dropTarget_ = nullptr;
    bool dragging_ = false;
    POINT grab_{};
    int expandedH_ = kDefaultH;
    bool painted_ = false;
    Palette pal_{};
    IconCache iconCache_;
    Content content_{};
    std::vector<std::wstring> memberNames_;
    std::vector<std::wstring> memberFolders_;
    bool grouped_ = false;
    bool renaming_ = false;
    HWND edit_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    COLORREF editBk_ = RGB(0, 0, 0);
    COLORREF editFg_ = RGB(255, 255, 255);
    WNDPROC oldEditProc_ = nullptr;
    std::wstring pendingRename_;    // 编辑框里待确认的内容
    std::wstring committedRename_;  // 已经按回车确认、等应用层处理的新名字
    std::wstring activeTab_;  // 空表示"全部"
    int scrollRow_ = 0;
    unsigned targetGen_ = 0;

    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* bodyBrush_ = nullptr;
    ID2D1SolidColorBrush* titleBrush_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    ID2D1SolidColorBrush* edgeBrush_ = nullptr;
    ID2D1SolidColorBrush* dimBrush_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* labelFormat_ = nullptr;
};
