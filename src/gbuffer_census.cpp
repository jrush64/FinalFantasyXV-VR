#include "research_diagnostics.h"
#include "gbuffer_census.h"
#include "native_draw_pose.h"
#include "runtime_log.h"

#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

// What the capture relies on:
//   - the engine keeps TWO full-size G-buffer families; native stereo gives
//     one to each eye, one-output modes (mono, AER) ALTERNATE them frame to
//     frame; stereo toggles recreate them with new pointers.  So candidates
//     are chosen dynamically every frame (any full-size depth / two-channel
//     target that took draws), never locked to a pointer.
//   - the engine records the next frame's G-buffer pass before the current
//     frame presents, so "draws this present" is racy.  Capture on unbind
//     after >= 8 draws SINCE THE LAST CAPTURE of that buffer instead.
//   - dumps need a hard total budget.
//   - velocity semantics need the camera: each capture reads the projection
//     and view the engine uploaded for those draws (NativeDrawPose stamps the
//     768-byte camera buffer) and the dump writes them to a sidecar.

namespace
{
constexpr std::size_t kResSlots = 512;
constexpr std::size_t kCtxSlots = 64;
constexpr std::size_t kPassSlots = 96;
constexpr int kTwins = 16;
constexpr std::uint32_t kMinDrawsForCapture = 8;
constexpr std::uint32_t kSceneFrameMinDepthDraws = 50;
constexpr std::uint32_t kDumpAfterAerScenePresents = 240; // let AER settle first
constexpr std::uint32_t kDumpBudget = 6;                 // total dumps, ever
constexpr std::uint32_t kCapturedLogPresents = 120;      // per-present lines after AER on
constexpr int kDumpSubsample = 4;

void Logf(const char* format, ...)
{
    char line[1400];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    RetailLogLine(line);
}

const char* FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16T";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "RG11B10F";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8s";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8T";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16G16_TYPELESS: return "RG16T";
    case DXGI_FORMAT_R16G16_UNORM: return "RG16U";
    case DXGI_FORMAT_R16G16_SNORM: return "RG16S";
    case DXGI_FORMAT_R16G16_UINT: return "RG16UI";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R32_TYPELESS: return "R32T";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8T";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24T";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R8G8_SNORM: return "RG8S";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R16_UNORM: return "R16U";
    case DXGI_FORMAT_R16_TYPELESS: return "R16T";
    case DXGI_FORMAT_D16_UNORM: return "D16";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    default: return nullptr;
    }
}

bool IsDepthFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
        return true;
    default:
        return false;
    }
}

// [EYEFULL] Full-size colour targets, only while the one-shot dump is armed.
bool IsColorFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
        return true;
    default:
        return false;
    }
}

bool IsTwoChannelFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_TYPELESS:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT TwinFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    default: return f;
    }
}

UINT BytesPerPixel(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 4;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return 2;
    case DXGI_FORMAT_R8_UNORM: return 1;
    default: return 0;
    }
}

struct Res
{
    std::atomic_uintptr_t key{};
    std::atomic_uint32_t ready{};
    DXGI_FORMAT format{};
    UINT width{}, height{}, arraySize{}, samples{};
    std::atomic_uint64_t rtvBinds{}, dsvBinds{};
    std::atomic_uint64_t rtvDraws{}, dsvDraws{};
    std::atomic_uint64_t drawsWithDsv{};
    std::atomic_uint64_t clears{};
    std::atomic_uint32_t slotMask{};
    std::atomic_int firstPass{-1};
    std::atomic_int lastDrawPass{-1};
    std::atomic_int lastClearPass{-1};
    std::atomic_uint32_t drawsThisFrame{};
    std::atomic_uint32_t drawsSinceCapture{};
    std::atomic_uint32_t firstPresent{};
    std::atomic_uint32_t lastPresent{};
    std::atomic_int twin{-1};
};
std::array<Res, kResSlots> g_res{};
std::atomic_uint32_t g_resOverflow{};

struct Ctx
{
    std::atomic_uintptr_t key{};
    std::atomic_int rt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    std::atomic_int dsv{-1};
};
std::array<Ctx, kCtxSlots> g_ctx{};

struct Pass
{
    char name[56]{};
    std::atomic_uint32_t ready{};
    std::atomic_uint32_t minOrdinal{0xFFFFFFFFu};
};
std::array<Pass, kPassSlots> g_pass{};
std::atomic_uint32_t g_passCount{};
std::atomic_int g_currentPass{-1};
std::atomic_uint32_t g_passOrdinalThisFrame{};

std::atomic_uint32_t g_present{};
std::atomic_uint32_t g_sceneFrames{};
std::atomic_uint32_t g_fullW{}, g_fullH{}; // the backbuffer-sized family, learnt from draws
bool g_enabled = ResearchDiagnostics::Enabled;
bool g_initDone = false;
std::atomic_uint64_t g_drawsSeen{};
std::atomic_uint64_t g_setRtCalls{};

struct Twin
{
    ID3D11Texture2D* texture{};
    ID3D11Texture2D* staging{};
    DXGI_FORMAT format{};
    UINT width{}, height{};
    std::atomic_int resIndex{-1};
    std::atomic_uint32_t capturedPresent{};
    std::atomic_uint64_t captures{};
    std::atomic_int lastCapturePass{-1};
    SRWLOCK camLock = SRWLOCK_INIT;
    float cam[32]{};        // projection (16) + view (16) uploaded for the captured draws
    bool camValid{};
};
Twin g_twin[kTwins]{};
SRWLOCK g_twinLock = SRWLOCK_INIT;
std::atomic_uint32_t g_twinFailures{};

std::atomic_bool g_colorArmed{};
// [EYEFULL] The engine keeps ONE full-size colour target and renders both eyes
// through it in turn (the census shows two depth families but a single colour),
// so each eye's full-resolution image only exists for part of the frame.  Eye A
// is grabbed at the moment the depth family switches - at that point A's post
// chain has finished and B has not started - and eye B at present, when the
// target holds the last eye rendered.
Twin g_eyeTwin[2]{};
std::atomic_int g_sceneColorIdx{-1};
ID3D11Texture2D* g_sceneColorTex{};      // AddRef'd, for the present-time grab
SRWLOCK g_sceneColorLock = SRWLOCK_INIT;
std::atomic_int g_lastDepthFamily{-1};
// The TWO main scene depth buffers (one per eye), by draw count.  Any
// full-size depth would include shadow and auxiliary passes, which fire
// several times a frame; the last one lands just after a clear.
std::atomic_int g_mainDepth[2] = {-1, -1};
std::atomic_bool g_eyeACaptured{};
std::atomic_uint64_t g_eyeCaptures[2]{};
std::atomic_uint32_t g_eyeCaptureMisses{};
std::uint32_t g_colorDumps = 0;
std::atomic_uint32_t g_aerPresents{};
std::atomic_uint32_t g_aerScenePresents{};
std::atomic_int g_presentedDepthTwin{-1};
std::atomic_int g_presentedVelTwin{-1};
std::atomic_uint32_t g_presentedPresent{};
std::uint32_t g_dumps = 0;
wchar_t g_dumpDir[MAX_PATH]{};

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
            *(slash + 1) = L'\0';
            std::wcsncpy(g_dumpDir, path, MAX_PATH - 1);
            std::wcsncat(g_dumpDir, L"gbufdump", MAX_PATH - 1 - std::wcslen(g_dumpDir));
            wchar_t ini[MAX_PATH]{};
            std::wcsncpy(ini, path, MAX_PATH - 1);
            std::wcsncat(ini, L"ffxv-vr.ini", MAX_PATH - 1 - std::wcslen(ini));
            g_enabled = ResearchDiagnostics::Enabled && GetPrivateProfileIntW(L"Probe", L"GBuf", 0, ini) != 0;
        }
    }
    Logf("[GBUF] v3 depth/velocity census %s (ini [Probe] GBuf). Every full-size depth and "
         "two-channel target is captured on unbind after >=%u draws; captured-per-present lines "
         "for the first %u AER presents; %u dumps max, AER only, after %u AER scene presents, "
         "each with a camera sidecar.",
        g_enabled ? "ON" : "off", kMinDrawsForCapture, kCapturedLogPresents, kDumpBudget,
        kDumpAfterAerScenePresents);
}

int PassIndexByName(const char* name)
{
    const auto count = std::min<std::uint32_t>(g_passCount.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(kPassSlots));
    for (std::uint32_t i = 0; i < count; ++i)
        if (g_pass[i].ready.load(std::memory_order_acquire) &&
            std::strncmp(g_pass[i].name, name, sizeof(g_pass[i].name) - 1) == 0)
            return static_cast<int>(i);
    return -1;
}

const char* PassName(int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= kPassSlots ||
        !g_pass[static_cast<std::size_t>(index)].ready.load(std::memory_order_acquire))
        return "-";
    return g_pass[static_cast<std::size_t>(index)].name;
}

ID3D11Texture2D* TextureOf(ID3D11View* view)
{
    if (!view)
        return nullptr;
    ID3D11Resource* resource{};
    view->GetResource(&resource);
    if (!resource)
        return nullptr;
    ID3D11Texture2D* texture{};
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    return texture;
}

int ResIndex(ID3D11Texture2D* texture)
{
    const auto key = reinterpret_cast<std::uintptr_t>(texture);
    for (std::size_t i = 0; i < kResSlots; ++i)
    {
        auto& r = g_res[i];
        const auto have = r.key.load(std::memory_order_acquire);
        if (have == key)
            return static_cast<int>(i);
        if (have == 0)
        {
            std::uintptr_t expected = 0;
            if (r.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
            {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                r.format = desc.Format;
                r.width = desc.Width;
                r.height = desc.Height;
                r.arraySize = desc.ArraySize;
                r.samples = desc.SampleDesc.Count;
                r.firstPresent.store(g_present.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                r.ready.store(1, std::memory_order_release);
                return static_cast<int>(i);
            }
            if (r.key.load(std::memory_order_acquire) == key)
                return static_cast<int>(i);
        }
    }
    g_resOverflow.fetch_add(1, std::memory_order_relaxed);
    return -1;
}

Ctx* CtxFor(ID3D11DeviceContext* context, bool create)
{
    const auto key = reinterpret_cast<std::uintptr_t>(context);
    for (auto& c : g_ctx)
    {
        const auto have = c.key.load(std::memory_order_acquire);
        if (have == key)
            return &c;
        if (have == 0)
        {
            if (!create)
                return nullptr;
            std::uintptr_t expected = 0;
            if (c.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel))
            {
                for (auto& slot : c.rt)
                    slot.store(-1, std::memory_order_relaxed);
                c.dsv.store(-1, std::memory_order_relaxed);
                return &c;
            }
            if (c.key.load(std::memory_order_acquire) == key)
                return &c;
        }
    }
    return nullptr;
}

bool Eligible(const Res& r)
{
    if (!r.ready.load(std::memory_order_acquire) || r.samples != 1)
        return false;
    const auto fw = g_fullW.load(std::memory_order_relaxed);
    const auto fh = g_fullH.load(std::memory_order_relaxed);
    if (!fw || r.width != fw || r.height != fh)
        return false;
    if (IsDepthFormat(r.format) || IsTwoChannelFormat(r.format))
        return true;
    return g_colorArmed.load(std::memory_order_acquire) && IsColorFormat(r.format);
}

const char* KindOf(const Res& r)
{
    return IsDepthFormat(r.format) ? "D" : (IsTwoChannelFormat(r.format) ? "V" : "C");
}

// Twin for a resource: its own, or a free one, or the one whose resource has
// been silent longest (stereo toggles retire whole families).
Twin* TwinFor(int resIndex, ID3D11Texture2D* source, Res& r)
{
    const int have = r.twin.load(std::memory_order_acquire);
    if (have >= 0)
        return &g_twin[have];
    AcquireSRWLockExclusive(&g_twinLock);
    Twin* result = nullptr;
    const int again = r.twin.load(std::memory_order_acquire);
    if (again >= 0)
        result = &g_twin[again];
    else
    {
        int pick = -1;
        std::uint32_t oldest = 0xFFFFFFFFu;
        const auto present = g_present.load(std::memory_order_relaxed);
        for (int t = 0; t < kTwins; ++t)
        {
            const int owner = g_twin[t].resIndex.load(std::memory_order_relaxed);
            if (owner < 0)
            {
                pick = t;
                break;
            }
            const auto last = g_res[static_cast<std::size_t>(owner)].lastPresent.load(std::memory_order_relaxed);
            if (present - last > 600 && last < oldest)
            {
                oldest = last;
                pick = t;
            }
        }
        if (pick >= 0)
        {
            Twin& twin = g_twin[pick];
            const int previous = twin.resIndex.load(std::memory_order_relaxed);
            if (previous >= 0)
                g_res[static_cast<std::size_t>(previous)].twin.store(-1, std::memory_order_release);
            D3D11_TEXTURE2D_DESC desc{};
            source->GetDesc(&desc);
            const DXGI_FORMAT wanted = TwinFormat(desc.Format);
            if (twin.texture && (twin.format != wanted || twin.width != desc.Width ||
                                    twin.height != desc.Height))
            {
                twin.texture->Release();
                twin.texture = nullptr;
                if (twin.staging)
                {
                    twin.staging->Release();
                    twin.staging = nullptr;
                }
            }
            if (!twin.texture)
            {
                ID3D11Device* device{};
                source->GetDevice(&device);
                if (device)
                {
                    desc.Format = wanted;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.MiscFlags = 0;
                    desc.CPUAccessFlags = 0;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.MipLevels = 1;
                    desc.ArraySize = 1;
                    ID3D11Texture2D* texture{};
                    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) && texture)
                    {
                        twin.texture = texture;
                        twin.format = desc.Format;
                        twin.width = desc.Width;
                        twin.height = desc.Height;
                    }
                    else
                        g_twinFailures.fetch_add(1, std::memory_order_relaxed);
                    device->Release();
                }
            }
            if (twin.texture)
            {
                twin.resIndex.store(resIndex, std::memory_order_release);
                twin.capturedPresent.store(0, std::memory_order_relaxed);
                twin.captures.store(0, std::memory_order_relaxed);
                r.twin.store(pick, std::memory_order_release);
                result = &twin;
                Logf("[GBUF] twin %d -> #%d %s %ux%u %s", pick, resIndex, KindOf(r), r.width,
                    r.height, FormatName(r.format) ? FormatName(r.format) : "?");
            }
        }
    }
    ReleaseSRWLockExclusive(&g_twinLock);
    return result;
}

// The eligible resource `resIndex` is leaving this context after real draws:
// copy it now, on this context, and remember the camera those draws used.
void Capture(ID3D11DeviceContext* context, int resIndex, ID3D11View* view)
{
    auto& r = g_res[static_cast<std::size_t>(resIndex)];
    if (!Eligible(r) || r.drawsSinceCapture.load(std::memory_order_relaxed) < kMinDrawsForCapture)
        return;
    ID3D11Texture2D* source = TextureOf(view);
    if (!source)
        return;
    Twin* twin = TwinFor(resIndex, source, r);
    if (twin)
    {
        context->CopyResource(twin->texture, source);
        const auto present = g_present.load(std::memory_order_relaxed);
        if (twin->capturedPresent.load(std::memory_order_relaxed) != present)
            twin->captures.fetch_add(1, std::memory_order_relaxed);
        twin->capturedPresent.store(present, std::memory_order_relaxed);
        twin->lastCapturePass.store(g_currentPass.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        float cam[32]{};
        const bool camValid = NativeDrawPose::ReadRawCamera(context, cam);
        AcquireSRWLockExclusive(&twin->camLock);
        std::memcpy(twin->cam, cam, sizeof(cam));
        twin->camValid = camValid;
        ReleaseSRWLockExclusive(&twin->camLock);
        r.drawsSinceCapture.store(0, std::memory_order_relaxed);
    }
    source->Release();
}

// Copy the shared scene-colour target into one of the two eye twins.
void CaptureEye(ID3D11DeviceContext* context, ID3D11Texture2D* source, int slot)
{
    if (!context || !source || slot < 0 || slot > 1)
        return;
    Twin& twin = g_eyeTwin[slot];
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (twin.texture && (twin.width != desc.Width || twin.height != desc.Height ||
            twin.format != desc.Format))
    {
        twin.texture->Release();
        twin.texture = nullptr;
        if (twin.staging)
        {
            twin.staging->Release();
            twin.staging = nullptr;
        }
    }
    if (!twin.texture)
    {
        ID3D11Device* device{};
        source->GetDevice(&device);
        if (!device)
            return;
        D3D11_TEXTURE2D_DESC td = desc;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        td.CPUAccessFlags = 0;
        td.MipLevels = 1;
        td.ArraySize = 1;
        ID3D11Texture2D* texture{};
        const HRESULT hr = device->CreateTexture2D(&td, nullptr, &texture);
        device->Release();
        if (FAILED(hr) || !texture)
        {
            g_eyeCaptureMisses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        twin.texture = texture;
        twin.width = desc.Width;
        twin.height = desc.Height;
        twin.format = desc.Format;
        Logf("[EYEFULL] eye twin %d created %ux%u %s", slot, desc.Width, desc.Height,
            FormatName(desc.Format) ? FormatName(desc.Format) : "?");
    }
    context->CopyResource(twin.texture, source);
    twin.capturedPresent.store(g_present.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_eyeCaptures[slot].fetch_add(1, std::memory_order_relaxed);
}

bool WriteDump(ID3D11DeviceContext* immediate, ID3D11Texture2D* source, DXGI_FORMAT format,
    UINT width, UINT height, ID3D11Texture2D** staging, const wchar_t* fileName)
{
    const UINT bpp = BytesPerPixel(format);
    if (!bpp)
        return false;
    if (!*staging)
    {
        ID3D11Device* device{};
        source->GetDevice(&device);
        if (!device)
            return false;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, staging);
        device->Release();
        if (FAILED(hr) || !*staging)
            return false;
    }
    immediate->CopyResource(*staging, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(immediate->Map(*staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    FILE* file = _wfopen(fileName, L"wb");
    if (!file)
    {
        immediate->Unmap(*staging, 0);
        return false;
    }
    const UINT outW = (width + kDumpSubsample - 1) / kDumpSubsample;
    const UINT outH = (height + kDumpSubsample - 1) / kDumpSubsample;
    std::uint32_t header[8] = {0x46554247u, static_cast<std::uint32_t>(format), width, height,
        outW, outH, bpp, static_cast<std::uint32_t>(kDumpSubsample)};
    fwrite(header, sizeof(header), 1, file);
    std::uint8_t* row = new (std::nothrow) std::uint8_t[static_cast<std::size_t>(outW) * bpp];
    if (row)
    {
        for (UINT y = 0; y < height; y += kDumpSubsample)
        {
            const auto* src = static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0, o = 0; x < width; x += kDumpSubsample, ++o)
                std::memcpy(row + static_cast<std::size_t>(o) * bpp,
                    src + static_cast<std::size_t>(x) * bpp, bpp);
            fwrite(row, static_cast<std::size_t>(outW) * bpp, 1, file);
        }
        delete[] row;
    }
    fclose(file);
    immediate->Unmap(*staging, 0);
    return true;
}


// Bounded diagnostic: preserve inputs to small fullscreen draws, never modify them.
struct StageShot { ID3D11Texture2D* texture{}; D3D11_TEXTURE2D_DESC desc{}; };
std::array<StageShot, 32> g_stageShots{};
unsigned g_stageCount = 0;
void CaptureStage(ID3D11DeviceContext* context, unsigned vertices)
{
    if (!g_colorArmed.load(std::memory_order_relaxed) || vertices == 0 || vertices > 6 ||
        g_stageCount >= g_stageShots.size() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ID3D11ShaderResourceView* srv{};
    context->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* resource{}; srv->GetResource(&resource); srv->Release();
    ID3D11Texture2D* source{};
    resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&source));
    resource->Release();
    if (!source) return;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Width != g_fullW || desc.Height != g_fullH || desc.ArraySize != 1 || desc.MipLevels != 1 ||
        desc.SampleDesc.Count != 1 || (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
    { source->Release(); return; }
    auto& shot = g_stageShots[g_stageCount];
    shot.desc = desc; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = 0; desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
    ID3D11Device* device{}; context->GetDevice(&device);
    const HRESULT result = device->CreateTexture2D(&desc, nullptr, &shot.texture); device->Release();
    if (SUCCEEDED(result)) {
        context->CopyResource(shot.texture, source);
        ID3D11PixelShader* shader{}; context->PSGetShader(&shader, nullptr, nullptr);
        ID3D11RenderTargetView* output{}; context->OMGetRenderTargets(1, &output, nullptr);
        ID3D11Resource* target{}; if (output) output->GetResource(&target);
        Logf("[GLARESTAGE] present=%u stage=%u vertices=%u ps=%p input=%p res=%d output=%p pass=%s",
            g_present.load(), g_stageCount, vertices, shader, source, ResIndex(source), target,
            PassName(g_currentPass.load()));
        if (target) target->Release(); if (output) output->Release(); if (shader) shader->Release();
        ++g_stageCount;
    }
    source->Release();
}
void DumpStages(ID3D11DeviceContext* context, unsigned present)
{
    for (unsigned i=0; i<g_stageCount; ++i) {
        auto& shot=g_stageShots[i]; ID3D11Texture2D* staging{}; wchar_t file[MAX_PATH]{};
        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\eye%u_STAGE%02u.raw", g_dumpDir, present, i);
        const bool ok=WriteDump(context,shot.texture,shot.desc.Format,shot.desc.Width,shot.desc.Height,&staging,file);
        Logf("[GLARESTAGE] saved=%d stage=%u present=%u",ok?1:0,i,present);
        if(staging) staging->Release(); shot.texture->Release(); shot={};
    }
    g_stageCount=0;
}




const GUID kGlareSuspect={0x92b521d8,0x8746,0x4952,{0x9a,0x12,0x44,0x86,0x72,0x64,0x9c,0x21}};
thread_local unsigned g_argStart=0,g_argLane=0;
thread_local int g_argBase=0;
const GUID kGlareLayout={0x74bc53af,0x41aa,0x4c98,{0x9c,0x1a,0x82,0x24,0x3a,0x81,0x69,0x27}};
struct LayoutElement {char name[40];UINT index,format,slot,offset,classification,step;};

const GUID kGlareVertexResource={0x592197bf,0x7333,0x4c70,{0x93,0x14,0x87,0x73,0x88,0x11,0x5c,0x61}};
struct WriterMap {ID3D11Resource* resource{};void* pointer{};UINT size{},type{},depth{};void* stack[12]{};};
thread_local WriterMap g_writerMaps[8]{};
struct WriterCopy {unsigned char* bytes{};UINT size{},type{},depth{},thread{},offset{};std::uintptr_t resource{},stack[12]{};};
WriterCopy g_writerCopies[16]{};unsigned g_writerCount=0,g_writerBytes=0;SRWLOCK g_writerLock=SRWLOCK_INIT;
void DumpWriters(unsigned present){
 AcquireSRWLockExclusive(&g_writerLock);
 const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
 for(unsigned i=0;i<g_writerCount;++i){auto& w=g_writerCopies[i];wchar_t file[MAX_PATH]{};
  _snwprintf_s(file,MAX_PATH,_TRUNCATE,L"%ls\\eye%u_WRITE%02u.bin",g_dumpDir,present,i);
  FILE* f=_wfopen(file,L"wb");size_t wrote=0;if(f){wrote=fwrite(w.bytes,1,w.size,f);fclose(f);}
  Logf("[GLAREWRITE] present=%u writer=%u resource=%llX type=%u thread=%u bytes=%zu offset=%u",present,i,static_cast<unsigned long long>(w.resource),w.type,w.thread,wrote,w.offset);
  for(unsigned j=0;j<w.depth;++j)Logf("[GLAREWRITE] writer=%u stack=%u pc=%llX exeRelative=%llX",i,j,static_cast<unsigned long long>(w.stack[j]),static_cast<unsigned long long>(w.stack[j]-base));
  delete[] w.bytes;w={};
 }
 g_writerCount=0;g_writerBytes=0;ReleaseSRWLockExclusive(&g_writerLock);
}
struct GeometryBuffer {ID3D11Buffer* copy{};UINT bytes{};unsigned draw{};char label[16]{};};
std::array<GeometryBuffer,512> g_geometry{};
unsigned g_geometryCount=0,g_geometryBytes=0,g_geometryDraws=0;
void CopyGeometryBuffer(ID3D11DeviceContext* ctx,ID3D11Buffer* source,unsigned draw,const char* label)
{
    if(!source || g_geometryCount>=g_geometry.size())return;
    D3D11_BUFFER_DESC desc{};source->GetDesc(&desc);
    if(desc.ByteWidth>256*1024*1024-g_geometryBytes){Logf("[GLAREGEOM] skipped budget draw=%u %s bytes=%u",draw,label,desc.ByteWidth);return;}
    auto& item=g_geometry[g_geometryCount];item.bytes=desc.ByteWidth;item.draw=draw;strncpy_s(item.label,label,_TRUNCATE);
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=0;desc.MiscFlags=0;desc.StructureByteStride=0;
    ID3D11Device* device{};ctx->GetDevice(&device);HRESULT hr=device->CreateBuffer(&desc,nullptr,&item.copy);device->Release();
    if(SUCCEEDED(hr)){ctx->CopyResource(item.copy,source);++g_geometryCount;g_geometryBytes+=item.bytes;}
}

void CopyGeometrySlice(ID3D11DeviceContext* ctx,ID3D11Buffer* source,unsigned draw,UINT offset,UINT bytes){
 if(!source || g_geometryCount>=g_geometry.size())return;
 D3D11_BUFFER_DESC original{};source->GetDesc(&original);
 if(offset>original.ByteWidth || bytes>original.ByteWidth-offset || !bytes || bytes>65536)return;
 auto& item=g_geometry[g_geometryCount];item.bytes=bytes;item.draw=draw;strcpy_s(item.label,"VB0slice");
 D3D11_BUFFER_DESC desc{};desc.ByteWidth=bytes;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Device* device{};ctx->GetDevice(&device);HRESULT hr=device->CreateBuffer(&desc,nullptr,&item.copy);device->Release();
 if(SUCCEEDED(hr)){D3D11_BOX range{offset,0,0,offset+bytes,1,1};ctx->CopySubresourceRegion(item.copy,0,0,0,0,source,0,&range);++g_geometryCount;g_geometryBytes+=bytes;}
}
void CaptureGeometry(ID3D11DeviceContext* ctx,ID3D11VertexShader* vs,unsigned draw,unsigned vertices)
{
    UINT tag=0,size=sizeof(tag);
    if(!vs || FAILED(vs->GetPrivateData(kGlareSuspect,&size,&tag)) || tag!=1 || g_geometryDraws>=128)return;
    ++g_geometryDraws;
    ID3D11InputLayout* layout{};ctx->IAGetInputLayout(&layout);
    if(layout){LayoutElement elements[32]{};UINT size=sizeof(elements);
        if(SUCCEEDED(layout->GetPrivateData(kGlareLayout,&size,elements)))for(UINT i=0;i<size/sizeof(LayoutElement);++i){auto& e=elements[i];
            Logf("[GLAREGEOM] draw=%u layout=%s%u format=%u slot=%u offset=%u class=%u step=%u",draw,e.name,e.index,e.format,e.slot,e.offset,e.classification,e.step);}
        layout->Release();}

    D3D11_PRIMITIVE_TOPOLOGY topology{};ctx->IAGetPrimitiveTopology(&topology);
    Logf("[GLAREGEOM] present=%u draw=%u count=%u topology=%u start=%u base=%d lane=%u",g_present.load(),draw,vertices,static_cast<UINT>(topology),g_argStart,g_argBase,g_argLane);

    ID3D11Buffer* vertex{};UINT stride{},offset{};ctx->IAGetVertexBuffers(0,1,&vertex,&stride,&offset);
    if(vertex){const UINT tag=1;vertex->SetPrivateData(kGlareVertexResource,sizeof(tag),&tag);
        CopyGeometrySlice(ctx,vertex,draw,offset+g_argStart*stride,vertices*stride);
        Logf("[GLAREGEOM] draw=%u VB0slice resource=%p stride=%u originalOffset=%u fileOffset=0",draw,vertex,stride,offset);vertex->Release();}
    ID3D11Buffer* cb[2]{};ctx->VSGetConstantBuffers(0,2,cb);
    for(unsigned i=0;i<2;++i)if(cb[i]){char label[16];sprintf_s(label,"VSCB%u",i);CopyGeometryBuffer(ctx,cb[i],draw,label);cb[i]->Release();}
}
void DumpGeometry(ID3D11DeviceContext* ctx,unsigned present)
{
    for(unsigned i=0;i<g_geometryCount;++i){auto& item=g_geometry[i];D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(ctx->Map(item.copy,0,D3D11_MAP_READ,0,&mapped))){wchar_t file[MAX_PATH]{};
            _snwprintf_s(file,MAX_PATH,_TRUNCATE,L"%ls\\eye%u_draw%u_%hs.bin",g_dumpDir,present,item.draw,item.label);
            FILE* f=_wfopen(file,L"wb");if(f){const size_t wrote=fwrite(mapped.pData,1,item.bytes,f);fclose(f);Logf("[GLAREGEOM] saved present=%u draw=%u %s bytes=%zu",present,item.draw,item.label,wrote);}
            ctx->Unmap(item.copy,0);}
        item.copy->Release();item={};}
    g_geometryCount=0;g_geometryBytes=0;g_geometryDraws=0;
}
const GUID kGlareShaderBytes={0x5cf52a17,0x8192,0x4eb2,{0x91,0x12,0x82,0x27,0x13,0x42,0x89,0x0a}};
std::array<StageShot,64> g_effectShots{};
unsigned g_effectCount=0;
unsigned g_captureFramesRemaining=0;
unsigned long long SaveShader(ID3D11DeviceChild* shader)
{
    if(!shader)return 0;
    UINT length=0;shader->GetPrivateData(kGlareShaderBytes,&length,nullptr);
    if(!length || length>262144)return 0;
    auto* data=new(std::nothrow) unsigned char[length];if(!data)return 0;
    if(FAILED(shader->GetPrivateData(kGlareShaderBytes,&length,data))){delete[] data;return 0;}
    unsigned long long hash=14695981039346656037ull;
    for(UINT i=0;i<length;++i){hash^=data[i];hash*=1099511628211ull;}
    wchar_t path[MAX_PATH]{};CreateDirectoryW(g_dumpDir,nullptr);
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%ls\\shader_%016llX.dxbc",g_dumpDir,hash);
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD wrote{};WriteFile(file,data,length,&wrote,nullptr);CloseHandle(file);}
    delete[] data;return hash;
}

std::array<StageShot, 32> g_transitionShots{};
unsigned g_transitionCount=0;
unsigned g_traceDraw=0;
void CaptureTransition(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* views)
{
    if (!g_colorArmed.load(std::memory_order_relaxed) || g_transitionCount>=g_transitionShots.size() ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    ID3D11RenderTargetView* old{};context->OMGetRenderTargets(1,&old,nullptr);
    if(!old)return;
    if(count && views && old==views[0]) { old->Release();return; }
    ID3D11Texture2D* source=TextureOf(old);old->Release();if(!source)return;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(desc.Width!=g_fullW || desc.Height!=g_fullH || desc.Format!=DXGI_FORMAT_R11G11B10_FLOAT ||
        desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1) {source->Release();return;}
    auto& shot=g_transitionShots[g_transitionCount];shot.desc=desc;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=0;desc.CPUAccessFlags=0;desc.MiscFlags=0;
    ID3D11Device* device{};context->GetDevice(&device);
    HRESULT result=device->CreateTexture2D(&desc,nullptr,&shot.texture);device->Release();
    if(SUCCEEDED(result)) {
        context->CopyResource(shot.texture,source);
        Logf("[GLARETRANS] present=%u transition=%u afterDraw=%u source=%p res=%d pass=%s",
            g_present.load(),g_transitionCount,g_traceDraw,source,ResIndex(source),PassName(g_currentPass.load()));
        ++g_transitionCount;
    }
    source->Release();
}
void TraceEffectDraw(ID3D11DeviceContext* context,unsigned vertices)
{
    if(!g_colorArmed.load(std::memory_order_relaxed) || g_traceDraw>=8192 || g_transitionCount==0 ||
        context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    ID3D11RenderTargetView* output{};context->OMGetRenderTargets(1,&output,nullptr);if(!output)return;
    ID3D11Texture2D* target=TextureOf(output);output->Release();if(!target)return;
    D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
    if(desc.Format==DXGI_FORMAT_R11G11B10_FLOAT && desc.Width==g_fullW && desc.Height==g_fullH) {
        ID3D11VertexShader* vs{};ID3D11GeometryShader* gs{};ID3D11PixelShader* ps{};
        context->VSGetShader(&vs,nullptr,nullptr);context->GSGetShader(&gs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
        Logf("[GLAREDRAW] present=%u draw=%u count=%u target=%d vs=%p gs=%p ps=%p pass=%s",
            g_present.load(),++g_traceDraw,vertices,ResIndex(target),vs,gs,ps,PassName(g_currentPass.load()));

        CaptureGeometry(context,vs,g_traceDraw,vertices);
        if(g_transitionCount==16 && g_effectCount<4096) {
            const unsigned ring=g_effectCount % g_effectShots.size();
            auto& shot=g_effectShots[ring];if(shot.texture){shot.texture->Release();shot.texture=nullptr;}shot.desc=desc;
            D3D11_TEXTURE2D_DESC copy=desc;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=0;copy.CPUAccessFlags=0;copy.MiscFlags=0;
            ID3D11Device* device{};context->GetDevice(&device);
            HRESULT result=device->CreateTexture2D(&copy,nullptr,&shot.texture);device->Release();
            if(SUCCEEDED(result)) {
                context->CopyResource(shot.texture,target);
                Logf("[GLAREEFFECT] present=%u snapshot=%u beforeDraw=%u count=%u vsHash=%016llX psHash=%016llX",
                    g_present.load(),ring,g_traceDraw,vertices,SaveShader(vs),SaveShader(ps));
                ++g_effectCount;
            }
        }
        if(vs)vs->Release();if(gs)gs->Release();if(ps)ps->Release();
    }
    target->Release();
}
void DumpTransitions(ID3D11DeviceContext* context,unsigned present)
{
    for(unsigned i=0;i<g_transitionCount;++i){
        auto& shot=g_transitionShots[i];ID3D11Texture2D* staging{};wchar_t file[MAX_PATH]{};
        _snwprintf_s(file,MAX_PATH,_TRUNCATE,L"%ls\\eye%u_TRANS%02u.raw",g_dumpDir,present,i);
        const bool ok=WriteDump(context,shot.texture,shot.desc.Format,shot.desc.Width,shot.desc.Height,&staging,file);
        Logf("[GLARETRANS] saved=%d transition=%u present=%u",ok?1:0,i,present);
        if(staging)staging->Release();shot.texture->Release();shot={};
    }

    for(unsigned i=0;i<std::min<unsigned>(g_effectCount,g_effectShots.size());++i){
        auto& shot=g_effectShots[i];ID3D11Texture2D* staging{};wchar_t file[MAX_PATH]{};
        _snwprintf_s(file,MAX_PATH,_TRUNCATE,L"%ls\\eye%u_EFFECT%02u.raw",g_dumpDir,present,i);
        const bool ok=WriteDump(context,shot.texture,shot.desc.Format,shot.desc.Width,shot.desc.Height,&staging,file);
        Logf("[GLAREEFFECT] saved=%d snapshot=%u present=%u",ok?1:0,i,present);
        if(staging)staging->Release();shot.texture->Release();shot={};
    }
    g_effectCount=0;g_transitionCount=0;g_traceDraw=0;
}

void Report(std::uint32_t present, const char* mode)
{
    const auto passCount = std::min<std::uint32_t>(g_passCount.load(std::memory_order_acquire),
        static_cast<std::uint32_t>(kPassSlots));
    std::array<int, kPassSlots> order{};
    for (std::uint32_t i = 0; i < passCount; ++i)
        order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.begin() + passCount, [](int a, int b) {
        return g_pass[static_cast<std::size_t>(a)].minOrdinal.load(std::memory_order_relaxed) <
            g_pass[static_cast<std::size_t>(b)].minOrdinal.load(std::memory_order_relaxed);
    });
    char line[1400];
    int used = std::snprintf(line, sizeof(line), "[GBUF] present=%u mode=%s sceneFrames=%u full=%ux%u passes:",
        present, mode, g_sceneFrames.load(std::memory_order_relaxed),
        g_fullW.load(std::memory_order_relaxed), g_fullH.load(std::memory_order_relaxed));
    for (std::uint32_t i = 0; i < passCount && used > 0 && used < static_cast<int>(sizeof(line)) - 80; ++i)
    {
        const int wrote = std::snprintf(line + used, sizeof(line) - used, " %d:%s", order[i],
            g_pass[static_cast<std::size_t>(order[i])].name);
        if (wrote < 0)
            break;
        used += wrote;
    }
    RetailLogLine(line);

    std::array<int, kResSlots> idx{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < kResSlots; ++i)
        if (g_res[i].ready.load(std::memory_order_acquire))
            idx[n++] = static_cast<int>(i);
    std::sort(idx.begin(), idx.begin() + n, [](int a, int b) {
        const auto& ra = g_res[static_cast<std::size_t>(a)];
        const auto& rb = g_res[static_cast<std::size_t>(b)];
        return ra.rtvDraws.load(std::memory_order_relaxed) + ra.dsvDraws.load(std::memory_order_relaxed) >
            rb.rtvDraws.load(std::memory_order_relaxed) + rb.dsvDraws.load(std::memory_order_relaxed);
    });
    Logf("[GBUF] targets=%zu (overflow=%u) setRt=%llu draws=%llu twinFailures=%u; top by draws:", n,
        g_resOverflow.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_setRtCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_drawsSeen.load(std::memory_order_relaxed)),
        g_twinFailures.load(std::memory_order_relaxed));
    const std::size_t limit = std::min<std::size_t>(n, 32);
    for (std::size_t k = 0; k < limit; ++k)
    {
        const auto& r = g_res[static_cast<std::size_t>(idx[k])];
        const char* fmt = FormatName(r.format);
        const int twin = r.twin.load(std::memory_order_relaxed);
        Logf("[GBUF]  #%d %s%u %ux%u binds rtv=%llu dsv=%llu draws rtv=%llu dsv=%llu withDsv=%llu "
             "clears=%llu slots=0x%02X first=%s lastDraw=%s lastClear=%s p=%u..%u%s%s",
            idx[k], fmt ? fmt : "fmt=", fmt ? 0u : static_cast<unsigned>(r.format), r.width, r.height,
            static_cast<unsigned long long>(r.rtvBinds.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(r.dsvBinds.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(r.rtvDraws.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(r.dsvDraws.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(r.drawsWithDsv.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(r.clears.load(std::memory_order_relaxed)),
            r.slotMask.load(std::memory_order_relaxed),
            PassName(r.firstPass.load(std::memory_order_relaxed)),
            PassName(r.lastDrawPass.load(std::memory_order_relaxed)),
            PassName(r.lastClearPass.load(std::memory_order_relaxed)),
            r.firstPresent.load(std::memory_order_relaxed),
            r.lastPresent.load(std::memory_order_relaxed),
            Eligible(r) ? (IsDepthFormat(r.format) ? " [DEPTH]" : " [VELOCITY]") : "",
            twin >= 0 ? " twin" : "");
    }
    for (int t = 0; t < kTwins; ++t)
    {
        const auto& twin = g_twin[t];
        const int owner = twin.resIndex.load(std::memory_order_relaxed);
        if (owner < 0)
            continue;
        Logf("[GBUF] twin %d = #%d %s captures=%llu lastPresent=%u capturePass=%s cam=%s", t, owner,
            KindOf(g_res[static_cast<std::size_t>(owner)]),
            static_cast<unsigned long long>(twin.captures.load(std::memory_order_relaxed)),
            twin.capturedPresent.load(std::memory_order_relaxed),
            PassName(twin.lastCapturePass.load(std::memory_order_relaxed)),
            twin.camValid ? "ok" : "none");
    }
}
} // namespace

namespace GBufferCensus
{


bool WatchedVertex(ID3D11Resource* resource){
 if(!g_enabled || !g_colorArmed.load() || !resource)return false;
 UINT tag=0,size=sizeof(tag);return SUCCEEDED(resource->GetPrivateData(kGlareVertexResource,&size,&tag)) && tag==1;
}
void VertexUpdated(ID3D11Resource* resource,UINT subresource,const D3D11_BOX* box,const void* data){
 if(subresource || !data || !WatchedVertex(resource))return;
 ID3D11Buffer* buffer{};if(FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&buffer))))return;
 D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);buffer->Release();
 UINT start=box?box->left:0,end=box?box->right:desc.ByteWidth;
 if(end<=start || end>desc.ByteWidth || end-start>8*1024*1024)return;
 AcquireSRWLockExclusive(&g_writerLock);
 if(g_writerCount<16 && end-start<=64*1024*1024-g_writerBytes){auto& w=g_writerCopies[g_writerCount];
  w.bytes=new(std::nothrow)unsigned char[end-start];
  if(w.bytes){std::memcpy(w.bytes,data,end-start);w.size=end-start;w.offset=start;w.type=100;w.thread=GetCurrentThreadId();w.resource=reinterpret_cast<std::uintptr_t>(resource);
   void* stack[12]{};w.depth=CaptureStackBackTrace(0,12,stack,nullptr);for(UINT j=0;j<w.depth;++j)w.stack[j]=reinterpret_cast<std::uintptr_t>(stack[j]);++g_writerCount;g_writerBytes+=w.size;}
 }
 ReleaseSRWLockExclusive(&g_writerLock);
}
void VertexCopying(ID3D11Resource* destination,ID3D11Resource* source,UINT destinationOffset,const D3D11_BOX* box){
 if(!WatchedVertex(destination) || !source)return;
 ID3D11Buffer* buffer{};if(FAILED(source->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&buffer))))return;
 const UINT tag=1;buffer->SetPrivateData(kGlareVertexResource,sizeof(tag),&tag);D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);buffer->Release();
 static std::atomic_uint count{};if(count.fetch_add(1)>=64)return;
 Logf("[GLARECOPY] present=%u dst=%p src=%p dstOffset=%u srcRange=%u..%u usage=%u",g_present.load(),destination,source,destinationOffset,box?box->left:0,box?box->right:desc.ByteWidth,desc.Usage);
 void* stack[12]{};USHORT depth=CaptureStackBackTrace(0,12,stack,nullptr);auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
 for(unsigned j=0;j<depth;++j)Logf("[GLARECOPY] stack=%u pc=%p exeRelative=%llX",j,stack[j],static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(stack[j])-base));
}
void VertexMapped(ID3D11Resource* resource,UINT subresource,D3D11_MAP type,void* bytes){
 if(!g_enabled || !g_colorArmed.load() || !resource || !bytes || subresource || type==D3D11_MAP_READ)return;
 UINT tag=0,size=sizeof(tag);if(FAILED(resource->GetPrivateData(kGlareVertexResource,&size,&tag)) || tag!=1)return;
 ID3D11Buffer* buffer{};if(FAILED(resource->QueryInterface(__uuidof(ID3D11Buffer),reinterpret_cast<void**>(&buffer))))return;
 D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);buffer->Release();if(desc.ByteWidth>8*1024*1024)return;
 for(auto& m:g_writerMaps)if(!m.resource){m.resource=resource;m.pointer=bytes;m.size=desc.ByteWidth;m.type=type;m.depth=CaptureStackBackTrace(0,12,m.stack,nullptr);break;}
}
void VertexUnmapping(ID3D11Resource* resource,UINT subresource){
 if(!g_enabled || subresource)return;
 for(auto& m:g_writerMaps)if(m.resource==resource){
  AcquireSRWLockExclusive(&g_writerLock);
  if(g_colorArmed.load() && g_writerCount<16 && m.size<=64*1024*1024-g_writerBytes){
   auto& w=g_writerCopies[g_writerCount];w.bytes=new(std::nothrow)unsigned char[m.size];
   if(w.bytes){std::memcpy(w.bytes,m.pointer,m.size);w.size=m.size;w.type=m.type;w.depth=m.depth;w.thread=GetCurrentThreadId();w.resource=reinterpret_cast<std::uintptr_t>(resource);
    for(unsigned j=0;j<m.depth;++j)w.stack[j]=reinterpret_cast<std::uintptr_t>(m.stack[j]);++g_writerCount;g_writerBytes+=m.size;}
  }
  ReleaseSRWLockExclusive(&g_writerLock);m={};break;
 }
}
bool CaptureEnabled(){Init();return g_enabled;}
void RememberLayout(ID3D11InputLayout* layout,const D3D11_INPUT_ELEMENT_DESC* elements,UINT count){
    if(!g_enabled || !layout || !elements || count>32)return;
    LayoutElement saved[32]{};
    for(UINT i=0;i<count;++i){auto& a=elements[i];auto& b=saved[i];strncpy_s(b.name,a.SemanticName?a.SemanticName:"",_TRUNCATE);
        b.index=a.SemanticIndex;b.format=a.Format;b.slot=a.InputSlot;b.offset=a.AlignedByteOffset;b.classification=a.InputSlotClass;b.step=a.InstanceDataStepRate;}
    layout->SetPrivateData(kGlareLayout,count*sizeof(LayoutElement),saved);
}

void RememberShader(IUnknown* shader,const void* bytes,size_t length){
    if(!g_enabled || !shader || !bytes || !length || length>262144)return;
    ID3D11DeviceChild* child{};
    if(SUCCEEDED(shader->QueryInterface(__uuidof(ID3D11DeviceChild),reinterpret_cast<void**>(&child)))){
        child->SetPrivateData(kGlareShaderBytes,static_cast<UINT>(length),bytes);
        unsigned long long hash=14695981039346656037ull;
        for(size_t i=0;i<length;++i){hash^=static_cast<const unsigned char*>(bytes)[i];hash*=1099511628211ull;}
        if(hash==0x47E10DBD23AB11A1ull){UINT tag=1;child->SetPrivateData(kGlareSuspect,sizeof(tag),&tag);}
        child->Release();
    }
}

void OnPass(const char* name)
{
    if (!g_enabled || !name || !*name)
        return;
    int index = PassIndexByName(name);
    if (index < 0)
    {
        const auto slot = g_passCount.fetch_add(1, std::memory_order_acq_rel);
        if (slot >= kPassSlots)
            return;
        auto& p = g_pass[slot];
        std::strncpy(p.name, name, sizeof(p.name) - 1);
        p.ready.store(1, std::memory_order_release);
        index = static_cast<int>(slot);
    }
    auto& p = g_pass[static_cast<std::size_t>(index)];
    const auto ordinal = g_passOrdinalThisFrame.fetch_add(1, std::memory_order_relaxed);
    auto prev = p.minOrdinal.load(std::memory_order_relaxed);
    while (ordinal < prev &&
        !p.minOrdinal.compare_exchange_weak(prev, ordinal, std::memory_order_relaxed))
    {
    }
    g_currentPass.store(index, std::memory_order_relaxed);
}

void OnSetRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* dsv)
{
    Init();
    if (!g_enabled || !context)
        return;
    CaptureTransition(context, count, views);
    g_setRtCalls.fetch_add(1, std::memory_order_relaxed);
    Ctx* ctx = CtxFor(context, true);
    if (!ctx)
        return;
    const auto present = g_present.load(std::memory_order_relaxed);
    const int pass = g_currentPass.load(std::memory_order_relaxed);

    int newRt[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    for (auto& v : newRt)
        v = -1;
    const UINT n = std::min<UINT>(count, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (UINT i = 0; i < n; ++i)
    {
        if (!views || !views[i])
            continue;
        ID3D11Texture2D* texture = TextureOf(views[i]);
        if (!texture)
            continue;
        const int index = ResIndex(texture);
        texture->Release();
        if (index < 0)
            continue;
        newRt[i] = index;
        // [EYEFULL] Keep a reference to the shared scene-colour target so it can
        // also be grabbed at present, when it holds the second eye.
        if (index == g_sceneColorIdx.load(std::memory_order_relaxed))
        {
            AcquireSRWLockExclusive(&g_sceneColorLock);
            if (g_sceneColorTex != texture)
            {
                if (g_sceneColorTex)
                    g_sceneColorTex->Release();
                g_sceneColorTex = texture;
                g_sceneColorTex->AddRef();
            }
            ReleaseSRWLockExclusive(&g_sceneColorLock);
        }
        auto& r = g_res[static_cast<std::size_t>(index)];
        r.rtvBinds.fetch_add(1, std::memory_order_relaxed);
        r.slotMask.fetch_or(1u << i, std::memory_order_relaxed);
        r.lastPresent.store(present, std::memory_order_relaxed);
        int expected = -1;
        r.firstPass.compare_exchange_strong(expected, pass, std::memory_order_relaxed);
    }
    int newDsv = -1;
    if (dsv)
    {
        ID3D11Texture2D* texture = TextureOf(dsv);
        if (texture)
        {
            newDsv = ResIndex(texture);
            texture->Release();
            if (newDsv >= 0)
            {
                auto& r = g_res[static_cast<std::size_t>(newDsv)];
                r.dsvBinds.fetch_add(1, std::memory_order_relaxed);
                r.lastPresent.store(present, std::memory_order_relaxed);
                int expected = -1;
                r.firstPass.compare_exchange_strong(expected, pass, std::memory_order_relaxed);
            }
        }
    }

    // [EYEFULL] The eye boundary: a DIFFERENT full-size depth family has just
    // been bound, so the previous eye's post chain is complete and the shared
    // colour target still holds its finished image.  Grab it before the second
    // eye overwrites it.
    if (newDsv >= 0 && g_colorArmed.load(std::memory_order_acquire))
    {
        const int mainA = g_mainDepth[0].load(std::memory_order_relaxed);
        const int mainB = g_mainDepth[1].load(std::memory_order_relaxed);
        if (newDsv == mainA || newDsv == mainB)
        {
            const int previous = g_lastDepthFamily.exchange(newDsv, std::memory_order_acq_rel);
            if (previous >= 0 && previous != newDsv &&
                (previous == mainA || previous == mainB))
            {
                AcquireSRWLockShared(&g_sceneColorLock);
                ID3D11Texture2D* scene = g_sceneColorTex;
                if (scene)
                    scene->AddRef();
                ReleaseSRWLockShared(&g_sceneColorLock);
                if (scene)
                {
                    CaptureEye(context, scene, 0);
                    scene->Release();
                    Logf("[EYEFULL] eye A grabbed at the scene-depth switch #%d -> #%d",
                        previous, newDsv);
                }
            }
        }
    }

    // Anything eligible that is leaving this context after enough draws.
    const int oldDsv = ctx->dsv.load(std::memory_order_relaxed);
    const bool dsvLeaving = oldDsv >= 0 && oldDsv != newDsv &&
        Eligible(g_res[static_cast<std::size_t>(oldDsv)]) &&
        g_res[static_cast<std::size_t>(oldDsv)].drawsSinceCapture.load(std::memory_order_relaxed) >=
            kMinDrawsForCapture;
    bool rtLeaving = false;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && !rtLeaving; ++i)
    {
        const int old = ctx->rt[i].load(std::memory_order_relaxed);
        if (old < 0)
            continue;
        const auto& r = g_res[static_cast<std::size_t>(old)];
        if (!Eligible(r) || r.drawsSinceCapture.load(std::memory_order_relaxed) < kMinDrawsForCapture)
            continue;
        bool still = false;
        for (UINT j = 0; j < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++j)
            if (newRt[j] == old)
                still = true;
        rtLeaving = !still;
    }
    if (dsvLeaving || rtLeaving)
    {
        ID3D11RenderTargetView* oldViews[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* oldDsvView{};
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldViews, &oldDsvView);
        if (dsvLeaving && oldDsvView)
            Capture(context, oldDsv, oldDsvView);
        if (rtLeaving)
        {
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            {
                const int old = ctx->rt[i].load(std::memory_order_relaxed);
                if (!oldViews[i] || old < 0)
                    continue;
                bool still = false;
                for (UINT j = 0; j < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++j)
                    if (newRt[j] == old)
                        still = true;
                if (!still)
                    Capture(context, old, oldViews[i]);
            }
        }
        for (auto* v : oldViews)
            if (v)
                v->Release();
        if (oldDsvView)
            oldDsvView->Release();
    }

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        ctx->rt[i].store(newRt[i], std::memory_order_relaxed);
    ctx->dsv.store(newDsv, std::memory_order_relaxed);
}

void OnClearDepth(ID3D11DeviceContext* context, ID3D11DepthStencilView* dsv)
{
    if (!g_enabled || !context || !dsv)
        return;
    ID3D11Texture2D* texture = TextureOf(dsv);
    if (!texture)
        return;
    const int index = ResIndex(texture);
    texture->Release();
    if (index < 0)
        return;
    auto& r = g_res[static_cast<std::size_t>(index)];
    r.clears.fetch_add(1, std::memory_order_relaxed);
    r.lastClearPass.store(g_currentPass.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void OnDraw(ID3D11DeviceContext* context, unsigned vertices, unsigned start, int base, unsigned lane)
{
    if (!g_enabled || !context)
        return;
    g_argStart=start;g_argBase=base;g_argLane=lane;
    TraceEffectDraw(context, vertices);
    CaptureStage(context, vertices);
    g_drawsSeen.fetch_add(1, std::memory_order_relaxed);
    Ctx* ctx = CtxFor(context, false);
    if (!ctx)
        return;
    const int pass = g_currentPass.load(std::memory_order_relaxed);
    const int dsvIdx = ctx->dsv.load(std::memory_order_relaxed);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        const int index = ctx->rt[i].load(std::memory_order_relaxed);
        if (index < 0)
            continue;
        auto& r = g_res[static_cast<std::size_t>(index)];
        r.rtvDraws.fetch_add(1, std::memory_order_relaxed);
        r.drawsThisFrame.fetch_add(1, std::memory_order_relaxed);
        r.drawsSinceCapture.fetch_add(1, std::memory_order_relaxed);
        r.lastDrawPass.store(pass, std::memory_order_relaxed);
        if (dsvIdx >= 0)
            r.drawsWithDsv.fetch_add(1, std::memory_order_relaxed);
    }
    if (dsvIdx >= 0)
    {
        auto& r = g_res[static_cast<std::size_t>(dsvIdx)];
        r.dsvDraws.fetch_add(1, std::memory_order_relaxed);
        r.drawsThisFrame.fetch_add(1, std::memory_order_relaxed);
        r.drawsSinceCapture.fetch_add(1, std::memory_order_relaxed);
        r.lastDrawPass.store(pass, std::memory_order_relaxed);
    }
}

bool GetPresentedFrame(ID3D11Texture2D** depthTwin, float* cam32, bool* camValid)
{
    if (depthTwin) *depthTwin = nullptr;
    if (camValid) *camValid = false;
    if (!g_enabled)
        return false;
    const auto framePresent = g_presentedPresent.load(std::memory_order_acquire);
    const int d = g_presentedDepthTwin.load(std::memory_order_relaxed);
    if (d < 0 || d >= kTwins)
        return false;
    Twin& depth = g_twin[d];
    if (!depth.texture || depth.capturedPresent.load(std::memory_order_relaxed) != framePresent)
        return false;
    if (depthTwin) *depthTwin = depth.texture;
    const int v = g_presentedVelTwin.load(std::memory_order_relaxed);
    Twin* camSource = (v >= 0 && v < kTwins && g_twin[v].capturedPresent.load(std::memory_order_relaxed) == framePresent)
        ? &g_twin[v] : &depth;
    if (cam32)
    {
        AcquireSRWLockShared(&camSource->camLock);
        std::memcpy(cam32, camSource->cam, sizeof(float) * 32);
        const bool valid = camSource->camValid;
        ReleaseSRWLockShared(&camSource->camLock);
        if (camValid) *camValid = valid;
    }
    return true;
}

void ArmColorDump()
{
    if (!g_enabled)
    {
        Logf("[EYEFULL] census is off (ini [Probe] GBuf=0); nothing to capture");
        return;
    }
    g_lastDepthFamily.store(-1, std::memory_order_release);
    g_captureFramesRemaining=3;
    g_colorArmed.store(true, std::memory_order_release);
    Logf("[EYEFULL] armed: next frame's full-size targets plus both per-eye full-resolution "
         "images (eye A at the depth-family switch, eye B at present)");
}

void OnPresent(IDXGISwapChain* swapChain, bool aerEnabled)
{
    Init();
    if (!g_enabled)
        return;
    const auto present = g_present.fetch_add(1, std::memory_order_acq_rel) + 1;
    const auto framePresent = present - 1; // what captures during the frame stamped
    g_passOrdinalThisFrame.store(0, std::memory_order_relaxed);

    // Learn the backbuffer-sized family and count 3D frames.
    bool scene = false;
    // The headset swapchain can be larger than the game (e.g. 3072x3264
    // versus 2560x1440). Never infer game dimensions from the largest target.
    UINT bestW = g_fullW.load(std::memory_order_relaxed), bestH = g_fullH.load(std::memory_order_relaxed);
    ID3D11Texture2D* gameBackbuffer{};
    if (swapChain && SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&gameBackbuffer))) && gameBackbuffer)
    {
        D3D11_TEXTURE2D_DESC desc{};
        gameBackbuffer->GetDesc(&desc);
        bestW = desc.Width; bestH = desc.Height;
        gameBackbuffer->Release();
    }
    for (auto& r : g_res)
    {
        if (!r.ready.load(std::memory_order_acquire))
            continue;
        const auto draws = r.drawsThisFrame.load(std::memory_order_relaxed);
        if (!scene && IsDepthFormat(r.format) && draws >= kSceneFrameMinDepthDraws)
            scene = true;
        r.drawsThisFrame.store(0, std::memory_order_relaxed);
    }
    if (bestW != g_fullW.load(std::memory_order_relaxed) || bestH != g_fullH.load(std::memory_order_relaxed))
    {
        g_fullW.store(bestW, std::memory_order_relaxed);
        g_fullH.store(bestH, std::memory_order_relaxed);
        Logf("[GBUF] full-size family = %ux%u", bestW, bestH);
    }
    const auto sceneFrames = scene ? g_sceneFrames.fetch_add(1, std::memory_order_relaxed) + 1
                                   : g_sceneFrames.load(std::memory_order_relaxed);
    if (scene && sceneFrames == 1)
        Logf("[GBUF] first 3D frame at present %u", present);

    const char* mode = aerEnabled ? "aer" : "other";
    const auto aerPresents = aerEnabled ? g_aerPresents.fetch_add(1, std::memory_order_relaxed) + 1 : 0u;
    const auto aerScene = (aerEnabled && scene) ? g_aerScenePresents.fetch_add(1, std::memory_order_relaxed) + 1
                                                : g_aerScenePresents.load(std::memory_order_relaxed);
    if (!aerEnabled)
    {
        g_aerPresents.store(0, std::memory_order_relaxed);
        g_aerScenePresents.store(0, std::memory_order_relaxed);
    }

    // Which twins were captured for the frame that just presented?
    int capturedDepth = -1, capturedVel = -1;
    char captured[400];
    int used = 0;
    captured[0] = '\0';
    for (int t = 0; t < kTwins; ++t)
    {
        const auto& twin = g_twin[t];
        const int owner = twin.resIndex.load(std::memory_order_relaxed);
        if (owner < 0 || twin.capturedPresent.load(std::memory_order_relaxed) != framePresent)
            continue;
        const auto& r = g_res[static_cast<std::size_t>(owner)];
        if (IsDepthFormat(r.format) && capturedDepth < 0)
            capturedDepth = t;
        if (IsTwoChannelFormat(r.format) && capturedVel < 0)
            capturedVel = t;
        if (used >= 0 && used < static_cast<int>(sizeof(captured)) - 40)
        {
            const int wrote = std::snprintf(captured + used, sizeof(captured) - used, " #%d%s(t%d,cam=%s)",
                owner, KindOf(r), t, twin.camValid ? "ok" : "-");
            used = wrote < 0 ? -1 : used + wrote;
        }
    }
    g_presentedDepthTwin.store(capturedDepth, std::memory_order_relaxed);
    g_presentedVelTwin.store(capturedVel, std::memory_order_relaxed);
    g_presentedPresent.store(framePresent, std::memory_order_release);
    if ((aerEnabled && aerPresents <= kCapturedLogPresents) || (present % 600) == 0)
        Logf("[GBUF] present=%u mode=%s scene=%d captured:%s", framePresent, mode, scene ? 1 : 0,
            captured[0] ? captured : " (none)");
    if ((scene && (sceneFrames == 300 || sceneFrames == 900)) || (present % 3600) == 0)
        Report(present, mode);

    // [EYEFULL] The two main scene depth buffers, one per eye, by draw count.
    {
        int first = -1, second = -1;
        std::uint64_t firstDraws = 0, secondDraws = 0;
        const auto fw = g_fullW.load(std::memory_order_relaxed);
        const auto fh = g_fullH.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < kResSlots && fw; ++i)
        {
            const auto& r = g_res[i];
            if (!r.ready.load(std::memory_order_acquire) || r.samples != 1)
                continue;
            if (r.width != fw || r.height != fh || !IsDepthFormat(r.format))
                continue;
            const auto draws = r.dsvDraws.load(std::memory_order_relaxed);
            if (draws > firstDraws)
            {
                second = first; secondDraws = firstDraws;
                first = static_cast<int>(i); firstDraws = draws;
            }
            else if (draws > secondDraws)
            {
                second = static_cast<int>(i); secondDraws = draws;
            }
        }
        if (first >= 0 && second >= 0 &&
            (first != g_mainDepth[0].load(std::memory_order_relaxed) ||
             second != g_mainDepth[1].load(std::memory_order_relaxed)))
        {
            g_mainDepth[0].store(first, std::memory_order_relaxed);
            g_mainDepth[1].store(second, std::memory_order_relaxed);
            Logf("[EYEFULL] scene depth pair = #%d (%llu draws) and #%d (%llu draws)",
                first, static_cast<unsigned long long>(firstDraws), second,
                static_cast<unsigned long long>(secondDraws));
        }
    }
    g_eyeACaptured.store(false, std::memory_order_release);

    // [EYEFULL] The shared scene colour is the full-size colour target with the
    // most draws.  Sticky once chosen: the two heaviest targets trade places
    // frame to frame and re-picking would recreate the twins for nothing.
    {
        int best = -1;
        std::uint64_t bestDraws = 0;
        const auto fw = g_fullW.load(std::memory_order_relaxed);
        const auto fh = g_fullH.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < kResSlots && fw; ++i)
        {
            const auto& r = g_res[i];
            if (!r.ready.load(std::memory_order_acquire) || r.samples != 1)
                continue;
            if (r.width != fw || r.height != fh || (r.format != DXGI_FORMAT_R11G11B10_FLOAT && r.format != DXGI_FORMAT_R16G16B16A16_FLOAT))
                continue;
            const auto draws = r.drawsWithDsv.load(std::memory_order_relaxed);
            if (draws > bestDraws)
            {
                bestDraws = draws;
                best = static_cast<int>(i);
            }
        }
        const int current = g_sceneColorIdx.load(std::memory_order_relaxed);
        const bool keep = current >= 0 &&
            g_res[static_cast<std::size_t>(current)].ready.load(std::memory_order_acquire) &&
            g_res[static_cast<std::size_t>(current)].drawsWithDsv.load(std::memory_order_relaxed) * 2 >= bestDraws;
        if (keep)
            best = current;
        if (best >= 0 && best != g_sceneColorIdx.exchange(best, std::memory_order_acq_rel))
            Logf("[EYEFULL] scene colour = #%d %s %ux%u (%llu draws)", best,
                FormatName(g_res[static_cast<std::size_t>(best)].format)
                    ? FormatName(g_res[static_cast<std::size_t>(best)].format) : "?",
                g_res[static_cast<std::size_t>(best)].width,
                g_res[static_cast<std::size_t>(best)].height,
                static_cast<unsigned long long>(bestDraws));
    }
    // Eye B: at present the shared target holds the last eye rendered.
    if (g_colorArmed.load(std::memory_order_acquire) && swapChain)
    {
        AcquireSRWLockShared(&g_sceneColorLock);
        ID3D11Texture2D* scene = g_sceneColorTex;
        if (scene)
            scene->AddRef();
        ReleaseSRWLockShared(&g_sceneColorLock);
        if (scene)
        {
            ID3D11Device* device{};
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device),
                    reinterpret_cast<void**>(&device))) && device)
            {
                ID3D11DeviceContext* immediate{};
                device->GetImmediateContext(&immediate);
                if (immediate)
                {
                    CaptureEye(immediate, scene, 1);
                    immediate->Release();
                }
                device->Release();
            }
            scene->Release();
        }
    }

    // [EYEFULL] One-shot: every full-size target captured for this frame,
    // named by resource index so the per-eye images can be told apart.
    if (g_colorArmed.load(std::memory_order_acquire))
    {
        bool haveColor = false;
        for (int t = 0; t < kTwins; ++t)
        {
            const int owner = g_twin[t].resIndex.load(std::memory_order_relaxed);
            if (owner >= 0 && g_twin[t].texture &&
                g_twin[t].capturedPresent.load(std::memory_order_relaxed) == framePresent &&
                IsColorFormat(g_res[static_cast<std::size_t>(owner)].format))
                haveColor = true;
        }
        if (haveColor && swapChain)
        {
            ID3D11Device* device{};
            ID3D11DeviceContext* immediate{};
            if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device),
                    reinterpret_cast<void**>(&device))) && device)
            {
                device->GetImmediateContext(&immediate);
                if (immediate)
                {
                    CreateDirectoryW(g_dumpDir, nullptr);
                    DumpWriters(framePresent);
                    DumpGeometry(immediate, framePresent);
                    DumpTransitions(immediate, framePresent);
                    DumpStages(immediate, framePresent);
                    ++g_colorDumps;
                    wchar_t file[MAX_PATH]{};
                    int written = 0;
                    for (int t = 0; t < kTwins; ++t)
                    {
                        auto& twin = g_twin[t];
                        const int owner = twin.resIndex.load(std::memory_order_relaxed);
                        if (owner < 0 || !twin.texture ||
                            twin.capturedPresent.load(std::memory_order_relaxed) != framePresent)
                            continue;
                        const auto& r = g_res[static_cast<std::size_t>(owner)];
                        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\eye%u_%hs%d.raw",
                            g_dumpDir, framePresent, KindOf(r), owner);
                        if (WriteDump(immediate, twin.texture, twin.format, twin.width,
                                twin.height, &twin.staging, file))
                            ++written;
                        Logf("[EYEFULL] #%d %s %s %ux%u slots=0x%02X draws rtv=%llu dsv=%llu "
                             "first=%s lastDraw=%s -> eye%u_%hs%d.raw",
                            owner, KindOf(r),
                            FormatName(r.format) ? FormatName(r.format) : "?", r.width, r.height,
                            r.slotMask.load(std::memory_order_relaxed),
                            static_cast<unsigned long long>(r.rtvDraws.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(r.dsvDraws.load(std::memory_order_relaxed)),
                            PassName(r.firstPass.load(std::memory_order_relaxed)),
                            PassName(r.lastDrawPass.load(std::memory_order_relaxed)),
                            framePresent, KindOf(r), owner);
                    }
                    for (int slot = 0; slot < 2; ++slot)
                    {
                        auto& twin = g_eyeTwin[slot];
                        if (!twin.texture)
                            continue;
                        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\eye%u_FULL%d.raw",
                            g_dumpDir, framePresent, slot);
                        WriteDump(immediate, twin.texture, twin.format, twin.width,
                            twin.height, &twin.staging, file);
                        Logf("[EYEFULL] eye image %d: captures=%llu lastPresent=%u -> eye%u_FULL%d.raw",
                            slot, static_cast<unsigned long long>(
                                g_eyeCaptures[slot].load(std::memory_order_relaxed)),
                            twin.capturedPresent.load(std::memory_order_relaxed), framePresent, slot);
                    }
                    ID3D11Texture2D* backBuffer{};
                    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&backBuffer))) && backBuffer)
                    {
                        static ID3D11Texture2D* eyeColorStaging = nullptr;
                        D3D11_TEXTURE2D_DESC bb{};
                        backBuffer->GetDesc(&bb);
                        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\eye%u_backbuffer.raw",
                            g_dumpDir, framePresent);
                        WriteDump(immediate, backBuffer, bb.Format, bb.Width, bb.Height,
                            &eyeColorStaging, file);
                        backBuffer->Release();
                    }
                    Logf("[EYEFULL] one-shot dump %u complete: %d targets at present %u -> %ls",
                        g_colorDumps, written, framePresent, g_dumpDir);
                    immediate->Release();
                    if(g_captureFramesRemaining>0)--g_captureFramesRemaining;
                    const bool finished=g_captureFramesRemaining==0;
                    g_colorArmed.store(!finished, std::memory_order_release);
                    if (finished && written > 0) { Beep(1500, 100); Beep(2000, 100); }
                }
                device->Release();
            }
        }
    }

    // AER-only dumps with a hard budget: every eligible twin captured for this
    // frame, the colour frame, and a camera sidecar.
    if (!aerEnabled || g_dumps >= kDumpBudget || aerScene < kDumpAfterAerScenePresents ||
        capturedDepth < 0 || capturedVel < 0)
        return;
    ID3D11Device* device{};
    ID3D11DeviceContext* immediate{};
    if (!swapChain || FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
            reinterpret_cast<void**>(&device))) || !device)
        return;
    device->GetImmediateContext(&immediate);
    if (!immediate)
    {
        device->Release();
        return;
    }
    ++g_dumps;
    CreateDirectoryW(g_dumpDir, nullptr);
    wchar_t file[MAX_PATH]{};
    bool ok = true;
    _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\p%u_camera.txt", g_dumpDir, framePresent);
    FILE* sidecar = _wfopen(file, L"w");
    if (sidecar)
        std::fprintf(sidecar, "present %u mode %s dump %u/%u full %ux%u\n", framePresent, mode, g_dumps,
            kDumpBudget, g_fullW.load(std::memory_order_relaxed), g_fullH.load(std::memory_order_relaxed));
    for (int t = 0; t < kTwins; ++t)
    {
        auto& twin = g_twin[t];
        const int owner = twin.resIndex.load(std::memory_order_relaxed);
        if (owner < 0 || twin.capturedPresent.load(std::memory_order_relaxed) != framePresent)
            continue;
        const auto& r = g_res[static_cast<std::size_t>(owner)];
        const char* kind = IsDepthFormat(r.format) ? "depth" : "velocity";
        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\p%u_%hs_t%d.raw", g_dumpDir, framePresent, kind, t);
        ok &= WriteDump(immediate, twin.texture, twin.format, twin.width, twin.height, &twin.staging, file);
        if (sidecar)
        {
            AcquireSRWLockShared(&twin.camLock);
            std::fprintf(sidecar, "twin %d res #%d kind %s pass %s cam %s\n", t, owner, kind,
                PassName(twin.lastCapturePass.load(std::memory_order_relaxed)), twin.camValid ? "ok" : "none");
            for (int m = 0; m < 2; ++m)
            {
                std::fprintf(sidecar, "  %s", m == 0 ? "P" : "V");
                for (int k = 0; k < 16; ++k)
                    std::fprintf(sidecar, " %.9g", twin.cam[m * 16 + k]);
                std::fprintf(sidecar, "\n");
            }
            ReleaseSRWLockShared(&twin.camLock);
        }
    }
    if (sidecar)
        fclose(sidecar);
    ID3D11Texture2D* backBuffer{};
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backBuffer))) && backBuffer)
    {
        static ID3D11Texture2D* colorStaging = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);
        _snwprintf_s(file, MAX_PATH, _TRUNCATE, L"%ls\\p%u_color.raw", g_dumpDir, framePresent);
        ok &= WriteDump(immediate, backBuffer, desc.Format, desc.Width, desc.Height, &colorStaging, file);
        backBuffer->Release();
    }
    immediate->Release();
    device->Release();
    Logf("[GBUF] DUMP %u/%u present=%u %s captured:%s -> %ls", g_dumps, kDumpBudget, framePresent,
        ok ? "ok" : "PARTIAL", captured, g_dumpDir);
}
}
