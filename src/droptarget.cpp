#include "droptarget.h"

#include <shellapi.h>

#include "util.h"

FileDropTarget::FileDropTarget(void* ctx, DropAcceptFn fn) : ctx_(ctx), fn_(fn) {}

STDMETHODIMP FileDropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) FileDropTarget::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

STDMETHODIMP_(ULONG) FileDropTarget::Release() {
    long n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

bool ExtractDroppedPaths(IDataObject* source, std::vector<std::wstring>& outPaths) {
    outPaths.clear();
    if (!source) return false;

    FORMATETC fe{};
    fe.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
    fe.ptd = nullptr;
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    HRESULT hr = source->GetData(&fe, &medium);
    if (FAILED(hr)) return false;
    if (!(medium.tymed & TYMED_HGLOBAL) || !medium.hGlobal) {
        ReleaseStgMedium(&medium);
        return false;
    }

    HDROP hdrop = static_cast<HDROP>(medium.hGlobal);
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT chars = DragQueryFileW(hdrop, i, nullptr, 0);
        if (chars == 0) continue;
        std::wstring path(chars, L'\0');
        // DragQueryFileW 写入 chars 个字符并补终止符，所以缓冲要留一格。
        if (DragQueryFileW(hdrop, i, path.data(), chars + 1) == 0) continue;
        path.resize(wcslen(path.c_str()));
        if (!path.empty()) outPaths.push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return !outPaths.empty();
}

static DWORD AllowMove(DWORD requested) {
    if (requested & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (requested & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}

STDMETHODIMP FileDropTarget::DragEnter(IDataObject* data, DWORD grfKeyState, POINTL, DWORD* pdwEffect) {
    if (!pdwEffect) return E_POINTER;
    std::vector<std::wstring> paths;
    *pdwEffect = ExtractDroppedPaths(data, paths) ? AllowMove(grfKeyState) : DROPEFFECT_NONE;
    return S_OK;
}

STDMETHODIMP FileDropTarget::DragOver(DWORD grfKeyState, POINTL, DWORD* pdwEffect) {
    if (!pdwEffect) return E_POINTER;
    *pdwEffect = AllowMove(grfKeyState);
    return S_OK;
}

STDMETHODIMP FileDropTarget::DragLeave() { return S_OK; }

STDMETHODIMP FileDropTarget::Drop(IDataObject* data, DWORD grfKeyState, POINTL, DWORD* pdwEffect) {
    if (!pdwEffect) return E_POINTER;
    std::vector<std::wstring> paths;
    if (!ExtractDroppedPaths(data, paths)) {
        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }
    Log(L"收到拖放 %zu 项", paths.size());
    if (fn_ && ctx_) fn_(ctx_, paths);
    // 移动由 IFileOperation 完成，这里如实回报效果，源侧才不会再多复制一份。
    *pdwEffect = (grfKeyState & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    return S_OK;
}
