// 生成程序文件图标：把 src/iconres.cpp 里那套托盘图标的画法按多个尺寸渲染，装成
// out\appicon-*.ico，验收后拷进 resources\DeskOrganization.ico，由 src\app.rc 编进 exe。
// 每个尺寸同时导出 PNG 给人工验收。纯输出工具：不写注册表、不动桌面，也不调用 Log。
#define WIN32_LEAN_AND_MEAN
#include <ole2.h>
#include <windows.h>

#include <shlwapi.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "iconres.h"
#include "theme.h"
#include "util.h"

namespace {

#pragma pack(push, 1)
struct IconDir {
    unsigned short reserved = 0;
    unsigned short type = 1;  // 1 = icon
    unsigned short count = 0;
};
struct IconEntry {
    unsigned char width = 0;   // 0 表示 256
    unsigned char height = 0;  // 0 表示 256
    unsigned char colorCount = 0;
    unsigned char reserved = 0;
    unsigned short planes = 1;
    unsigned short bitCount = 32;
    unsigned long bytesInRes = 0;
    unsigned long imageOffset = 0;
};
#pragma pack(pop)

// 资源管理器小/中/大图标、任务栏、托盘，以及 125%/150%/200% DPI 要的格子都在这里面。
// 最大到 128：256 那一档交给 shell 放大，扁平几何图形放大后边缘本来就软，而 256 的
// 位图条目要 270KB，为一条发丝描边不值当。
const int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128};
constexpr int kSizeCount = static_cast<int>(sizeof(kSizes) / sizeof(kSizes[0]));

IWICImagingFactory* g_factory = nullptr;

bool WritePng(const std::wstring& path, int size, const std::vector<uint32_t>& argb) {
    IStream* stream = nullptr;
    if (FAILED(SHCreateStreamOnFileW(path.c_str(), STGM_CREATE | STGM_WRITE, &stream))) {
        fwprintf(stderr, L"open failed: %s\n", path.c_str());
        return false;
    }
    IWICBitmap* bitmap = nullptr;
    HRESULT hr = g_factory->CreateBitmapFromMemory(size, size, GUID_WICPixelFormat32bppBGRA, size * 4,
                                                   static_cast<UINT>(size) * static_cast<UINT>(size) * 4u,
                                                   reinterpret_cast<BYTE*>(const_cast<uint32_t*>(argb.data())),
                                                   &bitmap);
    IWICBitmapEncoder* encoder = nullptr;
    if (SUCCEEDED(hr)) hr = g_factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    IWICBitmapFrameEncode* frame = nullptr;
    // 属性包传 NULL：WIC 允许，用默认编码器设置即可。
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(size, size);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(size, static_cast<UINT>(size) * 4u,
                                static_cast<UINT>(size) * static_cast<UINT>(size) * 4u,
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(argb.data())));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (bitmap) bitmap->Release();
    stream->Release();
    if (FAILED(hr)) {
        fwprintf(stderr, L"png encode failed hr=%08lx size=%d\n", hr, size);
        return false;
    }
    return true;
}

// rc.exe 不收 PNG 压缩的图标条目（报 RC2175 "not in 3.00 format"），所以每档都用位图
// 条目：头 + 自下而上的 32 位颜色 + 按 32 位对齐的单色 AND 掩码。头里的 biHeight 写
// 两倍高度是老规矩，指颜色一路加掩码一路。
std::string BuildIconImage(int size, const std::vector<uint32_t>& argb, const std::vector<uint8_t>& mask) {
    const auto colorBytes = static_cast<unsigned long>(size) * static_cast<unsigned long>(size) * 4u;
    const auto maskStride = (static_cast<unsigned long>(size) + 31) / 32 * 4u;
    const auto maskBytes = static_cast<unsigned long>(maskStride * static_cast<unsigned long>(size));

    std::string out;
    out.reserve(sizeof(BITMAPINFOHEADER) + colorBytes + maskBytes);

    BITMAPINFOHEADER header = {};
    header.biSize = sizeof(header);
    header.biWidth = size;
    header.biHeight = size * 2;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = colorBytes + maskBytes;
    out.append(reinterpret_cast<const char*>(&header), sizeof(header));

    std::vector<uint32_t> bottomUp(static_cast<size_t>(size) * static_cast<size_t>(size));
    for (int y = 0; y < size; ++y) {
        memcpy(&bottomUp[static_cast<size_t>(y) * size], &argb[static_cast<size_t>(size - 1 - y) * size],
               static_cast<size_t>(size) * 4u);
    }
    out.append(reinterpret_cast<const char*>(bottomUp.data()), colorBytes);

    // 画出来的掩码行宽只按字节对齐，图标里要按 32 位对齐，逐行搬一遍。
    const size_t srcStride = (static_cast<size_t>(size) + 7) / 8;
    std::vector<uint8_t> andMask(static_cast<size_t>(maskBytes), 0u);
    for (int y = 0; y < size; ++y) {
        const size_t srcRow = static_cast<size_t>(size - 1 - y) * srcStride;
        const size_t dstRow = static_cast<size_t>(y) * maskStride;
        for (int x = 0; x < size; ++x) {
            const bool transparent = (mask[srcRow + x / 8] >> (7 - (x % 8))) & 1u;
            if (!transparent) continue;
            andMask[dstRow + x / 8] = static_cast<uint8_t>(andMask[dstRow + x / 8] | (1u << (7 - (x % 8))));
        }
    }
    out.append(reinterpret_cast<const char*>(andMask.data()), maskBytes);
    return out;
}

bool EmitPalette(const wchar_t* tag, const Palette& palette) {
    const std::wstring dir = L"out\\iconpreview";
    CreateDirectoryW(L"out", nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    std::vector<uint32_t> argb;
    std::vector<uint8_t> mask;
    std::vector<std::string> images;
    for (int size : kSizes) {
        BuildBoxIconPixels(palette, size, argb, mask);
        wchar_t name[128];
        swprintf(name, 128, L"%s\\%s-%d.png", dir.c_str(), tag, size);
        if (!WritePng(name, size, argb)) return false;
        images.push_back(BuildIconImage(size, argb, mask));
    }

    // 目录要等每块图长度都定了才能写：一边 append 图块一边往 &blob[...] 里填目录的话，
    // 第一次扩容就让那个指针失效，写出来的后半截目录全是 0。同理，reinterpret_cast 指向
    // 一片裸字节不会执行结构体的默认成员初始化，type=1 得靠真的 IconDir 对象来带出来。
    const unsigned long headerBytes = sizeof(IconDir) + sizeof(IconEntry) * kSizeCount;
    std::string body;
    std::vector<IconEntry> entries(static_cast<size_t>(kSizeCount));
    unsigned long offset = headerBytes;
    for (int i = 0; i < kSizeCount; ++i) {
        entries[i].width = static_cast<unsigned char>(kSizes[i] >= 256 ? 0 : kSizes[i]);
        entries[i].height = static_cast<unsigned char>(kSizes[i] >= 256 ? 0 : kSizes[i]);
        entries[i].bytesInRes = static_cast<unsigned long>(images[i].size());
        entries[i].imageOffset = offset;
        body += images[i];
        offset += static_cast<unsigned long>(images[i].size());
    }

    IconDir header;
    header.count = static_cast<unsigned short>(kSizeCount);
    std::string blob;
    blob.reserve(headerBytes + body.size());
    blob.append(reinterpret_cast<const char*>(&header), sizeof(header));
    blob.append(reinterpret_cast<const char*>(entries.data()), sizeof(IconEntry) * entries.size());
    blob += body;

    wchar_t ico[128];
    swprintf(ico, 128, L"out\\appicon-%s.ico", tag);
    if (!WriteFileBytes(ico, blob)) {
        fwprintf(stderr, L"cannot write %s\n", ico);
        return false;
    }
    fwprintf(stdout, L"%s  %lu bytes, %d sizes\n", ico, static_cast<unsigned long>(blob.size()), kSizeCount);
    return true;
}

}  // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_factory)))) {
        fwprintf(stderr, L"WIC factory unavailable\n");
        CoUninitialize();
        return 1;
    }
    const bool ok = EmitPalette(L"dark", DarkPalette()) && EmitPalette(L"light", LightPalette());
    if (g_factory) g_factory->Release();
    CoUninitialize();
    return ok ? 0 : 1;
}
