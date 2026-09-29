#pragma once
// [PERF] Runtime kill-switch census for the native-stereo frame budget.
//
// Measured with PresentMon (native stereo, 2560x1440):
// measured GPU busy 5.6 ms of a 17.4 ms frame; the FPSAB probe reads gpuLag 1
// in every mode; the sampler had the present thread 55-78 % busy
// with ~40 % of its samples in kernel waits.  The GPU is not the limiter.  The
// Present-thread frame is: engine+mod CPU work outside the mod's hook (10-15 ms in
// stereo) plus a fixed-cost block inside the real DXGI Present (2.5-10 ms
// depending on build, uncorrelated with how long the CPU took beforehand,
// 0.1 ms before the HUD/marker bridges existed).
//
// This header adds (a) counters and cheap __rdtsc timers around every piece of
// per-call work the mod does on the game's D3D contexts, split by "present
// thread" (the critical path) vs "other threads" (deferred-context workers),
// (b) hotkey kill-switches so each suspect can be removed live and the effect
// read from the fps in the same run, and (c) a [PERF] summary every 2 s.
//
// Counting is thread-local and flushed to the shared atomics every 64 calls,
// so the census itself adds no cache-line contention across the game's
// worker threads.  Nothing here changes what the game renders unless a
// switch is pressed; every switch is off at start.
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "runtime_log.h"

namespace PerfCensus
{
enum Counter : unsigned
{
    DrawPt, DrawOther,              // draw calls reaching the ui_hook detours
    UiTicksPt, UiTicksOther,        // rdtsc spent in the detour body (not the game's draw)
    PoseTicks,                      // NativeDrawPose::BeforeDraw (subset of Ui ticks)
    LightningTicks,                 // LightningGeometryFix::Reject (subset of Ui ticks)
    MapPt, MapOther,                // Map calls reaching the ui_hook detours
    MapTicksPt, MapTicksOther,      // rdtsc in the Map+Unmap detour bodies (not the driver's Map)
    StoreCalls, StoreBytes, StoreWc,// UiWorld::Store calls / bytes copied / copies via the WC streaming path
    StoreTicksPt, StoreTicksOther,  // rdtsc inside Store (subset of Map ticks for mapped stores)
    FullEyeReplays, FullEyeTicks,   // FullEye::Replay executions and its rdtsc (draw included)
    VsCbPt, VsCbOther,              // VSSetConstantBuffers calls through the vtable hook
    CmdListExec,                    // ExecuteCommandList through the ui_hook detour
    PresentSkipped, PresentNotReady,// desktop presents skipped by switch / rejected by DO_NOT_WAIT
    Count
};

inline std::atomic<DWORD> presentTid{};
inline std::atomic<int> presentMode{};   // 0 normal, 1 DXGI_PRESENT_DO_NOT_WAIT, 2 skip (1 in 120 kept)
inline std::atomic_bool bypassHud{}, bypassUiDraw{}, bypassShadow{}, bypassFullEye{}, bypassXr{};
inline std::atomic_uint64_t counters[Count]{};
// PresentTiming stage accumulators (index = PresentTiming::Stage), fed by
// PresentTiming::Scope whether or not the bounded CSV capture is running.
inline std::atomic<long long> stageTicks[16]{};
inline std::atomic<unsigned> stageCalls[16]{};

struct Tally { std::uint64_t v[Count]{}; unsigned pending{}; };
inline thread_local Tally t_tally{};
inline void Flush()
{
    for (unsigned i = 0; i < Count; ++i)
        if (t_tally.v[i]) { counters[i].fetch_add(t_tally.v[i], std::memory_order_relaxed); t_tally.v[i] = 0; }
    t_tally.pending = 0;
}
inline void Add(Counter c, std::uint64_t n = 1)
{
    t_tally.v[c] += n;
    if (++t_tally.pending >= 64) Flush();
}
inline bool OnPresentThread() { return GetCurrentThreadId() == presentTid.load(std::memory_order_relaxed); }
// [Perf] Timers=1 in ffxv-vr.ini enables the per-call rdtsc timers; the
// counts and the [PERF] summary are always on (thread-local, ~free).
inline std::atomic_bool timers{};
inline std::atomic_bool sampler{};
inline std::uint64_t Tsc() { return timers.load(std::memory_order_relaxed) ? __rdtsc() : 0; }
inline void LoadIni()
{
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto* slash = wcsrchr(path, L'\\'); if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - size_t(slash + 1 - path), L"ffxv-vr.ini");
    timers.store(GetPrivateProfileIntW(L"Perf", L"Timers", 0, path) != 0);
    sampler.store(GetPrivateProfileIntW(L"Perf", L"Sampler", 0, path) != 0);
}

// Present-thread frame bookkeeping (only ever touched from the Present hook).
inline long long g_lastEntry{}, g_lastAfter{}, g_windowStartQpc{};
inline std::uint64_t g_windowStartTsc{};
inline unsigned g_frames{};
inline double g_sumInterval{}, g_sumPresent{}, g_sumPre{}, g_sumOutside{};
inline double g_maxInterval{};
inline long long g_processStartQpc{};
inline long long QpcNow() { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q.QuadPart; }
inline double QpcMs(long long ticks) { static const double f = [] { LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); return 1000.0 / double(q.QuadPart); }(); return double(ticks) * f; }

inline const char* PresentModeName(int mode) { return mode == 1 ? "NOWAIT" : mode == 2 ? "SKIP" : "normal"; }

inline void LogSwitches(const char* why)
{
    char line[400];
    std::snprintf(line, sizeof(line),
        "[PERF] switches (%s): present=%s hudBridges=%s uiDrawDetours=%s shadowCopies=%s fullEyeReplay=%s xrPresenter=%s (no hotkeys in this build; timers=%d sampler=%d)",
        why, PresentModeName(presentMode.load()),
        bypassHud.load() ? "BYPASSED" : "on", bypassUiDraw.load() ? "BYPASSED" : "on",
        bypassShadow.load() ? "BYPASSED" : "on", bypassFullEye.load() ? "BYPASSED" : "on",
        bypassXr.load() ? "BYPASSED" : "on", timers.load() ? 1 : 0, sampler.load() ? 1 : 0);
    RetailLogLine(line);
}

// Called once per Present from the Present hook after the real Present returns.
inline void OnPresentDone(long long entryQpc, long long beforePresentQpc, long long afterPresentQpc,
    bool stereo, bool aer)
{
    Flush(); // the present thread's own tallies are always current at report time
    if (!g_processStartQpc) g_processStartQpc = entryQpc;
    if (g_lastEntry)
    {
        const double interval = QpcMs(entryQpc - g_lastEntry);
        if (interval > 0.0 && interval < 2000.0)
        {
            ++g_frames;
            g_sumInterval += interval;
            g_maxInterval = interval > g_maxInterval ? interval : g_maxInterval;
            g_sumPresent += QpcMs(afterPresentQpc - beforePresentQpc);
            g_sumPre += QpcMs(beforePresentQpc - entryQpc);
            g_sumOutside += g_lastAfter ? QpcMs(entryQpc - g_lastAfter) : 0.0;
        }
    }
    g_lastEntry = entryQpc;
    g_lastAfter = afterPresentQpc;
    if (!g_windowStartQpc) { g_windowStartQpc = afterPresentQpc; g_windowStartTsc = Tsc(); return; }
    const double windowMs = QpcMs(afterPresentQpc - g_windowStartQpc);
    if (windowMs < 2000.0) return;
    const double tscPerMs = double(Tsc() - g_windowStartTsc) / windowMs;
    const double n = g_frames ? double(g_frames) : 1.0;
    auto take = [](Counter c) { return counters[c].exchange(0, std::memory_order_relaxed); };
    auto ms = [&](std::uint64_t ticks) { return tscPerMs > 0.0 ? double(ticks) / tscPerMs / n : 0.0; };
    auto stage = [&](int s) { const auto t = stageTicks[s].exchange(0, std::memory_order_relaxed); stageCalls[s].exchange(0, std::memory_order_relaxed); return QpcMs(t) / n; };
    // PresentTiming::Stage order: XrTotal, XrWait, XrBegin, HudTotal, EyeTotal, KeyedWait, SwapWait, XrEnd, Desktop
    const double xrTotal = stage(0), xrWait = stage(1), xrBegin = stage(2), hud = stage(3), eye = stage(4),
        keyed = stage(5), swapWait = stage(6), xrEnd = stage(7), desktop = stage(8);
    char line[900];
    std::snprintf(line, sizeof(line),
        "[PERF] t=%.0fs %s fps=%.1f frame=%.2fms(max %.0f) present=%.2f preHook=%.2f outsideHook=%.2f | xrTotal=%.2f xrWait=%.2f xrBegin=%.2f hud=%.2f eye=%.2f keyed=%.2f swapWait=%.2f xrEnd=%.2f desktopScope=%.2f | present=%s hud=%s uidraw=%s shadow=%s fulleye=%s xr=%s skipped=%llu notReady=%llu",
        QpcMs(afterPresentQpc - g_processStartQpc) / 1000.0,
        stereo ? "STEREO" : aer ? "AER" : "MONO",
        g_frames * 1000.0 / (g_sumInterval > 0.0 ? g_sumInterval : 1.0), g_sumInterval / n, g_maxInterval,
        g_sumPresent / n, g_sumPre / n, g_sumOutside / n,
        xrTotal, xrWait, xrBegin, hud, eye, keyed, swapWait, xrEnd, desktop,
        PresentModeName(presentMode.load()),
        bypassHud.load() ? "OFF" : "on", bypassUiDraw.load() ? "OFF" : "on", bypassShadow.load() ? "OFF" : "on",
        bypassFullEye.load() ? "OFF" : "on", bypassXr.load() ? "OFF" : "on",
        static_cast<unsigned long long>(take(PresentSkipped)), static_cast<unsigned long long>(take(PresentNotReady)));
    RetailLogLine(line);
    const auto drawPt = take(DrawPt), drawOther = take(DrawOther);
    const auto uiPt = take(UiTicksPt), uiOther = take(UiTicksOther);
    const auto pose = take(PoseTicks), lightning = take(LightningTicks);
    const auto mapPt = take(MapPt), mapOther = take(MapOther);
    const auto mapTPt = take(MapTicksPt), mapTOther = take(MapTicksOther);
    const auto stores = take(StoreCalls), storeBytes = take(StoreBytes), storeWc = take(StoreWc);
    const auto storeTPt = take(StoreTicksPt), storeTOther = take(StoreTicksOther);
    const auto feReplays = take(FullEyeReplays), feTicks = take(FullEyeTicks);
    const auto vsPt = take(VsCbPt), vsOther = take(VsCbOther), cmd = take(CmdListExec);
    std::snprintf(line, sizeof(line),
        "[PERF] per-frame: draws pt=%.0f other=%.0f uiDetourMs pt=%.3f other=%.3f (pose %.3f lightning %.3f) | maps pt=%.0f other=%.0f mapDetourMs pt=%.3f other=%.3f | stores=%.0f KB=%.0f wcPath=%.0f%% storeMs pt=%.3f other=%.3f | fullEye replays=%.1f ms=%.3f | vscb pt=%.0f other=%.0f | cmdlists=%.1f | tsc=%.0fMHz",
        drawPt / n, drawOther / n, ms(uiPt), ms(uiOther), ms(pose), ms(lightning),
        mapPt / n, mapOther / n, ms(mapTPt), ms(mapTOther),
        stores / n, storeBytes / 1024.0 / n, stores ? 100.0 * double(storeWc) / double(stores) : 0.0, ms(storeTPt), ms(storeTOther),
        feReplays / n, ms(feTicks), vsPt / n, vsOther / n, cmd / n, tscPerMs / 1000.0);
    RetailLogLine(line);
    g_frames = 0; g_sumInterval = g_sumPresent = g_sumPre = g_sumOutside = 0.0; g_maxInterval = 0.0;
    g_windowStartQpc = afterPresentQpc; g_windowStartTsc = Tsc();
}
} // namespace PerfCensus
