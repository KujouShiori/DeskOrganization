#include "shellmenu.h"

namespace {

// 只用普通字符串常量拼，避免把反斜杠写进字面量时被转义规则绊倒。
const wchar_t* kVerbKey = L"Software\\Classes\\DesktopBackground\\Shell\\DeskOrganization.Undo";
const wchar_t* kMenuText = L"撤销上一次桌面整理";

bool WriteString(HKEY root, const std::wstring& sub, const wchar_t* value, const std::wstring& data,
                 std::wstring& err) {
    HKEY key = nullptr;
    LONG r = RegCreateKeyExW(root, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key,
                             nullptr);
    if (r != ERROR_SUCCESS) {
        err = L"创建注册表项失败 err=" + std::to_wstring(r);
        return false;
    }
    r = RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                       static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) {
        err = L"写注册表值失败 err=" + std::to_wstring(r);
        return false;
    }
    return true;
}

bool ReadString(HKEY root, const std::wstring& sub, const wchar_t* value, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buf[2048] = {};
    DWORD size = sizeof buf, type = 0;
    LONG r = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    buf[_countof(buf) - 1] = 0;
    out = buf;
    return true;
}

}  // namespace

namespace shellmenu {

std::wstring ExePath() {
    wchar_t buf[4096] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, _countof(buf) - 1);
    if (n == 0 || n >= _countof(buf) - 1) return std::wstring();
    return buf;
}

std::wstring UndoCommandLine() {
    std::wstring exe = ExePath();
    if (exe.empty()) return std::wstring();
    // 路径里可能有空格，必须整体加引号。
    return L"\"" + exe + L"\" --undo";
}

std::wstring VerbKeyPath() { return kVerbKey; }

bool IsInstalled(std::wstring& installedCmd) {
    installedCmd.clear();
    std::wstring cmd;
    if (!ReadString(HKEY_CURRENT_USER, std::wstring(kVerbKey) + L"\\command", nullptr, cmd)) return false;
    installedCmd = cmd;
    return !cmd.empty();
}

bool Install(std::wstring& err) {
    err.clear();
    std::wstring cmd = UndoCommandLine();
    if (cmd.empty()) {
        err = L"取不到程序路径";
        return false;
    }
    if (!WriteString(HKEY_CURRENT_USER, kVerbKey, nullptr, kMenuText, err)) return false;
    // Position = Top 让它出现在菜单靠上处，免得被一长串"查看/刷新"埋掉。
    if (!WriteString(HKEY_CURRENT_USER, kVerbKey, L"Position", L"Top", err)) return false;
    if (!WriteString(HKEY_CURRENT_USER, std::wstring(kVerbKey) + L"\\command", nullptr, cmd, err)) return false;

    // Explorer 缓存了桌面动词表，不通知的话新菜单项要等它重启才出现。
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Log(L"桌面右键菜单撤销入口已安装：%s", cmd.c_str());
    return true;
}

bool Uninstall(std::wstring& err) {
    err.clear();
    LONG r = RegDeleteTreeW(HKEY_CURRENT_USER, kVerbKey);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
        err = L"删除注册表项失败 err=" + std::to_wstring(r);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    Log(L"桌面右键菜单撤销入口已卸载");
    return true;
}

}  // namespace shellmenu
