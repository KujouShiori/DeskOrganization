#pragma once

#include <string>
#include <vector>

#include "util.h"

struct BoxConfig {
    std::wstring name;
    RECT rect{};              // 相对桌面原点的物理像素
    bool collapsed = false;
    std::wstring folder;      // 相对 box_root 或绝对路径，留空则用段名
    // 盒内二级分类：只是显示分组，不移动文件。tab 空表示"全部"。
    bool grouped = false;
    std::wstring tab;
    // 多标签合并：逗号分隔的成员盒子名。非空时这个盒子只显示成员标签卡，
    // 成员盒子自己被隐藏（配置仍在，拆掉合并后自动恢复显示）。
    std::wstring members;
};

// 自动整理规则。ext / prefix / contains 至少要写一个：
// 三者全空的规则不匹配任何东西，免得一条没填完整的规则把整个桌面扫进盒子。
struct RuleConfig {
    std::wstring name;
    std::wstring boxName;         // 命中后归入的盒子，不存在就按 box_root 建
    std::wstring ext;             // 逗号或分号分隔，例如 .png,.jpg
    std::wstring prefix;          // 文件名前缀
    std::wstring contains;        // 文件名包含
    bool includeFolders = false;  // 默认只动文件，不动文件夹
    // 首次范围开关：1 = 连桌面现存的文件一起清扫；0 = 只处理这条规则建立之后新出现的文件。
    bool sweepExisting = true;
};

struct Layout {
    std::vector<BoxConfig> boxes;
    std::vector<RuleConfig> rules;
    bool hideIcons = false;
    // 空表示用桌面。盒子里的 folder 若写成相对路径，就相对这个根解析。
    std::wstring boxRoot;
    // auto = 跟随系统深浅色；也可以显式固定，方便截图对比和自检。
    std::wstring theme = L"auto";
};

bool LayoutLoad(Layout& out);
bool LayoutSave(const Layout& in);

// 自检用：读写指定文件，避免踩到用户真实的 layout.ini。
bool LayoutLoadFrom(Layout& out, const std::wstring& path);
bool LayoutSaveTo(const Layout& in, const std::wstring& path);

// 段名会被写进 INI，方括号和等号会破坏文件结构。
std::wstring SanitizeBoxName(std::wstring name);

// 规则匹配：大小写不敏感。ext / prefix / contains 全空的规则不匹配任何东西。
bool RuleMatches(const RuleConfig& rule, const std::wstring& itemName, bool isDirectory);
