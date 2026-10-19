#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[2300];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02d:%02d:%02d.%03d %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    HANDLE f = CreateFileW(AppFile(L"log.txt").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u8 = WideToUtf8(line);
    DWORD written = 0;
    WriteFile(f, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
    CloseHandle(f);
}

void LogHr(const wchar_t* what, HRESULT hr) {
    if (SUCCEEDED(hr)) {
        Log(L"%s ok", what);
        return;
    }
    Log(L"%s FAILED hr=0x%08lX gle=%lu", what, static_cast<unsigned long>(hr), GetLastError());
}

const std::wstring& AppDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;

    PWSTR base = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base);
    if (SUCCEEDED(hr) && base) {
        dir = base;
        CoTaskMemFree(base);
    } else {
        wchar_t buf[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf);
        dir = buf;
    }
    dir += L"\\DeskOrganization";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring AppFile(const wchar_t* name) { return AppDir() + L"\\" + name; }

std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string u8(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, u8.data(), n, nullptr, nullptr);
    return u8;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char chunk[8192];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, chunk, sizeof chunk, &got, nullptr) || got == 0) break;
        out.append(chunk, got);
    }
    CloseHandle(h);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"打开 %s 失败 gle=%lu", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = bytes.empty() || WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    if (ok && written != bytes.size()) ok = FALSE;
    if (!ok) Log(L"写入 %s 失败 gle=%lu", path.c_str(), GetLastError());
    CloseHandle(h);
    return ok ? true : false;
}

bool PathExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring DesktopPath() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
        std::wstring s = p;
        CoTaskMemFree(p);
        return s;
    }
    wchar_t buf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf);
    return buf;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

std::wstring ProfilePath() {
    wchar_t buf[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf);
    return buf;
}

bool EnsureDirExists(const std::wstring& path) {
    if (path.empty()) return false;
    if (PathExists(path)) return true;
    std::wstring acc;
    size_t i = 0;
    if (path[1] == L':') { acc = path.substr(0, 3); i = 3; }
    for (; i < path.size(); ++i) {
        acc += path[i];
        if (path[i] != L'\\') continue;
        CreateDirectoryW(acc.c_str(), nullptr);
    }
    if (!PathExists(path)) CreateDirectoryW(path.c_str(), nullptr);
    return PathExists(path);
}

void PumpMessages(DWORD ms) {
    MSG m;
    DWORD until = GetTickCount() + ms;
    while (GetTickCount() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}

std::wstring FileNameOfW(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void LogU8(const std::string& utf8) {
    Log(L"%s", Utf8ToWide(utf8.c_str()).c_str());
}

std::vector<std::wstring> SplitCommaList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t sep = s.find_first_of(L",;", start);
        std::wstring item = sep == std::wstring::npos ? s.substr(start) : s.substr(start, sep - start);
        size_t b = item.find_first_not_of(L" \t");
        size_t e = item.find_last_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(item.substr(b, e - b + 1));
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return out;
}
