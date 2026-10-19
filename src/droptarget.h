#pragma once

#include <ole2.h>
#include <windows.h>

#include <string>
#include <vector>

// 收到文件拖放时回调；返回是否接受。ctx 是盒子自己。
using DropAcceptFn = bool (*)(void* ctx, const std::vector<std::wstring>& paths);

class FileDropTarget : public IDropTarget {
 public:
    FileDropTarget(void* ctx, DropAcceptFn fn);

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP DragEnter(IDataObject* pcbDataObject, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;
    STDMETHODIMP DragLeave() override;
    STDMETHODIMP Drop(IDataObject* pcbDataObject, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override;

 private:
    ~FileDropTarget() = default;

    long refs_ = 1;
    void* ctx_ = nullptr;
    DropAcceptFn fn_ = nullptr;
};

bool ExtractDroppedPaths(IDataObject* source, std::vector<std::wstring>& outPaths);
