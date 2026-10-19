#include "merge.h"

#include <algorithm>

namespace {

std::vector<std::wstring> MembersOf(const BoxConfig& box) { return SplitCommaList(box.members); }

std::wstring JoinCsv(const std::vector<std::wstring>& parts) {
    std::wstring out;
    for (const auto& p : parts) {
        if (!out.empty()) out += L",";
        out += p;
    }
    return out;
}

bool SameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

size_t IndexOf(const Layout& layout, const std::wstring& name) {
    for (size_t i = 0; i < layout.boxes.size(); ++i) {
        if (SameName(layout.boxes[i].name, name)) return i;
    }
    return layout.boxes.size();
}

}  // namespace

bool IsMergeMember(const Layout& layout, const std::wstring& boxName, const std::wstring& memberName) {
    const size_t i = IndexOf(layout, boxName);
    if (i >= layout.boxes.size()) return false;
    const auto members = MembersOf(layout.boxes[i]);
    return std::find_if(members.begin(), members.end(),
                        [&](const std::wstring& m) { return SameName(m, memberName); }) != members.end();
}

bool IsAggregate(const Layout& layout, const std::wstring& boxName) {
    const size_t i = IndexOf(layout, boxName);
    return i < layout.boxes.size() && !layout.boxes[i].members.empty();
}

bool ToggleMergeMember(Layout& layout, const std::wstring& boxName, const std::wstring& memberName,
                       std::wstring& err) {
    err.clear();

    const size_t boxIdx = IndexOf(layout, boxName);
    const size_t memberIdx = IndexOf(layout, memberName);
    if (boxIdx >= layout.boxes.size()) {
        err = L"找不到盒子";
        return false;
    }
    if (memberIdx >= layout.boxes.size()) {
        err = L"找不到要合并的盒子";
        return false;
    }
    if (boxIdx == memberIdx) {
        err = L"盒子不能和自己合并";
        return false;
    }

    BoxConfig& box = layout.boxes[boxIdx];
    BoxConfig& candidate = layout.boxes[memberIdx];
    const bool wasMember = IsMergeMember(layout, boxName, candidate.name);

    if (!wasMember) {
        // 标签盒里套标签盒，界面上会出现"点一层再点一层"的嵌套，成员关系也容易看漏，
        // 所以只允许普通盒子进标签列表。想合并就先把它拆开。
        // 这条同时也排除了互为成员：要成环，成员必须先自己是标签盒，而这里已经拦住了。
        if (!candidate.members.empty()) {
            err = L"「" + candidate.name + L"」自己就是标签盒，先把它拆开再合并";
            return false;
        }
    }

    auto members = MembersOf(box);
    if (wasMember) {
        members.erase(std::remove_if(members.begin(), members.end(),
                                     [&](const std::wstring& m) { return SameName(m, candidate.name); }),
                      members.end());
    } else {
        members.push_back(candidate.name);
    }
    box.members = JoinCsv(members);

    // 成员表和分类标签卡是两套互斥的分组方式。
    if (!box.members.empty()) box.grouped = false;

    // 当前选中的标签卡如果被移出去了，就回到第一张，避免指向一个已经不存在的成员。
    if (!box.tab.empty() && !box.members.empty() && !IsMergeMember(layout, box.name, box.tab)) box.tab.clear();
    if (box.members.empty()) box.tab.clear();

    return true;
}
