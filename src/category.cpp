#include "category.h"

#include <map>

namespace {

const wchar_t* kFolder = L"文件夹";
const wchar_t* kOther = L"其他";

struct Table {
    const wchar_t* category;
    std::vector<std::wstring> exts;
};

const std::vector<Table>& Table_() {
    static const std::vector<Table> t = {
        {L"图片", {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"ico", L"tif", L"tiff", L"heic", L"svg"}},
        {L"文档", {L"txt", L"md", L"doc", L"docx", L"pdf", L"xls", L"xlsx", L"ppt", L"pptx", L"odt", L"ods",
                   L"odp", L"rtf", L"csv", L"log", L"ini"}},
        {L"程序", {L"exe", L"msi", L"lnk", L"bat", L"cmd", L"ps1", L"appx", L"apk", L"jar", L"msix"}},
        {L"压缩包", {L"zip", L"rar", L"7z", L"tar", L"gz", L"xz", L"bz2", L"tgz"}},
        {L"媒体", {L"mp3", L"wav", L"flac", L"aac", L"ogg", L"m4a", L"mp4", L"mkv", L"avi", L"mov", L"wmv",
                   L"flv"}},
    };
    return t;
}

const std::map<std::wstring, std::wstring>& ExtMap() {
    static const std::map<std::wstring, std::wstring> m = [] {
        std::map<std::wstring, std::wstring> out;
        for (const auto& row : Table_()) {
            for (const auto& e : row.exts) out[e] = row.category;
        }
        return out;
    }();
    return m;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// 取最后一个点之后的部分；没有点、点在开头（隐藏文件风格如 .gitignore）、或点在最后都算无后缀。
std::wstring ExtOf(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return std::wstring();
    return Lower(name.substr(dot + 1));
}

}  // namespace

namespace category {

const std::vector<std::wstring>& Names() {
    static const std::vector<std::wstring> n = [] {
        std::vector<std::wstring> out;
        for (const auto& row : Table_()) out.push_back(row.category);
        out.push_back(kFolder);
        out.push_back(kOther);
        return out;
    }();
    return n;
}

bool IsKnown(const std::wstring& name) {
    const auto& n = Names();
    for (const auto& c : n) {
        if (c == name) return true;
    }
    return false;
}

std::wstring CategoryOf(const std::wstring& itemName, bool isDirectory) {
    if (isDirectory) return kFolder;
    auto it = ExtMap().find(ExtOf(itemName));
    if (it != ExtMap().end()) return it->second;
    return kOther;  // 兜底：保证没有文件会落进分类的缝隙里
}

}  // namespace category
