#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1.h>
#include <windows.h>

struct BoxItem {
    std::wstring path;
    std::wstring name;
    bool isDirectory = false;
    HBITMAP hbmp = nullptr;        // 归 IconCache 所有，这里只是借用
    ID2D1Bitmap* bitmap = nullptr; // 上传到当前 render target 的副本，归本项所有
};

// 图标按路径共享缓存。多标签盒子切标签卡时换的是目录，缓存若挂在 Content 上
// 就会每次重新向 Shell 要图标，所以独立出来由盒子持有。
class IconCache {
 public:
    IconCache() = default;
    ~IconCache();
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    HBITMAP Get(const std::wstring& path, unsigned size);
    size_t size() const { return map_.size(); }

 private:
    std::unordered_map<std::wstring, HBITMAP> map_;
};

// 一个盒子就是一个真实目录的视图。
class Content {
 public:
    Content() = default;
    ~Content();
    Content(const Content&) = delete;
    Content& operator=(const Content&) = delete;

    void SetFolder(const std::wstring& folder, unsigned iconPx, IconCache* cache);
    bool Refresh();
    void UploadTo(ID2D1RenderTarget* rt, unsigned generation);
    void ReleaseBitmaps();

    size_t count() const { return items_.size(); }
    unsigned uploadedCount() const;
    bool empty() const { return items_.empty(); }
    const BoxItem& at(size_t i) const { return items_[i]; }
    const std::wstring& folder() const { return folder_; }
    unsigned iconPx() const { return iconPx_; }
    void CollectPaths(std::set<std::wstring>& into) const;

 private:
    void ClearItems();

    std::wstring folder_;
    unsigned iconPx_ = 44;
    unsigned uploadedGen_ = 0;
    std::vector<BoxItem> items_;
    IconCache* cache_ = nullptr;
};

bool WicInit();
void WicShutdown();
