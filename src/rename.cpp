#include "rename.h"

#include <algorithm>

#include "fileop.h"

static bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

size_t FindBoxIndex(const Layout& layout, const std::wstring& name) {
    for (size_t i = 0; i < layout.boxes.size(); ++i) {
        if (SameName(layout.boxes[i].name, name)) return i;
    }
    return layout.boxes.size();
}

static std::wstring JoinCsv(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& p : parts) {
        if (!out.empty()) out += L",";
        out += p;
    }
    return out;
}

// 把列表里等于 oldName 的那一项换成 newName，返回是否改过。
static bool ReplaceInList(std::vector<std::wstring>& list, const std::wstring& oldName,
                          const std::wstring& newName) {
    bool changed = false;
    for (auto& item : list) {
        if (SameName(item, oldName)) {
            item = newName;
            changed = true;
        }
    }
    return changed;
}

bool PlanBoxRename(Layout& layout, const std::wstring& oldName, const std::wstring& newName,
                   const std::map<std::wstring, std::wstring>& resolvedFolders,
                   const std::wstring& newFolderForDerived, std::vector<FolderRename>& folderRenames,
                   std::wstring& err) {
    folderRenames.clear();
    err.clear();

    const size_t idx = FindBoxIndex(layout, oldName);
    if (idx >= layout.boxes.size()) {
        err = L"找不到要改名的盒子";
        return false;
    }

    const std::wstring clean = SanitizeBoxName(newName);
    if (clean.empty() || newName.find_first_not_of(L" \t") == std::wstring::npos) {
        err = L"名字不能为空";
        return false;
    }

    const BoxConfig& cur = layout.boxes[idx];
    const std::wstring before = cur.name;
    if (SameName(before, clean)) return true;  // 名字没变，当成无事发生

    if (FindBoxIndex(layout, clean) < layout.boxes.size()) {
        err = L"已经有叫「" + clean + L"」的盒子了";
        return false;
    }

    // 只有目录是从盒子名派生的时候才需要动磁盘。显式指定过 folder 的（映射盒子）
    // 改的只是显示名，目录不碰。
    const bool derived = cur.folder.empty();
    if (derived) {
        auto it = resolvedFolders.find(before);
        if (it == resolvedFolders.end() || it->second.empty()) {
            err = L"取不到盒子当前的目录，不敢改名";
            return false;
        }
        const std::wstring fromFolder = it->second;
        if (newFolderForDerived.empty()) {
            err = L"算不出改名后的目录";
            return false;
        }
        // 目标目录已存在就意味着要么覆盖要么把两个目录合起来，两者都会碰用户文件。
        if (PathExists(newFolderForDerived)) {
            err = L"已经有同名目录了，先把它移走或换个名字";
            return false;
        }
        // 另一个盒子共用同一目录时，改名会把人家的目录也搬走。
        for (size_t i = 0; i < layout.boxes.size(); ++i) {
            if (i == idx) continue;
            auto other = resolvedFolders.find(layout.boxes[i].name);
            if (other == resolvedFolders.end()) continue;
            if (_wcsicmp(other->second.c_str(), fromFolder.c_str()) == 0) {
                err = L"盒子「" + layout.boxes[i].name + L"」用的是同一个目录，改名会把它也带走";
                return false;
            }
        }
        folderRenames.push_back(FolderRename{fromFolder, newFolderForDerived});
    }

    // 到这里才动手改 layout。
    BoxConfig& mut = layout.boxes[idx];
    mut.name = clean;

    // 指向旧名字的规则、多标签成员、当前选中的标签卡，全都要跟着改，
    // 否则规则会凭空造一个新盒子，多标签会少一张卡。
    for (auto& rule : layout.rules) {
        if (!rule.boxName.empty() && SameName(rule.boxName, before)) rule.boxName = clean;
    }
    for (auto& box : layout.boxes) {
        if (!box.members.empty()) {
            auto members = SplitCommaList(box.members);
            if (ReplaceInList(members, before, clean)) box.members = JoinCsv(members);
        }
        if (!box.tab.empty() && SameName(box.tab, before)) {
            // 派生目录的盒子改名后，标签卡引用也跟着换；显式目录的不受影响。
            box.tab = clean;
        }
    }
    if (!mut.tab.empty() && !mut.members.empty()) {
        auto m = SplitCommaList(mut.members);
        if (std::find(m.begin(), m.end(), mut.tab) == m.end()) mut.tab.clear();
    }
    return true;
}

bool ApplyFolderRenames(const std::vector<FolderRename>& renames, HWND owner, std::wstring& err) {
    err.clear();
    if (renames.empty()) return true;

    std::vector<std::wstring> sources;
    for (const auto& r : renames) sources.push_back(r.from);

    // 改名会让 Shell 重新安置桌面上那个图标，所以先记下位置，撤销时放回去。
    std::vector<IconPosition> positions = IconViewCaptureInDir(sources, DesktopPath());

    std::vector<MovedEntry> moves;
    for (const auto& r : renames) {
        MovedEntry got;
        std::wstring renameErr;
        if (!RenamePath(r.from, FileNameOf(r.to), owner, got, renameErr)) {
            err = renameErr;
            break;
        }
        moves.push_back(std::move(got));
    }

    // 中途失败时，已经改掉的部分照样登记撤销，用户能一步步退回去。
    if (!moves.empty()) {
        UndoHistoryAdd(std::move(moves), L"盒子改名", std::move(positions));
    }
    return err.empty();
}
