#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)(-4))
#endif

void Log(const wchar_t* fmt, ...);

// 手上已经是 UTF-8 字节时走这个：Log 是宽字符 printf，
// 拿 %hs 打 UTF-8 字节会按 ANSI 代码页解释，中文变成乱码。
void LogU8(const std::string& utf8);
void LogHr(const wchar_t* what, HRESULT hr);

// %APPDATA%\DeskOrganization，首次调用时建目录。
const std::wstring& AppDir();
std::wstring AppFile(const wchar_t* name);

std::wstring Utf8ToWide(const char* s);
std::string WideToUtf8(const std::wstring& s);

bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileBytes(const std::wstring& path, const std::string& bytes);

std::wstring DesktopPath();
std::wstring ProfilePath();
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);

// 逗号或分号分隔的列表，去空白去空项。members 和规则的 ext 都用它。
std::vector<std::wstring> SplitCommaList(const std::wstring& s);
bool EnsureDirExists(const std::wstring& path);
bool PathExists(const std::wstring& path);

// 跨进程挂到 Progman 下时，任何阻塞都可能连带卡住资源管理器，
// 因此所有窗口过程里只允许做非阻塞的事。
void PumpMessages(DWORD ms);

std::wstring FileNameOfW(const std::wstring& path);
