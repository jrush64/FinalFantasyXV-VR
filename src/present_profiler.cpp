#include "research_diagnostics.h"
#include "present_profiler.h"
#include "aer_control.h"
#include "runtime_log.h"

#include <strsafe.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

// [PROFILE] The sampler runs in the
// playable build (no research gate), names this proxy "MOD-dxgi.dll" so it
// cannot be confused with the system dxgi.dll, skips the module-base junk
// frame, and adds a "wait caller" table: for every sample whose leaf is a
// kernel wait in ntdll, the first stack word inside a module other than
// ntdll/kernel32/KERNELBASE/win32u.  That is the code that decided to wait
// (driver, runtime, engine or this mod), which the leaf alone never said.

namespace
{
std::atomic<DWORD> g_presentThreadId{0};
std::atomic_bool g_started{false};

struct Module
{
    char name[40];
    std::uintptr_t base, end;
    bool game;
    bool self;     // this proxy DLL
    bool system;   // ntdll / kernel32 / KERNELBASE / win32u: never a "wait caller"
};
std::array<Module, 128> g_modules{};
std::size_t g_moduleCount{};
long long g_modulesRefreshedAt{};

struct Bucket { std::uintptr_t address; std::uint32_t count; };
constexpr std::size_t kTableSize = 4096;
struct Tables
{
    std::array<Bucket, kTableSize> leaf{}, game{}, game2{}, wait{};
    std::array<std::uint32_t, 128> moduleHits{};
    std::uint32_t samples{};
    std::uint32_t waitSamples{};
};
Tables g_present{};   // the thread that calls Present (the render thread)
Tables g_hot{};       // the busiest other thread (the producer side)
std::atomic<DWORD> g_hotThreadId{0};
// [PROFILE] thread census: CPU time per thread over ~2 s, busiest first.
struct ThreadCpu { DWORD id; unsigned long long cpu; };
std::array<ThreadCpu, 1024> g_threadCpu{};
std::size_t g_threadCpuCount{};
long long g_censusAt{};
DWORD g_samplerThreadId{};
std::uint32_t g_reports{};
constexpr std::uint32_t kSamplesPerReport = 2000;
constexpr std::uint32_t kMaxReports = 16;

void LogF(const char* format, ...)
{
    char line[1024]{};
    va_list args;
    va_start(args, format);
    StringCchVPrintfA(line, _countof(line), format, args);
    va_end(args);
    RetailLogLine(line);
}

// kernel32-exported psapi entry points: no psapi.lib dependency.
using EnumModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
struct ModuleInfo { LPVOID base; DWORD size; LPVOID entry; };
using ModuleInfoFn = BOOL(WINAPI*)(HANDLE, HMODULE, ModuleInfo*, DWORD);
using ModuleNameFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPSTR, DWORD);

std::uintptr_t SelfBase()
{
    static const std::uintptr_t base = []
    {
        HMODULE self{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&SelfBase), &self))
            return reinterpret_cast<std::uintptr_t>(self);
        return std::uintptr_t{};
    }();
    return base;
}

void RefreshModules()
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel)
        return;
    auto enumerate = reinterpret_cast<EnumModulesFn>(GetProcAddress(kernel, "K32EnumProcessModules"));
    auto info = reinterpret_cast<ModuleInfoFn>(GetProcAddress(kernel, "K32GetModuleInformation"));
    auto name = reinterpret_cast<ModuleNameFn>(GetProcAddress(kernel, "K32GetModuleBaseNameA"));
    if (!enumerate || !info || !name)
        return;
    HMODULE handles[512]{};
    DWORD needed = 0;
    if (!enumerate(GetCurrentProcess(), handles, sizeof(handles), &needed))
        return;
    const std::size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), _countof(handles));
    std::size_t out = 0;
    for (std::size_t index = 0; index < count && out < g_modules.size(); ++index)
    {
        ModuleInfo mi{};
        if (!info(GetCurrentProcess(), handles[index], &mi, sizeof(mi)))
            continue;
        Module& m = g_modules[out];
        m.base = reinterpret_cast<std::uintptr_t>(mi.base);
        m.end = m.base + mi.size;
        m.name[0] = '\0';
        name(GetCurrentProcess(), handles[index], m.name, sizeof(m.name));
        m.game = _stricmp(m.name, "ffxv_s.exe") == 0;
        m.self = m.base == SelfBase();
        if (m.self)
            StringCchCopyA(m.name, sizeof(m.name), "MOD-dxgi.dll");
        m.system = _stricmp(m.name, "ntdll.dll") == 0 || _stricmp(m.name, "KERNELBASE.dll") == 0 ||
            _stricmp(m.name, "KERNEL32.DLL") == 0 || _stricmp(m.name, "kernel32.dll") == 0 ||
            _stricmp(m.name, "win32u.dll") == 0;
        ++out;
    }
    g_moduleCount = out;
}

int ModuleIndexOf(std::uintptr_t address)
{
    for (std::size_t index = 0; index < g_moduleCount; ++index)
        if (address >= g_modules[index].base && address < g_modules[index].end)
            return static_cast<int>(index);
    return -1;
}

void Count(std::array<Bucket, kTableSize>& table, std::uintptr_t address)
{
    std::size_t slot = (address >> 4) % kTableSize;
    for (std::size_t probe = 0; probe < 64; ++probe)
    {
        Bucket& bucket = table[(slot + probe) % kTableSize];
        if (bucket.address == address || bucket.address == 0)
        {
            bucket.address = address;
            ++bucket.count;
            return;
        }
    }
}

// Nearest preceding export in the module containing `address`; game code
// and this proxy have no useful exports, so those stay as +RVA.
bool NearestExport(std::uintptr_t address, int moduleIndex, char* out, std::size_t outSize)
{
    if (moduleIndex < 0 || g_modules[moduleIndex].game || g_modules[moduleIndex].self)
        return false;
    const std::uintptr_t base = g_modules[moduleIndex].base;
    __try
    {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size)
            return false;
        auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const DWORD rva = static_cast<DWORD>(address - base);
        DWORD bestRva = 0;
        const char* bestName = nullptr;
        for (DWORD index = 0; index < exports->NumberOfNames; ++index)
        {
            const WORD ordinal = ordinals[index];
            if (ordinal >= exports->NumberOfFunctions)
                continue;
            const DWORD functionRva = functions[ordinal];
            if (functionRva <= rva && functionRva > bestRva)
            {
                bestRva = functionRva;
                bestName = reinterpret_cast<const char*>(base + names[index]);
            }
        }
        if (!bestName || rva - bestRva > 0x4000)
            return false;
        StringCchPrintfA(out, outSize, "%s!%s+0x%X", g_modules[moduleIndex].name, bestName, rva - bestRva);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void Describe(std::uintptr_t address, char* out, std::size_t outSize)
{
    const int index = ModuleIndexOf(address);
    if (index < 0)
    {
        StringCchPrintfA(out, outSize, "%llX", static_cast<unsigned long long>(address));
        return;
    }
    if (NearestExport(address, index, out, outSize))
        return;
    StringCchPrintfA(out, outSize, "%s+0x%llX", g_modules[index].name,
        static_cast<unsigned long long>(address - g_modules[index].base));
}

void ReportTop(const char* label, std::array<Bucket, kTableSize>& table, std::uint32_t total)
{
    std::array<Bucket, 12> top{};
    for (const auto& bucket : table)
    {
        if (!bucket.address)
            continue;
        for (std::size_t rank = 0; rank < top.size(); ++rank)
        {
            if (bucket.count > top[rank].count)
            {
                for (std::size_t shift = top.size() - 1; shift > rank; --shift)
                    top[shift] = top[shift - 1];
                top[rank] = bucket;
                break;
            }
        }
    }
    char line[1000]{};
    StringCchPrintfA(line, _countof(line), "[PROFILE] %s:", label);
    for (const auto& entry : top)
    {
        if (!entry.address)
            break;
        char site[160]{};
        Describe(entry.address, site, sizeof(site));
        char part[200]{};
        StringCchPrintfA(part, _countof(part), " %s=%.1f%%", site,
            100.0 * static_cast<double>(entry.count) / static_cast<double>(std::max(1u, total)));
        StringCchCatA(line, _countof(line), part);
    }
    RetailLogLine(line);
    for (auto& bucket : table)
        bucket = {};
}

void Report(Tables& t, const char* who)
{
    char line[700]{};
    StringCchPrintfA(line, _countof(line), "[PROFILE] report %u (%s): %u samples in native stereo, %.0f%% in kernel waits; time by module:",
        g_reports + 1, who, t.samples, 100.0 * static_cast<double>(t.waitSamples) / static_cast<double>(std::max(1u, t.samples)));
    for (int pass = 0; pass < 7; ++pass)
    {
        std::size_t best = g_modules.size();
        for (std::size_t index = 0; index < g_moduleCount; ++index)
            if (t.moduleHits[index] && (best == g_modules.size() || t.moduleHits[index] > t.moduleHits[best]))
                best = index;
        if (best == g_modules.size())
            break;
        char part[96]{};
        StringCchPrintfA(part, _countof(part), " %s=%.1f%%", g_modules[best].name,
            100.0 * static_cast<double>(t.moduleHits[best]) / static_cast<double>(t.samples));
        StringCchCatA(line, _countof(line), part);
        t.moduleHits[best] = 0;
    }
    RetailLogLine(line);
    ReportTop("leaf (where the thread IS)", t.leaf, t.samples);
    ReportTop("wait caller (first non-kernel frame above a kernel wait; % of ALL samples)", t.wait, t.samples);
    ReportTop("innermost game frame (ffxv_s.exe return address nearest the top of stack)", t.game, t.samples);
    ReportTop("next game frame", t.game2, t.samples);
    t.moduleHits = {};
    t.samples = 0;
    t.waitSamples = 0;
}

using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

// Which threads burn CPU while the render thread idles?  Delta of
// kernel+user time per thread since the last census, busiest eight logged
// with their start addresses; the busiest that is not the render thread or
// this sampler becomes the "hot" sampled thread.
void ThreadCensus(DWORD presentId, double intervalSeconds)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    static auto queryThread = reinterpret_cast<QueryThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    struct Delta { DWORD id; unsigned long long delta; std::uintptr_t start; };
    std::array<Delta, 1024> deltas{};
    std::size_t deltaCount = 0;
    std::array<ThreadCpu, 1024> now{};
    std::size_t nowCount = 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok && nowCount < now.size(); ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid)
            continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
            continue;
        FILETIME create{}, exit{}, kernel{}, user{};
        unsigned long long cpu = 0;
        if (GetThreadTimes(thread, &create, &exit, &kernel, &user))
            cpu = (static_cast<unsigned long long>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                (static_cast<unsigned long long>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        std::uintptr_t start = 0;
        if (queryThread)
        {
            ULONG_PTR value = 0;
            if (queryThread(thread, 9 /*ThreadQuerySetWin32StartAddress*/, &value, sizeof(value), nullptr) == 0)
                start = value;
        }
        CloseHandle(thread);
        now[nowCount++] = {entry.th32ThreadID, cpu};
        unsigned long long previous = 0;
        bool seen = false;
        for (std::size_t index = 0; index < g_threadCpuCount; ++index)
            if (g_threadCpu[index].id == entry.th32ThreadID)
            {
                previous = g_threadCpu[index].cpu;
                seen = true;
                break;
            }
        if (seen && cpu > previous && deltaCount < deltas.size())
            deltas[deltaCount++] = {entry.th32ThreadID, cpu - previous, start};
    }
    CloseHandle(snapshot);
    g_threadCpu = now;
    g_threadCpuCount = nowCount;
    if (!deltaCount)
        return;
    std::sort(deltas.begin(), deltas.begin() + deltaCount,
        [](const Delta& a, const Delta& b) { return a.delta > b.delta; });
    unsigned long long totalDelta = 0;
    for (std::size_t index = 0; index < deltaCount; ++index)
        totalDelta += deltas[index].delta;
    char line[1000]{};
    StringCchPrintfA(line, _countof(line), "[PROFILE] threads busiest over %.1fs (%% of one core; process total %.0f%% = %.1f cores of %lu threads):",
        intervalSeconds, static_cast<double>(totalDelta) / (intervalSeconds * 1e7) * 100.0,
        static_cast<double>(totalDelta) / (intervalSeconds * 1e7), static_cast<unsigned long>(nowCount));
    DWORD hot = 0;
    for (std::size_t index = 0; index < deltaCount && index < 8; ++index)
    {
        const Delta& d = deltas[index];
        const double percent = static_cast<double>(d.delta) / (intervalSeconds * 1e7) * 100.0;
        char site[120]{};
        Describe(d.start, site, sizeof(site));
        char part[200]{};
        StringCchPrintfA(part, _countof(part), " tid=%lu %.0f%% start=%s%s%s", d.id, percent, site,
            d.id == presentId ? " (PRESENT)" : "", d.id == g_samplerThreadId ? " (sampler)" : "");
        StringCchCatA(line, _countof(line), part);
        if (!hot && d.id != presentId && d.id != g_samplerThreadId && percent > 20.0)
            hot = d.id;
    }
    RetailLogLine(line);
    if (hot)
        g_hotThreadId.store(hot, std::memory_order_release);
}

void SampleOnce(HANDLE thread, Tables& t, const char* who)
{
    if (SuspendThread(thread) == static_cast<DWORD>(-1))
        return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    std::uintptr_t rip = 0, rsp = 0;
    std::uintptr_t game1 = 0, game2 = 0, waitCaller = 0;
    bool inWait = false;
    if (GetThreadContext(thread, &context))
    {
        rip = context.Rip;
        rsp = context.Rsp;
        const int leafModule = ModuleIndexOf(rip);
        inWait = leafModule >= 0 && _stricmp(g_modules[leafModule].name, "ntdll.dll") == 0;
        // Heuristic unwind: scan the top of the stack for values inside
        // ffxv_s.exe.  Stale frames can slip in, which is why two sites are
        // kept and the counts are read as a distribution, not a call graph.
        __try
        {
            const auto* words = reinterpret_cast<const std::uintptr_t*>(rsp);
            for (std::size_t index = 0; index < 2048 && (!game2 || (inWait && !waitCaller)); ++index)
            {
                const std::uintptr_t value = words[index];
                const int module = ModuleIndexOf(value);
                if (module < 0 || value == g_modules[module].base)
                    continue;
                if (inWait && !waitCaller && !g_modules[module].system)
                    waitCaller = value;
                if (!g_modules[module].game)
                    continue;
                if (!game1)
                    game1 = value;
                else if (value != game1)
                    game2 = value;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    ResumeThread(thread);
    if (!rip)
        return;
    ++t.samples;
    const int module = ModuleIndexOf(rip);
    if (module >= 0)
        ++t.moduleHits[module];
    Count(t.leaf, rip);
    if (inWait)
    {
        ++t.waitSamples;
        if (waitCaller)
            Count(t.wait, waitCaller);
    }
    if (game1)
        Count(t.game, game1);
    if (game2)
        Count(t.game2, game2);
    if (t.samples >= kSamplesPerReport)
    {
        Report(t, who);
        if (&t == &g_present)
            ++g_reports;
    }
}

DWORD WINAPI SamplerThread(void*)
{
    g_samplerThreadId = GetCurrentThreadId();
    HANDLE thread = nullptr, hotThread = nullptr;
    DWORD threadId = 0, hotId = 0;
    LogF("[PROFILE] sampler armed: ~1 ms samples of the present thread and the busiest other thread while native stereo is on, %u reports of %u samples, then stops",
        kMaxReports, kSamplesPerReport);
    unsigned tick = 0;
    while (g_reports < kMaxReports)
    {
        Sleep(1);
        ++tick;
        const DWORD current = g_presentThreadId.load(std::memory_order_acquire);
        if (!current || !AerControl::IsNativeStereoEnabled())
            continue;
        if (current != threadId)
        {
            if (thread)
                CloseHandle(thread);
            thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, current);
            threadId = current;
            if (!thread)
                continue;
        }
        const long long now = GetTickCount64();
        if (now - g_modulesRefreshedAt > 10000)
        {
            RefreshModules();
            g_modulesRefreshedAt = now;
        }
        if (!g_censusAt)
            g_censusAt = now;
        else if (now - g_censusAt >= 2000)
        {
            ThreadCensus(current, static_cast<double>(now - g_censusAt) / 1000.0);
            g_censusAt = now;
        }
        const DWORD wantHot = g_hotThreadId.load(std::memory_order_acquire);
        if (wantHot != hotId)
        {
            if (hotThread)
                CloseHandle(hotThread);
            hotThread = wantHot ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, wantHot) : nullptr;
            hotId = wantHot;
        }
        if (!g_moduleCount)
            continue;
        if ((tick & 1) || !hotThread)
            SampleOnce(thread, g_present, "PRESENT thread");
        else
            SampleOnce(hotThread, g_hot, "HOT thread");
    }
    if (thread)
        CloseHandle(thread);
    if (hotThread)
        CloseHandle(hotThread);
    LogF("[PROFILE] sampler finished after %u reports", g_reports);
    return 0;
}
} // namespace

namespace PresentProfiler
{
void Start()
{
    if (g_started.exchange(true))
        return;
    if (HANDLE thread = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr))
        CloseHandle(thread);
}

void NotePresentThread(DWORD id)
{
    g_presentThreadId.store(id, std::memory_order_release);
}
}
