#include "organize.h"

#include <algorithm>
#include <map>

#include "fileop.h"
#include "iconview.h"
#include "util.h"

static bool PathSame(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::vector<OrganizeItem> PlanOrganize(const std::wstring& sourceDir, const std::vector<RuleConfig>& rules,
                                       const std::vector<OrganizeTarget>& targets) {
    std::vector<OrganizeItem> plan;
    if (sourceDir.empty() || rules.empty()) return plan;

    struct Entry {
        std::wstring name;
        bool isDir = false;
    };
    std::vector<Entry> entries;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(sourceDir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return plan;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
        entries.push_back(Entry{name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    for (const auto& e : entries) {
        std::wstring path = JoinPath(sourceDir, e.name);
        // 盒子自己的目录就在桌面根下时，绝不能把某个盒子当成文件搬进另一个盒子。
        bool isTargetFolder = false;
        for (const auto& t : targets) {
            if (PathSame(t.folder, path)) { isTargetFolder = true; break; }
        }
        if (isTargetFolder) continue;

        for (const auto& rule : rules) {
            if (!RuleMatches(rule, e.name, e.isDir)) continue;
            auto t = std::find_if(targets.begin(), targets.end(),
                                  [&](const OrganizeTarget& c) { return PathSame(c.boxName, rule.boxName); });
            if (t == targets.end() || t->folder.empty()) break;  // 目标盒子还没建，跳过
            plan.push_back(OrganizeItem{path, rule.name, rule.boxName, t->folder});
            break;  // 第一条命中的规则生效，后面的规则不再叠加
        }
    }
    return plan;
}

bool ApplyOrganize(const std::vector<OrganizeItem>& plan, HWND owner, bool captureDesktopPositions,
                   size_t& movedCount, std::wstring& err) {
    movedCount = 0;
    err.clear();
    if (plan.empty()) return true;

    std::vector<std::wstring> sources;
    for (const auto& p : plan) sources.push_back(p.path);

    std::vector<IconPosition> positionsBefore;
    if (captureDesktopPositions) positionsBefore = IconViewCaptureInDir(sources, DesktopPath());

    // 按目标目录分组，一个目录一次 IFileOperation，减少 Shell 往返。
    std::map<std::wstring, std::vector<std::wstring>> groups;
    for (const auto& p : plan) groups[p.destFolder].push_back(p.path);

    std::vector<MovedEntry> allMoves;
    bool anyFailure = false;
    for (auto& g : groups) {
        std::vector<MovedEntry> moves;
        std::wstring moveErr;
        if (!MoveFilesTo(g.second, g.first, owner, moves, moveErr)) {
            err = moveErr;
            anyFailure = true;
            continue;
        }
        allMoves.insert(allMoves.end(), moves.begin(), moves.end());
    }

    movedCount = allMoves.size();
    if (!allMoves.empty()) {
        std::wstring label = L"自动整理 " + std::to_wstring(movedCount) + L" 项";
        UndoHistoryAdd(std::move(allMoves), label, std::move(positionsBefore));
    }
    return !anyFailure;
}

static std::wstring LowerPath(const std::wstring& s) {
    std::wstring o = s;
    for (auto& c : o) c = static_cast<wchar_t>(towlower(c));
    return o;
}

bool OrganizeBaselines::Has(const std::wstring& ruleName) const { return byRule.count(ruleName) != 0; }

bool OrganizeBaselines::Contains(const std::wstring& ruleName, const std::wstring& path) const {
    auto it = byRule.find(ruleName);
    if (it == byRule.end()) return false;
    return it->second.count(LowerPath(path)) != 0;
}

void OrganizeBaselines::Add(const std::wstring& ruleName, const std::vector<std::wstring>& paths) {
    std::set<std::wstring>& bucket = byRule[ruleName];
    for (const auto& p : paths) bucket.insert(LowerPath(p));
}

void OrganizeBaselines::Prune(const std::vector<RuleConfig>& rules) {
    for (auto it = byRule.begin(); it != byRule.end();) {
        bool kept = std::any_of(rules.begin(), rules.end(),
                                [&](const RuleConfig& r) { return r.name == it->first; });
        if (kept) ++it;
        else it = byRule.erase(it);
    }
}

static std::wstring BaselinePath() { return AppFile(L"organize-baseline.txt"); }

bool OrganizeBaselines::Load(OrganizeBaselines& out, std::wstring& err) {
    out.byRule.clear();
    err.clear();
    std::string raw;
    if (!ReadFileBytes(BaselinePath(), raw)) return true;  // 没有基线文件是正常的

    std::wstring text = Utf8ToWide(raw.c_str());
    if (!text.empty() && text.front() == L'\uFEFF') text.erase(0, 1);
    size_t start = 0;
    std::wstring current;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = nl == std::wstring::npos ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        start = nl == std::wstring::npos ? text.size() + 1 : nl + 1;

        size_t b = line.find_first_not_of(L" \t");
        if (b == std::wstring::npos) continue;
        line = line.substr(b);
        if (line.front() == L'#') continue;
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            std::wstring sec = line.substr(1, line.size() - 2);
            const std::wstring prefix = L"rule:";
            if (sec.compare(0, prefix.size(), prefix) == 0) {
                current = sec.substr(prefix.size());
                out.byRule[current];
            } else {
                current.clear();
            }
            continue;
        }
        if (!current.empty()) out.byRule[current].insert(LowerPath(line));
    }
    return true;
}

bool OrganizeBaselines::Save(std::wstring& err) const {
    std::wstring text = L"# DeskOrganization 自动整理基线，程序维护，不用手改。\n";
    text += L"# 记下每条 sweep = 0 的规则建立时桌面上已有哪些命中文件，那些不会被自动搬走。\n";
    text += L"# 删掉本文件只会让 sweep = 0 的规则重建基线，不会因此去清扫现存文件。\n";
    for (const auto& kv : byRule) {
        text += L"[rule:" + kv.first + L"]\n";
        for (const auto& p : kv.second) text += p + L"\n";
    }
    if (!WriteFileBytes(BaselinePath(), WideToUtf8(text))) {
        err = L"基线文件写入失败";
        return false;
    }
    return true;
}

std::vector<OrganizeItem> FilterByBaselines(const std::vector<OrganizeItem>& plan, const std::vector<RuleConfig>& rules,
                                            const OrganizeBaselines& baselines) {
    std::vector<OrganizeItem> kept;
    for (const auto& item : plan) {
        auto r = std::find_if(rules.begin(), rules.end(),
                              [&](const RuleConfig& c) { return c.name == item.ruleName; });
        bool sweepAll = r == rules.end() || r->sweepExisting;
        if (sweepAll || !baselines.Contains(item.ruleName, item.path)) kept.push_back(item);
    }
    return kept;
}

void OrganizeBaselines::DropMissing() {
    // 基线里的路径已经不存在、或已经不在原地（比如已经被规则搬进盒子）的，都要丢掉：
    // 否则用户删掉再新建同名文件时，那条路径会被当成"规则建立前就有"而永远不被整理。
    for (auto& kv : byRule) {
        std::set<std::wstring> kept;
        for (const auto& p : kv.second) {
            if (PathExists(p)) kept.insert(p);
        }
        kv.second = std::move(kept);
    }
}

// ---- 撤销保护期 ----
// 键一律小写：Windows 路径大小写不敏感，撤销记录里的 from 是原样大小写，
// 不归一就会出现"看着一样却判成没命中"的错。
static std::map<std::wstring, ULONGLONG> g_holds;

static std::wstring HoldKey(const std::wstring& path) {
    std::wstring o = path;
    for (auto& c : o) c = static_cast<wchar_t>(towlower(c));
    return o;
}

static void PruneHolds() {
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_holds.begin(); it != g_holds.end();) {
        if (it->second <= now) it = g_holds.erase(it);
        else ++it;
    }
}

void OrganizeHold(const std::vector<std::wstring>& paths, DWORD ms) {
    if (ms == 0) return;
    const ULONGLONG until = GetTickCount64() + ms;
    for (const auto& p : paths) {
        if (p.empty()) continue;
        g_holds[HoldKey(p)] = until;
    }
}

bool OrganizeHeld(const std::wstring& path) {
    PruneHolds();
    return g_holds.count(HoldKey(path)) != 0;
}

size_t OrganizeHoldCount() {
    PruneHolds();
    return g_holds.size();
}

std::vector<OrganizeItem> FilterHolds(const std::vector<OrganizeItem>& plan, size_t& dropped) {
    dropped = 0;
    std::vector<OrganizeItem> kept;
    for (const auto& item : plan) {
        if (OrganizeHeld(item.path)) {
            dropped++;
            continue;
        }
        kept.push_back(item);
    }
    return kept;
}
