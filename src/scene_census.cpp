#include "research_diagnostics.h"
#include "scene_census.h"
#include "gbuffer_census.h"
#include "lightning_geometry_fix.h"
#include "native_draw_pose.h"

#include "aer_control.h"

#include <windows.h>
#include <d3d11_1.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace SceneCensus
{
namespace
{
constexpr std::uint32_t kCaptureFrames = 240;
constexpr std::size_t kShaderSlots = 32768;
constexpr std::size_t kContextSlots = 128;
constexpr std::size_t kSignatureSlots = 65536;

enum class SignatureKind : std::uint8_t
{
    Draw,
    Dispatch,
};

struct ShaderRecord
{
    std::atomic_uintptr_t object{};
    std::atomic_uint64_t hash{};
};

struct ContextState
{
    std::atomic_uintptr_t context{};
    std::atomic_uintptr_t vs{};
    std::atomic_uintptr_t ps{};
    std::atomic_uintptr_t cs{};
    std::atomic_uint64_t psSrvMask{};
    std::atomic_uint64_t vsCbMask{};
    std::atomic_uint64_t psCbMask{};
    std::atomic_uint64_t csCbMask{};
    std::atomic_uint32_t rtvMask{};
    std::atomic_bool dsvBound{};
};

struct SignatureRecord
{
    std::atomic_uint64_t key{};
    std::atomic_uintptr_t first{};
    std::atomic_uintptr_t second{};
    std::atomic_uint32_t kind{};
    std::array<std::atomic_uint64_t, 3> calls{};
    std::array<std::atomic_uint64_t, 3> work{};
    std::array<std::atomic_uint64_t, 3> psSrvOr{};
    std::array<std::atomic_uint64_t, 3> vsCbOr{};
    std::array<std::atomic_uint64_t, 3> psCbOr{};
    std::array<std::atomic_uint64_t, 3> csCbOr{};
    std::array<std::atomic_uint32_t, 3> rtvOr{};
    std::array<std::atomic_uint32_t, 3> dsvCalls{};
    std::atomic_uint64_t monoReference{};
};

using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateComputeShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11ComputeShader**);
using VSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
    ID3D11ClassInstance* const*, UINT);
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
    ID3D11ClassInstance* const*, UINT);
using CSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*,
    ID3D11ClassInstance* const*, UINT);
using SetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
    ID3D11Buffer* const*);
using PSSetShaderResourcesFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
    ID3D11ShaderResourceView* const*);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT,
    INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using OMSetRenderTargetsAndUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*, const UINT*);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispatchIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);

struct ContextOriginals
{
    std::atomic<void**> table{};
    VSSetShaderFn vsSetShader{};
    PSSetShaderFn psSetShader{};
    CSSetShaderFn csSetShader{};
    SetConstantBuffersFn vsSetCb{};
    SetConstantBuffersFn psSetCb{};
    SetConstantBuffersFn csSetCb{};
    PSSetShaderResourcesFn psSetSrv{};
    OMSetRenderTargetsFn omSetRt{};
    OMSetRenderTargetsAndUavsFn omSetRtUav{};
    DrawIndexedFn drawIndexed{};
    DrawFn draw{};
    DrawIndexedInstancedFn drawIndexedInstanced{};
    DrawInstancedFn drawInstanced{};
    DispatchFn dispatch{};
    DispatchIndirectFn dispatchIndirect{};
};

// [PASSDRAW] The interface is drawn AFTER the engine's last announced pass -
// there is no UI pass in the registrar's names, which end at AfterPostEffect.
// So attribute every draw to the pass last announced and watch where the
// interface piles up.  Depth binding cannot separate them here: the HUD
// leaves the depth buffer bound, so it reads as world geometry, while the
// deferred and post passes unbind it.
constexpr std::size_t kPassBuckets = 48;

struct PassBucket
{
    char name[56]{};
    std::atomic_uint64_t draws{};
    std::atomic_uint64_t depthBound{};
    std::atomic_uint32_t vpWidth{};
    std::atomic_uint32_t vpHeight{};
    std::atomic_uint32_t claimed{};
    std::atomic_uint32_t sampled{};
};

std::array<PassBucket, kPassBuckets> g_passBuckets{};
std::atomic_uint32_t g_passBucketCount{};
std::atomic_int g_currentPass{-1};
std::atomic_uint64_t g_passDrawFrames{};
// Arrivals before failures: count every entry to the draw hook and every
// early exit separately, so "no output" cannot mean four different things.
std::atomic_uint64_t g_passDrawCalls{};
std::atomic_uint64_t g_passDrawNoPass{};
std::atomic_uint64_t g_passSetCalls{};

std::atomic_uint64_t g_omSetRt{};
std::atomic_uint64_t g_omSetRtUav{};

std::array<ShaderRecord, kShaderSlots> g_shaders{};
std::array<ContextState, kContextSlots> g_contexts{};
std::array<SignatureRecord, kSignatureSlots> g_signatures{};
// Eight was enough for the contexts handed over at startup.  The game draws
// on contexts created later, each with its own private vtable, so this now has
// to hold every vtable it is shown.
std::array<ContextOriginals, 64> g_originals{};
std::atomic_uint64_t g_lateHooked{};
std::atomic_uint64_t g_lateRefused{};
SRWLOCK g_installLock = SRWLOCK_INIT;
LogFn g_log{};
ReadOutputFn g_readOutput{};
CreateVertexShaderFn g_createVs{};
CreatePixelShaderFn g_createPs{};
CreateComputeShaderFn g_createCs{};
std::atomic_bool g_installed{};
std::atomic_bool g_capture{};
std::atomic_uint32_t g_framesLeft{};
std::atomic_uint32_t g_captureId{};
std::atomic_bool g_captureStereo{};
std::atomic_bool g_f10Down{};
std::atomic_uint64_t g_uncataloguedShaders{};
std::atomic_uint64_t g_droppedSignatures{};

std::uint64_t HashBytes(const void* data, std::size_t size)
{
    constexpr std::uint64_t offset = 1469598103934665603ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    auto hash = offset;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (!bytes || size == 0)
        return 0;
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * prime;
    return hash;
}

std::size_t PointerSlot(std::uintptr_t value, std::size_t mask)
{
    return ((value >> 4) ^ (value >> 19) ^ (value >> 31)) & mask;
}

void RememberShader(IUnknown* shader, const void* bytecode, std::size_t length)
{
    if (!shader || !bytecode || length == 0)
        return;
    LightningGeometryFix::RememberShader(shader,bytecode,length);
    if constexpr (!ResearchDiagnostics::Enabled) return;
    GBufferCensus::RememberShader(shader, bytecode, length);
    const auto object = reinterpret_cast<std::uintptr_t>(shader);
    const auto hash = HashBytes(bytecode, length);
    const auto start = PointerSlot(object, kShaderSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe)
    {
        auto& slot = g_shaders[(start + probe) & (kShaderSlots - 1)];
        auto existing = slot.object.load(std::memory_order_acquire);
        if (existing == object || (existing == 0 && slot.object.compare_exchange_strong(existing,
                object, std::memory_order_acq_rel, std::memory_order_acquire)))
        {
            slot.hash.store(hash, std::memory_order_release);
            return;
        }
    }
    g_uncataloguedShaders.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ShaderHash(std::uintptr_t object)
{
    if (!object)
        return 0;
    const auto start = PointerSlot(object, kShaderSlots - 1);
    for (std::size_t probe = 0; probe < 64; ++probe)
    {
        const auto& slot = g_shaders[(start + probe) & (kShaderSlots - 1)];
        const auto existing = slot.object.load(std::memory_order_acquire);
        if (existing == object)
            return slot.hash.load(std::memory_order_acquire);
        if (existing == 0)
            return 0;
    }
    return 0;
}

ContextState* StateFor(ID3D11DeviceContext* context)
{
    const auto key = reinterpret_cast<std::uintptr_t>(context);
    const auto start = PointerSlot(key, kContextSlots - 1);
    for (std::size_t probe = 0; probe < 32; ++probe)
    {
        auto& slot = g_contexts[(start + probe) & (kContextSlots - 1)];
        auto existing = slot.context.load(std::memory_order_acquire);
        if (existing == key || (existing == 0 && slot.context.compare_exchange_strong(existing,
                key, std::memory_order_acq_rel, std::memory_order_acquire)))
            return &slot;
    }
    return nullptr;
}

ContextOriginals* OriginalsFor(ID3D11DeviceContext* context)
{
    auto** table = *reinterpret_cast<void***>(context);
    for (auto& originals : g_originals)
        if (originals.table.load(std::memory_order_acquire) == table)
            return &originals;
    return nullptr;
}

bool Patch(void** table, std::size_t index, void* replacement, void** original)
{
    if (!table || !replacement || !original)
        return false;
    auto** slot = table + index;
    DWORD oldProtect{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    if (*slot != replacement)
    {
        if (!*original)
            *original = *slot;
        InterlockedExchangePointer(slot, replacement);
    }
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

std::uint64_t UpdateMask(std::uint64_t previous, UINT start, UINT count,
    IUnknown* const* objects)
{
    for (UINT i = 0; i < count && start + i < 64; ++i)
    {
        const auto bit = 1ull << (start + i);
        if (objects && objects[i])
            previous |= bit;
        else
            previous &= ~bit;
    }
    return previous;
}

int Bucket()
{
    if (!g_captureStereo.load(std::memory_order_acquire))
        return 0;
    const int output = g_readOutput ? g_readOutput() : 0;
    return output == 1 ? 2 : 1;
}

std::uint64_t SignatureKey(SignatureKind kind, std::uintptr_t first, std::uintptr_t second)
{
    std::uint64_t key = 1469598103934665603ull;
    key = (key ^ static_cast<std::uint64_t>(kind)) * 1099511628211ull;
    key = (key ^ first) * 1099511628211ull;
    key = (key ^ second) * 1099511628211ull;
    return key ? key : 1;
}

void Record(SignatureKind kind, ContextState* state, std::uint64_t work)
{
    if (!state || !g_capture.load(std::memory_order_acquire))
        return;
    const auto first = kind == SignatureKind::Draw
        ? state->vs.load(std::memory_order_relaxed)
        : state->cs.load(std::memory_order_relaxed);
    const auto second = kind == SignatureKind::Draw
        ? state->ps.load(std::memory_order_relaxed) : 0;
    const auto key = SignatureKey(kind, first, second);
    const auto start = static_cast<std::size_t>(key) & (kSignatureSlots - 1);
    for (std::size_t probe = 0; probe < 128; ++probe)
    {
        auto& slot = g_signatures[(start + probe) & (kSignatureSlots - 1)];
        auto existing = slot.key.load(std::memory_order_acquire);
        if (existing != key && (existing != 0 || !slot.key.compare_exchange_strong(existing, key,
                std::memory_order_acq_rel, std::memory_order_acquire)))
            continue;
        if (existing == 0)
        {
            slot.first.store(first, std::memory_order_relaxed);
            slot.second.store(second, std::memory_order_relaxed);
            slot.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_release);
        }
        else if (slot.first.load(std::memory_order_acquire) != first ||
            slot.second.load(std::memory_order_acquire) != second ||
            slot.kind.load(std::memory_order_acquire) != static_cast<std::uint32_t>(kind))
            continue;
        const int bucket = Bucket();
        slot.calls[bucket].fetch_add(1, std::memory_order_relaxed);
        slot.work[bucket].fetch_add(work, std::memory_order_relaxed);
        slot.psSrvOr[bucket].fetch_or(state->psSrvMask.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        slot.vsCbOr[bucket].fetch_or(state->vsCbMask.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        slot.psCbOr[bucket].fetch_or(state->psCbMask.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        slot.csCbOr[bucket].fetch_or(state->csCbMask.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        slot.rtvOr[bucket].fetch_or(state->rtvMask.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        if (state->dsvBound.load(std::memory_order_relaxed))
            slot.dsvCalls[bucket].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_droppedSignatures.fetch_add(1, std::memory_order_relaxed);
}

using CreateLayoutFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,ID3D11InputLayout**);
CreateLayoutFn g_createLayout{};
HRESULT STDMETHODCALLTYPE HookCreateLayout(ID3D11Device* device,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count,const void* bytes,SIZE_T length,ID3D11InputLayout** layout){
    HRESULT hr=g_createLayout?g_createLayout(device,elements,count,bytes,length,layout):E_FAIL;
    if(SUCCEEDED(hr) && layout && *layout){LightningGeometryFix::RememberLayout(*layout,elements,count);GBufferCensus::RememberLayout(*layout,elements,count);}
    return hr;
}
HRESULT STDMETHODCALLTYPE HookCreateVs(ID3D11Device* device, const void* bytecode, SIZE_T length,
    ID3D11ClassLinkage* linkage, ID3D11VertexShader** shader)
{
    const HRESULT hr = g_createVs ? g_createVs(device, bytecode, length, linkage, shader) : E_FAIL;
    if (SUCCEEDED(hr) && shader)
    {
        RememberShader(*shader, bytecode, length);
        NativeDrawPose::RememberVertexShader(*shader, bytecode, length);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreatePs(ID3D11Device* device, const void* bytecode, SIZE_T length,
    ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader)
{
    const HRESULT hr = g_createPs ? g_createPs(device, bytecode, length, linkage, shader) : E_FAIL;
    if (SUCCEEDED(hr) && shader)
        RememberShader(*shader, bytecode, length);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCs(ID3D11Device* device, const void* bytecode, SIZE_T length,
    ID3D11ClassLinkage* linkage, ID3D11ComputeShader** shader)
{
    const HRESULT hr = g_createCs ? g_createCs(device, bytecode, length, linkage, shader) : E_FAIL;
    if (SUCCEEDED(hr) && shader)
        RememberShader(*shader, bytecode, length);
    return hr;
}

void STDMETHODCALLTYPE HookVsSetShader(ID3D11DeviceContext* context, ID3D11VertexShader* shader,
    ID3D11ClassInstance* const* classes, UINT count)
{
    if (auto* state = StateFor(context))
        state->vs.store(reinterpret_cast<std::uintptr_t>(shader), std::memory_order_relaxed);
    if (auto* original = OriginalsFor(context); original && original->vsSetShader)
        original->vsSetShader(context, shader, classes, count);
}

void STDMETHODCALLTYPE HookPsSetShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
    ID3D11ClassInstance* const* classes, UINT count)
{
    if (auto* state = StateFor(context))
        state->ps.store(reinterpret_cast<std::uintptr_t>(shader), std::memory_order_relaxed);
    if (auto* original = OriginalsFor(context); original && original->psSetShader)
        original->psSetShader(context, shader, classes, count);
}

void STDMETHODCALLTYPE HookCsSetShader(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
    ID3D11ClassInstance* const* classes, UINT count)
{
    if (auto* state = StateFor(context))
        state->cs.store(reinterpret_cast<std::uintptr_t>(shader), std::memory_order_relaxed);
    if (auto* original = OriginalsFor(context); original && original->csSetShader)
        original->csSetShader(context, shader, classes, count);
}

void SetCbMask(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11Buffer* const* buffers,
    std::atomic_uint64_t ContextState::* member)
{
    if (auto* state = StateFor(context))
    {
        const auto oldMask = (state->*member).load(std::memory_order_relaxed);
        (state->*member).store(UpdateMask(oldMask, start, count,
            reinterpret_cast<IUnknown* const*>(buffers)), std::memory_order_relaxed);
    }
}

void STDMETHODCALLTYPE HookVsSetCb(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11Buffer* const* buffers)
{
    SetCbMask(context, start, count, buffers, &ContextState::vsCbMask);
    if (auto* original = OriginalsFor(context); original && original->vsSetCb)
        original->vsSetCb(context, start, count, buffers);
}

void STDMETHODCALLTYPE HookPsSetCb(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11Buffer* const* buffers)
{
    SetCbMask(context, start, count, buffers, &ContextState::psCbMask);
    if (auto* original = OriginalsFor(context); original && original->psSetCb)
        original->psSetCb(context, start, count, buffers);
}

void STDMETHODCALLTYPE HookCsSetCb(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11Buffer* const* buffers)
{
    SetCbMask(context, start, count, buffers, &ContextState::csCbMask);
    if (auto* original = OriginalsFor(context); original && original->csSetCb)
        original->csSetCb(context, start, count, buffers);
}

void STDMETHODCALLTYPE HookPsSetSrv(ID3D11DeviceContext* context, UINT start, UINT count,
    ID3D11ShaderResourceView* const* views)
{
    if (auto* state = StateFor(context))
    {
        const auto oldMask = state->psSrvMask.load(std::memory_order_relaxed);
        state->psSrvMask.store(UpdateMask(oldMask, start, count,
            reinterpret_cast<IUnknown* const*>(views)), std::memory_order_relaxed);
    }
    if (auto* original = OriginalsFor(context); original && original->psSetSrv)
        original->psSetSrv(context, start, count, views);
}

void STDMETHODCALLTYPE HookOmSetRt(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv)
{
    if (auto* state = StateFor(context))
    {
        std::uint32_t mask{};
        for (UINT i = 0; i < count && i < 8; ++i)
            if (views && views[i])
                mask |= 1u << i;
        state->rtvMask.store(mask, std::memory_order_relaxed);
        state->dsvBound.store(dsv != nullptr, std::memory_order_relaxed);
    }
    g_omSetRt.fetch_add(1, std::memory_order_relaxed);
    if (auto* original = OriginalsFor(context); original && original->omSetRt)
        original->omSetRt(context, count, views, dsv);
}

// [UIFIT] Pull the interface into the part of the headset you can actually
// look at.  The game draws its HUD to the edges of a flat 16:9 screen, and in
// a headset those edges sit past the comfortable field of view.
//
// This belongs HERE, not in main.cpp.  These are the draw hooks the game
// actually dispatches through - per-context originals, which is the only
// design that survives this engine's per-object vtables.  Re-pointing those
// slots at a second hook set would cut THESE hooks out of the chain and
// crash on load.
//
// The classification costs nothing: this census already tracks, per context,
// whether a depth-stencil is bound.  World geometry always carries depth;
// screen-space work never does.  So a draw with no DSV, into a render target,
// is interface or post-process - and shrinking the raster viewport for it
// pulls screen-space content toward the middle without touching a shader.
std::atomic_uint64_t g_uiFitDraws{};

bool BeginUiViewport(ID3D11DeviceContext* context, ContextState* state,
    D3D11_VIEWPORT* saved, UINT* savedCount)
{
    // [D3DHOOK] The interface fit moved to ui_hook.cpp, on the d3d11.dll
    // implementations every draw passes through.  Fitting here as well would
    // shrink a draw twice, so this path is retired.
    if (state || !state)
        return false;
    if (!state || state->dsvBound.load(std::memory_order_relaxed))
        return false;
    if (!AerControl::GetUiFitEnabled())
        return false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    AerControl::GetUiFit(&scaleX, &scaleY, &offsetX, &offsetY);
    if (scaleX > 0.999f && scaleX < 1.001f && scaleY > 0.999f && scaleY < 1.001f &&
        offsetX > -0.001f && offsetX < 0.001f && offsetY > -0.001f && offsetY < 0.001f)
        return false;
    *savedCount = 1;
    context->RSGetViewports(savedCount, saved);
    if (*savedCount == 0 || saved[0].Width <= 0.0f || saved[0].Height <= 0.0f)
        return false;
    D3D11_VIEWPORT fitted = saved[0];
    fitted.Width = saved[0].Width * scaleX;
    fitted.Height = saved[0].Height * scaleY;
    fitted.TopLeftX = saved[0].TopLeftX +
        (saved[0].Width - fitted.Width) * 0.5f + offsetX * saved[0].Width;
    fitted.TopLeftY = saved[0].TopLeftY +
        (saved[0].Height - fitted.Height) * 0.5f + offsetY * saved[0].Height;
    context->RSSetViewports(1, &fitted);
    const auto count = g_uiFitDraws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_log && (count == 1 || (count % 20000) == 0))
        g_log("[UIFIT] fitted draw %llu  scale=%.2f/%.2f vp %.0fx%.0f -> %.0fx%.0f "
            "(omSetRt=%llu omSetRtUav=%llu)",
            static_cast<unsigned long long>(count), scaleX, scaleY,
            saved[0].Width, saved[0].Height, fitted.Width, fitted.Height,
            static_cast<unsigned long long>(g_omSetRt.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_omSetRtUav.load(std::memory_order_relaxed)));
    return true;
}

void STDMETHODCALLTYPE HookOmSetRtUav(ID3D11DeviceContext* context, UINT rtCount,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv,
    UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs,
    const UINT* initialCounts)
{
    g_omSetRtUav.fetch_add(1, std::memory_order_relaxed);
    if (rtCount != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
    {
        if (auto* state = StateFor(context))
        {
            std::uint32_t mask{};
            for (UINT i = 0; i < rtCount && i < 8; ++i)
                if (views && views[i])
                    mask |= 1u << i;
            state->rtvMask.store(mask, std::memory_order_relaxed);
            state->dsvBound.store(dsv != nullptr, std::memory_order_relaxed);
        }
    }
    if (auto* original = OriginalsFor(context); original && original->omSetRtUav)
        original->omSetRtUav(context, rtCount, views, dsv, uavStart, uavCount, uavs,
            initialCounts);
}

// Attribute one draw to the pass the engine last announced.  Costs an index
// load and two increments; the viewport is sampled once per bucket, ever.
void NotePassDraw(ID3D11DeviceContext* context, ContextState* state)
{
    g_passDrawCalls.fetch_add(1, std::memory_order_relaxed);
    const int index = g_currentPass.load(std::memory_order_relaxed);
    if (index < 0 || static_cast<std::size_t>(index) >= kPassBuckets)
    {
        g_passDrawNoPass.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& bucket = g_passBuckets[static_cast<std::size_t>(index)];
    bucket.draws.fetch_add(1, std::memory_order_relaxed);
    if (state && state->dsvBound.load(std::memory_order_relaxed))
        bucket.depthBound.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t sampled = 0;
    if (bucket.sampled.compare_exchange_strong(sampled, 1u,
            std::memory_order_acq_rel, std::memory_order_relaxed))
    {
        UINT count = 1;
        D3D11_VIEWPORT viewport{};
        context->RSGetViewports(&count, &viewport);
        if (count)
        {
            bucket.vpWidth.store(static_cast<std::uint32_t>(viewport.Width),
                std::memory_order_relaxed);
            bucket.vpHeight.store(static_cast<std::uint32_t>(viewport.Height),
                std::memory_order_relaxed);
        }
    }
}

void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* context, UINT indices, UINT start,
    INT base)
{
    auto* state = StateFor(context);
    Record(SignatureKind::Draw, state, indices);
    NotePassDraw(context, state);
    D3D11_VIEWPORT savedViewport[1]{};
    UINT savedCount = 0;
    const bool fitted = BeginUiViewport(context, state, savedViewport, &savedCount);
    if (auto* original = OriginalsFor(context); original && original->drawIndexed)
        original->drawIndexed(context, indices, start, base);
    if (fitted)
        context->RSSetViewports(savedCount, savedViewport);
}

void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* context, UINT vertices, UINT start)
{
    auto* state = StateFor(context);
    Record(SignatureKind::Draw, state, vertices);
    NotePassDraw(context, state);
    D3D11_VIEWPORT savedViewport[1]{};
    UINT savedCount = 0;
    const bool fitted = BeginUiViewport(context, state, savedViewport, &savedCount);
    if (auto* original = OriginalsFor(context); original && original->draw)
        original->draw(context, vertices, start);
    if (fitted)
        context->RSSetViewports(savedCount, savedViewport);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* context, UINT indices,
    UINT instances, UINT start, INT base, UINT firstInstance)
{
    Record(SignatureKind::Draw, StateFor(context), static_cast<std::uint64_t>(indices) * instances);
    if (auto* original = OriginalsFor(context); original && original->drawIndexedInstanced)
        original->drawIndexedInstanced(context, indices, instances, start, base, firstInstance);
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* context, UINT vertices,
    UINT instances, UINT start, UINT firstInstance)
{
    Record(SignatureKind::Draw, StateFor(context), static_cast<std::uint64_t>(vertices) * instances);
    if (auto* original = OriginalsFor(context); original && original->drawInstanced)
        original->drawInstanced(context, vertices, instances, start, firstInstance);
}

void STDMETHODCALLTYPE HookDispatch(ID3D11DeviceContext* context, UINT x, UINT y, UINT z)
{
    Record(SignatureKind::Dispatch, StateFor(context), static_cast<std::uint64_t>(x) * y * z);
    if (auto* original = OriginalsFor(context); original && original->dispatch)
        original->dispatch(context, x, y, z);
}

void STDMETHODCALLTYPE HookDispatchIndirect(ID3D11DeviceContext* context, ID3D11Buffer* args,
    UINT offset)
{
    Record(SignatureKind::Dispatch, StateFor(context), 0);
    if (auto* original = OriginalsFor(context); original && original->dispatchIndirect)
        original->dispatchIndirect(context, args, offset);
}

bool HookContext(ID3D11DeviceContext* context)
{
    if (!context)
        return false;
    auto** table = *reinterpret_cast<void***>(context);
    for (auto& existing : g_originals)
        if (existing.table.load(std::memory_order_acquire) == table)
            return true;
    ContextOriginals* selected{};
    for (auto& candidate : g_originals)
        if (!candidate.table.load(std::memory_order_acquire))
        {
            selected = &candidate;
            break;
        }
    if (!selected)
        return false;
    selected->table.store(table, std::memory_order_release);
    bool ok = true;
    ok &= Patch(table, 7, reinterpret_cast<void*>(&HookVsSetCb),
        reinterpret_cast<void**>(&selected->vsSetCb));
    ok &= Patch(table, 8, reinterpret_cast<void*>(&HookPsSetSrv),
        reinterpret_cast<void**>(&selected->psSetSrv));
    ok &= Patch(table, 9, reinterpret_cast<void*>(&HookPsSetShader),
        reinterpret_cast<void**>(&selected->psSetShader));
    ok &= Patch(table, 11, reinterpret_cast<void*>(&HookVsSetShader),
        reinterpret_cast<void**>(&selected->vsSetShader));
    ok &= Patch(table, 12, reinterpret_cast<void*>(&HookDrawIndexed),
        reinterpret_cast<void**>(&selected->drawIndexed));
    ok &= Patch(table, 13, reinterpret_cast<void*>(&HookDraw),
        reinterpret_cast<void**>(&selected->draw));
    ok &= Patch(table, 16, reinterpret_cast<void*>(&HookPsSetCb),
        reinterpret_cast<void**>(&selected->psSetCb));
    ok &= Patch(table, 20, reinterpret_cast<void*>(&HookDrawIndexedInstanced),
        reinterpret_cast<void**>(&selected->drawIndexedInstanced));
    ok &= Patch(table, 21, reinterpret_cast<void*>(&HookDrawInstanced),
        reinterpret_cast<void**>(&selected->drawInstanced));
    ok &= Patch(table, 33, reinterpret_cast<void*>(&HookOmSetRt),
        reinterpret_cast<void**>(&selected->omSetRt));
    ok &= Patch(table, 34, reinterpret_cast<void*>(&HookOmSetRtUav),
        reinterpret_cast<void**>(&selected->omSetRtUav));
    ok &= Patch(table, 41, reinterpret_cast<void*>(&HookDispatch),
        reinterpret_cast<void**>(&selected->dispatch));
    ok &= Patch(table, 42, reinterpret_cast<void*>(&HookDispatchIndirect),
        reinterpret_cast<void**>(&selected->dispatchIndirect));
    ok &= Patch(table, 69, reinterpret_cast<void*>(&HookCsSetShader),
        reinterpret_cast<void**>(&selected->csSetShader));
    ok &= Patch(table, 71, reinterpret_cast<void*>(&HookCsSetCb),
        reinterpret_cast<void**>(&selected->csSetCb));
    return ok;
}

void ResetWindow()
{
    for (auto& signature : g_signatures)
    {
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
        {
            signature.calls[bucket].store(0, std::memory_order_relaxed);
            signature.work[bucket].store(0, std::memory_order_relaxed);
            signature.psSrvOr[bucket].store(0, std::memory_order_relaxed);
            signature.vsCbOr[bucket].store(0, std::memory_order_relaxed);
            signature.psCbOr[bucket].store(0, std::memory_order_relaxed);
            signature.csCbOr[bucket].store(0, std::memory_order_relaxed);
            signature.rtvOr[bucket].store(0, std::memory_order_relaxed);
            signature.dsvCalls[bucket].store(0, std::memory_order_relaxed);
        }
    }
    g_droppedSignatures.store(0, std::memory_order_relaxed);
}

struct Ranked
{
    SignatureRecord* signature{};
    std::uint64_t score{};
};

void Rank(std::array<Ranked, 32>& top, SignatureRecord& signature, std::uint64_t score)
{
    if (score == 0)
        return;
    Ranked candidate{&signature, score};
    for (auto& ranked : top)
        if (!ranked.signature || candidate.score > ranked.score)
        {
            const auto displaced = ranked;
            ranked = candidate;
            candidate = displaced;
            if (!candidate.signature)
                break;
        }
}

void LogSignature(const char* label, std::size_t rank, const SignatureRecord& signature)
{
    const auto first = signature.first.load(std::memory_order_relaxed);
    const auto second = signature.second.load(std::memory_order_relaxed);
    const bool dispatch = signature.kind.load(std::memory_order_relaxed) ==
        static_cast<std::uint32_t>(SignatureKind::Dispatch);
    if (g_log)
        g_log("[SCENE] %s rank=%zu kind=%s ptr=%p/%p hash=%016llX/%016llX "
            "calls(mono/o0/o1)=%llu/%llu/%llu work=%llu/%llu/%llu "
            "srv=%016llX/%016llX/%016llX cbVS=%016llX/%016llX/%016llX "
            "cbPS=%016llX/%016llX/%016llX cbCS=%016llX/%016llX/%016llX "
            "rt=%02X/%02X/%02X dsv=%u/%u/%u",
            label, rank, dispatch ? "dispatch" : "draw",
            reinterpret_cast<void*>(first), reinterpret_cast<void*>(second),
            static_cast<unsigned long long>(ShaderHash(first)),
            static_cast<unsigned long long>(ShaderHash(second)),
            static_cast<unsigned long long>(signature.calls[0].load()),
            static_cast<unsigned long long>(signature.calls[1].load()),
            static_cast<unsigned long long>(signature.calls[2].load()),
            static_cast<unsigned long long>(signature.work[0].load()),
            static_cast<unsigned long long>(signature.work[1].load()),
            static_cast<unsigned long long>(signature.work[2].load()),
            static_cast<unsigned long long>(signature.psSrvOr[0].load()),
            static_cast<unsigned long long>(signature.psSrvOr[1].load()),
            static_cast<unsigned long long>(signature.psSrvOr[2].load()),
            static_cast<unsigned long long>(signature.vsCbOr[0].load()),
            static_cast<unsigned long long>(signature.vsCbOr[1].load()),
            static_cast<unsigned long long>(signature.vsCbOr[2].load()),
            static_cast<unsigned long long>(signature.psCbOr[0].load()),
            static_cast<unsigned long long>(signature.psCbOr[1].load()),
            static_cast<unsigned long long>(signature.psCbOr[2].load()),
            static_cast<unsigned long long>(signature.csCbOr[0].load()),
            static_cast<unsigned long long>(signature.csCbOr[1].load()),
            static_cast<unsigned long long>(signature.csCbOr[2].load()),
            signature.rtvOr[0].load(), signature.rtvOr[1].load(), signature.rtvOr[2].load(),
            signature.dsvCalls[0].load(), signature.dsvCalls[1].load(),
            signature.dsvCalls[2].load());
}

void FinishCapture()
{
    g_capture.store(false, std::memory_order_release);
    const bool stereo = g_captureStereo.load(std::memory_order_acquire);
    std::array<Ranked, 32> top{};
    std::uint64_t calls[3]{};
    std::uint32_t signatures[3]{};
    std::uint32_t missingEye0{};
    std::uint32_t missingEye1{};
    for (auto& signature : g_signatures)
    {
        if (!signature.key.load(std::memory_order_acquire))
            continue;
        const auto mono = signature.calls[0].load(std::memory_order_relaxed);
        const auto eye0 = signature.calls[1].load(std::memory_order_relaxed);
        const auto eye1 = signature.calls[2].load(std::memory_order_relaxed);
        for (std::size_t bucket = 0; bucket < 3; ++bucket)
        {
            calls[bucket] += signature.calls[bucket].load(std::memory_order_relaxed);
            if (signature.calls[bucket].load(std::memory_order_relaxed))
                ++signatures[bucket];
        }
        if (!stereo)
        {
            signature.monoReference.store(mono, std::memory_order_relaxed);
            Rank(top, signature, mono);
            continue;
        }
        const auto reference = signature.monoReference.load(std::memory_order_relaxed);
        if (!reference)
            continue;
        if (!eye0)
            ++missingEye0;
        if (!eye1)
            ++missingEye1;
        const auto deficit0 = reference > eye0 ? reference - eye0 : 0;
        const auto deficit1 = reference > eye1 ? reference - eye1 : 0;
        Rank(top, signature, deficit0 + deficit1);
    }
    if (g_log)
        g_log("[SCENE] capture=%u DONE stereo=%d frames=%u calls(mono/o0/o1)=%llu/%llu/%llu "
            "signatures=%u/%u/%u missingFromEye=%u/%u uncataloguedShaders=%llu dropped=%llu",
            g_captureId.load(std::memory_order_relaxed), stereo ? 1 : 0, kCaptureFrames,
            static_cast<unsigned long long>(calls[0]), static_cast<unsigned long long>(calls[1]),
            static_cast<unsigned long long>(calls[2]), signatures[0], signatures[1], signatures[2],
            missingEye0, missingEye1,
            static_cast<unsigned long long>(g_uncataloguedShaders.load()),
            static_cast<unsigned long long>(g_droppedSignatures.load()));
    for (std::size_t rank = 0; rank < top.size() && top[rank].signature; ++rank)
        LogSignature(stereo ? "DEFICIT" : "MONO-TOP", rank + 1, *top[rank].signature);
    Beep(stereo ? 1700 : 1100, 180);
}
} // namespace

void Install(ID3D11Device* device, ID3D11DeviceContext* immediate,
    ID3D11DeviceContext* deferred, LogFn log, ReadOutputFn readOutput)
{
    if constexpr (!ResearchDiagnostics::Enabled) {
        if (!device) return;
        const bool capture=GBufferCensus::CaptureEnabled();
        AcquireSRWLockExclusive(&g_installLock);
        if (!g_installed.load()) {
            auto** table=*reinterpret_cast<void***>(device);
            const bool layoutOk=Patch(table,11,reinterpret_cast<void*>(&HookCreateLayout),reinterpret_cast<void**>(&g_createLayout));
            const bool vsOk=Patch(table,12,reinterpret_cast<void*>(&HookCreateVs),reinterpret_cast<void**>(&g_createVs));
            const bool psOk=Patch(table,15,reinterpret_cast<void*>(&HookCreatePs),reinterpret_cast<void**>(&g_createPs));
            g_installed.store(true);
            if(log)log("[LIGHTNING] shader identification vs=%d ps=%d captureLayout=%d; full scene census remains off",vsOk,psOk,layoutOk);
        }
        ReleaseSRWLockExclusive(&g_installLock);
        return;
    }
    if (!device)
        return;
    AcquireSRWLockExclusive(&g_installLock);
    g_log = log;
    g_readOutput = readOutput;
    if (!g_installed.load(std::memory_order_acquire))
    {
        auto** table = *reinterpret_cast<void***>(device);
        const bool vsOk = Patch(table, 12, reinterpret_cast<void*>(&HookCreateVs),
            reinterpret_cast<void**>(&g_createVs));
        const bool psOk = Patch(table, 15, reinterpret_cast<void*>(&HookCreatePs),
            reinterpret_cast<void**>(&g_createPs));
        const bool csOk = Patch(table, 18, reinterpret_cast<void*>(&HookCreateCs),
            reinterpret_cast<void**>(&g_createCs));
        const bool immediateOk = HookContext(immediate);
        const bool deferredOk = HookContext(deferred);

        // FFXV submits the main scene through the D3D11.1 context interfaces. Those interfaces have
        // separate vtable addresses from the base contexts returned above. Patch both real immediate
        // interfaces and synthetic deferred-interface vtables so contexts created before Present are
        // covered as well.
        bool immediate1Ok = false;
        bool deferred1Ok = false;
        ID3D11Device1* device1{};
        if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1),
                reinterpret_cast<void**>(&device1))) && device1)
        {
            ID3D11DeviceContext1* immediate1{};
            device1->GetImmediateContext1(&immediate1);
            immediate1Ok = HookContext(reinterpret_cast<ID3D11DeviceContext*>(immediate1));
            if (immediate1)
                immediate1->Release();

            ID3D11DeviceContext1* deferred1{};
            if (SUCCEEDED(device1->CreateDeferredContext1(0, &deferred1)) && deferred1)
                deferred1Ok = HookContext(reinterpret_cast<ID3D11DeviceContext*>(deferred1));
            if (deferred1)
                deferred1->Release();
            device1->Release();
        }
        g_installed.store(vsOk && psOk && csOk && immediateOk && deferredOk &&
            immediate1Ok && deferred1Ok,
            std::memory_order_release);
        if (g_log)
            g_log("[SCENE] passive census installed device=%d immediate=%d deferred=%d immediate1=%d deferred1=%d; F10 starts 240-frame capture",
                vsOk && psOk && csOk ? 1 : 0, immediateOk ? 1 : 0, deferredOk ? 1 : 0,
                immediate1Ok ? 1 : 0, deferred1Ok ? 1 : 0);
    }
    ReleaseSRWLockExclusive(&g_installLock);
}

void BeginCapture(bool stereo)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!g_installed.load(std::memory_order_acquire))
    {
        if (g_log)
            g_log("[SCENE] capture rejected: hooks are not installed");
        Beep(300, 180);
        return;
    }
    g_capture.store(false, std::memory_order_release);
    ResetWindow();
    g_captureStereo.store(stereo, std::memory_order_release);
    g_framesLeft.store(kCaptureFrames, std::memory_order_release);
    const auto id = g_captureId.fetch_add(1, std::memory_order_relaxed) + 1;
    g_capture.store(true, std::memory_order_release);
    if (g_log)
        g_log("[SCENE] capture=%u START stereo=%d frames=%u", id, stereo ? 1 : 0,
            kCaptureFrames);
    Beep(900, 100);
}

void OnPresent(bool stereo)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    const bool wasDown = g_f10Down.exchange(f10, std::memory_order_acq_rel);
    if (f10 && !wasDown)
        BeginCapture(stereo);
    if (!g_capture.load(std::memory_order_acquire))
        return;
    const auto previous = g_framesLeft.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 1)
        FinishCapture();
}

bool IsCapturing()
{
    return g_capture.load(std::memory_order_acquire);
}

unsigned long long GetUiFitDrawCount()
{
    return g_uiFitDraws.load(std::memory_order_relaxed);
}

// [LATEHOOK] The census hooks were installed on the four contexts handed over
// at startup, and the [PASSDRAW] counters proved the game does not draw on any
// of them: 14000 pass announcements a frame, and drawHookCalls=0.  This engine
// gives each context its OWN vtable, so a context created later is invisible
// no matter how correctly the early ones were patched.
//
// HookContext does this: keyed by vtable, idempotent, originals stored PER
// VTABLE, writes through VirtualProtect.
// A hand-rolled version with a single global original would cut these hooks
// out of the chain on contexts where they are already installed.
bool EnsureContextHooked(ID3D11DeviceContext* context)
{
    if constexpr (!ResearchDiagnostics::Enabled) return false;
    if (!context)
        return false;
    const bool ok = HookContext(context);
    if (ok)
        g_lateHooked.fetch_add(1, std::memory_order_relaxed);
    else
        g_lateRefused.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void SetCurrentPass(const char* name)
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    if (!name || !*name)
        return;
    g_passSetCalls.fetch_add(1, std::memory_order_relaxed);
    const auto claimed = std::min<std::uint32_t>(
        g_passBucketCount.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(kPassBuckets));
    for (std::uint32_t i = 0; i < claimed; ++i)
        if (std::strncmp(g_passBuckets[i].name, name,
                sizeof(g_passBuckets[i].name) - 1) == 0)
        {
            g_currentPass.store(static_cast<int>(i), std::memory_order_relaxed);
            return;
        }
    const auto slot = g_passBucketCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= kPassBuckets)
        return;
    auto& bucket = g_passBuckets[slot];
    std::strncpy(bucket.name, name, sizeof(bucket.name) - 1);
    bucket.claimed.store(1, std::memory_order_release);
    g_currentPass.store(static_cast<int>(slot), std::memory_order_relaxed);
}

// Once per present: report where the frame's draws actually landed.  The pass
// holding a few dozen draws at the full display viewport, after every world
// pass, is the interface.
void OnPassDrawFrame()
{
    if constexpr (!ResearchDiagnostics::Enabled) return;
    const auto frame = g_passDrawFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((frame % 300) != 0 || !g_log)
        return;
    const auto claimed = std::min<std::uint32_t>(
        g_passBucketCount.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(kPassBuckets));
    // Always emit this line, even when every bucket is empty.  Otherwise the
    // silence of skipped per-bucket lines could mean the
    // hook was not running, the pass name was never fed, or the buckets were
    // never claimed - three different bugs with one symptom.
    g_log("[PASSDRAW] frame=%llu buckets=%u currentPass=%d drawHookCalls=%llu "
        "withoutPass=%llu setPassCalls=%llu ctxHooked=%llu ctxRefused=%llu",
        static_cast<unsigned long long>(frame), claimed,
        g_currentPass.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            g_passDrawCalls.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_passDrawNoPass.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_passSetCalls.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_lateHooked.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_lateRefused.load(std::memory_order_relaxed)));
    for (std::uint32_t i = 0; i < claimed; ++i)
    {
        auto& bucket = g_passBuckets[i];
        const auto draws = bucket.draws.exchange(0, std::memory_order_relaxed);
        const auto depth = bucket.depthBound.exchange(0, std::memory_order_relaxed);
        if (!draws)
            continue;
        g_log("[PASSDRAW] %-34s draws=%llu depthBound=%llu vp=%ux%u", bucket.name,
            static_cast<unsigned long long>(draws),
            static_cast<unsigned long long>(depth),
            bucket.vpWidth.load(std::memory_order_relaxed),
            bucket.vpHeight.load(std::memory_order_relaxed));
    }
}
} // namespace SceneCensus
