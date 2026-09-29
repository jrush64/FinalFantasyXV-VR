#include "research_diagnostics.h"
#include "fps_ab_probe.h"
#include "runtime_log.h"

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace
{
enum Mode : int { kNative = 0, kNativeOneView = 1, kAer = 2, kMono = 3, kModeCount = 4 };
const char* ModeName(int mode)
{
    switch (mode)
    {
    case kNative: return "native";
    case kNativeOneView: return "native1";
    case kAer: return "aer";
    default: return "mono";
    }
}

constexpr std::size_t kWindow = 600;
constexpr std::size_t kMinReport = 120;
constexpr std::size_t kQueryRing = 8;
constexpr std::uint8_t kNoLag = 255;

struct Sample { float interval, work, wait; std::uint8_t lag; };

struct ModeStats
{
    std::array<Sample, kWindow> ring{};
    std::size_t count{};          // samples in the current window
    std::uint64_t totalFrames{};
    double totalWallMs{};
    std::uint64_t lagSamples{};
    std::uint64_t lagSum{};
    std::uint64_t lagAtLeast2{};
};
ModeStats g_modes[kModeCount];

bool g_enabled = true;
bool g_initDone = false;
int g_lastMode = -1;
long long g_lastEntryQpc{};
std::uint64_t g_presentIndex{};
std::uint64_t g_skippedFlat{};
UINT g_backbufferW{}, g_backbufferH{};

struct PendingQuery
{
    ID3D11Query* query{};
    std::uint64_t presentIndex{};
    bool inFlight{};
};
std::array<PendingQuery, kQueryRing> g_queries{};
ID3D11Device* g_device{};
ID3D11DeviceContext* g_context{};
bool g_queryFailed = false;
std::uint64_t g_querySaturated{};
// Lag of the most recently completed query, attached to the next recorded sample.
int g_lastLag = -1;

void Logf(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    RetailLogLine(line);
}

double QpcMs(long long ticks)
{
    static const double frequency = []
    {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 1.0;
    }();
    return static_cast<double>(ticks) * 1000.0 / frequency;
}

void Init()
{
    if (g_initDone)
        return;
    g_initDone = true;
    wchar_t path[MAX_PATH]{};
    HMODULE self{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Init), &self) &&
        GetModuleFileNameW(self, path, MAX_PATH))
    {
        wchar_t* slash = std::wcsrchr(path, L'\\');
        if (slash)
        {
            std::wcsncpy(slash + 1, L"ffxv-vr.ini", MAX_PATH - (slash + 1 - path));
            path[MAX_PATH - 1] = L'\0';
            g_enabled = GetPrivateProfileIntW(L"Probe", L"FpsAb", 1, path) != 0;
        }
    }
    Logf("[FPSAB] frame-cost A/B probe %s (ini [Probe] FpsAb). Protocol: stand still in one "
         "spot, about 20 s per mode: F9 native stereo, then Insert menu -> AER, then both off "
         "(mono). Read the totals line: fps per mode at one backbuffer. native1 = native stereo "
         "frames the engine rendered as ONE output (AUTOMONO); they are kept apart so they "
         "cannot inflate the native average. gpuLag 3 with rising work = GPU-bound.",
        g_enabled ? "ON" : "off");
}

void EnsureQueries(IDXGISwapChain* swapChain)
{
    if (g_device || g_queryFailed || !swapChain)
        return;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) ||
        !g_device)
    {
        g_device = nullptr;
        g_queryFailed = true;
        Logf("[FPSAB] swapchain device unavailable; gpuLag disabled");
        return;
    }
    g_device->GetImmediateContext(&g_context);
    if (!g_context)
    {
        g_queryFailed = true;
        Logf("[FPSAB] immediate context unavailable; gpuLag disabled");
        return;
    }
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_TIMESTAMP;
    for (auto& pending : g_queries)
    {
        if (FAILED(g_device->CreateQuery(&desc, &pending.query)) || !pending.query)
        {
            pending.query = nullptr;
            g_queryFailed = true;
            Logf("[FPSAB] timestamp query creation failed; gpuLag disabled");
            return;
        }
    }
    ID3D11Texture2D* backBuffer{};
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backBuffer))) && backBuffer)
    {
        D3D11_TEXTURE2D_DESC bb{};
        backBuffer->GetDesc(&bb);
        g_backbufferW = bb.Width;
        g_backbufferH = bb.Height;
        backBuffer->Release();
    }
    Logf("[FPSAB] gpuLag queries ready (%zu in flight max) backbuffer=%ux%u",
        kQueryRing, g_backbufferW, g_backbufferH);
}

void PollQueries()
{
    if (!g_context || g_queryFailed)
        return;
    for (auto& pending : g_queries)
    {
        if (!pending.inFlight)
            continue;
        std::uint64_t stamp{};
        const HRESULT hr = g_context->GetData(pending.query, &stamp, sizeof(stamp),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK)
        {
            pending.inFlight = false;
            const auto lag = g_presentIndex - pending.presentIndex;
            g_lastLag = static_cast<int>(std::min<std::uint64_t>(lag, 250));
        }
        else if (hr != S_FALSE)
        {
            pending.inFlight = false; // device lost or query error: drop it
        }
    }
}

// Megapixels shaded per frame at this backbuffer.  Native stereo renders two
// half-width views into the one backbuffer; AER and mono render the whole
// backbuffer as one view.  Same pixel count either way, so an fps difference
// between native and AER at one backbuffer is the per-view cost (draw
// submission, culling, per-eye passes), not a pixel-count difference.
double MegapixelsPerFrame()
{
    return static_cast<double>(g_backbufferW) * static_cast<double>(g_backbufferH) / 1.0e6;
}

void Report(int mode, const char* why)
{
    auto& stats = g_modes[mode];
    if (stats.count < kMinReport)
    {
        stats.count = 0;
        return;
    }
    auto percentile = [&](float Sample::*field, double fraction)
    {
        std::array<float, kWindow> values{};
        for (std::size_t index = 0; index < stats.count; ++index)
            values[index] = stats.ring[index].*field;
        const auto rank = static_cast<std::size_t>(fraction * (stats.count - 1));
        std::nth_element(values.begin(), values.begin() + rank, values.begin() + stats.count);
        return values[rank];
    };
    std::array<std::uint8_t, kWindow> lags{};
    std::size_t lagCount = 0;
    std::size_t lagGe2 = 0;
    int lagMax = 0;
    for (std::size_t index = 0; index < stats.count; ++index)
    {
        const auto lag = stats.ring[index].lag;
        if (lag == kNoLag)
            continue;
        lags[lagCount++] = lag;
        if (lag >= 2)
            ++lagGe2;
        lagMax = std::max(lagMax, static_cast<int>(lag));
    }
    int lagP50 = -1;
    if (lagCount)
    {
        const auto rank = lagCount / 2;
        std::nth_element(lags.begin(), lags.begin() + rank, lags.begin() + lagCount);
        lagP50 = lags[rank];
    }
    double wallMs = 0.0;
    for (std::size_t index = 0; index < stats.count; ++index)
        wallMs += stats.ring[index].interval;
    const double fps = wallMs > 0.0 ? stats.count * 1000.0 / wallMs : 0.0;
    Logf("[FPSAB] %s window(%s): frames=%zu fps=%.1f interval p50/p95=%.2f/%.2f ms "
         "work p50/p95=%.2f/%.2f wait p50/p95=%.2f/%.2f gpuLag p50/max=%d/%d ge2=%.0f%% "
         "backbuffer=%ux%u (%.1f Mpx/frame)",
        ModeName(mode), why, stats.count, fps,
        percentile(&Sample::interval, 0.5), percentile(&Sample::interval, 0.95),
        percentile(&Sample::work, 0.5), percentile(&Sample::work, 0.95),
        percentile(&Sample::wait, 0.5), percentile(&Sample::wait, 0.95),
        lagP50, lagMax, lagCount ? 100.0 * lagGe2 / lagCount : 0.0,
        g_backbufferW, g_backbufferH, MegapixelsPerFrame());
    char totals[1024];
    int used = std::snprintf(totals, sizeof(totals), "[FPSAB] totals:");
    for (int m = 0; m < kModeCount && used > 0 && used < static_cast<int>(sizeof(totals)); ++m)
    {
        const auto& t = g_modes[m];
        const double tfps = t.totalWallMs > 0.0 ? t.totalFrames * 1000.0 / t.totalWallMs : 0.0;
        const int wrote = std::snprintf(totals + used, sizeof(totals) - used,
            " %s frames=%llu wall=%.1fs fps=%.1f gpuLag avg=%.2f ge2=%.0f%% |",
            ModeName(m), static_cast<unsigned long long>(t.totalFrames), t.totalWallMs / 1000.0,
            tfps, t.lagSamples ? static_cast<double>(t.lagSum) / t.lagSamples : 0.0,
            t.lagSamples ? 100.0 * t.lagAtLeast2 / t.lagSamples : 0.0);
        if (wrote < 0)
            break;
        used += wrote;
    }
    if (used > 0 && used < static_cast<int>(sizeof(totals)))
    {
        std::snprintf(totals + used, sizeof(totals) - used,
            " flatSkipped=%llu querySaturated=%llu",
            static_cast<unsigned long long>(g_skippedFlat),
            static_cast<unsigned long long>(g_querySaturated));
    }
    totals[sizeof(totals) - 1] = '\0';
    RetailLogLine(totals);
    stats.count = 0;
}
} // namespace

namespace FpsAbProbe
{
void BeforePresent(IDXGISwapChain* swapChain)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    Init();
    if (!g_enabled)
        return;
    EnsureQueries(swapChain);
    if (!g_context || g_queryFailed)
        return;
    for (auto& pending : g_queries)
    {
        if (pending.inFlight)
            continue;
        g_context->End(pending.query);
        pending.presentIndex = g_presentIndex;
        pending.inFlight = true;
        return;
    }
    ++g_querySaturated; // eight frames of GPU work outstanding: deeply GPU-bound
}

void AfterPresent(bool nativeStereo, bool aer, bool sceneFlat, bool monoFallback,
    long long entryQpc, long long beforePresentQpc, long long afterPresentQpc,
    long long xrWaitMicros)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_enabled)
        return;
    ++g_presentIndex;
    PollQueries();
    const int mode = nativeStereo ? (monoFallback ? kNativeOneView : kNative)
                                  : (aer ? kAer : kMono);
    const long long previousEntry = g_lastEntryQpc;
    g_lastEntryQpc = entryQpc;
    if (mode != g_lastMode)
    {
        if (g_lastMode >= 0)
            Report(g_lastMode, "mode change");
        Logf("[FPSAB] mode -> %s", ModeName(mode));
        g_lastMode = mode;
        g_lastLag = -1;
        return; // the first interval spans the switch; drop it
    }
    if (sceneFlat)
    {
        ++g_skippedFlat;
        return;
    }
    if (!previousEntry)
        return;
    const double interval = QpcMs(entryQpc - previousEntry);
    if (interval <= 0.0 || interval > 1000.0)
        return; // a stall or a clock hiccup, not a frame
    const double present = QpcMs(afterPresentQpc - beforePresentQpc);
    const double xrWait = static_cast<double>(xrWaitMicros) / 1000.0;
    Sample sample{};
    sample.interval = static_cast<float>(interval);
    sample.wait = static_cast<float>(present + xrWait);
    sample.work = static_cast<float>(std::max(0.0, interval - sample.wait));
    sample.lag = g_lastLag >= 0 ? static_cast<std::uint8_t>(std::min(g_lastLag, 250)) : kNoLag;
    g_lastLag = -1;
    auto& stats = g_modes[mode];
    stats.ring[stats.count++] = sample;
    ++stats.totalFrames;
    stats.totalWallMs += interval;
    if (sample.lag != kNoLag)
    {
        ++stats.lagSamples;
        stats.lagSum += sample.lag;
        if (sample.lag >= 2)
            ++stats.lagAtLeast2;
    }
    if (stats.count >= kWindow)
        Report(mode, "600 presents");
}
}
