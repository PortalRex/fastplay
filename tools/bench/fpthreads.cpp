// fpthreads: where does a process spend its CPU? Per-thread CPU over a window (thread name, start module) plus a
// poor man's sampling profile: every few ms each busy thread is suspended for an instant, its instruction pointer
// read and mapped to module + nearest exported symbol. Read-only apart from those micro-suspends; meant for our own
// test copies (FastPlay, MPC-HC test instances), never the user's processes.
//   fpthreads.exe <pid> [seconds=10] [--interval-ms 2] [--top 25] [--no-samples]
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dbghelp.h>
#include <winternl.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "psapi.lib")

typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

struct Thread {
    DWORD tid = 0;
    HANDLE h = nullptr;
    std::wstring name, startModule;
    ULONGLONG cpu0 = 0, cpu1 = 0;
    std::map<std::string, int> hits;  // module!symbol -> samples
    int samples = 0;
};

// Cycle counts, not GetThreadTimes: those are charged in whole clock ticks (15.6 ms), far too coarse for threads that
// run 0.1 ms per frame. Cycles are converted to time with the TSC rate measured over the window.
static ULONGLONG ThreadCpu(HANDLE h) {
    ULONG64 c = 0;
    QueryThreadCycleTime(h, &c);
    return c;
}

struct Module { ULONG64 base, size; std::wstring name; };
static std::vector<Module> g_mods;

static void LoadModules(HANDLE proc) {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_ALL)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        MODULEINFO mi = {};
        wchar_t name[MAX_PATH] = {};
        GetModuleInformation(proc, mods[i], &mi, sizeof(mi));
        GetModuleBaseNameW(proc, mods[i], name, MAX_PATH);
        g_mods.push_back({(ULONG64)mi.lpBaseOfDll, mi.SizeOfImage, name});
    }
}

static std::wstring ModuleOf(ULONG64 a) {
    for (auto& m : g_mods)
        if (a >= m.base && a < m.base + m.size) return m.name;
    return L"?";
}

static std::string Symbolize(HANDLE proc, ULONG64 a) {
    std::wstring mod = ModuleOf(a);
    std::string m8(mod.begin(), mod.end());
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, a, &disp, si) && disp < 0x10000) return m8 + "!" + si->Name;
    return m8;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: fpthreads <pid> [seconds] [--interval-ms N] [--top N]\n");
        return 2;
    }
    DWORD pid = _wtoi(argv[1]);
    double secs = argc > 2 && argv[2][0] != L'-' ? _wtof(argv[2]) : 10;
    int intervalMs = 2, top = 25;
    bool sample = true;
    DWORD onlyTid = 0;  // --tid: sample just this thread (with --interval-ms 0 it is polled continuously)
    for (int i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--interval-ms") && i + 1 < argc) intervalMs = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--top") && i + 1 < argc) top = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--no-samples")) sample = false;
        else if (!wcscmp(argv[i], L"--tid") && i + 1 < argc) onlyTid = _wtoi(argv[++i]);
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        fwprintf(stderr, L"cannot open pid %lu\n", pid);
        return 1;
    }
    timeBeginPeriod(1);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitializeW(proc, nullptr, TRUE);
    LoadModules(proc);
    auto getDesc = (GetThreadDescriptionFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    typedef NTSTATUS(NTAPI * NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto ntqit = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");

    std::map<DWORD, Thread> threads;
    auto refresh = [&] {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || threads.count(te.th32ThreadID)) continue;
            Thread t;
            t.tid = te.th32ThreadID;
            t.h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t.tid);
            if (!t.h) continue;
            PWSTR d = nullptr;
            if (getDesc && SUCCEEDED(getDesc(t.h, &d)) && d) {
                t.name = d;
                LocalFree(d);
            }
            ULONG64 start = 0;
            if (ntqit && ntqit(t.h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) == 0)
                t.startModule = ModuleOf(start);
            t.cpu0 = ThreadCpu(t.h);
            threads[t.tid] = std::move(t);
        }
        CloseHandle(snap);
    };
    refresh();
    LARGE_INTEGER f, q0, q;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    unsigned long long tsc0 = __rdtsc();
    std::map<DWORD, ULONGLONG> last;
    for (auto& [id, t] : threads) last[id] = t.cpu0;
    int rounds = 0;
    for (;;) {
        QueryPerformanceCounter(&q);
        if ((q.QuadPart - q0.QuadPart) / (double)f.QuadPart >= secs) break;
        if ((q.QuadPart - q0.QuadPart) / f.QuadPart > rounds) {  // pick up new threads once a second
            rounds++;
            refresh();
        }
        for (auto& [id, t] : threads) {
            // Sample only threads that ran since the last round (a sleeping thread's IP is always its wait call).
            ULONGLONG c = ThreadCpu(t.h);
            bool ran = c != last[id];
            last[id] = c;
            if (!ran || !sample || (onlyTid && id != onlyTid)) continue;
            if (SuspendThread(t.h) == (DWORD)-1) continue;
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t.h, &ctx)) {
                ResumeThread(t.h);
                t.hits[Symbolize(proc, ctx.Rip)]++;
                t.samples++;
            } else {
                ResumeThread(t.h);
            }
        }
        if (intervalMs > 0) Sleep(intervalMs);
        else YieldProcessor();
    }
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    double wall = (q1.QuadPart - q0.QuadPart) / (double)f.QuadPart;
    double tscPerSec = (__rdtsc() - tsc0) / wall;
    std::vector<Thread*> list;
    ULONGLONG total = 0;
    for (auto& [id, t] : threads) {
        t.cpu1 = ThreadCpu(t.h);
        total += t.cpu1 - t.cpu0;
        list.push_back(&t);
    }
    std::sort(list.begin(), list.end(), [](Thread* a, Thread* b) { return a->cpu1 - a->cpu0 > b->cpu1 - b->cpu0; });
    wprintf(L"pid %lu, %.1f s: total %.2f%% of one core, %zu threads\n", pid, wall, total / tscPerSec / wall * 100, threads.size());
    int shown = 0;
    for (Thread* t : list) {
        double pct = (t->cpu1 - t->cpu0) / tscPerSec / wall * 100;
        if (pct < 0.05 || shown++ >= top) break;
        wprintf(L"  %6.2f%%  tid %-6lu %-28s start %s\n", pct, t->tid, t->name.empty() ? L"-" : t->name.c_str(),
                t->startModule.c_str());
        std::vector<std::pair<int, std::string>> h;
        for (auto& [k, v] : t->hits) h.push_back({v, k});
        std::sort(h.rbegin(), h.rend());
        for (size_t i = 0; i < h.size() && i < 12; i++)
            wprintf(L"            %5.1f%%  %S\n", 100.0 * h[i].first / t->samples, h[i].second.c_str());
    }
    for (auto& [id, t] : threads) CloseHandle(t.h);
    SymCleanup(proc);
    CloseHandle(proc);
    timeEndPeriod(1);
    return 0;
}
