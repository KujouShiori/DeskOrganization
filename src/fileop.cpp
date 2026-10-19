#include "fileop.h"

#include <shobjidl.h>

#include <algorithm>

#include "organize.h"

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return std::wstring();
    std::wstring parent = path.substr(0, slash);
    if (!parent.empty() && parent.back() == L'\\') parent.pop_back();
    return parent;
}

static bool Taken(const std::wstring& destDir, const std::wstring& name,
                  const std::vector<std::wstring>& reserved) {
    if (PathExists(JoinPath(destDir, name))) return true;
    return std::find(reserved.begin(), reserved.end(), name) != reserved.end();
}

std::wstring PickFreeName(const std::wstring& destDir, const std::wstring& fileName,
                          const std::vector<std::wstring>& reserved) {
    if (!Taken(destDir, fileName, reserved)) return fileName;

    std::wstring stem = fileName;
    std::wstring ext;
    size_t dot = fileName.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = fileName.substr(0, dot);
        ext = fileName.substr(dot);
    }
    for (int i = 2; i < 1000; ++i) {
        std::wstring candidate = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!Taken(destDir, candidate, reserved)) return candidate;
    }
    return fileName;
}

static IFileOperation* CreateOperation(HWND owner, std::wstring& errorText) {
    IFileOperation* op = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (FAILED(hr) || !op) {
        wchar_t b[128];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"创建 IFileOperation 失败 hr=0x%08X", static_cast<unsigned>(hr));
        errorText = b;
        return nullptr;
    }

    // FOFX_ADDUNDORECORD 让 Explorer 自己的撤销也能看到这次操作；
    // 我们自己的撤销栈另算，不依赖它。
    DWORD flags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI | FOFX_ADDUNDORECORD;
    if (FAILED(op->SetOperationFlags(flags))) {
        op->SetOperationFlags(FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
    }
    if (owner) op->SetOwnerWindow(owner);
    return op;
}

static bool RunOperation(IFileOperation* op, std::wstring& errorText) {
    HRESULT hr = op->PerformOperations();
    if (FAILED(hr)) {
        wchar_t b[128];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"PerformOperations 失败 hr=0x%08X gle=%lu",
                     static_cast<unsigned>(hr), GetLastError());
        errorText = b;
        return false;
    }
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (aborted) {
        errorText = L"操作被中止";
        return false;
    }
    return true;
}

bool MoveFilesTo(const std::vector<std::wstring>& sources, const std::wstring& destDir, HWND owner,
                 std::vector<MovedEntry>& outMoves, std::wstring& errorText) {
    outMoves.clear();
    errorText.clear();

    if (destDir.empty() || !PathExists(destDir)) {
        errorText = L"目标目录不存在";
        return false;
    }

    std::vector<std::pair<std::wstring, std::wstring>> plan;
    std::vector<std::wstring> reserved;
    int skippedSameDir = 0;
    for (const auto& src : sources) {
        if (src.empty() || !PathExists(src)) continue;
        std::wstring parent = ParentOf(src);
        // 已经在盒子的目录里了，拖进去不是移动。
        if (_wcsicmp(parent.c_str(), destDir.c_str()) == 0) {
            skippedSameDir++;
            continue;
        }
        std::wstring chosen = PickFreeName(destDir, FileNameOf(src), reserved);
        reserved.push_back(chosen);
        plan.emplace_back(src, chosen);
    }

    if (plan.empty()) {
        if (skippedSameDir > 0) Log(L"移动跳过：%d 项本来就在盒子里", skippedSameDir);
        return true;
    }

    IFileOperation* op = CreateOperation(owner, errorText);
    if (!op) return false;

    bool ok = true;
    for (auto& item : plan) {
        IShellItem* srcItem = nullptr;
        IShellItem* dstDir = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(item.first.c_str(), nullptr, IID_PPV_ARGS(&srcItem));
        if (FAILED(hr)) {
            wchar_t b[160];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"源项目解析失败 hr=0x%08X", static_cast<unsigned>(hr));
            errorText = b;
            ok = false;
            break;
        }
        hr = SHCreateItemFromParsingName(destDir.c_str(), nullptr, IID_PPV_ARGS(&dstDir));
        if (FAILED(hr)) {
            srcItem->Release();
            wchar_t b[160];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"目标目录解析失败 hr=0x%08X", static_cast<unsigned>(hr));
            errorText = b;
            ok = false;
            break;
        }
        hr = op->MoveItem(srcItem, dstDir, item.second.c_str(), nullptr);
        srcItem->Release();
        dstDir->Release();
        if (FAILED(hr)) {
            wchar_t b[160];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"MoveItem 排队失败 hr=0x%08X", static_cast<unsigned>(hr));
            errorText = b;
            ok = false;
            break;
        }
    }

    if (ok) ok = RunOperation(op, errorText);
    op->Release();

    if (!ok) return false;

    for (auto& item : plan) {
        std::wstring target = JoinPath(destDir, item.second);
        if (!PathExists(target)) {
            errorText = L"Shell 报告成功但目标文件不存在：" + target;
            Log(L"%s", errorText.c_str());
            return false;
        }
        if (PathExists(item.first)) {
            errorText = L"源文件还在，移动没生效：" + item.first;
            Log(L"%s", errorText.c_str());
            return false;
        }
        outMoves.push_back(MovedEntry{item.first, target});
    }

    Log(L"移动完成 %zu 项到 %s", outMoves.size(), destDir.c_str());
    return true;
}

bool UndoMoves(const std::vector<MovedEntry>& moves, HWND owner, std::wstring& errorText) {
    errorText.clear();
    if (moves.empty()) return true;

    IFileOperation* op = CreateOperation(owner, errorText);
    if (!op) return false;

    bool ok = true;
    // 倒着放回去，后来的先撤销，避免同名占位导致前面的失败。
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        if (!PathExists(it->to)) {
            wchar_t b[256];
            _snwprintf_s(b, _countof(b), _TRUNCATE, L"撤销时发现文件已不在：%s", it->to.c_str());
            errorText = b;
            ok = false;
            break;
        }
        IShellItem* srcItem = nullptr;
        IShellItem* dstDir = nullptr;
        std::wstring parent = ParentOf(it->from);
        if (!PathExists(parent)) EnsureDirExists(parent);

        HRESULT hr = SHCreateItemFromParsingName(it->to.c_str(), nullptr, IID_PPV_ARGS(&srcItem));
        if (FAILED(hr)) {
            errorText = L"撤销时源项目解析失败";
            ok = false;
            break;
        }
        hr = SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&dstDir));
        if (FAILED(hr)) {
            srcItem->Release();
            errorText = L"撤销时原目录解析失败";
            ok = false;
            break;
        }
        hr = op->MoveItem(srcItem, dstDir, FileNameOf(it->from).c_str(), nullptr);
        srcItem->Release();
        dstDir->Release();
        if (FAILED(hr)) {
            errorText = L"撤销时 MoveItem 排队失败";
            ok = false;
            break;
        }
    }

    if (ok) ok = RunOperation(op, errorText);
    op->Release();
    if (!ok) return false;

    for (const auto& m : moves) {
        if (!PathExists(m.from) || PathExists(m.to)) {
            errorText = L"撤销后状态不对：" + m.from;
            Log(L"%s", errorText.c_str());
            return false;
        }
    }
    Log(L"撤销完成 %zu 项", moves.size());
    return true;
}

struct UndoRecord {
    std::vector<MovedEntry> moves;
    std::wstring label;
    std::vector<IconPosition> positionsBefore;
    // 跨会话的信息只在磁盘上存在；从文件读进来的条目标记出来，好让菜单文案带上时间。
    std::wstring stamp;
    bool crossSession = false;
};

static std::vector<UndoRecord> g_history;
static constexpr size_t kMaxHistory = 25;

static std::wstring NowStamp();


void UndoHistoryAdd(std::vector<MovedEntry>&& moves, const std::wstring& label,
                    std::vector<IconPosition> positionsBefore) {
    if (moves.empty()) return;
    UndoRecord rec;
    rec.moves = std::move(moves);
    rec.label = label;
    rec.positionsBefore = std::move(positionsBefore);
    rec.stamp = NowStamp();
    g_history.push_back(std::move(rec));
    if (g_history.size() > kMaxHistory) g_history.erase(g_history.begin());
    Log(L"撤销栈入栈：%s（当前 %zu 条）", label.c_str(), g_history.size());
    UndoHistorySave();
}

bool UndoHistoryLastLabel(std::wstring& outLabel) {
    if (g_history.empty()) return false;
    const UndoRecord& rec = g_history.back();
    outLabel = rec.label;
    // 上一次运行留下的记录带上时间，否则一开机点撤销根本不知道在撤什么。
    if (rec.crossSession && !rec.stamp.empty()) outLabel += L"（" + rec.stamp + L"）";
    return true;
}

bool UndoHistoryUndoLast(HWND owner, std::wstring& errorText) {
    if (g_history.empty()) {
        errorText = L"没有可撤销的操作";
        return false;
    }
    UndoRecord rec = std::move(g_history.back());
    g_history.pop_back();

    // 文件回到桌面时 Shell 会另挑空位放图标，所以撤销成功后还要把它摆回原来的格子。
    if (!UndoMoves(rec.moves, owner, errorText)) {
        // 撤销失败不能把记录丢掉，否则用户的文件就再也没有回头的路了。
        Log(L"撤销失败，记录退回栈顶：%s", errorText.c_str());
        g_history.push_back(std::move(rec));
        return false;
    }

    // 先立保护期再忙别的：否则中间夹进来的自动整理会立刻把刚放回的文件又搬走，
    // 用户在右键菜单上点撤销就看起来"什么都没发生"。
    std::vector<std::wstring> backTo;
    for (const auto& m : rec.moves) backTo.push_back(m.from);
    OrganizeHold(backTo, kUndoHoldMs);

    if (!rec.positionsBefore.empty()) {
        std::wstring posErr;
        if (IconViewRestore(rec.positionsBefore, posErr)) {
            Log(L"撤销后 %zu 个图标已放回原坐标", rec.positionsBefore.size());
        } else {
            Log(L"撤销后坐标还原不完整：%s", posErr.c_str());
        }
    }
    UndoHistorySave();
    return true;
}

size_t UndoHistorySize() { return g_history.size(); }



void UndoHistoryClear() {
    g_history.clear();
    UndoHistorySave();
}

bool DeleteFiles(const std::vector<std::wstring>& paths, HWND owner, std::wstring& errorText) {
    errorText.clear();
    std::vector<std::wstring> existing;
    for (const auto& p : paths) {
        if (!p.empty() && PathExists(p)) existing.push_back(p);
    }
    if (existing.empty()) return true;

    IFileOperation* op = CreateOperation(owner, errorText);
    if (!op) return false;

    bool ok = true;
    for (const auto& p : existing) {
        IShellItem* item = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(hr) || !item) {
            errorText = L"删除时项目解析失败";
            ok = false;
            break;
        }
        hr = op->DeleteItem(item, nullptr);
        item->Release();
        if (FAILED(hr)) {
            errorText = L"DeleteItem 排队失败";
            ok = false;
            break;
        }
    }

    if (ok) ok = RunOperation(op, errorText);
    op->Release();
    if (!ok) return false;

    for (const auto& p : existing) {
        if (PathExists(p)) {
            errorText = L"Shell 报告删除成功但文件还在：" + p;
            return false;
        }
    }
    return true;
}

// ---- 撤销栈持久化 ----
// 撤销栈原来是进程内存里的，重启程序就没了，于是"反正有撤销兜底"这句话
// 只在本次运行内成立。存成文件之后，昨天自动归走的东西今天还能撤销回来。
static std::wstring UndoFilePath() { return AppFile(L"undo-history.txt"); }

static std::wstring NowStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[32];
    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%02d-%02d %02d:%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

bool UndoHistorySave() {
    std::wstring text = L"# DeskOrganization 撤销栈，程序维护，不用手改。\n";
    text += L"# 最新的条目在最后。用竖线分隔是因为它在 Windows 文件名里是非法字符，路径中不会出现。\n";
    for (const auto& rec : g_history) {
        text += L"[entry]\n";
        text += L"label = " + rec.label + L"\n";
        text += L"at = " + rec.stamp + L"\n";
        for (const auto& m : rec.moves) text += L"move = " + m.from + L"|" + m.to + L"\n";
        for (const auto& p : rec.positionsBefore) {
            text += L"icon = " + p.name + L"|" + std::to_wstring(p.x) + L"|" + std::to_wstring(p.y) + L"\n";
        }
    }
    return WriteFileBytes(UndoFilePath(), WideToUtf8(text));
}

static std::wstring AfterFirst(const std::wstring& s, wchar_t sep) {
    size_t p = s.find(sep);
    return p == std::wstring::npos ? std::wstring() : s.substr(p + 1);
}

static std::wstring BeforeFirst(const std::wstring& s, wchar_t sep) {
    size_t p = s.find(sep);
    return p == std::wstring::npos ? s : s.substr(0, p);
}

void UndoHistoryLoad() {
    g_history.clear();
    std::string raw;
    if (!ReadFileBytes(UndoFilePath(), raw)) return;

    std::wstring text = Utf8ToWide(raw.c_str());
    if (!text.empty() && text.front() == L'\uFEFF') text.erase(0, 1);

    UndoRecord cur;
    bool inEntry = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = nl == std::wstring::npos ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (nl == std::wstring::npos) break;
        start = nl + 1;

        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        line = line.substr(b);
        if (line.front() == L'#') continue;

        auto flush = [&]() {
            if (inEntry && !cur.moves.empty()) g_history.push_back(cur);
            cur = UndoRecord{};
        };

        if (line == L"[entry]") {
            flush();
            inEntry = true;
            cur.crossSession = true;
            continue;
        }
        if (!inEntry) continue;

        std::wstring key = BeforeFirst(line, L'=');
        while (!key.empty() && key.back() == L' ') key.pop_back();
        std::wstring val = AfterFirst(line, L'=');
        while (!val.empty() && val.front() == L' ') val.erase(val.begin());

        if (key == L"label") cur.label = val;
        else if (key == L"at") cur.stamp = val;
        else if (key == L"move") {
            MovedEntry m;
            m.from = BeforeFirst(val, L'|');
            m.to = AfterFirst(val, L'|');
            if (!m.from.empty() && !m.to.empty()) cur.moves.push_back(std::move(m));
        } else if (key == L"icon") {
            IconPosition p;
            p.name = BeforeFirst(val, L'|');
            std::wstring rest = AfterFirst(val, L'|');
            size_t bar = rest.find(L'|');
            if (bar == std::wstring::npos) continue;
            p.x = static_cast<LONG>(_wtoi(rest.substr(0, bar).c_str()));
            p.y = static_cast<LONG>(_wtoi(rest.substr(bar + 1).c_str()));
            if (!p.name.empty()) cur.positionsBefore.push_back(std::move(p));
        }
    }
    if (inEntry && !cur.moves.empty()) g_history.push_back(cur);
    if (g_history.size() > kMaxHistory) {
        g_history.erase(g_history.begin(), g_history.end() - static_cast<ptrdiff_t>(kMaxHistory));
    }
    if (!g_history.empty()) Log(L"载入上次运行的撤销记录 %zu 条", g_history.size());
}

// Windows 名字大小写不敏感。
static bool SameText(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// Windows 文件名里的非法字符集合。转义写错时字面量长度会变，所以在这里钉死，
// 让它变成编译错误而不是一个悄悄失效的校验。
static constexpr wchar_t kIllegalNameChars[] = L"\\/:*?\"<>|";
static_assert(sizeof(kIllegalNameChars) / sizeof(wchar_t) == 10, "非法字符集合被转义写坏了");

bool RenamePath(const std::wstring& path, const std::wstring& newName, HWND owner, MovedEntry& outMove,
                std::wstring& errorText) {
    outMove = MovedEntry{};
    errorText.clear();
    if (path.empty() || !PathExists(path)) {
        errorText = L"要改名的东西不存在";
        return false;
    }
    // Windows 文件名里不能出现这些字符；写错一个反斜杠这个集合就会悄悄变形。
    if (newName.empty() || newName.find_first_of(kIllegalNameChars) != std::wstring::npos) {
        errorText = L"新名字里有不能用在文件名里的字符";
        return false;
    }
    const std::wstring parent = ParentOf(path);
    const std::wstring target = JoinPath(parent, newName);
    if (SameText(FileNameOf(path), newName)) {
        outMove = MovedEntry{path, target};
        return true;  // 名字没变，交给调用方当无事发生
    }
    if (PathExists(target)) {
        errorText = L"同目录下已经有「" + newName + L"」";
        return false;
    }

    IFileOperation* op = CreateOperation(owner, errorText);
    if (!op) return false;

    bool ok = true;
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) {
        errorText = L"改名时项目解析失败";
        ok = false;
    } else {
        hr = op->RenameItem(item, newName.c_str(), nullptr);
        item->Release();
        if (FAILED(hr)) {
            errorText = L"RenameItem 排队失败";
            ok = false;
        }
    }
    if (ok) ok = RunOperation(op, errorText);
    op->Release();
    if (!ok) return false;

    if (!PathExists(target) || PathExists(path)) {
        errorText = L"Shell 报告改名成功但结果不对：" + target;
        return false;
    }
    outMove = MovedEntry{path, target};
    return true;
}
