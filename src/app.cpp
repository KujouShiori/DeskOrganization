#include "app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <windowsx.h>

#include <dwmapi.h>

#include "desklayout.h"
#include "fileop.h"
#include "geometry.h"
#include "iconlayer.h"
#include "iconres.h"
#include "iconview.h"
#include "merge.h"
#include "organize.h"
#include "rename.h"
#include "render.h"
#include "shellmenu.h"
#include "theme.h"
#include "util.h"

static const wchar_t* kTrayClass = L"DeskOrgTray";
static constexpr UINT kTrayId = 1;
static constexpr UINT_PTR kHeartbeat = 7;

HWND TheTrayWnd() {
    App* a = App::The();
    return a ? a->trayWnd() : nullptr;
}

static std::wstring UniqueBoxName(const std::vector<std::unique_ptr<Box>>& boxes, std::wstring base) {
    int n = 1;
    std::wstring candidate = base;
    bool taken = true;
    while (taken) {
        taken = false;
        for (auto& b : boxes) {
            BoxConfig c;
            b->Current(c);
            if (c.name == candidate) {
                candidate = base + L"-" + std::to_wstring(++n);
                taken = true;
                break;
            }
        }
    }
    return candidate;
}

// 盒子的目录解析：绝对路径原样用，相对路径按 box_root 解析，没写就用 box_root 下同名片段。
// 解析结果只给运行时用，写回 layout.ini 的仍然是用户原本写的相对路径，这样盘符不会被固化。
static std::vector<std::wstring> SplitCsv(const std::wstring& s);

static std::wstring ResolveBoxFolder(const BoxConfig& cfg, const std::wstring& root) {
    const std::wstring& base = root.empty() ? DesktopPath() : root;
    if (cfg.folder.empty()) return JoinPath(base, cfg.name);
    if (cfg.folder.size() > 1 && cfg.folder[1] == L':') return cfg.folder;
    if (cfg.folder.front() == L'\\') return cfg.folder;
    return JoinPath(base, cfg.folder);
}

bool App::Init() {
    the_ = this;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    // 上一次如果是被强杀的，图标层可能还藏着，先把用户的桌面恢复回来。
    IconLayerSelfHeal();
    // 撤销栈跨重启：把上一次运行留下的记录读回来。
    UndoHistoryLoad();

    if (!RenderInit() || !WicInit()) {
        Log(L"RenderInit 失败，直接退出");
        return false;
    }
    if (!Box::Register(hi)) return false;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) {
        Log(L"注册托盘窗口类失败 gle=%lu", GetLastError());
        return false;
    }

    tray_ = CreateWindowExW(0, kTrayClass, L"DeskOrganization", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, hi, nullptr);
    if (!tray_) {
        Log(L"创建托盘宿主窗口失败 gle=%lu", GetLastError());
        return false;
    }

    bool loaded = LayoutLoad(layout_);
    boxRoot_ = layout_.boxRoot;
    ThemeSetMode(ThemeParse(layout_.theme));
    if (!loaded || layout_.boxes.empty()) {
        layout_.boxes.clear();
        BoxConfig c{};
        c.name = L"文档";
        c.rect = RECT{40, 40, 380, 300};
        layout_.boxes.push_back(c);
    }

    if (!AttachToDesktop()) {
        Log(L"找不到 Progman，桌面层挂不上");
        return false;
    }
    RebuildBoxes();
    // 上次保存的坐标可能是另一台显示器、另一个分辨率下的，先拉回来再显示。
    ClampBoxesToScreens(L"启动时发现盒子在屏幕外");

    if (layout_.hideIcons) IconLayerSet(true);

    // 首次运行立刻落盘，用户马上就能有一个可手改的 layout.ini。
    if (!loaded) {
        SyncLayoutFromBoxes();
        LayoutSave(layout_);
    }

    AddTray();

    timer_ = SetTimer(tray_, kHeartbeat, 1500, nullptr);
    Log(L"启动完成，盒子数=%zu", boxes_.size());
    return true;
}

void App::Run() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void App::Shutdown() {
    if (timer_) KillTimer(tray_, kHeartbeat);
    timer_ = 0;
    RemoveTray();
    if (iconOwned_ && icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
        iconOwned_ = false;
    }
    // 顺序要紧：SyncLayoutFromBoxes 是"把还活着的盒子抄回布局"，它先 clear 再按窗口重建。
    // 放在 Destroy 之后的话，退出的那一刻窗口全没了，抄回来的就是空表，
    // 接着 LayoutSave 会把用户所有盒子的定义从 layout.ini 里抹掉。
    SyncLayoutFromBoxes();
    for (auto& b : boxes_) b->Destroy();
    boxes_.clear();
    // 干净退出就把用户的桌面图标恢复；下次启动再按 hide_icons 设置隐藏。
    IconLayerSet(false);
    layout_.hideIcons = false;
    LayoutSave(layout_);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    Box::Unregister(hi);
    UnregisterClassW(kTrayClass, hi);
    WicShutdown();
    RenderShutdown();
    if (tray_) DestroyWindow(tray_);
    tray_ = nullptr;
    the_ = nullptr;
}

bool App::AttachToDesktop() {
    progman_ = FindWindowW(L"Progman", nullptr);
    defView_ = FindWindowExW(progman_, nullptr, L"SHELLDLL_DefView", nullptr);
    Log(L"AttachToDesktop progman=%p defView=%p", static_cast<void*>(progman_), static_cast<void*>(defView_));
    return progman_ != nullptr;
}

void App::RebuildBoxes() {
    for (auto& b : boxes_) b->Destroy();
    boxes_.clear();
    Layout snapshot = layout_;
    for (auto& c : snapshot.boxes) CreateOneBox(c);
    ApplyMembershipVisibility();
}

// 成员盒子要按它自己的配置解析出目录，所以建多标签盒子时得先把成员名单备齐。
std::vector<std::pair<std::wstring, std::wstring>> App::MemberTargets(const BoxConfig& cfg) const {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto& m : SplitCsv(cfg.members)) {
        if (_wcsicmp(m.c_str(), cfg.name.c_str()) == 0) continue;
        auto it = std::find_if(layout_.boxes.begin(), layout_.boxes.end(),
                               [&](const BoxConfig& b) { return _wcsicmp(b.name.c_str(), m.c_str()) == 0; });
        if (it == layout_.boxes.end()) {
            Log(L"盒子 %s 的成员 %s 在布局里找不到，已跳过", cfg.name.c_str(), m.c_str());
            continue;
        }
        out.emplace_back(SanitizeBoxName(m), ResolveBoxFolder(*it, boxRoot_));
    }
    return out;
}

void App::CreateOneBox(BoxConfig cfg) {
    std::wstring resolved = ResolveBoxFolder(cfg, boxRoot_);
    if (!EnsureDirExists(resolved)) {
        // box_root 手写错时（比如漏了反斜杠变成 C:xxx）不能将错就错，退回桌面并说清楚。
        Log(L"盒子目录创建失败，退回桌面：%s", resolved.c_str());
        resolved = JoinPath(DesktopPath(), cfg.name.empty() ? L"盒子" : cfg.name);
        if (!EnsureDirExists(resolved)) {
            Log(L"退回桌面也建不出来，跳过盒子 %s", cfg.name.c_str());
            return;
        }
    }
    auto box = std::make_unique<Box>();
    if (!box->Create(cfg, resolved, MemberTargets(cfg))) return;
    boxes_.push_back(std::move(box));
}

BoxConfig App::NextDefaultBox() {
    BoxConfig c{};
    c.name = UniqueBoxName(boxes_, L"新盒子");
    int offset = static_cast<int>(boxes_.size()) * 34;
    c.rect = RECT{40 + offset, 60 + offset, 40 + offset + 340, 60 + offset + 260};
    return c;
}

void App::SyncLayoutFromBoxes() {
    layout_.boxes.clear();
    for (auto& b : boxes_) {
        if (!b->alive()) continue;
        BoxConfig c{};
        b->Current(c);
        layout_.boxes.push_back(c);
    }
}

void App::DeleteBoxByHwnd(HWND h) {
    for (auto it = boxes_.begin(); it != boxes_.end(); ++it) {
        if ((*it)->hwnd() == h) {
            (*it)->Destroy();
            boxes_.erase(it);
            SyncLayoutFromBoxes();
            LayoutSave(layout_);
            return;
        }
    }
}

void App::OnHeartbeat() {
    // 盒子是顶层窗口，不依赖 Progman 存活，这里做两件事：
    // 盒子被外力销毁就重建；每隔几拍同步一次目录内容。
    bool anyDead = false;
    for (auto& b : boxes_) {
        if (!b->alive()) { anyDead = true; break; }
    }
    if (anyDead) {
        Log(L"检测到盒子窗口丢失，重建");
        RebuildBoxes();
        return;
    }

    if (++tick_ % 3 == 0) {
        for (auto& b : boxes_) b->RefreshContent();
        RunRules();
    }
}

void App::AddTray() {
    icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = tray_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon_;
    lstrcpynW(nid.szTip, L"桌面整理 DeskOrganization", _countof(nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) Log(L"托盘图标添加失败 gle=%lu", GetLastError());
    // 上面那份是兜底图标；能自己画就换成按当前配色生成的那一个。
    if (!RefreshTrayIcon()) Log(L"托盘图标未能换成自绘图标，继续使用系统默认图标");
}

void App::RemoveTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = tray_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void App::ShowTrayMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, CmdNewBox, L"新建盒子");
    AppendMenuW(menu, MF_STRING, CmdToggleVisible, hidden_ ? L"显示所有盒子" : L"隐藏所有盒子");
    AppendMenuW(menu, MF_STRING, CmdSaveNow, L"立即保存布局");
    AppendMenuW(menu, MF_STRING, CmdToggleIcons, IconLayerIsHidden() ? L"显示桌面图标" : L"隐藏桌面图标");
    std::wstring undoLabel;
    if (UndoHistoryLastLabel(undoLabel)) {
        AppendMenuW(menu, MF_STRING, CmdUndo, (L"撤销：" + undoLabel).c_str());
    } else {
        AppendMenuW(menu, MF_GRAYED, CmdUndo, L"没有可撤销的操作");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!layout_.rules.empty()) {
        AppendMenuW(menu, MF_STRING, CmdRunRules, L"按规则整理（只处理新文件）");
        AppendMenuW(menu, MF_STRING, CmdSweepNow, L"按规则整理现存全部文件");
    }
    AppendMenuW(menu, MF_STRING, CmdBackupLayout, L"备份桌面图标布局");
    if (NewestBackup().empty()) {
        AppendMenuW(menu, MF_GRAYED, CmdRestoreLayout, L"还原桌面图标布局（无备份）");
    } else {
        AppendMenuW(menu, MF_STRING, CmdRestoreLayout, L"还原桌面图标布局（最近一次）");
    }
    std::wstring shellCmd;
    const bool shellOn = shellmenu::IsInstalled(shellCmd);
    AppendMenuW(menu, MF_STRING, CmdToggleShellMenu,
                shellOn ? L"从桌面右键菜单移除撤销入口" : L"加入桌面右键菜单（撤销入口）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdQuit, L"退出");

    SetForegroundWindow(tray_);
    int id = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,
                            screenPt.x, screenPt.y, 0, tray_, nullptr);
    PostMessageW(tray_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (id) OnCommand(id);
}

void App::OnCommand(int id) {
    switch (id) {
        case CmdNewBox: {
            CreateOneBox(NextDefaultBox());
            SyncLayoutFromBoxes();
            LayoutSave(layout_);
            break;
        }
        case CmdToggleVisible: {
            hidden_ = !hidden_;
            ApplyMembershipVisibility();
            break;
        }
        case CmdSaveNow: {
            SyncLayoutFromBoxes();
            LayoutSave(layout_);
            break;
        }
        case CmdToggleIcons: {
            bool hide = !IconLayerIsHidden();
            if (IconLayerSet(hide)) {
                layout_.hideIcons = hide;
                LayoutSave(layout_);
            }
            break;
        }
        case CmdToggleShellMenu: {
            std::wstring installed;
            std::wstring err;
            if (shellmenu::IsInstalled(installed)) shellmenu::Uninstall(err);
            else if (!shellmenu::Install(err)) Log(L"安装桌面右键入口失败：%s", err.c_str());
            break;
        }
        case CmdRunRules:
            RunRules(false);
            break;
        case CmdSweepNow:
            RunRules(true);
            break;
        case CmdBackupLayout: {
            std::wstring err;
            if (!DesktopLayoutBackup(NewBackupPath(), err)) {
                Log(L"备份桌面布局失败：%s", err.c_str());
            }
            break;
        }
        case CmdRestoreLayout: {
            std::wstring newest = NewestBackup();
            if (newest.empty()) break;
            std::wstring err;
            if (!DesktopLayoutRestore(newest, err)) Log(L"还原桌面布局失败：%s", err.c_str());
            break;
        }
        case CmdUndo: {
            std::wstring err;
            if (!UndoHistoryUndoLast(tray_, err)) Log(L"撤销失败：%s", err.c_str());
            for (auto& b : boxes_) b->RefreshContent();
            break;
        }
        case CmdQuit: {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof nid;
            nid.hWnd = tray_;
            nid.uID = kTrayId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            break;
        }
        default:
            break;
    }
}

LRESULT CALLBACK App::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(App::The()));
        return DefWindowProcW(h, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
        case WM_APP_TRAY: {
            UINT ev = static_cast<UINT>(LOWORD(lp));
            if (ev == WM_LBUTTONUP) {
                self->OnCommand(CmdToggleVisible);
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                POINT pt{};
                GetCursorPos(&pt);
                self->ShowTrayMenu(pt);
            } else if (ev == WM_LBUTTONDBLCLK) {
                self->OnCommand(CmdNewBox);
            }
            return 0;
        }
        case WM_APP_LAYOUT_DIRTY:
            self->SyncLayoutFromBoxes();
            LayoutSave(self->layout_);
            return 0;
        case WM_APP_MERGE_MENU: {
            // 只传句柄和锚点：名字在这里查得到，不必让一个堆对象跨消息活着。
            HWND which = reinterpret_cast<HWND>(wp);
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (auto& b : self->boxes_) {
                if (b->hwnd() != which) continue;
                BoxConfig c;
                b->Current(c);
                self->ShowMergeMenu(c.name, pt);
                break;
            }
            return 0;
        }
        case WM_APP_RENAME_BOX: {
            HWND which = reinterpret_cast<HWND>(wp);
            for (auto& b : self->boxes_) {
                if (b->hwnd() != which) continue;
                BoxConfig c;
                b->Current(c);
                std::wstring proposed;
                if (b->takeRenameRequest(proposed)) self->RenameBoxTo(c.name, proposed);
                break;
            }
            return 0;
        }
        case WM_APP_UNDO_NOW: {
            std::wstring err;
            std::wstring label;
            UndoHistoryLastLabel(label);
            if (UndoHistoryUndoLast(self->tray_, err)) {
                Log(L"来自右键菜单的撤销完成：%s", label.c_str());
                for (auto& b : self->boxes_) b->RefreshContent();
            } else {
                Log(L"来自右键菜单的撤销失败：%s", err.c_str());
            }
            return 0;
        }
        case WM_APP_CONTENT_DIRTY:
            for (auto& b : self->boxes_) b->RefreshContent();
            return 0;
        case WM_APP_BOX_DELETE:
            self->DeleteBoxByHwnd(reinterpret_cast<HWND>(wp));
            return 0;
        case WM_SETTINGCHANGE: {
            // 深浅色切换也是这条广播，托盘图标跟着换一次配色。
            auto* area = reinterpret_cast<const wchar_t*>(lp);
            if (area && _wcsicmp(area, L"ImmersiveColorSet") == 0) self->RefreshTrayIcon();
            return 0;
        }
        case WM_TIMER:
            if (wp == kHeartbeat) self->OnHeartbeat();
            return 0;
        case WM_DISPLAYCHANGE:
            self->ClampBoxesToScreens(L"显示设置变化");
            self->OnHeartbeat();
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

// ---------------- 自检 ----------------

// 只实现自检要用的那部分 IDataObject：提供一个 CF_HDROP，走真实的 COM 接口交给被测代码。
class TestDataObject : public IDataObject {
 public:
    explicit TestDataObject(HGLOBAL h) : hglobal_(h) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDataObject)) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        long n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    STDMETHODIMP GetData(FORMATETC* pfe, STGMEDIUM* pmed) override {
        if (!pfe || !pmed) return E_INVALIDARG;
        if (pfe->cfFormat != static_cast<CLIPFORMAT>(CF_HDROP) || !(pfe->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        *pmed = {};
        pmed->tymed = TYMED_HGLOBAL;
        // 所有权一并交出，调用方 ReleaseStgMedium 时会把这块 HGLOBAL 释放掉。
        pmed->hGlobal = hglobal_;
        hglobal_ = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* pfe) override {
        return (pfe && pfe->cfFormat == static_cast<CLIPFORMAT>(CF_HDROP) && (pfe->tymed & TYMED_HGLOBAL))
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* pOut) override {
        if (!pOut) return E_INVALIDARG;
        *pOut = {};
        return DATA_E_FORMATETC;
    }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP DUnadvise(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

 private:
    ~TestDataObject() = default;
    long refs_ = 1;
    HGLOBAL hglobal_ = nullptr;
};

static HGLOBAL MakeHDrop(const std::vector<std::wstring>& paths) {
    size_t nameBytes = sizeof(wchar_t);  // 结尾多一个空字符
    for (const auto& p : paths) nameBytes += (p.size() + 1) * sizeof(wchar_t);

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + nameBytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    auto* out = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& p : paths) {
        wcscpy_s(out, p.size() + 1, p.c_str());
        out += p.size() + 1;
    }
    *out = 0;
    GlobalUnlock(h);
    return h;
}

static DWORD g_cloaked = 0;

static void RemoveDirIfExists(const std::wstring& dir) {
    // 测试自己造的目录要能收干净。
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = JoinPath(dir, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirIfExists(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static void TouchFile(const std::wstring& path, const char* content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, content, static_cast<DWORD>(strlen(content)), &w, nullptr);
    CloseHandle(h);
}

struct Check { const char* name; bool ok; std::string detail; };

void AddRuleChecks(std::vector<Check>& checks);
void AddUndoPersistenceChecks(std::vector<Check>& checks, const std::wstring& path);
void AddCategoryChecks(std::vector<Check>& checks, HINSTANCE hi);
void AddMultiTabChecks(std::vector<Check>& checks, HINSTANCE hi);
void AddThemeChecks(std::vector<Check>& checks, HINSTANCE hi);
void AddShellMenuChecks(std::vector<Check>& checks);
void AddHoldChecks(std::vector<Check>& checks);
void AddRenameChecks(std::vector<Check>& checks);
void AddInlineRenameChecks(std::vector<Check>& checks, HINSTANCE hi);
void AddGeometryChecks(std::vector<Check>& checks);
void AddMergeChecks(std::vector<Check>& checks);
void AddIconChecks(std::vector<Check>& checks);

int AppSelfTest() {
    std::vector<Check> checks;
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
        // 逐条落盘：万一自检卡住，日志的最后一条就是卡住的位置。
        Log(L"selftest check %zu: %hs -> %s", checks.size(), n, ok ? L"PASS" : L"FAIL");
    };

    HINSTANCE hi = GetModuleHandleW(nullptr);

    // 多处断言是拿具体像素值比对的，不固定主题就会随用户系统是深色还是浅色而变红，
    // 那种红是环境噪声，会把真正的渲染问题淹掉。主题本身由 AddThemeChecks 专门验。
    ThemeSetMode(ThemeMode::Dark);

    add("render_init", RenderInit());
    add("wic_init", WicInit());
    add("box_class", Box::Register(hi));

    HWND progman = FindWindowW(L"Progman", nullptr);
    add("progman_found", progman != nullptr);

    Box box;
    BoxConfig cfg{};
    cfg.name = L"自检盒子";
    cfg.rect = RECT{60, 60, 60 + 340, 60 + 260};
    // 这个盒子故意指向一个不存在的目录，让前面的像素断言落在纯填充区上。
    add("box_create", box.Create(cfg, AppFile(L"no_such_dir")));

    PumpMessages(900);

    add("box_alive", box.alive());
    add("box_is_toplevel", GetParent(box.hwnd()) == nullptr, "顶层窗口才会真的显示，Progman 子窗口实测不显示");
    add("box_visible", box.visible());
    add("box_painted", box.painted(), "Direct2D 至少成功完成一次 EndDraw");
    add("drop_registered", box.dropRegistered(), "OleInitialize + RegisterDragDrop 都通了");

    RECT rc{};
    GetWindowRect(box.hwnd(), &rc);
    add("position_exact", rc.left == 60 && rc.top == 60,
        "expected 60,60 got " + std::to_string(rc.left) + "," + std::to_string(rc.top));

    POINT probe{rc.left + 40, rc.top + 40};
    add("hit_test_ours", WindowFromPoint(probe) == box.hwnd());

    // 被 DWM cloak 的窗口：IsWindowVisible、WindowFromPoint、EndDraw 全部正常，唯独屏幕不画。
    // 没有这条断言，下面的像素检查会在"看不见"的情况下给出误导性结论。
    DWORD cloaked = 0;
    DwmGetWindowAttribute(box.hwnd(), DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    g_cloaked = cloaked;
    add("window_not_cloaked", cloaked == 0, "cloaked=" + std::to_string(cloaked));

    // z-order 断言：EnumWindows 自顶向下枚举，我们的序号必须比 Progman 小，才谈得上压在图标层之上。
    {
        struct Ctx { HWND mine; HWND progman; int minePos; int progmanPos; int i; } ctx{box.hwnd(), progman, -1, -1, 0};
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            auto c = reinterpret_cast<Ctx*>(l);
            if (h == c->mine) c->minePos = c->i;
            if (h == c->progman) c->progmanPos = c->i;
            c->i++;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        add("above_progman_in_zorder", ctx.minePos >= 0 && ctx.progmanPos >= 0 && ctx.minePos < ctx.progmanPos,
            "box@" + std::to_string(ctx.minePos) + " progman@" + std::to_string(ctx.progmanPos));
    }

    // 采样屏幕像素：painted 只说明 EndDraw 成功，这里验证屏幕上真的画出了两种不同区域。
    {
        box.SetAlpha(255);
        PumpMessages(150);
        HDC screen = GetDC(nullptr);
        // 每个采样点都要单独验命中：只验一个点的话，一个恰好压在中间边缘的别的应用窗口
        // 会让颜色读数完全失真，而命中断言依然是 PASS。
        const POINT bodyPt{rc.left + 150, rc.top + 60};
        const POINT titlePt{rc.left + 150, rc.top + 10};
        bool bodyOwned = WindowFromPoint(bodyPt) == box.hwnd();
        bool titleOwned = WindowFromPoint(titlePt) == box.hwnd();
        COLORREF body = GetPixel(screen, bodyPt.x, bodyPt.y);
        COLORREF title = GetPixel(screen, titlePt.x, titlePt.y);
        ReleaseDC(nullptr, screen);
        add("sample_points_are_ours", bodyOwned && titleOwned,
            "body point owned=" + std::to_string(bodyOwned) + " title point owned=" + std::to_string(titleOwned));
        char d[160];
        snprintf(d, sizeof d, "body=%02X%02X%02X title=%02X%02X%02X cloaked=%lu", GetRValue(body), GetGValue(body),
                 GetBValue(body), GetRValue(title), GetGValue(title), GetBValue(title),
                 static_cast<unsigned long>(g_cloaked));
        add("pixels_readable", body != CLR_INVALID && title != CLR_INVALID, d);
        add("title_differs_from_body",
            bodyOwned && titleOwned && abs(GetRValue(body) - GetRValue(title)) > 10 &&
                abs(GetBValue(body) - GetBValue(title)) > 10,
            d);
        add("body_is_blueish", bodyOwned && GetBValue(body) > GetRValue(body), d);
        // 不透明时内容区应当正好是我们的填充色 (0.13,0.16,0.21) 即 (33,41,54)。
        auto closeTo = [](int v, int expect, int tol) { return abs(v - expect) <= tol; };
        add("body_matches_intended_fill",
            bodyOwned && closeTo(GetRValue(body), 33, 24) && closeTo(GetGValue(body), 41, 24) &&
                closeTo(GetBValue(body), 54, 24),
            d);

        // 半透明真的生效了吗：换成默认 alpha 后同一个点应当被壁纸混合掉。
        box.SetAlpha(Box::kDefaultAlpha);
        PumpMessages(150);
        HDC s2 = GetDC(nullptr);
        bool stillOwned = WindowFromPoint(bodyPt) == box.hwnd();
        COLORREF blended = GetPixel(s2, bodyPt.x, bodyPt.y);
        ReleaseDC(nullptr, s2);
        bool moved = abs(GetRValue(blended) - GetRValue(body)) > 4 || abs(GetBValue(blended) - GetBValue(body)) > 4;
        char d2[96];
        snprintf(d2, sizeof d2, "opaque=(%d,%d,%d) alpha=%d -> (%d,%d,%d)", GetRValue(body), GetGValue(body),
                 GetBValue(body), Box::kDefaultAlpha, GetRValue(blended), GetGValue(blended), GetBValue(blended));
        add("translucency_active", stillOwned && moved, d2);
        box.SetAlpha(255);
    }

    // 图标层隐藏：藏掉原生图标时，我们的盒子必须还在。
    {
        HWND progman2 = FindWindowW(L"Progman", nullptr);
        HWND defView = FindWindowExW(progman2, nullptr, L"SHELLDLL_DefView", nullptr);
        add("defview_found", defView != nullptr);
        if (defView) {
            add("iconlayer_hide", IconLayerSet(true) && !IsWindowVisible(defView));
            add("box_survives_icon_hide", box.visible() && box.painted());
            add("hide_marker_written",
                GetFileAttributesW(AppFile(L"icons_hidden.marker").c_str()) != INVALID_FILE_ATTRIBUTES);
            add("iconlayer_show", IconLayerSet(false) && IsWindowVisible(defView));
            add("marker_cleared",
                GetFileAttributesW(AppFile(L"icons_hidden.marker").c_str()) == INVALID_FILE_ATTRIBUTES);
        }
        // 无论上面结果如何，都把用户的桌面恢复原样。
        if (defView) ShowWindow(defView, SW_SHOW);
        DeleteFileW(AppFile(L"icons_hidden.marker").c_str());
        add("iconlayer_restored", defView && IsWindowVisible(defView));
    }

    box.Show(false);
    PumpMessages(150);
    add("hide_works", !box.visible());
    box.Show(true);
    PumpMessages(150);
    add("show_works", box.visible());

    box.Destroy();
    add("destroyed", !box.alive());

    // 内容渲染：造一个临时目录放三个已知文件，确认盒子真的把它们画出来了。
    {
        const std::wstring tmp = AppFile(L"contenttest");
        EnsureDirExists(tmp);
        const wchar_t* names[] = {L"alpha.txt", L"beta.png", L"中文文档.txt"};
        for (const wchar_t* n : names) {
            HANDLE hf = CreateFileW(JoinPath(tmp, n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(hf, "x", 1, &w, nullptr);
                CloseHandle(hf);
            }
        }

        Box cbox;
        BoxConfig ccfg{};
        ccfg.name = L"内容测试";
        // 写成相对路径，顺带验证 box_root 解析。
        ccfg.folder = L"contenttest";
        std::wstring resolved = ResolveBoxFolder(ccfg, AppDir());
        add("resolve_relative_folder", resolved == tmp, WideToUtf8(resolved));
        // 放在第一个盒子已经验证过可以采样的同一坐标，排除"被别的应用窗口盖住"这个变量。
        ccfg.rect = RECT{60, 60, 60 + 340, 60 + 260};
        add("content_box_create", cbox.Create(ccfg, resolved));
        cbox.SetAlpha(255);
        PumpMessages(700);

        add("content_item_count", cbox.itemCount() == 3, "got " + std::to_string(cbox.itemCount()));
        add("content_icons_loaded", cbox.iconCount() == 3, "got " + std::to_string(cbox.iconCount()));
        cbox.RefreshContent();
        add("content_bitmap_uploaded", cbox.uploadedCount() == 3, "got " + std::to_string(cbox.uploadedCount()));

        // 网格度量：padX=10 iconPx=44 cellW=88 cellH=70 titleH=30 padTop=10
        // 第一格图标的中心大约在 (10+22+22, 30+10+22)。
        HDC screen = GetDC(nullptr);
        const POINT iconPt{60 + 54, 60 + 62};
        const POINT fillPt{60 + 320, 60 + 60};
        bool iconOwned = WindowFromPoint(iconPt) == cbox.hwnd();
        bool fillOwned = WindowFromPoint(fillPt) == cbox.hwnd();
        COLORREF iconPix = GetPixel(screen, iconPt.x, iconPt.y);
        COLORREF fillPix = GetPixel(screen, fillPt.x, fillPt.y);
        ReleaseDC(nullptr, screen);

        RECT crc{};
        GetWindowRect(cbox.hwnd(), &crc);
        POINT iconProbe = iconPt;
        char d[256];
        snprintf(d, sizeof d, "rect=[%ld,%ld %ldx%ld] painted=%d hit=%p icon=%08lX fill=%08lX owned=%d/%d", crc.left,
                 crc.top, crc.right - crc.left, crc.bottom - crc.top, cbox.painted() ? 1 : 0,
                 static_cast<void*>(WindowFromPoint(iconProbe)), static_cast<unsigned long>(iconPix),
                 static_cast<unsigned long>(fillPix), iconOwned ? 1 : 0, fillOwned ? 1 : 0);
        add("content_pixels_readable", iconPix != CLR_INVALID && fillPix != CLR_INVALID, d);
        add("content_box_painted", cbox.painted(), d);
        add("content_hit_is_cbox", WindowFromPoint(probe) == cbox.hwnd(), d);
        add("empty_area_is_plain_fill",
            fillOwned && abs(GetRValue(fillPix) - 33) <= 24 && abs(GetGValue(fillPix) - 41) <= 24 &&
                abs(GetBValue(fillPix) - 54) <= 24, d);
        add("icon_pixel_differs_from_fill",
            iconOwned && abs(GetRValue(iconPix) - GetRValue(fillPix)) + abs(GetGValue(iconPix) - GetGValue(fillPix)) +
                abs(GetBValue(iconPix) - GetBValue(fillPix)) >
                30,
            d);

        cbox.Destroy();
        for (const wchar_t* n : names) DeleteFileW(JoinPath(tmp, n).c_str());
        RemoveDirectoryW(tmp.c_str());
    }


    // 拖入 -> 真实移动 -> 撤销。这是任务 8 的核心，全部在临时目录里做，不碰用户的文件。
    {
        const std::wstring srcDir = AppFile(L"drop_src");
        const std::wstring boxDir = AppFile(L"drop_box");
        EnsureDirExists(srcDir);
        EnsureDirExists(boxDir);

        std::wstring a = JoinPath(srcDir, L"报告.txt");
        std::wstring b = JoinPath(srcDir, L"b.png");
        std::wstring occupier = JoinPath(boxDir, L"报告.txt");
        TouchFile(a, "aaa");
        TouchFile(b, "bbb");
        TouchFile(occupier, "occupied");

        std::vector<MovedEntry> moves;
        std::wstring err;
        bool moved = MoveFilesTo(std::vector<std::wstring>{a, b}, boxDir, nullptr, moves, err);
        add("fileop_move_ok", moved, WideToUtf8(err));
        add("fileop_src_gone", !PathExists(a) && !PathExists(b));
        add("fileop_collision_renamed",
            moves.size() == 2 && FileNameOf(moves[0].to) == L"报告 (2).txt",
            moves.empty() ? std::string("<none>") : WideToUtf8(moves[0].to));
        add("fileop_box_has_both",
            PathExists(JoinPath(boxDir, L"报告 (2).txt")) && PathExists(JoinPath(boxDir, L"b.png")));
        add("fileop_existing_untouched", PathExists(occupier));

        // 同一次拖入里两个同名文件也不能互相覆盖。
        std::wstring dup1 = JoinPath(AppFile(L"drop_src2"), L"重名.txt");
        std::wstring dup2 = JoinPath(AppFile(L"drop_src3"), L"重名.txt");
        EnsureDirExists(ParentOf(dup1));
        EnsureDirExists(ParentOf(dup2));
        TouchFile(dup1, "1");
        TouchFile(dup2, "2");
        std::vector<MovedEntry> dupMoves;
        std::wstring dupErr;
        bool dupOk = MoveFilesTo(std::vector<std::wstring>{dup1, dup2}, boxDir, nullptr, dupMoves, dupErr);
        add("fileop_batch_no_self_collision",
            dupOk && dupMoves.size() == 2 && dupMoves[0].to != dupMoves[1].to,
            dupErr.empty() ? WideToUtf8(dupMoves.empty() ? L"" : dupMoves[1].to) : WideToUtf8(dupErr));

        std::wstring undoErr;
        bool undone = UndoMoves(moves, nullptr, undoErr);
        add("fileop_undo_ok", undone, WideToUtf8(undoErr));
        add("fileop_undo_restored",
            PathExists(a) && PathExists(b) && !PathExists(JoinPath(boxDir, L"报告 (2).txt")) &&
                !PathExists(JoinPath(boxDir, L"b.png")));

        std::wstring dupUndoErr;
        UndoMoves(dupMoves, nullptr, dupUndoErr);

        // HDROP 解析走真实 IDataObject 接口。
        HGLOBAL hg = MakeHDrop(std::vector<std::wstring>{a, b});
        add("hdrop_built", hg != nullptr);
        IDataObject* dataObject = hg ? new TestDataObject(hg) : nullptr;
        std::vector<std::wstring> parsed;
        bool extracted = dataObject && ExtractDroppedPaths(dataObject, parsed);
        add("hdrop_extract_count", extracted && parsed.size() == 2, "got " + std::to_string(parsed.size()));
        add("hdrop_extract_accurate", extracted && parsed == std::vector<std::wstring>{a, b});
        if (dataObject) dataObject->Release();
        else if (hg) GlobalFree(hg);

        DeleteFileW(occupier.c_str());
        DeleteFileW(a.c_str());
        DeleteFileW(b.c_str());
        DeleteFileW(dup1.c_str());
        DeleteFileW(dup2.c_str());
        for (const wchar_t* d : {L"drop_src", L"drop_src2", L"drop_src3", L"drop_box"}) {
            RemoveDirectoryW(AppFile(d).c_str());
        }
    }


    // 桌面布局备份：这里只做读取与文件往返，不移动任何图标。
    // 真正"移出去再移回来"的验证在 --layouttest 里，因为它会动到用户的桌面。
    {
        add("iconview_available", IconViewAvailable());

        std::vector<IconPosition> items;
        std::wstring err;
        bool read = IconViewRead(items, err);
        add("iconview_read", read && !items.empty(), read ? "count=" + std::to_string(items.size()) : WideToUtf8(err));

        bool named = read;
        for (const auto& it : items) {
            if (it.name.empty()) named = false;
        }
        add("iconview_all_named", named);

        RECT virt = GetSystemMetrics(SM_XVIRTUALSCREEN) == 0 && GetSystemMetrics(SM_YVIRTUALSCREEN) == 0
                        ? RECT{0, 0, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)}
                        : RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                               0, 0};
        (void)virt;
        bool sane = read;
        for (const auto& it : items) {
            if (it.x < -4000 || it.x > 8000 || it.y < -4000 || it.y > 8000) sane = false;
        }
        add("iconview_coords_sane", sane,
            items.empty() ? std::string("<none>")
                          : "first=(" + std::to_string(items[0].x) + "," + std::to_string(items[0].y) + ")");

        const std::wstring file = AppFile(L"layout_probe.desklayout");
        bool backed = DesktopLayoutBackup(file, err);
        add("layout_backup_ok", backed, WideToUtf8(err));

        std::string raw;
        bool got = ReadFileBytes(file, raw);
        int need = got ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                             static_cast<int>(raw.size()), nullptr, 0)
                       : 0;
        add("layout_backup_is_utf8", need > 0, "bytes=" + std::to_string(raw.size()));

        size_t peekedCount = 0;
        bool peek = DesktopLayoutPeek(file, peekedCount, err);
        add("layout_peek_matches_live", peek && peekedCount == items.size(),
            std::to_string(peekedCount) + " vs " + std::to_string(items.size()));

        DeleteFileW(file.c_str());
    }

    // 布局往返：中文名、坐标、收起状态、folder 都要能原样读回。
    Layout src{};
    BoxConfig a{};
    a.name = L"文档与图片";
    a.rect = RECT{10, 20, 10 + 345, 20 + 267};
    a.collapsed = true;
    a.folder = L"Desktop\\文档";
    BoxConfig b{};
    b.name = L"截图";
    b.rect = RECT{400, 500, 400 + 200, 500 + 150};
    src.boxes.push_back(a);
    src.boxes.push_back(b);
    src.boxRoot = AppDir();
    src.hideIcons = true;

    const std::wstring tmp = AppFile(L"layout_roundtrip.ini");
    add("layout_save", LayoutSaveTo(src, tmp));

    // 编码断言：GBK 写出的中文在严格 UTF-8 解码下必然失败，这条专门用来抓编码跑偏。
    std::string iniBytes;
    if (ReadFileBytes(tmp, iniBytes)) {
        int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, iniBytes.data(),
                                       static_cast<int>(iniBytes.size()), nullptr, 0);
        add("layout_is_utf8", need > 0, "bytes=" + std::to_string(iniBytes.size()));
    } else {
        add("layout_is_utf8", false, "cannot read back");
    }

    Layout dst{};
    add("layout_load", LayoutLoadFrom(dst, tmp));
    add("layout_count", dst.boxes.size() == 2, "got " + std::to_string(dst.boxes.size()));
    if (dst.boxes.size() == 2) {
        add("layout_name", dst.boxes[0].name == a.name, WideToUtf8(dst.boxes[0].name));
        const RECT& r = dst.boxes[0].rect;
        add("layout_rect", r.left == 10 && r.top == 20 && (r.right - r.left) == 345 && (r.bottom - r.top) == 267,
            std::to_string(r.left) + "," + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + "x" +
                std::to_string(r.bottom - r.top));
        add("layout_collapsed", dst.boxes[0].collapsed && !dst.boxes[1].collapsed);
        add("layout_folder", dst.boxes[0].folder == a.folder, WideToUtf8(dst.boxes[0].folder));
        add("layout_box_root", dst.boxRoot == src.boxRoot, WideToUtf8(dst.boxRoot));
        add("layout_hide_icons_roundtrip", dst.hideIcons);
        add("layout_second_name", dst.boxes[1].name == b.name, WideToUtf8(dst.boxes[1].name));
    }

    // 用户手写的注释必须活过下一次保存。
    {
        std::string withComment = iniBytes;
        const std::string marker = "; \xE8\xBF\x99\xE6\x98\xAF\xE7\x94\xA8\xE6\x88\xB7\xE6\xB3\xA8\xE9\x87\x8A";
        withComment += marker;
        withComment += "\r\n";
        WriteFileBytes(tmp, withComment);

        Layout reloaded{};
        LayoutLoadFrom(reloaded, tmp);
        BoxConfig extra{};
        extra.name = L"第三个盒子";
        extra.rect = RECT{50, 50, 50 + 200, 50 + 200};
        reloaded.boxes.push_back(extra);
        LayoutSaveTo(reloaded, tmp);

        std::string after;
        ReadFileBytes(tmp, after);
        add("comment_survives_save", after.find(marker) != std::string::npos);
        int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, after.data(),
                                       static_cast<int>(after.size()), nullptr, 0);
        add("still_utf8_after_edit", need > 0);

        Layout finalCheck{};
        bool reloaded3 = LayoutLoadFrom(finalCheck, tmp);
        add("three_boxes_after_edit", reloaded3 && finalCheck.boxes.size() == 3,
            "got " + std::to_string(finalCheck.boxes.size()));
    }

    DeleteFileW(tmp.c_str());

    // 自检会往撤销栈里写东西，先把用户真实的撤销记录保下来，最后再放回去。
    const std::wstring undoFile = AppFile(L"undo-history.txt");
    std::string savedUndo;
    ReadFileBytes(undoFile, savedUndo);
    UndoHistoryClear();

    AddRuleChecks(checks);
    AddUndoPersistenceChecks(checks, undoFile);
    AddCategoryChecks(checks, hi);
    AddMultiTabChecks(checks, hi);
    AddThemeChecks(checks, hi);
    AddShellMenuChecks(checks);
    AddHoldChecks(checks);
    AddRenameChecks(checks);
    AddInlineRenameChecks(checks, hi);
    AddGeometryChecks(checks);
    AddMergeChecks(checks);
    AddIconChecks(checks);

    // 图形设备必须在上面这些检查之后释放：它们里面还会创建盒子窗口并真的绘制，
    // 提前释放会让 Paint 拿不到 render target，表现为盒子一片空白。
    WicShutdown();
    RenderShutdown();
    Box::Unregister(hi);

    std::wstring report;
    int failed = 0;
    for (auto& c : checks) {
        report += c.ok ? L"PASS  " : L"FAIL  ";
        report += Utf8ToWide(c.name);
        if (!c.detail.empty()) {
            report += L"  -- ";
            report += Utf8ToWide(c.detail.c_str());
        }
        report += L"\n";
        if (!c.ok) failed++;
    }
    report += L"\n" + std::to_wstring(checks.size() - failed) + L"/" + std::to_wstring(checks.size()) + L" passed\n";

    std::string u8 = WideToUtf8(report);
    HANDLE f = CreateFileW(L"selftest.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const char bom[] = {'\xEF', '\xBB', '\xBF'};
        DWORD written = 0;
        WriteFile(f, bom, 3, &written, nullptr);
        WriteFile(f, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
        CloseHandle(f);
    } else {
        Log(L"selftest.txt 写入失败 gle=%lu", GetLastError());
    }

    // 把用户真实的撤销记录放回去，自检不能洗掉它。
    WriteFileBytes(undoFile, savedUndo);
    UndoHistoryLoad();
    return failed == 0 ? 0 : 1;
}

// 单独跑：真的移动一个桌面图标，再从备份还原回来。会动用户的桌面，所以不进默认自检。
int AppLayoutTest() {
    FILE* out = nullptr;
    fopen_s(&out, "layouttest.txt", "w");
    auto say = [&](const std::string& s) {
        if (out) {
            fprintf(out, "%s\n", s.c_str());
            fflush(out);
        }
        LogU8(s);
    };

    std::vector<IconPosition> p0;
    std::wstring err;
    if (!IconViewRead(p0, err)) {
        say("FAIL cannot read desktop icon positions: " + WideToUtf8(err));
        if (out) fclose(out);
        return 1;
    }
    say("read " + std::to_string(p0.size()) + " icons, first=(" + std::to_string(p0[0].x) + "," +
        std::to_string(p0[0].y) + ")");

    const std::wstring file = AppFile(L"layouttest.desklayout");
    if (!DesktopLayoutBackup(file, err)) {
        say("FAIL backup: " + WideToUtf8(err));
        if (out) fclose(out);
        return 1;
    }

    IconPosition moved = p0.back();
    LONG targetX = moved.x + 100;
    LONG targetY = moved.y + 100;
    std::vector<IconPosition> one{IconPosition{moved.name, targetX, targetY}};
    int movedCount = 0, unmatched = 0;
    std::wstring applyErr;
    bool applied = IconViewApply(one, applyErr, movedCount, unmatched);
    say(std::string(applied && movedCount == 1 ? "PASS " : "FAIL ") + "apply asked to move one icon, err=" +
        WideToUtf8(applyErr));

    std::vector<IconPosition> p1;
    IconViewRead(p1, err);
    bool actuallyMoved = false;
    std::string where;
    for (const auto& it : p1) {
        if (it.name != moved.name) continue;
        where = "now at (" + std::to_string(it.x) + "," + std::to_string(it.y) + ") was (" +
                std::to_string(moved.x) + "," + std::to_string(moved.y) + ")";
        if (it.x != moved.x || it.y != moved.y) actuallyMoved = true;
    }
    say(std::string(actuallyMoved ? "PASS " : "FAIL ") + "icon really left its original slot " + where);

    bool restored = DesktopLayoutRestore(file, err);
    std::vector<IconPosition> p2;
    IconViewRead(p2, err);
    int diffs = 0;
    for (size_t i = 0; i < p0.size() && i < p2.size(); ++i) {
        if (p0[i].name != p2[i].name || abs(static_cast<int>(p0[i].x - p2[i].x)) > 4 ||
            abs(static_cast<int>(p0[i].y - p2[i].y)) > 4) {
            diffs++;
        }
    }
    say(std::string(restored && diffs == 0 ? "PASS " : "FAIL ") + "restore put the desktop back, " +
        std::to_string(diffs) + " icons still off");
    if (diffs != 0) say("  注意：桌面没有完全回到原样，可以用托盘的还原再试一次，或手工调整");

    DeleteFileW(file.c_str());

    // 第 4 步：撤销之后图标要回到原来的格子，而不是 Shell 另给的空位。
    // 先把测试图标挪到一个显眼的位置，这样"没做坐标还原"会明显落在别处。
    const std::wstring sentinelName = L"__deskorg_layout_test.txt";
    const std::wstring sentinel = JoinPath(DesktopPath(), sentinelName);
    bool undoPosOk = false;
    std::string undoDetail;
    do {
        TouchFile(sentinel, "sentinel");
        IconPosition placed{};
        bool appeared = false;
        for (int i = 0; i < 30; ++i) {
            std::vector<IconPosition> live;
            std::wstring e;
            if (IconViewRead(live, e)) {
                for (const auto& it : live) {
                    if (_wcsicmp(it.name.c_str(), sentinelName.c_str()) == 0) {
                        placed = it;
                        appeared = true;
                        break;
                    }
                }
            }
            if (appeared) break;
            Sleep(100);
        }
        if (!appeared) {
            undoDetail = "FAIL desktop icon never appeared for the sentinel file";
            break;
        }

        const LONG wantX = 1500, wantY = 900;
        int mv = 0, un = 0;
        std::wstring sentinelMoveErr;
        IconViewApply(std::vector<IconPosition>{IconPosition{placed.name, wantX, wantY}}, sentinelMoveErr, mv, un);
        undoDetail += "sentinel moved to " + std::to_string(wantX) + "," + std::to_string(wantY) + "; ";

        std::vector<IconPosition> captured = IconViewCaptureInDir(std::vector<std::wstring>{sentinel}, DesktopPath());
        undoDetail += "captured=" + std::to_string(captured.size()) + " at (" +
                      (captured.empty() ? "-" : std::to_string(captured[0].x) + "," + std::to_string(captured[0].y)) +
                      "); ";
        undoDetail += "dir=[" + WideToUtf8(DesktopPath()) + "] sentinel=[" + WideToUtf8(sentinel) + "] shown=[";
        {
            std::vector<IconPosition> live;
            std::wstring e;
            if (IconViewRead(live, e)) {
                for (size_t i = 0; i < live.size() && i < 12; ++i) {
                    if (i) undoDetail += "|";
                    undoDetail += WideToUtf8(live[i].name);
                }
            }
        }
        undoDetail += "] ";
        if (captured.empty()) break;

        const std::wstring boxDir = AppFile(L"undo_box");
        EnsureDirExists(boxDir);
        std::vector<MovedEntry> m;
        std::wstring mvErr;
        if (!MoveFilesTo(std::vector<std::wstring>{sentinel}, boxDir, nullptr, m, mvErr) || m.empty()) {
            undoDetail += "FAIL move out of desktop: " + WideToUtf8(mvErr);
            break;
        }
        UndoHistoryAdd(std::move(m), L"自检：哨兵文件", captured);

        std::wstring undoErr;
        if (!UndoHistoryUndoLast(nullptr, undoErr)) {
            undoDetail += "FAIL undo: " + WideToUtf8(undoErr);
            break;
        }

        bool back = false;
        IconPosition now{};
        for (int i = 0; i < 40; ++i) {
            std::vector<IconPosition> live;
            std::wstring e;
            if (IconViewRead(live, e)) {
                for (const auto& it : live) {
                    if (_wcsicmp(it.name.c_str(), sentinelName.c_str()) == 0) {
                        now = it;
                        back = true;
                        break;
                    }
                }
            }
            if (back && abs(static_cast<int>(now.x - wantX)) <= 40 && abs(static_cast<int>(now.y - wantY)) <= 40) break;
            Sleep(100);
        }
        undoDetail += "after undo at (" + std::to_string(now.x) + "," + std::to_string(now.y) + ")";
        undoPosOk = back && abs(static_cast<int>(now.x - wantX)) <= 40 && abs(static_cast<int>(now.y - wantY)) <= 40;
    } while (false);

    say(std::string(undoPosOk ? "PASS " : "FAIL ") + "undo returned the icon to its old slot" +
        (undoPosOk ? std::string() : " -- " + undoDetail));
    // 用 Shell 删，别用 DeleteFileW：绕过 Explorer 的删除会短暂留一个幽灵图标。
    std::wstring delErr;
    if (!DeleteFiles(std::vector<std::wstring>{sentinel}, nullptr, delErr)) {
        say("note: sentinel delete failed: " + WideToUtf8(delErr));
    }

    // 测试自己造的东西自己收：哨兵文件的落点目录、以及还原流程顺手留的安全备份。
    RemoveDirectoryW(AppFile(L"undo_box").c_str());
    for (const auto& f : ListBackups()) DeleteFileW(f.c_str());

    int failed = (applied && movedCount == 1 ? 0 : 1) + (actuallyMoved ? 0 : 1) + (restored && diffs == 0 ? 0 : 1) +
                 (undoPosOk ? 0 : 1);
    say("result: " + std::to_string(4 - failed) + "/4 passed");
    if (out) fclose(out);
    return failed == 0 ? 0 : 1;
}

void App::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = tray_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title.c_str(), _countof(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text.c_str(), _countof(nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring App::FolderForBoxName(const std::wstring& name) const {
    // 和 CreateOneBox 走同一套解析，保证虚拟目标和真盒子算出来的目录一致。
    BoxConfig probe;
    probe.name = SanitizeBoxName(name);
    return ResolveBoxFolder(probe, boxRoot_);
}

void App::RunRules(bool fullSweep) {
    if (layout_.rules.empty()) return;

    std::vector<OrganizeTarget> targets;
    auto collect = [&]() {
        targets.clear();
        for (auto& b : boxes_) {
            BoxConfig c;
            b->Current(c);
            targets.push_back(OrganizeTarget{c.name, b->folder()});
        }
    };
    collect();

    // 规则指向的盒子可能还没建。先按名字算出它将来所在的目录参与匹配，
    // 但只有真的命中了文件才建盒子，否则一条没匹配上的规则就能凭空造出空目录。
    std::vector<std::wstring> missingNames;
    for (const auto& r : layout_.rules) {
        if (r.boxName.empty()) continue;
        bool exists = std::any_of(targets.begin(), targets.end(), [&](const OrganizeTarget& t) {
            return _wcsicmp(t.boxName.c_str(), r.boxName.c_str()) == 0;
        });
        if (exists || std::find(missingNames.begin(), missingNames.end(), r.boxName) != missingNames.end()) continue;
        missingNames.push_back(r.boxName);
        targets.push_back(OrganizeTarget{r.boxName, FolderForBoxName(r.boxName)});
    }

    auto all = PlanOrganize(DesktopPath(), layout_.rules, targets);

    // 撤销过的文件在保护期内不再被规则搬走，否则撤销等于没做。
    // 手动"整理现存全部文件"是用户显式要求的，跳过这层保护。
    if (!fullSweep) {
        size_t held = 0;
        all = FilterHolds(all, held);
        if (held > 0) Log(L"自动整理跳过 %zu 项撤销保护期内的文件", held);
    }

    OrganizeBaselines baselines;
    std::wstring baseErr;
    OrganizeBaselines::Load(baselines, baseErr);
    baselines.DropMissing();

    // sweep = 0 的规则第一次运行时，把当时已经命中的文件记成基线，这一批就不动；
    // 之后新出现的文件才归。手动全量整理（托盘菜单）会跳过过滤，并在事后把基线清空。
    bool seeded = false;
    std::vector<OrganizeItem> plan = all;
    if (!fullSweep) {
        for (const auto& r : layout_.rules) {
            if (r.sweepExisting || baselines.Has(r.name)) continue;
            std::vector<std::wstring> paths;
            for (const auto& p : all) {
                if (p.ruleName == r.name) paths.push_back(p.path);
            }
            baselines.Add(r.name, paths);
            seeded = true;
        }
        plan = FilterByBaselines(all, layout_.rules, baselines);
    }
    baselines.Prune(layout_.rules);
    if (plan.empty()) {
        if (seeded) baselines.Save(baseErr);
        return;
    }

    bool createdAny = false;
    for (const auto& name : missingNames) {
        bool used = std::any_of(plan.begin(), plan.end(),
                                [&](const OrganizeItem& p) { return _wcsicmp(p.boxName.c_str(), name.c_str()) == 0; });
        if (!used) continue;
        BoxConfig nb;
        nb.name = SanitizeBoxName(name);
        int off = static_cast<int>(boxes_.size()) * 34;
        nb.rect = RECT{40 + off, 60 + off, 40 + off + 340, 60 + off + 260};
        CreateOneBox(nb);
        createdAny = true;
    }
    if (createdAny) {
        SyncLayoutFromBoxes();
        LayoutSave(layout_);
    }

    size_t moved = 0;
    std::wstring err;
    bool ok = ApplyOrganize(plan, tray_, true, moved, err);
    if (!ok) Log(L"自动整理部分失败：%s", err.c_str());

    if (fullSweep) {
        // 全量整理刚把现存文件处理完，基线该按"现在什么都还没见过"重开。
        for (const auto& r : layout_.rules) {
            if (!r.sweepExisting) baselines.Add(r.name, std::vector<std::wstring>{});
        }
    }
    if (seeded || fullSweep || moved > 0) baselines.Save(baseErr);

    if (moved > 0) {
        Log(L"自动整理完成：%zu 项", moved);
        for (auto& b : boxes_) b->RefreshContent();
        std::wstring detail;
        for (size_t i = 0; i < plan.size() && i < 5; ++i) {
            if (!detail.empty()) detail += L"\n";
            detail += FileNameOfW(plan[i].path) + L" → " + plan[i].boxName;
        }
        if (plan.size() > 5) detail += L"\n…";
        ShowBalloon(L"桌面整理：归类了 " + std::to_wstring(moved) + L" 项", detail);
    }
}

// 规则引擎自检：匹配是纯函数，端到端在临时目录里跑，不碰用户真实桌面。
void AddRuleChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    RuleConfig imgs;
    imgs.name = L"图片";
    imgs.boxName = L"图片";
    imgs.ext = L".png,.jpg,.JPEG";

    add("rule_ext_match_png", RuleMatches(imgs, L"a.png", false));
    add("rule_ext_match_case", RuleMatches(imgs, L"PHOTO.JPG", false));
    add("rule_ext_reject_txt", !RuleMatches(imgs, L"a.txt", false));
    add("rule_ext_reject_noext", !RuleMatches(imgs, L"README", false));
    add("rule_ext_reject_dir", !RuleMatches(imgs, L"photo.png", true));
    add("rule_folders_opt_in", [&] {
        RuleConfig r = imgs;
        r.includeFolders = true;
        return RuleMatches(r, L"photo.png", true);
    }());

    add("rule_empty_matches_nothing", [&] {
        RuleConfig blank;
        blank.boxName = L"什么都收";
        return !RuleMatches(blank, L"anything.txt", false);
    }());

    RuleConfig byPrefix;
    byPrefix.name = L"截图";
    byPrefix.boxName = L"截图";
    byPrefix.prefix = L"screenshot";
    add("rule_prefix_match", RuleMatches(byPrefix, L"Screenshot 2026-10-19.png", false));
    add("rule_prefix_reject", !RuleMatches(byPrefix, L"not-a-screenshot.png", false));

    RuleConfig byContains;
    byContains.name = L"报告";
    byContains.boxName = L"文档";
    byContains.contains = L"报告";
    add("rule_contains_match", RuleMatches(byContains, L"季度报告.docx", false));
    add("rule_contains_reject", !RuleMatches(byContains, L"季度计划.docx", false));

    RuleConfig combo;
    combo.name = L"组合";
    combo.boxName = L"图片";
    combo.ext = L".png";
    combo.contains = L"截图";
    add("rule_all_criteria_required",
        RuleMatches(combo, L"我的截图.png", false) && !RuleMatches(combo, L"我的截图.jpg", false) &&
            !RuleMatches(combo, L"照片.png", false));

    // 端到端：临时"桌面" + 两个盒子目录。
    const std::wstring desk = AppFile(L"rule_desktop");
    const std::wstring boxImgs = AppFile(L"rule_box_imgs");
    const std::wstring boxDocs = AppFile(L"rule_box_docs");
    for (const auto& d : {desk, boxImgs, boxDocs}) EnsureDirExists(d);

    std::vector<std::wstring> names{L"a.png", L"b.txt", L"我的截图.png", L"c.jpg"};
    for (const auto& n : names) TouchFile(JoinPath(desk, n), "x");
    // 盒子目录自己就在"桌面"下时，绝不能被当成文件搬进另一个盒子。
    EnsureDirExists(JoinPath(desk, L"内嵌盒"));

    Layout lay{};
    RuleConfig r1;
    r1.name = L"图片类";
    r1.boxName = L"图片";
    r1.ext = L".png,.jpg";
    RuleConfig r2;
    r2.name = L"文本类";
    r2.boxName = L"文档";
    r2.ext = L".txt";
    lay.rules.push_back(r1);
    lay.rules.push_back(r2);

    std::vector<OrganizeTarget> targets{OrganizeTarget{L"图片", boxImgs}, OrganizeTarget{L"文档", boxDocs},
                                        OrganizeTarget{L"内嵌盒", JoinPath(desk, L"内嵌盒")}};

    auto plan = PlanOrganize(desk, lay.rules, targets);
    // 4 个文件全部命中规则：3 个进图片盒，1 个进文档盒。
    add("plan_moves_all_matching", plan.size() == 4, "got " + std::to_string(plan.size()));
    add("plan_skips_inner_box_folder",
        std::none_of(plan.begin(), plan.end(),
                     [](const OrganizeItem& p) { return p.path.find(L"内嵌盒") != std::wstring::npos; }));
    add("plan_routes_to_right_box",
        std::count_if(plan.begin(), plan.end(), [&](const OrganizeItem& p) { return p.destFolder == boxImgs; }) == 3 &&
            std::count_if(plan.begin(), plan.end(), [&](const OrganizeItem& p) { return p.destFolder == boxDocs; }) == 1);

    // 规则配置本身也要能原样往返。
    const std::wstring iniFile = AppFile(L"rules_roundtrip.ini");
    bool saved = LayoutSaveTo(lay, iniFile);
    Layout back{};
    bool loaded = LayoutLoadFrom(back, iniFile);
    add("rules_roundtrip",
        saved && loaded && back.rules.size() == 2 && back.rules[0].ext == r1.ext && back.rules[0].boxName == r1.boxName &&
            back.rules[1].name == r2.name,
        "loaded=" + std::to_string(back.rules.size()));

    size_t movedCount = 0;
    std::wstring orgErr;
    bool applied = ApplyOrganize(plan, nullptr, false, movedCount, orgErr);
    add("organize_applied", applied && movedCount == 4, WideToUtf8(orgErr));
    add("organize_files_landed",
        !PathExists(JoinPath(desk, L"a.png")) && PathExists(JoinPath(boxImgs, L"a.png")) &&
            PathExists(JoinPath(boxDocs, L"b.txt")));
    add("organize_inner_box_untouched", PathExists(JoinPath(desk, L"内嵌盒")));

    std::wstring undoErr;
    bool undone = UndoHistoryUndoLast(nullptr, undoErr);
    int returned = 0;
    for (const auto& n : names) {
        if (PathExists(JoinPath(desk, n))) returned++;
    }
    add("organize_undo_returns_all", undone && returned == 4, WideToUtf8(undoErr));

    // ---- sweep 开关与基线 ----
    std::string savedBaseline;
    const std::wstring baselineFile = AppFile(L"organize-baseline.txt");
    bool hadBaseline = ReadFileBytes(baselineFile, savedBaseline);

    RuleConfig gentle = r1;
    gentle.name = L"图片类温和版";
    gentle.sweepExisting = false;
    std::vector<RuleConfig> gentleRules{gentle};
    const std::wstring ini2 = AppFile(L"sweep_roundtrip.ini");
    Layout glay{};
    glay.rules.push_back(gentle);
    bool gs = LayoutSaveTo(glay, ini2);
    Layout gback{};
    bool gl = LayoutLoadFrom(gback, ini2);
    add("sweep_switch_roundtrip",
        gs && gl && gback.rules.size() == 1 && gback.rules[0].sweepExisting == false && gback.rules[0].name == gentle.name,
        gl ? "loaded=" + std::to_string(gback.rules.size()) : std::string("<load failed>"));

    // 温和版规则第一次规划时，现存命中的文件应当全部进入基线并被过滤掉；
    // 之后新出现的文件才该被归走。
    auto allAgain = PlanOrganize(desk, gentleRules, targets);
    // 温和版规则 ext=.png,.jpg 在这个夹具里命中 3 个：a.png、我的截图.png、c.jpg
    add("gentle_rule_still_plans", allAgain.size() == 3, "got " + std::to_string(allAgain.size()));

    OrganizeBaselines bl;
    std::wstring blErr;
    OrganizeBaselines::Load(bl, blErr);
    std::vector<std::wstring> seed;
    for (const auto& p : allAgain) seed.push_back(p.path);
    bl.Add(gentle.name, seed);
    auto filtered = FilterByBaselines(allAgain, gentleRules, bl);
    add("baseline_filters_existing", filtered.empty(), "left " + std::to_string(filtered.size()));

    std::wstring newcomer = JoinPath(desk, L"刚出现的.png");
    TouchFile(newcomer, "x");
    auto withNew = PlanOrganize(desk, gentleRules, targets);
    auto filteredNew = FilterByBaselines(withNew, gentleRules, bl);
    add("baseline_allows_newcomer",
        filteredNew.size() == 1 && filteredNew[0].path == newcomer,
        "got " + std::to_string(filteredNew.size()));
    add("sweep_all_ignores_baseline", FilterByBaselines(withNew, gentleRules, OrganizeBaselines{}).size() == withNew.size());

    // 基线文件本身要能往返，中文规则名和大小写不同的路径都得认。
    OrganizeBaselines saveMe;
    saveMe.Add(L"图片类温和版", {JoinPath(desk, L"A.PNG")});
    bool bsaved = saveMe.Save(blErr);
    OrganizeBaselines bload;
    bool bloaded = OrganizeBaselines::Load(bload, blErr);
    add("baseline_file_roundtrip",
        bsaved && bloaded && bload.Has(gentle.name) && bload.Contains(gentle.name, JoinPath(desk, L"a.png")),
        WideToUtf8(blErr));

    // 用户删掉再新建同名文件时，那条基线不该继续挡住它。
    DeleteFileW(JoinPath(desk, L"a.png").c_str());
    bload.DropMissing();
    add("baseline_drops_gone_paths", !bload.Contains(gentle.name, JoinPath(desk, L"A.PNG")));

    DeleteFileW(newcomer.c_str());
    DeleteFileW(ini2.c_str());
    DeleteFileW(baselineFile.c_str());
    if (hadBaseline) WriteFileBytes(baselineFile, savedBaseline);
    else if (!savedBaseline.empty()) WriteFileBytes(baselineFile, savedBaseline);

    for (const auto& n : names) DeleteFileW(JoinPath(desk, n).c_str());
    DeleteFileW(JoinPath(boxImgs, L"a.png").c_str());
    DeleteFileW(JoinPath(boxImgs, L"我的截图.png").c_str());
    DeleteFileW(JoinPath(boxDocs, L"b.txt").c_str());
    RemoveDirIfExists(boxImgs);
    RemoveDirIfExists(boxDocs);
    RemoveDirIfExists(desk);
    DeleteFileW(iniFile.c_str());
}

// 撤销栈持久化的自检。先保护住用户真实的撤销文件，跑完再放回去。
void AddUndoPersistenceChecks(std::vector<Check>& checks, const std::wstring& path) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring src = AppFile(L"persist_src");
    const std::wstring box = AppFile(L"persist_box");
    EnsureDirExists(src);
    EnsureDirExists(box);
    std::wstring file = JoinPath(src, L"跨会话.txt");
    TouchFile(file, "payload");

    std::vector<MovedEntry> moves;
    std::wstring err;
    bool moved = MoveFilesTo(std::vector<std::wstring>{file}, box, nullptr, moves, err);
    add("persist_move_ok", moved && moves.size() == 1, WideToUtf8(err));
    if (!moved || moves.empty()) return;

    UndoHistoryAdd(std::move(moves), L"自检：跨会话撤销", std::vector<IconPosition>{});

    std::string persisted;
    bool readBack = ReadFileBytes(path, persisted);
    add("persist_file_written", readBack && persisted.find("label =") != std::string::npos,
        "bytes=" + std::to_string(persisted.size()));
    add("persist_size", UndoHistorySize() == 1, "size=" + std::to_string(UndoHistorySize()));

    // 模拟重启：丢掉内存内容，只从文件读回来。
    UndoHistoryLoad();
    add("persist_reloaded", UndoHistorySize() == 1, "size=" + std::to_string(UndoHistorySize()));

    std::wstring label;
    bool hasLabel = UndoHistoryLastLabel(label);
    // 从文件读回来的条目算"上一次运行"，菜单文案要带上时间，否则用户不知道自己在撤什么。
    add("persist_label_marks_cross_session",
        hasLabel && label.find(L"自检：跨会话撤销") != std::wstring::npos && label.find(L"（") != std::wstring::npos,
        WideToUtf8(label));

    std::wstring undoErr;
    bool undid = UndoHistoryUndoLast(nullptr, undoErr);
    add("persist_undo_works", undid && PathExists(file) && !PathExists(JoinPath(box, L"跨会话.txt")),
        WideToUtf8(undoErr));
    add("persist_stack_drained", UndoHistorySize() == 0, "size=" + std::to_string(UndoHistorySize()));

    DeleteFileW(JoinPath(box, L"跨会话.txt").c_str());
    RemoveDirIfExists(box);
    RemoveDirIfExists(src);
}

// 盒内二级分类的自检。重点是那条不变量：任何文件都必须落在某个分类里，
// 否则它会在盒子的标签卡之间"消失"，那比不分分类更糟。
void AddCategoryChecks(std::vector<Check>& checks, HINSTANCE hi) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    struct Sample {
        const wchar_t* name;
        bool isDir;
        const wchar_t* expect;
    };
    const Sample samples[] = {
        {L"a.png", false, L"图片"},        {L"PHOTO.JPG", false, L"图片"},
        {L"报告.docx", false, L"文档"},    {L"notes.txt", false, L"文档"},
        {L"app.exe", false, L"程序"},      {L"快捷方式.lnk", false, L"程序"},
        {L"x.zip", false, L"压缩包"},      {L"tar.gz", false, L"压缩包"},
        {L"song.mp3", false, L"媒体"},
        {L"noext", false, L"其他"},        {L".gitignore", false, L"其他"},
        {L"weird.zzz", false, L"其他"},    {L"trailingdot.", false, L"其他"},
        {L"my folder", true, L"文件夹"},
    };

    bool allMapped = true, allExact = true;
    std::string firstBad;
    for (const auto& s : samples) {
        std::wstring got = category::CategoryOf(s.name, s.isDir);
        if (!category::IsKnown(got)) {
            allMapped = false;
            if (firstBad.empty()) firstBad = "unmapped: " + WideToUtf8(s.name);
        }
        if (got != s.expect) {
            allExact = false;
            if (firstBad.empty()) {
                firstBad = WideToUtf8(s.name) + " -> " + WideToUtf8(got) + " want " + WideToUtf8(s.expect);
            }
        }
    }
    add("category_covers_every_sample", allMapped, firstBad);
    add("category_mapping_exact", allExact, firstBad);

    // 配置往返
    const std::wstring iniFile = AppFile(L"category_roundtrip.ini");
    Layout lay{};
    BoxConfig b;
    b.name = L"分类盒";
    b.rect = RECT{10, 10, 10 + 360, 10 + 260};
    b.grouped = true;
    b.tab = L"图片";
    lay.boxes.push_back(b);
    bool sv = LayoutSaveTo(lay, iniFile);
    Layout back{};
    bool ld = LayoutLoadFrom(back, iniFile);
    add("category_config_roundtrip",
        sv && ld && back.boxes.size() == 1 && back.boxes[0].grouped && back.boxes[0].tab == L"图片",
        ld && !back.boxes.empty() ? WideToUtf8(back.boxes[0].tab) : std::string("<load failed>"));
    DeleteFileW(iniFile.c_str());

    // 盒子层面的并集不变量：各分类标签卡看到的项目数之和必须等于总项目数。
    const std::wstring dir = AppFile(L"category_box");
    EnsureDirExists(dir);
    const wchar_t* files[] = {L"p1.png", L"p2.jpg", L"d1.txt", L"e.exe", L"odd.qqq", L"nomatter"};
    for (const wchar_t* f : files) TouchFile(JoinPath(dir, f), "x");
    EnsureDirExists(JoinPath(dir, L"子目录"));

    Box::Register(hi);
    Box box;
    BoxConfig cfg;
    cfg.name = L"分类盒";
    cfg.rect = RECT{40, 400, 40 + 360, 400 + 260};
    cfg.grouped = true;
    add("category_box_created", box.Create(cfg, dir));
    PumpMessages(500);

    size_t total = box.itemCount();
    add("category_all_tab_shows_everything", box.shownCount() == total,
        "shown=" + std::to_string(box.shownCount()) + " total=" + std::to_string(total));

    size_t sum = 0;
    bool tabsLookRight = true;
    for (const auto& t : box.TabNames()) {
        if (t == L"全部") continue;
        if (box.SetCategoryTab(t)) sum += box.shownCount();
        else tabsLookRight = false;
    }
    add("category_union_equals_total", sum == total, "sum=" + std::to_string(sum) + " total=" + std::to_string(total));
    add("category_tabs_switchable", tabsLookRight);
    add("category_other_catches_leftovers", [&] {
        box.SetCategoryTab(L"其他");
        return box.shownCount() == 2;  // odd.qqq 和 nomatter
    }(), "");
    add("category_images_tab", [&] {
        box.SetCategoryTab(L"图片");
        return box.shownCount() == 2;
    }());
    add("category_rejects_empty_tab", !box.SetCategoryTab(L"媒体"));

    // 标签卡必须真的画到屏幕上。只验证几何算得对，掩盖不了"根本没绘制"这类问题
    // —— Progman 子窗口当初就是靠命中测试骗过我的。
    box.SetCategoryTab(L"全部");
    box.SetAlpha(255);
    PumpMessages(350);
    {
        RECT br{};
        GetWindowRect(box.hwnd(), &br);
        const POINT pillPt{br.left + 48, br.top + 47};
        const POINT fillPt{br.left + 330, br.top + 100};
        bool pillOwned = WindowFromPoint(pillPt) == box.hwnd();
        bool fillOwned = WindowFromPoint(fillPt) == box.hwnd();
        HDC screen = GetDC(nullptr);
        COLORREF pill = GetPixel(screen, pillPt.x, pillPt.y);
        COLORREF fill = GetPixel(screen, fillPt.x, fillPt.y);
        ReleaseDC(nullptr, screen);
        int delta = abs(GetRValue(pill) - GetRValue(fill)) + abs(GetGValue(pill) - GetGValue(fill)) +
                    abs(GetBValue(pill) - GetBValue(fill));
        char d[160];
        snprintf(d, sizeof d, "pill=(%d,%d,%d) fill=(%d,%d,%d) owned=%d/%d delta=%d painted=%d", GetRValue(pill),
                 GetGValue(pill), GetBValue(pill), GetRValue(fill), GetGValue(fill), GetBValue(fill),
                 pillOwned ? 1 : 0, fillOwned ? 1 : 0, delta, box.painted() ? 1 : 0);
        add("category_tab_actually_painted", pillOwned && fillOwned && delta > 20, d);
    }
    add("category_tab_area_positive", box.tabAreaHeight() > 0);
    box.Destroy();
    RemoveDirIfExists(dir);
}

static std::vector<std::wstring> SplitCsv(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t sep = s.find_first_of(L",;", start);
        std::wstring item = sep == std::wstring::npos ? s.substr(start) : s.substr(start, sep - start);
        size_t b = item.find_first_not_of(L" \t");
        size_t e = item.find_last_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(item.substr(b, e - b + 1));
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return out;
}

std::vector<std::wstring> App::MemberNames() const {
    std::vector<std::wstring> all;
    for (const auto& b : layout_.boxes) {
        for (const auto& m : SplitCsv(b.members)) all.push_back(m);
    }
    return all;
}

// 成员盒子被合并进多标签盒子之后就不再单独显示；配置和位置都还在，
// 把 members 那一行删掉它们就各自回来。
void App::ApplyMembershipVisibility() {
    std::vector<std::wstring> members = MemberNames();
    for (auto& b : boxes_) {
        BoxConfig c;
        b->Current(c);
        bool absorbed = std::any_of(members.begin(), members.end(),
                                    [&](const std::wstring& m) { return _wcsicmp(m.c_str(), c.name.c_str()) == 0; });
        b->Show(!hidden_ && !absorbed);
    }
}

// 多标签合并盒子的自检。关键是拖入的落点：标签卡决定文件去哪，程序绝不替你猜目录。
void AddMultiTabChecks(std::vector<Check>& checks, HINSTANCE hi) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring agg = AppFile(L"tabs_aggregate");
    const std::wstring mA = AppFile(L"tabs_alpha");
    const std::wstring mB = AppFile(L"tabs_beta");
    const std::wstring src = AppFile(L"tabs_src");
    for (const auto& d : {agg, mA, mB, src}) EnsureDirExists(d);
    TouchFile(JoinPath(mA, L"甲.txt"), "a");
    TouchFile(JoinPath(mB, L"乙.png"), "b");
    TouchFile(JoinPath(mB, L"丙.exe"), "c");
    const std::wstring dropMe = JoinPath(src, L"待归类.txt");
    TouchFile(dropMe, "d");

    // 配置往返
    Layout lay{};
    BoxConfig a;
    a.name = L"合集";
    a.rect = RECT{20, 20, 20 + 380, 20 + 260};
    a.members = L"alpha, beta";
    BoxConfig b1;
    b1.name = L"alpha";
    b1.rect = RECT{0, 0, 320, 240};
    b1.folder = mA;
    BoxConfig b2;
    b2.name = L"beta";
    b2.rect = RECT{0, 0, 320, 240};
    b2.folder = mB;
    lay.boxes = {a, b1, b2};

    const std::wstring iniFile = AppFile(L"multitab_roundtrip.ini");
    bool sv = LayoutSaveTo(lay, iniFile);
    Layout back{};
    bool ld = LayoutLoadFrom(back, iniFile);
    add("multitab_config_roundtrip",
        sv && ld && back.boxes.size() == 3 && back.boxes[0].members == L"alpha, beta",
        WideToUtf8(ld ? back.boxes[0].members : L"<load failed>"));
    DeleteFileW(iniFile.c_str());

    Box::Register(hi);
    Box box;
    BoxConfig cfg;
    cfg.name = L"合集";
    cfg.rect = RECT{40, 700, 40 + 380, 700 + 260};
    cfg.members = L"alpha,beta,不存在的成员";
    std::vector<std::pair<std::wstring, std::wstring>> members{{L"alpha", mA}, {L"beta", mB}};
    add("multitab_created", box.Create(cfg, agg, members));
    PumpMessages(500);

    add("multitab_is_aggregate", box.IsAggregate());
    add("multitab_no_all_tab", [&] {
        auto t = box.TabNames();
        return t.size() == 2 && t[0] == L"alpha" && t[1] == L"beta";
    }(), "");
    add("multitab_grouping_disabled", !box.Grouped());
    add("multitab_starts_at_first_member", box.ActiveTab() == L"alpha" && box.itemCount() == 1,
        "tab=" + WideToUtf8(box.ActiveTab()) + " items=" + std::to_string(box.itemCount()));
    add("multitab_active_folder", box.activeFolderForTest() == mA, WideToUtf8(box.activeFolderForTest()));

    // 切到 beta：内容跟着换，且拖入的落点必须变成 beta 的目录。
    add("multitab_switch_tab", box.SetCategoryTab(L"beta") && box.itemCount() == 2 && box.ActiveTab() == L"beta");
    add("multitab_folder_follows_tab", box.activeFolderForTest() == mB, WideToUtf8(box.activeFolderForTest()));
    add("multitab_rejects_unknown_tab", !box.SetCategoryTab(L"没有这个标签"));

    bool dropped = box.AcceptFiles(std::vector<std::wstring>{dropMe});
    add("multitab_drop_lands_on_active_tab_folder",
        dropped && PathExists(JoinPath(mB, L"待归类.txt")) && !PathExists(dropMe) &&
            !PathExists(JoinPath(agg, L"待归类.txt")),
        "mB=" + std::to_string(PathExists(JoinPath(mB, L"待归类.txt")) ? 1 : 0) +
            " agg=" + std::to_string(PathExists(JoinPath(agg, L"待归类.txt")) ? 1 : 0));

    // 切回 alpha 后，图标来自共享缓存：这里验证 alpha 的内容仍然完整可见。
    box.SetCategoryTab(L"alpha");
    add("multitab_switch_back", box.itemCount() == 1 && box.activeFolderForTest() == mA);

    std::wstring undoErr;
    bool undid = UndoHistoryUndoLast(nullptr, undoErr);
    add("multitab_undo_returns_file", undid && PathExists(dropMe) && !PathExists(JoinPath(mB, L"待归类.txt")),
        WideToUtf8(undoErr));

    box.Destroy();
    for (const auto& d : {agg, mA, mB, src}) RemoveDirIfExists(d);
    DeleteFileW(dropMe.c_str());
}

void AddThemeChecks(std::vector<Check>& checks, HINSTANCE hi) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    // 本函数会强制 light 和 dark 各画一遍，结束后必须回到进来时的模式。
    const ThemeMode modeAtEntry = ThemeGetMode();

    // 深浅两套必须真的不同，而且文字和底要拉得开对比度 —— 否则切完主题字就看不见了。
    auto lum = [](unsigned rgb) {
        return 0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF);
    };
    const Palette dk = DarkPalette();
    const Palette lt = LightPalette();
    add("theme_palettes_differ", dk.body != lt.body && dk.text != lt.text);
    auto contrast = [&](const Palette& p) {
        double a = lum(p.text), b = lum(p.body), c = lum(p.title);
        return (a > b ? a - b : b - a) > 90.0 && (a > c ? a - c : c - a) > 60.0;
    };
    add("theme_dark_text_contrast", contrast(dk));
    add("theme_light_text_contrast", contrast(lt));

    add("theme_parse_strings",
        ThemeParse(L"light") == ThemeMode::Light && ThemeParse(L"DARK") == ThemeMode::Dark &&
            ThemeParse(L"") == ThemeMode::Auto && ThemeParse(L"随便写的") == ThemeMode::Auto);

    // 0 是深色、1 是浅色，这种极性最容易记反，直接对照注册表原值核一遍。
    {
        HKEY key = nullptr;
        DWORD value = 0xFFFFFFFF;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof value, type = 0;
            RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
            RegCloseKey(key);
        }
        bool expect = (value == 0);
        add("theme_system_polarity", value == 0xFFFFFFFF || SystemPrefersDark() == expect,
            "AppsUseLightTheme=" + std::to_string(value) + " prefersDark=" + std::to_string(SystemPrefersDark() ? 1 : 0));
    }

    // 配置往返
    const std::wstring iniFile = AppFile(L"theme_roundtrip.ini");
    Layout lay{};
    lay.theme = L"light";
    BoxConfig b;
    b.name = L"主题盒";
    b.rect = RECT{0, 0, 320, 240};
    lay.boxes.push_back(b);
    bool sv = LayoutSaveTo(lay, iniFile);
    Layout back{};
    bool ld = LayoutLoadFrom(back, iniFile);
    add("theme_config_roundtrip", sv && ld && back.theme == L"light", WideToUtf8(ld ? back.theme : L"<fail>"));
    DeleteFileW(iniFile.c_str());

    // 强制两种主题各真画一遍并采样像素，顺带验证收到主题变更消息会重绘。
    ThemeSetMode(ThemeMode::Light);
    Box::Register(hi);
    const std::wstring dir = AppFile(L"theme_box_dir");
    EnsureDirExists(dir);
    Box box;
    BoxConfig cfg;
    cfg.name = L"主题盒";
    cfg.rect = RECT{40, 760, 40 + 360, 760 + 260};
    add("theme_box_created", box.Create(cfg, dir));
    PumpMessages(500);
    box.SetAlpha(255);
    SetWindowPos(box.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    PumpMessages(300);

    RECT br{};
    GetWindowRect(box.hwnd(), &br);
    const POINT bodyPt{br.left + 330, br.top + 100};
    bool owned = WindowFromPoint(bodyPt) == box.hwnd();

    auto sampleBody = [&](void) {
        HDC s = GetDC(nullptr);
        COLORREF c = GetPixel(s, bodyPt.x, bodyPt.y);
        ReleaseDC(nullptr, s);
        return c;
    };
    COLORREF lightPix = sampleBody();
    bool lightRight = owned && abs(GetRValue(lightPix) - 245) <= 12 && abs(GetGValue(lightPix) - 245) <= 12 &&
                      abs(GetBValue(lightPix) - 247) <= 12;
    add("theme_light_renders", lightRight,
        "owned=" + std::to_string(owned ? 1 : 0) + " pixel=(" + std::to_string(GetRValue(lightPix)) + "," +
            std::to_string(GetGValue(lightPix)) + "," + std::to_string(GetBValue(lightPix)) + ")");

    // 模拟系统主题切换：Explorer 就是广播这条消息的。
    ThemeSetMode(ThemeMode::Dark);
    static const wchar_t kArea[] = L"ImmersiveColorSet";
    SendMessageW(box.hwnd(), WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(kArea));
    PumpMessages(400);
    COLORREF darkPix = sampleBody();
    bool darkRight = owned && abs(GetRValue(darkPix) - 33) <= 12 && abs(GetGValue(darkPix) - 41) <= 12 &&
                     abs(GetBValue(darkPix) - 54) <= 12;
    add("theme_switch_live_renders", darkRight,
        "pixel=(" + std::to_string(GetRValue(darkPix)) + "," + std::to_string(GetGValue(darkPix)) + "," +
            std::to_string(GetBValue(darkPix)) + ")");

    SetWindowPos(box.hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    box.Destroy();
    RemoveDirIfExists(dir);
    // 自检其余部分假定钉住的配色，所以回到进来时的值而不是留在 Auto：
    // 留在 Auto 会让后面的像素断言随用户深浅色而变，检查顺序就成了隐藏变量。
    ThemeSetMode(modeAtEntry);
    add("theme_mode_restored", ThemeGetMode() == modeAtEntry);
}

// 桌面右键菜单走这条命令：有界面实例在跑就转给它，没有就自己把撤销做掉。
int AppRunUndo() {
    HWND target = FindWindowW(kTrayClass, nullptr);
    if (target && PostMessageW(target, WM_APP_UNDO_NOW, 0, 0)) {
        Log(L"已把撤销请求转交给正在运行的实例");
        return 0;
    }

    UndoHistoryLoad();
    if (UndoHistorySize() == 0) {
        Log(L"没有可撤销的操作");
        return 1;
    }
    std::wstring label;
    UndoHistoryLastLabel(label);
    std::wstring err;
    if (!UndoHistoryUndoLast(nullptr, err)) {
        Log(L"撤销失败：%s", err.c_str());
        return 1;
    }
    Log(L"撤销完成：%s", label.c_str());
    return 0;
}

// 跑一个自己的子进程，拿它的退出码当断言依据。
static bool RunSelfChild(const wchar_t* args, DWORD& exitCode) {
    std::wstring exe = shellmenu::ExePath();
    if (exe.empty()) return false;
    std::wstring line = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(line.begin(), line.end());
    mutableCmd.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log(L"CreateProcess 失败 gle=%lu", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void AddShellMenuChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring cmd = shellmenu::UndoCommandLine();
    add("shell_cmd_quoted_and_flagged",
        !cmd.empty() && cmd.front() == L'"' && cmd.rfind(L"\" --undo") != std::wstring::npos, WideToUtf8(cmd));

    // 把用户当前的安装状态原样保住，测完还原，自检不能顺手给人加菜单项。
    std::wstring hadCmd;
    const bool had = shellmenu::IsInstalled(hadCmd);

    std::wstring err;
    bool installed = shellmenu::Install(err);
    std::wstring nowCmd;
    bool nowInstalled = shellmenu::IsInstalled(nowCmd);
    add("shell_install_roundtrip", installed && nowInstalled && nowCmd == cmd, WideToUtf8(err));

    // 动词项的默认值必须是菜单文字，否则菜单上是空白一条。
    std::wstring menuText;
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, shellmenu::VerbKeyPath().c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t buf[512] = {};
            DWORD size = sizeof buf, type = 0;
            if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                menuText = buf;
            }
            RegCloseKey(key);
        }
    }
    add("shell_verb_has_menu_text", !menuText.empty(), WideToUtf8(menuText));

    bool uninstalled = shellmenu::Uninstall(err);
    std::wstring afterCmd;
    add("shell_uninstall_cleans", uninstalled && !shellmenu::IsInstalled(afterCmd), WideToUtf8(err));

    if (had) shellmenu::Install(err);
    else shellmenu::Uninstall(err);

    // 右键菜单真正会做的事：另起一个进程，凭落盘的撤销栈把文件放回去。
    const std::wstring src = AppFile(L"cli_undo_src");
    const std::wstring box = AppFile(L"cli_undo_box");
    EnsureDirExists(src);
    EnsureDirExists(box);
    const std::wstring file = JoinPath(src, L"命令行撤销.txt");
    TouchFile(file, "x");

    const std::wstring hist = AppFile(L"undo-history.txt");
    std::string savedHist;
    bool hadHist = ReadFileBytes(hist, savedHist);

    std::vector<MovedEntry> moves;
    std::wstring moveErr;
    bool moved = MoveFilesTo(std::vector<std::wstring>{file}, box, nullptr, moves, moveErr);
    if (moved && !moves.empty()) UndoHistoryAdd(std::move(moves), L"自检：命令行撤销", std::vector<IconPosition>{});
    add("cli_undo_precondition", moved && !PathExists(file) && PathExists(JoinPath(box, L"命令行撤销.txt")),
        WideToUtf8(moveErr));

    DWORD code = 0;
    bool ran = RunSelfChild(L"--undo", code);
    add("cli_undo_child_exit_zero", ran && code == 0,
        "ran=" + std::to_string(ran ? 1 : 0) + " code=" + std::to_string(code));
    add("cli_undo_actually_reverted", PathExists(file) && !PathExists(JoinPath(box, L"命令行撤销.txt")));

    // 空栈时应当报告"没有可撤销"并以非 0 退出，而不是默默成功。
    UndoHistoryClear();
    DWORD emptyCode = 0;
    bool ran2 = RunSelfChild(L"--undo", emptyCode);
    add("cli_undo_reports_empty_stack", ran2 && emptyCode != 0, "code=" + std::to_string(emptyCode));

    if (hadHist) WriteFileBytes(hist, savedHist);
    else DeleteFileW(hist.c_str());
    DeleteFileW(file.c_str());
    DeleteFileW(JoinPath(box, L"命令行撤销.txt").c_str());
    RemoveDirIfExists(box);
    RemoveDirIfExists(src);
}

// 撤销保护期的自检。用毫秒级超时来验行为，不等 60 秒。
void AddHoldChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring mixed = L"C:\\Temp\\CaseTest\\File.TXT";
    const std::wstring sameDiffCase = L"c:\\temp\\casetest\\file.txt";
    const std::wstring other = L"C:\\Temp\\CaseTest\\Other.txt";

    const size_t holdsBefore = OrganizeHoldCount();
    OrganizeHold(std::vector<std::wstring>{mixed}, 5000);
    add("hold_blocks_case_insensitively", OrganizeHeld(mixed) && OrganizeHeld(sameDiffCase));
    add("hold_does_not_block_others", !OrganizeHeld(other));
    // 计数是进程全局的，同一轮自检里别的用例也立过保护，所以只能断言增量。
    add("hold_counted", OrganizeHoldCount() == holdsBefore + 1,
        "before=" + std::to_string(holdsBefore) + " after=" + std::to_string(OrganizeHoldCount()));

    // 0 毫秒等于不保护，免得误用把文件永久钉住。
    OrganizeHold(std::vector<std::wstring>{other}, 0);
    add("hold_zero_ms_is_no_hold", !OrganizeHeld(other));

    std::vector<OrganizeItem> plan;
    plan.push_back(OrganizeItem{mixed, L"r", L"b", L"C:\\dst"});
    plan.push_back(OrganizeItem{other, L"r", L"b", L"C:\\dst"});
    size_t dropped = 0;
    auto kept = FilterHolds(plan, dropped);
    add("hold_filter_drops_only_held", kept.size() == 1 && dropped == 1 && kept[0].path == other,
        "kept=" + std::to_string(kept.size()) + " dropped=" + std::to_string(dropped));

    // 到期就该放开
    OrganizeHold(std::vector<std::wstring>{other}, 120);
    add("hold_active_before_expiry", OrganizeHeld(other));
    Sleep(400);
    add("hold_expires", !OrganizeHeld(other), "remaining=" + std::to_string(OrganizeHoldCount()));

    // 撤销应当自动立起保护，不需要调用方记得做这件事。
    const std::wstring src = AppFile(L"hold_src");
    const std::wstring box = AppFile(L"hold_box");
    EnsureDirExists(src);
    EnsureDirExists(box);
    const std::wstring file = JoinPath(src, L"被撤销的文件.txt");
    TouchFile(file, "x");

    const std::wstring hist = AppFile(L"undo-history.txt");
    std::string savedHist;
    bool hadHist = ReadFileBytes(hist, savedHist);
    UndoHistoryClear();

    std::vector<MovedEntry> moves;
    std::wstring err;
    bool moved = MoveFilesTo(std::vector<std::wstring>{file}, box, nullptr, moves, err);
    if (moved && !moves.empty()) UndoHistoryAdd(std::move(moves), L"自检：保护期", std::vector<IconPosition>{});
    add("hold_precondition", moved && UndoHistorySize() == 1, WideToUtf8(err));

    std::wstring undoErr;
    bool undid = UndoHistoryUndoLast(nullptr, undoErr);
    add("undo_sets_hold_itself", undid && PathExists(file) && OrganizeHeld(file), WideToUtf8(undoErr));
    size_t dropped2 = 0;
    auto replanned = FilterHolds(std::vector<OrganizeItem>{OrganizeItem{file, L"r", L"b", box}}, dropped2);
    add("undo_hold_blocks_reorganize", dropped2 == 1 && replanned.empty());

    if (hadHist) WriteFileBytes(hist, savedHist);
    else DeleteFileW(hist.c_str());
    DeleteFileW(file.c_str());
    RemoveDirIfExists(box);
    RemoveDirIfExists(src);
}

// 先在一个副本上做规划和目录改名，全部成功才把副本换成正本：
// 中途失败时内存里的盒子还停在旧名字，不会出现"界面上改了、盘上没改"的半截状态。
void App::RenameBoxTo(const std::wstring& oldName, const std::wstring& newName) {
    // 清洗只做一次，后面规划、算目录、写日志用的都是同一个值：
    // 传原始输入进去的话，含非法字符的名字会在某一步被改写，几步之间就不是同一个名字了。
    const std::wstring clean = SanitizeBoxName(newName);
    std::map<std::wstring, std::wstring> folders;
    for (auto& b : boxes_) {
        BoxConfig c;
        b->Current(c);
        folders[c.name] = b->folder();
    }

    std::vector<FolderRename> renames;
    std::wstring err;
    Layout trial = layout_;
    if (!PlanBoxRename(trial, oldName, clean, folders, FolderForBoxName(clean), renames, err)) {
        if (!err.empty()) {
            Log(L"重命名被拒绝：%s", err.c_str());
            ShowBalloon(L"重命名未执行", err);
        }
        return;
    }

    if (!ApplyFolderRenames(renames, tray_, err)) {
        Log(L"盒子目录改名失败：%s", err.c_str());
        ShowBalloon(L"盒子目录改名失败", err);
        RebuildBoxes();
        return;
    }

    layout_ = trial;
    RebuildBoxes();
    SyncLayoutFromBoxes();
    // 重建后盒子少掉的话，LayoutSave 会把"少一个盒子"直接写进配置。
    // 这种不一致必须留在日志里，而不是等下次启动发现盒子凭空不见了。
    if (boxes_.size() < trial.boxes.size()) {
        Log(L"改名后盒子数从 %zu 变成 %zu，已中止保存以保留原配置", trial.boxes.size(), boxes_.size());
        ShowBalloon(L"重命名后盒子未能全部重建", L"为避免写坏配置，本次没有保存布局。请检查目录是否被其他程序占用。");
        layout_ = trial;
        return;
    }
    LayoutSave(layout_);
    Log(L"盒子已改名：%s -> %s", oldName.c_str(), clean.c_str());
}

void AddRenameChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring root = AppFile(L"rename_root");
    const std::wstring oldFolder = JoinPath(root, L"图片");
    const std::wstring newFolder = JoinPath(root, L"截图");
    const std::wstring mappedFolder = JoinPath(root, L"custom_target");
    RemoveDirIfExists(root);
    EnsureDirExists(oldFolder);
    EnsureDirExists(mappedFolder);
    TouchFile(JoinPath(oldFolder, L"猫.png"), "x");

    Layout lay{};
    lay.boxRoot = root;
    BoxConfig derived;
    derived.name = L"图片";
    derived.rect = RECT{0, 0, 320, 240};
    BoxConfig mapped;
    mapped.name = L"映射盒";
    mapped.rect = RECT{0, 0, 320, 240};
    mapped.folder = L"custom_target";
    BoxConfig agg;
    agg.name = L"合集";
    agg.rect = RECT{0, 0, 320, 240};
    agg.members = L"图片,映射盒";
    agg.tab = L"图片";
    lay.boxes = {derived, mapped, agg};
    RuleConfig rule;
    rule.name = L"规则一";
    rule.boxName = L"图片";
    rule.ext = L".png";
    lay.rules.push_back(rule);

    std::map<std::wstring, std::wstring> folders{{L"图片", oldFolder}, {L"映射盒", mappedFolder},
                                                  {L"合集", JoinPath(root, L"合集")}};

    // 拒绝路径
    std::vector<FolderRename> rr;
    std::wstring err;
    Layout t1 = lay;
    const size_t boxesBeforeReject = t1.boxes.size();
    add("rename_rejects_unknown_box",
        !PlanBoxRename(t1, L"没这个盒子", L"新名", folders, L"", rr, err) && t1.boxes.size() == boxesBeforeReject);
    t1 = lay;
    add("rename_rejects_empty_name", !PlanBoxRename(t1, L"图片", L"   ", folders, L"", rr, err));
    t1 = lay;
    add("rename_rejects_duplicate", !PlanBoxRename(t1, L"图片", L"映射盒", folders, L"", rr, err));
    t1 = lay;
    add("rename_rejects_existing_folder", !PlanBoxRename(t1, L"图片", L"已占用", folders, mappedFolder, rr, err));
    // 共享目录的危险只在"要动磁盘"时存在：派生盒子 图片 和显式写了 folder=图片 的盒子
    // 指向同一个目录，此时改名会把别人的目录一起搬走。
    t1 = lay;
    BoxConfig twin;
    twin.name = L"分身";
    twin.rect = RECT{0, 0, 10, 10};
    twin.folder = L"图片";  // 显式指向派生盒子所用的同一个目录
    t1.boxes.push_back(twin);
    folders[L"分身"] = oldFolder;
    Layout t2 = t1;
    err.clear();
    add("rename_rejects_shared_folder", !PlanBoxRename(t2, L"图片", L"截图", folders, newFolder, rr, err),
        WideToUtf8(err));
    folders.erase(L"分身");

    // 映射盒子只改显示名，不碰目录，所以即使目录和别的盒子共用也允许改。
    Layout t2b = t1;
    rr.clear();
    err.clear();
    add("rename_mapped_allows_shared_folder",
        PlanBoxRename(t2b, L"分身", L"随便改", folders, L"", rr, err) && rr.empty(), WideToUtf8(err));

    // 改名没变时应当成功且什么都不做
    Layout t3 = lay;
    rr.clear();
    add("rename_noop_when_same", PlanBoxRename(t3, L"图片", L"图片", folders, L"", rr, err) && rr.empty());

    // 派生目录的盒子：名字、目录、规则、成员、标签卡全部跟着走
    Layout t4 = lay;
    rr.clear();
    bool planned = PlanBoxRename(t4, L"图片", L"截图", folders, newFolder, rr, err);
    add("rename_derived_planned", planned && rr.size() == 1 && rr[0].from == oldFolder && rr[0].to == newFolder,
        WideToUtf8(err));
    add("rename_updates_box_name", FindBoxIndex(t4, L"截图") < t4.boxes.size() && FindBoxIndex(t4, L"图片") >= t4.boxes.size());
    add("rename_updates_rule_target", t4.rules.size() == 1 && t4.rules[0].boxName == L"截图");
    add("rename_updates_members", t4.boxes[2].members == L"截图,映射盒", WideToUtf8(t4.boxes[2].members));
    add("rename_updates_tab", t4.boxes[2].tab == L"截图", WideToUtf8(t4.boxes[2].tab));

    // 输入含非法字符时，改名结果、目录名、各处引用必须都是同一个清洗后的值
    Layout t4b = lay;
    rr.clear();
    err.clear();
    bool dirtyOk = PlanBoxRename(t4b, L"图片", L"截图[1]", folders, JoinPath(root, L"截图1"), rr, err);
    add("rename_sanitizes_consistently",
        dirtyOk && t4b.boxes[0].name == L"截图1" && t4b.boxes[2].members == L"截图1,映射盒" &&
            t4b.boxes[2].tab == L"截图1" && rr.size() == 1 && rr[0].to == JoinPath(root, L"截图1"),
        "name=" + WideToUtf8(t4b.boxes[0].name) + " err=" + WideToUtf8(err));

    // 映射盒子只改显示名，目录不动
    Layout t5 = lay;
    rr.clear();
    bool mappedPlanned = PlanBoxRename(t5, L"映射盒", L"改叫这个", folders, L"", rr, err);
    add("rename_mapped_keeps_folder", mappedPlanned && rr.empty() && t5.boxes[1].folder == L"custom_target",
        WideToUtf8(err));

    // 真的改名一次，并验证撤销能连目录名一起退回去
    UndoHistoryClear();
    std::wstring applyErr;
    bool applied = ApplyFolderRenames(rr = {FolderRename{oldFolder, newFolder}}, nullptr, applyErr);
    add("rename_applied", applied && !PathExists(oldFolder) && PathExists(JoinPath(newFolder, L"猫.png")),
        WideToUtf8(applyErr));
    std::wstring undoErr;
    bool undid = UndoHistoryUndoLast(nullptr, undoErr);
    add("rename_undone", undid && PathExists(oldFolder) && PathExists(JoinPath(oldFolder, L"猫.png")),
        WideToUtf8(undoErr));

    RemoveDirIfExists(newFolder);
    RemoveDirIfExists(oldFolder);
    RemoveDirIfExists(mappedFolder);
    RemoveDirIfExists(root);
}

void AddInlineRenameChecks(std::vector<Check>& checks, HINSTANCE hi) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const std::wstring root = AppFile(L"inl_rename");
    const std::wstring folder = JoinPath(root, L"就地盒");
    RemoveDirIfExists(root);
    EnsureDirExists(folder);
    TouchFile(JoinPath(folder, L"内容.txt"), "x");

    Box::Register(hi);
    Box box;
    BoxConfig cfg;
    cfg.name = L"就地盒";
    cfg.rect = RECT{40, 700, 40 + 380, 700 + 260};
    add("inl_box_created", box.Create(cfg, folder));
    PumpMessages(400);
    box.SetAlpha(255);

    box.BeginRename();
    PumpMessages(150);
    add("inl_rename_begun", box.renaming());
    HWND edit = FindWindowExW(box.hwnd(), nullptr, L"EDIT", nullptr);
    add("inl_edit_child_exists", edit != nullptr);
    if (!edit) {
        box.EndRename(false);
        box.Destroy();
        RemoveDirIfExists(root);
        return;
    }
    RECT er{};
    GetWindowRect(edit, &er);
    RECT br{};
    GetWindowRect(box.hwnd(), &br);
    add("inl_edit_covers_title_area", er.top >= br.top && er.bottom <= br.top + 60 && er.left > br.left,
        "edit h=" + std::to_string(er.bottom - er.top) + " box h=" + std::to_string(br.bottom - br.top));
    add("inl_edit_visible", IsWindowVisible(edit) != 0);
    // 编辑框是 CLIENTEDGE 边框，画出来是系统灰；和盒子自己的深色填充差别很大。
    // 采样它的边框像素就能证明子窗口控件在分层窗口上真的被绘制出来了。
    HDC s = GetDC(nullptr);
    COLORREF edgePix = GetPixel(s, er.left + 1, er.top + 1);
    COLORREF bodyPix = GetPixel(s, br.left + 340, br.top + 200);
    ReleaseDC(nullptr, s);
    int delta = abs(GetRValue(edgePix) - GetRValue(bodyPix)) + abs(GetGValue(edgePix) - GetGValue(bodyPix)) +
                abs(GetBValue(edgePix) - GetBValue(bodyPix));
    char pd[128];
    snprintf(pd, sizeof pd, "edge=(%d,%d,%d) body=(%d,%d,%d) delta=%d", GetRValue(edgePix), GetGValue(edgePix),
             GetBValue(edgePix), GetRValue(bodyPix), GetGValue(bodyPix), GetBValue(bodyPix), delta);
    add("inl_edit_actually_painted", delta > 60, pd);

    const wchar_t* want = L"新起的名字";
    SetWindowTextW(edit, want);
    PostMessageW(edit, WM_CHAR, VK_RETURN, 0);
    PumpMessages(200);
    add("inl_enter_ends_editing", !box.renaming());
    std::wstring proposed;
    add("inl_enter_commits", box.takeRenameRequest(proposed) && proposed == want, WideToUtf8(proposed));
    add("inl_request_consumed", !box.takeRenameRequest(proposed));

    // Esc 与失焦都必须放弃，不能留下半途状态
    box.BeginRename();
    PumpMessages(100);
    HWND e2 = FindWindowExW(box.hwnd(), nullptr, L"EDIT", nullptr);
    SetWindowTextW(e2, L"不该生效的名字");
    PostMessageW(e2, WM_CHAR, VK_ESCAPE, 0);
    PumpMessages(150);
    add("inl_escape_cancels", !box.renaming() && !box.takeRenameRequest(proposed));

    box.BeginRename();
    PumpMessages(100);
    HWND e3 = FindWindowExW(box.hwnd(), nullptr, L"EDIT", nullptr);
    SetWindowTextW(e3, L"也不该生效");
    PostMessageW(box.hwnd(), WM_COMMAND, MAKEWPARAM(0, EN_KILLFOCUS), reinterpret_cast<LPARAM>(e3));
    PumpMessages(150);
    add("inl_focusloss_cancels", !box.renaming() && !box.takeRenameRequest(proposed));

    box.Destroy();
    RemoveDirIfExists(root);
}

// 至少要让人抓住这么一块标题栏，否则盒子点不到也没法拖回来。
static constexpr int kGrabWidth = 80;
static constexpr int kGrabHeight = 24;

void App::ClampBoxesToScreens(const wchar_t* reason) {
    const auto areas = geometry::MonitorWorkAreas();
    if (areas.empty()) return;

    int moved = 0;
    for (auto& b : boxes_) {
        BoxConfig c;
        b->Current(c);
        const RECT fixed = geometry::ClampIntoMonitors(c.rect, areas, kGrabWidth, kGrabHeight);
        if (fixed.left == c.rect.left && fixed.top == c.rect.top) continue;
        b->MoveTo(fixed);
        moved++;
        Log(L"%s：把盒子「%s」从 (%d,%d) 移到 (%d,%d)", reason, c.name.c_str(), static_cast<int>(c.rect.left),
            static_cast<int>(c.rect.top), static_cast<int>(fixed.left), static_cast<int>(fixed.top));
    }
    if (moved == 0) return;
    SyncLayoutFromBoxes();
    LayoutSave(layout_);
}

void AddGeometryChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    // RECT 没有 operator==，只能逐字段比。
    auto RectSame = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    auto W = [](const RECT& r) { return r.right - r.left; };
    auto H = [](const RECT& r) { return r.bottom - r.top; };

    // 两台屏幕：主屏 0..2000，副屏在主屏左边，坐标是负的。
    const std::vector<RECT> two = {RECT{-1400, 0, -20, 1000}, RECT{0, 0, 2000, 1200}};
    const int gw = 80, gh = 24;

    RECT onScreen = {400, 300, 760, 560};
    add("clamp_leaves_visible_box_alone", RectSame(geometry::ClampIntoMonitors(onScreen, two, gw, gh), onScreen) &&
                                            !geometry::NeedsClamp(onScreen, two, gw, gh));

    RECT farRight = {5000, 300, 5360, 560};
    RECT fixedRight = geometry::ClampIntoMonitors(farRight, two, gw, gh);
    add("clamp_pulls_in_from_right", fixedRight.left <= 2000 - gw && fixedRight.left >= 0 &&
                                         W(fixedRight) == 360 && H(fixedRight) == 260,
        "left=" + std::to_string(fixedRight.left));

    RECT aboveTop = {400, -900, 760, -640};
    RECT fixedTop = geometry::ClampIntoMonitors(aboveTop, two, gw, gh);
    add("clamp_pulls_down_from_above", fixedTop.top == 0 && fixedTop.left == 400 && W(fixedTop) == 360 && H(fixedTop) == 260,
        "top=" + std::to_string(fixedTop.top) + " left=" + std::to_string(fixedTop.left));

    // 关键一条：盒子在左侧副屏上（负坐标），不能被硬拽到主屏。
    RECT onSecond = {-1200, 100, -840, 360};
    RECT fixedSecond = geometry::ClampIntoMonitors(onSecond, two, gw, gh);
    add("clamp_keeps_box_on_secondary", RectSame(fixedSecond, onSecond) && W(fixedSecond) == 360,
        "left=" + std::to_string(fixedSecond.left));
    RECT offSecond = {-4000, 100, -3640, 360};
    RECT fixedOffSecond = geometry::ClampIntoMonitors(offSecond, two, gw, gh);
    add("clamp_pulls_back_onto_secondary", fixedOffSecond.left >= -1400 && fixedOffSecond.left <= -20 - gw &&
                                               fixedOffSecond.top == 100 && W(fixedOffSecond) == 360 && H(fixedOffSecond) == 260,
        "left=" + std::to_string(fixedOffSecond.left));

    // 盒体探出屏幕底边但标题栏够得着：不该动它。
    RECT hanging = {400, 1150, 760, 1450};
    add("clamp_allows_body_overflow", RectSame(geometry::ClampIntoMonitors(hanging, two, gw, gh), hanging) &&
                                         !geometry::NeedsClamp(hanging, two, gw, gh));

    // 比屏幕还高的盒子：至少让标题栏留在可用区内。
    RECT huge = {400, -5000, 1200, 9000};
    RECT fixedHuge = geometry::ClampIntoMonitors(huge, two, gw, gh);
    add("clamp_handles_taller_than_screen", fixedHuge.top >= 0 && fixedHuge.top <= 1200 - gh &&
                                                H(fixedHuge) == 14000,
        "top=" + std::to_string(fixedHuge.top));

    RECT nowhere = {9000, 9000, 9360, 9260};
    add("clamp_no_monitors_changes_nothing", RectSame(geometry::ClampIntoMonitors(nowhere, {}, gw, gh), nowhere));
    add("clamp_needs_matches_result",
        geometry::NeedsClamp(farRight, two, gw, gh) && !geometry::NeedsClamp(onSecond, two, gw, gh));

    // 真实显示器列表必须非空，否则夹紧逻辑等于空转。
    const auto real = geometry::MonitorWorkAreas();
    add("clamp_real_monitors_present", !real.empty(), "count=" + std::to_string(real.size()));
    bool allSane = !real.empty();
    for (const auto& r : real) {
        if (r.right <= r.left || r.bottom <= r.top) allSane = false;
    }
    add("clamp_real_monitors_sane", allSane);
}

// 切换成员：先在副本上做，成功才换正本并重建，失败时界面上的盒子完全不动。
void App::ShowMergeMenu(const std::wstring& boxName, POINT screenPt) {
    HWND boxHwnd = nullptr;
    for (auto& b : boxes_) {
        BoxConfig c;
        b->Current(c);
        if (c.name == boxName) {
            boxHwnd = b->hwnd();
            break;
        }
    }
    if (!tray_ || !boxHwnd || boxName.empty()) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"勾选即合并，再点一次移出");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    UINT idBase = 1;
    std::vector<std::wstring> targets;
    for (const auto& c : layout_.boxes) {
        if (_wcsicmp(c.name.c_str(), boxName.c_str()) == 0) continue;
        const bool member = IsMergeMember(layout_, boxName, c.name);
        // 已经是标签盒的不能当成员，否则会出现套了两层的标签卡。
        const bool nested = IsAggregate(layout_, c.name);
        UINT flags = MF_STRING;
        if (member) flags |= MF_CHECKED;
        if (nested) flags |= MF_GRAYED;
        std::wstring label = c.name;
        if (nested) label += L"（已是标签盒）";
        AppendMenuW(menu, flags, idBase, label.c_str());
        targets.push_back(c.name);
        idBase++;
    }
    if (targets.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"没有可以合并的盒子");
    }

    SetForegroundWindow(tray_);
    const int picked = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD, screenPt.x, screenPt.y, 0,
                                      tray_, nullptr);
    DestroyMenu(menu);
    if (picked < 1 || static_cast<size_t>(picked) > targets.size()) return;

    const std::wstring memberName = targets[static_cast<size_t>(picked) - 1];
    Layout trial = layout_;
    std::wstring err;
    if (!ToggleMergeMember(trial, boxName, memberName, err)) {
        Log(L"合并操作被拒绝：%s", err.c_str());
        ShowBalloon(L"合并未执行", err);
        return;
    }
    layout_ = trial;
    RebuildBoxes();
    SyncLayoutFromBoxes();
    LayoutSave(layout_);
    Log(L"合并关系已更新：%s 与 %s", boxName.c_str(), memberName.c_str());
    // 重新弹出来，让一次操作里可以连续勾好几个盒子。
    ShowMergeMenu(boxName, screenPt);
}

// 这里所有断言都遵守同一个写法：先把会改动状态的调用单独执行完，再拿结果和细节去 add。
// 写成 add(name, Toggle(...), DetailOf(被 Toggle 改过的东西)) 时，C++ 不保证参数求值顺序，
// detail 常常在 Toggle 之前就算好了，于是断言是对的、给出来的理由却是上一步的，
// 真失败时会把人往错的方向带。这个坑我已经踩过两次，不能再写第三遍。
void AddMergeChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    auto makeLayout = [](void) {
        Layout l{};
        const wchar_t* names[] = {L"合集", L"图片", L"文档", L"其他"};
        for (int i = 0; i < 4; ++i) {
            BoxConfig b;
            b.name = names[i];
            b.rect = RECT{0, 0, 320, 240};
            l.boxes.push_back(b);
        }
        return l;
    };

    std::wstring err;
    bool ok = false;

    Layout lay = makeLayout();
    add("merge_starts_plain", !IsAggregate(lay, L"合集") && !IsMergeMember(lay, L"合集", L"图片"));

    ok = ToggleMergeMember(lay, L"合集", L"图片", err);
    add("merge_toggle_on", ok && lay.boxes[0].members == L"图片", WideToUtf8(lay.boxes[0].members));

    ok = ToggleMergeMember(lay, L"合集", L"文档", err);
    add("merge_toggle_second", ok && lay.boxes[0].members == L"图片,文档", WideToUtf8(lay.boxes[0].members));

    add("merge_reports_aggregate", IsAggregate(lay, L"合集") && IsMergeMember(lay, L"合集", L"文档"));
    add("merge_member_query_true", IsMergeMember(lay, L"合集", L"图片"));
    add("merge_member_query_false", !IsMergeMember(lay, L"合集", L"其他"));

    Layout off = lay;
    ok = ToggleMergeMember(off, L"合集", L"图片", err);
    add("merge_toggle_off", ok && off.boxes[0].members == L"文档", WideToUtf8(off.boxes[0].members));

    const std::wstring beforeSelfMerge = lay.boxes[0].members;
    Layout r1 = lay;
    err.clear();
    ok = ToggleMergeMember(r1, L"合集", L"合集", err);
    add("merge_rejects_self", !ok && r1.boxes[0].members == beforeSelfMerge, WideToUtf8(err));

    Layout r2 = lay;
    err.clear();
    ok = ToggleMergeMember(r2, L"合集", L"没这个盒子", err);
    add("merge_rejects_unknown", !ok && r2.boxes[0].members == beforeSelfMerge, WideToUtf8(err));

    Layout nested = makeLayout();
    ToggleMergeMember(nested, L"图片", L"文档", err);
    Layout nestedTry = nested;
    err.clear();
    ok = ToggleMergeMember(nestedTry, L"合集", L"图片", err);
    add("merge_rejects_nested_aggregate", !ok && nestedTry.boxes[0].members.empty(), WideToUtf8(err));

    Layout cycle = makeLayout();
    ToggleMergeMember(cycle, L"合集", L"图片", err);
    Layout cycleTry = cycle;
    err.clear();
    ok = ToggleMergeMember(cycleTry, L"图片", L"合集", err);
    // 双向合并由"成员不能是标签盒"这条挡住，不存在单独的成环路径。
    add("merge_rejects_two_way", !ok && cycleTry.boxes[1].members.empty() && err.find(L"标签盒") != std::wstring::npos,
        WideToUtf8(err));

    Layout g = makeLayout();
    g.boxes[0].grouped = true;
    ok = ToggleMergeMember(g, L"合集", L"图片", err);
    add("merge_disables_grouping", ok && !g.boxes[0].grouped);

    Layout t = makeLayout();
    ToggleMergeMember(t, L"合集", L"图片", err);
    ToggleMergeMember(t, L"合集", L"文档", err);
    t.boxes[0].tab = L"文档";
    ToggleMergeMember(t, L"合集", L"文档", err);
    add("merge_clears_stale_tab", t.boxes[0].tab.empty(), WideToUtf8(t.boxes[0].tab));

    Layout t2 = makeLayout();
    ToggleMergeMember(t2, L"合集", L"图片", err);
    t2.boxes[0].tab = L"图片";
    ToggleMergeMember(t2, L"合集", L"图片", err);
    add("merge_clears_tab_when_emptied", t2.boxes[0].members.empty() && t2.boxes[0].tab.empty());
}

// 生成并换上当前配色的托盘图标。返回 false 表示连生成都失败、只能继续用兜底图标。
bool App::RefreshTrayIcon() {
    const int size = GetSystemMetrics(SM_CXSMICON);
    HICON made = CreateBoxIcon(ThemeCurrent(), size > 1 ? size : 16);
    HICON next = made;
    bool nextOwned = made != nullptr;
    if (!next) {
        next = LoadIconW(nullptr, IDI_APPLICATION);
        nextOwned = false;
    }
    if (!next) return false;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = tray_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON;
    nid.hIcon = next;
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        if (nextOwned) DestroyIcon(next);
        return false;
    }

    // 先换成功再销毁旧的，避免中途把正在用的图标销毁掉。
    if (iconOwned_ && icon_ && icon_ != next) DestroyIcon(icon_);
    icon_ = next;
    iconOwned_ = nextOwned;
    return true;
}

void AddIconChecks(std::vector<Check>& checks) {
    auto add = [&](const char* n, bool ok, std::string detail = {}) {
        checks.push_back(Check{n, ok, std::move(detail)});
    };

    const Palette p = DarkPalette();
    const int size = 32;
    std::vector<uint32_t> argb;
    std::vector<uint8_t> mask;
    BuildBoxIconPixels(p, size, argb, mask);
    add("icon_pixels_sized", argb.size() == static_cast<size_t>(size) * static_cast<size_t>(size),
        "n=" + std::to_string(argb.size()));

    auto px = [&](int x, int y) { return argb[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)]; };
    auto transparentAt = [&](int x, int y) {
        const size_t stride = (static_cast<size_t>(size) + 7) / 8;
        return (mask[static_cast<size_t>(y) * stride + static_cast<size_t>(x / 8)] >> (7 - (x % 8))) & 1u;
    };
    const uint32_t expectBody = (255u << 24) | ((p.body >> 16) << 16) | ((p.body >> 8) << 8) | (p.body & 0xFFu);
    const uint32_t center = px(size / 2, size - 3);
    add("icon_body_pixel", center == expectBody,
        "got=" + std::to_string(center) + " want=" + std::to_string(expectBody));
    add("icon_corner_transparent", transparentAt(0, 0) && !transparentAt(size / 2, size - 2),
        "corner=" + std::to_string(transparentAt(0, 0)) + " bottommid=" + std::to_string(transparentAt(size / 2, size - 2)));
    add("icon_title_band_differs", px(size / 2, 2) != expectBody && px(size / 2, 2) != 0u,
        "band=" + std::to_string(px(size / 2, 2)));
    add("icon_uses_both_palettes", DarkPalette().body != LightPalette().body);

    std::vector<uint32_t> lightArgb;
    std::vector<uint8_t> lightMask;
    BuildBoxIconPixels(LightPalette(), size, lightArgb, lightMask);
    add("icon_light_differs_from_dark", lightArgb != argb);

    HICON icon = CreateBoxIcon(p, size);
    add("icon_handle_created", icon != nullptr);
    if (icon) {
        ICONINFO info = {};
        bool got = GetIconInfo(icon, &info) != FALSE;
        int w = 0, h = 0;
        if (got && info.hbmColor) {
            BITMAP bm = {};
            if (GetObjectW(info.hbmColor, sizeof bm, &bm)) {
                w = bm.bmWidth;
                h = bm.bmHeight;
            }
        }
        add("icon_dimensions", got && w == size && h == size, "w=" + std::to_string(w) + " h=" + std::to_string(h));
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        DestroyIcon(icon);
    }
    // 尺寸非法时不能崩，也不能返回一个用不了的句柄。
    add("icon_rejects_bad_size", CreateBoxIcon(p, 0) == nullptr && CreateBoxIcon(p, 1) == nullptr);
}
