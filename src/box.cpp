#include "box.h"

#include <windowsx.h>

#include <shlwapi.h>

#include <algorithm>

#include "fileop.h"
#include "iconview.h"
#include "menuicons.h"
#include "messages.h"
#include "util.h"

static const wchar_t* kBoxClass = L"DeskOrgBox";
static constexpr int kCmdCollapse = 1;
static constexpr int kCmdDelete = 2;
static constexpr int kCmdUndo = 3;
static constexpr int kCmdOpenFolder = 4;
static constexpr int kCmdRename = 6;
static constexpr int kCmdMerge = 7;
static constexpr int kCmdGroup = 5;

// 第二排标签卡里的「回到盒子自己那一层」那一张。
static const wchar_t* kThisLevel = L"本层";

bool Box::Register(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kBoxClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    ATOM a = RegisterClassExW(&wc);
    if (!a) Log(L"Box::Register FAILED gle=%lu", GetLastError());
    return a != 0;
}

void Box::Unregister(HINSTANCE instance) { UnregisterClassW(kBoxClass, instance); }

bool Box::Create(const BoxConfig& cfg, const std::wstring& resolvedFolder,
                 const std::vector<std::pair<std::wstring, std::wstring>>& members) {
    cfg_ = cfg;
    cfg_.name = SanitizeBoxName(cfg_.name);
    for (const auto& m : members) {
        if (m.first.empty() || m.second.empty()) continue;
        if (m.first == cfg_.name) continue;  // 自己不能是自己的成员
        memberNames_.push_back(m.first);
        memberFolders_.push_back(m.second);
    }

    int w = cfg_.rect.right - cfg_.rect.left;
    int h = cfg_.rect.bottom - cfg_.rect.top;
    if (w < kMinW) w = kMinW;
    expandedH_ = h < kTitleBarH + 60 ? kDefaultH : h;
    int shown = cfg_.collapsed ? kTitleBarH : expandedH_;

    // 实测：Progman 的子窗口在本机不显示（能命中测试、能收点击，但画面出不来）。
    // 能显示的只有顶层窗口；顶层窗口压在 HWND_BOTTOM 上同样位于图标层之上、被应用窗口正常遮挡。
    // WS_EX_NOACTIVATE 保证点盒子不会从前台应用抢走键盘焦点。
    // WS_EX_LAYERED 只有在顶层窗口上可用（子窗口实测创建失败），用来做整体半透明。
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kBoxClass, cfg_.name.c_str(),
                            WS_POPUP, cfg_.rect.left, cfg_.rect.top, w, shown, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        Log(L"Box::Create(%s) FAILED gle=%lu", cfg_.name.c_str(), GetLastError());
        return false;
    }

    // 内容必须在第一次绘制之前准备好：ApplyRegion 里的 SetWindowRgn 会同步触发 WM_PAINT，
    // 放在它后面就会先画出一张空盒子，而且之后不会自动重绘。
    if (IsAggregate()) {
        // 多标签盒子的标签卡就是成员盒子，和按类型分类互斥。
        grouped_ = false;
        std::wstring want = cfg_.tab;
        auto it = std::find(memberNames_.begin(), memberNames_.end(), want);
        size_t idx = it == memberNames_.end() ? 0 : static_cast<size_t>(std::distance(memberNames_.begin(), it));
        SelectMember(idx, false);
    } else {
        grouped_ = cfg_.grouped;
        activeTab_ = cfg_.tab;
        // 手写成「全部」和留空是同一个意思，不能让 activeTab_ 拿去和分类名比。
        if (activeTab_ == L"全部") activeTab_.clear();
        resolvedFolder_ = resolvedFolder;
    }
    // 配置里的 sub 只有当当前目录下真有这么一个子文件夹时才算数，
    // 剩下的校验和目录切换都在 RefreshContent 里做。
    if (!cfg_.sub.empty()) activeSub_ = cfg_.sub;
    RefreshContent();

    ApplyRegion();
    SetAlpha(kDefaultAlpha);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    dropTarget_ = new FileDropTarget(this, &Box::OnDrop);
    HRESULT drp = RegisterDragDrop(hwnd_, dropTarget_);
    if (FAILED(drp)) {
        Log(L"RegisterDragDrop(%s) 失败 hr=0x%08X", cfg_.name.c_str(), static_cast<unsigned>(drp));
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    cfg_.rect.bottom = cfg_.rect.top + shown;

    Log(L"盒子 %s 建在 [%d,%d %dx%d] 目录=%s 内容=%zu", cfg_.name.c_str(), cfg_.rect.left, cfg_.rect.top, w, shown,
        resolvedFolder_.c_str(), content_.count());
    return true;
}

int Box::TitleH() const { return static_cast<int>(kTitleBarH * Scale()); }

Box::Metrics Box::GetMetrics() const {
    float s = Scale();
    Metrics m;
    m.padX = 10.0f * s;
    m.padTop = 10.0f * s;
    m.iconPx = 44.0f * s;
    m.labelH = 20.0f * s;
    m.cellW = 88.0f * s;
    m.cellH = m.iconPx + m.labelH + 6.0f * s;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float usable = static_cast<float>(rc.right) - m.padX * 2.0f;
    m.cols = usable > m.cellW ? static_cast<int>(usable / m.cellW) : 1;
    if (m.cols < 1) m.cols = 1;
    return m;
}


void Box::ClampScroll(const Metrics& m) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int viewportRows = static_cast<int>((rc.bottom - BodyTop() - m.padTop * 2) / m.cellH);
    if (viewportRows < 1) viewportRows = 1;
    int totalRows = static_cast<int>((content_.count() + m.cols - 1) / m.cols);
    int maxRow = totalRows - viewportRows;
    if (maxRow < 0) maxRow = 0;
    if (scrollRow_ > maxRow) scrollRow_ = maxRow;
    if (scrollRow_ < 0) scrollRow_ = 0;
}

unsigned Box::iconCount() const {
    unsigned n = 0;
    for (size_t i = 0; i < content_.count(); ++i) {
        if (content_.at(i).hbmp) n++;
    }
    return n;
}

int Box::ItemIndexAt(POINT clientPt, const Metrics& m) const {
    if (clientPt.y < TitleH() + m.padTop) return -1;
    if (static_cast<float>(clientPt.x) < m.padX) return -1;
    float y = clientPt.y - TitleH() - m.padTop + scrollRow_ * m.cellH;
    int row = static_cast<int>(y / m.cellH);
    if (row < 0) return -1;
    int col = static_cast<int>((static_cast<float>(clientPt.x) - m.padX) / m.cellW);
    if (col < 0 || col >= m.cols) return -1;
    auto visible = VisibleIndices();
    size_t idx = static_cast<size_t>(row) * m.cols + col;
    if (idx >= visible.size()) return -1;
    return static_cast<int>(visible[idx]);
}

void Box::OpenItem(int index) {
    if (index < 0 || static_cast<size_t>(index) >= content_.count()) return;
    const BoxItem& it = content_.at(index);
    HINSTANCE r = ShellExecuteW(hwnd_, L"open", it.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"打开 %s 结果=%lld", it.path.c_str(), reinterpret_cast<INT_PTR>(r));
}

void Box::RefreshContent() {
    RefreshSubFolders();
    // 子文件夹被删掉或被搬到别处了，就退回本层：显示一个不存在的目录会让盒子变成空白框。
    if (!activeSub_.empty() && !PathExists(JoinPath(activeFolder(), activeSub_))) {
        Log(L"盒子 %s 的第二层 %s 已经不存在，退回本层", cfg_.name.c_str(), activeSub_.c_str());
        activeSub_.clear();
        PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
    }
    content_.SetFolder(displayFolder(), static_cast<unsigned>(44 * Scale()), &iconCache_);
    content_.Refresh();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Box::SetAlpha(unsigned char alpha) {
    if (!hwnd_) return;
    if (!SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA)) {
        Log(L"SetLayeredWindow alpha=%u FAILED gle=%lu", alpha, GetLastError());
    }
}

void Box::Destroy() {
    if (edit_) {
        // 窗口正在消失，别再走提交/取消那套，直接撤掉子类化并销毁子控件。
        if (oldEditProc_) SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldEditProc_));
        oldEditProc_ = nullptr;
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }
    renaming_ = false;
    if (dropTarget_) {
        if (hwnd_) RevokeDragDrop(hwnd_);
        dropTarget_->Release();
        dropTarget_ = nullptr;
    }
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    painted_ = false;
    ReleaseTarget();
}

bool Box::OnDrop(void* ctx, const std::vector<std::wstring>& paths) {
    Box* self = static_cast<Box*>(ctx);
    return self && self->AcceptFiles(paths);
}

bool Box::AcceptFiles(const std::vector<std::wstring>& paths) {
    // 移走之前先记下这些图标在桌面上的格子，撤销时要按这个坐标放回去。
    std::vector<IconPosition> before = IconViewCaptureInDir(paths, DesktopPath());

    // 落点是当前显示的那一层：正看着子文件夹，拖进来的就该在这里，所见即所得。
    const std::wstring target = displayFolder();
    std::vector<MovedEntry> moves;
    std::wstring err;
    if (!MoveFilesTo(paths, target, hwnd_, moves, err)) {
        Log(L"盒子 %s 收文件失败：%s", cfg_.name.c_str(), err.c_str());
        return false;
    }
    if (!moves.empty()) {
        std::wstring where = cfg_.name;
        if (!activeSub_.empty()) where += L"/" + activeSub_;
        UndoHistoryAdd(std::move(moves), L"把 " + std::to_wstring(moves.size()) + L" 项放进「" + where + L"」",
                       std::move(before));
    }
    RefreshContent();
    return true;
}

void Box::Show(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOWNA : SW_HIDE);
}

void Box::Current(BoxConfig& out) const {
    out = cfg_;
    if (!hwnd_) return;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    POINT tl{rc.left, rc.top};
    POINT br{rc.right, rc.bottom};
    HWND parent = GetParent(hwnd_);
    if (parent) {
        ScreenToClient(parent, &tl);
        ScreenToClient(parent, &br);
    }
    int height = cfg_.collapsed ? kTitleBarH : expandedH_;
    out.rect = RECT{tl.x, tl.y, tl.x + (br.x - tl.x), tl.y + height};
    out.collapsed = cfg_.collapsed;
    out.grouped = grouped_;
    out.tab = activeTab_;
    out.sub = activeSub_;
}

void Box::ReleaseTarget() {
    content_.ReleaseBitmaps();
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (bodyBrush_) { bodyBrush_->Release(); bodyBrush_ = nullptr; }
    if (titleBrush_) { titleBrush_->Release(); titleBrush_ = nullptr; }
    if (textBrush_) { textBrush_->Release(); textBrush_ = nullptr; }
    if (edgeBrush_) { edgeBrush_->Release(); edgeBrush_ = nullptr; }
    if (dimBrush_) { dimBrush_->Release(); dimBrush_ = nullptr; }
    if (titleFormat_) { titleFormat_->Release(); titleFormat_ = nullptr; }
    if (labelFormat_) { labelFormat_->Release(); labelFormat_ = nullptr; }
    painted_ = false;
}

void Box::ApplyRegion() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, kRadius * 2, kRadius * 2);
    if (!rgn) return;
    // 成功时区域所有权交给 USER，不能再 DeleteObject。
    if (SetWindowRgn(hwnd_, rgn, TRUE) == 0) {
        Log(L"SetWindowRgn 失败 gle=%lu", GetLastError());
        DeleteObject(rgn);
    }
}

void Box::EnsureTarget() {
    if (rt_) return;
    if (!hwnd_ || !TheD2D()) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    UINT dpi = GetDpiForWindow(hwnd_);
    HRESULT hr = TheD2D()->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                     static_cast<FLOAT>(dpi), static_cast<FLOAT>(dpi)),
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))),
        &rt_);
    if (FAILED(hr)) {
        LogHr(L"CreateHwndRenderTarget", hr);
        rt_ = nullptr;
        return;
    }

    auto mk = [this](D2D1_COLOR_F c, ID2D1SolidColorBrush** out) {
        if (FAILED(rt_->CreateSolidColorBrush(c, out))) *out = nullptr;
    };
    // 颜色一律来自当前配色，切主题时由 ApplyPalette 改，不再散着写死。
    mk(D2D1::ColorF(0.13f, 0.16f, 0.21f, 1.0f), &bodyBrush_);
    mk(D2D1::ColorF(0.22f, 0.27f, 0.35f, 1.0f), &titleBrush_);
    mk(D2D1::ColorF(0.92f, 0.94f, 0.98f, 1.0f), &textBrush_);
    mk(D2D1::ColorF(0.40f, 0.47f, 0.60f, 1.0f), &edgeBrush_);
    mk(D2D1::ColorF(0.72f, 0.78f, 0.88f, 1.0f), &dimBrush_);
    ApplyPalette();

    // 图标位图绑定在这个 target 上，新 target 就是新的一代，触发 Content 重新上传。
    targetGen_++;

    HRESULT fh = TheDWrite()->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"",
                                               &titleFormat_);
    if (FAILED(fh)) {
        LogHr(L"CreateTextFormat", fh);
        titleFormat_ = nullptr;
        return;
    }
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trim{};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    titleFormat_->SetTrimming(&trim, nullptr);

    HRESULT lh = TheDWrite()->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"",
                                               &labelFormat_);
    if (FAILED(lh)) {
        LogHr(L"CreateTextFormat label", lh);
        labelFormat_ = nullptr;
        return;
    }
    labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TRIMMING ellipsis{};
    ellipsis.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ellipsis.delimiter = 0x2026;
    ellipsis.delimiterCount = 1;
    labelFormat_->SetTrimming(&ellipsis, nullptr);
}

void Box::Paint() {
    if (!hwnd_) return;
    EnsureTarget();
    if (!rt_ || !bodyBrush_ || !edgeBrush_ || !textBrush_ || !titleFormat_ || !labelFormat_) {
        Log(L"Paint 提前返回 rt=%d body=%d edge=%d text=%d titleFmt=%d labelFmt=%d", rt_ ? 1 : 0, bodyBrush_ ? 1 : 0,
            edgeBrush_ ? 1 : 0, textBrush_ ? 1 : 0, titleFormat_ ? 1 : 0, labelFormat_ ? 1 : 0);
        painted_ = false;
        return;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    FLOAT W = static_cast<FLOAT>(rc.right);
    FLOAT H = static_cast<FLOAT>(rc.bottom);

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(ColorOf(pal_.body));

    D2D1_ROUNDED_RECT body = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f),
                                               static_cast<FLOAT>(kRadius), static_cast<FLOAT>(kRadius));
    rt_->FillRoundedRectangle(body, bodyBrush_);

    Metrics m = GetMetrics();
    ClampScroll(m);
    FLOAT titleBottom = static_cast<FLOAT>(TitleH());
    if (H > titleBottom + 1.0f) {
        rt_->FillRectangle(D2D1::RectF(0.5f, 0.5f, W - 0.5f, titleBottom), titleBrush_);
        rt_->DrawLine(D2D1::Point2F(0.5f, titleBottom), D2D1::Point2F(W - 0.5f, titleBottom), edgeBrush_, 1.0f);
    }

    rt_->DrawRoundedRectangle(body, edgeBrush_, 1.0f);

    if (!renaming_) {
        D2D1_RECT_F textRect = D2D1::RectF(m.padX, 0.0f, W - 34.0f, titleBottom);
        rt_->DrawText(cfg_.name.c_str(), static_cast<UINT32>(cfg_.name.size()), titleFormat_, &textRect,
                      textBrush_, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    const wchar_t* fold = cfg_.collapsed ? L"▸" : L"▾";
    D2D1_RECT_F foldRect = D2D1::RectF(W - 30.0f, 0.0f, W - 8.0f, titleBottom);
    rt_->DrawText(fold, 1, titleFormat_, &foldRect, textBrush_, D2D1_DRAW_TEXT_OPTIONS_NONE,
                  DWRITE_MEASURING_MODE_NATURAL);

    FLOAT bodyTop = static_cast<FLOAT>(TitleH());
    if (!cfg_.collapsed) {
        if (HasTabRow()) PaintRow(TabGeometry(), activeTab_.empty() ? L"全部" : activeTab_);
        if (HasSubRow()) PaintRow(SubGeometry(), activeSub_.empty() ? kThisLevel : activeSub_);
        bodyTop = BodyTop();
    }

    if (!cfg_.collapsed) {
        if (content_.empty()) {
            D2D1_RECT_F hint = D2D1::RectF(m.padX, bodyTop + m.padTop, W - m.padX, H - m.padTop);
            const wchar_t* msg = L"空盒子，把文件拖进来";
            rt_->DrawText(msg, static_cast<UINT32>(wcslen(msg)), titleFormat_, &hint, dimBrush_,
                          D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        } else if (VisibleIndices().empty()) {
            D2D1_RECT_F hint = D2D1::RectF(m.padX, bodyTop + m.padTop, W - m.padX, H - m.padTop);
            const wchar_t* msg = L"这个分类下没有东西";
            rt_->DrawText(msg, static_cast<UINT32>(wcslen(msg)), titleFormat_, &hint, dimBrush_,
                          D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        } else {
            content_.UploadTo(rt_, targetGen_);
            auto visible = VisibleIndices();
            for (size_t slot = 0; slot < visible.size(); ++slot) {
                int row = static_cast<int>(slot / m.cols);
                int col = static_cast<int>(slot % m.cols);
                float x = m.padX + col * m.cellW;
                float y = bodyTop + m.padTop + (row - scrollRow_) * m.cellH;
                if (y + m.cellH <= bodyTop) continue;
                if (y > H) break;

                const BoxItem& it = content_.at(visible[slot]);
                float iconX = x + (m.cellW - m.iconPx) / 2.0f;
                D2D1_RECT_F iconRect = D2D1::RectF(iconX, y, iconX + m.iconPx, y + m.iconPx);
                if (it.bitmap) {
                    rt_->DrawBitmap(it.bitmap, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                   D2D1::RectF(0, 0, it.bitmap->GetSize().width, it.bitmap->GetSize().height));
                } else {
                    rt_->FillRectangle(iconRect, edgeBrush_);
                }

                D2D1_RECT_F labelRect = D2D1::RectF(x, y + m.iconPx + 2.0f, x + m.cellW, y + m.cellH);
                rt_->DrawText(it.name.c_str(), static_cast<UINT32>(it.name.size()), labelFormat_, &labelRect,
                              dimBrush_ ? dimBrush_ : textBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                              DWRITE_MEASURING_MODE_NATURAL);
            }
        }
    }

    HRESULT hr = rt_->EndDraw();
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || FAILED(hr)) {
        Log(L"EndDraw 需要重建渲染目标 hr=0x%08lX", static_cast<unsigned long>(hr));
        ReleaseTarget();
        painted_ = false;
    } else {
        painted_ = true;
    }
}

bool Box::InTitleBar(int clientY) const { return clientY >= 0 && clientY < TitleH(); }

void Box::StartDrag(LPARAM lp) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ClientToScreen(hwnd_, &sp);
    grab_.x = sp.x - rc.left;
    grab_.y = sp.y - rc.top;
    dragging_ = true;
    SetCapture(hwnd_);
}

void Box::DragTo(LPARAM lp) {
    POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ClientToScreen(hwnd_, &sp);
    POINT origin = sp;
    HWND parent = GetParent(hwnd_);
    if (parent) ScreenToClient(parent, &origin);

    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int width = rc.right - rc.left;
    int height = cfg_.collapsed ? kTitleBarH : expandedH_;
    MoveWindow(hwnd_, origin.x - grab_.x, origin.y - grab_.y, width, height, TRUE);
}

void Box::EndDrag() {
    if (!dragging_) return;
    dragging_ = false;
    ReleaseCapture();
    PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
}

void Box::ToggleCollapsed() {
    cfg_.collapsed = !cfg_.collapsed;
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int width = rc.right - rc.left;
    int height = cfg_.collapsed ? kTitleBarH : expandedH_;
    POINT tl{rc.left, rc.top};
    HWND parent = GetParent(hwnd_);
    if (parent) ScreenToClient(parent, &tl);
    MoveWindow(hwnd_, tl.x, tl.y, width, height, TRUE);
    PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
}

void Box::ShowContextMenu(POINT screenPt) {
    // 菜单项左侧的小图形由 IconMenu 现画：owner-draw 之后勾选列、图形和文字都要自己摆。
    IconMenu menu;
    menu.Add(kCmdCollapse, cfg_.collapsed ? Glyph::Expand : Glyph::Collapse,
             cfg_.collapsed ? L"展开盒子" : L"收起盒子");
    menu.Add(kCmdRename, Glyph::Rename, L"重命名（直接输入，回车确认）");
    menu.Add(kCmdMerge, Glyph::Merge, L"与其他盒子合并成标签盒");
    menu.Add(kCmdOpenFolder, Glyph::OpenFolder, L"在资源管理器中打开目录");
    menu.Add(kCmdGroup, grouped_ ? Glyph::Ungroup : Glyph::Category,
             grouped_ ? L"关闭盒内分类" : L"按类型分标签卡");

    std::wstring undoLabel;
    if (UndoHistoryLastLabel(undoLabel)) {
        menu.Add(kCmdUndo, Glyph::Undo, L"撤销：" + undoLabel);
    } else {
        menu.Add(kCmdUndo, Glyph::Undo, L"没有可撤销的操作", false);
    }

    menu.Separator();
    menu.Add(kCmdDelete, Glyph::Delete, L"删除这个盒子");

    int cmd = menu.Track(hwnd_, screenPt);

    switch (cmd) {
        case kCmdCollapse:
            ToggleCollapsed();
            break;
        case kCmdOpenFolder:
            // 开的是当前显示的那一层：正看着子目录，就应该在那里被打开。
            ShellExecuteW(nullptr, L"open", displayFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kCmdRename:
            BeginRename();
            break;
        case kCmdMerge:
            // 成员关系在布局里，得由界面实例来改；坐标打包进 lParam 传过去。
            PostMessageW(TheTrayWnd(), WM_APP_MERGE_MENU, reinterpret_cast<WPARAM>(hwnd_),
                         MAKELPARAM(static_cast<WORD>(screenPt.x), static_cast<WORD>(screenPt.y)));
            break;
        case kCmdUndo: {
            std::wstring err;
            if (UndoHistoryUndoLast(hwnd_, err)) {
                Log(L"撤销成功");
            } else {
                Log(L"撤销失败：%s", err.c_str());
            }
            // 撤销会把文件放回原位，可能跨了好几个盒子，全部刷新一次。
            PostMessageW(TheTrayWnd(), WM_APP_CONTENT_DIRTY, 0, 0);
            break;
        }
        case kCmdGroup:
            SetGrouped(!grouped_);
            break;
        case kCmdDelete:
            PostMessageW(TheTrayWnd(), WM_APP_BOX_DELETE, reinterpret_cast<WPARAM>(hwnd_), 0);
            break;
        default:
            break;
    }
}

LRESULT CALLBACK Box::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, msg, wp, lp);
    }

    Box* self = From(h);
    if (!self) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MEASUREITEM:
            if (IconMenu::HandleMeasureItem(lp)) return TRUE;
            break;
        case WM_DRAWITEM:
            if (IconMenu::HandleDrawItem(lp)) return TRUE;
            break;
        case WM_SIZE: {
            int w = LOWORD(lp);
            int hh = HIWORD(lp);
            self->ApplyRegion();
            if (self->rt_ && w > 0 && hh > 0) {
                self->rt_->Resize(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(hh)));
                self->painted_ = false;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int cy = GET_Y_LPARAM(lp);
            if (self->InTitleBar(cy)) {
                self->StartDrag(lp);
                return 0;
            }
            const POINT pt{GET_X_LPARAM(lp), cy};
            if (self->HasTabRow()) {
                int i = self->HitRow(self->TabGeometry(), pt);
                if (i >= 0) {
                    auto names = self->TabNames();
                    self->SetCategoryTab(names[static_cast<size_t>(i)]);
                    return 0;
                }
            }
            if (self->HasSubRow()) {
                int i = self->HitRow(self->SubGeometry(), pt);
                if (i >= 0) {
                    auto subs = self->SubNames();
                    self->SetSubTab(subs[static_cast<size_t>(i)]);
                    return 0;
                }
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (self->dragging_) {
                self->DragTo(lp);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (self->dragging_) {
                self->EndDrag();
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK: {
            int cx = GET_X_LPARAM(lp);
            int cy = GET_Y_LPARAM(lp);
            if (self->InTitleBar(cy)) {
                self->ToggleCollapsed();
                return 0;
            }
            Metrics m = self->GetMetrics();
            self->OpenItem(self->ItemIndexAt(POINT{cx, cy}, m));
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (self->cfg_.collapsed) break;
            Metrics m = self->GetMetrics();
            int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            self->scrollRow_ -= notches;
            self->ClampScroll(m);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &pt);
            self->ShowContextMenu(pt);
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, self->editBk_);
            SetTextColor(dc, self->editFg_);
            return reinterpret_cast<LRESULT>(self->editBrush_);
        }
        case WM_COMMAND: {
            // 失焦就放弃：用户点到别处不该被当成确认改名。
            if (self->renaming_ && reinterpret_cast<HWND>(lp) == self->edit_ && HIWORD(wp) == EN_KILLFOCUS) {
                self->EndRename(false);
                return 0;
            }
            break;
        }
        case WM_SETTINGCHANGE: {
            // 系统深浅色变化时 Explorer 会广播这个消息，Auto 模式下重新取一次配色。
            const wchar_t* area = reinterpret_cast<const wchar_t*>(lp);
            if (area && _wcsicmp(area, L"ImmersiveColorSet") == 0) self->ApplyPalette();
            InvalidateRect(h, nullptr, FALSE);
            break;
        }
        case WM_DESTROY:
            self->ReleaseTarget();
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 第一排标签卡：按类型或按成员分卡，只改显示，不动任何文件 ----

std::vector<std::wstring> Box::TabNames() const {
    if (IsAggregate()) return memberNames_;  // 多标签盒子：标签卡就是成员盒子，不给"全部"，否则拖进来不知道该放哪个目录
    std::vector<std::wstring> names;
    names.push_back(L"全部");
    if (!grouped_) return names;
    for (const auto& cat : category::Names()) {
        for (size_t i = 0; i < content_.count(); ++i) {
            const BoxItem& it = content_.at(i);
            if (category::CategoryOf(it.name, it.isDirectory) == cat) {
                names.push_back(cat);
                break;
            }
        }
    }
    return names;
}

int Box::RowBlockHeight(const TabGeom& g) const {
    if (g.names.empty()) return 0;
    int rows = static_cast<int>((g.names.size() + g.perRow - 1) / g.perRow);
    return static_cast<int>(rows * (g.tabH + 4.0f * Scale())) + 4;
}

int Box::TabAreaHeight() const {
    // 两排各自算，第二排不存在的时候高度就是 0，盒子看起来和以前一样。
    int h = 0;
    if (HasTabRow()) h += RowBlockHeight(TabGeometry());
    if (HasSubRow()) h += RowBlockHeight(SubGeometry());
    return h;
}

Box::TabGeom Box::TabGeometry() const {
    float s = Scale();
    TabGeom g;
    g.names = HasTabRow() ? TabNames() : std::vector<std::wstring>();
    g.x0 = 10.0f * s;
    g.y0 = static_cast<float>(TitleH()) + 4.0f * s;
    g.tabW = kTabW * s;
    g.tabH = kTabH * s;
    g.gap = kTabGap * s;
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    float usable = static_cast<float>(rc.right) - g.x0 * 2.0f;
    g.perRow = static_cast<int>(usable / (g.tabW + g.gap));
    if (g.perRow < 1) g.perRow = 1;
    return g;
}

Box::TabGeom Box::SubGeometry() const {
    // 宽度、每行几张这些算法和第一排一样，只是接在它下面，名字换成子目录。
    TabGeom g = TabGeometry();
    const int firstRowHeight = RowBlockHeight(g);
    g.names = HasSubRow() ? SubNames() : std::vector<std::wstring>();
    g.y0 = static_cast<float>(TitleH()) + static_cast<float>(firstRowHeight) + 4.0f * Scale();
    return g;
}

int Box::HitRow(const TabGeom& g, POINT clientPt) const {
    for (size_t i = 0; i < g.names.size(); ++i) {
        int row = static_cast<int>(i / g.perRow);
        int col = static_cast<int>(i % g.perRow);
        float x = g.x0 + static_cast<float>(col) * (g.tabW + g.gap);
        float y = g.y0 + static_cast<float>(row) * (g.tabH + 4.0f * Scale());
        if (clientPt.x >= x && clientPt.x < x + g.tabW && clientPt.y >= y && clientPt.y < y + g.tabH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Box::PaintRow(const TabGeom& g, const std::wstring& active) {
    if (!rt_) return;
    for (size_t i = 0; i < g.names.size(); ++i) {
        int row = static_cast<int>(i / g.perRow);
        int col = static_cast<int>(i % g.perRow);
        FLOAT tx = g.x0 + static_cast<FLOAT>(col) * (g.tabW + g.gap);
        FLOAT ty = g.y0 + static_cast<FLOAT>(row) * (g.tabH + 4.0f * Scale());
        D2D1_RECT_F tr = D2D1::RectF(tx, ty, tx + g.tabW, ty + g.tabH);
        const bool on = g.names[i] == active;
        if (on) {
            D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(tr, 5.0f * Scale(), 5.0f * Scale());
            rt_->FillRoundedRectangle(pill, titleBrush_);
            rt_->DrawRoundedRectangle(pill, edgeBrush_, 1.0f);
        }
        rt_->DrawText(g.names[i].c_str(), static_cast<UINT32>(g.names[i].size()), labelFormat_, &tr,
                      on ? textBrush_ : edgeBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
}

void Box::RefreshSubFolders() {
    subNames_.clear();
    // 第二层列的永远是「盒子当前目录」下面的一层子目录，不是正在显示的那个子目录里面的内容，
    // 否则一路点下去就变成了文件浏览器，也就没有"二级"这个上限了。
    const std::wstring base = activeFolder();
    if (base.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(base, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
        subNames_.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(subNames_.begin(), subNames_.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
}

bool Box::HasSubRow() const {
    // 有一个子目录就值得多一排；已经选中的那个子目录即使被删了也要留着，
    // 好让人（或者自检）点回「本层」。
    return !subNames_.empty() || !activeSub_.empty();
}

std::vector<std::wstring> Box::SubNames() const {
    std::vector<std::wstring> out;
    out.push_back(kThisLevel);
    for (const auto& n : subNames_) out.push_back(n);
    return out;
}

std::wstring Box::displayFolder() const {
    const std::wstring base = activeFolder();
    if (activeSub_.empty()) return base;
    return JoinPath(base, activeSub_);
}

bool Box::SetSubTab(const std::wstring& name) {
    auto names = SubNames();
    auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return false;
    // 「本层」和配置里 sub 留空是同一个意思。
    const std::wstring next = (name == kThisLevel) ? std::wstring() : name;
    if (next == activeSub_) return true;
    activeSub_ = next;
    scrollRow_ = 0;
    RefreshContent();
    PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
    return true;
}

size_t Box::shownCount() const {
    if (!grouped_ || activeTab_.empty() || activeTab_ == L"全部") return content_.count();
    size_t n = 0;
    for (size_t i = 0; i < content_.count(); ++i) {
        const BoxItem& it = content_.at(i);
        if (category::CategoryOf(it.name, it.isDirectory) == activeTab_) n++;
    }
    return n;
}

bool Box::SetCategoryTab(const std::wstring& tab) {
    auto names = TabNames();
    auto it = std::find(names.begin(), names.end(), tab);
    if (it == names.end()) return false;
    if (IsAggregate()) {
        SelectMember(static_cast<size_t>(std::distance(names.begin(), it)), true);
        return true;
    }
    if (tab == activeTab_) return true;
    // 「全部」一律存成空字符串，和 layout.ini 里 tab 留空的含义保持一致。
    activeTab_ = (tab == L"全部") ? std::wstring() : tab;
    scrollRow_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
    return true;
}

void Box::SetGrouped(bool on) {
    if (grouped_ == on) return;
    grouped_ = on;
    if (!on) activeTab_.clear();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
}

std::vector<size_t> Box::VisibleIndices() const {
    std::vector<size_t> out;
    for (size_t i = 0; i < content_.count(); ++i) {
        if (!grouped_ || activeTab_.empty()) {
            out.push_back(i);
            continue;
        }
        const BoxItem& it = content_.at(i);
        if (category::CategoryOf(it.name, it.isDirectory) == activeTab_) out.push_back(i);
    }
    return out;
}

std::wstring Box::activeFolder() const {
    if (!IsAggregate()) return resolvedFolder_;
    auto it = std::find(memberNames_.begin(), memberNames_.end(), activeTab_);
    size_t idx = it == memberNames_.end() ? 0 : static_cast<size_t>(std::distance(memberNames_.begin(), it));
    return memberFolders_[idx];
}

void Box::SelectMember(size_t index, bool notify) {
    if (index >= memberFolders_.size()) return;
    activeTab_ = memberNames_[index];
    resolvedFolder_ = memberFolders_[index];
    // 换成员等于换目录，第二层是相对旧目录列出来的，留着会指到一个不相干的子文件夹。
    activeSub_.clear();
    scrollRow_ = 0;
    RefreshContent();
    if (notify) PostMessageW(TheTrayWnd(), WM_APP_LAYOUT_DIRTY, 0, 0);
}

static inline D2D1_COLOR_F ToD2D(unsigned rgb) {
    D2D1_COLOR_F c{};
    c.r = ((rgb >> 16) & 0xFF) / 255.0f;
    c.g = ((rgb >> 8) & 0xFF) / 255.0f;
    c.b = (rgb & 0xFF) / 255.0f;
    c.a = 1.0f;
    return c;
}

D2D1_COLOR_F Box::ColorOf(unsigned rgb) { return ToD2D(rgb); }

void Box::ApplyPalette() {
    pal_ = ThemeCurrent();
    if (bodyBrush_) bodyBrush_->SetColor(ToD2D(pal_.body));
    if (titleBrush_) titleBrush_->SetColor(ToD2D(pal_.title));
    if (textBrush_) textBrush_->SetColor(ToD2D(pal_.text));
    if (edgeBrush_) edgeBrush_->SetColor(ToD2D(pal_.edge));
    if (dimBrush_) dimBrush_->SetColor(ToD2D(pal_.dim));
}

void Box::BeginRename() {
    if (!hwnd_ || renaming_) return;
    renaming_ = true;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float s = Scale();
    const int m = static_cast<int>(10 * s);
    // 让出右侧收起标记那一格，编辑框盖住标题文字区域。
    const int right = rc.right - static_cast<int>(34 * s);
    edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cfg_.name.c_str(),
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, m, static_cast<int>(4 * s),
                            right - m, TitleH() - static_cast<int>(8 * s), hwnd_, nullptr, GetModuleHandleW(nullptr),
                            this);
    if (!edit_) {
        renaming_ = false;
        return;
    }
    SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // 手工子类化而不是 SetWindowSubclass：后者要 comctl32 v6 的 manifest，
    // 而这个编辑框属于本进程，直接换 WNDPROC 是合法的。
    oldEditProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    const auto RgbToRef = [](unsigned rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); };
    editBk_ = RgbToRef(pal_.body);
    editFg_ = RgbToRef(pal_.text);
    editBrush_ = CreateSolidBrush(editBk_);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, GetDpiForWindow(hwnd_));
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DeleteObject(font);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, static_cast<LPARAM>(-1));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK Box::EditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Box*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_CHAR && self) {
        if (w == VK_RETURN) {
            self->EndRename(true);
            return 0;
        }
        if (w == VK_ESCAPE) {
            self->EndRename(false);
            return 0;
        }
    }
    WNDPROC prev = self ? self->oldEditProc_ : nullptr;
    return prev ? CallWindowProcW(prev, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

void Box::EndRename(bool commit) {
    if (!renaming_) return;
    std::wstring text;
    if (edit_) {
        int len = GetWindowTextLengthW(edit_);
        text.resize(static_cast<size_t>(len));
        if (len > 0) GetWindowTextW(edit_, text.data(), len + 1);
    }
    renaming_ = false;
    if (edit_) {
        if (oldEditProc_) {
            SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oldEditProc_));
            oldEditProc_ = nullptr;
        }
        DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (editBrush_) {
        DeleteObject(editBrush_);
        editBrush_ = nullptr;
    }

    size_t b = text.find_first_not_of(L" \t\r\n");
    size_t e = text.find_last_not_of(L" \t\r\n");
    text = b == std::wstring::npos ? std::wstring() : text.substr(b, e - b + 1);
    if (commit && !text.empty() && text != cfg_.name) committedRename_ = text;
    if (hwnd_) SetFocus(hwnd_);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Box::takeRenameRequest(std::wstring& outNewName) {
    if (committedRename_.empty()) return false;
    outNewName = committedRename_;
    committedRename_.clear();
    return true;
}

void Box::MoveTo(const RECT& screenRect) {
    if (!hwnd_) return;
    const int w = screenRect.right - screenRect.left;
    const int h = screenRect.bottom - screenRect.top;
    if (w < kMinW || h < TitleH()) return;
    cfg_.rect = screenRect;
    expandedH_ = cfg_.collapsed ? expandedH_ : h;
    SetWindowPos(hwnd_, HWND_BOTTOM, screenRect.left, screenRect.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ApplyRegion();
    ClampScroll(GetMetrics());
    InvalidateRect(hwnd_, nullptr, FALSE);
}
