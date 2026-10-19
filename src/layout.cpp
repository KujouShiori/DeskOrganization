#include "layout.h"

#include <algorithm>

// layout.ini 由我们自己按 UTF-8 读写，不用 WritePrivateProfileStringW：
// 那个 API 建新文件时跟随系统 ANSI 代码页，中文盒名在非中文区域设置的机器上会变成 ?。
// 同时保留逐行原样写回的能力，这样用户加的注释和未知键不会被吃掉。

static const wchar_t* kSectionPrefix = L"box:";
static const wchar_t* kRulePrefix = L"rule:";

static bool HasPrefix(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::wstring TrimPrefix(const std::wstring& s, const wchar_t* prefix) {
    return s.substr(wcslen(prefix));
}

static std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::wstring IniPath() { return AppFile(L"layout.ini"); }

static std::wstring SectionOf(const std::wstring& line) {
    std::wstring t = Trim(line);
    if (t.size() < 2 || t.front() != L'[' || t.back() != L']') return std::wstring();
    return t.substr(1, t.size() - 2);
}

static bool IsSkippable(const std::wstring& line) {
    std::wstring t = Trim(line);
    return t.empty() || t.front() == L';' || t.front() == L'#';
}

struct IniDoc {
    std::vector<std::wstring> lines;
};

static bool DocLoad(const std::wstring& path, IniDoc& doc) {
    doc.lines.clear();
    std::string raw;
    if (!ReadFileBytes(path, raw)) return false;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        raw.erase(0, 3);
    }
    std::wstring all = Utf8ToWide(raw.c_str());

    size_t start = 0;
    while (start <= all.size()) {
        size_t nl = all.find(L'\n', start);
        std::wstring line = nl == std::wstring::npos ? all.substr(start) : all.substr(start, nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (nl == std::wstring::npos) {
            if (!line.empty()) doc.lines.push_back(line);
            break;
        }
        doc.lines.push_back(line);
        start = nl + 1;
        if (start == all.size()) break;
    }
    return true;
}

static bool DocSave(const std::wstring& path, const IniDoc& doc) {
    std::wstring out;
    for (const auto& l : doc.lines) {
        out += l;
        out += L"\r\n";
    }
    return WriteFileBytes(path, WideToUtf8(out));
}

static size_t FindHeader(const IniDoc& doc, const std::wstring& section) {
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        std::wstring t = Trim(doc.lines[i]);
        if (t.size() >= 2 && t.front() == L'[' && t.back() == L']' && t.substr(1, t.size() - 2) == section) return i;
    }
    return std::wstring::npos;
}

static size_t SectionEnd(const IniDoc& doc, size_t headerIndex) {
    for (size_t i = headerIndex + 1; i < doc.lines.size(); ++i) {
        if (!SectionOf(doc.lines[i]).empty()) return i;
    }
    return doc.lines.size();
}

static std::wstring GetRaw(const IniDoc& doc, size_t from, size_t to, const std::wstring& key) {
    for (size_t i = from; i < to; ++i) {
        const std::wstring& line = doc.lines[i];
        if (IsSkippable(line) || !SectionOf(line).empty()) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (Trim(line.substr(0, eq)) == key) return Trim(line.substr(eq + 1));
    }
    return std::wstring();
}

static void SetKey(IniDoc& doc, const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    size_t hdr = FindHeader(doc, section);
    if (hdr == std::wstring::npos) {
        if (!doc.lines.empty() && !Trim(doc.lines.back()).empty()) doc.lines.push_back(std::wstring());
        doc.lines.push_back(L"[" + section + L"]");
        doc.lines.push_back(key + L" = " + value);
        return;
    }
    size_t end = SectionEnd(doc, hdr);
    for (size_t i = hdr + 1; i < end; ++i) {
        const std::wstring& line = doc.lines[i];
        if (IsSkippable(line)) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (Trim(line.substr(0, eq)) == key) {
            doc.lines[i] = key + L" = " + value;
            return;
        }
    }
    // 追加到本段末尾而不是紧跟段名，这样写出来的键顺序就是我们调用 SetKey 的顺序。
    size_t insertAt = end;
    while (insertAt > hdr + 1 && Trim(doc.lines[insertAt - 1]).empty()) --insertAt;
    doc.lines.insert(doc.lines.begin() + static_cast<ptrdiff_t>(insertAt), key + L" = " + value);
}

static int GetInt(const IniDoc& doc, size_t from, size_t to, const std::wstring& key, int def) {
    std::wstring v = GetRaw(doc, from, to, key);
    if (v.empty()) return def;
    return _wtoi(v.c_str());
}

std::wstring SanitizeBoxName(std::wstring name) {
    for (wchar_t bad : {L'[', L']', L'=', L';', L'"', L'/', L'#'}) {
        name.erase(std::remove(name.begin(), name.end(), bad), name.end());
    }
    while (!name.empty() && name.front() == L' ') name.erase(name.begin());
    while (!name.empty() && name.back() == L' ') name.pop_back();
    if (name.empty()) name = L"盒子";
    if (name.size() > 60) name.resize(60);
    return name;
}

bool LayoutLoadFrom(Layout& out, const std::wstring& path) {
    out.boxes.clear();
    out.rules.clear();
    out.hideIcons = false;
    IniDoc doc;
    if (!DocLoad(path, doc)) {
        Log(L"layout.ini 不存在，用默认布局");
        return false;
    }

    bool hadGeneral = false;
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        std::wstring section = SectionOf(doc.lines[i]);
        if (section.empty()) continue;
        if (section == L"general") {
            hadGeneral = true;
            size_t ge = SectionEnd(doc, i);
            out.hideIcons = GetInt(doc, i, ge, L"hide_icons", 0) != 0;
            out.boxRoot = GetRaw(doc, i, ge, L"box_root");
            out.theme = GetRaw(doc, i, ge, L"theme");
            if (out.theme.empty()) out.theme = L"auto";
        }
    }

    for (size_t i = 0; i < doc.lines.size(); ++i) {
        std::wstring section = SectionOf(doc.lines[i]);
        if (section.empty() || !HasPrefix(section, kSectionPrefix)) continue;
        size_t end = SectionEnd(doc, i);

        BoxConfig c;
        c.name = TrimPrefix(section, kSectionPrefix);
        if (c.name.empty()) continue;

        int left = GetInt(doc, i, end, L"left", 40);
        int top = GetInt(doc, i, end, L"top", 40);
        int width = GetInt(doc, i, end, L"width", 320);
        int height = GetInt(doc, i, end, L"height", 240);
        c.rect = RECT{left, top, left + width, top + height};
        c.collapsed = GetInt(doc, i, end, L"collapsed", 0) != 0;
        c.folder = GetRaw(doc, i, end, L"folder");
        c.grouped = GetInt(doc, i, end, L"grouped", 0) != 0;
        c.tab = GetRaw(doc, i, end, L"tab");
        c.members = GetRaw(doc, i, end, L"members");

        out.boxes.push_back(std::move(c));
        i = end - 1;
    }

    for (size_t i = 0; i < doc.lines.size(); ++i) {
        std::wstring section = SectionOf(doc.lines[i]);
        if (section.empty() || !HasPrefix(section, kRulePrefix)) continue;
        size_t end = SectionEnd(doc, i);

        RuleConfig r;
        r.name = TrimPrefix(section, kRulePrefix);
        if (r.name.empty()) continue;
        r.boxName = GetRaw(doc, i, end, L"box");
        r.ext = GetRaw(doc, i, end, L"ext");
        r.prefix = GetRaw(doc, i, end, L"prefix");
        r.contains = GetRaw(doc, i, end, L"contains");
        r.includeFolders = GetInt(doc, i, end, L"folders", 0) != 0;
        r.sweepExisting = GetInt(doc, i, end, L"sweep", 1) != 0;

        out.rules.push_back(std::move(r));
        i = end - 1;
    }

    Log(L"layout.ini 读到 %zu 个盒子、%zu 条规则", out.boxes.size(), out.rules.size());
    // 只要文件里解析出东西就算加载成功：只有规则、还没建盒子的配置不能被默认布局覆盖掉。
    return !out.boxes.empty() || !out.rules.empty() || hadGeneral;
}

// 把名字列表变成不重复的段名：同名的加后缀，
// 否则后写的会静默覆盖前面的。
static std::vector<std::wstring> UniqueSections(const wchar_t* prefix, const std::vector<std::wstring>& names) {
    std::vector<std::wstring> sections;
    for (const auto& raw : names) {
        std::wstring base = std::wstring(prefix) + SanitizeBoxName(raw);
        std::wstring section = base;
        int suffix = 2;
        while (std::find(sections.begin(), sections.end(), section) != sections.end()) {
            section = base + L"-" + std::to_wstring(suffix++);
        }
        sections.push_back(section);
    }
    return sections;
}

bool LayoutSaveTo(const Layout& in, const std::wstring& path) {
    IniDoc doc;
    bool existed = DocLoad(path, doc);
    if (!existed) {
        doc.lines.clear();
        doc.lines.push_back(L"; 桌面整理 DeskOrganization 布局文件，UTF-8 编码，可以直接手改。");
        doc.lines.push_back(L"; left/top 是相对桌面左上角的像素，width/height 是展开时的尺寸。");
        doc.lines.push_back(L"; collapsed = 1 表示这个盒子默认收成一条标题栏。");
        doc.lines.push_back(L"; [general] box_root 是盒子目录的根，留空就是桌面（%USERPROFILE%\\Desktop）。");
        doc.lines.push_back(L"; 每个盒子的 folder 写成绝对路径就原样用，写成相对路径则相对 box_root 解析，");
        doc.lines.push_back(L"; folder 留空时用盒子的段名当目录名。改完保存后重启程序生效。");
        doc.lines.push_back(L"; [general] theme = auto 跟随系统深浅色，也可以写 light 或 dark 固定。");
        doc.lines.push_back(L"; grouped = 1 让盒子按文件类型分标签卡显示（只是分组显示，不会移动任何文件）。");
        doc.lines.push_back(L"; tab 是当前选中的分类名，留空表示「全部」。可选值见盒子内的标签卡。");
        doc.lines.push_back(L"; members = 逗号分隔的成员盒子名，把这个盒子变成多标签盒子：每个标签卡显示一个成员盒子的目录。");
        doc.lines.push_back(L"; 成员盒子会被自动隐藏，删掉 members 那一行就各自恢复显示。");
        doc.lines.push_back(L"; [rule:名字] 是自动整理规则：box = 命中后归入的盒子（没有就建），");
        doc.lines.push_back(L"; ext = 逗号分隔的后缀如 .png,.jpg，prefix = 文件名前缀，contains = 文件名包含，");
        doc.lines.push_back(L"; folders = 1 才连文件夹一起归。这三条至少写一个，全空的规则不生效。");
        doc.lines.push_back(L"; sweep = 1（默认）连桌面现存文件一起清扫；sweep = 0 只处理这条规则建立之后新出现的文件。");
    }

    std::vector<std::wstring> boxNames;
    for (const auto& b : in.boxes) boxNames.push_back(b.name);
    std::vector<std::wstring> sections = UniqueSections(kSectionPrefix, boxNames);

    std::vector<std::wstring> ruleNames;
    for (const auto& r : in.rules) ruleNames.push_back(r.name);
    std::vector<std::wstring> ruleSections = UniqueSections(kRulePrefix, ruleNames);

    // 删掉已经不再存在的盒子段和规则段。
    for (size_t i = 0; i < doc.lines.size();) {
        std::wstring section = SectionOf(doc.lines[i]);
        bool isBox = !section.empty() && HasPrefix(section, kSectionPrefix);
        bool isRule = !section.empty() && HasPrefix(section, kRulePrefix);
        if (!isBox && !isRule) {
            ++i;
            continue;
        }
        size_t end = SectionEnd(doc, i);
        const std::vector<std::wstring>& keep = isBox ? sections : ruleSections;
        if (std::find(keep.begin(), keep.end(), section) == keep.end()) {
            doc.lines.erase(doc.lines.begin() + static_cast<ptrdiff_t>(i),
                            doc.lines.begin() + static_cast<ptrdiff_t>(end));
            continue;
        }
        i = end;
    }

    SetKey(doc, L"general", L"hide_icons", in.hideIcons ? L"1" : L"0");
    SetKey(doc, L"general", L"box_root", in.boxRoot);
    SetKey(doc, L"general", L"theme", in.theme.empty() ? L"auto" : in.theme);

    for (size_t idx = 0; idx < in.rules.size(); ++idx) {
        const RuleConfig& r = in.rules[idx];
        const std::wstring& sec = ruleSections[idx];
        SetKey(doc, sec, L"box", r.boxName);
        SetKey(doc, sec, L"ext", r.ext);
        SetKey(doc, sec, L"prefix", r.prefix);
        SetKey(doc, sec, L"contains", r.contains);
        SetKey(doc, sec, L"folders", r.includeFolders ? L"1" : L"0");
        SetKey(doc, sec, L"sweep", r.sweepExisting ? L"1" : L"0");
    }

    for (size_t idx = 0; idx < in.boxes.size(); ++idx) {
        const BoxConfig& b = in.boxes[idx];
        const std::wstring& sec = sections[idx];
        wchar_t num[32];
        int w = b.rect.right - b.rect.left;
        int h = b.rect.bottom - b.rect.top;

        _snwprintf_s(num, _countof(num), _TRUNCATE, L"%d", static_cast<int>(b.rect.left));
        SetKey(doc, sec, L"left", num);
        _snwprintf_s(num, _countof(num), _TRUNCATE, L"%d", static_cast<int>(b.rect.top));
        SetKey(doc, sec, L"top", num);
        _snwprintf_s(num, _countof(num), _TRUNCATE, L"%d", w);
        SetKey(doc, sec, L"width", num);
        _snwprintf_s(num, _countof(num), _TRUNCATE, L"%d", h);
        SetKey(doc, sec, L"height", num);
        SetKey(doc, sec, L"collapsed", b.collapsed ? L"1" : L"0");
        SetKey(doc, sec, L"folder", b.folder);
        SetKey(doc, sec, L"grouped", b.grouped ? L"1" : L"0");
        SetKey(doc, sec, L"tab", b.tab);
        SetKey(doc, sec, L"members", b.members);
    }

    bool ok = DocSave(path, doc);
    Log(L"布局已保存：%zu 个盒子 ok=%d", in.boxes.size(), ok ? 1 : 0);
    return ok;
}

bool LayoutLoad(Layout& out) { return LayoutLoadFrom(out, IniPath()); }
bool LayoutSave(const Layout& in) { return LayoutSaveTo(in, IniPath()); }

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

static std::vector<std::wstring> SplitList(const std::wstring& s) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t sep = s.find_first_of(L",;", start);
        std::wstring item = sep == std::wstring::npos ? s.substr(start) : s.substr(start, sep - start);
        item = Trim(item);
        if (!item.empty()) parts.push_back(item);
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return parts;
}

bool RuleMatches(const RuleConfig& rule, const std::wstring& itemName, bool isDirectory) {
    if (isDirectory && !rule.includeFolders) return false;

    std::wstring lower = ToLower(itemName);
    bool hasCriterion = false;

    if (!Trim(rule.ext).empty()) {
        hasCriterion = true;
        size_t dot = lower.find_last_of(L'.');
        if (dot == std::wstring::npos) return false;
        std::wstring ext = lower.substr(dot);
        bool hit = false;
        for (auto p : SplitList(rule.ext)) {
            p = ToLower(p);
            if (p.front() != L'.') p = L"." + p;
            if (ext == p) hit = true;
        }
        if (!hit) return false;
    }

    if (!Trim(rule.prefix).empty()) {
        hasCriterion = true;
        std::wstring want = ToLower(Trim(rule.prefix));
        if (lower.rfind(want, 0) != 0) return false;
    }

    if (!Trim(rule.contains).empty()) {
        hasCriterion = true;
        if (lower.find(ToLower(Trim(rule.contains))) == std::wstring::npos) return false;
    }

    // 三个条件都没写的规则不匹配任何东西。
    return hasCriterion;
}
