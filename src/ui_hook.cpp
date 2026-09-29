#include "mapped_copy.h"
#include "perf_census.h"
#include "dlss_probe.h"
#include "dlss_upscaler.h"
#if defined(GPU_POSE_TRACE_TEST)
static constexpr bool kPoseTraceEnabled = true;
#else
static constexpr bool kPoseTraceEnabled = false;
#endif
#include "full_eye_capture.h"
#include "lightning_geometry_fix.h"
#include "flash_probe.h"
#include "mv_fix.h"
#include "gpu_pose_trace.h"
#include "native_draw_pose.h"
#include "gbuffer_census.h"
#include <d3d11_1.h>
#include "research_diagnostics.h"
#include "ui_hook.h"
#include "ui_world_routing.h"
#include "ui_marker_assets.h"
#include "ui_world_probe.h"
#include "ui_objective_positions.h"
#include "target_world.h"
#include "ui_remaining_probe.h"

#include <windows.h>
#include <d3d11.h>

#include "MinHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace UiHook
{
namespace
{
LogFn g_log{};
ReadOutputFn g_readOutput{};
std::atomic_bool g_installed{};
// Identity only - never dereferenced through, never released.
ID3D11DeviceContext* g_immediate{};
ID3D11DeviceContext* g_deferredProbe{};

using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn =
    void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using ExecuteCommandListFn =
    void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using FinishCommandListFn =
    HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using RSSetStateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using OMSetRenderTargetsAndUavFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*, const UINT*);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
    ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

// Lane indices (array slots) and the bits recorded per shader.
enum Lane : std::uint32_t
{
    LaneDraw = 0,
    LaneDrawIndexed,
    LaneDrawIndexedInstanced,
    LaneDrawInstanced,
    LaneDrawAuto,
    LaneIndexedIndirect,
    LaneIndirect,
    LaneCount
};
constexpr const char* kLaneNames[LaneCount] = {
    "Draw", "DrawIndexed", "DrawIdxInst", "DrawInst", "DrawAuto", "IdxIndirect", "Indirect"};

// Two vtable variants: d3d11 instantiates immediate and deferred contexts
// separately, and their method bodies may differ.  Variant 0 = the vtable of
// the game's immediate context, 1 = a deferred context's, hooked only for the
// slots whose implementation differs from variant 0.
struct Originals
{
    DrawIndexedFn drawIndexed{};
    DrawFn draw{};
    DrawIndexedInstancedFn drawIndexedInstanced{};
    DrawInstancedFn drawInstanced{};
    DrawAutoFn drawAuto{};
    DrawIndirectFn drawIndexedInstancedIndirect{};
    DrawIndirectFn drawInstancedIndirect{};
    ExecuteCommandListFn executeCommandList{};
    FinishCommandListFn finishCommandList{};
    RSSetStateFn rsSetState{};
    OMSetRenderTargetsFn omSetRenderTargets{};
    OMSetRenderTargetsAndUavFn omSetRenderTargetsAndUav{};
    ClearDepthStencilViewFn clearDepthStencilView{};
    HRESULT (STDMETHODCALLTYPE* map)(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out){};
    void (STDMETHODCALLTYPE* unmap)(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s){};
    void (STDMETHODCALLTYPE* copyRegion)(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* box){};
    void (STDMETHODCALLTYPE* copyResource)(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s){};
    void (STDMETHODCALLTYPE* update)(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* box, const void* data, UINT row, UINT depth){};
    void (STDMETHODCALLTYPE* copyRegion1)(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* box, UINT flags){};
    void (STDMETHODCALLTYPE* update1)(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* box, const void* data, UINT row, UINT depth, UINT flags){};
    void (STDMETHODCALLTYPE* dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT){};
    void (STDMETHODCALLTYPE* psSetSamplers)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*){};
    DrawIndirectFn dispatchIndirect{};   // same signature as the indirect draws
};
Originals g_orig[2]{};

// [WIRE] Is a "wireframe" frame literally a D3D11_FILL_WIREFRAME rasterizer
// state?  Every RSSetState is checked (desc cached per state object) and the
// count of wireframe binds per present is reported.
std::atomic_uint32_t g_wireSetsThisFrame{};
std::atomic_uint64_t g_wireFramesWindow{};
std::atomic_uintptr_t g_wireLastState{};
std::atomic_uint64_t g_wireLogged{};
constexpr std::size_t kWireCacheSlots = 64;
std::atomic_uintptr_t g_wireCacheState[kWireCacheSlots]{};
std::atomic_int g_wireCacheFill[kWireCacheSlots]{};

bool RasterizerIsWireframe(ID3D11RasterizerState* state)
{
    const auto key = reinterpret_cast<std::uintptr_t>(state);
    for (std::size_t i = 0; i < kWireCacheSlots; ++i)
    {
        const auto existing = g_wireCacheState[i].load(std::memory_order_acquire);
        if (existing == key)
            return g_wireCacheFill[i].load(std::memory_order_relaxed) == D3D11_FILL_WIREFRAME;
        if (!existing)
            break;
    }
    D3D11_RASTERIZER_DESC desc{};
    state->GetDesc(&desc);
    for (std::size_t i = 0; i < kWireCacheSlots; ++i)
    {
        std::uintptr_t expected = 0;
        if (g_wireCacheState[i].load(std::memory_order_acquire) == key)
            break;
        if (g_wireCacheState[i].compare_exchange_strong(expected, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            g_wireCacheFill[i].store(desc.FillMode, std::memory_order_relaxed);
            break;
        }
    }
    return desc.FillMode == D3D11_FILL_WIREFRAME;
}

thread_local int t_modRender = 0;
// [RTCACHE] BeginDraw used to call OMGetRenderTargets on every
// one of ~10,000 draws a frame only to discover a depth buffer was bound and
// return.  The OM detours already see every binding, so the calling thread
// remembers "depth bound" per context and the draw path needs no COM call.
// Invalidated when a command list executes without state restore, when a
// deferred list is finished, and whenever the context differs.
struct RtCache { ID3D11DeviceContext* context{}; bool depth{}; bool valid{}; };
thread_local RtCache t_rt{};

// ---- per-frame state -----------------------------------------------------
std::atomic_uint32_t g_ordinal{};        // draw ordinal within the current frame
std::atomic_uint32_t g_lastFrameTotal{}; // previous frame's draw total
std::atomic_uint64_t g_presents{};
// Window sums since the last summary line.
std::atomic_uint64_t g_laneSum[LaneCount]{};
std::atomic_uint64_t g_thisImmediate{};
std::atomic_uint64_t g_thisDeferredProbe{};
std::atomic_uint64_t g_thisOther{};
std::atomic_uint64_t g_backbufferDraws{};
std::atomic_uint64_t g_candidateDraws{};
std::atomic_uint64_t g_fittedWindow{};
std::atomic_uint64_t g_modDraws{};
std::atomic_uint64_t g_noShader{};
std::atomic_uint64_t g_finish{};
std::atomic_uint64_t g_execute{};
std::atomic_uint64_t g_hookTicks{};
std::atomic_uint64_t g_windowFrames{};

// ---- backbuffer identity -------------------------------------------------
std::atomic_uintptr_t g_backBufferResource{};
std::atomic_uint32_t g_displayW{};
std::atomic_uint32_t g_displayH{};

// ---- fit -------------------------------------------------------------------
std::atomic_bool g_fitEnabled{true};
std::atomic<float> g_fitScaleX{0.85f};
std::atomic<float> g_fitScaleY{0.85f};
std::atomic<float> g_fitOffsetX{0.0f};
std::atomic<float> g_fitOffsetY{0.0f};
std::atomic_uint64_t g_fitDraws{};

// ---- [HUDLAYER] --------------------------------------------------------------
// Everything here runs on the game's render thread: the draws, and Present.
ID3D11Device* g_device{};
std::atomic_bool g_hudLayerEnabled{true};
std::atomic_bool g_hudConsumer{};
ID3D11Texture2D* g_hudTexture{};
ID3D11RenderTargetView* g_hudRtv{};
// [UIWORLD] second interface target: draws classified as world-anchored
// (they move on screen with the camera) land here and are shown on the
// rendered lens per eye; the panel keeps everything else.
ID3D11Texture2D* g_objectiveTextures[ObjectiveWorld::MaxMarkers]{};
ID3D11Texture2D*& g_objectiveTexture=g_objectiveTextures[0];
ID3D11RenderTargetView* g_objectiveRtvs[ObjectiveWorld::MaxMarkers]{};
unsigned g_objectiveSlot{};
ID3D11RenderTargetView*& ActiveObjectiveRtv(){return g_objectiveRtvs[g_objectiveSlot];}
ID3D11RenderTargetView*& g_objectiveRtv=g_objectiveRtvs[0];
ID3D11RasterizerState* g_objectiveRasterizer{};
std::atomic_bool g_objectiveContent{};
ObjectiveWorld::Frame g_objectiveFrames[ObjectiveWorld::MaxMarkers]{};
ObjectiveWorld::Frame& g_objectiveFrame=g_objectiveFrames[0];
std::atomic_bool g_objectiveSlotContent[ObjectiveWorld::MaxMarkers]{};
ObjectiveWorld::Frame& ActiveObjective(){return g_objectiveFrames[g_objectiveSlot];}
void ClearObjectiveTextures(ID3D11DeviceContext* context,const float* clear) {
    for(auto* rtv:g_objectiveRtvs)if(rtv)context->ClearRenderTargetView(rtv,clear);
}
ID3D11Texture2D* g_targetTexture{};
ID3D11RenderTargetView* g_targetRtv{};
std::atomic_bool g_targetContent{};
TargetWorld::Group g_targetGroup{};
ID3D11Texture2D* g_interactionTexture{};
ID3D11RenderTargetView* g_interactionRtv{};
std::atomic_bool g_interactionContent{};
ObjectiveWorld::Frame g_interactionFrame{};
float g_interactionCentre[2]{};
unsigned g_interactionLast{};
ID3D11Texture2D* g_hudWorldTexture{};
ID3D11RenderTargetView* g_hudWorldRtv{};
std::atomic_bool g_hudWorldFrameContent{};
std::atomic_bool g_hudWorldRouting{true};
std::atomic_uint64_t g_hudWorldRouted{};
std::uint32_t g_hudW{};
std::uint32_t g_hudH{};
std::uint32_t g_hudFmt{};
std::atomic_bool g_hudFrameContent{};
bool g_hudFrameCleared{};
std::atomic_uint64_t g_hudRedirects{};      // window counter
std::atomic_uint64_t g_hudLeft{};           // half-viewport draws (native SBS)
std::atomic_uint64_t g_hudRight{};
std::atomic_uint64_t g_hudDropped{};
std::atomic_uint64_t g_hudUntextured{};
std::atomic_uint64_t g_hudBlendPass{};      // [HUDALPHA] no scalar coverage: left in the game frame
std::atomic_uint64_t g_hudFmtPass{};        // interface draw on a non-swapchain-format target: left in the game frame
std::atomic_uint32_t g_displayFmt{};        // final output format, not necessarily the sprite target format
std::atomic_uint64_t g_hudPassClears{};     // [HUDPASS] layer rebuilt for a later interface copy
// Draws on the render thread since the last redirected interface draw.  Only
// the render thread touches it (draw hooks and Present run there).
std::uint32_t g_hudDrawsSinceRedirect{};
std::uint32_t g_hudDrawsThisFrame{};        // interface draws redirected so far this frame
bool g_hudPassBulkThisFrame{};              // [HUDPASS] bulk rule fired already this frame (2 clears/frame measured)
std::atomic_uint64_t g_hudPassClearsWindow{}; // [HUDPASS] clears, reported in the [UIWORLD] line
std::uint64_t g_hudFirstSignature{};         // first interface draw of this frame
std::atomic_bool g_hudNativeStereo{};
// [AERICONS] The world icons (objective, target, interaction,
// focus/parking/talk, labels) were captured only while g_hudNativeStereo was
// on, so AER showed none.  They need a VR camera, not native stereo; the
// second-HUD-copy logic below stays on g_hudNativeStereo (AER draws one copy).
std::atomic_bool g_hudWorldIcons{};
constexpr std::uint32_t kHudPassGap = 256;
std::atomic_uint64_t g_hudRedirectTotal{};
std::atomic_uint32_t g_hudLogged{};
// The game's interface blend states keep their COLOUR blending exactly; only
// the alpha channel is rewritten so the layer carries a usable coverage:
// alpha = src.a + dst.a * (1 - src.a), the union of everything drawn.
struct DerivedBlend
{
    ID3D11BlendState* key{};
    ID3D11BlendState* derived{};
};
std::array<DerivedBlend, 32> g_derivedBlends{};
std::size_t g_derivedBlendCount{};

// ---- caches ------------------------------------------------------------------
// Pointer-keyed, cleared periodically from Present so a freed-and-reused
// address cannot carry stale facts for long.  Readers that race a clear just
// miss and re-probe.
struct RtvInfo
{
    std::atomic_uintptr_t key{};
    std::atomic_uint32_t valid{};
    std::atomic_uint32_t w{};
    std::atomic_uint32_t h{};
    std::atomic_uint32_t fmt{};
    std::atomic_uint32_t isBackBuffer{};
};
constexpr std::size_t kRtvCache = 512;
std::array<RtvInfo, kRtvCache> g_rtvCache{};

// [HUDALPHA] Which coverage rule the layer may use for a blend state.  The
// layer is composited premultiplied: out = layer.rgb + world * (1 - layer.a),
// so layer.a must equal 1 - (how much of the world this draw lets through),
// and that transmittance is the game's colour DestBlend factor:
//   INV_SRC_ALPHA -> union   alpha = a + dst.a * (1 - a)   (ONE / INV_SRC_ALPHA)
//   ONE (additive) -> keep   alpha = dst.a                 (ZERO / ONE)
//   ZERO (replace) -> opaque alpha = a (1 for a fill)      (ONE / ZERO)
// Anything else (colour-dependent factors, MIN/MAX/subtract ops, dest-alpha
// factors) has no scalar coverage, so the draw stays in the game frame.
// Measured: the menu highlight / battle banners are
// SRC_ALPHA/ONE glows; the old fixed ONE/INV_SRC_ALPHA rule gave a glow the
// coverage of a panel and the compositor dimmed the world under it - the
// grey bar behind LOAD GAME, the black box behind "Mod", black outlines on
// the battle-start and objective banners.
enum BlendRule : std::uint32_t
{
    kBlendRuleNone = 0,
    kBlendRuleUnion = 1,
    kBlendRuleKeep = 2,
    kBlendRuleOpaque = 3,
};
const char* const kBlendRuleNames[] = {"not-layerable", "union", "keep", "opaque"};

std::uint32_t BlendRuleFor(const D3D11_RENDER_TARGET_BLEND_DESC& rt)
{
    if (!rt.BlendEnable)
        return kBlendRuleNone;
    if (rt.BlendOp != D3D11_BLEND_OP_ADD)
        return kBlendRuleNone;
    switch (rt.SrcBlend)
    {
    case D3D11_BLEND_DEST_ALPHA:
    case D3D11_BLEND_INV_DEST_ALPHA:
    case D3D11_BLEND_DEST_COLOR:
    case D3D11_BLEND_INV_DEST_COLOR:
        return kBlendRuleNone; // reads the destination, which differs on the layer
    default:
        break;
    }
    switch (rt.DestBlend)
    {
    case D3D11_BLEND_INV_SRC_ALPHA: return kBlendRuleUnion;
    case D3D11_BLEND_ONE: return kBlendRuleKeep;
    case D3D11_BLEND_ZERO: return kBlendRuleOpaque;
    default: return kBlendRuleNone;
    }
}

struct BlendInfo
{
    std::atomic_uintptr_t key{};
    std::atomic_uint32_t valid{};
    std::atomic_uint32_t blend{};
    std::atomic_uint32_t rule{};
};
constexpr std::size_t kBlendCache = 256;
std::array<BlendInfo, kBlendCache> g_blendCache{};

// ---- per pixel shader census -------------------------------------------------
struct PsSlot
{
    std::atomic_uintptr_t ps{};
    std::atomic_uint64_t calls{};
    std::atomic_uint64_t uiCalls{};
    std::atomic_uint32_t lanes{};
    std::atomic_uint32_t lastOrdinal{};
    std::atomic_uint32_t lastCount{};
    std::atomic_uint32_t lastInstances{};
    std::atomic_uint32_t blend{};
    std::atomic_uint32_t fmt{};
    std::atomic_uint32_t rtW{};
    std::atomic_uint32_t rtH{};
    std::atomic_uint32_t isBackBuffer{};
    std::atomic_uint32_t depth{};
    std::atomic_uint32_t blendLogged{};
    std::atomic_uint32_t probed{};
    std::atomic_uint32_t vpW{};
    std::atomic_uint32_t vpH{};
    std::atomic_uint32_t fitLogged{};
    std::atomic_uint32_t leftHalf{};
    std::atomic_uint32_t rightHalf{};
    std::atomic_uint32_t untextured{};
    std::atomic_uint32_t passLogged{};
    std::atomic_uint32_t output0{};
    std::atomic_uint32_t output1{};
};
constexpr std::size_t kPsSlots = 4096;
std::array<PsSlot, kPsSlots> g_ps{};
std::atomic_uint64_t g_psDropped{};
std::atomic_uint32_t g_dumps{};

template <typename... Args>
void LogF(const char* format, Args... args)
{
    if (g_log)
        g_log(format, args...);
}

const char* ModuleNameOf(const void* address, char* buffer, std::size_t size)
{
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) && module)
    {
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(module, path, sizeof(path)))
        {
            const char* base = std::strrchr(path, '\\');
            base = base ? base + 1 : path;
            std::snprintf(buffer, size, "%s+0x%llX", base,
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(address) -
                    reinterpret_cast<std::uintptr_t>(module)));
            return buffer;
        }
    }
    std::snprintf(buffer, size, "%p", address);
    return buffer;
}

bool IsLdrFormat(std::uint32_t format)
{
    switch (static_cast<DXGI_FORMAT>(format))
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    // [UIGATE] FFXV's pipeline is HDR end to end: the sprites (6-index quads,
    // blended, no depth) land in a display-sized R16G16B16A16_FLOAT target
    // right before the backbuffer copy.  Measured: the 8-bit-only
    // rule matched nothing for a whole run while ~1.4M sprite draws went past.
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

struct RtFacts
{
    std::uint32_t w{};
    std::uint32_t h{};
    std::uint32_t fmt{};
    bool isBackBuffer{};
};

RtFacts ProbeRtv(ID3D11RenderTargetView* rtv)
{
    RtFacts facts{};
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
    rtv->GetDesc(&viewDesc);
    facts.fmt = static_cast<std::uint32_t>(viewDesc.Format);
    ID3D11Resource* resource{};
    rtv->GetResource(&resource);
    if (resource)
    {
        facts.isBackBuffer = reinterpret_cast<std::uintptr_t>(resource) ==
            g_backBufferResource.load(std::memory_order_relaxed);
        ID3D11Texture2D* texture{};
        if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture))) && texture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            facts.w = desc.Width;
            facts.h = desc.Height;
            texture->Release();
        }
        resource->Release();
    }
    return facts;
}

RtFacts LookupRtv(ID3D11RenderTargetView* rtv)
{
    const auto key = reinterpret_cast<std::uintptr_t>(rtv);
    const auto start = (key >> 4) & (kRtvCache - 1);
    RtvInfo* home{};
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& entry = g_rtvCache[(start + probe) & (kRtvCache - 1)];
        auto existing = entry.key.load(std::memory_order_acquire);
        if (existing == key)
        {
            if (entry.valid.load(std::memory_order_acquire))
                return RtFacts{entry.w.load(std::memory_order_relaxed),
                    entry.h.load(std::memory_order_relaxed),
                    entry.fmt.load(std::memory_order_relaxed),
                    entry.isBackBuffer.load(std::memory_order_relaxed) != 0};
            home = &entry;
            break;
        }
        if (existing == 0 &&
            entry.key.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            home = &entry;
            break;
        }
        if (existing == key)
        {
            home = &entry;
            break;
        }
    }
    const RtFacts facts = ProbeRtv(rtv);
    if (home)
    {
        home->w.store(facts.w, std::memory_order_relaxed);
        home->h.store(facts.h, std::memory_order_relaxed);
        home->fmt.store(facts.fmt, std::memory_order_relaxed);
        home->isBackBuffer.store(facts.isBackBuffer ? 1u : 0u, std::memory_order_relaxed);
        home->valid.store(1u, std::memory_order_release);
    }
    return facts;
}

bool LookupBlend(ID3D11BlendState* state, std::uint32_t* rule)
{
    *rule = kBlendRuleNone;
    if (!state)
        return false; // default blend state: blending off
    const auto key = reinterpret_cast<std::uintptr_t>(state);
    const auto start = (key >> 4) & (kBlendCache - 1);
    BlendInfo* home{};
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& entry = g_blendCache[(start + probe) & (kBlendCache - 1)];
        auto existing = entry.key.load(std::memory_order_acquire);
        if (existing == key)
        {
            if (entry.valid.load(std::memory_order_acquire))
            {
                *rule = entry.rule.load(std::memory_order_relaxed);
                return entry.blend.load(std::memory_order_relaxed) != 0;
            }
            home = &entry;
            break;
        }
        if (existing == 0 &&
            entry.key.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            home = &entry;
            break;
        }
        if (existing == key)
        {
            home = &entry;
            break;
        }
    }
    D3D11_BLEND_DESC desc{};
    state->GetDesc(&desc);
    const bool blend = desc.RenderTarget[0].BlendEnable != FALSE;
    *rule = BlendRuleFor(desc.RenderTarget[0]);
    if (home)
    {
        home->blend.store(blend ? 1u : 0u, std::memory_order_relaxed);
        home->rule.store(*rule, std::memory_order_relaxed);
        home->valid.store(1u, std::memory_order_release);
    }
    return blend;
}

void ClearCaches()
{
    for (auto& entry : g_rtvCache)
    {
        entry.valid.store(0u, std::memory_order_release);
        entry.key.store(0u, std::memory_order_release);
    }
    for (auto& entry : g_blendCache)
    {
        entry.valid.store(0u, std::memory_order_release);
        entry.key.store(0u, std::memory_order_release);
    }
}

struct FitState
{
    D3D11_VIEWPORT viewport{};
    UINT viewportCount{};
    D3D11_RECT scissor{};
    UINT scissorCount{};
    bool active{};
    bool objectiveRaster{};
    ID3D11RasterizerState* savedRaster{};
    // [HUDLAYER] state saved around a redirected draw
    bool redirected{};
    bool drop{}; // native SBS right-half copy of an interface draw: skip it
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* savedDsv{};
    ID3D11BlendState* savedBlend{};
    FLOAT savedFactor[4]{};
    UINT savedMask{};
};

// Shrink the raster viewport (and the scissor with it) toward the centre for
// one draw.  Returns false when the settings are neutral.
bool BeginFit(ID3D11DeviceContext* context, FitState& fit)
{
    const float scaleX = g_fitScaleX.load(std::memory_order_relaxed);
    const float scaleY = g_fitScaleY.load(std::memory_order_relaxed);
    const float offsetX = g_fitOffsetX.load(std::memory_order_relaxed);
    const float offsetY = g_fitOffsetY.load(std::memory_order_relaxed);
    if (std::fabs(scaleX - 1.0f) < 1e-4f && std::fabs(scaleY - 1.0f) < 1e-4f &&
        std::fabs(offsetX) < 1e-4f && std::fabs(offsetY) < 1e-4f)
        return false;
    fit.viewportCount = 1;
    context->RSGetViewports(&fit.viewportCount, &fit.viewport);
    if (fit.viewportCount == 0 || fit.viewport.Width <= 0.0f || fit.viewport.Height <= 0.0f)
        return false;
    const auto& vp = fit.viewport;
    D3D11_VIEWPORT fitted = vp;
    fitted.Width = vp.Width * scaleX;
    fitted.Height = vp.Height * scaleY;
    fitted.TopLeftX = vp.TopLeftX + (vp.Width - fitted.Width) * 0.5f + offsetX * vp.Width;
    fitted.TopLeftY = vp.TopLeftY + (vp.Height - fitted.Height) * 0.5f + offsetY * vp.Height;
    context->RSSetViewports(1, &fitted);

    fit.scissorCount = 1;
    context->RSGetScissorRects(&fit.scissorCount, &fit.scissor);
    if (fit.scissorCount)
    {
        const auto mapX = [&](LONG x)
        {
            return static_cast<LONG>(std::lround(fitted.TopLeftX +
                (static_cast<float>(x) - vp.TopLeftX) * scaleX));
        };
        const auto mapY = [&](LONG y)
        {
            return static_cast<LONG>(std::lround(fitted.TopLeftY +
                (static_cast<float>(y) - vp.TopLeftY) * scaleY));
        };
        D3D11_RECT scaled{};
        scaled.left = mapX(fit.scissor.left);
        scaled.right = mapX(fit.scissor.right);
        scaled.top = mapY(fit.scissor.top);
        scaled.bottom = mapY(fit.scissor.bottom);
        context->RSSetScissorRects(1, &scaled);
    }
    fit.active = true;
    return true;
}

void EndFit(ID3D11DeviceContext* context, FitState& fit)
{
    if (fit.redirected)
    {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            fit.savedRtvs, fit.savedDsv);
        for (auto*& view : fit.savedRtvs)
            if (view)
            {
                view->Release();
                view = nullptr;
            }
        if (fit.savedDsv)
        {
            fit.savedDsv->Release();
            fit.savedDsv = nullptr;
        }
        context->OMSetBlendState(fit.savedBlend, fit.savedFactor, fit.savedMask);
        if (fit.savedBlend)
        {
            fit.savedBlend->Release();
            fit.savedBlend = nullptr;
        }
        fit.redirected = false;
    }
    if (fit.objectiveRaster) {
        context->RSSetState(fit.savedRaster);
        if(fit.savedRaster) fit.savedRaster->Release();
        if(!fit.scissorCount) context->RSSetScissorRects(0,nullptr);
    }
    if (!fit.active)
        return;
    context->RSSetViewports(fit.viewportCount, &fit.viewport);
    if (fit.scissorCount)
        context->RSSetScissorRects(fit.scissorCount, &fit.scissor);
}

// [HUDLAYER] The mod's interface target: same size and format as the game's own
// sprite target, so the game's shaders and blending behave identically.
bool EnsureHudTarget(std::uint32_t width, std::uint32_t height, std::uint32_t format)
{
    if (g_hudTexture && g_hudRtv && g_hudW == width && g_hudH == height &&
        g_hudFmt == format)
        return true;
    if (!g_device || !width || !height)
        return false;
    if (g_hudRtv)
    {
        g_hudRtv->Release();
        g_hudRtv = nullptr;
    }
    if (g_hudTexture)
    {
        g_hudTexture->Release();
        g_hudTexture = nullptr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (g_hudWorldRtv) { g_hudWorldRtv->Release(); g_hudWorldRtv = nullptr; }
    if (g_hudWorldTexture) { g_hudWorldTexture->Release(); g_hudWorldTexture = nullptr; }
    HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_hudTexture);
    if (SUCCEEDED(hr) && g_hudTexture)
        hr = g_device->CreateRenderTargetView(g_hudTexture, nullptr, &g_hudRtv);
    if (FAILED(hr) || !g_hudTexture || !g_hudRtv)
    {
        LogF("[HUDLAYER] interface target %ux%u fmt=%u creation failed hr=0x%08X",
            width, height, format, static_cast<unsigned>(hr));
        if (g_hudRtv)
            g_hudRtv->Release();
        if (g_hudTexture)
            g_hudTexture->Release();
        g_hudRtv = nullptr;
        g_hudTexture = nullptr;
        return false;
    }
    hr = g_device->CreateTexture2D(&desc, nullptr, &g_hudWorldTexture);
    if (SUCCEEDED(hr) && g_hudWorldTexture)
        hr = g_device->CreateRenderTargetView(g_hudWorldTexture, nullptr, &g_hudWorldRtv);
    if (FAILED(hr) || !g_hudWorldTexture || !g_hudWorldRtv)
    {
        LogF("[UIWORLD] world interface target creation failed hr=0x%08X; markers stay on the panel",
            static_cast<unsigned>(hr));
        if (g_hudWorldRtv) g_hudWorldRtv->Release();
        if (g_hudWorldTexture) g_hudWorldTexture->Release();
        g_hudWorldRtv = nullptr;
        g_hudWorldTexture = nullptr;
    }
    desc.Width=desc.Height=ObjectiveWorld::TextureSize;
    for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i) {
        if(g_objectiveRtvs[i]){g_objectiveRtvs[i]->Release();g_objectiveRtvs[i]=nullptr;}
        if(g_objectiveTextures[i]){g_objectiveTextures[i]->Release();g_objectiveTextures[i]=nullptr;}
        hr=g_device->CreateTexture2D(&desc,nullptr,&g_objectiveTextures[i]);
        if(SUCCEEDED(hr))hr=g_device->CreateRenderTargetView(g_objectiveTextures[i],nullptr,&g_objectiveRtvs[i]);
    }
    if(g_targetRtv){g_targetRtv->Release();g_targetRtv=nullptr;}
    if(g_targetTexture){g_targetTexture->Release();g_targetTexture=nullptr;}
    hr=g_device->CreateTexture2D(&desc,nullptr,&g_targetTexture);
    if(SUCCEEDED(hr))hr=g_device->CreateRenderTargetView(g_targetTexture,nullptr,&g_targetRtv);
    if(g_interactionRtv){g_interactionRtv->Release();g_interactionRtv=nullptr;}
    if(g_interactionTexture){g_interactionTexture->Release();g_interactionTexture=nullptr;}
    hr=g_device->CreateTexture2D(&desc,nullptr,&g_interactionTexture);
    if(SUCCEEDED(hr))hr=g_device->CreateRenderTargetView(g_interactionTexture,nullptr,&g_interactionRtv);
    if(!g_objectiveRasterizer) {
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
        raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
        g_device->CreateRasterizerState(&raster,&g_objectiveRasterizer);
    }
    g_hudW = width;
    g_hudH = height;
    g_hudFmt = format;
    g_hudFrameCleared = false;
    LogF("[HUDLAYER] interface target %ux%u fmt=%u ready", width, height, format);
    return true;
}

ID3D11BlendState* DerivedBlendFor(ID3D11BlendState* state, std::uint32_t rule)
{
    if (!state || !g_device || rule == kBlendRuleNone)
        return nullptr;
    for (std::size_t i = 0; i < g_derivedBlendCount; ++i)
        if (g_derivedBlends[i].key == state)
            return g_derivedBlends[i].derived;
    if (g_derivedBlendCount >= g_derivedBlends.size())
        return nullptr;
    D3D11_BLEND_DESC desc{};
    state->GetDesc(&desc);
    const std::size_t targets = desc.IndependentBlendEnable ? 8 : 1;
    for (std::size_t i = 0; i < targets; ++i)
    {
        auto& rt = desc.RenderTarget[i];
        switch (rule)
        {
        case kBlendRuleKeep:
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            break;
        case kBlendRuleOpaque:
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            break;
        default:
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            break;
        }
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    ID3D11BlendState* derived{};
    if (FAILED(g_device->CreateBlendState(&desc, &derived)) || !derived)
        return nullptr;
    state->AddRef(); // pin the key so the pointer cannot be recycled
    g_derivedBlends[g_derivedBlendCount++] = DerivedBlend{state, derived};
    LogF("[HUDLAYER] derived blend %zu: colour %u/%u op %u kept, alpha rule=%s",
        g_derivedBlendCount, static_cast<unsigned>(desc.RenderTarget[0].SrcBlend),
        static_cast<unsigned>(desc.RenderTarget[0].DestBlend),
        static_cast<unsigned>(desc.RenderTarget[0].BlendOp), kBlendRuleNames[rule & 3]);
    return derived;
}

PsSlot* SlotFor(std::uintptr_t key)
{
    const auto start = (key >> 4) & (kPsSlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_ps[(start + probe) & (kPsSlots - 1)];
        auto existing = slot.ps.load(std::memory_order_acquire);
        if (existing == 0 &&
            !slot.ps.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
            existing = slot.ps.load(std::memory_order_acquire);
        if (existing == key)
            return &slot;
    }
    g_psDropped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void LaneString(std::uint32_t lanes, char* out, std::size_t size)
{
    int written = 0;
    out[0] = '\0';
    for (std::uint32_t lane = 0; lane < LaneCount; ++lane)
    {
        if (!(lanes & (1u << lane)))
            continue;
        const int added = std::snprintf(out + written, size - static_cast<std::size_t>(written),
            "%s%s", written ? "|" : "", kLaneNames[lane]);
        if (added < 0)
            break;
        written += added;
        if (static_cast<std::size_t>(written) >= size - 1)
            break;
    }
    if (!written)
        std::snprintf(out, size, "none");
}

// ---- [UIWORLD] which interface draws are anchored to the WORLD? ------------
// Goal: keep the readable head-locked HUD panel AND put lock-on / objective /
// enemy markers on their targets.  That needs the two kinds told apart per
// draw.  This census records, for every draw the layer redirects: the vertex
// shader, the vertex-buffer stride, the texture size, and the first vertex's
// bytes (read from a CPU shadow of the dynamic vertex buffer taken at Unmap /
// UpdateSubresource), then tracks per draw IDENTITY (vs, ps, texture, count)
// how often the quad's position changes between frames.  World-anchored
// elements move whenever the camera moves; screen-space ones sit still.
namespace UiWorld
{
// NoHeadset is a process-start setting, also cached by main.cpp. HUD fitting
// only transforms viewports; without XR there is no world-layer consumer.
bool ShadowsEnabled() {
    if constexpr (ResearchDiagnostics::Enabled || ResearchDiagnostics::MarkerCapture) return true;
    static const bool enabled = [] {
        wchar_t path[MAX_PATH]{};
        const auto n=GetModuleFileNameW(nullptr,path,MAX_PATH);
        if(!n || n>=MAX_PATH)return true;
        auto* slash=wcsrchr(path,L'\\');
        if(!slash || wcscpy_s(slash+1,MAX_PATH-(slash+1-path),L"ffxv-vr.ini"))return true;
        return GetPrivateProfileIntW(L"AER",L"NoHeadset",0,path)==0;
    }();
    return enabled;
}
constexpr std::size_t kShadowSlots = 16;
constexpr std::size_t kShadowBytes = 4u << 20; // 4 MB per slot
struct Shadow
{
    ID3D11Resource* resource{};
    std::uint32_t bytes{};
    std::uint32_t stamp{};
    std::uint8_t* data{};
};
Shadow g_shadows[kShadowSlots]{};
std::uint32_t g_shadowStamp{};
struct Pending { ID3D11Resource* resource{}; void* data{}; std::uint32_t bytes{}; };
Pending g_pending[8]{};
SRWLOCK g_lock = SRWLOCK_INIT;
// Measured: the interface vertex buffer is ONE 32 MB DYNAMIC buffer
// (usage 2, cpu write) mapped WRITE_NO_OVERWRITE ~1000x per present, one
// small region per sprite, each draw at start=0/base=0 with the vertex-buffer
// OFFSET selecting the sprite.  Copying 32 MB per map is impossible, so the
// mapping pointer is retained and the sprite's vertex is read from it at
// draw time (the runtime keeps dynamic buffers mapped in host-visible memory
// between NO_OVERWRITE maps; the read is SEH-guarded in case it does not).
// [SHADOWFIX] Perf census: copying every mapped buffer at Unmap
// cost 10-13 ms per frame ON THE PRESENT THREAD (3300 stores, 7 MB, read from
// the driver's uncached upload memory; the streaming fast path fired on 1-2 %
// of them) and was the whole difference between 40 and 89 fps in native
// stereo.  The only consumers are the interface draws: VS constant buffer
// slot 0 (first 544 bytes) and the sprite vertex buffer, both read at draw
// time on the immediate context right after the game mapped them.  So the
// mapped pointer is retained for EVERY writable Map (no copy, no size query),
// and the bytes are read at draw time through the SEH-guarded read the
// sprite buffer already used.  Real copies remain only for UpdateSubresource
// on buffers the reader has actually asked for (the "wanted" set).
constexpr std::size_t kLiveSlots = 1024;
struct LiveMap { ID3D11Resource* resource{}; std::uint8_t* base{}; std::uint32_t stamp{}; };
LiveMap g_live[kLiveSlots]{};
std::uint32_t g_liveStamp{};
std::atomic_uint64_t g_liveReads{}, g_liveFaults{};
extern std::uint32_t g_present; // defined below (present ordinal)
constexpr std::size_t kWantedSlots = 256;
struct Wanted { std::atomic<std::uintptr_t> resource{}; std::atomic<std::uint32_t> present{}; };
Wanted g_wanted[kWantedSlots]{};
inline std::size_t SlotHash(const void* p) { const auto k = reinterpret_cast<std::uintptr_t>(p); return static_cast<std::size_t>((k >> 4) ^ (k >> 13) ^ (k >> 22)); }
// The reader asked for this resource: remember it so UpdateSubresource copies
// (the only path that still copies) are kept for it.  Entries expire after
// 600 presents without a read.
void MarkWanted(ID3D11Resource* resource)
{
    const auto key = reinterpret_cast<std::uintptr_t>(resource);
    const auto now = g_present;
    const auto start = SlotHash(resource) & (kWantedSlots - 1);
    std::size_t victim = start; std::uint32_t oldest = 0xFFFFFFFFu;
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& w = g_wanted[(start + probe) & (kWantedSlots - 1)];
        const auto existing = w.resource.load(std::memory_order_relaxed);
        if (existing == key) { w.present.store(now, std::memory_order_relaxed); return; }
        const auto seen = w.present.load(std::memory_order_relaxed);
        if (!existing || now - seen > 600) { victim = (start + probe) & (kWantedSlots - 1); break; }
        if (seen < oldest) { oldest = seen; victim = (start + probe) & (kWantedSlots - 1); }
    }
    g_wanted[victim].resource.store(key, std::memory_order_relaxed);
    g_wanted[victim].present.store(now, std::memory_order_relaxed);
}
bool IsWanted(ID3D11Resource* resource)
{
    const auto key = reinterpret_cast<std::uintptr_t>(resource);
    const auto start = SlotHash(resource) & (kWantedSlots - 1);
    for (std::size_t probe = 0; probe < 16; ++probe)
    {
        auto& w = g_wanted[(start + probe) & (kWantedSlots - 1)];
        const auto existing = w.resource.load(std::memory_order_relaxed);
        if (!existing) return false;
        if (existing == key) return g_present - w.present.load(std::memory_order_relaxed) <= 600;
    }
    return false;
}

bool SafeCopy(void* dst, const void* src, std::size_t n)
{
    __try { std::memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Upload-path census (measured: 553k interface draws, ZERO with a
// vertex shadow -> the sprite vertices do not arrive by Map-discard or
// UpdateSubresource on a vertex-bound buffer).  Count every way a buffer can
// be written so the next report names the path.
std::atomic_uint64_t g_mapByMode[8]{};      // D3D11_MAP 1..5
std::atomic_uint64_t g_mapVb{}, g_mapStaging{}, g_mapOther{}, g_mapTooBig{};
std::atomic_uint64_t g_copies{}, g_copiesAliased{}, g_regionCopies{}, g_regionCopied{};
std::atomic_uint64_t g_bytesThisPresent{};
constexpr std::uint64_t kBytesPerPresentCap = 96ull << 20;
std::atomic_uint64_t g_capHits{};
struct MissReason { std::atomic_uint64_t vbNull{}, strideZero{}, noShadow{}, srvBuffer{}; };
MissReason g_miss{};
struct VbSeen { ID3D11Resource* resource{}; std::uint32_t logged{}; };
VbSeen g_vbSeen[32]{};

bool BufferFacts(ID3D11Resource* resource, std::uint32_t* bytes, std::uint32_t* bind)
{
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        return false;
    ID3D11Buffer* buffer{};
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) || !buffer)
        return false;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    buffer->Release();
    *bytes = desc.ByteWidth;
    *bind = desc.BindFlags;
    return true;
}

void StoreImpl(ID3D11Resource* resource, const void* data, std::uint32_t bytes, bool mapped)
{
    if (!data || !bytes || bytes > kShadowBytes)
        return;
    if (g_bytesThisPresent.fetch_add(bytes, std::memory_order_relaxed) + bytes > kBytesPerPresentCap)
    {
        if constexpr (ResearchDiagnostics::Enabled) g_capHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    Shadow* home = nullptr;
    for (auto& shadow : g_shadows)
        if (shadow.resource == resource) { home = &shadow; break; }
    if (!home)
    {
        home = &g_shadows[0];
        for (auto& shadow : g_shadows)
            if (!shadow.resource) { home = &shadow; break; }
            else if (shadow.stamp < home->stamp) home = &shadow;
    }
    if (!home->data)
        home->data = static_cast<std::uint8_t*>(std::malloc(kShadowBytes));
    if (home->data)
    {
        home->resource = resource;
        home->bytes = bytes;
        home->stamp = ++g_shadowStamp;
        if(mapped) MappedCopy::Copy(home->data, data, bytes);
        else std::memcpy(home->data, data, bytes);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// [PERF] timed/counted wrapper; StoreImpl is the unchanged copy.
void Store(ID3D11Resource* resource, const void* data, std::uint32_t bytes, bool mapped = false)
{
    const auto t0 = PerfCensus::Tsc();
    StoreImpl(resource, data, bytes, mapped);
    PerfCensus::Add(PerfCensus::StoreCalls);
    PerfCensus::Add(PerfCensus::StoreBytes, bytes);
    PerfCensus::Add(PerfCensus::OnPresentThread() ? PerfCensus::StoreTicksPt : PerfCensus::StoreTicksOther, PerfCensus::Tsc() - t0);
}

// [FLASHPROBE] read through a retained mapping only; never marks the
// resource wanted, so effect buffers cannot evict marker shadows.
bool ReadLive(ID3D11Resource* resource, std::uint32_t offset, std::uint8_t* out, std::uint32_t n)
{
    if (!resource || !n) return false;
    std::uint8_t* base = nullptr;
    AcquireSRWLockShared(&g_lock);
    const auto start = SlotHash(resource) & (kLiveSlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& live = g_live[(start + probe) & (kLiveSlots - 1)];
        if (!live.resource) break;
        if (live.resource == resource) { base = live.base; break; }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!base) return false;
    std::uint32_t bytes = 0, bind = 0;
    if (!BufferFacts(resource, &bytes, &bind) || offset > bytes || n > bytes - offset) return false;
    return SafeCopy(out, base + offset, n);
}

// Copies out the bytes of one vertex (up to 64) for the draw hook.
bool ReadVertex(ID3D11Resource* resource, std::uint32_t offset, std::uint8_t* out, std::uint32_t n)
{
    if (!resource) return false;
    MarkWanted(resource);
    bool ok = false;
    std::uint8_t* liveBase = nullptr;
    AcquireSRWLockShared(&g_lock);
    for (auto& shadow : g_shadows)
        if (shadow.resource == resource && shadow.data && offset + n <= shadow.bytes)
        {
            std::memcpy(out, shadow.data + offset, n);
            ok = true;
            break;
        }
    if (!ok)
    {
        const auto start = SlotHash(resource) & (kLiveSlots - 1);
        for (std::size_t probe = 0; probe < 32; ++probe)
        {
            auto& live = g_live[(start + probe) & (kLiveSlots - 1)];
            if (!live.resource) break;
            if (live.resource == resource) { liveBase = live.base; break; }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!ok && liveBase)
    {
        // Size is queried here, on the rare read, never on the 3800 maps a frame.
        std::uint32_t bytes = 0, bind = 0;
        if (BufferFacts(resource, &bytes, &bind) && offset + n <= bytes)
        {
            if (SafeCopy(out, liveBase + offset, n))
            {
                ok = true;
                g_liveReads.fetch_add(1, std::memory_order_relaxed);
            }
            else
                g_liveFaults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return ok;
}

void OnMapped(ID3D11Resource* resource, D3D11_MAP mode, void* data)
{
    if (!ShadowsEnabled()) return;
    if (PerfCensus::bypassShadow.load(std::memory_order_relaxed)) return; // [PERF] Numpad7
    if (!data || !resource || mode == D3D11_MAP_READ)
        return;
    // [SHADOWFIX] retain the mapping pointer only; no copy, no size query.
    // Textures land here too (rare); ReadVertex's BufferFacts rejects them.
    AcquireSRWLockExclusive(&g_lock);
    const auto start = SlotHash(resource) & (kLiveSlots - 1);
    LiveMap* home = nullptr; LiveMap* oldest = nullptr;
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& live = g_live[(start + probe) & (kLiveSlots - 1)];
        if (live.resource == resource || !live.resource) { home = &live; break; }
        if (!oldest || live.stamp < oldest->stamp) oldest = &live;
    }
    if (!home) home = oldest;
    *home = LiveMap{resource, static_cast<std::uint8_t*>(data), ++g_liveStamp};
    ReleaseSRWLockExclusive(&g_lock);
}

void OnUnmapping(ID3D11Resource*)
{
    // [SHADOWFIX] nothing to do: the retained pointer is read at draw time.
}

void OnUpdated(ID3D11Resource* resource, const D3D11_BOX* box, const void* data)
{
    if (!ShadowsEnabled()) return;
    if (PerfCensus::bypassShadow.load(std::memory_order_relaxed)) return; // [PERF] Numpad7
    if (!data || box || !resource)
        return;
    if (!IsWanted(resource)) return; // [SHADOWFIX] copy only what the reader has asked for
    std::uint32_t bytes = 0, bind = 0;
    if (!BufferFacts(resource, &bytes, &bind))
        return;
    Store(resource, data, bytes);
}

// CopyResource(dst, src): if src has a shadow (a CPU-written staging buffer),
// dst now holds the same bytes.
void OnCopied(ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!ShadowsEnabled()) return;
    if (PerfCensus::bypassShadow.load(std::memory_order_relaxed)) return; // [PERF] Numpad7
    if constexpr (ResearchDiagnostics::Enabled) g_copies.fetch_add(1, std::memory_order_relaxed);
    AcquireSRWLockShared(&g_lock);
    const Shadow* from = nullptr;
    for (auto& shadow : g_shadows)
        if (shadow.resource == src && shadow.data) { from = &shadow; break; }
    std::uint8_t* tmp = nullptr; std::uint32_t bytes = 0;
    if (from)
    {
        bytes = from->bytes;
        tmp = static_cast<std::uint8_t*>(std::malloc(bytes));
        if (tmp) std::memcpy(tmp, from->data, bytes);
    }
    ReleaseSRWLockShared(&g_lock);
    if (tmp)
    {
        if constexpr (ResearchDiagnostics::Enabled) g_copiesAliased.fetch_add(1, std::memory_order_relaxed);
        Store(dst, tmp, bytes);
        std::free(tmp);
    }
}

// CopySubresourceRegion(dst @ x, src box): propagate the range.
void OnCopiedRegion(ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box)
{
    if (!ShadowsEnabled()) return;
    if (PerfCensus::bypassShadow.load(std::memory_order_relaxed)) return; // [PERF] Numpad7
    if constexpr (ResearchDiagnostics::Enabled) g_regionCopies.fetch_add(1, std::memory_order_relaxed);
    D3D11_RESOURCE_DIMENSION dim{};
    dst->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER)
        return;
    AcquireSRWLockExclusive(&g_lock);
    const Shadow* from = nullptr;
    for (auto& shadow : g_shadows)
        if (shadow.resource == src && shadow.data) { from = &shadow; break; }
    if (from)
    {
        const std::uint32_t left = box ? box->left : 0u;
        const std::uint32_t right = box ? box->right : from->bytes;
        if (right > left && right <= from->bytes)
        {
            Shadow* to = nullptr;
            for (auto& shadow : g_shadows)
                if (shadow.resource == dst) { to = &shadow; break; }
            if (!to)
            {
                std::uint32_t dstBytes = 0, bind = 0;
                if (BufferFacts(dst, &dstBytes, &bind) && dstBytes <= kShadowBytes)
                {
                    to = &g_shadows[0];
                    for (auto& shadow : g_shadows)
                        if (!shadow.resource) { to = &shadow; break; }
                        else if (shadow.stamp < to->stamp) to = &shadow;
                    if (!to->data) to->data = static_cast<std::uint8_t*>(std::malloc(kShadowBytes));
                    if (to->data) { to->resource = dst; to->bytes = dstBytes; std::memset(to->data, 0, dstBytes); }
                    else to = nullptr;
                }
            }
            if (to && to->data && dstX + (right - left) <= to->bytes)
            {
                std::memcpy(to->data + dstX, from->data + left, right - left);
                to->stamp = ++g_shadowStamp;
                if constexpr (ResearchDiagnostics::Enabled) g_regionCopied.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void LogVbFacts(ID3D11Buffer* vb, LogFn log)
{
    if (!vb || !log) return;
    auto* res = static_cast<ID3D11Resource*>(vb);
    for (auto& seen : g_vbSeen)
    {
        if (seen.resource == res) return;
        if (!seen.resource)
        {
            seen.resource = res;
            D3D11_BUFFER_DESC desc{};
            vb->GetDesc(&desc);
            log("[UIWORLD] interface VB %p without shadow: bytes=%u usage=%u bind=0x%X cpu=0x%X misc=0x%X",
                static_cast<void*>(vb), desc.ByteWidth, static_cast<unsigned>(desc.Usage),
                desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
            return;
        }
    }
}

// cb0[16..31] is not a marker signal. The embedded UI shaders
// (e.g. file offset 0x3076710) never use those registers for SV_POSITION:
// worldMatrix is at byte 0; viewProjMatrix is at byte 128 (floats 32..47).
// A changing unused camera matrix is NOT evidence that a draw is a marker.
// Select the two marker atlases by exact asset payload instead.
std::uint32_t g_present{};
thread_local INT t_baseVertex{};
UiMarkerAssets::Tracker g_markerAssets;
UiWorldRouting::PassBudget g_worldBudget;
UiWorldRouting::ObjectiveGroups g_objectiveGroups;
std::uint64_t g_sharedIconDraws{}, g_sharedIconPanel{}, g_markerOnlyClears{};
std::uint32_t g_interfaceDrawsSinceClear{};
std::uint64_t g_worldCapHits{}, g_layerBudgetResets{}, g_targetDraws{}, g_objectiveDraws{};
std::uint32_t g_copy{}, g_traceFrame[4]{}, g_traceCopy[4]{};
// Kept for the presenter's existing diagnostic API; no lens is inferred from
// unused constants. The world quad continues using the rendered eye claim.
std::atomic<float> g_uiLensP00{}, g_uiLensP11{};
std::atomic<float> g_baseCol2[3]{}, g_headCol2[3]{};

UiWorldRouting::ObjectiveLabelRun g_objectiveLabels[ObjectiveWorld::MaxMarkers]{};
auto& g_objectiveLabel=g_objectiveLabels[0];
auto& ActiveLabel(){return g_objectiveLabels[g_objectiveSlot];}
struct AuxiliaryGroup { UiMarkerAssets::Kind kind{}; unsigned slot{},last{},used{},buttons{}; float x{},y{},diameter{}; };
AuxiliaryGroup g_auxiliary{};
unsigned g_objectiveBudget{};
bool TakeObjectiveBudget(){if(g_objectiveBudget>=ObjectiveWorld::MaxMarkers*48)return false;++g_objectiveBudget;return true;}
void OnLayerClear(bool replacement)
{
    g_objectiveSlot=0;g_objectiveBudget=0;g_auxiliary={};
    for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i){g_objectiveFrames[i]={};g_objectiveLabels[i].Clear();g_objectiveSlotContent[i]=false;}
    g_interactionFrame={};g_interactionLast=0;g_interactionContent=false;
    g_worldBudget.ClearLayer();
    g_objectiveFrame={};
    g_targetGroup={};g_targetContent.store(false,std::memory_order_relaxed);
    g_objectiveContent.store(false,std::memory_order_relaxed);
    g_interfaceDrawsSinceClear = 0;
    g_objectiveGroups.Clear();
    ++g_layerBudgetResets;
    g_copy = replacement ? g_copy + 1 : 0;
}

void TraceMarker(ID3D11DeviceContext* context, UiMarkerAssets::Kind kind, LogFn log)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto i = static_cast<unsigned>(kind);
    if (!log || (g_present % 30) ||
        (g_traceFrame[i] == g_present && g_traceCopy[i] == g_copy)) return;
    g_traceFrame[i] = g_present; g_traceCopy[i] = g_copy;
    ID3D11Buffer* cb{}; context->VSGetConstantBuffers(0, 1, &cb);
    float c[136]{};
    const bool have = cb && ReadVertex(cb, 0, reinterpret_cast<std::uint8_t*>(c), sizeof(c));
    if (cb) cb->Release();
    ID3D11Buffer* vb{}; UINT stride{}, offset{};
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    float corner[3]{};
    const bool haveVertex = vb && stride == 24 && t_baseVertex == 0 &&
        ReadVertex(vb, offset, reinterpret_cast<std::uint8_t*>(corner), sizeof(corner));
    if (vb) vb->Release();
    UINT viewportCount = 1; D3D11_VIEWPORT vp{}; context->RSGetViewports(&viewportCount, &vp);
    float x{}, y{};
    const bool projected = have && haveVertex && UiWorldRouting::ProjectSprite(c, corner[0], corner[1], x, y);
    log("[UIMARK] draw=%s present=%u copy=%u context=%u cb=%u vertex=%u canvas=(%.3f,%.3f) "
        "projected=%u ndc=(%.5f,%.5f) ortho=(%.6f,%.6f,%.6f,%.6f) viewport=%.0fx%.0f@%.0f,%.0f",
        UiMarkerAssets::Name(kind), g_present, g_copy, static_cast<unsigned>(context->GetType()),
        have ? 1u : 0u, haveVertex ? 1u : 0u, c[12], c[13], projected ? 1u : 0u, x, y,
        c[32], c[37], c[44], c[45], vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY);
}

// The destination SWF imports its main symbol from icon_reblack. Its own
// atlas holds decorations only. Read the full quad, including UVs, to identify
// the two circular backgrounds in destination_marker.listb (43,56,40,40 and
// 1,56,40,40), then associate only a centred shared-atlas symbol in this pass.
bool ReadSpriteBounds(ID3D11DeviceContext* context, UiWorldRouting::SpriteBounds& bounds,
    bool& objectiveRing, float& u, float& v)
{
    ID3D11Buffer* cb{}; context->VSGetConstantBuffers(0, 1, &cb);
    float c[136]{};
    const bool constants = cb && ReadVertex(cb, 0, reinterpret_cast<std::uint8_t*>(c), sizeof(c));
    if (cb) cb->Release();
    ID3D11Buffer* vb{}; UINT stride{}, offset{};
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    struct Vertex { float x,y,z; std::uint32_t color; float u,v; } vertices[4]{};
    const bool geometry = vb && stride == sizeof(Vertex) && t_baseVertex == 0 &&
        ReadVertex(vb, offset, reinterpret_cast<std::uint8_t*>(vertices), sizeof(vertices));
    if (vb) vb->Release();
    if (!constants || !geometry) return false;
    float minX=1e30f,minY=1e30f,maxX=-1e30f,maxY=-1e30f;
    float minU=1e30f,minV=1e30f,maxU=-1e30f,maxV=-1e30f;
    for (const auto& corner : vertices)
    {
        float x{},y{};
        if (!UiWorldRouting::ProjectSprite(c,corner.x,corner.y,x,y) ||
            !std::isfinite(corner.u) || !std::isfinite(corner.v)) return false;
        x=(x+1)*960; y=(1-y)*540;
        minX=std::min(minX,x);maxX=std::max(maxX,x);
        minY=std::min(minY,y);maxY=std::max(maxY,y);
        minU=std::min(minU,corner.u);maxU=std::max(maxU,corner.u);
        minV=std::min(minV,corner.v);maxV=std::max(maxV,corner.v);
    }
    bounds={(minX+maxX)*.5f,(minY+maxY)*.5f,maxX-minX,maxY-minY};
    u=minU;v=minV;
    constexpr float epsilon=.75f; // texel-centre sampling can inset each edge
    const float left=minU*320,top=minV*112,right=maxU*320,bottom=maxV*112;
    objectiveRing = std::fabs(top-56)<=epsilon && std::fabs(bottom-96)<=epsilon &&
        ((std::fabs(left-43)<=epsilon && std::fabs(right-83)<=epsilon) ||
         (std::fabs(left-1)<=epsilon && std::fabs(right-41)<=epsilon));
    return true;
}

UiMarkerAssets::Kind BoundMarkerAsset(ID3D11DeviceContext* context, bool observe = true)
{
    ID3D11ShaderResourceView* srv{}; context->PSGetShaderResources(0, 1, &srv);
    if (!srv) return UiMarkerAssets::Kind::None;
    ID3D11Resource* resource{}; srv->GetResource(&resource); srv->Release();
    if (!resource) return UiMarkerAssets::Kind::None;
    ID3D11Texture2D* texture{};
    const auto hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(hr) || !texture) return UiMarkerAssets::Kind::None;
    UiMarkerAssets::Kind kind{};
    if (observe) kind = g_markerAssets.Observe(context, texture, g_present);
    else UiMarkerAssets::ReadTag(texture, kind);
    texture->Release();
    return kind;
}

bool ReadObjectiveLabel(ID3D11DeviceContext* context,UiWorldRouting::SpriteBounds& bounds) {
    if(!ActiveObjective().draws || !ActiveLabel().next)return false;
    ID3D11ShaderResourceView* srv{};context->PSGetShaderResources(0,1,&srv);
    bool font=false;
    if(srv){ID3D11Resource* resource{};srv->GetResource(&resource);srv->Release();
        if(resource){ID3D11Texture2D* texture{};
            if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))) {
                D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);texture->Release();
                font=d.Width==1024 && d.Height==1024 && d.Format==DXGI_FORMAT_BC4_UNORM;
            }resource->Release();}}
    if(!font)return false;
    float c[136]{};ID3D11Buffer* cb{};context->VSGetConstantBuffers(0,1,&cb);
    const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));if(cb)cb->Release();
    if(!have)return false;
    for(unsigned i=0;i<16;++i)if(!std::isfinite(c[i]) || std::fabs(c[i]-(i%5==0?1.f:0.f))>.0001f)return false;
    if(std::fabs(c[32]-1.f/960)>.00001f || std::fabs(c[37]+1.f/540)>.00001f ||
       std::fabs(c[44]+1)>.001f || std::fabs(c[45]-1)>.001f)return false;
    bool ring{};float u{},v{};
    return ReadSpriteBounds(context,bounds,ring,u,v) &&
        ActiveLabel().Matches(bounds,g_ordinal.load(std::memory_order_relaxed));
}
bool BeginObjectiveLabel(ID3D11DeviceContext* context,FitState& fit) {
    UiWorldRouting::SpriteBounds bounds{};
    if(!ReadObjectiveLabel(context,bounds))return false;
    fit.viewportCount=1;context->RSGetViewports(&fit.viewportCount,&fit.viewport);
    fit.scissorCount=1;context->RSGetScissorRects(&fit.scissorCount,&fit.scissor);
    if(fit.viewportCount!=1 || fit.scissorCount>1)return false;
    context->RSGetState(&fit.savedRaster);fit.objectiveRaster=fit.active=true;
    const auto& f=ActiveObjective();const float scale=f.texelsPerCanvasPixel;
    D3D11_VIEWPORT vp{256-f.anchor.screen[0]*scale,256-(f.anchor.screen[1]-f.bodyShift)*scale,1920*scale,1080*scale,0,1};
    D3D11_RECT clip{0,0,ObjectiveWorld::TextureSize,ObjectiveWorld::TextureSize};
    context->RSSetState(g_objectiveRasterizer);context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&clip);
    ActiveLabel().Advance();return true;
}

// Captured: parking disk/quarter ring and focus magnifier start
// independent groups. Fonts and button glyphs are admitted only immediately
// after the exact focus atlas, inside that world's compact prompt rectangle.
struct AuxiliaryDraw { UiWorldRouting::SpriteBounds b{}; ObjectiveWorld::Anchor anchor{}; float x{},y{},diameter{}; bool first{},button{}; UiMarkerAssets::Kind kind{}; };
bool ReadAuxiliary(ID3D11DeviceContext* context,AuxiliaryDraw& d) {
    if(!g_hudWorldIcons.load() || !g_objectiveRasterizer)return false;
    d.kind=BoundMarkerAsset(context,false);
    if(d.kind!=UiMarkerAssets::Kind::FocusInfo && d.kind!=UiMarkerAssets::Kind::Parking && d.kind!=UiMarkerAssets::Kind::NpcTalk &&
       !(d.kind==UiMarkerAssets::Kind::None && g_auxiliary.used && g_ordinal.load()==g_auxiliary.last+1))return false;
    float c[136]{};ID3D11Buffer* cb{};context->VSGetConstantBuffers(0,1,&cb);
    const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));if(cb)cb->Release();
    bool ring{};float u{},v{};
    if(!have || !ReadSpriteBounds(context,d.b,ring,u,v) ||
       std::fabs(c[32]-1.f/960)>.00001f || std::fabs(c[37]+1.f/540)>.00001f ||
       std::fabs(c[44]+1)>.001f || std::fabs(c[45]-1)>.001f)return false;
    const auto ordinal=g_ordinal.load();
    const bool focus=d.kind==UiMarkerAssets::Kind::FocusInfo;
    const bool parking=d.kind==UiMarkerAssets::Kind::Parking;
    const bool same=g_auxiliary.used && ordinal==g_auxiliary.last+1 && g_auxiliary.used<40;
    const bool glass=focus && std::fabs(u*704-567)<.75f && std::fabs(v*528-171)<.75f;
    const bool speech=d.kind==UiMarkerAssets::Kind::NpcTalk;
    const bool halo=speech && std::fabs(u*176-1)<.75f && std::fabs(v*208-1)<.75f;
    const bool disk=parking && std::fabs(u*256-119)<.75f && std::fabs(v*192-1)<.75f;
    const bool quarter=parking && std::fabs(u*256-1)<.75f && std::fabs(v*192-119)<.75f && c[0]>0 && c[5]>0;
    if(halo || glass || disk || (quarter && (!same || g_auxiliary.kind!=d.kind))) {
        const float cx=halo?0.f:quarter?52.f:glass?23.f:56.f,cy=glass?24.f:cx; d.x=c[12]+cx*c[0]+cy*c[4];
        d.y=c[13]+cx*c[1]+cy*c[5]; // Exact pre-raster centre, before pixel rounding.
        d.diameter=halo?60.f:quarter?104*c[0]:d.b.width; // Speech halo animates; the bubble stays 60px.
        if(d.diameter<4 || d.diameter>256 || !ObjectivePositions::Find(d.x,d.y,d.anchor,128))return false;
        d.first=true;return true;
    }
    if(!same)return false;
    d.x=g_auxiliary.x;d.y=g_auxiliary.y;d.diameter=g_auxiliary.diameter;
    if(d.kind!=g_auxiliary.kind) {
        if(d.kind!=UiMarkerAssets::Kind::None || g_auxiliary.kind!=UiMarkerAssets::Kind::FocusInfo)return false;
        ID3D11ShaderResourceView* srv{};context->PSGetShaderResources(0,1,&srv);
        ID3D11Resource* resource{};if(srv){srv->GetResource(&resource);srv->Release();}
        ID3D11Texture2D* texture{};D3D11_TEXTURE2D_DESC desc{};
        if(resource){if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))){texture->GetDesc(&desc);texture->Release();}resource->Release();}
        const bool font=desc.Width==1024 && desc.Height==1024 && desc.Format==DXGI_FORMAT_BC4_UNORM;
        d.button=desc.Width==768 && desc.Height==768 && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;
        if((!font && !d.button) || (font && g_auxiliary.buttons) || (d.button && g_auxiliary.buttons>=2))return false;
        if(d.button && (d.b.width>64 || d.b.height>48 || std::fabs(d.b.x-d.x)>8 || d.b.y<d.y || d.b.y>d.y+64))return false;
        if(font && (d.b.width>48 || d.b.height>48 || d.b.y>d.y || d.b.y<d.y-80))return false;
    } else if(focus && !(std::fabs(u*704-515)<.75f && std::fabs(v*528-171)<.75f))return false;
    const float radius=parking?d.diameter*2:120;
    return d.b.width>0 && d.b.height>0 && d.b.width<=256 && d.b.height<=256 &&
        std::fabs(d.b.x-d.x)+d.b.width*.5f<=radius && std::fabs(d.b.y-d.y)+d.b.height*.5f<=radius;
}
bool BeginAuxiliary(ID3D11DeviceContext* context,FitState& fit) {
    AuxiliaryDraw d{};if(!ReadAuxiliary(context,d))return false;
    unsigned slot=g_auxiliary.slot;
    if(d.first){
        slot=ObjectiveWorld::MaxMarkers;
        for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i)if(g_objectiveFrames[i].draws && g_objectiveFrames[i].anchor.id==d.anchor.id){slot=i;break;}
        if(slot==ObjectiveWorld::MaxMarkers)for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i)if(!g_objectiveFrames[i].draws){slot=i;break;}
        if(slot==ObjectiveWorld::MaxMarkers)return false;
    }
    if(!g_objectiveRtvs[slot])return false;
    fit.viewportCount=1;context->RSGetViewports(&fit.viewportCount,&fit.viewport);
    fit.scissorCount=1;context->RSGetScissorRects(&fit.scissorCount,&fit.scissor);
    if(fit.viewportCount!=1 || fit.scissorCount>1)return false;
    g_objectiveSlot=slot;
    if(d.first){
        const float clear[4]{};context->ClearRenderTargetView(ActiveObjectiveRtv(),clear);
        ActiveObjective()={};ActiveLabel().Clear();g_objectiveSlotContent[slot]=false;
        auto& f=ActiveObjective();f.anchor=d.anchor;f.firstOrdinal=g_ordinal.load();
        f.canvasDiameter=d.diameter;f.texelsPerCanvasPixel=d.kind==UiMarkerAssets::Kind::Parking?std::fmin(2.f,110.f/d.diameter):2.f;
        f.bodyShift=f.anchor.screen[1]-d.y;
        g_auxiliary={d.kind,slot,0,0,0,d.x,d.y,d.diameter};
    }
    context->RSGetState(&fit.savedRaster);fit.objectiveRaster=fit.active=true;
    const auto scale=ActiveObjective().texelsPerCanvasPixel;
    D3D11_VIEWPORT vp{256-d.x*scale,256-d.y*scale,1920*scale,1080*scale,0,1};
    D3D11_RECT clip{0,0,512,512};context->RSSetState(g_objectiveRasterizer);context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&clip);
    g_auxiliary.last=g_ordinal.load();++g_auxiliary.used;if(d.button)++g_auxiliary.buttons;
    ++ActiveObjective().draws;return true;
}

bool OnInterfaceDraw(ID3D11DeviceContext* context, std::uintptr_t, int lane,
    std::uint32_t count, std::uint32_t start, LogFn log, bool* drop = nullptr)
{
    if (lane != LaneDrawIndexed || count != 6 || start != 0) return false;
    const auto kind = BoundMarkerAsset(context);
    AuxiliaryDraw auxiliary{};
    if(ReadAuxiliary(context,auxiliary)) {
        if(!TakeObjectiveBudget()){if(drop)*drop=true;return false;}
        return true;
    }
    if (kind == UiMarkerAssets::Kind::None) {
        UiWorldRouting::SpriteBounds label{};
        if(!ReadObjectiveLabel(context,label))return false;
        if(!TakeObjectiveBudget()){++g_worldCapHits;if(drop)*drop=true;return false;}
        return true;
    }
    if(kind==UiMarkerAssets::Kind::CommonInteraction) {
        if(!g_hudWorldIcons.load() || !g_interactionRtv)return false;
        if(!g_worldBudget.Take()){++g_worldCapHits;if(drop)*drop=true;return false;}
        return true;
    }
    // Identify these assets for capture only. Accepted visual routing is unchanged.
    if(kind==UiMarkerAssets::Kind::Interaction || kind==UiMarkerAssets::Kind::Parking || kind==UiMarkerAssets::Kind::FocusInfo || kind==UiMarkerAssets::Kind::NpcTalk) {
        if(RemainingUiProbe::enabled.load())UiWorldProbe::SeenMarker();
        return false;
    }
    if (kind == UiMarkerAssets::Kind::Objective || kind == UiMarkerAssets::Kind::SharedIcons)
    {
        UiWorldRouting::SpriteBounds bounds{}; bool ring=false; float u{},v{};
        const bool read=ReadSpriteBounds(context,bounds,ring,u,v);
        const auto ordinal=g_ordinal.load(std::memory_order_relaxed);
        bool matched=false;
        if (read && kind == UiMarkerAssets::Kind::Objective && ring)
            g_objectiveGroups.Add(bounds,ordinal);
        if (read && kind == UiMarkerAssets::Kind::SharedIcons)
            matched=g_objectiveGroups.Matches(bounds,ordinal,
                std::fabs(u*688-634)<.75f && std::fabs(v*704-220)<.75f);
        // One sampled pass per 600 presents; bounded asset draws only. This
        // names unresolved ordering/geometry instead of silently guessing.
        static std::uint32_t detailFrame=~0u, detailCount=0;
        if (detailFrame!=g_present) { detailFrame=g_present;detailCount=0; }
        if (ResearchDiagnostics::MarkerCapture && log && !(g_present%600) && detailCount++ < 128)
            log("[UIOBJECTIVE] asset=%s present=%u copy=%u ordinal=%u read=%u ring=%u "
                "joined=%u centre=(%.2f,%.2f) size=(%.2f,%.2f) uv=(%.5f,%.5f)",
                UiMarkerAssets::Name(kind),g_present,g_copy,ordinal,read?1u:0u,ring?1u:0u,
                matched?1u:0u,bounds.x,bounds.y,bounds.width,bounds.height,u,v);
        if (kind == UiMarkerAssets::Kind::SharedIcons)
        {
            if (!matched) { ++g_sharedIconPanel; return false; }
            ++g_sharedIconDraws;
        }
    }
    if (!(kind==UiMarkerAssets::Kind::Target ? g_worldBudget.Take() : TakeObjectiveBudget()))
    {
        // A known marker must never switch to the panel when the guard trips.
        // Dropping overflow bounds damage without creating a second layer copy.
        ++g_worldCapHits;
        if (drop) *drop = true;
        return false;
    }
    if (kind == UiMarkerAssets::Kind::Target) ++g_targetDraws; else ++g_objectiveDraws;
    if(kind==UiMarkerAssets::Kind::Target || (RemainingUiProbe::enabled.load() && kind==UiMarkerAssets::Kind::Objective)) UiWorldProbe::SeenMarker();
    if (UiWorldProbe::Sampling())
    {
        UiWorldRouting::SpriteBounds b{}; bool ring{}; float u{},v{};
        ID3D11Buffer* cb{}; context->VSGetConstantBuffers(0,1,&cb);
        float c[136]{};
        const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));
        if(cb) cb->Release();
        float x{},y{};
        if(have && ReadSpriteBounds(context,b,ring,u,v) && UiWorldRouting::ProjectSprite(c,0,0,x,y))
        {
            const float root[4]={c[12],c[13],c[0],c[5]};
            const float bounds[4]={b.x,b.y,b.width,b.height};
            const float uvRoot[4]={u,v,(x+1)*960,(1-y)*540};
            UiWorldProbe::Sprite(static_cast<unsigned>(kind),g_copy,
                g_ordinal.load(std::memory_order_relaxed),root,bounds,uvRoot);
        }
    }
    TraceMarker(context, kind, log);
    return true;
}

void Report(LogFn log, std::uint32_t presents)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!log) return;
    const auto stats = g_markerAssets.Stats();
    log("[UIMARK] present=%u targetDraws=%llu objectiveDraws=%llu capDrops=%llu layerClears=%llu "
        "assetAttempts=%u matches=%u mismatches=%u timeouts=%u hudPassClears=%llu joinedIcons=%llu panelIcons=%llu markerOnlyClears=%llu",
        presents, static_cast<unsigned long long>(g_targetDraws),
        static_cast<unsigned long long>(g_objectiveDraws), static_cast<unsigned long long>(g_worldCapHits),
        static_cast<unsigned long long>(g_layerBudgetResets), stats.attempts,
        stats.matches, stats.mismatches, stats.timeouts,
        static_cast<unsigned long long>(g_hudPassClearsWindow.exchange(0)),
        static_cast<unsigned long long>(g_sharedIconDraws),static_cast<unsigned long long>(g_sharedIconPanel),
        static_cast<unsigned long long>(g_markerOnlyClears));
    g_targetDraws = g_objectiveDraws = g_worldCapHits = g_layerBudgetResets = 0;
    g_sharedIconDraws=g_sharedIconPanel=g_markerOnlyClears=0;
}
bool BeginObjective(ID3D11DeviceContext* context,FitState& fit)
{
    if(!g_hudWorldIcons.load(std::memory_order_relaxed) || !ActiveObjectiveRtv() || !g_objectiveRasterizer) return false;
    if(BeginAuxiliary(context,fit))return true;
    const auto kind=BoundMarkerAsset(context,false);
    if(kind==UiMarkerAssets::Kind::FocusInfo || kind==UiMarkerAssets::Kind::Parking || kind==UiMarkerAssets::Kind::NpcTalk)return false;
    if(kind==UiMarkerAssets::Kind::None)return BeginObjectiveLabel(context,fit);
    if(kind!=UiMarkerAssets::Kind::Objective && kind!=UiMarkerAssets::Kind::SharedIcons) return false;
    UiWorldRouting::SpriteBounds bounds{};bool ring{};float u{},v{};
    float c[136]{}; ID3D11Buffer* cb{}; context->VSGetConstantBuffers(0,1,&cb);
    const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));
    if(cb)cb->Release();
    float root{};
    if(!have || !ReadSpriteBounds(context,bounds,ring,u,v) ||
       !ObjectiveWorld::RootX(static_cast<unsigned>(kind),c[12],c[0],u,v,root)) return false;
    ObjectiveWorld::Anchor anchor{};
    const auto ordinal=g_ordinal.load(std::memory_order_relaxed);
    // The close-range SWF moves the stem origin to its top. Recover the
    // attachment from the bottom; looking up the top fails beyond 128px.
    const bool stem=kind==UiMarkerAssets::Kind::Objective && std::fabs(u*320-1)<.75f && std::fabs(v*112-98)<.75f;
    const float anchorY=stem ? bounds.y+bounds.height*.5f-28.f : c[13];
    const bool found=ObjectivePositions::Find(root,anchorY,anchor);
    const bool newGroup=found && kind==UiMarkerAssets::Kind::Objective &&
        (!ActiveObjective().draws || anchor.id!=ActiveObjective().anchor.id ||
         !ObjectiveWorld::InGroup(ActiveObjective(),ordinal,bounds.x,bounds.y,bounds.width,bounds.height));
    if(newGroup) {
        unsigned chosen=ObjectiveWorld::MaxMarkers;
        for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i)
            if(g_objectiveFrames[i].draws && g_objectiveFrames[i].anchor.id==anchor.id){chosen=i;break;}
        if(chosen==ObjectiveWorld::MaxMarkers)for(unsigned i=0;i<ObjectiveWorld::MaxMarkers;++i)
            if(!g_objectiveFrames[i].draws){chosen=i;break;}
        if(chosen==ObjectiveWorld::MaxMarkers)return false;
        g_objectiveSlot=chosen;if(!ActiveObjectiveRtv())return false;
        const float clear[4]{};context->ClearRenderTargetView(ActiveObjectiveRtv(),clear);
        ActiveObjective()={};ActiveLabel().Clear();g_objectiveSlotContent[chosen]=false;
    }
    if(ActiveObjective().draws) {
        // Rotated quarter-rings have different transform origins; the tall stem
        // also moves the symbol beyond the projector lookup's 128px Y tolerance.
        // Once attached, keep this bounded contiguous group on ONE anchor/crop.
        if((found && anchor.id!=ActiveObjective().anchor.id) ||
           !ObjectiveWorld::InGroup(ActiveObjective(),ordinal,bounds.x,bounds.y,bounds.width,bounds.height))return false;
        anchor=ActiveObjective().anchor;
    } else if(!found) return false;
    // This is the real UI canvas projection, not cb0's unused camera block.
    if(std::fabs(c[32]-1.f/960)>.00001f || std::fabs(c[37]+1.f/540)>.00001f ||
       std::fabs(c[44]+1)>.001f || std::fabs(c[45]-1)>.001f) return false;
    fit.viewportCount=1;context->RSGetViewports(&fit.viewportCount,&fit.viewport);
    fit.scissorCount=1;context->RSGetScissorRects(&fit.scissorCount,&fit.scissor);
    if(fit.viewportCount!=1 || fit.scissorCount>1) return false;
    context->RSGetState(&fit.savedRaster);fit.objectiveRaster=fit.active=true;
    const float scale=ActiveObjective().draws ? ActiveObjective().texelsPerCanvasPixel :
        ObjectiveWorld::CaptureScale(anchor.screen[1],bounds.y,bounds.height);

    if(!ActiveObjective().draws && stem)
        ActiveObjective().bodyShift=anchor.screen[1]-(bounds.y-bounds.height*.5f)+28.f;
    // Main ring centre now sits at the world attachment; tail stays 20 canvas pixels.
    float scaleY=scale,topY=256-(anchor.screen[1]-ActiveObjective().bodyShift)*scale;
    if(stem && bounds.height>0) {
        scaleY=scale*20.f/bounds.height;
        topY=256+28.f*scale-(bounds.y-bounds.height*.5f)*scaleY;
    }
    D3D11_VIEWPORT vp{256-anchor.screen[0]*scale,topY,1920*scale,1080*scaleY,0,1};
    D3D11_RECT clip{0,0,ObjectiveWorld::TextureSize,ObjectiveWorld::TextureSize};
    context->RSSetState(g_objectiveRasterizer);
    context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&clip);
    if(!ActiveObjective().draws) {
        ActiveObjective().firstOrdinal=ordinal;
        ActiveObjective().texelsPerCanvasPixel=scale;
    }
    ActiveObjective().anchor=anchor;++ActiveObjective().draws;
    if(kind==UiMarkerAssets::Kind::SharedIcons)ActiveLabel().Start(bounds,ordinal);
    return true;
}

bool BeginTarget(ID3D11DeviceContext* context,FitState& fit) {
    if(!g_hudWorldIcons.load(std::memory_order_relaxed) || !g_targetRtv || !g_objectiveRasterizer ||
       BoundMarkerAsset(context,false)!=UiMarkerAssets::Kind::Target)return false;
    UiWorldRouting::SpriteBounds bounds{};bool ring{};float u{},v{};
    float c[136]{};ID3D11Buffer* cb{};context->VSGetConstantBuffers(0,1,&cb);
    const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));if(cb)cb->Release();
    if(!have || !ReadSpriteBounds(context,bounds,ring,u,v))return false;
    if(std::fabs(c[32]-1.f/960)>.00001f || std::fabs(c[37]+1.f/540)>.00001f ||
       std::fabs(c[44]+1)>.001f || std::fabs(c[45]-1)>.001f)return false;
    if(!g_targetGroup.count && (std::fabs(u*240-38)>.75f || std::fabs(v*176-117)>.75f))return false;
    fit.viewportCount=1;context->RSGetViewports(&fit.viewportCount,&fit.viewport);
    fit.scissorCount=1;context->RSGetScissorRects(&fit.scissorCount,&fit.scissor);
    if(fit.viewportCount!=1 || fit.scissorCount>1)return false;
    context->RSGetState(&fit.savedRaster);fit.objectiveRaster=fit.active=true;
    if(!g_targetGroup.count)g_targetGroup.captureScale=std::fmin(1.f,128.f/std::fmax(120.f*std::hypot(c[0],c[1]),1.f));
    g_targetGroup.Add(c[12],c[13],u,v);
    // Wide local canvas contains both opposite corners before their centre is resolved.
    const float scale=g_targetGroup.captureScale;
    D3D11_VIEWPORT vp{256-g_targetGroup.capture[0]*scale,256-g_targetGroup.capture[1]*scale,1920*scale,1080*scale,0,1};
    D3D11_RECT clip{0,0,512,512};context->RSSetState(g_objectiveRasterizer);
    context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&clip);return true;
}

// Common interaction atlas: car, bed, item, talk and other interaction symbols.
// Captured groups begin with four repeated quarter-ring quads, then arrow,
// stem and optional centre symbol. Only exact fingerprint + known group starts.
bool BeginInteraction(ID3D11DeviceContext* context,FitState& fit) {
    if(!g_hudWorldIcons.load() || !g_interactionRtv || !g_objectiveRasterizer ||
       BoundMarkerAsset(context,false)!=UiMarkerAssets::Kind::CommonInteraction)return false;
    float c[136]{};ID3D11Buffer* cb{};context->VSGetConstantBuffers(0,1,&cb);
    const bool have=cb && ReadVertex(cb,0,reinterpret_cast<std::uint8_t*>(c),sizeof(c));if(cb)cb->Release();
    UiWorldRouting::SpriteBounds b{};bool ring{};float u{},v{};
    if(!have || !ReadSpriteBounds(context,b,ring,u,v) ||
       std::fabs(c[32]-1.f/960)>.00001f || std::fabs(c[37]+1.f/540)>.00001f ||
       std::fabs(c[44]+1)>.001f || std::fabs(c[45]-1)>.001f)return false;
    const unsigned ordinal=g_ordinal.load();
    const bool quarter=(std::fabs(u*848-52)<.75f && std::fabs(v*768-527)<.75f) ||
        (std::fabs(u*848-819)<.75f && std::fabs(v*768-41)<.75f);
    const bool first=quarter && c[0]>0 && c[5]>0 && std::fabs(c[1])<.0001f && std::fabs(c[4])<.0001f;
    if(first) {
        ObjectiveWorld::Anchor anchor{};
        if(b.width<4 || b.width>64 || b.height<4 || b.height>64 ||
           !ObjectivePositions::Find(c[12],c[13],anchor,256))return false;
        const float clear[4]{};context->ClearRenderTargetView(g_interactionRtv,clear);
        g_interactionFrame={};g_interactionFrame.anchor=anchor;
        g_interactionFrame.firstOrdinal=ordinal;g_interactionFrame.texelsPerCanvasPixel=2;
        g_interactionFrame.canvasDiameter=std::fmax(40.f,b.height*2);
        g_interactionCentre[0]=c[12];g_interactionCentre[1]=b.y+b.height*.5f;
    } else if(!g_interactionFrame.draws || ordinal!=g_interactionLast+1 ||
              g_interactionFrame.draws>=16)return false;
    if(std::fabs(b.x-g_interactionCentre[0])>96 || b.width>192 || b.height>1024)return false;
    fit.viewportCount=1;context->RSGetViewports(&fit.viewportCount,&fit.viewport);
    fit.scissorCount=1;context->RSGetScissorRects(&fit.scissorCount,&fit.scissor);
    if(fit.viewportCount!=1 || fit.scissorCount>1)return false;
    context->RSGetState(&fit.savedRaster);fit.objectiveRaster=fit.active=true;
    float scaleY=2,top=256-g_interactionCentre[1]*2;
    const bool stem=std::fabs(u*848-60)<.75f && std::fabs(v*768-273)<.75f;
    if(stem && b.height>0) {
        // Keep the pointer attachment and a compact tail instead of the native
        // head-dependent stretched line. The symbol stays on the world point.
        scaleY=std::fmin(2.f,40.f/b.height);top=256+(b.y-b.height*.5f-g_interactionCentre[1])*2-(b.y-b.height*.5f)*scaleY;
    }
    D3D11_VIEWPORT vp{256-g_interactionCentre[0]*2,top,3840,1080*scaleY,0,1};
    D3D11_RECT clip{0,0,512,512};context->RSSetState(g_objectiveRasterizer);
    context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&clip);
    g_interactionLast=ordinal;++g_interactionFrame.draws;return true;
}

// Diagnostic only: the animated NPC speech box may bypass the HUD layer.
// Dimensions select evidence to record, never rendering ownership.
bool NpcProbeTexture(ID3D11DeviceContext* context) {
    ID3D11ShaderResourceView* srv{};context->PSGetShaderResources(0,1,&srv);
    if(!srv)return false;
    ID3D11Resource* resource{};srv->GetResource(&resource);srv->Release();
    if(!resource)return false;
    ID3D11Texture2D* texture{};D3D11_TEXTURE2D_DESC desc{};
    if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))){texture->GetDesc(&desc);texture->Release();}
    resource->Release();return desc.Width==176 && desc.Height==208;
}
void CaptureRemainingDraw(ID3D11DeviceContext* context,unsigned lane,unsigned count,unsigned start,unsigned diagnosticFlags=0) {
    if(!RemainingUiProbe::Sampling())return;
    RemainingUiProbe::Record r{};r.flags=diagnosticFlags;r.present=g_present;r.copy=g_copy;
    r.ordinal=g_ordinal.load(std::memory_order_relaxed);r.lane=lane;r.count=count;r.start=start;
    r.kind=static_cast<unsigned>(BoundMarkerAsset(context,false));r.contextType=context->GetType();r.baseVertex=t_baseVertex;
    LARGE_INTEGER ticks{};QueryPerformanceCounter(&ticks);r.ticks=ticks.QuadPart;
    ID3D11VertexShader* vs{};ID3D11PixelShader* ps{};
    context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
    r.vs=reinterpret_cast<std::uintptr_t>(vs);r.ps=reinterpret_cast<std::uintptr_t>(ps);
    if(vs)vs->Release();if(ps)ps->Release();
    D3D11_VIEWPORT vp{};UINT n=1;context->RSGetViewports(&n,&vp);
    r.viewport[0]=vp.TopLeftX;r.viewport[1]=vp.TopLeftY;r.viewport[2]=vp.Width;r.viewport[3]=vp.Height;r.viewport[4]=vp.MinDepth;r.viewport[5]=vp.MaxDepth;
    ID3D11ShaderResourceView* srv{};context->PSGetShaderResources(0,1,&srv);
    if(srv){ID3D11Resource* resource{};srv->GetResource(&resource);srv->Release();
        if(resource){r.texture=reinterpret_cast<std::uintptr_t>(resource);ID3D11Texture2D* texture{};
            if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture)))) {
                D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);r.texWidth=d.Width;r.texHeight=d.Height;r.texFormat=d.Format;texture->Release();}
            resource->Release();}}
    ID3D11Buffer* cb[2]{};context->VSGetConstantBuffers(0,2,cb);
    for(unsigned i=0;i<2;++i)if(cb[i]) {
        D3D11_BUFFER_DESC d{};cb[i]->GetDesc(&d);const auto bytes=std::min(d.ByteWidth,i?unsigned(sizeof(r.cb1)):unsigned(sizeof(r.cb0)));
        if(ReadVertex(cb[i],0,reinterpret_cast<std::uint8_t*>(i?r.cb1:r.cb0),bytes)) {
            (i?r.cb1Bytes:r.cb0Bytes)=bytes;r.flags|=1u<<i;}
        cb[i]->Release();}
    unsigned first=lane==LaneDraw?start:0;
    if(lane==LaneDrawIndexed || lane==LaneDrawIndexedInstanced) {
        ID3D11Buffer* ib{};DXGI_FORMAT format{};UINT offset{};context->IAGetIndexBuffer(&ib,&format,&offset);
        if(ib){D3D11_BUFFER_DESC d{};ib->GetDesc(&d);r.indexFormat=format;
            const unsigned unit=format==DXGI_FORMAT_R16_UINT?2:format==DXGI_FORMAT_R32_UINT?4:0;
            const std::uint64_t begin=std::uint64_t(offset)+std::uint64_t(start)*unit;
            const unsigned bytes=unit && begin<d.ByteWidth?std::min({48u,count*unit,d.ByteWidth-static_cast<unsigned>(begin)}):0;
            if(bytes && ReadVertex(ib,static_cast<unsigned>(begin),r.indices,bytes)) {
                r.indexBytes=bytes;r.flags|=8;first=~0u;
                for(unsigned j=0;j<bytes/unit;++j){unsigned index{};std::memcpy(&index,r.indices+j*unit,unit);first=std::min(first,index);}}
            ib->Release();}}
    r.firstVertex=first;
    ID3D11Buffer* vb{};UINT offset{};context->IAGetVertexBuffers(0,1,&vb,&r.stride,&offset);
    if(vb){D3D11_BUFFER_DESC d{};vb->GetDesc(&d);
        const auto index=static_cast<std::int64_t>(first)+(lane==LaneDrawIndexed || lane==LaneDrawIndexedInstanced?r.baseVertex:0);
        const auto begin=static_cast<std::int64_t>(offset)+index*r.stride;
        if(index>=0 && begin>=0 && begin<d.ByteWidth){const unsigned bytes=std::min(192u,d.ByteWidth-static_cast<unsigned>(begin));
            if(ReadVertex(vb,static_cast<unsigned>(begin),r.vertices,bytes)){r.vertexBytes=bytes;r.flags|=4;}}
        vb->Release();}
    RemainingUiProbe::Append(r);
}

} // namespace UiWorld


// The one place every draw in the process passes through.
void BeginDraw(ID3D11DeviceContext* context, Lane lane, UINT count, UINT instances,
    FitState& fit, UINT start = 0)
{
    fit.active = false;
    if (!context)
        return;
    if (t_modRender)
    {
        g_modDraws.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // [DLSSP] resolve identification (read-only; only <=6-vertex draws inspect).
    DlssProbe::OnDraw(context, count,
        lane == LaneDrawIndexed || lane == LaneDrawIndexedInstanced);
    DlssUpscaler::OnGeometryDraw(context);   // [MVFIX] eye camera capture, first draw after a velocity-pass bind
    // [DLSS] The engine's temporal resolve is a full-screen draw; when DLSS has
    // produced its output the engine's draw is skipped.  NGX and the module's compute
    // pass issue their own calls, so they run under the mod-render guard.
    if (count > 0 && count <= 6)
    {
        ++t_modRender;
        const bool replaced = DlssUpscaler::OnDraw(context);
        --t_modRender;
        if (replaced)
        {
            fit.drop = true;
            return;
        }
    }
    // No consumer and an identity fit means there is no UI work to perform.
    // Keep research census intact when explicitly compiled in.
    if constexpr (!ResearchDiagnostics::Enabled && !ResearchDiagnostics::MarkerCapture) {
        const bool layer = g_hudLayerEnabled.load(std::memory_order_relaxed) &&
            g_hudConsumer.load(std::memory_order_relaxed);
        const bool fitNeeded = g_fitEnabled.load(std::memory_order_relaxed) &&
            (std::fabs(g_fitScaleX.load(std::memory_order_relaxed)-1.0f) >= 1e-4f ||
             std::fabs(g_fitScaleY.load(std::memory_order_relaxed)-1.0f) >= 1e-4f ||
             std::fabs(g_fitOffsetX.load(std::memory_order_relaxed)) >= 1e-4f ||
             std::fabs(g_fitOffsetY.load(std::memory_order_relaxed)) >= 1e-4f);
        if (!layer && !fitNeeded) return;
    }
    if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::OnDraw(context, count, start, UiWorld::t_baseVertex, static_cast<unsigned>(lane));
    LARGE_INTEGER t0{};
    if constexpr (ResearchDiagnostics::Enabled) QueryPerformanceCounter(&t0);

    const auto ordinal = g_ordinal.fetch_add(1, std::memory_order_relaxed);
    if (context == g_immediate)
        ++g_hudDrawsSinceRedirect;
    if constexpr (ResearchDiagnostics::Enabled) {
        g_laneSum[lane].fetch_add(1, std::memory_order_relaxed);
        if (context == g_immediate)
            g_thisImmediate.fetch_add(1, std::memory_order_relaxed);
        else if (context == g_deferredProbe)
            g_thisDeferredProbe.fetch_add(1, std::memory_order_relaxed);
        else
            g_thisOther.fetch_add(1, std::memory_order_relaxed);
    }

    if constexpr (!ResearchDiagnostics::Enabled && !ResearchDiagnostics::MarkerCapture) {
        if (t_rt.valid && t_rt.context == context && t_rt.depth) return; // [RTCACHE]
    }
    ID3D11RenderTargetView* rtv{};
    ID3D11DepthStencilView* dsv{};
    context->OMGetRenderTargets(1, &rtv, &dsv);
    const bool depth = dsv != nullptr;
    t_rt = RtCache{context, depth, true};
    if (dsv) dsv->Release();
    if constexpr (!ResearchDiagnostics::Enabled && !ResearchDiagnostics::MarkerCapture) {
        // The existing UI predicate always rejects depth-bound draws. Avoid
        // shader/blend/cache queries for those draws without changing routing.
        if (depth) { if (rtv) rtv->Release(); return; }
    }
    RtFacts rt{};
    if (rtv) { rt = LookupRtv(rtv); rtv->Release(); }
    ID3D11PixelShader* shader{};
    UINT classInstances = 0;
    context->PSGetShader(&shader, nullptr, &classInstances);
    const auto key = reinterpret_cast<std::uintptr_t>(shader);
    if (shader) shader->Release();
    if (!key) g_noShader.fetch_add(1, std::memory_order_relaxed);

    ID3D11BlendState* blendState{};
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
    std::uint32_t blendRule = kBlendRuleNone;
    const bool blend = LookupBlend(blendState, &blendRule);
    if (blendState)
        blendState->Release();

    const auto displayW = g_displayW.load(std::memory_order_relaxed);
    const auto displayH = g_displayH.load(std::memory_order_relaxed);
    const bool fullDisplay = displayW && rt.w == displayW && rt.h == displayH;
    // [UIGATE] v2 rule, from the draw census.  Interface work is
    // alpha-blended, binds NO depth-stencil, and covers the display.  World
    // transparency is blended too but always carries the scene depth (the
    // census shows every blended R11G11B10 draw with depth=1), and every
    // full-size post pass on the sprite target is unblended.
    const bool candidate = blend && !depth && IsLdrFormat(rt.fmt) && fullDisplay;
    if (rt.isBackBuffer)
        g_backbufferDraws.fetch_add(1, std::memory_order_relaxed);
    if (candidate)
        g_candidateDraws.fetch_add(1, std::memory_order_relaxed);

    PsSlot* slot = ResearchDiagnostics::Enabled && key ? SlotFor(key) : nullptr;
    if (slot)
    {
        slot->calls.fetch_add(1, std::memory_order_relaxed);
        if (candidate)
            slot->uiCalls.fetch_add(1, std::memory_order_relaxed);
        slot->lanes.fetch_or(1u << lane, std::memory_order_relaxed);
        slot->lastOrdinal.store(ordinal, std::memory_order_relaxed);
        slot->lastCount.store(count, std::memory_order_relaxed);
        slot->lastInstances.store(instances, std::memory_order_relaxed);
        slot->blend.store(blend ? 1u : 0u, std::memory_order_relaxed);
        slot->fmt.store(rt.fmt, std::memory_order_relaxed);
        slot->rtW.store(rt.w, std::memory_order_relaxed);
        slot->rtH.store(rt.h, std::memory_order_relaxed);
        slot->isBackBuffer.store(rt.isBackBuffer ? 1u : 0u, std::memory_order_relaxed);
        slot->depth.store(depth ? 1u : 0u, std::memory_order_relaxed);
        std::uint32_t probed = 0;
        if (slot->probed.compare_exchange_strong(probed, 1u,
                std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            UINT viewportCount = 1;
            D3D11_VIEWPORT viewport{};
            context->RSGetViewports(&viewportCount, &viewport);
            if (viewportCount)
            {
                slot->vpW.store(static_cast<std::uint32_t>(viewport.Width),
                    std::memory_order_relaxed);
                slot->vpH.store(static_cast<std::uint32_t>(viewport.Height),
                    std::memory_order_relaxed);
            }
        }
    }

    // [HUDLAYER] Where on the target does this draw land, and is it textured?
    //  - Native SBS draws the interface once per eye, squeezed into each
    //    half-width viewport.  The layer keeps the LEFT copy and drops the
    //    right one; the quad shows the left half unsqueezed.
    //  - An UNTEXTURED interface draw (no PS resource in slot 0) is a solid
    //    fill: a fade or a dimming veil over the world.  On a 1.6 m quad it
    //    reads as a black rectangle, so it stays in the game frame instead.
    bool leftHalf = false;
    bool rightHalf = false;
    bool untextured = false;
    if (candidate)
    {
        UINT viewportCount = 1;
        D3D11_VIEWPORT viewport{};
        context->RSGetViewports(&viewportCount, &viewport);
        if (viewportCount && displayW &&
            viewport.Width * 2.0f <= static_cast<float>(displayW) + 2.0f)
        {
            if (viewport.TopLeftX < static_cast<float>(displayW) * 0.25f)
                leftHalf = true;
            else
                rightHalf = true;
        }
        ID3D11ShaderResourceView* srv{};
        context->PSGetShaderResources(0, 1, &srv);
        untextured = srv == nullptr;
        if (srv)
            srv->Release();
        if (leftHalf)
            g_hudLeft.fetch_add(1, std::memory_order_relaxed);
        if (rightHalf)
            g_hudRight.fetch_add(1, std::memory_order_relaxed);
        if (untextured)
            g_hudUntextured.fetch_add(1, std::memory_order_relaxed);
        if (slot)
        {
            if (leftHalf)
                slot->leftHalf.fetch_add(1, std::memory_order_relaxed);
            if (rightHalf)
                slot->rightHalf.fetch_add(1, std::memory_order_relaxed);
            if (untextured)
            {
                slot->untextured.fetch_add(1, std::memory_order_relaxed);
                if (slot->passLogged.exchange(1u, std::memory_order_acq_rel) == 0)
                    LogF("[HUDLAYER] passthrough ps=%llX lane=%s n=%u ordinal=%u/%u: "
                         "untextured fill stays in the game frame",
                        static_cast<unsigned long long>(key), kLaneNames[lane], count,
                        ordinal, g_lastFrameTotal.load(std::memory_order_relaxed));
            }
        }
    }
    // Output tag, telemetry only.  Do not route on it: measured,
    // the interface is drawn after the engine's per-output passes, so the TLS
    // output at that point is stale - per-shader o0/o1 splits came out in
    // random ratios, and dropping "output 1" copies threw away most of the
    // HUD on some frames (kept 212, dropped 1977 in one window): the flashing
    // loading screens, menus and minimap in native stereo.
    if (candidate && g_readOutput && slot)
    {
        const int output = g_readOutput() & 1;
        (output ? slot->output1 : slot->output0).fetch_add(1, std::memory_order_relaxed);
    }
    bool layerWants = candidate && !untextured &&
        g_hudLayerEnabled.load(std::memory_order_relaxed) &&
        g_hudConsumer.load(std::memory_order_relaxed);
    if (layerWants)
    {
        // [HUDALPHA] Only draws whose blend has a scalar coverage go on the
        // layer; the rest are composited by the game itself.
        const auto displayFmt = g_displayFmt.load(std::memory_order_relaxed);
        // FFXV composes sprites in FP16 before converting to the SDR 10-bit
        // backbuffer. Match that intermediate target, not the final output.
        // Keep one format per output mode so unrelated 8-bit passes cannot
        // recreate the layer and discard the interface collected so far.
        const auto hudFmt = displayFmt == DXGI_FORMAT_R10G10B10A2_UNORM
            ? static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT) : displayFmt;
        if (hudFmt && rt.fmt != hudFmt)
        {
            // Other full-size targets stay in the game frame.
            layerWants = false;
            g_hudFmtPass.fetch_add(1, std::memory_order_relaxed);
        }
        else if (blendRule == kBlendRuleNone)
        {
            layerWants = false;
            g_hudBlendPass.fetch_add(1, std::memory_order_relaxed);
            if (slot && slot->blendLogged.exchange(1u, std::memory_order_acq_rel) == 0)
                LogF("[HUDLAYER] passthrough ps=%llX lane=%s n=%u fmt=%u: blend has no "
                     "scalar coverage, stays in the game frame",
                    static_cast<unsigned long long>(key), kLaneNames[lane], count, rt.fmt);
        }
    }
    if(!layerWants && RemainingUiProbe::Sampling() && UiWorld::NpcProbeTexture(context))
        UiWorld::CaptureRemainingDraw(context,static_cast<unsigned>(lane),count,start,
            (1u<<16)|(depth?1u<<17:0)|(fullDisplay?1u<<18:0));
    if (layerWants && rightHalf)
    {
        fit.drop = true;
        g_hudDropped.fetch_add(1, std::memory_order_relaxed);
    }
    else if (layerWants && EnsureHudTarget(rt.w, rt.h, rt.fmt))
    {
        // [HUDLAYER] Redirect: the game draws this interface element into
        // the mod's texture with its own shaders and viewport; only the target and
        // the alpha rule change.  The game frame keeps the world only.
        const std::uint64_t signature = (static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ull) ^
            (static_cast<std::uint64_t>(lane) << 56) ^
            (static_cast<std::uint64_t>(count) << 28) ^ static_cast<std::uint64_t>(start);
        if (!g_hudFrameCleared)
        {
            const float clear[4]{};
            context->ClearRenderTargetView(g_hudRtv, clear);
            if (g_hudWorldRtv) context->ClearRenderTargetView(g_hudWorldRtv, clear);
            ClearObjectiveTextures(context,clear);
            if (g_targetRtv) context->ClearRenderTargetView(g_targetRtv, clear);
            if(g_interactionRtv)context->ClearRenderTargetView(g_interactionRtv,clear);
            UiWorld::OnLayerClear(false);
            g_hudFrameContent.store(false, std::memory_order_relaxed);
            g_hudWorldFrameContent.store(false, std::memory_order_relaxed);
            g_hudFrameCleared = true;
            g_hudFirstSignature = signature;
        }
        else if (g_hudNativeStereo.load(std::memory_order_relaxed) &&
            g_hudDrawsSinceRedirect > kHudPassGap &&
            (signature == g_hudFirstSignature ||
                (g_hudDrawsThisFrame >= 150 && !g_hudPassBulkThisFrame)))
        {
            if (signature != g_hudFirstSignature)
                g_hudPassBulkThisFrame = true;
            // The signature match alone missed the second copy
            // (CPU-projected target markers doubled on the panel
            // with per-output parallax), so a gap after a BULK of interface
            // draws (>= 150, i.e. a whole HUD copy; the early mono group is a
            // handful) also counts as the second copy's start.
            // [HUDPASS] A second copy of the interface starts here: native
            // stereo only, a long run of non-interface draws since the last
            // interface draw (the second output's whole world), AND this draw
            // is identical to the frame's first interface draw (same shader,
            // draw type, size, start).  The last condition matters: in mono
            // some interface shaders draw early in the frame (~draw 516) and
            // others at the very end (~8760), and a gap alone would wipe the
            // early group.  Rebuild the layer from this copy so translucent
            // panels are never blended twice and no frame is left empty.
            const float clear[4]{};
            context->ClearRenderTargetView(g_hudRtv, clear);
            if (g_hudWorldRtv) context->ClearRenderTargetView(g_hudWorldRtv, clear);
            ClearObjectiveTextures(context,clear);
            if (g_targetRtv) context->ClearRenderTargetView(g_targetRtv, clear);
            if(g_interactionRtv)context->ClearRenderTargetView(g_interactionRtv,clear);
            UiWorld::OnLayerClear(true);
            g_hudFrameContent.store(false, std::memory_order_relaxed);
            g_hudWorldFrameContent.store(false, std::memory_order_relaxed);
            g_hudPassClears.fetch_add(1, std::memory_order_relaxed);
            g_hudPassClearsWindow.fetch_add(1, std::memory_order_relaxed);
            g_hudDrawsThisFrame = 0;
        }
        else if (g_hudNativeStereo.load(std::memory_order_relaxed) &&
            g_hudDrawsSinceRedirect > kHudPassGap &&
            UiWorld::g_interfaceDrawsSinceClear >= 150 &&
            g_hudWorldRtv && (g_hudWorldFrameContent.load(std::memory_order_relaxed) || g_objectiveContent.load(std::memory_order_relaxed) || g_targetContent.load(std::memory_order_relaxed) || g_interactionContent.load(std::memory_order_relaxed)))
        {
            // Measured: objective sets at ordinals 11219
            // and 20190 both had copy=1. An early HUD burst had already spent
            // the panel's one bulk replacement, so both eye copies accumulated
            // in the marker image. Give markers their own replacement after
            // every completed HUD burst/world gap. Keep panel policy intact.
            const float clear[4]{};
            context->ClearRenderTargetView(g_hudWorldRtv, clear);
            ClearObjectiveTextures(context,clear);
            if (g_targetRtv) context->ClearRenderTargetView(g_targetRtv, clear);
            if(g_interactionRtv)context->ClearRenderTargetView(g_interactionRtv,clear);
            UiWorld::OnLayerClear(true);
            g_hudWorldFrameContent.store(false, std::memory_order_relaxed);
            if constexpr (ResearchDiagnostics::Enabled) ++UiWorld::g_markerOnlyClears;
            if (ResearchDiagnostics::Enabled && (UiWorld::g_markerOnlyClears <= 4 || !(UiWorld::g_present % 600)))
                LogF("[UIMARKPASS] present=%u copy=%u ordinal=%u gap=%u marker-only replacement",
                    UiWorld::g_present,UiWorld::g_copy,ordinal,g_hudDrawsSinceRedirect);
        }
        g_hudDrawsSinceRedirect = 0;
        ++g_hudDrawsThisFrame;
        ++UiWorld::g_interfaceDrawsSinceClear;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
            fit.savedRtvs, &fit.savedDsv);
        const bool toWorld = g_hudWorldRtv && g_hudWorldRouting.load(std::memory_order_relaxed) &&
            UiWorld::OnInterfaceDraw(context, key, lane, count, start, g_log, &fit.drop);
        UiWorld::CaptureRemainingDraw(context,static_cast<unsigned>(lane),count,start);
        const bool toObjective=toWorld && !fit.drop && UiWorld::BeginObjective(context,fit);
        const bool toTarget=toWorld && !fit.drop && !toObjective && UiWorld::BeginTarget(context,fit);
        const bool toInteraction=toWorld && !fit.drop && !toObjective && !toTarget && UiWorld::BeginInteraction(context,fit);
        if(toWorld && !toInteraction && UiWorld::BoundMarkerAsset(context,false)==UiMarkerAssets::Kind::CommonInteraction)fit.drop=true;
        context->OMSetRenderTargets(1,toInteraction ? &g_interactionRtv : toTarget ? &g_targetRtv : toObjective ? &ActiveObjectiveRtv() : toWorld ? &g_hudWorldRtv : &g_hudRtv,nullptr);
        context->OMGetBlendState(&fit.savedBlend, fit.savedFactor, &fit.savedMask);
        if (auto* derived = DerivedBlendFor(fit.savedBlend, blendRule))
            context->OMSetBlendState(derived, fit.savedFactor, fit.savedMask);
        fit.redirected = true;
        if(toObjective)g_objectiveSlotContent[g_objectiveSlot]=true;
        if (toWorld)
        {
            (toInteraction ? g_interactionContent : toTarget ? g_targetContent : toObjective ? g_objectiveContent : g_hudWorldFrameContent).store(true, std::memory_order_relaxed);
            g_hudWorldRouted.fetch_add(1, std::memory_order_relaxed);
        }
        else
            g_hudFrameContent.store(true, std::memory_order_relaxed);
        g_hudRedirects.fetch_add(1, std::memory_order_relaxed);
        g_hudRedirectTotal.fetch_add(1, std::memory_order_relaxed);
        if (g_hudLogged.load(std::memory_order_relaxed) < 8 && slot &&
            slot->fitLogged.exchange(1u, std::memory_order_acq_rel) == 0)
        {
            g_hudLogged.fetch_add(1, std::memory_order_relaxed);
            LogF("[HUDLAYER] redirecting ps=%llX lane=%s n=%u ordinal=%u/%u fmt=%u",
                static_cast<unsigned long long>(key), kLaneNames[lane], count,
                ordinal, g_lastFrameTotal.load(std::memory_order_relaxed), rt.fmt);
        }
    }
    else if (candidate && !untextured &&
        g_fitEnabled.load(std::memory_order_relaxed) && BeginFit(context, fit))
    {
        g_fitDraws.fetch_add(1, std::memory_order_relaxed);
        g_fittedWindow.fetch_add(1, std::memory_order_relaxed);
        std::uint32_t logged = 0;
        if (slot && slot->fitLogged.compare_exchange_strong(logged, 1u,
                std::memory_order_acq_rel, std::memory_order_relaxed))
            LogF("[UIGATE] fitting ps=%llX lane=%s n=%u inst=%u ordinal=%u/%u "
                 "rt=%ux%u fmt=%u bb=%u depth=%u vp=%.0fx%.0f@%.0f,%.0f",
                static_cast<unsigned long long>(key), kLaneNames[lane], count,
                instances, ordinal, g_lastFrameTotal.load(std::memory_order_relaxed),
                rt.w, rt.h, rt.fmt, rt.isBackBuffer ? 1u : 0u, depth ? 1u : 0u,
                fit.viewport.Width, fit.viewport.Height, fit.viewport.TopLeftX,
                fit.viewport.TopLeftY);
    }

    if (g_log && !(UiWorld::g_present % 600) && lane == LaneDrawIndexed && count == 6)
    {
        const auto asset = UiWorld::BoundMarkerAsset(context, false);
        static std::uint32_t sampleFrame=~0u, samples=0;
        if (sampleFrame!=UiWorld::g_present) { sampleFrame=UiWorld::g_present;samples=0; }
        if ((asset==UiMarkerAssets::Kind::Target || asset==UiMarkerAssets::Kind::Objective) && samples++<128)
        {
            ID3D11RenderTargetView* selected{};context->OMGetRenderTargets(1,&selected,nullptr);
            const char* route = fit.drop ? "drop" : selected==g_targetRtv ? "target-3d" : selected==ActiveObjectiveRtv() ? "objective-3d" : selected==g_hudWorldRtv ? "marker" :
                selected==g_hudRtv ? "panel" : "game";
            if (selected) selected->Release();
            LogF("[UIMARKSOURCE] asset=%s present=%u copy=%u ordinal=%u route=%s "
                "sourceRtv=%p size=%ux%u fmt=%u depth=%u blend=%u coverage=%u start=%u base=%d",
                UiMarkerAssets::Name(asset),UiWorld::g_present,UiWorld::g_copy,ordinal,route,
                static_cast<void*>(rtv),rt.w,rt.h,rt.fmt,depth?1u:0u,blend?1u:0u,blendRule,start,
                UiWorld::t_baseVertex);
        }
    }

    LARGE_INTEGER t1{};
    if constexpr (ResearchDiagnostics::Enabled)
    {
        QueryPerformanceCounter(&t1);
        g_hookTicks.fetch_add(static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart),
            std::memory_order_relaxed);
    }
}

// ---- detours -------------------------------------------------------------
template <int V>
void STDMETHODCALLTYPE DetourDrawIndexed(ID3D11DeviceContext* context, UINT indexCount,
    UINT startIndex, INT baseVertex)
{
    const bool pt = PerfCensus::OnPresentThread();
    PerfCensus::Add(pt ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (!t_modRender && FlashProbe::OnDraw(context, indexCount, startIndex, baseVertex, true, g_readOutput ? g_readOutput() : 0, &UiWorld::ReadLive)) return; // [FLASHPROBE] kill switch
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) // [PERF] Numpad6: detour body off, FullEye kept
    {
        g_orig[V].drawIndexed(context, indexCount, startIndex, baseVertex);
        if(!t_modRender){++t_modRender;FullEye::Replay(context,indexCount,[&](){g_orig[V].drawIndexed(context,indexCount,startIndex,baseVertex);});--t_modRender;}
        return;
    }
    const auto t0 = PerfCensus::Tsc();
    std::uint64_t mod = 0;
    FitState fit{};
    UiWorld::t_baseVertex = baseVertex;
    BeginDraw(context, LaneDrawIndexed, indexCount, 1, fit, startIndex);
    if (!fit.drop)
    {
        if (!t_modRender) { const auto p0 = PerfCensus::Tsc(); NativeDrawPose::BeforeDraw(context); PerfCensus::Add(PerfCensus::PoseTicks, PerfCensus::Tsc() - p0); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        const auto t1 = PerfCensus::Tsc(); mod += t1 - t0;
        g_orig[V].drawIndexed(context, indexCount, startIndex, baseVertex);
        const auto t2 = PerfCensus::Tsc();
        if(!t_modRender){++t_modRender;FullEye::Replay(context,indexCount,[&](){g_orig[V].drawIndexed(context,indexCount,startIndex,baseVertex);});--t_modRender;}
        EndFit(context, fit);
        mod += PerfCensus::Tsc() - t2;
    }
    else { EndFit(context, fit); mod += PerfCensus::Tsc() - t0; }
    PerfCensus::Add(pt ? PerfCensus::UiTicksPt : PerfCensus::UiTicksOther, mod);
}

template <int V>
void STDMETHODCALLTYPE DetourDraw(ID3D11DeviceContext* context, UINT vertexCount,
    UINT startVertex)
{
    const bool pt = PerfCensus::OnPresentThread();
    PerfCensus::Add(pt ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (!t_modRender && FlashProbe::OnDraw(context, vertexCount, startVertex, 0, false, g_readOutput ? g_readOutput() : 0, &UiWorld::ReadLive)) return; // [FLASHPROBE] kill switch
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) // [PERF] Numpad6: detour body off (lightning guard too), FullEye kept
    {
        g_orig[V].draw(context, vertexCount, startVertex);
        if(!t_modRender){++t_modRender;FullEye::Replay(context,vertexCount,[&](){g_orig[V].draw(context,vertexCount,startVertex);});--t_modRender;}
        return;
    }
    const auto t0 = PerfCensus::Tsc();
    std::uint64_t mod = 0;
    FitState fit{};
    BeginDraw(context, LaneDraw, vertexCount, 1, fit, startVertex);
    if (!fit.drop)
    {
        if (!t_modRender) { const auto p0 = PerfCensus::Tsc(); NativeDrawPose::BeforeDraw(context); PerfCensus::Add(PerfCensus::PoseTicks, PerfCensus::Tsc() - p0); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        const auto l0 = PerfCensus::Tsc();
        const bool reject = LightningGeometryFix::Reject(context, vertexCount, startVertex);
        const auto t1 = PerfCensus::Tsc(); PerfCensus::Add(PerfCensus::LightningTicks, t1 - l0); mod += t1 - t0;
        std::uint64_t t2 = t1;
        if (!reject)
        {
            g_orig[V].draw(context, vertexCount, startVertex);
            t2 = PerfCensus::Tsc();
            if(!t_modRender){++t_modRender;FullEye::Replay(context,vertexCount,[&](){g_orig[V].draw(context,vertexCount,startVertex);});--t_modRender;}
        }
        EndFit(context, fit);
        mod += PerfCensus::Tsc() - t2;
    }
    else { EndFit(context, fit); mod += PerfCensus::Tsc() - t0; }
    PerfCensus::Add(pt ? PerfCensus::UiTicksPt : PerfCensus::UiTicksOther, mod);
}

template <int V>
void STDMETHODCALLTYPE DetourDrawIndexedInstanced(ID3D11DeviceContext* context,
    UINT indexCountPerInstance, UINT instanceCount, UINT startIndex, INT baseVertex,
    UINT startInstance)
{
    const bool pt = PerfCensus::OnPresentThread();
    PerfCensus::Add(pt ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (!t_modRender && FlashProbe::OnDraw(context, indexCountPerInstance, startIndex, baseVertex, true, g_readOutput ? g_readOutput() : 0, &UiWorld::ReadLive, true)) return; // [FLASHPROBE] kill switch
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) // [PERF] Numpad6
    {
        g_orig[V].drawIndexedInstanced(context, indexCountPerInstance, instanceCount, startIndex, baseVertex, startInstance);
        if(!t_modRender && instanceCount==1){++t_modRender;FullEye::Replay(context,indexCountPerInstance,[&](){g_orig[V].drawIndexedInstanced(context,indexCountPerInstance,instanceCount,startIndex,baseVertex,startInstance);});--t_modRender;}
        return;
    }
    const auto t0 = PerfCensus::Tsc();
    std::uint64_t mod = 0;
    FitState fit{};
    UiWorld::t_baseVertex = baseVertex;
    BeginDraw(context, LaneDrawIndexedInstanced, indexCountPerInstance, instanceCount, fit,
        startIndex);
    if (!fit.drop)
    {
        if (!t_modRender) { const auto p0 = PerfCensus::Tsc(); NativeDrawPose::BeforeDraw(context); PerfCensus::Add(PerfCensus::PoseTicks, PerfCensus::Tsc() - p0); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        const auto t1 = PerfCensus::Tsc(); mod += t1 - t0;
        g_orig[V].drawIndexedInstanced(context, indexCountPerInstance, instanceCount,
        startIndex, baseVertex, startInstance);
        const auto t2 = PerfCensus::Tsc();
        if(!t_modRender && instanceCount==1){++t_modRender;FullEye::Replay(context,indexCountPerInstance,[&](){g_orig[V].drawIndexedInstanced(context,indexCountPerInstance,instanceCount,startIndex,baseVertex,startInstance);});--t_modRender;}
        EndFit(context, fit);
        mod += PerfCensus::Tsc() - t2;
    }
    else { EndFit(context, fit); mod += PerfCensus::Tsc() - t0; }
    PerfCensus::Add(pt ? PerfCensus::UiTicksPt : PerfCensus::UiTicksOther, mod);
}

template <int V>
void STDMETHODCALLTYPE DetourDrawInstanced(ID3D11DeviceContext* context,
    UINT vertexCountPerInstance, UINT instanceCount, UINT startVertex, UINT startInstance)
{
    const bool pt = PerfCensus::OnPresentThread();
    PerfCensus::Add(pt ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (!t_modRender && FlashProbe::OnDraw(context, vertexCountPerInstance, startVertex, 0, false, g_readOutput ? g_readOutput() : 0, &UiWorld::ReadLive, true)) return; // [FLASHPROBE] kill switch
    // The flash probe observes this path too. Validate the per-vertex stream
    // before either the normal draw or the diagnostic UI bypass can submit it.
    if (!t_modRender && instanceCount && LightningGeometryFix::Reject(context, vertexCountPerInstance, startVertex)) return;
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) // [PERF] Numpad6
    {
        g_orig[V].drawInstanced(context, vertexCountPerInstance, instanceCount, startVertex, startInstance);
        return;
    }
    const auto t0 = PerfCensus::Tsc();
    std::uint64_t mod = 0;
    FitState fit{};
    BeginDraw(context, LaneDrawInstanced, vertexCountPerInstance, instanceCount, fit,
        startVertex);
    if (!fit.drop)
    {
        if (!t_modRender) { const auto p0 = PerfCensus::Tsc(); NativeDrawPose::BeforeDraw(context); PerfCensus::Add(PerfCensus::PoseTicks, PerfCensus::Tsc() - p0); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        const auto t1 = PerfCensus::Tsc(); mod += t1 - t0;
        g_orig[V].drawInstanced(context, vertexCountPerInstance, instanceCount, startVertex,
        startInstance);
        const auto t2 = PerfCensus::Tsc();
        EndFit(context, fit);
        mod += PerfCensus::Tsc() - t2;
    }
    else { EndFit(context, fit); mod += PerfCensus::Tsc() - t0; }
    PerfCensus::Add(pt ? PerfCensus::UiTicksPt : PerfCensus::UiTicksOther, mod);
}

template <int V>
void STDMETHODCALLTYPE DetourDrawAuto(ID3D11DeviceContext* context)
{
    PerfCensus::Add(PerfCensus::OnPresentThread() ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) { g_orig[V].drawAuto(context); return; }
    FitState fit{};
    BeginDraw(context, LaneDrawAuto, 0, 1, fit);
    if (!fit.drop)
    {
        if (!t_modRender) { NativeDrawPose::BeforeDraw(context); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        g_orig[V].drawAuto(context);
    }
    EndFit(context, fit);
}

template <int V>
void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect(ID3D11DeviceContext* context,
    ID3D11Buffer* args, UINT offset)
{
    PerfCensus::Add(PerfCensus::OnPresentThread() ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) { g_orig[V].drawIndexedInstancedIndirect(context, args, offset); return; }
    FitState fit{};
    BeginDraw(context, LaneIndexedIndirect, 0, 1, fit);
    if (!fit.drop)
    {
        if (!t_modRender) { NativeDrawPose::BeforeDraw(context); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        g_orig[V].drawIndexedInstancedIndirect(context, args, offset);
    }
    EndFit(context, fit);
}

template <int V>
void STDMETHODCALLTYPE DetourDrawInstancedIndirect(ID3D11DeviceContext* context,
    ID3D11Buffer* args, UINT offset)
{
    PerfCensus::Add(PerfCensus::OnPresentThread() ? PerfCensus::DrawPt : PerfCensus::DrawOther);
    if (PerfCensus::bypassUiDraw.load(std::memory_order_relaxed)) { g_orig[V].drawInstancedIndirect(context, args, offset); return; }
    FitState fit{};
    BeginDraw(context, LaneIndirect, 0, 1, fit);
    if (!fit.drop)
    {
        if (!t_modRender) { NativeDrawPose::BeforeDraw(context); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::BeforeDraw(context); }
        g_orig[V].drawInstancedIndirect(context, args, offset);
    }
    EndFit(context, fit);
}

template <int V>
void STDMETHODCALLTYPE DetourExecuteCommandList(ID3D11DeviceContext* context,
    ID3D11CommandList* list, BOOL restoreState)
{
    g_execute.fetch_add(1, std::memory_order_relaxed);
    PerfCensus::Add(PerfCensus::CmdListExec);
    t_rt.valid = false; // [RTCACHE] the list's state, or a cleared state, is bound afterwards
    NativeDrawPose::BeginExecute();
    g_orig[V].executeCommandList(context, list, restoreState);
    NativeDrawPose::Executed(list);
    if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::Executed(list);
}

// Measured: no wireframe bind while stereo is on, 34 per present
// from the first mono frame after stereo-off, none again once stereo is back.
// The engine's own stereo-off leaves per-output state behind that picks a
// wireframe rasterizer.  Capture the call stacks of the first few wireframe
// binds and of one solid bind for comparison: same engine caller with a
// different state index points at the table the residue lives in.
std::atomic_uint32_t g_wireStacksLogged{};
std::atomic_bool g_solidStackLogged{};

void LogBindStack(const char* kind, ID3D11RasterizerState* state, int variant)
{
    void* frames[16]{};
    const auto count = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    char line[1200];
    int n = std::snprintf(line, sizeof(line), "[WIRE] %s bind state=%p ctx=%s stack:",
        kind, static_cast<void*>(state), variant ? "deferred" : "immediate");
    for (USHORT i = 0; i < count && n < static_cast<int>(sizeof(line)) - 80; ++i)
    {
        char where[96];
        ModuleNameOf(frames[i], where, sizeof(where));
        n += std::snprintf(line + n, sizeof(line) - n, " %s", where);
    }
    LogF("%s", line);
}

// [WIREFIX] Captured wireframe binds: the renderer's
// command-buffer executor (+0x2DB009D -> +0x2DB0200) applies a recorded
// state block (+0x2DB4E20), which hashes the recorded rasterizer desc
// (+0x2DB41B0) and binds the matching state (+0x2DB5065, RSSetState).  The
// wireframe fill is in the RECORDED DATA - corrupt per-output state after a
// stereo toggle - not a debug mode being selected.  Retail FFXV never draws
// gameplay in wireframe, so every wireframe state is replaced with a solid
// twin that has the identical desc.  Twins are verified against the original
// desc on every use, so a recycled state pointer can never get a stale twin.
constexpr std::size_t kSolidTwinSlots = 32;
std::atomic_uintptr_t g_solidTwinFrom[kSolidTwinSlots]{};
std::atomic<ID3D11RasterizerState*> g_solidTwinTo[kSolidTwinSlots]{};
std::atomic_uint64_t g_wireReplaced{};
std::atomic_uint64_t g_wireReplaceFailed{};

bool SameDescExceptFill(const D3D11_RASTERIZER_DESC& a, const D3D11_RASTERIZER_DESC& b)
{
    return a.CullMode == b.CullMode && a.FrontCounterClockwise == b.FrontCounterClockwise &&
        a.DepthBias == b.DepthBias && a.DepthBiasClamp == b.DepthBiasClamp &&
        a.SlopeScaledDepthBias == b.SlopeScaledDepthBias &&
        a.DepthClipEnable == b.DepthClipEnable && a.ScissorEnable == b.ScissorEnable &&
        a.MultisampleEnable == b.MultisampleEnable &&
        a.AntialiasedLineEnable == b.AntialiasedLineEnable;
}

ID3D11RasterizerState* SolidTwin(ID3D11RasterizerState* state, const D3D11_RASTERIZER_DESC& desc)
{
    const auto key = reinterpret_cast<std::uintptr_t>(state);
    for (std::size_t i = 0; i < kSolidTwinSlots; ++i)
    {
        if (g_solidTwinFrom[i].load(std::memory_order_acquire) != key)
            continue;
        auto* twin = g_solidTwinTo[i].load(std::memory_order_acquire);
        if (twin)
        {
            D3D11_RASTERIZER_DESC twinDesc{};
            twin->GetDesc(&twinDesc);
            if (twinDesc.FillMode == D3D11_FILL_SOLID && SameDescExceptFill(twinDesc, desc))
                return twin;
        }
        // recycled pointer or not yet published: rebuild this slot's twin
        D3D11_RASTERIZER_DESC solid = desc;
        solid.FillMode = D3D11_FILL_SOLID;
        ID3D11RasterizerState* fresh{};
        if (!g_device || FAILED(g_device->CreateRasterizerState(&solid, &fresh)) || !fresh)
            return nullptr;
        g_solidTwinTo[i].store(fresh, std::memory_order_release); // old twin leaked on purpose
        return fresh;
    }
    D3D11_RASTERIZER_DESC solid = desc;
    solid.FillMode = D3D11_FILL_SOLID;
    ID3D11RasterizerState* fresh{};
    if (!g_device || FAILED(g_device->CreateRasterizerState(&solid, &fresh)) || !fresh)
        return nullptr;
    for (std::size_t i = 0; i < kSolidTwinSlots; ++i)
    {
        std::uintptr_t expected = 0;
        if (g_solidTwinFrom[i].compare_exchange_strong(expected, key,
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            g_solidTwinTo[i].store(fresh, std::memory_order_release);
            LogF("[WIREFIX] solid twin %p for wireframe state %p (cull=%d depthClip=%d slot=%zu)",
                static_cast<void*>(fresh), static_cast<void*>(state),
                static_cast<int>(desc.CullMode), desc.DepthClipEnable ? 1 : 0, i);
            return fresh;
        }
    }
    return fresh; // table full: still use it, it just is not cached
}

template <int V>
void STDMETHODCALLTYPE DetourRSSetState(ID3D11DeviceContext* context,
    ID3D11RasterizerState* state)
{
    if (state)
    {
        D3D11_RASTERIZER_DESC desc{};
        state->GetDesc(&desc); // always fresh: pointers get recycled
        if (desc.FillMode == D3D11_FILL_WIREFRAME)
        {
            g_wireSetsThisFrame.fetch_add(1, std::memory_order_relaxed);
            g_wireLastState.store(reinterpret_cast<std::uintptr_t>(state),
                std::memory_order_relaxed);
            if (g_wireStacksLogged.fetch_add(1, std::memory_order_relaxed) < 6)
                LogBindStack("WIREFRAME", state, V);
            if (auto* twin = SolidTwin(state, desc))
            {
                g_wireReplaced.fetch_add(1, std::memory_order_relaxed);
                g_orig[V].rsSetState(context, twin);
                return;
            }
            g_wireReplaceFailed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_orig[V].rsSetState(context, state);
}

template <int V>
HRESULT STDMETHODCALLTYPE DetourFinishCommandList(ID3D11DeviceContext* context,
    BOOL restoreState, ID3D11CommandList** list)
{
    g_finish.fetch_add(1, std::memory_order_relaxed);
    t_rt.valid = false; // [RTCACHE] FinishCommandList clears the deferred context's state
    const HRESULT result = g_orig[V].finishCommandList(context, restoreState, list);
    if (SUCCEEDED(result) && list && *list) { NativeDrawPose::Finished(context,*list); if constexpr(ResearchDiagnostics::Enabled || kPoseTraceEnabled) GpuPoseTrace::Finished(context, *list); }
    return result;
}

template<int V> HRESULT STDMETHODCALLTYPE PoseDetourMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    const auto hr=g_orig[V].map(c,r,s,type,flags,out);
    const bool pt=PerfCensus::OnPresentThread(); PerfCensus::Add(pt?PerfCensus::MapPt:PerfCensus::MapOther); const auto t0=PerfCensus::Tsc();
    if(SUCCEEDED(hr)&&out){MvFix::OnMapped(r,type,out->pData);FlashProbe::OnFill(r,c->GetType()==D3D11_DEVICE_CONTEXT_IMMEDIATE?FlashProbe::FillMapImm:FlashProbe::FillMapDef);LightningGeometryFix::Invalidate(r);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexMapped(r,s,type,out->pData);NativeDrawPose::Mapped(c,r,type,out->pData);UiWorld::OnMapped(r,type,out->pData);DlssProbe::OnMapped(r,type,out->pData);DlssUpscaler::OnMapped(r,type,out->pData);}
    PerfCensus::Add(pt?PerfCensus::MapTicksPt:PerfCensus::MapTicksOther,PerfCensus::Tsc()-t0); return hr;
}

template<int V> void STDMETHODCALLTYPE PoseDetourUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) {
    const auto t0=PerfCensus::Tsc();
    if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexUnmapping(r,s);NativeDrawPose::Unmapping(c,r);UiWorld::OnUnmapping(r);DlssProbe::OnUnmapping(r);DlssUpscaler::OnUnmapping(r);
    PerfCensus::Add(PerfCensus::OnPresentThread()?PerfCensus::MapTicksPt:PerfCensus::MapTicksOther,PerfCensus::Tsc()-t0);
    MvFix::OnUnmapping(r); // [MVFIX]
    g_orig[V].unmap(c,r,s);
}

template<int V> void STDMETHODCALLTYPE PoseDetourCopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* box) {
    FlashProbe::OnFill(d,FlashProbe::FillCopyRegion,s,box?box->left:0u,x);LightningGeometryFix::Invalidate(d);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexCopying(d,s,x,box);NativeDrawPose::Copied(d);g_orig[V].copyRegion(c,d,ds,x,y,z,s,ss,box);if(ds==0&&ss==0)UiWorld::OnCopiedRegion(d,x,s,box);
}

template<int V> void STDMETHODCALLTYPE PoseDetourCopyResource(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) {
    FlashProbe::OnFill(d,FlashProbe::FillCopy,s,0,0);LightningGeometryFix::Invalidate(d);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexCopying(d,s,0,nullptr);NativeDrawPose::Copied(d);g_orig[V].copyResource(c,d,s);UiWorld::OnCopied(d,s);
}

template<int V> void STDMETHODCALLTYPE PoseDetourUpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* box, const void* data, UINT row, UINT depth) {
    FlashProbe::OnFill(d,FlashProbe::FillUpdate);if(FlashProbe::g_enabled.load(std::memory_order_relaxed)==1)FlashUpload::Updated(c,d,ds,box,data);LightningGeometryFix::Updated(c,d,ds,box,data);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexUpdated(d,ds,box,data);if(ds==0)MvFix::OnUpdate(d,box,data);g_orig[V].update(c,d,ds,box,data,row,depth);NativeDrawPose::Updated(c,d,ds,box,data,false);if(ds==0){UiWorld::OnUpdated(d,box,data);DlssUpscaler::OnUpdated(d,box,data);}
}

template<int V> void STDMETHODCALLTYPE PoseDetourCopySubresourceRegion1(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* box, UINT flags) {
    FlashProbe::OnFill(d,FlashProbe::FillCopyRegion,s,box?box->left:0u,x);LightningGeometryFix::Invalidate(d);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexCopying(d,s,x,box);NativeDrawPose::Copied(d);g_orig[V].copyRegion1(c,d,ds,x,y,z,s,ss,box,flags);if(ds==0&&ss==0)UiWorld::OnCopiedRegion(d,x,s,box);
}

template<int V> void STDMETHODCALLTYPE PoseDetourUpdateSubresource1(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* box, const void* data, UINT row, UINT depth, UINT flags) {
    FlashProbe::OnFill(d,FlashProbe::FillUpdate);if(FlashProbe::g_enabled.load(std::memory_order_relaxed)==1)FlashUpload::Updated(c,d,ds,box,data);LightningGeometryFix::Updated(c,d,ds,box,data);if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::VertexUpdated(d,ds,box,data);if(ds==0)MvFix::OnUpdate(d,box,data);g_orig[V].update1(c,d,ds,box,data,row,depth,flags);NativeDrawPose::Updated(c,d,ds,box,data,true);if(ds==0){UiWorld::OnUpdated(d,box,data);DlssUpscaler::OnUpdated(d,box,data);}
}

// [GBUF] Render-target bindings and depth clears, fed to the depth/velocity
// census before the engine's call lands.  Same MinHook route as the draws.
template <int V>
void STDMETHODCALLTYPE DetourOMSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv)
{
    if (!t_modRender)
        if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::OnSetRenderTargets(context, count, views, dsv);
    if (!t_modRender)
    {
        DlssProbe::OnSetRenderTargets(count, views, dsv);
        DlssUpscaler::OnSetRenderTargets(context, count, views, dsv);
    }
    t_rt = RtCache{context, dsv != nullptr, true}; // [RTCACHE]
    g_orig[V].omSetRenderTargets(context, count, views, dsv);
}

template <int V>
void STDMETHODCALLTYPE DetourOMSetRenderTargetsAndUav(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv, UINT uavStart,
    UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
{
    if (!t_modRender && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::OnSetRenderTargets(context, count, views, dsv);
    if (!t_modRender && count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    {
        DlssProbe::OnSetRenderTargets(count, views, dsv);
        DlssUpscaler::OnSetRenderTargets(context, count, views, dsv);
    }
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) t_rt = RtCache{context, dsv != nullptr, true}; // [RTCACHE]
    g_orig[V].omSetRenderTargetsAndUav(context, count, views, dsv, uavStart, uavCount, uavs,
        initialCounts);
}

template <int V>
void STDMETHODCALLTYPE DetourClearDepthStencilView(ID3D11DeviceContext* context,
    ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (!t_modRender)
        if constexpr(ResearchDiagnostics::Enabled) GBufferCensus::OnClearDepth(context, dsv);
    g_orig[V].clearDepthStencilView(context, dsv, flags, depth, stencil);
}

// [TEXBIAS] Texture detail: a negative mip LOD bias makes the GPU pick a
// sharper mip level on world textures (ground, walls at a distance).  Applied
// to pixel-shader samplers that wrap/mirror (world materials; post-process
// passes clamp) with mip filtering and no comparison.  Each eligible game
// sampler gets a biased clone, attached to it as private data so its lifetime
// follows the original; the clone is rebuilt when the bias changes.  0 = the
// game's own samplers untouched (no per-call work).
std::atomic<float> g_textureBias{0.0f};
const GUID kBiasInfoTag = {0x7e3a1c52, 0x44d1, 0x4b7e, {0x9c, 0x1a, 0x2f, 0x61, 0x0b, 0x8d, 0x33, 0x01}};
const GUID kBiasCloneTag = {0x7e3a1c52, 0x44d1, 0x4b7e, {0x9c, 0x1a, 0x2f, 0x61, 0x0b, 0x8d, 0x33, 0x02}};
struct BiasInfo { float bias; int state; };   // state 1 = not eligible, 2 = clone attached
std::atomic<std::uint64_t> g_biasClones{};
// Returns an AddRef'd biased clone, or null to bind the original.
ID3D11SamplerState* BiasedSampler(ID3D11SamplerState* sampler, float bias)
{
    if (!sampler) return nullptr;
    BiasInfo info{};
    UINT size = sizeof(info);
    if (FAILED(sampler->GetPrivateData(kBiasInfoTag, &size, &info)) || size != sizeof(info)) info = {};
    if (info.state == 1) return nullptr;
    if (info.state == 2 && info.bias == bias)
    {
        IUnknown* unknown{};
        UINT pointerSize = sizeof(unknown);
        if (SUCCEEDED(sampler->GetPrivateData(kBiasCloneTag, &pointerSize, &unknown)) && unknown)
        {
            ID3D11SamplerState* clone{};
            unknown->QueryInterface(__uuidof(ID3D11SamplerState), reinterpret_cast<void**>(&clone));
            unknown->Release();
            return clone;
        }
    }
    D3D11_SAMPLER_DESC desc{};
    sampler->GetDesc(&desc);
    const bool wraps = desc.AddressU == D3D11_TEXTURE_ADDRESS_WRAP || desc.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR ||
        desc.AddressV == D3D11_TEXTURE_ADDRESS_WRAP || desc.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR;
    const bool mips = D3D11_DECODE_MIP_FILTER(desc.Filter) == D3D11_FILTER_TYPE_LINEAR ||
        D3D11_DECODE_IS_ANISOTROPIC_FILTER(desc.Filter);
    const bool comparison = D3D11_DECODE_IS_COMPARISON_FILTER(desc.Filter) != 0;
    if (!wraps || !mips || comparison || desc.MaxLOD <= 0.0f)
    {
        info = {bias, 1};
        sampler->SetPrivateData(kBiasInfoTag, sizeof(info), &info);
        return nullptr;
    }
    desc.MipLODBias = std::clamp(desc.MipLODBias + bias, -16.0f, 15.99f);
    ID3D11Device* device{};
    sampler->GetDevice(&device);
    ID3D11SamplerState* clone{};
    const HRESULT hr = device ? device->CreateSamplerState(&desc, &clone) : E_FAIL;
    if (device) device->Release();
    if (FAILED(hr) || !clone) return nullptr;
    sampler->SetPrivateDataInterface(kBiasCloneTag, clone);
    info = {bias, 2};
    sampler->SetPrivateData(kBiasInfoTag, sizeof(info), &info);
    if (g_biasClones.fetch_add(1, std::memory_order_relaxed) < 3)
        LogF("[TEXBIAS] biased sampler clone bias=%+.2f (game bias %+.2f, filter 0x%X)", bias, desc.MipLODBias - bias, static_cast<unsigned>(desc.Filter));
    return clone;   // one reference for the caller, one held by the original
}

template <int V>
void STDMETHODCALLTYPE DetourPSSetSamplers(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11SamplerState* const* samplers)
{
    const float bias = g_textureBias.load(std::memory_order_relaxed);
    if (bias == 0.0f || t_modRender || !samplers || count == 0 || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
    {
        g_orig[V].psSetSamplers(context, start, count, samplers);
        return;
    }
    ID3D11SamplerState* bound[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
    ID3D11SamplerState* clones[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]{};
    for (UINT i = 0; i < count; ++i)
    {
        clones[i] = BiasedSampler(samplers[i], bias);
        bound[i] = clones[i] ? clones[i] : samplers[i];
    }
    g_orig[V].psSetSamplers(context, start, count, bound);
    for (UINT i = 0; i < count; ++i)
        if (clones[i]) clones[i]->Release();
}

// [DLSS] compute dispatches: the second eye's empty-input fix and the
// resolve-reader census.  The engine's call is forwarded unchanged.
template <int V>
void STDMETHODCALLTYPE DetourDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    if (!t_modRender)
    {
        DlssProbe::OnDispatch(context);
        DlssUpscaler::OnDispatch(context);
    }
    g_orig[V].dispatch(context, x, y, z);
}

template <int V>
void STDMETHODCALLTYPE DetourDispatchIndirect(ID3D11DeviceContext* context,
    ID3D11Buffer* args, UINT offset)
{
    if (!t_modRender)
    {
        DlssProbe::OnDispatch(context);
        DlssUpscaler::OnDispatch(context);
    }
    g_orig[V].dispatchIndirect(context, args, offset);
}

struct HookSpec
{
    int slot;
    const char* name;
    void* detour[2];
    void** original[2];
};

const HookSpec kHooks[] = {
    {14, "Map", {reinterpret_cast<void*>(&PoseDetourMap<0>),reinterpret_cast<void*>(&PoseDetourMap<1>)},
        {reinterpret_cast<void**>(&g_orig[0].map),reinterpret_cast<void**>(&g_orig[1].map)}},
    {15, "Unmap", {reinterpret_cast<void*>(&PoseDetourUnmap<0>),reinterpret_cast<void*>(&PoseDetourUnmap<1>)},
        {reinterpret_cast<void**>(&g_orig[0].unmap),reinterpret_cast<void**>(&g_orig[1].unmap)}},
    {46, "CopySubresourceRegion", {reinterpret_cast<void*>(&PoseDetourCopySubresourceRegion<0>),reinterpret_cast<void*>(&PoseDetourCopySubresourceRegion<1>)},
        {reinterpret_cast<void**>(&g_orig[0].copyRegion),reinterpret_cast<void**>(&g_orig[1].copyRegion)}},
    {47, "CopyResource", {reinterpret_cast<void*>(&PoseDetourCopyResource<0>),reinterpret_cast<void*>(&PoseDetourCopyResource<1>)},
        {reinterpret_cast<void**>(&g_orig[0].copyResource),reinterpret_cast<void**>(&g_orig[1].copyResource)}},
    {48, "UpdateSubresource", {reinterpret_cast<void*>(&PoseDetourUpdateSubresource<0>),reinterpret_cast<void*>(&PoseDetourUpdateSubresource<1>)},
        {reinterpret_cast<void**>(&g_orig[0].update),reinterpret_cast<void**>(&g_orig[1].update)}},
    {115, "CopySubresourceRegion1", {reinterpret_cast<void*>(&PoseDetourCopySubresourceRegion1<0>),reinterpret_cast<void*>(&PoseDetourCopySubresourceRegion1<1>)},
        {reinterpret_cast<void**>(&g_orig[0].copyRegion1),reinterpret_cast<void**>(&g_orig[1].copyRegion1)}},
    {116, "UpdateSubresource1", {reinterpret_cast<void*>(&PoseDetourUpdateSubresource1<0>),reinterpret_cast<void*>(&PoseDetourUpdateSubresource1<1>)},
        {reinterpret_cast<void**>(&g_orig[0].update1),reinterpret_cast<void**>(&g_orig[1].update1)}},
    {12, "DrawIndexed",
        {reinterpret_cast<void*>(&DetourDrawIndexed<0>),
            reinterpret_cast<void*>(&DetourDrawIndexed<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawIndexed),
            reinterpret_cast<void**>(&g_orig[1].drawIndexed)}},
    {13, "Draw",
        {reinterpret_cast<void*>(&DetourDraw<0>), reinterpret_cast<void*>(&DetourDraw<1>)},
        {reinterpret_cast<void**>(&g_orig[0].draw), reinterpret_cast<void**>(&g_orig[1].draw)}},
    {20, "DrawIndexedInstanced",
        {reinterpret_cast<void*>(&DetourDrawIndexedInstanced<0>),
            reinterpret_cast<void*>(&DetourDrawIndexedInstanced<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawIndexedInstanced),
            reinterpret_cast<void**>(&g_orig[1].drawIndexedInstanced)}},
    {21, "DrawInstanced",
        {reinterpret_cast<void*>(&DetourDrawInstanced<0>),
            reinterpret_cast<void*>(&DetourDrawInstanced<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawInstanced),
            reinterpret_cast<void**>(&g_orig[1].drawInstanced)}},
    {38, "DrawAuto",
        {reinterpret_cast<void*>(&DetourDrawAuto<0>),
            reinterpret_cast<void*>(&DetourDrawAuto<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawAuto),
            reinterpret_cast<void**>(&g_orig[1].drawAuto)}},
    {39, "DrawIndexedInstancedIndirect",
        {reinterpret_cast<void*>(&DetourDrawIndexedInstancedIndirect<0>),
            reinterpret_cast<void*>(&DetourDrawIndexedInstancedIndirect<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawIndexedInstancedIndirect),
            reinterpret_cast<void**>(&g_orig[1].drawIndexedInstancedIndirect)}},
    {40, "DrawInstancedIndirect",
        {reinterpret_cast<void*>(&DetourDrawInstancedIndirect<0>),
            reinterpret_cast<void*>(&DetourDrawInstancedIndirect<1>)},
        {reinterpret_cast<void**>(&g_orig[0].drawInstancedIndirect),
            reinterpret_cast<void**>(&g_orig[1].drawInstancedIndirect)}},
    {41, "Dispatch",
        {reinterpret_cast<void*>(&DetourDispatch<0>),
            reinterpret_cast<void*>(&DetourDispatch<1>)},
        {reinterpret_cast<void**>(&g_orig[0].dispatch),
            reinterpret_cast<void**>(&g_orig[1].dispatch)}},
    {42, "DispatchIndirect",
        {reinterpret_cast<void*>(&DetourDispatchIndirect<0>),
            reinterpret_cast<void*>(&DetourDispatchIndirect<1>)},
        {reinterpret_cast<void**>(&g_orig[0].dispatchIndirect),
            reinterpret_cast<void**>(&g_orig[1].dispatchIndirect)}},
    {10, "PSSetSamplers",
        {reinterpret_cast<void*>(&DetourPSSetSamplers<0>),
            reinterpret_cast<void*>(&DetourPSSetSamplers<1>)},
        {reinterpret_cast<void**>(&g_orig[0].psSetSamplers),
            reinterpret_cast<void**>(&g_orig[1].psSetSamplers)}},
    {43, "RSSetState",
        {reinterpret_cast<void*>(&DetourRSSetState<0>),
            reinterpret_cast<void*>(&DetourRSSetState<1>)},
        {reinterpret_cast<void**>(&g_orig[0].rsSetState),
            reinterpret_cast<void**>(&g_orig[1].rsSetState)}},
    {33, "OMSetRenderTargets",
        {reinterpret_cast<void*>(&DetourOMSetRenderTargets<0>),
            reinterpret_cast<void*>(&DetourOMSetRenderTargets<1>)},
        {reinterpret_cast<void**>(&g_orig[0].omSetRenderTargets),
            reinterpret_cast<void**>(&g_orig[1].omSetRenderTargets)}},
    {34, "OMSetRenderTargetsAndUnorderedAccessViews",
        {reinterpret_cast<void*>(&DetourOMSetRenderTargetsAndUav<0>),
            reinterpret_cast<void*>(&DetourOMSetRenderTargetsAndUav<1>)},
        {reinterpret_cast<void**>(&g_orig[0].omSetRenderTargetsAndUav),
            reinterpret_cast<void**>(&g_orig[1].omSetRenderTargetsAndUav)}},
    {53, "ClearDepthStencilView",
        {reinterpret_cast<void*>(&DetourClearDepthStencilView<0>),
            reinterpret_cast<void*>(&DetourClearDepthStencilView<1>)},
        {reinterpret_cast<void**>(&g_orig[0].clearDepthStencilView),
            reinterpret_cast<void**>(&g_orig[1].clearDepthStencilView)}},
    {58, "ExecuteCommandList",
        {reinterpret_cast<void*>(&DetourExecuteCommandList<0>),
            reinterpret_cast<void*>(&DetourExecuteCommandList<1>)},
        {reinterpret_cast<void**>(&g_orig[0].executeCommandList),
            reinterpret_cast<void**>(&g_orig[1].executeCommandList)}},
    {114, "FinishCommandList",
        {reinterpret_cast<void*>(&DetourFinishCommandList<0>),
            reinterpret_cast<void*>(&DetourFinishCommandList<1>)},
        {reinterpret_cast<void**>(&g_orig[0].finishCommandList),
            reinterpret_cast<void**>(&g_orig[1].finishCommandList)}},
};

void DumpShaders()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    struct Entry
    {
        PsSlot* slot;
        std::uint64_t calls;
        std::uint64_t ui;
    };
    std::array<Entry, kPsSlots> entries{};
    std::size_t count = 0;
    for (auto& slot : g_ps)
    {
        if (!slot.ps.load(std::memory_order_relaxed))
            continue;
        entries[count++] = Entry{&slot, slot.calls.load(std::memory_order_relaxed),
            slot.uiCalls.load(std::memory_order_relaxed)};
    }
    const auto frameTotal = g_lastFrameTotal.load(std::memory_order_relaxed);
    const auto line = [&](const char* title, const Entry& e)
    {
        char lanes[96];
        LaneString(e.slot->lanes.load(std::memory_order_relaxed), lanes, sizeof(lanes));
        LogF("[D3DHOOK] %s ps=%llX %s calls=%llu ui=%llu last=%u/%u n=%u inst=%u "
             "blend=%u fmt=%u rt=%ux%u bb=%u depth=%u vp=%ux%u L=%u R=%u untex=%u o0=%u o1=%u",
            title, static_cast<unsigned long long>(e.slot->ps.load(std::memory_order_relaxed)),
            lanes, static_cast<unsigned long long>(e.calls),
            static_cast<unsigned long long>(e.ui),
            e.slot->lastOrdinal.load(std::memory_order_relaxed), frameTotal,
            e.slot->lastCount.load(std::memory_order_relaxed),
            e.slot->lastInstances.load(std::memory_order_relaxed),
            e.slot->blend.load(std::memory_order_relaxed),
            e.slot->fmt.load(std::memory_order_relaxed),
            e.slot->rtW.load(std::memory_order_relaxed),
            e.slot->rtH.load(std::memory_order_relaxed),
            e.slot->isBackBuffer.load(std::memory_order_relaxed),
            e.slot->depth.load(std::memory_order_relaxed),
            e.slot->vpW.load(std::memory_order_relaxed),
            e.slot->vpH.load(std::memory_order_relaxed),
            e.slot->leftHalf.load(std::memory_order_relaxed),
            e.slot->rightHalf.load(std::memory_order_relaxed),
            e.slot->untextured.load(std::memory_order_relaxed),
            e.slot->output0.load(std::memory_order_relaxed),
            e.slot->output1.load(std::memory_order_relaxed));
    };
    LogF("[D3DHOOK] shaders=%zu dropped=%llu drawsLastFrame=%u", count,
        static_cast<unsigned long long>(g_psDropped.load(std::memory_order_relaxed)),
        frameTotal);
    // Interface candidates first: everything the gate has ever accepted.
    std::sort(entries.begin(), entries.begin() + count,
        [](const Entry& a, const Entry& b) { return a.ui > b.ui; });
    std::size_t shown = 0;
    for (std::size_t i = 0; i < count && shown < 32; ++i)
    {
        if (entries[i].ui == 0)
            break;
        ++shown;
        line("UI  ", entries[i]);
    }
    if (!shown)
        LogF("[D3DHOOK] UI   none - the gate accepted no draw yet");
    // Then the busiest shaders overall, which is where the world lives.
    std::sort(entries.begin(), entries.begin() + count,
        [](const Entry& a, const Entry& b) { return a.calls > b.calls; });
    for (std::size_t i = 0; i < count && i < 16; ++i)
        line("BUSY", entries[i]);
    // And the last draws of the frame: whatever composites the interface
    // sits here even if the gate above never accepted it.
    std::sort(entries.begin(), entries.begin() + count,
        [](const Entry& a, const Entry& b)
        {
            return a.slot->lastOrdinal.load(std::memory_order_relaxed) >
                b.slot->lastOrdinal.load(std::memory_order_relaxed);
        });
    for (std::size_t i = 0; i < count && i < 12; ++i)
        line("LATE", entries[i]);
}
} // namespace

bool Install(ID3D11Device* device, ID3D11DeviceContext* immediate,
    ID3D11DeviceContext* deferred, LogFn log, ReadOutputFn readOutput)
{
    if (g_installed.exchange(true, std::memory_order_acq_rel))
        return true;
    g_log = log;
    g_readOutput = readOutput;
    g_immediate = immediate;
    g_deferredProbe = deferred;
    g_device = device;
    if (g_device)
        g_device->AddRef();
    if (!immediate)
    {
        LogF("[D3DHOOK] no immediate context - nothing installed");
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogF("[D3DHOOK] MH_Initialize failed: %s", MH_StatusToString(init));
        return false;
    }
    ID3D11DeviceContext* contexts[2] = {immediate, deferred};
    std::uint32_t created = 0;
    std::uint32_t failed = 0;
    void* immediateTargets[sizeof(kHooks) / sizeof(kHooks[0])]{};
    for (int variant = 0; variant < 2; ++variant)
    {
        auto* context = contexts[variant];
        if (!context)
            continue;
        auto** table = *reinterpret_cast<void***>(context);
        if (!table)
            continue;
        ID3D11DeviceContext1* context1{};
        const bool haveContext1=SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),reinterpret_cast<void**>(&context1)));
        if(context1)context1->Release();
        std::size_t index = 0;
        for (const auto& hook : kHooks)
        {
            const std::size_t at = index++;
            if(hook.slot>=115 && !haveContext1)continue;
            void* target = table[hook.slot];
            if (!target)
                continue;
            if (variant == 0)
                immediateTargets[at] = target;
            else if (target == immediateTargets[at])
                continue; // shared implementation, already hooked by variant 0
            char owner[160];
            ModuleNameOf(target, owner, sizeof(owner));
            const MH_STATUS status =
                MH_CreateHook(target, hook.detour[variant], hook.original[variant]);
            LogF("[D3DHOOK] %s vtable slot %d %s -> %s (%s)",
                variant == 0 ? "immediate" : "deferred ", hook.slot, hook.name, owner,
                MH_StatusToString(status));
            if (status == MH_OK)
                ++created;
            else
                ++failed;
        }
    }
    const MH_STATUS enable = MH_EnableHook(MH_ALL_HOOKS);
    NativeDrawPose::SetHooksReady(enable == MH_OK && failed == 0);
    if (enable == MH_OK) ObjectivePositions::Install(g_log);
    LogF("[D3DHOOK] installed: created=%u failed=%u enable=%s immediate=%p deferred=%p",
        created, failed, MH_StatusToString(enable), static_cast<void*>(immediate),
        static_cast<void*>(deferred));
    return enable == MH_OK && created > 0;
}

void OnPresent(IDXGISwapChain* swapChain)
{
    FullEye::OnPresent(swapChain,g_hudNativeStereo.load());
    FlashProbe::OnPresent();
    if (!g_installed.load(std::memory_order_acquire))
        return;
    const auto presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto total = g_ordinal.exchange(0, std::memory_order_acq_rel);
    g_lastFrameTotal.store(total, std::memory_order_relaxed);
    g_windowFrames.fetch_add(1, std::memory_order_relaxed);
    g_hudFrameCleared = false; // next frame's first interface draw clears
    UiWorld::g_present = static_cast<std::uint32_t>(presents);
    UiWorldProbe::Present(static_cast<unsigned>(presents));
    RemainingUiProbe::Present(static_cast<unsigned>(presents),g_log);
    g_hudDrawsThisFrame = 0;
    g_hudPassBulkThisFrame = false;
    if (g_immediate) UiWorld::g_markerAssets.Poll(g_immediate, static_cast<std::uint32_t>(presents), g_log);
    UiWorld::g_bytesThisPresent.store(0, std::memory_order_relaxed);
    if (ResearchDiagnostics::Enabled && (presents % 600) == 0)
        UiWorld::Report(g_log, static_cast<std::uint32_t>(presents));
    {
        const auto wire = g_wireSetsThisFrame.exchange(0, std::memory_order_acq_rel);
        if (wire)
        {
            g_wireFramesWindow.fetch_add(1, std::memory_order_relaxed);
            const auto logged = g_wireLogged.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logged <= 12 || (logged % 600) == 0)
                LogF("[WIRE] present=%llu wireframe rasterizer bound %u times (state=%p) "
                     "parity=%u",
                    static_cast<unsigned long long>(presents), wire,
                    reinterpret_cast<void*>(g_wireLastState.load(std::memory_order_relaxed)),
                    static_cast<unsigned>(presents & 1));
        }
        if ((presents % 300) == 0)
        {
            const auto frames = g_wireFramesWindow.exchange(0, std::memory_order_acq_rel);
            if (frames)
                LogF("[WIRE] window: %llu of 300 presents bound a wireframe rasterizer "
                     "(replaced with solid: %llu binds total, failed %llu)",
                    static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(g_wireReplaced.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_wireReplaceFailed.load(std::memory_order_relaxed)));
        }
    }

    if (swapChain)
    {
        ID3D11Texture2D* texture{};
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture))) && texture)
        {
            ID3D11Resource* resource{};
            if (SUCCEEDED(texture->QueryInterface(__uuidof(ID3D11Resource),
                    reinterpret_cast<void**>(&resource))) && resource)
            {
                const auto previous = g_backBufferResource.exchange(
                    reinterpret_cast<std::uintptr_t>(resource), std::memory_order_relaxed);
                if (previous != reinterpret_cast<std::uintptr_t>(resource))
                    ClearCaches(); // identity changed (first frame or resize)
                resource->Release();
            }
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            g_displayW.store(desc.Width, std::memory_order_relaxed);
            g_displayH.store(desc.Height, std::memory_order_relaxed);
            g_displayFmt.store(static_cast<std::uint32_t>(desc.Format), std::memory_order_relaxed);
            texture->Release();
        }
    }
    if ((presents % 600) == 0)
        ClearCaches();

    // Available in release builds: prove that SDR sprites reach the layer
    // without enabling the expensive research census.
    if (presents == 120 || (presents % 600) == 0)
        LogF("[HUDROUTE] displayFmt=%u layerFmt=%u redirectedTotal=%llu formatRejectedTotal=%llu consumer=%u",
            g_displayFmt.load(), g_hudFmt,
            static_cast<unsigned long long>(g_hudRedirectTotal.load()),
            static_cast<unsigned long long>(g_hudFmtPass.load()),
            g_hudConsumer.load() ? 1u : 0u);

    if (ResearchDiagnostics::Enabled && (presents % 300) == 0)
    {
        const auto frames = std::max<std::uint64_t>(
            g_windowFrames.exchange(0, std::memory_order_acq_rel), 1);
        std::uint64_t lanes[LaneCount]{};
        std::uint64_t draws = 0;
        for (std::uint32_t lane = 0; lane < LaneCount; ++lane)
        {
            lanes[lane] = g_laneSum[lane].exchange(0, std::memory_order_acq_rel) / frames;
            draws += lanes[lane];
        }
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const auto ticks = g_hookTicks.exchange(0, std::memory_order_acq_rel) / frames;
        const auto hookUs = frequency.QuadPart > 0
            ? (ticks * 1000000ull) / static_cast<std::uint64_t>(frequency.QuadPart) : 0ull;
        LogF("[D3DHOOK] per frame: draws=%llu Draw=%llu DrawIndexed=%llu DrawIdxInst=%llu "
             "DrawInst=%llu DrawAuto=%llu IdxIndirect=%llu Indirect=%llu this(imm=%llu "
             "def=%llu other=%llu) cmdlist(finish=%llu exec=%llu) backbuffer=%llu "
             "ldrBlendFull=%llu fitted=%llu hud=%llu hudL=%llu hudR=%llu dropped=%llu "
             "hudPassClears/window=%llu untex=%llu blendPass=%llu fmtPass=%llu mod=%llu noShader=%llu hookUs=%llu "
             "display=%ux%u control=%s hudLayer=%s consumer=%d",
            static_cast<unsigned long long>(draws),
            static_cast<unsigned long long>(lanes[LaneDraw]),
            static_cast<unsigned long long>(lanes[LaneDrawIndexed]),
            static_cast<unsigned long long>(lanes[LaneDrawIndexedInstanced]),
            static_cast<unsigned long long>(lanes[LaneDrawInstanced]),
            static_cast<unsigned long long>(lanes[LaneDrawAuto]),
            static_cast<unsigned long long>(lanes[LaneIndexedIndirect]),
            static_cast<unsigned long long>(lanes[LaneIndirect]),
            static_cast<unsigned long long>(
                g_thisImmediate.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_thisDeferredProbe.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_thisOther.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_finish.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_execute.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_backbufferDraws.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_candidateDraws.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_fittedWindow.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudRedirects.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudLeft.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudRight.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudDropped.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudPassClears.exchange(0, std::memory_order_acq_rel)),
            static_cast<unsigned long long>(
                g_hudUntextured.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudBlendPass.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_hudFmtPass.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_modDraws.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(
                g_noShader.exchange(0, std::memory_order_acq_rel) / frames),
            static_cast<unsigned long long>(hookUs),
            g_displayW.load(std::memory_order_relaxed),
            g_displayH.load(std::memory_order_relaxed),
            g_fitEnabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_hudLayerEnabled.load(std::memory_order_relaxed) ? "ON" : "off",
            g_hudConsumer.load(std::memory_order_relaxed) ? 1 : 0);
    }
    if (ResearchDiagnostics::Enabled && (presents % 900) == 0 &&
        g_dumps.load(std::memory_order_relaxed) < 40)
    {
        g_dumps.fetch_add(1, std::memory_order_relaxed);
        DumpShaders();
    }
}

void SetNativeStereo(bool active)
{
    g_hudNativeStereo.store(active, std::memory_order_release);
}

void SetWorldIcons(bool active)
{
    g_hudWorldIcons.store(active, std::memory_order_release);
}

void SetModRender(bool on)
{
    t_modRender += on ? 1 : -1;
    if (t_modRender < 0)
        t_modRender = 0;
}

void SetFitEnabled(bool enabled)
{
    g_fitEnabled.store(enabled, std::memory_order_release);
}

bool GetFitEnabled()
{
    return g_fitEnabled.load(std::memory_order_acquire);
}

void SetFit(float scaleX, float scaleY, float offsetX, float offsetY)
{
    g_fitScaleX.store(std::clamp(scaleX, 0.30f, 1.20f), std::memory_order_release);
    g_fitScaleY.store(std::clamp(scaleY, 0.30f, 1.20f), std::memory_order_release);
    g_fitOffsetX.store(std::clamp(offsetX, -0.35f, 0.35f), std::memory_order_release);
    g_fitOffsetY.store(std::clamp(offsetY, -0.35f, 0.35f), std::memory_order_release);
}

unsigned long long GetFitDrawCount()
{
    return g_fitDraws.load(std::memory_order_relaxed);
}

void SetHudLayerEnabled(bool enabled)
{
    g_hudLayerEnabled.store(enabled, std::memory_order_release);
}

bool GetHudLayerEnabled()
{
    return g_hudLayerEnabled.load(std::memory_order_acquire);
}

void SetTextureBias(float bias)
{
    g_textureBias.store(std::clamp(bias, -2.0f, 0.0f), std::memory_order_relaxed);
}
float GetTextureBias() { return g_textureBias.load(std::memory_order_relaxed); }

void SetHudLayerConsumer(bool active)
{
    g_hudConsumer.store(active, std::memory_order_release);
}

// [MENUHUD] the Insert menu draws itself onto the interface layer too, so it
// is readable in the headset in native stereo (the backbuffer copy alone ends
// up split across the seam or replaced by the full-eye images).
ID3D11RenderTargetView* HudOverlayTarget()
{
    return g_hudConsumer.load(std::memory_order_acquire) && g_hudTexture ? g_hudRtv : nullptr;
}
ID3D11Texture2D* HudOverlayTexture() { return g_hudTexture; }
void MarkHudContent() { g_hudFrameContent.store(true, std::memory_order_relaxed); }

ID3D11Texture2D* TakeHudFrame(bool* hasContent)
{
    const bool content = g_hudFrameContent.exchange(false, std::memory_order_acq_rel);
    if (hasContent)
        *hasContent = content;
    return g_hudTexture;
}

ID3D11Texture2D* TakeHudWorldFrame(bool* hasContent)
{
    const bool content = g_hudWorldFrameContent.exchange(false, std::memory_order_acq_rel);
    if (hasContent)
        *hasContent = content;
    return g_hudWorldTexture;
}

void SetCameraAxes(const float* baseCol2, const float* headCol2)
{
    for (int i = 0; i < 3; ++i)
    {
        UiWorld::g_baseCol2[i].store(baseCol2[i], std::memory_order_relaxed);
        UiWorld::g_headCol2[i].store(headCol2[i], std::memory_order_relaxed);
    }
}

bool GetUiLens(float* p00, float* p11)
{
    const float x = UiWorld::g_uiLensP00.load(std::memory_order_relaxed);
    const float y = UiWorld::g_uiLensP11.load(std::memory_order_relaxed);
    if (x <= 0.0f || y <= 0.0f)
        return false;
    *p00 = x;
    *p11 = y;
    return true;
}

ID3D11Texture2D* TakeObjectiveFrame(ObjectiveWorld::Frame* frame,unsigned slot)
{
    if(slot>=ObjectiveWorld::MaxMarkers){if(frame)*frame={};return nullptr;}
    const bool content=g_objectiveSlotContent[slot].exchange(false);
    if(frame)*frame=content?g_objectiveFrames[slot]:ObjectiveWorld::Frame{};
    bool any=false;for(const auto& c:g_objectiveSlotContent)any|=c.load();g_objectiveContent=any;
    return g_objectiveTextures[slot];
}

ID3D11Texture2D* TakeInteractionFrame(ObjectiveWorld::Frame* frame) {
    auto f=g_interactionFrame;
    if(!g_interactionContent.exchange(false) || f.draws<6)f.draws=0;
    if(frame)*frame=f;return g_interactionTexture;
}
ID3D11Texture2D* TakeTargetFrame(ObjectiveWorld::Frame* frame) {
    ObjectiveWorld::Frame f{};float x{},y{};
    const bool content=g_targetContent.exchange(false,std::memory_order_acq_rel);
    if(content && g_targetGroup.Centre(x,y) && ObjectivePositions::Find(x,y,f.anchor,.02f)) {
        f.draws=g_targetGroup.count;f.texelsPerCanvasPixel=g_targetGroup.captureScale;
        f.canvasDiameter=std::hypot(g_targetGroup.roots[0][0]-g_targetGroup.roots[2][0],g_targetGroup.roots[0][1]-g_targetGroup.roots[2][1]);
        f.captureOffset[0]=g_targetGroup.capture[0]-x;
        f.captureOffset[1]=g_targetGroup.capture[1]-y;
    }
    static unsigned reports=0;
    if(content && (++reports%300)==1)LogF("[TARGET3D] grouped=%u matched=%u centre=%.3f/%.3f id=%llX",g_targetGroup.count,f.draws?1:0,x,y,f.anchor.id);
    if(frame)*frame=f;return g_targetTexture;
}

void SetHudWorldRouting(bool enabled)
{
    g_hudWorldRouting.store(enabled, std::memory_order_release);
}

unsigned long long GetHudRedirectCount()
{
    return g_hudRedirectTotal.load(std::memory_order_relaxed);
}
} // namespace UiHook
