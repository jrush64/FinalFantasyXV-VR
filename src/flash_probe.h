#pragma once
// [FLASHPROBE] Residual left-eye light flashes in Magitek battles.
//
// Engine output IDs are not proven eye IDs.
// Reads bounded CPU uploads after arming an effect buffer, and reports
// draw API, upload coverage, anomalies, and guard rejection counts.
// Anomalies are recorded before the guard and do not prove a bad draw escaped.
// Numpad 7 pauses/resumes recording (guard stays active), with no timeout.
// Numpad 8 cycles experimental output suppression; leave at 0 for normal play.
// Numpad 9 adds a user mark and immediate summary.
// Recording defaults OFF (no per-draw cost in normal play); ini
// [Probe] Flash=1 or Numpad 7 starts it.
#include <d3d11.h>
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "aer_control.h"
#include "runtime_log.h"
#include "lightning_geometry_fix.h"
#include "flash_upload.h"

namespace FlashProbe
{
using ReadFn = bool (*)(ID3D11Resource*, std::uint32_t offset, std::uint8_t* out, std::uint32_t n);

inline std::atomic<int> g_enabled{-1};
inline std::atomic<int> g_dropMode{};
inline bool On()
{
    int e = g_enabled.load(std::memory_order_relaxed);
    if (e >= 0) return e != 0;
    wchar_t path[MAX_PATH]{};
    const auto n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
    e = 1;
    if (slash)
    {
        wcscpy_s(slash + 1, MAX_PATH - static_cast<std::size_t>(slash + 1 - path), L"ffxv-vr.ini");
        e = GetPrivateProfileIntW(L"Probe", L"Flash", 0, path) != 0 ? 1 : 0;
    }
    g_enabled.store(e, std::memory_order_relaxed);
    char line[240];
    sprintf_s(line, "[FLASHPROBE] v3 %s: effect CPU uploads readable; independent guard-neutral tracking; Numpad7 = pause/resume; Numpad8 = drop effects; Numpad9 = mark; no timeout", e ? "ON" : "off");
    RetailLogLine(line);
    return e != 0;
}

// ---- fill-path census per buffer ----
enum Fill : unsigned { FillMapImm, FillMapDef, FillUpdate, FillCopy, FillCopyRegion, FillCount };
struct FillEntry
{
    std::atomic<std::uintptr_t> key{};
    std::atomic<std::uint32_t> counts[FillCount]{};
    std::atomic<std::uintptr_t> lastSrc{};   // last CopyResource / CopySubresourceRegion source
    std::atomic<std::uint32_t> lastSrcLeft{}, lastDstX{};
};
inline FillEntry g_fills[512];
inline FillEntry* FillFor(ID3D11Resource* r, bool create)
{
    const auto key = reinterpret_cast<std::uintptr_t>(r);
    const std::size_t start = static_cast<std::size_t>(((key >> 4) ^ (key >> 13)) & 511);
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& e = g_fills[(start + probe) & 511];
        auto existing = e.key.load(std::memory_order_acquire);
        if (existing == key) return &e;
        if (existing == 0)
        {
            if (!create) return nullptr;
            if (e.key.compare_exchange_strong(existing, key, std::memory_order_acq_rel)) return &e;
            if (existing == key) return &e;
        }
    }
    return nullptr;
}
// Only buffers already seen at an effect draw are tracked (the fill hooks
// run on every Map in the game; a table insert per Map would be noise).
inline void OnFill(ID3D11Resource* r, Fill kind, ID3D11Resource* src = nullptr, std::uint32_t srcLeft = 0, std::uint32_t dstX = 0)
{
    if (!r || g_enabled.load(std::memory_order_relaxed) != 1) return;
    if(kind!=FillUpdate) FlashUpload::Invalidate(r);
    auto* e = FillFor(r, false);
    if (!e) return;
    e->counts[kind].fetch_add(1, std::memory_order_relaxed);
    if (src)
    {
        e->lastSrc.store(reinterpret_cast<std::uintptr_t>(src), std::memory_order_relaxed);
        e->lastSrcLeft.store(srcLeft, std::memory_order_relaxed);
        e->lastDstX.store(dstX, std::memory_order_relaxed);
    }
}

inline std::uint64_t ShaderHash(ID3D11DeviceChild* shader)
{
    std::uint64_t hash = 0;
    UINT size = sizeof(hash);
    if (shader && SUCCEEDED(shader->GetPrivateData(LightningGeometryFix::kHashTag, &size, &hash)) && size == sizeof(hash))
        return hash;
    return 0;
}
inline void MaxBits(std::atomic<std::uint32_t>& slot, float value)
{
    if (!(value >= 0.0f)) return;
    std::uint32_t bits{};
    std::memcpy(&bits, &value, 4);
    auto current = slot.load(std::memory_order_relaxed);
    while (bits > current && !slot.compare_exchange_weak(current, bits, std::memory_order_relaxed)) {}
}
inline float BitsToFloat(std::uint32_t bits) { float f{}; std::memcpy(&f, &bits, 4); return f; }

struct Entry
{
    std::atomic<std::uint64_t> key{};
    std::uint64_t vs{}, ps{};
    std::uint32_t topology{}, indexed{}, deferred{}, guarded{}, instanced{};
    std::atomic<std::uint64_t> draws[2]{}, vertices[2]{}, dropped[2]{};
    std::atomic<std::uint64_t> fromGuard{}, fromMap{}, fromCopySrc{}, fromUpload{}, unread{};
    std::atomic<std::uint32_t> fills[FillCount]{};
    std::atomic<std::uint64_t> anomalies[2]{};
    std::atomic<std::uint32_t> maxPos[2]{}, maxColour[2]{}, maxAlpha[2]{};
    // render target at the draw, per output: first seen this window
    std::atomic<std::uint32_t> rtFormat[2]{}, rtWidth[2]{}, rtHeight[2]{};
    std::atomic<std::uintptr_t> rtIdentity[2]{};
    std::atomic<std::uint32_t> rtChanges[2]{};
};
inline Entry g_table[96];
inline std::atomic<std::uint64_t> g_tableFull{};
inline std::atomic<std::uint32_t> g_anomalyLines{};
inline std::atomic<std::uint64_t> g_nextReport{};
inline std::atomic<std::uint64_t> g_marks{};

inline Entry* Find(std::uint64_t vs, std::uint64_t ps, std::uint32_t topology, bool indexed, bool deferred, bool guarded, bool instanced)
{
    std::uint64_t key = vs * 0x9E3779B97F4A7C15ull ^ ps * 0xC2B2AE3D27D4EB4Full ^
        (static_cast<std::uint64_t>(topology) << 3) ^ (indexed ? 1ull : 0ull) ^ (deferred ? 2ull : 0ull) ^ (guarded ? 4ull : 0ull) ^ (instanced ? (1ull<<32) : 0ull);
    if (!key) key = 1;
    const std::size_t start = static_cast<std::size_t>(key % 96);
    for (std::size_t probe = 0; probe < 96; ++probe)
    {
        Entry& e = g_table[(start + probe) % 96];
        auto existing = e.key.load(std::memory_order_acquire);
        if (existing == key) return &e;
        if (existing == 0)
        {
            static SRWLOCK lock = SRWLOCK_INIT;
            AcquireSRWLockExclusive(&lock);
            existing = e.key.load(std::memory_order_acquire);
            if (existing == 0)
            {
                e.vs = vs; e.ps = ps; e.topology = topology; e.indexed = indexed; e.deferred = deferred; e.guarded = guarded; e.instanced = instanced;
                e.key.store(key, std::memory_order_release);
                ReleaseSRWLockExclusive(&lock);
                return &e;
            }
            ReleaseSRWLockExclusive(&lock);
            if (existing == key) return &e;
        }
    }
    g_tableFull.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

inline void Report(const char* why)
{
    char line[640];
    sprintf_s(line,"[FLASHPROBE] uploads armed=%llu copied=%llu bytes=%llu oversize=%llu rangeMiss=%llu guardRejected=%llu",FlashUpload::armed.load(),FlashUpload::copied.load(),FlashUpload::copiedBytes.load(),FlashUpload::oversize.load(),FlashUpload::rangeMiss.load(),LightningGeometryFix::g_rejectedDraws.load());
    RetailLogLine(line);
    unsigned printed = 0;
    for (auto& e : g_table)
    {
        if (!e.key.load(std::memory_order_acquire)) continue;
        const auto d0 = e.draws[0].exchange(0), d1 = e.draws[1].exchange(0);
        const auto x0 = e.dropped[0].exchange(0), x1 = e.dropped[1].exchange(0);
        const auto a0 = e.anomalies[0].exchange(0), a1 = e.anomalies[1].exchange(0);
        const auto v0 = e.vertices[0].exchange(0), v1 = e.vertices[1].exchange(0);
        const auto up = e.fromUpload.exchange(0);
        const auto g = e.fromGuard.exchange(0), m = e.fromMap.exchange(0), cs = e.fromCopySrc.exchange(0), u = e.unread.exchange(0);
        std::uint32_t f[FillCount]; for (unsigned i = 0; i < FillCount; ++i) f[i] = e.fills[i].exchange(0);
        const float p0 = BitsToFloat(e.maxPos[0].exchange(0)), p1 = BitsToFloat(e.maxPos[1].exchange(0));
        const float c0 = BitsToFloat(e.maxColour[0].exchange(0)), c1 = BitsToFloat(e.maxColour[1].exchange(0));
        const float al0 = BitsToFloat(e.maxAlpha[0].exchange(0)), al1 = BitsToFloat(e.maxAlpha[1].exchange(0));
        std::uint32_t rf[2], rw[2], rh[2], rc[2]; std::uintptr_t ri[2];
        for (int o = 0; o < 2; ++o) { rf[o] = e.rtFormat[o].exchange(0); rw[o] = e.rtWidth[o].exchange(0); rh[o] = e.rtHeight[o].exchange(0); ri[o] = e.rtIdentity[o].exchange(0); rc[o] = e.rtChanges[o].exchange(0); }
        if (!d0 && !d1) continue;
        if (printed++ >= 20) continue;
        sprintf_s(line, "[FLASHPROBE] class vs=%016llX ps=%016llX topo=%u idx=%u inst=%u def=%u guardPair=%u | out0 draws=%llu drop=%llu verts=%llu anom=%llu maxPos=%.3g maxCol=%.3g maxA=%.3g rt=fmt%u %ux%u id=%llX chg=%u | out1 draws=%llu drop=%llu verts=%llu anom=%llu maxPos=%.3g maxCol=%.3g maxA=%.3g rt=fmt%u %ux%u id=%llX chg=%u | data guard=%llu map=%llu copySrc=%llu unread=%llu upload=%llu | fills mapImm=%u mapDef=%u update=%u copy=%u region=%u",
            static_cast<unsigned long long>(e.vs), static_cast<unsigned long long>(e.ps), e.topology, e.indexed, e.instanced, e.deferred, e.guarded,
            static_cast<unsigned long long>(d0), static_cast<unsigned long long>(x0), static_cast<unsigned long long>(v0), static_cast<unsigned long long>(a0), p0, c0, al0, rf[0], rw[0], rh[0], static_cast<unsigned long long>(ri[0]), rc[0],
            static_cast<unsigned long long>(d1), static_cast<unsigned long long>(x1), static_cast<unsigned long long>(v1), static_cast<unsigned long long>(a1), p1, c1, al1, rf[1], rw[1], rh[1], static_cast<unsigned long long>(ri[1]), rc[1],
            static_cast<unsigned long long>(g), static_cast<unsigned long long>(m), static_cast<unsigned long long>(cs), static_cast<unsigned long long>(u), static_cast<unsigned long long>(up),
            f[FillMapImm], f[FillMapDef], f[FillUpdate], f[FillCopy], f[FillCopyRegion]);
        RetailLogLine(line);
    }
    if (printed)
    {
        sprintf_s(line, "[FLASHPROBE] summary (%s): %u active classes%s dropMode=%d tableFull=%llu", why, printed,
            printed > 20 ? " (first 20 shown)" : "", g_dropMode.load(), static_cast<unsigned long long>(g_tableFull.load()));
        RetailLogLine(line);
    }
}

inline void ToggleRecording()
{
    const bool enable = !On();
    if(!enable) Report("paused");
    g_enabled.store(enable?1:0,std::memory_order_relaxed);
    g_dropMode.store(0); // pause/resume always restores normal effects
    for(auto& slot:FlashUpload::tracked)slot.store(0); // reacquire fresh uploads on resume
    g_anomalyLines.store(0);
    g_nextReport.store(GetTickCount64()+5000);
    RetailLogLine(enable?"[FLASHPROBE] recording ON (Numpad7); no timeout; effect drop OFF":"[FLASHPROBE] recording PAUSED (Numpad7); rendering guard remains ON; effect drop OFF");
}
inline void OnPresent()
{
    On(); // initialize startup default even when paused in INI
    static bool wasDown7=false;
    const bool down7=(GetAsyncKeyState(VK_NUMPAD7)&0x8000)!=0;
    if(down7 && !wasDown7 && AerControl::GetDeveloperKeysEnabled()){ToggleRecording();Beep(g_enabled.load()?1500:700,80);}
    wasDown7=down7;
    if (!On()) return;
    static bool wasDown9 = false, wasDown8 = false;
    const bool down9 = (GetAsyncKeyState(VK_NUMPAD9) & 0x8000) != 0;
    if (down9 && !wasDown9 && AerControl::GetDeveloperKeysEnabled())
    {
        char line[96];
        sprintf_s(line, "[FLASHPROBE] ===== USER MARK %llu (flash seen) =====", static_cast<unsigned long long>(g_marks.fetch_add(1) + 1));
        RetailLogLine(line);
        Beep(1600, 80);
        Report("user mark");
        g_nextReport.store(GetTickCount64() + 5000);
    }
    wasDown9 = down9;
    const bool down8 = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    if (down8 && !wasDown8 && AerControl::GetDeveloperKeysEnabled())
    {
        const int mode = (g_dropMode.load() + 1) & 3;
        g_dropMode.store(mode);
        static const char* names[4] = {"off (all effect draws drawn)", "DROP effect draws on output 1", "DROP effect draws on output 0", "DROP all effect draws"};
        char line[128];
        sprintf_s(line, "[FLASHPROBE] ===== Numpad 8: kill switch %d = %s =====", mode, names[mode]);
        RetailLogLine(line);
        Beep(900 + 200 * mode, 100);
    }
    wasDown8 = down8;
    const auto now = GetTickCount64();
    auto due = g_nextReport.load(std::memory_order_relaxed);
    if (now >= due && g_nextReport.compare_exchange_strong(due, now + 5000))
        Report("5 s window");
}

// Before a non-mod draw.  Returns true when the draw must be DROPPED (kill switch).
inline bool OnDraw(ID3D11DeviceContext* c, UINT count, UINT start, INT baseVertex, bool indexed, int output, ReadFn readMapped, bool instanced=false)
{
    if (!c || count < 2 || !On()) return false;
    ID3D11Buffer* vb{}; UINT stride{}, offset{};
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return false;
    if (stride != 36) { vb->Release(); return false; }
    ID3D11InputLayout* input{};
    c->IAGetInputLayout(&input);
    LightningGeometryFix::Layout layout{};
    UINT layoutBytes = sizeof(layout);
    const bool effectLayout = input && SUCCEEDED(input->GetPrivateData(LightningGeometryFix::kLayoutTag, &layoutBytes, &layout)) &&
        layout.positionFormat == DXGI_FORMAT_R32G32B32_FLOAT && layout.positionSlot == 0 && layout.positionOffset == 0 &&
        layout.colourFormat == DXGI_FORMAT_R32G32B32A32_FLOAT && layout.colourSlot == 0 && layout.colourOffset == 12;
    if (input) input->Release();
    if (!effectLayout) { vb->Release(); return false; }

    const int eye = output & 1;
    const int drop = g_dropMode.load(std::memory_order_relaxed);
    const bool dropThis = drop == 3 || (drop == 1 && eye == 1) || (drop == 2 && eye == 0);

    D3D11_PRIMITIVE_TOPOLOGY topology{};
    c->IAGetPrimitiveTopology(&topology);
    ID3D11VertexShader* vs{}; ID3D11PixelShader* ps{};
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    const std::uint64_t vsHash = ShaderHash(vs), psHash = ShaderHash(ps);
    const bool guarded = LightningGeometryFix::Tagged(vs, 1) && LightningGeometryFix::Tagged(ps, 2);
    if (vs) vs->Release();
    if (ps) ps->Release();
    const bool deferred = c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE;
    if(!indexed && !deferred && (topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP || topology==D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)) FlashUpload::Arm(vb);
    Entry* e = Find(vsHash, psHash, static_cast<std::uint32_t>(topology), indexed, deferred, guarded, instanced);
    FillEntry* fill = FillFor(vb, true);

    // Render target at this draw.
    if (e)
    {
        ID3D11RenderTargetView* rtv{}; ID3D11DepthStencilView* dsv{};
        c->OMGetRenderTargets(1, &rtv, &dsv);
        if (dsv) dsv->Release();
        if (rtv)
        {
            ID3D11Resource* res{}; rtv->GetResource(&res);
            if (res)
            {
                const auto id = reinterpret_cast<std::uintptr_t>(res);
                std::uintptr_t expected = 0;
                if (!e->rtIdentity[eye].compare_exchange_strong(expected, id, std::memory_order_relaxed) && expected != id)
                    e->rtChanges[eye].fetch_add(1, std::memory_order_relaxed);
                if (expected == 0)
                {
                    D3D11_RENDER_TARGET_VIEW_DESC rd{}; rtv->GetDesc(&rd);
                    e->rtFormat[eye].store(static_cast<std::uint32_t>(rd.Format), std::memory_order_relaxed);
                    ID3D11Texture2D* tex{};
                    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
                    {
                        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td); tex->Release();
                        e->rtWidth[eye].store(td.Width, std::memory_order_relaxed);
                        e->rtHeight[eye].store(td.Height, std::memory_order_relaxed);
                    }
                }
                res->Release();
            }
            rtv->Release();
        }
    }

    // Vertex bytes: guard copy, retained mapping of the buffer, or the retained
    // mapping of the last copy SOURCE (staging ring) at the copied offset.
    const UINT first = indexed ? static_cast<UINT>(baseVertex < 0 ? 0 : baseVertex) : start;
    const UINT n = count > 1024 ? 1024u : count;
    thread_local std::uint8_t bytes[1024 * 36];
    bool have = false; int source = 0; // 1 guard, 2 map, 3 copy source
    if (!indexed && first <= (UINT_MAX - offset) / 36)
    {
        const UINT byteOffset = offset + first * 36;
        if(FlashUpload::Read(vb,byteOffset,bytes,n*36)){have=true;source=4;}
        if (!have) if (auto* u = LightningGeometryFix::Get(vb))
        {
            AcquireSRWLockShared(&u->lock);
            if (u->known && byteOffset >= u->offset && byteOffset - u->offset <= u->bytes.size() &&
                n * 36 <= u->bytes.size() - (byteOffset - u->offset))
            {
                std::memcpy(bytes, u->bytes.data() + (byteOffset - u->offset), n * 36);
                have = true; source = 1;
            }
            ReleaseSRWLockShared(&u->lock);
            u->Release();
        }
        if (!have && readMapped && readMapped(vb, byteOffset, bytes, n * 36)) { have = true; source = 2; }
        if (!have && readMapped && fill)
        {
            const auto src = fill->lastSrc.load(std::memory_order_relaxed);
            const auto dstX = fill->lastDstX.load(std::memory_order_relaxed);
            if (src && byteOffset >= dstX &&
                readMapped(reinterpret_cast<ID3D11Resource*>(src), fill->lastSrcLeft.load(std::memory_order_relaxed) + (byteOffset - dstX), bytes, n * 36))
            { have = true; source = 3; }
        }
    }
    vb->Release();
    if (e)
    {
        e->draws[eye].fetch_add(1, std::memory_order_relaxed);
        e->vertices[eye].fetch_add(count, std::memory_order_relaxed);
        if (dropThis) e->dropped[eye].fetch_add(1, std::memory_order_relaxed);
        (source == 4 ? e->fromUpload : source == 1 ? e->fromGuard : source == 2 ? e->fromMap : source == 3 ? e->fromCopySrc : e->unread).fetch_add(1, std::memory_order_relaxed);
        if (fill)
            for (unsigned i = 0; i < FillCount; ++i)
            {
                const auto v = fill->counts[i].exchange(0, std::memory_order_relaxed);
                if (v) e->fills[i].fetch_add(v, std::memory_order_relaxed);
            }
    }
    if (!have) return dropThis;

    float maxPos = 0.0f, maxColour = 0.0f, maxAlpha = 0.0f;
    bool nonFinite = false, negative = false;
    for (UINT i = 0; i < n; ++i)
    {
        float v[7];
        std::memcpy(v, bytes + i * 36, sizeof(v));
        for (int j = 0; j < 7; ++j)
        {
            if (!std::isfinite(v[j])) { nonFinite = true; continue; }
            const float a = std::fabs(v[j]);
            if (j < 3) maxPos = a > maxPos ? a : maxPos;
            else if (j < 6) { maxColour = v[j] > maxColour ? v[j] : maxColour; negative |= v[j] < -0.0001f; }
            else { maxAlpha = v[j] > maxAlpha ? v[j] : maxAlpha; negative |= v[j] < -0.0001f; }
        }
    }
    if (e)
    {
        MaxBits(e->maxPos[eye], maxPos);
        MaxBits(e->maxColour[eye], maxColour);
        MaxBits(e->maxAlpha[eye], maxAlpha);
    }
    const bool anomaly = nonFinite || negative || maxPos > 1.0e5f || maxColour > 20.0f || maxAlpha > 1.0001f;
    if (anomaly)
    {
        if (e) e->anomalies[eye].fetch_add(1, std::memory_order_relaxed);
        if (g_anomalyLines.fetch_add(1, std::memory_order_relaxed) < 64)
        {
            char line[360];
            sprintf_s(line, "[FLASHPROBE] ANOMALY out=%d vs=%016llX ps=%016llX topo=%u idx=%d inst=%d def=%d guardPair=%d count=%u start=%u base=%d src=%s nonFinite=%d negative=%d maxPos=%.4g maxCol=%.4g maxA=%.4g",
                eye, static_cast<unsigned long long>(vsHash), static_cast<unsigned long long>(psHash), static_cast<unsigned>(topology),
                indexed ? 1 : 0, instanced ? 1 : 0, deferred ? 1 : 0, guarded ? 1 : 0, count, start, baseVertex, source == 4 ? "upload" : source == 1 ? "guard" : source == 2 ? "map" : "copySrc",
                nonFinite ? 1 : 0, negative ? 1 : 0, maxPos, maxColour, maxAlpha);
            RetailLogLine(line);
        }
    }
    return dropThis;
}
}
