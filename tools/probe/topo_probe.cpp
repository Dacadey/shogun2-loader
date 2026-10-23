// Runs the engine's own CPU-topology and worker-pool code outside the game and reports what it does on this PC:
// the topology records it builds, the affinity mask it leaves on the calling (main) thread, and the mask of every
// thread the TBB pool creates.
//
//   topo_probe.exe <path to empire.retail.dll> [number_of_threads, default 0 = auto] [--pool]
//
// The DLL's dependencies are looked up next to it (SetDllDirectory), so point it at a copy of the game's DLLs (such as
// engine\test\build\input, which engine\test\test.cmd makes) rather than at a game folder where other mods put their
// own dinput8.dll or d3d11.dll. Addresses are for empire.retail.dll 1.1.0 (12 May 2023).
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const DWORD kBase = 0x10000000;
static const DWORD kTopology = 0x100C12E0;   // void __cdecl (TOPOLOGY *out)
static const DWORD kSingleton = 0x112F1A40;  // TOPOLOGY_AND_POOL *__cdecl ()
static const DWORD kPoolSetup = 0x112C00D0;  // void __thiscall (int number_of_threads, bool pin)

typedef LONG(NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
struct THREAD_BASIC_INFORMATION_ {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    struct { HANDLE UniqueProcess, UniqueThread; } ClientId;
    ULONG_PTR AffinityMask;
    LONG Priority, BasePriority;
};

static ULONG_PTR thread_affinity(HANDLE h) {
    static auto q = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    THREAD_BASIC_INFORMATION_ tbi = {};
    if (q(h, 0, &tbi, sizeof tbi, nullptr) != 0) return 0;
    return tbi.AffinityMask;
}

static std::string mask_str(ULONG_PTR m) {
    std::string s;
    for (int i = 0; i < 32; i++)
        if (m & (1u << i)) { s += (s.empty() ? "" : ","); s += std::to_string(i); }
    return s.empty() ? "-" : s;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: topo_probe <empire.retail.dll> [number_of_threads] [--pool]\n"); return 2; }
    int nthreads = argc > 2 && argv[2][0] != '-' ? atoi(argv[2]) : 0;
    bool pool = false;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--pool")) pool = true;

    char dir[MAX_PATH];
    strcpy_s(dir, argv[1]);
    if (char *s = strrchr(dir, '\\')) *s = 0;
    SetDllDirectoryA(dir);
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    DWORD delta = (DWORD)m - kBase;
    printf("empire.retail.dll at %08lX\n", (DWORD)m);

    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    printf("process mask %08lX (%s)\n", (DWORD)proc, mask_str(proc).c_str());
    printf("main thread before: %s\n", mask_str(thread_affinity(GetCurrentThread())).c_str());

    // the topology record: +0 vendor (0 Intel path, 1 AMD, 5 other), +4 logical processors,
    // +8 + 16*i: {a, core id, b, -}, +0x1008 cache count, +0x100C + 24*i cache records
    static unsigned char topo[0x4000];
    auto topology = (void(__cdecl *)(void *))(kTopology + delta);
    topology(topo);
    DWORD vendor = *(DWORD *)(topo + 0), nlp = *(DWORD *)(topo + 4);
    printf("topology: vendor path %lu, %lu logical processors\n", vendor, nlp);
    std::vector<std::vector<int>> cores;
    for (DWORD i = 0; i < nlp && i < 64; i++) {
        DWORD *r = (DWORD *)(topo + 8 + 16 * i);
        printf("  LP %2lu: rec0=%lu core=%lu rec2=%lu\n", i, r[0], r[1], r[2]);
        if (r[1] >= cores.size()) cores.resize(r[1] + 1);
        cores[r[1]].push_back((int)i);
    }
    printf("main thread after topology: %s\n", mask_str(thread_affinity(GetCurrentThread())).c_str());
    printf("auto mode would make %zu worker slots:\n", cores.size());
    for (size_t c = 0; c < cores.size(); c++) {
        ULONG_PTR mask = 0;
        for (int lp : cores[c]) mask += (ULONG_PTR)1 << (lp & 31);
        printf("  slot %2zu: mask %08lX (%s)\n", c, (DWORD)mask, mask_str(mask).c_str());
    }

    if (pool) {
        // the real thing: the engine's singleton and its pool setup, as called at 1009D485
        void *obj = ((void *(__cdecl *)())(kSingleton + delta))();
        void *setup = (void *)(kPoolSetup + delta);
        int n = nthreads;
        DWORD t0 = GetTickCount();
        __asm {
            push 1
            push n
            mov ecx, obj
            call setup
        }
        printf("pool setup(%d, pin) took %lu ms\n", nthreads, GetTickCount() - t0);
        Sleep(200);
        printf("main thread after pool setup: %s\n", mask_str(thread_affinity(GetCurrentThread())).c_str());
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof te};
        int count = 0;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            printf("  thread %6lu%s: %s\n", te.th32ThreadID, te.th32ThreadID == GetCurrentThreadId() ? " (main)" : "",
                   mask_str(thread_affinity(h)).c_str());
            CloseHandle(h);
            count++;
        }
        CloseHandle(snap);
        printf("%d threads\n", count);
    }
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);  // skip the engine's static destructors
    return 0;
}
