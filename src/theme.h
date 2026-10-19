#pragma once

#include <string>

// 配色。用 0..255 的 RGB，绘制时再换算成 Direct2D 的浮点色。
struct Palette {
    unsigned body = 0;
    unsigned title = 0;
    unsigned text = 0;
    unsigned edge = 0;
    unsigned dim = 0;
};

enum class ThemeMode { Auto, Light, Dark };

ThemeMode ThemeParse(const std::wstring& text);
std::wstring ThemeToString(ThemeMode mode);

// HKCU ...\Themes\Personalize 的 AppsUseLightTheme。读不到时按浅色处理。
bool SystemPrefersDark();

void ThemeSetMode(ThemeMode mode);
ThemeMode ThemeGetMode();

// Auto 模式下每次调用都会重新读注册表，这样切主题时不用重启程序。
Palette ThemeCurrent();

const Palette& DarkPalette();
const Palette& LightPalette();
