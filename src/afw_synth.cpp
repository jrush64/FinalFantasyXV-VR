#include "afw_synth.h"
#include "runtime_log.h"

#include <d3dcompiler.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
void Logf(const char* format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    RetailLogLine(line);
}

const char kShaderSource[] = R"(
cbuffer Cb : register(b0)
{
    uint W; uint H; float P00; float P22;
    float P32; float DeltaT; float Pad0; float Pad1;
};
Texture2D<float>  Depth : register(t0);
Texture2D<float4> Color : register(t1);
RWTexture2D<uint>   Splat : register(u0);
RWTexture2D<float4> Out   : register(u1);
RWStructuredBuffer<uint> Stats : register(u2);
SamplerState Linear : register(s0);

float Disparity(float d)
{
    float negz = -P32 / (d + P22);
    return DeltaT * P00 * (0.5 * W) / negz;
}

[numthreads(16, 16, 1)]
void SplatMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    float d = Depth.Load(int3(id.xy, 0));
    float disp = (d >= 1.0) ? 0.0 : Disparity(min(d, 0.99999));
    float tx = id.x + disp;
    int x0 = (int)floor(tx);
    int x1 = x0 + 1;
    uint bits = asuint(d);
    if (x0 >= 0 && x0 < (int)W) InterlockedMin(Splat[uint2(x0, id.y)], bits);
    if (x1 >= 0 && x1 < (int)W) InterlockedMin(Splat[uint2(x1, id.y)], bits);
}

[numthreads(16, 16, 1)]
void GatherMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    uint bits = Splat[id.xy];
    if (bits == 0xFFFFFFFFu)
    {
        Out[id.xy] = float4(0, 0, 0, 0);
        InterlockedAdd(Stats[0], 1);
        return;
    }
    float d = asfloat(bits);
    float disp = (d >= 1.0) ? 0.0 : Disparity(min(d, 0.99999));
    float sx = id.x + 0.5 - disp;
    float2 uv = float2(sx / W, (id.y + 0.5) / H);
    float4 c = Color.SampleLevel(Linear, uv, 0);
    Out[id.xy] = float4(c.rgb, 1);
}

[numthreads(16, 16, 1)]
void FillMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H) return;
    if (Out[id.xy].a > 0.5) return;
    float4 left = 0, right = 0;
    float dl = -1, dr = -1;
    int il = -1, ir = -1;
    for (int k = 1; k <= 160; ++k)
    {
        if (il < 0)
        {
            int x = (int)id.x - k;
            if (x >= 0)
            {
                float4 c = Out[uint2(x, id.y)];
                if (c.a > 0.5) { left = c; il = k; dl = asfloat(Splat[uint2(x, id.y)]); }
            }
        }
        if (ir < 0)
        {
            int x = (int)id.x + k;
            if (x < (int)W)
            {
                float4 c = Out[uint2(x, id.y)];
                if (c.a > 0.5) { right = c; ir = k; dr = asfloat(Splat[uint2(x, id.y)]); }
            }
        }
        if (il >= 0 && ir >= 0) break;
    }
    float4 pick;
    if (il >= 0 && ir >= 0) pick = (dl >= dr) ? left : right;
    else if (il >= 0) pick = left;
    else if (ir >= 0) pick = right;
    else { InterlockedAdd(Stats[1], 1); pick = float4(0, 0, 0, 1); }
    Out[id.xy] = float4(pick.rgb, 0.5);
}
)";

struct alignas(16) CbData
{
    std::uint32_t w, h;
    float p00, p22, p32, deltaT, pad0, pad1;
};

ID3D11Device* g_device{};
ID3D11ComputeShader* g_splat{};
ID3D11ComputeShader* g_gather{};
ID3D11ComputeShader* g_fill{};
ID3D11Buffer* g_cb{};
ID3D11SamplerState* g_sampler{};
ID3D11Texture2D* g_splatTex{};
ID3D11UnorderedAccessView* g_splatUav{};
ID3D11Texture2D* g_out{};
ID3D11UnorderedAccessView* g_outUav{};
ID3D11Buffer* g_stats{};
ID3D11UnorderedAccessView* g_statsUav{};
ID3D11Buffer* g_statsStaging{};
UINT g_w{}, g_h{};
bool g_failed = false;
std::uint64_t g_calls{};
bool g_statsPending = false;
std::uint64_t g_statsCall{};

struct SrvCache { ID3D11Texture2D* texture{}; ID3D11ShaderResourceView* srv{}; };
std::array<SrvCache, 12> g_srvs{};

ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* texture, DXGI_FORMAT format)
{
    for (auto& e : g_srvs)
        if (e.texture == texture && e.srv)
            return e.srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv{};
    if (FAILED(g_device->CreateShaderResourceView(texture, &desc, &srv)) || !srv)
        return nullptr;
    for (auto& e : g_srvs)
    {
        if (!e.srv)
        {
            e.texture = texture;
            e.srv = srv;
            return srv;
        }
    }
    // Cache full: evict the first entry.
    g_srvs[0].srv->Release();
    g_srvs[0].texture = texture;
    g_srvs[0].srv = srv;
    return srv;
}

bool Compile(const char* entry, ID3D11ComputeShader** out)
{
    ID3DBlob* code{};
    ID3DBlob* errors{};
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "afw_synth", nullptr,
        nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) || !code)
    {
        Logf("[AFW] shader %s compile failed hr=0x%08X: %s", entry, static_cast<unsigned>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no message)");
        if (errors) errors->Release();
        return false;
    }
    if (errors) errors->Release();
    const HRESULT created = g_device->CreateComputeShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, out);
    code->Release();
    if (FAILED(created))
    {
        Logf("[AFW] CreateComputeShader %s failed hr=0x%08X", entry, static_cast<unsigned>(created));
        return false;
    }
    return true;
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void ReleaseSized()
{
    SafeRelease(g_splatUav);
    SafeRelease(g_splatTex);
    SafeRelease(g_outUav);
    SafeRelease(g_out);
    for (auto& e : g_srvs)
    {
        SafeRelease(e.srv);
        e.texture = nullptr;
    }
    g_w = g_h = 0;
}

bool EnsureResources(ID3D11Device* device, UINT w, UINT h)
{
    if (g_failed)
        return false;
    if (g_device != device)
    {
        AfwSynth::Shutdown();
        g_device = device;
        g_device->AddRef();
    }
    if (!g_splat && !(Compile("SplatMain", &g_splat) && Compile("GatherMain", &g_gather) &&
                      Compile("FillMain", &g_fill)))
    {
        g_failed = true;
        return false;
    }
    if (!g_cb)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(CbData);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &g_cb)))
        {
            g_failed = true;
            return false;
        }
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &g_sampler)))
        {
            g_failed = true;
            return false;
        }
        D3D11_BUFFER_DESC st{};
        st.ByteWidth = 16;
        st.Usage = D3D11_USAGE_DEFAULT;
        st.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        st.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        st.StructureByteStride = 4;
        if (FAILED(device->CreateBuffer(&st, nullptr, &g_stats)))
        {
            g_failed = true;
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = 4;
        if (FAILED(device->CreateUnorderedAccessView(g_stats, &ud, &g_statsUav)))
        {
            g_failed = true;
            return false;
        }
        st.Usage = D3D11_USAGE_STAGING;
        st.BindFlags = 0;
        st.MiscFlags = 0;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&st, nullptr, &g_statsStaging)))
        {
            g_failed = true;
            return false;
        }
    }
    if (g_w != w || g_h != h)
    {
        ReleaseSized();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &g_splatTex)) ||
            FAILED(device->CreateUnorderedAccessView(g_splatTex, nullptr, &g_splatUav)))
        {
            Logf("[AFW] splat texture creation failed");
            g_failed = true;
            return false;
        }
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &g_out)) ||
            FAILED(device->CreateUnorderedAccessView(g_out, nullptr, &g_outUav)))
        {
            Logf("[AFW] output texture creation failed");
            g_failed = true;
            return false;
        }
        g_w = w;
        g_h = h;
        Logf("[AFW] synthesizer ready %ux%u (splat R32_UINT + out RGBA16F)", w, h);
    }
    return true;
}

// The game's compute state around the mod's dispatches, restored afterwards: this
// runs on the engine's immediate context in the middle of its frame.
struct SavedState
{
    ID3D11ComputeShader* cs{};
    ID3D11ShaderResourceView* srv[2]{};
    ID3D11UnorderedAccessView* uav[3]{};
    ID3D11Buffer* cb{};
    ID3D11SamplerState* sampler{};
    void Save(ID3D11DeviceContext* c)
    {
        c->CSGetShader(&cs, nullptr, nullptr);
        c->CSGetShaderResources(0, 2, srv);
        c->CSGetUnorderedAccessViews(0, 3, uav);
        c->CSGetConstantBuffers(0, 1, &cb);
        c->CSGetSamplers(0, 1, &sampler);
    }
    void Restore(ID3D11DeviceContext* c)
    {
        UINT counts[3] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetShaderResources(0, 2, srv);
        c->CSSetUnorderedAccessViews(0, 3, uav, counts);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetSamplers(0, 1, &sampler);
        SafeRelease(cs);
        for (auto& v : srv) SafeRelease(v);
        for (auto& v : uav) SafeRelease(v);
        SafeRelease(cb);
        SafeRelease(sampler);
    }
};
} // namespace

namespace AfwSynth
{
ID3D11Texture2D* Synthesize(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* color, ID3D11Texture2D* depthTwin, const Params& params)
{
    if (!device || !context || !color || !depthTwin)
        return nullptr;
    D3D11_TEXTURE2D_DESC cd{}, dd{};
    color->GetDesc(&cd);
    depthTwin->GetDesc(&dd);
    if (cd.Width != dd.Width || cd.Height != dd.Height)
    {
        if ((g_calls++ % 600) == 0)
            Logf("[AFW] size mismatch colour %ux%u depth %ux%u", cd.Width, cd.Height, dd.Width, dd.Height);
        return nullptr;
    }
    if (!EnsureResources(device, cd.Width, cd.Height))
        return nullptr;
    DXGI_FORMAT depthView = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    switch (dd.Format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS: depthView = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: depthView = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R24G8_TYPELESS: depthView = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: depthView = DXGI_FORMAT_R16_UNORM; break;
    default:
        if ((g_calls % 600) == 0)
            Logf("[AFW] unsupported depth format %u", static_cast<unsigned>(dd.Format));
        return nullptr;
    }
    ID3D11ShaderResourceView* depthSrv = SrvFor(depthTwin, depthView);
    ID3D11ShaderResourceView* colorSrv = SrvFor(color, cd.Format);
    if (!depthSrv || !colorSrv)
    {
        if ((g_calls % 600) == 0)
            Logf("[AFW] SRV creation failed (depth=%p colour=%p)", static_cast<void*>(depthSrv),
                static_cast<void*>(colorSrv));
        return nullptr;
    }
    ++g_calls;

    CbData cb{};
    cb.w = cd.Width;
    cb.h = cd.Height;
    cb.p00 = params.p00;
    cb.p22 = params.p22;
    cb.p32 = params.p32;
    cb.deltaT = params.deltaT;
    context->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);

    SavedState saved{};
    saved.Save(context);

    const UINT clear[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    context->ClearUnorderedAccessViewUint(g_splatUav, clear);
    const UINT zero[4] = {0, 0, 0, 0};
    context->ClearUnorderedAccessViewUint(g_statsUav, zero);

    ID3D11ShaderResourceView* srvs[2] = {depthSrv, colorSrv};
    ID3D11UnorderedAccessView* uavs[3] = {g_splatUav, g_outUav, g_statsUav};
    const UINT counts[3] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
    context->CSSetConstantBuffers(0, 1, &g_cb);
    context->CSSetSamplers(0, 1, &g_sampler);
    context->CSSetShaderResources(0, 2, srvs);
    context->CSSetUnorderedAccessViews(0, 3, uavs, counts);
    const UINT gx = (cd.Width + 15) / 16, gy = (cd.Height + 15) / 16;
    context->CSSetShader(g_splat, nullptr, 0);
    context->Dispatch(gx, gy, 1);
    context->CSSetShader(g_gather, nullptr, 0);
    context->Dispatch(gx, gy, 1);
    context->CSSetShader(g_fill, nullptr, 0);
    context->Dispatch(gx, gy, 1);

    // Unbind ours before restoring theirs (a UAV and an SRV on the same
    // resource may not coexist).
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav[3] = {};
    context->CSSetShaderResources(0, 2, nullSrv);
    context->CSSetUnorderedAccessViews(0, 3, nullUav, counts);
    saved.Restore(context);

    // Hole statistics, read back a few frames later without stalling.
    if (g_statsPending)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(g_statsStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (SUCCEEDED(hr))
        {
            const auto* v = static_cast<const std::uint32_t*>(mapped.pData);
            const double total = static_cast<double>(cd.Width) * cd.Height;
            Logf("[AFW] call %llu: holes before fill %.2f%% (%u px), unfilled %.3f%% (%u px), deltaT=%+.4f P00=%.4f",
                static_cast<unsigned long long>(g_statsCall), 100.0 * v[0] / total, v[0],
                100.0 * v[1] / total, v[1], params.deltaT, params.p00);
            context->Unmap(g_statsStaging, 0);
            g_statsPending = false;
        }
        else if (hr != DXGI_ERROR_WAS_STILL_DRAWING)
            g_statsPending = false;
    }
    else if ((g_calls % 600) == 1)
    {
        context->CopyResource(g_statsStaging, g_stats);
        g_statsPending = true;
        g_statsCall = g_calls;
    }
    return g_out;
}

void Shutdown()
{
    ReleaseSized();
    SafeRelease(g_splat);
    SafeRelease(g_gather);
    SafeRelease(g_fill);
    SafeRelease(g_cb);
    SafeRelease(g_sampler);
    SafeRelease(g_statsUav);
    SafeRelease(g_stats);
    SafeRelease(g_statsStaging);
    SafeRelease(g_device);
    g_failed = false;
    g_calls = 0;
    g_statsPending = false;
}
}
