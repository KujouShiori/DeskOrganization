#include "theme.h"

#include "util.h"

// 用显式赋值而不是聚合初始化：Palette 有五个字段，写成 {33,41,54,56,...} 时
// 顺序一旦看错不会报错，只会得到一个说不上哪里不对的配色。
static Palette Build(unsigned bodyR, unsigned bodyG, unsigned bodyB, unsigned titleR, unsigned titleG,
                     unsigned titleB, unsigned textR, unsigned textG, unsigned textB, unsigned edgeR,
                     unsigned edgeG, unsigned edgeB, unsigned dimR, unsigned dimG, unsigned dimB) {
    Palette p;
    p.body = (bodyR << 16) | (bodyG << 8) | bodyB;
    p.title = (titleR << 16) | (titleG << 8) | titleB;
    p.text = (textR << 16) | (textG << 8) | textB;
    p.edge = (edgeR << 16) | (edgeG << 8) | edgeB;
    p.dim = (dimR << 16) | (dimG << 8) | dimB;
    return p;
}

static const Palette kDark = Build(33, 41, 54, 56, 69, 89, 235, 240, 250, 102, 120, 153, 184, 199, 224);
static const Palette kLight = Build(245, 245, 247, 224, 227, 234, 26, 28, 36, 158, 166, 184, 89, 96, 115);

ThemeMode ThemeParse(const std::wstring& text) {
    if (_wcsicmp(text.c_str(), L"light") == 0) return ThemeMode::Light;
    if (_wcsicmp(text.c_str(), L"dark") == 0) return ThemeMode::Dark;
    return ThemeMode::Auto;
}

std::wstring ThemeToString(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return L"light";
        case ThemeMode::Dark:
            return L"dark";
        default:
            return L"auto";
    }
}

bool SystemPrefersDark() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof value, type = 0;
    LONG r = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value == 0;  // 0 = 深色，1 = 浅色
}

static ThemeMode g_mode = ThemeMode::Auto;

void ThemeSetMode(ThemeMode mode) { g_mode = mode; }

ThemeMode ThemeGetMode() { return g_mode; }

Palette ThemeCurrent() {
    if (g_mode == ThemeMode::Dark) return kDark;
    if (g_mode == ThemeMode::Light) return kLight;
    // Auto 每次现读，切换系统主题后不用重启程序。
    return SystemPrefersDark() ? kDark : kLight;
}

const Palette& DarkPalette() { return kDark; }
const Palette& LightPalette() { return kLight; }
