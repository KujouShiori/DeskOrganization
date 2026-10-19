#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 桌面图标的真实坐标，通过桌面 SysListView32 读写。
// 实测结论：写注册表的 IconLayouts 不会生效（Explorer 用内存缓存把它覆盖回去），
// 而 LVM_SETITEMPOSITION 会立刻移动图标，注册表随后由 Shell 自己更新。
struct IconPosition {
    std::wstring name;
    LONG x = 0;
    LONG y = 0;
};

bool IconViewAvailable();
bool IconViewRead(std::vector<IconPosition>& out, std::wstring& err);
bool IconViewApply(const std::vector<IconPosition>& in, std::wstring& err, int& moved, int& unmatched);

// 撤销之后图标是异步出现的，所以这里会等它出现再套位置，最多等 timeoutMs。
bool IconViewRestore(const std::vector<IconPosition>& wanted, std::wstring& err, DWORD timeoutMs = 3000);

// 取出当前桌面上、且路径属于 dir 的那几个图标的坐标，用来在移走之前留档。
std::vector<IconPosition> IconViewCaptureInDir(const std::vector<std::wstring>& paths, const std::wstring& dir);
