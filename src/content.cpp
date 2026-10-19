#include "content.h"

#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>

#include "util.h"

static IWICImagingFactory* g_wic = nullptr;

bool WicInit() {
    if (g_wic) return true;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_wic));
    LogHr(L"WIC imaging factory", hr);
    if (FAILED(hr)) g_wic = nullptr;
    return g_wic != nullptr;
}

void WicShutdown() {
    if (g_wic) {
        g_wic->Release();
        g_wic = nullptr;
    }
}

static HBITMAP LoadShellBitmap(const std::wstring& path, unsigned size) {
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return nullptr;

    SIZE want{static_cast<LONG>(size), static_cast<LONG>(size)};
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage(want, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &hbmp);
    factory->Release();
    return SUCCEEDED(hr) ? hbmp : nullptr;
}

// 缓存上限：超过就整体清空。分类标签卡和多标签盒子共用一份缓存，
// 正常用量远到不了这个量级，清空最坏也只是下次刷新多取一遍图标。
static constexpr size_t kIconCacheCap = 600;

IconCache::~IconCache() {
    for (auto& kv : map_) {
        if (kv.second) DeleteObject(kv.second);
    }
}

HBITMAP IconCache::Get(const std::wstring& path, unsigned size) {
    auto it = map_.find(path);
    if (it != map_.end()) return it->second;
    HBITMAP hbmp = LoadShellBitmap(path, size);
    if (!hbmp) return nullptr;  // 取失败不缓存，下次刷新还会再试
    if (map_.size() >= kIconCacheCap) {
        for (auto& kv : map_) {
            if (kv.second) DeleteObject(kv.second);
        }
        map_.clear();
    }
    map_[path] = hbmp;
    return hbmp;
}

Content::~Content() { ClearItems(); }

void Content::SetFolder(const std::wstring& folder, unsigned iconPx, IconCache* cache) {
    folder_ = folder;
    iconPx_ = iconPx < 16 ? 16 : iconPx;
    cache_ = cache;
}

void Content::ClearItems() {
    for (auto& it : items_) {
        if (it.bitmap) it.bitmap->Release();
        it.bitmap = nullptr;
    }
    items_.clear();  // hbmp 归 IconCache 所有，这里不能删
    uploadedGen_ = 0;
}

void Content::CollectPaths(std::set<std::wstring>& into) const {
    for (const auto& it : items_) into.insert(it.path);
}

void Content::ReleaseBitmaps() {
    for (auto& it : items_) {
        if (it.bitmap) {
            it.bitmap->Release();
            it.bitmap = nullptr;
        }
    }
    uploadedGen_ = 0;
}

unsigned Content::uploadedCount() const {
    unsigned n = 0;
    for (const auto& it : items_) {
        if (it.bitmap) n++;
    }
    return n;
}

bool Content::Refresh() {
    struct Entry {
        std::wstring path;
        std::wstring name;
        bool isDir = false;
    };
    std::vector<Entry> next;

    if (!folder_.empty() && PathExists(folder_)) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(JoinPath(folder_, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                // desktop.ini 这类系统文件不是用户内容，画出来只是噪音。
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
                Entry e;
                e.name = name;
                e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.path = JoinPath(folder_, name);
                next.push_back(std::move(e));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    std::sort(next.begin(), next.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });

    bool same = next.size() == items_.size();
    for (size_t i = 0; same && i < next.size(); ++i) {
        if (next[i].path != items_[i].path) same = false;
    }
    if (same) return false;

    ClearItems();

    for (auto& e : next) {
        BoxItem item;
        item.path = std::move(e.path);
        item.name = std::move(e.name);
        item.isDirectory = e.isDir;
        item.hbmp = cache_ ? cache_->Get(item.path, iconPx_) : nullptr;
        items_.push_back(std::move(item));
    }

    unsigned withIcon = 0;
    for (const auto& it : items_) {
        if (it.hbmp) withIcon++;
    }
    Log(L"内容刷新 %s：%u 项，%u 项有图标", folder_.c_str(), static_cast<unsigned>(items_.size()), withIcon);
    return true;
}

void Content::UploadTo(ID2D1RenderTarget* rt, unsigned generation) {
    if (!rt || !g_wic) return;
    if (uploadedGen_ != generation) {
        ReleaseBitmaps();
        uploadedGen_ = generation;
    }

    for (auto& it : items_) {
        if (!it.hbmp || it.bitmap) continue;
        IWICBitmap* wic = nullptr;
        // Shell 给的是预乘 BGRA，必须走一遍 WIC 才能把 alpha 正确带进 Direct2D。
        HRESULT hr = g_wic->CreateBitmapFromHBITMAP(it.hbmp, nullptr, WICBitmapUsePremultipliedAlpha, &wic);
        if (FAILED(hr) || !wic) continue;
        ID2D1Bitmap* bmp = nullptr;
        HRESULT hr2 = rt->CreateBitmapFromWicBitmap(wic, nullptr, &bmp);
        if (SUCCEEDED(hr2)) it.bitmap = bmp;
        wic->Release();
    }
}
