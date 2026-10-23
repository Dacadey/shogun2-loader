// Checks the version.dll fixes without starting the game: loads a copy of empire.retail.dll (build\input) with the
// freshly built version.dll next to this program, then runs the engine's own code through the patched places:
//   - the worker-pool setup (as the engine calls it at start) and the affinity of every thread afterwards;
//   - the pack reopen, on a real .pack of the game, and whether the handle is still unbuffered;
//   - the intro-movie branch and the per-frame vtable slot;
//   - the DX11 renderer's present, on a stand-in renderer and swap chain: which presents keep vsync;
//   - an overlay that hooks Present again on every new swap chain, as Steam's does, with the engine making its swap
//     chain twice (a switch between window and full screen): the presents must not go round in a circle;
//   - with --crash: a std::bad_alloc, which must leave a minidump (deleted again afterwards).
// Instead of those, with --unload: the engine is unloaded at once, as shogun2.exe does when the game ends, and
// version.dll has to stay loaded through the next round of its statistics thread (31 s); the process then ends
// normally. With --other-build: a copy of the engine with another time stamp, which must be left as it is. With
// --no-vsync-hook: loads_without_vsync=0, and the engine's present must be left as it is. With --stack-overflow: the
// main thread runs out of stack and the process ends there; the next run, with --check-dump only, checks that
// version.dll logged it and wrote a minidump (deleted afterwards). With --ini-case N: settings written as a player may
// write them (case N of kIniCases), and the settings line and the "ini:" lines version.dll must log for them. With
// --cyrillic-folder: this program and version.dll run again from a folder named with Cyrillic letters, whose name must
// be in the log in UTF-8 (up to 0.4 the lines with a path lost all their text).
//
//   hooks_test.exe <input folder> <a .pack file> [number_of_threads]
//                  [--crash | --unload | --other-build | --no-vsync-hook | --stack-overflow | --ini-case N |
//                   --cyrillic-folder]
//   hooks_test.exe --check-dump
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static const DWORD kSingleton = 0x12F1A40;  // RVAs, as in engine.cpp
static const DWORD kPoolSetupCall = 0x9D485;
static const DWORD kPackReopenCall = 0x12E150F;
static const DWORD kIntroMoviesJump = 0xE81DA6;
static const DWORD kFrameSlot = 0x17547F8;
static const DWORD kIatGetProcAddress = 0x15A1190;
static const DWORD kPresent = 0x1487180;      // the DX11 renderer's present (thiscall, two stack arguments it ignores)
static const DWORD kPresentSync = 0x148719A;  // in it: xor eax,eax; cmp byte ptr [esi+1BF1h],al; setne al
static const BYTE kSyncCode[11] = {0x33, 0xC0, 0x38, 0x86, 0xF1, 0x1B, 0x00, 0x00, 0x0F, 0x95, 0xC0};

typedef LONG(NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef LONG(NTAPI *NtQueryInformationFile_t)(HANDLE, PVOID, PVOID, ULONG, int);
struct THREAD_BASIC_INFORMATION_ {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    struct { HANDLE UniqueProcess, UniqueThread; } ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority, BasePriority;
};

static int g_failures;
static void Check(bool ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

static ULONG_PTR Affinity(HANDLE h) {
    static auto q = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    THREAD_BASIC_INFORMATION_ tbi = {};
    return q(h, 0, &tbi, sizeof tbi, nullptr) == 0 ? tbi.AffinityMask : 0;
}

static std::string Lps(ULONG_PTR m) {
    if (m == 0xFFFFFFFF) return "all";
    std::string s;
    for (int i = 0; i < 32; i++)
        if (m & (1u << i)) s += (s.empty() ? "" : ",") + std::to_string(i);
    return s.empty() ? "-" : s;
}

static BYTE *CallTarget(BYTE *site) { return site + 5 + *(int *)(site + 1); }

static void SetSlot(void **slot, void *value) {
    DWORD old;
    VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old);
    *slot = value;
    VirtualProtect(slot, 4, old, &old);
}

// A stand-in for the renderer the engine's present works on: its swap chain at +0BCh, the vsync setting at +1BF1h.
// The swap chain only notes the sync interval it is given.
static UINT g_fakeSync = 99;
static HRESULT STDMETHODCALLTYPE FakePresent(void *self, UINT sync, UINT flags) {
    g_fakeSync = sync;
    return S_OK;
}
static void *g_fakeVtbl[16];
static void *g_fakeChain = g_fakeVtbl;  // the object: its first field is its vtable
static BYTE g_renderer[0x2000];

// Runs the engine's present on the stand-in; returns the sync interval the swap chain got (98: the present failed).
static UINT EnginePresent(BYTE *base, bool vsync) {
    g_fakeVtbl[8] = (void *)&FakePresent;  // IDXGISwapChain::Present
    *(void **)(g_renderer + 0xBC) = &g_fakeChain;
    g_renderer[0x1BF1] = vsync;
    g_fakeSync = 99;
    void *fn = base + kPresent;
    void *self = g_renderer;
    BYTE ok;
    __asm {
        push 0
        push 0
        mov ecx, self
        call fn
        mov ok, al
    }
    return ok ? g_fakeSync : 98;
}

// An overlay like Steam's: it hooks IDXGIFactory::CreateSwapChain and, on every new swap chain, puts its own hook into
// the Present slot unless it is there already, keeping what was there as the original it calls.
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainFn)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                      IDXGISwapChain **);
static CreateSwapChainFn g_overlayCreate;
static PresentFn g_overlayOrig;
static int g_overlayDepth, g_overlayMaxDepth;
static HRESULT STDMETHODCALLTYPE OverlayPresent(IDXGISwapChain *self, UINT sync, UINT flags) {
    if (g_overlayDepth >= 5) return E_FAIL;  // the calls go round in a circle: stop them before the stack runs out
    g_overlayMaxDepth = std::max(g_overlayMaxDepth, ++g_overlayDepth);
    HRESULT hr = g_overlayOrig(self, sync, flags);
    g_overlayDepth--;
    return hr;
}
static HRESULT STDMETHODCALLTYPE OverlayCreateSwapChain(IDXGIFactory *self, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc,
                                                        IDXGISwapChain **chain) {
    HRESULT hr = g_overlayCreate(self, dev, desc, chain);
    if (SUCCEEDED(hr) && chain && *chain) {
        void **slot = *(void ***)*chain + 8;
        if (*slot != (void *)&OverlayPresent) {
            g_overlayOrig = (PresentFn)*slot;
            SetSlot(slot, (void *)&OverlayPresent);
        }
    }
    return hr;
}

static IDXGISwapChain *MakeSwapChain(IDXGIFactory *factory, ID3D11Device *dev) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    sd.Windowed = TRUE;
    IDXGISwapChain *chain = nullptr;
    return factory && dev && SUCCEEDED(factory->CreateSwapChain(dev, &sd, &chain)) ? chain : nullptr;
}

#pragma warning(disable : 4717)  // recursion on all paths: running out of stack is the point
static __declspec(noinline) int Recurse(int n) {
    volatile char pad[256];
    pad[n & 255] = (char)n;
    return Recurse(n + 1) + pad[0];
}

// Settings as a player may write them: the ini (nullptr: none, version.dll writes its own), the settings line
// version.dll must log for it, and the "ini:" lines it must add (each one the start of a line, in order).
struct IniCase {
    const char *what;
    const char *text;
    std::string settings;
    std::vector<const char *> notes;
};
static const char *const kDefaults = "threads=free main_thread=pcores workers=any worker_count=0 pack_cache=1 "
                                     "texture_fast_path=1 loads_without_vsync=1 skip_intro_movies=0 frame_log=0 "
                                     "memory_log=1 crash_dumps=1";
static std::string With(std::string s, const char *key, const char *value) {
    size_t at = s.find(std::string(key) + "="), end = s.find(' ', at);
    return s.substr(0, at) + key + "=" + value + (end == std::string::npos ? "" : s.substr(end));
}
static const std::vector<IniCase> kIniCases = {
    {"mistakes of every kind",
     "; a player's edits\n[engine]\nthreads = Engine\nmain_thread=banana\nworkers=ECORES ; the slow cores\n"
     "worker_count=12abc\npack_cache=Off\ntexture_fast_path=true\nloads_without_vsync=-1\n"
     "skip_intro_movies=yes   # no movies\nframe_log=0x1\nmemory_log=\ncrash_dumps=abc\nskip_intro_movie=1\n",
     "threads=engine main_thread=pcores workers=ecores worker_count=0 pack_cache=0 texture_fast_path=1 "
     "loads_without_vsync=1 skip_intro_movies=1 frame_log=0 memory_log=1 crash_dumps=1",
     {"ini: skip_intro_movie is not a setting of this DLL (a typo?); ignored",
      "ini: main_thread=banana is not one of pcores, any; using pcores",
      "ini: worker_count=12abc is not a whole number; using 0",
      "ini: frame_log=0x1 is not understood as on/off (write 1 or 0; yes/no, true/false, on/off also work); using 0",
      "ini: memory_log= is not understood as on/off", "ini: crash_dumps=abc is not understood as on/off"}},
    {"worker_count=-1", "[engine]\nworker_count=-1\n", kDefaults, {"ini: worker_count=-1 is less than 0; using 0"}},
    {"worker_count=0", "[engine]\nworker_count=0\n", kDefaults, {}},
    {"worker_count=64", "[engine]\nworker_count=64\n", With(kDefaults, "worker_count", "64"), {}},
    {"worker_count=65", "[engine]\nworker_count=65\n", With(kDefaults, "worker_count", "64"),
     {"ini: worker_count=65 is more than 64; using 64"}},
    {"worker_count far beyond any number", "[engine]\nworker_count=99999999999999999999999\n",
     With(kDefaults, "worker_count", "64"), {"ini: worker_count=99999999999999999999999 is more than 64; using 64"}},
    {"on/off as other whole numbers", "[engine]\npack_cache=2\ntexture_fast_path=+1\nmemory_log=-1\ncrash_dumps=0\n",
     With(kDefaults, "crash_dumps", "0"), {}},
    {"keys missing", "[engine]\nthreads=free\n", kDefaults, {}},
    {"no [engine] section", "threads=engine\npack_cache=0\n", kDefaults,
     {"ini: shogun2_engine.ini has no [engine] section; every setting keeps its default"}},
    {"the ini version.dll writes itself", nullptr, kDefaults, {}},
    {"settings written more than once (Windows reads the first)",
     "[engine]\ncrash_dumps=0\nskip_intro_movie=1\npack_cache=1\nCrash_Dumps = 1 ; corrected at the end\n"
     "skip_intro_movie=0\nworker_count=2\nworker_count=3\nWORKER_COUNT=4\n",
     With(With(kDefaults, "worker_count", "2"), "crash_dumps", "0"),
     {"ini: crash_dumps appears twice; the first value (0) is used",
      "ini: skip_intro_movie is not a setting of this DLL (a typo?); ignored",
      "ini: worker_count appears 3 times; the first value (2) is used"}},
};

// The settings line (after "settings: ") and the "ini:" lines of shogun2_engine.log next to this program.
static void ReadSettingsLog(const std::string &dir, std::string &settings, std::vector<std::string> &notes) {
    FILE *lf = _fsopen((dir + "\\shogun2_engine.log").c_str(), "r", _SH_DENYNO);
    char line[1024];
    while (lf && fgets(line, sizeof line, lf)) {
        std::string s(line);
        s.erase(s.find_last_not_of("\r\n") + 1);
        size_t at = s.find(" settings: ");
        if (at != std::string::npos) settings = s.substr(at + 11);
        at = s.find(" ini: ");
        if (at != std::string::npos) notes.push_back(s.substr(at + 1));
    }
    if (lf) fclose(lf);
}

// --cyrillic-folder (found after the laptop beta of 0.4, where the game folder had none): this program and version.dll
// are copied into build\<"Igry" in Cyrillic letters> and run there with --paths-only. The log there must have the
// folder, in UTF-8, in its first line (the one with the version) and in the line of a file opened through the pack
// reopen; up to 0.4 both kept only their time.
static int CyrillicFolder(const char *input) {
    wchar_t exe[MAX_PATH], inputW[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, input, -1, inputW, MAX_PATH);
    std::wstring dir(exe, wcsrchr(exe, L'\\')), sub = dir + L"\\\u0418\u0433\u0440\u044b";
    const char *subUtf8 = "\xD0\x98\xD0\xB3\xD1\x80\xD1\x8B";
    CreateDirectoryW(sub.c_str(), nullptr);
    DeleteFileW((sub + L"\\shogun2_engine.log").c_str());
    DeleteFileW((sub + L"\\shogun2_engine.ini").c_str());  // version.dll writes its own there
    bool copied = CopyFileW(exe, (sub + L"\\hooks_test.exe").c_str(), FALSE) &&
                  CopyFileW((dir + L"\\version.dll").c_str(), (sub + L"\\version.dll").c_str(), FALSE);
    Check(copied, "this program and version.dll copied into a folder named with Cyrillic letters");
    std::wstring cmd = L"\"" + sub + L"\\hooks_test.exe\" \"" + inputW + L"\" - --paths-only";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    DWORD code = 99;
    if (copied && CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, sub.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Check(code == 0, "the copy loaded the engine there");
    printf("---- shogun2_engine.log there (UTF-8), first lines and \"open\" lines\n");
    FILE *lf = _wfsopen((sub + L"\\shogun2_engine.log").c_str(), L"r", _SH_DENYNO);
    char line[1024];
    int lines = 0, bare = 0;
    bool first = false, opened = false;
    while (lf && fgets(line, sizeof line, lf)) {
        std::string s(line);
        s.erase(s.find_last_not_of("\r\n") + 1);
        bool open = s.find(" open ") != std::string::npos;
        if (++lines <= 2 || open) printf("      %s\n", s.c_str());
        if (lines == 1) first = s.find("Shogun 2 engine fixes") != std::string::npos && s.find(subUtf8) != std::string::npos;
        opened |= open && s.find(subUtf8) != std::string::npos;
        bare += s.size() <= 13;  // "hh:mm:ss.mmm " and nothing after it
    }
    if (lf) fclose(lf);
    Check(first, "the first line names version.dll and the folder");
    Check(opened, "the line of a file opened through the pack reopen names the folder");
    Check(lines > 2 && bare == 0, "no line is left with only its time");
    Check(GetFileAttributesW((sub + L"\\shogun2_engine.ini").c_str()) != INVALID_FILE_ATTRIBUTES,
          "version.dll wrote its own ini there");
    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}

static void ShowLog(const std::string &dir, int maxLines) {
    printf("---- shogun2_engine.log\n");
    std::string log = dir + "\\shogun2_engine.log";
    FILE *lf = _fsopen(log.c_str(), "r", _SH_DENYNO);
    char line[1024];
    int shown = 0;
    while (lf && fgets(line, sizeof line, lf) && shown++ < maxLines) fputs(line, stdout);
    if (lf) fclose(lf);
}

int main(int argc, char **argv) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe, strrchr(exe, '\\'));
    if (argc == 2 && strcmp(argv[1], "--check-dump") == 0) {
        // after --stack-overflow: the log must name the overflow and a minidump that was written
        std::string log = dir + "\\shogun2_engine.log", dump;
        FILE *lf = _fsopen(log.c_str(), "r", _SH_DENYNO);
        char line[1024];
        bool named = false;
        printf("---- shogun2_engine.log\n");
        while (lf && fgets(line, sizeof line, lf)) {
            fputs(line, stdout);
            named |= strstr(line, "CRASH (unhandled): exception C00000FD") != nullptr;
            const char *d = strstr(line, "crash dump: "), *w = strstr(line, " written");
            if (d && w > d) dump.assign(d + 12, w);
        }
        if (lf) fclose(lf);
        Check(named, "the log names the stack overflow");
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        bool written = !dump.empty() && GetFileAttributesExA(dump.c_str(), GetFileExInfoStandard, &fa) &&
                       (fa.nFileSizeLow || fa.nFileSizeHigh);
        Check(written, "a minidump was written");
        if (written) {
            printf("      %s, %lu KB (deleted)\n", dump.c_str(), fa.nFileSizeLow / 1024);
            DeleteFileA(dump.c_str());
        }
        printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
        return g_failures ? 1 : 0;
    }
    if (argc < 3) {
        printf("usage: hooks_test <input folder> <a .pack file> [number_of_threads] [--crash | --unload | "
               "--other-build | --no-vsync-hook | --stack-overflow | --ini-case N | --cyrillic-folder]\n"
               "       hooks_test --check-dump\n");
        return 2;
    }
    int nthreads = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 0;
    bool crash = false, unload = false, otherBuild = false, noVsyncHook = false, stackOverflow = false;
    bool cyrillicFolder = false, pathsOnly = false;
    int iniCase = -1;
    for (int i = 3; i < argc; i++) {
        crash |= strcmp(argv[i], "--crash") == 0;
        unload |= strcmp(argv[i], "--unload") == 0;
        otherBuild |= strcmp(argv[i], "--other-build") == 0;
        noVsyncHook |= strcmp(argv[i], "--no-vsync-hook") == 0;
        stackOverflow |= strcmp(argv[i], "--stack-overflow") == 0;
        cyrillicFolder |= strcmp(argv[i], "--cyrillic-folder") == 0;
        pathsOnly |= strcmp(argv[i], "--paths-only") == 0;
        if (strcmp(argv[i], "--ini-case") == 0 && i + 1 < argc) iniCase = atoi(argv[++i]);
    }
    if (iniCase >= (int)kIniCases.size()) {
        printf("no ini case %d\n", iniCase);
        return 2;
    }
    if (cyrillicFolder) return CyrillicFolder(argv[1]);
    if (pathsOnly) {
        // the copy --cyrillic-folder runs: the engine with version.dll and the ini version.dll writes, then one file
        // (this program) opened through the pack reopen, for an "open" line with the folder in it
        SetDllDirectoryA(argv[1]);
        HMODULE m = LoadLibraryA((std::string(argv[1]) + "\\empire.retail.dll").c_str());
        if (!m) return 1;
        BYTE *site = (BYTE *)m + kPackReopenCall;
        if (site[0] == 0xE8) {
            auto open = (HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))CallTarget(site);
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            HANDLE h = open(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0x70000001, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        TerminateProcess(GetCurrentProcess(), 0);  // the engine is still loaded
    }

    // a test ini next to this program: everything on (texture_fast_path left out, as in older inis), dumps only with
    // --crash and --stack-overflow; with --ini-case the case's ini instead
    std::string ini = dir + "\\shogun2_engine.ini";
    if (iniCase >= 0) {
        DeleteFileA(ini.c_str());
        if (const char *text = kIniCases[iniCase].text) {
            FILE *f = fopen(ini.c_str(), "w");
            fputs(text, f);
            fclose(f);
        }
    } else {
        FILE *f = fopen(ini.c_str(), "w");
        fprintf(f, "[engine]\nthreads=free\nmain_thread=pcores\nworkers=any\nworker_count=0\npack_cache=1\n"
                   "skip_intro_movies=1\nframe_log=0\nmemory_log=1\ncrash_dumps=%d\nloads_without_vsync=%d\n",
                crash || stackOverflow ? 1 : 0, noVsyncHook ? 0 : 1);
        fclose(f);
    }

    SetDllDirectoryA(argv[1]);
    std::string engine = std::string(argv[1]) + "\\empire.retail.dll";
    if (otherBuild) {  // the same engine one second younger, in a folder of its own (its DLLs still come from the input folder)
        std::string copyDir = dir + "\\other_build";
        CreateDirectoryA(copyDir.c_str(), nullptr);
        std::string copy = copyDir + "\\empire.retail.dll";
        CopyFileA(engine.c_str(), copy.c_str(), FALSE);
        FILE *c = fopen(copy.c_str(), "r+b");
        LONG nt = 0;
        DWORD stamp = 0;
        if (c && fseek(c, 0x3C, SEEK_SET) == 0 && fread(&nt, 4, 1, c) == 1 && fseek(c, nt + 8, SEEK_SET) == 0 &&
            fread(&stamp, 4, 1, c) == 1) {
            stamp++;
            fseek(c, nt + 8, SEEK_SET);
            fwrite(&stamp, 4, 1, c);
        }
        if (c) fclose(c);
        printf("      engine copy with time stamp %08lX: %s\n", stamp, copy.c_str());
        engine = copy;
    }
    HMODULE m = LoadLibraryA(engine.c_str());
    if (!m) { printf("LoadLibrary(%s) failed: %lu\n", engine.c_str(), GetLastError()); return 1; }
    BYTE *base = (BYTE *)m;
    HMODULE proxy = GetModuleHandleA("version.dll");
    char proxyPath[MAX_PATH] = "";
    if (proxy) GetModuleFileNameA(proxy, proxyPath, MAX_PATH);
    Check(proxy && _strnicmp(proxyPath, dir.c_str(), dir.size()) == 0, "version.dll is the proxy next to this program");
    printf("      %s\n", proxyPath);
    auto inProxy = [&](const void *p) { return (BYTE *)p >= (BYTE *)proxy && (BYTE *)p < (BYTE *)proxy + 0x100000; };

    // through GetProcAddress: a static import would load version.dll before the engine, with nothing to patch
    auto sizeW = (DWORD(WINAPI *)(LPCWSTR, LPDWORD))GetProcAddress(proxy, "GetFileVersionInfoSizeW");
    DWORD size = sizeW ? sizeW(L"C:\\Windows\\SysWOW64\\kernel32.dll", nullptr) : 0;
    Check(size > 0, "GetFileVersionInfoSizeW is forwarded to the real version.dll");

    std::string settings;
    std::vector<std::string> notes;
    ReadSettingsLog(dir, settings, notes);
    if (iniCase >= 0) {
        const IniCase &c = kIniCases[iniCase];
        printf("      ini case %d: %s\n", iniCase, c.what);
        Check(settings == c.settings, "the settings line");
        if (settings != c.settings) printf("      got      %s\n      expected %s\n", settings.c_str(), c.settings.c_str());
        bool same = notes.size() == c.notes.size();
        for (size_t i = 0; same && i < notes.size(); i++) same = notes[i].compare(0, strlen(c.notes[i]), c.notes[i]) == 0;
        Check(same, c.notes.empty() ? "no \"ini:\" line" : "the \"ini:\" lines");
        for (const std::string &n : notes) printf("      %s\n", n.c_str());
        if (!c.text) Check(GetFileAttributesA(ini.c_str()) != INVALID_FILE_ATTRIBUTES, "version.dll wrote its own ini");
        printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0);  // the engine is still loaded
    }
    Check(notes.empty(), "no \"ini:\" line for the test ini");

    if (stackOverflow) {
        // as in the beta test of 2026-10-23: the main thread runs out of stack, and the filter left to run on it has
        // a few KB; version.dll must still log the crash and write a minidump (checked by the next run)
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        printf("      running the main thread out of stack\n");
        fflush(stdout);
        Recurse(0);
    }
    auto frameStart = (void(WINAPI *)(DWORD))GetProcAddress(proxy, MAKEINTRESOURCEA(101));
    if (otherBuild) {
        BYTE *slot = *(BYTE **)(base + kFrameSlot);
        BYTE *setup = base + kPoolSetupCall;
        Check(memcmp(base + kIntroMoviesJump, "\x0F\x84", 2) == 0, "intro movies: the je is still there");
        Check(slot >= base && slot < base + 0x2000000, "frame vtable slot still points into the engine");
        Check(setup[0] == 0xE8 && CallTarget(setup) >= base && CallTarget(setup) < base + 0x2000000,
              "pool setup call still goes into the engine");
        Check(memcmp(base + kPresentSync, kSyncCode, sizeof kSyncCode) == 0, "the engine's present is left as it is");
    }
    if (unload) {
        // as shogun2.exe does once the game is over: version.dll is the engine's import, nothing else holds it
        FreeLibrary(m);
        Check(GetModuleHandleA("empire.retail.dll") == nullptr, "the engine is unloaded");
        Check(GetModuleHandleA("version.dll") == proxy, "version.dll stays loaded");
        printf("      waiting 31 s: the statistics thread wakes up once\n");
        fflush(stdout);
        Sleep(31000);
        Check(true, "no crash meanwhile");
    }
    if (noVsyncHook) {
        Check(memcmp(base + kPresentSync, kSyncCode, sizeof kSyncCode) == 0, "the engine's present is left as it is");
        UINT second = 99;
        if (frameStart) {
            frameStart(200);
            EnginePresent(base, true);
            second = EnginePresent(base, true);
        }
        Check(second == 1, "a second present 200 ms into a frame keeps vsync");
    }
    if (unload || otherBuild || noVsyncHook) {
        ShowLog(dir, 1000);
        printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
        fflush(stdout);
        if (!unload) TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0);  // the engine is still loaded
        return g_failures ? 1 : 0;  // a normal end: version.dll logs how it was unloaded
    }

    Check(memcmp(base + kIntroMoviesJump, "\x90\xE9\xA3\x04\x00\x00", 6) == 0, "intro movies: je -> nop; jmp");
    BYTE *slot = *(BYTE **)(base + kFrameSlot);
    Check(slot >= (BYTE *)proxy && slot < (BYTE *)proxy + 0x100000, "frame vtable slot points into version.dll");

    // the engine's present asks version.dll for the sync interval; which presents keep vsync, as if frames of the
    // game had begun some time ago (ordinal 101)
    BYTE *sync = base + kPresentSync;
    Check(sync[0] == 0xE8 && inProxy(CallTarget(sync)) && memcmp(sync + 5, "\x90\x90\x90\x90\x90\x90", 6) == 0,
          "the engine's present gets its sync interval from version.dll");
    if (frameStart) {
        frameStart(200);
        UINT slowFrame = EnginePresent(base, true);
        UINT loading = EnginePresent(base, true);
        frameStart(5);
        EnginePresent(base, true);
        UINT shortFrame = EnginePresent(base, true);
        frameStart(200);
        UINT offFirst = EnginePresent(base, false);
        UINT offLoading = EnginePresent(base, false);
        Check(slowFrame == 1, "a frame 200 ms long presents with vsync (a hitch in battle)");
        Check(loading == 0, "its second present goes without vsync (the loading screen)");
        Check(shortFrame == 1, "a second present 5 ms into a frame keeps vsync");
        Check(offFirst == 0 && offLoading == 0, "with vsync off in the game no present waits for it");
    } else {
        Check(false, "the frame-start export is missing");
    }

    // An overlay in the process before the engine (Steam's is), then the swap chain the way the engine makes it:
    // CreateDXGIFactory1 through its GetProcAddress import, and the swap chain made again, as a switch between window
    // and full screen does. Up to 0.2 version.dll hooked Present in the same vtable slot: the overlay took that hook
    // for the original while version.dll's original was the overlay's hook, and the game crashed.
    wchar_t d3d11[MAX_PATH];
    GetSystemDirectoryW(d3d11, MAX_PATH);
    wcscat_s(d3d11, L"\\d3d11.dll");
    auto createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(LoadLibraryW(d3d11), "D3D11CreateDevice");
    ID3D11Device *dev = nullptr;
    if (createDevice)
        createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto plainCreate = (HRESULT(WINAPI *)(REFIID, void **))GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory1 *plain = nullptr;
    if (plainCreate) plainCreate(__uuidof(IDXGIFactory1), (void **)&plain);
    void **createSlot = plain ? *(void ***)plain + 10 : nullptr;  // IDXGIFactory::CreateSwapChain, shared by all
    if (createSlot) {
        g_overlayCreate = (CreateSwapChainFn)*createSlot;
        SetSlot(createSlot, (void *)&OverlayCreateSwapChain);
    }
    auto gpa = *(FARPROC(WINAPI **)(HMODULE, LPCSTR))(base + kIatGetProcAddress);
    Check((void *)gpa == (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress"),
          "the engine's GetProcAddress import is left as it is");
    auto createFactory = (HRESULT(WINAPI *)(REFIID, void **))gpa(dxgi, "CreateDXGIFactory1");
    IDXGIFactory1 *factory = nullptr;
    if (createFactory) createFactory(__uuidof(IDXGIFactory1), (void **)&factory);
    IDXGISwapChain *first = MakeSwapChain(factory, dev);
    IDXGISwapChain *second = MakeSwapChain(factory, dev);
    g_overlayMaxDepth = 0;
    HRESULT hr = second ? second->Present(0, 0) : E_FAIL;
    printf("      overlay hook entered %d time(s) in one present\n", g_overlayMaxDepth);
    Check(first && second && SUCCEEDED(hr) && g_overlayMaxDepth == 1,
          "an overlay that hooks Present on every new swap chain, two swap chains: one call through each hook");
    if (second) SetSlot(*(void ***)second + 8, (void *)g_overlayOrig);
    if (createSlot) SetSlot(createSlot, (void *)g_overlayCreate);
    if (first) first->Release();
    if (second) second->Release();
    if (factory) factory->Release();
    if (plain) plain->Release();
    if (dev) dev->Release();

    // the pack reopen: flags 0x70000001 as the engine passes them
    BYTE *site = base + kPackReopenCall;
    Check(site[0] == 0xE8 && site[5] == 0x90, "pack reopen call is redirected");
    auto open = (HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))CallTarget(site);
    wchar_t pack[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, pack, MAX_PATH);
    HANDLE h = open(pack, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0x70000001, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "pack opened through the hook");
    if (h != INVALID_HANDLE_VALUE) {
        auto q = (NtQueryInformationFile_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
        ULONG_PTR iosb[2];
        ULONG mode = 0;
        LONG st = q(h, iosb, &mode, sizeof mode, 16 /* FileModeInformation */);
        Check(st == 0 && !(mode & 0x8 /* FILE_NO_INTERMEDIATE_BUFFERING */), "the handle is buffered (file cache on)");
        // an aligned overlapped read, as the I/O thread does it
        void *buf = VirtualAlloc(nullptr, 65536, MEM_COMMIT, PAGE_READWRITE);
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf, 65536, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING;
        ok = ok && GetOverlappedResult(h, &ov, &got, TRUE);
        Check(ok && got > 0, "an aligned overlapped read works");
        CloseHandle(ov.hEvent);
        VirtualFree(buf, 0, MEM_RELEASE);
        CloseHandle(h);
    }

    // the pool setup exactly as the engine calls it at start: singleton, (number_of_threads, pin = 1)
    BYTE *setupSite = base + kPoolSetupCall;
    Check(setupSite[0] == 0xE8 && CallTarget(setupSite) >= (BYTE *)proxy, "pool setup call is redirected");
    void *obj = ((void *(__cdecl *)())(base + kSingleton))();
    void *hook = CallTarget(setupSite);
    int n = nthreads;
    __asm {
        push 1
        push n
        mov ecx, obj
        call hook
    }
    Sleep(300);
    ULONG_PTR all = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &all, &sys);
    ULONG_PTR mainMask = Affinity(GetCurrentThread());
    ULONG ids[64], count = 0;
    GetThreadSelectedCpuSets(GetCurrentThread(), ids, 64, &count);
    printf("      main thread: affinity %s, %lu selected CPU sets\n", Lps(mainMask).c_str(), count);
    Check(mainMask == all, "main thread is not pinned");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    int threads = 0, pinned = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE t = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        ULONG_PTR mask = Affinity(t);
        threads++;
        if (mask != all) {
            pinned++;
            printf("      thread %lu pinned to %s\n", te.th32ThreadID, Lps(mask).c_str());
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    printf("      %d threads in the process\n", threads);
    Check(pinned == 0, "no thread is pinned");

    if (crash) {
        try {
            throw std::bad_alloc();
        } catch (const std::bad_alloc &) {
        }
        wchar_t dumps[MAX_PATH];
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dumps);
        wcscat_s(dumps, L"\\The Creative Assembly\\Shogun2\\crash_dumps\\shogun2_*.mdmp");
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(dumps, &fd);
        std::wstring newest;
        if (find != INVALID_HANDLE_VALUE) {
            do newest = std::max(newest, std::wstring(fd.cFileName));
            while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        Check(!newest.empty(), "a bad_alloc leaves a minidump");
        if (!newest.empty()) {
            std::wstring path(dumps);
            path = path.substr(0, path.rfind(L'\\') + 1) + newest;
            printf("      %ls (deleted)\n", path.c_str());
            DeleteFileW(path.c_str());
        }
    }

    fflush(stdout);
    ShowLog(dir, 60);
    printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0);  // skip the engine's static destructors
    return 0;
}
