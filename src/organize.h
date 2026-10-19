#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "layout.h"

struct OrganizeTarget {
    std::wstring boxName;
    std::wstring folder;  // 已解析的绝对路径
};

struct OrganizeItem {
    std::wstring path;
    std::wstring ruleName;
    std::wstring boxName;
    std::wstring destFolder;
};

// sweep = 0 的规则需要知道"这条规则建立时桌面上已经有哪些命中文件"，
// 那些就不该被自动搬走，只处理之后新出现的。基线单独存文件：它是程序记账，不是用户配置。
class OrganizeBaselines {
 public:
    bool Has(const std::wstring& ruleName) const;
    bool Contains(const std::wstring& ruleName, const std::wstring& path) const;
    void Add(const std::wstring& ruleName, const std::vector<std::wstring>& paths);
    void Prune(const std::vector<RuleConfig>& rules);
    // 丢掉已经不在原地的路径：用户删掉再新建同名文件时，那应当算"新出现的文件"。
    void DropMissing();
    size_t ruleCount() const { return byRule.size(); }

    static bool Load(OrganizeBaselines& out, std::wstring& err);
    bool Save(std::wstring& err) const;

 private:
    std::map<std::wstring, std::set<std::wstring>> byRule;  // 路径一律小写存
};

// 纯函数：扫一遍 sourceDir，按规则算出该把哪些文件搬去哪个盒子目录。
// 做成可注入目录的形式，是为了让自检能在临时目录里跑完整流程，不碰用户真实桌面。
std::vector<OrganizeItem> PlanOrganize(const std::wstring& sourceDir, const std::vector<RuleConfig>& rules,
                                        const std::vector<OrganizeTarget>& targets);

// 按基线过滤掉 sweep = 0 规则「建立时已存在」的那些项。
std::vector<OrganizeItem> FilterByBaselines(const std::vector<OrganizeItem>& plan,
                                            const std::vector<RuleConfig>& rules, const OrganizeBaselines& baselines);

// 执行计划：每个目标目录一次 IFileOperation，整轮合并成一条撤销记录。
// captureDesktopPositions 为真时，会先记下这些图标在桌面上的格子，撤销时放回去。
bool ApplyOrganize(const std::vector<OrganizeItem>& plan, HWND owner, bool captureDesktopPositions,
                   size_t& movedCount, std::wstring& err);

// 撤销保护期。刚被撤销放回原处的文件，如果马上又被规则命中，就等于"撤销没生效"
// （实测撤销 3 秒后文件就被下一轮自动整理又搬走了），所以在一段时间内跳过它们。
// 手动"整理现存全部文件"不受此限制：那是用户显式要求的。
void OrganizeHold(const std::vector<std::wstring>& paths, DWORD ms);
bool OrganizeHeld(const std::wstring& path);
size_t OrganizeHoldCount();
std::vector<OrganizeItem> FilterHolds(const std::vector<OrganizeItem>& plan, size_t& dropped);
