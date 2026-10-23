// Checks the texture fast path of version.dll against D3DX on real game textures (tools\dds_sample.py picks them).
// For every .dds under <folder> it makes the texture twice, as the engine asks for it (sRGB format, every mip level,
// filter NONE | SRGB_IN | SRGB_OUT): once through d3dx11_42.dll and once through the fast path. Legacy DXT and RGBA
// files are also tried wrapped in a DX10 header, which is how the engine hands most textures over at run time, and
// with the top mip level skipped (lower texture quality), with and without the size given. Then it reads both back
// and compares every mip level, decoding BC1/BC2/BC3 blocks to pixels. A different size, mip count or format is a
// problem; different pixels are not (D3DX re-encodes the blocks, the fast path keeps the file's own).
//
//   tex_test.exe <folder> <version.dll>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

struct LoadInfo {
    UINT width, height, depth, firstMip, mipLevels, usage, bindFlags, cpuFlags, miscFlags, format, filter, mipFilter;
    void *srcInfo;
};
typedef HRESULT(WINAPI *CreateFn)(ID3D11Device *, const void *, SIZE_T, LoadInfo *, void *, ID3D11Resource **, HRESULT *);
typedef HRESULT(WINAPI *FastFn)(void *, const void *, SIZE_T, void *, void **);

static void Rgb565(unsigned c, int *rgb) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2), rgb[1] = (g << 2) | (g >> 4), rgb[2] = (b << 3) | (b >> 2);
}

// BC1 colour part into px[16][4]; three-colour mode only for BC1 itself.
static void DecodeColor(const BYTE *b, BYTE px[16][4], bool bc1) {
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    int c[4][4];
    Rgb565(c0, c[0]), Rgb565(c1, c[1]);
    c[0][3] = c[1][3] = 255;
    if (!bc1 || c0 > c1) {
        for (int i = 0; i < 3; i++) c[2][i] = (2 * c[0][i] + c[1][i]) / 3, c[3][i] = (c[0][i] + 2 * c[1][i]) / 3;
        c[2][3] = c[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) c[2][i] = (c[0][i] + c[1][i]) / 2, c[3][i] = 0;
        c[2][3] = 255, c[3][3] = 0;
    }
    unsigned idx = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24;
    for (int p = 0; p < 16; p++)
        for (int i = 0; i < 4; i++) px[p][i] = (BYTE)c[(idx >> (2 * p)) & 3][i];
}

static void DecodeBlock(DXGI_FORMAT f, const BYTE *b, BYTE px[16][4]) {
    if (f == DXGI_FORMAT_BC1_UNORM || f == DXGI_FORMAT_BC1_UNORM_SRGB) return DecodeColor(b, px, true);
    DecodeColor(b + 8, px, false);
    if (f == DXGI_FORMAT_BC2_UNORM || f == DXGI_FORMAT_BC2_UNORM_SRGB) {
        for (int p = 0; p < 16; p++) {
            int a = (b[p / 2] >> (4 * (p & 1))) & 15;
            px[p][3] = (BYTE)(a * 17);
        }
    } else {  // BC3
        int a[8] = {b[0], b[1]};
        if (a[0] > a[1])
            for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else {
            for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
            a[6] = 0, a[7] = 255;
        }
        unsigned long long bits = 0;
        for (int i = 0; i < 6; i++) bits |= (unsigned long long)b[2 + i] << (8 * i);
        for (int p = 0; p < 16; p++) px[p][3] = (BYTE)a[(bits >> (3 * p)) & 7];
    }
}

static bool IsBc(DXGI_FORMAT f) { return f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB; }

// Pixels (RGBA8) of one mip level of a staging texture.
static std::vector<BYTE> Pixels(ID3D11DeviceContext *ctx, ID3D11Texture2D *staging, UINT sub, UINT w, UINT h,
                                DXGI_FORMAT f) {
    std::vector<BYTE> out((size_t)w * h * 4);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging, sub, D3D11_MAP_READ, 0, &m))) return {};
    const BYTE *src = (const BYTE *)m.pData;
    if (IsBc(f)) {
        UINT bs = (f <= DXGI_FORMAT_BC1_UNORM_SRGB) ? 8 : 16;
        for (UINT by = 0; by < (h + 3) / 4; by++)
            for (UINT bx = 0; bx < (w + 3) / 4; bx++) {
                BYTE px[16][4];
                DecodeBlock(f, src + by * m.RowPitch + bx * bs, px);
                for (int p = 0; p < 16; p++) {
                    UINT x = bx * 4 + p % 4, y = by * 4 + p / 4;
                    if (x < w && y < h) memcpy(&out[((size_t)y * w + x) * 4], px[p], 4);
                }
            }
    } else {
        for (UINT y = 0; y < h; y++) memcpy(&out[(size_t)y * w * 4], src + y * m.RowPitch, (size_t)w * 4);
    }
    ctx->Unmap(staging, sub);
    return out;
}

static ID3D11Texture2D *Staging(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Resource *res, D3D11_TEXTURE2D_DESC &d) {
    ((ID3D11Texture2D *)res)->GetDesc(&d);
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING, s.BindFlags = 0, s.CPUAccessFlags = D3D11_CPU_ACCESS_READ, s.MiscFlags = 0;
    ID3D11Texture2D *t = nullptr;
    if (FAILED(dev->CreateTexture2D(&s, nullptr, &t))) return nullptr;
    ctx->CopyResource(t, res);
    return t;
}

static DXGI_FORMAT FileFormat(const std::vector<BYTE> &d) {
    DWORD pf = *(DWORD *)&d[80], cc = *(DWORD *)&d[84];
    if (pf & 4) {
        if (cc == MAKEFOURCC('D', 'X', '1', '0')) return (DXGI_FORMAT) * (DWORD *)&d[128];
        if (cc == MAKEFOURCC('D', 'X', 'T', '1')) return DXGI_FORMAT_BC1_UNORM;
        if (cc == MAKEFOURCC('D', 'X', 'T', '3')) return DXGI_FORMAT_BC2_UNORM;
        if (cc == MAKEFOURCC('D', 'X', 'T', '5')) return DXGI_FORMAT_BC3_UNORM;
        return DXGI_FORMAT_UNKNOWN;
    }
    if (*(DWORD *)&d[88] == 32 && *(DWORD *)&d[92] == 0xFF0000 && *(DWORD *)&d[104] == 0xFF000000)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (*(DWORD *)&d[88] == 32 && *(DWORD *)&d[92] == 0xFF && *(DWORD *)&d[104] == 0xFF000000)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

static DXGI_FORMAT Srgb(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    default: return f;
    }
}

// The same file with a DX10 header for format f (pixel data unchanged).
static std::vector<BYTE> WrapDx10(const std::vector<BYTE> &d, DXGI_FORMAT f) {
    std::vector<BYTE> out(d.begin(), d.begin() + 128);
    *(DWORD *)&out[80] = 4, *(DWORD *)&out[84] = MAKEFOURCC('D', 'X', '1', '0');
    DWORD dx10[5] = {(DWORD)f, D3D11_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 0};
    out.insert(out.end(), (BYTE *)dx10, (BYTE *)dx10 + 20);
    out.insert(out.end(), d.begin() + 128, d.end());
    return out;
}

struct Stat {
    int files = 0, fast = 0, same = 0;
    double d3dxMs = 0, fastMs = 0, sumDiff = 0;
    long long pixels = 0;
    int maxDiff = 0;
};

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: tex_test <folder> <version.dll>\n"); return 2; }
    HMODULE d3dx = LoadLibraryA("d3dx11_42.dll");
    HMODULE fix = LoadLibraryA(argv[2]);
    auto create = d3dx ? (CreateFn)GetProcAddress(d3dx, "D3DX11CreateTextureFromMemory") : nullptr;
    auto fast = fix ? (FastFn)GetProcAddress(fix, MAKEINTRESOURCEA(100)) : nullptr;
    if (!create || !fast) { printf("d3dx11_42.dll or the fast path export is missing\n"); return 1; }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx))) { printf("no D3D11 device\n"); return 1; }
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    std::map<std::string, Stat> stats;
    std::vector<std::string> files;
    std::vector<std::string> dirs = {argv[1]};
    while (!dirs.empty()) {
        std::string d = dirs.back();
        dirs.pop_back();
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(d + "\\" + n);
            else if (n.size() > 4 && _stricmp(n.c_str() + n.size() - 4, ".dds") == 0) files.push_back(d + "\\" + n);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    int problems = 0;
    for (auto &path : files) {
        FILE *f = fopen(path.c_str(), "rb");
        std::vector<BYTE> data;
        if (f) {
            fseek(f, 0, SEEK_END);
            data.resize(ftell(f));
            fseek(f, 0, SEEK_SET);
            fread(data.data(), 1, data.size(), f);
            fclose(f);
        }
        if (data.size() < 128) continue;
        DXGI_FORMAT ff = FileFormat(data);
        std::string kind = path.substr(strlen(argv[1]) + 1);
        kind = kind.substr(0, kind.find('\\'));
        UINT width = *(DWORD *)&data[16], height = *(DWORD *)&data[12];
        UINT mips = (*(DWORD *)&data[8] & 0x20000) && *(DWORD *)&data[28] ? *(DWORD *)&data[28] : 1;
        for (int wrap = 0; wrap < 2; wrap++)
        // skip 0: as the engine asks at full texture quality. skip 1: the top level left out with the size given, as
        // the engine asks at lower quality (width >> 1). skip 1 no size: D3DX then keeps the full size, so the fast
        // path has to leave it to D3DX.
        for (int variant = 0; variant < 3; variant++) {
            if (wrap && (ff == DXGI_FORMAT_UNKNOWN || *(DWORD *)&data[84] == MAKEFOURCC('D', 'X', '1', '0'))) continue;
            if (variant && mips < 2) continue;
            std::vector<BYTE> d = wrap ? WrapDx10(data, Srgb(ff)) : data;
            std::string key = std::string(variant == 1 ? "skip 1: " : variant == 2 ? "skip 1, no size: " : "") +
                              (wrap ? "DX10(" + kind + ")" : kind);
            LoadInfo li = {width, height, 0xFFFFFFFF, 0, 0, D3D11_USAGE_DEFAULT,
                           D3D11_BIND_SHADER_RESOURCE, 0, 0, (UINT)(ff ? Srgb(ff) : 0xFFFFFFFD), 0x600001, 0x600001,
                           nullptr};
            if (*(DWORD *)&data[112] & 0x200) li.miscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;  // as the engine asks
            if (variant) li.firstMip = 1;
            if (variant == 1) li.width = std::max(1u, width >> 1), li.height = std::max(1u, height >> 1);
            if (variant == 2) li.width = li.height = 0xFFFFFFFF;  // D3DX11_DEFAULT
            Stat &s = stats[key];
            s.files++;
            ID3D11Resource *a = nullptr, *b = nullptr;
            LARGE_INTEGER t0, t1, t2;
            QueryPerformanceCounter(&t0);
            HRESULT ha = create(dev, d.data(), d.size(), &li, nullptr, &a, nullptr);
            QueryPerformanceCounter(&t1);
            HRESULT hb = fast(dev, d.data(), d.size(), &li, (void **)&b);
            QueryPerformanceCounter(&t2);
            s.d3dxMs += (t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart;
            if (hb != S_OK || !b) {
                if (a) a->Release();
                continue;  // left to D3DX at run time
            }
            s.fast++;
            s.fastMs += (t2.QuadPart - t1.QuadPart) * 1000.0 / qpf.QuadPart;
            if (FAILED(ha) || !a) {
                printf("D3DX failed (%08lX) where the fast path made a texture: %s\n", ha, path.c_str());
                problems++;
                b->Release();
                continue;
            }
            D3D11_TEXTURE2D_DESC da, db;
            ID3D11Texture2D *sa = Staging(dev, ctx, a, da), *sb = Staging(dev, ctx, b, db);
            if (da.Width != db.Width || da.Height != db.Height || da.MipLevels != db.MipLevels ||
                da.ArraySize != db.ArraySize || da.Format != db.Format) {
                printf("different texture: %s: d3dx %ux%u m%u a%u f%d, fast %ux%u m%u a%u f%d\n", path.c_str(),
                       da.Width, da.Height, da.MipLevels, da.ArraySize, da.Format, db.Width, db.Height, db.MipLevels,
                       db.ArraySize, db.Format);
                problems++;
            } else {
                bool same = true;
                for (UINT slice = 0; slice < da.ArraySize; slice++)
                    for (UINT mip = 0; mip < da.MipLevels; mip++) {
                        UINT w = std::max(1u, da.Width >> mip), h = std::max(1u, da.Height >> mip);
                        UINT sub = D3D11CalcSubresource(mip, slice, da.MipLevels);
                        auto pa = Pixels(ctx, sa, sub, w, h, da.Format), pb = Pixels(ctx, sb, sub, w, h, db.Format);
                        int shownHere = 0;
                        for (size_t i = 0; i < pa.size() && i < pb.size(); i++) {
                            int diff = abs((int)pa[i] - (int)pb[i]);
                            if (diff > 100 && shownHere++ < 2 && getenv("SHOWBIG"))
                                printf("big diff %d: %s mip %u/%u (%ux%u) pixel %zu,%zu ch %zu: d3dx %d fast %d\n",
                                       diff, path.c_str() + strlen(argv[1]) + 1, mip, da.MipLevels, w, h, (i / 4) % w,
                                       (i / 4) / w, i % 4, pa[i], pb[i]);
                            s.maxDiff = std::max(s.maxDiff, diff);
                            s.sumDiff += diff;
                            if (diff) same = false;
                        }
                        s.pixels += (long long)w * h * 4;
                    }
                s.same += same;
            }
            sa->Release(), sb->Release(), a->Release(), b->Release();
        }
    }
    printf("%-44s %5s %5s %5s %10s %10s %8s %8s\n", "kind", "files", "fast", "same", "d3dx ms", "fast ms", "max diff",
           "mean");
    for (auto &kv : stats) {
        Stat &s = kv.second;
        printf("%-44s %5d %5d %5d %10.1f %10.2f %8d %8.3f\n", kv.first.c_str(), s.files, s.fast, s.same, s.d3dxMs,
               s.fastMs, s.maxDiff, s.pixels ? s.sumDiff / s.pixels : 0.0);
    }
    printf("%s\n", problems ? "PROBLEMS FOUND" : "no structural differences");
    return problems ? 1 : 0;
}
