// How does D3DX fill the mip levels a .dds file lacks, with the engine's load description (MipLevels 0 = full chain,
// MipFilter NONE | SRGB_IN | SRGB_OUT)? For files whose chain stops early it prints the file's last level (decoded) and
// what D3DX made of the next levels, next to two candidates: the top-left pixel and the (sRGB-aware) 2x2 average.
//
//   tail_probe.exe <folder with .dds files> [max files]
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")

struct LoadInfo {
    UINT width, height, depth, firstMip, mipLevels, usage, bindFlags, cpuFlags, miscFlags, format, filter, mipFilter;
    void *srcInfo;
};
typedef HRESULT(WINAPI *CreateFn)(ID3D11Device *, const void *, SIZE_T, LoadInfo *, void *, ID3D11Resource **, HRESULT *);

static void Rgb565(unsigned c, int *rgb) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2), rgb[1] = (g << 2) | (g >> 4), rgb[2] = (b << 3) | (b >> 2);
}
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
        for (int p = 0; p < 16; p++) px[p][3] = (BYTE)(((b[p / 2] >> (4 * (p & 1))) & 15) * 17);
    } else {
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
static double ToLin(int v) { double c = v / 255.0; return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static int ToSrgb(double l) {
    double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1 / 2.4) - 0.055;
    return std::max(0, std::min(255, (int)lround(c * 255)));
}

int main(int argc, char **argv) {
    int maxFiles = argc > 2 ? atoi(argv[2]) : 12;
    auto create = (CreateFn)GetProcAddress(LoadLibraryA("d3dx11_42.dll"), "D3DX11CreateTextureFromMemory");
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    std::vector<std::string> files, dirs = {argv[1]};
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
    int shown = 0;
    for (auto &path : files) {
        FILE *f = fopen(path.c_str(), "rb");
        std::vector<BYTE> d;
        fseek(f, 0, SEEK_END);
        d.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(d.data(), 1, d.size(), f);
        fclose(f);
        if (!(*(DWORD *)&d[80] & 4)) continue;
        DWORD cc = *(DWORD *)&d[84];
        DXGI_FORMAT ff = cc == MAKEFOURCC('D', 'X', 'T', '1')   ? DXGI_FORMAT_BC1_UNORM_SRGB
                         : cc == MAKEFOURCC('D', 'X', 'T', '5') ? DXGI_FORMAT_BC3_UNORM_SRGB
                                                                : DXGI_FORMAT_UNKNOWN;
        if (!ff || (*(DWORD *)&d[112] & 0x200)) continue;
        UINT w = *(DWORD *)&d[16], h = *(DWORD *)&d[12], mips = (*(DWORD *)&d[8] & 0x20000) ? *(DWORD *)&d[28] : 1;
        UINT full = 1;
        while ((std::max(w, h) >> full) > 0) full++;
        if (getenv("CUT") && mips == full && full > 4) {  // pretend the file stops CUT levels early
            mips = full - atoi(getenv("CUT"));
            *(DWORD *)&d[28] = mips;
        }
        if (mips >= full || mips < 2 || full - mips > 3) continue;
        LoadInfo li = {w, h, 0xFFFFFFFF, 0, 0, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, (UINT)ff, 0x600001,
                       0x600001, nullptr};
        ID3D11Resource *res = nullptr;
        if (FAILED(create(dev, d.data(), d.size(), &li, nullptr, &res, nullptr))) continue;
        D3D11_TEXTURE2D_DESC desc;
        ((ID3D11Texture2D *)res)->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC s = desc;
        s.Usage = D3D11_USAGE_STAGING, s.BindFlags = 0, s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *st;
        dev->CreateTexture2D(&s, nullptr, &st);
        ctx->CopyResource(st, res);
        printf("%s: %ux%u, %u of %u mips in the file, D3DX made %u\n", path.c_str() + strlen(argv[1]) + 1, w, h, mips,
               full, desc.MipLevels);
        for (UINT m = mips - 1; m < desc.MipLevels; m++) {
            UINT mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
            D3D11_MAPPED_SUBRESOURCE map;
            ctx->Map(st, m, D3D11_MAP_READ, 0, &map);
            BYTE px[16][4];
            DecodeBlock(ff, (const BYTE *)map.pData, px);
            ctx->Unmap(st, m);
            printf("   level %u (%ux%u)%s:", m, mw, mh, m < mips ? " file" : " made");
            for (UINT y = 0; y < std::min(mh, 4u); y++)
                for (UINT x = 0; x < std::min(mw, 4u); x++) {
                    BYTE *p = px[y * 4 + x];
                    printf("  (%3d %3d %3d)", p[0], p[1], p[2]);
                }
            if (m >= mips) {  // candidates from the level above as D3DX holds it
                ctx->Map(st, m - 1, D3D11_MAP_READ, 0, &map);
                BYTE up[16][4];
                DecodeBlock(ff, (const BYTE *)map.pData, up);
                ctx->Unmap(st, m - 1);
                UINT uw = std::max(1u, w >> (m - 1)), uh = std::max(1u, h >> (m - 1));
                double lin[4] = {0, 0, 0, 0};
                int n = 0, plain[4] = {0, 0, 0, 0};
                for (UINT y = 0; y < std::min(uh, 2u); y++)
                    for (UINT x = 0; x < std::min(uw, 2u); x++, n++)
                        for (int c = 0; c < 4; c++) {
                            lin[c] += c < 3 ? ToLin(up[y * 4 + x][c]) : up[y * 4 + x][c];
                            plain[c] += up[y * 4 + x][c];
                        }
                printf("   | top-left (%3d %3d %3d %3d)", up[0][0], up[0][1], up[0][2], up[0][3]);
                printf(" | srgb avg (%3d %3d %3d %3d)", ToSrgb(lin[0] / n), ToSrgb(lin[1] / n), ToSrgb(lin[2] / n),
                       (int)lround(lin[3] / n));
                printf(" | plain avg (%3d %3d %3d %3d)", (plain[0] + n / 2) / n, (plain[1] + n / 2) / n,
                       (plain[2] + n / 2) / n, (plain[3] + n / 2) / n);
            }
            printf("\n");
        }
        st->Release(), res->Release();
        if (++shown >= maxFiles) break;
    }
    return 0;
}
