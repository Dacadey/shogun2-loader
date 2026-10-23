// version.dll proxy for Total War: SHOGUN 2 (empire.retail.dll 1.1.0, build 2023-05-12): engine fixes for modern PCs.
//
// empire.retail.dll imports version.dll, so a DLL of that name in the game folder is loaded with the engine, on the
// main thread, before any engine code runs. The real version.dll is loaded from the system folder and every export is
// forwarded to it. The patches only go into the one build of empire.retail.dll they were made for (its time stamp and
// image size); with any other build the DLL logs that and only forwards. Each patch also checks that the engine holds
// exactly the expected code before changing it. In any other process (the game folder also holds
// AwesomiumProcess.exe, cef_process.exe and chromiumcapsule.retail.exe) the DLL only forwards.
//
// Settings (shogun2_engine.ini next to shogun2.exe, written with defaults on first run), section [engine]:
//   threads=free          free: neither the main thread nor the worker pool is pinned to a core, so Windows can put
//                         them on whichever core is free and fastest. engine: as the game does it (main thread pinned
//                         to core 0, one pool thread pinned to each other core, E-cores included).
//   main_thread=pcores    with threads=free: pcores makes the main thread (game logic and rendering) prefer the
//                         performance cores of a hybrid CPU; any lets Windows decide.
//   workers=any           with threads=free: which cores the pool threads prefer: any, pcores or ecores.
//   worker_count=0        threads in the pool, the main thread included; 0 keeps the game's choice (number_of_threads
//                         in preferences.script.txt, or one per physical core when that is 0).
//   pack_cache=1          1 reads the .pack files through the Windows file cache; the game opens them with
//                         FILE_FLAG_NO_BUFFERING, so every load reads the disk again. 0: as the game does it.
//   texture_fast_path=1   1 creates a texture straight from its .dds data when the file already holds the format the
//                         game asks for; D3DX re-encodes every such texture instead (most of a battle's load time,
//                         measured 2026-10-22). 0: everything through D3DX, as the game does it.
//   loads_without_vsync=1 1 draws the loading screen without waiting for vsync, so that with vsync on the loads are no
//                         slower than without it (measured 2026-10-22: 1.3-3.4 s per load); the game itself keeps
//                         vsync. 0: as the game does it (its present is left as it is).
//   skip_intro_movies=0   1 goes straight to the main menu (no INTRO, Intel and SEGA movies).
//   frame_log=0           1 writes shogun2_frames.csv: one line per frame with its time and the CPU the main thread
//                         was on (about 15 MB an hour at 240 fps; written anew at every start). A summary goes to the
//                         log either way.
//   memory_log=1          1 logs the 32-bit address space (used, free, largest free block) every 30 s and at exit.
//   crash_dumps=1         1 writes a minidump and a log line when the game crashes or runs out of memory, to
//                         %APPDATA%\The Creative Assembly\Shogun2\crash_dumps (the 10 newest are kept).
// Values: the on/off settings take 1 or 0, also yes/no, true/false and on/off in any case; any other whole number but 0
// is on, as older versions read it. Text after ';' or '#' is a comment. A missing key keeps its default without a
// word; a value the DLL does not understand keeps it too, with a line starting with "ini:" under the settings line in
// the log, as do keys that are no setting (typos), a setting written more than once (the first one counts, as Windows
// reads it) and an ini without [engine].
// Log: shogun2_engine.log next to shogun2.exe, in UTF-8.
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace {

const char kVersion[] = "0.5";  // in the first line of the log; the package's README carries the same number

// ---------------------------------------------------------------- empire.retail.dll 1.1.0 (RVAs, base 0x10000000)
const DWORD kPoolSetupCall = 0x9D485;     // engine start: call <pool setup> (thiscall (number_of_threads, pin), ret 8)
const DWORD kPoolSetup = 0x12C00D0;       // builds one affinity mask per pool thread, starts TBB, pins every thread
const DWORD kPoolCount = 0x10B8;          // pool setup object: number of masks = threads in the pool
const DWORD kPoolMasks = 0x10BC;          // pool setup object: the masks
const DWORD kPinCall = 0x12A6917;         // call [SetThreadAffinityMask] in the task each pool thread runs once
const DWORD kPackReopenCall = 0x12E150F;  // call [CreateFileW] reopening a .pack for async unbuffered reads
const DWORD kFileOpenCall = 0x1284FED;    // call [CreateFileW] opening a file for async unbuffered sequential reads
const DWORD kIatCreateFileW = 0x15A1144;
const DWORD kIatSetThreadAffinityMask = 0x15A121C;
const DWORD kIatSetUnhandledExceptionFilter = 0x15A10F4;
const DWORD kPresentSync = 0x148719A;     // in the DX11 renderer's present (11487180): the sync interval it passes
const DWORD kRendererVsync = 0x1BF1;      // renderer field (not an RVA): the game's vsync setting, a bool
const DWORD kIntroMoviesJump = 0xE81DA6;  // "je" over the startup movie list (INTRO, INTEL_LOGO, adverts, SEGA)
const DWORD kFrameSlot = 0x17547F8;       // vtable slot of the per-frame method, called once a frame by the main loop
const DWORD kFrameFunc = 0x13E4480;
const DWORD kCreateTextureCall = 0x14EE344;  // the engine's only call of D3DX11CreateTextureFromMemory (DX11 mode)
const DWORD kCreateTextureStub = 0x14FBBFF;  // jmp [import] stub it calls
const DWORD kTimestamp = 0x645E92BB;         // the build all of the above belong to: file header time stamp
const DWORD kImageSize = 0x1D88000;          // and size of the loaded image
static_assert(std::max({kPoolSetupCall, kPoolSetup, kPinCall, kPackReopenCall, kFileOpenCall, kIatCreateFileW,
                        kIatSetThreadAffinityMask, kIatSetUnhandledExceptionFilter, kPresentSync, kIntroMoviesJump,
                        kFrameSlot, kFrameFunc, kCreateTextureCall, kCreateTextureStub}) + 16 <=
                  kImageSize,
              "every place the patches read lies inside the image");

HMODULE g_real;
FARPROC g_fn[15];
const char* const kNames[15] = {"GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
                                "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
                                "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
                                "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
                                "VerInstallFileW",         "VerQueryValueA",             "VerQueryValueW"};

FILE* g_log;
BYTE* g_base;
wchar_t g_dir[MAX_PATH];
DWORD g_mainThread;
bool g_threadsFree = true, g_mainPcores = true, g_packCache = true, g_skipIntro = false, g_frameLog = false;
bool g_memoryLog = true, g_crashDumps = true, g_loadsNoVsync = true;
int g_workers = 0;  // 0 any, 1 pcores, 2 ecores
int g_workerCount = 0;
int g_textureFast = 1;

bool g_ending;  // the process is ending: its other threads are gone, perhaps in the middle of writing a log line
DWORD g_crashWriter;  // the thread that writes a crash for a thread out of stack, which may hold the log's lock

// The log is UTF-8: a wide string that may hold more than English letters (a path, what a player wrote) goes into it
// through Utf8() and %s. With %ls the C runtime converts it in the "C" locale, which gives up at the first Cyrillic
// letter, and the line keeps only its time: up to 0.4 a game folder with Cyrillic letters left the first line,
// version included, empty (found after the laptop beta of 0.4).
std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2048];
    int n = sprintf_s(line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    int m = _vsnprintf_s(line + n, sizeof line - n - 1, _TRUNCATE, fmt, a);
    va_end(a);
    n = m < 0 ? (int)strlen(line) : n + m;
    line[n++] = '\n';
    if (g_ending || GetCurrentThreadId() == g_crashWriter) {  // a thread killed or crashed inside fwrite holds its lock
        _fwrite_nolock(line, 1, n, g_log);
        _fflush_nolock(g_log);
    } else {
        fwrite(line, 1, n, g_log);
        fflush(g_log);
    }
}

void LoadReal() {
    if (g_real) return;
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit process
    wcscat_s(path, L"\\version.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) {
        DWORD error = GetLastError();
        Log("could not load %s (error %lu)", Utf8(path).c_str(), error);
        return;
    }
    for (int i = 0; i < 15; ++i) g_fn[i] = GetProcAddress(g_real, kNames[i]);
}

// ---------------------------------------------------------------- settings

// Players edit the ini by hand. GetPrivateProfileInt read "true" or "yes" as 0, so texture_fast_path=true silently
// turned the main fix off (player test of 0.3). What is not understood now keeps its default, and the log says so in
// lines starting with "ini:" right under the settings line.
const wchar_t* const kSettings[] = {L"threads",           L"main_thread",         L"workers",
                                    L"worker_count",      L"pack_cache",          L"texture_fast_path",
                                    L"loads_without_vsync", L"skip_intro_movies", L"frame_log",
                                    L"memory_log",        L"crash_dumps"};
std::vector<std::string> g_iniNotes;

void IniNote(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, a);
    va_end(a);
    g_iniNotes.push_back(line);
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

// The value of a key in [engine] without its comment (from ';' or '#') and the blanks around it; false when the key
// is not there.
bool IniValue(const wchar_t* ini, const wchar_t* key, std::wstring& out) {
    wchar_t v[256];
    GetPrivateProfileStringW(L"engine", key, L"\x1", v, 256, ini);
    if (v[0] == 1 && v[1] == 0) return false;
    out = v;
    size_t comment = out.find_first_of(L";#");
    if (comment != std::wstring::npos) out.resize(comment);
    out = Trim(out);
    return true;
}

// A whole decimal number with an optional sign ("12abc" and "1.5" are not); far beyond any setting it stops growing
// instead of wrapping around to 0 or below.
bool ParseInt(const std::wstring& s, long long& out) {
    size_t i = !s.empty() && (s[0] == L'+' || s[0] == L'-') ? 1 : 0;
    if (i == s.size()) return false;
    long long v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
        if (v < 1000000000000LL) v = v * 10 + (s[i] - L'0');
    }
    out = s[0] == L'-' ? -v : v;
    return true;
}

// On/off: 1 or 0, yes/no, true/false, on/off in any case; any other whole number is on unless it is 0, as older
// versions read it.
bool ReadOnOff(const wchar_t* ini, const wchar_t* key, bool def) {
    std::wstring v;
    if (!IniValue(ini, key, v)) return def;
    for (const wchar_t* w : {L"yes", L"true", L"on"})
        if (_wcsicmp(v.c_str(), w) == 0) return true;
    for (const wchar_t* w : {L"no", L"false", L"off"})
        if (_wcsicmp(v.c_str(), w) == 0) return false;
    long long n;
    if (ParseInt(v, n)) return n != 0;
    IniNote("ini: %ls=%s is not understood as on/off (write 1 or 0; yes/no, true/false, on/off also work); using %d",
            key, Utf8(v).c_str(), (int)def);
    return def;
}

// One of the words, in any case; the first one is the default. Returns its position in the list.
int ReadWord(const wchar_t* ini, const wchar_t* key, std::initializer_list<const wchar_t*> words) {
    std::wstring v, list;
    if (!IniValue(ini, key, v)) return 0;
    int i = 0;
    for (const wchar_t* w : words) {
        if (_wcsicmp(v.c_str(), w) == 0) return i;
        ++i;
        list += (list.empty() ? L"" : L", ") + std::wstring(w);
    }
    IniNote("ini: %ls=%s is not one of %s; using %ls", key, Utf8(v).c_str(), Utf8(list).c_str(), *words.begin());
    return 0;
}

// A whole number from 0 to max; 0 when it is not one.
int ReadCount(const wchar_t* ini, const wchar_t* key, int max) {
    std::wstring v;
    long long n;
    if (!IniValue(ini, key, v)) return 0;
    if (!ParseInt(v, n)) {
        IniNote("ini: %ls=%s is not a whole number; using 0", key, Utf8(v).c_str());
        return 0;
    }
    if (n > max) {
        IniNote("ini: %ls=%s is more than %d; using %d", key, Utf8(v).c_str(), max, max);
        return max;
    }
    if (n < 0) {
        IniNote("ini: %ls=%s is less than 0; using 0", key, Utf8(v).c_str());
        return 0;
    }
    return (int)n;
}

// Keys in [engine] that are no setting (a typo leaves the setting meant at its default), a setting written more than
// once (Windows reads the first one, so a line added at the end to correct it does nothing; laptop beta of 0.4), and
// an ini without [engine] (with nothing to read, every setting keeps its default).
void CheckIniKeys(const wchar_t* ini) {
    std::vector<wchar_t> buf(32768);
    GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), ini);
    bool found = false;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) found |= _wcsicmp(p, L"engine") == 0;
    if (!found) {
        IniNote("ini: shogun2_engine.ini has no [engine] section; every setting keeps its default");
        return;
    }
    GetPrivateProfileSectionW(L"engine", buf.data(), (DWORD)buf.size(), ini);
    std::vector<std::pair<std::wstring, int>> keys;  // as first written, in the file's order, and how often
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring line = Trim(p);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        std::wstring key = Trim(line.substr(0, line.find(L'=')));
        auto same = std::find_if(keys.begin(), keys.end(),
                                 [&](const auto& k) { return _wcsicmp(k.first.c_str(), key.c_str()) == 0; });
        if (same != keys.end())
            same->second++;
        else
            keys.push_back({key, 1});
    }
    for (const auto& k : keys) {
        bool known = false;
        for (const wchar_t* s : kSettings) known |= _wcsicmp(k.first.c_str(), s) == 0;
        std::wstring first;
        if (!known)
            IniNote("ini: %s is not a setting of this DLL (a typo?); ignored", Utf8(k.first).c_str());
        else if (k.second > 1 && IniValue(ini, k.first.c_str(), first))
            IniNote("ini: %s appears %s; the first value (%s) is used", Utf8(k.first).c_str(),
                    k.second == 2 ? "twice" : (std::to_string(k.second) + " times").c_str(), Utf8(first).c_str());
    }
}

void ReadSettings() {
    wchar_t ini[MAX_PATH];
    swprintf_s(ini, L"%ls\\shogun2_engine.ini", g_dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini, L"w") == 0 && f) {
            fputs("; Shogun 2 engine fixes (version.dll). Delete version.dll to turn them all off.\n"
                  "; Values: 1 or 0 for on/off (yes/no, true/false and on/off also work). A value the DLL does not\n"
                  ";        understand keeps its default; shogun2_engine.log then has a line starting with \"ini:\".\n"
                  "; threads: free lets Windows place the main thread and the worker pool on any core;\n"
                  ";        engine pins them as the game does (main thread on core 0, one pool thread per core).\n"
                  "; main_thread: pcores makes the main thread prefer the performance cores (hybrid CPUs); any.\n"
                  "; workers: the cores the pool threads prefer: any, pcores or ecores.\n"
                  "; worker_count: threads in the pool, main thread included; 0 keeps the game's choice.\n"
                  "; pack_cache: 1 reads the .pack files through the Windows file cache (repeated loads come from RAM).\n"
                  "; texture_fast_path: 1 makes textures straight from the file when it already holds the format the\n"
                  ";        game asks for, instead of D3DX re-encoding every one (most of a battle's load time); 0 off.\n"
                  "; loads_without_vsync: 1 draws the loading screen without waiting for vsync. With vsync on that saves\n"
                  ";        1.5-3.5 s per load at 60-120 Hz and almost nothing at 240 Hz; the game itself keeps vsync.\n"
                  ";        0: as the game does it.\n"
                  "; skip_intro_movies: 1 goes straight to the main menu.\n"
                  "; frame_log: 1 writes shogun2_frames.csv (frame times and the main thread's CPU), for diagnostics:\n"
                  ";        about 15 MB an hour at 240 fps; the file is written anew at every start.\n"
                  "; memory_log: 1 logs the 32-bit address space every 30 s.\n"
                  "; crash_dumps: 1 writes a minidump to %APPDATA%\\The Creative Assembly\\Shogun2\\crash_dumps on a crash.\n"
                  "[engine]\nthreads=free\nmain_thread=pcores\nworkers=any\nworker_count=0\npack_cache=1\n"
                  "texture_fast_path=1\nloads_without_vsync=1\n"
                  "skip_intro_movies=0\nframe_log=0\nmemory_log=1\ncrash_dumps=1\n",
                  f);
            fclose(f);
        }
    }
    CheckIniKeys(ini);
    g_threadsFree = ReadWord(ini, L"threads", {L"free", L"engine"}) == 0;
    g_mainPcores = ReadWord(ini, L"main_thread", {L"pcores", L"any"}) == 0;
    g_workers = ReadWord(ini, L"workers", {L"any", L"pcores", L"ecores"});
    g_workerCount = ReadCount(ini, L"worker_count", 64);
    g_packCache = ReadOnOff(ini, L"pack_cache", true);
    g_skipIntro = ReadOnOff(ini, L"skip_intro_movies", false);
    g_frameLog = ReadOnOff(ini, L"frame_log", false);
    g_memoryLog = ReadOnOff(ini, L"memory_log", true);
    g_crashDumps = ReadOnOff(ini, L"crash_dumps", true);
    g_textureFast = ReadOnOff(ini, L"texture_fast_path", true);  // also on in older inis
    g_loadsNoVsync = ReadOnOff(ini, L"loads_without_vsync", true);
    static const char* const kWorkers[3] = {"any", "pcores", "ecores"};
    Log("settings: threads=%s main_thread=%s workers=%s worker_count=%d pack_cache=%d texture_fast_path=%d "
        "loads_without_vsync=%d skip_intro_movies=%d frame_log=%d memory_log=%d crash_dumps=%d",
        g_threadsFree ? "free" : "engine", g_mainPcores ? "pcores" : "any", kWorkers[g_workers], g_workerCount,
        (int)g_packCache, g_textureFast, (int)g_loadsNoVsync, (int)g_skipIntro, (int)g_frameLog, (int)g_memoryLog,
        (int)g_crashDumps);
    for (const std::string& note : g_iniNotes) Log("%s", note.c_str());
}

std::string MaskString(DWORD_PTR m) {
    std::string s;
    for (int i = 0; i < 32; ++i)
        if (m & ((DWORD_PTR)1 << i)) s += (s.empty() ? "" : ",") + std::to_string(i);
    return s.empty() ? "-" : s;
}

// ---------------------------------------------------------------- CPU topology (performance cores)

std::vector<ULONG> g_pcoreSets, g_ecoreSets;  // CPU set ids (both empty when all cores are alike)
DWORD g_pcoreMaskLow;                        // performance-core logical processors in group 0
bool g_isPcore[64];
DWORD_PTR g_processMask = 1;

void FindPcores() {
    DWORD_PTR sys;
    GetProcessAffinityMask(GetCurrentProcess(), &g_processMask, &sys);
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<BYTE> buf(len);
    if (!len || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0))
        return;
    BYTE maxClass = 0, minClass = 255;
    for (ULONG off = 0; off < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + off);
        if (s->Type == CpuSetInformation) {
            maxClass = std::max(maxClass, s->CpuSet.EfficiencyClass);
            minClass = std::min(minClass, s->CpuSet.EfficiencyClass);
        }
        off += s->Size;
    }
    int lps = 0;
    for (ULONG off = 0; off < len;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + off);
        if (s->Type == CpuSetInformation) {
            ++lps;
            if (maxClass != minClass && s->CpuSet.EfficiencyClass == maxClass) {
                g_pcoreSets.push_back(s->CpuSet.Id);
                if (s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 32)
                    g_pcoreMaskLow |= 1u << s->CpuSet.LogicalProcessorIndex;
                if (s->CpuSet.LogicalProcessorIndex < 64) g_isPcore[s->CpuSet.LogicalProcessorIndex] = true;
            } else if (maxClass != minClass) {
                g_ecoreSets.push_back(s->CpuSet.Id);
            }
        }
        off += s->Size;
    }
    if (g_pcoreSets.empty())
        Log("CPU: %d logical processors, all of one kind; process mask %s", lps, MaskString(g_processMask).c_str());
    else
        Log("CPU: %d logical processors, performance cores on %s", lps, MaskString(g_pcoreMaskLow).c_str());
}

// Unpins the calling thread (back to the whole process mask) and gives it the CPU-set preference of its role.
DWORD_PTR FreeThread(HANDLE thread, bool main, DWORD_PTR (WINAPI* setAffinity)(HANDLE, DWORD_PTR)) {
    DWORD_PTR prev = setAffinity(thread, g_processMask);
    const std::vector<ULONG>* sets = nullptr;
    if (main ? g_mainPcores : g_workers == 1) sets = &g_pcoreSets;
    if (!main && g_workers == 2) sets = &g_ecoreSets;
    if (sets && !sets->empty()) SetThreadSelectedCpuSets(thread, sets->data(), (ULONG)sets->size());
    return prev ? prev : g_processMask;
}

// ---------------------------------------------------------------- code patching helpers

bool WriteCode(BYTE* at, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

bool CallsTo(BYTE* site, DWORD target) { return site[0] == 0xE8 && site + 5 + *(int*)(site + 1) == g_base + target; }

bool RedirectCall(BYTE* site, void* to) {
    int rel = (int)((BYTE*)to - (site + 5));
    return WriteCode(site + 1, &rel, 4);
}

// "call dword ptr [iat]" (FF 15 <address of the import slot>, relocated with the module)
bool CallsImport(BYTE* site, DWORD iat) { return site[0] == 0xFF && site[1] == 0x15 && *(BYTE**)(site + 2) == g_base + iat; }

// Turns "call dword ptr [iat]" (6 bytes) into "call hook; nop". The hook must have the import's signature.
bool RedirectImportCall(BYTE* site, void* to) {
    BYTE code[6] = {0xE8, 0, 0, 0, 0, 0x90};
    *(int*)(code + 1) = (int)((BYTE*)to - (site + 5));
    return WriteCode(site, code, 6);
}

template <class T>
T Import(DWORD iat) { return *(T*)(g_base + iat); }  // whatever the import slot holds now (other mods may hook it)

// ---------------------------------------------------------------- 1. threads

// At start the engine (1) reads the CPU topology with Intel's 2009 cpu_topo.c, which pins the calling thread to each
// logical processor in turn and leaves it on the last one (an E-core on Intel hybrids), then (2) builds one affinity
// mask per pool thread: one per physical core when number_of_threads is 0, else logical processor i for thread i, and
// (3) starts TBB with that many threads and has every pool thread, the main thread included, pin itself to its mask.
// Measured with the engine's own code on an i9-13900KF (tools\probe\topo_probe.exe): the main thread ends up pinned
// to core 0, and 23 workers to the other 23 cores, 16 of them E-cores. With number_of_threads 4 the main thread is
// pinned to logical processor 0 and a worker to its Hyper-Threading sibling.
typedef void(__fastcall* PoolSetupFn)(void* self, void* edx, int nthreads, int pin);
PoolSetupFn g_origPoolSetup;
struct Pin {
    DWORD thread;
    DWORD_PTR mask;
};
Pin g_pins[64];
volatile LONG g_pinCount;

DWORD_PTR WINAPI HookPin(HANDLE thread, DWORD_PTR mask) {
    auto setAffinity = Import<DWORD_PTR(WINAPI*)(HANDLE, DWORD_PTR)>(kIatSetThreadAffinityMask);
    LONG i = InterlockedIncrement(&g_pinCount) - 1;
    if (i < 64) g_pins[i] = {GetCurrentThreadId(), mask};  // the engine passes GetCurrentThread()
    if (!g_threadsFree) return setAffinity(thread, mask);
    return FreeThread(thread, GetCurrentThreadId() == g_mainThread, setAffinity);
}

void __fastcall HookPoolSetup(void* self, void* edx, int nthreads, int pin) {
    int n = g_workerCount > 0 ? g_workerCount : nthreads;
    g_pinCount = 0;
    g_origPoolSetup(self, edx, n, pin);
    DWORD count = *(DWORD*)((BYTE*)self + kPoolCount);
    DWORD_PTR* masks = *(DWORD_PTR**)((BYTE*)self + kPoolMasks);
    Log("threads: number_of_threads %d%s -> %lu pool threads (main thread included)", nthreads,
        n != nthreads ? " (worker_count overrides it)" : "", count);
    std::string wanted;
    for (DWORD i = 0; i < count && i < 64; ++i) wanted += (i ? " " : "") + MaskString(masks[i]);
    Log("threads: masks the engine wanted: %s", wanted.c_str());
    std::string mainMask;
    LONG pins = g_pinCount < 64 ? g_pinCount : 64;
    for (LONG i = 0; i < pins; ++i)
        if (g_pins[i].thread == g_mainThread) mainMask = MaskString(g_pins[i].mask);
    Log("threads: %ld pool threads pinned themselves; the engine wanted the main thread on %s", g_pinCount,
        mainMask.empty() ? "(it took no pinning task)" : mainMask.c_str());
    if (g_threadsFree) {
        // also undoes cpu_topo.c's leftover pin when the main thread took no pinning task
        FreeThread(GetCurrentThread(), GetCurrentThreadId() == g_mainThread,
                   Import<DWORD_PTR(WINAPI*)(HANDLE, DWORD_PTR)>(kIatSetThreadAffinityMask));
        Log("threads: free (main thread %s, pool threads %s)",
            g_mainPcores && !g_pcoreSets.empty() ? "prefers the performance cores" : "on any core",
            g_workers == 1 && !g_pcoreSets.empty()   ? "prefer the performance cores"
            : g_workers == 2 && !g_ecoreSets.empty() ? "prefer the efficiency cores"
                                                     : "on any core");
    }
}

void InstallThreads() {
    static const BYTE before[] = {0x6A, 0x01, 0x56, 0x8B, 0xC8};  // push 1; push esi; mov ecx, eax
    BYTE* setup = g_base + kPoolSetupCall;
    BYTE* pin = g_base + kPinCall;
    if (!CallsTo(setup, kPoolSetup) || memcmp(setup - 5, before, 5) != 0 || !CallsImport(pin, kIatSetThreadAffinityMask)) {
        Log("threads: not the supported build of empire.retail.dll; left alone");
        return;
    }
    g_origPoolSetup = (PoolSetupFn)(g_base + kPoolSetup);
    bool ok = RedirectCall(setup, &HookPoolSetup) && RedirectImportCall(pin, &HookPin);
    Log("threads: %s", !ok ? "patching failed" : g_threadsFree ? "pool and main thread will be left unpinned"
                                                               : "as the engine does it (logged only)");
}

// ---------------------------------------------------------------- 2. pack files through the file cache

// The pack reader opens every .pack once to read its index, then reopens it with
// FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS (0x70000001) for the I/O thread, which reads
// it with ReadFileEx into sector-aligned buffers. Another reader opens files with OVERLAPPED | NO_BUFFERING |
// SEQUENTIAL_SCAN (0x68000001). Without NO_BUFFERING the same aligned requests are valid, and Windows keeps what was
// read in RAM, so a second battle or a return to the campaign map reads from memory instead of the disk.
volatile LONG g_opens, g_unbuffered;

HANDLE WINAPI HookOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                       DWORD flags, HANDLE templ) {
    auto createFile = Import<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)>(
        kIatCreateFileW);
    DWORD f = flags;
    if (f & FILE_FLAG_NO_BUFFERING) {
        InterlockedIncrement(&g_unbuffered);
        if (g_packCache) f &= ~FILE_FLAG_NO_BUFFERING;
    }
    HANDLE h = createFile(path, access, share, sa, disposition, f, templ);
    LONG n = InterlockedIncrement(&g_opens);
    if (n <= 400)
        Log("open %08lX -> %08lX %s: %s", flags, f, h == INVALID_HANDLE_VALUE ? "failed" : "ok", Utf8(path).c_str());
    if (n == 400) Log("open: further opens are not logged");
    return h;
}

void InstallPackCache() {
    BYTE* a = g_base + kPackReopenCall;
    BYTE* b = g_base + kFileOpenCall;
    // the flags pushed before each call: push 70000001h / push 68000001h
    if (!CallsImport(a, kIatCreateFileW) || !CallsImport(b, kIatCreateFileW) || *(DWORD*)(a - 0x18) != 0x70000001 ||
        *(DWORD*)(b - 0x19) != 0x68000001) {
        Log("pack_cache: not the supported build of empire.retail.dll; left alone");
        return;
    }
    bool ok = RedirectImportCall(a, &HookOpen) && RedirectImportCall(b, &HookOpen);
    Log("pack_cache: %s", !ok ? "patching failed" : g_packCache ? "on (no FILE_FLAG_NO_BUFFERING)" : "off (opens logged only)");
}

// ---------------------------------------------------------------- 3. intro movies

// The front end asks one function for its startup movies (INTRO, INTEL_LOGO_ENGLISH, advert<n>, SEGA_logo_sting_HD).
// It returns an empty list when the "play startup movies" flag is clear, as it is on every later visit of the main
// menu; the patch takes that branch always ("je" -> "nop; jmp", same target).
void SkipIntroMovies() {
    BYTE* code = g_base + kIntroMoviesJump;
    static const BYTE cmp[] = {0x38, 0x9E, 0xE8, 0x02, 0x00, 0x00};  // cmp byte ptr [esi+2E8h], bl
    if (memcmp(code - 6, cmp, 6) != 0 || code[0] != 0x0F || code[1] != 0x84) {
        Log("skip_intro_movies: not the supported build of empire.retail.dll; intro movies stay on");
        return;
    }
    BYTE jmp[6] = {0x90, 0xE9};
    memcpy(jmp + 2, code + 2, 4);  // same end address, so the same displacement
    if (WriteCode(code, jmp, 6)) Log("skip_intro_movies: intro movies turned off");
}

// ---------------------------------------------------------------- 4. per-frame statistics

// The main loop renders each frame through one virtual method (vtable slot 161 of the frame view); the hook notes the
// time and the CPU the main thread is on, then runs the frame.
typedef bool(__fastcall* FrameFn)(void* self, void* edx);
FrameFn g_origFrame;
void FlushTextures();
LARGE_INTEGER g_qpf, g_lastFrame, g_statStart;
FILE* g_frames;
struct Window {
    int frames = 0, ecore = 0;
    double sum = 0, worst = 0;
    std::vector<float> ms;
} g_win;
CRITICAL_SECTION g_statLock;
volatile LONGLONG g_frameStart;              // when the current frame began (QPC): Present may run on any thread
volatile LONG g_framePresents;               // presents since then
volatile LONG g_loadPresents, g_loadNoVsync;  // loading-screen frames since the last frame, and those without vsync

void FlushStats() {
    if (g_win.frames == 0) return;
    std::vector<float>& v = g_win.ms;
    std::sort(v.begin(), v.end());
    float p99 = v[(size_t)(v.size() * 0.99)], p50 = v[v.size() / 2];
    Log("frames: %d in %.1f s, %.1f fps, median %.2f ms, 99th %.2f ms, worst %.1f ms; main thread on E-cores in %.1f%%",
        g_win.frames, g_win.sum / 1000.0, g_win.frames * 1000.0 / g_win.sum, p50, p99, g_win.worst,
        g_win.ecore * 100.0 / g_win.frames);
    g_win = Window();
}

bool __fastcall HookFrame(void* self, void* edx) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&g_frameStart, now.QuadPart);
    InterlockedExchange(&g_framePresents, 0);
    LONG screens = InterlockedExchange(&g_loadPresents, 0), noVsync = InterlockedExchange(&g_loadNoVsync, 0);
    if (g_lastFrame.QuadPart) {
        double ms = (now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / g_qpf.QuadPart;
        PROCESSOR_NUMBER pn;
        GetCurrentProcessorNumberEx(&pn);
        bool ecore = !g_pcoreSets.empty() && pn.Number < 64 && !g_isPcore[pn.Number];
        EnterCriticalSection(&g_statLock);
        if (ms >= 1000) {
            FlushStats();  // a load: the summaries after it count from its end, so runs line up
            FlushTextures();
        } else {
            g_win.frames++, g_win.sum += ms, g_win.ecore += ecore;
            g_win.worst = std::max(g_win.worst, ms);
            g_win.ms.push_back((float)ms);
        }
        if (g_frames)
            fprintf(g_frames, "%.3f,%.3f,%u\n", (now.QuadPart - g_statStart.QuadPart) * 1.0 / g_qpf.QuadPart, ms,
                    (unsigned)pn.Number);
        if (ms >= 250 && screens)  // loads and hitches, with their time of day
            Log("long frame: %.0f ms; loading screen drawn %ld times, %ld without vsync", ms, screens, noVsync);
        else if (ms >= 250)
            Log("long frame: %.0f ms", ms);
        if (g_win.sum >= 30000) FlushStats();  // a summary every 30 s of frames
        LeaveCriticalSection(&g_statLock);
    } else {
        g_statStart = now;
        FILETIME created, exited, kernel, user, nowFt;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimeAsFileTime(&nowFt);
        double s = ((ULARGE_INTEGER&)nowFt).QuadPart - ((ULARGE_INTEGER&)created).QuadPart;
        Log("first frame: %.1f s after the process started", s / 1e7);
    }
    g_lastFrame = now;
    return g_origFrame(self, edx);
}

void HookFrames() {
    BYTE** slot = (BYTE**)(g_base + kFrameSlot);
    if (*slot != g_base + kFrameFunc) {
        Log("frame statistics: not the supported build of empire.retail.dll; left alone");
        return;
    }
    QueryPerformanceFrequency(&g_qpf);
    InitializeCriticalSection(&g_statLock);
    g_origFrame = (FrameFn)(g_base + kFrameFunc);
    void* hook = (void*)&HookFrame;
    if (WriteCode((BYTE*)slot, &hook, 4)) Log("frame statistics: on");
    if (g_frameLog) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%ls\\shogun2_frames.csv", g_dir);
        g_frames = _wfsopen(path, L"w", _SH_DENYWR);
        if (g_frames) fputs("time_s,frame_ms,cpu\n", g_frames);
    }
}

// ---------------------------------------------------------------- 5. texture loading (statistics)

// In DX11 mode every texture goes through D3DX11CreateTextureFromMemory (d3dx11_42.dll, DirectX SDK 2009) with a load
// description the engine fills in (114652xx): size and first mip level (texture quality), mip count, usage, binding,
// and an explicit DXGI format from the engine's own format enum. A battle load spends most of its main-thread time
// inside that call. The hook times every call and groups them by the format in the file and the format asked for.
struct LoadInfo {  // D3DX11_IMAGE_LOAD_INFO
    UINT width, height, depth, firstMip, mipLevels, usage, bindFlags, cpuFlags, miscFlags, format, filter, mipFilter;
    void* srcInfo;
};
typedef HRESULT(WINAPI* CreateTextureFn)(void* device, const void* data, SIZE_T size, LoadInfo* info, void* pump,
                                         void** texture, HRESULT* result);
CreateTextureFn g_origCreateTexture;
CRITICAL_SECTION g_texLock;
struct TexKind {
    char key[96];
    int count = 0;
    double ms = 0, worst = 0;
    double mb = 0;
};
std::vector<TexKind> g_texKinds, g_texTotal;
LARGE_INTEGER g_texQpf;

// "DXT1 m10" etc.: the pixel format of a DDS file and its mip count, or "?" when it is not one.
void DescribeDds(const BYTE* d, SIZE_T size, char* out, size_t n) {
    if (size < 128 || memcmp(d, "DDS ", 4) != 0) {
        sprintf_s(out, n, "not-dds");
        return;
    }
    DWORD mips = *(DWORD*)(d + 28), pfFlags = *(DWORD*)(d + 80), fourcc = *(DWORD*)(d + 84), bits = *(DWORD*)(d + 88);
    DWORD rmask = *(DWORD*)(d + 92), amask = *(DWORD*)(d + 104), caps2 = *(DWORD*)(d + 112);
    char fmt[32];
    if (pfFlags & 4) {  // DDPF_FOURCC
        if (fourcc == MAKEFOURCC('D', 'X', '1', '0') && size >= 148)
            sprintf_s(fmt, "DX10:%lu", *(DWORD*)(d + 128));
        else if (fourcc < 256)
            sprintf_s(fmt, "D3DFMT%lu", fourcc);
        else
            sprintf_s(fmt, "%.4s", (const char*)&fourcc);
    } else {
        sprintf_s(fmt, "%lubit%s%s", bits, amask ? "A" : "", rmask == 0xFF0000 ? "BGR" : rmask == 0xFF ? "RGB" : "");
    }
    sprintf_s(out, n, "%s%s m%lu", fmt, (caps2 & 0x200) ? " cube" : "", mips ? mips : 1);
}

void AddTexture(std::vector<TexKind>& kinds, const char* key, double ms, double mb) {
    TexKind* k = nullptr;
    for (auto& x : kinds)
        if (strcmp(x.key, key) == 0) k = &x;
    if (!k) {
        kinds.emplace_back();
        k = &kinds.back();
        strcpy_s(k->key, key);
    }
    k->count++, k->ms += ms, k->mb += mb, k->worst = std::max(k->worst, ms);
}

void LogTextures(std::vector<TexKind>& kinds, const char* what) {
    if (kinds.empty()) return;
    std::sort(kinds.begin(), kinds.end(), [](const TexKind& a, const TexKind& b) { return a.ms > b.ms; });
    int count = 0;
    double ms = 0, mb = 0;
    for (auto& k : kinds) count += k.count, ms += k.ms, mb += k.mb;
    Log("textures%s: %d created in %.0f ms (%.0f MB of files)", what, count, ms, mb);
    int shown = 0;
    for (auto& k : kinds) {
        if (shown++ == 14) break;
        Log("  %-58s %5d  %7.0f ms  avg %6.2f ms  worst %6.1f ms  %6.1f MB", k.key, k.count, k.ms, k.ms / k.count,
            k.worst, k.mb);
    }
}

// ---- the fast path
// With D3DX11_FILTER_SRGB_IN | SRGB_OUT in the load description, D3DX treats every load as a conversion, even when
// the file already holds the requested format: it decodes every block, takes the pixels through linear colour and back,
// and encodes the blocks again with its own (slow, offline-quality) BC encoder: 140-800 ms for a 1024-2048 px texture.
// The engine asks for exactly the format of the file (or its sRGB twin, same bytes) with all the mip levels the file
// already has, so the result of all that work is the file's own data, re-encoded. CreateTexture2D straight from the
// file's bytes gives the texture without the re-encoding loss, in well under a millisecond. Anything else (resizing,
// mip generation, real format changes, volumes, odd layouts) still goes to D3DX.
const UINT kD3dxDefault = 0xFFFFFFFF, kD3dxFromFile = 0xFFFFFFFD;
const UINT kFilterSrgbIn = 0x200000, kFilterSrgbOut = 0x400000;
struct ImageInfo {  // D3DX11_IMAGE_INFO
    UINT width, height, depth, arraySize, mipLevels, miscFlags, format, dimension, fileFormat;
};
struct Dds {
    UINT width, height, mips, arraySize;
    bool cube, legacyRgb;  // legacyRgb: an uncompressed layout given by bit masks (D3DX may pick another format)
    DXGI_FORMAT format;
    const BYTE* bits;
};
// Why a texture went to D3DX (the statistics show it): index 0 is "made by the fast path".
const char* const kWhy[] = {"fast", "not a supported dds", "mips missing", "format change", "colour space change",
                            "resize", "usage", "cube/2d", "truncated", "create failed", "other"};
enum Why { kMade, kNotDds, kMips, kFormat, kSrgb, kResize, kUsage, kCube, kTruncated, kCreate, kOther };
volatile LONG g_fastCount, g_slowCount;

bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_BC1_UNORM_SRGB || f == DXGI_FORMAT_BC2_UNORM_SRGB || f == DXGI_FORMAT_BC3_UNORM_SRGB ||
           f == DXGI_FORMAT_BC7_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

DXGI_FORMAT SrgbTwin(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default: return f;
    }
}

// Bytes per 4x4 block for the block-compressed formats, 0 for the others.
UINT BlockBytes(DXGI_FORMAT f) {
    if (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC1_UNORM_SRGB) return 8;
    if (f >= DXGI_FORMAT_BC4_TYPELESS && f <= DXGI_FORMAT_BC4_SNORM) return 8;
    if (f >= DXGI_FORMAT_BC2_TYPELESS && f <= DXGI_FORMAT_BC3_UNORM_SRGB) return 16;
    if (f >= DXGI_FORMAT_BC5_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) return 16;
    if (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB) return 16;
    return 0;
}

// Bytes per pixel of the plain formats the fast path takes, 0 for any other.
UINT PixelBytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R32G32_FLOAT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
        return 2;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 0;
    }
}

// The 2D (or cube, or array) textures the fast path can take straight from a .dds file.
bool ParseDds(const BYTE* d, SIZE_T size, Dds& out) {
    if (size < 128 || memcmp(d, "DDS ", 4) != 0 || *(DWORD*)(d + 4) != 124) return false;
    DWORD flags = *(DWORD*)(d + 8), pfFlags = *(DWORD*)(d + 80), fourcc = *(DWORD*)(d + 84);
    DWORD bits = *(DWORD*)(d + 88), r = *(DWORD*)(d + 92), g = *(DWORD*)(d + 96), b = *(DWORD*)(d + 100);
    DWORD a = *(DWORD*)(d + 104), caps2 = *(DWORD*)(d + 112);
    out.height = *(DWORD*)(d + 12);
    out.width = *(DWORD*)(d + 16);
    out.mips = (flags & 0x20000) && *(DWORD*)(d + 28) ? *(DWORD*)(d + 28) : 1;  // DDSD_MIPMAPCOUNT
    out.arraySize = 1;
    out.cube = out.legacyRgb = false;
    out.bits = d + 128;
    bool dx10 = false;
    if ((caps2 & 0x200000) || ((flags & 0x800000) && *(DWORD*)(d + 24) > 1)) return false;  // volume texture
    if (pfFlags & 4) {  // DDPF_FOURCC
        if (fourcc == MAKEFOURCC('D', 'X', '1', '0')) {
            if (size < 148 || *(DWORD*)(d + 132) != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
            out.format = (DXGI_FORMAT) * (DWORD*)(d + 128);
            out.cube = (*(DWORD*)(d + 136) & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
            out.arraySize = *(DWORD*)(d + 140) * (out.cube ? 6 : 1);
            out.bits = d + 148;
            dx10 = true;
        } else if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) {
            out.format = DXGI_FORMAT_BC1_UNORM;
        } else if (fourcc == MAKEFOURCC('D', 'X', 'T', '3')) {
            out.format = DXGI_FORMAT_BC2_UNORM;
        } else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) {
            out.format = DXGI_FORMAT_BC3_UNORM;
        } else if (fourcc == MAKEFOURCC('A', 'T', 'I', '1') || fourcc == MAKEFOURCC('B', 'C', '4', 'U')) {
            out.format = DXGI_FORMAT_BC4_UNORM;
        } else if (fourcc == MAKEFOURCC('A', 'T', 'I', '2') || fourcc == MAKEFOURCC('B', 'C', '5', 'U')) {
            out.format = DXGI_FORMAT_BC5_UNORM;
        } else {
            return false;  // DXT2/DXT4 (premultiplied) and the D3DFMT numbers: left to D3DX
        }
    } else if ((pfFlags & 0x40) && bits == 32) {  // DDPF_RGB
        if (r == 0xFF && g == 0xFF00 && b == 0xFF0000 && a == 0xFF000000)
            out.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        else if (r == 0xFF0000 && g == 0xFF00 && b == 0xFF && a == 0xFF000000)
            out.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        else if (r == 0xFF0000 && g == 0xFF00 && b == 0xFF && a == 0)
            out.format = DXGI_FORMAT_B8G8R8X8_UNORM;
        else
            return false;
        if ((flags & 0x8) && *(DWORD*)(d + 20) != out.width * 4) return false;  // DDSD_PITCH, not tightly packed
        out.legacyRgb = true;
    } else {
        return false;
    }
    if (out.width == 0 || out.height == 0 || out.arraySize == 0 || out.mips > 16) return false;
    if (!dx10 && (caps2 & 0x200)) {  // legacy cube map: all six faces, one after another
        if ((caps2 & 0xFC00) != 0xFC00) return false;
        out.cube = true;
        out.arraySize = 6;
    }
    return BlockBytes(out.format) || PixelBytes(out.format);
}

// Creates the texture from the file's bytes when that is exactly what D3DX would produce (kMade); any other result
// says why it is left to D3DX.
Why FastTexture(ID3D11Device* device, const BYTE* data, SIZE_T size, const LoadInfo* li, ID3D11Resource** out) {
    Dds dds;
    if (!ParseDds(data, size, dds)) return kNotDds;
    auto given = [](UINT v) { return v != 0 && v != kD3dxDefault && v != kD3dxFromFile; };
    UINT skip = li && given(li->firstMip) ? li->firstMip : 0;
    if (skip >= dds.mips) return kMips;
    // Skipping the top levels without an explicit size, D3DX still makes the texture at the file's full size
    // (engine\test\tex_test checks it). The engine always gives the size along with the level (width >> skip).
    if (skip && !(given(li->width) && given(li->height))) return kResize;
    UINT w = std::max(1u, dds.width >> skip), h = std::max(1u, dds.height >> skip);
    if (li && ((given(li->width) && li->width != w) || (given(li->height) && li->height != h))) return kResize;
    if (li && given(li->depth) && li->depth != 1) return kResize;
    UINT full = 1;
    while ((std::max(w, h) >> full) > 0) ++full;
    UINT avail = dds.mips - skip, need = full;
    if (li && li->mipLevels == kD3dxFromFile) need = avail;
    else if (li && given(li->mipLevels)) need = std::min(li->mipLevels, full);
    // Levels the file lacks (most of the game's files stop at 2x2): with the mip filter NONE, D3DX makes each one the
    // top-left corner of the level above, unscaled (checked with engine\test\tail_probe.exe), so every missing level
    // is the top-left corner of the file's last level. That is the same bytes read with that level's row pitch.
    if (need > avail && !(li && (li->mipFilter & 0xFF) == 1 /* D3DX11_FILTER_NONE */)) return kMips;
    DXGI_FORMAT format = dds.format;
    bool swizzle = false;  // B8G8R8A8/X8 in the file, R8G8B8A8 asked for: D3DX swaps red and blue (and sets X to 255)
    if (li && given(li->format) && li->format != (UINT)DXGI_FORMAT_UNKNOWN) {
        format = (DXGI_FORMAT)li->format;
        bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        swizzle = rgba && (dds.format == DXGI_FORMAT_B8G8R8A8_UNORM || dds.format == DXGI_FORMAT_B8G8R8X8_UNORM);
        if (format != dds.format && format != SrgbTwin(dds.format) && !swizzle) return kFormat;
    } else if (dds.legacyRgb) {
        return kFormat;  // D3DX maps some bit-mask layouts to another format (X8R8G8B8 -> RGBA, swizzled)
    }
    // the colour values only stay as they are when both ends are in the same space
    UINT filter = li && given(li->filter) ? li->filter : 0;
    bool srgbIn = (filter & kFilterSrgbIn) || IsSrgb(dds.format), srgbOut = (filter & kFilterSrgbOut) || IsSrgb(format);
    if (srgbIn != srgbOut) return kSrgb;
    if (swizzle && dds.cube) return kFormat;  // not needed by the game so far
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = need;
    desc.ArraySize = dds.arraySize;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = li && li->usage != kD3dxDefault ? (D3D11_USAGE)li->usage : D3D11_USAGE_DEFAULT;
    desc.BindFlags = li && li->bindFlags != kD3dxDefault ? li->bindFlags : D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = li && li->cpuFlags != kD3dxDefault ? li->cpuFlags : 0;
    bool miscGiven = li && li->miscFlags != kD3dxDefault;
    desc.MiscFlags = miscGiven ? li->miscFlags : (dds.cube ? (UINT)D3D11_RESOURCE_MISC_TEXTURECUBE : 0u);
    if (desc.Usage != D3D11_USAGE_DEFAULT && desc.Usage != D3D11_USAGE_IMMUTABLE) return kUsage;
    if (desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) return kMips;
    // a cube file loaded without the cube flag becomes a plain texture of one face in D3DX, and the reverse fails
    if (dds.cube != ((desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0) || (dds.cube && dds.arraySize % 6))
        return kCube;
    std::vector<D3D11_SUBRESOURCE_DATA> init(dds.arraySize * need);
    const BYTE* p = dds.bits;
    const BYTE* end = data + size;
    UINT block = BlockBytes(dds.format), pixel = PixelBytes(dds.format);
    std::vector<BYTE> swapped;  // the swizzled copy of the levels used
    if (swizzle) swapped.reserve(size);
    for (UINT slice = 0; slice < dds.arraySize; ++slice) {
        for (UINT mip = 0; mip < dds.mips; ++mip) {
            UINT mw = std::max(1u, dds.width >> mip), mh = std::max(1u, dds.height >> mip);
            UINT pitch = block ? std::max(1u, (mw + 3) / 4) * block : mw * pixel;
            UINT rows = block ? std::max(1u, (mh + 3) / 4) : mh;
            SIZE_T bytes = (SIZE_T)pitch * rows;
            if (p + bytes > end) return kTruncated;  // D3DX decides what to do
            if (mip >= skip && mip < skip + need) {
                const BYTE* src = p;
                if (swizzle) {
                    SIZE_T at = swapped.size();
                    swapped.insert(swapped.end(), p, p + bytes);
                    BYTE* q = swapped.data() + at;
                    bool x8 = dds.format == DXGI_FORMAT_B8G8R8X8_UNORM;
                    for (SIZE_T i = 0; i + 3 < bytes; i += 4) {
                        std::swap(q[i], q[i + 2]);
                        if (x8) q[i + 3] = 255;
                    }
                    src = nullptr;  // pointed at below, once the buffer has stopped growing
                    init[slice * need + (mip - skip)] = {(const void*)at, pitch, (UINT)bytes};
                } else {
                    init[slice * need + (mip - skip)] = {src, pitch, (UINT)bytes};
                }
            }
            p += bytes;
        }
        // the levels the file lacks: the file's last level read with its own row pitch (its top-left corner)
        for (UINT m = avail; m < need; ++m) init[slice * need + m] = init[slice * need + avail - 1];
    }
    if (swizzle)
        for (auto& s : init) s.pSysMem = swapped.data() + (SIZE_T)s.pSysMem;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, init.data(), &tex)) || !tex) return kCreate;
    *out = tex;
    if (li && li->srcInfo) {  // what D3DX reports about the file
        ImageInfo info = {dds.width, dds.height, 1, dds.arraySize, dds.mips,
                          dds.cube ? (UINT)D3D11_RESOURCE_MISC_TEXTURECUBE : 0u, (UINT)dds.format,
                          (UINT)D3D11_RESOURCE_DIMENSION_TEXTURE2D, 4 /* D3DX11_IFF_DDS */};
        *(ImageInfo*)li->srcInfo = info;
    }
    return kMade;
}

void FlushTextures() {  // after each load
    if (!g_origCreateTexture) return;
    EnterCriticalSection(&g_texLock);
    LogTextures(g_texKinds, " since the last load");
    g_texKinds.clear();
    LeaveCriticalSection(&g_texLock);
}

HRESULT WINAPI HookCreateTexture(void* device, const void* data, SIZE_T size, LoadInfo* info, void* pump,
                                 void** texture, HRESULT* result) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    HRESULT hr;
    Why why = !g_textureFast ? kOther
              : pump || !device || !data || !texture
                  ? kOther
                  : FastTexture((ID3D11Device*)device, (const BYTE*)data, size, info, (ID3D11Resource**)texture);
    bool fast = why == kMade;
    if (fast) {
        hr = S_OK;
        if (result) *result = S_OK;
        InterlockedIncrement(&g_fastCount);
    } else {
        hr = g_origCreateTexture(device, data, size, info, pump, texture, result);
        InterlockedIncrement(&g_slowCount);
    }
    QueryPerformanceCounter(&t1);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / g_texQpf.QuadPart;
    char src[48], key[96];
    DescribeDds((const BYTE*)data, size, src, sizeof src);
    const char* how = fast ? "fast" : g_textureFast ? kWhy[why] : "d3dx";
    if (info)
        sprintf_s(key, "[%s] %s -> fmt %d mips %d skip %u filter %X misc %X%s", how, src, (int)info->format,
                  (int)info->mipLevels, info->firstMip, info->filter, info->miscFlags, FAILED(hr) ? " FAILED" : "");
    else
        sprintf_s(key, "[%s] %s -> (no load info)%s", how, src, FAILED(hr) ? " FAILED" : "");
    EnterCriticalSection(&g_texLock);
    AddTexture(g_texKinds, key, ms, size / 1048576.0);
    AddTexture(g_texTotal, key, ms, size / 1048576.0);
    LeaveCriticalSection(&g_texLock);
    return hr;
}

void HookTextures() {
    BYTE* site = g_base + kCreateTextureCall;
    BYTE* stub = g_base + kCreateTextureStub;
    if (!CallsTo(site, kCreateTextureStub) || stub[0] != 0xFF || stub[1] != 0x25) {
        Log("texture statistics: not the supported build of empire.retail.dll; left alone");
        return;
    }
    QueryPerformanceFrequency(&g_texQpf);
    InitializeCriticalSection(&g_texLock);
    g_origCreateTexture = **(CreateTextureFn**)(stub + 2);  // whatever the import slot holds
    if (RedirectCall(site, &HookCreateTexture))
        Log("textures: %s", g_textureFast ? "fast path on (files that already hold the asked-for format skip D3DX)"
                                          : "D3DX as in the game (statistics only)");
}

// ---------------------------------------------------------------- 6. address space

// A 32-bit game with the large-address-aware flag gets 4 GB of addresses on 64-bit Windows. When no free block is
// large enough for an allocation, it fails; the largest free block shows how close that is.
struct Space {
    double usedMb, freeMb, largestMb;
};

Space Measure() {
    Space s = {0, 0, 0};
    BYTE* p = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        double mb = mbi.RegionSize / 1048576.0;
        if (mbi.State == MEM_FREE) {
            s.freeMb += mb;
            s.largestMb = std::max(s.largestMb, mb);
        } else {
            s.usedMb += mb;
        }
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    return s;
}

double g_lowestLargest = 1e9, g_mostUsed;

void LogSpace(const char* when) {
    Space s = Measure();
    g_lowestLargest = std::min(g_lowestLargest, s.largestMb);
    g_mostUsed = std::max(g_mostUsed, s.usedMb);
    Log("memory%s: %.0f MB of addresses in use, %.0f MB free, largest free block %.0f MB (lowest so far %.0f MB)", when,
        s.usedMb, s.freeMb, s.largestMb, g_lowestLargest);
}

// ---------------------------------------------------------------- 7. crash dumps

LPTOP_LEVEL_EXCEPTION_FILTER g_nextFilter;
volatile LONG g_dumped;

void WriteDump(EXCEPTION_POINTERS* ep, const char* why, DWORD thread) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    BYTE* addr = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod)) {
        GetModuleFileNameA(mod, modName, MAX_PATH);
    }
    const char* shortName = strrchr(modName, '\\') ? strrchr(modName, '\\') + 1 : modName;
    Space s = Measure();
    Log("CRASH (%s): exception %08lX at %s+0x%lX in thread %lu; addresses: %.0f MB in use, largest free block %.0f MB",
        why, code, shortName, (DWORD)(addr - (BYTE*)mod), thread, s.usedMb, s.largestMb);
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, dir))) return;
    wcscat_s(dir, L"\\The Creative Assembly\\Shogun2\\crash_dumps");
    SHCreateDirectoryExW(nullptr, dir, nullptr);
    // keep the 9 newest older dumps
    std::vector<std::wstring> old;
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%ls\\shogun2_*.mdmp", dir);
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do old.push_back(fd.cFileName);
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 9 < old.size(); ++i) DeleteFileW((std::wstring(dir) + L"\\" + old[i]).c_str());
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%ls\\shogun2_%04d%02d%02d_%02d%02d%02d.mdmp", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto write = dbghelp ? (decltype(&MiniDumpWriteDump))GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (!write || f == INVALID_HANDLE_VALUE) {
        Log("crash dump: could not write %s", Utf8(path).c_str());
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {thread, ep, FALSE};
    auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
    Log("crash dump: %s %s", Utf8(path).c_str(), ok ? "written" : "failed");
}

// A stack overflow leaves the thread a few KB of stack, less than the log line and the minidump take: in the beta test
// of 2026-10-23 the filter itself faulted there, and nothing was written. Such a crash is written by a thread made at
// start for it, while the crashed thread waits. Other crashes are written where they happen, because a crashed thread
// may hold a lock (the heap's, the loader's) that another thread would wait on.
HANDLE g_crashGo, g_crashDone;
EXCEPTION_POINTERS* g_crashEp;
const char* g_crashWhy;
DWORD g_crashThread;

DWORD WINAPI CrashWriter(void*) {
    WaitForSingleObject(g_crashGo, INFINITE);
    WriteDump(g_crashEp, g_crashWhy, g_crashThread);
    SetEvent(g_crashDone);
    return 0;
}

void Dump(EXCEPTION_POINTERS* ep, const char* why) {
    if (InterlockedExchange(&g_dumped, 1)) return;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW && g_crashWriter) {
        g_crashEp = ep;
        g_crashWhy = why;
        g_crashThread = GetCurrentThreadId();
        SetEvent(g_crashGo);
        WaitForSingleObject(g_crashDone, 60000);
    } else {
        WriteDump(ep, why, GetCurrentThreadId());
    }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    Dump(ep, "unhandled");
    // the filter behind ours is usually the engine's, and the engine is unloaded before the process ends
    HMODULE m;
    bool loaded = g_nextFilter && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                     (LPCWSTR)g_nextFilter, &m);
    return loaded ? g_nextFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// The engine (its C runtime) may install a filter later; it goes behind ours.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_nextFilter;
    g_nextFilter = filter;
    return prev;
}

// Running out of addresses usually ends in std::bad_alloc, which the C runtime turns into a fail-fast exit that no
// exception filter sees; so the first bad_alloc thrown is recorded when it is thrown (the game may still catch it).
bool IsBadAlloc(EXCEPTION_RECORD* r) {
    if (r->ExceptionCode != 0xE06D7363 || r->NumberParameters < 3) return false;
    __try {
        auto* throwInfo = (DWORD*)r->ExceptionInformation[2];
        auto* catchables = (DWORD*)throwInfo[3];
        for (DWORD i = 0; i < catchables[0] && i < 16; ++i) {
            auto* type = (DWORD*)catchables[1 + i];
            const char* name = (const char*)type[1] + 8;  // TypeDescriptor: vftable, spare, name
            if (strcmp(name, ".?AVbad_alloc@std@@") == 0) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

LONG WINAPI FirstChance(EXCEPTION_POINTERS* ep) {
    if (!g_dumped && IsBadAlloc(ep->ExceptionRecord)) Dump(ep, "std::bad_alloc thrown, out of memory");
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashDumps() {
    g_nextFilter = SetUnhandledExceptionFilter(&CrashFilter);
    BYTE** slot = (BYTE**)(g_base + kIatSetUnhandledExceptionFilter);
    void* hook = (void*)&HookSetFilter;
    bool iat = *slot == (BYTE*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter") &&
               WriteCode((BYTE*)slot, &hook, 4);
    AddVectoredExceptionHandler(0, &FirstChance);
    g_crashGo = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_crashDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD id = 0;
    HANDLE writer = g_crashGo && g_crashDone ? CreateThread(nullptr, 256 * 1024, &CrashWriter, nullptr,
                                                            STACK_SIZE_PARAM_IS_A_RESERVATION, &id)
                                             : nullptr;
    if (writer) {
        g_crashWriter = id;
        CloseHandle(writer);
    }
    Log("crash dumps: on%s%s", iat ? "" : " (the engine's own filter calls are not chained)",
        writer ? "" : " (no thread for stack overflows)");
}

// ---------------------------------------------------------------- 8. loading screens without vsync

// With vsync on, a battle loads 1.3-2.4 s slower and leaving it for the menu takes 3.4 s longer (2026-10-22, the same
// replay with gfx_vsync true and false): while it loads, the engine keeps redrawing the loading screen, and with vsync
// each of those frames waits for the monitor. The engine presents every frame in one place, the DX11 renderer's present
// (11487180), which passes the game's vsync setting to IDXGISwapChain::Present as the sync interval, 1 or 0; the patch
// has that interval worked out here instead. A frame of the game presents once, even a slow one, while a load redraws
// the loading screen hundreds of times within one long frame (228 for a replay, 441 for leaving a battle). So the wait
// for vsync is dropped from the second present of a frame on, once the frame has lasted over 100 ms; every frame of the
// game itself keeps vsync.
// Up to 0.2 this was a hook in IDXGISwapChain's vtable. The Steam overlay hooks Present again on every new swap chain
// (switching between window and full screen makes one) and took ours for the original, while ours called the overlay's:
// the two called each other until the stack ran out (beta test of 2026-10-23). No other tool sees this patch.
UINT __stdcall PresentSync(const BYTE* renderer) {
    UINT sync = renderer[kRendererVsync] ? 1 : 0;  // what the engine passes
    LONGLONG start = InterlockedCompareExchange64(&g_frameStart, 0, 0);
    if (start && InterlockedIncrement(&g_framePresents) > 1) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - start) * 10 > g_qpf.QuadPart) {  // a second present over 100 ms into the frame: a load
            InterlockedIncrement(&g_loadPresents);
            if (sync) {
                sync = 0;
                InterlockedIncrement(&g_loadNoVsync);
            }
        }
    }
    return sync;
}

// Takes the place of "xor eax,eax; cmp byte ptr [esi+1BF1h],al; setne al" (esi: the renderer). The engine has the
// swap chain's Present in ecx and the swap chain in edx; both must survive.
__declspec(naked) void PresentSyncThunk() {
    __asm {
        push ecx
        push edx
        push esi
        call PresentSync
        pop edx
        pop ecx
        ret
    }
}

void InstallLoadingVsync() {
    BYTE* code = g_base + kPresentSync;
    static const BYTE sync[11] = {0x33, 0xC0, 0x38, 0x86, 0xF1, 0x1B, 0x00, 0x00, 0x0F, 0x95, 0xC0};
    if (memcmp(code, sync, sizeof sync) != 0) {
        Log("loads_without_vsync: not the supported build of empire.retail.dll; left alone");
        return;
    }
    BYTE call[11] = {0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    *(int*)(call + 1) = (int)((BYTE*)&PresentSyncThunk - (code + 5));
    if (WriteCode(code, call, sizeof call)) Log("loads_without_vsync: on (loading screens are presented without vsync)");
}

// ---------------------------------------------------------------- background statistics

DWORD WINAPI StatsThread(void*) {
    for (;;) {
        Sleep(30000);
        if (g_memoryLog) LogSpace("");
        if (g_crashDumps) {  // something else may have replaced our filter: go back in front of it
            LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&CrashFilter);
            if (prev != &CrashFilter) g_nextFilter = prev;
        }
    }
}

void Install() {
    HMODULE engine = GetModuleHandleW(L"empire.retail.dll");
    g_base = (BYTE*)engine;
    Log("empire.retail.dll at %p, main thread %lu", g_base, g_mainThread);
    // The patches read and write fixed places of one build; in another build those may hold other code or lie past
    // the end of the module, so even reading them could fault.
    auto* nt = (IMAGE_NT_HEADERS32*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kImageSize) {
        Log("empire.retail.dll is build %08lX of %lX bytes, not %08lX of %lX (1.1.0 of 2023-05-12): nothing is "
            "changed, version.dll only forwards", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
            kTimestamp, kImageSize);
        return;
    }
    // shogun2.exe unloads empire.retail.dll with FreeLibrary when the game ends, and this DLL with it, while the
    // process lives on for a moment: the statistics thread and the exception handlers would then run unloaded code.
    // Pinned, the DLL stays until the process ends.
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&Install,
                            &self)) {
        Log("could not keep version.dll loaded until the game ends (error %lu): nothing is changed", GetLastError());
        return;
    }
    FindPcores();
    InstallThreads();
    InstallPackCache();
    if (g_skipIntro) SkipIntroMovies();
    HookFrames();
    HookTextures();
    if (!g_loadsNoVsync)
        Log("loads_without_vsync: off (the engine's present is left as it is)");
    else if (g_origFrame)  // it tells a load by the frame timing
        InstallLoadingVsync();
    if (g_crashDumps) InstallCrashDumps();
    if (g_memoryLog) LogSpace(" at start");
    if (g_memoryLog || g_crashDumps) CloseHandle(CreateThread(nullptr, 0, &StatsThread, nullptr, 0, nullptr));
}

}  // namespace

// ---------------------------------------------------------------- forwarded exports
// Naked stubs: load the real DLL on first use and jump to the real function with the caller's stack untouched.
extern "C" {
static void __stdcall EnsureReal() { LoadReal(); }
// argbytes: the stdcall argument size, for the (unexpected) case that the real function is missing.
#define FORWARD(name, index, argbytes)                      \
    __declspec(naked) void Proxy_##name() {                 \
        __asm { pushad }                                     \
        __asm { call EnsureReal }                            \
        __asm { popad }                                      \
        __asm { mov eax, dword ptr [g_fn + index * 4] }      \
        __asm { test eax, eax }                              \
        __asm { jz fail }                                    \
        __asm { jmp eax }                                    \
        __asm { fail: xor eax, eax }                         \
        __asm { ret argbytes }                               \
    }
FORWARD(GetFileVersionInfoA, 0, 16)
FORWARD(GetFileVersionInfoByHandle, 1, 16)
FORWARD(GetFileVersionInfoExA, 2, 20)
FORWARD(GetFileVersionInfoExW, 3, 20)
FORWARD(GetFileVersionInfoSizeA, 4, 8)
FORWARD(GetFileVersionInfoSizeExA, 5, 12)
FORWARD(GetFileVersionInfoSizeExW, 6, 12)
FORWARD(GetFileVersionInfoSizeW, 7, 8)
FORWARD(GetFileVersionInfoW, 8, 16)
FORWARD(VerFindFileA, 9, 32)
FORWARD(VerFindFileW, 10, 32)
FORWARD(VerInstallFileA, 11, 32)
FORWARD(VerInstallFileW, 12, 32)
FORWARD(VerQueryValueA, 13, 16)
FORWARD(VerQueryValueW, 14, 16)
}

// For engine\test\tex_test.exe: the fast path on its own (ordinal 100). S_OK when it made the texture, S_FALSE when
// the request would go to D3DX.
extern "C" HRESULT WINAPI EngineFixes_FastTexture(void* device, const void* data, SIZE_T size, void* info,
                                                  void** texture) {
    return FastTexture((ID3D11Device*)device, (const BYTE*)data, size, (const LoadInfo*)info,
                       (ID3D11Resource**)texture) == kMade
               ? S_OK
               : S_FALSE;
}

// For engine\test\hooks_test.exe: as if a frame of the game had begun msAgo ms ago (ordinal 101).
extern "C" void WINAPI EngineFixes_FrameStart(DWORD msAgo) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&g_frameStart, now.QuadPart - (LONGLONG)msAgo * g_qpf.QuadPart / 1000);
    InterlockedExchange(&g_framePresents, 0);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!GetModuleHandleW(L"empire.retail.dll")) return TRUE;  // another program in the game folder: only forward
        g_mainThread = GetCurrentThreadId();
        GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
        wchar_t log[MAX_PATH];
        swprintf_s(log, L"%ls\\shogun2_engine.log", g_dir);
        g_log = _wfsopen(log, L"w", _SH_DENYWR);
        Log("Shogun 2 engine fixes %s (version.dll) loaded into %s", kVersion, Utf8(g_dir).c_str());
        ReadSettings();
        Install();
    } else if (reason == DLL_PROCESS_DETACH && g_log) {
        // Pinned, the DLL is only told when the process ends; its other threads are gone by then, so no waiting for a
        // lock one of them may have held.
        g_ending = reserved != nullptr;
        if (g_origFrame && TryEnterCriticalSection(&g_statLock)) {
            FlushStats();
            LeaveCriticalSection(&g_statLock);
        }
        if (g_frames) g_ending ? _fclose_nolock(g_frames) : fclose(g_frames);
        if (g_origCreateTexture) {
            LogTextures(g_texTotal, " over the whole run");
            Log("textures: %ld made from the file's bytes, %ld by D3DX", g_fastCount, g_slowCount);
        }
        Log("pack_cache: %ld opens through the patched calls, %ld of them asked for FILE_FLAG_NO_BUFFERING", g_opens,
            g_unbuffered);
        if (g_memoryLog) {
            LogSpace(" at exit");
            Log("memory: most addresses in use %.0f MB, smallest largest-free-block %.0f MB", g_mostUsed,
                g_lowestLargest);
        }
        Log("unloaded (%s)", reserved ? "the process ends" : "FreeLibrary");
    }
    return TRUE;
}
