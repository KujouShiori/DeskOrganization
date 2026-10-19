#include "render.h"

#include "util.h"

static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dwrite = nullptr;

bool RenderInit() {
    if (g_d2d && g_dwrite) return true;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                   reinterpret_cast<void**>(&g_d2d));
    LogHr(L"D2D1CreateFactory", hr);
    if (FAILED(hr)) { g_d2d = nullptr; return false; }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&g_dwrite));
    LogHr(L"DWriteCreateFactory", hr);
    if (FAILED(hr)) { g_dwrite = nullptr; RenderShutdown(); return false; }

    return true;
}

void RenderShutdown() {
    if (g_dwrite) { g_dwrite->Release(); g_dwrite = nullptr; }
    if (g_d2d) { g_d2d->Release(); g_d2d = nullptr; }
}

ID2D1Factory* TheD2D() { return g_d2d; }
IDWriteFactory* TheDWrite() { return g_dwrite; }
